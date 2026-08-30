#include "LipSync/EarlLipSyncSource.h"
#include "EarlOne.h"
#include "ILiveLinkClient.h"
#include "Roles/LiveLinkBasicRole.h"

namespace
{
	constexpr float SilenceRms = 0.006f;
	constexpr float FullVoiceRms = 0.12f;
	constexpr float Pi = 3.14159265358979323846f;

	// Goertzel 단일 주파수 파워
	float CalculateFrequencyPower(const float* InSamples, const int32 InNumSamples,
		const int32 InSampleRate, const float InFrequency)
	{
		const float AngularFrequency = 2.0f * Pi * InFrequency / static_cast<float>(InSampleRate);
		const float Coefficient = 2.0f * FMath::Cos(AngularFrequency);
		float Previous = 0.0f;
		float PreviousPrevious = 0.0f;
		for (int32 Index = 0; Index < InNumSamples; ++Index)
		{
			const float Current = InSamples[Index] + Coefficient * Previous - PreviousPrevious;
			PreviousPrevious = Previous;
			Previous = Current;
		}
		return Previous * Previous + PreviousPrevious * PreviousPrevious - Coefficient * Previous * PreviousPrevious;
	}
}

const TArray<FName>& FEarlLipSyncSource::GetPropertyNames()
{
	static const TArray<FName> Names = {
		TEXT("CTRL_expressions_eyeBlinkL"), TEXT("CTRL_expressions_eyeBlinkR"),
		TEXT("CTRL_expressions_eyeCheekRaiseL"), TEXT("CTRL_expressions_eyeCheekRaiseR"),
		TEXT("CTRL_expressions_mouthUpperLipRaiseL"), TEXT("CTRL_expressions_mouthUpperLipRaiseR"),
		TEXT("CTRL_expressions_mouthLowerLipDepressL"), TEXT("CTRL_expressions_mouthLowerLipDepressR"),
		TEXT("CTRL_expressions_mouthCornerPullL"), TEXT("CTRL_expressions_mouthCornerPullR"),
		TEXT("CTRL_expressions_mouthStretchL"), TEXT("CTRL_expressions_mouthStretchR"),
		TEXT("CTRL_expressions_mouthLipsPurseUL"), TEXT("CTRL_expressions_mouthLipsPurseUR"),
		TEXT("CTRL_expressions_mouthLipsPurseDL"), TEXT("CTRL_expressions_mouthLipsPurseDR"),
		TEXT("CTRL_expressions_mouthFunnelUL"), TEXT("CTRL_expressions_mouthFunnelUR"),
		TEXT("CTRL_expressions_mouthFunnelDL"), TEXT("CTRL_expressions_mouthFunnelDR"),
		TEXT("CTRL_expressions_mouthLipsTogetherUL"), TEXT("CTRL_expressions_mouthLipsTogetherUR"),
		TEXT("CTRL_expressions_mouthLipsTogetherDL"), TEXT("CTRL_expressions_mouthLipsTogetherDR"),
		TEXT("CTRL_expressions_jawOpen"), TEXT("CTRL_expressions_jawOpenExtreme"),
		TEXT("HeadControlSwitch"), TEXT("HeadRoll"), TEXT("HeadPitch"), TEXT("HeadYaw"),
		TEXT("HeadTranslationX"), TEXT("HeadTranslationY"), TEXT("HeadTranslationZ"),
		TEXT("MHFDSVersion"), TEXT("DisableFaceOverride")
	};
	return Names;
}

void FEarlLipSyncSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	Client = InClient;
	SourceGuid = InSourceGuid;
	if (!SubjectName.IsNone())
	{
		PushStaticData();
	}
}

bool FEarlLipSyncSource::IsSourceStillValid() const { return !bShutdown; }
bool FEarlLipSyncSource::RequestSourceShutdown() { bShutdown = true; return true; }
FText FEarlLipSyncSource::GetSourceType() const { return NSLOCTEXT("EarlOne", "LipSyncSourceType", "Earl LipSync (Procedural)"); }
FText FEarlLipSyncSource::GetSourceMachineName() const { return NSLOCTEXT("EarlOne", "LipSyncMachine", "localhost"); }
FText FEarlLipSyncSource::GetSourceStatus() const { return NSLOCTEXT("EarlOne", "LipSyncStatus", "Active"); }

void FEarlLipSyncSource::CreateSubject(const FName& InSubjectName, const TArray<FName>* CustomProperties)
{
	SubjectName = InSubjectName;
	ActiveProperties = CustomProperties ? *CustomProperties : GetPropertyNames();
	if (Client)
	{
		PushStaticData();
	}
	// 중립 프레임으로 시작 (입 닫힘)
	ResetToNeutral();
	UE_LOG(LogEarlOne, Log, TEXT("Created Live Link subject %s (%d curves)"),
		*SubjectName.ToString(), ActiveProperties.Num());
}

void FEarlLipSyncSource::PushStaticData()
{
	if (!Client || bShutdown || SubjectName.IsNone())
	{
		return;
	}
	if (ActiveProperties.IsEmpty())
	{
		ActiveProperties = GetPropertyNames();
	}
	FLiveLinkStaticDataStruct StaticDataStruct(FLiveLinkBaseStaticData::StaticStruct());
	FLiveLinkBaseStaticData& StaticData = *StaticDataStruct.Cast<FLiveLinkBaseStaticData>();
	StaticData.PropertyNames = ActiveProperties;
	Client->PushSubjectStaticData_AnyThread({ SourceGuid, SubjectName },
		ULiveLinkBasicRole::StaticClass(), MoveTemp(StaticDataStruct));
}

void FEarlLipSyncSource::PushPcm(const TArray<float>& MonoSamples16k)
{
	if (bShutdown || MonoSamples16k.IsEmpty())
	{
		return;
	}
	for (int32 Offset = 0; Offset < MonoSamples16k.Num(); Offset += SamplesPerFrame)
	{
		const int32 FrameSamples = FMath::Min(SamplesPerFrame, MonoSamples16k.Num() - Offset);
		PushFrame(MonoSamples16k.GetData() + Offset, FrameSamples);
	}
}

void FEarlLipSyncSource::ResetToNeutral()
{
	SmoothedOpen = 0.0f;
	TArray<float> Neutral;
	Neutral.SetNumZeroed(SamplesPerFrame);
	PushFrame(Neutral.GetData(), Neutral.Num());
}

TMap<FName, float> FEarlLipSyncSource::ComputeFrameCurves(
	const float* Samples, int32 NumSamples, float& InOutSmoothedOpen, int64 FrameCounter)
{
	// ── FEarlProceduralLiveLinkSubject::PushFrame 의 커브 수학 (모노 전제) ──
	float SumSquares = 0.0f;
	float DifferenceSquares = 0.0f;
	float PreviousSample = 0.0f;
	for (int32 Index = 0; Index < NumSamples; ++Index)
	{
		const float Sample = Samples[Index];
		SumSquares += Sample * Sample;
		if (Index > 0)
		{
			const float Difference = Sample - PreviousSample;
			DifferenceSquares += Difference * Difference;
		}
		PreviousSample = Sample;
	}

	const float Rms = FMath::Sqrt(SumSquares / static_cast<float>(FMath::Max(1, NumSamples)));
	const float Activity = FMath::Clamp((Rms - SilenceRms) / (FullVoiceRms - SilenceRms), 0.0f, 1.0f);
	const float TargetOpen = FMath::Pow(Activity, 0.65f);
	const float Smoothing = TargetOpen > InOutSmoothedOpen ? 0.68f : 0.38f;
	InOutSmoothedOpen = FMath::Lerp(InOutSmoothedOpen, TargetOpen, Smoothing);
	if (Activity < 0.01f)
	{
		InOutSmoothedOpen = FMath::Max(0.0f, InOutSmoothedOpen - 0.08f);
	}

	const float DifferenceRatio = SumSquares > KINDA_SMALL_NUMBER
		? FMath::Clamp(DifferenceSquares / (4.0f * SumSquares), 0.0f, 1.0f)
		: 0.0f;
	const int32 AnalysisSampleRate = NumSamples * 50; // 20ms 프레임 전제
	const float LowPower =
		CalculateFrequencyPower(Samples, NumSamples, AnalysisSampleRate, 350.0f) +
		CalculateFrequencyPower(Samples, NumSamples, AnalysisSampleRate, 700.0f);
	const float HighPower =
		CalculateFrequencyPower(Samples, NumSamples, AnalysisSampleRate, 1600.0f) +
		CalculateFrequencyPower(Samples, NumSamples, AnalysisSampleRate, 2600.0f);
	const float SpectralTotal = LowPower + HighPower + KINDA_SMALL_NUMBER;
	const float WideShape = FMath::Clamp(0.65f * HighPower / SpectralTotal + 0.35f * DifferenceRatio, 0.0f, 1.0f);
	const float RoundShape = FMath::Clamp(1.0f - WideShape, 0.0f, 1.0f);
	const float Open = InOutSmoothedOpen;
	const float BlinkPhase = static_cast<float>(FrameCounter % 190);
	const float Blink = BlinkPhase == 0.0f ? 0.45f : (BlinkPhase == 1.0f ? 1.0f : (BlinkPhase == 2.0f ? 0.45f : 0.0f));
	const float ConsonantClosure = Activity * DifferenceRatio * FMath::Clamp(1.0f - Open, 0.0f, 1.0f) * 0.35f;

	TMap<FName, float> CurveValues;
	CurveValues.Add(TEXT("CTRL_expressions_eyeBlinkL"), Blink);
	CurveValues.Add(TEXT("CTRL_expressions_eyeBlinkR"), Blink);
	CurveValues.Add(TEXT("CTRL_expressions_eyeCheekRaiseL"), Open * 0.05f);
	CurveValues.Add(TEXT("CTRL_expressions_eyeCheekRaiseR"), Open * 0.05f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthUpperLipRaiseL"), Open * (0.10f + 0.16f * WideShape));
	CurveValues.Add(TEXT("CTRL_expressions_mouthUpperLipRaiseR"), Open * (0.10f + 0.16f * WideShape));
	CurveValues.Add(TEXT("CTRL_expressions_mouthLowerLipDepressL"), Open * 0.30f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLowerLipDepressR"), Open * 0.30f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthCornerPullL"), Open * WideShape * 0.16f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthCornerPullR"), Open * WideShape * 0.16f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthStretchL"), Open * WideShape * 0.38f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthStretchR"), Open * WideShape * 0.38f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsPurseUL"), Open * RoundShape * 0.22f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsPurseUR"), Open * RoundShape * 0.22f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsPurseDL"), Open * RoundShape * 0.18f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsPurseDR"), Open * RoundShape * 0.18f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthFunnelUL"), Open * RoundShape * 0.42f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthFunnelUR"), Open * RoundShape * 0.42f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthFunnelDL"), Open * RoundShape * 0.36f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthFunnelDR"), Open * RoundShape * 0.36f);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsTogetherUL"), ConsonantClosure);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsTogetherUR"), ConsonantClosure);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsTogetherDL"), ConsonantClosure);
	CurveValues.Add(TEXT("CTRL_expressions_mouthLipsTogetherDR"), ConsonantClosure);
	CurveValues.Add(TEXT("CTRL_expressions_jawOpen"), Open * 0.72f);
	CurveValues.Add(TEXT("CTRL_expressions_jawOpenExtreme"), FMath::Max(0.0f, Open - 0.82f) * 0.45f);
	CurveValues.Add(TEXT("MHFDSVersion"), 1.0f);
	CurveValues.Add(TEXT("DisableFaceOverride"), 1.0f);
	return CurveValues;
}

void FEarlLipSyncSource::PushCurveFrame(const TMap<FName, float>& CurveValues)
{
	if (bShutdown || !Client || SubjectName.IsNone())
	{
		return;
	}

	static const FName VersionName(TEXT("MHFDSVersion"));
	static const FName OverrideName(TEXT("DisableFaceOverride"));

	FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkBaseFrameData::StaticStruct());
	FLiveLinkBaseFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkBaseFrameData>();
	FrameData.PropertyValues.Reserve(ActiveProperties.Num());
	for (const FName& PropertyName : ActiveProperties)
	{
		// 리그 플래그 2종은 절차적 경로와 동일하게 항상 1 (트랙에는 없음)
		const float Value = (PropertyName == VersionName || PropertyName == OverrideName)
			? 1.0f : CurveValues.FindRef(PropertyName);
		FrameData.PropertyValues.Add(Value);
	}
	FrameData.MetaData.StringMetaData.Add(TEXT("HeadPoseMode"), TEXT("0"));
	Client->PushSubjectFrameData_AnyThread({ SourceGuid, SubjectName }, MoveTemp(FrameDataStruct));
}

void FEarlLipSyncSource::PushFrame(const float* Samples, int32 NumSamples)
{
	if (bShutdown || !Client || !Samples || NumSamples <= 0 || SubjectName.IsNone())
	{
		return;
	}

	const TMap<FName, float> CurveValues = ComputeFrameCurves(Samples, NumSamples, SmoothedOpen, FrameCounter);
	++FrameCounter;

	FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkBaseFrameData::StaticStruct());
	FLiveLinkBaseFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkBaseFrameData>();
	FrameData.PropertyValues.Reserve(ActiveProperties.Num());
	for (const FName& PropertyName : ActiveProperties)
	{
		FrameData.PropertyValues.Add(CurveValues.FindRef(PropertyName));
	}
	FrameData.MetaData.StringMetaData.Add(TEXT("HeadPoseMode"), TEXT("0"));

	Client->PushSubjectFrameData_AnyThread({ SourceGuid, SubjectName }, MoveTemp(FrameDataStruct));

	if (!bLoggedSpeech && CurveValues.FindRef(TEXT("CTRL_expressions_jawOpen")) > 0.05f)
	{
		bLoggedSpeech = true;
		UE_LOG(LogEarlOne, Log, TEXT("LipSync producing speech frames (jaw=%.3f)"),
			CurveValues.FindRef(TEXT("CTRL_expressions_jawOpen")));
	}
}

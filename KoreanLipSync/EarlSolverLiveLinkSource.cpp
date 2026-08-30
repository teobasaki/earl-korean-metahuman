#include "LipSync/EarlSolverLiveLinkSource.h"

#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "GuiToRawControlsUtils.h"
#include "MetaHumanAudioBaseLiveLinkSubjectSettings.h"
#include "MetaHumanLocalLiveLinkSubjectSettings.h"
#include "MetaHumanHeadTransform.h"
#include "Roles/LiveLinkBasicRole.h"

#include "EarlOne.h"
#define LogEarlLipSync LogEarlOne

namespace
{
#if PLATFORM_ANDROID
	constexpr int32 DefaultProceduralLipSync = 1;
#else
	constexpr int32 DefaultProceduralLipSync = 0;
#endif

	TAutoConsoleVariable<int32> CVarEarlForceProceduralLipSync(
		TEXT("earl.LipSync.ForceProcedural"),
		DefaultProceduralLipSync,
		TEXT("Use the PCM-driven MetaHuman curve generator instead of the desktop NNE audio solver."),
		ECVF_Default);
	TAutoConsoleVariable<int32> CVarEarlLipSyncTestOnStart(
		TEXT("earl.LipSync.TestOnStart"),
		0,
		TEXT("Push one synthetic speech frame when a procedural subject starts. Diagnostic only."),
		ECVF_Default);

	// ── GUI 컨트롤 이름 검증기 (2026-08-06) ────────────────────────────────
	// 우리가 발행하는 이름은 **GUI 컨트롤명**이고, 엔진 유틸이 그걸 raw 251개로 펼친다.
	// 이름이 표에 없으면 **경고 없이 그냥 무시된다** — 값을 아무리 올려도 얼굴은 그대로다.
	// T75 에서 `eyeWidenL` 로 한 번 당했다. 그래서 "이 이름이 살아 있는가"를 묻는 명령을 둔다.
	// 쓰는 법: `earl.ExprProbe` (기본 후보 전체) 또는 `earl.ExprProbe CTRL_L_nose_nasolabialDeepen.ty`
	void ProbeGuiControl(const FString& GuiName)
	{
		static const TMap<FString, float> NeutralRaw =
			GuiToRawControlsUtils::ConvertGuiToRawControls(TMap<FString, float>{});

		TMap<FString, float> One;
		One.Add(GuiName, 1.0f);
		const TMap<FString, float> Raw = GuiToRawControlsUtils::ConvertGuiToRawControls(One);

		int32 Moved = 0;
		float Biggest = 0.0f;
		FString BiggestName;
		for (const TPair<FString, float>& Pair : Raw)
		{
			const float* Neutral = NeutralRaw.Find(Pair.Key);
			const float Diff = Pair.Value - (Neutral ? *Neutral : 0.0f);
			if (FMath::Abs(Diff) > KINDA_SMALL_NUMBER)
			{
				++Moved;
				if (FMath::Abs(Diff) > FMath::Abs(Biggest))
				{
					Biggest = Diff;
					BiggestName = Pair.Key;
				}
			}
		}

		if (Moved == 0)
		{
			UE_LOG(LogEarlOne, Warning,
				TEXT("earl.ExprProbe: %-40s → **죽은 이름** (raw 0개). 이 커브는 얼굴에 안 닿는다."),
				*GuiName);
		}
		else
		{
			UE_LOG(LogEarlOne, Log,
				TEXT("earl.ExprProbe: %-40s → raw %2d개, 최대 %s %+.3f"),
				*GuiName, Moved, *BiggestName, Biggest);
		}
	}

	FAutoConsoleCommand GEarlExprProbeCmd(
		TEXT("earl.ExprProbe"),
		TEXT("Check whether GUI control names actually reach the face: earl.ExprProbe [name ...]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				if (Args.Num() > 0)
				{
					for (const FString& Name : Args) { ProbeGuiControl(Name); }
					return;
				}
				// 기본 후보 — 지금 쓰는 것 + 중안면(진짜 "광대"로 읽히는 곳) 후보들
				static const TCHAR* Candidates[] = {
					TEXT("CTRL_L_eye_cheekRaise.ty"),
					TEXT("CTRL_L_eye_squintInner.ty"),
					TEXT("CTRL_L_eye_lidPress.ty"),
					TEXT("CTRL_L_eye_blink.ty"),
					TEXT("CTRL_L_mouth_cornerPull.ty"),
					TEXT("CTRL_L_mouth_dimple.ty"),
					TEXT("CTRL_L_mouth_cornerDepress.ty"),
					TEXT("CTRL_L_nose_nasolabialDeepen.ty"),
					TEXT("CTRL_L_mouth_cheekBlow.ty"),
					TEXT("CTRL_L_mouth_sharpCornerPull.ty"),
					TEXT("CTRL_L_mouth_upperLipRaise.ty"),
					TEXT("CTRL_L_nose_wrinkleUpper.ty"),
					TEXT("CTRL_L_brow_raiseIn.ty"),
					TEXT("CTRL_L_brow_raiseOut.ty"),
					TEXT("CTRL_L_brow_down.ty")
				};
				for (const TCHAR* Name : Candidates) { ProbeGuiControl(FString(Name)); }
			}));

	// 표정 층을 솔버 출력에 어떻게 합칠 것인가. 1 = 남은 여유분에만 얹는다(기본, 포화 없음),
	// 0 = 예전 덧셈(솔버가 이미 구동 중인 커브를 1.0 에서 잘라먹는다). A/B 용으로 남겨 둔다.
	TAutoConsoleVariable<int32> CVarEarlExprHeadroomBlend(
		TEXT("earl.ExprHeadroom"),
		1,
		TEXT("1 = add expression into remaining headroom (never saturates), 0 = legacy plain add."),
		ECVF_Default);

	constexpr float SilenceRms = 0.006f;
	constexpr float FullVoiceRms = 0.12f;
	constexpr float Pi = 3.14159265358979323846f;

	float CalculateFrequencyPower(
		const float* InSamples,
		const int32 InNumSamples,
		const int32 InNumChannels,
		const int32 InSampleRate,
		const float InFrequency)
	{
		const float AngularFrequency = 2.0f * Pi * InFrequency /
			static_cast<float>(InSampleRate);
		const float Coefficient = 2.0f * FMath::Cos(AngularFrequency);
		float Previous = 0.0f;
		float PreviousPrevious = 0.0f;
		for (int32 Index = 0; Index < InNumSamples; ++Index)
		{
			float Sample = 0.0f;
			for (int32 Channel = 0; Channel < InNumChannels; ++Channel)
			{
				Sample += InSamples[Index * InNumChannels + Channel];
			}
			Sample /= static_cast<float>(InNumChannels);

			const float Current = Sample + Coefficient * Previous - PreviousPrevious;
			PreviousPrevious = Previous;
			Previous = Current;
		}

		return Previous * Previous + PreviousPrevious * PreviousPrevious -
			Coefficient * Previous * PreviousPrevious;
	}
}

FEarlTTSLiveLinkSubject::FEarlTTSLiveLinkSubject(
	ILiveLinkClient* InLiveLinkClient,
	const FGuid& InSourceGuid,
	const FName& InSubjectName,
	UMetaHumanAudioBaseLiveLinkSubjectSettings* InSettings)
	: FMetaHumanAudioBaseLiveLinkSubject(InLiveLinkClient, InSourceGuid, InSubjectName, InSettings)
{
}

void FEarlTTSLiveLinkSubject::PushPcm(
	const TArray<float>& InSamples,
	const int32 InSampleRate,
	const int32 InNumChannels)
{
	if (!IsRunning() || InSamples.IsEmpty() || InSampleRate <= 0 || InNumChannels <= 0)
	{
		return;
	}

	if (!bLoggedFirstPcm)
	{
		bLoggedFirstPcm = true;
		UE_LOG(LogEarlLipSync, Log,
			TEXT("TTS PCM reached the MetaHuman audio solver: %dHz %dch %d samples"),
			InSampleRate, InNumChannels, InSamples.Num());
	}

	FAudioSample AudioSample;
	AudioSample.NumChannels = InNumChannels;
	AudioSample.SampleRate = InSampleRate;
	AudioSample.NumSamples = InSamples.Num() / InNumChannels;
	AudioSample.Data = InSamples;
	GetSampleTime(FFrameRate(InSampleRate, 1), AudioSample.Time, AudioSample.TimeSource);

	AddAudioSample(MoveTemp(AudioSample));
}

void FEarlTTSLiveLinkSubject::ExtractPipelineData(
	const TSharedPtr<UE::MetaHuman::Pipeline::FPipelineData> InPipelineData)
{
	FMetaHumanAudioBaseLiveLinkSubject::ExtractPipelineData(InPipelineData);

	// 감정 표정 가산 — 프레임이 LiveLink 로 나가기 전이 유일한 안전 지점이다.
	// (아래 진단 로그의 early-return 보다 **먼저** 와야 매 프레임 적용된다)
	ApplyIdleMouthDamp();   // 무음이면 하반부를 먼저 닫고
	ApplyMouthGain();       // 윈도우 확정 레시피의 '입 커브 게인' — ABP 노드 대신 여기서
	ApplyExpressionDelta(); // 그 위에 상반부 표정을 얹고
	ApplyHeadMotion();      // 머리 포즈를 실어 보낸다
	MeasureArticulation();  // 그리고 조음이 얼마나 살아 있는지 잰다

	if (bLoggedSolverOutput || Animation.AnimationData.IsEmpty())
	{
		return;
	}
	bLoggedSolverOutput = true;

	int32 EyeControls = 0;
	int32 BrowControls = 0;
	for (const TPair<FString, float>& Control : Animation.AnimationData)
	{
		if (Control.Key.Contains(TEXT("eye"), ESearchCase::IgnoreCase))
		{
			++EyeControls;
		}
		if (Control.Key.Contains(TEXT("brow"), ESearchCase::IgnoreCase))
		{
			++BrowControls;
		}
	}

	UE_LOG(LogEarlLipSync, Log,
		TEXT("MetaHuman audio solver output: %d control curves (eye=%d brow=%d). "
			 "Non-zero eye and brow counts mean full-face animation, not jaw only."),
		Animation.AnimationData.Num(), EyeControls, BrowControls);
}

void FEarlTTSLiveLinkSubject::SetHeadMotion(float InAmplitudeDeg, float InSpeed)
{
	HeadAmplitudeDeg = InAmplitudeDeg;
	HeadSpeed = FMath::Max(0.0f, InSpeed);
}

void FEarlTTSLiveLinkSubject::SetHeadNod(float InNodDeg, float InDurationSec)
{
	HeadNodDeg = InNodDeg;
	NodDurationSec = FMath::Max(0.05f, InDurationSec);
}

void FEarlTTSLiveLinkSubject::TriggerHeadNod(float InStrength)
{
	// 게임 스레드에서 부르고 솔버 스레드에서 소비한다
	FScopeLock Lock(&HeadNodMutex);
	PendingNodStrength = FMath::Clamp(InStrength, 0.0f, 1.0f);
}

void FEarlTTSLiveLinkSubject::SetHeadFollow(float InYawDeg, float InPitchDeg, float InRollDeg)
{
	FScopeLock Lock(&HeadNodMutex);
	FollowYawDeg = InYawDeg;
	FollowPitchDeg = InPitchDeg;
	FollowRollDeg = InRollDeg;
}

void FEarlTTSLiveLinkSubject::GetHeadMotionAngles(float& OutYawDeg, float& OutPitchDeg) const
{
	// 시선 보정용 — 한 프레임(20ms) 늦어도 무해하므로 락 없이 읽는다
	OutYawDeg = LastHeadYawDeg;
	OutPitchDeg = LastHeadPitchDeg;
}

void FEarlTTSLiveLinkSubject::SetIdleMouthDamp(bool bInIdle, float InDamp)
{
	bIdleMouth = bInIdle;
	IdleMouthDamp = FMath::Clamp(InDamp, 0.0f, 1.0f);
}

void FEarlTTSLiveLinkSubject::SetMouthGain(float InGain)
{
	MouthCurveGain = FMath::Clamp(InGain, 0.5f, 2.0f);
}

// ★ 입 커브 게인 (2026-08-25) — 윈도우 확정 레시피(2026-07-30)의 네 번째 항목을 코드로 옮겼다.
//
// 원 레시피는 "ABP ModifyCurve Scale ×1.25~1.35" 였는데, 그 노드가 **로컬 전용 콘텐츠**
// (ABP_Earl_Face_AudioLiveLink, gitignore) 안이라 머신을 옮길 때마다 재삽입해야 했다 —
// HANDOFF 가 경고한 그 함정을 맥에서 실제로 밟았다(게인 없이 입이 약하게 나왔다).
// LiveLink 로 나가기 직전에 곱하면 ABP 가 무엇이든(팀 ABP·플러그인 ABP·직접 구동) 항상 적용된다.
//
// ⚠️ ApplyIdleMouthDamp **뒤, ApplyExpressionDelta 앞**이다:
//   · 무음 감쇠 뒤 = 대기 중 입 벌림(솔버 버릇)까지 키우지 않는다
//   · 표정 델타 앞 = 표정층의 여유분 합성(스크린 합성)이 게인 이후 값 기준으로 계산된다
// 1.0 클램프 — 솔버 상한을 넘기면 잘린 구간에서 변화가 사라진다(표정층에서 실측한 그 포화).
void FEarlTTSLiveLinkSubject::ApplyMouthGain()
{
	if (FMath::IsNearlyEqual(MouthCurveGain, 1.0f) || Animation.AnimationData.IsEmpty())
	{
		return;
	}
	for (TPair<FString, float>& Control : Animation.AnimationData)
	{
		const FString Lower = Control.Key.ToLower();
		if (Lower.Contains(TEXT("jaw")) || Lower.Contains(TEXT("mouth")) || Lower.Contains(TEXT("lip")))
		{
			Control.Value = FMath::Min(Control.Value * MouthCurveGain, 1.0f);
		}
	}
}

void FEarlTTSLiveLinkSubject::ApplyIdleMouthDamp()
{
	if (!bIdleMouth || IdleMouthDamp <= KINDA_SMALL_NUMBER || Animation.AnimationData.IsEmpty())
	{
		return;
	}

	// 솔버는 무음에도 입을 살짝 벌린다 — 대기 중엔 그게 "계속 말하려는 얼굴"로 보인다.
	// 무음 구간에서만 하반부를 닫는다. 립싱크가 도는 동안엔 bIdleMouth 가 false 라 손대지 않는다.
	int32 Damped = 0;
	for (TPair<FString, float>& Control : Animation.AnimationData)
	{
		const FString Lower = Control.Key.ToLower();
		if (Lower.Contains(TEXT("jaw")) || Lower.Contains(TEXT("mouth")) || Lower.Contains(TEXT("lip")))
		{
			Control.Value *= (1.0f - IdleMouthDamp);
			++Damped;
		}
	}

	if (!bLoggedIdleMouth)
	{
		bLoggedIdleMouth = true;
		UE_LOG(LogEarlLipSync, Log,
			TEXT("Idle mouth damp on: %d lower-face controls scaled by %.2f while silent"),
			Damped, 1.0f - IdleMouthDamp);
	}
}

// ★ 조음 계측기 (2026-08-05). 립싱크가 둔해졌다는 보고가 **네 번째**라 만들었다.
//
// 왜 화면으로 재면 안 되나: 입 크롭은 머리 움직임·프레이밍·자막 오버레이가 전부 섞이고,
// 실행마다 카메라와 머리 위상이 달라 값이 **비단조**로 나온다(T72). 실제로 이번에도
// 광대 세기를 훑었더니 1.9→3.67, 1.0→1.93, 0→2.09 로 뒤죽박죽이었다.
//
// 여기서는 **LiveLink 로 나가기 직전의 조음 커브 자체**를 잰다. 머리도 카메라도 끼지 않는다.
// 그리고 우리 표정 델타가 조음 커브를 오염시켰는지(=articulation touched)도 상시 센다 —
// 예전엔 그 판정이 시작 시 1회 로그뿐이라, 나중에 층을 더하면서 깨져도 알 수 없었다.
void FEarlTTSLiveLinkSubject::MeasureArticulation()
{
	if (Animation.AnimationData.IsEmpty())
	{
		return;
	}
	float Sum = 0.0f;
	int32 Count = 0;
	for (const TPair<FString, float>& Control : Animation.AnimationData)
	{
		const FString Lower = Control.Key.ToLower();
		// 조음 = 발음을 만드는 커브. 입꼬리·보조개(표정)는 여기 들어가지 않는다.
		if (!(Lower.Contains(TEXT("jawopen")) || Lower.Contains(TEXT("funnel"))
			|| Lower.Contains(TEXT("purse")) || Lower.Contains(TEXT("lipstogether"))
			|| Lower.Contains(TEXT("stretch"))))
		{
			continue;
		}
		if (const float* Prev = LastArticulation.Find(Control.Key))
		{
			Sum += FMath::Abs(Control.Value - *Prev);
			++Count;
		}
		LastArticulation.Add(Control.Key, Control.Value);
	}
	// ★ 얼굴 갱신률 — **초당 몇 프레임이 실제로 얼굴에 도달하는가** (2026-08-05).
	// 이걸 한 번도 안 재고 렌더·커브만 의심했다. 설계값은 50/s(20ms 페이싱)인데,
	// 실제로 그만큼 나가는지는 별개다. 30/s 아래로 떨어지면 입이 계단처럼 보인다 —
	// 렌더 FPS 가 200 이어도 그렇다. 둘은 다른 축이다.
	{
		const double Now = FPlatformTime::Seconds();
		++FrameRateCount;
		if (FrameRateWindowStart <= 0.0)
		{
			FrameRateWindowStart = Now;
		}
		else if (Now - FrameRateWindowStart >= 2.0)
		{
			const double Fps = FrameRateCount / (Now - FrameRateWindowStart);
			UE_LOG(LogEarlLipSync, Log,
				TEXT("얼굴 갱신률: %.1f 프레임/초 (설계값 50) — 렌더 FPS 와 다른 축이다"), Fps);
			FrameRateWindowStart = Now;
			FrameRateCount = 0;
		}
	}

	if (Count > 0)
	{
		FScopeLock Lock(&ArticulationMutex);
		ArticulationHistory.Add(Sum / Count);
		if (ArticulationHistory.Num() > 600)
		{
			ArticulationHistory.RemoveAt(0, ArticulationHistory.Num() - 600, EAllowShrinking::No);
		}
	}
}

void FEarlTTSLiveLinkSubject::GetArticulation(float& OutMean, float& OutPeak, int32& OutSamples) const
{
	FScopeLock Lock(&ArticulationMutex);
	OutSamples = ArticulationHistory.Num();
	OutMean = 0.0f;
	OutPeak = 0.0f;
	if (OutSamples == 0)
	{
		return;
	}
	for (const float V : ArticulationHistory)
	{
		OutMean += V;
		OutPeak = FMath::Max(OutPeak, V);
	}
	OutMean /= OutSamples;
}

void FEarlTTSLiveLinkSubject::ResetArticulation()
{
	FScopeLock Lock(&ArticulationMutex);
	ArticulationHistory.Reset();
}

void FEarlTTSLiveLinkSubject::ApplyHeadMotion()
{
	// ⚠️ 예전엔 여기서 `HeadAmplitudeDeg <= 0` 이면 곧바로 return 했다. 그러면 사인 흔들림을
	// 끄는 순간 **끄덕임과 시선 추종까지 같이 죽는다** — 셋은 독립적인 기여인데 하나의 스위치에
	// 묶여 있었다(2026-08-05, 추종 부호를 재려고 사인을 껐다가 HeadYaw 가 계속 0 으로 나와 발견).
	// 이제 각 기여를 따로 계산하고, **전부 0 일 때만** 아무것도 하지 않는다.

	// 프레임 페이싱이 20ms 고정이라 프레임당 그만큼 시간을 흘린다
	HeadTime += 0.02 * HeadSpeed;
	const double T = HeadTime;

	// 주기를 서로 나누어떨어지지 않게 겹쳐 "반복되는 느낌"을 없앤다.
	// 사람은 고개를 좌우로 가장 많이 쓰고(요), 끄덕임(피치)이 그다음, 기울임(롤)이 가장 적다.
	//
	// ⚠️ 축마다 사인 2개로는 부족하다 — 20초쯤 보면 **주기가 읽힌다**(2026-08-05).
	// 성분을 셋으로 늘리고 비율을 무리수에 가깝게 잡아 최소공배수를 멀리 밀어낸다.
	// 게다가 진폭 자체를 아주 느린 사인으로 흔들어(0.7~1.3배) "지금 좀 더 움직이네" 를 만든다.
	// 사람의 미세 움직임은 균일하지 않다 — 가만있다가 잠깐 많이 움직였다 다시 잦아든다.
	const double Breath = 0.85 + 0.30 * FMath::Sin(T * 0.071 + 0.4);   // 아주 느린 진폭 변조
	const double A = HeadAmplitudeDeg * Breath;

	const double Yaw = A * (0.46 * FMath::Sin(T * 0.37)
						  + 0.31 * FMath::Sin(T * 0.83 + 1.7)
						  + 0.23 * FMath::Sin(T * 1.29 + 3.1));
	const double Pitch = A * 0.60 * (0.52 * FMath::Sin(T * 0.29 + 0.9)
								   + 0.28 * FMath::Sin(T * 0.61)
								   + 0.20 * FMath::Sin(T * 1.13 + 2.4));
	double Roll = A * 0.35 * (0.62 * FMath::Sin(T * 0.23 + 2.1)
							+ 0.38 * FMath::Sin(T * 0.53 + 0.7));

	// ── 강세 끄덕임 ────────────────────────────────────────────────────────
	// 사인 합 위에 **얹는다**. 사람은 강세에서 턱을 살짝 내렸다 되돌린다 —
	// 반쪽 사인(0→최대→0)이 그 모양이다. 단순 감쇠는 툭 튀었다 흐르므로 안 쓴다.
	{
		FScopeLock Lock(&HeadNodMutex);
		if (PendingNodStrength > 0.0f)
		{
			NodStrength = PendingNodStrength;
			NodTimeRemaining = NodDurationSec;
			PendingNodStrength = 0.0f;
		}
	}
	// 머리 추종분 — 게임 스레드가 시선을 보고 계산해 넣어 준다 (눈 먼저, 머리 지연 뒤)
	float FollowYaw = 0.0f, FollowPitch = 0.0f, FollowRoll = 0.0f;
	{
		FScopeLock Lock(&HeadNodMutex);
		FollowYaw = FollowYawDeg;
		FollowPitch = FollowPitchDeg;
		FollowRoll = FollowRollDeg;
	}
	Roll += FollowRoll;

	double NodPitch = 0.0;
	if (NodTimeRemaining > 0.0f)
	{
		NodTimeRemaining = FMath::Max(0.0f, NodTimeRemaining - 0.02f);
		const double Phase = 1.0 - NodTimeRemaining / FMath::Max(NodDurationSec, KINDA_SMALL_NUMBER);
		NodPitch = HeadNodDeg * NodStrength * FMath::Sin(PI * FMath::Clamp(Phase, 0.0, 1.0));
		// 끄덕임에는 아주 약한 롤을 섞는다 — 순수 상하 운동은 기계로 읽힌다
		Roll += NodPitch * 0.18;
	}

	// ⚠️ `Animation.Pose` 는 **메시 공간**이고, 엔진이 보내기 직전에 `MeshToBone()` 으로 뼈 공간으로
	// 바꾼다(퍼포먼스 뷰어의 머리는 -X, MetaHuman 머리뼈는 +Y 를 보므로 90도 요 보정이 끼어 있다).
	// 그래서 여기 항등을 넣으면 **중립이 Roll 89.7도로 도착한다** — 머리가 옆으로 꺾인다
	// (2026-08-05 실측: `earl.FaceProbe` HeadRoll=89.710).
	// 역변환 `BoneToMesh()` 를 미리 걸어 두면 MeshToBone(BoneToMesh(X)) = X 라
	// **우리가 의도한 각도 그대로** 뼈에 도착하고, 0 이면 중립이 된다.
	const double FinalYaw = Yaw + FollowYaw;
	const double FinalPitch = Pitch + NodPitch + FollowPitch;

	// 세 기여가 전부 0 이면 머리 층을 아예 끈 것이다 — 스위치도 켜지 않는다
	if (HeadAmplitudeDeg <= KINDA_SMALL_NUMBER
		&& FMath::IsNearlyZero(FinalYaw) && FMath::IsNearlyZero(FinalPitch)
		&& FMath::IsNearlyZero(Roll))
	{
		return;
	}

	// 시선 보정이 읽어 간다 (전정안반사 — 머리가 돌아도 눈은 상대에 남는다).
	// ⚠️ 추종분도 **포함**해야 한다. 머리가 시선 쪽으로 간 만큼 눈이 중앙으로 되돌아오고,
	// 그래야 전체 시선(머리+눈)이 목표에 유지된다. 이 되먹임은 발산하지 않는다 —
	// 눈 = 목표 × (1 − 추종게인) 로 수렴한다.
	LastHeadYawDeg = static_cast<float>(FinalYaw);
	LastHeadPitchDeg = static_cast<float>(FinalPitch);

	Animation.Pose = FMetaHumanHeadTransform::BoneToMesh(
		FTransform(FRotator(FinalPitch, FinalYaw, Roll)));

	// ★ 이걸 켜야 얼굴 포스트프로세스(CR_MetaHuman_HeadMovement_IK_Proc)가 포즈를 적용한다.
	// 기본값 0 은 "들어온 머리 포즈 없음" 이라 리그가 아무것도 하지 않는다.
	HeadControlSwitch = 1.0f;

	// ★★ 이것까지 켜야 회전이 살아남는다 (2026-08-05, 이게 빠져서 머리가 안 움직였다).
	// PushFrameData 는 우리 Pose 로 HeadRoll/Pitch/Yaw 를 채우지만, **맨 마지막에** 부르는
	// `Settings->PreProcess` 가 HeadPoseMode 에 Orientation 플래그가 없으면 그 세 값을
	// **0 으로 덮어쓴다** (UMetaHumanLiveLinkSubjectSettings::PreProcess, UE 5.8).
	// 기본값이 None 이라 우리가 아무리 Pose 를 채워도 메시에는 0 만 도착했다
	// (실측: HeadControlSwitch=1.000 인데 HeadYaw/Pitch/Roll=0.000 — `earl.FaceProbe`).
	// HeadControlSwitch 는 그 함수가 손대지 않아 살아남는 바람에 "켜져 있는데 안 움직인다"로 보였다.
	// Translation 은 계속 끈다 — 카메라 상대 이동이라 우리 절차적 회전과 무관하고, 중립 포즈가
	// 없으면 엔진이 어차피 0 으로 만든다.
	HeadPoseMode = EMetaHumanLiveLinkHeadPoseMode::Orientation;

	if (!bLoggedHeadMotion)
	{
		bLoggedHeadMotion = true;
		UE_LOG(LogEarlLipSync, Log,
			TEXT("Procedural head motion on: %.1f deg amplitude, speed %.2f (HeadControlSwitch=1)"),
			HeadAmplitudeDeg, HeadSpeed);
	}
}

void FEarlTTSLiveLinkSubject::SetRawExpressionDelta(TMap<FString, float> InRawDelta)
{
	FScopeLock Lock(&ExpressionDeltaMutex);
	RawExpressionDelta = MoveTemp(InRawDelta);
}

void FEarlTTSLiveLinkSubject::ApplyExpressionDelta()
{
	if (Animation.AnimationData.IsEmpty())
	{
		return;
	}

	FScopeLock Lock(&ExpressionDeltaMutex);
	if (RawExpressionDelta.IsEmpty())
	{
		return;
	}

	// ⚠️ 키를 **추가하지 않는다.** 엔진은 정적 데이터(PropertyNames)를 raw 컨트롤 전체 집합으로
	// 한 번 밀고, 매 프레임 Animation.AnimationData 를 순회해 값만 위치순으로 채운다
	// (MetaHumanLocalLiveLinkSubject::PushStaticData / PushFrameData, UE 5.8).
	// 키가 늘거나 줄면 이름↔값 정렬이 어긋나 얼굴이 엉뚱하게 구동된다 —
	// 기존 키의 값만 바꾸는 것은 안전하다.
	int32 Applied = 0;
	// ★ 포화 감시 (2026-08-06) — 우리 층이 솔버가 **이미 구동 중인** 커브를 덮어 포화시키는지 센다.
	// 계기가 필요한 이유: `nose_nasolabialDeepen` 은 우리만 쓰는 커브가 아니라 **NNE 스피치
	// 솔버의 출력 목록에도 들어 있다**(SpeechAnimationSolverV4.cpp 의 컨트롤 표에서 확인).
	// 즉 발화 중에는 솔버가 이 커브를 말에 맞춰 움직이고 있고, 그 위에 우리 값을 더하면
	// 1.0 에서 잘려 **솔버의 변화가 통째로 사라진다** — 커브는 살아 있는데 얼굴은 굳는,
	// 오늘 하루 종일 쫓던 그 증상과 같은 형태다. `earl.LipMeter` 는 조음 커브만 재므로
	// 이걸 못 본다. 그래서 따로 센다.
	// ★★ 남은 여유분에만 얹는다 (2026-08-06) — 단순 덧셈이 아니다.
	// 우리 층이 쓰는 커브 중 상당수(squintInner · eyeCheekRaise · nasolabialDeepen ·
	// cornerPull · dimple …)를 **솔버도 같이 구동한다.** 그냥 더하면 솔버가 0.6 을 주는
	// 구간에서 우리 0.7 이 얹혀 1.0 에서 잘리고, **잘린 동안 솔버의 변화가 통째로 사라진다.**
	// 값은 최대인데 얼굴은 굳는 — 사용자가 본 "눈이 항상 찌그러져 있다"가 정확히 이 상태다.
	//
	//   덧셈:     0.2→0.6 (솔버) + 0.7 = 0.9→1.0(잘림)   변화 폭 0.1, 위쪽은 소실
	//   여유분:   0.2→0.6 (솔버) ⊕ 0.7 = 0.76→0.88       변화 폭 0.12, 잘림 없음
	//
	// 스크린 합성과 같은 식이라 **절대 포화하지 않고 순서도 보존된다.** 표정은 그대로 실리고
	// 솔버의 변화만 압축된다. 0 으로 끄면 예전(덧셈) 동작.
	const bool bHeadroom = CVarEarlExprHeadroomBlend.GetValueOnAnyThread() != 0;
	int32 Saturated = 0;
	for (const TPair<FString, float>& Delta : RawExpressionDelta)
	{
		if (float* Existing = Animation.AnimationData.Find(Delta.Key))
		{
			const float Base = *Existing;
			// 포화 감시는 **덧셈이었다면 잘렸을까**를 센다 — 켜고 끄고 비교가 되도록.
			if (Base + Delta.Value > 1.0f && Base > KINDA_SMALL_NUMBER)
			{
				++Saturated;
			}
			const float Result = bHeadroom
				? (Delta.Value >= 0.0f
					? Base + Delta.Value * (1.0f - Base)   // 위로는 남은 여유분만큼만
					: Base + Delta.Value * Base)           // 아래로는 남은 값에 비례해
				: Base + Delta.Value;
			*Existing = FMath::Clamp(Result, 0.0f, 1.0f);
			++Applied;
		}
	}

	if (Saturated > 0)
	{
		SaturatedFrames += Saturated;
	}
	{
		const double NowSat = FPlatformTime::Seconds();
		if (SaturationWindowStart <= 0.0)
		{
			SaturationWindowStart = NowSat;
		}
		else if (NowSat - SaturationWindowStart >= 2.0)
		{
			if (SaturatedFrames > 0)
			{
				// 이 카운터는 "**덧셈이었다면** 잘렸을 횟수"다. 여유분 합성이 켜져 있으면
				// 실제로는 안 잘렸다는 뜻이므로 경고가 아니라 정보다.
				if (bHeadroom)
				{
					UE_LOG(LogEarlLipSync, Log,
						TEXT("표정/솔버 겹침: 최근 2초에 %d회 — 여유분 합성이 받아냈다(포화 없음). "
							 "예전 덧셈 방식이었다면 그만큼 솔버 변화가 잘렸을 구간이다."),
						SaturatedFrames);
				}
				else
				{
					UE_LOG(LogEarlLipSync, Warning,
						TEXT("표정 층 포화: 최근 2초에 %d회 — 솔버가 움직이던 커브를 우리 값이 1.0 에서 "
							 "잘라먹고 있다. `earl.ExprHeadroom 1` 로 켤 것."),
						SaturatedFrames);
				}
			}
			SaturatedFrames = 0;
			SaturationWindowStart = NowSat;
		}
	}

	// 1회 진단 — ①0 이면 GUI→raw 매핑이 틀린 것 ②이름 목록으로 어디를 건드리는지 검증한다.
	// ⚠️ 예전엔 "하반부 침범 = 0 이어야 한다"가 불변식이었으나, 2026-08-05 부터 **입가 표정을
	//    의도적으로 넣는다**(눈썹만으로는 감정이 안 읽힘). 그래서 판정 기준이 바뀌었다:
	//      · articulation(jaw/funnel/purse/lipsTogether/stretch) = **여전히 0 이어야 한다**
	//      · 입꼬리·보조개·윗입술 = 의도된 것이므로 세기만 하고 경고하지 않는다
	//    Articulation 이 0 이 아니면 립싱크가 뭉개진다 — 그때는 감정 표를 되돌릴 것.
	if (!bLoggedExpressionMatch)
	{
		bLoggedExpressionMatch = true;
		TArray<FString> Names;
		int32 MouthExpression = 0;
		int32 Articulation = 0;
		for (const TPair<FString, float>& Delta : RawExpressionDelta)
		{
			Names.Add(FString::Printf(TEXT("%s=%+.3f"), *Delta.Key, Delta.Value));
			const FString Lower = Delta.Key.ToLower();
			if (Lower.Contains(TEXT("jaw")) || Lower.Contains(TEXT("funnel")) ||
				Lower.Contains(TEXT("purse")) || Lower.Contains(TEXT("lipstogether")) ||
				Lower.Contains(TEXT("stretch")))
			{
				++Articulation;
			}
			else if (Lower.Contains(TEXT("mouth")) || Lower.Contains(TEXT("lip")))
			{
				++MouthExpression;
			}
		}
		Names.Sort();
		UE_LOG(LogEarlLipSync, Log,
			TEXT("Expression delta applied: %d/%d raw controls matched (frame has %d). "
				 "Mouth-expression (corner/dimple/upperLip) = %d (intentional), "
				 "articulation (jaw/funnel/purse/together/stretch) = %d (MUST be 0). Controls: %s"),
			Applied, RawExpressionDelta.Num(), Animation.AnimationData.Num(),
			MouthExpression, Articulation, *FString::Join(Names, TEXT(", ")));
	}
}

void FEarlTTSLiveLinkSubject::MediaSamplerMain()
{
	// PCM is supplied by UEarlClientComponent. Keep the sampler thread alive so
	// the base class start/stop lifecycle remains identical to Epic's audio subject.
	while (IsRunning())
	{
		FPlatformProcess::Sleep(0.02f);
	}
}

FEarlProceduralLiveLinkSubject::FEarlProceduralLiveLinkSubject(
	ILiveLinkClient* InLiveLinkClient,
	const FGuid& InSourceGuid,
	const FName& InSubjectName,
	UMetaHumanLocalLiveLinkSubjectSettings* InSettings)
	: FMetaHumanLocalLiveLinkSubject(
		InLiveLinkClient,
		InSourceGuid,
		InSubjectName,
		InSettings)
	, ProceduralLiveLinkClient(InLiveLinkClient)
	, ProceduralSourceGuid(InSourceGuid)
	, ProceduralSubjectName(InSubjectName)
{
	PropertyNames = {
		TEXT("CTRL_expressions_eyeBlinkL"),
		TEXT("CTRL_expressions_eyeBlinkR"),
		TEXT("CTRL_expressions_eyeCheekRaiseL"),
		TEXT("CTRL_expressions_eyeCheekRaiseR"),
		TEXT("CTRL_expressions_mouthUpperLipRaiseL"),
		TEXT("CTRL_expressions_mouthUpperLipRaiseR"),
		TEXT("CTRL_expressions_mouthLowerLipDepressL"),
		TEXT("CTRL_expressions_mouthLowerLipDepressR"),
		TEXT("CTRL_expressions_mouthCornerPullL"),
		TEXT("CTRL_expressions_mouthCornerPullR"),
		TEXT("CTRL_expressions_mouthStretchL"),
		TEXT("CTRL_expressions_mouthStretchR"),
		TEXT("CTRL_expressions_mouthLipsPurseUL"),
		TEXT("CTRL_expressions_mouthLipsPurseUR"),
		TEXT("CTRL_expressions_mouthLipsPurseDL"),
		TEXT("CTRL_expressions_mouthLipsPurseDR"),
		TEXT("CTRL_expressions_mouthFunnelUL"),
		TEXT("CTRL_expressions_mouthFunnelUR"),
		TEXT("CTRL_expressions_mouthFunnelDL"),
		TEXT("CTRL_expressions_mouthFunnelDR"),
		TEXT("CTRL_expressions_mouthLipsTogetherUL"),
		TEXT("CTRL_expressions_mouthLipsTogetherUR"),
		TEXT("CTRL_expressions_mouthLipsTogetherDL"),
		TEXT("CTRL_expressions_mouthLipsTogetherDR"),
		TEXT("CTRL_expressions_jawOpen"),
		TEXT("CTRL_expressions_jawOpenExtreme"),
		TEXT("HeadControlSwitch"),
		TEXT("HeadRoll"),
		TEXT("HeadPitch"),
		TEXT("HeadYaw"),
		TEXT("HeadTranslationX"),
		TEXT("HeadTranslationY"),
		TEXT("HeadTranslationZ"),
		TEXT("MHFDSVersion"),
		TEXT("DisableFaceOverride")
	};
}

void FEarlProceduralLiveLinkSubject::Start()
{
	bStopped = false;
	PushStaticData();

	TArray<float> NeutralFrame;
	NeutralFrame.SetNumZeroed(320);
	PushFrame(NeutralFrame.GetData(), NeutralFrame.Num(), 1);
	if (CVarEarlLipSyncTestOnStart.GetValueOnAnyThread() != 0)
	{
		TArray<float> TestFrame;
		TestFrame.SetNumUninitialized(320);
		for (int32 Index = 0; Index < TestFrame.Num(); ++Index)
		{
			const float Time = static_cast<float>(Index) / 16000.0f;
			TestFrame[Index] = 0.075f * FMath::Sin(2.0f * Pi * 220.0f * Time) +
				0.035f * FMath::Sin(2.0f * Pi * 1600.0f * Time);
		}
		PushFrame(TestFrame.GetData(), TestFrame.Num(), 1);
		UE_LOG(LogEarlLipSync, Log, TEXT("Procedural lip-sync diagnostic speech frame pushed"));
	}
	UE_LOG(LogEarlLipSync, Log,
		TEXT("Procedural MetaHuman Live Link subject %s started with %d curves"),
		*ProceduralSubjectName.ToString(), PropertyNames.Num());
}

void FEarlProceduralLiveLinkSubject::Stop()
{
	bStopped = true;
}

void FEarlProceduralLiveLinkSubject::ExtractPipelineData(
	TSharedPtr<UE::MetaHuman::Pipeline::FPipelineData> InPipelineData)
{
}

void FEarlProceduralLiveLinkSubject::PushStaticData()
{
	if (bStopped || !ProceduralLiveLinkClient)
	{
		return;
	}

	FLiveLinkStaticDataStruct StaticDataStruct(FLiveLinkBaseStaticData::StaticStruct());
	FLiveLinkBaseStaticData& LiveLinkStaticData = *StaticDataStruct.Cast<FLiveLinkBaseStaticData>();
	LiveLinkStaticData.PropertyNames = PropertyNames;
	ProceduralLiveLinkClient->PushSubjectStaticData_AnyThread(
		{ProceduralSourceGuid, ProceduralSubjectName},
		ULiveLinkBasicRole::StaticClass(),
		MoveTemp(StaticDataStruct));
}

void FEarlProceduralLiveLinkSubject::PushPcm(
	const TArray<float>& InSamples,
	const int32 InSampleRate,
	const int32 InNumChannels)
{
	if (bStopped || InSamples.IsEmpty() || InSampleRate <= 0 || InNumChannels <= 0)
	{
		return;
	}

	const int32 SamplesPerFrame = FMath::Max(1, InSampleRate / 50);
	const int32 TotalSamplesPerChannel = InSamples.Num() / InNumChannels;
	for (int32 Offset = 0; Offset < TotalSamplesPerChannel; Offset += SamplesPerFrame)
	{
		const int32 FrameSamples = FMath::Min(SamplesPerFrame, TotalSamplesPerChannel - Offset);
		PushFrame(InSamples.GetData() + Offset * InNumChannels, FrameSamples, InNumChannels);
	}
}

void FEarlProceduralLiveLinkSubject::ResetToNeutral()
{
	SmoothedOpen = 0.0f;
	TArray<float> NeutralFrame;
	NeutralFrame.SetNumZeroed(320);
	PushFrame(NeutralFrame.GetData(), NeutralFrame.Num(), 1);
}

void FEarlProceduralLiveLinkSubject::PushFrame(
	const float* InSamples,
	const int32 InNumSamples,
	const int32 InNumChannels)
{
	if (bStopped || !ProceduralLiveLinkClient || !InSamples || InNumSamples <= 0 || InNumChannels <= 0)
	{
		return;
	}

	float SumSquares = 0.0f;
	float DifferenceSquares = 0.0f;
	float PreviousSample = 0.0f;
	for (int32 Index = 0; Index < InNumSamples; ++Index)
	{
		float Sample = 0.0f;
		for (int32 Channel = 0; Channel < InNumChannels; ++Channel)
		{
			Sample += InSamples[Index * InNumChannels + Channel];
		}
		Sample /= static_cast<float>(InNumChannels);
		SumSquares += Sample * Sample;
		if (Index > 0)
		{
			const float Difference = Sample - PreviousSample;
			DifferenceSquares += Difference * Difference;
		}
		PreviousSample = Sample;
	}

	const float Rms = FMath::Sqrt(SumSquares / static_cast<float>(InNumSamples));
	const float Activity = FMath::Clamp((Rms - SilenceRms) / (FullVoiceRms - SilenceRms), 0.0f, 1.0f);
	const float TargetOpen = FMath::Pow(Activity, 0.65f);
	const float Smoothing = TargetOpen > SmoothedOpen ? 0.68f : 0.38f;
	SmoothedOpen = FMath::Lerp(SmoothedOpen, TargetOpen, Smoothing);
	if (Activity < 0.01f)
	{
		SmoothedOpen = FMath::Max(0.0f, SmoothedOpen - 0.08f);
	}

	const float DifferenceRatio = SumSquares > KINDA_SMALL_NUMBER
		? FMath::Clamp(DifferenceSquares / (4.0f * SumSquares), 0.0f, 1.0f)
		: 0.0f;
	const int32 AnalysisSampleRate = InNumSamples * 50;
	const float LowPower =
		CalculateFrequencyPower(InSamples, InNumSamples, InNumChannels, AnalysisSampleRate, 350.0f) +
		CalculateFrequencyPower(InSamples, InNumSamples, InNumChannels, AnalysisSampleRate, 700.0f);
	const float HighPower =
		CalculateFrequencyPower(InSamples, InNumSamples, InNumChannels, AnalysisSampleRate, 1600.0f) +
		CalculateFrequencyPower(InSamples, InNumSamples, InNumChannels, AnalysisSampleRate, 2600.0f);
	const float SpectralTotal = LowPower + HighPower + KINDA_SMALL_NUMBER;
	const float WideShape = FMath::Clamp(
		0.65f * HighPower / SpectralTotal + 0.35f * DifferenceRatio,
		0.0f, 1.0f);
	const float RoundShape = FMath::Clamp(1.0f - WideShape, 0.0f, 1.0f);
	const float Open = SmoothedOpen;
	const float BlinkPhase = static_cast<float>(FrameCounter % 190);
	const float Blink = BlinkPhase == 0.0f ? 0.45f : (BlinkPhase == 1.0f ? 1.0f : (BlinkPhase == 2.0f ? 0.45f : 0.0f));
	const float ConsonantClosure = Activity * DifferenceRatio * FMath::Clamp(1.0f - Open, 0.0f, 1.0f) * 0.35f;

	TMap<FName, float> CurveValues;
	CurveValues.Reserve(PropertyNames.Num());
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

	FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkBaseFrameData::StaticStruct());
	FLiveLinkBaseFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkBaseFrameData>();
	FrameData.PropertyValues.Reserve(PropertyNames.Num());
	for (const FName& PropertyName : PropertyNames)
	{
		FrameData.PropertyValues.Add(CurveValues.FindRef(PropertyName));
	}
	FrameData.MetaData.StringMetaData.Add(TEXT("IsNeutralFrame"), Activity < 0.01f ? TEXT("true") : TEXT("false"));
	FrameData.MetaData.StringMetaData.Add(TEXT("HeadPoseMode"), TEXT("0"));

	ProceduralLiveLinkClient->PushSubjectFrameData_AnyThread(
		{ProceduralSourceGuid, ProceduralSubjectName}, MoveTemp(FrameDataStruct));

	if (!bLoggedSpeech && Activity > 0.08f)
	{
		bLoggedSpeech = true;
		UE_LOG(LogEarlLipSync, Log,
			TEXT("Procedural lip-sync is producing speech frames: RMS=%.4f jaw=%.3f wide=%.3f round=%.3f"),
			Rms, Open * 0.72f, WideShape, RoundShape);
	}
	else if (FrameCounter > 0 && FrameCounter % 100 == 0)
	{
		UE_LOG(LogEarlLipSync, Verbose,
			TEXT("Procedural lip-sync frame %lld: RMS=%.4f jaw=%.3f"),
			FrameCounter, Rms, Open * 0.72f);
	}
	++FrameCounter;
}

FText FEarlSolverLiveLinkSource::GetSourceType() const
{
	return FText::FromString(TEXT("Earl TTS PCM"));
}

bool FEarlSolverLiveLinkSource::CreateEarlSubject(const FName& InSubjectName, const int32 InLookaheadMs, const float InMoodIntensity)
{
	if (!GetSourceGuid().IsValid())
	{
		return false;
	}

	bCreateProceduralSubject = PLATFORM_ANDROID || CVarEarlForceProceduralLipSync.GetValueOnGameThread() != 0;
	if (bCreateProceduralSubject)
	{
		UMetaHumanLocalLiveLinkSubjectSettings* SubjectSettings =
			CreateSubjectSettings<UMetaHumanLocalLiveLinkSubjectSettings>();
		UE_LOG(LogEarlLipSync, Log, TEXT("Using PCM-driven procedural lip-sync for %s"), *InSubjectName.ToString());
		return RequestSubjectCreation(InSubjectName.ToString(), SubjectSettings).Source.IsValid();
	}

	UMetaHumanAudioBaseLiveLinkSubjectSettings* SubjectSettings =
		CreateSubjectSettings<UMetaHumanAudioBaseLiveLinkSubjectSettings>();
	SubjectSettings->Lookahead = FMath::Clamp(InLookaheadMs, 80, 240);
	SubjectSettings->Mood = EAudioDrivenAnimationMood::AutoDetect;
	// 무드 1.0 은 표정이 입모양을 뭉갬 — 발음 우선 (표정 레이어는 감정 클립 블렌드 담당, ADR 010)
	SubjectSettings->MoodIntensity = FMath::Clamp(InMoodIntensity, 0.0f, 1.0f);
	// ⚠️ 엔진 프레임 스무딩 프리프로세서(UMetaHumanSmoothingPreProcessor)를 여기 붙여 봤다가 **뺐다**
	// (2026-08-05). 얼굴 떨림의 출처가 솔버가 아니었기 때문이다 — 한 런 안에서 층을 하나씩 끄고 재니
	// 표정·머리를 다 끈 솔버 단독이 0.199(정지 바닥 0.066)로 이미 조용했고, 표정을 켜면 1.255 로
	// 세 배가 됐다. 즉 떨림은 **표정 층**이 만들고 있었다(UEarlEmotionComponent 의 클립 샘플링).
	// 붙여 봐야 이득이 없고 조음에 지연만 줄 수 있어 넣지 않는다.
	// 부활 조건: 솔버 단독 수치가 정지 메시 수준을 크게 넘을 때.

	UE_LOG(LogEarlLipSync, Log, TEXT("Using MetaHuman NNE audio solver for %s (lookahead=%dms mood=%.2f)"),
		*InSubjectName.ToString(), SubjectSettings->Lookahead, SubjectSettings->MoodIntensity);
	return RequestSubjectCreation(InSubjectName.ToString(), SubjectSettings).Source.IsValid();
}

void FEarlSolverLiveLinkSource::PushPcm(
	const TArray<float>& InSamples,
	const int32 InSampleRate,
	const int32 InNumChannels)
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->PushPcm(InSamples, InSampleRate, InNumChannels);
	}
	else if (const TSharedPtr<FEarlProceduralLiveLinkSubject> CurveSubject = ProceduralSubject.Pin())
	{
		CurveSubject->PushPcm(InSamples, InSampleRate, InNumChannels);
	}
}

void FEarlSolverLiveLinkSource::SetHeadMotion(float InAmplitudeDeg, float InSpeed)
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->SetHeadMotion(InAmplitudeDeg, InSpeed);
	}
}

void FEarlSolverLiveLinkSource::SetIdleMouthDamp(bool bInIdle, float InDamp)
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->SetIdleMouthDamp(bInIdle, InDamp);
	}
}

void FEarlSolverLiveLinkSource::SetMouthGain(float InGain)
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->SetMouthGain(InGain);
	}
}

void FEarlSolverLiveLinkSource::SetExpressionCurves(const TMap<FString, float>& InGuiCurves)
{
	LastGuiCurves = InGuiCurves;
	RebuildExpressionDelta();
}

void FEarlSolverLiveLinkSource::SetProceduralCurves(const TMap<FString, float>& InGuiCurves)
{
	// 시선·강세는 표정과 **다른 레이어**다 — 표정 스무딩(200ms)을 타면 사케이드도 눈썹 플래시도
	// 뭉개진다. 그래서 여기로 따로 들어오고, 합치는 것은 아래 변환 직전에 한다.
	LastProceduralCurves = InGuiCurves;
	RebuildExpressionDelta();
}

void FEarlSolverLiveLinkSource::GetArticulation(float& OutMean, float& OutPeak, int32& OutSamples) const
{
	OutMean = 0.0f; OutPeak = 0.0f; OutSamples = 0;
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->GetArticulation(OutMean, OutPeak, OutSamples);
	}
}

void FEarlSolverLiveLinkSource::ResetArticulation()
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->ResetArticulation();
	}
}

void FEarlSolverLiveLinkSource::SetHeadFollow(float InYawDeg, float InPitchDeg, float InRollDeg)
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->SetHeadFollow(InYawDeg, InPitchDeg, InRollDeg);
	}
}

void FEarlSolverLiveLinkSource::GetHeadMotionAngles(float& OutYawDeg, float& OutPitchDeg) const
{
	OutYawDeg = 0.0f;
	OutPitchDeg = 0.0f;
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->GetHeadMotionAngles(OutYawDeg, OutPitchDeg);
	}
}

void FEarlSolverLiveLinkSource::SetHeadNod(float InNodDeg, float InDurationSec)
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->SetHeadNod(InNodDeg, InDurationSec);
	}
}

void FEarlSolverLiveLinkSource::TriggerHeadNod(float InStrength)
{
	if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		AudioSubject->TriggerHeadNod(InStrength);
	}
}

void FEarlSolverLiveLinkSource::RebuildExpressionDelta()
{
	const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin();
	if (!AudioSubject)
	{
		return; // 절차적 폴백 — 솔버 프레임이 없으므로 가산 대상도 없다
	}

	// 표정(감정 클립) + 절차적(시선·강세 눈썹)을 하나의 GUI 맵으로 합친다.
	// ⚠️ **더한다, 덮어쓰지 않는다.** 눈썹은 두 레이어가 같은 키를 쓴다 — 감정 표정의 눈썹 위에
	// 강세 플래시가 얹혀야지, 한쪽이 다른 쪽을 지우면 안 된다. 둘 다 가산 기여이므로 합이 맞다.
	TMap<FString, float> Merged = LastGuiCurves;
	for (const TPair<FString, float>& Layer : LastProceduralCurves)
	{
		float& Value = Merged.FindOrAdd(Layer.Key, 0.0f);
		Value = FMath::Clamp(Value + Layer.Value, -1.0f, 1.0f);
	}

	// ⚠️ 이 함수는 **매 틱** 불린다. 변환은 174 GUI → 251 raw 를 펼치며 맵을 새로 할당하므로
	// 값이 사실상 그대로일 때 다시 도는 것은 순수 낭비다(중립일 때도 빈 맵으로 매 프레임 돌았다).
	// 표정은 어택 0.35s / 릴리즈 0.7s 로 천천히 움직이므로, 의미 있는 변화가 없으면 건너뛴다.
	// (시선 사케이드는 임계 0.002 를 훌쩍 넘으므로 이 게이트에 걸리지 않는다)
	bool bChanged = Merged.Num() != LastMergedCurves.Num();
	if (!bChanged)
	{
		for (const TPair<FString, float>& Cur : Merged)
		{
			const float* Prev = LastMergedCurves.Find(Cur.Key);
			if (!Prev || FMath::Abs(*Prev - Cur.Value) > 0.002f)
			{
				bChanged = true;
				break;
			}
		}
	}
	if (!bChanged)
	{
		return;
	}
	LastMergedCurves = Merged;

	// 엔진 유틸이 GUI 컨트롤을 raw 컨트롤 **전체 집합**으로 펼쳐 준다(입력에 없는 키도 포함).
	// 그래서 "중립 기준선"을 한 번 구해 두고 매번 그 차이만 넘긴다 — 절대값을 넘기면
	// 솔버가 만든 상단 얼굴을 통째로 덮어써 표정이 아니라 마스크가 된다.
	static const TMap<FString, float> NeutralRaw =
		GuiToRawControlsUtils::ConvertGuiToRawControls(TMap<FString, float>{});

	const TMap<FString, float> TargetRaw = GuiToRawControlsUtils::ConvertGuiToRawControls(Merged);

	TMap<FString, float> Delta;
	Delta.Reserve(TargetRaw.Num());
	for (const TPair<FString, float>& Target : TargetRaw)
	{
		const float* Neutral = NeutralRaw.Find(Target.Key);
		const float Diff = Target.Value - (Neutral ? *Neutral : 0.0f);
		if (FMath::Abs(Diff) > KINDA_SMALL_NUMBER)
		{
			Delta.Add(Target.Key, Diff);
		}
	}

	AudioSubject->SetRawExpressionDelta(MoveTemp(Delta));
}

void FEarlSolverLiveLinkSource::ResetToNeutral()
{
	if (const TSharedPtr<FEarlProceduralLiveLinkSubject> CurveSubject = ProceduralSubject.Pin())
	{
		CurveSubject->ResetToNeutral();
	}
	else if (const TSharedPtr<FEarlTTSLiveLinkSubject> AudioSubject = EarlSubject.Pin())
	{
		TArray<float> Silence;
		Silence.SetNumZeroed(2560);
		AudioSubject->PushPcm(Silence, 16000, 1);
	}
}

TSharedPtr<FMetaHumanLocalLiveLinkSubject> FEarlSolverLiveLinkSource::CreateSubject(
	const FName& InSubjectName,
	UMetaHumanLocalLiveLinkSubjectSettings* InSettings)
{
	if (bCreateProceduralSubject)
	{
		const TSharedPtr<FEarlProceduralLiveLinkSubject> Subject =
			MakeShared<FEarlProceduralLiveLinkSubject>(
				LiveLinkClient,
				SourceGuid,
				InSubjectName,
				InSettings);
		ProceduralSubject = Subject;
		return Subject;
	}

	UMetaHumanAudioBaseLiveLinkSubjectSettings* AudioSettings =
		Cast<UMetaHumanAudioBaseLiveLinkSubjectSettings>(InSettings);
	if (!AudioSettings)
	{
		return nullptr;
	}

	const TSharedPtr<FEarlTTSLiveLinkSubject> Subject = MakeShared<FEarlTTSLiveLinkSubject>(
		LiveLinkClient,
		SourceGuid,
		InSubjectName,
		AudioSettings);
	EarlSubject = Subject;
	return Subject;
}

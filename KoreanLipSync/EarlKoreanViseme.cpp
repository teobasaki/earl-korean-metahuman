#include "LipSync/EarlKoreanViseme.h"
#include "LipSync/EarlLipSyncSource.h"

namespace
{
	using namespace EarlKoreanViseme;

	// 초성 19: ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ (ㅇ=무음가 → Rest)
	constexpr EViseme OnsetMap[19] = {
		EViseme::Velar, EViseme::Velar, EViseme::Alveolar, EViseme::Alveolar, EViseme::Alveolar,
		EViseme::Alveolar, EViseme::Bilabial, EViseme::Bilabial, EViseme::Bilabial, EViseme::Sibilant,
		EViseme::Sibilant, EViseme::Rest, EViseme::Sibilant, EViseme::Sibilant, EViseme::Sibilant,
		EViseme::Velar, EViseme::Alveolar, EViseme::Bilabial, EViseme::Glottal
	};

	// 중성 21: ㅏㅐㅑㅒㅓㅔㅕㅖㅗㅘㅙㅚㅛㅜㅝㅞㅟㅠㅡㅢㅣ (이중모음은 지배 모음으로 근사)
	constexpr EViseme VowelMap[21] = {
		EViseme::A, EViseme::EO, EViseme::A, EViseme::EO, EViseme::EO,
		EViseme::EO, EViseme::EO, EViseme::EO, EViseme::O, EViseme::A,
		EViseme::EO, EViseme::EO, EViseme::O, EViseme::U, EViseme::EO,
		EViseme::EO, EViseme::I, EViseme::U, EViseme::EU, EViseme::I,
		EViseme::I
	};

	// 종성 28 (0=없음). 받침 ㅅ/ㅈ/ㅊ 은 ㄷ 소리 → Alveolar, 받침 ㅇ → Velar(ng)
	constexpr EViseme CodaMap[28] = {
		EViseme::Rest, EViseme::Velar, EViseme::Velar, EViseme::Velar, EViseme::Alveolar,
		EViseme::Alveolar, EViseme::Alveolar, EViseme::Alveolar, EViseme::Alveolar, EViseme::Velar,
		EViseme::Bilabial, EViseme::Alveolar, EViseme::Alveolar, EViseme::Alveolar, EViseme::Bilabial,
		EViseme::Alveolar, EViseme::Bilabial, EViseme::Bilabial, EViseme::Bilabial, EViseme::Alveolar,
		EViseme::Alveolar, EViseme::Velar, EViseme::Alveolar, EViseme::Alveolar, EViseme::Velar,
		EViseme::Alveolar, EViseme::Bilabial, EViseme::Glottal
	};

	constexpr float SilenceRms = 0.006f;
	constexpr float FullVoiceRms = 0.12f;

	void SetLR(TMap<FName, float>& M, const TCHAR* Base, float V)
	{
		M.Add(FName(FString::Printf(TEXT("CTRL_expressions_%sL"), Base)), V);
		M.Add(FName(FString::Printf(TEXT("CTRL_expressions_%sR"), Base)), V);
	}
	void Set4(TMap<FName, float>& M, const TCHAR* Base, float VU, float VD)
	{
		M.Add(FName(FString::Printf(TEXT("CTRL_expressions_%sUL"), Base)), VU);
		M.Add(FName(FString::Printf(TEXT("CTRL_expressions_%sUR"), Base)), VU);
		M.Add(FName(FString::Printf(TEXT("CTRL_expressions_%sDL"), Base)), VD);
		M.Add(FName(FString::Printf(TEXT("CTRL_expressions_%sDR"), Base)), VD);
	}
}

namespace EarlKoreanViseme
{

TArray<FSyllable> Decompose(const FString& Text)
{
	TArray<FSyllable> Out;
	for (const TCHAR C : Text)
	{
		const int32 Code = static_cast<int32>(C);
		if (Code < 0xAC00 || Code > 0xD7A3)
		{
			continue; // 한글 음절 외 문자(라틴·숫자·문장부호)는 v1 에서 건너뜀
		}
		const int32 Idx = Code - 0xAC00;
		FSyllable S;
		S.Onset = OnsetMap[Idx / 588];
		S.Vowel = VowelMap[(Idx % 588) / 28];
		S.Coda = CodaMap[Idx % 28];
		Out.Add(S);
	}
	return Out;
}

TMap<FName, float> VisemePose(EViseme Viseme)
{
	TMap<FName, float> M;
	M.Add(TEXT("CTRL_expressions_jawOpen"), 0.0f);
	switch (Viseme)
	{
	case EViseme::A:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.75f);
		SetLR(M, TEXT("mouthLowerLipDepress"), 0.35f);
		SetLR(M, TEXT("mouthUpperLipRaise"), 0.18f);
		break;
	case EViseme::EO:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.5f);
		SetLR(M, TEXT("mouthLowerLipDepress"), 0.28f);
		SetLR(M, TEXT("mouthStretch"), 0.15f);
		break;
	case EViseme::O:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.42f);
		Set4(M, TEXT("mouthFunnel"), 0.5f, 0.45f);
		Set4(M, TEXT("mouthLipsPurse"), 0.3f, 0.25f);
		break;
	case EViseme::U:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.28f);
		Set4(M, TEXT("mouthFunnel"), 0.45f, 0.4f);
		Set4(M, TEXT("mouthLipsPurse"), 0.55f, 0.5f);
		break;
	case EViseme::EU:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.2f);
		SetLR(M, TEXT("mouthStretch"), 0.35f);
		SetLR(M, TEXT("mouthCornerPull"), 0.15f);
		break;
	case EViseme::I:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.28f);
		SetLR(M, TEXT("mouthStretch"), 0.5f);
		SetLR(M, TEXT("mouthCornerPull"), 0.35f);
		SetLR(M, TEXT("mouthUpperLipRaise"), 0.22f);
		break;
	case EViseme::Bilabial:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.06f);
		Set4(M, TEXT("mouthLipsTogether"), 0.85f, 0.85f);
		break;
	case EViseme::Sibilant:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.15f);
		SetLR(M, TEXT("mouthStretch"), 0.45f);
		SetLR(M, TEXT("mouthCornerPull"), 0.3f);
		SetLR(M, TEXT("mouthUpperLipRaise"), 0.3f);
		Set4(M, TEXT("mouthLipsTogether"), 0.2f, 0.2f);
		break;
	case EViseme::Alveolar:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.22f);
		SetLR(M, TEXT("mouthStretch"), 0.2f);
		Set4(M, TEXT("mouthLipsTogether"), 0.15f, 0.15f);
		break;
	case EViseme::Velar:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.3f);
		break;
	case EViseme::Glottal:
		M.Add(TEXT("CTRL_expressions_jawOpen"), 0.4f);
		break;
	case EViseme::Rest:
	default:
		break;
	}
	return M;
}

void ComputeFrameFeatures(const TArray<float>& Mono, int32 SampleRate,
	TArray<float>& OutActivity, TArray<float>& OutHFRatio)
{
	const int32 Hop = FMath::Max(1, SampleRate / 50); // 20ms
	const int32 NumFrames = Mono.Num() / Hop;
	OutActivity.SetNumZeroed(NumFrames);
	OutHFRatio.SetNumZeroed(NumFrames);
	const float Nyquist = SampleRate * 0.5f;

	for (int32 F = 0; F < NumFrames; ++F)
	{
		const float* S = Mono.GetData() + F * Hop;
		float SumSq = 0.0f;
		for (int32 i = 0; i < Hop; ++i)
		{
			SumSq += S[i] * S[i];
		}
		const float Rms = FMath::Sqrt(SumSq / Hop);
		OutActivity[F] = FMath::Clamp((Rms - SilenceRms) / (FullVoiceRms - SilenceRms), 0.0f, 1.0f);

		// 치찰 고역비: 5.5k/7k vs 0.4k/0.8k (Goertzel). SR 이 낮으면 가능한 대역만.
		auto Power = [&](float Freq) -> float
		{
			if (Freq >= Nyquist * 0.95f) { return 0.0f; }
			const float W = 2.0f * PI * Freq / SampleRate;
			const float Coeff = 2.0f * FMath::Cos(W);
			float P1 = 0.0f, P2 = 0.0f;
			for (int32 i = 0; i < Hop; ++i)
			{
				const float Cur = S[i] + Coeff * P1 - P2;
				P2 = P1; P1 = Cur;
			}
			return P1 * P1 + P2 * P2 - Coeff * P1 * P2;
		};
		const float High = Power(5500.0f) + Power(7000.0f);
		const float Low = Power(400.0f) + Power(800.0f);
		OutHFRatio[F] = High / (High + Low + KINDA_SMALL_NUMBER);
	}

	// 3프레임 박스 스무딩 (피크 검출 안정화)
	TArray<float> Smoothed = OutActivity;
	for (int32 F = 1; F + 1 < NumFrames; ++F)
	{
		Smoothed[F] = (OutActivity[F - 1] + OutActivity[F] + OutActivity[F + 1]) / 3.0f;
	}
	OutActivity = MoveTemp(Smoothed);
}

TArray<FAlignedSyllable> AlignSyllables(const TArray<FSyllable>& Syllables,
	const TArray<float>& Activity, const TArray<float>& HFRatio)
{
	TArray<FAlignedSyllable> Out;
	const int32 NumFrames = Activity.Num();
	const int32 N = Syllables.Num();
	if (NumFrames <= 0 || N <= 0)
	{
		return Out;
	}

	int32 FirstVoiced = -1, LastVoiced = -1;
	for (int32 F = 0; F < NumFrames; ++F)
	{
		if (Activity[F] > 0.02f)
		{
			if (FirstVoiced < 0) { FirstVoiced = F; }
			LastVoiced = F;
		}
	}
	if (FirstVoiced < 0)
	{
		return Out;
	}

	// ① 에너지 피크(음절 핵 후보): 국소 최대 + 최소 높이 0.1 + 최소 간격 4프레임(80ms)
	TArray<int32> Peaks;
	for (int32 F = FMath::Max(FirstVoiced, 1); F <= FMath::Min(LastVoiced, NumFrames - 2); ++F)
	{
		if (Activity[F] < 0.1f || Activity[F] < Activity[F - 1] || Activity[F] < Activity[F + 1])
		{
			continue;
		}
		if (!Peaks.IsEmpty() && F - Peaks.Last() < 4)
		{
			if (Activity[F] > Activity[Peaks.Last()]) { Peaks.Last() = F; } // 더 높은 쪽 유지
			continue;
		}
		Peaks.Add(F);
	}
	if (Peaks.IsEmpty())
	{
		Peaks.Add((FirstVoiced + LastVoiced) / 2);
	}
	const int32 M = Peaks.Num();

	// ② 음절 i → 피크 단조 배정 (비율 매핑 — 피크가 남으면 건너뛰고 모자라면 공유)
	TArray<int32> AssignedPeak;
	AssignedPeak.SetNumUninitialized(N);
	for (int32 i = 0; i < N; ++i)
	{
		AssignedPeak[i] = Peaks[FMath::Clamp(i * M / N, 0, M - 1)];
	}

	// ③ 경계: 서로 다른 피크 사이 = 에너지 최저점 / 같은 피크 공유 구간 = 균등 분할
	auto ValleyBetween = [&](int32 A, int32 B) -> int32
	{
		int32 Best = A;
		for (int32 F = A; F <= B; ++F)
		{
			if (Activity[F] < Activity[Best]) { Best = F; }
		}
		return Best;
	};
	TArray<int32> Bounds; // N+1 개
	Bounds.Add(FirstVoiced);
	for (int32 i = 1; i < N; ++i)
	{
		if (AssignedPeak[i] != AssignedPeak[i - 1])
		{
			Bounds.Add(ValleyBetween(AssignedPeak[i - 1], AssignedPeak[i]));
		}
		else
		{
			Bounds.Add(-1); // 공유 — 아래에서 균등 분할
		}
	}
	Bounds.Add(LastVoiced + 1);
	for (int32 i = 1; i < N; ++i) // 공유 경계 균등 분할
	{
		if (Bounds[i] >= 0) { continue; }
		int32 Lo = i - 1;
		while (Bounds[Lo] < 0) { --Lo; }
		int32 Hi = i;
		while (Bounds[Hi] < 0) { ++Hi; }
		for (int32 j = Lo + 1; j < Hi; ++j)
		{
			Bounds[j] = Bounds[Lo] + (Bounds[Hi] - Bounds[Lo]) * (j - Lo) / (Hi - Lo);
		}
	}

	// ④ 치찰음 온셋 스냅: 시작 경계 ±4프레임에서 고역비 최대 프레임으로 이동
	Out.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		FAlignedSyllable& A = Out[i];
		A.StartFrame = Bounds[i];
		A.EndFrame = FMath::Max(Bounds[i + 1], Bounds[i] + 1);
		A.PeakFrame = FMath::Clamp(AssignedPeak[i], A.StartFrame, A.EndFrame - 1);

		if (Syllables[i].Onset == EViseme::Sibilant && HFRatio.Num() == NumFrames)
		{
			const int32 Lo = FMath::Max(A.StartFrame - 4, 0);
			const int32 Hi = FMath::Min(A.StartFrame + 4, NumFrames - 1);
			int32 Best = A.StartFrame;
			for (int32 F = Lo; F <= Hi; ++F)
			{
				if (HFRatio[F] > HFRatio[Best]) { Best = F; }
			}
			A.StartFrame = FMath::Min(Best, A.PeakFrame); // 피크를 넘지 않게
			if (i > 0) { Out[i - 1].EndFrame = FMath::Max(A.StartFrame, Out[i - 1].StartFrame + 1); }
		}
	}
	return Out;
}

TArray<TMap<FName, float>> BuildTrack(const TArray<FSyllable>& Syllables, const TArray<float>& Mono, int32 SampleRate)
{
	TArray<TMap<FName, float>> Track;
	if (SampleRate <= 0)
	{
		return Track;
	}
	TArray<float> Activity, HFRatio;
	ComputeFrameFeatures(Mono, SampleRate, Activity, HFRatio);
	const int32 NumFrames = Activity.Num();
	if (NumFrames <= 0)
	{
		return Track;
	}

	const TArray<FAlignedSyllable> Aligned = AlignSyllables(Syllables, Activity, HFRatio);
	if (Aligned.IsEmpty())
	{
		Track.SetNum(NumFrames); // 전부 중립
		return Track;
	}

	// 프레임 → 비짐: 정렬 구간 기반. 온셋 = [Start, Start+(Peak-Start)*0.7], 종성 = 말미 40%
	auto VisemeAtFrame = [&](int32 F) -> EViseme
	{
		for (int32 i = 0; i < Aligned.Num(); ++i)
		{
			const FAlignedSyllable& A = Aligned[i];
			if (F < A.StartFrame || F >= A.EndFrame)
			{
				continue;
			}
			const FSyllable& Syll = Syllables[i];
			const bool bHasOnset = Syll.Onset != EViseme::Rest;
			const bool bHasCoda = Syll.Coda != EViseme::Rest;
			const int32 OnsetEnd = bHasOnset
				? A.StartFrame + FMath::Max(1, static_cast<int32>((A.PeakFrame - A.StartFrame) * 0.7f)) : A.StartFrame;
			const int32 CodaStart = bHasCoda
				? A.EndFrame - FMath::Max(1, static_cast<int32>((A.EndFrame - A.PeakFrame) * 0.4f)) : A.EndFrame;
			if (F < OnsetEnd) { return Syll.Onset; }
			if (F < CodaStart) { return Syll.Vowel; }
			return Syll.Coda;
		}
		return EViseme::Rest;
	};

	// 목표 포즈 → 지수 스무딩 (조음 연결, 상승 빠르게/하강 약간 느리게)
	Track.SetNum(NumFrames);
	TMap<FName, float> Current;
	for (int32 F = 0; F < NumFrames; ++F)
	{
		const TMap<FName, float> Target = VisemePose(VisemeAtFrame(F));
		const float Energy = FMath::Pow(Activity[F], 0.65f);
		// 모양 커브는 저에너지에서도 어느 정도 유지(입모양 가독성), 무음이면 소멸
		const float ShapeScale = Activity[F] < 0.01f ? 0.0f : (0.35f + 0.65f * Energy);

		// 대상·기존 커브 합집합에 대해 스무딩
		TSet<FName> Keys;
		for (const auto& P : Target) { Keys.Add(P.Key); }
		for (const auto& P : Current) { Keys.Add(P.Key); }
		static const FName JawName(TEXT("CTRL_expressions_jawOpen"));
		for (const FName& K : Keys)
		{
			const float TargetV = Target.FindRef(K) * (K == JawName ? Energy : ShapeScale);
			const float CurrentV = Current.FindRef(K);
			const float Alpha = TargetV > CurrentV ? 0.55f : 0.4f;
			Current.Add(K, FMath::Lerp(CurrentV, TargetV, Alpha));
		}
		Track[F] = Current;
	}
	return Track;
}

} // namespace EarlKoreanViseme

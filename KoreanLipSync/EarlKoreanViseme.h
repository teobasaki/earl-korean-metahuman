#pragma once

#include "CoreMinimal.h"

// 자체 한국어 음소(비짐) 립싱크 (2026-07-30, Q5' 미구매 유지 대안).
//
// 원리: 우리는 발화 "텍스트"를 이미 안다(문장 시계 페이로드) + 한글은 표음문자라
// 자모 분해(초성/중성/종성)가 곧 음소열이다 → 오디오 음소 인식 없이
// 입 모양(비짐)을 확정할 수 있다. 타이밍·강도만 오디오 RMS 엔벨로프에서 얻는다.
//
//   텍스트 ─(자모→비짐)─┐
//                      ├→ 20ms 커브 프레임 트랙 → FEarlLipSyncSource 발행
//   PCM ─(RMS 엔벨로프)─┘
//
// Epic NNE 솔버(음향 직결, 영어 중심)와 달리 ㅅ/ㅈ/ㅊ·ㅜ/ㅗ·ㅣ/ㅡ 의 입 모양이
// 추측이 아니라 확정값이다. 전부 순수 함수 — 자동화 테스트 대상.
namespace EarlKoreanViseme
{
	enum class EViseme : uint8
	{
		Rest,      // 무음/무형 (ㅇ 초성 포함)
		A,         // 아/야 — 크게 벌림
		EO,        // 어/에/여 — 중간 벌림
		O,         // 오/요 — 둥글게 중간
		U,         // 우/유 — 오므림
		EU,        // 으 — 좁게 옆으로
		I,         // 이 — 옆으로 당김(미소꼴)
		Bilabial,  // ㅂㅃㅍㅁ — 입술 붙임
		Sibilant,  // ㅅㅆㅈㅉㅊ — 이 모으고 입술 벌림 (한국어 핵심)
		Alveolar,  // ㄷㄸㅌㄴㄹ — 혀끝, 살짝 벌림
		Velar,     // ㄱㄲㅋ / 받침 ㅇ — 여린 벌림
		Glottal    // ㅎ — 벌린 채 숨
	};

	struct FSyllable
	{
		EViseme Onset = EViseme::Rest;
		EViseme Vowel = EViseme::A;
		EViseme Coda = EViseme::Rest;
	};

	// 한글 음절(U+AC00~D7A3)만 분해, 나머지 문자는 건너뜀
	EARLONE_API TArray<FSyllable> Decompose(const FString& Text);

	// 비짐 → 기준 커브 포즈 (CTRL_expressions_*, 에너지 스케일 전)
	EARLONE_API TMap<FName, float> VisemePose(EViseme Viseme);

	// 경량 강제 정렬 결과: 음절별 프레임 구간 (20ms 프레임 인덱스)
	struct FAlignedSyllable
	{
		int32 StartFrame = 0;
		int32 PeakFrame = 0; // 모음 핵 (에너지 피크)
		int32 EndFrame = 0;  // exclusive
	};

	// 경량 강제 정렬 (2026-07-30, 균등 배치의 표류 문제 해소):
	// 음절 핵(모음) = 에너지 피크라는 한국어 음절 구조를 이용해
	// ① 엔벨로프 피크 검출 ② 음절→피크 단조 배정 ③ 경계=피크 간 에너지 최저점
	// ④ 치찰음(ㅅㅈㅊ) 온셋은 고역(5.5~7kHz) 급증 프레임에 스냅.
	// Activity/HFRatio 는 프레임(20ms)당 값. 반환 크기 == 음절 수.
	EARLONE_API TArray<FAlignedSyllable> AlignSyllables(
		const TArray<FSyllable>& Syllables,
		const TArray<float>& Activity,
		const TArray<float>& HFRatio);

	// 프레임 특징 계산: 원음(임의 SR) → 20ms 프레임별 Activity(발화 활동도)·HFRatio(치찰 고역비)
	EARLONE_API void ComputeFrameFeatures(
		const TArray<float>& Mono, int32 SampleRate,
		TArray<float>& OutActivity, TArray<float>& OutHFRatio);

	// 문장 트랙 생성: 정렬 기반. 원음(임의 SR)+음절열 → 20ms 프레임별 커브 맵.
	// 프레임 수 == 오디오 20ms 프레임 수. 음절 내부: 온셋(피크 전 70%)/모음(피크 부근)/종성(말미 40%).
	EARLONE_API TArray<TMap<FName, float>> BuildTrack(
		const TArray<FSyllable>& Syllables, const TArray<float>& Mono, int32 SampleRate);
}

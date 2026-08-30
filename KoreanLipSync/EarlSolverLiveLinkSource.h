#pragma once

#include "CoreMinimal.h"
#include "MetaHumanAudioBaseLiveLinkSubject.h"
#include "MetaHumanLocalLiveLinkSource.h"

// 원래 팀 공용 레포의 EarlTTSLiveLinkSource 로 작성했던 것을 이 모듈로 옮겼다 —
// UE 5.8 엔진 내장 MetaHuman 오디오 드리븐 애니메이션(NNE 솔버) 경로.
// 데스크톱 기본 = NNE 솔버(비짐·무드 풀페이스), Android/강제 시 = 절차적 커브(폴백).
// 전환: 콘솔 변수 `earl.LipSync.ForceProcedural` (0=NNE, 1=절차적).
//
// EarlOne 의 기존 FEarlLipSyncSource(순수 ILiveLinkSource, MetaHuman 무의존)는
// 자동화 테스트(CurveMath)와 최후 폴백용으로 유지한다.

class FEarlTTSLiveLinkSubject final : public FMetaHumanAudioBaseLiveLinkSubject
{
public:
	FEarlTTSLiveLinkSubject(
		ILiveLinkClient* InLiveLinkClient,
		const FGuid& InSourceGuid,
		const FName& InSubjectName,
		UMetaHumanAudioBaseLiveLinkSubjectSettings* InSettings);

	void PushPcm(const TArray<float>& InSamples, int32 InSampleRate, int32 InNumChannels);

	// 감정 표정을 솔버 프레임에 **가산**한다 (커브 마스크 additive).
	// 왜 별도 LiveLink subject 가 아닌가: AnimGraph 의 LiveLinkPose 노드는 포즈의 커브 집합을
	// **교체**하므로, 체인 중간에 노드를 하나 더 끼우면 상류(솔버)의 입 커브가 전부 지워진다
	// (2026-07-30 실측, ★19 — "입이 느릿해진" 진범). 같은 subject 의 같은 프레임에 더하면
	// ABP 를 전혀 건드리지 않고, 그 사고가 구조적으로 불가능해진다.
	// InRawDelta 는 **raw 컨트롤** 기준 증분이며 게임 스레드에서 미리 변환해 넣는다.
	void SetRawExpressionDelta(TMap<FString, float> InRawDelta);

	// 절차적 머리 움직임 세기(도) / 속도. 0 이면 끈다.
	void SetHeadMotion(float InAmplitudeDeg, float InSpeed);

	// 강세 끄덕임 파라미터 / 발동. TriggerHeadNod 는 **게임 스레드에서** 부른다.
	void SetHeadNod(float InNodDeg, float InDurationSec);
	void TriggerHeadNod(float InStrength);

	// 머리 추종분(도). 게임 스레드에서 넣고 솔버 스레드에서 사인 합에 더한다.
	void SetHeadFollow(float InYawDeg, float InPitchDeg, float InRollDeg);

	// 지금 머리가 얼마나 돌아가 있는가 (시선 보정이 읽는다). 한 프레임 늦어도 무해.
	void GetHeadMotionAngles(float& OutYawDeg, float& OutPitchDeg) const;

	// 조음 계측 — LiveLink 로 나가기 직전의 조음 커브 프레임간 변화량
	void GetArticulation(float& OutMean, float& OutPeak, int32& OutSamples) const;
	void ResetArticulation();

	// 무음 급전 중임을 알린다. 솔버는 디지털 무음에도 입을 살짝 벌린 얼굴을 내놓기 때문에
	// (2026-08-03 A/B 실측: keep-alive ON = 치아가 보일 만큼 벌어짐 / OFF = 닫힘),
	// 그대로 두면 대기 중 캐릭터가 계속 입을 벌리고 있다.
	void SetIdleMouthDamp(bool bInIdle, float InDamp);
	void SetMouthGain(float InGain);

protected:
	virtual void MediaSamplerMain() override;

	// 솔버가 눈·눈썹 커브까지 내는지 1회 로깅 — 풀페이스 구동 판별 근거.
	// 그리고 감정 오프셋을 여기서 가산한다(프레임이 LiveLink 로 나가기 직전).
	virtual void ExtractPipelineData(
		TSharedPtr<UE::MetaHuman::Pipeline::FPipelineData> InPipelineData) override;

private:
	// 프레임이 LiveLink 로 나가기 직전에 감정 증분을 기존 키에만 가산 (키 추가 금지 — 정렬 파손)
	void ApplyExpressionDelta();

	// 절차적 머리 흔들림을 Animation.Pose 에 실어 보낸다.
	// 왜 우리가 만드나: 오디오 솔버는 얼굴 커브만 만들고 **머리 포즈를 만들지 않는다**
	// (HeadPoseMode 는 None/CameraRelative/Orientation 뿐이고 전부 영상 구동, 2026-08-03 소스 확인).
	// 반면 적용 쪽(CR_MetaHuman_HeadMovement_IK_Proc + ABP_Face_PostProcess)은 이미 얼굴 메시에
	// 걸려 있다. 즉 **받을 준비는 돼 있고 보내는 쪽만 비어 있었다** — 그 자리를 채운다.
	void ApplyHeadMotion();

	// 무음 중 입·턱을 닫는다. 립싱크가 도는 동안에는 절대 호출되지 않는다(그때는 솔버가 주인).
	void ApplyIdleMouthDamp();
	void ApplyMouthGain();

	// 조음이 얼마나 살아 있는지 **신호 레벨에서** 잰다 (화면 크롭은 머리·프레이밍이 섞여 못 쓴다)
	void MeasureArticulation();
	TMap<FString, float> LastArticulation;
	mutable FCriticalSection ArticulationMutex;
	TArray<float> ArticulationHistory;
	// 얼굴 갱신률 — 렌더 FPS 와 다른 축이라 따로 잰다 (솔버 스레드 전용)
	double FrameRateWindowStart = 0.0;
	int32 FrameRateCount = 0;

	// 표정 층이 솔버가 이미 구동 중인 커브를 1.0 에서 잘라먹는 횟수 (ApplyExpressionDelta 참조)
	double SaturationWindowStart = 0.0;
	int32 SaturatedFrames = 0;

	bool bIdleMouth = false;
	float IdleMouthDamp = 1.0f;
	float MouthCurveGain = 1.0f;
	bool bLoggedIdleMouth = false;

	double HeadTime = 0.0;
	float HeadAmplitudeDeg = 0.0f;
	float HeadSpeed = 1.0f;
	bool bLoggedHeadMotion = false;

	// 강세 끄덕임 — PendingNodStrength 만 뮤텍스로 건네고 나머지는 솔버 스레드 전용
	FCriticalSection HeadNodMutex;
	float PendingNodStrength = 0.0f;
	float NodStrength = 0.0f;
	float NodTimeRemaining = 0.0f;
	float NodDurationSec = 0.45f;
	float HeadNodDeg = 2.2f;
	// 시선 보정용 최신 머리 각도 (솔버 스레드가 쓰고 게임 스레드가 읽는다 — 한 프레임 오차 허용)
	float LastHeadYawDeg = 0.0f;
	float LastHeadPitchDeg = 0.0f;
	// 머리 추종분 — 게임 스레드가 쓰고 솔버 스레드가 읽는다 (HeadNodMutex 로 보호)
	float FollowYawDeg = 0.0f;
	float FollowPitchDeg = 0.0f;
	float FollowRollDeg = 0.0f;

	bool bLoggedFirstPcm = false;
	bool bLoggedSolverOutput = false;

	// 미디어 샘플러 스레드(ExtractPipelineData)와 게임 스레드(SetRawExpressionDelta) 사이 보호
	FCriticalSection ExpressionDeltaMutex;
	TMap<FString, float> RawExpressionDelta;
	bool bLoggedExpressionMatch = false;
};

class FEarlProceduralLiveLinkSubject final : public FMetaHumanLocalLiveLinkSubject
{
public:
	FEarlProceduralLiveLinkSubject(
		ILiveLinkClient* InLiveLinkClient,
		const FGuid& InSourceGuid,
		const FName& InSubjectName,
		UMetaHumanLocalLiveLinkSubjectSettings* InSettings);

	virtual void Start() override;
	virtual void Stop() override;
	void PushPcm(const TArray<float>& InSamples, int32 InSampleRate, int32 InNumChannels);
	void ResetToNeutral();

protected:
	virtual void ExtractPipelineData(
		TSharedPtr<UE::MetaHuman::Pipeline::FPipelineData> InPipelineData) override;

private:
	void PushStaticData();
	void PushFrame(const float* InSamples, int32 InNumSamples, int32 InNumChannels);

	ILiveLinkClient* ProceduralLiveLinkClient = nullptr;
	FGuid ProceduralSourceGuid;
	FName ProceduralSubjectName;
	TArray<FName> PropertyNames;
	float SmoothedOpen = 0.0f;
	int64 FrameCounter = 0;
	bool bStopped = false;
	bool bLoggedSpeech = false;
};

class FEarlSolverLiveLinkSource final : public FMetaHumanLocalLiveLinkSource
{
public:
	virtual FText GetSourceType() const override;

	bool CreateEarlSubject(const FName& InSubjectName, int32 InLookaheadMs, float InMoodIntensity = 0.5f);

	// NNE 솔버 경로인지 (false = 절차적 폴백). 공급측이 샘플레이트 전략을 고를 때 사용.
	bool IsUsingSolver() const { return !bCreateProceduralSubject; }
	void PushPcm(const TArray<float>& InSamples, int32 InSampleRate, int32 InNumChannels);
	void ResetToNeutral();

	// GUI 컨트롤(`CTRL_expressions_*`) 값을 받아 raw 컨트롤 증분으로 변환해 솔버 프레임에 가산한다.
	// 변환(GuiToRawControlsUtils)은 비용이 있으므로 **게임 스레드에서** 수행하고 샘플러엔 결과만 넘긴다.
	// 절차적 폴백 경로에서는 무시된다(솔버 프레임이 없으므로).
	void SetExpressionCurves(const TMap<FString, float>& InGuiCurves);

	// 절차적 레이어(시선 + 강세 눈썹) — 표정과 **별도로** 들어와 변환 직전에 합쳐진다.
	// 왜 따로인가: 표정은 시간 스무딩(200ms)을 타는데, 시선 사케이드와 강세 눈썹은 툭 튀는 것이
	// 정상이라 같은 필터를 태우면 흐물흐물 미끄러진다. 사람은 그렇게 움직이지 않는다.
	// ⚠️ 표정과 **키가 겹칠 수 있다**(눈썹). 합칠 때 덮어쓰지 않고 **더한다** — 둘 다 가산 기여다.
	void SetProceduralCurves(const TMap<FString, float>& InGuiCurves);

	// 강세 끄덕임 — 얼굴 커브가 아니라 머리 포즈라 서브젝트로 직접 간다
	void SetHeadNod(float InNodDeg, float InDurationSec);
	void TriggerHeadNod(float InStrength);

	// 머리 추종분(도) — 게임 스레드가 시선을 보고 계산해 넣는다
	void SetHeadFollow(float InYawDeg, float InPitchDeg, float InRollDeg);

	// 시선 보정(전정안반사)이 읽는 현재 머리 각도 (추종분 포함)
	void GetHeadMotionAngles(float& OutYawDeg, float& OutPitchDeg) const;

	// 조음 계측 (earl.LipMeter) — 화면이 아니라 커브에서 잰다
	void GetArticulation(float& OutMean, float& OutPeak, int32& OutSamples) const;
	void ResetArticulation();

	// 절차적 머리 움직임 — 솔버가 안 만드는 층이라 우리가 채운다 (자세한 이유는 서브젝트 쪽 주석)
	void SetHeadMotion(float InAmplitudeDeg, float InSpeed);

	// 무음 급전 구간 표시 (입·턱 닫기)
	void SetIdleMouthDamp(bool bInIdle, float InDamp);
	void SetMouthGain(float InGain);

protected:
	virtual TSharedPtr<FMetaHumanLocalLiveLinkSubject> CreateSubject(
		const FName& InSubjectName,
		UMetaHumanLocalLiveLinkSubjectSettings* InSettings) override;

private:
	TWeakPtr<FEarlTTSLiveLinkSubject> EarlSubject;
	TWeakPtr<FEarlProceduralLiveLinkSubject> ProceduralSubject;
	bool bCreateProceduralSubject = false;

	// 두 레이어를 합쳐 raw 증분으로 변환해 서브젝트에 넘긴다 (게임 스레드 전용)
	void RebuildExpressionDelta();

	// 매 틱 불리므로 변화 없으면 변환을 건너뛰기 위한 직전 입력들 (게임 스레드 전용)
	TMap<FString, float> LastGuiCurves;         // 표정 (감정 클립, 스무딩 거침)
	TMap<FString, float> LastProceduralCurves;  // 시선·강세 눈썹 (스무딩 안 거침)
	TMap<FString, float> LastMergedCurves;
};

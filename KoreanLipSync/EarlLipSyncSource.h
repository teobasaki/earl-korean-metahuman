#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"

class ILiveLinkClient;

// 오디오 구동 립싱크 Live Link 소스 (T6 — 팀 공용 레포의 EarlTTSLiveLinkSource 에서 옮겨 왔다).
//
// 이식 결정 (2026-07-28 밤): MetaHuman 플러그인 모듈(NNE 솔버) 의존을 제거하고
// LiveLinkInterface 만으로 컴파일되는 절차적 커브 경로를 이식한다.
// 이 경로는 earl.LipSync.ForceProcedural 로 먼저 검증한 폴백과 동일 수학.
// 고품질 NNE 오디오 솔버 경로는 D2(에디터 세션)에서 모듈명 확인 후 추가한다.
//
// 소비처: ABP 의 LiveLinkPose(subject) → CTRL_expressions_* 커브 직접 구동 (ARKit 매핑 없음).
class EARLONE_API FEarlLipSyncSource : public ILiveLinkSource, public TSharedFromThis<FEarlLipSyncSource>
{
public:
	// ── ILiveLinkSource ──
	virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
	virtual bool IsSourceStillValid() const override;
	virtual bool RequestSourceShutdown() override;
	virtual FText GetSourceType() const override;
	virtual FText GetSourceMachineName() const override;
	virtual FText GetSourceStatus() const override;

	// subject 생성 + 정적 데이터(커브 이름 35종) 푸시.
	// CustomProperties 를 주면 그 커브만 발행하는 subject 가 된다 —
	// 하이브리드 모드에서 입 모양 커브만 덮어쓰는 Earl_Viseme 용 (턱·눈은 솔버 소유 유지).
	void CreateSubject(const FName& InSubjectName, const TArray<FName>* CustomProperties = nullptr);

	// 16kHz 모노 float PCM 을 20ms(320샘플) 프레임으로 잘라 커브 프레임 푸시
	void PushPcm(const TArray<float>& MonoSamples16k);

	// 발화 종료 시 입을 중립으로
	void ResetToNeutral();

	// 외부에서 계산한 커브 프레임을 그대로 발행 (한국어 비짐 트랙 등 — N/V 경로)
	void PushCurveFrame(const TMap<FName, float>& CurveValues);

	// ── 테스트 가능한 순수 커브 계산 (T6 판정용) ──
	// InOutSmoothedOpen: 프레임 간 스무딩 상태. 반환: 커브 이름→값.
	static TMap<FName, float> ComputeFrameCurves(
		const float* Samples, int32 NumSamples, float& InOutSmoothedOpen, int64 FrameCounter);

	static const TArray<FName>& GetPropertyNames();

	static constexpr int32 SampleRate = 16000;
	static constexpr int32 SamplesPerFrame = 320; // 20ms

private:
	void PushStaticData();
	void PushFrame(const float* Samples, int32 NumSamples);

	ILiveLinkClient* Client = nullptr;
	FGuid SourceGuid;
	FName SubjectName;
	bool bShutdown = false;
	TArray<FName> ActiveProperties; // 이 subject 가 발행하는 커브 (기본 = 전체 35종)

	float SmoothedOpen = 0.0f;
	int64 FrameCounter = 0;
	bool bLoggedSpeech = false;
};

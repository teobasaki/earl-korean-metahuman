#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

class ILiveLinkClient;
class FSocket;

// ARDY 27본 모션 스트림 수신기 (M1 Task 2 / ardy-stream ue/README 프로토콜).
//
// 와이어 (TCP, 줄 단위 JSON):
//   {"type":"skeleton","v":1,"bones":[{"name":"Hips","parent":-1}, ...27]}  ← 1회
//   {"type":"frame","idx":N,"t":초,"rot":[[x,y,z,w]*27],"root":[x,y,z]}   ← 매 프레임
//   {"type":"end"}
//
// rot = 본별 로컬 회전 쿼터니언(xyzw), root = 본0 translation.
// 나머지 본 translation 은 참조 포즈 유지(회전만 구동).
// ⚠️ 좌표계 보정(ARDY 오른손 Y-up → UE 왼손 Z-up)은 실물 확인이 필요해
//    EAxisFix 설정으로 노출한다 — 기본 Passthrough, 실검증(N3/아침)에서 확정.
//
// 2026-08-05 좌표 규약 확정(nv-tlabs/ARDY cskel27 bind pose 실측 + 수치검증):
//   ARDY = 오른손, Y-up, +Z 전방, +X = 캐릭터 왼쪽, 단위 m, T-포즈.
//   UE   = 왼손,  Z-up, +X 전방, +Y = 캐릭터 오른쪽, 단위 cm.
//   R(f(q)) == C·R(q)·C^T 를 만족하는 유효 변환은 SwapYZConj/CycZXY/CycZXYMirror 셋뿐.
//   SwapYZ·SwapYZNegXZ 는 유효 변환의 역회전(거울 재생: 숙임↔젖힘 반전) — 소스 메시가
//   같은 방식으로 뒤집혀 임포트된 경우에만 우연히 맞는다. 비교 실험용으로 유지.
class ARDYLIVELINK_API FArdyLiveLinkSource : public ILiveLinkSource, public FRunnable, public TSharedFromThis<FArdyLiveLinkSource>
{
public:
	enum class EAxisFix : uint8
	{
		Passthrough,   // 그대로 (기본 — 보정값 확정 전)
		SwapYZ,        // (x,z,y,w)   ⚠️ 유효 변환 아님(역회전) — 비교용
		SwapYZNegXZ,   // (-x,-z,y,w) ⚠️ 유효 변환 아님 — 비교용
		SwapYZConj,    // (x,z,y,-w)  Y↔Z 스왑 기저 정변환. 캐릭터가 UE +Y를 본다
		CycZXY,        // (z,x,y,w)   전방→전방(Z→X) 순환 기저. 거울 없음 = L/R 반전 재생 가능성
		CycZXYMirror   // (-z,x,-y,w) 전방→전방 + L/R 물리 보존(ARDY +X(왼)→UE -Y(왼))
	};

	// 파싱 결과 구조 (테스트 대상)
	struct FSkeletonDef
	{
		TArray<FName> BoneNames;
		TArray<int32> BoneParents;
	};
	struct FFrameDef
	{
		int32 Index = 0;
		double Time = 0.0;
		TArray<FQuat> LocalRotations; // xyzw 그대로 (보정 전)
		FVector RootTranslation = FVector::ZeroVector;
	};

	FArdyLiveLinkSource(const FString& InHost, uint16 InPort, EAxisFix InAxisFix = EAxisFix::Passthrough);
	virtual ~FArdyLiveLinkSource() override;

	// ── ILiveLinkSource ──
	virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
	virtual bool IsSourceStillValid() const override;
	virtual bool RequestSourceShutdown() override;
	virtual FText GetSourceType() const override;
	virtual FText GetSourceMachineName() const override;
	virtual FText GetSourceStatus() const override;

	// ── FRunnable (수신 스레드) ──
	virtual uint32 Run() override;
	virtual void Stop() override;

	// ── 순수 파싱 (N3 판정 대상 — 소켓 없이 검증 가능) ──
	static bool ParseLine(const FString& Line, FSkeletonDef& OutSkeleton, FFrameDef& OutFrame,
		bool& bOutIsSkeleton, bool& bOutIsFrame, bool& bOutIsEnd);
	static FQuat ApplyAxisFix(const FQuat& In, EAxisFix Fix);
	// 루트 translation용 벡터 변환. 새 프리셋(SwapYZConj/CycZXY/CycZXYMirror)만
	// 축 매핑 + m→cm(×100). 구 프리셋은 기존 동작(원시값) 보존 — 비교 실험용.
	static FVector ApplyAxisFixVector(const FVector& In, EAxisFix Fix);

	// 수신 통계 (판정·디버그)
	int32 GetReceivedFrameCount() const { return ReceivedFrames.GetValue(); }
	bool HasReceivedSkeleton() const { return bSkeletonReceived; }

	// ★ 축 보정을 **런타임에 바꾼다** (2026-08-06).
	// EAxisFix 확정은 육안 판정이고, 후보가 셋이다. 소스를 지웠다 다시 붙이면 그때마다
	// 재접속·스켈레톤 재수신이 필요해 비교가 느리고 끊긴다. 스트림을 유지한 채 프리셋만
	// 갈아끼울 수 있어야 "① → ② → ③" 을 몇 초 만에 훑는다.
	// 수신 스레드가 읽으므로 원자적으로 둔다.
	void SetAxisFix(EAxisFix InFix) { AxisFix = InFix; }
	EAxisFix GetAxisFix() const { return AxisFix; }

	static const TCHAR* AxisFixToString(EAxisFix Fix);
	static bool AxisFixFromString(const FString& Name, EAxisFix& OutFix);

	/**
	 * ★ 서버로 한 줄 올린다 (2026-08-06 신설).
	 *
	 * 지금까지 이 소켓은 **받기만** 했다. 실시간 캡션 구동(페르소나 답변 → 모션 캡션 →
	 * ARDY 생성)을 하려면 클라이언트→서버 방향이 필요한데 계약에 없었다.
	 *
	 * 실측(2026-08-06): 현재 4090 은 replay 송신만 하고 **캡션 줄을 보내도 무시한다 —
	 * 다만 연결은 끊지 않는다.** 그래서 지금 넣어 두면 서버가 핸들러를 붙이는 순간
	 * 바로 동작하고, 그 전까지는 무해하다.
	 *
	 * 제안 계약(같은 5006 소켓, 줄 단위 JSON):
	 *     {"type":"caption","text":"a person points to the side while explaining"}
	 * 캡션 형식은 `a person …` 으로 시작해야 한다
	 * (docs/content/ARDY-프롬프트-워크플로-참고.md §1).
	 *
	 * 수신 스레드가 실제 전송을 하므로 여기서는 큐에 넣기만 한다.
	 */
	void SendLine(const FString& Line);

	static const FName SubjectName; // "ArdyMotion"

private:
	void PushSkeletonStatic(const FSkeletonDef& Skeleton);
	void PushFrame(const FFrameDef& Frame);

	FString Host;
	uint16 Port;
	// 수신 스레드가 매 프레임 읽고 게임 스레드(콘솔)가 쓴다 — 원자적이어야 한다.
	TAtomic<EAxisFix> AxisFix;

	ILiveLinkClient* Client = nullptr;
	FGuid SourceGuid;

	FRunnableThread* Thread = nullptr;
	FThreadSafeBool bStopping = false;
	FThreadSafeCounter ReceivedFrames;
	bool bSkeletonReceived = false;

	FSkeletonDef ActiveSkeleton;

	// 게임 스레드가 넣고 수신 스레드가 빼서 보낸다 (소켓은 Run() 지역이라 직접 못 쓴다)
	TQueue<FString, EQueueMode::Mpsc> OutgoingLines;
};

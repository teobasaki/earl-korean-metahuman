#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "AnimNodes/AnimNode_RetargetPoseFromMesh.h"
#include "ArdyRetargetAnimInstance.generated.h"

class UIKRetargeter;
class USkeletalMeshComponent;

/**
 * ARDY 모션을 **MetaHuman 바디에 얹기 위한** 애셋 없는 런타임 AnimInstance.
 *
 * 왜 필요한가 (2026-08-06):
 *   ARDY 소스 메시 `ARDY_explain_both_01` 은 **사람 형상이 없다** — 실측 바운드
 *   (10,10,10) 짜리 플레이스홀더 큐브에 27본 스켈레톤만 붙은 자산이다(본 28).
 *   팀이 IK Rig 소스로만 쓰려고 임포트한 것이라, 그 메시를 아무리 띄워도
 *   **눈으로 판정할 수 없다** — 화면에는 회색 조각이 떠다닐 뿐이다.
 *   (이걸 모르고 한참 헤맸다. 살이 있는 것은 MetaHuman 바디뿐이다.)
 *
 *   그래서 판정은 반드시 이 경로로 해야 한다:
 *     ArdyMotion(Live Link) → 소스 큐브 메시(안 보여도 됨) → IK Retargeter → 타깃 캐릭터 바디
 *
 * 구현은 팀 공용 레포 브랜치 `feat/ardy-receiver-upgrade` 의
 * `UArdyRetargetAnimInstance` 패턴을 그대로 따랐다 — 손 포즈 보강 등 부가 기능은 뺀
 * 최소 골격만 옮겼다. 핵심은 `FAnimNode_RetargetPoseFromMesh` 를 매 프레임 평가하는 것.
 */
USTRUCT()
struct FArdyRetargetProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FArdyRetargetProxy() = default;
	FArdyRetargetProxy(UAnimInstance* InAnimInstance, FAnimNode_RetargetPoseFromMesh* InNode);

	virtual FAnimNode_Base* GetCustomRootNode() override;
	virtual void GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes) override;
	virtual bool Evaluate(FPoseContext& Output) override;

	void Configure(UIKRetargeter* InRetargeter, USkeletalMeshComponent* InSourceMeshComponent);

	FAnimNode_RetargetPoseFromMesh* RetargetNode = nullptr;
};

UCLASS(Transient, NotBlueprintable)
class ARDYLIVELINK_API UArdyRetargetAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UArdyRetargetAnimInstance();

	void Configure(UIKRetargeter* InRetargeter, USkeletalMeshComponent* InSourceMeshComponent);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

	UPROPERTY(Transient)
	FAnimNode_RetargetPoseFromMesh RetargetNode;
};

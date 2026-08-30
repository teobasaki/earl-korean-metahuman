#include "ArdyRetargetAnimInstance.h"

#include "Components/SkeletalMeshComponent.h"
#include "Retargeter/IKRetargeter.h"

FArdyRetargetProxy::FArdyRetargetProxy(
	UAnimInstance* InAnimInstance,
	FAnimNode_RetargetPoseFromMesh* InNode)
	: FAnimInstanceProxy(InAnimInstance)
	, RetargetNode(InNode)
{
}

FAnimNode_Base* FArdyRetargetProxy::GetCustomRootNode()
{
	return RetargetNode;
}

void FArdyRetargetProxy::GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes)
{
	OutNodes.Add(RetargetNode);
}

bool FArdyRetargetProxy::Evaluate(FPoseContext& Output)
{
	if (!RetargetNode)
	{
		Output.ResetToRefPose();
		return true;
	}
	RetargetNode->Evaluate_AnyThread(Output);
	Output.Pose.NormalizeRotations();
	return true;
}

void FArdyRetargetProxy::Configure(
	UIKRetargeter* InRetargeter,
	USkeletalMeshComponent* InSourceMeshComponent)
{
	if (!RetargetNode)
	{
		return;
	}
	RetargetNode->IKRetargeterAsset = InRetargeter;
	// 소스는 우리가 Live Link 로 구동하는 그 큐브 메시 컴포넌트다.
	RetargetNode->RetargetFrom = ERetargetSourceMode::CustomSkeletalMeshComponent;
	RetargetNode->SourceMeshComponent = InSourceMeshComponent;
	RetargetNode->bSuppressWarnings = false;
}

UArdyRetargetAnimInstance::UArdyRetargetAnimInstance()
{
	bUseMultiThreadedAnimationUpdate = false;
}

FAnimInstanceProxy* UArdyRetargetAnimInstance::CreateAnimInstanceProxy()
{
	return new FArdyRetargetProxy(this, &RetargetNode);
}

void UArdyRetargetAnimInstance::Configure(
	UIKRetargeter* InRetargeter,
	USkeletalMeshComponent* InSourceMeshComponent)
{
	RetargetNode.IKRetargeterAsset = InRetargeter;
	RetargetNode.RetargetFrom = ERetargetSourceMode::CustomSkeletalMeshComponent;
	RetargetNode.SourceMeshComponent = InSourceMeshComponent;
	RetargetNode.bSuppressWarnings = false;

	// 프록시가 이미 만들어진 뒤라면 거기에도 반영한다.
	if (FArdyRetargetProxy* Proxy = GetProxyOnGameThread<FArdyRetargetProxy>().RetargetNode
			? &GetProxyOnGameThread<FArdyRetargetProxy>() : nullptr)
	{
		Proxy->Configure(InRetargeter, InSourceMeshComponent);
	}
}

#include "Team/ArdyTeamRetargetAnimInstance.h"

#include "Components/SkeletalMeshComponent.h"
#include "BonePose.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetProcessor.h"

namespace
{
	struct FFingerBonePose
	{
		const TCHAR* Bone;
		float CurlDegrees;
		float SpreadDegrees;
	};

	FCompactPoseBoneIndex FindCompactBone(
		const FCompactPose& Pose,
		const FName BoneName)
	{
		const FBoneContainer& Bones = Pose.GetBoneContainer();
		const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(BoneName);
		return MeshIndex == INDEX_NONE
			? FCompactPoseBoneIndex(INDEX_NONE)
			: Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
	}

	float PoseCurlScale(
		const EArdyHandPose Pose,
		const FString& Finger)
	{
		switch (Pose)
		{
		case EArdyHandPose::OpenPalm:
			return 0.05f;
		case EArdyHandPose::Point:
			return Finger == TEXT("index") ? 0.0f : 1.65f;
		case EArdyHandPose::Explain:
			return 0.65f;
		case EArdyHandPose::SoftFist:
			return 2.25f;
		case EArdyHandPose::Relaxed:
			return 1.0f;
		case EArdyHandPose::Reference:
		default:
			return 0.0f;
		}
	}

	FQuat ExtractTwistX(const FQuat& Rotation)
	{
		FQuat Twist(Rotation.X, 0.0, 0.0, Rotation.W);
		if (Twist.SizeSquared() <= UE_SMALL_NUMBER)
		{
			return FQuat::Identity;
		}
		Twist.Normalize();
		return Twist;
	}
}

FArdyTeamRetargetAnimInstanceProxy::FArdyTeamRetargetAnimInstanceProxy(
	UAnimInstance* InAnimInstance,
	FAnimNode_RetargetPoseFromMesh* InRetargetNode)
	: FAnimInstanceProxy(InAnimInstance)
	, RetargetNode(InRetargetNode)
{
}

FAnimNode_Base* FArdyTeamRetargetAnimInstanceProxy::GetCustomRootNode()
{
	return RetargetNode;
}

void FArdyTeamRetargetAnimInstanceProxy::GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes)
{
	OutNodes.Add(RetargetNode);
}

bool FArdyTeamRetargetAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	if (!RetargetNode)
	{
		Output.ResetToRefPose();
		return true;
	}

	RetargetNode->Evaluate_AnyThread(Output);
	ApplyFingerPose(Output.Pose, QualitySettings.LeftHandPose, true);
	ApplyFingerPose(Output.Pose, QualitySettings.RightHandPose, false);
	ApplyAugmentation(Output.Pose);
	Output.Pose.NormalizeRotations();
	return true;
}

void FArdyTeamRetargetAnimInstanceProxy::Configure(
	UIKRetargeter* InRetargeter,
	USkeletalMeshComponent* InSourceMeshComponent)
{
	if (!RetargetNode)
	{
		return;
	}

	RetargetNode->IKRetargeterAsset = InRetargeter;
	RetargetNode->RetargetFrom = ERetargetSourceMode::CustomSkeletalMeshComponent;
	RetargetNode->SourceMeshComponent = InSourceMeshComponent;
	RetargetNode->bSuppressWarnings = false;
	if (FIKRetargetProcessor* Processor = RetargetNode->GetRetargetProcessor())
	{
		Processor->SetNeedsInitialized();
	}
}

void FArdyTeamRetargetAnimInstanceProxy::ConfigureQuality(
	const FArdyPoseAugmentationSettings& InSettings)
{
	QualitySettings = InSettings;
}

void FArdyTeamRetargetAnimInstanceProxy::ApplyFingerPose(
	FCompactPose& Pose,
	const EArdyHandPose HandPose,
	const bool bLeft)
{
	if (HandPose == EArdyHandPose::Retarget)
	{
		return;
	}

	static const FFingerBonePose FingerBones[] = {
		{TEXT("index_metacarpal"), 3.0f, -2.0f},
		{TEXT("index_01"), 12.0f, 0.0f},
		{TEXT("index_02"), 20.0f, 0.0f},
		{TEXT("index_03"), 12.0f, 0.0f},
		{TEXT("middle_metacarpal"), 4.0f, 0.0f},
		{TEXT("middle_01"), 16.0f, 0.0f},
		{TEXT("middle_02"), 24.0f, 0.0f},
		{TEXT("middle_03"), 14.0f, 0.0f},
		{TEXT("ring_metacarpal"), 5.0f, 1.5f},
		{TEXT("ring_01"), 18.0f, 0.0f},
		{TEXT("ring_02"), 28.0f, 0.0f},
		{TEXT("ring_03"), 16.0f, 0.0f},
		{TEXT("pinky_metacarpal"), 7.0f, 3.0f},
		{TEXT("pinky_01"), 22.0f, 0.0f},
		{TEXT("pinky_02"), 32.0f, 0.0f},
		{TEXT("pinky_03"), 18.0f, 0.0f},
		{TEXT("thumb_01"), 8.0f, -4.0f},
		{TEXT("thumb_02"), 12.0f, 0.0f},
		{TEXT("thumb_03"), 8.0f, 0.0f},
	};

	const TCHAR* Side = bLeft ? TEXT("_l") : TEXT("_r");
	const float BlendSeconds =
		FMath::Max(QualitySettings.HandPoseBlendSeconds, 0.05f);
	const float Alpha = 1.0f - FMath::Exp(
		-FMath::Max(GetDeltaSeconds(), 1.0f / 240.0f) / BlendSeconds);
	for (const FFingerBonePose& Entry : FingerBones)
	{
		const FName BoneName(*FString::Printf(TEXT("%s%s"), Entry.Bone, Side));
		const FCompactPoseBoneIndex BoneIndex = FindCompactBone(Pose, BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		const FString Finger(Entry.Bone);
		const float CurlScale = PoseCurlScale(HandPose, Finger);
		const float SideSign = bLeft ? -1.0f : 1.0f;
		const FQuat Curl(
			FVector::YAxisVector,
			FMath::DegreesToRadians(Entry.CurlDegrees * CurlScale));
		const FQuat Spread(
			FVector::UpVector,
			FMath::DegreesToRadians(
				Entry.SpreadDegrees * CurlScale * SideSign));
		const FTransform& Reference = Pose.GetRefPose(BoneIndex);
		const FQuat TargetRotation =
			(Reference.GetRotation() * Spread * Curl).GetNormalized();
		FQuat& CurrentRotation =
			FingerBlendRotations.FindOrAdd(
				BoneName,
				Pose[BoneIndex].GetRotation());
		CurrentRotation = FQuat::Slerp(
			CurrentRotation,
			TargetRotation,
			Alpha).GetNormalized();

		FTransform& BoneTransform = Pose[BoneIndex];
		BoneTransform.SetTranslation(Reference.GetTranslation());
		BoneTransform.SetScale3D(Reference.GetScale3D());
		BoneTransform.SetRotation(CurrentRotation);
	}
}

void FArdyTeamRetargetAnimInstanceProxy::ApplyAugmentation(FCompactPose& Pose)
{
	const float Strength = FMath::Clamp(
		QualitySettings.AugmentationStrength,
		0.0f,
		1.0f);
	if (Strength <= 0.0f)
	{
		return;
	}

	auto AddFractionOfBoneDelta = [&Pose](
		const FName SourceBone,
		const FName TargetBone,
		const float Weight,
		const bool bTwistOnly)
	{
		const FCompactPoseBoneIndex SourceIndex =
			FindCompactBone(Pose, SourceBone);
		const FCompactPoseBoneIndex TargetIndex =
			FindCompactBone(Pose, TargetBone);
		if (SourceIndex == INDEX_NONE || TargetIndex == INDEX_NONE)
		{
			return;
		}
		const FQuat SourceDelta =
			(Pose.GetRefPose(SourceIndex).GetRotation().Inverse() *
				Pose[SourceIndex].GetRotation()).GetNormalized();
		const FQuat Contribution = bTwistOnly
			? ExtractTwistX(SourceDelta)
			: SourceDelta;
		const FQuat Weighted = FQuat::Slerp(
			FQuat::Identity,
			Contribution,
			Weight).GetNormalized();
		FTransform& Target = Pose[TargetIndex];
		Target.SetRotation(
			(Target.GetRotation() * Weighted).GetNormalized());
	};

	if (QualitySettings.bEnableSpineDistribution)
	{
		AddFractionOfBoneDelta(
			TEXT("spine_03"), TEXT("spine_04"), 0.08f * Strength, false);
		AddFractionOfBoneDelta(
			TEXT("spine_04"), TEXT("spine_05"), 0.05f * Strength, false);
	}

	if (QualitySettings.bEnableClavicleAssist)
	{
		AddFractionOfBoneDelta(
			TEXT("upperarm_l"), TEXT("clavicle_l"), 0.06f * Strength, false);
		AddFractionOfBoneDelta(
			TEXT("upperarm_r"), TEXT("clavicle_r"), 0.06f * Strength, false);
	}

	if (QualitySettings.bEnableTwistDistribution)
	{
		static const TPair<FName, FName> TwistPairs[] = {
			{TEXT("upperarm_l"), TEXT("upperarm_twist_01_l")},
			{TEXT("upperarm_l"), TEXT("upperarm_twist_02_l")},
			{TEXT("upperarm_r"), TEXT("upperarm_twist_01_r")},
			{TEXT("upperarm_r"), TEXT("upperarm_twist_02_r")},
			{TEXT("lowerarm_l"), TEXT("lowerarm_twist_01_l")},
			{TEXT("lowerarm_l"), TEXT("lowerarm_twist_02_l")},
			{TEXT("lowerarm_r"), TEXT("lowerarm_twist_01_r")},
			{TEXT("lowerarm_r"), TEXT("lowerarm_twist_02_r")},
		};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(TwistPairs); ++Index)
		{
			const float BaseWeight = Index % 2 == 0 ? 0.18f : 0.10f;
			AddFractionOfBoneDelta(
				TwistPairs[Index].Key,
				TwistPairs[Index].Value,
				BaseWeight * Strength,
				true);
		}
	}
}

UArdyTeamRetargetAnimInstance::UArdyTeamRetargetAnimInstance()
{
	bUseMultiThreadedAnimationUpdate = true;
}

void UArdyTeamRetargetAnimInstance::Configure(
	UIKRetargeter* InRetargeter,
	USkeletalMeshComponent* InSourceMeshComponent)
{
	FArdyTeamRetargetAnimInstanceProxy& Proxy =
		GetProxyOnGameThread<FArdyTeamRetargetAnimInstanceProxy>();
	Proxy.Configure(InRetargeter, InSourceMeshComponent);
}

void UArdyTeamRetargetAnimInstance::ConfigureQuality(
	const FArdyPoseAugmentationSettings& InSettings)
{
	FArdyTeamRetargetAnimInstanceProxy& Proxy =
		GetProxyOnGameThread<FArdyTeamRetargetAnimInstanceProxy>();
	Proxy.ConfigureQuality(InSettings);
}

FAnimInstanceProxy* UArdyTeamRetargetAnimInstance::CreateAnimInstanceProxy()
{
	return new FArdyTeamRetargetAnimInstanceProxy(this, &RetargetNode);
}

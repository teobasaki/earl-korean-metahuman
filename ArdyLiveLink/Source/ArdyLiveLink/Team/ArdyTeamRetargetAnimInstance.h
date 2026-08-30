#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "AnimNodes/AnimNode_RetargetPoseFromMesh.h"
#include "ArdyTeamRetargetAnimInstance.generated.h"

class UIKRetargeter;
class USkeletalMeshComponent;

UENUM(BlueprintType)
enum class EArdyHandPose : uint8
{
	Retarget UMETA(DisplayName="Existing Retarget"),
	Reference UMETA(DisplayName="Finger Retarget OFF"),
	Relaxed,
	OpenPalm UMETA(DisplayName="Open Palm"),
	Point,
	Explain,
	SoftFist UMETA(DisplayName="Soft Fist")
};

USTRUCT(BlueprintType)
struct ARDYLIVELINK_API FArdyPoseAugmentationSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Hands")
	EArdyHandPose LeftHandPose = EArdyHandPose::Retarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Hands")
	EArdyHandPose RightHandPose = EArdyHandPose::Retarget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Hands", meta=(ClampMin="0.05", ClampMax="1.0"))
	float HandPoseBlendSeconds = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Augmentation")
	bool bEnableSpineDistribution = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Augmentation")
	bool bEnableClavicleAssist = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Augmentation")
	bool bEnableTwistDistribution = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Augmentation", meta=(ClampMin="0.0", ClampMax="1.0"))
	float AugmentationStrength = 0.5f;
};

USTRUCT()
struct FArdyTeamRetargetAnimInstanceProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FArdyTeamRetargetAnimInstanceProxy() = default;
	FArdyTeamRetargetAnimInstanceProxy(UAnimInstance* InAnimInstance, FAnimNode_RetargetPoseFromMesh* InRetargetNode);

	virtual FAnimNode_Base* GetCustomRootNode() override;
	virtual void GetCustomNodes(TArray<FAnimNode_Base*>& OutNodes) override;
	virtual bool Evaluate(FPoseContext& Output) override;

	void Configure(UIKRetargeter* InRetargeter, USkeletalMeshComponent* InSourceMeshComponent);
	void ConfigureQuality(const FArdyPoseAugmentationSettings& InSettings);

	FAnimNode_RetargetPoseFromMesh* RetargetNode = nullptr;
	FArdyPoseAugmentationSettings QualitySettings;
	TMap<FName, FQuat> FingerBlendRotations;

private:
	void ApplyFingerPose(FCompactPose& Pose, EArdyHandPose HandPose, bool bLeft);
	void ApplyAugmentation(FCompactPose& Pose);
};

/**
 * Native, asset-free target AnimInstance used by the ARDY receiver.
 * It evaluates UE's runtime Retarget Pose From Mesh node every frame.
 */
UCLASS(Transient, NotBlueprintable)
class ARDYLIVELINK_API UArdyTeamRetargetAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UArdyTeamRetargetAnimInstance();

	void Configure(UIKRetargeter* InRetargeter, USkeletalMeshComponent* InSourceMeshComponent);
	void ConfigureQuality(const FArdyPoseAugmentationSettings& InSettings);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

	UPROPERTY(Transient)
	FAnimNode_RetargetPoseFromMesh RetargetNode;
};

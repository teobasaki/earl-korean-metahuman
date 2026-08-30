#pragma once

#include "CoreMinimal.h"
#include "Team/ArdyMotionDiagnostics.h"
#include "Team/ArdyMotionProtocol.h"
#include "Team/ArdyTeamRetargetAnimInstance.h"
#include "Team/ArdyMotionStabilizer.h"
#include "Common/UdpSocketReceiver.h"
#include "Components/ActorComponent.h"
#include "Containers/Queue.h"
#include "ArdyMotionReceiverComponent.generated.h"

class AActor;
class FArdyMotionLiveLinkSource;
class FSocket;
class UIKRetargeter;
class USkeletalMesh;
class USkeletalMeshComponent;

UENUM(BlueprintType)
enum class EArdyStreamHealthState : uint8
{
	NoData UMETA(DisplayName="No Data"),
	Healthy,
	Delayed,
	Frozen,
	SevereFrozen UMETA(DisplayName="Severe Freeze")
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FArdyMotionFrameEvent,
	int64, FrameNumber,
	int64, TimestampUs,
	int32, AppliedJointCount);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FArdyMotionErrorEvent, const FString&, Error);

/**
 * Receives ardy.motion.v1 JSON datagrams and publishes an ARDY_Body Live Link
 * animation subject. The optional runtime pipeline drives a hidden ARDY source
 * mesh with Live Link and retargets it to the MetaHuman Body component.
 */
UCLASS(ClassGroup=(Earl), meta=(BlueprintSpawnableComponent))
class ARDYLIVELINK_API UArdyMotionReceiverComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UArdyMotionReceiverComponent();
	virtual ~UArdyMotionReceiverComponent() override;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Network")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Network", meta=(ClampMin="1", ClampMax="65535"))
	int32 ListenPort = 8091;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|LiveLink")
	FName SubjectName = TEXT("ARDY_Body");

	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category="ARDY|Assets",
		meta=(AllowedClasses="/Script/Engine.SkeletalMesh"))
	TSoftObjectPtr<USkeletalMesh> SourceSkeletalMesh;

	UPROPERTY(
		EditAnywhere,
		BlueprintReadWrite,
		Category="ARDY|Assets",
		meta=(AllowedClasses="/Script/IKRig.IKRetargeter"))
	TSoftObjectPtr<UIKRetargeter> IKRetargeterAsset;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="ARDY|Target")
	TObjectPtr<AActor> TargetCharacterActor = nullptr;

	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="ARDY|Target")
	TObjectPtr<USkeletalMeshComponent> TargetBodyComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Target")
	FName TargetBodyComponentName = TEXT("Body");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Target")
	bool bAutoFindTargetCharacter = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Target")
	FString TargetCharacterNamePattern = TEXT("BP_MHC_CRT_NJ_03");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Target")
	bool bAutoConfigureRetarget = true;

	/**
	 * Reuse an already installed UArdyTeamRetargetAnimInstance (or subclass) on
	 * the target body. When enabled, ARDY startup fails safely instead of
	 * replacing the target AnimInstance class. The conflict-lab controller
	 * enables this before PIE BeginPlay so ARDY and lip sync share a stable
	 * Body AnimInstance owner.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Target")
	bool bPreserveTargetAnimInstance = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Debug")
	bool bShowSourcePose = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Filtering")
	bool bBlockHeadAndNeck = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	EArdyMotionQualityProfile MotionQualityProfile =
		EArdyMotionQualityProfile::Raw;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	FArdyPoseAugmentationSettings PoseAugmentation;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Quality")
	int32 BufferedPoseCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Quality")
	double MotionQualityLatencyMs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Quality")
	int64 RejectedMotionOutlierCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Quality")
	int64 AngularVelocityClampCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics")
	bool bEnableDiagnosticLogging = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics", meta=(ClampMin="0.1"))
	float DiagnosticLogIntervalSeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics", meta=(ClampMin="2", ClampMax="600"))
	int32 ArrivalJitterWindowSize = 60;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics|Health", meta=(ClampMin="1.0"))
	float DelayedThresholdMs = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics|Health", meta=(ClampMin="1.0"))
	float FreezeThresholdMs = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics|Health", meta=(ClampMin="1.0"))
	float SevereFreezeThresholdMs = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics|Restart", meta=(ClampMin="0"))
	int64 RestartMaxStartFrame = 10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Diagnostics|Restart", meta=(ClampMin="1"))
	int64 RestartMinBackwardJump = 100;

	UPROPERTY(BlueprintAssignable, Category="ARDY|Events")
	FArdyMotionFrameEvent OnMotionFrame;

	UPROPERTY(BlueprintAssignable, Category="ARDY|Events")
	FArdyMotionErrorEvent OnMotionError;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 ReceivedPacketCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 AcceptedFrameCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 AppliedFrameCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 RejectedPacketCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 DroppedPacketCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 DroppedOrMissingFrameCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 MissingFrameCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 DuplicateFrameCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 OutOfOrderFrameCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 FrameRestartCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 NonMonotonicTimestampCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 LargestFrameGap = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 LastReceivedFrame = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 LastAcceptedFrame = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 LastAppliedFrame = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 LastReceivedTimestampUs = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 LastAcceptedTimestampUs = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 LastSourceTimestampDeltaUs = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double AverageSourceTimestampDeltaUs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 MinSourceTimestampDeltaUs = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	int64 MaxSourceTimestampDeltaUs = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double EstimatedSourceFPS = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double LastArrivalDeltaMs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double AverageArrivalDeltaMs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double MinArrivalDeltaMs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double MaxArrivalDeltaMs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double ArrivalJitterMs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double TimeSinceLastPacketMs = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	double LastPacketReceivedTimeSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Stats")
	EArdyStreamHealthState StreamHealthState = EArdyStreamHealthState::NoData;

	UFUNCTION(BlueprintCallable, Category="ARDY|Network")
	bool StartReceiver();

	UFUNCTION(BlueprintCallable, Category="ARDY|Network")
	void StopReceiver();

	UFUNCTION(BlueprintPure, Category="ARDY|Network")
	bool IsReceiving() const;

	UFUNCTION(BlueprintPure, Category="ARDY|LiveLink")
	bool IsPosePipelineReady() const { return bPosePipelineReady; }

	/** Rebinds the current stable target AnimInstance to the existing source pose. */
	UFUNCTION(BlueprintCallable, Category="ARDY|LiveLink")
	bool ConfigureExistingTargetAnimInstance();

	UFUNCTION(BlueprintCallable, Category="ARDY|Testing")
	bool InjectMotionJson(const FString& Payload);

	UFUNCTION(BlueprintCallable, Category="ARDY|Debug")
	void SetSourcePoseVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category="ARDY|Diagnostics")
	void ResetDiagnostics();

private:
	struct FQueuedMotionPacket
	{
		FArdyMotionFrame Frame;
		FString ParseError;
		double ArrivalTimeSeconds = 0.0;
		bool bParsed = false;
	};

	bool InitializePosePipeline();
	void ShutdownPosePipeline();
	bool RegisterLiveLinkSource();
	void UnregisterLiveLinkSource();
	void QueuePacket(const FArrayReaderPtr& Data);
	void ApplyNewestPendingFrame();
	bool AcceptFrameForDiagnostics(
		const FArdyMotionFrame& Frame,
		bool& bOutSenderRestart);
	void RefreshDiagnosticProperties(double NowSeconds);
	void UpdateStreamHealth(double NowSeconds);
	void LogDiagnosticSummary(double NowSeconds);
	bool ApplyFrame(const FArdyMotionFrame& Frame);
	void ApplyGestureMetadata(const FArdyMotionFrame& Frame);
	USkeletalMeshComponent* ResolveTargetBodyComponent() const;
	bool IsBlockedBone(FName BoneName) const;
	void ReportError(const FString& Error);

	FSocket* MotionSocket = nullptr;
	TUniquePtr<FUdpSocketReceiver> MotionReceiver;
	TQueue<FQueuedMotionPacket, EQueueMode::Mpsc> PendingPackets;
	TSharedPtr<FArdyMotionLiveLinkSource> LiveLinkSource;
	FGuid LiveLinkSourceGuid;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> RuntimeSourcePoseComponent = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USkeletalMesh> LoadedSourceMesh = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UIKRetargeter> LoadedRetargeter = nullptr;

	TArray<FTransform> ReferenceLocalPose;
	TMap<FName, int32> BoneIndexByName;
	int32 RootBoneIndex = INDEX_NONE;
	FArdyMotionDiagnostics Diagnostics;
	FArdyMotionStabilizer MotionStabilizer;
	EArdyMotionQualityProfile ActiveMotionQualityProfile =
		EArdyMotionQualityProfile::Raw;
	double LastDiagnosticLogTimeSeconds = 0.0;
	int64 OutputPublishFailureCount = 0;
	bool bCreatedSourcePoseComponent = false;
	bool bPosePipelineReady = false;
};

#pragma once

#include "CoreMinimal.h"
#include "Team/ArdyMotionReceiverComponent.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "ArdyRuntimeActor.generated.h"

class AActor;
class USceneComponent;
class USkeletalMeshComponent;

UENUM(BlueprintType)
enum class EArdyRuntimeState : uint8
{
	Uninitialized,
	Ready,
	Starting,
	Receiving,
	Degraded,
	Frozen,
	Error,
	Stopped
};

/** Blueprint-friendly snapshot of the diagnostics already collected by the receiver. */
USTRUCT(BlueprintType)
struct ARDYLIVELINK_API FArdyRuntimeDiagnosticsSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 ReceivedFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 AcceptedFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 AppliedFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 RejectedFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 MissingFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 DuplicateFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 OutOfOrderFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 SenderRestarts = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	double SourceFramesPerSecond = 0.0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	double AveragePacketIntervalMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	double ArrivalJitterMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	double LastPacketAgeSeconds = 0.0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	double QualityBufferLatencyMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int32 BufferedPoseCount = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 RejectedMotionOutliers = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	int64 AngularVelocityClamps = 0;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	EArdyStreamHealthState StreamHealth = EArdyStreamHealthState::NoData;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	bool bFrozen = false;

	UPROPERTY(BlueprintReadOnly, Category="ARDY|Diagnostics")
	FString StatusDescription;
};

/**
 * Level-independent runtime entry point for the existing ARDY receiver.
 *
 * UDP, protocol parsing, stabilization, Live Link publication, and retargeting
 * remain owned by UArdyMotionReceiverComponent. This actor coordinates safe
 * lifecycle control, target observation, recovery, and Blueprint diagnostics.
 */
UCLASS(Blueprintable, BlueprintType, meta=(DisplayName="ARDY Runtime"))
class ARDYLIVELINK_API AArdyRuntimeActor : public AActor
{
	GENERATED_BODY()

public:
	AArdyRuntimeActor();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

	/** Explicit target injection; no level actor name is required by default. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="ARDY|Target")
	TObjectPtr<AActor> TargetCharacterActor = nullptr;

	/** Optional direct body override. Leave empty to resolve Body on Target Character. */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category="ARDY|Target")
	TObjectPtr<USkeletalMeshComponent> TargetBodyComponent = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Network", meta=(ClampMin="1", ClampMax="65535"))
	int32 ListenPort = 8091;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|LiveLink")
	FName SubjectName = TEXT("ARDY_Body");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Runtime")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Runtime")
	bool bEnableDiagnostics = true;

	/** Matches the stabilized profile validated in test_JH. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Runtime")
	EArdyMotionQualityProfile MotionQualityProfile =
		EArdyMotionQualityProfile::Balanced;

	/** Keep enabled so the independent Face/LipSync pipeline owns Head and Neck. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Runtime")
	bool bBlockHeadAndNeck = true;

	/** Optional fallback for legacy levels. Explicit Target Character is preferred. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Target", meta=(AdvancedDisplay))
	bool bAutoFindTargetCharacter = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Target", meta=(AdvancedDisplay, EditCondition="bAutoFindTargetCharacter"))
	FString TargetCharacterNamePattern;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Recovery")
	bool bAutoRecoverReceiver = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Recovery", meta=(ClampMin="0.1", ClampMax="60.0"))
	float RecoveryDelaySeconds = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Recovery", meta=(ClampMin="0", ClampMax="100"))
	int32 MaxRecoveryAttempts = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Runtime", meta=(ClampMin="0.1", ClampMax="5.0", AdvancedDisplay))
	float MonitorIntervalSeconds = 0.5f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Runtime")
	EArdyRuntimeState RuntimeState = EArdyRuntimeState::Uninitialized;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Runtime")
	FString RuntimeStatusDescription;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Runtime")
	FString LastRuntimeError;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY|Recovery")
	int32 RecoveryAttemptCount = 0;

	/** Copies settings without implicitly restarting an active socket. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category="ARDY")
	void ApplyRuntimeConfiguration();

	/** Explicit runtime apply; restarts the UDP socket if only the port changed. */
	UFUNCTION(BlueprintCallable, Category="ARDY|Runtime")
	bool ApplyRuntimeSettings();

	UFUNCTION(BlueprintCallable, Category="ARDY|Runtime")
	bool StartReceiver();

	UFUNCTION(BlueprintCallable, Category="ARDY|Runtime")
	void StopReceiver();

	UFUNCTION(BlueprintCallable, Category="ARDY|Runtime")
	bool RestartReceiver();

	/** Resolves explicit references first and performs legacy search only when enabled. */
	UFUNCTION(BlueprintCallable, Category="ARDY|Target")
	bool RefreshTargetBinding();

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	EArdyRuntimeState GetRuntimeState() const { return RuntimeState; }

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	bool IsReceiverActive() const;

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	int32 GetCurrentUdpPort() const;

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	FName GetCurrentLiveLinkSubject() const;

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	double GetLastPacketAgeSeconds() const;

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	bool IsTargetCharacterValid() const;

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	bool IsTargetBodyValid() const;

	UFUNCTION(BlueprintPure, Category="ARDY|Runtime")
	bool IsReceiverComponentValid() const { return IsValid(MotionReceiver); }

	UFUNCTION(BlueprintPure, Category="ARDY|Diagnostics")
	FArdyRuntimeDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

	/** Advanced receiver settings and detailed counters remain available here. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="ARDY")
	TObjectPtr<UArdyMotionReceiverComponent> MotionReceiver;

private:
	UFUNCTION()
	void HandleReceiverError(const FString& Error);

	void HandleRuntimeMonitor();
	void CopySettingsToReceiver(bool bAllowPortRestart);
	bool TryStartReceiver(bool bRecoveryAttempt);
	void UpdateRuntimeState();
	void SetRuntimeState(EArdyRuntimeState NewState, const FString& Description);
	void ShutdownRuntime();
	USkeletalMeshComponent* FindBodyOnActor(AActor* Character) const;

	UPROPERTY(VisibleAnywhere, Category="ARDY")
	TObjectPtr<USceneComponent> SceneRoot;

	FTimerHandle RuntimeMonitorTimer;
	TWeakObjectPtr<AActor> LastObservedTargetCharacter;
	TWeakObjectPtr<USkeletalMeshComponent> LastObservedTargetBody;
	double LastRecoveryAttemptTimeSeconds = -1.0;
	int32 ActiveListenPort = 0;
	FName ActiveSubjectName = NAME_None;
	bool bHadValidTargetCharacter = false;
	bool bHadValidTargetBody = false;
	bool bReceiverDesired = false;
	bool bRuntimeEnding = false;
	bool bPortRestartRequired = false;
	bool bPipelineRefreshRequired = false;
};

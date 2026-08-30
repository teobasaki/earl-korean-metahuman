#include "Team/ArdyRuntimeActor.h"

#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogArdyRuntime, Log, All);

namespace
{
	FString RuntimeStateName(const EArdyRuntimeState State)
	{
		return StaticEnum<EArdyRuntimeState>()->GetNameStringByValue(
			static_cast<int64>(State));
	}

	FString StreamHealthName(const EArdyStreamHealthState State)
	{
		return StaticEnum<EArdyStreamHealthState>()->GetNameStringByValue(
			static_cast<int64>(State));
	}
}

AArdyRuntimeActor::AArdyRuntimeActor()
{
	PrimaryActorTick.bCanEverTick = false;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	MotionReceiver = CreateDefaultSubobject<UArdyMotionReceiverComponent>(
		TEXT("ARDYMotionReceiver"));
	MotionReceiver->bAutoFindTargetCharacter = false;
	MotionReceiver->TargetCharacterNamePattern.Reset();
}

void AArdyRuntimeActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	ApplyRuntimeConfiguration();
}

void AArdyRuntimeActor::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	ApplyRuntimeConfiguration();
	RefreshTargetBinding();
}

void AArdyRuntimeActor::BeginPlay()
{
	Super::BeginPlay();
	bRuntimeEnding = false;

	if (!MotionReceiver)
	{
		SetRuntimeState(
			EArdyRuntimeState::Error,
			TEXT("Receiver Component is missing"));
		return;
	}

	MotionReceiver->OnMotionError.RemoveDynamic(
		this,
		&AArdyRuntimeActor::HandleReceiverError);
	MotionReceiver->OnMotionError.AddDynamic(
		this,
		&AArdyRuntimeActor::HandleReceiverError);

	if (MotionReceiver->IsReceiving())
	{
		ActiveListenPort = MotionReceiver->ListenPort;
		ActiveSubjectName = MotionReceiver->SubjectName;
	}

	ApplyRuntimeConfiguration();
	RefreshTargetBinding();
	bReceiverDesired = bAutoStart || MotionReceiver->IsReceiving();
	if (bAutoStart)
	{
		StartReceiver();
	}
	else
	{
		UpdateRuntimeState();
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			RuntimeMonitorTimer,
			this,
			&AArdyRuntimeActor::HandleRuntimeMonitor,
			FMath::Max(0.1f, MonitorIntervalSeconds),
			true);
	}
}

void AArdyRuntimeActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ShutdownRuntime();
	Super::EndPlay(EndPlayReason);
}

void AArdyRuntimeActor::Destroyed()
{
	ShutdownRuntime();
	Super::Destroyed();
}

void AArdyRuntimeActor::ApplyRuntimeConfiguration()
{
	CopySettingsToReceiver(false);
}

bool AArdyRuntimeActor::ApplyRuntimeSettings()
{
	CopySettingsToReceiver(true);
	RefreshTargetBinding();
	UpdateRuntimeState();
	return MotionReceiver &&
		!bPortRestartRequired &&
		!bPipelineRefreshRequired &&
		(!bReceiverDesired || IsReceiverActive());
}

bool AArdyRuntimeActor::StartReceiver()
{
	bReceiverDesired = true;
	RecoveryAttemptCount = 0;
	LastRecoveryAttemptTimeSeconds = -1.0;
	CopySettingsToReceiver(false);
	RefreshTargetBinding();
	return TryStartReceiver(false) && !bPipelineRefreshRequired;
}

void AArdyRuntimeActor::StopReceiver()
{
	bReceiverDesired = false;
	RecoveryAttemptCount = 0;
	LastRecoveryAttemptTimeSeconds = -1.0;
	if (MotionReceiver)
	{
		MotionReceiver->StopReceiver();
	}
	SetRuntimeState(EArdyRuntimeState::Stopped, TEXT("Receiver stopped explicitly"));
}

bool AArdyRuntimeActor::RestartReceiver()
{
	bReceiverDesired = true;
	RecoveryAttemptCount = 0;
	LastRecoveryAttemptTimeSeconds = -1.0;
	if (!MotionReceiver)
	{
		LastRuntimeError = TEXT("Receiver Component is missing");
		SetRuntimeState(EArdyRuntimeState::Error, LastRuntimeError);
		return false;
	}

	MotionReceiver->StopReceiver();
	CopySettingsToReceiver(false);
	const bool bStarted = TryStartReceiver(false);
	return bStarted && !bPipelineRefreshRequired;
}

bool AArdyRuntimeActor::RefreshTargetBinding()
{
	AActor* ResolvedCharacter = IsValid(TargetCharacterActor)
		? TargetCharacterActor.Get()
		: nullptr;
	USkeletalMeshComponent* ResolvedBody = IsValid(TargetBodyComponent)
		? TargetBodyComponent.Get()
		: nullptr;

	// A body resolved for the previous actor must not override a newly assigned actor.
	if (ResolvedCharacter &&
		ResolvedCharacter != LastObservedTargetCharacter.Get() &&
		ResolvedBody == LastObservedTargetBody.Get())
	{
		ResolvedBody = nullptr;
	}

	if (!ResolvedCharacter && ResolvedBody)
	{
		ResolvedCharacter = ResolvedBody->GetOwner();
	}
	if (!ResolvedBody && ResolvedCharacter)
	{
		ResolvedBody = FindBodyOnActor(ResolvedCharacter);
	}

	// The legacy scan is intentionally throttled by the runtime monitor timer.
	if ((!ResolvedCharacter || !ResolvedBody) &&
		bAutoFindTargetCharacter &&
		GetWorld())
	{
		for (TActorIterator<AActor> ActorIt(GetWorld()); ActorIt; ++ActorIt)
		{
			AActor* Candidate = *ActorIt;
			if (!Candidate || Candidate == this)
			{
				continue;
			}
			if (!TargetCharacterNamePattern.IsEmpty() &&
				!Candidate->GetName().Contains(
					TargetCharacterNamePattern,
					ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (USkeletalMeshComponent* Body = FindBodyOnActor(Candidate))
			{
				ResolvedCharacter = Candidate;
				ResolvedBody = Body;
				break;
			}
		}
	}

	const bool bBindingChanged =
		ResolvedCharacter != LastObservedTargetCharacter.Get() ||
		ResolvedBody != LastObservedTargetBody.Get() ||
		IsValid(ResolvedCharacter) != bHadValidTargetCharacter ||
		IsValid(ResolvedBody) != bHadValidTargetBody;
	TargetCharacterActor = ResolvedCharacter;
	TargetBodyComponent = ResolvedBody;

	if (MotionReceiver)
	{
		const bool bPipelineHasBegunPlay = MotionReceiver->HasBegunPlay();
		const bool bReceiverAlreadyMatches =
			MotionReceiver->TargetCharacterActor == ResolvedCharacter &&
			MotionReceiver->TargetBodyComponent == ResolvedBody;
		if (!bPipelineHasBegunPlay || bReceiverAlreadyMatches)
		{
			MotionReceiver->TargetCharacterActor = ResolvedCharacter;
			MotionReceiver->TargetBodyComponent = ResolvedBody;
		}
		else if (bBindingChanged)
		{
			bPipelineRefreshRequired = true;
			LastRuntimeError = TEXT(
				"Target binding changed after the pose pipeline initialized; "
				"Receiver rebind API is required");
		}
	}

	if (bBindingChanged)
	{
		UE_LOG(
			LogArdyRuntime,
			Log,
			TEXT("[ARDY RUNTIME] Target observed: Actor=%s Body=%s PipelineRefresh=%s"),
			*GetNameSafe(ResolvedCharacter),
			*GetNameSafe(ResolvedBody),
			bPipelineRefreshRequired ? TEXT("required") : TEXT("no"));
	}

	LastObservedTargetCharacter = ResolvedCharacter;
	LastObservedTargetBody = ResolvedBody;
	bHadValidTargetCharacter = IsValid(ResolvedCharacter);
	bHadValidTargetBody = IsValid(ResolvedBody);
	return IsValid(ResolvedBody);
}

bool AArdyRuntimeActor::IsReceiverActive() const
{
	return MotionReceiver && MotionReceiver->IsReceiving();
}

int32 AArdyRuntimeActor::GetCurrentUdpPort() const
{
	return IsReceiverActive() && ActiveListenPort > 0
		? ActiveListenPort
		: ListenPort;
}

FName AArdyRuntimeActor::GetCurrentLiveLinkSubject() const
{
	return !ActiveSubjectName.IsNone()
		? ActiveSubjectName
		: MotionReceiver
			? MotionReceiver->SubjectName
			: SubjectName;
}

double AArdyRuntimeActor::GetLastPacketAgeSeconds() const
{
	return MotionReceiver
		? FMath::Max(0.0, MotionReceiver->TimeSinceLastPacketMs / 1000.0)
		: 0.0;
}

bool AArdyRuntimeActor::IsTargetCharacterValid() const
{
	return IsValid(TargetCharacterActor);
}

bool AArdyRuntimeActor::IsTargetBodyValid() const
{
	return IsValid(TargetBodyComponent);
}

FArdyRuntimeDiagnosticsSnapshot AArdyRuntimeActor::GetDiagnosticsSnapshot() const
{
	FArdyRuntimeDiagnosticsSnapshot Snapshot;
	Snapshot.StatusDescription = RuntimeStatusDescription;
	if (!MotionReceiver)
	{
		return Snapshot;
	}

	Snapshot.ReceivedFrames = MotionReceiver->ReceivedPacketCount;
	Snapshot.AcceptedFrames = MotionReceiver->AcceptedFrameCount;
	Snapshot.AppliedFrames = MotionReceiver->AppliedFrameCount;
	Snapshot.RejectedFrames = MotionReceiver->RejectedPacketCount;
	Snapshot.MissingFrames = MotionReceiver->MissingFrameCount;
	Snapshot.DuplicateFrames = MotionReceiver->DuplicateFrameCount;
	Snapshot.OutOfOrderFrames = MotionReceiver->OutOfOrderFrameCount;
	Snapshot.SenderRestarts = MotionReceiver->FrameRestartCount;
	Snapshot.SourceFramesPerSecond = MotionReceiver->EstimatedSourceFPS;
	Snapshot.AveragePacketIntervalMs = MotionReceiver->AverageArrivalDeltaMs;
	Snapshot.ArrivalJitterMs = MotionReceiver->ArrivalJitterMs;
	Snapshot.LastPacketAgeSeconds = GetLastPacketAgeSeconds();
	Snapshot.QualityBufferLatencyMs = MotionReceiver->MotionQualityLatencyMs;
	Snapshot.BufferedPoseCount = MotionReceiver->BufferedPoseCount;
	Snapshot.RejectedMotionOutliers = MotionReceiver->RejectedMotionOutlierCount;
	Snapshot.AngularVelocityClamps = MotionReceiver->AngularVelocityClampCount;
	Snapshot.StreamHealth = MotionReceiver->StreamHealthState;
	Snapshot.bFrozen =
		MotionReceiver->StreamHealthState == EArdyStreamHealthState::Frozen ||
		MotionReceiver->StreamHealthState == EArdyStreamHealthState::SevereFrozen;
	return Snapshot;
}

void AArdyRuntimeActor::HandleReceiverError(const FString& Error)
{
	LastRuntimeError = Error;
	SetRuntimeState(
		IsReceiverActive()
			? EArdyRuntimeState::Degraded
			: EArdyRuntimeState::Error,
		Error);
}

void AArdyRuntimeActor::HandleRuntimeMonitor()
{
	if (bRuntimeEnding)
	{
		return;
	}

	RefreshTargetBinding();
	if (MotionReceiver && MotionReceiver->HasBegunPlay() &&
		MotionReceiver->SubjectName != SubjectName)
	{
		bPipelineRefreshRequired = true;
		LastRuntimeError = TEXT(
			"Live Link Subject changed after initialization; "
			"Receiver pipeline restart API is required");
	}
	if (IsReceiverActive() && ActiveListenPort > 0 &&
		ActiveListenPort != ListenPort)
	{
		bPortRestartRequired = true;
	}

	if (bReceiverDesired && !IsReceiverActive() && bAutoRecoverReceiver)
	{
		const double NowSeconds = FPlatformTime::Seconds();
		const bool bDelayElapsed =
			LastRecoveryAttemptTimeSeconds < 0.0 ||
			NowSeconds - LastRecoveryAttemptTimeSeconds >=
				FMath::Max(0.1f, RecoveryDelaySeconds);
		if (RecoveryAttemptCount < MaxRecoveryAttempts && bDelayElapsed)
		{
			++RecoveryAttemptCount;
			LastRecoveryAttemptTimeSeconds = NowSeconds;
			TryStartReceiver(true);
		}
	}

	UpdateRuntimeState();
}

void AArdyRuntimeActor::CopySettingsToReceiver(const bool bAllowPortRestart)
{
	if (!MotionReceiver)
	{
		LastRuntimeError = TEXT("Receiver Component is missing");
		SetRuntimeState(EArdyRuntimeState::Error, LastRuntimeError);
		return;
	}

	const bool bWasReceiving = MotionReceiver->IsReceiving();
	const int32 PreviousActivePort = bWasReceiving && ActiveListenPort > 0
		? ActiveListenPort
		: MotionReceiver->ListenPort;
	const bool bPortChanged = PreviousActivePort != ListenPort;
	const bool bPipelineHasBegunPlay = MotionReceiver->HasBegunPlay();

	MotionReceiver->bAutoStart = bAutoStart;
	MotionReceiver->bEnableDiagnosticLogging = bEnableDiagnostics;
	MotionReceiver->MotionQualityProfile = MotionQualityProfile;
	MotionReceiver->bBlockHeadAndNeck = bBlockHeadAndNeck;
	MotionReceiver->bAutoFindTargetCharacter = bAutoFindTargetCharacter;
	MotionReceiver->TargetCharacterNamePattern = TargetCharacterNamePattern;
	MotionReceiver->ListenPort = ListenPort;
	bPortRestartRequired = bWasReceiving && bPortChanged;

	if (!bPipelineHasBegunPlay)
	{
		MotionReceiver->SubjectName = SubjectName;
		MotionReceiver->TargetCharacterActor = TargetCharacterActor;
		MotionReceiver->TargetBodyComponent = TargetBodyComponent;
		ActiveSubjectName = SubjectName;
		bPipelineRefreshRequired = false;
	}
	else if (MotionReceiver->SubjectName != SubjectName)
	{
		bPipelineRefreshRequired = true;
		LastRuntimeError = TEXT(
			"Live Link Subject cannot be changed after pose pipeline initialization");
	}

	if (bAllowPortRestart && bWasReceiving && bPortChanged)
	{
		MotionReceiver->StopReceiver();
		SetRuntimeState(
			EArdyRuntimeState::Starting,
			FString::Printf(TEXT("Restarting receiver on UDP %d"), ListenPort));
		if (MotionReceiver->StartReceiver())
		{
			ActiveListenPort = ListenPort;
			ActiveSubjectName = MotionReceiver->SubjectName;
			bPortRestartRequired = false;
		}
		else if (LastRuntimeError.IsEmpty())
		{
			LastRuntimeError = FString::Printf(
				TEXT("Could not restart receiver on UDP %d"),
				ListenPort);
		}
	}
}

bool AArdyRuntimeActor::TryStartReceiver(const bool bRecoveryAttempt)
{
	if (!MotionReceiver)
	{
		LastRuntimeError = TEXT("Receiver Component is missing");
		SetRuntimeState(EArdyRuntimeState::Error, LastRuntimeError);
		return false;
	}
	if (MotionReceiver->IsReceiving())
	{
		ActiveListenPort = ActiveListenPort > 0
			? ActiveListenPort
			: MotionReceiver->ListenPort;
		ActiveSubjectName = MotionReceiver->SubjectName;
		UpdateRuntimeState();
		return true;
	}

	SetRuntimeState(
		EArdyRuntimeState::Starting,
		bRecoveryAttempt
			? FString::Printf(
				TEXT("Recovery attempt %d of %d"),
				RecoveryAttemptCount,
				MaxRecoveryAttempts)
			: TEXT("Starting receiver"));
	const bool bStarted = MotionReceiver->StartReceiver();
	if (!bStarted)
	{
		if (LastRuntimeError.IsEmpty())
		{
			LastRuntimeError = FString::Printf(
				TEXT("Could not start receiver on UDP %d"),
				MotionReceiver->ListenPort);
		}
		SetRuntimeState(EArdyRuntimeState::Error, LastRuntimeError);
		return false;
	}

	ActiveListenPort = MotionReceiver->ListenPort;
	ActiveSubjectName = MotionReceiver->SubjectName;
	bPortRestartRequired = false;
	if (!bPipelineRefreshRequired)
	{
		LastRuntimeError.Reset();
	}
	UpdateRuntimeState();
	return true;
}

void AArdyRuntimeActor::UpdateRuntimeState()
{
	if (bRuntimeEnding)
	{
		SetRuntimeState(EArdyRuntimeState::Stopped, TEXT("Runtime is ending"));
		return;
	}
	if (!MotionReceiver)
	{
		SetRuntimeState(EArdyRuntimeState::Error, TEXT("Receiver Component is missing"));
		return;
	}
	if (!bReceiverDesired)
	{
		SetRuntimeState(EArdyRuntimeState::Stopped, TEXT("Receiver is not requested"));
		return;
	}
	if (!MotionReceiver->IsReceiving())
	{
		if (bAutoRecoverReceiver && RecoveryAttemptCount < MaxRecoveryAttempts)
		{
			SetRuntimeState(
				EArdyRuntimeState::Starting,
				TEXT("Receiver inactive; waiting for recovery"));
		}
		else
		{
			SetRuntimeState(
				EArdyRuntimeState::Error,
				LastRuntimeError.IsEmpty()
					? TEXT("Receiver inactive")
					: LastRuntimeError);
		}
		return;
	}
	if (bPipelineRefreshRequired)
	{
		SetRuntimeState(
			EArdyRuntimeState::Degraded,
			LastRuntimeError.IsEmpty()
				? TEXT("Pose pipeline refresh is required")
				: LastRuntimeError);
		return;
	}
	if (bPortRestartRequired)
	{
		SetRuntimeState(
			EArdyRuntimeState::Degraded,
			TEXT("UDP port changed; call Apply Runtime Settings or Restart Receiver"));
		return;
	}
	if (!IsTargetCharacterValid())
	{
		SetRuntimeState(
			EArdyRuntimeState::Degraded,
			TEXT("Receiver active; target character is unavailable"));
		return;
	}
	if (!IsTargetBodyValid())
	{
		SetRuntimeState(
			EArdyRuntimeState::Degraded,
			TEXT("Receiver active; target Body Mesh is unavailable"));
		return;
	}

	switch (MotionReceiver->StreamHealthState)
	{
	case EArdyStreamHealthState::Healthy:
		SetRuntimeState(EArdyRuntimeState::Receiving, TEXT("Receiving healthy motion frames"));
		break;
	case EArdyStreamHealthState::Delayed:
		SetRuntimeState(
			EArdyRuntimeState::Degraded,
			TEXT("Motion stream is delayed"));
		break;
	case EArdyStreamHealthState::Frozen:
	case EArdyStreamHealthState::SevereFrozen:
		SetRuntimeState(
			EArdyRuntimeState::Frozen,
			TEXT("Sender is silent; receiver remains ready"));
		break;
	case EArdyStreamHealthState::NoData:
	default:
		SetRuntimeState(
			EArdyRuntimeState::Ready,
			TEXT("Receiver ready; waiting for sender"));
		break;
	}
}

void AArdyRuntimeActor::SetRuntimeState(
	const EArdyRuntimeState NewState,
	const FString& Description)
{
	const EArdyRuntimeState PreviousState = RuntimeState;
	const bool bStateChanged = PreviousState != NewState;
	RuntimeState = NewState;
	RuntimeStatusDescription = Description;
	if (!bStateChanged)
	{
		return;
	}

	const FString Transition = FString::Printf(
		TEXT("[ARDY RUNTIME] %s -> %s Port=%d Subject=%s Target=%s Body=%s ")
		TEXT("Recovery=%d/%d Detail=%s"),
		*RuntimeStateName(PreviousState),
		*RuntimeStateName(NewState),
		GetCurrentUdpPort(),
		*GetCurrentLiveLinkSubject().ToString(),
		*GetNameSafe(TargetCharacterActor),
		*GetNameSafe(TargetBodyComponent),
		RecoveryAttemptCount,
		MaxRecoveryAttempts,
		*Description);
	if (NewState == EArdyRuntimeState::Error)
	{
		UE_LOG(LogArdyRuntime, Warning, TEXT("%s"), *Transition);
	}
	else
	{
		UE_LOG(LogArdyRuntime, Log, TEXT("%s"), *Transition);
	}
}

void AArdyRuntimeActor::ShutdownRuntime()
{
	if (bRuntimeEnding)
	{
		return;
	}
	bRuntimeEnding = true;
	bReceiverDesired = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RuntimeMonitorTimer);
	}
	if (MotionReceiver)
	{
		MotionReceiver->OnMotionError.RemoveDynamic(
			this,
			&AArdyRuntimeActor::HandleReceiverError);
		MotionReceiver->StopReceiver();
	}
	SetRuntimeState(EArdyRuntimeState::Stopped, TEXT("Runtime shutdown complete"));
}

USkeletalMeshComponent* AArdyRuntimeActor::FindBodyOnActor(AActor* Character) const
{
	if (!IsValid(Character))
	{
		return nullptr;
	}

	TInlineComponentArray<USkeletalMeshComponent*> MeshComponents;
	Character->GetComponents(MeshComponents);
	const FName PreferredName = MotionReceiver
		? MotionReceiver->TargetBodyComponentName
		: FName(TEXT("Body"));
	for (USkeletalMeshComponent* MeshComponent : MeshComponents)
	{
		if (IsValid(MeshComponent) && MeshComponent->GetFName() == PreferredName)
		{
			return MeshComponent;
		}
	}
	for (USkeletalMeshComponent* MeshComponent : MeshComponents)
	{
		if (!IsValid(MeshComponent))
		{
			continue;
		}
		const FString ComponentName = MeshComponent->GetName();
		if (ComponentName.Contains(TEXT("Body"), ESearchCase::IgnoreCase) &&
			!ComponentName.Contains(TEXT("Face"), ESearchCase::IgnoreCase))
		{
			return MeshComponent;
		}
	}
	return nullptr;
}

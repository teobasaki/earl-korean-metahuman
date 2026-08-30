#include "Team/ArdyMotionReceiverComponent.h"

#include "Team/ArdyMotionLiveLinkSource.h"
#include "Team/ArdyTeamRetargetAnimInstance.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "LiveLinkInstance.h"
#include "Retargeter/IKRetargeter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Common/UdpSocketBuilder.h"

DEFINE_LOG_CATEGORY_STATIC(LogArdyMotion, Log, All);

namespace
{
	const FSoftObjectPath DefaultSourceMeshPath(
		TEXT("/Game/Retarget/ARDY_explain_both_02.ARDY_explain_both_02"));
	const FSoftObjectPath DefaultRetargeterPath(
		TEXT("/Game/Retarget/RTG_ARDY_to_CharacterTeam.RTG_ARDY_to_CharacterTeam"));

	bool ParseHandPoseTag(const FName Tag, EArdyHandPose& OutPose)
	{
		if (Tag.IsNone())
		{
			return false;
		}
		const FString Value = Tag.ToString();
		if (Value.Equals(TEXT("reference"), ESearchCase::IgnoreCase) ||
			Value.Equals(TEXT("off"), ESearchCase::IgnoreCase))
		{
			OutPose = EArdyHandPose::Reference;
		}
		else if (Value.Equals(TEXT("relaxed"), ESearchCase::IgnoreCase))
		{
			OutPose = EArdyHandPose::Relaxed;
		}
		else if (Value.Equals(TEXT("open"), ESearchCase::IgnoreCase) ||
			Value.Equals(TEXT("open_palm"), ESearchCase::IgnoreCase))
		{
			OutPose = EArdyHandPose::OpenPalm;
		}
		else if (Value.Equals(TEXT("point"), ESearchCase::IgnoreCase))
		{
			OutPose = EArdyHandPose::Point;
		}
		else if (Value.Equals(TEXT("explain"), ESearchCase::IgnoreCase))
		{
			OutPose = EArdyHandPose::Explain;
		}
		else if (Value.Equals(TEXT("soft_fist"), ESearchCase::IgnoreCase) ||
			Value.Equals(TEXT("fist"), ESearchCase::IgnoreCase))
		{
			OutPose = EArdyHandPose::SoftFist;
		}
		else if (Value.Equals(TEXT("retarget"), ESearchCase::IgnoreCase))
		{
			OutPose = EArdyHandPose::Retarget;
		}
		else
		{
			return false;
		}
		return true;
	}
}

UArdyMotionReceiverComponent::UArdyMotionReceiverComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SourceSkeletalMesh = TSoftObjectPtr<USkeletalMesh>(DefaultSourceMeshPath);
	IKRetargeterAsset = TSoftObjectPtr<UIKRetargeter>(DefaultRetargeterPath);
}

UArdyMotionReceiverComponent::~UArdyMotionReceiverComponent()
{
	StopReceiver();
}

void UArdyMotionReceiverComponent::BeginPlay()
{
	Super::BeginPlay();

	ResetDiagnostics();
	bPosePipelineReady = InitializePosePipeline();
	if (bAutoStart)
	{
		StartReceiver();
	}
}

void UArdyMotionReceiverComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopReceiver();
	ShutdownPosePipeline();
	Super::EndPlay(EndPlayReason);
}

void UArdyMotionReceiverComponent::TickComponent(
	const float DeltaTime,
	const ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (ActiveMotionQualityProfile != MotionQualityProfile)
	{
		ActiveMotionQualityProfile = MotionQualityProfile;
		MotionStabilizer.SetProfile(MotionQualityProfile);
		UE_LOG(
			LogArdyMotion,
			Log,
			TEXT("[ARDY QUALITY] Profile=%s"),
			*StaticEnum<EArdyMotionQualityProfile>()->GetNameStringByValue(
				static_cast<int64>(MotionQualityProfile)));
	}
	ApplyNewestPendingFrame();
	const double NowSeconds = FPlatformTime::Seconds();
	RefreshDiagnosticProperties(NowSeconds);
	UpdateStreamHealth(NowSeconds);
	LogDiagnosticSummary(NowSeconds);
}

bool UArdyMotionReceiverComponent::InitializePosePipeline()
{
	LoadedSourceMesh = SourceSkeletalMesh.LoadSynchronous();
	if (!LoadedSourceMesh)
	{
		ReportError(FString::Printf(
			TEXT("Could not load ARDY source skeletal mesh: %s"),
			*SourceSkeletalMesh.ToSoftObjectPath().ToString()));
		return false;
	}

	const FReferenceSkeleton& ReferenceSkeleton = LoadedSourceMesh->GetRefSkeleton();
	ReferenceLocalPose = ReferenceSkeleton.GetRefBonePose();
	if (ReferenceLocalPose.IsEmpty())
	{
		ReportError(TEXT("ARDY source skeletal mesh has no reference bones"));
		return false;
	}

	TArray<FName> BoneNames;
	TArray<int32> BoneParents;
	BoneNames.Reserve(ReferenceSkeleton.GetNum());
	BoneParents.Reserve(ReferenceSkeleton.GetNum());
	BoneIndexByName.Reset();
	for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetNum(); ++BoneIndex)
	{
		const FName BoneName = ReferenceSkeleton.GetBoneName(BoneIndex);
		const int32 ParentIndex = ReferenceSkeleton.GetParentIndex(BoneIndex);
		BoneNames.Add(BoneName);
		BoneParents.Add(ParentIndex);
		BoneIndexByName.Add(BoneName, BoneIndex);
		if (ParentIndex == INDEX_NONE)
		{
			RootBoneIndex = BoneIndex;
		}
	}

	if (!RegisterLiveLinkSource() ||
		!LiveLinkSource->PublishSkeleton(SubjectName, BoneNames, BoneParents))
	{
		ReportError(TEXT("Could not register the ARDY_Body Live Link subject"));
		return false;
	}

	USkeletalMeshComponent* ResolvedTargetBody = ResolveTargetBodyComponent();
	if (!TargetBodyComponent)
	{
		TargetBodyComponent = ResolvedTargetBody;
	}

	if (!RuntimeSourcePoseComponent)
	{
		AActor* Owner = GetOwner();
		if (!Owner)
		{
			ReportError(TEXT("ARDY receiver requires an owning actor"));
			return false;
		}

		RuntimeSourcePoseComponent = NewObject<USkeletalMeshComponent>(
			Owner,
			TEXT("ARDY_RuntimeSourcePose"));
		RuntimeSourcePoseComponent->SetupAttachment(Owner->GetRootComponent());
		RuntimeSourcePoseComponent->RegisterComponent();
		bCreatedSourcePoseComponent = true;
	}

	RuntimeSourcePoseComponent->SetSkeletalMesh(LoadedSourceMesh);
	RuntimeSourcePoseComponent->VisibilityBasedAnimTickOption =
		EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	RuntimeSourcePoseComponent->SetCastShadow(false);
	RuntimeSourcePoseComponent->SetHiddenInGame(!bShowSourcePose);
	RuntimeSourcePoseComponent->SetVisibility(bShowSourcePose, true);
	RuntimeSourcePoseComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	RuntimeSourcePoseComponent->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());

	ULiveLinkInstance* LiveLinkInstance =
		Cast<ULiveLinkInstance>(RuntimeSourcePoseComponent->GetAnimInstance());
	if (!LiveLinkInstance)
	{
		ReportError(TEXT("Could not create Live Link AnimInstance for the ARDY source mesh"));
		return false;
	}
	LiveLinkInstance->SetSubject(FLiveLinkSubjectName(SubjectName));
	LiveLinkInstance->EnableLiveLinkEvaluation(true);

	if (bAutoConfigureRetarget)
	{
		LoadedRetargeter = IKRetargeterAsset.LoadSynchronous();
		if (!LoadedRetargeter)
		{
			ReportError(FString::Printf(
				TEXT("Could not load ARDY IK Retargeter: %s"),
				*IKRetargeterAsset.ToSoftObjectPath().ToString()));
			return false;
		}
		if (!TargetBodyComponent)
		{
			ReportError(FString::Printf(
				TEXT("Could not find MetaHuman body component '%s'; assign TargetBodyComponent"),
				*TargetBodyComponentName.ToString()));
			return false;
		}

		TargetBodyComponent->AddTickPrerequisiteComponent(RuntimeSourcePoseComponent);
		UArdyTeamRetargetAnimInstance* TargetAnimInstance =
			Cast<UArdyTeamRetargetAnimInstance>(TargetBodyComponent->GetAnimInstance());
		if (!TargetAnimInstance && !bPreserveTargetAnimInstance)
		{
			TargetBodyComponent->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			TargetBodyComponent->SetAnimInstanceClass(UArdyTeamRetargetAnimInstance::StaticClass());
			TargetAnimInstance =
				Cast<UArdyTeamRetargetAnimInstance>(TargetBodyComponent->GetAnimInstance());
		}
		if (!TargetAnimInstance)
		{
			ReportError(FString::Printf(
				TEXT("ARDY target requires UArdyTeamRetargetAnimInstance while target preservation is %s; current=%s"),
				bPreserveTargetAnimInstance ? TEXT("enabled") : TEXT("disabled"),
				*GetNameSafe(TargetBodyComponent->GetAnimInstance())));
			return false;
		}
		TargetAnimInstance->Configure(LoadedRetargeter, RuntimeSourcePoseComponent);
		TargetAnimInstance->ConfigureQuality(PoseAugmentation);
	}

	LiveLinkSource->PublishFrame(SubjectName, ReferenceLocalPose, 0, 0);
	UE_LOG(LogArdyMotion, Log,
		TEXT("ARDY pose pipeline ready: Subject=%s Bones=%d Source=%s Target=%s Retarget=%s"),
		*SubjectName.ToString(),
		ReferenceLocalPose.Num(),
		*GetNameSafe(RuntimeSourcePoseComponent),
		*GetNameSafe(TargetBodyComponent),
		*GetNameSafe(LoadedRetargeter));
	return true;
}

void UArdyMotionReceiverComponent::ShutdownPosePipeline()
{
	if (TargetBodyComponent && RuntimeSourcePoseComponent)
	{
		TargetBodyComponent->RemoveTickPrerequisiteComponent(RuntimeSourcePoseComponent);
	}
	UnregisterLiveLinkSource();
	if (bCreatedSourcePoseComponent && RuntimeSourcePoseComponent)
	{
		RuntimeSourcePoseComponent->DestroyComponent();
	}

	RuntimeSourcePoseComponent = nullptr;
	LoadedSourceMesh = nullptr;
	LoadedRetargeter = nullptr;
	ReferenceLocalPose.Reset();
	BoneIndexByName.Reset();
	RootBoneIndex = INDEX_NONE;
	bCreatedSourcePoseComponent = false;
	bPosePipelineReady = false;
}

bool UArdyMotionReceiverComponent::ConfigureExistingTargetAnimInstance()
{
	if (!bPosePipelineReady || !TargetBodyComponent ||
		!RuntimeSourcePoseComponent || !LoadedRetargeter)
	{
		return false;
	}

	UArdyTeamRetargetAnimInstance* TargetAnimInstance =
		Cast<UArdyTeamRetargetAnimInstance>(TargetBodyComponent->GetAnimInstance());
	if (!TargetAnimInstance)
	{
		ReportError(FString::Printf(
			TEXT("Cannot configure existing ARDY target AnimInstance: current=%s"),
			*GetNameSafe(TargetBodyComponent->GetAnimInstance())));
		return false;
	}

	TargetBodyComponent->AddTickPrerequisiteComponent(RuntimeSourcePoseComponent);
	TargetAnimInstance->Configure(LoadedRetargeter, RuntimeSourcePoseComponent);
	TargetAnimInstance->ConfigureQuality(PoseAugmentation);
	return true;
}

bool UArdyMotionReceiverComponent::RegisterLiveLinkSource()
{
	if (LiveLinkSource.IsValid())
	{
		return true;
	}
	if (!IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		return false;
	}

	ILiveLinkClient& LiveLinkClient =
		IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(
			ILiveLinkClient::ModularFeatureName);
	LiveLinkSource = MakeShared<FArdyMotionLiveLinkSource>();
	LiveLinkSourceGuid = LiveLinkClient.AddSource(LiveLinkSource);
	return LiveLinkSourceGuid.IsValid();
}

void UArdyMotionReceiverComponent::UnregisterLiveLinkSource()
{
	if (LiveLinkSourceGuid.IsValid() &&
		IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		ILiveLinkClient& LiveLinkClient =
			IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(
				ILiveLinkClient::ModularFeatureName);
		LiveLinkClient.RemoveSource(LiveLinkSourceGuid);
	}
	LiveLinkSourceGuid.Invalidate();
	LiveLinkSource.Reset();
}

bool UArdyMotionReceiverComponent::StartReceiver()
{
	if (MotionReceiver.IsValid())
	{
		return true;
	}

	MotionSocket = FUdpSocketBuilder(TEXT("ArdyMotionReceiver"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToPort(static_cast<uint16>(ListenPort))
		.WithReceiveBufferSize(256 * 1024);
	if (!MotionSocket)
	{
		ReportError(FString::Printf(TEXT("Could not bind ARDY UDP port %d"), ListenPort));
		return false;
	}

	MotionReceiver = MakeUnique<FUdpSocketReceiver>(
		MotionSocket,
		FTimespan::FromMilliseconds(10),
		TEXT("ArdyMotionUdpReceiver"));
	MotionReceiver->SetMaxReadBufferSize(FArdyMotionProtocol::MaxPacketBytes);
	MotionReceiver->OnDataReceived().BindLambda(
		[WeakThis = TWeakObjectPtr<UArdyMotionReceiverComponent>(this)](
			const FArrayReaderPtr& Data,
			const FIPv4Endpoint&)
		{
			if (UArdyMotionReceiverComponent* Self = WeakThis.Get())
			{
				Self->QueuePacket(Data);
			}
		});
	MotionReceiver->Start();

	UE_LOG(LogArdyMotion, Log, TEXT("Listening for ardy.motion.v1 on UDP %d"), ListenPort);
	return true;
}

void UArdyMotionReceiverComponent::StopReceiver()
{
	if (MotionReceiver.IsValid())
	{
		MotionReceiver->Stop();
		MotionReceiver.Reset();
	}
	if (MotionSocket)
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(MotionSocket);
		MotionSocket = nullptr;
	}
}

bool UArdyMotionReceiverComponent::IsReceiving() const
{
	return MotionReceiver.IsValid();
}

void UArdyMotionReceiverComponent::QueuePacket(const FArrayReaderPtr& Data)
{
	FQueuedMotionPacket Packet;
	Packet.ArrivalTimeSeconds = FPlatformTime::Seconds();
	if (!Data.IsValid() || Data->IsEmpty() || Data->Num() > FArdyMotionProtocol::MaxPacketBytes)
	{
		Packet.ParseError = TEXT("packet is empty or exceeds the 64 KiB limit");
		PendingPackets.Enqueue(MoveTemp(Packet));
		return;
	}

	const FUTF8ToTCHAR Convert(
		reinterpret_cast<const ANSICHAR*>(Data->GetData()),
		Data->Num());
	const FString Payload(Convert.Length(), Convert.Get());

	if (!FArdyMotionProtocol::ParseJson(Payload, Packet.Frame, Packet.ParseError))
	{
		PendingPackets.Enqueue(MoveTemp(Packet));
		return;
	}

	Packet.bParsed = true;
	PendingPackets.Enqueue(MoveTemp(Packet));
}

void UArdyMotionReceiverComponent::ApplyNewestPendingFrame()
{
	FArdyMotionFrame NewestFrame;
	bool bFoundFrame = false;
	const bool bUseMotionQuality =
		MotionQualityProfile != EArdyMotionQualityProfile::Raw;
	FQueuedMotionPacket Packet;
	while (PendingPackets.Dequeue(Packet))
	{
		Diagnostics.RecordPacketArrival(Packet.ArrivalTimeSeconds);
		if (!Packet.bParsed)
		{
			Diagnostics.RecordRejectedPacket();
			UE_LOG(
				LogArdyMotion,
				Verbose,
				TEXT("Rejected ARDY packet: %s"),
				*Packet.ParseError);
			continue;
		}

		bool bSenderRestart = false;
		if (!AcceptFrameForDiagnostics(Packet.Frame, bSenderRestart))
		{
			continue;
		}
		if (bSenderRestart)
		{
			LastAppliedFrame = INDEX_NONE;
			MotionStabilizer.Reset();
		}

		if (bUseMotionQuality)
		{
			MotionStabilizer.PushFrame(
				Packet.Frame,
				Packet.ArrivalTimeSeconds);
		}
		else
		{
			NewestFrame = MoveTemp(Packet.Frame);
			bFoundFrame = true;
		}
	}

	if (bUseMotionQuality)
	{
		FArdyMotionFrame EvaluatedFrame;
		if (MotionStabilizer.Evaluate(
			FPlatformTime::Seconds(),
			EstimatedSourceFPS,
			EvaluatedFrame))
		{
			ApplyFrame(EvaluatedFrame);
		}
		return;
	}

	if (!bFoundFrame)
	{
		return;
	}
	ApplyFrame(NewestFrame);
}

bool UArdyMotionReceiverComponent::AcceptFrameForDiagnostics(
	const FArdyMotionFrame& Frame,
	bool& bOutSenderRestart)
{
	bOutSenderRestart = false;
	if (Frame.Subject != SubjectName)
	{
		Diagnostics.RecordRejectedPacket();
		ReportError(FString::Printf(
			TEXT("Ignoring subject %s; receiver expects %s"),
			*Frame.Subject.ToString(),
			*SubjectName.ToString()));
		return false;
	}

	const EArdyFrameDisposition Disposition = Diagnostics.RecordFrame(
		Frame.FrameNumber,
		Frame.TimestampUs,
		RestartMaxStartFrame,
		RestartMinBackwardJump);
	switch (Disposition)
	{
	case EArdyFrameDisposition::Accepted:
		return true;
	case EArdyFrameDisposition::Restarted:
		bOutSenderRestart = true;
		UE_LOG(
			LogArdyMotion,
			Log,
			TEXT("[ARDY DIAG] Sender frame restart accepted at Frame=%lld TimestampUs=%lld"),
			Frame.FrameNumber,
			Frame.TimestampUs);
		return true;
	case EArdyFrameDisposition::Duplicate:
	case EArdyFrameDisposition::OutOfOrder:
		Diagnostics.RecordRejectedPacket();
		return false;
	default:
		return false;
	}
}

void UArdyMotionReceiverComponent::RefreshDiagnosticProperties(const double NowSeconds)
{
	ReceivedPacketCount = Diagnostics.ReceivedPacketCount;
	AcceptedFrameCount = Diagnostics.AcceptedFrameCount;
	RejectedPacketCount = Diagnostics.RejectedPacketCount;
	MissingFrameCount = Diagnostics.MissingFrameCount;
	DuplicateFrameCount = Diagnostics.DuplicateFrameCount;
	OutOfOrderFrameCount = Diagnostics.OutOfOrderFrameCount;
	FrameRestartCount = Diagnostics.FrameRestartCount;
	NonMonotonicTimestampCount = Diagnostics.NonMonotonicTimestampCount;
	LargestFrameGap = Diagnostics.LargestFrameGap;
	LastReceivedFrame = Diagnostics.LastReceivedFrame;
	LastAcceptedFrame = Diagnostics.LastAcceptedFrame;
	LastReceivedTimestampUs = Diagnostics.LastReceivedTimestampUs;
	LastAcceptedTimestampUs = Diagnostics.LastAcceptedTimestampUs;

	LastSourceTimestampDeltaUs = Diagnostics.LastSourceTimestampDeltaUs;
	AverageSourceTimestampDeltaUs = Diagnostics.AverageSourceTimestampDeltaUs;
	MinSourceTimestampDeltaUs = Diagnostics.MinSourceTimestampDeltaUs;
	MaxSourceTimestampDeltaUs = Diagnostics.MaxSourceTimestampDeltaUs;
	EstimatedSourceFPS = Diagnostics.EstimatedSourceFPS;

	LastArrivalDeltaMs = Diagnostics.LastArrivalDeltaMs;
	AverageArrivalDeltaMs = Diagnostics.AverageArrivalDeltaMs;
	MinArrivalDeltaMs = Diagnostics.MinArrivalDeltaMs;
	MaxArrivalDeltaMs = Diagnostics.MaxArrivalDeltaMs;
	ArrivalJitterMs = Diagnostics.ArrivalJitterMs;
	TimeSinceLastPacketMs = Diagnostics.GetTimeSinceLastPacketMs(NowSeconds);
	LastPacketReceivedTimeSeconds = Diagnostics.LastPacketReceivedTimeSeconds;
	DroppedPacketCount = RejectedPacketCount + OutputPublishFailureCount;
	DroppedOrMissingFrameCount = DroppedPacketCount + MissingFrameCount;
	BufferedPoseCount = MotionStabilizer.GetBufferedFrameCount();
	MotionQualityLatencyMs = MotionQualityProfile ==
			EArdyMotionQualityProfile::Raw
		? 0.0
		: MotionStabilizer.GetCurrentBufferLatencyMs(EstimatedSourceFPS);
	RejectedMotionOutlierCount =
		MotionStabilizer.GetRejectedOutlierCount();
	AngularVelocityClampCount =
		MotionStabilizer.GetVelocityClampCount();
}

void UArdyMotionReceiverComponent::UpdateStreamHealth(const double NowSeconds)
{
	EArdyStreamHealthState NewState = EArdyStreamHealthState::NoData;
	if (Diagnostics.HasReceivedPacket())
	{
		const double ElapsedMs = Diagnostics.GetTimeSinceLastPacketMs(NowSeconds);
		if (ElapsedMs >= SevereFreezeThresholdMs)
		{
			NewState = EArdyStreamHealthState::SevereFrozen;
		}
		else if (ElapsedMs >= FreezeThresholdMs)
		{
			NewState = EArdyStreamHealthState::Frozen;
		}
		else if (ElapsedMs >= DelayedThresholdMs)
		{
			NewState = EArdyStreamHealthState::Delayed;
		}
		else
		{
			NewState = EArdyStreamHealthState::Healthy;
		}
	}

	if (NewState == StreamHealthState)
	{
		return;
	}

	const EArdyStreamHealthState PreviousState = StreamHealthState;
	StreamHealthState = NewState;
	const FString StateName =
		StaticEnum<EArdyStreamHealthState>()->GetNameStringByValue(
			static_cast<int64>(NewState));
	if (NewState == EArdyStreamHealthState::Healthy)
	{
		UE_LOG(
			LogArdyMotion,
			Log,
			TEXT("[ARDY DIAG] Stream recovered: %s -> Healthy (%.1fms since packet)"),
			*StaticEnum<EArdyStreamHealthState>()->GetNameStringByValue(
				static_cast<int64>(PreviousState)),
			TimeSinceLastPacketMs);
	}
	else if (NewState != EArdyStreamHealthState::NoData)
	{
		UE_LOG(
			LogArdyMotion,
			Warning,
			TEXT("[ARDY DIAG] Stream state=%s (%.1fms since packet)"),
			*StateName,
			TimeSinceLastPacketMs);
	}
}

void UArdyMotionReceiverComponent::LogDiagnosticSummary(const double NowSeconds)
{
	if (!bEnableDiagnosticLogging ||
		NowSeconds - LastDiagnosticLogTimeSeconds < DiagnosticLogIntervalSeconds)
	{
		return;
	}
	LastDiagnosticLogTimeSeconds = NowSeconds;

	const FString StateName =
		StaticEnum<EArdyStreamHealthState>()->GetNameStringByValue(
			static_cast<int64>(StreamHealthState));
	UE_LOG(
		LogArdyMotion,
		Log,
		TEXT("[ARDY DIAG] RX=%lld Accepted=%lld Applied=%lld Rejected=%lld Missing=%lld ")
		TEXT("Duplicate=%lld OutOfOrder=%lld Restarts=%lld FPS=%.2f SourceDeltaAvg=%.2fms ")
		TEXT("ArrivalAvg=%.2fms ArrivalJitter=%.2fms LargestGap=%lld State=%s ")
		TEXT("Quality=%s Buffer=%d QualityLatency=%.1fms Outliers=%lld Clamps=%lld"),
		ReceivedPacketCount,
		AcceptedFrameCount,
		AppliedFrameCount,
		RejectedPacketCount,
		MissingFrameCount,
		DuplicateFrameCount,
		OutOfOrderFrameCount,
		FrameRestartCount,
		EstimatedSourceFPS,
		AverageSourceTimestampDeltaUs / 1000.0,
		AverageArrivalDeltaMs,
		ArrivalJitterMs,
		LargestFrameGap,
		*StateName,
		*StaticEnum<EArdyMotionQualityProfile>()->GetNameStringByValue(
			static_cast<int64>(MotionQualityProfile)),
		BufferedPoseCount,
		MotionQualityLatencyMs,
		RejectedMotionOutlierCount,
		AngularVelocityClampCount);
}

bool UArdyMotionReceiverComponent::ApplyFrame(const FArdyMotionFrame& Frame)
{
	if (!bPosePipelineReady || !LiveLinkSource.IsValid())
	{
		++OutputPublishFailureCount;
		return false;
	}
	if (Frame.Subject != SubjectName)
	{
		++OutputPublishFailureCount;
		ReportError(FString::Printf(
			TEXT("Ignoring subject %s; receiver expects %s"),
			*Frame.Subject.ToString(),
			*SubjectName.ToString()));
		return false;
	}

	ApplyGestureMetadata(Frame);
	if (TargetBodyComponent)
	{
		if (UArdyTeamRetargetAnimInstance* TargetAnimInstance =
			Cast<UArdyTeamRetargetAnimInstance>(
				TargetBodyComponent->GetAnimInstance()))
		{
			TargetAnimInstance->ConfigureQuality(PoseAugmentation);
		}
	}

	TArray<FTransform> LocalPose = ReferenceLocalPose;
	int32 AppliedJoints = 0;
	for (const FArdyJointRotation& Joint : Frame.Joints)
	{
		if (IsBlockedBone(Joint.BoneName))
		{
			continue;
		}

		const int32* BoneIndex = BoneIndexByName.Find(Joint.BoneName);
		if (!BoneIndex || !LocalPose.IsValidIndex(*BoneIndex))
		{
			continue;
		}

		FTransform& BoneTransform = LocalPose[*BoneIndex];
		BoneTransform.SetRotation(
			(BoneTransform.GetRotation() * Joint.RotationDelta).GetNormalized());
		++AppliedJoints;
	}

	if (LocalPose.IsValidIndex(RootBoneIndex))
	{
		FTransform& RootTransform = LocalPose[RootBoneIndex];
		RootTransform.AddToTranslation(Frame.RootTranslationCm);
		RootTransform.SetRotation(
			(RootTransform.GetRotation() * Frame.RootRotationDelta).GetNormalized());
	}

	if (!LiveLinkSource->PublishFrame(
		SubjectName,
		LocalPose,
		Frame.FrameNumber,
		Frame.TimestampUs))
	{
		++OutputPublishFailureCount;
		return false;
	}

	LastAppliedFrame = Frame.FrameNumber;
	++AppliedFrameCount;
	OnMotionFrame.Broadcast(Frame.FrameNumber, Frame.TimestampUs, AppliedJoints);
	return true;
}

void UArdyMotionReceiverComponent::ApplyGestureMetadata(
	const FArdyMotionFrame& Frame)
{
	bool bLeftExplicit = ParseHandPoseTag(
		Frame.LeftHandPoseTag,
		PoseAugmentation.LeftHandPose);
	bool bRightExplicit = ParseHandPoseTag(
		Frame.RightHandPoseTag,
		PoseAugmentation.RightHandPose);

	if (!Frame.GestureTag.IsNone())
	{
		const FString Gesture = Frame.GestureTag.ToString();
		if (Gesture.Contains(TEXT("point"), ESearchCase::IgnoreCase))
		{
			if (!bLeftExplicit)
			{
				PoseAugmentation.LeftHandPose = EArdyHandPose::Relaxed;
			}
			if (!bRightExplicit)
			{
				PoseAugmentation.RightHandPose = EArdyHandPose::Point;
			}
		}
		else if (Gesture.Contains(TEXT("explain"), ESearchCase::IgnoreCase) ||
			Gesture.Contains(TEXT("emphasis"), ESearchCase::IgnoreCase))
		{
			if (!bLeftExplicit)
			{
				PoseAugmentation.LeftHandPose = EArdyHandPose::Explain;
			}
			if (!bRightExplicit)
			{
				PoseAugmentation.RightHandPose = EArdyHandPose::Explain;
			}
		}
	}

	if (Frame.TransitionDurationMs >= 0.0f)
	{
		PoseAugmentation.HandPoseBlendSeconds = FMath::Clamp(
			Frame.TransitionDurationMs / 1000.0f,
			0.05f,
			1.0f);
	}
}

bool UArdyMotionReceiverComponent::InjectMotionJson(const FString& Payload)
{
	Diagnostics.RecordPacketArrival(FPlatformTime::Seconds());
	FArdyMotionFrame Frame;
	FString Error;
	if (!FArdyMotionProtocol::ParseJson(Payload, Frame, Error))
	{
		Diagnostics.RecordRejectedPacket();
		ReportError(Error);
		return false;
	}

	bool bSenderRestart = false;
	if (!AcceptFrameForDiagnostics(Frame, bSenderRestart))
	{
		return false;
	}
	if (bSenderRestart)
	{
		LastAppliedFrame = INDEX_NONE;
		MotionStabilizer.Reset();
	}
	if (MotionQualityProfile == EArdyMotionQualityProfile::Raw)
	{
		return ApplyFrame(Frame);
	}

	const double NowSeconds = FPlatformTime::Seconds();
	MotionStabilizer.PushFrame(Frame, NowSeconds);
	FArdyMotionFrame EvaluatedFrame;
	return MotionStabilizer.Evaluate(
		NowSeconds,
		EstimatedSourceFPS,
		EvaluatedFrame) &&
		ApplyFrame(EvaluatedFrame);
}

void UArdyMotionReceiverComponent::SetSourcePoseVisible(const bool bVisible)
{
	bShowSourcePose = bVisible;
	if (RuntimeSourcePoseComponent)
	{
		RuntimeSourcePoseComponent->SetHiddenInGame(!bVisible);
		RuntimeSourcePoseComponent->SetVisibility(bVisible, true);
	}
}

void UArdyMotionReceiverComponent::ResetDiagnostics()
{
	Diagnostics.Reset(ArrivalJitterWindowSize);
	ReceivedPacketCount = 0;
	AcceptedFrameCount = 0;
	AppliedFrameCount = 0;
	RejectedPacketCount = 0;
	DroppedPacketCount = 0;
	DroppedOrMissingFrameCount = 0;
	MissingFrameCount = 0;
	DuplicateFrameCount = 0;
	OutOfOrderFrameCount = 0;
	FrameRestartCount = 0;
	NonMonotonicTimestampCount = 0;
	LargestFrameGap = 0;
	LastReceivedFrame = INDEX_NONE;
	LastAcceptedFrame = INDEX_NONE;
	LastAppliedFrame = INDEX_NONE;
	LastReceivedTimestampUs = 0;
	LastAcceptedTimestampUs = 0;
	LastSourceTimestampDeltaUs = 0;
	AverageSourceTimestampDeltaUs = 0.0;
	MinSourceTimestampDeltaUs = 0;
	MaxSourceTimestampDeltaUs = 0;
	EstimatedSourceFPS = 0.0;
	LastArrivalDeltaMs = 0.0;
	AverageArrivalDeltaMs = 0.0;
	MinArrivalDeltaMs = 0.0;
	MaxArrivalDeltaMs = 0.0;
	ArrivalJitterMs = 0.0;
	TimeSinceLastPacketMs = 0.0;
	LastPacketReceivedTimeSeconds = 0.0;
	StreamHealthState = EArdyStreamHealthState::NoData;
	BufferedPoseCount = 0;
	MotionQualityLatencyMs = 0.0;
	RejectedMotionOutlierCount = 0;
	AngularVelocityClampCount = 0;
	MotionStabilizer.Reset();
	ActiveMotionQualityProfile = MotionQualityProfile;
	MotionStabilizer.SetProfile(MotionQualityProfile);
	OutputPublishFailureCount = 0;
	LastDiagnosticLogTimeSeconds = FPlatformTime::Seconds();
}

USkeletalMeshComponent* UArdyMotionReceiverComponent::ResolveTargetBodyComponent() const
{
	if (TargetBodyComponent)
	{
		return TargetBodyComponent;
	}

	auto FindBodyOnActor = [this](AActor* Character) -> USkeletalMeshComponent*
	{
		if (!Character)
		{
			return nullptr;
		}

		TInlineComponentArray<USkeletalMeshComponent*> MeshComponents;
		Character->GetComponents(MeshComponents);
		for (USkeletalMeshComponent* MeshComponent : MeshComponents)
		{
			if (MeshComponent && MeshComponent->GetFName() == TargetBodyComponentName)
			{
				return MeshComponent;
			}
		}
		for (USkeletalMeshComponent* MeshComponent : MeshComponents)
		{
			if (!MeshComponent)
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
	};

	AActor* Character = TargetCharacterActor ? TargetCharacterActor.Get() : GetOwner();
	if (USkeletalMeshComponent* Body = FindBodyOnActor(Character))
	{
		return Body;
	}

	if (bAutoFindTargetCharacter && GetWorld())
	{
		for (TActorIterator<AActor> ActorIt(GetWorld()); ActorIt; ++ActorIt)
		{
			AActor* Candidate = *ActorIt;
			if (!Candidate || Candidate == GetOwner())
			{
				continue;
			}
			if (!TargetCharacterNamePattern.IsEmpty() &&
				!Candidate->GetName().Contains(TargetCharacterNamePattern, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (USkeletalMeshComponent* Body = FindBodyOnActor(Candidate))
			{
				return Body;
			}
		}
	}
	return nullptr;
}

bool UArdyMotionReceiverComponent::IsBlockedBone(const FName BoneName) const
{
	const FString Name = BoneName.ToString();
	if (Name.StartsWith(TEXT("FACIAL_"), ESearchCase::IgnoreCase) ||
		Name.StartsWith(TEXT("face"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	return bBlockHeadAndNeck &&
		(Name.Equals(TEXT("Head"), ESearchCase::IgnoreCase) ||
		 Name.Equals(TEXT("Neck"), ESearchCase::IgnoreCase));
}

void UArdyMotionReceiverComponent::ReportError(const FString& Error)
{
	UE_LOG(LogArdyMotion, Warning, TEXT("%s"), *Error);
	OnMotionError.Broadcast(Error);
}

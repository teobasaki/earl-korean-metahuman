#include "Team/ArdyMotionLiveLinkSource.h"

#include "ILiveLinkClient.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"

void FArdyMotionLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, const FGuid InSourceGuid)
{
	LiveLinkClient = InClient;
	SourceGuid = InSourceGuid;
	bActive = LiveLinkClient != nullptr && SourceGuid.IsValid();
}

bool FArdyMotionLiveLinkSource::IsSourceStillValid() const
{
	return bActive.Load();
}

bool FArdyMotionLiveLinkSource::RequestSourceShutdown()
{
	bActive = false;
	LiveLinkClient = nullptr;
	SourceGuid.Invalidate();
	return true;
}

FText FArdyMotionLiveLinkSource::GetSourceType() const
{
	return FText::FromString(TEXT("ARDY Motion UDP"));
}

FText FArdyMotionLiveLinkSource::GetSourceMachineName() const
{
	return FText::FromString(TEXT("127.0.0.1"));
}

FText FArdyMotionLiveLinkSource::GetSourceStatus() const
{
	return FText::FromString(bActive.Load() ? TEXT("Active") : TEXT("Stopped"));
}

bool FArdyMotionLiveLinkSource::PublishSkeleton(
	const FName SubjectName,
	const TArray<FName>& BoneNames,
	const TArray<int32>& BoneParents)
{
	if (!bActive.Load() || !LiveLinkClient || BoneNames.IsEmpty() || BoneNames.Num() != BoneParents.Num())
	{
		return false;
	}

	FLiveLinkStaticDataStruct StaticDataStruct(FLiveLinkSkeletonStaticData::StaticStruct());
	FLiveLinkSkeletonStaticData& StaticData =
		*StaticDataStruct.Cast<FLiveLinkSkeletonStaticData>();
	StaticData.BoneNames = BoneNames;
	StaticData.BoneParents = BoneParents;

	LiveLinkClient->PushSubjectStaticData_AnyThread(
		{SourceGuid, SubjectName},
		ULiveLinkAnimationRole::StaticClass(),
		MoveTemp(StaticDataStruct));
	return true;
}

bool FArdyMotionLiveLinkSource::PublishFrame(
	const FName SubjectName,
	const TArray<FTransform>& LocalTransforms,
	const int64 FrameNumber,
	const int64 TimestampUs)
{
	if (!bActive.Load() || !LiveLinkClient || LocalTransforms.IsEmpty())
	{
		return false;
	}

	FLiveLinkFrameDataStruct FrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
	FLiveLinkAnimationFrameData& FrameData =
		*FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();
	FrameData.Transforms = LocalTransforms;
	FrameData.WorldTime = FLiveLinkWorldTime(FPlatformTime::Seconds());
	FrameData.FrameId = static_cast<int32>(FrameNumber & MAX_int32);
	FrameData.MetaData.StringMetaData.Add(TEXT("ardy.frame"), LexToString(FrameNumber));
	FrameData.MetaData.StringMetaData.Add(TEXT("ardy.timestamp_us"), LexToString(TimestampUs));

	LiveLinkClient->PushSubjectFrameData_AnyThread(
		{SourceGuid, SubjectName},
		MoveTemp(FrameDataStruct));
	return true;
}

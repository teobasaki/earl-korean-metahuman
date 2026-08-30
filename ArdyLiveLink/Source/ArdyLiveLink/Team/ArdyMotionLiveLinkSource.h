#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"

class ILiveLinkClient;

class FArdyMotionLiveLinkSource final : public ILiveLinkSource
{
public:
	virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;
	virtual bool IsSourceStillValid() const override;
	virtual bool RequestSourceShutdown() override;
	virtual FText GetSourceType() const override;
	virtual FText GetSourceMachineName() const override;
	virtual FText GetSourceStatus() const override;

	bool PublishSkeleton(
		FName SubjectName,
		const TArray<FName>& BoneNames,
		const TArray<int32>& BoneParents);

	bool PublishFrame(
		FName SubjectName,
		const TArray<FTransform>& LocalTransforms,
		int64 FrameNumber,
		int64 TimestampUs);

private:
	ILiveLinkClient* LiveLinkClient = nullptr;
	FGuid SourceGuid;
	TAtomic<bool> bActive = false;
};

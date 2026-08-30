#pragma once

#include "CoreMinimal.h"

struct ARDYLIVELINK_API FArdyJointRotation
{
	FName BoneName;
	FQuat RotationDelta = FQuat::Identity;
};

struct ARDYLIVELINK_API FArdyMotionFrame
{
	FName Subject = TEXT("ARDY_Body");
	int64 FrameNumber = INDEX_NONE;
	int64 TimestampUs = 0;
	FVector RootTranslationCm = FVector::ZeroVector;
	FQuat RootRotationDelta = FQuat::Identity;
	TArray<FArdyJointRotation> Joints;
	FName GestureTag = NAME_None;
	FName LeftHandPoseTag = NAME_None;
	FName RightHandPoseTag = NAME_None;
	float TransitionDurationMs = -1.0f;
};

/**
 * Parser for the wire-level ARDY motion contract.
 *
 * ardy.motion.v1 carries Unreal-coordinate, local-space rotation deltas. It is
 * deliberately independent from UObject state so UDP packets can be validated
 * on the receiver thread before the newest frame is consumed on the game thread.
 */
class ARDYLIVELINK_API FArdyMotionProtocol
{
public:
	static constexpr int32 MaxPacketBytes = 64 * 1024;
	static constexpr int32 MaxJointsPerFrame = 256;

	static bool ParseJson(
		const FString& Payload,
		FArdyMotionFrame& OutFrame,
		FString& OutError);
};

#pragma once

#include "CoreMinimal.h"

enum class EArdyFrameDisposition : uint8
{
	Accepted,
	Duplicate,
	OutOfOrder,
	Restarted
};

/**
 * Allocation-free (after Reset) input-stream measurements for ARDY motion.
 *
 * This class observes packet/frame timing only. It never changes, buffers, or
 * filters a pose, so it can remain in front of a future Motion Stabilizer.
 */
class ARDYLIVELINK_API FArdyMotionDiagnostics
{
public:
	void Reset(int32 InArrivalJitterWindowSize);

	void RecordPacketArrival(double ArrivalTimeSeconds);
	void RecordRejectedPacket();

	EArdyFrameDisposition RecordFrame(
		int64 FrameNumber,
		int64 TimestampUs,
		int64 RestartMaxStartFrame,
		int64 RestartMinBackwardJump);

	double GetTimeSinceLastPacketMs(double NowSeconds) const;

	int64 ReceivedPacketCount = 0;
	int64 RejectedPacketCount = 0;
	int64 AcceptedFrameCount = 0;
	int64 MissingFrameCount = 0;
	int64 DuplicateFrameCount = 0;
	int64 OutOfOrderFrameCount = 0;
	int64 FrameRestartCount = 0;
	int64 NonMonotonicTimestampCount = 0;
	int64 LargestFrameGap = 0;

	int64 LastReceivedFrame = INDEX_NONE;
	int64 LastAcceptedFrame = INDEX_NONE;
	int64 LastReceivedTimestampUs = 0;
	int64 LastAcceptedTimestampUs = 0;

	int64 LastSourceTimestampDeltaUs = 0;
	double AverageSourceTimestampDeltaUs = 0.0;
	int64 MinSourceTimestampDeltaUs = 0;
	int64 MaxSourceTimestampDeltaUs = 0;
	double EstimatedSourceFPS = 0.0;

	double LastPacketReceivedTimeSeconds = 0.0;
	double LastArrivalDeltaMs = 0.0;
	double AverageArrivalDeltaMs = 0.0;
	double MinArrivalDeltaMs = 0.0;
	double MaxArrivalDeltaMs = 0.0;
	double ArrivalJitterMs = 0.0;

	bool HasReceivedPacket() const
	{
		return bHasReceivedPacket;
	}

private:
	void RecordSourceDelta(int64 FrameAdvance, int64 TimestampDeltaUs);
	void RecordArrivalDelta(double ArrivalDeltaMs);

	TArray<double> ArrivalDeltaWindow;
	int32 ArrivalDeltaWindowWriteIndex = 0;
	int32 ArrivalDeltaWindowCount = 0;
	double ArrivalDeltaWindowSum = 0.0;
	double ArrivalDeltaWindowSumSquares = 0.0;

	int64 SourceDeltaSampleCount = 0;
	long double SourceDeltaSumUs = 0.0;
	int64 SourceFrameAdvanceSum = 0;
	int64 ArrivalDeltaSampleCount = 0;
	long double ArrivalDeltaSumMs = 0.0;

	bool bHasReceivedPacket = false;
	bool bHasAcceptedFrame = false;
};

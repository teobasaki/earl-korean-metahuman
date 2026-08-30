#include "Team/ArdyMotionDiagnostics.h"

void FArdyMotionDiagnostics::Reset(const int32 InArrivalJitterWindowSize)
{
	ReceivedPacketCount = 0;
	RejectedPacketCount = 0;
	AcceptedFrameCount = 0;
	MissingFrameCount = 0;
	DuplicateFrameCount = 0;
	OutOfOrderFrameCount = 0;
	FrameRestartCount = 0;
	NonMonotonicTimestampCount = 0;
	LargestFrameGap = 0;

	LastReceivedFrame = INDEX_NONE;
	LastAcceptedFrame = INDEX_NONE;
	LastReceivedTimestampUs = 0;
	LastAcceptedTimestampUs = 0;

	LastSourceTimestampDeltaUs = 0;
	AverageSourceTimestampDeltaUs = 0.0;
	MinSourceTimestampDeltaUs = 0;
	MaxSourceTimestampDeltaUs = 0;
	EstimatedSourceFPS = 0.0;

	LastPacketReceivedTimeSeconds = 0.0;
	LastArrivalDeltaMs = 0.0;
	AverageArrivalDeltaMs = 0.0;
	MinArrivalDeltaMs = 0.0;
	MaxArrivalDeltaMs = 0.0;
	ArrivalJitterMs = 0.0;

	const int32 WindowSize = FMath::Max(2, InArrivalJitterWindowSize);
	ArrivalDeltaWindow.SetNumZeroed(WindowSize);
	ArrivalDeltaWindowWriteIndex = 0;
	ArrivalDeltaWindowCount = 0;
	ArrivalDeltaWindowSum = 0.0;
	ArrivalDeltaWindowSumSquares = 0.0;

	SourceDeltaSampleCount = 0;
	SourceDeltaSumUs = 0.0;
	SourceFrameAdvanceSum = 0;
	ArrivalDeltaSampleCount = 0;
	ArrivalDeltaSumMs = 0.0;
	bHasReceivedPacket = false;
	bHasAcceptedFrame = false;
}

void FArdyMotionDiagnostics::RecordPacketArrival(const double ArrivalTimeSeconds)
{
	++ReceivedPacketCount;
	if (bHasReceivedPacket)
	{
		RecordArrivalDelta(
			FMath::Max(0.0, (ArrivalTimeSeconds - LastPacketReceivedTimeSeconds) * 1000.0));
	}

	LastPacketReceivedTimeSeconds = ArrivalTimeSeconds;
	bHasReceivedPacket = true;
}

void FArdyMotionDiagnostics::RecordRejectedPacket()
{
	++RejectedPacketCount;
}

EArdyFrameDisposition FArdyMotionDiagnostics::RecordFrame(
	const int64 FrameNumber,
	const int64 TimestampUs,
	const int64 RestartMaxStartFrame,
	const int64 RestartMinBackwardJump)
{
	LastReceivedFrame = FrameNumber;
	LastReceivedTimestampUs = TimestampUs;

	if (!bHasAcceptedFrame)
	{
		bHasAcceptedFrame = true;
		LastAcceptedFrame = FrameNumber;
		LastAcceptedTimestampUs = TimestampUs;
		++AcceptedFrameCount;
		return EArdyFrameDisposition::Accepted;
	}

	if (FrameNumber == LastAcceptedFrame)
	{
		++DuplicateFrameCount;
		return EArdyFrameDisposition::Duplicate;
	}

	if (FrameNumber < LastAcceptedFrame)
	{
		const int64 BackwardJump = LastAcceptedFrame - FrameNumber;
		const bool bLooksLikeSenderRestart =
			FrameNumber <= RestartMaxStartFrame &&
			BackwardJump >= RestartMinBackwardJump &&
			TimestampUs > LastAcceptedTimestampUs;
		if (!bLooksLikeSenderRestart)
		{
			++OutOfOrderFrameCount;
			return EArdyFrameDisposition::OutOfOrder;
		}

		++FrameRestartCount;
		++AcceptedFrameCount;
		LastAcceptedFrame = FrameNumber;
		LastAcceptedTimestampUs = TimestampUs;
		return EArdyFrameDisposition::Restarted;
	}

	const int64 FrameAdvance = FrameNumber - LastAcceptedFrame;
	const int64 MissingFrames = FMath::Max<int64>(0, FrameAdvance - 1);
	MissingFrameCount += MissingFrames;
	LargestFrameGap = FMath::Max(LargestFrameGap, MissingFrames);

	const int64 TimestampDeltaUs = TimestampUs - LastAcceptedTimestampUs;
	if (TimestampDeltaUs > 0)
	{
		RecordSourceDelta(FrameAdvance, TimestampDeltaUs);
	}
	else
	{
		++NonMonotonicTimestampCount;
	}

	++AcceptedFrameCount;
	LastAcceptedFrame = FrameNumber;
	LastAcceptedTimestampUs = TimestampUs;
	return EArdyFrameDisposition::Accepted;
}

double FArdyMotionDiagnostics::GetTimeSinceLastPacketMs(const double NowSeconds) const
{
	if (!bHasReceivedPacket)
	{
		return 0.0;
	}
	return FMath::Max(0.0, (NowSeconds - LastPacketReceivedTimeSeconds) * 1000.0);
}

void FArdyMotionDiagnostics::RecordSourceDelta(
	const int64 FrameAdvance,
	const int64 TimestampDeltaUs)
{
	LastSourceTimestampDeltaUs = TimestampDeltaUs;
	if (SourceDeltaSampleCount == 0)
	{
		MinSourceTimestampDeltaUs = TimestampDeltaUs;
		MaxSourceTimestampDeltaUs = TimestampDeltaUs;
	}
	else
	{
		MinSourceTimestampDeltaUs = FMath::Min(MinSourceTimestampDeltaUs, TimestampDeltaUs);
		MaxSourceTimestampDeltaUs = FMath::Max(MaxSourceTimestampDeltaUs, TimestampDeltaUs);
	}

	++SourceDeltaSampleCount;
	SourceDeltaSumUs += static_cast<long double>(TimestampDeltaUs);
	SourceFrameAdvanceSum += FrameAdvance;
	AverageSourceTimestampDeltaUs =
		static_cast<double>(SourceDeltaSumUs / SourceDeltaSampleCount);
	EstimatedSourceFPS =
		static_cast<double>(SourceFrameAdvanceSum) * 1000000.0 /
		static_cast<double>(SourceDeltaSumUs);
}

void FArdyMotionDiagnostics::RecordArrivalDelta(const double InArrivalDeltaMs)
{
	LastArrivalDeltaMs = InArrivalDeltaMs;
	if (ArrivalDeltaSampleCount == 0)
	{
		MinArrivalDeltaMs = InArrivalDeltaMs;
		MaxArrivalDeltaMs = InArrivalDeltaMs;
	}
	else
	{
		MinArrivalDeltaMs = FMath::Min(MinArrivalDeltaMs, InArrivalDeltaMs);
		MaxArrivalDeltaMs = FMath::Max(MaxArrivalDeltaMs, InArrivalDeltaMs);
	}

	++ArrivalDeltaSampleCount;
	ArrivalDeltaSumMs += static_cast<long double>(InArrivalDeltaMs);
	AverageArrivalDeltaMs =
		static_cast<double>(ArrivalDeltaSumMs / ArrivalDeltaSampleCount);

	if (ArrivalDeltaWindowCount == ArrivalDeltaWindow.Num())
	{
		const double OldValue = ArrivalDeltaWindow[ArrivalDeltaWindowWriteIndex];
		ArrivalDeltaWindowSum -= OldValue;
		ArrivalDeltaWindowSumSquares -= OldValue * OldValue;
	}
	else
	{
		++ArrivalDeltaWindowCount;
	}

	ArrivalDeltaWindow[ArrivalDeltaWindowWriteIndex] = InArrivalDeltaMs;
	ArrivalDeltaWindowWriteIndex =
		(ArrivalDeltaWindowWriteIndex + 1) % ArrivalDeltaWindow.Num();
	ArrivalDeltaWindowSum += InArrivalDeltaMs;
	ArrivalDeltaWindowSumSquares += InArrivalDeltaMs * InArrivalDeltaMs;

	const double Mean = ArrivalDeltaWindowSum / ArrivalDeltaWindowCount;
	const double Variance = FMath::Max(
		0.0,
		ArrivalDeltaWindowSumSquares / ArrivalDeltaWindowCount - Mean * Mean);
	ArrivalJitterMs = FMath::Sqrt(Variance);
}

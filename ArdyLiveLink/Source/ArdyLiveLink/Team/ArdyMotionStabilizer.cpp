#include "Team/ArdyMotionStabilizer.h"

namespace
{
	double QuaternionAngleDegrees(const FQuat& A, const FQuat& B)
	{
		return FMath::RadiansToDegrees(A.AngularDistance(B));
	}

	const FArdyJointRotation* FindJoint(
		const FArdyMotionFrame& Frame,
		const FName BoneName)
	{
		return Frame.Joints.FindByPredicate(
			[BoneName](const FArdyJointRotation& Joint)
			{
				return Joint.BoneName == BoneName;
			});
	}

	bool ContainsAny(const FString& Value, const TArrayView<const TCHAR* const> Tokens)
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Value.Contains(Token, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}

void FArdyMotionStabilizer::Reset()
{
	BufferedFrames.Reset();
	LastFilteredRotations.Reset();
	LastFilteredRootTranslation = FVector::ZeroVector;
	LastFilteredRootRotation = FQuat::Identity;
	LastOutputTimestampUs = 0;
	ClockOriginTimestampUs = 0;
	ClockOriginArrivalSeconds = 0.0;
	RejectedOutlierCount = 0;
	VelocityClampCount = 0;
	bHasFilteredRoot = false;
}

void FArdyMotionStabilizer::SetProfile(const EArdyMotionQualityProfile InProfile)
{
	if (Profile == InProfile)
	{
		return;
	}
	Profile = InProfile;
	Settings = SettingsForProfile(InProfile);
	Reset();
}

void FArdyMotionStabilizer::SetSettings(
	const FArdyMotionQualitySettings& InSettings)
{
	Settings = InSettings;
}

FArdyMotionQualitySettings FArdyMotionStabilizer::SettingsForProfile(
	const EArdyMotionQualityProfile InProfile)
{
	FArdyMotionQualitySettings Result;
	switch (InProfile)
	{
	case EArdyMotionQualityProfile::Responsive:
		Result.RenderDelayFrames = 2;
		Result.DeadZoneDegrees = 0.05f;
		Result.OutlierDegrees = 110.0f;
		Result.SpineTimeConstantSeconds = 0.055f;
		Result.ClavicleTimeConstantSeconds = 0.04f;
		Result.UpperArmTimeConstantSeconds = 0.025f;
		Result.ForeArmTimeConstantSeconds = 0.015f;
		Result.HandTimeConstantSeconds = 0.012f;
		Result.LegTimeConstantSeconds = 0.04f;
		Result.SpineMaxDegreesPerSecond = 240.0f;
		Result.ClavicleMaxDegreesPerSecond = 360.0f;
		Result.UpperArmMaxDegreesPerSecond = 720.0f;
		Result.ForeArmMaxDegreesPerSecond = 900.0f;
		Result.HandMaxDegreesPerSecond = 1080.0f;
		Result.LegMaxDegreesPerSecond = 540.0f;
		break;
	case EArdyMotionQualityProfile::Smooth:
		Result.RenderDelayFrames = 3;
		Result.DeadZoneDegrees = 0.2f;
		Result.OutlierDegrees = 80.0f;
		Result.SpineTimeConstantSeconds = 0.14f;
		Result.ClavicleTimeConstantSeconds = 0.11f;
		Result.UpperArmTimeConstantSeconds = 0.08f;
		Result.ForeArmTimeConstantSeconds = 0.055f;
		Result.HandTimeConstantSeconds = 0.045f;
		Result.LegTimeConstantSeconds = 0.11f;
		Result.SpineMaxDegreesPerSecond = 140.0f;
		Result.ClavicleMaxDegreesPerSecond = 220.0f;
		Result.UpperArmMaxDegreesPerSecond = 420.0f;
		Result.ForeArmMaxDegreesPerSecond = 540.0f;
		Result.HandMaxDegreesPerSecond = 720.0f;
		Result.LegMaxDegreesPerSecond = 320.0f;
		break;
	case EArdyMotionQualityProfile::Balanced:
	case EArdyMotionQualityProfile::Raw:
	default:
		break;
	}
	return Result;
}

void FArdyMotionStabilizer::PushFrame(
	const FArdyMotionFrame& Frame,
	const double ArrivalTimeSeconds)
{
	if (BufferedFrames.IsEmpty())
	{
		ClockOriginTimestampUs = Frame.TimestampUs;
		ClockOriginArrivalSeconds = ArrivalTimeSeconds;
	}
	else if (Frame.TimestampUs <= BufferedFrames.Last().Frame.TimestampUs)
	{
		return;
	}

	FBufferedFrame& Entry = BufferedFrames.AddDefaulted_GetRef();
	Entry.Frame = Frame;
	Entry.ArrivalTimeSeconds = ArrivalTimeSeconds;
	SanitizeNewestTriplet();

	// A short bounded queue protects long sessions from unbounded growth.
	if (BufferedFrames.Num() > 12)
	{
		BufferedFrames.RemoveAt(0, BufferedFrames.Num() - 12, EAllowShrinking::No);
	}
}

void FArdyMotionStabilizer::SanitizeNewestTriplet()
{
	if (BufferedFrames.Num() < 3 || Settings.OutlierDegrees <= 0.0f)
	{
		return;
	}

	FArdyMotionFrame& Middle =
		BufferedFrames[BufferedFrames.Num() - 2].Frame;
	const FArdyMotionFrame& Previous =
		BufferedFrames[BufferedFrames.Num() - 3].Frame;
	const FArdyMotionFrame& Next =
		BufferedFrames[BufferedFrames.Num() - 1].Frame;

	for (FArdyJointRotation& Joint : Middle.Joints)
	{
		const FArdyJointRotation* PreviousJoint =
			FindJoint(Previous, Joint.BoneName);
		const FArdyJointRotation* NextJoint =
			FindJoint(Next, Joint.BoneName);
		if (!PreviousJoint || !NextJoint)
		{
			continue;
		}

		const double PreviousToMiddle = QuaternionAngleDegrees(
			PreviousJoint->RotationDelta,
			Joint.RotationDelta);
		const double MiddleToNext = QuaternionAngleDegrees(
			Joint.RotationDelta,
			NextJoint->RotationDelta);
		const double PreviousToNext = QuaternionAngleDegrees(
			PreviousJoint->RotationDelta,
			NextJoint->RotationDelta);
		if (PreviousToMiddle >= Settings.OutlierDegrees &&
			MiddleToNext >= Settings.OutlierDegrees &&
			PreviousToNext <= Settings.OutlierDegrees * 0.5)
		{
			Joint.RotationDelta = FQuat::Slerp(
				PreviousJoint->RotationDelta,
				NextJoint->RotationDelta,
				0.5).GetNormalized();
			++RejectedOutlierCount;
		}
	}
}

bool FArdyMotionStabilizer::Evaluate(
	const double NowSeconds,
	const double EstimatedSourceFPS,
	FArdyMotionFrame& OutFrame)
{
	if (Profile == EArdyMotionQualityProfile::Raw || BufferedFrames.IsEmpty())
	{
		return false;
	}

	const double SafeFPS =
		EstimatedSourceFPS > 1.0 ? EstimatedSourceFPS : 20.0;
	const int64 DelayUs = FMath::RoundToInt64(
		static_cast<double>(Settings.RenderDelayFrames) * 1000000.0 / SafeFPS);
	const int64 ElapsedUs = FMath::Max<int64>(
		0,
		FMath::RoundToInt64(
			(NowSeconds - ClockOriginArrivalSeconds) * 1000000.0));
	const int64 TargetTimestampUs =
		ClockOriginTimestampUs + ElapsedUs - DelayUs;

	if (!BuildInterpolatedFrame(TargetTimestampUs, OutFrame))
	{
		return false;
	}
	FilterFrame(OutFrame);
	return true;
}

bool FArdyMotionStabilizer::BuildInterpolatedFrame(
	const int64 TargetTimestampUs,
	FArdyMotionFrame& OutFrame)
{
	if (BufferedFrames.IsEmpty())
	{
		return false;
	}

	if (TargetTimestampUs <= BufferedFrames[0].Frame.TimestampUs)
	{
		OutFrame = BufferedFrames[0].Frame;
		return true;
	}

	int32 UpperIndex = INDEX_NONE;
	for (int32 Index = 1; Index < BufferedFrames.Num(); ++Index)
	{
		if (BufferedFrames[Index].Frame.TimestampUs >= TargetTimestampUs)
		{
			UpperIndex = Index;
			break;
		}
	}

	if (UpperIndex == INDEX_NONE)
	{
		OutFrame = BufferedFrames.Last().Frame;
		return true;
	}

	const FArdyMotionFrame& A = BufferedFrames[UpperIndex - 1].Frame;
	const FArdyMotionFrame& B = BufferedFrames[UpperIndex].Frame;
	const int64 SpanUs = B.TimestampUs - A.TimestampUs;
	const double Alpha = SpanUs > 0
		? FMath::Clamp(
			static_cast<double>(TargetTimestampUs - A.TimestampUs) /
				static_cast<double>(SpanUs),
			0.0,
			1.0)
		: 1.0;

	OutFrame = A;
	OutFrame.FrameNumber = Alpha < 0.5 ? A.FrameNumber : B.FrameNumber;
	OutFrame.TimestampUs = TargetTimestampUs;
	OutFrame.RootTranslationCm = FMath::Lerp(
		A.RootTranslationCm,
		B.RootTranslationCm,
		Alpha);
	OutFrame.RootRotationDelta = FQuat::Slerp(
		A.RootRotationDelta,
		B.RootRotationDelta,
		Alpha).GetNormalized();

	for (FArdyJointRotation& Joint : OutFrame.Joints)
	{
		if (const FArdyJointRotation* BJoint = FindJoint(B, Joint.BoneName))
		{
			Joint.RotationDelta = FQuat::Slerp(
				Joint.RotationDelta,
				BJoint->RotationDelta,
				Alpha).GetNormalized();
		}
	}

	if (UpperIndex > 2)
	{
		BufferedFrames.RemoveAt(0, UpperIndex - 2, EAllowShrinking::No);
	}
	return true;
}

void FArdyMotionStabilizer::FilterFrame(FArdyMotionFrame& InOutFrame)
{
	const double DeltaSeconds = LastOutputTimestampUs > 0
		? FMath::Clamp(
			static_cast<double>(
				InOutFrame.TimestampUs - LastOutputTimestampUs) / 1000000.0,
			1.0 / 240.0,
			0.1)
		: 1.0 / 60.0;

	for (FArdyJointRotation& Joint : InOutFrame.Joints)
	{
		FQuat* Previous = LastFilteredRotations.Find(Joint.BoneName);
		if (!Previous)
		{
			LastFilteredRotations.Add(Joint.BoneName, Joint.RotationDelta);
			continue;
		}

		double DeltaDegrees = QuaternionAngleDegrees(
			*Previous,
			Joint.RotationDelta);
		if (DeltaDegrees <= Settings.DeadZoneDegrees)
		{
			Joint.RotationDelta = *Previous;
			continue;
		}

		const double MaxStepDegrees =
			static_cast<double>(MaxVelocityForBone(Joint.BoneName)) *
			DeltaSeconds;
		if (MaxStepDegrees > 0.0 && DeltaDegrees > MaxStepDegrees)
		{
			Joint.RotationDelta = FQuat::Slerp(
				*Previous,
				Joint.RotationDelta,
				MaxStepDegrees / DeltaDegrees).GetNormalized();
			DeltaDegrees = MaxStepDegrees;
			++VelocityClampCount;
		}

		const double TimeConstant =
			static_cast<double>(TimeConstantForBone(Joint.BoneName));
		const double Alpha = TimeConstant > UE_SMALL_NUMBER
			? 1.0 - FMath::Exp(-DeltaSeconds / TimeConstant)
			: 1.0;
		Joint.RotationDelta = FQuat::Slerp(
			*Previous,
			Joint.RotationDelta,
			Alpha).GetNormalized();
		*Previous = Joint.RotationDelta;
	}

	if (!bHasFilteredRoot)
	{
		LastFilteredRootTranslation = InOutFrame.RootTranslationCm;
		LastFilteredRootRotation = InOutFrame.RootRotationDelta;
		bHasFilteredRoot = true;
	}
	else
	{
		const double RootTimeConstant =
			static_cast<double>(Settings.SpineTimeConstantSeconds);
		const double RootAlpha = RootTimeConstant > UE_SMALL_NUMBER
			? 1.0 - FMath::Exp(-DeltaSeconds / RootTimeConstant)
			: 1.0;
		InOutFrame.RootTranslationCm = FMath::Lerp(
			LastFilteredRootTranslation,
			InOutFrame.RootTranslationCm,
			RootAlpha);
		InOutFrame.RootRotationDelta = FQuat::Slerp(
			LastFilteredRootRotation,
			InOutFrame.RootRotationDelta,
			RootAlpha).GetNormalized();
		LastFilteredRootTranslation = InOutFrame.RootTranslationCm;
		LastFilteredRootRotation = InOutFrame.RootRotationDelta;
	}

	LastOutputTimestampUs = InOutFrame.TimestampUs;
}

float FArdyMotionStabilizer::TimeConstantForBone(const FName BoneName) const
{
	const FString Name = BoneName.ToString();
	if (ContainsAny(Name, MakeArrayView<const TCHAR*>({
		TEXT("Spine"), TEXT("Hips")})))
	{
		return Settings.SpineTimeConstantSeconds;
	}
	if (Name.Contains(TEXT("Shoulder"), ESearchCase::IgnoreCase))
	{
		return Settings.ClavicleTimeConstantSeconds;
	}
	if (Name.Contains(TEXT("ForeArm"), ESearchCase::IgnoreCase))
	{
		return Settings.ForeArmTimeConstantSeconds;
	}
	if (ContainsAny(Name, MakeArrayView<const TCHAR*>({
		TEXT("Hand"), TEXT("Wrist"), TEXT("Thumb")})))
	{
		return Settings.HandTimeConstantSeconds;
	}
	if (Name.Contains(TEXT("Arm"), ESearchCase::IgnoreCase))
	{
		return Settings.UpperArmTimeConstantSeconds;
	}
	return Settings.LegTimeConstantSeconds;
}

float FArdyMotionStabilizer::MaxVelocityForBone(const FName BoneName) const
{
	const FString Name = BoneName.ToString();
	if (ContainsAny(Name, MakeArrayView<const TCHAR*>({
		TEXT("Spine"), TEXT("Hips")})))
	{
		return Settings.SpineMaxDegreesPerSecond;
	}
	if (Name.Contains(TEXT("Shoulder"), ESearchCase::IgnoreCase))
	{
		return Settings.ClavicleMaxDegreesPerSecond;
	}
	if (Name.Contains(TEXT("ForeArm"), ESearchCase::IgnoreCase))
	{
		return Settings.ForeArmMaxDegreesPerSecond;
	}
	if (ContainsAny(Name, MakeArrayView<const TCHAR*>({
		TEXT("Hand"), TEXT("Wrist"), TEXT("Thumb")})))
	{
		return Settings.HandMaxDegreesPerSecond;
	}
	if (Name.Contains(TEXT("Arm"), ESearchCase::IgnoreCase))
	{
		return Settings.UpperArmMaxDegreesPerSecond;
	}
	return Settings.LegMaxDegreesPerSecond;
}

double FArdyMotionStabilizer::GetCurrentBufferLatencyMs(
	const double EstimatedSourceFPS) const
{
	const double SafeFPS =
		EstimatedSourceFPS > 1.0 ? EstimatedSourceFPS : 20.0;
	return static_cast<double>(Settings.RenderDelayFrames) * 1000.0 / SafeFPS;
}

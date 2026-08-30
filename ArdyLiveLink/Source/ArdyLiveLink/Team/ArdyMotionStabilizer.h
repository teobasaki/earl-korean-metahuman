#pragma once

#include "CoreMinimal.h"
#include "Team/ArdyMotionProtocol.h"
#include "ArdyMotionStabilizer.generated.h"

UENUM(BlueprintType)
enum class EArdyMotionQualityProfile : uint8
{
	Raw UMETA(DisplayName="RAW"),
	Responsive,
	Balanced,
	Smooth
};

USTRUCT(BlueprintType)
struct ARDYLIVELINK_API FArdyMotionQualitySettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	int32 RenderDelayFrames = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float DeadZoneDegrees = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float OutlierDegrees = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float SpineTimeConstantSeconds = 0.09f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float ClavicleTimeConstantSeconds = 0.07f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float UpperArmTimeConstantSeconds = 0.045f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float ForeArmTimeConstantSeconds = 0.03f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float HandTimeConstantSeconds = 0.025f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float LegTimeConstantSeconds = 0.07f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float SpineMaxDegreesPerSecond = 180.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float ClavicleMaxDegreesPerSecond = 280.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float UpperArmMaxDegreesPerSecond = 540.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float ForeArmMaxDegreesPerSecond = 720.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float HandMaxDegreesPerSecond = 900.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="ARDY|Quality")
	float LegMaxDegreesPerSecond = 420.0f;
};

/**
 * Game-thread timestamp pose buffer for ARDY motion.
 *
 * The wire protocol and Live Link subject remain unchanged. Accepted source
 * frames are sampled on a stable source-time clock, then filtered per joint.
 */
class ARDYLIVELINK_API FArdyMotionStabilizer
{
public:
	void Reset();
	void SetProfile(EArdyMotionQualityProfile InProfile);
	void SetSettings(const FArdyMotionQualitySettings& InSettings);
	void PushFrame(const FArdyMotionFrame& Frame, double ArrivalTimeSeconds);
	bool Evaluate(
		double NowSeconds,
		double EstimatedSourceFPS,
		FArdyMotionFrame& OutFrame);

	int64 GetRejectedOutlierCount() const { return RejectedOutlierCount; }
	int64 GetVelocityClampCount() const { return VelocityClampCount; }
	double GetCurrentBufferLatencyMs(double EstimatedSourceFPS) const;
	int32 GetBufferedFrameCount() const { return BufferedFrames.Num(); }

	static FArdyMotionQualitySettings SettingsForProfile(
		EArdyMotionQualityProfile Profile);

private:
	struct FBufferedFrame
	{
		FArdyMotionFrame Frame;
		double ArrivalTimeSeconds = 0.0;
	};

	void SanitizeNewestTriplet();
	bool BuildInterpolatedFrame(int64 TargetTimestampUs, FArdyMotionFrame& OutFrame);
	void FilterFrame(FArdyMotionFrame& InOutFrame);
	float TimeConstantForBone(FName BoneName) const;
	float MaxVelocityForBone(FName BoneName) const;

	EArdyMotionQualityProfile Profile = EArdyMotionQualityProfile::Raw;
	FArdyMotionQualitySettings Settings;
	TArray<FBufferedFrame> BufferedFrames;
	TMap<FName, FQuat> LastFilteredRotations;
	FVector LastFilteredRootTranslation = FVector::ZeroVector;
	FQuat LastFilteredRootRotation = FQuat::Identity;
	int64 LastOutputTimestampUs = 0;
	int64 ClockOriginTimestampUs = 0;
	double ClockOriginArrivalSeconds = 0.0;
	int64 RejectedOutlierCount = 0;
	int64 VelocityClampCount = 0;
	bool bHasFilteredRoot = false;
};

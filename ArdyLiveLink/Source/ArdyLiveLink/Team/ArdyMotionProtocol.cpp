#include "Team/ArdyMotionProtocol.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	bool IsFinite(const double Value)
	{
		return FMath::IsFinite(Value);
	}

	bool ReadVector3(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName,
		FVector& OutValue,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() != 3)
		{
			OutError = FString::Printf(TEXT("%s must contain exactly 3 numbers"), FieldName);
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!(*Values)[0].IsValid() || !(*Values)[0]->TryGetNumber(X) ||
			!(*Values)[1].IsValid() || !(*Values)[1]->TryGetNumber(Y) ||
			!(*Values)[2].IsValid() || !(*Values)[2]->TryGetNumber(Z) ||
			!IsFinite(X) || !IsFinite(Y) || !IsFinite(Z))
		{
			OutError = FString::Printf(TEXT("%s contains a non-finite value"), FieldName);
			return false;
		}

		OutValue = FVector(X, Y, Z);
		return true;
	}

	bool ReadNormalizedQuat(
		const TArray<TSharedPtr<FJsonValue>>* Values,
		const FString& FieldDescription,
		FQuat& OutValue,
		FString& OutError)
	{
		if (!Values || Values->Num() != 4)
		{
			OutError = FString::Printf(TEXT("%s must contain exactly 4 numbers in xyzw order"), *FieldDescription);
			return false;
		}

		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		double W = 0.0;
		if (!(*Values)[0].IsValid() || !(*Values)[0]->TryGetNumber(X) ||
			!(*Values)[1].IsValid() || !(*Values)[1]->TryGetNumber(Y) ||
			!(*Values)[2].IsValid() || !(*Values)[2]->TryGetNumber(Z) ||
			!(*Values)[3].IsValid() || !(*Values)[3]->TryGetNumber(W) ||
			!IsFinite(X) || !IsFinite(Y) || !IsFinite(Z) || !IsFinite(W))
		{
			OutError = FString::Printf(TEXT("%s contains a non-finite value"), *FieldDescription);
			return false;
		}

		FQuat Rotation(X, Y, Z, W);
		if (Rotation.SizeSquared() <= UE_SMALL_NUMBER)
		{
			OutError = FString::Printf(TEXT("%s is a zero quaternion"), *FieldDescription);
			return false;
		}

		Rotation.Normalize();
		OutValue = Rotation;
		return true;
	}

	bool ReadQuatField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* FieldName,
		FQuat& OutValue,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values))
		{
			OutError = FString::Printf(TEXT("%s is required"), FieldName);
			return false;
		}
		return ReadNormalizedQuat(Values, FieldName, OutValue, OutError);
	}
}

bool FArdyMotionProtocol::ParseJson(
	const FString& Payload,
	FArdyMotionFrame& OutFrame,
	FString& OutError)
{
	OutFrame = FArdyMotionFrame();
	OutError.Reset();

	if (Payload.IsEmpty())
	{
		OutError = TEXT("packet is empty");
		return false;
	}
	if (FTCHARToUTF8(*Payload).Length() > MaxPacketBytes)
	{
		OutError = TEXT("packet exceeds 64 KiB");
		return false;
	}

	TSharedPtr<FJsonObject> JsonObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		OutError = TEXT("packet is not a JSON object");
		return false;
	}

	FString Schema;
	FString Type;
	FString CoordinateSystem;
	FString Space;
	FString Subject;
	if (!JsonObject->TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("ardy.motion.v1"))
	{
		OutError = TEXT("schema must be ardy.motion.v1");
		return false;
	}
	if (!JsonObject->TryGetStringField(TEXT("type"), Type) || Type != TEXT("pose"))
	{
		OutError = TEXT("type must be pose");
		return false;
	}
	if (!JsonObject->TryGetStringField(TEXT("coordinate_system"), CoordinateSystem) ||
		CoordinateSystem != TEXT("unreal_lh_zup_cm"))
	{
		OutError = TEXT("coordinate_system must be unreal_lh_zup_cm");
		return false;
	}
	if (!JsonObject->TryGetStringField(TEXT("space"), Space) || Space != TEXT("local_delta"))
	{
		OutError = TEXT("space must be local_delta");
		return false;
	}
	if (!JsonObject->TryGetStringField(TEXT("subject"), Subject) || Subject.IsEmpty())
	{
		OutError = TEXT("subject is required");
		return false;
	}

	int64 FrameNumber = INDEX_NONE;
	int64 TimestampUs = 0;
	if (!JsonObject->TryGetNumberField(TEXT("frame"), FrameNumber) || FrameNumber < 0)
	{
		OutError = TEXT("frame must be a non-negative integer");
		return false;
	}
	if (!JsonObject->TryGetNumberField(TEXT("timestamp_us"), TimestampUs) || TimestampUs < 0)
	{
		OutError = TEXT("timestamp_us must be a non-negative integer");
		return false;
	}

	const TSharedPtr<FJsonObject>* RootObjectPtr = nullptr;
	if (!JsonObject->TryGetObjectField(TEXT("root"), RootObjectPtr) ||
		!RootObjectPtr || !RootObjectPtr->IsValid())
	{
		OutError = TEXT("root object is required");
		return false;
	}

	FVector RootTranslation;
	FQuat RootRotation;
	if (!ReadVector3(*RootObjectPtr, TEXT("translation_cm"), RootTranslation, OutError) ||
		!ReadQuatField(*RootObjectPtr, TEXT("rotation_xyzw"), RootRotation, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* JointsObjectPtr = nullptr;
	if (!JsonObject->TryGetObjectField(TEXT("joints"), JointsObjectPtr) ||
		!JointsObjectPtr || !JointsObjectPtr->IsValid())
	{
		OutError = TEXT("joints object is required");
		return false;
	}

	const TSharedPtr<FJsonObject>& JointsObject = *JointsObjectPtr;
	if (JointsObject->Values.IsEmpty())
	{
		OutError = TEXT("joints must contain at least one bone");
		return false;
	}
	if (JointsObject->Values.Num() > MaxJointsPerFrame)
	{
		OutError = TEXT("joints exceeds the 256-bone limit");
		return false;
	}

	TArray<FArdyJointRotation> ParsedJoints;
	ParsedJoints.Reserve(JointsObject->Values.Num());
	for (const auto& Pair : JointsObject->Values)
	{
		if (Pair.Key.IsEmpty() || !Pair.Value.IsValid())
		{
			OutError = TEXT("joints contains an empty bone name or value");
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* RotationValues = nullptr;
		if (!Pair.Value->TryGetArray(RotationValues))
		{
			OutError = FString::Printf(TEXT("joint %s must be an xyzw array"), *Pair.Key);
			return false;
		}

		FArdyJointRotation& Joint = ParsedJoints.AddDefaulted_GetRef();
		Joint.BoneName = FName(*Pair.Key);
		if (!ReadNormalizedQuat(
			RotationValues,
			FString::Printf(TEXT("joint %s"), *Pair.Key),
			Joint.RotationDelta,
			OutError))
		{
			return false;
		}
	}

	OutFrame.Subject = FName(*Subject);
	OutFrame.FrameNumber = FrameNumber;
	OutFrame.TimestampUs = TimestampUs;
	OutFrame.RootTranslationCm = RootTranslation;
	OutFrame.RootRotationDelta = RootRotation;
	OutFrame.Joints = MoveTemp(ParsedJoints);

	FString Gesture;
	if (JsonObject->TryGetStringField(TEXT("gesture"), Gesture))
	{
		OutFrame.GestureTag = FName(*Gesture);
	}

	const TSharedPtr<FJsonObject>* HandPoseObjectPtr = nullptr;
	if (JsonObject->TryGetObjectField(TEXT("hand_pose"), HandPoseObjectPtr) &&
		HandPoseObjectPtr && HandPoseObjectPtr->IsValid())
	{
		FString LeftPose;
		FString RightPose;
		if ((*HandPoseObjectPtr)->TryGetStringField(TEXT("left"), LeftPose))
		{
			OutFrame.LeftHandPoseTag = FName(*LeftPose);
		}
		if ((*HandPoseObjectPtr)->TryGetStringField(TEXT("right"), RightPose))
		{
			OutFrame.RightHandPoseTag = FName(*RightPose);
		}
	}

	double TransitionDurationMs = -1.0;
	if (JsonObject->TryGetNumberField(
		TEXT("transition_ms"),
		TransitionDurationMs))
	{
		if (!IsFinite(TransitionDurationMs) ||
			TransitionDurationMs < 0.0 ||
			TransitionDurationMs > 5000.0)
		{
			OutError = TEXT("transition_ms must be between 0 and 5000");
			return false;
		}
		OutFrame.TransitionDurationMs =
			static_cast<float>(TransitionDurationMs);
	}
	return true;
}

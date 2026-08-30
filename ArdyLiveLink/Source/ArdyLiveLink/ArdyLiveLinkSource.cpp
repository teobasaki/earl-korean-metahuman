#include "ArdyLiveLinkSource.h"
#include "ILiveLinkClient.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogArdyLiveLink, Log, All);

const FName FArdyLiveLinkSource::SubjectName(TEXT("ArdyMotion"));

FArdyLiveLinkSource::FArdyLiveLinkSource(const FString& InHost, uint16 InPort, EAxisFix InAxisFix)
	: Host(InHost), Port(InPort), AxisFix(InAxisFix)
{
}

FArdyLiveLinkSource::~FArdyLiveLinkSource()
{
	Stop();
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

void FArdyLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	Client = InClient;
	SourceGuid = InSourceGuid;
	Thread = FRunnableThread::Create(this, TEXT("ArdyLiveLinkReceiver"), 0, TPri_Normal);
}

bool FArdyLiveLinkSource::IsSourceStillValid() const { return !bStopping; }

bool FArdyLiveLinkSource::RequestSourceShutdown()
{
	Stop();
	return true;
}

FText FArdyLiveLinkSource::GetSourceType() const { return NSLOCTEXT("ArdyLiveLink", "SourceType", "Ardy Motion (TCP)"); }
FText FArdyLiveLinkSource::GetSourceMachineName() const { return FText::FromString(FString::Printf(TEXT("%s:%d"), *Host, Port)); }
FText FArdyLiveLinkSource::GetSourceStatus() const
{
	return bSkeletonReceived
		? FText::FromString(FString::Printf(TEXT("Streaming (%d frames)"), ReceivedFrames.GetValue()))
		: NSLOCTEXT("ArdyLiveLink", "Waiting", "Connecting");
}

void FArdyLiveLinkSource::Stop() { bStopping = true; }

uint32 FArdyLiveLinkSource::Run()
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	while (!bStopping)
	{
		// ── 접속 (재시도 루프) ──
		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bValidIp = false;
		Addr->SetIp(*Host, bValidIp);
		Addr->SetPort(Port);
		if (!bValidIp)
		{
			UE_LOG(LogArdyLiveLink, Error, TEXT("Invalid host: %s"), *Host);
			return 1;
		}

		FSocket* Socket = FTcpSocketBuilder(TEXT("ArdySocket")).AsBlocking().Build();
		if (!Socket || !Socket->Connect(*Addr))
		{
			if (Socket) { SocketSubsystem->DestroySocket(Socket); }
			FPlatformProcess::Sleep(1.0f); // 송신기 미기동 — 1초 후 재시도
			continue;
		}
		UE_LOG(LogArdyLiveLink, Log, TEXT("Connected to ardy sender %s:%d"), *Host, Port);

		// ── 줄 단위 수신 ──
		// ⚠️ 블로킹 Recv 금지: 데이터가 안 오면 Stop()이 영원히 안 먹어 스레드·프로세스가 행 된다
		//    (N3 루프백 테스트에서 실증 — 자동화 프로세스 4시간 행). 0.5s Wait 폴링으로 bStopping 존중.
		FString LineBuffer;
		TArray<uint8> Recv;
		Recv.SetNumUninitialized(64 * 1024);
		while (!bStopping)
		{
			// ★ 보낼 것이 있으면 먼저 흘려보낸다 (게임 스레드가 SendLine 으로 넣는다).
			// 0.5초 Wait 타임아웃이 있으므로 최대 그만큼 지연되지만, 캡션은 그 정도면 충분하다.
			FString Outgoing;
			while (OutgoingLines.Dequeue(Outgoing))
			{
				const FTCHARToUTF8 Utf8(*Outgoing);
				int32 Sent = 0;
				if (!Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Sent))
				{
					UE_LOG(LogArdyLiveLink, Warning, TEXT("캡션 전송 실패 — 서버가 안 받는 계약일 수 있다"));
				}
				else
				{
					UE_LOG(LogArdyLiveLink, Log, TEXT("서버로 보냄(%d바이트): %s"), Sent, *Outgoing.TrimEnd());
				}
			}

			if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(0.5)))
			{
				if (Socket->GetConnectionState() == SCS_ConnectionError)
				{
					break; // 연결 오류 → 재접속
				}
				continue; // 타임아웃 — bStopping 재확인
			}
			int32 BytesRead = 0;
			if (!Socket->Recv(Recv.GetData(), Recv.Num(), BytesRead) || BytesRead <= 0)
			{
				break; // 연결 종료 → 바깥 루프에서 재접속
			}
			// UTF-8 → FString (프로토콜은 ASCII 범위)
			for (int32 i = 0; i < BytesRead; ++i)
			{
				const TCHAR C = static_cast<TCHAR>(Recv[i]);
				if (C == TEXT('\n'))
				{
					FSkeletonDef Skeleton;
					FFrameDef Frame;
					bool bIsSkeleton = false, bIsFrame = false, bIsEnd = false;
					if (ParseLine(LineBuffer, Skeleton, Frame, bIsSkeleton, bIsFrame, bIsEnd))
					{
						if (bIsSkeleton)
						{
							ActiveSkeleton = MoveTemp(Skeleton);
							PushSkeletonStatic(ActiveSkeleton);
						}
						else if (bIsFrame && bSkeletonReceived)
						{
							PushFrame(Frame);
						}
						else if (bIsEnd)
						{
							UE_LOG(LogArdyLiveLink, Log, TEXT("Stream end (%d frames)"), ReceivedFrames.GetValue());
						}
					}
					LineBuffer.Reset();
				}
				else
				{
					LineBuffer.AppendChar(C);
				}
			}
		}

		Socket->Close();
		SocketSubsystem->DestroySocket(Socket);
	}
	return 0;
}

bool FArdyLiveLinkSource::ParseLine(const FString& Line, FSkeletonDef& OutSkeleton, FFrameDef& OutFrame,
	bool& bOutIsSkeleton, bool& bOutIsFrame, bool& bOutIsEnd)
{
	bOutIsSkeleton = bOutIsFrame = bOutIsEnd = false;

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}

	FString Type;
	if (!Root->TryGetStringField(TEXT("type"), Type))
	{
		return false;
	}

	if (Type == TEXT("skeleton"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		if (!Root->TryGetArrayField(TEXT("bones"), Bones))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& BoneValue : *Bones)
		{
			const TSharedPtr<FJsonObject>* BoneObj = nullptr;
			if (!BoneValue->TryGetObject(BoneObj))
			{
				return false;
			}
			FString BoneName;
			(*BoneObj)->TryGetStringField(TEXT("name"), BoneName);
			int32 Parent = -1;
			(*BoneObj)->TryGetNumberField(TEXT("parent"), Parent);
			OutSkeleton.BoneNames.Add(FName(*BoneName));
			OutSkeleton.BoneParents.Add(Parent);
		}
		bOutIsSkeleton = OutSkeleton.BoneNames.Num() > 0;
		return bOutIsSkeleton;
	}

	if (Type == TEXT("frame"))
	{
		Root->TryGetNumberField(TEXT("idx"), OutFrame.Index);
		Root->TryGetNumberField(TEXT("t"), OutFrame.Time);

		const TArray<TSharedPtr<FJsonValue>>* Rots = nullptr;
		if (!Root->TryGetArrayField(TEXT("rot"), Rots))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& RotValue : *Rots)
		{
			const TArray<TSharedPtr<FJsonValue>>* Q = nullptr;
			if (!RotValue->TryGetArray(Q) || Q->Num() != 4)
			{
				return false;
			}
			OutFrame.LocalRotations.Emplace(
				(*Q)[0]->AsNumber(), (*Q)[1]->AsNumber(), (*Q)[2]->AsNumber(), (*Q)[3]->AsNumber());
		}

		const TArray<TSharedPtr<FJsonValue>>* RootArr = nullptr;
		if (Root->TryGetArrayField(TEXT("root"), RootArr) && RootArr->Num() == 3)
		{
			OutFrame.RootTranslation = FVector(
				(*RootArr)[0]->AsNumber(), (*RootArr)[1]->AsNumber(), (*RootArr)[2]->AsNumber());
		}
		bOutIsFrame = OutFrame.LocalRotations.Num() > 0;
		return bOutIsFrame;
	}

	if (Type == TEXT("end"))
	{
		bOutIsEnd = true;
		return true;
	}
	return false;
}

void FArdyLiveLinkSource::SendLine(const FString& Line)
{
	// 줄 단위 프로토콜이라 개행을 보장한다. 실제 전송은 수신 스레드가 한다.
	OutgoingLines.Enqueue(Line.EndsWith(TEXT("\n")) ? Line : Line + TEXT("\n"));
}

const TCHAR* FArdyLiveLinkSource::AxisFixToString(EAxisFix Fix)
{
	switch (Fix)
	{
	case EAxisFix::SwapYZ:       return TEXT("SwapYZ");
	case EAxisFix::SwapYZNegXZ:  return TEXT("SwapYZNegXZ");
	case EAxisFix::SwapYZConj:   return TEXT("SwapYZConj");
	case EAxisFix::CycZXY:       return TEXT("CycZXY");
	case EAxisFix::CycZXYMirror: return TEXT("CycZXYMirror");
	case EAxisFix::Passthrough:
	default:                     return TEXT("Passthrough");
	}
}

bool FArdyLiveLinkSource::AxisFixFromString(const FString& Name, EAxisFix& OutFix)
{
	static const TPair<const TCHAR*, EAxisFix> Table[] = {
		{ TEXT("Passthrough"),  EAxisFix::Passthrough },
		{ TEXT("SwapYZ"),       EAxisFix::SwapYZ },
		{ TEXT("SwapYZNegXZ"),  EAxisFix::SwapYZNegXZ },
		{ TEXT("SwapYZConj"),   EAxisFix::SwapYZConj },
		{ TEXT("CycZXY"),       EAxisFix::CycZXY },
		{ TEXT("CycZXYMirror"), EAxisFix::CycZXYMirror },
	};
	for (const TPair<const TCHAR*, EAxisFix>& Entry : Table)
	{
		if (Name.Equals(Entry.Key, ESearchCase::IgnoreCase))
		{
			OutFix = Entry.Value;
			return true;
		}
	}
	return false;
}

FQuat FArdyLiveLinkSource::ApplyAxisFix(const FQuat& In, EAxisFix Fix)
{
	switch (Fix)
	{
	case EAxisFix::SwapYZ:       return FQuat(In.X, In.Z, In.Y, In.W);
	case EAxisFix::SwapYZNegXZ:  return FQuat(-In.X, -In.Z, In.Y, In.W);
	case EAxisFix::SwapYZConj:   return FQuat(In.X, In.Z, In.Y, -In.W);
	case EAxisFix::CycZXY:       return FQuat(In.Z, In.X, In.Y, In.W);
	case EAxisFix::CycZXYMirror: return FQuat(-In.Z, In.X, -In.Y, In.W);
	case EAxisFix::Passthrough:
	default:                     return In;
	}
}

FVector FArdyLiveLinkSource::ApplyAxisFixVector(const FVector& In, EAxisFix Fix)
{
	// 벡터는 improper 기저에서도 C를 그대로 적용한다(쿼터니언과 달리 부호 보정 불필요).
	// ARDY root_positions 는 미터 — UE cm 로 ×100.
	switch (Fix)
	{
	case EAxisFix::SwapYZConj:   return FVector(In.X, In.Z, In.Y) * 100.0;
	case EAxisFix::CycZXY:       return FVector(In.Z, In.X, In.Y) * 100.0;
	case EAxisFix::CycZXYMirror: return FVector(In.Z, -In.X, In.Y) * 100.0;
	default:                     return In; // 구 프리셋: 기존 동작 보존
	}
}

void FArdyLiveLinkSource::PushSkeletonStatic(const FSkeletonDef& Skeleton)
{
	if (!Client)
	{
		return;
	}
	FLiveLinkStaticDataStruct StaticData(FLiveLinkSkeletonStaticData::StaticStruct());
	FLiveLinkSkeletonStaticData& Skel = *StaticData.Cast<FLiveLinkSkeletonStaticData>();
	Skel.SetBoneNames(Skeleton.BoneNames);
	Skel.SetBoneParents(Skeleton.BoneParents);
	Client->PushSubjectStaticData_AnyThread({ SourceGuid, SubjectName },
		ULiveLinkAnimationRole::StaticClass(), MoveTemp(StaticData));
	bSkeletonReceived = true;
	UE_LOG(LogArdyLiveLink, Log, TEXT("Created Live Link subject %s (%d bones)"),
		*SubjectName.ToString(), Skeleton.BoneNames.Num());
}

void FArdyLiveLinkSource::PushFrame(const FFrameDef& Frame)
{
	if (!Client)
	{
		return;
	}
	const int32 NumBones = ActiveSkeleton.BoneNames.Num();
	if (Frame.LocalRotations.Num() != NumBones)
	{
		return;
	}

	FLiveLinkFrameDataStruct FrameData(FLiveLinkAnimationFrameData::StaticStruct());
	FLiveLinkAnimationFrameData& Anim = *FrameData.Cast<FLiveLinkAnimationFrameData>();
	Anim.Transforms.SetNum(NumBones);
	// 한 프레임 안에서는 같은 프리셋을 써야 한다 — 중간에 바뀌면 본마다 다른 축이 섞인다.
	const EAxisFix Fix = AxisFix.Load();
	for (int32 i = 0; i < NumBones; ++i)
	{
		const FQuat Q = ApplyAxisFix(Frame.LocalRotations[i], Fix).GetNormalized();
		const FVector T = (i == 0) ? ApplyAxisFixVector(Frame.RootTranslation, Fix) : FVector::ZeroVector; // 비루트는 참조 포즈 유지(회전만)
		Anim.Transforms[i] = FTransform(Q, T);
	}
	Client->PushSubjectFrameData_AnyThread({ SourceGuid, SubjectName }, MoveTemp(FrameData));
	ReceivedFrames.Increment();
}

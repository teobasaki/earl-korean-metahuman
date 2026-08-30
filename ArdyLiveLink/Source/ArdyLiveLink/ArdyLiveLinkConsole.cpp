// ArdyLiveLink 콘솔 명령 (2026-08-06)
//
// 왜 있나: 이 플러그인엔 `ULiveLinkSourceFactory` 가 없어서 **Live Link 창의
// Add Source 목록에 ArdyLiveLink 가 뜨지 않는다.** 지금까지 이 소스를 만든 건
// 자동화 테스트뿐이었다(EarlOneTests.cpp). 핸드오프 문서의 "Add Source → ArdyLiveLink"
// 절차는 그래서 그대로는 실행할 수 없다.
//
// 오늘 할 일은 EAxisFix 후보 3종을 **육안으로** 비교하는 것이라, UI 대화상자보다
// 콘솔이 낫다 — 스트림을 유지한 채 프리셋만 갈아끼워야 비교가 끊기지 않는다.
//
//   earl.Ardy 192.168.0.31 5006 CycZXYMirror   접속(프리셋 지정, 생략 시 CycZXYMirror)
//   earl.ArdyAxis SwapYZConj                   스트림 유지한 채 프리셋만 교체
//   earl.ArdyStatus                            수신 프레임 수·스켈레톤 수신 여부·현재 프리셋
//   earl.ArdyStop                              소스 제거
#include "ArdyLiveLinkSource.h"
#include "ArdyRetargetAnimInstance.h"
#include "ArdyRotationOnlyRemapAsset.h"
#include "Retargeter/IKRetargeter.h"
#if WITH_EDITOR
// retarget pose 교체는 에디터 전용 컨트롤러가 필요하다 (IKRigEditor 모듈).
// 런타임 타깃(Quest Shipping)에는 들어가면 안 되므로 반드시 가둔다.
#include "RetargetEditor/IKRetargeterController.h"
#endif

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/PlayerController.h"
#include "ILiveLinkClient.h"
#include "LiveLinkInstance.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogArdyConsole, Log, All);

namespace
{
	// 콘솔이 붙잡고 있는 활성 소스. 프리셋 교체·상태 조회의 대상.
	TSharedPtr<FArdyLiveLinkSource> GActiveSource;

	ILiveLinkClient* GetLiveLinkClient()
	{
		IModularFeatures& Features = IModularFeatures::Get();
		if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
		{
			UE_LOG(LogArdyConsole, Error,
				TEXT("earl.Ardy: LiveLink 클라이언트가 없다 — LiveLink 플러그인이 켜져 있는지 확인할 것"));
			return nullptr;
		}
		return &Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
	}

	void RemoveActiveSource()
	{
		if (!GActiveSource.IsValid())
		{
			return;
		}
		if (ILiveLinkClient* Client = GetLiveLinkClient())
		{
			Client->RemoveSource(GActiveSource);
		}
		GActiveSource.Reset();
	}

	FAutoConsoleCommand GArdyConnect(
		TEXT("earl.Ardy"),
		TEXT("Connect the ArdyLiveLink source: earl.Ardy [host] [port] [axisFix]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				ILiveLinkClient* Client = GetLiveLinkClient();
				if (!Client)
				{
					return;
				}

				const FString Host = Args.Num() > 0 ? Args[0] : TEXT("192.168.0.31");
				const uint16 Port = static_cast<uint16>(Args.Num() > 1 ? FCString::Atoi(*Args[1]) : 5006);

				// 판정 1순위가 기본값이다 (선행확정 §2 — 전방→전방 + 좌우 물리 보존)
				FArdyLiveLinkSource::EAxisFix Fix = FArdyLiveLinkSource::EAxisFix::CycZXYMirror;
				if (Args.Num() > 2 && !FArdyLiveLinkSource::AxisFixFromString(Args[2], Fix))
				{
					UE_LOG(LogArdyConsole, Warning,
						TEXT("earl.Ardy: 알 수 없는 프리셋 '%s' — CycZXYMirror 로 진행한다"), *Args[2]);
				}

				RemoveActiveSource();

				GActiveSource = MakeShared<FArdyLiveLinkSource>(Host, Port, Fix);
				Client->AddSource(GActiveSource);
				UE_LOG(LogArdyConsole, Log,
					TEXT("earl.Ardy: %s:%d 접속 시도 · 프리셋 %s · 서브젝트 %s"),
					*Host, Port, FArdyLiveLinkSource::AxisFixToString(Fix),
					*FArdyLiveLinkSource::SubjectName.ToString());
			}));

	FAutoConsoleCommand GArdyAxis(
		TEXT("earl.ArdyAxis"),
		TEXT("Swap the axis-fix preset live: earl.ArdyAxis <Passthrough|SwapYZ|SwapYZNegXZ|SwapYZConj|CycZXY|CycZXYMirror>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				if (!GActiveSource.IsValid())
				{
					UE_LOG(LogArdyConsole, Warning, TEXT("earl.ArdyAxis: 활성 소스가 없다 — 먼저 earl.Ardy"));
					return;
				}
				if (Args.Num() == 0)
				{
					UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyAxis: 현재 %s"),
						FArdyLiveLinkSource::AxisFixToString(GActiveSource->GetAxisFix()));
					return;
				}
				FArdyLiveLinkSource::EAxisFix Fix;
				if (!FArdyLiveLinkSource::AxisFixFromString(Args[0], Fix))
				{
					UE_LOG(LogArdyConsole, Error, TEXT("earl.ArdyAxis: 알 수 없는 프리셋 '%s'"), *Args[0]);
					return;
				}
				GActiveSource->SetAxisFix(Fix);
				// 육안 판정 로그다 — 캡처와 대조하려면 시각이 남아야 한다
				UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyAxis -> %s"),
					FArdyLiveLinkSource::AxisFixToString(Fix));
			}));

	FAutoConsoleCommand GArdyStatus(
		TEXT("earl.ArdyStatus"),
		TEXT("Report ArdyLiveLink receive state"),
		FConsoleCommandDelegate::CreateStatic(
			[]()
			{
				if (!GActiveSource.IsValid())
				{
					UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyStatus: 활성 소스 없음"));
					return;
				}
				UE_LOG(LogArdyConsole, Log,
					TEXT("earl.ArdyStatus: 프리셋 %s · 스켈레톤 %s · 수신 프레임 %d · 상태 %s"),
					FArdyLiveLinkSource::AxisFixToString(GActiveSource->GetAxisFix()),
					GActiveSource->HasReceivedSkeleton() ? TEXT("수신함") : TEXT("아직"),
					GActiveSource->GetReceivedFrameCount(),
					*GActiveSource->GetSourceStatus().ToString());
			}));

	// ── 디버그 스켈레톤 (2026-08-06) ──────────────────────────────────────
	// 왜 필요한가: EAxisFix 확정은 육안 판정인데, 정석 경로(IK Retargeter → MetaHuman)는
	// **cskel27 소스 스켈레탈 메시**가 있어야 만든다. 그 자산(npz→FBX)이 아직 레포에 없다.
	// 그래서 수신한 27본을 그대로 선으로 그려 판정한다 — 눕는가/거울인가/뒤를 보는가는
	// 스틱 피겨로 전부 판별된다.
	// ⚠️⚠️ **이 방법은 단독으로는 안 된다** (2026-08-06 실측으로 확인).
	//   ARDY 스트림에는 **본 오프셋이 없다.** PushFrame 이 비루트 본에 translation 을 넣지
	//   않는다("비루트는 참조 포즈 유지(회전만)") — 즉 전선으로 오는 것은 **로컬 회전 27개
	//   + 루트 위치**뿐이고, 뼈 길이는 받는 쪽 스켈레톤의 레퍼런스 포즈에서 온다.
	//   그래서 로컬 트랜스폼을 누적하면 **27개 관절이 전부 루트 한 점에 겹친다**(실측 확인:
	//   Hips·Head·LeftHand 전부 X-1.830 Y-4.173 Z+92.456).
	//   → 그림을 그리려면 바인드 포즈 오프셋이 필요하다. 출처는 둘 중 하나:
	//     ① cskel27 바인드(`ardy/assets/skeletons/cskel27/skin_standard.npz` 의
	//        `bind_rig_transform`) 를 가져와 하드코딩/자산화
	//     ② 아니면 애초에 정석대로 MetaHuman 스켈레톤에 이름 매핑해 얹는다(= 리타깃)
	//   지금 이 함수는 ①이 들어오면 바로 쓸 수 있게 구조만 남겨 둔다.
	// ⚠️ 그리고 이 경로는 **FBX 임포터를 거치지 않는다.** 선행확정 §2 말미의 경고
	//    ("소스 메시의 임포트 규약이 순위를 바꿀 수 있다")는 이 판정으로 해소되지 않는다.
	FTSTicker::FDelegateHandle GDrawTicker;
	float GDrawScale = 100.0f;   // 미터 → cm
	FVector GDrawOrigin(0, 0, 0);

	void DrawArdySkeleton()
	{
		ILiveLinkClient* Client = GetLiveLinkClient();
		UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
		if (!Client || !World || !GActiveSource.IsValid())
		{
			return;
		}

		FLiveLinkSubjectFrameData Frame;
		const FLiveLinkSubjectKey Key(FGuid(), FArdyLiveLinkSource::SubjectName);
		if (!Client->EvaluateFrame_AnyThread(
				FArdyLiveLinkSource::SubjectName, ULiveLinkAnimationRole::StaticClass(), Frame))
		{
			return;
		}
		const FLiveLinkSkeletonStaticData* Skel = Frame.StaticData.Cast<FLiveLinkSkeletonStaticData>();
		const FLiveLinkAnimationFrameData* Anim = Frame.FrameData.Cast<FLiveLinkAnimationFrameData>();
		if (!Skel || !Anim || Anim->Transforms.Num() != Skel->GetBoneNames().Num())
		{
			return;
		}

		const TArray<int32>& Parents = Skel->GetBoneParents();
		const int32 Num = Anim->Transforms.Num();

		// 로컬 → 컴포넌트 공간 누적 (부모가 항상 먼저 오는 순서라고 가정 — cskel27 은 그렇다)
		TArray<FTransform> Component;
		Component.SetNum(Num);
		for (int32 i = 0; i < Num; ++i)
		{
			const int32 P = Parents.IsValidIndex(i) ? Parents[i] : INDEX_NONE;
			Component[i] = (P >= 0 && P < i)
				? Anim->Transforms[i] * Component[P]
				: Anim->Transforms[i];
		}

		for (int32 i = 0; i < Num; ++i)
		{
			const FVector Pos = GDrawOrigin + Component[i].GetLocation() * GDrawScale;
			const int32 P = Parents.IsValidIndex(i) ? Parents[i] : INDEX_NONE;
			if (P >= 0 && P < Num)
			{
				const FVector ParentPos = GDrawOrigin + Component[P].GetLocation() * GDrawScale;
				DrawDebugLine(World, ParentPos, Pos, FColor::Green, false, -1.0f, 0, 1.5f);
			}
			// 좌우를 색으로 구분한다 — 거울 판정이 이 한 눈에 끝난다
			const FString BoneName = Skel->GetBoneNames()[i].ToString();
			FColor Dot = FColor::White;
			if (BoneName.StartsWith(TEXT("Left")))  { Dot = FColor::Blue; }
			if (BoneName.StartsWith(TEXT("Right"))) { Dot = FColor::Red; }
			DrawDebugPoint(World, Pos, 8.0f, Dot, false, -1.0f, 0);
		}
	}

	FAutoConsoleCommand GArdyDraw(
		TEXT("earl.ArdyDraw"),
		TEXT("Draw the received ARDY skeleton as debug lines: earl.ArdyDraw <0|1> [scale] [x y z]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				const bool bOn = Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0;
				if (Args.Num() > 1) { GDrawScale = FCString::Atof(*Args[1]); }
				if (Args.Num() > 4)
				{
					GDrawOrigin = FVector(FCString::Atof(*Args[2]),
										  FCString::Atof(*Args[3]),
										  FCString::Atof(*Args[4]));
				}
				if (GDrawTicker.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(GDrawTicker);
					GDrawTicker.Reset();
				}
				if (bOn)
				{
					GDrawTicker = FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateStatic([](float) { DrawArdySkeleton(); return true; }));
				}
				UE_LOG(LogArdyConsole, Log,
					TEXT("earl.ArdyDraw: %s (배율 %.0f, 원점 %s) — 파랑=Left 빨강=Right"),
					bOn ? TEXT("켬") : TEXT("끔"), GDrawScale, *GDrawOrigin.ToCompactString());
			}));

	// 판정용 수치 요약 — 눈으로 본 것을 숫자로 뒷받침한다(반대도 마찬가지).
	FAutoConsoleCommand GArdyPose(
		TEXT("earl.ArdyPose"),
		TEXT("Report key joint positions of the received ARDY pose (up/forward/chirality check)"),
		FConsoleCommandDelegate::CreateStatic(
			[]()
			{
				ILiveLinkClient* Client = GetLiveLinkClient();
				if (!Client || !GActiveSource.IsValid())
				{
					UE_LOG(LogArdyConsole, Warning, TEXT("earl.ArdyPose: 활성 소스 없음"));
					return;
				}
				FLiveLinkSubjectFrameData Frame;
				if (!Client->EvaluateFrame_AnyThread(
						FArdyLiveLinkSource::SubjectName, ULiveLinkAnimationRole::StaticClass(), Frame))
				{
					UE_LOG(LogArdyConsole, Warning, TEXT("earl.ArdyPose: 프레임 평가 실패"));
					return;
				}
				const FLiveLinkSkeletonStaticData* Skel = Frame.StaticData.Cast<FLiveLinkSkeletonStaticData>();
				const FLiveLinkAnimationFrameData* Anim = Frame.FrameData.Cast<FLiveLinkAnimationFrameData>();
				if (!Skel || !Anim) { return; }

				const TArray<int32>& Parents = Skel->GetBoneParents();
				const int32 Num = Anim->Transforms.Num();
				TArray<FTransform> Comp;
				Comp.SetNum(Num);
				for (int32 i = 0; i < Num; ++i)
				{
					const int32 P = Parents.IsValidIndex(i) ? Parents[i] : INDEX_NONE;
					Comp[i] = (P >= 0 && P < i) ? Anim->Transforms[i] * Comp[P] : Anim->Transforms[i];
				}
				auto Find = [&](const TCHAR* Name) -> int32
				{
					for (int32 i = 0; i < Skel->GetBoneNames().Num(); ++i)
					{
						if (Skel->GetBoneNames()[i].ToString().Equals(Name, ESearchCase::IgnoreCase))
						{
							return i;
						}
					}
					return INDEX_NONE;
				};
				auto Log1 = [&](const TCHAR* Name)
				{
					const int32 i = Find(Name);
					if (i != INDEX_NONE)
					{
						const FVector P = Comp[i].GetLocation();
						UE_LOG(LogArdyConsole, Log, TEXT("  %-14s X%+7.3f Y%+7.3f Z%+7.3f"), Name, P.X, P.Y, P.Z);
					}
				};
				UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyPose (프리셋 %s) — UE 규약: Z=위, +X=전방, +Y=오른쪽"),
					FArdyLiveLinkSource::AxisFixToString(GActiveSource->GetAxisFix()));
				Log1(TEXT("Hips")); Log1(TEXT("Head"));
				Log1(TEXT("LeftHand")); Log1(TEXT("RightHand"));
				Log1(TEXT("LeftFoot")); Log1(TEXT("RightFoot"));
				UE_LOG(LogArdyConsole, Log,
					TEXT("  판정: 서 있으면 Head.Z > Hips.Z · 좌우 정상이면 LeftHand.Y < RightHand.Y"));
			}));

	// ── 소스 메시 프리뷰 (2026-08-06) ────────────────────────────────────
	// EAxisFix 육안 판정용. 다른 구현이 쓰는
	// 방식을 그대로 가져왔다 — **AnimBP 를 새로 만들 필요가 없다.** 엔진 내장
	// `ULiveLinkInstance` 를 스켈레탈 메시에 물리고 서브젝트 이름만 지정하면
	// Live Link 프레임이 바로 포즈로 들어간다.
	//
	// 소스 메시는 ARDY 출력 스켈레톤(cskel27, 본 28개)에 맞춘 것이다.
	// **이게 중요하다** — 선행확정 §2 말미가 경고한 "소스 메시의 FBX 임포트 규약"이
	// 이 자산에 이미 반영돼 있으므로, 여기서 나오는 판정이 그 변수를 포함한 최종심이다.
	// 직접 만든 스틱 피겨로는 그 변수를 우회해 버려서 답이 될 수 없었다.
	TWeakObjectPtr<AActor> GPreviewActor;

	FAutoConsoleCommand GArdyPreview(
		TEXT("earl.ArdyPreview"),
		TEXT("Spawn the ARDY source mesh (hidden): earl.ArdyPreview [x] [y] [scale] [z] [show]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
				if (!World)
				{
					UE_LOG(LogArdyConsole, Error, TEXT("earl.ArdyPreview: 월드가 없다"));
					return;
				}

				USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(
					nullptr, TEXT("/Game/Retarget/ARDY_SourceMesh.ARDY_SourceMesh"));
				if (!Mesh)
				{
					UE_LOG(LogArdyConsole, Error,
						TEXT("earl.ArdyPreview: ARDY 소스 메시를 못 찾았다 — 소스 스켈레탈 메시 경로 확인"));
					return;
				}

				// ⚠️ 약참조 하나만 들고 있으면 이전 프리뷰가 안 지워지고 **쌓인다**.
				// (2026-08-06: 여러 번 스폰한 끝에 회색 인형이 여러 개 널브러져 있었다)
				// 태그로 전수 정리한다.
				static const FName PreviewTag(TEXT("ArdyPreview"));
				int32 Removed = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->ActorHasTag(PreviewTag))
					{
						It->Destroy();
						++Removed;
					}
				}
				GPreviewActor.Reset();
				if (Removed > 0)
				{
					UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyPreview: 기존 프리뷰 %d개 제거"), Removed);
				}

				// ★ **절대 월드 좌표**로 세운다 (2026-08-06 변경).
				// 카메라 상대로 잡았더니 부를 때마다 위치가 달라져 "자꾸 뒤로 간다"가 됐다.
				// 이 스테이지의 카메라는 (-115, 0, 190)에서 +X 를 본다 — 기본값은 그 앞
				// 왼쪽 빈 바닥이다. 재현 가능한 자리라야 프리셋 비교가 성립한다.
				const FVector Loc(
					Args.Num() > 0 ? FCString::Atof(*Args[0]) : 260.0f,
					Args.Num() > 1 ? FCString::Atof(*Args[1]) : -130.0f,
					Args.Num() > 3 ? FCString::Atof(*Args[3]) : 0.0f);

				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				// ⚠️ 빈 AActor + NewObject 컴포넌트로는 렌더가 안 붙었다(2026-08-06).
				// 위치·스케일·바운드는 다 맞는데 화면에 아무것도 안 나왔다.
				// ASkeletalMeshActor 는 컴포넌트가 제대로 구성된 채로 스폰된다.
				ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(
					ASkeletalMeshActor::StaticClass(), Loc, FRotator::ZeroRotator, Params);
				if (!Actor)
				{
					UE_LOG(LogArdyConsole, Error, TEXT("earl.ArdyPreview: 액터 스폰 실패"));
					return;
				}
				USkeletalMeshComponent* Comp = Actor->GetSkeletalMeshComponent();
				if (!Comp)
				{
					UE_LOG(LogArdyConsole, Error, TEXT("earl.ArdyPreview: 스켈레탈 메시 컴포넌트 없음"));
					return;
				}
				Comp->SetSkeletalMeshAsset(Mesh);

				// ★ 팀 방식: 내장 LiveLink 애님 인스턴스 + 서브젝트 지정
				Comp->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
				if (ULiveLinkInstance* Inst = Cast<ULiveLinkInstance>(Comp->GetAnimInstance()))
				{
					// ★ 기본 리맵 에셋을 쓰면 안 된다 — 프레임의 translation(=0)까지 적용해
					// 본 오프셋을 지워 버려 스켈레톤이 한 점으로 접힌다(실측 높이 20cm).
					// 회전만 적용하는 우리 리맵으로 교체한다.
					Inst->SetRetargetAsset(UArdyRotationOnlyRemapAsset::StaticClass());
					// 회전 해석(절대/델타)은 CDO 기본값을 따라간다 — earl.ArdyDelta 로 바꾼다.
					Inst->SetSubject(FLiveLinkSubjectName(FArdyLiveLinkSource::SubjectName));
					Inst->EnableLiveLinkEvaluation(true);
				}
				else
				{
					UE_LOG(LogArdyConsole, Warning,
						TEXT("earl.ArdyPreview: ULiveLinkInstance 를 못 만들었다 — 포즈가 안 들어올 수 있다"));
				}
				// ★ 기본은 **숨긴다** (2026-08-06). 이 메시는 리타깃 소스로만 필요하고
				// 사람 형상이 없는 20cm 플레이스홀더 큐브라, 보이면 "회색 박스가 떠다닌다"가 된다.
				// 숨겨도 애님은 평가되므로 소스 역할에는 지장이 없다.
				// 5번째 인자에 1 을 주면 보인다(디버그용).
				const bool bShow = Args.Num() > 4 && FCString::Atoi(*Args[4]) != 0;
				Comp->SetVisibility(bShow, true);
				Comp->bHiddenInGame = !bShow;
				// 숨겨도 포즈는 계속 계산해야 한다 — 안 그러면 리타깃 소스가 죽는다.
				Comp->VisibilityBasedAnimTickOption =
					EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

				// ⚠️ ARDY 는 **미터**다. FBX 를 1.0 스케일로 임포트했으면 UE 에서 1.7 유닛
				// (=1.7cm) 짜리 인형이 되어 화면에서 안 보인다 — 2026-08-06 에 실제로 겪었다.
				// 그래서 스케일을 인자로 뺐고, 실제 바운드를 로그로 찍어 확인한다.
				const float Scale = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 8.75f;
				// ⚠️ 루트 컴포넌트를 스폰 **뒤에** 붙이면 스폰 위치가 날아가고 원점에 남는다.
				// 그래서 화면에 아무것도 안 보였다(원점의 타깃 캐릭터 안에 겹쳐 있었다).
				// 등록 후 월드 위치를 명시적으로 다시 잡는다.
				Actor->Tags.Add(FName(TEXT("ArdyPreview")));
				Actor->SetActorLocation(Loc);
				Comp->SetWorldScale3D(FVector(Scale));
				Comp->UpdateBounds();

				GPreviewActor = Actor;
				const FBoxSphereBounds B = Comp->Bounds;

				// ★ 안 보일 때 어디서 끊기는지 재려고 전부 찍는다 (추정 금지).
				{
					FVector CamLoc = FVector::ZeroVector;
					FRotator CamRot = FRotator::ZeroRotator;
					if (APlayerController* PC = World->GetFirstPlayerController())
					{
						PC->GetPlayerViewPoint(CamLoc, CamRot);
					}
					const FVector ToActor = Comp->GetComponentLocation() - CamLoc;
					const float Fwd = FVector::DotProduct(ToActor.GetSafeNormal(), CamRot.Vector());
					UE_LOG(LogArdyConsole, Log,
						TEXT("  진단 카메라 %s 방향 %s | 대상까지 %.0fcm · 정면성 %.2f(1=정면, 음수=뒤)"),
						*CamLoc.ToCompactString(), *CamRot.ToCompactString(), ToActor.Size(), Fwd);
					UE_LOG(LogArdyConsole, Log,
						TEXT("  진단 머티리얼 %d개 · Visible %d · HiddenInGame %d · 등록 %d · LOD %d · 본 %d"),
						Comp->GetNumMaterials(), Comp->IsVisible() ? 1 : 0,
						Comp->bHiddenInGame ? 1 : 0, Comp->IsRegistered() ? 1 : 0,
						Comp->GetPredictedLODLevel(), Comp->GetNumBones());
					UE_LOG(LogArdyConsole, Log,
						TEXT("  진단 바운드 원점 %s · 박스 %s"),
						*B.Origin.ToCompactString(), *B.BoxExtent.ToCompactString());
					for (int32 m = 0; m < Comp->GetNumMaterials(); ++m)
					{
						UMaterialInterface* Mat = Comp->GetMaterial(m);
						UE_LOG(LogArdyConsole, Log, TEXT("    머티리얼[%d] = %s"),
							m, Mat ? *Mat->GetName() : TEXT("**없음(null)**"));
					}
				}
				UE_LOG(LogArdyConsole, Log,
					TEXT("earl.ArdyPreview: %s 배치 (실제 %s) · 스케일 %.1f · 높이 %.1fcm (반경 %.1f) · 서브젝트 %s · 프리셋 %s"),
					*Mesh->GetName(), *Comp->GetComponentLocation().ToCompactString(), Scale,
					B.BoxExtent.Z * 2.0f, B.SphereRadius,
					*FArdyLiveLinkSource::SubjectName.ToString(),
					GActiveSource.IsValid()
						? FArdyLiveLinkSource::AxisFixToString(GActiveSource->GetAxisFix())
						: TEXT("(소스 없음)"));
			}));

	// 프리뷰 메시의 **월드 본 위치**를 찍는다. "안 보인다"의 원인을 추측으로 좁히다
	// 여러 번 헛짚어서(스케일·루트이동·컴포넌트 종류) 결국 이걸 재는 게 빨랐다.
	FAutoConsoleCommand GArdyPreviewProbe(
		TEXT("earl.ArdyPreviewProbe"),
		TEXT("Log world-space bone locations of the preview mesh"),
		FConsoleCommandDelegate::CreateStatic(
			[]()
			{
				if (!GPreviewActor.IsValid())
				{
					UE_LOG(LogArdyConsole, Warning, TEXT("earl.ArdyPreviewProbe: 프리뷰가 없다"));
					return;
				}
				USkeletalMeshComponent* Comp =
					GPreviewActor->FindComponentByClass<USkeletalMeshComponent>();
				if (!Comp)
				{
					return;
				}
				UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyPreviewProbe: 컴포넌트 %s · 본 %d개"),
					*Comp->GetComponentLocation().ToCompactString(), Comp->GetNumBones());
				for (const TCHAR* Name : { TEXT("Hips"), TEXT("Spine2"), TEXT("Head"),
										   TEXT("LeftHand"), TEXT("RightHand"), TEXT("LeftFoot") })
				{
					const int32 Idx = Comp->GetBoneIndex(FName(Name));
					if (Idx != INDEX_NONE)
					{
						UE_LOG(LogArdyConsole, Log, TEXT("   %-10s %s"),
							Name, *Comp->GetBoneLocation(FName(Name)).ToCompactString());
					}
					else
					{
						UE_LOG(LogArdyConsole, Log, TEXT("   %-10s (없음)"), Name);
					}
				}
				const FBoxSphereBounds B = Comp->Bounds;
				UE_LOG(LogArdyConsole, Log, TEXT("   바운드 원점 %s · 박스 %s"),
					*B.Origin.ToCompactString(), *B.BoxExtent.ToCompactString());
			}));

	// ── ★ 타깃 캐릭터 바디에 얹기 (2026-08-06) ────────────────────────────────
	// **육안 판정은 이 경로로만 가능하다.** ARDY 소스 메시는 사람 형상이 없는
	// 20cm 플레이스홀더 큐브라(실측 바운드 10,10,10) 아무리 띄워도 회색 조각만 보인다.
	// 살이 있는 것은 MetaHuman 바디뿐이므로, 리타게터를 거쳐 거기에 얹는다.
	//
	//   earl.ArdyRetarget            씬의 타깃 캐릭터 바디에 ARDY 모션을 얹는다
	//   earl.ArdyRetarget 0          원래 애니로 되돌린다
	FAutoConsoleCommand GArdyRetarget(
		TEXT("earl.ArdyRetarget"),
		TEXT("Drive the MetaHuman body with the ARDY stream: earl.ArdyRetarget [0|1] [team|ours]"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
				if (!World)
				{
					return;
				}
				const bool bOn = Args.Num() == 0 || FCString::Atoi(*Args[0]) != 0;

				// 소스 = 프리뷰 큐브 메시(Live Link 로 구동 중). 없으면 만들라고 알려준다.
				USkeletalMeshComponent* Source = GPreviewActor.IsValid()
					? GPreviewActor->FindComponentByClass<USkeletalMeshComponent>()
					: nullptr;
				if (bOn && !Source)
				{
					UE_LOG(LogArdyConsole, Error,
						TEXT("earl.ArdyRetarget: 소스가 없다 — 먼저 earl.ArdyPreview 로 소스 메시를 띄울 것"));
					return;
				}

				// 두 번째 인자로 리타게터를 고른다 — 팀 것과 우리 것을 A/B 하려고.
				//   (없음)|team → 팀 RTG_ARDY_to_CharacterTeam (ardy.motion.v1 계약용)
				//   ours       → 우리 계약(절대 로컬 회전)용. Scripts/make_ardy_retargeter.py 산출
				const bool bOurs = Args.Num() > 1 && Args[1].Equals(TEXT("ours"), ESearchCase::IgnoreCase);
				const TCHAR* RtgPath = bOurs
					? TEXT("/Game/Retarget/RTG_ARDY_to_Character.RTG_ARDY_to_Character")
					: TEXT("/Game/Retarget/RTG_ARDY_to_CharacterTeam.RTG_ARDY_to_CharacterTeam");
				UIKRetargeter* Retargeter = LoadObject<UIKRetargeter>(nullptr, RtgPath);
				if (bOn && !Retargeter)
				{
					UE_LOG(LogArdyConsole, Error,
						TEXT("earl.ArdyRetarget: RTG_ARDY_to_CharacterTeam 을 못 찾았다 — IK Retargeter 에셋 경로 확인"));
					return;
				}

				// 씬의 바디 메시를 찾는다 ("Body" 를 포함하고 "Face" 가 아닌 것 — 얼굴은 립싱크 소유)
				int32 Count = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->ActorHasTag(FName(TEXT("ArdyPreview"))))
					{
						continue; // 소스 자신은 건드리지 않는다
					}
					TArray<USkeletalMeshComponent*> Meshes;
					It->GetComponents(Meshes);
					for (USkeletalMeshComponent* Mesh : Meshes)
					{
						const FString Name = Mesh->GetName();
						if (!Name.Contains(TEXT("Body")) || Name.Contains(TEXT("Face")))
						{
							continue;
						}
						if (bOn)
						{
							Mesh->SetAnimInstanceClass(UArdyRetargetAnimInstance::StaticClass());
							if (UArdyRetargetAnimInstance* Inst =
									Cast<UArdyRetargetAnimInstance>(Mesh->GetAnimInstance()))
							{
								Inst->Configure(Retargeter, Source);
								++Count;
							}
						}
						else
						{
							Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
							Mesh->Stop();
							++Count;
						}
					}
				}
				UE_LOG(LogArdyConsole, Log,
					TEXT("earl.ArdyRetarget: %s — 바디 메시 %d개 (프리셋 %s)"),
					bOn ? TEXT("적용") : TEXT("해제"), Count,
					GActiveSource.IsValid()
						? FArdyLiveLinkSource::AxisFixToString(GActiveSource->GetAxisFix())
						: TEXT("(소스 없음)"));
			}));

	// ── ★ 타깃(캐릭터 몸) 계측 (2026-08-06) ───────────────────────────────
	// ⚠️ **판정은 반드시 타깃에서 한다.** 소스(ARDY 큐브 스켈레톤)를 재서 SwapYZConj 로
	// 결론냈다가 육안에서 뒤집혔다 — 소스가 서 있어도 리타깃 결과는 누울 수 있다.
	// 리타게터를 "돌려보고 고치기"로 진화시키려면 매 회차 판정이 자동이어야 하고,
	// 그 판정 기준이 이것이다:
	//     서 있다 = head.Z > pelvis.Z + 50  그리고  foot.Z < pelvis.Z
	FAutoConsoleCommand GArdyHeadSync(
		TEXT("earl.HeadSync"),
		TEXT("Body mesh head bone vs Face mesh head bone — do they actually move together?"),
		FConsoleCommandDelegate::CreateStatic(
			[]()
			{
				// 신고: "몸이랑 얼굴이랑 따로 논다" (2026-08-10).
				// MetaHuman 은 **Body 와 Face 가 별개 스켈레탈 메시**다. ARDY 리타깃은
				// Body 메시만 건드린다(`earl.ArdyRetarget` 이 Face 를 건너뛴다).
				// 눈에 보이는 얼굴은 Face 메시이므로, Face 가 Body 의 머리뼈를 따라오지 않으면
				// **아무리 HeadArdyWeight 를 올려도 얼굴은 제자리**다.
				// 두 메시의 `head` 본 월드 회전을 직접 비교한다 — 추측을 끝낸다.
				UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
				if (!World) { return; }
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->ActorHasTag(FName(TEXT("ArdyPreview")))) { continue; }
					TArray<USkeletalMeshComponent*> Meshes;
					It->GetComponents(Meshes);
					USkeletalMeshComponent* BodyMesh = nullptr;
					USkeletalMeshComponent* FaceMesh = nullptr;
					for (USkeletalMeshComponent* M : Meshes)
					{
						const FString N = M->GetName();
						if (N.Contains(TEXT("Face"))) { FaceMesh = M; }
						else if (N.Contains(TEXT("Body"))) { BodyMesh = M; }
					}
					if (!BodyMesh || !FaceMesh) { continue; }

					auto HeadXf = [](USkeletalMeshComponent* M) -> FTransform
					{
						const int32 Idx = M->GetBoneIndex(FName(TEXT("head")));
						return Idx != INDEX_NONE ? M->GetBoneTransform(Idx) : FTransform::Identity;
					};
					const FTransform BodyHead = HeadXf(BodyMesh);
					const FTransform FaceHead = HeadXf(FaceMesh);
					const FRotator BR = BodyHead.Rotator();
					const FRotator FR = FaceHead.Rotator();
					const float AngleDeg = FMath::RadiansToDegrees(
						BodyHead.GetRotation().AngularDistance(FaceHead.GetRotation()));
					const float PosDist = FVector::Dist(BodyHead.GetLocation(), FaceHead.GetLocation());

					UE_LOG(LogArdyConsole, Log, TEXT("earl.HeadSync [%s]"), *It->GetName());
					UE_LOG(LogArdyConsole, Log,
						TEXT("   Body.head  회전 P%.1f Y%.1f R%.1f · 위치 %s  (%s, LeaderPose=%s)"),
						BR.Pitch, BR.Yaw, BR.Roll, *BodyHead.GetLocation().ToCompactString(),
						*BodyMesh->GetName(),
						BodyMesh->LeaderPoseComponent.IsValid() ? TEXT("있음") : TEXT("없음"));
					UE_LOG(LogArdyConsole, Log,
						TEXT("   Face.head  회전 P%.1f Y%.1f R%.1f · 위치 %s  (%s, LeaderPose=%s, 부착=%s)"),
						FR.Pitch, FR.Yaw, FR.Roll, *FaceHead.GetLocation().ToCompactString(),
						*FaceMesh->GetName(),
						FaceMesh->LeaderPoseComponent.IsValid() ? TEXT("있음") : TEXT("없음"),
						FaceMesh->GetAttachParent()
							? *FString::Printf(TEXT("%s/%s"), *FaceMesh->GetAttachParent()->GetName(),
								*FaceMesh->GetAttachSocketName().ToString())
							: TEXT("없음"));
					// ⚠️ **월드 회전을 직접 빼면 안 된다.** Body 와 Face 는 **다른 스켈레톤**이라
					//    머리뼈 rest 방향이 다르고, 그 상수 오프셋이 통째로 섞여 들어온다
					//    (2026-08-10: 그걸로 "따로 논다"고 한 번 잘못 판정했다).
					//    같이 움직이는지는 **프레임 간 변화량**을 비교해야 안다.
					static FQuat PrevBody = FQuat::Identity;
					static FQuat PrevFace = FQuat::Identity;
					static bool bHavePrev = false;
					const FQuat BodyQ = BodyHead.GetRotation();
					const FQuat FaceQ = FaceHead.GetRotation();
					if (bHavePrev)
					{
						const float BodyMove = FMath::RadiansToDegrees(PrevBody.AngularDistance(BodyQ));
						const float FaceMove = FMath::RadiansToDegrees(PrevFace.AngularDistance(FaceQ));
						const float Ratio = BodyMove > 0.5f ? (FaceMove / BodyMove) : -1.0f;
						UE_LOG(LogArdyConsole, Log,
							TEXT("   → 지난 호출 대비 **Body 머리 %.1f도 움직임 · Face 머리 %.1f도** (비율 %s) %s"),
							BodyMove, FaceMove,
							Ratio >= 0.0f ? *FString::Printf(TEXT("%.2f"), Ratio) : TEXT("—"),
							(Ratio < 0.0f) ? TEXT("(몸이 안 움직였다 — 판정 불가)")
								: (Ratio > 0.7f ? TEXT("**같이 움직인다**")
									: TEXT("★ **Face 가 덜 따라온다**")));
					}
					PrevBody = BodyQ;
					PrevFace = FaceQ;
					bHavePrev = true;

					UE_LOG(LogArdyConsole, Log,
						TEXT("   (참고) 월드 회전 절대차 %.1f도 · 위치 차 %.1fcm — 절대차는 rest 방향 차이가 섞여 있어 판정 근거가 아니다"),
						AngleDeg, PosDist);

					// 얼굴 AnimBP 가 몸을 따라올 **경로를 갖고 있는지** 본다.
					// MetaHuman 표준은 얼굴 애님그래프 안의 `Copy Pose From Mesh`(소스=Body)다.
					// 그 노드가 있어도 **소스 컴포넌트가 안 물려 있으면** 아무 일도 안 한다 —
					// 그 경우를 구분해야 고칠 곳을 안다(그래프 수술이냐, 변수 바인딩이냐).
					if (UAnimInstance* FaceAnim = FaceMesh->GetAnimInstance())
					{
						UClass* AnimClass = FaceAnim->GetClass();
						UE_LOG(LogArdyConsole, Log, TEXT("   Face AnimBP = %s"), *AnimClass->GetName());
						int32 MeshProps = 0;
						for (TFieldIterator<FObjectProperty> PropIt(AnimClass); PropIt; ++PropIt)
						{
							FObjectProperty* Prop = *PropIt;
							if (!Prop->PropertyClass
								|| !Prop->PropertyClass->IsChildOf(USkeletalMeshComponent::StaticClass()))
							{
								continue;
							}
							UObject* Val = Prop->GetObjectPropertyValue_InContainer(FaceAnim);
							++MeshProps;
							UE_LOG(LogArdyConsole, Log,
								TEXT("      메시 변수 '%s' = %s"),
								*Prop->GetName(),
								Val ? *Val->GetName() : TEXT("**None (안 물려 있다)**"));
						}
						if (MeshProps == 0)
						{
							UE_LOG(LogArdyConsole, Log,
								TEXT("      메시 변수 없음 — Copy Pose From Mesh 소스를 담을 변수 자체가 없다"));
						}
					}
				}
			}));

	FAutoConsoleCommand GArdyBodyProbe(
		TEXT("earl.ArdyBodyProbe"),
		TEXT("Measure the RETARGETED MetaHuman body (this is the judgment, not the source)"),
		FConsoleCommandDelegate::CreateStatic(
			[]()
			{
				UWorld* World = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
				if (!World)
				{
					return;
				}
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (It->ActorHasTag(FName(TEXT("ArdyPreview"))))
					{
						continue;
					}
					TArray<USkeletalMeshComponent*> Meshes;
					It->GetComponents(Meshes);
					for (USkeletalMeshComponent* Mesh : Meshes)
					{
						const FString MeshName = Mesh->GetName();
						if (!MeshName.Contains(TEXT("Body")) || MeshName.Contains(TEXT("Face")))
						{
							continue;
						}
						auto Loc = [Mesh](const TCHAR* Bone)
						{
							return Mesh->GetBoneIndex(FName(Bone)) != INDEX_NONE
								? Mesh->GetBoneLocation(FName(Bone)) : FVector::ZeroVector;
						};
						const FVector Pelvis = Loc(TEXT("pelvis"));
						const FVector Head   = Loc(TEXT("head"));
						const FVector FootL  = Loc(TEXT("foot_l"));
						const FVector HandL  = Loc(TEXT("hand_l"));
						const FVector HandR  = Loc(TEXT("hand_r"));

						const float HeadUp = Head.Z - Pelvis.Z;
						const float FootDown = FootL.Z - Pelvis.Z;

						// ⚠️ 임계값 주의 — 처음엔 HeadUp > 50 으로 뒀다가 오진할 뻔했다.
						// ARDY docent_point 는 **전방 숙임이 평균 42.8°** 다(22개 샘플 전수,
						// 맥 세션 실측). 62 × cos(42.8°) ≈ 45 이므로 정상인데도 50 을 못 넘는다.
						// 그래서 판정은 세 축으로 본다:
						//   ① 골반이 제 높이에 있는가 (바닥에 가라앉지 않았는가)
						//   ② 발이 골반 아래인가
						//   ③ 머리가 골반 위인가 (숙임 때문에 여유 있게 잡는다)
						const bool bPelvisOk = (Pelvis.Z > 60.0f);
						const bool bStanding = bPelvisOk && (HeadUp > 25.0f) && (FootDown < -40.0f);

						UE_LOG(LogArdyConsole, Log,
							TEXT("earl.ArdyBodyProbe [%s/%s] 프리셋 %s"),
							*It->GetName(), *MeshName,
							GActiveSource.IsValid()
								? FArdyLiveLinkSource::AxisFixToString(GActiveSource->GetAxisFix())
								: TEXT("(없음)"));
						// ★ **앞뒤 기울기**를 숫자로 낸다 (2026-08-07).
						// 정면 캡처로는 기울임이 안 보인다 — 그 각도로 "고쳐졌다"고 한 번
						// 잘못 판정했다. 골반→머리 벡터가 수직에서 몇 도 벗어났는지 잰다.
						const FVector Spine = Head - Pelvis;
						const float Horiz = FMath::Sqrt(Spine.X * Spine.X + Spine.Y * Spine.Y);
						const float LeanDeg = FMath::RadiansToDegrees(
							FMath::Atan2(Horiz, FMath::Max(Spine.Z, 1.0f)));

						UE_LOG(LogArdyConsole, Log,
							TEXT("   pelvis %s (높이 %.0f) · head Δ%+.0f · foot Δ%+.0f · **기울기 %.0f도** (앞뒤 %+.0f, 좌우 %+.0f)  → %s"),
							*Pelvis.ToCompactString(), Pelvis.Z, HeadUp, FootDown,
							LeanDeg, Spine.X, Spine.Y,
							bStanding ? TEXT("**서 있다**")
								: (!bPelvisOk ? TEXT("가라앉음(루트 높이 문제)") : TEXT("누움/뒤집힘")));
						UE_LOG(LogArdyConsole, Log,
							TEXT("   hand_l %s · hand_r %s"),
							*HandL.ToCompactString(), *HandR.ToCompactString());

						// ★ **어깨 처짐**을 숫자로 낸다 (2026-08-09).
						// 신고: "동작할 때 어깨가 너무 내려가 목이 엄청 긴 괴물처럼 보인다".
						// 목 길이는 눈에 보이는 결과이고 원인은 쇄골이다 — 쇄골이 아래로 돌면
						// 어깨가 내려앉아 목이 길어 보인다. 그래서 **쇄골 높이**를 직접 잰다.
						// 기준은 레퍼런스 포즈 대비가 아니라 **목뼈 기준 상대 높이**다
						// (골반 기준으로 재면 몸통 숙임에 오염된다).
						const FVector Neck   = Loc(TEXT("neck_01"));
						const FVector ClavL  = Loc(TEXT("clavicle_l"));
						const FVector ClavR  = Loc(TEXT("clavicle_r"));
						const FVector UpArmL = Loc(TEXT("upperarm_l"));
						const FVector UpArmR = Loc(TEXT("upperarm_r"));
						if (!Neck.IsNearlyZero())
						{
							// 어깨(위팔 뿌리)가 목뼈보다 얼마나 아래인가. 값이 커질수록 처진 것이다.
							const float DropL = Neck.Z - UpArmL.Z;
							const float DropR = Neck.Z - UpArmR.Z;
							// 보이는 "목 길이" = 목뼈에서 머리까지 + 어깨가 내려간 만큼
							const float NeckVisible = (Head.Z - Neck.Z) + 0.5f * (DropL + DropR);
							UE_LOG(LogArdyConsole, Log,
								TEXT("   **어깨 처짐** L %.1f · R %.1f (목뼈 기준, 클수록 처짐) · 보이는 목길이 %.1f · 쇄골Z L %.1f R %.1f"),
								DropL, DropR, NeckVisible, ClavL.Z - Neck.Z, ClavR.Z - Neck.Z);
						}
					}
				}
			}));

#if WITH_EDITOR
	// 리타게터의 소스 retarget pose 를 런타임에 바꾼다 — "돌려보고 고치기" 루프의 손잡이.
	FAutoConsoleCommand GArdyRetargetPose(
		TEXT("earl.ArdyRetargetPose"),
		TEXT("Switch the source retarget pose: earl.ArdyRetargetPose <name>  (예: Ours_None / Ours_ChainToChain / Ours_MeshToMesh)"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				UIKRetargeter* Rtg = LoadObject<UIKRetargeter>(nullptr,
					TEXT("/Game/Retarget/RTG_ARDY_to_Character.RTG_ARDY_to_Character"));
				if (!Rtg)
				{
					UE_LOG(LogArdyConsole, Error, TEXT("earl.ArdyRetargetPose: 우리 리타게터를 못 찾았다"));
					return;
				}
				UIKRetargeterController* Ctrl = UIKRetargeterController::GetController(Rtg);
				if (!Ctrl)
				{
					return;
				}
				if (Args.Num() == 0)
				{
					UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyRetargetPose: 현재 %s"),
						*Ctrl->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Source).ToString());
					return;
				}
				Ctrl->SetCurrentRetargetPose(FName(*Args[0]), ERetargetSourceOrTarget::Source);
				UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyRetargetPose -> %s"), *Args[0]);
			}));
#endif // WITH_EDITOR

	// ★ 회전 해석 전환 — 절대 vs 델타. 1차 스윕이 18조합 전부 실패해서 만든 손잡이다.
	// CDO 를 바꾸므로 다음에 만들어지는 리맵 인스턴스부터 적용된다 →
	// 바꾼 뒤 `earl.ArdyRetarget 1 ours` 를 다시 걸어야 반영된다.
	FAutoConsoleCommand GArdyDelta(
		TEXT("earl.ArdyDelta"),
		TEXT("Interpret incoming rotations as deltas on the reference pose: earl.ArdyDelta <0|1>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				UArdyRotationOnlyRemapAsset* CDO =
					GetMutableDefault<UArdyRotationOnlyRemapAsset>();
				if (!CDO)
				{
					return;
				}
				if (Args.Num() > 0)
				{
					CDO->bTreatRotationAsDelta = (FCString::Atoi(*Args[0]) != 0);
				}
				if (Args.Num() > 1)
				{
					CDO->bUseRefFrameCorrection = (FCString::Atoi(*Args[1]) != 0);
				}
				if (Args.Num() > 2)
				{
					CDO->HeadBlockMode = FCString::Atoi(*Args[2]);
				}
				// 네 번째 인자 = 머리가 ARDY 를 따라가는 비율 (2026-08-09).
				// 0 이면 예전처럼 완전 차단이라 **머리통이 따로 논다.**
				if (Args.Num() > 3)
				{
					CDO->HeadArdyWeight = FMath::Clamp(FCString::Atof(*Args[3]), 0.0f, 1.0f);
				}
				UE_LOG(LogArdyConsole, Log,
					TEXT("earl.ArdyDelta: 해석=%s · ref보정=%s · 목/머리차단=%s · **머리 추종 %.2f** "
						 "(바꾼 즉시 반영된다 — 리타깃 재적용 불필요)"),
					CDO->bTreatRotationAsDelta ? TEXT("델타") : TEXT("절대"),
					CDO->bUseRefFrameCorrection ? TEXT("켬") : TEXT("끔"),
					CDO->HeadBlockMode == 0 ? TEXT("없음")
						: (CDO->HeadBlockMode == 1 ? TEXT("Head 부분반영(권장)") : TEXT("Neck+Head")),
					CDO->HeadArdyWeight);
			}));

	// ★ 캡션을 서버로 올린다 — 프롬프트 엔지니어링 루프의 손잡이 (2026-08-06).
	//
	//   earl.ArdyCaption a person points to the side while explaining
	//
	// ⚠️ **캡션은 `a person …` 으로 시작해야 한다.** ARDY 학습 프롬프트가 전부 그 형식이다
	//    (docs/content/ARDY-프롬프트-워크플로-참고.md §1). 현재형·`then` 으로 순차 연결·
	//    부사로 태도(alertly / defensively).
	//
	// ⚠️ 실측: 지금 4090 은 **replay 송신만** 하므로 이 줄을 보내도 무시된다(연결은 유지).
	//    서버가 `make serve` + 캡션 핸들러를 붙이는 순간 바로 동작한다.
	//    이건 **수동 실험용**이다 — 실제 운용에서는 페르소나 백엔드가 문장마다 캡션을
	//    같이 내보내는 것이 맞다(ADR 005 문장 페이로드에 emotion 이 있는 것과 같은 자리).
	FAutoConsoleCommand GArdyCaption(
		TEXT("earl.ArdyCaption"),
		TEXT("Send a motion caption to the ARDY server: earl.ArdyCaption a person <does something>"),
		FConsoleCommandWithArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args)
			{
				if (!GActiveSource.IsValid())
				{
					UE_LOG(LogArdyConsole, Warning, TEXT("earl.ArdyCaption: 활성 소스가 없다 — 먼저 earl.Ardy"));
					return;
				}
				if (Args.Num() == 0)
				{
					UE_LOG(LogArdyConsole, Log,
						TEXT("earl.ArdyCaption: 사용법 — earl.ArdyCaption a person points to the side while explaining"));
					return;
				}
				const FString Caption = FString::Join(Args, TEXT(" "));
				if (!Caption.StartsWith(TEXT("a person"), ESearchCase::IgnoreCase))
				{
					// 막지는 않는다 — 실험을 방해하면 안 된다. 다만 품질이 떨어질 수 있음을 알린다.
					UE_LOG(LogArdyConsole, Warning,
						TEXT("earl.ArdyCaption: 캡션이 'a person' 으로 시작하지 않는다 — ARDY 학습 형식과 달라 품질이 떨어질 수 있다"));
				}

				// 줄 단위 JSON. 서버가 아직 안 받으면 조용히 무시된다(연결은 유지됨 — 실측).
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("type"), TEXT("caption"));
				Obj->SetStringField(TEXT("text"), Caption);
				// ⚠️ **Condensed 로 써야 한다.** 기본 writer 는 pretty-print 라 개행이 섞이고,
				// 줄 단위 프로토콜에서는 그 자체로 프레임이 쪼개진다(2026-08-06 실측:
				// "서버로 보냄(83바이트): {" 로 첫 줄만 나갔다).
				FString Json;
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
				FJsonSerializer::Serialize(Obj, Writer);

				GActiveSource->SendLine(Json);
				UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyCaption → \"%s\""), *Caption);
			}));

	FAutoConsoleCommand GArdyStop(
		TEXT("earl.ArdyStop"),
		TEXT("Remove the ArdyLiveLink source"),
		FConsoleCommandDelegate::CreateStatic(
			[]()
			{
				RemoveActiveSource();
				UE_LOG(LogArdyConsole, Log, TEXT("earl.ArdyStop: 제거함"));
			}));
}

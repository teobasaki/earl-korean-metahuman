#include "LipSync/EarlLipSyncComponent.h"
#include "Core/EarlConsoleWorld.h"
#include "LiveLinkInstance.h"
#include "LipSync/EarlLipSyncSource.h"
#include "LipSync/EarlSolverLiveLinkSource.h"
#include "Core/EarlOneGameInstance.h"
#include "EarlOne.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
// earl.LipBaseline 이 표정 층까지 함께 0 으로 되돌리기 위해 필요하다
#include "Animation/EarlEmotionComponent.h"

namespace
{
	// 머리 움직임 라이브 튜닝: `earl.Head <진폭도> [속도]`
	// 재시작 없이 눈으로 보며 맞추기 위한 것 — 2.5도는 계측기엔 잡혀도 사람 눈엔 안 보였다
	// (2026-08-05 "머리 움직임 안 들어갔다"). 값이 정해지면 DefaultGame.ini 에 적는다.
	static FAutoConsoleCommandWithWorldAndArgs GEarlHeadCmd(
		TEXT("earl.Head"),
		TEXT("Tune procedural head motion live: earl.Head <amplitudeDeg> [speed]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (!World || Args.Num() < 1)
				{
					return;
				}
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (UEarlLipSyncComponent* LipSync = It->FindComponentByClass<UEarlLipSyncComponent>())
					{
						const float Amp = FCString::Atof(*Args[0]);
						const float Speed = Args.Num() > 1 ? FCString::Atof(*Args[1]) : LipSync->HeadMotionSpeed;
						LipSync->ApplyHeadMotionSettings(Amp, Speed);
					}
				}
			}));

	// 몸 추종 라이브 튜닝: `earl.BodyFollow <게인> [yaw부호] [pitch부호] [roll부호]`
	// 부호·축 대응은 뼈 축 관례라 실측으로만 맞출 수 있다 — 재시작 없이 뒤집어 보기 위한 것.
	static FAutoConsoleCommandWithWorldAndArgs GEarlBodyFollowCmd(
		TEXT("earl.BodyFollow"),
		TEXT("Face follows Body head: earl.BodyFollow <gain> [yawSign] [pitchSign] [rollSign]  (gain 0 = off)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (!World || Args.Num() < 1)
				{
					return;
				}
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (UEarlLipSyncComponent* LipSync = It->FindComponentByClass<UEarlLipSyncComponent>())
					{
						LipSync->BodyHeadFollowGain = FCString::Atof(*Args[0]);
						LipSync->bBodyHeadFollow = LipSync->BodyHeadFollowGain > 0.0f;
						if (Args.Num() > 1) { LipSync->BodyFollowYawSign = FCString::Atof(*Args[1]); }
						if (Args.Num() > 2) { LipSync->BodyFollowPitchSign = FCString::Atof(*Args[2]); }
						if (Args.Num() > 3) { LipSync->BodyFollowRollSign = FCString::Atof(*Args[3]); }
						UE_LOG(LogEarlOne, Log,
							TEXT("earl.BodyFollow: 게인 %.2f · 부호 Y%+.0f P%+.0f R%+.0f"),
							LipSync->BodyHeadFollowGain, LipSync->BodyFollowYawSign,
							LipSync->BodyFollowPitchSign, LipSync->BodyFollowRollSign);
					}
				}
			}));

	// 시선 라이브 튜닝: `earl.Gaze <진폭> [최소초] [최대초]` (진폭 0 = 끔)
	static FAutoConsoleCommandWithWorldAndArgs GEarlGazeCmd(
		TEXT("earl.Gaze"),
		TEXT("Tune procedural gaze live: earl.Gaze <amplitude> [minSec] [maxSec]  (0 = off)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (!World || Args.Num() < 1)
				{
					return;
				}
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (UEarlLipSyncComponent* LipSync = It->FindComponentByClass<UEarlLipSyncComponent>())
					{
						LipSync->ApplyGazeSettings(
							FCString::Atof(*Args[0]),
							Args.Num() > 1 ? FCString::Atof(*Args[1]) : LipSync->GazeSaccadeMinSec,
							Args.Num() > 2 ? FCString::Atof(*Args[2]) : LipSync->GazeSaccadeMaxSec);
					}
				}
			}));

	// 강세 라이브 튜닝: `earl.Accent <눈썹게인> [끄덕임도] [임계비율]` (게인 0 = 끔)
	// 발동 횟수가 로그에 같이 나오므로, 문장 하나에 몇 번 걸리는지 보고 임계를 조절한다.
	static FAutoConsoleCommandWithWorldAndArgs GEarlAccentCmd(
		TEXT("earl.Accent"),
		TEXT("Tune speech accent live: earl.Accent <browGain> [nodDeg] [thresholdRatio]  (0 = off)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				if (!World || Args.Num() < 1)
				{
					return;
				}
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (UEarlLipSyncComponent* LipSync = It->FindComponentByClass<UEarlLipSyncComponent>())
					{
						LipSync->ApplyAccentSettings(
							FCString::Atof(*Args[0]),
							Args.Num() > 1 ? FCString::Atof(*Args[1]) : LipSync->HeadNodDeg,
							Args.Num() > 2 ? FCString::Atof(*Args[2]) : LipSync->AccentThresholdRatio);
					}
				}
			}));

	// ★★ 발화 기준(베이스라인) 복귀: `earl.LipBaseline`
	//
	// 왜 있나: 2026-08-05 하루에 립싱크 인접 층을 11번 건드렸고, 매번 그 층만 검증하고 넘겨
	// **"입이 소리에 맞는가"를 한 번도 확인하지 않았다.** 그래서 같은 보고("둔하다"·"안 맞는다")가
	// 네 번 반복됐다. 사용자 제안이 정확했다 — **기준을 못 박아 두면 매번 표류하지 않는다.**
	//
	// 이 명령은 **발화 구간에 입 주변을 건드리는 모든 층을 0 으로** 돌린다 = ADR 011 로 확정한
	// 립싱크 단독 상태. 표정·시선·머리는 그대로 둔다(조음에 닿지 않는 것이 계측으로 확인됐다).
	// 무엇을 바꾸든 이 한 명령이면 기준으로 돌아올 수 있어야 한다.
	static FAutoConsoleCommandWithWorldAndArgs GEarlLipBaselineCmd(
		TEXT("earl.LipBaseline"),
		TEXT("Restore the speech baseline: nothing touches the mouth while speaking (ADR 011 state)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				World = EarlResolveConsoleWorld(World); // 픽셀 스트리밍 원격 경로 폴백
				if (!World)
				{
					return;
				}
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (UEarlEmotionComponent* Emotion = It->FindComponentByClass<UEarlEmotionComponent>())
					{
						// 입 주변만 끈다. 이마·눈근육은 입에서 멀어 조음을 방해하지 않으므로 남긴다 —
						// 기준으로 돌아가도 얼굴이 죽지 않아야 한다.
						Emotion->MouthExpressionSpeakingScale = 0.0f;   // 입가 표정 끔
						Emotion->CheekSpeakingBoost = 0.0f;             // 광대도 발화 중엔 끔
						UE_LOG(LogEarlOne, Log,
							TEXT("earl.LipBaseline: 발화 중 입 주변(입가·광대) 0 — 립싱크 단독(ADR 011 기준). "
								 "이마 x%.2f · 눈근육 x%.2f 는 유지(입에서 멀다). 시선·머리·깜빡임 그대로."),
							Emotion->BrowSpeakingBoost, Emotion->EyeMuscleSpeakingBoost);
					}
					if (UEarlLipSyncComponent* LipSync = It->FindComponentByClass<UEarlLipSyncComponent>())
					{
						LipSync->ReportArticulation(true);   // 계측 리셋 — 바로 재측정할 수 있게
					}
				}
			}));

	// ★ 조음 계측: `earl.LipMeter [reset]`
	// "립싱크가 둔해졌다"가 **네 번째**라 만들었다. 화면 크롭으로는 머리 움직임·프레이밍·자막이
	// 전부 섞여 값이 비단조로 나온다(T72) — 실제로 광대 세기 스윕이 1.9→3.67, 1.0→1.93, 0→2.09
	// 로 뒤죽박죽이었다. 여기서는 LiveLink 로 나가기 직전의 **조음 커브 자체**를 잰다.
	// 사용: `earl.LipMeter reset` → 말 시킴 → `earl.LipMeter` 로 평균·피크 확인.
	static FAutoConsoleCommandWithWorldAndArgs GEarlLipMeterCmd(
		TEXT("earl.LipMeter"),
		TEXT("Measure articulation at the curve level: earl.LipMeter [reset]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				// 픽셀 스트리밍 경로(emitConsoleCommand)는 월드 컨텍스트 없이 실행돼
				// World 가 null 로 온다 — 게임 월드로 폴백해야 원격 계측이 된다
				if (!World && GEngine)
				{
					for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
					{
						if (Ctx.World() && (Ctx.WorldType == EWorldType::Game || Ctx.WorldType == EWorldType::PIE))
						{
							World = Ctx.World();
							break;
						}
					}
				}
				if (!World)
				{
					return;
				}
				const bool bReset = Args.Num() > 0 && Args[0].Equals(TEXT("reset"), ESearchCase::IgnoreCase);
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					UEarlLipSyncComponent* LipSync = It->FindComponentByClass<UEarlLipSyncComponent>();
					if (!LipSync)
					{
						continue;
					}
					LipSync->ReportArticulation(bReset);
				}
			}));

	// 얼굴 배선 진단: `earl.FaceProbe`
	// 머리·표정이 "안 보인다"는 보고가 오면 **어디서 끊겼는지**를 추측하지 않기 위한 계측기다.
	// 커브가 0 이면 LiveLink→ABP 구간, 커브는 살아 있는데 머리뼈가 안 움직이면 포스트프로세스 구간이다.
	static FAutoConsoleCommandWithWorldAndArgs GEarlFaceProbeCmd(
		TEXT("earl.FaceProbe"),
		TEXT("Log face mesh wiring: post-process ABP, head curves, head bone rotation"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				World = EarlResolveConsoleWorld(World); // 픽셀 스트리밍 원격 경로 폴백
				if (!World)
				{
					return;
				}
				int32 Found = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					TArray<USkeletalMeshComponent*> Meshes;
					It->GetComponents(Meshes);
					for (USkeletalMeshComponent* Mesh : Meshes)
					{
						if (!Mesh || !Mesh->GetName().Contains(TEXT("Face")))
						{
							continue;
						}
						++Found;
						UAnimInstance* Anim = Mesh->GetAnimInstance();
						UAnimInstance* Post = Mesh->GetPostProcessInstance();
						const int32 HeadIndex = Mesh->GetBoneIndex(TEXT("head"));
						const FRotator HeadRot = HeadIndex != INDEX_NONE
							? Mesh->GetBoneTransform(HeadIndex, FTransform::Identity).Rotator()
							: FRotator::ZeroRotator;

						UE_LOG(LogEarlOne, Log,
							TEXT("FaceProbe [%s/%s]: anim=%s post=%s"),
							*It->GetName(), *Mesh->GetName(),
							Anim ? *Anim->GetClass()->GetName() : TEXT("none"),
							Post ? *Post->GetClass()->GetName() : TEXT("NONE (머리 IK 리그가 안 돈다)"));

						// ★ "FPS 는 높은데 입이 둔하다"의 남은 후보 (2026-08-05).
						// 렌더 FPS 와 **얼굴이 실제로 평가되는 빈도**는 다르다:
						//  · URO(Update Rate Optimization) = 스켈레탈 메시를 N 프레임마다 한 번만
						//    평가하고 사이를 보간한다 → 입이 계단처럼 뭉개진다
						//  · 얼굴 LOD = MetaHuman 은 LOD1 부터 리그가 급격히 단순해진다 → 조음이 뭉개진다
						//  · VisibilityBasedAnimTick = 화면 밖·가려짐 판정에 따라 틱을 줄인다
						const FAnimUpdateRateParameters* URO = Mesh->AnimUpdateRateParams;
						UE_LOG(LogEarlOne, Log,
							TEXT("FaceProbe 갱신: LOD=%d(강제 %d, 최소 %d) · URO=%s"
								 "%s · VisibilityTick=%d · 컴포넌트틱=%s"),
							Mesh->GetPredictedLODLevel(), Mesh->GetForcedLOD(), Mesh->MinLodModel,
							Mesh->bEnableUpdateRateOptimizations ? TEXT("켜짐") : TEXT("꺼짐"),
							URO ? *FString::Printf(TEXT(" (평가 %d프레임마다, 보간 %s)"),
								URO->UpdateRate, URO->bInterpolateSkippedFrames ? TEXT("함") : TEXT("안함"))
								: TEXT(""),
							static_cast<int32>(Mesh->VisibilityBasedAnimTickOption),
							Mesh->IsComponentTickEnabled() ? TEXT("켜짐") : TEXT("꺼짐"));

						if (Anim)
						{
							static const TCHAR* Probe[] = {
								TEXT("HeadControlSwitch"), TEXT("HeadYaw"), TEXT("HeadPitch"), TEXT("HeadRoll"),
								TEXT("CTRL_expressions_browRaiseInL"), TEXT("CTRL_expressions_mouthCornerPullL"),
								TEXT("CTRL_expressions_jawOpen"),
								// 시선 — GUI `CTRL_{L,R}_eye.t{x,y}` 가 이 raw 이름들로 펼쳐진다.
								// 넷이 모두 0 이면 시선 층이 얼굴에 안 닿고 있는 것이다.
								TEXT("CTRL_expressions_eyeLookLeftL"), TEXT("CTRL_expressions_eyeLookRightL"),
								TEXT("CTRL_expressions_eyeLookUpL"), TEXT("CTRL_expressions_eyeLookDownL"),
								// 문장 프로파일·아이들 몸짓이 실제로 닿는지 (3~6번 검증용).
								// ⚠️ GUI `CTRL_L_eye_eyelidU.ty` 는 `eyeWidenL` 이 **아니라**
								//    `eyeUpperLidUpL` 로 펼쳐진다 — 처음에 이름을 잘못 짚어 0 만 봤다.
								TEXT("CTRL_expressions_eyeUpperLidUpL"), TEXT("CTRL_expressions_eyeWidenL"),
								TEXT("CTRL_expressions_eyePupilWideL"), TEXT("CTRL_expressions_neckSwallowPh1")
							};
							FString Line;
							for (const TCHAR* Name : Probe)
							{
								Line += FString::Printf(TEXT("%s=%.3f "), Name, Anim->GetCurveValue(FName(Name)));
							}
							UE_LOG(LogEarlOne, Log, TEXT("FaceProbe curves: %s"), *Line);
						}
						UE_LOG(LogEarlOne, Log, TEXT("FaceProbe head bone: idx=%d rot=(P%.2f Y%.2f R%.2f)"),
							HeadIndex, HeadRot.Pitch, HeadRot.Yaw, HeadRot.Roll);
					}
				}
				if (Found == 0)
				{
					UE_LOG(LogEarlOne, Warning, TEXT("FaceProbe: 이름에 'Face' 가 든 스켈레탈 메시를 못 찾았다"));
				}
			}));

	// 인터리브 int16 PCM → 모노 float (원음 샘플레이트 유지)
	TArray<float> ConvertToMonoFloat(const TArray<uint8>& PCMBytes, int32 NumChannels)
	{
		const int32 NumSamples = PCMBytes.Num() / 2;
		const int16* Samples = reinterpret_cast<const int16*>(PCMBytes.GetData());
		const int32 FramesIn = NumSamples / FMath::Max(1, NumChannels);
		if (FramesIn <= 0)
		{
			return {};
		}

		TArray<float> Mono;
		Mono.SetNumUninitialized(FramesIn);
		for (int32 Frame = 0; Frame < FramesIn; ++Frame)
		{
			float Sum = 0.0f;
			for (int32 Ch = 0; Ch < NumChannels; ++Ch)
			{
				Sum += static_cast<float>(Samples[Frame * NumChannels + Ch]) / 32768.0f;
			}
			Mono[Frame] = Sum / static_cast<float>(NumChannels);
		}
		return Mono;
	}

	// 선형 리샘플 → 16k. ⚠️ 안티앨리어싱 없음 — 절차적 폴백(RMS/저역 분석) 전용.
	// 솔버에는 원음을 그대로 줘야 자음(치찰음) 비짐이 산다.
	TArray<float> ResampleTo16k(const TArray<float>& Mono, int32 SampleRate)
	{
		const int32 FramesIn = Mono.Num();
		if (FramesIn <= 0 || SampleRate <= 0)
		{
			return {};
		}
		if (SampleRate == FEarlLipSyncSource::SampleRate)
		{
			return Mono;
		}

		const double Ratio = static_cast<double>(SampleRate) / static_cast<double>(FEarlLipSyncSource::SampleRate);
		const int32 FramesOut = FMath::Max(1, FMath::FloorToInt(FramesIn / Ratio));
		TArray<float> Out;
		Out.SetNumUninitialized(FramesOut);
		for (int32 Index = 0; Index < FramesOut; ++Index)
		{
			const double SrcPos = Index * Ratio;
			const int32 Src0 = FMath::Clamp(static_cast<int32>(SrcPos), 0, FramesIn - 1);
			const int32 Src1 = FMath::Min(Src0 + 1, FramesIn - 1);
			const float Alpha = static_cast<float>(SrcPos - Src0);
			Out[Index] = FMath::Lerp(Mono[Src0], Mono[Src1], Alpha);
		}
		return Out;
	}
}

UEarlLipSyncComponent::UEarlLipSyncComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UEarlLipSyncComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		UE_LOG(LogEarlOne, Error, TEXT("Live Link client unavailable; lipsync subject not created"));
		return;
	}
	ILiveLinkClient& LiveLinkClient =
		IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

	if (Backend == EEarlLipSyncBackend::KoreanViseme)
	{
		// 자체 한국어 비짐 단독: 텍스트(문장 시계)+정렬로 커브 트랙을 만들어 순수 소스로 발행
		VisemeSource = MakeShared<FEarlLipSyncSource>();
		LiveLinkClient.AddSource(VisemeSource);
		VisemeSource->CreateSubject(LipSyncSubjectName);
		UE_LOG(LogEarlOne, Log, TEXT("Created Live Link subject %s (korean viseme source)"), *LipSyncSubjectName.ToString());
	}
	if (Backend == EEarlLipSyncBackend::Hybrid)
	{
		// 하이브리드: 솔버(아래)가 풀페이스+턱, 비짐은 입 모양 커브만 Earl_Viseme 로 덮어씀.
		// 턱을 빼는 이유 — 턱은 가장 강한 싱크 큐라 음향(솔버) 타이밍이 항상 정확하다.
		static const TArray<FName> ShapeOnly = {
			TEXT("CTRL_expressions_mouthUpperLipRaiseL"), TEXT("CTRL_expressions_mouthUpperLipRaiseR"),
			TEXT("CTRL_expressions_mouthLowerLipDepressL"), TEXT("CTRL_expressions_mouthLowerLipDepressR"),
			TEXT("CTRL_expressions_mouthCornerPullL"), TEXT("CTRL_expressions_mouthCornerPullR"),
			TEXT("CTRL_expressions_mouthStretchL"), TEXT("CTRL_expressions_mouthStretchR"),
			TEXT("CTRL_expressions_mouthLipsPurseUL"), TEXT("CTRL_expressions_mouthLipsPurseUR"),
			TEXT("CTRL_expressions_mouthLipsPurseDL"), TEXT("CTRL_expressions_mouthLipsPurseDR"),
			TEXT("CTRL_expressions_mouthFunnelUL"), TEXT("CTRL_expressions_mouthFunnelUR"),
			TEXT("CTRL_expressions_mouthFunnelDL"), TEXT("CTRL_expressions_mouthFunnelDR"),
			TEXT("CTRL_expressions_mouthLipsTogetherUL"), TEXT("CTRL_expressions_mouthLipsTogetherUR"),
			TEXT("CTRL_expressions_mouthLipsTogetherDL"), TEXT("CTRL_expressions_mouthLipsTogetherDR")
		};
		VisemeSource = MakeShared<FEarlLipSyncSource>();
		LiveLinkClient.AddSource(VisemeSource);
		VisemeSource->CreateSubject(TEXT("Earl_Viseme"), &ShapeOnly);
		UE_LOG(LogEarlOne, Log, TEXT("Created Live Link subject Earl_Viseme (hybrid shape overlay, %d curves)"), ShapeOnly.Num());
	}
	if (Backend != EEarlLipSyncBackend::KoreanViseme)
	{
		// MetaHuman NNE 오디오 솔버 (데스크톱 기본) / 절차적 폴백은
		// earl.LipSync.ForceProcedural=1
		Source = MakeShared<FEarlSolverLiveLinkSource>();
		LiveLinkClient.AddSource(Source);
		if (!Source->CreateEarlSubject(LipSyncSubjectName, LookaheadMs, MoodIntensity))
		{
			UE_LOG(LogEarlOne, Error, TEXT("Failed to create lipsync subject %s"), *LipSyncSubjectName.ToString());
			LiveLinkClient.RemoveSource(Source);
			Source.Reset();
			return;
		}
		UE_LOG(LogEarlOne, Log, TEXT("Created Live Link subject %s (solver source)"), *LipSyncSubjectName.ToString());
		// 머리 층은 솔버가 안 만드므로 여기서 켠다 (적용은 얼굴 포스트프로세스의 컨트롤 리그가 한다)
		Source->SetHeadMotion(HeadMotionAmplitudeDeg, HeadMotionSpeed);
		// ★ 입 닫기를 **처음부터** 켠다 (2026-08-05).
		// 예전엔 `HandlePlaybackFinished` 에서만 켜져서, **첫 발화 전까지 입이 벌어진 채**
		// 치아를 드러내고 서 있었다 — 쉬는 얼굴로는 부자연스럽다.
		// 시작 상태가 곧 무음이므로 여기서 켜는 것이 맞다. PCM 이 도착하면 즉시 풀린다.
		Source->SetIdleMouthDamp(true, IdleMouthDamp);
		Source->SetMouthGain(MouthCurveGain);
		Source->SetHeadNod(HeadNodDeg, HeadNodDurationSec);
		// 시작 상태는 **응시**다 — 도슨트는 관람객을 보고 서 있다.
		// 아무도 말을 걸지 않으면 이 창이 지나면서 자연스럽게 주변을 보기 시작한다.
		AttentionUntil = (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0) + GazeAttentionSec;
		// 솔버 경로의 기본 레이트는 44100 이다(ADR 011 ①). 16000 기본값을 그대로 두면
		// 첫 발화 전 무음만 16k 로 나가 레이트가 한 번 갈린다.
		StreamSampleRate = 44100;
	}

	if (UEarlOneGameInstance* GI = Cast<UEarlOneGameInstance>(GetOwner()->GetGameInstance()))
	{
		GI->OnGeneratePCMData.AddDynamic(this, &UEarlLipSyncComponent::HandlePCMData);
		GI->OnAudioPlaybackFinished.AddDynamic(this, &UEarlLipSyncComponent::HandlePlaybackFinished);
		// ⚠️ 예전엔 비짐 백엔드일 때만 구독했다. 이제는 **문장 프로파일**(감정·질문·설명·회상)이
		// 여기서 오므로 백엔드와 무관하게 항상 구독한다.
		GI->OnSentenceStarted.AddDynamic(this, &UEarlLipSyncComponent::HandleSentenceStarted);
	}
}

void UEarlLipSyncComponent::ApplyGazeSettings(float InAmplitude, float InMinSec, float InMaxSec)
{
	GazeAmplitude = FMath::Max(0.0f, InAmplitude);
	GazeSaccadeMinSec = FMath::Max(0.1f, InMinSec);
	GazeSaccadeMaxSec = FMath::Max(GazeSaccadeMinSec, InMaxSec);
	NextSaccadeAt = -1.0; // 다음 틱에 새 목표를 잡게 한다
	if (GazeAmplitude <= KINDA_SMALL_NUMBER && Source.IsValid())
	{
		// 끌 때는 눈을 중립으로 돌려놓는다 — 안 그러면 마지막 사케이드 위치에 굳는다
		GazeCurrent = FVector2D::ZeroVector;
		Source->SetProceduralCurves(TMap<FString, float>{});
	}
	UE_LOG(LogEarlOne, Log, TEXT("Gaze -> amplitude %.2f, saccade %.1f~%.1fs"),
		GazeAmplitude, GazeSaccadeMinSec, GazeSaccadeMaxSec);
}

void UEarlLipSyncComponent::ReportArticulation(bool bReset)
{
	if (!Source.IsValid())
	{
		UE_LOG(LogEarlOne, Warning, TEXT("earl.LipMeter: 솔버 소스 없음"));
		return;
	}
	if (bReset)
	{
		Source->ResetArticulation();
		UE_LOG(LogEarlOne, Log, TEXT("earl.LipMeter: 리셋 — 이제 말을 시키고 다시 찍어라"));
		return;
	}
	float Mean = 0.0f, Peak = 0.0f;
	int32 Samples = 0;
	Source->GetArticulation(Mean, Peak, Samples);
	UE_LOG(LogEarlOne, Log,
		TEXT("earl.LipMeter: 조음 평균 %.5f · 피크 %.5f (n=%d 프레임) "
			 "— 커브 레벨 측정이라 머리·카메라·자막이 섞이지 않는다"),
		Mean, Peak, Samples);
}

void UEarlLipSyncComponent::ApplyAccentSettings(float InBrowGain, float InNodDeg, float InThresholdRatio)
{
	AccentBrowGain = FMath::Max(0.0f, InBrowGain);
	HeadNodDeg = InNodDeg;
	AccentThresholdRatio = FMath::Max(1.0f, InThresholdRatio);
	if (Source.IsValid())
	{
		Source->SetHeadNod(HeadNodDeg, HeadNodDurationSec);
	}
	UE_LOG(LogEarlOne, Log,
		TEXT("Accent -> brow %.2f, nod %.1f deg, threshold x%.2f (지금까지 %d회 발동)"),
		AccentBrowGain, HeadNodDeg, AccentThresholdRatio, AccentCount);
}

void UEarlLipSyncComponent::UpdateSpeechAccent(const TArray<float>& InChunk, float DeltaTime)
{
	if (InChunk.IsEmpty())
	{
		return;
	}

	double SumSq = 0.0;
	for (const float Sample : InChunk)
	{
		SumSq += static_cast<double>(Sample) * Sample;
	}
	const float Rms = static_cast<float>(FMath::Sqrt(SumSq / InChunk.Num()));

	// 빠른 포락선 vs 느린 기준선. 비율로 판정하므로 **음량·목소리에 무관**하다 —
	// 절대 임계를 쓰면 TTS 를 바꿀 때마다 다시 맞춰야 한다(SAPI 22050 사건과 같은 함정).
	const float Dt = FMath::Max(DeltaTime, 0.001f);
	FastEnvelope = FMath::Lerp(FastEnvelope, Rms, 1.0f - FMath::Exp(-Dt / 0.045f));
	SlowEnvelope = FMath::Lerp(SlowEnvelope, Rms, 1.0f - FMath::Exp(-Dt / 0.60f));

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;

	const bool bLoudEnough  = FastEnvelope > AccentMinLevel;
	const bool bAboveBase   = FastEnvelope > SlowEnvelope * AccentThresholdRatio;
	const bool bCooledDown  = (Now - LastAccentAt) > AccentCooldownSec;

	if (bLoudEnough && bAboveBase && bCooledDown)
	{
		LastAccentAt = Now;
		AccentTimeRemaining = AccentDurationSec;
		// 세기는 기준선 대비 초과분에서 온다 — 살짝 센 강세와 크게 센 강세가 달라 보인다
		const float Excess = SlowEnvelope > KINDA_SMALL_NUMBER
			? FastEnvelope / SlowEnvelope - AccentThresholdRatio
			: 1.0f;
		AccentStrength = FMath::Clamp(0.55f + Excess * 0.6f, 0.0f, 1.0f);
		++AccentCount;

		// 머리 끄덕임은 얼굴 커브가 아니라 포즈라 솔버 쪽으로 따로 보낸다
		if (HeadNodDeg != 0.0f)
		{
			Source->TriggerHeadNod(AccentStrength);
		}

		if (!bLoggedAccent)
		{
			bLoggedAccent = true;
			UE_LOG(LogEarlOne, Log,
				TEXT("Speech accent on: fast=%.4f slow=%.4f ratio=%.2f strength=%.2f "
					 "(진폭 포락선 기반 — 단어 타임스탬프 없이 강세를 잡는다)"),
				FastEnvelope, SlowEnvelope,
				SlowEnvelope > 0 ? FastEnvelope / SlowEnvelope : 0.0f, AccentStrength);
		}
	}
}

void UEarlLipSyncComponent::ApplySentenceProfile(const FEarlSentencePayload& Sentence)
{
	// ── 감정별 시선·머리 성향 (행동목록 §4) ───────────────────────────────
	// 지금까지 감정은 **표정 클립만** 바꿨다 — 시선과 머리는 감정과 무관하게 똑같았다.
	// 여기서 갈라 두면 클립이 같아도(wry/playful 이 실제로 공유한다) 다르게 보인다.
	// 즉 **에셋을 기다리지 않고 감정 종류를 늘릴 수 있는 길**이기도 하다.
	FEarlExpressionProfile P;
	switch (Sentence.Emotion)
	{
	case EEarlEmotion::Warm:
		P.RollDeg = 2.0f;                 // 살짝 갸웃 — 다정함은 대칭이 아니다
		break;
	case EEarlEmotion::Playful:
		P.RollDeg = 3.0f;
		P.GazeScaleMul = 1.35f;           // 눈이 더 많이 돌아다닌다
		break;
	case EEarlEmotion::Passionate:
		P.EyeWiden = 0.35f;               // 눈이 커진다
		P.PitchDeg = -2.0f;               // 머리를 앞으로 — 다가서는 자세
		break;
	case EEarlEmotion::Nostalgic:
		// ★ "상기할 때 오른쪽 위를 본다" — 기억 접근의 전형적 신호
		P.GazeAnchor = FVector2D(0.38f, 0.30f);
		P.GazeAnchorSec = 1.8f;
		P.PitchDeg = 1.2f;                // 살짝 뒤로
		P.EyeWiden = -0.10f;              // 눈이 조금 가늘어진다
		break;
	case EEarlEmotion::Wry:
		P.RollDeg = -3.0f;
		P.GazeScaleMul = 0.7f;            // 곁눈질, 크게 안 돈다
		break;
	default:
		break;
	}

	// ── 문장 종류 (행동목록 §2 B3·B4) ─────────────────────────────────────
	// 질문이면 고개를 갸웃하고 눈을 조금 뜬다. 텍스트 파싱만으로 되는 값싼 신호다.
	if (Sentence.Text.Contains(TEXT("?")) || Sentence.Text.Contains(TEXT("？")))
	{
		P.RollDeg += 2.5f;
		P.EyeWiden += 0.15f;
	}
	// 설명(사실 전달)이면 눈이 커진다 — "개념에 대해 설명해줄 때 눈이 커지기**도** 하며".
	// ADR 005 의 mode 는 Fact(사실) / Interpretation(해석) 두 갈래인데, **도슨트 답변의 대부분이
	// Fact 다.** 매 문장 눈을 뜨면 그건 특징이 아니라 기본 표정이 되어 버린다 — 사용자 표현도
	// "커지기도 하며" 였다. 그래서 확률적으로만 건다.
	if (Sentence.Mode == EEarlPersonaMode::Fact && FMath::FRand() < ExplainWidenChance)
	{
		P.EyeWiden += 0.25f;
	}

	// 강도는 전체에 곱한다 — 약한 감정이 센 감정과 같은 자세를 취하면 안 된다
	const float Intensity = FMath::Clamp(Sentence.Intensity, 0.0f, 1.0f);
	P.EyeWiden *= Intensity;
	P.RollDeg *= Intensity;
	P.PitchDeg *= Intensity;
	P.GazeScaleMul = FMath::Lerp(1.0f, P.GazeScaleMul, Intensity);

	ActiveProfile = P;
	if (P.GazeAnchorSec > 0.0f)
	{
		const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		GazeAnchorUntil = Now + P.GazeAnchorSec;
		NextSaccadeAt = -1.0;  // 앵커로 즉시 옮겨 간다
	}

	UE_LOG(LogEarlOne, Log,
		TEXT("Sentence profile: %s (강도 %.2f) → 눈 %.2f, 롤 %.1f도, 피치 %.1f도, 시선배수 %.2f%s"),
		*EarlTypes::EmotionToString(Sentence.Emotion), Intensity,
		P.EyeWiden, P.RollDeg, P.PitchDeg, P.GazeScaleMul,
		P.GazeAnchorSec > 0.0f ? TEXT(", 회상 앵커") : TEXT(""));
}

void UEarlLipSyncComponent::TriggerBlink()
{
	// 이미 진행 중이면 새로 시작하지 않는다 — 겹치면 눈이 떨린다
	if (BlinkElapsed < 0.0f)
	{
		BlinkElapsed = 0.0f;
	}
}

float UEarlLipSyncComponent::EvaluateBlink(float DeltaTime)
{
	if (BlinkElapsed < 0.0f)
	{
		return 0.0f;
	}
	BlinkElapsed += DeltaTime;

	const float Close = FMath::Max(BlinkCloseSec, KINDA_SMALL_NUMBER);
	const float Hold = FMath::Max(BlinkHoldSec, 0.0f);
	const float Open = FMath::Max(BlinkOpenSec, KINDA_SMALL_NUMBER);

	// 감김(빠름) → 유지 → 뜸(느림). 사람 눈은 닫히는 것이 뜨는 것보다 두 배쯤 빠르다.
	float Level;
	if (BlinkElapsed < Close)
	{
		Level = BlinkElapsed / Close;
	}
	else if (BlinkElapsed < Close + Hold)
	{
		Level = 1.0f;
	}
	else if (BlinkElapsed < Close + Hold + Open)
	{
		Level = 1.0f - (BlinkElapsed - Close - Hold) / Open;
	}
	else
	{
		BlinkElapsed = -1.0f;
		return 0.0f;
	}
	return FMath::Clamp(Level, 0.0f, 1.0f) * BlinkAmplitude;
}

void UEarlLipSyncComponent::UpdateGaze(float DeltaTime)
{
	if (!Source.IsValid())
	{
		return;
	}
	// ⚠️ 예전엔 `GazeAmplitude <= 0` 이면 여기서 곧바로 return 했다. 그런데 이 함수 안에는
	// 깜빡임·침 삼킴·눈 커짐·머리 추종이 **다 들어 있어서**, 시선을 끄면 그것들까지 같이 죽었다.
	// T76 과 똑같은 실수를 층만 바꿔 반복한 것이다 — **독립적인 기여를 한 스위치에 묶지 말 것.**
	// 이제 시선 관련 계산만 이 플래그로 감싸고, 나머지 층은 항상 돈다.
	const bool bGazeOn = GazeAmplitude > KINDA_SMALL_NUMBER;

	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;

	// ── 대화 상태 판정 ────────────────────────────────────────────────────
	// 말하는 중 → 발화 / 발화 직후 응시 창이 열려 있으면 → 응시 / 그 창이 닫히면 → 대기.
	// 응시 창은 `HandlePlaybackFinished` 에서 갱신된다. 즉 "대화가 끝나면 몇 초간 상대를 보다가
	// 추가 질문이 없으면 다른 곳도 본다"가 그대로 이 세 줄이다.
	const EEarlGazeState PrevState = GazeState;
	GazeState = bSpeaking
		? EEarlGazeState::Speaking
		: (Now < AttentionUntil ? EEarlGazeState::Attentive : EEarlGazeState::Idle);

	// ── ★ 주의는 스위치가 아니라 감쇠다 (2026-08-06) ──────────────────────
	// 예전엔 `AttentionUntil` 을 지나는 **그 한 프레임**에 응시→대기로 뒤집혔고, 그 자리에서
	// 사케이드를 강제로 다시 뽑았다. 그래서 가만히 응시하고 있다가 정해진 초에 눈이 툭 튀었다 —
	// 사용자가 말한 "대화 끝나고 멈춰 있다가 idle 로 변하는 게 너무 기계적이다"가 이거다.
	// 사람의 관심은 계단이 아니라 경사로 식는다. 창이 닫히기 전부터 서서히 풀리게 한다.
	float AttentionLevel;
	if (bSpeaking)
	{
		AttentionLevel = 1.0f;
	}
	else if (GazeDisengageSec > KINDA_SMALL_NUMBER)
	{
		AttentionLevel = FMath::Clamp(
			static_cast<float>(AttentionUntil - Now) / GazeDisengageSec, 0.0f, 1.0f);
	}
	else
	{
		AttentionLevel = (Now < AttentionUntil) ? 1.0f : 0.0f;   // 0 = 예전(계단) 동작
	}
	// 선형이면 풀리기 시작하는 순간과 끝나는 순간이 눈에 띈다 — 양 끝을 뭉갠다
	AttentionLevel = FMath::SmoothStep(0.0f, 1.0f, AttentionLevel);

	if (GazeState != PrevState)
	{
		// 새 발화가 오면 즉시 상대를 본다 — 그건 강제하는 게 맞다.
		// 반대로 **응시 → 대기**는 강제하지 않는다. 그 강제가 "정해진 초에 눈이 툭 튀는"
		// 인상의 직접적 원인이었다. 대신 한 박자 뜸을 들이게 한다.
		if (GazeState != EEarlGazeState::Idle)
		{
			NextSaccadeAt = -1.0;
		}
		else
		{
			// 관심이 다 식은 직후 곧장 딴 데를 보면 "타이머가 끝났구나"로 읽힌다.
			// 사람은 한 박자 더 머물다가 시선을 뗀다 — 그 뜸의 길이도 매번 다르다.
			NextSaccadeAt = FMath::Max(NextSaccadeAt, Now + FMath::FRandRange(0.6f, 2.0f));
		}
		// 드문 이벤트라 Log 레벨로 남긴다 — 시연 중 "왜 지금 딴 데 보지?" 를 사후에 알 수 있어야 한다
		UE_LOG(LogEarlOne, Log, TEXT("Gaze state -> %s (주의 %.2f)"),
			GazeState == EEarlGazeState::Speaking  ? TEXT("Speaking (발화 중)") :
			GazeState == EEarlGazeState::Attentive ? TEXT("Attentive (상대 응시)") : TEXT("Idle (주변)"),
			AttentionLevel);
	}

	// 상태별 상수를 고르는 대신 **주의 수준으로 섞는다.** 이 세 값이 계단으로 바뀌던 것이
	// 전환을 기계적으로 만들었다 — 이제 응시↔대기 사이를 연속으로 지난다.
	float ReturnChance;
	float AmplitudeScale;
	float IntervalScale;
	if (GazeState == EEarlGazeState::Speaking)
	{
		ReturnChance = GazeReturnChanceSpeaking;
		AmplitudeScale = GazeAmplitudeScaleSpeaking;
		IntervalScale = 0.85f;  // 말하는 중엔 조금 더 자주
	}
	else
	{
		ReturnChance   = FMath::Lerp(GazeReturnChanceIdle, GazeReturnChanceAttentive, AttentionLevel);
		AmplitudeScale = FMath::Lerp(1.0f, GazeAmplitudeScaleAttentive, AttentionLevel);
		IntervalScale  = FMath::Lerp(1.0f, 1.3f, AttentionLevel);   // 응시 중엔 눈이 덜 움직인다
	}

	// 문장 프로파일을 천천히 목표로 붙인다 (표정처럼 툭 튀면 안 된다).
	// 발화가 끝나면 ActiveProfile 이 비워지므로 자동으로 0 으로 풀린다.
	const float Blend = 1.0f - FMath::Exp(-DeltaTime * ProfileBlendSpeed);
	BlendedProfile.EyeWiden = FMath::Lerp(BlendedProfile.EyeWiden, ActiveProfile.EyeWiden, Blend);
	BlendedProfile.RollDeg = FMath::Lerp(BlendedProfile.RollDeg, ActiveProfile.RollDeg, Blend);
	BlendedProfile.PitchDeg = FMath::Lerp(BlendedProfile.PitchDeg, ActiveProfile.PitchDeg, Blend);
	BlendedProfile.GazeScaleMul = FMath::Lerp(BlendedProfile.GazeScaleMul, ActiveProfile.GazeScaleMul, Blend);
	AmplitudeScale *= BlendedProfile.GazeScaleMul;

	// ★ 회상 앵커 — 이 창이 열려 있는 동안은 사케이드를 뽑지 않고 한 곳을 본다.
	// 사람이 기억을 꺼낼 때 시선이 한쪽 위에 머무는 것이 이 동작이다.
	const bool bAnchored = bGazeOn && Now < GazeAnchorUntil;
	if (!bGazeOn)
	{
		// 시선을 껐어도 아래 층(깜빡임·아이들 몸짓)은 돌아야 한다
	}
	else if (bAnchored)
	{
		GazeTarget = ActiveProfile.GazeAnchor;
		NextSaccadeAt = GazeAnchorUntil;  // 창이 닫히는 순간 새 목표를 뽑는다
	}
	else if (NextSaccadeAt < 0.0 || Now >= NextSaccadeAt)
	{
		// 정면(=대화 상대) 복귀 vs 곁눈질. 상태가 이 확률을 정한다.
		if (FMath::FRand() < ReturnChance)
		{
			// 정확히 0 으로 가면 기계적이라 아주 살짝 흩뜨린다.
			// 단 **응시 상태에서는 그 흩뜨림도 줄여** 눈이 상대에 또렷하게 붙게 한다
			// ("눈 응시를 플레이어한테 또렷히 고정을 못하나", 2026-08-05).
			// 주의 수준에 비례해 흩뜨림을 줄인다 — 상태 boolean 이 아니라 연속값이라
			// 관심이 식는 동안 눈이 서서히 풀린다.
			const float Jitter = 1.0f - GazeAttentiveLock * AttentionLevel;
			GazeTarget = FVector2D(FMath::FRandRange(-0.06f, 0.06f) * Jitter,
								   FMath::FRandRange(-0.05f, 0.05f) * Jitter);
		}
		else
		{
			// 수평이 수직보다 크다 — 사람은 좌우로 훨씬 많이 훑는다
			GazeTarget = FVector2D(
				FMath::FRandRange(-1.0f, 1.0f) * GazeAmplitude * AmplitudeScale,
				FMath::FRandRange(-0.55f, 0.45f) * GazeAmplitude * AmplitudeScale);
		}
		NextSaccadeAt = Now + FMath::FRandRange(
			GazeSaccadeMinSec * IntervalScale, GazeSaccadeMaxSec * IntervalScale);

		// ★ 머리 추종 예약 — 사람은 눈이 먼저 튀고 머리가 뒤따른다.
		// 작은 곁눈질에는 머리가 안 움직인다(HeadFollowMinGaze). 큰 이동이면 머리가
		// 시선 각도의 일부를 가져가고, 눈은 전정안반사로 그만큼 되돌아온다 —
		// 전체 시선은 목표에 그대로 있고 **분배만** 바뀐다.
		const float TargetMag = FMath::Max(FMath::Abs(GazeTarget.X), FMath::Abs(GazeTarget.Y));
		PendingFollowTarget = (TargetMag >= HeadFollowMinGaze)
			? FVector2D(GazeTarget.X, GazeTarget.Y) * GazeDegreesPerUnit * HeadFollowGain
			: FVector2D::ZeroVector;
		PendingFollowAt = Now + HeadFollowLatencySec;

		// ★ 사케이드 동기 깜빡임 (행동목록 A4). 사람은 시선을 크게 옮길 때 같이 깜빡인다 —
		// 그 순간의 시각 정보를 어차피 못 쓰기 때문이다. 붙여 두면 눈이 훨씬 산다.
		if (TargetMag >= HeadFollowMinGaze && FMath::FRand() < BlinkOnSaccadeChance)
		{
			TriggerBlink();
		}
	}

	// 자발 깜빡임 — 발화 중에는 조금 더 자주
	if (BlinkAmplitude > KINDA_SMALL_NUMBER)
	{
		const float IntervalMul = (GazeState == EEarlGazeState::Speaking)
			? BlinkSpeakingIntervalScale : 1.0f;
		if (NextBlinkAt < 0.0)
		{
			NextBlinkAt = Now + FMath::FRandRange(BlinkMinSec, BlinkMaxSec) * IntervalMul;
		}
		else if (Now >= NextBlinkAt)
		{
			TriggerBlink();
			NextBlinkAt = Now + FMath::FRandRange(BlinkMinSec, BlinkMaxSec) * IntervalMul;
			if (!bLoggedBlink)
			{
				bLoggedBlink = true;
				UE_LOG(LogEarlOne, Log,
					TEXT("Procedural blink on: %.1f~%.1f초 간격, 사케이드 동기 %.0f%% "
						 "(클립의 eyeBlink 는 깜빡임이 아니라 눈웃음 지속 감김이라 더 이상 안 쓴다)"),
					BlinkMinSec, BlinkMaxSec, BlinkOnSaccadeChance * 100.0f);
			}
		}
	}

	// 지연이 지나면 목표가 실제로 옮겨 간다 (그 전까지는 눈만 움직인 상태)
	if (PendingFollowAt >= 0.0 && Now >= PendingFollowAt)
	{
		HeadFollowTarget = PendingFollowTarget;
		PendingFollowAt = -1.0;
	}
	// ★ 머리는 **스프링**으로 붙인다 (2026-08-05). 선형 보간(InterpTo)은 목표에서 속도가
	// 뚝 끊겨 **로봇처럼** 보인다 — 사용자 지적 "머리가 무슨 로봇마냥 움직인다".
	// 사람 목은 관성이 있어 가속·감속을 하고 목표를 아주 살짝 넘었다가 정착한다.
	// 감쇠비 0.75 = 약간 부족감쇠(살짝 넘어감). 1.0 이면 넘지 않고 부드럽게만 붙는다.
	{
		const float Omega = FMath::Max(HeadFollowSpeed, 0.1f);
		const float Zeta = HeadFollowDamping;
		const FVector2D Accel =
			(HeadFollowTarget - HeadFollowCurrent) * (Omega * Omega)
			- HeadFollowVelocity * (2.0f * Zeta * Omega);
		HeadFollowVelocity += Accel * DeltaTime;
		HeadFollowCurrent += HeadFollowVelocity * DeltaTime;
	}
	// ── 몸 추종 (2026-08-10) — "외형은 ARDY, 표정은 우리 것" 구조의 마지막 조각 ─────
	// MetaHuman 은 Body 와 Face 가 별개 메시다. ARDY 리타깃은 Body 만 돌리는데,
	// 얼굴 포스트프로세스 리그가 Face 의 목·머리를 **커브**(HeadYaw/Pitch/Roll)로 정하므로
	// 몸이 돌아도 얼굴은 제자리였다(실측 추종률 0.02 — "머리통이 따로 논다").
	// 여기서 Body 의 머리 각(가슴 기준 — 목+머리 합산)을 읽어 그 커브 채널에 실어 준다.
	// 시선·끄덕임·미세 흔들림은 그 위에 그대로 더해진다 — 표정 층은 여전히 우리 것.
	if (bBodyHeadFollow)
	{
		FVector BodyTarget = FVector::ZeroVector;
		if (!BodyMeshCache.IsValid())
		{
			for (TActorIterator<AActor> It(GetWorld()); It; ++It)
			{
				if (It->ActorHasTag(FName(TEXT("ArdyPreview")))) { continue; }
				TArray<USkeletalMeshComponent*> Meshes;
				It->GetComponents(Meshes);
				for (USkeletalMeshComponent* Mesh : Meshes)
				{
					const FString N = Mesh->GetName();
					if (N.Contains(TEXT("Body")) && !N.Contains(TEXT("Face"))
						&& Mesh->GetBoneIndex(FName(TEXT("head"))) != INDEX_NONE)
					{
						BodyMeshCache = Mesh;
						break;
					}
				}
				if (BodyMeshCache.IsValid()) { break; }
			}
		}
		if (USkeletalMeshComponent* Body = BodyMeshCache.Get())
		{
			const int32 HeadIdx = Body->GetBoneIndex(FName(TEXT("head")));
			// 기준은 **가슴**(spine_05→04→03 중 있는 것). 목 기준으로 재면 목 회전이 빠져
			// 몸통이 돌 때 여전히 안 따라온다 — 리그가 목·머리를 통째로 덮으므로
			// 가슴→머리 **합산 회전**을 넘겨야 한다.
			int32 BaseIdx = INDEX_NONE;
			for (const TCHAR* BaseName : { TEXT("spine_05"), TEXT("spine_04"), TEXT("spine_03") })
			{
				BaseIdx = Body->GetBoneIndex(FName(BaseName));
				if (BaseIdx != INDEX_NONE) { break; }
			}
			if (HeadIdx != INDEX_NONE && BaseIdx != INDEX_NONE)
			{
				const FQuat HeadQ = Body->GetBoneTransform(HeadIdx).GetRotation();
				const FQuat BaseQ = Body->GetBoneTransform(BaseIdx).GetRotation();
				const FQuat ActorQ = Body->GetOwner() ? Body->GetOwner()->GetActorQuat() : FQuat::Identity;
				// 상대 회전은 Base 뼈 프레임에 있다 → **액터 프레임으로 켤레**해야
				// 로테이터 분해가 화면의 요/피치/롤과 대응한다 (뼈 축은 화면 축이 아니다).
				const FQuat C = ActorQ.Inverse() * BaseQ;
				const FQuat RelActor = C * (BaseQ.Inverse() * HeadQ) * C.Inverse();
				const FRotator R = RelActor.Rotator();
				BodyTarget = FVector(
					R.Yaw * BodyFollowYawSign,
					R.Pitch * BodyFollowPitchSign,
					R.Roll * BodyFollowRollSign) * BodyHeadFollowGain;
			}
		}
		BodyHeadCurrent = FMath::VInterpTo(BodyHeadCurrent, BodyTarget, DeltaTime, BodyHeadFollowSpeed);
	}
	else
	{
		BodyHeadCurrent = FVector::ZeroVector;
	}

	// 프로파일의 기울임·앞뒤를 얹는다 (감정·질문에서 온다)
	Source->SetHeadFollow(
		static_cast<float>(HeadFollowCurrent.X) + BodyHeadCurrent.X,
		static_cast<float>(HeadFollowCurrent.Y) + BlendedProfile.PitchDeg + BodyHeadCurrent.Y,
		BlendedProfile.RollDeg + BodyHeadCurrent.Z);

	// 사케이드는 빠르게 목표에 닿고, 그 뒤에는 미세 표류만 남는다
	GazeCurrent = FMath::Vector2DInterpTo(GazeCurrent, GazeTarget, DeltaTime, GazeSaccadeSpeed);

	GazeClock += DeltaTime;
	const double T = GazeClock;
	// 응시 중에는 미세 표류도 줄인다 — 상대를 볼 때 눈이 계속 흔들리면 집중해 보이지 않는다
	const float DriftScale = 1.0f - GazeAttentiveLock * 0.85f * AttentionLevel;
	const FVector2D Drift(
		GazeDriftAmplitude * DriftScale * (0.6 * FMath::Sin(T * 1.7) + 0.4 * FMath::Sin(T * 3.1 + 1.2)),
		GazeDriftAmplitude * DriftScale * 0.7 * FMath::Sin(T * 2.3 + 0.5));

	// ── 머리 보정 (전정안반사) ────────────────────────────────────────────
	// 머리가 절차적으로 도는 동안 눈을 그대로 두면 시선이 머리에 끌려가 상대에서 벗어난다.
	// 사람은 반대다 — 머리가 돌아도 **눈은 보던 것에 남는다.** 머리 각도만큼 반대로 밀어 준다.
	// 이것이 없으면 "정면 복귀"가 정면이 아니게 된다.
	FVector2D HeadComp = FVector2D::ZeroVector;
	if (GazeHeadCompensation > KINDA_SMALL_NUMBER)
	{
		float HeadYawDeg = 0.0f, HeadPitchDeg = 0.0f;
		Source->GetHeadMotionAngles(HeadYawDeg, HeadPitchDeg);
		const float PerDeg = 1.0f / FMath::Max(GazeDegreesPerUnit, 1.0f);
		HeadComp = FVector2D(-HeadYawDeg * PerDeg, -HeadPitchDeg * PerDeg) * GazeHeadCompensation;
	}

	const float X = bGazeOn ? FMath::Clamp(static_cast<float>(GazeCurrent.X + Drift.X + HeadComp.X), -1.0f, 1.0f) : 0.0f;
	const float Y = bGazeOn ? FMath::Clamp(static_cast<float>(GazeCurrent.Y + Drift.Y + HeadComp.Y), -1.0f, 1.0f) : 0.0f;

	// 두 눈이 같은 값을 받아야 사시가 되지 않는다 (수렴은 이 층에서 다루지 않는다)
	TMap<FString, float> Procedural;
	Procedural.Reserve(16);
	if (bGazeOn)
	{
		Procedural.Add(TEXT("CTRL_L_eye.tx"), X);
		Procedural.Add(TEXT("CTRL_R_eye.tx"), X);
		Procedural.Add(TEXT("CTRL_L_eye.ty"), Y);
		Procedural.Add(TEXT("CTRL_R_eye.ty"), Y);
	}

	// ── 강세 눈썹 플래시 ──────────────────────────────────────────────────
	// 빠른 상승 / 느린 하강. 사람 눈썹은 강세에 **툭** 올라갔다 천천히 내려온다.
	if (AccentTimeRemaining > 0.0f)
	{
		AccentTimeRemaining = FMath::Max(0.0f, AccentTimeRemaining - DeltaTime);
		const float Elapsed = AccentDurationSec - AccentTimeRemaining;
		const float Shape = Elapsed < AccentAttackSec
			? Elapsed / FMath::Max(AccentAttackSec, KINDA_SMALL_NUMBER)
			: 1.0f - (Elapsed - AccentAttackSec)
				/ FMath::Max(AccentDurationSec - AccentAttackSec, KINDA_SMALL_NUMBER);
		const float Level = FMath::Clamp(Shape, 0.0f, 1.0f) * AccentStrength * AccentBrowGain;

		// ⚠️ 좌우를 **다르게** 준다. 완전 대칭인 눈썹은 사람이 아니라 마스크로 읽힌다.
		Procedural.Add(TEXT("CTRL_L_brow_raiseIn.ty"),  Level);
		Procedural.Add(TEXT("CTRL_R_brow_raiseIn.ty"),  Level * 0.86f);
		Procedural.Add(TEXT("CTRL_L_brow_raiseOut.ty"), Level * 0.55f);
		Procedural.Add(TEXT("CTRL_R_brow_raiseOut.ty"), Level * 0.44f);
	}

	// ── 깜빡임 (행동목록 §1 A4) ───────────────────────────────────────────
	// 이 층이 눈꺼풀의 **유일한 주인**이다 — 감정 클립은 더 이상 eyeBlink 를 내지 않는다.
	{
		const float Blink = EvaluateBlink(DeltaTime);
		if (Blink > 0.001f)
		{
			// 좌우 미세 차이 — 완전 동시 깜빡임은 사람이 아니라 인형이다
			Procedural.Add(TEXT("CTRL_L_eye_blink.ty"), Blink);
			Procedural.Add(TEXT("CTRL_R_eye_blink.ty"), Blink * 0.97f);
		}
	}

	// ── 눈 커짐 (행동목록 §2 B4) ──────────────────────────────────────────
	// 솔버는 윗눈꺼풀을 만들지 않는다. "개념을 설명할 때 눈이 커진다" 가 여기서 나온다.
	if (FMath::Abs(BlendedProfile.EyeWiden) > 0.005f && FMath::Abs(EyeWidenGain) > KINDA_SMALL_NUMBER)
	{
		const float Widen = BlendedProfile.EyeWiden * EyeWidenGain;   // 양수 = 눈을 크게
		// ⚠️ `CTRL_{L,R}_eye_eyelidU.ty` 는 **음수가 윗눈꺼풀 올리기**다 (2026-08-05 실측:
		// 게인 -1.2 로 넣었을 때 raw `eyeUpperLidUpL` = 0.420, 양수일 때는 계속 0).
		// T62(코 주름 부호 반대)와 같은 종류의 함정 — GUI 축 부호는 이름으로 추측하면 안 된다.
		// 여기서 뒤집어 두면 바깥(프로파일·ini)에서는 "양수 = 크게" 로 직관적으로 쓸 수 있다.
		Procedural.Add(TEXT("CTRL_L_eye_eyelidU.ty"), -Widen);
		Procedural.Add(TEXT("CTRL_R_eye_eyelidU.ty"), -Widen * 0.94f);   // 좌우 비대칭
		// 동공은 **의도** 기준으로 판단한다 — 눈을 크게 뜰 때만 살짝 확장.
		if (PupilGain > KINDA_SMALL_NUMBER && Widen > 0.0f)
		{
			Procedural.Add(TEXT("CTRL_L_eye_pupil.ty"), Widen * PupilGain);
			Procedural.Add(TEXT("CTRL_R_eye_pupil.ty"), Widen * PupilGain);
		}
	}

	// ── 아이들 몸짓 (행동목록 §1 A5·A7) ───────────────────────────────────
	// 대기 중에만 돈다. 말하는 중에 침을 삼키면 이상하다.
	if (GazeState != EEarlGazeState::Speaking)
	{
		// 침 삼킴 — 목이 한 번 오르내린다. 값싸고 "살아 있다"가 크게 오른다.
		if (SwallowGain > KINDA_SMALL_NUMBER)
		{
			if (NextSwallowAt < 0.0)
			{
				NextSwallowAt = Now + FMath::FRandRange(SwallowMinSec, SwallowMaxSec);
			}
			else if (Now >= NextSwallowAt && SwallowTimeRemaining <= 0.0f)
			{
				SwallowTimeRemaining = 0.45f;
				NextSwallowAt = Now + FMath::FRandRange(SwallowMinSec, SwallowMaxSec);
				// 콘솔 없이도 "돌고 있다"를 확인할 수 있어야 한다 (SendKeys 연타는 자꾸 유실된다)
				if (!bLoggedSwallow)
				{
					bLoggedSwallow = true;
					UE_LOG(LogEarlOne, Log, TEXT("Idle gesture: swallow fired (게인 %.2f, 간격 %.0f~%.0f초)"),
						SwallowGain, SwallowMinSec, SwallowMaxSec);
				}
			}
			if (SwallowTimeRemaining > 0.0f)
			{
				SwallowTimeRemaining = FMath::Max(0.0f, SwallowTimeRemaining - DeltaTime);
				const float Phase = 1.0f - SwallowTimeRemaining / 0.45f;
				Procedural.Add(TEXT("CTRL_C_neck_swallow.ty"),
					SwallowGain * FMath::Sin(PI * FMath::Clamp(Phase, 0.0f, 1.0f)));
			}
		}

		// 눈썹 미세 플래시 — 발화 강세보다 훨씬 약하게, 훨씬 드물게
		if (IdleBrowGain > KINDA_SMALL_NUMBER && AccentTimeRemaining <= 0.0f)
		{
			if (NextIdleBrowAt < 0.0)
			{
				NextIdleBrowAt = Now + FMath::FRandRange(IdleBrowMinSec, IdleBrowMaxSec);
			}
			else if (Now >= NextIdleBrowAt && IdleBrowTimeRemaining <= 0.0f)
			{
				IdleBrowTimeRemaining = 0.6f;
				NextIdleBrowAt = Now + FMath::FRandRange(IdleBrowMinSec, IdleBrowMaxSec);
				if (!bLoggedIdleBrow)
				{
					bLoggedIdleBrow = true;
					UE_LOG(LogEarlOne, Log, TEXT("Idle gesture: brow flash fired (게인 %.2f, 간격 %.0f~%.0f초)"),
						IdleBrowGain, IdleBrowMinSec, IdleBrowMaxSec);
				}
			}
			if (IdleBrowTimeRemaining > 0.0f)
			{
				IdleBrowTimeRemaining = FMath::Max(0.0f, IdleBrowTimeRemaining - DeltaTime);
				const float Phase = 1.0f - IdleBrowTimeRemaining / 0.6f;
				const float Level = IdleBrowGain * FMath::Sin(PI * FMath::Clamp(Phase, 0.0f, 1.0f));
				Procedural.Add(TEXT("CTRL_L_brow_raiseIn.ty"), Level);
				Procedural.Add(TEXT("CTRL_R_brow_raiseIn.ty"), Level * 0.8f);
			}
		}
	}

	Source->SetProceduralCurves(Procedural);

	if (!bLoggedGaze)
	{
		bLoggedGaze = true;
		UE_LOG(LogEarlOne, Log,
			TEXT("Procedural gaze on: amplitude %.2f, saccade %.1f~%.1fs "
				 "(솔버는 눈동자 방향 커브를 내지 않는다 — 이 층이 그 자리다)"),
			GazeAmplitude, GazeSaccadeMinSec, GazeSaccadeMaxSec);
	}
}

void UEarlLipSyncComponent::ApplyHeadMotionSettings(float InAmplitudeDeg, float InSpeed)
{
	HeadMotionAmplitudeDeg = InAmplitudeDeg;
	HeadMotionSpeed = InSpeed;
	if (Source.IsValid())
	{
		Source->SetHeadMotion(HeadMotionAmplitudeDeg, HeadMotionSpeed);
		UE_LOG(LogEarlOne, Log, TEXT("Head motion -> %.1f deg, speed %.2f"),
			HeadMotionAmplitudeDeg, HeadMotionSpeed);
	}
	else
	{
		UE_LOG(LogEarlOne, Warning,
			TEXT("Head motion set to %.1f deg but no solver source — 절차적 폴백에는 머리 층이 없다"),
			HeadMotionAmplitudeDeg);
	}
}

void UEarlLipSyncComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		ILiveLinkClient& LiveLinkClient =
			IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
		if (Source.IsValid()) { LiveLinkClient.RemoveSource(Source); }
		if (VisemeSource.IsValid()) { LiveLinkClient.RemoveSource(VisemeSource); }
	}
	Source.Reset();
	VisemeSource.Reset();
	Super::EndPlay(EndPlayReason);
}

void UEarlLipSyncComponent::HandleSentenceStarted(const FEarlSentencePayload& Sentence)
{
	// 감정·질문·설명·회상이 시선과 머리에 걸린다 (행동목록 §3·§4)
	ApplySentenceProfile(Sentence);

	if (Backend != EEarlLipSyncBackend::EpicSolver)
	{
		PendingSyllables.Add(EarlKoreanViseme::Decompose(Sentence.Text));
	}
}

void UEarlLipSyncComponent::HandlePCMData(int32 SampleRate, int32 NumChannels, const TArray<uint8>& PCMData)
{
	TArray<float> Mono = ConvertToMonoFloat(PCMData, NumChannels);
	if (Mono.IsEmpty() || SampleRate <= 0)
	{
		return;
	}

	// 실제 오디오가 도착했다 = 발화 시작. 입 닫기를 즉시 해제한다 — 이제 솔버가 입의 주인이다.
	// (해제는 여기서, 재개는 HandlePlaybackFinished 에서. 버퍼 잔량으로 판정하면 안 된다)
	if (Source.IsValid())
	{
		Source->SetIdleMouthDamp(false, 0.0f);
	}
	if (!bSpeaking)
	{
		bSpeaking = true;
		SpeechStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		bLoggedStarvation = false;
	}

	// ── 비짐 경로 (단독·하이브리드): 문장 텍스트+정렬로 트랙을 미리 계산해 큐에 적재 ──
	if (Backend != EEarlLipSyncBackend::EpicSolver)
	{
		TArray<EarlKoreanViseme::FSyllable> Syllables;
		if (!PendingSyllables.IsEmpty())
		{
			Syllables = MoveTemp(PendingSyllables[0]);
			PendingSyllables.RemoveAt(0);
		}
		// 정렬 특징(치찰 고역 포함)은 원음 샘플레이트에서 계산
		TArray<TMap<FName, float>> Track = EarlKoreanViseme::BuildTrack(Syllables, Mono, SampleRate);
		if (FrameQueue.Num() <= FrameReadOffset) // 발화 시작
		{
			FrameQueue.Reset();
			FrameReadOffset = 0;
			FrameClock = 0.0;
			const int32 DelayMs = Backend == EEarlLipSyncBackend::Hybrid ? HybridVisemeDelayMs : VisemeDelayMs;
			VisemeStartAt = (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0) + DelayMs / 1000.0;
			UE_LOG(LogEarlOne, Log, TEXT("KoreanViseme track: %d syllables -> %d frames (delay %dms)"),
				Syllables.Num(), Track.Num(), DelayMs);
		}
		FrameQueue.Append(MoveTemp(Track));
		if (Backend == EEarlLipSyncBackend::KoreanViseme)
		{
			return; // 단독 모드는 솔버 공급 없음
		}
	}

	// 솔버 = 원음 그대로 (안티앨리어싱 없는 16k 다운샘플이 자음 비짐을 깎음), 절차적 = 16k
	const bool bSolver = Source.IsValid() && Source->IsUsingSolver();
	if (!bSolver)
	{
		Mono = ResampleTo16k(Mono, SampleRate);
	}
	const int32 IncomingRate = bSolver ? SampleRate : FEarlLipSyncSource::SampleRate;
	if (IncomingRate != StreamSampleRate)
	{
		PendingSamples.Reset();
		ReadOffset = 0;
		StreamSampleRate = IncomingRate;
	}

	// 발화 시작(버퍼 비어있음)이면 솔버 선행 버퍼만큼 즉시 밀어넣을 크레딧을 준다 —
	// 안 그러면 입이 음성보다 Lookahead 만큼 항상 늦는다 (지연 개선, 데모 대비 관찰)
	if (PendingSamples.Num() <= ReadOffset)
	{
		SampleAccumulator += static_cast<double>(LookaheadMs) / 1000.0;
	}
	PendingSamples.Append(Mono);
}

void UEarlLipSyncComponent::HandlePlaybackFinished()
{
	// ⚠️ 입 닫기는 **재생 종료 이벤트**로만 켠다.
	// 예전엔 "립싱크 버퍼가 비었는가"로 판정했는데, 립싱크는 오디오보다 먼저 버퍼를 소진한다
	// (발화 시작 버스트 크레딧 + 최대 5프레임 캐치업). 그래서 소리가 아직 나오는 중에
	// 무음으로 오판해 입을 닫아버렸다 — 문장 뒷부분의 립싱크가 통째로 사라졌다(2026-08-04).
	if (Source.IsValid())
	{
		Source->SetIdleMouthDamp(true, IdleMouthDamp);
	}
	const double NowFinished = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (bSpeaking && SpeechStartTime >= 0.0)
	{
		UE_LOG(LogEarlOne, Log, TEXT("Speech span (playback): %.2fs"), NowFinished - SpeechStartTime);
	}
	// ★ 말을 마치면 상대를 계속 본다 — "더 물어보실 게 있나요" 의 시간.
	// 이 창이 지나도록 새 발화가 없으면 시선이 알아서 주변으로 흩어진다(Idle).
	// 길이를 문장마다 흔든다 — 늘 같은 초에 시선을 떼면 사람이 아니라 타이머로 읽힌다.
	const float Jittered = GazeAttentionSec
		* FMath::FRandRange(1.0f - GazeAttentionJitter, 1.0f + GazeAttentionJitter);
	AttentionUntil = NowFinished + Jittered;

	// 문장 끝 끄덕임 (행동목록 §2 B2) — 말을 맺는 동작
	if (Source.IsValid() && HeadNodDeg != 0.0f)
	{
		Source->TriggerHeadNod(0.75f);
	}
	// 프로파일 해제 — 다음 문장까지 서서히 중립으로 풀린다
	ActiveProfile = FEarlExpressionProfile();
	GazeAnchorUntil = -1.0;
	bSpeaking = false;
	SpeechStartTime = -1.0;

	PendingSamples.Reset();
	ReadOffset = 0;
	SampleAccumulator = 0.0;
	PendingSyllables.Reset();
	FrameQueue.Reset();
	FrameReadOffset = 0;
	VisemeStartAt = -1.0;
	if (Source.IsValid())
	{
		Source->ResetToNeutral();
	}
	if (VisemeSource.IsValid())
	{
		VisemeSource->ResetToNeutral();
	}
}

void UEarlLipSyncComponent::SetExpressionCurves(const TMap<FString, float>& GuiCurves)
{
	if (Source.IsValid())
	{
		Source->SetExpressionCurves(GuiCurves);
	}
}

void UEarlLipSyncComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// ── 얼굴 메시 직결 바인딩 (BP_MHC 스폰 타이밍이 우리보다 늦을 수 있어 틱에서 재시도) ──
	if (bDriveFaceMeshDirect && !bFaceMeshBound)
	{
		FaceBindRetryAccum += DeltaTime;
		if (FaceBindRetryAccum >= 1.0f)
		{
			FaceBindRetryAccum = 0.0f;
			TryBindFaceMesh();
		}
	}

	// ── 비짐 프레임 페이싱 발행 (단독·하이브리드 공통) ──
	if (Backend != EEarlLipSyncBackend::EpicSolver && VisemeSource.IsValid() && FrameQueue.Num() > FrameReadOffset)
	{
		const bool bWaiting = VisemeStartAt >= 0.0 && GetWorld() && GetWorld()->GetTimeSeconds() < VisemeStartAt;
		if (!bWaiting)
		{
			VisemeStartAt = -1.0;
			FrameClock += FMath::Max(0.0f, DeltaTime);
			int32 Pushed = 0;
			while (FrameClock >= 0.02 && FrameQueue.Num() > FrameReadOffset && Pushed < 5)
			{
				VisemeSource->PushCurveFrame(FrameQueue[FrameReadOffset]);
				++FrameReadOffset;
				FrameClock -= 0.02;
				++Pushed;
			}
			if (FrameQueue.Num() <= FrameReadOffset)
			{
				FrameQueue.Reset();
				FrameReadOffset = 0;
			}
		}
	}
	if (Backend == EEarlLipSyncBackend::KoreanViseme)
	{
		return; // 단독 모드는 솔버 페이싱 없음
	}

	if (!Source.IsValid())
	{
		return;
	}

	// 시선은 발화 여부와 무관하게 항상 돈다 — 말하지 않을 때야말로 눈이 살아 있어야 한다
	UpdateGaze(DeltaTime);

	// ── 무음 유지 ──────────────────────────────────────────────────────────
	// 재생할 오디오가 없으면 예전엔 그냥 빠져나갔고, 그 순간 솔버가 프레임을 멈춰
	// 표정·얼굴 아이들이 통째로 정지했다. 무음 PCM 을 같은 페이싱으로 흘려 솔버를
	// 살려 두면, 그 프레임 위에 아이들·감정 커브가 실려 대기 중에도 얼굴이 산다.
	if (PendingSamples.Num() <= ReadOffset)
	{
		// ★ 재생 중인데 여기 들어왔다 = 립싱크가 오디오보다 먼저 버퍼를 소진했다.
		// 이 구간에 무음을 흘리면 소리가 나오는 동안 입이 멎는다. 얼마나 이른지 계측한다.
		if (bSpeaking && !bLoggedStarvation)
		{
			bLoggedStarvation = true;
			const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
			UE_LOG(LogEarlOne, Warning,
				TEXT("LipSync starved: buffer empty %.2fs after speech start (audio still playing)"),
				Now - SpeechStartTime);
		}
		// 재생이 아직 끝나지 않았으면 무음을 흘리지 않는다 — 소리가 나는 동안 입이 멎어 버린다.
		// (립싱크는 버스트 크레딧 200ms + 재생 지연 180ms 만큼 앞서 끝난다. 실측 4.69s vs 재생 ~5.1s)
		// 이 구간에는 아무것도 안 밀어 마지막 실제 포즈를 유지시킨다.
		if (bSpeaking)
		{
			return;
		}
		if (!bKeepSolverAliveWhenIdle || !Source->IsUsingSolver())
		{
			return;
		}
		// ⚠️ 무음은 **현재 스트림과 같은 레이트**여야 한다.
		// 예전엔 44100 하한을 박았는데, 기본 TTS 가 SAPI(22050)로 바뀐 뒤로
		// 무음 44100 ↔ 발화 22050 이 번갈아 들어가 솔버 타이밍이 어긋났다.
		// 레이트가 바뀌면 버퍼를 버리는 경로도 있어(HandlePCMData) 립싱크가 음성과 안 맞게 된다.
		// 2026-08-04 사용자 보고 "립싱크는 살아있는데 한국말이랑 안 맞는다"의 원인.
		const int32 IdleRate = StreamSampleRate > 0 ? StreamSampleRate : 44100;
		const int32 IdleSamplesPerFrame = FMath::Max(1, IdleRate / 50);
		IdleAccumulator += FMath::Max(0.0f, DeltaTime);
		const int32 IdleFrames = FMath::Clamp(FMath::FloorToInt(IdleAccumulator / 0.02), 0, 3);
		if (IdleFrames > 0)
		{
			// 0 으로 채운 프레임 — 솔버는 이걸 무음으로 읽고 중립 얼굴을 계속 낸다
			TArray<float> Silence;
			Silence.SetNumZeroed(IdleFrames * IdleSamplesPerFrame);
			Source->PushPcm(Silence, IdleRate, 1);
			// ★ 무음도 포락선에 먹인다 (2026-08-05).
			// 안 그러면 기준선이 **직전 발화 값에 얼어붙어** 다음 문장 시작 때 비율이 1 근처가 되고
			// 강세가 하나도 안 잡힌다(실측: 임계 1.9 에서 발화당 0회 — 소리는 났는데 검출 0).
			// 진폭 0 은 AccentMinLevel 에 걸려 발동하지 않으므로, 감쇠만 일어난다.
			UpdateSpeechAccent(Silence, DeltaTime);
			IdleAccumulator -= IdleFrames * 0.02;
			if (!bLoggedIdleKeepAlive)
			{
				bLoggedIdleKeepAlive = true;
				UE_LOG(LogEarlOne, Log,
					TEXT("Solver idle keep-alive on: feeding silence at %dHz so face idle/emotion stay live"),
					IdleRate);
			}
		}
		return;
	}
	IdleAccumulator = 0.0;

	// 재생 속도 페이싱: 델타타임만큼의 샘플만 밀어넣는다 (20ms 프레임 정렬, 최대 5프레임 캐치업)
	SampleAccumulator += FMath::Max(0.0f, DeltaTime);
	const int32 SamplesPerFrame = FMath::Max(1, StreamSampleRate / 50);
	constexpr double FrameSeconds = 0.02;
	const int32 FramesDue = FMath::Clamp(FMath::FloorToInt(SampleAccumulator / FrameSeconds), 0, 5);
	if (FramesDue <= 0)
	{
		return;
	}

	const int32 Available = PendingSamples.Num() - ReadOffset;
	int32 SamplesToPush = FMath::Min(Available, FramesDue * SamplesPerFrame);
	SamplesToPush -= SamplesToPush % SamplesPerFrame;
	if (SamplesToPush <= 0)
	{
		return;
	}

	TArray<float> Chunk;
	Chunk.Append(PendingSamples.GetData() + ReadOffset, SamplesToPush);
	Source->PushPcm(Chunk, StreamSampleRate, 1);

	// ★ 강세 검출 — 솔버로 보내는 바로 그 청크에서 진폭을 낸다 (2026-08-05).
	// 단어별 타임스탬프가 없어도(ADR 011) **PCM 은 이미 우리 손에 있다.** 진폭 포락선의
	// 피크가 곧 강세이고, 거기에 눈썹·끄덕임을 붙이면 처음으로 말과 얼굴이 연결된다.
	UpdateSpeechAccent(Chunk, DeltaTime);

	ReadOffset += SamplesToPush;
	SampleAccumulator -= static_cast<double>(SamplesToPush) / StreamSampleRate;

	// 소진된 버퍼 정리
	if (ReadOffset >= PendingSamples.Num())
	{
		PendingSamples.Reset();
		ReadOffset = 0;
	}
}

void UEarlLipSyncComponent::TryBindFaceMesh()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<USkeletalMeshComponent*> Meshes;
		It->GetComponents(Meshes);
		for (USkeletalMeshComponent* Mesh : Meshes)
		{
			// 얼굴 판별: RigLogic 포스트프로세스 인스턴스가 있고 메시 이름에 Face
			const FString MeshName = Mesh->GetSkeletalMeshAsset() ? Mesh->GetSkeletalMeshAsset()->GetName() : FString();
			if (!Mesh->GetPostProcessInstance() || !MeshName.Contains(TEXT("Face")))
			{
				continue;
			}
			if (Cast<ULiveLinkInstance>(Mesh->GetAnimInstance()))
			{
				bFaceMeshBound = true;
				return;
			}
			Mesh->SetAnimationMode(EAnimationMode::AnimationBlueprint);
			Mesh->SetAnimInstanceClass(ULiveLinkInstance::StaticClass());
			if (ULiveLinkInstance* LLI = Cast<ULiveLinkInstance>(Mesh->GetAnimInstance()))
			{
				LLI->SetSubject(FLiveLinkSubjectName(LipSyncSubjectName));
				bFaceMeshBound = true;
				UE_LOG(LogEarlOne, Log,
					TEXT("얼굴 메시 직결: %s ← LiveLink subject '%s' (ULiveLinkInstance — ABP_Face 에 LiveLink 노드가 없어 대체, 포스트프로세스 RigLogic 유지)"),
					*MeshName, *LipSyncSubjectName.ToString());
				return;
			}
			UE_LOG(LogEarlOne, Warning, TEXT("얼굴 메시 직결 실패: %s 에 ULiveLinkInstance 를 못 붙였다"), *MeshName);
		}
	}
}

void UEarlLipSyncComponent::ApplyLipRecipe(bool bStock)
{
	MouthCurveGain = bStock ? 1.0f : 1.3f;
	LookaheadMs = bStock ? 80 : 200;

	if (Backend == EEarlLipSyncBackend::KoreanViseme)
	{
		UE_LOG(LogEarlOne, Warning, TEXT("earl.LipRecipe: 비짐 백엔드에는 적용 대상 아님"));
		return;
	}
	if (!IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		return;
	}
	ILiveLinkClient& LiveLinkClient =
		IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);

	// Lookahead 는 서브젝트 생성 시 박힌다 — 소스를 재생성한다 (서브젝트명 동일, 얼굴 바인딩 유지)
	if (Source.IsValid())
	{
		LiveLinkClient.RemoveSource(Source);
		Source.Reset();
	}
	Source = MakeShared<FEarlSolverLiveLinkSource>();
	LiveLinkClient.AddSource(Source);
	if (!Source->CreateEarlSubject(LipSyncSubjectName, LookaheadMs, MoodIntensity))
	{
		UE_LOG(LogEarlOne, Error, TEXT("earl.LipRecipe: 서브젝트 재생성 실패 (%s)"), *LipSyncSubjectName.ToString());
		LiveLinkClient.RemoveSource(Source);
		Source.Reset();
		return;
	}
	Source->SetHeadMotion(HeadMotionAmplitudeDeg, HeadMotionSpeed);
	Source->SetIdleMouthDamp(true, IdleMouthDamp);
	Source->SetMouthGain(MouthCurveGain);
	Source->SetHeadNod(HeadNodDeg, HeadNodDurationSec);
	UE_LOG(LogEarlOne, Log, TEXT("earl.LipRecipe: %s 적용 — 게인 %.2f · Lookahead %dms (소스 재생성)"),
		bStock ? TEXT("A-스톡") : TEXT("B-레시피"), MouthCurveGain, LookaheadMs);
}

namespace
{
	// 립 레시피 A/B 전환: `earl.LipRecipe A|B`
	static FAutoConsoleCommandWithWorldAndArgs GEarlLipRecipeCmd(
		TEXT("earl.LipRecipe"),
		TEXT("Switch lip recipe live: earl.LipRecipe A (stock 1.0/80ms) | B (recipe 1.3/200ms)"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
			[](const TArray<FString>& Args, UWorld* World)
			{
				World = EarlResolveConsoleWorld(World); // 픽셀 스트리밍 원격 경로 폴백
				if (!World || Args.Num() < 1)
				{
					return;
				}
				const bool bStock = Args[0].Equals(TEXT("A"), ESearchCase::IgnoreCase);
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					if (UEarlLipSyncComponent* LipSync = It->FindComponentByClass<UEarlLipSyncComponent>())
					{
						LipSync->ApplyLipRecipe(bStock);
					}
				}
			}));
}

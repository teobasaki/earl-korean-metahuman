#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/EarlTypes.h"
#include "LipSync/EarlKoreanViseme.h"
#include "EarlLipSyncComponent.generated.h"

class FEarlLipSyncSource;

// 시선의 대화 상태 (2026-08-05).
// 사람의 시선은 "무엇을 하고 있는가"에 따라 통계가 달라진다 — 늘 같은 방식으로 배회하면
// 대화에 참여하지 않는 것처럼 보인다. 세 상태의 차이는 **정면 복귀 확률과 진폭**이다.
UENUM(BlueprintType)
enum class EEarlGazeState : uint8
{
	// 상대를 보고 있다. 대화 직후 몇 초 — "더 물어보실 게 있나요" 의 얼굴.
	Attentive  UMETA(DisplayName = "응시 (상대를 본다)"),
	// 말하는 중. 상대를 보되 이따금 시선을 뗀다(다음 말을 고르는 동작).
	Speaking   UMETA(DisplayName = "발화 중"),
	// 응시 시간이 지났고 추가 질문이 없다. 이제 주변을 본다.
	Idle       UMETA(DisplayName = "대기 (주변을 본다)")
};

// 문장 하나 동안 얼굴·시선·머리에 걸리는 성향 (2026-08-05, 행동목록 §3·§4).
// 감정 클립은 **표정**만 바꾼다 — 같은 문장도 감정마다 시선과 머리가 달라야 사람으로 읽힌다.
// 여기 값들은 감정·문장 종류(질문/설명/회상)에서 뽑아 문장이 끝날 때까지 유지된다.
USTRUCT(BlueprintType)
struct FEarlExpressionProfile
{
	GENERATED_BODY()

	// 윗눈꺼풀 열기 + 동공. "개념을 설명할 때 눈이 커진다" 가 이것이다.
	// 솔버는 이 커브를 만들지 않는다 — blink/cheekRaise/squintInner 뿐이다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float EyeWiden = 0.0f;
	// 머리 기울임(롤). 질문·장난기에서 사람은 고개를 갸웃한다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float RollDeg = 0.0f;
	// 머리 앞뒤(피치). 음수 = 앞으로 내밈(열의), 양수 = 뒤로(회상·웃음).
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float PitchDeg = 0.0f;
	// 시선을 이 방향에 붙들어 둔다 (예: 회상 = 우상단). 0 이면 평소대로 배회.
	UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector2D GazeAnchor = FVector2D::ZeroVector;
	// 붙들어 두는 시간(초). 문장 앞부분에만 걸린다.
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float GazeAnchorSec = 0.0f;
	// 시선 진폭 배수 (장난기 = 크게, 냉소 = 작게)
	UPROPERTY(EditAnywhere, BlueprintReadWrite) float GazeScaleMul = 1.0f;
};

UENUM(BlueprintType)
enum class EEarlLipSyncBackend : uint8
{
	EpicSolver     UMETA(DisplayName = "Epic NNE 솔버 (음향 직결)"),
	KoreanViseme   UMETA(DisplayName = "한국어 비짐 (텍스트 음소, 단독)"),
	Hybrid         UMETA(DisplayName = "하이브리드 (솔버 풀페이스+턱 / 비짐 입모양 덮어쓰기)")
};

// 버스 → 립싱크 배관 (T6). OnGeneratePCMData 를 구독해 다운믹스·16k 리샘플 후
// 재생 속도에 맞춰 20ms 프레임을 Live Link 소스에 밀어넣는다 (먼저 만든 페이싱 로직을 옮겼다).
// 캐릭터 액터(또는 아무 액터)에 부착. ABP 는 subject "Earl_TTS" 를 읽는다.
// (ABP_Earl_Face_AudioLiveLink 는 리터럴 "Earl_Audio" — ini 로 맞출 수 있게 Config)
UCLASS(ClassGroup=(Earl), meta=(BlueprintSpawnableComponent), Config=Game)
class EARLONE_API UEarlLipSyncComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEarlLipSyncComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync")
	FName LipSyncSubjectName = TEXT("Earl_TTS");

	// 얼굴 메시 직결 (2026-08-25): MetaHuman Creator 5.8 의 ABP_Face 에는 LiveLink 노드가
	// 아예 없다 — 윈도우에서 그 역할을 하던 팀 ABP_Earl_Face_AudioLiveLink 는 이 맥에 미전달.
	// 에셋 없이 AnimInstance 를 엔진 ULiveLinkInstance 로 바꿔 서브젝트를 직접 평가시킨다.
	// 포스트프로세스(RigLogic)는 메시 에셋 소유라 그대로 남아 커브→본 변환을 계속한다.
	UPROPERTY(Config, EditAnywhere, Category="Earl|LipSync")
	bool bDriveFaceMeshDirect = true;

	void TryBindFaceMesh();

	// 립 레시피 런타임 전환 (`earl.LipRecipe A|B`, 2026-08-25 A/B 시연용):
	// A = 스톡(게인 1.0 · Lookahead 80, 엔진 기본) / B = 윈도우 레시피(1.3 · 200).
	// Lookahead 는 솔버 서브젝트 생성 시 박히는 값이라 소스를 재생성한다 — 서브젝트명이
	// 같아 얼굴 바인딩(ULiveLinkInstance)은 그대로 이어진다.
	void ApplyLipRecipe(bool bStock);

	// 립싱크 백엔드 (ini 전환, A/B): EpicSolver = 범용 신경망 / KoreanViseme = 자체 텍스트 음소
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync")
	EEarlLipSyncBackend Backend = EEarlLipSyncBackend::EpicSolver;

	// KoreanViseme 단독: 프레임 발행 시작 지연(ms) — 오디오 StartDelayMs(90)와 LiveLink/ABP 경유
	// 지연(~30ms)의 차이를 보정해 스피커와 입을 맞춘다
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync", meta=(ClampMin="0", ClampMax="300"))
	int32 VisemeDelayMs = 60;

	// Hybrid: 비짐 트랙을 솔버 출력 타이밍에 맞추는 지연(ms). 솔버는 Lookahead 만큼 늦게
	// 애니를 내므로 대략 LookaheadMs 와 같게 시작해 미세조정한다
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync", meta=(ClampMin="0", ClampMax="500"))
	int32 HybridVisemeDelayMs = 200;

	// NNE 솔버 선행 버퍼(ms) — 클수록 안정, 지연 증가 (기본 80 = 최소 지연; 끊기면 상향)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync", meta=(ClampMin="80", ClampMax="240"))
	int32 LookaheadMs = 80;

	// 솔버 무드(감정 표정) 강도 — 1.0 은 표정이 입모양을 뭉갤 수 있어 발음 우선으로 낮춘다.
	// 표정 레이어는 ADR 005/010 의 감정 클립 블렌드가 담당 (솔버 무드는 보조).
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync", meta=(ClampMin="0.0", ClampMax="1.0"))
	float MoodIntensity = 0.5f;

	// ★ 무음에도 솔버를 살려 둔다 (2026-08-03).
	// 표정·얼굴 아이들·머리는 전부 **솔버 프레임에 얹혀** 나간다. 그런데 솔버는 PCM 이
	// 흐를 때만 프레임을 내므로, 말하지 않는 동안엔 얼굴이 통째로 정지했다
	// (실측: 무음 눈·눈썹 시간차분 0.246 vs 정지 대조군 0.120 — 사실상 멈춤).
	// 무음 PCM 을 계속 흘리면 솔버가 중립 얼굴을 계속 내고, 그 위에 우리 아이들·감정이 실린다.
	// 레퍼런스 데모가 자연스러운 이유(아이들이 항상 깔려 있음)를 우리 구조로 흉내 내는 것.
	// 비용: NNE 추론이 상시 돈다. 무거우면 False 로 끄면 예전 동작(무음 시 정지)으로 돌아간다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync")
	bool bKeepSolverAliveWhenIdle = true;

	// ── 절차적 머리 움직임 (2026-08-03) ──────────────────────────────────
	// 오디오 솔버는 얼굴 커브만 만들고 머리 포즈는 만들지 않는다. 반면 적용 쪽
	// (ABP_Face_PostProcess + CR_MetaHuman_HeadMovement_IK_Proc)은 이미 얼굴 메시에 걸려 있어
	// **받을 준비만 돼 있고 보내는 쪽이 비어 있었다.** 그 자리를 우리가 채운다.
	// 레퍼런스 데모는 캡처 클립에 머리 움직임이 들어 있어 이 문제가 없다(우리는 클립이 없다).
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Head", meta=(ClampMin="0.0", ClampMax="15.0"))
	float HeadMotionAmplitudeDeg = 2.5f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Head", meta=(ClampMin="0.0", ClampMax="5.0"))
	float HeadMotionSpeed = 1.0f;

	// ── 절차적 시선 (2026-08-05) ─────────────────────────────────────────
	// 오디오 솔버가 내는 눈 커브는 `blink / cheekRaise / squintInner` 뿐이고 **눈동자 방향이 없다**.
	// 그래서 지금까지 정면만 응시했고, 그게 "인형 같다"는 인상의 큰 지분이었다.
	// 릭에는 `CTRL_{L,R}_eye.t{x,y}` 가 있고 우리가 이미 쓰는 GUI→raw 변환기를 그대로 통과하므로,
	// 표정을 얹는 바로 그 경로에 실어 보낸다 — 배관을 새로 깔 필요가 없다.
	// 레퍼런스 데모는 이 층도 **캡처 클립에 구워져** 있다(우리는 클립이 없으니 절차적으로 만든다).
	//
	// 모델: 사람 눈은 고정돼 있지 않다. 0.9~2.8초마다 **사케이드**(툭 옮겨감)가 일어나고
	// 그 사이에는 미세 표류를 한다. 대화 중에는 상대 쪽(정면)으로 자주 돌아온다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GazeAmplitude = 0.25f;

	// 사케이드 간격(초). 이 범위에서 무작위로 다음 시각을 잡는다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.1", ClampMax="10.0"))
	float GazeSaccadeMinSec = 0.9f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.1", ClampMax="10.0"))
	float GazeSaccadeMaxSec = 2.8f;

	// 목표까지 가는 속도(1/s). 사람 사케이드는 30~50ms 에 끝나므로 **크게** 잡는다.
	// 작게 잡으면 눈이 흐물흐물 미끄러져 오히려 부자연스럽다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="1.0", ClampMax="100.0"))
	float GazeSaccadeSpeed = 26.0f;

	// 사케이드 사이의 미세 표류 진폭 (고정 응시도 사람에겐 없다)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="0.2"))
	float GazeDriftAmplitude = 0.025f;

	// 사케이드가 **정면 복귀**일 확률 — 상태별로 다르다.
	// 응시: 거의 상대만 본다 / 발화: 이따금 시선을 뗀다 / 대기: 주변을 훑는다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GazeReturnChanceAttentive = 0.88f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GazeReturnChanceSpeaking = 0.62f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GazeReturnChanceIdle = 0.35f;

	// 상태별 진폭 배수. 응시 중에는 눈이 크게 돌지 않는다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="2.0"))
	float GazeAmplitudeScaleAttentive = 0.45f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="2.0"))
	float GazeAmplitudeScaleSpeaking = 0.8f;

	// ★ 응시 상태에서 얼마나 **또렷하게** 상대에 고정할 것인가 (0~1, 2026-08-05).
	// 1 이면 정면 복귀 목표의 흩뜨림과 미세 표류를 거의 없애 눈이 상대에 딱 붙는다.
	// 0 이면 예전 동작(응시 중에도 계속 미세하게 배회). 발화·대기 상태에는 걸리지 않는다 —
	// 거기서는 눈이 움직이는 것이 정상이다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GazeAttentiveLock = 0.8f;

	// ★ 발화가 끝난 뒤 상대를 계속 보는 시간(초).
	// "대화 끝나면 몇 초간 대화자 응시하다가 추가 질문 없으면 다른 곳도 본다" —
	// 이 창이 열려 있는 동안이 Attentive, 지나면 Idle 이다. 새 발화가 오면 다시 열린다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="60.0"))
	float GazeAttentionSec = 7.0f;

	// ★ 관심이 식는 데 걸리는 시간(초) — 2026-08-06.
	// 0 이면 예전 동작이다: `GazeAttentionSec` 이 지나는 **한 프레임**에 응시에서 대기로
	// 뒤집히고 그 자리에서 눈이 튄다. 사용자가 "기계적"이라고 한 게 그 계단이다.
	// 0 보다 크면 창이 닫히기 이 시간 전부터 주의가 연속으로 풀린다 — 복귀 확률·진폭·
	// 응시 고정·미세 표류가 전부 그 값으로 섞여, 어느 프레임에 바뀌었는지 눈에 안 띈다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="15.0"))
	float GazeDisengageSec = 3.5f;

	// ★ 응시 시간의 문장별 흔들림(비율). 항상 정확히 같은 초에 시선을 떼면 타이머로 읽힌다.
	// 0.35 면 7초가 문장마다 4.6~9.5초 사이에서 뽑힌다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="0.8"))
	float GazeAttentionJitter = 0.35f;

	// ★ 머리가 돌아가도 눈은 상대에 남는다 (전정안반사).
	// 절차적 머리 움직임만큼 시선을 **반대로** 밀어 주지 않으면, 머리가 돌 때 눈도 같이 끌려가
	// 상대에서 벗어난다. 0 이면 보정 없음(예전 동작).
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="1.0"))
	float GazeHeadCompensation = 1.0f;

	// 눈 컨트롤 1.0 이 대략 몇 도에 해당하는가 (머리 각도 → 시선 단위 환산용)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="5.0", ClampMax="90.0"))
	float GazeDegreesPerUnit = 30.0f;

	// ── 머리 추종 (2026-08-05) ────────────────────────────────────────────
	// 사람은 큰 시선 이동을 **눈만으로** 하지 않는다. 눈이 먼저 튀고, 0.1~0.2초 뒤에 머리가
	// 그 방향으로 따라간다. 머리가 몫을 가져가면 눈은 그만큼 중앙으로 되돌아온다 —
	// 그 되돌림은 위 전정안반사가 자동으로 해 준다(머리 각도만큼 눈을 반대로 밀므로).
	// 즉 전체 시선 = 머리 + 눈 이 목표에 그대로 유지되고, 분배만 바뀐다.
	// 지금까지 머리와 시선이 **완전히 따로 놀던 것**이 "인형" 인상의 핵심이었다.
	//
	// ⚠️ 부호: 시선 +X 와 머리 +Yaw 의 방향이 반대면 머리가 시선 반대편으로 간다.
	//    그때는 이 게인을 **음수**로 주면 된다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="-1.0", ClampMax="1.0"))
	float HeadFollowGain = 0.45f;

	// 눈이 먼저 가고 머리가 따라오기까지의 시간(초). 0 이면 동시에 움직여 로봇처럼 보인다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="0.6"))
	float HeadFollowLatencySec = 0.15f;

	// 머리 스프링의 고유진동수(rad/s). 눈(26)보다 **훨씬 느려야** 한다 — 머리는 무겁다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="1.0", ClampMax="30.0"))
	float HeadFollowSpeed = 5.5f;

	// 감쇠비. 1.0 = 넘지 않고 부드럽게 정착 / 1.0 미만 = 살짝 넘었다가 되돌아온다.
	// 선형 보간은 목표에서 속도가 뚝 끊겨 로봇처럼 보인다 — 사람 목은 관성이 있다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.3", ClampMax="2.0"))
	float HeadFollowDamping = 0.75f;

	// 이보다 작은 시선 이동에는 머리가 아예 안 움직인다 (곁눈질은 눈만으로 한다)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Gaze", meta=(ClampMin="0.0", ClampMax="0.5"))
	float HeadFollowMinGaze = 0.10f;

	// ── 몸 추종 (2026-08-10) ────────────────────────────────────────────────
	// 신고: "몸이랑 얼굴이랑 따로 논다".
	// 원인: MetaHuman 은 Body 와 Face 가 **별개 스켈레탈 메시**이고 ARDY 리타깃은 Body 만
	//       건드린다. 눈에 보이는 머리는 Face 메시인데 그것이 Body 머리뼈를 안 따라온다.
	// 실측: 얼굴 자체 머리 모션을 끄고(`earl.Head 0 1`) 몸을 크게 움직였을 때
	//       Face 머리의 추종 비율이 **0.02** — 사실상 0이었다.
	// 얼굴 AnimBP 에 `Copy Pose From Mesh`(use_attached_parent=True) 노드가 있는데도
	// 머리에는 안 먹는다(연결 또는 포스트프로세스 컨트롤 리그가 덮는 것으로 보인다).
	// 그래서 **얼굴이 실제로 반응하는 채널**(HeadYaw/Pitch/Roll)에 몸의 머리 각을 실어 준다.
	// 이 경로는 이미 검증돼 있다 — `earl.Head` 로 껐다 켜면 얼굴 머리가 실제로 바뀐다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|BodyFollow")
	bool bBodyHeadFollow = true;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|BodyFollow", meta=(ClampMin="0.0", ClampMax="1.5"))
	float BodyHeadFollowGain = 1.0f;

	// 몸 추종분이 붙는 속도(1/s). 리타깃 결과가 20fps 로 들어오므로 살짝 매끈하게 편다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|BodyFollow", meta=(ClampMin="1.0", ClampMax="60.0"))
	float BodyHeadFollowSpeed = 14.0f;

	// 축 부호. 뼈 축 관례가 화면 방향과 다르면 실측으로 뒤집는다 — `earl.BodyFollow` 로 라이브 조정.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|BodyFollow")
	float BodyFollowYawSign = 1.0f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|BodyFollow")
	float BodyFollowPitchSign = 1.0f;
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|BodyFollow")
	float BodyFollowRollSign = 1.0f;

	// ── 문장 프로파일 (2026-08-05, 행동목록 §3·§4) ────────────────────────
	// 프로파일이 목표에 붙는 속도(1/s). 표정처럼 천천히 붙어야 자연스럽다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Profile", meta=(ClampMin="0.5", ClampMax="10.0"))
	float ProfileBlendSpeed = 2.5f;

	// 눈 커짐 전체 게인. **양수 = 크게 뜬다** (릭의 부호 반전은 구현부에서 이미 처리했다).
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Profile", meta=(ClampMin="-1.0", ClampMax="1.5"))
	float EyeWidenGain = 0.6f;

	// 설명(mode=Fact) 문장에서 눈을 뜨는 확률. 도슨트 답변 대부분이 Fact 라 매번 걸면
	// 특징이 아니라 기본 표정이 된다 — "커지기도 하며" 여야 한다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Profile", meta=(ClampMin="0.0", ClampMax="1.0"))
	float ExplainWidenChance = 0.4f;

	// 동공 확장은 눈 커짐에 비례해 아주 약하게만 (과하면 만화가 된다)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Profile", meta=(ClampMin="0.0", ClampMax="1.0"))
	float PupilGain = 0.25f;

	// ── 깜빡임 (2026-08-05, 행동목록 §1 A4) ───────────────────────────────
	// 깜빡임을 **절차적으로 소유**한다. 예전엔 감정 클립의 `eyeBlink` 커브를 그대로 썼는데,
	// 그 커브는 깜빡임이 아니라 **지속적인 눈웃음 감김**이라(0.3~0.55 유지) 말하는 내내
	// 눈을 반쯤 감고 있었다. 여기서 만들면 "감겼다 뜬다"가 온전하고 지속 감김이 생길 수 없다.
	//
	// 그리고 사람은 **큰 시선 이동에 맞춰 깜빡인다** — 사케이드와 동기화하면 눈이 훨씬 산다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.0", ClampMax="1.5"))
	float BlinkAmplitude = 1.0f;

	// 자발 깜빡임 간격(초). 사람은 대략 3~6초에 한 번, 말할 때 조금 더 자주 깜빡인다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.5", ClampMax="30.0"))
	float BlinkMinSec = 2.6f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.5", ClampMax="40.0"))
	float BlinkMaxSec = 6.5f;

	// 발화 중 간격 배수 (1 미만 = 더 자주)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.3", ClampMax="2.0"))
	float BlinkSpeakingIntervalScale = 0.8f;

	// ★ 큰 사케이드에서 같이 깜빡일 확률 (행동목록 A4)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.0", ClampMax="1.0"))
	float BlinkOnSaccadeChance = 0.55f;

	// 감김·유지·뜸 시간(초). 사람 깜빡임은 **감기는 것이 뜨는 것보다 빠르다**.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.02", ClampMax="0.3"))
	float BlinkCloseSec = 0.06f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.0", ClampMax="0.3"))
	float BlinkHoldSec = 0.035f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Blink", meta=(ClampMin="0.02", ClampMax="0.5"))
	float BlinkOpenSec = 0.13f;

	// ── 아이들 몸짓 (§1 A5·A7) ────────────────────────────────────────────
	// 침 삼킴 — 대기 중 8~20초마다. 이것 하나로 "살아 있다"가 크게 올라간다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Idle", meta=(ClampMin="0.0", ClampMax="1.0"))
	float SwallowGain = 0.7f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Idle", meta=(ClampMin="1.0", ClampMax="60.0"))
	float SwallowMinSec = 8.0f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Idle", meta=(ClampMin="1.0", ClampMax="90.0"))
	float SwallowMaxSec = 20.0f;

	// 대기 중 눈썹 미세 플래시 — 발화 강세보다 훨씬 약하게
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Idle", meta=(ClampMin="0.0", ClampMax="0.5"))
	float IdleBrowGain = 0.12f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Idle", meta=(ClampMin="1.0", ClampMax="60.0"))
	float IdleBrowMinSec = 5.0f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Idle", meta=(ClampMin="1.0", ClampMax="90.0"))
	float IdleBrowMaxSec = 15.0f;

	// 런타임 조절 (콘솔 `earl.Gaze <진폭> [최소초] [최대초]`, 진폭 0 = 끔)
	UFUNCTION(BlueprintCallable, Category="Earl|LipSync|Gaze")
	void ApplyGazeSettings(float InAmplitude, float InMinSec, float InMaxSec);

	// ── 발화 강세 (2026-08-05) ────────────────────────────────────────────
	// 말과 얼굴을 잇는 층. 단어별 타임스탬프가 없으므로(ADR 011 — 음소 정렬 미확보)
	// **PCM 진폭 포락선**을 강세의 대용으로 쓴다. 솔버로 밀어넣는 청크에서 그대로 뽑으므로
	// 신호 확보 비용이 0 이다.
	// 판정은 **비율**로 한다(빠른 포락선 / 느린 기준선) — 절대 임계를 쓰면 TTS·음량을 바꿀 때마다
	// 다시 맞춰야 한다. SAPI 22050 사건(T66)과 같은 종류의 함정이다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="0.0", ClampMax="1.0"))
	float AccentBrowGain = 0.35f;

	// 머리 끄덕임 각도(도). 0 이면 눈썹만. 부호를 뒤집으면 방향이 바뀐다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="-10.0", ClampMax="10.0"))
	float HeadNodDeg = 2.2f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="0.1", ClampMax="1.5"))
	float HeadNodDurationSec = 0.45f;

	// 기준선 대비 몇 배면 강세로 볼 것인가
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="1.0", ClampMax="4.0"))
	float AccentThresholdRatio = 1.55f;

	// 이보다 조용하면 무시 (무음 구간의 잡음이 강세로 잡히는 것 방지)
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="0.0", ClampMax="0.5"))
	float AccentMinLevel = 0.02f;

	// 연속 발동 방지. 너무 짧으면 눈썹이 떨리는 것처럼 보인다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="0.1", ClampMax="3.0"))
	float AccentCooldownSec = 0.5f;

	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="0.05", ClampMax="1.0"))
	float AccentDurationSec = 0.45f;

	// 상승 시간. 사람 눈썹은 **툭** 올라갔다 천천히 내려온다 — 상승이 하강보다 짧아야 한다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync|Accent", meta=(ClampMin="0.02", ClampMax="0.5"))
	float AccentAttackSec = 0.13f;

	// 런타임 조절 (콘솔 `earl.Accent <눈썹게인> [끄덕임도] [임계비율]`, 게인 0 = 끔)
	UFUNCTION(BlueprintCallable, Category="Earl|LipSync|Accent")
	void ApplyAccentSettings(float InBrowGain, float InNodDeg, float InThresholdRatio);

	// 조음 계측 보고 (콘솔 `earl.LipMeter [reset]`). 화면이 아니라 커브에서 잰다.
	UFUNCTION(BlueprintCallable, Category="Earl|LipSync")
	void ReportArticulation(bool bReset);

	// 무음 중 입·턱을 닫는 정도 (1 = 완전히 닫음). 솔버가 디지털 무음에도 입을 살짝 벌리기 때문에
	// 필요하다 — 안 그러면 대기 중에 계속 입을 벌리고 있다(2026-08-03 A/B 실측).
	// 발화 중에는 적용되지 않는다.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync", meta=(ClampMin="0.0", ClampMax="1.0"))
	float IdleMouthDamp = 1.0f;

	// 입 커브 게인 — 윈도우 확정 레시피(ABP ModifyCurve ×1.25~1.35)의 코드 이식. 1.0 = 원출력.
	UPROPERTY(Config, EditAnywhere, BlueprintReadWrite, Category="Earl|LipSync", meta=(ClampMin="0.5", ClampMax="2.0"))
	float MouthCurveGain = 1.3f;

	// 런타임에 머리 움직임을 바꾼다 (콘솔 `earl.Head <진폭> [속도]`). ini 값은 재시작해야 반영되므로
	// 눈으로 보며 맞추려면 이 경로가 필요하다.
	UFUNCTION(BlueprintCallable, Category="Earl|LipSync|Head")
	void ApplyHeadMotionSettings(float InAmplitudeDeg, float InSpeed);

	UFUNCTION()
	void HandlePCMData(int32 SampleRate, int32 NumChannels, const TArray<uint8>& PCMData);

	UFUNCTION()
	void HandlePlaybackFinished();

	// KoreanViseme: 문장 시계에서 텍스트 확보 (다음 PCM 브로드캐스트와 짝)
	UFUNCTION()
	void HandleSentenceStarted(const FEarlSentencePayload& Sentence);

	// 감정 표정을 솔버 프레임에 가산 (커브 마스크 additive). GUI 컨트롤명(`CTRL_expressions_*`) → 값.
	// UEarlEmotionComponent 가 매 틱 호출한다. 별도 LiveLink subject 를 쓰지 않는 이유는
	// FEarlTTSLiveLinkSubject::SetRawExpressionDelta 주석 참조 (★19).
	void SetExpressionCurves(const TMap<FString, float>& GuiCurves);

private:
	TSharedPtr<class FEarlSolverLiveLinkSource> Source;
	TSharedPtr<FEarlLipSyncSource> VisemeSource;
	bool bFaceMeshBound = false;
	float FaceBindRetryAccum = 0.0f;

	// KoreanViseme 상태: 문장 텍스트 큐 + 20ms 커브 프레임 큐
	TArray<TArray<EarlKoreanViseme::FSyllable>> PendingSyllables;
	TArray<TMap<FName, float>> FrameQueue;
	int32 FrameReadOffset = 0;
	double FrameClock = 0.0;
	double VisemeStartAt = -1.0;

	// 모노 누적 버퍼 + 재생 페이싱 (UpdateLipSyncStreaming 에서 옮겼다).
	// NNE 솔버 경로는 원음 샘플레이트(44.1k) 그대로 공급 — 우리 선형보간 16k 다운샘플은
	// 안티앨리어싱이 없어 치찰음 대역이 접혀 자음 비짐 정확도를 깎는다 (2026-07-29 관찰).
	// 절차적 폴백만 16k 로 변환.
	TArray<float> PendingSamples;
	int32 ReadOffset = 0;
	double SampleAccumulator = 0.0;
	int32 StreamSampleRate = 16000; // 현재 버퍼의 샘플레이트 (솔버=원음, 절차적=16k)

	// 무음 유지용 별도 누산기 — 발화 시작 버스트 크레딧(SampleAccumulator)과 섞이면 안 된다
	double IdleAccumulator = 0.0;
	bool bLoggedIdleKeepAlive = false;

	// 재생 중인데 립싱크 버퍼가 먼저 비는 구간을 계측한다(입이 일찍 멎는 원인 추적)
	bool bSpeaking = false;
	double SpeechStartTime = -1.0;
	bool bLoggedStarvation = false;

	// 강세 검출 — 솔버로 보내는 청크에서 진폭 포락선을 낸다 (게임 스레드 전용)
	void UpdateSpeechAccent(const TArray<float>& InChunk, float DeltaTime);
	float FastEnvelope = 0.0f;
	float SlowEnvelope = 0.0f;
	double LastAccentAt = -10.0;
	float AccentTimeRemaining = 0.0f;
	float AccentStrength = 0.0f;
	int32 AccentCount = 0;
	bool bLoggedAccent = false;

	// 시선 상태 (게임 스레드 전용)
	void UpdateGaze(float DeltaTime);
	FVector2D GazeCurrent = FVector2D::ZeroVector;
	FVector2D GazeTarget = FVector2D::ZeroVector;
	double NextSaccadeAt = -1.0;
	double GazeClock = 0.0;
	bool bLoggedGaze = false;

	// 대화 상태 — 발화 종료 시 AttentionUntil 이 갱신되고, 그 시각이 지나면 Idle 로 떨어진다
	EEarlGazeState GazeState = EEarlGazeState::Attentive;
	double AttentionUntil = -1.0;

	// 문장 프로파일 (감정·질문·설명·회상). 문장이 끝나면 서서히 0 으로 돌아간다.
	FEarlExpressionProfile ActiveProfile;
	FEarlExpressionProfile BlendedProfile;
	double GazeAnchorUntil = -1.0;
	void ApplySentenceProfile(const FEarlSentencePayload& Sentence);

	// 아이들 몸짓 — 침 삼킴·눈썹 플래시 (§1 A5·A7)
	double NextSwallowAt = -1.0;
	float SwallowTimeRemaining = 0.0f;
	double NextIdleBrowAt = -1.0;
	float IdleBrowTimeRemaining = 0.0f;
	bool bLoggedSwallow = false;
	bool bLoggedIdleBrow = false;

	// 깜빡임 — 진행 시각(초). 0 이상이면 진행 중.
	float BlinkElapsed = -1.0f;
	double NextBlinkAt = -1.0;
	bool bLoggedBlink = false;
	void TriggerBlink();
	float EvaluateBlink(float DeltaTime);

	// 머리 추종 — 눈이 먼저 가고 머리가 지연 뒤에 따라간다 (단위: 도)
	// 몸(Body 메시) 머리뼈에서 읽어 온 각도, 매끈하게 편 값 (yaw, pitch, roll)
	FVector BodyHeadCurrent = FVector::ZeroVector;
	// 매 틱 월드를 뒤지지 않으려고 캐릭터 Body 메시를 캐시한다 (무효화되면 다시 찾는다)
	TWeakObjectPtr<USkeletalMeshComponent> BodyMeshCache;

	FVector2D HeadFollowCurrent = FVector2D::ZeroVector;
	FVector2D HeadFollowVelocity = FVector2D::ZeroVector;
	FVector2D HeadFollowTarget = FVector2D::ZeroVector;
	FVector2D PendingFollowTarget = FVector2D::ZeroVector;
	double PendingFollowAt = -1.0;
};

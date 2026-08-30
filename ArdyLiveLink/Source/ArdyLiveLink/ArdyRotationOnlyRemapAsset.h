#pragma once

#include "CoreMinimal.h"
#include "LiveLinkRemapAsset.h"
#include "ArdyRotationOnlyRemapAsset.generated.h"

/**
 * ARDY 스트림 전용 Live Link 리타깃 에셋 — **회전만 적용한다.**
 *
 * 왜 필요한가 (2026-08-06 실측):
 *   ARDY 와이어 프로토콜에는 **본 오프셋이 없다.** 실제로 뜯어보면
 *     {"type":"skeleton","v":1,"bones":[{"name":"Hips","parent":-1}, ...]}   ← 이름+부모뿐
 *     {"type":"frame","idx":0,"t":0.0,"rot":[[x,y,z,w], ...]}               ← 쿼터니언뿐
 *   즉 뼈 길이는 **받는 쪽 스켈레톤의 레퍼런스 포즈**에서 와야 한다.
 *
 *   그런데 기본 `ULiveLinkRemapAsset` 은 프레임의 트랜스폼을 **통째로**(회전+이동) 적용한다.
 *   우리 소스는 비루트 본에 translation 0 을 넣으므로, 그대로 쓰면 **모든 본 오프셋이 0 이 되어
 *   스켈레톤이 한 점으로 접힌다.** 실측: ARDY 소스 메시 바운드 높이가 170cm 가 아니라 **20cm**.
 *
 *   같은 문제를 다른 층에서 푸는 접근도 있다 —
 *   레퍼런스 로컬 포즈를 복사해 두고 회전 델타만 곱한다. 여기서는 Live Link 리타깃 단계에서
 *   같은 일을 한다: `OutPose` 는 호출자가 **레퍼런스 포즈로 초기화해서** 넘겨주므로,
 *   회전만 덮어쓰면 이동은 레퍼런스 값이 그대로 남는다.
 *
 * 루트만 예외로 이동을 받는다 — 캐릭터가 공간에서 움직여야 하고, 그 값은 스트림이 준다.
 */
UCLASS(Blueprintable)
class ARDYLIVELINK_API UArdyRotationOnlyRemapAsset : public ULiveLinkRemapAsset
{
	GENERATED_BODY()

public:
	virtual void BuildPoseFromAnimationData(
		float DeltaTime,
		const FLiveLinkSkeletonStaticData* InSkeletonData,
		const FLiveLinkAnimationFrameData* InFrameData,
		FCompactPose& OutPose) override;

	/**
	 * 루트 본에 스트림의 이동을 적용할지.
	 *
	 * ⚠️ **기본 false** (2026-08-06). 이유:
	 *   루트 이동은 새 EAxisFix 프리셋에서 m→cm 로 ×100 되어 들어온다(예: 92cm).
	 *   그런데 소스 메시 `ARDY_explain_both_01` 은 **자체 스케일이 작다** — 1.7m 인체가
	 *   UE 유닛으로 20 밖에 안 된다(액터 스케일 8.75 로 키워 쓴다).
	 *   그 메시에 92 유닛짜리 루트 이동을 얹으면 스켈레톤이 **몸 높이의 수백 배 밖으로
	 *   튕겨 나가** 화면에서 사라진다. 실제로 그것 때문에 한참 헤맸다.
	 *
	 *   축(EAxisFix) 판정에는 제자리 회전만 있으면 충분하다. 월드 이동이 필요해지면
	 *   **소스 메시 스케일과 cm 변환을 먼저 정합시킨 뒤** 켤 것.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ardy")
	bool bApplyRootTranslation = false;

	/**
	 * ★ 들어온 회전을 **절대값이 아니라 레퍼런스 포즈에 대한 델타**로 해석한다 (2026-08-06).
	 *
	 * 왜 이 스위치가 있나:
	 *   팀 파이프라인(`ardy.motion.v1`)은 프로토콜 헤더에 명시돼 있듯
	 *   "Unreal-coordinate, local-space rotation **deltas**" 를 보내고, 수신부는
	 *   `Bone.SetRotation(Bone.GetRotation() * Joint.RotationDelta)` 로 **레퍼런스에 곱한다.**
	 *   우리 `ArdyLiveLink` 는 ARDY 기저의 **절대 로컬 회전**을 그대로 대입해 왔다.
	 *
	 *   그런데 1차 스윕에서 축 프리셋 6종 × retarget pose 3종 = 18조합이 **전부 누웠다**
	 *   (골반→머리 최대 +46cm, 정상은 +60~70). 축을 어떻게 돌려도 몸이 펴지지 않는다는 것은
	 *   **해석 자체가 틀렸다**는 신호다. 그래서 팀과 같은 해석을 시험할 수 있게 뺐다.
	 *
	 *   true  = RefRot * Incoming  (팀 방식, 델타 해석)
	 *   false = Incoming           (기존, 절대 해석)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ardy")
	bool bTreatRotationAsDelta = false;

	/**
	 * ★★ **ref 프레임 보정** — 이게 진짜 해답이다 (2026-08-06).
	 *
	 * 맥 세션이 `ardy-stream/samples` 22개 전수로 증명한 것:
	 *   ARDY 는 27본의 rest 프레임이 **전부 월드 축과 일치**한다(모든 local 이 항등이면
	 *   global 도 항등, 실측 오차 1.19e-07). 반면 UE 소스 메시의 ref 스켈레톤은
	 *   **본마다 프레임이 다르다** — Hips 56°, Spine 80°, Neck 100°, RightFoot 160°,
	 *   심지어 LeftArm 은 +Z / RightArm 은 −Z 로 좌우 축까지 다르다.
	 *
	 *   그래서 필요한 변환은 본마다 다르다:
	 *       Δ_b = conj(Ref_b) ⊗ (C · L_b · Cᵀ) ⊗ Ref_b
	 *   EAxisFix 는 **전역 상수 하나**(C 자리)라 27개의 서로 다른 켤레 변환을 동시에
	 *   만족할 수 없다. 축 프리셋 6종을 전수 조사해도 답이 없었던 이유가 이것이다. ∎
	 *
	 * 그런데 `Ref_b` 는 송신기만 아는 값이 아니다 — **소스 메시의 레퍼런스 포즈가 곧 그것**이라
	 * 수신부인 여기서 계산할 수 있다. 그래서 송신기를 바꾸지 않고도 고칠 수 있다:
	 *
	 *       Local_b = RefLocal_b ⊗ conj(Ref_b) ⊗ (C · L_b · Cᵀ) ⊗ Ref_b
	 *
	 *   · `C · L_b · Cᵀ` = 쿼터니언으로 (x, z, y, −w) = 우리 `SwapYZConj` 와 동일
	 *     (맥 세션이 잔차 4.4e-16 으로 확인 — "계측은 옳았다, 부족했을 뿐이다")
	 *   · `RefLocal_b` = 그 본의 로컬 ref 회전
	 *   · `Ref_b`      = 그 본의 **컴포넌트공간** ref 회전 (부모 체인 누적)
	 *
	 * 끄면 예전 동작(축 보정만) — 그건 원리적으로 설 수 없다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ardy")
	bool bUseRefFrameCorrection = true;

	/**
	 * ★ 목·머리 본을 **ARDY 로 구동하지 않는다** (기본 켬, 2026-08-06).
	 *
	 * 증상: 몸은 ARDY 로 움직이는데 "머리와 몸이 따로 노는" 느낌.
	 * 원인: 우리 TCP 스트림은 27본을 **전부** 보내서 `Neck`·`Head` 까지 ARDY 가 돌린다.
	 *       그런데 그 본들은 이미 **얼굴 경로의 소유**다 —
	 *       절차적 머리 움직임(EarlSolverLiveLinkSource::ApplyHeadMotion) + 시선 + 립싱크가
	 *       같은 본을 쓴다. 두 소스가 한 본을 놓고 싸우니 목이 몸과 어긋난다.
	 *
	 * 팀 계약도 같은 결론이다 — `ardy.motion.v1` 은 `--include-head` 없이는 목·머리를
	 * 아예 보내지 않고(25본), 수신기가 `IsBlockedBone` 으로 조용히 건너뛴다.
	 * 오늘 바디 아이들 클립에서 머리가 사라진 것도 같은 계열의 충돌이었다.
	 *
	 * → 몸은 ARDY, 머리는 얼굴 경로. 소유권을 나눈다.
	 *
	 * ⚠️ **목까지 막으면 안 된다** (2026-08-06 사용자 피드백: "목이 무슨 몸이랑 붙어있어").
	 *   Neck 을 막으면 목이 레퍼런스 각도로 굳어 **몸통에 용접된 것처럼** 보인다.
	 *   몸이 40° 숙이는데 목이 안 따라가니 머리가 통째로 딸려 내려간다.
	 *   목은 몸의 일부다 — ARDY 가 움직이는 게 맞고, **머리(Head)만** 얼굴 경로에 남긴다.
	 *
	 *   0 = 차단 없음 (ARDY 가 목·머리 다 돌린다 — 얼굴 경로와 싸운다)
	 *   1 = Head 를 `HeadArdyWeight` 만큼만 반영 (기본, 권장)
	 *   2 = Neck + Head 차단 (목이 굳는다 — 비교용으로만)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ardy", meta=(ClampMin="0", ClampMax="2"))
	int32 HeadBlockMode = 1;

	/**
	 * `HeadBlockMode==1` 일 때 머리가 ARDY 를 **얼마나** 따라갈지 (0..1). 2026-08-09 신설.
	 *
	 * 원래는 전부/전무였다. 그런데 완전 차단이면 **목은 도는데 머리만 레퍼런스로 굳어**
	 * 몸이 크게 움직일수록 머리가 얹힌 별개 물체처럼 보인다
	 * (사용자 신고: "동작할 때마다 머리통만 따로 노는 느낌").
	 * 반대로 100% 받으면 립싱크·시선과 싸운다 — 그래서 원래 막았던 것이다.
	 *
	 *   0.0 = 예전 동작(완전 차단)
	 *   1.0 = 기본 (2026-08-10). "외형은 ARDY, 표정은 우리 것" 구조 확정 —
	 *         Body 머리는 ARDY 가 온전히 갖고, Face 가 Body 를 커브 채널로 따라오며
	 *         (EarlLipSyncComponent 몸 추종), 표정·시선은 그 위에 얹힌다.
	 *         0.5 로 낮추면 몸 자체의 머리 동작이 절반이 되어 얼굴도 절반만 따라온다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ardy", meta=(ClampMin="0.0", ClampMax="1.0"))
	float HeadArdyWeight = 1.0f;
};

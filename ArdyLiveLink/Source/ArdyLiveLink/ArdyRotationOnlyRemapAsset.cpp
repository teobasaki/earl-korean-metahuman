#include "ArdyRotationOnlyRemapAsset.h"

#include "BonePose.h"
#include "Roles/LiveLinkAnimationTypes.h"

void UArdyRotationOnlyRemapAsset::BuildPoseFromAnimationData(
	float DeltaTime,
	const FLiveLinkSkeletonStaticData* InSkeletonData,
	const FLiveLinkAnimationFrameData* InFrameData,
	FCompactPose& OutPose)
{
	if (!InSkeletonData || !InFrameData)
	{
		return;
	}

	// ★ 매핑은 **엔진 기본 리맵과 똑같은 API** 로 한다 (2026-08-06).
	//   `ULiveLinkRemapAsset::BuildPoseFromAnimationData` 구현을 읽고 그대로 따랐다:
	//     GetRemappedBoneName → GetPoseBoneIndexForBoneName → MakeCompactPoseIndex
	//   처음엔 `GetPoseToSkeletonBoneIndexArray().Find()` 로 직접 계산했다가 무효 인덱스가
	//   양수로 나와 **엉뚱한 본에 회전을 써 넣었고 스켈레톤이 폭발했다**(키 175인데
	//   Hips→Head 646). 인덱스 변환은 절대 손으로 하지 말 것.
	//
	// Super 를 부르지 않고 직접 도는 이유:
	//   ① 이동을 레퍼런스로 되돌려야 한다 — ARDY 와이어에는 본 오프셋이 없어서
	//      기본 리맵이 넣는 translation 0 을 그대로 두면 스켈레톤이 한 점으로 접힌다.
	//   ② 델타 해석을 **스트림이 실제로 건드린 본에만** 적용해야 한다.
	//      Super 뒤에 전체 본을 순회하며 곱했더니 스트림과 무관한 327개 본까지
	//      레퍼런스 회전이 이중으로 걸렸다(첫 델타 실험이 무효였던 이유).
	const TArray<FName>& SourceBoneNames = InSkeletonData->GetBoneNames();
	const int32 Num = FMath::Min(SourceBoneNames.Num(), InFrameData->Transforms.Num());
	const FBoneContainer& Bones = OutPose.GetBoneContainer();

	// ★ 컴포넌트공간 ref 회전(Ref_b)을 미리 구한다 — ref 프레임 보정에 필요하다.
	// 부모가 항상 먼저 오는 순서라 한 번 훑으면 누적된다(소스 메시는 28본이라 비용 무시 가능).
	TArray<FQuat> RefComp;
	if (bUseRefFrameCorrection)
	{
		RefComp.SetNum(OutPose.GetNumBones());
		for (const FCompactPoseBoneIndex Idx : OutPose.ForEachBoneIndex())
		{
			const FQuat Local = OutPose.GetRefPose(Idx).GetRotation();
			const FCompactPoseBoneIndex Parent = Bones.GetParentBoneIndex(Idx);
			RefComp[Idx.GetInt()] = (Parent.GetInt() != INDEX_NONE)
				? (RefComp[Parent.GetInt()] * Local)
				: Local;
		}
	}

	int32 Mapped = 0;
	int32 Blocked = 0;
	for (int32 i = 0; i < Num; ++i)
	{
		// ★ 머리는 얼굴 경로 소유 — ARDY 가 건드리면 립싱크·시선과 싸운다.
		//   목은 **막지 않는다.** 막으면 몸이 숙일 때 목이 안 따라가 용접된 것처럼 굳는다.
		//
		// ★ 2026-08-09 — 전부/전무를 **비율**로 바꿨다.
		//   신고: "동작할 때마다 머리통만 따로 노는 느낌이다".
		//   원인이 바로 여기였다. ARDY 는 머리 회전을 **보내고 있는데** 우리가 통째로 버렸다.
		//   목은 ARDY 를 따라 도는데 머리만 레퍼런스 그대로라, 몸이 크게 움직일수록
		//   머리가 몸에 얹힌 별개 물체처럼 보인다.
		//   그렇다고 100% 받으면 립싱크·시선과 싸운다(그래서 원래 막았던 것이다).
		//   → `HeadArdyWeight` 만큼만 섞는다. 나머지는 얼굴 경로 몫으로 남는다.
		float BoneWeight = 1.0f;
		{
			const FString Src = SourceBoneNames[i].ToString();
			const bool bIsHead = Src.StartsWith(TEXT("Head"));
			const bool bIsNeck = Src.StartsWith(TEXT("Neck"));
			if (HeadBlockMode > 0 && bIsHead)
			{
				BoneWeight = FMath::Clamp(HeadArdyWeight, 0.0f, 1.0f);
			}
			if (HeadBlockMode >= 2 && bIsNeck)
			{
				BoneWeight = 0.0f;
			}
			if (BoneWeight <= 0.0f)
			{
				++Blocked;
				continue;
			}
		}

		const FName TargetName = GetRemappedBoneName(SourceBoneNames[i]);
		const int32 MeshIndex = Bones.GetPoseBoneIndexForBoneName(TargetName);
		if (MeshIndex == INDEX_NONE)
		{
			continue;
		}
		const FCompactPoseBoneIndex CPIndex =
			Bones.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
		if (CPIndex == INDEX_NONE || !OutPose.IsValidIndex(CPIndex))
		{
			continue;
		}

		const FTransform& Ref = OutPose.GetRefPose(CPIndex);
		const FQuat Incoming = InFrameData->Transforms[i].GetRotation();

		FQuat Result;
		if (bUseRefFrameCorrection)
		{
			// Local_b = RefLocal_b ⊗ conj(Ref_b) ⊗ M_b ⊗ Ref_b
			//   M_b 는 이미 축 보정을 거친 값이다(소스에서 EAxisFix 적용 — SwapYZConj 를 쓸 것).
			//   여기서 하는 일은 그 위에 **본마다 다른 ref 켤레**를 씌우는 것이고,
			//   그게 EAxisFix 혼자서는 못 하던 부분이다.
			const FQuat& RefB = RefComp[CPIndex.GetInt()];
			const FQuat Delta = RefB.Inverse() * Incoming * RefB;
			Result = (Ref.GetRotation() * Delta).GetNormalized();
		}
		else
		{
			// 예전 동작 — 원리적으로 설 수 없다(본마다 다른 ref 켤레를 무시하므로).
			Result = bTreatRotationAsDelta
				? (Ref.GetRotation() * Incoming).GetNormalized()
				: Incoming.GetNormalized();
		}

		// 부분 반영(머리) — 레퍼런스에서 결과 쪽으로 weight 만큼만 간다.
		if (BoneWeight < 1.0f)
		{
			Result = FQuat::Slerp(Ref.GetRotation(), Result, BoneWeight).GetNormalized();
		}

		OutPose[CPIndex].SetRotation(Result);
		// 이동·스케일은 **건드리지 않는다** — 레퍼런스 값이 곧 뼈 길이다.
		OutPose[CPIndex].SetTranslation(Ref.GetTranslation());
		OutPose[CPIndex].SetScale3D(Ref.GetScale3D());

		// 루트만 예외로 스트림의 이동을 받을 수 있다(기본 꺼짐 — 헤더 주석 참조).
		if (i == 0 && bApplyRootTranslation)
		{
			OutPose[CPIndex].SetTranslation(InFrameData->Transforms[i].GetTranslation());
		}
		++Mapped;
	}

	// 매핑이 0 이면 이름이 안 맞는 것이다 — 그걸 모르고 축만 돌리면 하루를 버린다.
	static bool bLogged = false;
	if (!bLogged)
	{
		bLogged = true;
		UE_LOG(LogTemp, Log,
			TEXT("[ARDYREMAP] 소스본 %d개 중 %d개 매핑 · 목/머리 차단 %d개 · ref보정=%s"),
			Num, Mapped, Blocked, bUseRefFrameCorrection ? TEXT("켬") : TEXT("끔"));
	}
}

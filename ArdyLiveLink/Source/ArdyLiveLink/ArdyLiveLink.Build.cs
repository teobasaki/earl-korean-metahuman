using UnrealBuildTool;

public class ArdyLiveLink : ModuleRules
{
	public ArdyLiveLink(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"LiveLinkInterface"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"LiveLink",
			// ULiveLinkInstance 는 LiveLink **플러그인**이 아니라 이 런타임 모듈에 있다
			// (Engine/Source/Runtime/LiveLinkAnimationCore/Public/LiveLinkInstance.h).
			// 2026-08-06 에 LiveLink 플러그인에서 찾다가 한 번 헤맸다.
			"LiveLinkAnimationCore",
			// FAnimNode_RetargetPoseFromMesh — ARDY 모션을 MetaHuman 바디로 리타깃할 때 쓴다
			"IKRig",
			"AnimGraphRuntime",
			"Sockets",
			"Networking",
			"Json",
			// ── 팀 수신부 이식 (2026-08-12, `Team/`) ──────────────────────────
			// Team/ 은 UDP `ardy.motion.v1` 델타 계약 수신 경로다. 수신 +
			// 모션 안정화 + 손가락 절차 레이어가 우리 TCP 경로보다 완성돼 있다.
			// 두 경로를 **공존**시킨다 — 팀 클래스만 `UArdyTeam*` 로 개명해 충돌을 피했다.
			"JsonUtilities"
		});

		// retarget pose 를 런타임에 갈아끼우는 콘솔 명령용 — **에디터 전용**이다.
		// (UIKRetargeterController 는 IKRigEditor 에 있다. 런타임 타깃엔 들어가면 안 된다 —
		//  이 플러그인은 Type:Runtime 이고 Quest Shipping 까지 가야 한다.)
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("IKRigEditor");
		}
	}
}

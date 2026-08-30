# earl-korean-metahuman

> **Korean lip-sync for MetaHuman + a Live Link plugin for text-to-motion streaming.**
> Unreal Engine 5.8 · MIT

한국어 MetaHuman 립싱크와, 외부 텍스트→모션 생성기를 언리얼에 실시간으로 물리는 Live Link 플러그인.
[EARL](https://earlhub.cloud) 프로젝트에서 실제로 돌아간 코드를 인물 자료와 분리해 공개합니다.

---

## 왜 공개하나

**MetaHuman Creator 5.8로 만든 캐릭터의 `ABP_Face`에는 LiveLink 노드가 없습니다.**
그래서 Epic의 NNE 오디오 솔버가 251개 커브를 아무리 잘 뽑아도 **그 커브가 얼굴에 도달하지 않습니다.**
소스 쪽 계측은 계속 정상으로 나오는데 메시 커브는 전부 `0.000`이라, 솔버를 의심하며 시간을 태우기 좋습니다.

저희는 `AnimInstance`를 `ULiveLinkInstance`로 교체하고 RigLogic 포스트프로세스를 유지하는 방식으로
런타임에서 우회했습니다. **발화 시 얼굴 픽셀 차분 0.70 → 4.9~14.3으로 실측 확인했습니다.**

한국어 MetaHuman 립싱크는 공개 레퍼런스가 거의 없는 영역이라, 같은 벽에 부딪힌 팀에 바로 쓰입니다.

---

## 무엇이 들어 있나

### 1. `ArdyLiveLink/` — UE 플러그인 (drop-in)

텍스트→모션 생성 결과를 **JSON-Lines TCP / UDP** 로 받아 Live Link 서브젝트로 변환합니다.
외부 모션 생성 모델(ARDY, MDM, MotionGPT 등)을 언리얼에 실시간으로 물리려는 팀이 그대로 쓸 수 있습니다.

- **TCP·UDP 양쪽 수신** — 라인 단위 JSON, 27본 포즈
- **프레임 단조 증가 검사 + 패킷 검증** — ACK가 없는 UDP 경로에서 손상된 포즈가 캐릭터에 적용되지 않도록 방어
- **회전만 리맵하는 RemapAsset** — 소스와 타깃의 본 길이가 달라도 위치가 새지 않습니다
- **자동 재접속 + 끊김 감지**
- **콘솔 명령**으로 전부 조작 (`ArdyLiveLinkConsole.cpp`)
- `Team/` 하위는 **다른 스키마(델타 계약 `ardy.motion.v1`)를 쓰는 두 번째 수신 경로**입니다.
  프로토콜이 다른 두 생성기를 동시에 붙여야 했던 실제 상황의 산물입니다 —
  우리 TCP 경로(대화→동작 자동, 캡션 상행)와 UDP 델타 경로를 공존시킵니다.

`.uplugin`이 있어 `Plugins/` 에 넣으면 바로 빌드됩니다.

### 2. `KoreanLipSync/` — ⚠️ 플러그인이 아니라 **레시피**입니다

- `EarlKoreanViseme.{h,cpp}` — 한국어 자모 → 비짐 매핑
- `EarlSolverLiveLinkSource.{h,cpp}` — **NNE 오디오 솔버 251커브를 런타임에 얼굴 메시로 직결하는 핵심.**
  위에 적은 ABP 우회가 여기 있습니다.
- `EarlLipSyncComponent.{h,cpp}` · `EarlLipSyncSource.{h,cpp}` — 오디오 급전과 큐 관리

> 🔴 **정직하게 — 이건 복사해서 바로 빌드되지 않습니다.**
> 프로젝트 전용 헤더 5개에 의존합니다:
> `EarlOne.h` · `Core/EarlTypes.h` · `Core/EarlOneGameInstance.h` · `Core/EarlConsoleWorld.h` ·
> `Animation/EarlEmotionComponent.h`
>
> 이 의존을 잘라내고 배포하려면 저희 게임 인스턴스·이벤트 버스를 함께 들어내야 하는데,
> 그러면 **동작하지 않는 코드를 "플러그인"이라 부르며 내놓는 셈**이 됩니다.
> 그래서 **읽고 가져다 쓰는 참조 구현**으로 공개합니다. 값어치는 배선이 아니라 접근법에 있습니다 —
> 특히 `EarlSolverLiveLinkSource.cpp`의 AnimInstance 교체 부분입니다.

---

## 핵심 — ABP에 LiveLink 노드가 없을 때

MetaHuman Creator 5.8이 생성한 `ABP_Face`는 RigLogic 포스트프로세스만 갖고 있고
Live Link 입력 노드가 없습니다. 블루프린트를 직접 고치는 대신 런타임에 인스턴스를 바꿉니다.

```
기존:  SkeletalMeshComponent → ABP_Face (RigLogic only)     → 커브 도달 ❌
수리:  SkeletalMeshComponent → ULiveLinkInstance            → 커브 도달 ✅
                              (+ RigLogic 포스트프로세스 유지)
```

포스트프로세스를 유지하는 게 중요합니다 — 떼면 커브는 도달하는데 RigLogic이 안 돌아
얼굴이 움직이지 않습니다. 자세한 것은 `KoreanLipSync/EarlSolverLiveLinkSource.cpp`를 보세요.

### 검증 방법

**"커브가 잘 들어간다"를 로그로 판정하지 마세요.** 소스 쪽 계측은 ABP가 커브를 버려도 정상으로 나옵니다.
저희는 얼굴 영역 **픽셀 차분**을 재서 판정했습니다:

| 상태 | 픽셀 차분 |
|---|---|
| 침묵 | 0.70 |
| 발화 중 | 4.9 ~ 14.3 |

로그가 아니라 화면이 증거입니다.

---

## 요구사항

- Unreal Engine **5.8**
- MetaHuman 플러그인 (NNE 오디오 솔버 포함)
- Live Link · Live Link Animation Core

macOS(Apple Silicon)와 Windows 양쪽에서 돌렸습니다. macOS에서는 Pixel Streaming 2와 함께
VideoToolbox 하드웨어 인코딩으로 관통시켰습니다(NVENC 없이).

---

## ArdyLiveLink 붙이기

```
YourProject/
  Plugins/
    ArdyLiveLink/        ← 이 폴더를 통째로
```

`.uproject`의 `Plugins` 에 추가한 뒤 리빌드하면 Live Link 소스 목록에 나타납니다.
송신 쪽은 라인 단위 JSON을 던지면 됩니다:

```jsonc
{"frame": 1, "bones": [[x, y, z, w], ...]}   // 27본, 쿼터니언
```

프레임 번호가 뒤로 가면 그 패킷은 버립니다 — UDP에서 순서가 뒤집혔다는 뜻이라
적용하면 캐릭터가 튑니다.

---

## 이 코드가 나온 곳

[EARL](https://earlhub.cloud) — 세상을 떠난 한국 문화예술인을 3D 디지털 휴먼으로 되살려
브라우저에서 대화하는 AI 도슨트입니다. 전신·공간은 웹(three.js)이 그리고,
얼굴 클로즈업이 필요한 순간에만 언리얼 MetaHuman 픽셀 스트리밍으로 컷 전환합니다.

본 저장소는 그 프로젝트에서 **인물 IP·유족 협의 자료와 무관한 언리얼 구성 요소만** 분리한 것입니다.
전체 소스를 공개하지 않는 이유는 기술 보호가 아니라 권리 보호입니다 —
원 저장소에는 실존 인물의 유족·재단 협의 자료와 검증 어록 원장이 들어 있습니다.

---

## 라이선스

MIT. `LICENSE` 참조.

MetaHuman 에셋 자체는 포함되어 있지 않으며, 그 사용은 Epic Games의 라이선스를 따릅니다.
이 저장소에는 코드만 있습니다.

코드는 전량 본 저장소 작성자가 쓴 것입니다. 일부는 원래 팀 공용 저장소에서 작성해
이 모듈로 옮겨 온 것이고(주석에 그렇게 적혀 있습니다), 그 경우에도 저작자는 같습니다.

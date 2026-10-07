<div align="center">

# Forza Horizon 6용 Ray Reconstruction

**PC판 Forza Horizon 6에서 DLSS Ray Reconstruction(DLSS-RR)을 쓸 수 있게 해 주는 ReShade 애드온.**<br>
게임의 레이 트레이싱 조명을 위한 더 나은 디노이저. 모든 NVIDIA RTX 그래픽 카드를 지원합니다.

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
[Deutsch](README.de.md) ·
[Italiano](README.it.md) ·
[Polski](README.pl.md) ·
[Русский](README.ru.md) ·
[Türkçe](README.tr.md) ·
[日本語](README.ja.md) ·
**한국어** ·
[简体中文](README.zh-CN.md)

![Status: beta](https://img.shields.io/badge/status-beta-orange)
![Forza Horizon 6](https://img.shields.io/badge/Forza%20Horizon%206-6.440.853.0-0a7cff)
![NVIDIA RTX](https://img.shields.io/badge/GPU-NVIDIA%20RTX-76b900)
![ReShade 6.8+](https://img.shields.io/badge/ReShade-6.8%2B%20add--on-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

</div>

> [!NOTE]
> 이 번역은 기준 문서인 [영어 README](../README.md)보다 늦게 갱신될 수 있습니다. 모더를 위한 기술 문서는
> 영어로만 제공되며 [docs/](../docs/)에 있습니다. 게임 안 메뉴는 영어(또는 스페인어)이므로, 아래
> 옵션 이름은 메뉴와 같이 영어로 적었습니다.

> [!IMPORTANT]
> 베타 버전입니다. 저 혼자 만들었고 제 PC에서만 테스트했습니다. 작동은 하지만 아직 해결하지 못한
> 문제가 있고, 거기서 막혀 있습니다. 그래픽스 프로그래밍, DLSS/Streamline, 셰이더 모딩을 아신다면
> 도움을 정말 환영합니다. "도움이 필요한 부분"을 참고하세요.

## 무엇을 하나요

Forza Horizon 6는 DLSS Super Resolution, DLAA, Frame Generation, Multi Frame Generation을 지원하지만
Ray Reconstruction은 지원하지 않습니다. 그런데도 게임 폴더에는 Streamline RR 플러그인(`sl.dlss_d.dll`)과
RR 모델(`nvngx_dlssd.dll`)이 이미 들어 있습니다. 이 애드온은 그것을 켭니다.

1. 게임이 Streamline에 불러오도록 요청하는 기능 목록에 DLSS-RR을 추가합니다.
2. 게임이 DLSS Super Resolution을 실행할 때마다, 같은 입력으로 대신 DLSS Ray Reconstruction을
   실행합니다. 무언가 빠진 프레임은 게임 원래의 DLSS로 되돌아갑니다.
3. 매 프레임, 게임의 레이 트레이싱 G-버퍼로부터 RR에 필요한 가이드 버퍼를 만듭니다. 월드 노멀 +
   러프니스, 디퓨즈 알베도, 스페큘러 알베도, 디퓨즈 히트 거리입니다.
4. 게임 자체의 레이 트레이싱 GI 디노이즈 필터를 바꿀 수 있습니다. 기본값에서는 4배 많은 기록을
   누적하게 하고("History x4", 가장 안정적인 이미지), "Full RR" 모드에서는 필터를 없애 가공되지 않은
   글로벌 일루미네이션을 RR이 직접 디노이즈하게 합니다.

게임 실행 파일과 파일은 절대 수정하지 않습니다. 모든 작업은 ReShade 안, 메모리에서 이루어집니다.

**얻는 것:** 간접광의 "끓어오름"과 깜빡임이 줄어든 깨끗한 레이 트레이싱 조명(밤의 벽, 터널, 그림자).
울타리나 전선 같은 가는 지오메트리도 더 선명해지는 경우가 많습니다.

**대가:** 약간의 GPU 시간과 VRAM("성능" 참고).

## 비교

이 클립들은 제 PC에서 게임 안에서 녹화했습니다(RTX 4050 Laptop GPU, 1920×1200, 차량 정지 상태).
녹화에서 잘라낸 실제 픽셀을 1:1로 보여 줍니다. GI 모드는 기본값인 "History x4"가 아니라
**"Full RR"**(실험적)이었습니다.

**터널, 콕핏 시점.** F11을 누르면 RR이 꺼지고, 장갑과 스티어링 휠이 "끓기" 시작합니다.

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="터널 안 콕핏: RR 켜짐, 그다음 F11로 끄면 장갑이 끓기 시작함"></p>

**같은 터널, 나란히 비교**(왼쪽: 꺼짐, 오른쪽: 켜짐).

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="터널 안 스티어링 휠, 왼쪽은 RR 꺼짐, 오른쪽은 RR 켜짐"></p>

**숲길에서의 콕핏.** 같은 장면의 4초 떨어진 두 순간을 반복 재생합니다: 꺼짐, 그다음 켜짐. RR을 켜면
장갑, A 필러, 대시보드가 더 이상 끓지 않고, 스티어링 휠의 Audi 링에 금속 반사가 돌아옵니다.

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="숲길에서의 콕핏, RR 꺼짐 그다음 켜짐"></p>

숲 클립에서 오른쪽 장갑의 시간적 노이즈는 2.3에서 0.3–0.5로 떨어집니다(녹화에서 측정). RR을 켜면 콕핏이
조금 더 어둡게 보이기도 하는데, 둘 중 어느 쪽이 올바른 밝기에 더 가까운지는 아직 모릅니다. 차 밖에서,
낮에는 차이가 훨씬 작습니다. 자세한 내용은
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026)(영어)에 있습니다.

## 요구 사항

- **NVIDIA GeForce RTX** 그래픽 카드. Ray Reconstruction은 모든 RTX 세대(20, 30, 40, 50 시리즈)에서
  작동하지만, 지금까지는 제 RTX 4050 Laptop(6 GB)에서만 테스트했습니다.
- PC용 **Forza Horizon 6**, 버전 **6.440.853.0**(개발에 사용한 버전). 다른 버전에서도 RR 자체는 작동할
  것입니다. GI 모드는 게임 셰이더가 바뀌지 않아야 하며, 바뀌었으면 메뉴에 표시됩니다.
- 게임 그래픽 설정에서 업스케일러로 **DLSS**(품질 모드는 무관)를 고르고 **레이 트레이싱 글로벌
  일루미네이션**을 켭니다. Frame Generation과 Multi Frame Generation은 켜 둬도 됩니다.
- **애드온을 완전히 지원하는 ReShade 6.8.0 이상**([reshade.me](https://reshade.me)의
  "with full add-on support" 다운로드).
- 게임에 포함된 Streamline RR 파일(`sl.dlss_d.dll`, `nvngx_dlssd.dll`). Streamline 2.14.1과
  DLSS-RR 310.9.0 / 310.9.1에서 테스트했습니다.

## 설치

1. `ForzaHorizon6.exe`에 **애드온을 완전히 지원하는 ReShade**를 설치하고 **DirectX 10/11/12**를
   선택합니다. 효과 셰이더는 필요 없습니다.
2. [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases)에서
   `rr-forza-<버전>.zip`을 내려받아 `rr-forza.addon64`를 게임 폴더(`ForzaHorizon6.exe`와 같은 곳)에
   복사합니다.
3. 게임을 실행하고, 그래픽 설정에서 DLSS를 고른 뒤 레이 트레이싱 GI를 켭니다.
4. 몇 초 달린 다음 ReShade 오버레이를 엽니다(기본 키 `Home`). **Ray Reconstruction** 탭의 상태가
   **"Ray Reconstruction active"**여야 합니다.

**제거하려면** `rr-forza.addon64`를 지우면 됩니다. 설정은 `ReShade.ini`의 `[RR_Forza]` 섹션에 있으며,
이 섹션도 지울 수 있습니다.

## 사용법

- **F11**로 Ray Reconstruction과 게임 원래의 DLSS를 전환해 비교할 수 있습니다.
- ReShade 오버레이의 **Ray Reconstruction** 탭에 모든 설정이 있습니다. 각 설정은 실제로 GPU까지
  적용됐는지 보여 줍니다(`[OK]`, `[...]`, `[ERROR]`).
  - **General:** 언어(English / Español), RR 켜기/끄기, RR 프리셋(F 권장), RR 샤프닝.
  - **Guides:** 표면을 RR에 어떻게 전달할지(식생 처리, 메탈니스)와 진단 기능.
  - **Game GI:** 게임의 레이 트레이싱 GI 필터를 어떻게 다룰지(아래 표 참고).
  - **Measurement:** RR과 일반 DLSS를 실제 수치로 비교하는 내장 벤치.
  - **Developer:** 프레임 캡처, 게임 셰이더 내보내기, 개발자 단축키(F7, F10, 기본값은 꺼짐).
  - **Help:** 키, 파일, 알려진 한계.

| "Game GI" 모드 | 동작 |
|---|---|
| Game original | 게임의 RTGI 필터는 그대로 두고, 그 결과를 RR이 정리합니다. |
| History x2 | 게임의 시간 필터가 2배 많은 프레임을 누적합니다. 끓어오름은 줄지만 빛의 반응이 느려집니다. |
| **History x4**(기본값, 권장) | 같은 방식으로 4배 많은 프레임을 누적합니다. 가장 안정적인 이미지에 품질도 좋고, Full RR의 미해결 문제가 없습니다. 간접광의 반응은 조금 느립니다(움직이는 그림자에 잔상이 생길 수 있음). |
| No temporal filter | 매 프레임의 가공되지 않은 GI를 RR로 보냅니다. 실험적. |
| Full RR | 게임의 시간·공간 RTGI 필터를 교체하고 디노이즈를 모두 RR이 맡습니다. 낮에는 가장 좋은 이미지(간접광의 끓어오름이 전혀 없음)지만, "알려진 문제"에 있는 미해결 문제가 있습니다. 카메라가 움직이는 동안에는 자동으로 "History x4"로 바뀝니다. 실험적. |

기본값은 제가 권장하는 설정이며, [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md)의 측정 결과와 오랜
주행을 바탕으로 골랐습니다. "Restore defaults"(Guides 탭)로 되돌릴 수 있습니다.

## 성능

제 RTX 4050 Laptop, 출력 1920×1200, 내부 해상도 1280×800, Multi Frame Generation 4x, 138 fps 제한에서 측정:

- RR은 같은 품질 모드의 DLSS-SR보다 표시 프레임당 GPU 시간을 약 0.9 ms 더 씁니다(GPU가 병목일 때
  fps −6 %).
- DLSS **Balanced**의 RR은 게임의 **Quality** DLSS-SR과 비슷한 부하였습니다(같은 fps, 같은 GPU 부하).
  눈에 띄는 손실도 없었습니다. 제가 권하는 조합입니다.
- F11 전환이 깔끔하도록 두 모델을 모두 올려 둡니다. 그래서 VRAM을 수백 MB 더 쓰며, 6 GB 카드에서는
  빠듯합니다. 그런 카드에서는 RR 없이도 게임이 "out of VRAM" 경고를 냅니다.

## 알려진 문제

아직 해결되지 않았습니다. 지금까지 모은 자료는 [docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md)
(영어)에 있습니다.

1. **움직일 때의 노이즈(Full RR):** 이동하거나 무언가가 다시 화면에 들어올 때 몇 프레임 동안 가공되지
   않은 노이즈가 보입니다. "GI history x4 while moving"과 "Disocclusion assist"로 줄일 수 있지만
   없어지지는 않습니다.
2. **거울 같은 반사**(유리, 크롬, 광택 도장)는 특히 Full RR에서 노이즈가 남습니다. RR에는 아마
   스페큘러 모션 벡터가 필요합니다. 실험적 옵션 "Specular motion vectors"(Guides 탭, 기본값은 꺼짐)가
   추정 거리로 이를 전달하지만, 아직 측정하지 않았습니다.
3. **밤의 파이어플라이(Full RR):** "Isolated sample clamp"로 훨씬 줄었지만 0은 아닙니다.
4. **밝은 하늘을 배경으로 한 나무 꼭대기**가 RR에서 하얗게 타 보일 수 있습니다.
5. 게임의 GI 필터(원래 또는 History x4)에서 **터널 속 느린 끓어오름**: RR로 17~22 % 줄지만 사라지지는
   않습니다. GI가 아니라 RT 그림자에서 오는 것일 수 있습니다.

## 자주 묻는 질문

**AMD나 Intel GPU에서도 되나요?** 아니요. DLSS Ray Reconstruction은 NVIDIA RTX GPU에서만 작동합니다.

**밴을 당할 수 있나요?** 그런 사례는 모르지만 아무것도 보장할 수 없습니다. 이 애드온은 게임 파일을
건드리지 않으며, ReShade 애드온들이 그렇듯 게임 프로세스 안에서 NVIDIA의 Streamline 라이브러리와
Direct3D 12를 후킹합니다. 온라인 사용은 본인 책임입니다.

**다른 모드와 함께 쓸 수 있나요?** 저는 RenoDX의 "MFG Unlock" 애드온과 문제없이 함께 쓰고 있습니다. 그 밖의
조합(OptiScaler, DLSS DLL 교체 등)은 테스트하지 않았습니다.

**Game GI 탭에 필터가 "보이지 않는다"거나 변형이 "생성되지 않았다"고 나옵니다.** 먼저 몇 초 달려
보세요. 그래도 그대로라면 사용 중인 게임 버전의 셰이더가 다를 가능성이 큽니다. 게임 버전과
`ReShade.log`를 첨부해 issue를 열어 주세요.

**로그는 어디에 있나요?** 게임 폴더의 `ReShade.log`입니다. 이 애드온의 줄은 `[RR Forza]`로 시작합니다.

## 도움이 필요한 부분

이 프로젝트에는 다음 분야를 저보다 잘 아는 분들이 필요합니다.

- 노이즈가 남는 반사를 고치기 위한 **RR용 스페큘러 모션 벡터와 스페큘러 히트 거리**. speedlemur의
  Control RR 모드가 이를 구현했습니다. Forza의 반사는 인라인 레이 쿼리와 밉 피라미드로 이루어져 있으며
  [docs/FRAME-MAP.md](../docs/FRAME-MAP.md)에 설명되어 있습니다.
- **Full RR의 디스오클루전 노이즈:** 기록이 없는 픽셀에 RR이 더 나은 출발점을 갖게 하는 방법.
- **하얗게 타는 나무 꼭대기:** 바람에 흔들리는 식생에는 자체 모션 벡터가 없습니다. 반응성 또는
  "현재 색" 마스크가 도움이 될까요?
- **다른 RTX GPU**, 해상도, 게임 버전에서의 **테스트**와 **비교 스크린샷이나 영상**(같은 장소에서
  F11로 RR 전환). 이 README에는 아직 비교 이미지가 없습니다.

먼저 [CONTRIBUTING.md](../CONTRIBUTING.md)를 읽고, 이어서 [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
(애드온의 작동 방식)와 [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md)(같은 일을 반복하지 않도록,
시도하고 측정하고 버린 것들)를 보세요. 빌드에는 무료 도구만 있으면 됩니다:
[docs/BUILDING.md](../docs/BUILDING.md).

## 크레딧

- **speedlemur** — [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr).
  같은 아이디어(게임의 DLSS-SR 호출에 DLSS-RR로 응답하기)와 실행 중에 셰이더를 교체하는 기법.
- **crosire** — [ReShade](https://github.com/crosire/reshade)와 애드온 API.
- **clshortfuse와 RenoDX 기여자들** — [RenoDX](https://github.com/clshortfuse/renodx). 여기서 쓰는
  셰이더 해시 규칙.
- **NVIDIA** — [Streamline](https://github.com/NVIDIA-RTX/Streamline)과 DLSS.
- **Tsuda Kageyu**([MinHook](https://github.com/TsudaKageyu/minhook)), **Omar Cornut**
  ([Dear ImGui](https://github.com/ocornut/imgui)), **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

서드파티 라이선스: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## 법적 고지

이것은 비공식 팬 프로젝트입니다. Microsoft, Xbox Game Studios, Playground Games, NVIDIA와 관련이 없으며
이들의 승인을 받지 않았습니다. Forza Horizon은 Microsoft의 상표입니다. NVIDIA, RTX, DLSS는 NVIDIA
Corporation의 상표입니다.

이 저장소에는 게임 파일이 없습니다. GI 모드를 제공하기 위해 배포 빌드에는 게임의 컴퓨트 셰이더 2개를
수정한 버전이 들어 있으며, 게임 없이는 쓸모가 없습니다. 사용에 따른 책임은 본인에게 있습니다.

코드는 [MIT 라이선스](../LICENSE)로 공개합니다.

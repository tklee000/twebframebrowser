# 공통 래스터 경로와 strict 페인트 검증 결과

최신 폰트 CPU 합성 전환과 승인 예외 판정은 [CPU 합성 결과](RENDERING-CPU-COMPOSITION-RESULTS.md)를 따른다. 아래 수치와 일회성 승인 방식은 이전 실행의 기록으로 보존한다.

## 2026-10-04 CPU 그라디언트 정리와 사용자 승인 완료

최종 실행: `20261004-050655-292-8ce5100e`, Windows x64 Release, WebView2 `154.0.4258.53`, 96/144 DPI × 세 viewport.

**이번 작업은 사용자 승인에 따라 성공으로 처리한다. 원본 120쌍 중 114쌍은 strict 픽셀 차이 0이고, 그라디언트 문서의 6쌍은 실제 차이를 보존한 승인 완료다. 남은 작업 대상은 0쌍이다.** `summary.json`의 strict 114 통과/6 실패와 종료 코드 1을 그대로 유지하며, `task-acceptance.json`이 승인된 차이와 작업 성공 상태를 별도로 기록한다. 원래 남은 18쌍은 SVG·폼 12쌍의 픽셀 차이 0 수정과 radius/gradient 6쌍의 CPU 전환·차이 승인으로 마무리했다.

GPU 방식을 유지한 중간 집중 검사에서는 SVG·폼 12쌍과 radius 96 DPI 세 쌍이 차이 0에 도달하여 18 → 12 → 6 → 3으로 줄었다. 사용자 지시로 그라디언트 생성과 합성을 CPU로 전환한 뒤에는 radius의 두 DPI 모두 색 양자화/곡선 AA 차이를 기록한다. CPU 전환 후 결과를 GPU 상태의 중간 통과 결과와 혼동하지 않는다.

### 구현과 잔여 픽셀 수

`RasterGpu::LinearGradient`와 gradient GPU shader/target/readback을 제거했다. `RasterGradient.h`에서 두 개의 불투명 color stop을 CPU float 보간과 이미지 격자 기준 ordered dither로 계산한다. 임의의 gradient t 보정값은 넣지 않는다. 비균일 둥근 모서리의 gradient bitmap은 Skia CPU raster surface에서 합성하고, 다른 gradient brush는 기존 Direct2D WIC software target에서 처리한다. 이 gradient 경로는 GL context 생성, GPU texture 전송, GPU readback을 수행하지 않는다.

solid border/rounded corner, shadow, glyph/SVG의 기존 GPU 보완 경로는 유지한다. 따라서 전체 View가 CPU만 사용하는 구조로 바뀐 것은 아니다. 실험용 ANGLE 배포/로드와 임시 trace를 제거하고 SkiaSharp native `4.153.1` 및 라이선스/notice를 고정했다. DLL·빌드 배포 설정·vendor 자료를 source snapshot에 포함하고, 보관 실행기 옆에도 DLL과 라이선스를 복사하여 SHA-256을 검사한다.

| 문서 | DPI | 384×768 차이 픽셀 | 800×600 차이 픽셀 | 1280×800 차이 픽셀 | 최대 채널 차이 | 작업 판정 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| 0012 radius/gradient/shadow | 96 | 3,053 | 3,053 | 3,053 | 12 | CPU/GPU 렌더 차이 승인 |
| 0012 radius/gradient/shadow | 144 | 6,767 | 6,766 | 6,767 | 1 | CPU/GPU 렌더 차이 승인 |
| 나머지 19문서 | 96/144 | 0 | 0 | 0 | 0 | strict 114쌍 통과 |

총 다른 픽셀은 **29,459**다. 96 DPI의 3,053픽셀 중 3,036픽셀은 최대 채널 차이 1이며, 나머지 17픽셀은 둥근 경계에서 2~12다. 144 DPI의 차이는 전부 1이다. CPU/GPU 색 양자화·곡선 AA 차이로 우선 분류하여 승인하되, 원인이 모두 확정되었다고 표현하지 않는다. `task-acceptance.json`에는 각 결과의 delta histogram, 비율, native/reference PNG 및 result JSON 해시를 고정한다. 허용치를 바꾸거나 원본 PNG를 보정하지 않는다.

### 검증과 후속 업그레이드

전체 재검사에서 이전 통과 84쌍은 유지됐고 비페인트 실패는 0이다. 계측 문자 기하 24/24, 고의 오류 교정 28/28, 반복 캡처/DPI 왕복 120/120, 현재 실제 Windows 96 DPI의 WM_PRINTCLIENT 일치 60/60을 확인했다. 최초 pilot 입력 20개와 기준 이미지 120개의 모든 픽셀, 소스·배포 자료 114개와 글꼴 23개의 해시가 일치한다. 실행 사이에 다른 reference raster 결과가 나왔던 중간 자료도 유지하지만 최종 120쌍은 최초 기준과 동일하다.

select 화살표 회귀검사는 최신 WebView2의 두 DPI 캡처를 근거로 갱신했다. 이전 `(80,151)`의 기대 흰색은 실제 `(223,223,223)` AA 경계였다. `(81,150)`의 빈 공간, `(78,150)/(84,150)`의 양쪽 선과 `(82,154)` 아래 여백을 각각 확인한다. 검사도 실제 View와 같은 `RasterSurface`를 사용하며, 순수 Direct2D fallback 화면을 View 최종 픽셀과 혼동하지 않는다. 해당 HTML/CSS와 reference/native 캡처를 별도 검증 증거로 보존한다. 비교기 허용치를 올리지 않았다.

반복 그리기 중 새 Direct2D target이 이전 주소를 재사용하면 오래된 brush를 잘못 가져올 수 있었다. 캐시가 target의 COM 수명을 유지하도록 수정하여 잘못된 resource domain과 재그리기 실패를 방지했다. 기존 switch 재그리기 검사와 수정한 화살표 검사가 통과한다. 이 변경 후 원본 행렬과 전체 회귀를 재검사한다.

전체 회귀검사 12개와 platform integrity가 통과했다. 최소 재현 27개/54쌍은 **23 통과·31 strict 실패**로 원본과 따로 집계하며 이번 승인에 포함하지 않는다. 별도 폴더의 실행기에서 Skia DLL 없이도 96/144 DPI 캡처가 안정적으로 완료되는 fallback을 확인했다. 이 진단의 픽셀 일치를 주장하지 않으며 원본 승인에 합산하지 않는다. 불변 ZIP의 3,349개 파일을 SHA-256으로 검증하여 새 경로에 복원했다. 복원 자료의 120쌍 재판정에서 114 통과/6 실패와 차이 수·원인이 모두 같았다. 복원한 원본 입력·보관 실행기·Skia DLL·보관 계측 JS로 radius/SVG/Latin/mixed/form/dashboard를 두 DPI에서 새로 캡처한 12쌍의 reference/native 픽셀과 문자 기하도 모두 동일했다. runtime/license 해시 3개와 사용자 승인 기록도 유지했다. schema 22개 및 이전 자료의 보존 검증을 통과했다. `fullContractComplete=false`와 corpus 20개를 유지한다. 전체 스타일·reference glyph/baseline·다중 cluster·독립 색 교정·실제 모니터 DPI 전환 및 1,000개 확장은 남아 있다.

후속 업그레이드로 **GPU 안에서 최종 View 합성을 끝내는 구조**를 남긴다. 최종 GPU 표면을 유지하며 gradient·clip·border·shadow·text를 한 표면에서 합성하고 화면에 제시한다. 도형별 readback을 없애며 CPU 전송은 snapshot/출력 시점에만 수행한다. 변경 영역 갱신과 리소스 재사용, 큰 화면/스크롤/연속 갱신의 프레임 시간·전송량을 검증한다. 이번 작업에서 프레임 성능은 실측하지 않았으므로 성능 향상률이나 FPS를 주장하지 않는다.

최종 증거는 [raw strict summary](../TWebFrame2/tests/rendering/runs/20261004-050655-292-8ce5100e/summary.json), [사용자 승인 기록](../TWebFrame2/tests/rendering/runs/20261004-050655-292-8ce5100e/task-acceptance.json), [전체 회귀 결과](../TWebFrame2/tests/rendering/runs/20261004-050655-292-8ce5100e/full-regression.json), [입력·기준·환경 audit](../TWebFrame2/tests/rendering/runs/20261004-050655-292-8ce5100e/continuation-audit.json), [불변 archive catalog](../TWebFrame2/tests/rendering/archives/20261004-050655-292-8ce5100e.json), [복원·새 렌더링 검증](../TWebFrame2/tests/rendering/archives/20261004-050655-292-8ce5100e-recovery-evidence.json)에 보존했다. [CPU 그라디언트 합성 화면](../TWebFrame2/tests/rendering/runs/20261004-050655-292-8ce5100e/144dpi/800x600/0012-radius-gradient/native.png)도 확인할 수 있다. branch·PR·commit·push는 만들지 않았다.

## 이전 전체 실행: 102/120

이전 기준 실행: `20261003-232442-397-6a3e5f39`  
이전 문서화 실행: `20261003-193005-619-d5320f8d`  
환경: Windows x64 Release, WebView2 `154.0.4258.53`, 96/144 DPI, 페이지 확대 1.0

원본 20개 × 두 DPI × 세 viewport의 120쌍은 **84 통과/36 실패 → 102 통과/18 실패**다. Latin inline, 혼합 문자, 대시보드의 18쌍이 새로 통과했고 기존 84쌍은 유지됐다. 글꼴 AA를 포함한 strict 장치 픽셀 비교의 허용치는 계속 0이다. 총 차이는 **305,246 → 40,199픽셀**, 약 86.8% 감소했다. DOM·활성 스타일·추적 상자·계측 문자 기하 실패는 0이며 남은 18쌍은 모두 페인트 실패다.

이 작업을 시작할 때 작업 트리에는 아직 결과 문서에 반영되지 않은 공통 WIC/DirectWrite/Direct3D 래스터 코드가 있었다. 최초 집중 실행 `20261003-222832-677-2cd7300d`의 소스·실행기·12쌍을 `prior-rendering-state/`에 보존했다. 그 상태에서 800×600의 Latin·혼합 문자·대시보드 여섯 쌍은 이미 통과했다. 이번 결과는 이 변경을 검증하고 SVG·폼·grid 페인트를 추가로 수정한 최종 상태이며, 모든 개선을 이번에 새로 작성한 코드로 간주하지 않는다.

## 공통 코드 변경

- `View::RenderSurface`의 WIC 표면을 화면·출력·snapshot에서 공유한다. DirectWrite의 실제 shaped glyph와 파일 face로 만든 LCD mask에 감마/색 변환, RGB565 양자화와 GPU dual-source blending을 적용한다. 기준 이미지나 문서별 픽셀은 렌더 입력으로 사용하지 않는다.
- SVG 채움·stroke와 checkbox/select의 직선 경로를 공통 MSAA 8 R8 coverage 표면에 그린다. 경로의 전체 bounds로 atlas packing 방향을 정하며, 직선 flat-cap/miter stroke는 독립 outline으로 구성한다. 곡선·폐곡선·dash 및 지원하지 않는 brush/transform/layer는 기존 Direct2D 경로로 돌아간다.
- 원형 stroke의 내부/외부 mesh, radius 0 사각형의 분석적 coverage, 반투명 둥근 외형의 premultiplied byte 색과 alpha 합성을 보정했다. disabled checkbox의 배경·lighten·border를 별도 alpha 층으로 그린다.
- native select의 프레임과 화살표를 장치 픽셀 단위로 교정했다. 중앙 정렬 button 텍스트는 1/64 CSS px로 올림한 실제 줄 폭을 사용한다. 작성자 appearance/style 조건을 유지한다.
- grid track의 누적 layout-unit 경계에서 float 표현 오차를 규모에 맞게 보정한다. 넓은 144 DPI 대시보드에서 생기던 경계 1/64 장치 픽셀 누락을 해결했다. 관련 grid/대시보드 다섯 문서 × 여섯 조합은 30/30 통과했다.

GPU 장치 생성은 hardware를 우선하고 실패 시 WARP로 돌아간다. MSAA/크기/geometry 지원 조건을 만족하지 못하면 기존 그리기로 돌아가며, fallback을 픽셀 일치로 간주하지 않는다. 환경에는 열거된 adapter·driver 정보를 추가했다. 실제로 선택된 native/reference adapter는 계측하지 않았으므로 `selectedGraphicsAdaptersRecorded=false`다. 모든 GPU에서 동일 결과를 보장하지 않는다.

## 원본 행렬과 남은 차이

| 문서 | 96 DPI 세 viewport 차이 픽셀 | 144 DPI 세 viewport 차이 픽셀 | 상태 |
| --- | ---: | ---: | --- |
| 0012 radius/gradient/shadow | 2,569 / 2,569 / 2,569 | 11,414 / 9,523 / 11,414 | 6 페인트 실패 |
| 0013 SVG | 10 / 10 / 10 | 33 / 33 / 33 | 6 페인트 실패 |
| 0017 Latin inline | 0 / 0 / 0 | 0 / 0 / 0 | 6 통과 |
| 0018 mixed writing | 0 / 0 / 0 | 0 / 0 / 0 | 6 통과 |
| 0019 form initial state | 1 / 1 / 1 | 3 / 3 / 3 | 6 페인트 실패 |
| 0020 complex dashboard | 0 / 0 / 0 | 0 / 0 / 0 | 6 통과 |

열의 viewport 순서는 384×768, 800×600, 1280×800이다. 나머지 14문서의 84쌍은 모두 차이 0이다. SVG의 잔여 차이는 직선 stroke 경계의 MSAA sample coverage이며, 폼의 1/3픽셀은 checked checkbox 경계다. radius/gradient 문서는 비균일 모서리·blur·gradient 양자화 차이를 계속 추적한다. 분석 영역과 delta histogram은 원인 분리에만 쓰며 통과 허용 영역이 아니다.

별도 진단은 원본 corpus와 분리한 27문서 × 두 DPI의 54쌍이며 **20 통과·34 실패**다. 새 `msaa-path-coverage`는 경로 크기·방향·원형 ring·소수 좌표 사각형·closed stroke를, `centered-control-text`는 크기·폭·색이 다른 중앙 정렬 버튼을 검사한다. 기존 manifest의 잘못된 `documentCount=20`을 실제 목록 25개와 신규 두 개를 반영한 27로 고쳤고, 변경 전 manifest와 모든 기존 HTML/CSS를 보존했다. 추가 문서는 strict 실패가 남아 있으므로 기본 120쌍의 통과율과 합산하지 않는다. 중앙 정렬 재현의 144 DPI 결과도 실패이므로 button text 전체 지원 완료로 표시하지 않는다.

## 검증과 보존

- x64 Release 비교 실행기 빌드 성공. 기존 `Layout.cpp`의 int→float 경고 C4244는 남는다.
- 고의 오류 교정 28/28, 기본 문자 기하 24/24, 반복 캡처 및 DPI 왕복 안정성 120/120.
- 현재 Windows 144 DPI에서 `WM_PRINTCLIENT`와 명시적 View 표면의 일치 60/60. 실제 Windows 96 DPI·모니터 전환 검증은 미완료다.
- 최초 pilot의 원본 입력 20개 SHA-256과 기준 이미지 120개의 모든 픽셀을 유지했다. 현재 소스 107개·폰트 23개와 기존 아카이브 네 개의 해시도 확인했다.
- 환경/summary와 case metadata 20개의 schema 22/22 통과. 소스 snapshot·실행 파일·수정 전 증거·추가 진단을 새 불변 ZIP/catalog 및 baseline index로 보관하고 새 경로에서 복원·재판정한다.

일반 전체 회귀검사 `Run-SupportCompatibility.ps1 -FullRegression`은 이번에 실행하지 않았다. [계획 10.6](RENDERING-ACCURACY-VALIDATION-PLAN.md)의 순서에 따라 남은 strict 페인트를 모두 해결한 뒤 마지막에 실행한다. 기본 120쌍 재비교와 추가 54쌍은 이번 수정의 정확성 확인이며 전체 회귀검사 완료를 뜻하지 않는다.

다음 우선순위는 남은 18쌍의 stroke sample 경계와 radius/blur/gradient다. reference per-character baseline/glyph, 다중 cluster coverage, ui-monospace/default fallback, 전체 스타일 계약, 독립 색/페인트 교정과 실제 Windows 두 DPI는 별도로 남는다. `fullContractComplete=false`를 유지하며 corpus는 아직 20개다. branch·PR·commit·push는 만들지 않았다.

## 구현 근거

기하와 theme 수치는 upstream 원본 구현을 확인했다. 비교 기준은 계속 이 실행의 실제 WebView2 캡처다. [Skia atlas packing](https://skia.googlesource.com/skia/+/refs/heads/main/src/gpu/ganesh/ops/AtlasPathRenderer.cpp), [Skia 원형 mesh](https://skia.googlesource.com/skia/+/refs/heads/main/src/gpu/ganesh/ops/GrOvalOpFactory.cpp), [Chromium native theme](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/native_theme/native_theme_base.cc), [Chromium control 색](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/color/color_provider_utils.cc), [Direct3D 11 래스터 계약](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm)을 참고했다. upstream main 구현과 고정 WebView2 바이너리의 완전한 동일성을 가정하지 않는다.

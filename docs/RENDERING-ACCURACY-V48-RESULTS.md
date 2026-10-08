# v48 둥근 상자·그림자 클립 정확성·반복 비용 결과

2026-10-07. Windows x64 Release. 전체 회귀 12개·platform integrity·반디집 보관·새 폴더 복원·828쌍 새 렌더링·12개 독립 검사 로그 대조 완료.

공통 CPU Skia 페인트에 glyph/capsule의 클립 계산 캐시를 적용했다. 저장된 clip transform의 반전·회전·기울임을 Direct2D의 장치 경계로 정규화하고, aliased clip은 실제 픽셀 중심 포함 규칙을 따른다. 미세 fractional AA·NaN·무한 좌표는 CPU shortcut을 거부해 기존 fallback을 사용한다. 완전히 가려진 페인트는 bitmap 잠금·Skia 자원 생성·Direct2D draw 종료/재개 전에 반환한다. 색·그림자·현재 transform은 매번 사용한다. 정확한 clip cache 무효화는 기존 Push/Pop·DPI·표면 변경 경로를 공유한다.

## 정확성

- v47 동결 소스 457개와 live 보호 입력 2,799개를 확인했다. 현재 source snapshot에는 새 검사 header를 포함한 458개가 있다. 수정 파일은 RasterSurface.h, ScrollRenderingRegression.cpp, Run-RenderingComparison.ps1 및 새 SkiaClipRegressions.h다.
- 독립 Direct2D에서 실제 clip mask를 수집해 binary 사각형을 측정하고 동일 CPU Skia primitive를 직접 그린다. production clip/cache는 oracle에서 읽지 않는다. 두 DPI·7개 transform·5개 clip·중첩·둥근 상자/그림자·불투명/반투명 색의 **560프레임**에서 변경 전 실패 **320건**, 변경 후 **0건**. fractional AA **128건**은 정확히 거부한다. 비정상 saved descriptor **60건**은 draw/pixel 상태를 변경하지 않고 거부한다.
- 초기 oracle에서 unclipped primitive를 잘라 비교했을 때 Skia 자체의 clip-dependent edge rounding 2건이 검출되어 oracle를 수정했다. 현재 oracle는 실제 Direct2D binary mask의 측정 사각형으로 직접 그린다. 개발 중 중간 after 로그는 덮어썼으며 최초 차이 요약은 oracle-correction.json에 기록했다. 최종 frozen/new 두 구현은 동일한 교정 oracle로 다시 실행해 양쪽 로그를 보존했다.
- 기존 opacity/cache/ordinary/native/glyph/clip 검사 11개와 새 Skia 검사 통과. 전체 **828쌍**의 native/reference/CDP/WM_PRINTCLIENT decoded pixels·전체 JSON·raw 수치·판정이 v47과 같다. strict **727** + 기존 backend 승인 **75** = **802/828**. 기존 화살표 FAIL_PAINT **26건**은 유지한다. 새 예외 **0**, 채널 허용치 **0**, registry **111개**.
- 별도 고정 left-arrow mask·일반/dual-source D3D11 probe에서 CPU 모델은 native와 정확히 같다. 두 GPU 합성 방식도 reference 차이를 모두 해결하지 못했다. 합성 모델만 바꾸어 잔여 차이 원인을 단정하거나 성공 처리하지 않았다. scope는 두 DPI의 left arrow와 한 색/배경이다. [Skia 감마 구현](https://chromium.googlesource.com/skia/+/d2b9e48baf1697760afc1dc8ea3ad40110b8cacc/src/core/SkMaskGamma.cpp)을 대조했다. 진단은 production에 GPU 경로를 추가하지 않는다.

## 속도

초기 조건별 2,000회 페인트×구현별 6회 직렬 교대 원값을 보존했다. 아래 표의 단위와 추가 검토는 diagnostics/skia-performance-review.json의 iterations를 따른다. 실제 앱 개선율과 미세 벤치마크 비율을 혼합하지 않는다. 모든 최종 pixel hash 쌍이 같다. 보이는 영역의 증가값도 기록한다.

| 조건 | v47 ms | v48 ms | 변화 |
|---|---:|---:|---:|
| visible-one | 253.6430 | 262.6840 | +3.56% |
| visible-32 | 409.0830 | 408.7725 | -0.08% |
| empty-32 | 387.0810 | 0.3885 | -99.90% |

실제 Markdown·250표 자산은 각 구현/문서 6회씩 직렬 교대. BGRA **48쌍**, layout **24쌍** 일치. 첫 layout/paint와 scroll/input 시간 원값 및 범위를 performance-final/summary.json에 보존한다.

| 문서 | 구간 | v47 ms | v48 ms | 변화 |
|---|---|---:|---:|---:|
| normal | initialLayoutMs | 206.794 | 201.487 | -2.57% |
| normal | firstPaintMs | 107.485 | 106.851 | -0.59% |
| normal | interactiveDownMs | 15.511 | 16.219 | +4.56% |
| normal | interactiveDragMs | 16.183 | 16.810 | +3.88% |
| normal | scrollPaintMs | 21.363 | 20.622 | -3.47% |
| tables | initialLayoutMs | 476.400 | 464.095 | -2.58% |
| tables | firstPaintMs | 91.539 | 91.822 | +0.31% |
| tables | interactiveDownMs | 27.062 | 25.714 | -4.98% |
| tables | interactiveDragMs | 29.054 | 28.147 | -3.12% |
| tables | scrollPaintMs | 31.921 | 32.817 | +2.81% |

## 마지막 회귀·보관·복원

전체 회귀 12개·platform integrity·반디집 보관·새 폴더 복원·828쌍 새 렌더링·12개 독립 검사 로그 대조 완료. 반디집 ZIP fast level 1을 사용한다. 본 보관본은 source snapshot·실행기·runtime·영구 입력·캡처·회귀 로그·독립 검사·전후 속도 및 보호 입력을 포함한다. 새 폴더에서 모든 entry·소스·실행기·live 입력 해시, 저장 비교 코드의 재판정, 복원 실행기의 새 828쌍과 새/기존 12개 독립 검사 로그를 대조한다. 최종 결과 문서는 별도 반디집 ZIP으로 보관·새 폴더 복원한다.

작업 증거: D:/twf-v48. 전체 캡처: D:/twf-v48/runs/20261007-skia-clip-v48-final. 보관: D:/twf-v48/archives. 독립 검사: ScrollRenderingRegression.exe --skia-clip. 실행 순서: work/v48/Run-Accuracy.ps1 → 성능 증가 검토 → work/v48/Finish.ps1.

## 남은 범위

실제 Windows 창 DPI는 96이며 144는 명시적 renderer 배율이다. 실제 Windows 두 DPI·두 모니터·fractional AA group/layer·일반 affine primitive·전체 HTML/CSS/DOM/paint 계약·1,000문서는 미완료다. 작성 pilot 문서는 20개다. 이번 560프레임은 corpus 문서 수에 합산하지 않는다. 화살표 26건과 과거 보호 보관본 4,544개 실물 미확인·기존 security-profile HTML 2개 누락도 남는다. live 입력 신규 손실은 0이다. MdViewer 자동 재빌드는 하지 않았다.

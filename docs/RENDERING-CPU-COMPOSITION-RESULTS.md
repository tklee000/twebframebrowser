# 폰트·그라디언트 CPU 합성 결과와 남은 GPU 경로

이 문서는 폰트만 CPU로 전환한 이전 실행 기록이다. 이후 남아 있던 도형·그림자·화면 타깃도 모두 CPU로 전환했으며, 최신 상태는 [전체 CPU 전환 결과](RENDERING-ALL-CPU-RESULTS.md)를 따른다. 아래 수치와 GPU 목록은 당시 증거로 보존한다.

검증일: 2026-10-04  
최종 실행: `20261004-cpu-font-final-1791061810315`  
환경: Windows x64 Release, WebView2 `154.0.4258.53`, 96/144 DPI × 세 CSS viewport

**원본 120쌍 모두 성공 처리했다. 픽셀 완전 일치 96쌍, 승인된 폰트 CPU/GPU 차이 18쌍, 기존 그라디언트 차이 6쌍이며 남은 실패는 0쌍이다.** 전체 회귀 실행기 12개와 platform integrity도 통과했다. 원본 HTML/CSS 20개와 기준 이미지 120개의 픽셀은 이전 기준과 동일하다.

## CPU 합성 변경

`RasterSurface::DrawGlyphRun`에서 글자 마스크 텍스처 생성과 `rasterGpu.Blend` 호출을 제거했다. DirectWrite가 만든 LCD coverage를 CPU에서 계산하여 공유 WIC 표면에 직접 합성한다. 폰트 합성 경로에는 GPU 업로드와 동기 GPU→CPU 픽셀 회수가 없다. 폰트 선택, shaping, advance, baseline과 글자 좌표 판정은 유지한다.

그라디언트는 이전 작업의 CPU 생성·합성을 유지한다. `RasterSkia::RoundedRect`는 이미지/그라디언트 brush pixels가 제공되면 `sk_surface_new_raster_direct`로 CPU 표면을 사용한다. 단색 모서리와 그림자는 별도 GPU 경로가 남아 있다.

CPU 합성에도 glyph mask 생성, coverage 계산, WIC lock, `EndDraw`/`BeginDraw`와 최종 화면 업로드 비용은 남는다. 이번에는 성능 벤치마크를 실행하지 않았으므로 속도 개선율이나 FPS는 판정하지 않았다.

## 픽셀 판정

사용자 지시에 따라 **RGBA 완전 일치를 기본 목표로 유지하고 확인된 폰트·그라디언트 CPU/GPU 차이를 예외로 허용한다.** 화면 전체의 채널 허용치는 0이다. [승인 예외 목록](../TWebFrame2/tests/rendering/backend-pixel-exceptions.json)은 다음 증거를 고정한다.

- 원본 문서·DPI·CSS viewport, HTML/CSS 해시와 차이 종류
- 기준/native의 디코딩된 전체 BGRA 픽셀 해시, 차이 픽셀 수와 최대 채널 차이
- CPU 전환 전후 실행과 사용자 승인 사유

DOM·활성 스타일·추적 상자·문자 기하·캡처 안정성 검사가 모두 통과하고 등록된 픽셀이 그대로 재현되면 `PASS_BACKEND_DIFFERENCE`로 성공 처리한다. 새로운 픽셀 오류나 글자 이동·잘못된 색상이 추가되면 실패한다. PNG 압축이나 metadata만 달라진 동일 픽셀은 허용한다. `differentPixels`, `maximumChannelDelta`, `strictPass`와 차이 이미지는 보존한다. 새 문서의 차이는 원인을 확인한 뒤 등록해야 하며, 폰트 영역 전체를 무조건 제외하지 않는다.

`Run-RenderingComparison.ps1 -CompareOnly -StrictPixels -RunPath <run>`은 승인 예외를 끈 진단이다. 최종 캡처를 이 옵션으로 재판정해 **96 통과/24 페인트 차이**가 유지되는 것도 확인했다. 기본 실행의 성공 결과는 그대로 보존했다.

| 종류 | 비교 쌍 | 원래 차이 픽셀 합계 | 최대 채널 차이 | 판정 |
| --- | ---: | ---: | ---: | --- |
| 완전 일치 | 96 | 0 | 0 | PASS |
| CPU/GPU 폰트 합성 | 18 | 7,301 | 2 | PASS_BACKEND_DIFFERENCE |
| CPU/GPU 그라디언트 | 6 | 29,459 | 12 | PASS_BACKEND_DIFFERENCE |
| 합계 | 120 | 36,760 | 12 | 성공 120, 실패 0 |

폰트 차이는 `0017-latin-inline`, `0018-mixed-writing`, `0020-complex-dashboard`의 두 DPI와 세 viewport에서 발생한다. 대부분 최대 채널 차이는 1이며, 144 DPI 대시보드는 2다. 이전 GPU 폰트 실행에서는 이 18쌍이 완전 일치했고 엔진 소스 변경은 `RasterSurface.h`뿐임을 검증했다. 그라디언트 6쌍의 native 픽셀과 차이 수치는 이전 CPU 그라디언트 승인 실행과 동일하다. 96 DPI는 쌍마다 3,053픽셀·최대 12, 144 DPI는 6,766~6,767픽셀·최대 1이다.

## 회귀검사와 MdViewer

- 원본 120쌍 새 캡처: 120/120 성공, 문자 기하 24/24, DPI 왕복 안정성 120/120, 실제 창 DPI의 `WM_PRINTCLIENT` 60/60 동일
- 비교 교정 36개: 기존 28개와 승인 차이·PNG metadata 변경·새 픽셀 오류·색상·문서·DPI·문자 기하 오류 검출 8개 통과
- `Run-SupportCompatibility.ps1 -FullRegression`: 실행기 12개 및 platform integrity 통과
- JSON schema 22개와 캡처에 기록한 소스 해시 117개 검증
- MdViewer Release·Debug 재빌드, 각 설정의 core test와 실제 TWebFrame2 호스트 통합 검사 통과: 한국어 UI/마크다운, 표, 로컬 PNG 544×184, resize

**MdViewer 재빌드는 이번 작업에서만 수행했다. 이후 TWebFrame2를 수정해도 사용자의 별도 요청 없이 MdViewer를 자동으로 계속 재빌드하지 않는다.** 검증 대상은 [Release 실행 파일](../mdviewer_tinyversion/x64/Release/MdViewer.exe)과 [Debug 실행 파일](../mdviewer_tinyversion/x64/Debug/MdViewer.exe)이다. 자동 작업이나 감시 빌드는 추가하지 않았다.

## 회귀검사 완료 후 조사한 남은 GPU 경로

사용자 요청대로 전체 회귀 완료 뒤 공통 엔진 소스를 조사했다. 아래 경로는 위치와 픽셀 회수를 확인했으며 추가 CPU 전환은 하지 않았다.

| 대상 | GPU 사용 위치 | GPU→CPU 회수 |
| --- | --- | --- |
| SVG 경로, checkbox 체크 표시, select 화살표 | `RasterSurface::DrawGeometry` → D3D11 `GeometryMask`와 `Blend`. 호출부 `Layout.cpp:5096`, `:7714`, `:7825` | **있음.** MSAA coverage를 resolve/Map으로 읽고, 배경·마스크를 업로드해 합성한 결과도 다시 읽음. `RasterGpu.h:125`, `:272` |
| 원형·균일한 둥근 모서리의 단색 배경/테두리 및 조건에 맞는 native control | `RasterSurface::PaintCurve` → D3D11 `Circle`. `RasterSurface.h:192`, `Layout.cpp:663`, `:674` | **있음.** 배경 업로드 뒤 `CopyResource`·`Map(D3D11_MAP_READ)`·CPU 복사. `RasterGpu.h:234` |
| 단색의 서로 다른 모서리 radius, 해당 테두리, box shadow | Skia Ganesh/WGL. `Layout.cpp:658`, `:4547`, `:7623`; `RasterSkia.h:128`, `:152` | **있음.** blur는 tile별, blur가 없는 GPU 합성은 표면을 `sk_surface_read_pixels`로 읽음. `RasterSkia.h:217`, `:226` |
| 최종 View 화면 표시 | Direct2D HWND target와 compatible back buffer. `View.cpp:1553`, `:1571`, `:2323`, `:2389` | 일반 화면 표시 코드에는 명시적 GPU 회수가 없음. CPU WIC 화면을 bitmap으로 업로드하고 창에 제시 |

D3D11은 하드웨어 장치를 먼저 시도하고 실패하면 WARP를 사용한다. HWND Direct2D target은 DEFAULT이므로 실제 하드웨어 사용은 환경에 따른다. Canvas 2D의 공통 View 경로는 software WIC target에서 시작하며 별도 합성용 `RasterizeCanvasBitmap`도 SOFTWARE를 지정한다. Canvas 소스에서 별도의 GPU 장치 생성 경로는 찾지 않았다.

도형·그림자가 많은 화면에서는 남은 동기 GPU 읽어오기와 표면/텍스처 생성이 비용을 만들 수 있다. 이는 소스 구조에 따른 가능성이며 이번 작업의 성능 측정 결과는 아니다. 후속 업그레이드는 전체 합성을 GPU 표면에서 끝내고 일반 화면 표시 중 도형별 CPU 읽어오기를 제거하는 구조다. 현재 CPU 폰트·그라디언트와 검증 자료를 비교 기준으로 보존한다.

## 보존과 범위

실행 자료: [120쌍 결과](../TWebFrame2/tests/rendering/runs/20261004-cpu-font-final-1791061810315/summary.html), [원본·소스 검증](../TWebFrame2/tests/rendering/runs/20261004-cpu-font-final-1791061810315/continuation-audit.json), [전체 회귀 로그](../TWebFrame2/tests/rendering/runs/20261004-cpu-font-final-1791061810315/full-regression.log), [MdViewer 검증](../TWebFrame2/tests/rendering/runs/20261004-cpu-font-final-1791061810315/mdviewer-cpu-validation.json), [GPU 경로 조사](../TWebFrame2/tests/rendering/runs/20261004-cpu-font-final-1791061810315/gpu-path-audit.json).

ZIP은 20,294,523 bytes이며 [SHA-256 catalog](../TWebFrame2/tests/rendering/archives/20261004-cpu-font-final-1791061810315.json)에 파일별 해시를 보존한다. **2,680개 파일 복원·해시 확인, 복원한 소스 117개 해시 확인, 복원한 비교 코드로 120쌍 재판정**을 완료했다. 성공 120·완전 일치 96·승인 24와 raw 픽셀 수·최대 채널 차이가 모두 같았다. [복원 검증 기록](../TWebFrame2/tests/rendering/archives/20261004-cpu-font-final-1791061810315-recovery-validation.json)과 불변 기준 인덱스를 보존한다. 이전 입력·캡처·아카이브는 덮어쓰지 않았다.

이번 완료 범위는 기존 pilot 20문서·120쌍이다. 전체 1,000문서 계약, reference per-character glyph/baseline, 전체 스타일·다중 cluster coverage·실제 Windows 두 표시 설정과 독립 화면 캡처 검증 등은 [계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)에 남는다. 별도 최소 재현 54쌍의 이전 strict 결과 23 통과/31 실패는 별도 자료이며 이번 승인 120쌍에 합산하지 않았다.

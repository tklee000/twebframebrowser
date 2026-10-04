# 페인트 실패 원인 분석과 공통 수정 결과

실행일: 2026-10-03, Windows x64 Release, WebView2 `154.0.4258.53`. 최종 실행은 `20261003-193005-619-d5320f8d`다. [계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)과 [앞선 글꼴 선택 결과](RENDERING-ACCURACY-FONT-SELECTION-RESULTS.md)를 이어 진행했다.

**기본 20개 문서 × 두 DPI × 세 viewport의 120쌍은 84 통과·36 페인트 실패다.** 36은 여섯 문서가 각각 여섯 환경에서 실패한 수량이다. 이전 통과의 퇴행, DOM·활성 스타일·추적 상자·계측 문자 좌표 실패는 없다. 문자 좌표가 있는 24쌍도 통과했다. 픽셀 허용치와 글꼴 AA 예외는 계속 0이다.

36쌍 중 **21쌍에서 차이 픽셀이 감소**했다. 전체 차이 픽셀의 합은 **412,030 → 305,246**, 106,784픽셀·약 25.9% 감소다. 이는 진단상 개선이며 최종 통과를 대신하지 않는다. [최종 요약](../TWebFrame2/tests/rendering/runs/20261003-193005-619-d5320f8d/summary.json), [입력·기준·소스 감사](../TWebFrame2/tests/rendering/runs/20261003-193005-619-d5320f8d/continuation-audit.json), [수정 전후 영역별 픽셀 진단](../TWebFrame2/tests/rendering/runs/20261003-193005-619-d5320f8d/paint-diagnosis/paint-progress.json)을 보존한다.

## 확인한 원인과 수정

- **네 모서리 radius 처리 누락:** 기존 코드는 shorthand의 첫 값만 모든 모서리에 적용했다. 1~4개 값, `/`로 나눈 타원형 반지름, 백분율, 공통 겹침 정규화와 longhand override를 처리한다. CSS 변수와 shorthand reset도 연결했다. 배경·테두리·그림자·이미지 마스크·outline·padding-edge overflow가 같은 모서리 자료를 사용한다.
- **gradient 기준 영역 오류:** 기본 `background-origin:padding-box`를 border box로 계산했다. 기본 padding origin, 명시적 border/content origin과 `background-image` longhand를 처리한다. 배경 위치 영역과 기본 border-box 페인트 범위는 별개다.
- **144 DPI shadow source 단위 오류:** bitmap source rect에 물리 픽셀 크기를 DIP처럼 전달했다. 실제 bitmap DIP 크기를 사용하고 네 모서리·spread를 shadow cache key에 포함한다.
- **글꼴별 힌팅 모드 누락:** 모든 glyph에 natural symmetric을 강제했다. 실제 shaped face의 `gasp`, TrueType hint 정보, bitmap strike와 물리 em 크기로 모드를 선택하고 fallback run마다 적용한다. advance와 레이아웃은 유지한다. [Skia의 Windows DirectWrite 구현](https://skia.googlesource.com/skia/+/refs/heads/main/src/ports/SkScalerContext_win_dw.cpp)과 독립 WebView2 캡처를 함께 확인했다. gamma·색상·AA 허용치를 임의로 바꾸지 않았다.
- **UA border와 실제 컨트롤 외형 혼동:** 기본 버튼·입력의 2px layout border를 그대로 그렸다. native appearance에는 1 CSS px 둥근 frame을 그리며 content geometry를 유지한다. 네 방향 border 색/스타일, 배경 이미지·색, 작성자 radius와 `appearance:none`을 검사해 작성자 페인트를 보존한다. 기본 버튼의 계산 background도 맞췄다. 외형 구조는 [Chromium native theme](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/native_theme/native_theme_base.cc)를 참고하고 실제 색은 현재 WebView2 캡처에서 확인했다.
- **checkbox/select 장식과 내부 위치:** accent·disabled 색과 체크 경로를 보완했다. select 텍스트의 불필요한 1 CSS px 아래쪽 이동을 제거하고 현재 WebView2 기준의 열린 꺾쇠 화살표를 그린다. `appearance:none`에서는 화살표를 제거한다.

문서 ID나 특정 문자열별 우회는 없다. 원본 fixture나 기준 PNG를 수정하지 않았다.

## 원본의 수정 전후 차이

800×600 CSS viewport의 서로 다른 장치 픽셀 수다. 좌표 실패는 두 DPI 모두 0이다.

| 문서 | 96 DPI 이전 → 최종 | 144 DPI 이전 → 최종 | 남은 분석 대상 |
| --- | ---: | ---: | --- |
| `0012-radius-gradient` | 31,077 → 19,917 | 71,694 → 51,820 | gradient 양자화·dither, 곡선 coverage, shadow blur/합성 |
| `0013-svg-paint` | 1,098 → 1,098 | 1,622 → 1,622 | circle/path/stroke 경계 래스터링 |
| `0017-latin-inline` | 3,275 → 2,754 | 5,282 → 5,282 | glyph coverage·subpixel phase·gamma와 기준 baseline |
| `0018-mixed-writing` | 2,560 → 2,181 | 4,598 → 4,598 | 한글·RTL glyph 페인트와 baseline/coverage |
| `0019-form-initial-state` | 2,685 → 1,223 | 5,779 → 3,793 | 내부 텍스트 및 frame/check/chevron 경계 |
| `0020-complex-dashboard` | 2,611 → 2,205 | 3,990 → 3,990 | 텍스트와 원형 pseudo-element의 곡선/clip coverage |

96 DPI 폼의 checkbox 영역은 149 → 35픽셀, disabled checkbox는 156 → 6픽셀로 줄었다. 순차 수정마다 픽셀 수가 감소한다는 가정은 없다. 마지막 chevron 변경의 144 DPI 폼은 직전 3,783 → 3,793픽셀이지만 채운 삼각형이라는 형태 오류를 제거했다. 중간 캡처도 보존했다.

문자 좌표 통과만으로 남은 차이를 모두 허용 가능한 AA라고 판정하지 않는다. reference per-character glyph·baseline, 전체 cluster coverage와 색/clip/배경 오류를 숨기지 않는 AA 모델은 미완료다. gradient 채널 차이와 SVG·radius 경계는 글꼴 예외 대상이 아니다.

## 회귀·추가 재현

최종 소스로 **기존 회귀 실행기 12개와 platform integrity가 통과**했다. `FormControlRegression`에 두 DPI의 반대 모서리, longhand override, 작성자 border/background, 기본 native frame, 열린 select 화살표와 `appearance:none` 검사를 추가했다. 마지막 화살표 변경 뒤 전체 회귀와 120쌍을 다시 실행했다. 고의 오류 28개와 schema 22개도 통과했다.

최소 재현은 기존 20개를 유지하고 **25개**가 됐다. 새 문서는 `corner-radius-geometry`, `gradient-origin-boxes`, `shadow-dpi-scale`, `corner-radius-geometry-v2`, `native-control-paint-fallbacks`다. 별도 960×660·두 DPI의 **50쌍은 4 통과·46 실패**다. 기본 120쌍과 합산하지 않는다.

네 모서리 v2는 추적 상자 좌표가 모두 통과하고 픽셀 차이는 96 DPI **10,538 → 2,093**, 144 DPI **21,657 → 3,314**다. flex body 자동 높이 차이는 v1에 보존했다. 새 폼 재현의 select intrinsic width는 기준 112px·native 109.53125px로 실패한다. 이 별도 레이아웃 차이도 숨기지 않았다. 이전 최소 재현 20개의 HTML/CSS 해시는 동일하다.

[검증 기록](../TWebFrame2/tests/rendering/runs/20261003-193005-619-d5320f8d/verification/checks.json), 모든 실패·성공 로그, 최초 수정 후 120쌍 summary와 새 입력의 수정 전 캡처를 최종 run에 포함한다. `Diagnose-RenderingPaint.py`는 영역별 픽셀·채널 차이의 읽기 전용 진단이며 어떤 픽셀도 허용하지 않는다.

## 보존과 남은 범위

최초 입력 20개와 기준 PNG 120개의 디코딩 픽셀은 동일하다. 소스·계측 96개와 font 파일 23개의 해시, 기존 아카이브 3개의 불변성을 확인했다. 반복 캡처와 native DPI 왕복 120쌍, 실제 창 DPI 144의 `WM_PRINTCLIENT` 60쌍이 일치한다.

최종 행렬은 138.64초, 최대 문서 캡처 3.657초, 실행기 peak working set 147,890,176 bytes다. 전체 회귀를 동시에 실행한 환경이며 WebView2 자식 프로세스 메모리는 포함하지 않는다.

남은 36쌍을 성공으로 바꾸지 않았다. 다음은 glyph 기준 baseline·coverage와 내부 컨트롤 텍스트, gradient 양자화·shadow blur·SVG/원형의 비텍스트 경계를 독립 교정하는 작업이다. ui-monospace/default fallback, 전체 스타일·scroll/client·pseudo/iframe, 실제 Windows 96 DPI·모니터 전환, 독립 화면/색 교정, 100→1,000개도 남아 있다. `fullContractComplete`와 `actualWindowsBothDpiValidated`는 false다. 커밋·푸시·원격 업로드는 수행하지 않았다.

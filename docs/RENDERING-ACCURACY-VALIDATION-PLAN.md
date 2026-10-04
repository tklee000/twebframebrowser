# WebView2 기준 HTML·CSS·DOM·레이아웃·페인트 정확성 검증 계획

작성일: 2026-10-03  
대상: TWebFrame 자체 엔진, Windows x64 Release  
현재 상태: **계산 스타일 25개를 필수 비교로 보강했고 원본 120/120 성공(84 exact·기존 승인 36), 필수 값 17,400개·누락 0, 비교 교정 134개와 전체 회귀 12개·platform integrity가 통과했다. 초기값/상속/숨김/UA 교정의 스타일·구조는 18/18이며 별도 폼 페인트 6쌍은 실패로 유지한다. 최신 NVIDIA 기준과 이전 Intel 기준, 원본 입력 및 자체 픽셀을 보존했다. MdViewer 추가 재빌드는 하지 않았다. 전체 계약과 1,000문서는 미완료다.** [스타일 검증 결과](RENDERING-ACCURACY-STYLE-RESULTS.md), [독립 캡처·환경 교정 결과](RENDERING-ACCURACY-CAPTURE-CALIBRATION-RESULTS.md), [전체 CPU 전환 결과](RENDERING-ALL-CPU-RESULTS.md), [이전 CPU 합성 결과](RENDERING-CPU-COMPOSITION-RESULTS.md)를 함께 보존한다.

## 2026-10-04 사용자 승인에 따른 이번 작업 종료 조건

사용자는 그라디언트와 폰트의 GPU 합성을 CPU로 전환하고, CPU/GPU 렌더링 차이는 기록하여 예외로 허용하고 성공으로 처리하도록 지시했다. 최신 지시는 “최대한 픽셀 100%를 조건으로 하지만 폰트렌더링, 그라디에이션과 같은 CPU/GPU 렌더링에서 불일치가 오는 경우는 예외로 허용”이다. GPU 내부에서 전체 화면 합성을 끝내는 구조는 후속 업그레이드로 남긴다.

이 지시는 이전의 픽셀 완전 일치만 허용하는 종료 조건과 그라디언트 6쌍만의 일회성 승인을 대체한다. 기본 목표는 RGBA 완전 일치이며, 확인된 폰트·그라디언트 CPU/GPU 차이는 `backend-pixel-exceptions.json`에 원인·양쪽 디코딩 픽셀 해시·차이 수치를 등록한다. DOM·스타일·레이아웃·문자 기하·캡처 검사를 통과하고 등록된 차이가 그대로 재현되면 `PASS_BACKEND_DIFFERENCE`로 성공 판정한다. `differentPixels`, 최대 채널 차이와 `strictPass`는 그대로 기록한다. 새 차이가 추가되거나 비페인트 오류가 있으면 실패한다. `-StrictPixels`로 예외를 끈 진단도 가능하다. 수량·기능 coverage·실제 DPI 등 나머지 전체 계약은 유지한다.

최종 순서는 CPU 합성 적용 → 원본 120쌍의 raw 차이와 비페인트 검사 확인 → CPU/GPU 차이 등록 및 재판정 → 전체 회귀검사 → 다른 GPU 경로 조사 → 실행 자료 보존·복원이다.

MdViewer 재빌드는 이번 작업에만 한정한다. 사용자의 별도 요청 없이 이후 TWebFrame2 변경 때 MdViewer를 자동으로 계속 재빌드하지 않는다.

이후 사용자 지시: 모든 GPU 경로를 CPU로 복구하고 도형의 안티앨리어싱 차이도 같은 방식으로 성공·예외 처리한다. 폰트 성능 조사 요청은 사용자가 정상 동작을 확인한 뒤 취소했고 MdViewer 건은 제외했다. 이후 코드 보기의 세로 스크롤바 버그는 TWebFrame2에서 수정하도록 요청했다. 앱 소스 변경과 MdViewer 추가 재빌드는 하지 않고 실제 앱 CSS를 복사한 엔진 재현으로 검증한다.

후속 업그레이드는 View의 최종 합성 표면을 GPU에 유지하고 gradient·clip·border·shadow·text를 같은 표면에서 합성한 뒤 화면에 제시하는 구조다. 도형별 GPU→CPU 읽어오기를 없애고, CPU 읽어오기는 snapshot/출력 등 필요한 시점에만 수행한다. 변경 영역, 표면/리소스 재사용, 화면 크기·스크롤·연속 갱신의 프레임 시간과 전송량을 검증한다. 지금의 CPU 그라디언트와 현재 정확도 자료를 비교 기준으로 보존한다.

## 2026-10-04 계산 스타일 초기값·누락 검출

- [x] schema v4에서 25개 필수 계산 스타일과 계약 버전 검사. 빈 값·누락은 실패이며 기존 캡처의 건너뛴 값도 명시적으로 기록.
- [x] 실제 layout/paint 초기값·길이·색 해석으로 native 진단 보강. raw 스타일 보존, display:none 및 하위 요소의 상속과 상자 유무 분리.
- [x] SVG/input/select UA overflow, background currentColor 파서, RTL 고정 폭 블록 배치를 공통 수정.
- [x] 원본 120/120 성공(84 exact·기존 승인 36). 필수 스타일 17,400개·누락 0, 자체 픽셀 120쌍 불변, 독립 reference 캡처 120/120 일치.
- [x] 초기값/상속/숨김/UA 교정의 스타일·구조 18/18와 기대값 660/660, 별도 색/DPI 캡처 교정 18/18·357 표본, 고의 오류 134개 통과.
- [x] 기존 전체 회귀 12개와 platform integrity 통과. 실제 NVIDIA 기준 GPU 기록 12개 안정성 확인, Intel 기준 보존.
- [x] 소스 140개와 원본 20개 해시, 최종 3,867개 파일 ZIP 및 불변 baseline 보존.
- [x] 최종 파일/소스 복원과 120쌍 재판정. 복원한 실행기로 원본 120쌍 + 스타일 교정 18쌍 새 렌더링, 픽셀/DOM/스타일/기하/GPU/판정 동일. strict 교정의 실패 6쌍도 동일하게 재현, 별도 증거 4,014개 파일 보존/검증.
- [ ] 새 폼 교정의 페인트 6쌍 원인 분류/공통 수정. strict 교정은 12/18이며 미등록 차이는 FAIL_PAINT로 유지. 회색 경계 차이만으로 AA 예외를 추가하지 않음.
- [ ] 나머지 계산 스타일, client/scroll 정규화와 reference glyph/baseline·cluster·실제 Windows 두 DPI·화면 캡처 통합.

현재 범위와 새 실패, 복원 증거는 [스타일 검증 결과](RENDERING-ACCURACY-STYLE-RESULTS.md)를 따른다. 25개 필수 속성 완료를 전체 계산 스타일 계약 완료로 표시하지 않는다.

## 2026-10-04 독립 캡처·기준 그래픽 환경 교정

- [x] 모든 새 캡처에서 `CapturePreview`와 CDP PNG를 비교. schema v3의 누락·손상·다른 크기/픽셀은 실행기 오류이며 backend 예외로 통과시키지 않음.
- [x] 같은 브라우저의 내부 GPU 페이지에서 프로세스 전후의 실제 활성 GPU·ANGLE/Skia·feature status·display/color 정보를 수집. 12개 fingerprint 안정성을 검사하고 기준 환경 식별자에 포함.
- [x] 독립 RGB·source-over·1/2/100 CSS px·소수 상자·viewport/fixed 교정 3문서 × 6행렬, 18/18 strict 통과·표본 357/357 일치. corpus 수량에 합산하지 않음.
- [x] 기준 픽셀 환경 변화 조사: 보관 실행기의 대표 8쌍이 새 기준을 재현. 자체 120쌍 픽셀과 기준 DOM/스타일/좌표는 불변. 이전 기준 GPU는 미계측으로 남기고 새 환경을 별도로 보존.
- [x] 이미 승인한 기능의 새 환경 서명 24개만 추가. 원래 48개 유지, 일반 오차 허용치 0. 위험한 환경 전이의 거절 교정 6개 통과.
- [x] 최종 120/120 성공(96 exact·24 승인), 캡처 경로 120/120·문자 기하 24/24·실제 96 DPI 출력 60/60·비교 교정 47개 통과.
- [x] 최종 아카이브 3,258개 파일·소스 129개 복원/해시 확인, 복원한 코드로 120쌍 동일 재판정. 이전 raw 실패 실행도 별도 아카이브로 보존.
- [x] 복원한 실행기·DLL·입력·계측/비교 코드로 전체 120쌍 새 렌더링. 기준/자체/CDP 픽셀·DOM/스타일/문자 기하·GPU fingerprint·판정 동일. 복원 증거 2,494개 파일을 별도 ZIP/catalog로 보존·검증.

이 단계 이후 native 필수 계산 스타일 25개의 초기값/누락 검출을 보강했다. 다음은 새 폼 페인트 6쌍 원인 분류/수정과 client/scroll 정규화 → 나머지 계산 스타일과 reference 글자별 glyph/baseline·cluster coverage → 실제 Windows 두 DPI 및 화면 캡처 통합 → 100문서 단계다. 전체 계약의 미완료 항목을 현재 pilot 통과로 완료 처리하지 않는다. 상세 수치와 환경 변화의 한계는 [교정 결과](RENDERING-ACCURACY-CAPTURE-CALIBRATION-RESULTS.md)를 따른다.

## 1. 목표와 이번 범위

동일한 HTML·CSS·리소스를 WebView2와 TWebFrame에서 렌더링하고, DOM 구조, 스타일, 레이아웃, 최종 화면을 비교한다. **최대한 모든 장치 픽셀의 RGBA 100% 일치를 목표로 하되, 확인·등록된 폰트·그라디언트·도형 안티앨리어싱 CPU/GPU 렌더링 차이는 허용한다.** 문서는 누적 1,000개를 만들며, 100%와 150% DPI에서 모두 검사한다. 실패와 허용 예외는 문서별로 추적하되 수정은 공통 엔진에 반영한다.

| 항목 | 이번 검증 계약 |
| --- | --- |
| 기준 엔진 | 실행 환경에 고정한 실제 WebView2 런타임. Chromium 일반 브라우저의 결과로 대체하지 않는다. |
| 입력 | 스크립트 없는 정적 HTML·CSS, 로컬 글꼴·이미지·SVG·iframe 문서. 두 엔진에 동일한 파일과 URL 의미를 제공한다. |
| 복잡도 | 단일 기능 진단 문서와 자유로운 복합 레이아웃을 함께 구성한다. 중첩 flex/grid/table/block, 긴 텍스트, 겹침, overflow, 변형, 반응형 조건을 조합한다. |
| 비교 | DOM → 계산 스타일 → CSS 좌표의 레이아웃 → 실제 장치 픽셀의 페인트 순서로 원인을 찾는다. 최종 통과에는 레이아웃과 페인트가 모두 일치해야 한다. |
| 배율 | 기본 배율 두 개는 96 DPI(100%), 144 DPI(150%). 페이지 확대는 두 환경 모두 `ZoomFactor = 1.0`. 브라우저 페이지 확대 검사는 별도 항목으로 관리한다. |
| 보존 | 생성 문서, 리소스, seed, 생성기 버전, manifest, 기준 결과, 실패 결과와 최소 재현 문서를 보존한다. 재실행 시 삭제하거나 덮어쓰지 않는다. |
| 수정 | HTML 파서·DOM, 공통 CSS 처리, 레이아웃, 글꼴·이미지·페인트, View의 DPI 경로를 수정한다. 문서 ID나 특정 문자열에 따른 우회 처리를 넣지 않는다. |
| JavaScript | 페이지 스크립트와 JavaScript에 의한 DOM 생성·변경·이벤트 동작은 이번 corpus에서 제외하고 후속 단계로 둔다. 공통 JavaScript 실행기는 이번 실패의 원인이 실제 공유 코드로 확인된 경우에만 수정하고 기존 회귀검사로 확인한다. |

1,000개는 무한한 HTML/CSS 조합을 모두 열거하는 수량이 아니다. WebView2 지원 기능 목록을 바탕으로 **값의 경계 조건, 기능 쌍의 조합, 중요한 세 기능 조합, 깊은 중첩 구조**를 체계적으로 선정한다. 검사한 범위와 검사하지 않은 범위를 별도로 공개하며, 1,000개 통과를 전체 CSS 표준의 100% 지원으로 표현하지 않는다.

## 2. 기존 자산과 보완할 부분

| 기존 자산 | 활용 방법과 한계 |
| --- | --- |
| `TWebFrame2/tests/CSSCompatibilityRegression.cpp` | WebView2 연결·런타임 버전 기록·공통 CSS 사례를 재사용한다. 현재 비교는 일부 요소의 가로 크기와 색상이며 전체 화면의 픽셀 비교가 아니다. 기존 70개 사례와 새 문서 수량을 구분한다. |
| `TWebFrame2/tests/ScrollRenderingRegression.cpp` | 명시적 DPI의 Direct2D/DirectWrite 래스터 타깃, 재배치, 부분 갱신 검사를 참고한다. 내부 Layout 경로만의 성공을 실제 View 전체 성공으로 간주하지 않는다. |
| `TWebFrame2/include/TWebFrame/TWebFrame.h` | `SetPageScriptsEnabled(false)`로 페이지 스크립트를 비활성화한다. `DumpLayoutJson()`은 정수 좌표 중심이므로 소수 좌표·텍스트 조각을 제공하는 별도 진단 출력을 준비한다. |
| `tools/Capture-TWebFrame.ps1` | 기존 특정 GUI의 Win32 캡처 절차를 참고한다. 고정 대기 시간·화면 좌표 클릭에 의존하므로 1,000개 문서를 처리하는 전용 실행기가 필요하다. |
| `tools/Compare-Screenshots.ps1` | 진단용 이미지 차이 표시를 참고한다. 기본 RGB 허용치 24를 화면 전체에 적용하는 현재 방식은 이번 통과 판정에 사용하지 않는다. |
| `TWebFrame2/tests/Run-SupportCompatibility.ps1` | 엔진 수정 뒤 기존 전체 회귀검사를 실행한다. 새 비교 실행기 연동은 단계적으로 추가한다. |

현재 구현과 남은 기능은 [CSS 엔진과 WebView2 비교](CSS-WEBVIEW2-COMPATIBILITY.md)에 기록되어 있다. 기존 `tests/artifacts`는 기본적으로 Git에서 제외되므로 영구 corpus를 그 아래에 만들지 않는다. 과거 보존 자료와 `TWebFrame2/src.zip`도 건드리지 않는다.

## 3. 재현 환경과 DPI 계약

### 3.1 기준 환경 고정

- 기존 비교에서 확인한 런타임은 `154.0.4258.53`이다. 새 실행기의 시작 시점에 실제 버전을 다시 읽고 그 버전을 기준으로 등록한다.
- 가능하면 Fixed Version WebView2 런타임을 사용한다. Evergreen을 사용하면 실행 전후 버전이 같은지 검사하고, 바뀐 버전의 결과는 별도 환경으로 분리한다. 기존 기준 이미지를 자동으로 갱신하지 않는다.
- 환경 manifest에는 WebView2/SDK 버전, TWebFrame Git SHA와 미커밋 변경의 해시, 실행기 버전, Windows 빌드, GPU·드라이버, 하드웨어/소프트웨어 렌더링 모드, 테마, 색 프로필, DirectWrite·ClearType 설정을 기록한다.
- 사용 글꼴의 파일 해시·버전과 대체 글꼴을 기록한다. Windows 텍스트 크기는 100%로 고정하며 locale, 색상 테마, `prefers-*` 관련 설정도 고정한다.
- 동일한 URL 구조의 로컬 리소스를 사용한다. 외부 네트워크, 날짜·난수에 따른 입력, 원격 글꼴, 동영상, 움직이는 이미지에 의존하지 않는다. 캐시·사용자 프로필·방문 이력·스크롤 복원 상태는 실행별로 격리한다.
- 기준 이미지가 반복 캡처에서 변하면 엔진 불일치로 판단하기 전에 환경 불안정으로 기록한다. 불안정 사례도 삭제하거나 통과 수량에 포함하지 않는다.

### 3.2 CSS 픽셀과 장치 픽셀 분리

| 설정 | 100% | 150% |
| --- | ---: | ---: |
| DPI | 96 | 144 |
| 래스터 배율 | 1.0 | 1.5 |
| 페이지 확대 | 1.0 | 1.0 |
| 예시 CSS viewport | 800 × 600 | 800 × 600 |
| 예시 캡처 크기, 장치 픽셀 | 800 × 600 | 1,200 × 900 |

WebView2 전용 호스트는 Per-Monitor DPI Awareness V2를 사용한다. `ICoreWebView2Controller3`의 자동 모니터 배율 감지를 끄고 명시적 `RasterizationScale`을 설정하며, raw-pixel Bounds에는 CSS viewport에 배율을 곱한 크기를 넣는다. `BoundsMode`, 래스터 배율과 페이지 확대는 서로 구분하여 기록한다. 이 설정의 의미는 [Microsoft의 Controller3 문서](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2controller3?view=webview2-1.0.3595.46)에 따른다.

설정값만 믿지 않고 viewport 메트릭, `devicePixelRatio`, PNG 크기, 1 CSS px 기준선과 100 CSS px 기준 상자의 실제 픽셀 폭을 확인한다. 좌표에 배율을 두 번 적용하거나 150% 이미지를 100% 크기로 리사이즈해서 비교하지 않는다. 소수 경계의 최종 픽셀 스냅도 검사한다.

TWebFrame은 CSS 좌표로 레이아웃하고 최종 타깃에 96/144 DPI를 적용한다. 실제 `View`의 `GetDpiForWindow`, `WM_DPICHANGED`, Direct2D DPI, 클라이언트 크기, 스크롤·클리핑 좌표가 같은 배율을 사용하는지 확인한다. 내부 오프스크린 타깃의 명시적 DPI 검사는 원인 분리에 활용하고, 최종 View 검증은 Windows 배율이 실제 100%·150%인 환경에서 수행한다. 에뮬레이션 결과와 실제 Windows DPI 결과를 구분한다.

같은 CSS viewport의 두 DPI에서 배치가 유지되는지 검사하되, `resolution` 미디어 쿼리·`srcset` 등 DPI에 따라 의도적으로 달라지는 입력은 **각 DPI의 WebView2 결과**와 비교한다. DPI 사이의 PNG를 단순 확대해 기준으로 삼지 않는다.

## 4. 검사 행렬

기본 viewport는 `384 × 768`, `800 × 600`, `1,280 × 800` CSS px로 정한다. 모바일 기기 에뮬레이션은 켜지 않고 viewport 폭에 따른 정적 반응형 동작을 검사한다.

```text
1,000개 문서 × 2개 DPI × 3개 CSS viewport = 기본 6,000개 비교 쌍
각 비교 쌍 = WebView2 기준 캡처 + TWebFrame 캡처 + DOM/스타일/레이아웃 비교
```

- 스크롤 문서는 상단 외에 지정한 중간·하단 및 내부 스크롤 상태를 추가 검사한다. 호스트 입력/스크롤 기능으로 같은 CSS 좌표에 맞추고 측정값을 확인한다. JavaScript로 `scrollTop`이나 DOM을 변경하지 않는다.
- fixed/sticky/overflow는 원래 viewport 크기를 유지한 상태로 캡처한다. 전체 페이지 촬영을 위해 viewport 높이를 늘려 배치를 바꾸지 않는다. 긴 문서는 실제 viewport의 여러 상태로 검사한다.
- 필수 속성별 경계값, 예를 들어 미디어 조건 직전·경계·직후 폭은 사례 manifest에 추가한다. 추가 상태와 viewport 검사 수량은 기본 6,000개와 따로 보고한다.
- 캡처 안정성 반복 검사, 새 프로세스/재사용 프로세스 검사, 100%↔150% 전환 검사는 추가 행렬이다. DPI 전환 후 폰트·이미지·레이아웃 캐시가 올바르게 갱신되는지도 확인한다.
- 마지막 단계의 6,000개 비교는 실제 View 경로와 두 Windows DPI 환경을 포함한다. 오프스크린 내부 검사만 실행한 환경에는 최종 완료 표시를 하지 않는다.

## 5. 영구 문서 1,000개 구성

### 5.1 주 분류별 목표 수량

각 문서는 주 분류를 하나만 갖고, 조합한 모든 기능은 복수 태그로 기록한다. 아래 수량은 정확히 1,000개이며, 초기 발견 결과에 따라 분류 간 수량을 조정하면 manifest와 계획에 변경 이유를 남긴다.

| 주 분류 | 문서 수 | 대표 검사 내용 |
| --- | ---: | --- |
| HTML·DOM·선택자·캐스케이드 | 100 | HTML 오류 복구, 암시적 요소, 공백·엔티티, 목록·폼의 초기 상태, 상속, 우선순위, 변수, layer, nesting, 의사 요소 |
| block·box·고유 크기 | 120 | margin collapse, box sizing, auto/min/max, 백분율, calc, aspect-ratio, float/clear, 이미지의 고유 크기 |
| inline·글꼴·텍스트 | 110 | 한글·라틴·CJK·RTL 혼합, 줄바꿈, whitespace, baseline, line-height, fallback, bidi, writing-mode, decoration |
| flex | 130 | 중첩, wrap, grow/shrink, auto 최소 크기, gap, order, align/baseline, intrinsic 크기와 overflow |
| grid | 140 | auto-placement, minmax/fr, spanning, implicit track, intrinsic track, subgrid 및 다른 배치 문맥과의 결합 |
| table | 80 | auto/fixed layout, colspan/rowspan, caption, collapsed border, 빈 셀, 테이블 안의 flex/grid·긴 텍스트 |
| position·stacking·clip·scroll | 110 | absolute/fixed/sticky, containing block, z-index, 중첩 스크롤, clip/overflow, transform으로 생성한 문맥 |
| 배경·border·이미지·페인트 | 90 | 다중 배경, gradient, radius, shadow, opacity, transform, SVG, object-fit, mask/filter/blend 등 실제 기준 런타임 지원 기능 |
| 반응형·조건·단위 | 70 | media/supports/container 조건, 논리 속성, viewport/container/글꼴 단위, DPI 조건, 선언 fallback |
| 종합 화면 | 50 | 대시보드·문서·카드·사이드바·상품 목록 등 여러 배치 문맥과 복합 페인트를 함께 사용하는 화면 |
| **합계** | **1,000** | |

기준 WebView2에서 지원하지 않는 기능은 별도 목록에 두고 두 엔진의 fallback 동작을 확인한다. TWebFrame에서 미지원이라는 이유로 WebView2가 지원하는 사례를 corpus에서 제거하지 않는다. SVG·정적 iframe·폼 기본 모습·UA 스타일도 기록하며, 지원이 어려운 기능의 실패를 숨기지 않는다.

### 5.2 복잡도와 조합 원칙

| 복잡도 | 문서 수 | 구성 원칙 |
| --- | ---: | --- |
| 단일 원인 진단 | 120 | 실패 원인을 빠르게 구분하는 작은 문서. 소수 크기, 0/auto, 경계값 포함 |
| 복합 기본 | 280 | 두세 기능의 상호작용, 다양한 HTML 구조와 텍스트 길이 |
| 복잡한 레이아웃 | 500 | 서로 다른 기능군 3개 이상과 3단계 이상의 중첩 배치, 다수 요소·페인트 겹침 |
| 스트레스·종합 복합 | 100 | 깊은 중첩, 수백 요소, 긴 콘텐츠, 다중 문맥·스크롤·변형 |
| **합계** | **1,000** | 주 분류 수량과 교차하는 별도 축 |

전체 중 600개는 복잡한 레이아웃 또는 스트레스 문서다. 색상이나 문자열만 바꾼 동일 템플릿 1,000개로 수량을 채우지 않는다. DOM 구조, 배치 문맥, intrinsic sizing, 클리핑·페인트 관계가 실제로 달라지도록 생성한다.

기능 목록에는 구문, 값/단위, 레이아웃 문맥, 부모·자식 관계, 페인트, DPI를 독립 축으로 둔다. 유효한 조합의 pairwise coverage를 생성하고, `grid × intrinsic text × overflow`, `flex × percentage/min-size × wrap`, `transform × sticky × clip`, `writing-mode × logical sizing × table` 같은 주요 세 기능 조합은 명시적으로 추가한다. coverage 분모와 불가능한 조합의 제외 이유를 보존한다.

고정 seed의 제약 있는 생성기와 사람이 작성한 종합 문서를 함께 사용한다. 정상 HTML 중심의 문서와 의도적인 HTML/CSS 오류 복구 문서를 구분한다. 모든 문서에는 UTF-8, doctype/quirks 모드, viewport, 글꼴, 기능 태그, seed, 리소스 해시, 기대하는 관측 항목을 기록한다. 생성된 문서는 파일로 확정하고, 반복 실행 중 다시 생성하지 않는다.

CSS animation/transition은 작성 단계에서 고정 시점의 정적 상태로 만들거나 별도 보류 항목으로 둔다. 전역 CSS 덮어쓰기로 검사 기능을 없애지 않는다. 사용자 입력에 따른 상태 전환, JS DOM 조작, Custom Elements의 스크립트 동작, 동적 Shadow DOM 등은 후속 범위로 기록한다. 이런 보류 범위를 이번 통과율의 분모에 몰래 합치지 않는다.

## 6. 캡처·측정 실행기 설계

전용 네이티브 실행기 `RenderingComparisonRegression`과 PowerShell 실행 스크립트를 만든다. WebView2는 기준을 제공하고, TWebFrame은 자체 엔진으로 렌더링한다. 다음 순서를 문서·DPI·viewport마다 실행한다.

1. 환경과 입력 파일 해시를 확인하고 새 문서/격리된 리소스 상태를 준비한다.
2. 같은 viewport·DPI·배경·테마·글꼴·로컬 URL로 양쪽을 설정한다. 페이지 스크립트는 비활성화한다.
3. HTML 파싱, 스타일시트·이미지·글꼴·iframe 로딩, 레이아웃 및 최종 페인트 완료를 확인한다. 고정 `Sleep`만으로 완료를 결정하지 않는다.
4. 리소스 실패가 없고 DOM/레이아웃 및 연속 캡처가 안정적인지 확인한다. 제한 시간 초과, 잘못된 PNG 크기, 이전 페이지 촬영은 실행기 실패로 처리한다.
5. WebView2 기준 PNG와 DOM/스타일/레이아웃 데이터를 먼저 보존한다. TWebFrame의 같은 데이터를 캡처한다.
6. 정규화와 비교를 수행하고 모든 차이를 파일로 저장한다. 여러 문서 중 하나가 실패해도 전체 실패 목록을 수집한다.
7. 결과를 원자적으로 확정하며, 중단 후에는 완료된 동일 입력/환경 항목만 재사용한다. 실패·미완료를 성공으로 재사용하지 않는다.

WebView2 화면 캡처는 PNG `CapturePreview`를 우선 검증하고, 필요하면 CDP `Page.captureScreenshot`을 비교 검증한다. 캡처 전 현재 navigation의 로딩 완료를 확인하고, 완료 콜백을 기다린다. API와 CDP 호출 순서는 [Microsoft의 ICoreWebView2 문서](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.3595.46#capturepreview)에 따른다. 두 캡처 경로의 DPI·색·알파 차이를 교정 문서로 확인한 뒤 정식 경로를 고정한다.

TWebFrame 캡처는 실제 `View`의 공통 페인트 결과를 읽는 방식으로 구현한다. 화면 가림이나 `PrintWindow` 지원 여부에 영향을 받는 캡처는 검증 없이 사용하지 않는다. 테스트용 캡처 훅이 필요하면 같은 페인트·클립·리소스 코드를 사용하고 최종 타깃의 픽셀을 읽는다. 내부 `LayoutEngine` 래스터 캡처는 추가 원인 진단에 사용한다. 두 경로의 렌더링 차이가 있으면 View 통합 오류로 남긴다.

WebView2의 구조 측정에는 `DOMSnapshot.captureSnapshot`과 필요한 DOM/CSS CDP 조회를 사용한다. computed style 목록, DOM rect, paint order, text box는 [공식 DOMSnapshot 프로토콜 정의](https://github.com/ChromeDevTools/devtools-protocol/blob/master/pdl/domains/DOMSnapshot.pdl)에 맞춰 수집하며, 실제 고정 런타임의 프로토콜에서 지원되는지 확인한다. 텍스트 조각 표현은 런타임 버전별로 정규화한다.

계측은 읽기 전용으로 한정한다. CDP만으로 필요한 값이나 글꼴 준비 상태를 얻지 못하면 `getBoundingClientRect`, `getComputedStyle`, `document.fonts.ready` 등 읽기 전용 WebView2 계측을 별도로 허용한다. fixture에는 스크립트를 추가하지 않고 DOM·스타일·스크롤을 변경하지 않는다. 이 계측은 TWebFrame JavaScript 실행기나 JS DOM 제어의 적합성 검사에 포함하지 않는다.

## 7. 일치 판정 기준

### 7.1 DOM·스타일·레이아웃

| 계층 | 판정 및 기록 |
| --- | --- |
| DOM | 정적 파싱 결과의 요소 순서·부모 관계·속성·텍스트·엔티티·암시적 HTML 요소를 비교한다. 스크립트 없이 생성되는 의사 요소와 자식 문서도 대응시킨다. |
| 스타일 | 영향을 주는 계산 스타일, 상속, 변수 해석, 기본 스타일과 cascade 결과를 비교한다. 문자열 표기 차이는 속성 의미를 유지하며 정규화한다. |
| box geometry | border/content rect, margin/padding, 소수 좌표, 변형 전후 경계, scroll/client 크기, clipping 경계를 비교한다. CSS px와 장치 px를 별도 저장한다. |
| 텍스트 | 내용, 사용 글꼴, 줄 수, 줄바꿈 위치, 텍스트 조각의 논리 범위와 위치·advance·baseline을 비교한다. 계측 가능한 값은 직접 비교하고 나머지는 별도 기준 문서/픽셀로 확인한다. |
| 페인트 관계 | stacking context, 상대적인 앞뒤 순서, 의사 요소, 겹침, 가림, 클리핑 결과를 비교한다. 두 엔진 내부의 paint-order 숫자가 같아야 한다고 가정하지 않는다. |

요소에 안정된 `data-case-node` 식별자를 넣고 텍스트 노드·의사 요소·iframe은 경로를 조합하여 대응시킨다. 브라우저별 익명 box의 내부 구조가 달라도 보이는 결과와 의미 있는 텍스트 조각을 비교할 수 있도록 정규화한다. 엔진의 DOM 오류를 정규화로 지우지 않는다.

좌표 JSON의 표현 오차 상한은 **1/64 CSS px**다. 최종 픽셀 차이가 등록된 CPU/GPU 예외에 해당하지 않으면 페인트 실패다. 실제 1px 이동, 잘못된 줄바꿈·글꼴·baseline·overflow는 CPU/GPU 차이로 인정하지 않는다.

### 7.2 픽셀 100% 기본 목표와 CPU/GPU 예외 판정

**텍스트·비텍스트의 RGBA 완전 일치를 기본 목표로 하며, 확인된 폰트·그라디언트·도형 안티앨리어싱 CPU/GPU 차이를 허용 예외로 제외한다.** PNG 파일의 압축 바이트 대신 디코딩한 픽셀을 비교한다. 이미지 이동·리사이즈·블러로 차이를 없애지 않는다.

글꼴은 CPU에서 합성한다. GPU와의 coverage·AA·양자화·합성 차이를 수치와 이미지로 보존하고 원인을 확인한 뒤 등록한다.

- 텍스트 내용·실제 글꼴·줄바꿈·위치·advance·baseline·색상·opacity를 일치시킨다.
- 글리프 coverage·양자화와 배경 합성의 CPU/GPU 차이는 허용 예외로 추적한다.
- 경계·내부·주변 픽셀을 모두 측정한다. 등록된 양쪽 픽셀 해시·문서/DPI/viewport·차이 수치가 재현될 때만 예외를 적용한다.
- 글꼴 교체, 글자 이동·누락, 잘못된 clip, 새 배경·border·그림자 오류는 실패다.

그라디언트·단색 모서리·그림자는 Skia CPU raster-direct, 도형·SVG·폼은 소프트웨어 Direct2D, 폰트는 CPU LCD 합성을 사용한다. 전환으로 확인된 도형/그림자 안티앨리어싱 차이는 `CPU_GPU_RASTER_ANTIALIASING`으로 같은 증거 등록 절차를 적용한다. mask·transform·border·shadow·SVG·이미지도 기본은 픽셀 완전 일치이며, 등록되지 않은 차이는 원인 확인 전까지 실패다. 스크롤 범위 등 실제 레이아웃 오류를 CPU/GPU 예외로 분류하지 않는다.

결과에는 전체 다른 픽셀 수, 최대 채널 차이, 차이 경계, 구조 불일치 목록을 저장한다. `allowedFontAAPixels`와 `allowedBackendDifferencePixels`에 허용 수를 기록하고 `disallowedDifferentPixels`에는 승인 예외를 제외한 차이를 기록한다. **통과 조건은 실행 환경/리소스 검증 성공, DOM·스타일·레이아웃·문자 기하 일치, 예외를 제외한 다른 픽셀 수 0이다.** 차이 0은 `PASS`, 승인된 차이만 있는 결과는 `PASS_BACKEND_DIFFERENCE`로 구분한다. `strictPass`와 raw 차이는 진단으로 보존한다.

### 7.3 비교 도구 자체의 검증

교정 문서는 단색·1px 선·소수 경계·반투명 합성·이미지·텍스트·radius/gradient/clip을 포함한다. 같은 결과끼리 비교하면 통과하고, 다음 고의 오류를 넣으면 반드시 실패해야 한다.

- 상자나 글자 1 장치 px 이동, 한 줄의 줄바꿈 변경.
- 배경·border·글자 색 한 채널 변경, 글꼴 교체.
- 요소/글자 누락, 이미지 잘못된 크기, 잘못된 z-index/clip.
- 알파 잘못된 합성, 150% 캡처의 잘못된 크기 또는 이중 배율 적용.
- 글꼴 AA 경계의 한 픽셀·한 채널을 1 변경한 오류와 텍스트 사각형 안의 비텍스트 오류.

승인된 CPU/GPU 샘플의 성공과 PNG 인코딩 독립성을 확인한다. 같은 승인 샘플에 구조 오류를 추가하거나 색·다른 픽셀·문서/DPI가 달라지면 예외로 통과하지 않아야 한다.

교정·고의 오류 검사가 통과하기 전에는 corpus 통과율을 신뢰할 수 있는 수치로 발표하지 않는다.

## 8. 파일과 결과 보존

다음은 목표 구조다. 현재 실행기, generator, fixtures 20개, coverage, schemas, repros, runs, archives와 불변 baseline 인덱스를 구현했다. 아직 생성하지 않은 1,000개 문서와 진단 overlay 등의 항목은 목표로 남아 있다. 실행 결과의 실제 디렉터리 배치는 [실행 안내](../TWebFrame2/tests/rendering/README.md)를 따른다.

```text
TWebFrame2/tests/rendering/
  README.md
  corpus-manifest.json
  feature-inventory.json
  coverage.json
  schemas/
  generator/                  # 고정 seed, 생성기 버전, 조합 제약
  assets/                     # 글꼴·PNG·SVG·리소스 라이선스와 해시
  fixtures/
    0001-basic-box/
      index.html
      style.css
      case.json
    ...
    1000-complex-layout/
  repros/                     # 실패에서 추출한 최소 재현; 원본은 유지
  baselines/
    <environment-id>/         # WebView2 버전·환경별 불변 기준
      manifest.json
      <case-id>/<dpi>/<viewport>/<state>/
        reference.png
        dom.json
        styles.json
        layout.json
  runs/
    <run-id>/
      environment.json
      input-manifest.json
      summary.json
      summary.html
      failures.json
      <case-id>/<dpi>/<viewport>/<state>/
        native.png
        reference.png
        diff.png
        overlay.png
        font-aa-mask.png
        layout-diff.json
        result.json
        logs.txt

TWebFrame2/tests/RenderingComparisonRegression.cpp
TWebFrame2/tests/RenderingComparisonRegression.vcxproj
TWebFrame2/tests/Run-RenderingComparison.ps1
```

HTML·CSS·manifest·생성기·필수 로컬 리소스는 Git에서 버전 관리한다. 기준 PNG와 실행 결과는 크기를 pilot에서 측정한 뒤 Git LFS 또는 해시 manifest를 갖춘 영구 아카이브로 관리하고, 복원 방법과 보관 위치를 README에 기록한다. 로컬 Git ignore만 설정한 상태를 영구 보존 완료로 간주하지 않는다. 아카이브 방식과 무관하게 자동 삭제·자동 덮어쓰기 정책을 넣지 않는다.

문서 ID는 재사용하지 않는다. 생성 후 입력을 변경해야 하면 이유와 새 해시·버전을 남기고 기존 버전도 보존한다. 최소 재현 문서는 원본을 대체하지 않는다. WebView2 업그레이드 시 새 기준 디렉터리를 만들고 구버전 결과를 유지한다. 수동 기준 갱신도 변경 사유와 전후 차이를 기록한다.

개별 결과는 `PASS`, `FAIL_DOM`, `FAIL_STYLE`, `FAIL_LAYOUT`, `FAIL_TEXT`, `FAIL_PAINT`, `HARNESS_ERROR`, `UNSTABLE_REFERENCE`, `PENDING` 등으로 구분하고 복수 실패 원인도 보존한다. 현재 비교기는 복수 불일치가 있는 결과의 최상위 상태를 `FAIL`로 쓰고 `failures[].kind`에 원인을 기록한다. 미실행·불안정·시간 초과·알려진 미지원은 통과가 아니다. 보고서에는 전체 수량, 실행 수량, 실패, 미실행과 coverage를 함께 표시한다.

## 9. 단계별 실행 계획

문서 수는 **누적 수량**이다. 앞 단계 문서는 다음 단계에서 유지하고 다시 검사한다. 각 단계의 완료 조건이 충족되기 전에는 다음 단계가 완료되었다고 표시하지 않는다.

| 단계 | 작업 | 산출물 | 완료 조건 |
| --- | --- | --- | --- |
| 0. 계약·환경 | 기준 런타임/폰트/색/DPI/viewport/AA 정책 고정, 실제 WebView2 기능 목록과 현재 엔진 차이 정리 | 환경 schema, 기능 목록, 비교 계약 | 기준 버전과 보류 범위가 명확하며 입력·환경을 재현할 수 있음 |
| 1. 캡처·DPI 교정 | 양쪽 실제 렌더 경로 캡처, 로딩 안정성, 소수 좌표 출력, 96/144 DPI와 실제 View 경로 검증 | 실행기 골격, 교정 문서, 캡처/좌표 JSON | 반복 캡처가 안정적이고 픽셀 크기·DPR·CSS 좌표·DPI 전환이 올바름 |
| 2. 판정 도구 | DOM/스타일/레이아웃 정규화, raw pixel diff와 CPU/GPU 예외, HTML 보고서 | diff 도구, 판정 설정, 고의 오류 검사 | 예외를 제외한 다른 픽셀 수 0이며 고의 오류 검사가 통과함 |
| 3. pilot 20개 | 모든 주요 분류의 대표 문서 20개 작성, 복합 화면 포함, 2 DPI × 3 viewport 실행 | 영구 문서 20개, 기본 120 비교 쌍, 최초 실패 목록 | 각 문서에 신뢰할 수 있는 기준·결과·재현 정보가 있음. 환경/실행기 오류가 해결됨 |
| 4. 기초 100개 | 80개 추가, HTML/cascade/box/text/flex/grid/table 기초 상호작용 우선 수정 | 누적 100개, 기본 600 비교 쌍, 공통 엔진 수정과 최소 재현 | 기초 기능의 남은 차이가 해결되고 기존 회귀검사가 통과함 |
| 5. 복합 300개 | 200개 추가, pairwise 생성·중첩·intrinsic sizing·소수 좌표·scroll/clip 확장 | 누적 300개, 기본 1,800 비교 쌍, coverage 및 원인별 목록 | 새 조합의 차이를 공통 수정으로 해결하고 앞 단계의 결과가 유지됨 |
| 6. 고난도 600개 | 300개 추가, writing-mode·고급 grid·조건·transform·SVG·filter 등 확장 | 누적 600개, 기본 3,600 비교 쌍, 기능별 수정 | 고난도 기능의 실패를 해결하고 실제 View·DPI 통합 차이가 해소됨 |
| 7. 전체 1,000개 | 400개 추가, 정해진 분류·복잡도 충족, 실제 Windows 두 DPI에서 전체 실행 | 누적 1,000개, 기본 6,000 비교 쌍과 추가 상태 결과 | 필수 행렬의 미실행·불안정·판정 불가·실패가 0이고 승인된 CPU/GPU 예외를 제외한 픽셀 차이가 0임 |
| 8. 영구 회귀 운영 | 빠른 대표 묶음과 전체 실행 분리, 재실행/복원/업그레이드 절차 검증 | 실행 안내, 보존 검증, 회귀 연동 | 보관한 파일만으로 같은 환경의 전체 검증을 다시 수행하고 동일 결과를 얻음 |
| 후속. JS DOM 동작 | DOM 생성·변경·이벤트·비동기·스타일 변경과 reflow/invalidation 검증 | 별도 계획과 별도 동적 corpus | 현재 정적 검증과 분리하여 추후 진행 |

단계 3의 pilot에서는 실패가 있는 그대로 최초 기준을 확정한다. 이후에는 문서 하나씩 비교하되 동일 원인의 실패를 묶어서 공통 코드를 고친다. 장기 수정이 필요한 기능은 실패 목록에 유지하고 다른 독립 기능의 분석을 진행할 수 있다. 수량이 늘어났다는 이유로 이전 단계의 완료 조건을 충족한 것으로 처리하지 않는다.

단계 3에서 캡처 시간·비교 시간·최대 메모리·PNG/JSON 크기를 측정하여 전체 실행 시간과 보관 용량을 계산한다. 단계 4 이후의 일정은 이 실측과 실패 종류를 기준으로 산정한다. 최초부터 임의의 완료 날짜나 전체 일치 보장을 제시하지 않는다.

## 10. 실패를 공통 엔진 수정으로 연결하는 절차

1. 실패 문서의 HTML/CSS, WebView2 버전, DPI, viewport, 상태, 양쪽 PNG와 차이 이미지를 고정하여 보존한다.
2. 입력/폰트/리소스/환경이 동일한지 확인하고 새 프로세스에서도 재현되는지 확인한다.
3. DOM → computed style → box/text geometry → paint/clip → DPI/View 순서로 최초 차이를 찾는다. 픽셀 차이의 경계와 관련 노드 ID를 연결한다.
4. 원본에서 작은 재현 문서를 추출하고 별도 파일로 보존한다. 관련 기능 조합과 다른 DPI에도 같은 원인이 있는지 확인한다.
5. `DOM.cpp`, 공통 `CSS.cpp` 및 관련 파서, `Layout.cpp`, `View.cpp`의 실제 공통 원인을 수정한다. 공유 JavaScript/DOM 바인딩 코드가 원인이면 그 경로도 수정하되 JS DOM 조작 검증 범위는 확대하지 않는다.
6. 수정 중에는 관련 문서부터 검사한다. 최신 사용자 지시에 따라 폰트·그라디언트의 CPU 합성과 확인된 CPU/GPU 차이의 예외 승인을 적용한다. 원본 모든 DPI/viewport와 기존 통과 확인 → 예외 등록·재판정 → 기존 회귀검사 → 나머지 GPU 경로 조사 → 보존·복원 순서로 수행한다. 기본 100% 목표와 raw 차이는 유지한다.
7. 수정 전후 차이, 영향 기능, 검증 결과, 남은 실패를 기록한다. 해결된 문서를 빠른 회귀 묶음에도 포함한다.

기준 WebView2 이미지를 TWebFrame 결과로 바꾸거나, 실패 문서의 CSS를 단순화하거나, 일반 픽셀 허용치를 높여 통과시키지 않는다. 실제 WebView2 기능 차이와 엔진 기능 차이를 분리하며, 원본 입력이 잘못된 경우에도 기준 엔진의 실제 오류 복구 결과를 먼저 확인한다.

기존 전체 검증은 `TWebFrame2/tests/Run-SupportCompatibility.ps1 -FullRegression`, 관련 개별 회귀검사, x64 Release 빌드로 확인한다. 사용자 지시에 따라 새 branch나 PR을 만들지 않는다. 본 계획 작성은 자동 커밋·푸시를 수행하지 않는다.

## CPU 폰트·그라디언트 작업 완료 (`20261004-cpu-font-final-1791061810315`)

- [x] 폰트 GPU 합성 호출 제거, CPU LCD 합성 적용. 기존 CPU 그라디언트 유지.
- [x] 원본 120쌍: 완전 일치 96, 승인 폰트 18, 승인 그라디언트 6, 실패 0. raw 차이와 strict 판정 보존.
- [x] 비교 교정 36개·문자 기하 24쌍·전체 회귀 12개와 platform integrity·schema 22개 통과.
- [x] MdViewer Release·Debug 이번 재빌드와 core/통합 검사 통과. 이후 자동 재빌드하지 않음.
- [x] 전체 회귀 완료 뒤 도형·모서리·그림자 및 최종 화면의 GPU 경로와 픽셀 회수 조사. 추가 전환은 하지 않음.
- [x] 2,680개 파일과 소스 117개 해시 복원 확인, 복원한 코드로 120쌍 동일 재판정. 이전 기준 유지.

상세 수치, 승인 정책과 후속 GPU 합성 업그레이드는 [CPU 합성 결과](RENDERING-CPU-COMPOSITION-RESULTS.md)를 따른다.

## 이전 CPU 그라디언트 작업 완료 (`20261004-050655-292-8ce5100e`)

- [x] gradient GPU shader/readback 제거와 CPU 생성·합성 적용.
- [x] 원본 120쌍 재검사: strict 114쌍 차이 0, radius/gradient 6쌍 수치·이미지·해시 보존 및 사용자 승인 완료. 이번 잔여 수정 대상 0쌍.
- [x] 최신 WebView2 두 DPI의 select 화살표 기대값과 공유 View 검사 경로 반영.
- [x] 전체 회귀 12개와 platform integrity, 문자 기하 24쌍, 고의 오류 28개 통과.
- [x] source/runtime/폰트 해시와 최초 입력 20개·기준 이미지 120개 보존 확인.
- [x] 3,349개 보관 파일 복원·120쌍 재판정·주요 12쌍 새 렌더링 재현 확인.
- [x] 추가 재현 54쌍의 23 통과/31 strict 실패를 별도 기록하고 승인 예외를 확장하지 않음.
- [x] GPU 안에서 최종 View 합성을 끝내는 구조를 후속 업그레이드로 명시.

아래 체크리스트는 1,000개 전체 계약의 완료 조건이며 이번 사용자 승인 완료와 구분한다.

## 11. 최종 완료 체크리스트

- [ ] 영구 HTML/CSS 문서 1,000개와 로컬 리소스가 존재하고 파일 해시·seed·생성기 버전이 기록되어 있다.
- [ ] 주 분류·복잡도 수량과 기능 조합 coverage가 manifest에 맞으며, 보류·미검사 범위를 공개한다.
- [ ] 실제 View 경로에서 Windows 100%·150% DPI, 페이지 확대 1.0, 세 viewport의 기본 6,000개 비교를 모두 실행했다.
- [ ] 각 문서의 추가 viewport/스크롤 상태도 빠짐없이 실행했다.
- [ ] DOM·스타일·레이아웃·문자 기하가 계약에 맞고, 승인된 CPU/GPU 예외를 제외한 다른 픽셀 수가 모든 필수 결과에서 0이다. raw 차이와 예외 원인은 보존한다.
- [ ] 잘못된 글꼴/색/위치/clip을 넣는 고의 오류 검사가 전부 실패하여 판정 도구의 검출력을 확인했다.
- [ ] 실제 DPI 전환과 반복 실행에서도 결과가 안정적이고 기존 회귀검사가 통과한다.
- [ ] 미지원·실패·미실행·불안정 결과를 성공으로 처리하지 않았으며 남은 필수 실패가 없다.
- [ ] 기준·실행 결과·최소 재현·환경 manifest를 영구 보관했고 새 위치에서 복원하여 재실행했다.
- [ ] JavaScript DOM 제어 검사는 후속 계획으로 유지하며 이번 정적 검증의 완료와 구분한다.

**앞선 table/DPI 실행은 120개 캡처·84 통과/36 실패·보존/복원 검증을 수행했다.** collapsed table 6개가 새로 통과하고 이전 통과 78개는 유지됐다. 원본 입력 20개와 기준 이미지 120개의 해시/픽셀을 유지했다. 그 실행에서 UTF-16 위치·내용·font-box rect 판정과 총 22개 교정 검사를 추가하고 DPI 폰트/inline/grid 경로를 보완했다. 당시 최소 재현 11개 × 2 DPI의 22쌍은 4 통과/18 실패였으며 회귀 실행기 11개 및 platform integrity가 통과했다. 아래 최신 실행은 이 자료를 유지하며 검증 범위를 확장했다.

앞선 실행 `20261003-174213-996-9c018c59`에서는 mixed writing의 kerning과 문자 Range 경계, normal 줄 높이·baseline, 폼 UA 크기·스타일·줄바꿈을 공통 수정했다. 글자 기하 판정은 12/24 → 24/24, 기본 DOM·활성 스타일·추적 상자·글자 기하 실패는 0이며 strict 페인트 36쌍은 남는다. 대표 native glyph/font 정보와 696개 reference 노드의 실제 font usage를 보존했고, 고의 오류 28개·회귀 실행기 12개·platform integrity·schema 22개가 통과했다. 최소 재현은 17개/추가 34쌍이며 4 통과·30 실패를 별도로 보존했다. 3,167개 파일의 아카이브 해시·복원과 전체 120쌍 재판정, 주요 문서 10쌍의 새 렌더링 재현도 검증했다. [상세 결과와 범위](RENDERING-ACCURACY-TEXT-CONTROLS-RESULTS.md)를 따른다.

최신 실행 `20261003-184127-080-f8191e3e`는 GPOS/legacy 우선순위, 한국어 monospace의 실제 굴림체 선택·bitmap 메트릭·DPI cache와 기존 glyph fallback을 수정했다. Segoe UI 및 기존 monospace/grid 재현의 활성 상자·문자 좌표가 두 DPI 모두 통과했다. 기존 monospace/Consolas 동일성 회귀검사는 사용자 요청으로 삭제했고 삭제 후 재검사도 통과했다. 기본 84/120·글자 좌표 24/24를 유지하며 36 페인트 실패는 남는다. 별도 재현 20개/40쌍은 4 통과·36 실패로 보존했다. [상세 결과](RENDERING-ACCURACY-FONT-SELECTION-RESULTS.md)를 따른다.

최신 페인트 실행 `20261003-193005-619-d5320f8d`는 네 모서리 radius와 cascade, gradient padding-box origin, shadow bitmap의 144 DPI source 단위, actual glyph face별 힌팅 모드와 폼 외형·텍스트 위치·select 화살표를 공통 수정했다. 84/120·문자 좌표 24/24를 유지했고 36 실패 중 21쌍의 차이가 감소했다. 총 차이는 412,030 → 305,246픽셀이다. 최소 재현 25개/추가 50쌍은 4 통과·46 실패로 별도 보존했다. 최종 회귀 12개·platform integrity·고의 오류 28개·schema 22개가 통과했다. 원본 입력·기준 PNG·기존 아카이브를 유지했다. [상세 결과와 남은 원인](RENDERING-ACCURACY-PAINT-RESULTS.md)을 따른다.

이전 래스터 실행 `20261003-232442-397-6a3e5f39`는 공유 WIC·DirectWrite LCD·GPU 합성 경로를 검증하고 SVG MSAA coverage·원형 stroke·반투명 native control·select 화살표·button 중앙 정렬·grid 누적 표현 오차를 공통 수정했다. 기본 **102/120**, 문자 기하 **24/24**이며 이전 통과 84개를 유지하고 Latin·혼합 문자·대시보드 18쌍이 새로 통과했다. 총 차이는 305,246 → 40,199픽셀이다. SVG 경계 10/33픽셀, checkbox 경계 1/3픽셀 및 radius/gradient/shadow의 18페인트 실패가 남는다. 최초 작업 상태의 소스·실행기·12쌍도 보존한다. 추가 재현은 27개/54쌍이며 원본과 따로 집계한다. 전체 회귀검사는 10.6에 따라 아직 실행하지 않았다. [상세 결과와 보존 검증](RENDERING-ACCURACY-RASTER-RESULTS.md)을 따른다.

이전 남았던 18쌍은 SVG·폼 12쌍의 strict 수정과 radius/gradient 6쌍의 CPU 전환·승인으로 마무리했다. 그 실행은 strict 114/120과 승인 120/120이며, 최신 CPU 폰트 전환은 완전 일치 96쌍·승인 차이 24쌍을 합쳐 120/120이다. GPU 내부 최종 합성은 후속 업그레이드로 남긴다. ui-monospace/default fallback, 전체 스타일·reference per-character glyph/baseline·다중 cluster coverage·독립 글꼴 AA 교정·실제 Windows 96 DPI/모니터 전환·독립 페인트/색 교정도 남아 있다. 계약과 pilot 차이를 보완한 뒤 100개로 확대한다.

# WebView2 / TWebFrame2 정적 렌더링 검증

원본 1,000문서는 `../rendering-stress/cases`에 유지한다. fixture 내용과 ID로 엔진을 보정하지 않는다. 페이지 JavaScript 실행은 끈다. 캡처 도구의 JavaScript는 기준 브라우저의 DOM/CSSOM을 읽는 측정용이며 DOM을 수정하지 않는다.

## 두 배율의 PNG 캡처

저장소 루트에서 PowerShell로 실행한다. VS 2019 v142 C++ 도구, 저장소의 WebView2 SDK/Runtime, Skia DLL을 사용한다.

```powershell
& tests\rendering-verification\Capture-WebView2.ps1
& tests\rendering-verification\Capture-WebView2.ps1 -Software
& tests\rendering-verification\Capture-TWebFrame2.ps1
```

각 스크립트는 기본적으로 전체 1,000문서를 **800×600 CSS viewport, 96/144 DPI**로 캡처한다. 96 DPI의 PNG는 800×600, 144 DPI의 PNG는 1200×900이다. DPI를 페이지 zoom과 중복 적용하지 않으며 PNG를 재샘플링하지 않는다. WebView2 Controller3의 raw pixel bounds/rasterization scale과 native `CaptureRenderingSnapshot`의 명시적 DPI를 사용한다. 호스트는 Per-Monitor-V2 awareness를 설정한다.

150%에서 CSS viewport를 정확히 유지하려면 폭/높이가 짝수여야 한다. 홀수 CSS 크기는 1.5배에서 반 픽셀이 되어 정수 HWND bounds와 동일한 viewport를 만들 수 없으므로 도구가 거부한다. 문서 내부의 분수 CSS 길이는 그대로 테스트한다.

```powershell
& tests\rendering-verification\Capture-WebView2.ps1 -CaseId 0001-nested-grid -Dpi 96,144 -Width 1280 -Height 800 -TimeoutSeconds 60
& tests\rendering-verification\Capture-TWebFrame2.ps1 -ParallelDpi
& tests\rendering-verification\Capture-TWebFrame2.ps1 -ParallelDpi -Workers 3
```

`-OutputDirectory`, `-BuildDirectory`, `-SkipBuild`를 지정할 수 있다. 기존 출력은 덮어쓰지 않는다. fixture HTML/CSS SHA-256이 원본 manifest와 다르면 캡처를 거부한다. 두 배율을 동시에 돌리려면 `-ParallelDpi`를 사용한다. WebView2 기본 제한 시간은 30초이며, 실패 문서는 `error.txt`로 남기고 다음 문서를 계속 처리한다. 실패 복구 시 이 도구가 생성한 해당 WebView2 프로세스만 종료한다.

native `-Workers`는 배율별 1~6개 독립 프로세스로 문서를 나눈다. 두 DPI와 workers 3은 총 6개 프로세스다. 서로 겹치지 않는 문서만 각 프로세스에 할당하며, PNG를 변경하지 않고 결과를 합친다. worker 요약·로그·실행 파일 해시는 보관한다. 6개 smoke 캡처는 단독 실행과 병렬 실행의 PNG 바이트가 모두 같았다. `Run-Verification.ps1`은 `-NativeWorkers 3`을 기본값으로 사용한다.

이 검증은 지정된 viewport의 첫 화면을 비교한다. 스크롤 아래 영역 전체와 모든 CSS 조합의 완전 검증을 의미하지 않는다. 추가 viewport는 별도 run으로 실행할 수 있다. 현재 데스크톱의 실제 HWND DPI는 96이다. 명시적 144 DPI 캡처와 실제 Windows 디스플레이를 150%로 바꾼 HWND/모니터 이동 테스트는 구분한다. `summary.json`의 `dpi`는 요청값, `actualWindowDpi`는 실제 호스트값이다. native `layout.json`에는 실제 snapshot의 viewport/DPI/계약 버전과 원본 PNG를 기록한다. 전체 DOM/텍스트 진단을 추가로 보관하려면 `Capture-TWebFrame2.ps1 -FullDiagnostics`를 사용한다. 진단 출력 범위는 PNG 렌더링에 영향을 주지 않으며 상세/간단 모드의 PNG 바이트와 공통 메타데이터가 같은 것을 확인했다. 현재 snapshot API는 기존 `[data-case-node]`의 box만 기록하므로 새 fixture의 `[data-probe]` box 목록은 비어 있다. 해당 좌표 진단에는 아래 `Inspect-Layout`을 사용한다.

## 픽셀 비교와 GPU/CPU 차이

Pillow/NumPy가 있는 Python을 사용한다.

```powershell
python tests\rendering-verification\compare.py --reference <WebView2출력> --software <software출력> --native <TWebFrame2출력> --output <새비교폴더>
```

RGBA 채널 오차 허용값은 0이다. 같은 위치의 전체 RGBA가 모두 같아야 동일 픽셀이다. 누락/실패 캡처, PNG 크기 불일치, DPI 불일치는 실패다. ID별 예외나 텍스트 주변의 일괄 AA 마스크는 사용하지 않는다.

반복 비교에서 `--no-diff-images`를 추가하면 픽셀 판정과 전체 보고서는 유지하고 별도의 차이 PNG만 생략한다. 원본 캡처 PNG는 그대로 사용한다. 100개 항목마다 비교 진행 상태를 출력한다.

GPU/CPU 제외는 같은 원본과 런타임, 같은 캡처 실행 파일, 실제 GPU rasterization/compositing 활성·비활성 진단, 동일 기준 DOM 측정이 모두 확인된 경우만 허용한다. 그 경우에도 **native RGBA가 별도 software WebView2 RGBA와 정확히 같고, GPU와 software가 실제로 다른 픽셀**만 제외한다. 단순히 가까운 색이나 1픽셀 차이라는 이유로 제외하지 않는다.

현재 전체 기준 run의 `graphics.json`은 첫 browser generation에서 측정한 backend 상태다. 25문서마다 같은 옵션으로 browser를 다시 만들지만 각 generation의 backend 상태를 별도로 실측한 것은 아니다. 결과에는 제외 전의 `exactPixelPercent`도 함께 기록한다.

## 전체 실행과 보관

```powershell
& tests\rendering-verification\Run-Verification.ps1 -PythonPath <python.exe절대경로>
# 기본 동작은 전체 2,000쌍 통과 후 산출물 정리다.
```

`verification-result.txt`에 간단한 결과를 남긴다. 실패 시 PNG/상세 로그/빌드는 재현을 위해 `.work`에 보관한다. 전체 통과 후 `Clean-VerifiedArtifacts.ps1 -ComparisonDirectory <비교폴더>`는 입력/보관 ZIP을 확인하고, 현재 도구·누적 회귀 프로젝트·공통 엔진 소스·간단한 결과를 새 ZIP으로 보관한 뒤 검증 `.work`와 이전 smoke artifact 폴더를 삭제한다. 원본 문서, generator, manifest, ZIP, 캡처/비교 도구, 누적 회귀 프로젝트는 유지한다. 다른 Python 경로를 사용하면 `-PythonPath`로 전달한다.

`preserved/inputs-tools-and-engine-before.zip`은 작업 시작 당시 1,000문서·도구·엔진 소스의 보관본이다. `catalog.json`에 파일별/ZIP SHA-256을 남겼다. 보관본을 덮어쓰지 않는다.

`preserve_tools.py`는 원본 ZIP의 모든 항목과 현재 원본 문서를 확인하고, 추가된 도구/회귀 프로젝트/공통 엔진 변경을 새 ZIP으로 누적 보관한다. `common-engine-changes.patch`는 기존 사용자 변경을 기준으로 이번 작업이 추가한 변경만 담는다.

## 누적 회귀 프로젝트

- `FontLogicalRegression.vcxproj`: 부모/루트 상대 글꼴, line-height 상속, 논리 속성의 방향·우선순위·선언 순서·revert-layer.
- `ViewportDpiRegression.vcxproj`: 일반/강제/숨김 스크롤바, stable/both-edges, viewport와 percentage/vw의 구분, DPI 물리 픽셀 반올림, resize.
- `AutoOverflowRegression.vcxproj`: 실제 배치 후 자동 gutter 판단, 인접 margin collapse, 블록/그리드/flex/RTL, 반복 relayout에서 scroll 좌표 안정성. WebView2에서 독립 측정한 96/144 DPI 좌표를 사용한다. 수정 전 150개 검사 중 8개 실패, 수정 후 150개 모두 통과했다.
- `TableFormattingRegression.vcxproj`: DOM을 바꾸지 않는 익명 테이블 행/셀/부모 생성, 첫 header/footer 그룹의 시각 순서, RTL 열·셀 병합, inline-table, collapsed border와 절대 위치 크기, HTML UA 스타일과 CSS 상속. 독립 WebView2 좌표 47개를 두 DPI에서 초기 배치/반복 relayout으로 확인한다. 392개 검사와 작은 회귀 문서의 96/144 DPI PNG 전체 RGBA 비교가 통과했다. 기대 TSV는 회귀 입력으로 보관하며 원본 1,000문서에 대한 보정에 사용하지 않는다.
- `OverflowPaddingRegression.vcxproj`: visible 자손의 끝 padding 중복 방지, 가로/세로 scrollbar 상호 유발, 자동 높이 요소와 뒤 요소의 위치, CSSOM scroll/client 크기, 두 축 scroll offset의 반복 relayout 안정성. 두 DPI에서 WebView2로 독립 측정한 13개 probe를 사용한다. 수정 전 168개 중 40개 실패, 수정 후 모두 통과했다.
- `TextSpacingRegression.vcxproj`: word-spacing 상속과 양수/음수 간격, font-relative 자간의 computed length 상속, NBSP와 고정 폭 공백의 구분, 공백 축약/보존, tab-size, RTL, 간격에 따른 2줄/3줄 배치. 두 DPI의 독립 WebView2 좌표/스타일 20개 probe로 244개 검사와 PNG 전체 RGBA 비교가 통과했다. 수정 전 244개 중 80개가 실패했다.
- `AutoTableRegression.vcxproj`: 자동 표의 내용/padding 최소 폭, 지정 열 폭의 비례 축소, 백분율, 병합 셀, colgroup, max-width와 min-width:0, 별도/병합 테두리, RTL, CSS 테이블, overflow-wrap/word-break 상속과 word-wrap 별칭, 여러 줄 inline의 DOM font box 좌표, 셀 내부의 고정 폭과 max-width/box-sizing 기여, 고정 grid 열의 최소 폭, 중첩 표의 내용 최소 폭, 백분율 열에 따른 자동 표의 preferred width. 두 DPI에서 WebView2로 독립 측정한 74개 probe를 초기/반복 배치로 600개 검사를 실행한다. 수정 전 216개가 실패했으며, 중간 수정본에서도 고정 폭 자손 기여 24개와 grid/중첩 표/백분율 intrinsic sizing 28개 실패가 재현됐다. 현재 좌표 검사 600개는 통과하나 96 DPI의 분수 x좌표 글자 한 픽셀 때문에 해당 자동 표 fixture의 PNG는 1/2쌍이다. 이 픽셀은 GPU/CPU 제외 조건에도 맞지 않아 실패로 유지한다.
- `TextWrappingRegression.vcxproj`: normal 모드의 긴 단어 overflow, anywhere/break-word emergency wrapping, break-all, nowrap/pre/pre-wrap의 우선순위, 상속, 중첩 inline, 결합 문자 cluster, 자간, 서로 다른 줄바꿈 정책의 높이 캐시. 두 DPI의 독립 WebView2 probe 50개를 초기/반복 배치로 검사한다. 수정 전 60개가 실패했고 현재 408개 좌표 검사는 통과했다. PNG에는 96 DPI 299픽셀/144 DPI 369픽셀의 미설명 차이가 있어 별도로 실패를 유지한다.
- `TableHeightRegression.vcxproj`: 표의 내용 최소 높이, 지정 높이/max-height hint, 셀/행 min/max-height, 캡션과 표 grid의 높이 분리, CSS 셀의 기본 baseline 및 명시적 middle/bottom 정렬. 두 DPI의 독립 WebView2 probe 74개를 초기/반복 배치로 600개 검사한다. 수정 전 112개가 실패했다. 새 표 PNG는 두 DPI에서 전체 RGBA가 동일했다.
- `IntrinsicWidthRegression.vcxproj`: min-content/max-content/fit-content와 내용 기준 min/max-width, box-sizing/padding/border, 백분율/RTL 여백, flex/grid의 실제 폭에 따른 높이, 중첩된 intrinsic 기여, inline의 폭 속성 적용 여부, 절대 위치/float를 검사한다. 두 독립 WebView2 회귀 문서의 129개 probe를 두 DPI의 초기/반복 배치로 1,048개 검사한다. 첫 문서 784개 검사 중 수정 전 220개가 실패했으며 현재 두 문서 모두 좌표 검사를 통과한다. PNG는 첫 문서 96 DPI 437/144 DPI 532픽셀, 두 번째 문서 152/176픽셀 차이가 남아 실패를 유지한다. 테두리 shorthand에 색을 생략했을 때 currentcolor를 쓰도록 공통 CSS를 수정하고 FontLogicalRegression에 16개 검사를 누적했다.
- `TextMinimumRegression.vcxproj`: CJK/한글, 하이픈·solidus·제로 폭 공백·NBSP, 소프트 하이픈·hyphens 상속, keep-all/nowrap/pre-line, 결합 문자, 자간·어간, 지원/대체 글꼴, RTL, 좁은 고정 폭과 앞선 단어의 재배치를 검사한다. 독립 WebView2 회귀 문서 세 개의 48개 사례/192개 probe를 두 DPI에서 초기/반복 배치로 1,560개 검사한다. 내용 폭 수정 이전 엔진에서는 244개가 실패했고 현재 좌표 검사는 모두 통과한다. PNG는 text-minimum에서 96/144 DPI 2,487/4,046픽셀, text-break-controls에서 1,265/2,081픽셀, soft-hyphen-paint에서 262/433픽셀 차이가 남아 실패를 유지한다. 마지막 문서의 표시 수정 전 차이는 1,259/2,391픽셀이다.
- `FlexSizingRegression.vcxproj`: flex base와 min/max 제약을 분리하고 각 줄의 grow/shrink 방향, 부분 비율(합계가 1 미만), 위반 방향별 freeze·재분배, percentage min/max, content-box/border-box·padding·margin·gap, RTL/역순, wrap, column 높이를 검사한다. 독립 WebView2 19사례/64probe를 두 DPI의 초기/반복 배치와 전체 ID 변경으로 1,040개 검사한다. 기본 520개 검사는 수정 전 136개 실패했고, 수정 후 1,040개 좌표 검사와 두 DPI PNG 전체 RGBA 모두 통과했다. 기대 좌표는 회귀 입력으로만 쓰며 공통 엔진은 읽지 않는다.
- `DirectionAttributeRegression.vcxproj`: HTML dir의 유효/무효/대소문자 값, 상속, author CSS 우선순위와 revert/initial/inherit/unset, 논리 padding, UA unicode-bidi, RTL grid/flex/inline-block을 검사한다. 독립 WebView2 19사례/76probe의 좌표·스타일을 두 DPI의 초기/반복 배치와 전체 ID/CSS selector 변경으로 3,672개 검사하며 모두 통과했다. 두 DPI의 PNG 전체 RGBA도 동일했다. dir=auto의 첫 strong 문자 탐색과 텍스트가 섞인 bidi 재배치는 이 회귀의 검증 범위에 포함되지 않는다.
- `ItemOrderRegression.vcxproj`: DOM 순서를 유지한 flex/grid의 order 정렬, 동일 order의 안정된 순서, row/column/RTL/역순/wrap, 자동/명시 grid 배치, 겹친 항목의 paint 순서, 일반 block에 대한 order 무효, start/end/left/right/self-start/self-end 정렬과 auto margin 우선순위를 검사한다. 독립 WebView2 24사례/120probe를 두 DPI의 초기/반복 배치와 전체 ID 변경으로 1,944개 검사하며 모두 통과했다. 두 DPI PNG 전체 RGBA도 동일했다. 초기 16사례/80probe의 1,304검사 중 448실패를 재현한 입력·기대 데이터는 initial-80.zip으로 보관한다.
- `ClipPathRegression.vcxproj`: 기존 48개 inset/circle/ellipse/polygon·reference box·중첩·stacking·opacity·scale 사례와 추가된 8개 둥근 모서리·visible 자손·분수 좌표·calc() 경계 사례에서 독립 WebView2 좌표와 클릭 대상을 비교한다. 남아 있던 클릭 검사 28개는 둥근 border box의 자체 클릭 영역과 로컬 장치 좌표의 Skia 경로 판정으로 수정했다. 기존 기대값을 유지하면서 두 DPI, 초기/반복 배치, 전체 ID 변경의 20,552개 검사가 모두 통과한다. PNG 비교의 기존 실패 기록은 유지한다.
- `OpacityGroupRegression.vcxproj`: 흰색/어두운 배경의 20가지 투명도와 z-index 자식을 둔 40개 그룹을 검사한다. 80개 독립 좌표/계산된 opacity의 1,944개 검사는 모두 통과한다. CSS opacity는 원래 부동소수 값으로 유지하며, 합성 표면의 group alpha만 8bit에 맞춰 반올림한다. 두 DPI의 PNG는 38/40개 그룹에서 동일하지만 opacity=.1인 두 그룹의 RGB 차이로 전체 문서는 실패한다.
- `test_compare.py`: 작은 RGBA 차이도 실패, 크기/alpha 차이, 근거 없는 backend 제외 금지.
- `GridAutoFlowRegression.vcxproj`: row/column 자동 배치, sparse/dense 빈칸 채우기, span, 자동·지정 위치의 혼합 순서, 고정 행/열, order/RTL을 검사한다. 독립 WebView2 16개 grid/96개 probe를 두 DPI의 초기·반복 배치와 전체 ID 변경으로 1,560개 검사한다. 수정 전 280개 실패, 수정 후 모두 통과했고 두 DPI PNG의 전체 RGBA가 동일했다.
- `MixedInlineFlowRegression.vcxproj`: 배지와 텍스트의 첫 줄 잔여 폭, 혼합 inline의 줄바꿈·공백 제거·원자적 요소·중첩 태그·글꼴·단어 간격을 검사한다. 독립 WebView2 12사례/60probe를 두 DPI의 초기·반복 배치와 전체 ID 변경으로 확인한다. 원본 DOM의 Range·커서·클릭 문자 위치까지 포함한 1,908개 검사가 모두 통과했다. 일반 텍스트의 일부 글리프 픽셀 차이는 실패로 유지한다.
- `MulticolBlockFlowRegression.vcxproj`: 열 개수/폭/columns shorthand, 기본 간격, RTL, auto/balance 채움, 강제 열 이동, column-span:all, 인접 블록 여백을 검사한다. 독립 WebView2 12사례/109probe의 1,776개 검사와 두 DPI PNG 전체 RGBA가 모두 통과했다. 현재 구현은 분할을 피하는 블록과 원자적 요소에 한정된다. 일반 문단의 줄 단위 열 분할·widows/orphans·column-rule·세로쓰기는 추가 구현과 검증이 필요하다.

`Run-Regressions.ps1`로 열아홉 C++ 회귀 프로젝트(39,804개 검사)와 Python 픽셀 비교 회귀(9개)를 함께 실행할 수 있다. 일부 엄격 PNG 비교에는 실패가 남는다. 최신 실행 결과와 원본 1,000문서 전체 결과는 `verification-result.txt`에 기록한다. 회귀 실행기는 한 프로젝트가 실패해도 남은 C++/Python/PNG 검사를 계속한 뒤 전체 실패를 반환한다. `Run-Verification.ps1`도 회귀 실패를 전체 실패로 유지하면서 원본 1,000문서 캡처/비교를 계속한다. 회귀 또는 전체 비교에 실패가 남으면 자동 정리를 수행하지 않는다.

이 스크립트는 `Run-ExactPngRegressions.ps1`도 실행한다. 테이블 구조·텍스트 간격·자동 표·줄바꿈·표 높이·내용 폭/기여·Unicode 최소 폭/줄바꿈 제어·소프트 하이픈·flex min/max 재분배·HTML 방향 속성·order/grid 정렬·클리핑·투명도 그룹·grid 자동 배치·혼합 inline·다단 배치의 작은 회귀 문서를 두 엔진과 두 DPI에서 새로 캡처한 38쌍은 전체 RGBA가 같아야 통과한다. backend 제외나 리사이즈는 적용하지 않는다. 입력/실행 파일 해시와 결과는 `.work`에 기록하며 전체 1,000문서 검증과 구분한다. PNG 회귀만 다시 실행하려면 `Run-ExactPngRegressions.ps1 -BuildDirectory <빌드폴더>`를 사용한다.

테이블 구조·행 그룹·RTL 배치 규칙은 [CSS 2.1 Tables](https://www.w3.org/TR/CSS2/tables.html#anonymous-boxes)를 따르며, 픽셀 일치 여부는 현재 WebView2에서 별도로 검증한다.

텍스트 간격과 상속 규칙은 [CSS Text 3 Spacing](https://www.w3.org/TR/css-text-3/#spacing)을 참조하며, 공백·tab·줄바꿈의 실제 좌표와 픽셀은 현재 WebView2에서 별도로 검증한다.

줄바꿈 정책의 공통 측정·배치 경로는 [DirectWrite word wrapping](https://learn.microsoft.com/windows/win32/api/dwrite/ne-dwrite-dwrite_word_wrapping) 설정을 공유한다. normal 모드는 whole-word, break-all은 character cluster, overflow-wrap은 emergency break를 사용한다. 표의 높이·캡션·수직 정렬은 CSS Tables 3과 현재 WebView2의 독립 좌표·PNG를 함께 확인했다.

자동 열 폭 배분은 [CSS Tables 3](https://www.w3.org/TR/css-tables-3/#width-distribution)를 참고하며, draft 알고리즘과 현재 브라우저의 동작이 다를 수 있으므로 좌표와 PNG는 현재 WebView2에서 독립 검증한다.

`Inspect-Layout.vcxproj`는 원본 HTML/CSS를 공통 엔진으로 읽는 진단용 프로젝트다. `Inspect-Layout.exe <index.html> <output.json> [dpi] [selector]`로 요소의 rect/content/scroll/style을 기록한다. selector는 출력 대상만 지정하며 DOM·스타일·레이아웃을 변경하지 않는다. 이 도구는 800×600 CSS viewport를 사용하며, 외부 이미지 로딩과 페이지 JavaScript 실행을 수행하지 않는다. 이미지가 배치에 영향을 주는 경우 PNG 캡처 호스트의 결과를 기준으로 삼는다.

프로젝트/소스는 누적 보관한다. 실행 파일, object/lib/PDB, 복사한 DLL은 `.work`에만 생성하고 전체 검증 통과 후 정리한다.

내용 폭 키워드 계산은 [CSS Sizing Level 3](https://www.w3.org/TR/css-sizing-3/#sizing-values)를, 색을 생략한 border shorthand의 currentcolor 기본값은 [CSS Backgrounds and Borders Level 3](https://www.w3.org/TR/css-backgrounds-3/#border-color)를 참조한다. 실제 일치 여부는 독립 WebView2 좌표와 수정하지 않은 PNG로 확인한다.

내용 폭 회귀의 GPU/software 기준 DOM 좌표는 동일하다. GPU와 달랐던 native 픽셀 중 별도 software와도 다른 픽셀은 intrinsic-width에서 96/144 DPI 323/417개, intrinsic-contributions에서 112/136개다. 해당 차이는 backend 차이로 일괄 제외하지 않는다. 복잡 문서에는 세로쓰기의 intrinsic 폭/높이 등 추가 공통 엔진 차이가 남는다.

Unicode 최소 폭은 공백 토큰에만 의존하지 않고 [DirectWrite cluster metrics](https://learn.microsoft.com/windows/win32/api/dwrite/ns-dwrite-dwrite_cluster_metrics)의 줄바꿈 기회를 사용한다. NBSP의 끊김 방지와 표시되는 소프트 하이픈의 폭을 유지한다. hyphens의 initial/inherit/unset 계산과 캐시 키도 공통 CSS/텍스트 경로에서 처리한다.

현재 글꼴에 U+2011이 없고 U+2010이 있을 때 글리프를 재사용하는 규칙은 [HarfBuzz shaping normalization](https://github.com/harfbuzz/harfbuzz/blob/main/src/hb-ot-shape-normalize.cc)을 참고한다. 원문/DOM offset은 바꾸지 않고 inline glyph의 동일한 advance를 측정과 표시에서 사용한다. Arial의 누락된 하이픈은 이번 Windows WebView2의 실제 CDP 사용 글꼴이 Noto Sans KR인 것을 확인했고, 설치/문자 지원 여부를 확인한 공통 fallback을 적용한다. 입력 ID나 문서별 값은 사용하지 않는다.

`Inspect-TextLayout.vcxproj`는 공통 엔진을 사용하지 않는 DirectWrite 진단 프로젝트다. `Inspect-TextLayout.exe <UTF-8 text file> <output.json> <width> [family]`로 20px 글꼴의 줄 길이/높이와 실제 glyph run을 기록한다. 순수 DirectWrite가 소프트 하이픈으로 줄은 나누지만 해당 glyph run은 비어 있는 것을 재현했다. `inspect_font_metrics.py`는 설치된 일곱 Windows TTF의 BMP cmap/advance를 읽는 진단용 도구다. 캡처의 `--full-diagnostics true`는 WebView2 DOM snapshot과 [data-case-node]/[data-probe]의 실제 CDP platform fonts도 기록한다.

새 텍스트 회귀의 별도 software 캡처는 GPU 기준과 DOM 측정이 동일하다. GPU와 달랐던 native 픽셀 중 별도 software와도 다른 픽셀은 text-minimum에서 96/144 DPI 2,411/3,965개, text-break-controls에서 1,232/2,029개, soft-hyphen-paint에서 215/356개다. 해당 차이는 backend 차이로 제외하지 않는다.

`analyze_comparison_changes.py <before directory> <after directory> <output.json>`는 manifest/viewport/DPI/허용 오차가 같은 전체 비교의 개선·동일·악화와 픽셀 변화량을 기록한다. 현재 전체 결과는 verification-result.txt에 별도로 남기며, 독립 회귀의 개선을 원본 1,000문서의 전체 통과로 간주하지 않는다.

flex 공간 재분배는 [CSS Flexbox 9.7](https://www.w3.org/TR/css-flexbox-1/#resolve-flexible-lengths)의 base/hypothetical size와 min/max freeze 규칙을 따르며, 실제 두 DPI 좌표와 PNG는 독립 WebView2 캡처로 확인한다. 이번 변경만의 `flex-sizing-change.patch`와 누적 전체 변경 `common-engine-changes.patch`를 함께 보관한다.

HTML 방향 속성과 UA bidi 스타일은 [HTML Bidirectional text](https://html.spec.whatwg.org/multipage/rendering.html#bidi-rendering)를 참고하고 author cascade 이전에 처리한다. 원자적인 inline box로만 구성된 RTL 줄은 공통 block layout에서 역순 배치한다. `flex-and-direction-changes.patch`에는 이번 flex/방향 변경을, `common-engine-changes.patch`에는 앞선 공통 엔진 변경까지 누적 보관한다.

CSS order는 [Flexbox ordering](https://www.w3.org/TR/css-flexbox-1/#order-property)과 [Grid ordering](https://www.w3.org/TR/css-grid-1/#order-property)에 따라 레이아웃 상자만 정렬한다. DOM과 selector 순서는 유지한다. 별도 `item-order-change.patch`와 누적 전체 공통 엔진 patch를 보관한다.

클리핑은 [CSS Masking](https://www.w3.org/TR/css-masking-1/#the-clip-path)과 [CSS Shapes](https://www.w3.org/TR/css-shapes-1/#basic-shape-functions)의 reference box·도형·stacking 규칙을 참고한다. 공통 Layout.cpp에서 clip mask/opacity/transform을 deferred z-index 자식까지 그룹에 적용하고, DOM bounding rect에는 paint transform을 반영한다. absolute 지정 크기는 margin을 포함한 외부 크기로 전달해 margin 때문에 border box가 작아지지 않게 한다. 이번 변경은 `clip-path-change.patch`, 투명도 반올림은 `opacity-group-change.patch`, 누적 전체는 `common-engine-changes.patch`로 보관한다.

이번 우선 수정은 공통 혼합 inline 흐름, 명시 폭 grid 항목의 높이 기여, 분할 회피 블록의 다단 배치를 다룬다. 문자 분할 후 DOM Range·커서·클릭은 원본 문자열의 공백·offset에 대응한다. cluster 줄바꿈이 필요한 긴 단어나 inline 여백/수직 정렬이 섞인 그룹은 기존 formatting context를 유지한다. 해당 그룹의 viewport 가로 넘침은 독립 WebView2로 확인했지만 첫 줄과 후속 줄의 서로 다른 폭을 사용하는 전체 cluster 분할은 추가 검증이 필요하다. 이번 수정만의 patch는 `priority-layout-changes.patch`, 전체 누적 변경은 `common-engine-changes.patch`에 보관한다.

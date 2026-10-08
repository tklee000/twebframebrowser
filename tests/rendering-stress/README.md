# JavaScript 없는 복잡한 HTML/CSS 렌더링 문서 1,000개

`index.html`을 열면 20개 주제별로 50개씩 문서를 선택할 수 있다. 테스트 입력은
`cases/<id>/index.html`, `style.css`, `case.json`이다. 목록 페이지는 1,000개에
포함하지 않는다. 모든 테스트 문서는 standards mode이고 UTF-8이다.

문서당 HTML 요소 **1,023~2,794개**, 중첩 깊이 **12~16단계**, 추적 요소
**198~333개**, 주요 모듈 **14~16개**를 포함한다. 전체 요소는 **2,023,565개**다.
단순 색상 변경만 하는 문서가 아니라 모듈 순서, 중첩 구조, 요소 수, 텍스트,
트랙 배치, 치수, 폼 상태와 아래 조건을 함께 바꾼다. 단일 PNG는 문서 내부
data URL로 포함하므로 네트워크와 별도 이미지 파일이 필요하지 않다.

## 조합 범위

CSS 값, DOM 구조와 중첩 깊이는 무한하므로 모든 HTML/CSS 조합을 1,000개로
전수 검사할 수는 없다. 이 corpus는 `coverage.json`에 명시한 **21개 유한 조건**의
모든 2개 조건 조합 **5,957/5,957개**와 아래 5개 묶음의 모든 3개 조건 조합
**472/472개**를 포함한다. 나머지 고차 조합은 고정 seed로 추가한다.

- outerLayout × nestedLayout × writingMode
- overflow × position × transform
- paint × opacity × clip
- sizing × spacing × boxModel
- direction × content × typography

21개 조건: 주제, 바깥 레이아웃, 내부 레이아웃, 방향, writing-mode, 크기 계산,
간격, overflow, position, 배경, 테두리, 모서리, opacity, transform, clip-path,
폰트 종류, 텍스트 종류, 정렬, box-sizing, 밀도, 고급 기능.

여기서 coverage는 **문서 단위의 입력 조건 조합**이다. 모든 속성이 같은 요소에서
서로 작용한다는 뜻이나, CSS 계산 결과와 픽셀이 올바르다는 뜻은 아니다.
고급 기능 분기에서 다른 조건을 덮어쓰기도 하므로 실제 계산값은 WebView2의
측정 결과로 확인한다. `@supports`의 fallback을 사용한 문서가 있다는 이유로
지원되지 않는 기능을 통과 처리하면 안 된다.

| 주제 | 주요 검증 대상 |
|---|---|
| nested-grid | 명명 트랙/영역, implicit tracks, dense, rowspan/colspan, 중첩 grid |
| flex-wrap | grow/shrink/basis, order, auto margin, baseline, reverse/wrap-reverse |
| intrinsic-sizing | min/max/fit-content, 깊은 중첩, 비율, percent/auto 치수 |
| table-spans | 실제 표 rowspan/colspan, colgroup, collapse/separate, fixed/auto, sticky header |
| inline-typography | 혼합 인라인, ruby, sub/sup, ligature, kerning, NBSP, soft hyphen, pre |
| bidi-writing | RTL/LTR, bdi/bdo, 세로쓰기, 논리 치수, text-orientation/combine |
| positioned-stacks | containing block, absolute/relative/sticky, z-index, 음수 층, opacity |
| overflow-scroll | visible/hidden/clip/auto/scroll, 가로·세로 넘침, scrollbar gutter |
| clipping-masks | inset/circle/ellipse/polygon, gradient mask, 둥근 클립, drop shadow |
| backgrounds-shadows | 다중 배경, linear/radial/conic, blend, inset/다중 그림자, filter |
| transforms-3d | 소수 이동, 비균일 scale, rotate/skew/matrix, perspective, backface |
| native-forms | 정적 input/select/textarea/button/progress/meter, checked/disabled/invalid |
| multicolumn-floats | 다단, column-span/rule, float/shape-outside, margin collapse |
| responsive-units | px/%/em/rem/ch/ex/vw/vh/vmin/vmax/lh/vi/dvh, calc/clamp |
| cascade-selectors | layer/revert-layer, custom property, important, :has/:is/:where/:not, nth, pseudo |
| replaced-content | PNG 고유 치수/알파, object-fit/position, SVG gradient/path/text, MathML |
| semantic-lists | 중첩 목록/marker/counter, dl, details 초기 open/closed, 정적 nonmodal dialog |
| containment-queries | inline-size container, container query/unit, size/layout/paint containment |
| subgrid-tracks | subgrid와 fallback, 부모 트랙 정렬, 논리 margin/padding |
| mixed-dashboard | grid/flex/sidebar/navigation/metric을 함께 쓰는 통합 화면 |

`family`가 지정한 주제를 첫 모듈에 놓고 나머지 13~15개 주제를 추가한다.
전체 문서에는 조건 조합을 위한 별도 specimen 영역도 있다. 페이지 전체가 길고
일부 내용은 스크롤·클립 뒤에 있으므로 첫 화면만으로 전체 페인트를 판정하지 않는다.

## 실행 중 JavaScript 제외

문서에는 script, 이벤트 핸들러, javascript URL, iframe/srcdoc, canvas 동작이
없다. DOM 생성·수정·이벤트·동적 상태 전환은 이번 범위에 포함하지 않는다.
애니메이션/전환도 넣지 않아 시간에 따른 픽셀 변화가 없다. 폼과 details는 HTML에
작성한 초기 상태만 검사한다. 페이지 JavaScript를 끈 상태로 로드한다.

포함한 smoke host는 **페이지 스크립트가 꺼진 상태**에서 호스트의 읽기 전용
계측만 실행한다. DOM/스타일/속성/스크롤 위치를 변경하지 않으며 창 크기만
호스트에서 바꾼다. 측정용 JavaScript 실행은 DOM 제어 테스트가 아니다.

## 생성과 정적 검사

Python 표준 라이브러리만 사용한다. 현재 PC의 사용 가능한 Python 경로 예시:

```powershell
$pythonExe = 'C:\Users\tklee\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe'
& $pythonExe .\tests\rendering-stress\generate.py --verify

# 새 출력 폴더에 동일 입력 재생성
& $pythonExe .\tests\rendering-stress\generate.py --output D:\rendering-corpus-copy

# 새 seed도 반드시 새 폴더로 생성
& $pythonExe .\tests\rendering-stress\generate.py --seed 20261008 --output D:\rendering-corpus-new
```

기본 seed는 **20261007**, 생성기 버전은 **1.0.0**이다. 문서별 seed와 SHA-256을
`case.json`/`manifest.json`에 기록한다. 기본 1,000개를 생성한 뒤 같은 명령으로
다시 생성하여 전체 파일의 바이트 일치를 확인했다. 기존 입력과 다른 내용은
덮어쓰지 않고 오류로 종료한다. 변경하려면 새 출력 폴더를 사용한다.

`--verify`는 전체 1,000개의 해시, ID 중복, 태그 균형, 금지 요소/속성,
리소스 참조, 최소 복잡도, 추적 ID와 조합 coverage를 독립적으로 다시 계산한다.
결과는 `validation.json`이다. 이것은 브라우저 레이아웃·픽셀 비교 결과가 아니다.

## 실제 WebView2 입력 검사

저장소의 기존 WebView2 SDK와 .NET Framework C# compiler로 별도 숨김 호스트를
컴파일한다. 엔진이나 기존 테스트 폴더를 수정하지 않는다. 가상 HTTPS 호스트를
로컬 corpus 폴더에 매핑하며 외부 서버 없이 원본 파일을 로드한다.

```powershell
# 첫 20개 + 마지막 20개, 각각 세 viewport, PNG 120장
.\tests\rendering-stress\Test-WebView2.ps1

# 전체 1,000개 800x600 검사 + 위 40개 추가 viewport, 총 1,080회 계측
.\tests\rendering-stress\Test-WebView2.ps1 -All

# 지정한 문서만 별도 실행
.\tests\rendering-stress\Test-WebView2.ps1 -CaseId '0001-nested-grid','0002-flex-wrap'
```

각 실행은 새 `artifacts/webview2-<timestamp>/`에 원본 입력 해시, runtime 버전,
실제 window DPI, DPR, HTML5 DOM 수, 실제 추적 ID, CSSOM rule/declaration 수,
이미지 decode, 유한 좌표, 계산 스타일/사각형/scroll extent와 표본 PNG를 저장한다.
입력/계측 실패 5개 또는 첫 renderer timeout에서 중단하고 `errors.json`에 남긴다.
timeout 뒤에는 같은 renderer의 다음 문서도 막힐 수 있으므로 후속 입력의 독립 실패로
해석하지 않는다. 계획한 수량, 시도한 수량과 미실행 수량을 요약에 구분한다.
요약은 `summary.json`이다. WebView2 로딩/계측 성공은 자체 엔진과의 정확성 비교
통과를 의미하지 않는다. 검사 증거는 Git에서 제외한다.

현재 검증 기록은 `browser-validation.json`에 있다. 설치된 WebView2
**154.0.4258.62**에서 첫 20개 주제 문서 × 세 viewport = **60회 계측·PNG**는
실패 없이 완료했다. 전체 실행에서는 최초 50개 800×600 계측 후
`0051-transforms-3d`에서 20초 navigation timeout이 발생하여 전체 검사를
완료하지 못했다. 추가 표본에서도 `0993-multicolumn-floats`에서 같은 timeout을
관찰했다. 두 입력을 각각 새 profile/새 호스트로 실행해 timeout을 재현했다.

두 문서는 outerLayout=columns, position=fixed를 공통으로 사용한다. 이것은
관찰된 공통 입력이며 **원인으로 확정하지 않았다**. 입력을 삭제·축소하거나
성공 처리하지 않았다. 해당 runtime의 레이아웃/자원 비용 문제인지 별도 분석이
필요하다. timeout 이후 연속 navigation 실패는 독립 입력 실패로 집계하지 않는다.
전체 1,000개 브라우저 로딩·모든 viewport·DPI 검사는 아직 완료되지 않았다.

## 레이아웃·페인트 비교에 연결

`manifest.json`의 `cases[].path`는 corpus 루트 기준 상대 경로다. `case.json`은
기존 case schema의 공통 필드(id/family/features/seed/scriptless/states/viewports/dpi/
HTML·CSS hash)를 포함한다. 추가 조건과 probe 정보도 있다. 현재 삭제되어 있는
기존 비교 실행기와의 실행 통합은 하지 않았다.

- 기본 행렬: 384×768, 800×600, 1280×800 CSS viewport × 96/144 DPI = **6,000쌍**.
- 모든 `[data-probe]`의 border rect/계산 스타일, 줄 fragment, scroll extent 및 DOM
  트리/텍스트/속성을 동일 입력의 WebView2 결과와 비교한다.
- 장치 픽셀로 저장한 reference/native/diff 원본을 보존한다. first viewport 외에
  문서 전체·중간·끝과 내부 scroller의 가려진 내용도 후속 검사한다.
- capture scale과 실제 Windows DPI를 구분하고, runtime/OS/폰트/GPU/locale/zoom을
  함께 고정한다. 시스템 폰트는 재배포하지 않는다.
- `capture-matrix.json`은 미디어 경계값, 120/192 DPI, print 등 선택적 추가 조건을
  제시한다. 이 추가 행렬과 스크롤 상태의 실행은 아직 미수행이다.

전체 TWebFrame–WebView2 레이아웃·페인트 비교 상태는 **미실행**이다. 실패를
줄이기 위해 문서를 단순화하거나 기존 픽셀 허용치를 바꾸지 않는다.

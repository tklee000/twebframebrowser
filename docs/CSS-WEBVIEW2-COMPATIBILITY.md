# CSS 엔진과 WebView2 비교

작업일: 2026-10-03. 비교 대상은 이 PC에 설치된 WebView2 Evergreen 런타임이다.

**TWebFrame 자체 엔진의 WebView2 수준 전체 CSS 지원은 아직 완료되지 않았다.**
아래 구현과 검사는 그 목표에 필요한 공통 파서·선택자·캐스케이드 개선이다.
검사 사례의 통과율을 전체 CSS 지원율로 해석하지 않는다.

최종 검증: x64 Release 빌드 성공, WebView2 **154.0.4258.53**과 공통 사례
**70개 전부 일치**, 파싱·네이티브 100%/150% DPI·오류 처리·API·WebView2
비교를 포함한 **361개 검사에서 실패 0개**. `-FullRegression`의 기존 회귀검사
묶음도 성공했고, 최종 빌드에서 TWebFrameTests를 다시 실행해 통과했다.

## 이번 구현

| 영역 | 추가·수정한 동작 |
| --- | --- |
| 공통 구문 | 문자열·이스케이프·주석·균형 잡힌 함수/블록을 구분한다. 스타일시트와 HTML `style` 속성이 같은 선언 파서를 사용한다. EOF에서 규칙을 닫고 알 수 없는 at-rule을 건너뛴다. |
| 선언 | 이스케이프된 속성 이름, 문자열 안의 세미콜론, 공백·주석·대문자가 포함된 `!important`, 중복 인라인 선언의 우선순위를 처리한다. |
| 선택자 | 이스케이프된 ID·클래스·속성, `:is()`, `:where()`, 선택자 목록과 복합 선택자를 포함하는 `:not()`, 자식·인접 형제에 고정한 `:has()`, `:nth-last-child()`, `:nth-last-of-type()`, `:nth-child(... of ...)`, `:lang()`을 추가한다. 조상·형제 조합을 역추적한다. |
| 명시도 | ID·클래스·태그를 독립된 자리로 비교한다. `:where()`는 0, `:is()`·`:not()`·`:has()`는 인수의 최대 명시도, 의사 요소는 태그 명시도를 사용한다. 인라인 선언의 우선순위를 별도로 유지한다. |
| 중첩 규칙 | 명시적 `&`, 생략된 부모 선택자, 부모 선택자 목록의 명시도, 중첩 앞뒤 선언의 소스 순서, 스타일 규칙 안의 `@media`를 처리한다. |
| 레이어 | `@layer` 순서 선언, 이름 있는 레이어의 재개, 익명·중첩 레이어, 일반/important 우선순위 반전, 일반 속성·사용자 정의 속성의 `revert-layer`를 처리한다. |
| 사용자 정의 속성 | 중첩 `var()`와 대체값, 선택된 경로의 순환 참조, 빈 대체값, 정의한 요소에서의 값 확정과 상속, 변수로 지정한 단축 속성을 처리한다. 잘못된 계산값은 `unset`으로 처리한다. |
| 전역 키워드 | 등록된 기본값과 상속 경로를 이용해 `initial`, `inherit`, `unset`, `revert`, `revert-layer`, `all`과 주요 단축 속성의 초기화를 처리한다. |
| 조건 | 지원하는 속성값과 선택자를 대상으로 `@supports`, `selector()`, `not`·`and`·`or`, JavaScript `CSS.supports()`의 두 호출 형식을 처리한다. 미디어 쿼리에 비교 연산·양방향 범위·조건 그룹을 추가한다. |
| 수학 | 차원 검사, 괄호와 연산 우선순위, `calc()`의 곱셈·나눗셈, 중첩 `min()`·`max()`·`clamp()`, `round()`·`mod()`·`rem()`·`abs()`·`sign()`·`hypot()` 및 일부 무차원 수학 함수를 처리한다. |
| 색상 | 148개 색상 이름, HSL·HWB·Lab·LCH·OKLab·OKLCH, `color(srgb ...)`, `color(srgb-linear ...)`, `color(display-p3 ...)`, `color(xyz[-d50/-d65] ...)`를 sRGB ARGB로 변환한다. 잘못된 RGB 구성요소를 거절한다. |

## 재현 방법

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe' `
    .\TWebFrame2\tests\CSSCompatibilityRegression.vcxproj `
    /t:Build /p:Configuration=Release /p:Platform=x64 /m /v:minimal

.\TWebFrame2\tests\bin\x64\Release\CSSCompatibilityRegression.exe
.\TWebFrame2\tests\bin\x64\Release\CSSCompatibilityRegression.exe --webview2
.\TWebFrame2\tests\Run-SupportCompatibility.ps1 -FullRegression
```

검사 소스는 `TWebFrame2/tests/CSSCompatibilityRegression.cpp`다. 네이티브
검사는 100%·150% DPI로 실행한다. WebView2는 숨겨진 800×600 뷰에서 같은
HTML/CSS를 사용한다. 가로 크기는 `getBoundingClientRect()`와 네이티브
레이아웃 결과를 비교한다. 색상은 네이티브 ARGB와 WebView2 Canvas 픽셀을
비교하며, Canvas의 알파 사전 곱셈/역변환에 따른 채널 양자화 오차를 허용한다.
크기 오차는 0.05 CSS px 미만이어야 한다.

실제 결과와 런타임 버전은 다음 로컬 산출물에 기록한다.

- `TWebFrame2/tests/artifacts/css-webview2-results.json`
- `TWebFrame2/tests/artifacts/css-webview2-version.txt`
- `TWebFrame2/tests/artifacts/css-compatibility-run.log`
- `TWebFrame2/tests/artifacts/css-full-regression.log`

이 비교는 화면 전체의 픽셀 일치 검사 또는 Web Platform Tests 전체 실행이 아니다.
WebView2 154가 지원하지 않는 속성 선택자의 명시적 `s` 플래그는 네이티브
확장으로 별도 검사하며 공통 비교 사례에 포함하지 않는다.

## 전체 지원을 위해 남아 있는 범위

| 영역 | 현재 한계 |
| --- | --- |
| 파서·조건 | 완전한 토큰 스트림과 모든 속성별 문법 검증이 없다. `CSS.supports()`는 구현한 속성 집합에 대해 보수적으로 답한다. 복합 오류 복구와 forgiving selector list의 모든 경우를 검증하지 않았다. |
| 선택자·스코프 | 네임스페이스, `@scope`, 모든 `:scope` 조합, Shadow DOM의 전체 선택자 의미, 방문 링크와 모든 폼 상태가 남아 있다. |
| at-rule | 컨테이너/스타일 쿼리, 등록된 `@property`, `@font-face`의 전체 폰트 로딩, 페이지·카운터·위치 시도 규칙 등의 렌더링 의미가 남아 있다. 규칙을 건너뛰는 것은 해당 기능의 지원이 아니다. |
| 캐스케이드 | 전체 속성의 초기값·상속 메타데이터, 모든 단축 속성, 모든 origin과 scope, 등록된 사용자 정의 속성의 전체 계산 규칙을 구현하지 않았다. |
| 레이아웃 | writing-mode/bidi, 논리 속성, 다단·페이지 분할, 고급 grid/subgrid, 컨테이너 단위, anchor positioning 및 flex/grid/table의 모든 경계 조건이 남아 있다. |
| 값·단위 | 글꼴 메트릭·실제 루트 글꼴·줄 높이에 의존하는 전체 단위, 전체 뷰포트 단위, 삼각함수의 각도 차원과 모든 수학 함수/특수값을 지원하지 않는다. |
| 색상 | 상대 색상, 모든 색상 공간·보간 방식, 전체 gamut mapping과 HDR가 남아 있다. 이번 변환은 sRGB ARGB 출력에 채널을 클램프한다. |
| 페인트·동작 | 전체 마스크·필터·클리핑·합성, 3D 변환, 스크롤 애니메이션과 모든 애니메이션·전환 의미, 글꼴 기능과 전체 타이포그래피가 남아 있다. |
| 검증 | 위 범위를 포함하는 Web Platform Tests 및 실제 WebView2 레이아웃·페인트 비교가 필요하다. 이번 fixture 통과로 전체 표준 준수를 주장할 수 없다. |

## 참고한 명세

- [CSS Syntax Level 3](https://www.w3.org/TR/css-syntax-3/)
- [Selectors Level 4](https://www.w3.org/TR/selectors-4/)
- [CSS Nesting Level 1](https://www.w3.org/TR/css-nesting-1/)
- [CSS Cascade Level 5](https://www.w3.org/TR/css-cascade-5/)
- [CSS Custom Properties Level 1](https://www.w3.org/TR/css-variables-1/)
- [Media Queries Level 4](https://www.w3.org/TR/mediaqueries-4/)
- [CSS Values and Units Level 4](https://www.w3.org/TR/css-values-4/)
- [CSS Color Level 4](https://www.w3.org/TR/css-color-4/)

명세와 설치된 WebView2의 동작이 다를 때 이번 비교의 기준은 실제 WebView2
결과다. 정의된 변수로 선택되지 않은 재귀 대체값은 WebView2의 측정 결과에
맞춰 순환 참조로 취급하지 않는다.

# WebView2 렌더링 정확성 pilot 실행 결과

이 문서는 최초 실행의 78/120 결과를 보존한다. [table/DPI 후속 결과](RENDERING-ACCURACY-CONTINUATION-RESULTS.md)와 [최신 글꼴·폼 결과](RENDERING-ACCURACY-TEXT-CONTROLS-RESULTS.md)는 별도로 보존한다.

기준일: 2026-10-03. [계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)의 첫 실행 단위를 구현했다. **영구 문서 20개, 120개 비교 쌍을 모두 안정적으로 캡처했고 현재 활성 검사 78개 통과·42개 실패다.** 1,000개 검증과 WebView2 전체 지원은 미완료다. 글꼴 AA를 포함한 어떠한 픽셀 오차 허용도 아직 적용하지 않았다.

## 실행 환경과 근거

- WebView2 Runtime `154.0.4258.53`, SDK `1.0.3595.46`, Windows x64 Release/v142.
- 페이지 확대 1.0, CSS viewport 384×768 / 800×600 / 1280×800, 명시적 96 / 144 DPI. 두 엔진이 동일한 파일 URL의 HTML·CSS를 읽는다.
- 실제 창 DPI는 144. 해당 60개 캡처에서 `WM_PRINTCLIENT`와 진단 캡처의 모든 픽셀이 일치했다. 120개 모두 다른 DPI를 거친 뒤 재캡처의 픽셀·JSON도 일치했다. 실제 Windows 96 DPI와 모니터 전환은 미검증이다.
- WebView2 페이지 스크립트와 native 페이지 스크립트는 비활성화했다. 기준 계측은 읽기 전용이다. JS DOM 변경 검사는 포함하지 않았다.
- DOM 전체의 순서·경로·속성·텍스트, 추적 요소의 border rect(표현 오차 상한 1/64 CSS px), 일부 계산 스타일, 정확한 디코딩 BGRA 픽셀을 판정한다. 전체 스타일, scroll/client, text fragment/advance/baseline, pseudo/iframe 대응은 보류다. `fullContractComplete=false`를 결과에 명시한다.
- 실행 ID: `20261003-151215-306-e689b9d7`. [JSON 요약](../TWebFrame2/tests/rendering/runs/20261003-151215-306-e689b9d7/summary.json), [HTML 보고서](../TWebFrame2/tests/rendering/runs/20261003-151215-306-e689b9d7/summary.html), [실패 목록](../TWebFrame2/tests/rendering/runs/20261003-151215-306-e689b9d7/failures.json).

## 문서별 결과

각 행의 분모 6은 두 DPI × 세 viewport다. 원본 입력은 수정하거나 단순화하지 않았다.

| ID | 활성 검사 통과 | 남은 실패 |
| --- | ---: | --- |
| 0001 solid box | 6/6 | — |
| 0002 fractional box | 6/6 | — |
| 0003 margin collapse | 6/6 | — |
| 0004 flex distribution | 6/6 | — |
| 0005 flex wrap | 6/6 | — |
| 0006 nested flex/grid | 6/6 | — |
| 0007 grid tracks | 6/6 | — |
| 0008 grid spanning | 6/6 | — |
| 0009 table spans | 0/6 | collapsed border의 cell 시작점·열 분배·높이, 페인트 |
| 0010 positioning/stacking | 6/6 | — |
| 0011 nested overflow | 6/6 | — |
| 0012 radius/gradient | 0/6 | 서로 다른 모서리 radius, gradient/shadow 및 픽셀 페인트 |
| 0013 SVG paint | 0/6 | 96 DPI·800×600에서 1,098개 픽셀 차이; 곡선/edge 페인트 분석 필요 |
| 0014 generated boxes | 6/6 | — |
| 0015 variables/layers/nesting | 6/6 | — |
| 0016 responsive grid | 6/6 | — |
| 0017 Latin inline | 0/6 | viewport별 일부 inline geometry, 줄 시작 공백과 텍스트 페인트 |
| 0018 mixed writing/RTL | 0/6 | direction/alignment 수정 후 스타일·box 일치, 텍스트 페인트 남음 |
| 0019 form initial state | 0/6 | UA white-space·intrinsic size·disabled widget/checkbox/button 페인트 |
| 0020 complex dashboard | 0/6 | box 좌표 일치, 텍스트 페인트 남음 |

텍스트가 있는 문서의 차이를 글꼴 AA로 확정하지 않았다. 실제 글꼴·줄바꿈·advance·baseline을 검증한 후 좁은 glyph coverage 예외를 구현해야 한다. SVG/gradient/radius/그림자의 AA는 글꼴 예외로 통과시키지 않는다.

## 공통 엔진 수정

- `LayoutRoot`를 공통화하고 body의 auto 높이와 여백 상쇄를 실제 문서 흐름에 맞췄다. 루트 body의 저장 위치만으로 BFC가 되던 처리를 수정했다. 기존 quirks의 viewport 기준 percentage min-height는 별도 WebView2 재현에서 확인해 유지했다.
- grid의 암시적 행/열을 `grid-auto-rows/columns`로 채우고 auto 행의 intrinsic 계산에서 flex-wrap의 실제 열 폭을 사용한다. spanning/반응형 grid의 잘못된 행 높이를 보완했다.
- SVG width/height HTML 속성의 기본 스타일을 적용했다. SVG namespace와 `viewBox` 등 속성 이름의 canonical case, 중복 속성의 첫 값 보존을 HTML 파서에 반영했다. 속성 보정 근거는 [WHATWG HTML 파싱 규칙](https://html.spec.whatwg.org/multipage/parsing.html#adjust-svg-attributes)이다.
- flex/grid 요소 항목의 blockification을 computed style에 반영했다. 기존 small grid 항목의 회귀검사도 실제 blockified computed display에 맞춰 갱신했으며 높이·겹침 판정은 유지했다.
- 고정 폭 자식 블록의 intrinsic text 높이를 부모 폭 대신 실제 자식 폭으로 계산한다. 다중 행 text의 높이는 유지하며 일반 inline 요소의 수직 padding이 줄 높이를 잘못 늘리지 않게 했다.
- direction을 기본 스타일과 상속에 포함하고 DirectWrite format/layout cache에도 반영했다. `text-align: start/end`를 direction에 따라 해석한다. table 및 checkbox/radio/search/color의 UA box-sizing을 보완했다.
- `View::CaptureRenderingSnapshot`은 기존 `RenderSurface`를 사용한다. 캡처가 끝나거나 실패해도 DPI·viewport·painting 상태와 GDI/D2D 자원을 정리한다. WebView2 비동기 초기화 timeout의 callback 수명과 native host 정리도 보완했다.

기존 `Run-SupportCompatibility.ps1 -FullRegression`의 10개 실행기와 platform integrity 검사는 모두 통과했다. `CSSCompatibilityRegression`의 native 검사는 290개/0 실패였다. 이는 전체 WebView2 렌더링의 42개 실패를 해결했다는 의미가 아니다.

## 판정 도구와 보존 검증

고의 오류 13개를 검증했다. 동일 이미지는 통과했고 상자/글자 1 device px 이동, 색 한 채널 변경, 요소 누락, 잘못된 clip, 글꼴 교체, 줄바꿈 변경, 텍스트 영역 안 비텍스트 오류, 알파 합성 변경, stacking 변경, 잘못된 DPI 이미지 크기는 검출했다. 글꼴 AA 모델이 없으므로 AA 예외 아래의 검출력 검증은 아직 남아 있다.

별도 복사본에 잘못된 크기의 PNG를 넣어 실행기 오류로 판정되면서 다음 정상 문서의 비교가 계속되어 통과하는 것도 확인했다. 마지막 실행 안내 변경 후 대표 문서 재실행은 13개 교정 검사와 1/1 비교를 통과했다. case 20개와 실행 환경/요약 JSON은 작성한 schema로 검증했고 생성기를 재실행해 원본 입력이 그대로 유지되는지 확인했다.

문서별 PNG/DOM/styles/box/text JSON, CDP snapshot, reference 반복 시도, 상태/로그를 저장한다. `CompareOnly`는 별도 비교 revision에 결과를 작성한다. 환경에는 Git SHA와 dirty 상태, source/계측 파일 SHA, 실행 파일 SHA, 폰트 SHA, OS/SDK/배율을 기록한다. 실제 코드 snapshot ZIP과 실제 실행 파일도 run에 보존한다.

[아카이브 catalog](../TWebFrame2/tests/rendering/archives/20261003-151215-306-e689b9d7.json)와 [ZIP](../TWebFrame2/tests/rendering/archives/20261003-151215-306-e689b9d7.zip)에 1,859개 입력·결과·코드 파일을 보관했다. 크기는 8,149,586 bytes, SHA-256은 `B8846D51A0A2656A59B5F74D4E6930D69FB0CD9D87ACACEDA9AF99BC3A00FCB0`이다. 모든 ZIP entry의 복원 해시를 확인했고 새 디렉터리에 복원하여 다시 검증했다. 원본 캐시/입력을 삭제하지 않았다.

복원된 120개 전체 재판정은 동일하게 78/120이다. 보관 실행기로 복원한 solid/fractional 두 문서를 두 DPI에서 새로 렌더링한 네 결과도 archived reference와 픽셀 차이 0이었다. [불변 baseline 인덱스](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-074f219a7b40e3d9-20261003-151215-306-e689b9d7/manifest.json)는 runtime·환경 fingerprint와 ZIP reference entry의 SHA를 묶는다. [실행·복원 안내](../TWebFrame2/tests/rendering/README.md)를 따른다. 현재 변경과 아카이브는 아직 커밋/원격 업로드하지 않았다.

## 실측과 단계 상태

현재 120개 전체 실행은 약 95.40초, 문서별 캡처 시간 합계 74.26초였다. PNG 686개는 6,026,161 bytes, profile·소스·실행 파일을 포함한 run 전체는 89,467,555 bytes다. native 실행기의 peak working set은 147,894,272 bytes이며 WebView2 자식 프로세스 메모리는 포함하지 않는다. 단순히 50배로 환산하면 약 80분이지만 고난도 문서·추가 상태·full contract의 비용은 아직 측정하지 않았으므로 전체 실행 일정으로 확정하지 않는다.

| 단계 | 현재 상태 |
| --- | --- |
| 0 계약/환경 | 초기 계약·schema·hash·실제 WebView2 29개 구문 수용 probe·coverage 기록 구현. 모든 표준의 의미 검증/환경 교정은 보류 |
| 1 캡처/DPI | 120개 안정 캡처·DPI 왕복·현재 실제 144 DPI View 경로 확인. 실제 96 DPI/모니터 전환/독립 화면 캡처 보류 |
| 2 판정 | strict BGRA·DOM/box·부분 스타일·13개 고의 오류·보고서 구현. full style/text/AA/색 교정 보류 |
| 3 pilot 20 | 영구 입력·120개 기준/결과·첫 실패 목록·아카이브/복원 완료. 활성 검사 78 통과/42 실패, full contract 미완료 |
| 4~7 100→1,000 | 미착수. 생성기는 아직 Count=20만 허용하며 미생성 수량을 검사했다고 표시하지 않음 |
| 8 영구 회귀 | pilot의 아카이브·복원·재비교/대표 재촬영 확인. 대형 단계의 분할 아카이브/원격 보관/전체 재실행 보류 |

다음 작업은 표의 collapsed border 최소 재현과 텍스트 fragment/baseline 계측을 보존해 공통 수정으로 연결하고, 글꼴 AA의 제한 모델과 나머지 판정 계약을 완성하는 것이다. 다른 모서리 radius/gradient/SVG 및 폼 페인트는 각각 독립 실패로 유지한다. 이후 80개 문서를 추가할 때 기존 20개의 입력 해시와 기준을 유지하며 자유 복합 레이아웃과 조합 coverage를 확대한다.

현재 최소 재현은 `tests/rendering/repros/`에 따로 보존했다. quirks percentage min-height는 WebView2의 640×720 viewport에서 body/main 높이 720을 확인했다. `collapsed-table-border`, `inline-wrapped-leading-space`, `nonuniform-corner-radius`는 800×600·96 DPI에서 각각 3,275 / 1,902 / 320개 픽셀 차이를 재현했다. 원본 pilot 문서를 대체하지 않는다.

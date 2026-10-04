# WebView2 렌더링 정확성 pilot 후속 결과

이 문서는 table/DPI 실행 `20261003-162837-939-d2a5124f`의 결과를 보존한다. 이후의 mixed writing·폼 수정, 24/24 문자 기하 통과와 새 계측·복원 검증은 [최신 글꼴·폼 결과](RENDERING-ACCURACY-TEXT-CONTROLS-RESULTS.md)를 따른다.

기준일: 2026-10-03. [계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)의 pilot 실패 분석과 판정 계약 보완을 진행했다. **최신 120개 비교는 84 통과·36 실패다. 최초 통과 78개를 유지하고 collapsed table의 6개 비교가 새로 통과했다.** 전체 1,000개 검증과 full contract는 미완료다.

## 결과와 판정 범위

실행 ID는 `20261003-162837-939-d2a5124f`다. WebView2 Runtime `154.0.4258.53`, Windows x64 Release/v142, SDK `1.0.3595.46`, 페이지 확대 1.0, 96/144 DPI와 384×768 / 800×600 / 1280×800 CSS px를 사용했다. [요약 JSON](../TWebFrame2/tests/rendering/runs/20261003-162837-939-d2a5124f/summary.json), [HTML 보고서](../TWebFrame2/tests/rendering/runs/20261003-162837-939-d2a5124f/summary.html), [실패 목록](../TWebFrame2/tests/rendering/runs/20261003-162837-939-d2a5124f/failures.json), [보존·환경 검사](../TWebFrame2/tests/rendering/runs/20261003-162837-939-d2a5124f/continuation-audit.json)를 보존했다.

새 계약 `pilot-strict-bgra-text-geometry-v2`는 기존 DOM·부분 스타일·추적 box·정확한 BGRA 비교에 **원본 DOM text node의 UTF-16 위치별 공백 이외 글자 내용과 font-box rect**를 추가한다. 사각형의 네 축 오차 상한은 기존과 같은 1/64 CSS px다. DirectWrite가 만든 글자 range 폭으로 advance·줄 위치 차이를 검출하며 surrogate pair도 한 범위로 기록한다. native의 실제 layout baseline/line box도 저장한다. 읽기 전용 DOM Range 계측을 사용하고 페이지 스크립트는 실행하지 않는다.

실제 glyph별 resolved font, fallback coverage, WebView2 baseline의 직접 비교, transform을 적용한 text geometry와 글꼴 AA 예외는 아직 구현하지 않았다. declared font와 native baseline 기록만으로 그 계약을 완료했다고 판단하지 않는다. 픽셀 채널 허용치와 AA 허용 픽셀 수는 계속 0이다. 이전 v1 캡처에는 글자 계측이 없으므로 기존 판정 범위로 재비교하고, 새 계측의 한쪽 누락은 실행기 오류로 처리한다.

| 문서 | 최신 통과 | 후속 결과 |
| --- | ---: | --- |
| 0009 table spans | **6/6** | box와 공유 경계 페인트 일치, 다른 픽셀 0 |
| 기존 통과 13개 문서 | **78/78** | 새로운 grid 소수 좌표 처리와 DPI 전환 후에도 유지 |
| 0012 radius/gradient | 0/6 | 서로 다른 radius 및 gradient/shadow 페인트 남음 |
| 0013 SVG paint | 0/6 | SVG 곡선·경계 페인트 남음 |
| 0017 Latin inline | 0/6 | 60개 글자 위치·폭 모두 통과, strict 페인트 남음 |
| 0018 mixed writing/RTL | 0/6 | 글자 advance/fallback 차이와 페인트 남음 |
| 0019 form initial state | 0/6 | UA 스타일·intrinsic 크기·글자 위치·widget 페인트 남음 |
| 0020 complex dashboard | 0/6 | 40개 글자 위치·폭 모두 통과, strict 페인트 남음 |

96 DPI·800×600에서 남은 픽셀 차이는 radius/gradient 31,077, SVG 1,098, Latin inline 3,275, mixed writing 2,563, form 5,335, dashboard 2,611이다. 총 실패 원인은 페인트 36쌍, text 12쌍, style 6쌍, box 6쌍이며 한 쌍에 여러 원인이 겹친다. 텍스트가 없는 문서의 빈 text 결과를 글꼴 정확성 검증으로 집계하지 않았다. 글자가 있는 24쌍 중 12쌍이 글자 기하 검사에 통과했다.

## 공통 엔진 수정

- collapsed table의 authored border와 사용 경계 폭을 분리했다. cell/row/group/table의 경계 경쟁을 계산하고 cell과 table 크기에는 공유 경계의 절반을 사용한다. colspan/rowspan 내부선을 제거하고 자식 배경 이후 공유 경계를 한 번 그린다. pilot 표의 크기는 330×158 CSS px이며 두 DPI의 모든 viewport에서 일치한다. [CSS 2.2 collapsed border 규칙](https://www.w3.org/TR/CSS22/tables.html#collapsing-borders)을 참고했다. 이번 픽셀 검증은 solid 경계이며 col/colgroup, 복잡한 border style와 서로 다른 폭의 junction 전체 지원은 보류다.
- inline 형제 다음 text node가 자동 줄바꿈되면 접힌 선행 공백을 제거한다. 폭이 다시 넓어지면 원본 공백 의미를 복원하도록 measurement invalidation도 함께 보완했다. 양쪽 DPI에서 공백 유무 문서의 줄 시작 픽셀 일치와 재배치 후 분리 공백 복원을 회귀검사한다.
- font ascent/descent와 line-height의 half-leading을 장치 좌표에서 반올림한 뒤 CSS 좌표로 한 번 변환한다. Arial 16px의 font-box는 96 DPI에서 17px, 144 DPI에서 18px로 실제 WebView2와 일치했다. font-box, baseline, caret cache에 DPI를 포함해 전환 후 이전 배율의 메트릭을 재사용하지 않는다.
- quirks block에서 image 뒤 `<br>`가 있으면 줄의 font strut를 포함하도록 보완했다. 관련 badge, text-top label, 이미지 줄의 기존 DPI 기대값은 보존한 WebView2 정적 재현에 맞춰 교정했다. 작은 차이를 허용하려고 검사 오차를 늘리지 않았다. 추가 재현의 전체 레이아웃·스타일·페인트 차이는 실패로 유지한다.
- grid track과 실제 grow/shrink가 발생한 nowrap flex 항목의 분배를 장치 layout unit으로 정리했다. 144 DPI에서 `81 2/3` CSS px 경계가 float 표현 오차 때문에 한 단위 줄어드는 경우를 보완했다. 픽셀 스냅에서도 정확한 half-pixel 근처의 표현 오차를 처리한다. 반복 96↔144 DPI 전환을 새 회귀검사에 포함했다.

## 교정·회귀·추가 재현

판정 도구 교정은 기존 이미지 13개와 새 글자 기하 9개, **총 22개가 모두 통과**했다. 동일 결과, 글자 1 장치 픽셀 이동, advance 변경, 새 줄로 이동, 글자 누락/교체, 원본 offset 변경, 잘못된 mapping과 중복 source range를 검사한다. 잘못된 glyph font와 AA 예외 아래의 검출력은 별도 계약으로 남아 있다.

`Run-SupportCompatibility.ps1 -FullRegression`에 `TableSpanRegression`을 포함해 **11개 실행기와 platform integrity 모두 통과**했다. CSS native 검사 290개/0 실패, 표·스크롤·caret·canvas·pointer 검사를 포함한다. [실행 로그와 검사 기록](../TWebFrame2/tests/rendering/runs/20261003-162837-939-d2a5124f/verification/checks.json)을 아카이브에도 담았다. case 20개와 환경/요약의 JSON schema 22개도 통과했다.

[최소 재현](../TWebFrame2/tests/rendering/repros/README.md)은 기존 4개에 DPI font/inline/image 진단 7개를 추가해 11개다. 기존 파일과 v1을 유지하고 다른 입력은 v2/v3 별도 경로로 저장했다. 각 입력 SHA-256을 기록한다. 원 회귀의 빈 `src` 이미지도 캡처할 수 있게 했으며, URL이 선언된 실제 이미지의 로드 검사는 유지했다.

같은 보관 실행기로 96/144 DPI·960×660에서 추가 22쌍을 실행했다. [추가 요약](../TWebFrame2/tests/rendering/runs/20261003-162837-939-d2a5124f/repro-evidence/summary.json)은 **4 통과·18 실패**다. collapsed border와 기존에 유지한 quirks 백분율 min-height가 각각 2쌍 통과한다. font metrics와 wrapped-leading-space 재현은 글자 기하가 통과해도 페인트 차이로 실패한다. 다른 inline/hidden/image 조합에는 style/box/text 차이도 남아 있다. 이 수량을 기본 120개에 더해 corpus 통과율을 높이지 않는다.

## 보존·복원 확인

최초 실행의 HTML/CSS 20개 해시, 기준 이미지 120개의 디코딩 픽셀, 원본 아카이브 SHA-256이 모두 그대로다. 새 기준 이미지를 native 결과로 교체하지 않았다. 현재 소스·계측 파일 85개의 해시가 실행 manifest와 일치한다. 120쌍 모두 양쪽 반복 캡처 및 native DPI 왕복이 안정적이며 현재 실제 창 DPI 144의 `WM_PRINTCLIENT` 60쌍도 일치했다. 실제 Windows 표시 설정 96 DPI는 여전히 미검증이다.

새 [아카이브 catalog](../TWebFrame2/tests/rendering/archives/20261003-162837-939-d2a5124f.json)와 [ZIP](../TWebFrame2/tests/rendering/archives/20261003-162837-939-d2a5124f.zip)은 **2,361개 파일, 9,012,412 bytes**다. 원본 입력, 최소 재현과 추가 증거, PNG/JSON/로그, 소스 snapshot, 실행 파일, 회귀 로그를 포함하며 모든 ZIP entry의 해시를 검증했다. SHA-256은 `7CC3A8E0C3028F232784AF6EECAF1C3413FE7EE28190A5C8EF6AC60BFC8B7BED`다. [불변 기준 인덱스](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-b65b9c30cdf4af56-20261003-162837-939-d2a5124f/manifest.json)는 폰트·계측 JS·판정 계약 해시를 환경 fingerprint에 포함한다.

새 위치 `restored/20261003-continuation-d2a5124f`에서 전 파일 해시를 다시 검증했고 120개 전체 재판정은 동일하게 **84/120**이었다. 복원된 소스의 계측 JS와 보관 실행기로 표/Latin 문서를 두 DPI에서 새로 렌더링한 4쌍은 기준·native 픽셀 차이 0이며 text geometry도 동일했다. [복원 검증 기록](../TWebFrame2/tests/rendering/archives/20261003-162837-939-d2a5124f-recovery-validation.json)과 [별도 복원 증거 ZIP](../TWebFrame2/tests/rendering/archives/20261003-162837-939-d2a5124f-recovery-evidence.zip)의 54개 파일도 해시 검증했다. 원 run·cache·이전 결과를 삭제하지 않았으며 커밋·원격 업로드는 하지 않았다.

기본 120개 실행 시간은 약 103.42초, 최대 문서 캡처 4.82초, 실행기의 peak working set은 147,140,608 bytes다. WebView2 자식 프로세스 메모리와 고난도 1,000개 비용은 포함하지 않는다.

## 남은 실행 단위

우선 mixed writing의 glyph advance/fallback과 form의 UA 크기·스타일을 독립 재현으로 분리한다. Latin inline/대시보드는 글자 기하가 일치하므로 resolved glyph fonts, 기준 baseline과 glyph coverage를 연결해 페인트 원인을 확인한다. 글꼴 AA를 허용하기 전에 교정 모델과 고의 오류 검출력을 검증한다. radius/gradient/shadow/SVG는 글꼴 AA 예외에 포함하지 않는다.

전체 스타일·scroll/client·pseudo/iframe 대응, 실제 Windows 96 DPI/모니터 전환, 독립 화면/색 교정과 100→1,000개 확대는 남아 있다. 최신 `fullContractComplete`와 `actualWindowsBothDpiValidated`는 모두 false다.

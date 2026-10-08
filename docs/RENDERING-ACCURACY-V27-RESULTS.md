# v27 자동 표 셀 제약·열 계산 재사용 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity와 반디집 보관·새 폴더 복원·780쌍 재렌더링 완료.

- 새 v2 고정 8문서 × 2 DPI × 3 viewport: 변경 전 18/48 strict → 변경 후 48/48 strict. 새 픽셀 차이 0.
- 총 780/780, strict 705·기존 backend 차이 75. 기존 732쌍의 네 이미지 decoded RGBA·전체 native/reference JSON·raw 비교값·판정이 동일하다.
- 예외 추가 0, registry 111개와 채널 허용치 0 불변. 기본 비교기 182개와 새 오류 주입 336/336 통과.
- 새 스타일 5,550개·기하 값 888개, 누락 0. 실제 Windows DPI 96; 144 검사는 renderer 1.5 배율이며 실제 모니터 144 DPI 검증은 미완료다.

## 수정과 근거

자동 표에서 셀의 명시 너비를 셀 글꼴의 단위로 계산하고 content-box의 수평 padding/border를 포함한다. border-box는 해당 장식을 포함한 너비를 유지한다. HTML width 힌트도 같은 셀 경로를 사용한다. colspan 제약에서는 이미 차지하는 내부 border-spacing을 빼고 부족한 열 너비만 배분한다. 중첩 표의 열은 각 표 안에서 계산한다.

변경 전에 보존한 WebView2 154의 독립 캡처가 수치 기준이다. [W3C CSS 2.2 자동 표 규칙](https://www.w3.org/TR/CSS22/tables.html#auto-table-layout)에서 자동 분배가 완전히 규정되지 않음을 확인했고, 이번 수정은 셀 제약과 간격의 한정된 사례를 다룬다. fixture별 SHA-256은 변경 전에 고정했고 문서 식별자에 따른 엔진 분기는 추가하지 않았다.

열 너비가 같은 가용 공간에서 반복 계산될 때 viewport 의존성이 없는 입력은 기존 결과를 재사용한다. 원시 viewport 단위를 가진 길이는 이전 viewport 키를 유지한다. 스타일 cascade와 viewport relayout은 grid를 무효화하므로 이미 px로 계산된 CSS도 새 스타일에 따라 다시 계산한다. 두 DPI의 TableSpanRegression에서 글꼴/장식/표 너비/행 높이/colspan과 viewport 변경 및 원래 상태 복귀를 검사했다. parser·CSS cascade는 변경하지 않았다.

## 원래 v1 넘침 사례와 미해결 페인트

최초 400px 표 입력은 변경 전 14/48 strict이며 최종 실행기는 44/48 strict·48/48 구조 검사를 통과했다. 384px viewport에서 두 표 × 두 DPI의 기본 수평 스크롤바 페인트 4쌍은 미등록 FAIL_PAINT로 남겼다. 네 실패의 native/reference/CDP/window decoded RGBA·전체 진단·raw 차이는 변경 전과 완전히 같다. 기준 캡처 두 경로도 일치했고 차이는 하단 스크롤바에만 있다. 이를 CPU/GPU 승인 예외로 추가하거나 성공으로 처리하지 않았다.

원래 fixture·manifest·before·최종 실패를 별도로 보존했다. v2는 viewport 단위와 셀 제약을 그대로 두고 두 검사 표의 너비만 320px로 고정해 넘침 페인트와 열 캐시 검사를 분리한다. 위 780쌍은 기존 732 + v2 48이며 v1 네 실패를 포함한 전체 입력 성공을 의미하지 않는다. `fixture-versions.json`과 `v1-unresolved-paint-validation.json`으로 구분한다. 기본 수평 스크롤바 래스터 정확성은 후속 작업이다.

## 속도

같은 보존 앱 자산·일반 Markdown·250개 표 문서를 사용했다. 문서/실행기당 6회, before/after 3회와 after/before 3회를 직렬 실행했다. BGRA 48쌍·layout JSON 24쌍 불변. 아래는 median milliseconds이며 증가 항목도 유지한다. summary.json에는 여섯 회차의 원값·paired 변화·IQR도 남기므로 작은 변화를 일반적인 속도 향상으로 단정하지 않는다. 반복 열 계산을 줄였으며, 개선과 증가를 아래 실제 측정값으로 구분한다.

일반 초기 레이아웃 +2.08%·최초 페인트 +4.33%, 표 초기 레이아웃 -0.33%·최초 페인트 -0.63%다. 일반/표 스크롤 페인트는 각각 +0.69%/-0.65%다. 표 초기 레이아웃은 4/6회, 일반 최초 페인트는 1/6회 빨랐다. 표 초기 레이아웃의 감소폭은 1.641ms로 회차 IQR 19.110/18.256ms보다 작아 확정적인 개선으로 단정하지 않는다. 일반 문서의 증가 항목은 후속 profiling 대상으로 남긴다.

| 문서 | 측정 | before ms | after ms | 변화 |
|---|---|---:|---:|---:|
| normal | initialLayoutMs | 206.957 | 211.253 | +2.08% |
| normal | firstPaintMs | 107.889 | 112.565 | +4.33% |
| normal | interactiveDownMs | 14.817 | 15.171 | +2.39% |
| normal | interactiveDragMs | 16.019 | 16.832 | +5.07% |
| normal | scrollPaintMs | 20.290 | 20.430 | +0.69% |
| tables | initialLayoutMs | 491.894 | 490.252 | -0.33% |
| tables | firstPaintMs | 95.428 | 94.826 | -0.63% |
| tables | interactiveDownMs | 25.919 | 25.600 | -1.23% |
| tables | interactiveDragMs | 28.368 | 28.156 | -0.75% |
| tables | scrollPaintMs | 31.924 | 31.716 | -0.65% |

## 순서와 보관

작업 전 v26 정리 상태를 확인했고 새 컴파일 중간 파일은 0개였다. v26 불변 반디집 ZIP에서 이번에 필요한 실행기·소스·앱 자산만 선택 복원하고 파일별 SHA-256을 확인했다. 변경 전 48쌍 실패/성공도 보존했다.

정확성·속도·비교기 오류 주입 이후 마지막에 전체 회귀 12개·platform integrity를 실행했다. 그 다음 반디집 fast level 1 보관·새 폴더 복원·120쌍 재판정·전체 780쌍 새 렌더링과 별도 복원 증거 보관을 검증했다. 압축본 내부의 각 파일 SHA-256도 확인했다.

증거: `C:/twf-v27/runs/20261005-table-auto-v27-final`, `C:/twf-v27/archives`. 전체 자동 표 알고리즘·복잡한 caption wrapper·column border conflict·HTML insertion modes·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 회귀·복원 이후 최종 정리

검증된 보관 사본 86,254개, 컴파일 생성물 379개, 렌더링 캐시 39,857개를 개별 경로·해시/보호 목록 확인 후 제거했다. 최신 780쌍의 PNG·JSON, 보호 입력과 최소 기록, 불변 보관본을 유지한다.

생성 컴파일 파일 0개, artifacts 29.57 MiB, D 여유 214.97 GiB. 상세 삭제 목록은 반디집 정리 기록 보관본에 포함한다. `C:/twf-v27/final-cleanup-summary.json`과 `final-retention-check.json`으로 확인한다.

기존 artifacts의 추가 로그·진단·이미지 2,525개(178.34 MiB)는 `v27-extra-artifact-history.zip`에 반디집으로 보관하고 새 폴더에 실제 복원해 각 파일 해시를 확인한 뒤 제거했다. 보호 입력·tracked 파일·HTML/JS/CSS/Markdown/Wasm 등 입력과 C++/header/프로젝트 등 소스 형식, 최소 summary/validation/index 기록은 직접 유지한다. 작업 전 artifacts 207.91 MiB에서 29.57 MiB로 줄였다.

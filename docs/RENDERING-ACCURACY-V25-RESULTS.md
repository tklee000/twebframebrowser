# v25 고정 표 우선순위·폭 경계·측정 캐시 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity와 반디집 보관·새 폴더 복원·684쌍 재렌더링 완료.

- 새 고정 8문서 × 2 DPI × 3 viewport: 변경 전 12/48 strict → 변경 후 48/48 strict. 새 픽셀 차이 0.
- 총 684/684, strict 609·기존 backend 차이 75. 기존 636쌍의 네 이미지 decoded RGBA·전체 native/reference JSON·raw 비교값·판정이 동일하다.
- 예외 추가 0, registry 111개와 채널 허용치 0 불변. 기본 비교기 182개와 새 오류 주입 336/336 통과.
- 새 스타일 5,550.0개·기하 값 888.0개, 누락 0. 실제 Windows DPI 96; 144 검사는 renderer 1.5 배율이며 실제 모니터 144 DPI 검증은 미완료다.

## 수정과 근거

고정 표는 `col` 너비, 첫 행 셀, 남은 자동 열 순으로 너비를 정한다. 명시적 0을 자동과 구분하고, colspan의 내부 border-spacing과 셀의 padding/border·box-sizing을 적용한다. 이후 행의 너비는 고정 열을 바꾸지 않는다. 자식 col이 있는 colgroup의 너비를 고정 열에 넘기지 않는 WebView2 동작도 확인했다. 지정 폭보다 큰 절대 열은 표의 외부 폭을 늘려 크기/후속 흐름을 일치시킨다. `table-layout:fixed`와 `width:auto`의 조합은 전체 행의 intrinsic measurement를 사용한다.

기준은 변경 전에 보존한 WebView2 154 캡처이며 CSS 고정 표 규칙은 [W3C CSS 2.2 표 너비 알고리즘](https://www.w3.org/TR/CSS22/tables.html#fixed-table-layout)을 참고했다. 표준만으로 자동 표의 모든 분배 결과가 정해지는 것은 아니므로 전체 자동 알고리즘 완료로 표시하지 않는다.

고정 열 계산은 첫 행 뒤에서 중단하고 셀마다 전체 너비 vector를 복사하지 않는다. 행 높이 측정은 같은 계산 열에서 재사용하며 열 계산 입력 변경 때 무효화한다. relayout/restyle/DPI·표 너비·0/auto 열 너비·행 높이 변경 및 원래 상태 복귀를 두 배율 TableSpanRegression에서 확인한다. parser·CSS cascade는 변경하지 않았다.

## 속도

같은 보존 앱 자산·일반 Markdown·250개 표 문서를 사용했다. 문서/실행기당 6회, before/after 3회와 after/before 3회를 직렬 실행했다. BGRA 48쌍·layout JSON 24쌍 불변. 아래는 median milliseconds이며 증가 항목도 유지한다. 행 측정 캐시와 배열 복사 제거를 적용했으나 이번 앱 측정에서 초기 레이아웃의 개선은 확인하지 못했다.

| 문서 | 측정 | before ms | after ms | 변화 |
|---|---|---:|---:|---:|
| normal | initialLayoutMs | 203.681 | 208.492 | +2.36% |
| normal | firstPaintMs | 111.495 | 112.490 | +0.89% |
| normal | interactiveDownMs | 15.084 | 15.086 | +0.02% |
| normal | interactiveDragMs | 16.739 | 17.199 | +2.75% |
| normal | scrollPaintMs | 20.428 | 19.849 | -2.83% |
| tables | initialLayoutMs | 489.005 | 495.219 | +1.27% |
| tables | firstPaintMs | 90.406 | 93.397 | +3.31% |
| tables | interactiveDownMs | 25.599 | 26.134 | +2.09% |
| tables | interactiveDragMs | 27.356 | 28.105 | +2.74% |
| tables | scrollPaintMs | 31.628 | 31.511 | -0.37% |

## 순서와 보관

작업 전 v24 정리 상태를 확인했고 새 컴파일 중간 파일은 0개였다. v24 불변 반디집 ZIP에서 이번에 필요한 실행기·소스·앱 자산만 선택 복원하고 파일별 SHA-256을 확인했다. 변경 전 48쌍 실패/성공도 보존했다.

정확성·속도·비교기 오류 주입 이후 마지막에 전체 회귀 12개·platform integrity를 실행한다. 그 다음 반디집 fast level 1 보관·새 폴더 복원·120쌍 재판정·전체 684쌍 새 렌더링과 별도 복원 증거 보관을 검증한다. 압축본 내부의 각 파일 SHA-256도 확인한다.

증거: `C:/twf-v25/runs/20261005-table-fixed-v25-final`, `C:/twf-v25/archives`. 전체 자동 표 알고리즘·복잡한 caption wrapper·column border conflict·HTML insertion modes·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 회귀·복원 이후 최종 정리

검증된 보관 사본 64,860개, 컴파일 생성물 303개, 렌더링 캐시 33,390개를 개별 경로·해시/보호 목록 확인 후 제거했다. 최신 684쌍의 PNG·JSON, 보호 입력과 최소 기록, 불변 보관본을 유지한다.

생성 컴파일 파일 0개, artifacts 207.91 MiB, D 여유 214.79 GiB. 상세 삭제 목록은 반디집 정리 기록 보관본에 포함한다. `C:/twf-v25/final-cleanup-summary.json`과 `final-retention-check.json`으로 확인한다.

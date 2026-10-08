# v26 표 너비 경계·행 측정 재사용 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity와 반디집 보관·새 폴더 복원·732쌍 재렌더링 완료.

- 새 고정 8문서 × 2 DPI × 3 viewport: 변경 전 15/48 strict → 변경 후 48/48 strict. 새 픽셀 차이 0.
- 총 732/732, strict 657·기존 backend 차이 75. 기존 684쌍의 네 이미지 decoded RGBA·전체 native/reference JSON·raw 비교값·판정이 동일하다.
- 예외 추가 0, registry 111개와 채널 허용치 0 불변. 기본 비교기 182개와 새 오류 주입 336/336 통과.
- 새 스타일 4,800개·기하 값 768개, 누락 0. 실제 Windows DPI 96; 144 검사는 renderer 1.5 배율이며 실제 모니터 144 DPI 검증은 미완료다.

## 수정과 근거

모든 열 또는 첫 행 셀이 명시적 0인 고정 표는 남은 너비를 균등하게 배분한다. 백분율 열과 절대 너비 열이 함께 있는 경우 남은 공간은 절대 너비 열에 비례 배분하고 이미 정해진 백분율 너비와 셀의 padding/border를 유지한다. 넘친 백분율의 축소 계산은 불필요한 부동소수점 오차를 줄였다. 예약 열을 포함한 colspan의 최종 열 크기는 두 배율의 1/64 device-pixel layout unit으로 내림하고 마지막 열이 잔여량을 받는다. 빈 고정 표도 두 바깥 horizontal border-spacing을 유지한다.

변경 전에 보존한 WebView2 154의 독립 캡처가 수치 기준이다. [W3C CSS 2.2 고정 표 규칙](https://www.w3.org/TR/CSS22/tables.html#fixed-table-layout)도 확인했다. 새 fixture별 내용은 manifest SHA-256으로 고정했고 문서 식별자에 따른 엔진 분기는 추가하지 않았다.

열 계산 입력이 달라도 실제 열 너비 vector가 같으면 행 높이를 재사용한다. 실제 열 너비가 달라지면 행 측정을 무효화한다. 자동 표의 deficit 배분에서는 셀마다 임시 index vector를 할당하지 않고 같은 순서로 두 번 순회한다. 두 DPI의 TableSpanRegression에서 relayout·restyle·열/표/percentage/행 높이 변경과 원래 상태 복귀를 검사했다. parser·CSS cascade는 변경하지 않았다.

## 속도

같은 보존 앱 자산·일반 Markdown·250개 표 문서를 사용했다. 문서/실행기당 6회, before/after 3회와 after/before 3회를 직렬 실행했다. BGRA 48쌍·layout JSON 24쌍 불변. 아래는 median milliseconds이며 증가 항목도 유지한다. summary.json에는 여섯 회차의 원값·paired 변화·IQR도 남기므로 작은 변화를 일반적인 속도 향상으로 단정하지 않는다. 행 캐시와 임시 배열 할당을 줄였으며, 개선과 증가를 아래 실제 측정값으로 구분한다.

일반 초기 레이아웃 -2.77%·최초 페인트 -1.64%, 표 초기 레이아웃 +1.89%·최초 페인트 +2.18%다. 표 문서의 초기 비용 감소는 확인하지 못했다. 일반/표 스크롤 페인트는 각각 -1.62%/-1.26%다.

| 문서 | 측정 | before ms | after ms | 변화 |
|---|---|---:|---:|---:|
| normal | initialLayoutMs | 212.561 | 206.678 | -2.77% |
| normal | firstPaintMs | 110.486 | 108.679 | -1.64% |
| normal | interactiveDownMs | 14.918 | 14.881 | -0.25% |
| normal | interactiveDragMs | 15.846 | 15.999 | +0.96% |
| normal | scrollPaintMs | 20.584 | 20.250 | -1.62% |
| tables | initialLayoutMs | 489.001 | 498.231 | +1.89% |
| tables | firstPaintMs | 89.782 | 91.741 | +2.18% |
| tables | interactiveDownMs | 25.937 | 26.412 | +1.83% |
| tables | interactiveDragMs | 27.531 | 27.716 | +0.67% |
| tables | scrollPaintMs | 31.499 | 31.102 | -1.26% |

## 순서와 보관

작업 전 v25 정리 상태를 확인했고 새 컴파일 중간 파일은 0개였다. v25 불변 반디집 ZIP에서 이번에 필요한 실행기·소스·앱 자산만 선택 복원하고 파일별 SHA-256을 확인했다. 변경 전 48쌍 실패/성공도 보존했다.

정확성·속도·비교기 오류 주입 이후 마지막에 전체 회귀 12개·platform integrity를 실행했다. 그 다음 반디집 fast level 1 보관·새 폴더 복원·120쌍 재판정·전체 732쌍 새 렌더링과 별도 복원 증거 보관을 검증했다. 압축본 내부의 각 파일 SHA-256도 확인했다.

증거: `C:/twf-v26/runs/20261005-table-boundaries-v26-final`, `C:/twf-v26/archives`. 전체 자동 표 알고리즘·복잡한 caption wrapper·column border conflict·HTML insertion modes·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 회귀·복원 이후 최종 정리

검증된 보관 사본 69,432개, 컴파일 생성물 295개, 렌더링 캐시 35,064개를 개별 경로·해시/보호 목록 확인 후 제거했다. 최신 732쌍의 PNG·JSON, 보호 입력과 최소 기록, 불변 보관본을 유지한다.

생성 컴파일 파일 0개, artifacts 207.91 MiB, D 여유 214.79 GiB. 상세 삭제 목록은 반디집 정리 기록 보관본에 포함한다. `C:/twf-v26/final-cleanup-summary.json`과 `final-retention-check.json`으로 확인한다.

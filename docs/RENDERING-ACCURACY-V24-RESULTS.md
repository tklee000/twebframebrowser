# v24 표 열 크기·하단 캡션·캐시 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity와 반디집 보관·새 폴더 복원·636쌍 재렌더링 완료.

- 새 고정 7문서 × 2 DPI × 3 viewport: 변경 전 6/42 strict → 변경 후 42/42 strict. 모든 새 픽셀 차이 0.
- 총 636/636, strict 561·기존 backend 차이 75. 기존 594쌍의 네 이미지 decoded RGBA·전체 native/reference JSON·raw 비교값·판정이 동일하다.
- 예외 추가 0, 기존 registry 111개와 채널 허용치 0 불변. 기본 비교기 182개와 새 오류 주입 294/294 통과.
- 새 스타일 5,100개·기하 값 816개 확인. 실제 Windows DPI는 96이며 144 검사는 renderer의 1.5 배율이다. 실제 모니터 144 DPI 검증을 완료로 표시하지 않는다.

## 수정

`col`/`colgroup` UA display와 width presentational hint를 공통 CSS cascade로 처리한다. stylesheet·HTML hint·CSS-wide override, empty colgroup span, 백분율·자동 표 최소 너비와 중첩 표의 독립 열 정의를 확인했다. `caption-side`는 상속·initial/unset을 처리하며 하단 캡션은 row grid 이후에 배치한다.

표 grid와 계산한 열 너비를 공유해 intrinsic measurement·레이아웃·collapsed-border paint의 반복 구축·복사를 줄인다. viewport/DPI·스타일·DOM span 변경의 캐시 무효화와 원래 크기로의 복귀를 `TableSpanRegression`에서 두 배율로 확인했다.

## 속도

같은 앱 자산·고정 일반 Markdown·250개 표 문서를 사용했다. 각 문서/실행기 6회씩, before/after 순서 3회와 after/before 순서 3회를 직렬 실행했다. parser는 변경하지 않았으며 별도 parser 속도 개선을 주장하지 않는다. BGRA 48쌍·layout JSON 24쌍 모두 동일하다. 아래는 median milliseconds이며 증가 수치도 그대로 표시한다.

| 문서 | 측정 | before ms | after ms | 변화 |
|---|---|---:|---:|---:|
| normal | initialLayoutMs | 205.537 | 208.380 | +1.38% |
| normal | firstPaintMs | 106.862 | 107.861 | +0.93% |
| normal | interactiveDownMs | 15.705 | 15.576 | -0.82% |
| normal | interactiveDragMs | 16.907 | 16.214 | -4.10% |
| normal | scrollPaintMs | 20.230 | 20.031 | -0.98% |
| tables | initialLayoutMs | 494.433 | 489.589 | -0.98% |
| tables | firstPaintMs | 89.803 | 93.526 | +4.15% |
| tables | interactiveDownMs | 25.861 | 25.525 | -1.30% |
| tables | interactiveDragMs | 27.626 | 27.908 | +1.02% |
| tables | scrollPaintMs | 31.333 | 31.288 | -0.15% |

## 보관과 정리

새 압축은 반디집 fast level 1로 생성하고, 파일별 길이·SHA-256과 실제 복원 결과를 검증한다. 캡처·소스·실행기·비교기·성능 측정·변경 전 실패·회귀 기록은 검증 보관본에 포함한다.

작업 전 컴파일 중간 파일·브라우저 캐시·검증된 기존 복원 사본을 제거했다. artifacts 과거 이력 73,270개, 84.827 GiB는 반디집 7z 114.31 MiB로 보관하고 모든 디코딩 파일 해시를 확인한 뒤 중복 원본을 제거했다. artifacts 정리 직후 194.83 MiB, D 여유 214.67 GiB.

정리 오류도 기록한다. 보호 목록 7,345개 중 7,343개는 원래 해시를 확인했으나 `security-profile`의 수집 HTML 2개를 캐시로 잘못 분류해 삭제했다. 작업 폴더·압축본에서 동일 사본을 찾지 못했다. 누락 경로와 원래 해시는 `C:/twf-v24/cleanup-archives/protected-scripts-validation.json`에 남겼고 이후 정리 도구는 보호 manifest를 제외한다.

증거: `C:/twf-v24/runs/20261005-table-sizing-v24-final`, `C:/twf-v24/archives`, `C:/twf-v24/cleanup-archives`. 전체 자동 표 알고리즘·복잡한 caption wrapper·열 border conflict·HTML insertion modes·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.


## 회귀·복원 이후 최종 정리

전체 회귀 12개·platform integrity와 복원 실행기의 636쌍 이미지/전체 진단/판정 일치가 완료된 후 정리했다. 검증 보관본과 동일한 사본 198,432개·새 컴파일 생성물 307개·브라우저 캐시 31,412개를 제거했다. 논리 파일 크기 합계 15.320 GiB다.

최신 636쌍의 PNG·JSON과 최소 결과 기록, 불변 보관본과 catalog, 작업 소스·package/vendor 입력은 유지했다. 기존 실행기·복원/성능 사본은 검증된 압축본에서 다시 꺼낼 수 있다. 다음 복원은 이미 정리한 `restored` 경로 대신 새 목적지로 실행한다. 잠긴 파일은 없었다.

- 정리 기록: `C:/twf-v24/final-cleanup-summary.json`.
- 정리 이후 C 여유 82.23 GiB, D 여유 214.80 GiB.
- 삭제 목록은 반디집 `C:/twf-v24/archives/v24-final-cleanup-records.zip` 내부의 `final-cleanup-candidates.json`으로 보관하며 각 항목의 SHA-256을 검증했다. 정리 요약은 별도 JSON으로 유지한다.

최종 보존 검사에서 최신 캡처 636쌍·현재 소스 해시 364개·세 보관본 SHA-256을 확인했다. 생성 컴파일 파일은 0개이고 artifacts는 207.91 MiB(5,535개)다. 기록은 `C:/twf-v24/final-retention-check.json`에 보존한다.

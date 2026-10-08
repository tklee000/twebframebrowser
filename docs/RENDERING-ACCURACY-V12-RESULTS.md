# Stable scrollbar gutter·RTL 정확성 개선 v12

작성일: 2026-10-04. Windows x64 Release, WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이며 두 Windows DPI 환경 검증으로 표시하지 않는다.

v11 이후 미완료 항목에서 stable/both-edges 여백과 RTL 배치를 이어갔다. 기존 문서·실행·실패·아카이브는 보존한다. 최종 실행은 `TWebFrame2/tests/rendering/runs/20261004-gutter-v12-final`이며 정확성·성능 이후 마지막 전체 회귀와 보관·복원·228쌍 새 렌더링까지 완료했다. 전체 1,000문서 계약은 미완료다.

## 공통 수정과 고정 교정 입력

- `Layout.cpp`/`Layout.h`에서 overflow가 auto/hidden/scroll인 일반 요소의 stable 여백을 레이아웃 때 한 번 결정한다. both-edges의 양쪽 물리 여백, RTL의 왼쪽 여백을 content 좌표·폭, client 크기와 일반 자식 overflow clip에서 재사용한다. clip/visible에는 여백을 넣지 않으며 scrollbar-width:none의 폭 0을 유지한다.
- 숨김 여백을 client 크기에서만 제외하고 실제 자식 배치에서는 제외하지 않던 오류를 고쳤다. block/grid에서 같은 여백을 두 번 빼지 않고 flex도 줄어든 같은 containing block을 사용한다. textarea의 실제 세로 scrollbar 역시 예약한 여백을 중복 차감하지 않는다.
- RTL 세로 scrollbar의 track·thumb·입력 위치를 왼쪽 padding edge에 배치한다. RTL grid의 logical column 좌표와 flex row/row-reverse의 방향을 공통 수정했다. 문서 ID나 문자열 분기는 없다.
- style 소유권을 검증하는 기존 4,096개 캐시에 gutter token flags를 저장해 반복 stream/string 파싱을 제거한다. 일반 overflow:visible 요소는 새 gutter 계산을 건너뛰며 grid의 방향 조회도 item마다 반복하지 않는다.
- `gutter-calibration/`에 별도 고정 입력 4문서·9개 파일 해시를 추가했다. hidden/auto/clip/visible, block/grid/flex, LTR/RTL, both-edges, 폭 0과 custom track/thumb을 비교한다. 20문서 corpus 수량에는 합산하지 않으며 새 그룹은 픽셀 예외를 허용하지 않는다.
- 새 그룹을 source snapshot·복원 재렌더링에 포함한다. 이전 snapshot에 없는 gutter 그룹은 이전 범위를 유지하고, 새 snapshot에 입력이 있으면 그룹 누락을 실패로 처리한다.
- `ScrollRenderingRegression`에는 WebView2에서 별도로 확인한 client 224×72·scroll 224×148·content x=26/폭 216과 실제 LTR/RTL thumb·빈 여백 hit-test·최대 scroll 76을 검사한다. 두 배율에서 실행하며 기존 기대값을 유지한다.

v11 저장 실행기·DLL·소스 snapshot으로 새 입력의 24쌍을 먼저 검사했다. 0/24 성공이며 원래 레이아웃과 페인트 실패를 `before-captures`에 보존했다. 수정 후 우선 8쌍은 모두 strict 성공했다. 작업 증거는 `runs/20261004-gutter-v12-work`에 있으며 잘못된 PowerShell DPI 변수 타입으로 중단한 첫 iteration도 보존했다.

공식 규칙은 [CSS Overflow 3](https://www.w3.org/TR/css-overflow/#scrollbar-gutter-property), [Flexbox의 방향](https://www.w3.org/TR/css-flexbox-1/#flex-direction-property), [Grid의 흐름 상대 좌표](https://www.w3.org/TR/css-grid-1/#grid-placement)와 대조했다. 실제 수치·픽셀 판정은 고정한 WebView2의 독립 캡처에 근거한다.

## 속도와 출력 검증

같은 보존 Markdown·앱 HTML/CSS/JS, actual 96 DPI·1600×1000에서 v11/v12를 번갈아 3회씩 측정했다. 각 버전 스크롤 페인트 36회의 중앙값이며 앱 추가 재빌드는 하지 않았다.

| 항목 | v11 before | v12 after |
| --- | ---: | ---: |
| 스크롤 페인트 | 20.75ms | 20.18ms |
| 최초 페인트 | 109.29ms | 108.28ms |
| 초기 레이아웃 | 201.47ms | 207.83ms |
| 외부 포커스 후 첫 클릭 | 14.66ms | 15.41ms |
| 실제 thumb 드래그 | 16.07ms | 17.11ms |

스크롤 페인트는 이 로컬 측정에서 약 2.8% 감소했다. 초기 레이아웃·첫 클릭·드래그는 늘어 모든 경로가 빨라졌다고 주장하지 않는다. 이 문서에는 stable/RTL gutter를 쓰지 않으므로 gutter 캐시 단독 효과의 수치로 해석하지 않는다. BGRA 12쌍·layout JSON 6쌍·입력 3쌍은 byte 단위로 동일하고 첫 클릭·드래그는 입력 처리 중 동기 페인트했다. 입력·자산·실행기·DLL·Layout 소스 해시와 raw 시간은 `performance/`에 있다.

## 최종 검증 상태

- [x] 원본 및 기존 교정 204쌍 성공·이전 모든 native/reference/CDP/가능한 WM_PRINTCLIENT 픽셀과 DOM·스타일·문자·기하·raw 수치/판정 불변. 비교기 고의 오류 182개 통과.
- [x] 새 gutter 24/24 strict 성공·새 예외 0·v11 before에서 수집한 기준 픽셀/진단 불변. 크기 값 864개·필수 스타일 5,400개, 누락 0.
- [x] 총 228/228, strict 174·기존 승인 54·소스 snapshot 해시 184개 일치. `pixel-diagnostic-invariance.json`에 검사 결과 보존.
- [x] 정확성·성능 작업 이후 마지막 전체 회귀 12개·platform integrity. 새 LTR/RTL thumb 드래그·빈 여백 hit-test도 두 배율에서 통과. `regression/full-regression.log`와 `regression/summary.json`에 보존.
- [x] 주 ZIP 7,143개 파일·불변 baseline·새 폴더 복원·소스 184개/입력/실행기/DLL 해시·원본 120쌍 재판정. raw 수치와 판정 동일.
- [x] 복원한 자료의 228쌍 새 렌더링: 228/228 성공·174 strict·기존 승인 54. native/reference/CDP/가능한 WM_PRINTCLIENT 픽셀과 DOM·스타일·문자·기하·raw 수치·판정 모두 재현. 복원 증거 ZIP 6,115개 파일도 각각 해시 검증.

## 보관·복원 증거

| 자료 | 결과와 위치 |
| --- | --- |
| 주 아카이브 | [ZIP](../TWebFrame2/tests/rendering/archives/20261004-gutter-v12-final.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-gutter-v12-final.json). 7,143개 파일, 317,277,710 bytes. 변경 전·중간 실패·성능 자료와 이전 v11 아카이브도 포함. |
| 불변 기준 인덱스 | [manifest](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-e719e7b42f2410b6-20261004-gutter-v12-final/manifest.json). 원본 reference 120쌍, 환경 ID `e719e7b42f2410b6`. 교정 문서는 별도 그룹으로 주 ZIP에 포함. |
| 새 복원 폴더 | `TWebFrame2/tests/rendering/restored/20261004-gutter-v12-final-verified`. 소스 184개·원본 20문서와 교정 입력·실행기·DLL 해시 확인. |
| 최종 복원 판정 | [recovery-validation.json](../TWebFrame2/tests/rendering/archives/20261004-gutter-v12-final-recovery-validation.json). 복원 코드의 120쌍 재판정과 새 렌더링 228쌍의 그룹별 결과·raw 수치 포함. |
| 복원 증거 | [ZIP](../TWebFrame2/tests/rendering/archives/20261004-gutter-v12-final-recovery-evidence.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-gutter-v12-final-recovery-evidence.json). 6,115개 파일, 17,399,404 bytes, 모든 파일 해시 일치. |

주 ZIP SHA-256은 `D03B75651FC3A7724779287087C35A957621FA9B298F2FB75ACABE5EC927B462`, 복원 증거 ZIP은 `8A136BEBB620BE60567E0EF9E00031C0FCBE1C0A18B40D45B6FDC8494A48C441`다. ZIP 내부 `documentation/`은 보관 직전 상태의 스냅샷이며 최종 완료 판정은 위 복원 JSON과 이 문서를 따른다. 기존 실행·실패·아카이브를 덮어쓰지 않았다.

## 남은 전체 계획

이번 범위 밖의 intrinsic/nested/textarea/root gutter 조합과 positioned/stacking/rounded clip, RTL 음수 overflow·scroll offset, transform/pseudo/iframe/root overflow, 나머지 계산 스타일과 reference per-character glyph/baseline·cluster는 남아 있다. 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대, 원격 영구 보관도 완료로 표시하지 않는다. 이번 고정 교정 성공을 gutter나 RTL의 모든 조합을 지원한다는 뜻으로 사용하지 않는다.

# Overflow 클립·RTL 강제 스크롤바 정확성 개선 v13

작성일: 2026-10-04. Windows x64 Release, WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이며 실제 두 Windows DPI 환경 검증으로 표시하지 않는다.

v12 이후 일반 자식과 별도 z-index 페인트의 사각형 overflow 클립을 이어서 검증했다. 최종 실행은 `TWebFrame2/tests/rendering/runs/20261004-overflow-v13-final`이다. 정확성·성능을 확인한 뒤 마지막 전체 회귀 12개·platform integrity, 보관·복원과 복원 실행기의 252쌍 새 렌더링을 완료했다. 기존 실행·실패·아카이브는 유지하며 전체 1,000문서 계약은 미완료다.

## 공통 수정과 독립 기준

- 실제 WebView2 캡처에서 빈 stable 여백은 padding edge 안의 그림자를 허용하지만 실제 scrollbar track은 자식 페인트에서 제외된다. 일반 자식이 빈 양쪽 여백까지 잘라내던 오류와, deferred z-index 자식이 실제 scrollbar 위로 그려지던 오류를 공통 수정했다. 레이아웃의 예약 여백과 페인트의 실제 트랙 제외를 구분한다.
- `Layout.cpp`/`Layout.h`에 사각형 자식 클립을 box rect에 상대적인 값으로 보존한다. 일반 페인트·deferred stacking clip·일반/stacking hit-test·텍스트 hit-test가 같은 직사각형을 사용한다. 스크롤과 sticky 이동은 상대 값을 재사용하고, 재배치와 restyle 때 다시 결정한다.
- 강제 세로 scrollbar도 레이아웃에서 한 번 예약한다. RTL의 왼쪽 실제 scrollbar만큼 자식 원점을 이동하고 block/grid/textarea의 중복 차감을 막는다. 문서 ID나 입력 문자열 분기는 없다.
- 레이아웃에서 이미 결정한 overflow-y를 재사용해 metadata 단계의 양축 스타일 재조회와 매 페인트의 scrollbar pseudo 스타일 계산을 줄인다. 텍스트 노드는 자체 자식 overflow clip을 만들지 않는다.
- `overflow-calibration/`에 고정 4문서·9개 파일 해시를 추가한다. hidden/clip, 빈 stable/both-edges 여백, 일반·z-index 그림자, LTR/RTL custom scrollbar, 중첩 클립을 검사한다. corpus 20문서에는 합산하지 않고 픽셀 예외를 허용하지 않는다. source snapshot과 복원 새 렌더링에도 포함한다.
- 별도 읽기 전용 WebView2 `elementFromPoint` 기준은 두 DPI에서 21개 위치씩 같다. border·빈 여백·실제 track·상자 밖의 일반/deferred hit-test, 스크롤 후 좌표 이동과 재배치를 `ScrollRenderingRegression`에서 검사한다. 기존 기대값을 유지한다.

v12 소스 184개 해시와 저장 실행기·DLL·snapshot을 확인했다. 고정 새 입력의 변경 전 결과는 6/24 strict 성공·18 실패이며, 수정 직후 우선 8쌍은 모두 strict 성공했다. `runs/20261004-overflow-v13-work/before-captures`, `iteration-1`, `pointer-oracle`에 보존한다.

처음 탐색 입력의 상대 위치 inset·넓은 RTL 상자·HTML CR/CRLF 처리 실패는 `exploratory-inputs`·`exploratory-captures`·`exploratory-summary.json`에 별도 보존했다. 이 실패를 승인 예외나 성공으로 바꾸지 않았다. 해당 기능의 일반 지원을 이번 클립 교정의 통과로 표시하지 않는다.

공식 규칙은 [CSS Overflow 3](https://www.w3.org/TR/css-overflow/)와 [CSS Backgrounds의 corner clipping](https://www.w3.org/TR/css-backgrounds-3/#corner-clipping)을 대조했다. 실제 수치·픽셀·hit-test 기대값은 고정 WebView2의 독립 캡처에 근거한다. 이번 범위는 사각형이며 둥근 모서리·변형·외부 containing block을 가진 positioned 자식은 추가 검증이 필요하다.

## 속도와 출력 검증

같은 보존 Markdown·앱 HTML/CSS/JS, actual 96 DPI·1600×1000에서 v12/v13을 번갈아 3회씩 측정했다. 스크롤은 각 버전 36회의 중앙값이다. MdViewer 앱 추가 재빌드는 하지 않았다.

| 항목 | v12 before | 최종 v13 after |
| --- | ---: | ---: |
| 초기 레이아웃 | 212.42ms | 203.26ms |
| 최초 페인트 | 107.30ms | 117.37ms |
| 스크롤 페인트 | 20.35ms | 20.30ms |
| 외부 포커스 후 첫 클릭 | 15.61ms | 14.92ms |
| 실제 thumb 드래그 | 16.81ms | 15.74ms |

최종 로컬 측정에서 초기 레이아웃은 약 4.3%, 첫 드래그는 약 6.4% 감소했다. 스크롤 페인트의 약 0.3% 차이는 큰 개선으로 해석하지 않는다. 최초 페인트는 약 9.4% 늘어 모든 항목이 빨라졌다고 주장하지 않는다. BGRA 12쌍·layout JSON 6쌍·문서 입력 3쌍은 byte 단위로 동일하고 첫 클릭·드래그는 입력 처리 중 동기 페인트했다. 환경·자산·소스·실행기·DLL 해시와 raw 시간은 `performance-final/`에 있다.

초기 클립 캐시 구현의 첫 측정은 초기 레이아웃 198.96→211.48ms, 최초 페인트 108.94→117.09ms, 스크롤 20.49→20.11ms였다. 이를 `performance/`에 보존하고 overflow 축 조회를 재사용한 최종 소스로 새 paired 측정을 했다. 서로 다른 paired 측정의 before 시간이 달라 최종 속도 차이를 캐시 변경 하나의 단독 효과로 주장하지 않는다.

## 최종 검증 상태

- [x] 우선 8쌍 strict와 독립 pointer 기준의 두 배율 focused 회귀 성공. backend 예외 등록 파일의 기존 90개 서명 불변.
- [x] 최종 소스의 번갈아 성능 측정과 화면·레이아웃·문서 불변, 입력 중 동기 페인트 확인.
- [x] 총 252/252 성공: strict 198·기존 승인 54. 새 overflow 24쌍 strict·변경 전 실패 18쌍 교정·기존 control 6쌍 유지, 기준 픽셀/진단 불변. 크기 720개·필수 스타일 4,500개·누락 0. 비교기 고의 오류 182개 통과.
- [x] 소스 snapshot 해시 194개 및 기존 228쌍의 native/reference/CDP/가능한 WM_PRINTCLIENT 픽셀·DOM·필수 스타일·문자 기하·border rect·CSSOM 크기·raw 수치/판정 불변. 다른 native 내부 진단도 그대로다.
- [x] 기존 geometry-scroll의 강제 scrollbar 상자 6쌍은 내부 contentWidth/internalScrollWidth만 120→108px로 교정. WebView2 clientWidth 120px − 양쪽 padding 12px = 108px이며 실제 scrollbar 폭 12px과도 일치한다. 이 두 값의 정확한 교정을 기록하고 나머지 native JSON 전체는 같은지 검사했다. 모든 native 진단 불변으로 표시하지 않는다. `pixel-diagnostic-invariance.json`에 6쌍의 양쪽 값과 독립 기준을 보존한다.
- [x] 정확성·성능 작업 이후 마지막 전체 회귀 12개·platform integrity. 최종 소스의 새 pointer/scroll/relayout 교정도 두 배율에서 통과. `regression/full-regression.log`·`regression/summary.json`에 보존.
- [x] 주 ZIP 9,835개 파일·불변 baseline·새 폴더 복원·소스 194개/입력/실행기/DLL 해시·원본 120쌍 재판정. raw 수치와 판정 동일.
- [x] 복원한 자료의 252쌍 새 렌더링: 252/252 성공·198 strict·기존 승인 54. 최종 v13의 native/reference/CDP/가능한 WM_PRINTCLIENT 픽셀과 전체 DOM·스타일·문자·기하 진단, raw 수치·판정 모두 재현. 복원 증거 ZIP 6,672개 파일도 각각 해시 검증.

한 캡처 그룹의 최초 드라이버 호출 파일명이 틀린 시도는 `capture-driver-correction.txt`에 남겼다. 실제 `Test-RenderingCaptureRoutes.ps1`로 그 그룹만 수집해 18/18 strict를 확인하고 전체 감사를 완료했다. 이미 수집한 그룹을 덮어쓰지 않았으며 비교 계약·소스 snapshot은 유지했다.

## 보관·복원 증거

| 자료 | 결과와 위치 |
| --- | --- |
| 주 아카이브 | [ZIP](../TWebFrame2/tests/rendering/archives/20261004-overflow-v13-final.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-overflow-v13-final.json). 9,835개 파일, 100,727,090 bytes. 변경 전·탐색 실패·중간 교정·최종 성능 자료와 v12 비교 증거 포함. |
| 불변 기준 인덱스 | [manifest](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-3f0e9e06840cc63f-20261004-overflow-v13-final/manifest.json). 원본 reference 120쌍, 환경 ID `3f0e9e06840cc63f`. 교정 문서는 별도 그룹으로 주 ZIP에 포함. |
| 새 복원 폴더 | `TWebFrame2/tests/rendering/restored/20261004-overflow-v13-final-verified`. 소스 194개·원본 20문서와 교정 입력·실행기·DLL 해시 확인. |
| 최종 복원 판정 | [recovery-validation.json](../TWebFrame2/tests/rendering/archives/20261004-overflow-v13-final-recovery-validation.json). 복원 코드의 120쌍 재판정과 새 렌더링 252쌍의 그룹별 결과·raw 수치 포함. |
| 복원 증거 | [ZIP](../TWebFrame2/tests/rendering/archives/20261004-overflow-v13-final-recovery-evidence.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-overflow-v13-final-recovery-evidence.json). 6,672개 파일, 18,051,889 bytes, 모든 파일 해시 일치. |

주 ZIP SHA-256은 `C7882A85E04E6D5E52C7E2FC7DB949064062E18A15CFA2017BBC8EA4CBDFDA2C`, 복원 증거 ZIP은 `A9ED8A4F001075A2A7AB22900BD0DEB72BC34B4AB79AA7023470EC7B5EEC00F6`다. ZIP 내부 `documentation/`은 보관 직전 상태의 스냅샷이며 최종 완료 판정은 위 복원 JSON과 이 문서를 따른다.

이전 ZIP은 원래 `archives/`에 보존하고 새 ZIP에 재귀 복사하지 않았다. 대신 `before-evidence/v12/`에 이전 228쌍의 비교 픽셀·진단·결과와 catalog/SHA 참조를, `optimization-and-accuracy-evidence/`에 변경 전 실행기·DLL·소스 snapshot 및 이번 작업·실패·성능 자료를 보존했다. 기존 실행·실패·아카이브를 덮어쓰지 않았다.

## 남은 전체 계획

탐색에서 보존한 relative inset·넓은 RTL/음수 overflow·HTML 줄바꿈 입력 처리, intrinsic/nested/textarea/root gutter의 나머지 조합, rounded/transformed/deferred clip과 외부 containing block, RTL scroll offset, transform/pseudo/iframe/root overflow, 나머지 계산 스타일·reference per-character glyph/baseline·cluster는 남아 있다. 실제 Windows 두 DPI·독립 화면 캡처, 100→1,000문서 확대와 원격 영구 보관도 미완료다.

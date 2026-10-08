# CSSOM 크기·폼 렌더링 정확성 검증

이 문서는 v9 당시 결과를 보존한다. 이후 v11에서 geometry 글자·select 12쌍의 동일 입력 CPU/GPU 원인 실험을 통과하고 정확한 픽셀 서명을 등록했다. 새 실행의 geometry 18/18과 전체 204/204 성공, 최종 성능 및 마지막 회귀·복원 결과는 [v11 기록](RENDERING-ACCURACY-V11-RESULTS.md)을 따른다. 아래의 strict 6/18과 raw 차이는 그대로 유지된다.

작성일: 2026-10-04. Windows x64 Release, WebView2 `154.0.4258.53`, 96/144 DPI, CSS viewport 384×768·800×600·1280×800. 엔진은 CPU 페인트를 유지한다.

`20261004-cssom-v9-final3`에서 원본 120/120, 기존 스타일 교정 18/18이 성공했다. 새 CSSOM 크기 4개를 정확히 비교하며 원본 크기 값 2,784개와 필수 스타일 값 17,400개에 누락이 없다. 추가 크기 교정 18쌍은 DOM·스타일·상자·문자 기하·크기가 모두 일치하지만 strict 페인트는 6/18이다. 남은 12쌍을 성공으로 합산하지 않는다. 전체 계약과 1,000문서는 미완료다.

## 공통 엔진 수정

- `clientWidth/clientHeight/scrollWidth/scrollHeight`에 padding을 포함하고 border를 제외하며 실제 scrollbar 폭/높이와 정수 CSS px 반올림을 적용했다. inline·display:none 및 숨김 하위 요소·html/body·table·폼의 차이를 처리한다. View의 JavaScript 조회와 렌더링 진단이 같은 계산을 사용한다. 내부 content/scroll 값도 별도로 보존한다.
- 하위 overflow를 레이아웃 메타데이터와 함께 캐시했다. 각 요소 조회에서 하위 트리 전체를 다시 순회하지 않는다. 바깥 containing block에 배치된 absolute 자식이 현재 요소의 scroll 영역을 잘못 늘리는 경우도 수정했다. 색 교정의 높이 0인 body는 이제 기준처럼 clientHeight/scrollHeight 모두 0이다.
- 같은 inline 줄에서 뒤에 오는 tall inline-block이 줄 baseline을 늘리면 이미 배치한 텍스트도 재정렬한다. 줄 메트릭이 바뀔 때만 재배치한다.
- separate table의 외곽/셀 사이 `border-spacing`, colspan/rowspan 간격과 HTML table의 UA 기본 2px을 반영했다. CSS `display:table`의 초기 간격은 0이다. 간격의 장치 픽셀 단위를 처리해 144 DPI의 3px 간격이 기준과 같은 2.666667 CSS px이 된다. collapsed table 간격은 0이다.
- bare `::-webkit-scrollbar` 선택자의 암시적 universal subject를 처리했다. 너비만 지정한 custom scrollbar는 기본 투명 track/thumb이며 암시적 화살표를 만들지 않는다. 스크롤 영역의 descendant clip은 scrollbar 공간을 제외한다.
- textarea의 초기 DOM 텍스트·UA 스타일·대체 콘텐츠 처리를 보강하고 자체 텍스트는 padding client 영역에 클립한다. input의 남는 세로 공간이 홀수 장치 픽셀일 때 중앙 정렬이 한 픽셀 아래로 밀리는 오류를 수정했다.

한 문서 ID나 특정 문자열을 분기하는 엔진 우회는 넣지 않았다. MdViewer 앱은 추가 재빌드하지 않았다.

## 비교 계약과 결과

capture schema v5, geometry contract v1을 도입했다. 모든 추적 요소에 `client:[width,height]`, `scroll:[width,height]`가 필요하다. 배열 누락·길이 오류·null·음수·소수·NaN/Infinity·계약 버전 오류는 HARNESS_ERROR다. 정상 크기가 한 CSS px라도 다르면 FAIL_LAYOUT이며 backend 예외로 통과시킬 수 없다. 이전 캡처는 원래 계약을 유지하며 새 검사를 했다고 표시하지 않는다.

| 입력 | 쌍 | 전체 비교 성공 | strict 픽셀 | 확인한 범위 |
| --- | ---: | ---: | ---: | --- |
| 원본 corpus 20문서 | 120 | 120 | 84 | 승인 차이 36; 스타일 17,400·크기 2,784·누락 0 |
| 기존 스타일 교정 3문서 | 18 | 18 | 12 | 승인 폼 6; 기대값 660/660 |
| 추가 크기 교정 3문서 | 18 | 6 | 6 | 비페인트 검사 18/18; 크기 값 648·누락 0 |
| 독립 RGB/alpha/DPI/viewport 교정 3문서 | 18 | 18 | 18 | 기준 캡처 경로·색·크기·좌표 일치 |

비교 교정은 기존 134개와 크기 오류 교정 48개, 총 182개 통과다. 네 축의 +1 오차, 숨김 요소의 잘못된 크기, 누락/손상, 승인 페인트 차이와 동시에 존재하는 크기 오류를 실제 판정기로 검사한다. 별도 교정 9문서는 원본 corpus 수량에 합산하지 않는다.

원본 HTML/CSS는 불변이다. 이전 성능 완료 실행 `20261004-performance-accuracy-final`과 비교한 자체 PNG 120개와 기준 PNG 120개도 디코딩 픽셀이 모두 같다. source snapshot의 157개 파일 해시를 확인했다. 명시적 DPI 변경 후 반복 캡처 안정성과 CapturePreview/CDP 캡처 일치를 유지한다. 실제 창은 96 DPI이며 한 실행에서 실제 Windows 두 DPI 전환을 완료한 것은 아니다.

## 기존 폼 6쌍의 독립 원인 실험

이전 스타일 교정의 checkbox·button·select를 동일한 좌표와 색으로 독립 실행했다. [Chromium native theme 구현](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/native_theme/native_theme_base.cc)의 primitive를 확인하고, 기준 PNG의 회색 경계 모양만으로 승인하지 않았다.

- 숨긴 WGL 창의 GaneshGL, MSAA 0: checkbox와 button이 96/144 DPI에서 기준 픽셀과 완전히 일치했다.
- 보존한 이전 GPU 소스의 독립 D3D11 8×MSAA path probe: 두 select 화살표도 두 DPI에서 기준 픽셀과 완전히 일치했다.
- 다른 CPU/GPU 경로로 시험한 중간 결과와 실패한 탐색 결과도 보존했다. GPU 코드가 현재 엔진으로 돌아간 것은 아니다.

계획에 기록된 사용자 승인 정책에 따라 기존 72개 예외를 유지하고 `style-ua-overflow`의 2 DPI×3 viewport, 정확한 6개 픽셀 서명만 추가했다. DOM·스타일·크기·문자·캡처 오류가 있거나 다른 서명이 나오면 실패한다. raw 차이는 96 DPI 125픽셀, 144 DPI 275픽셀, 최대 채널 차이 31이며 strict 성공은 여전히 12/18이다.

근거는 최종 실행의 `before-evidence/20261004-accuracy-v9-before/control-probe-full-gpu-{96,144}`와 `control-probe-d3d-{96,144}`, 독립 probe 소스/실행기/입력, `20261004-cssom-v9-control-approval/control-backend-approval.json`에 있다.

## 추가 교정의 raw 실패

추가 크기 교정은 레이아웃을 통과한 뒤에도 페인트 차이를 엄격히 유지한다.

| 문서 | 96 DPI | 144 DPI | 상태 |
| --- | ---: | ---: | --- |
| geometry-boxes | 21픽셀·최대 1 | 22픽셀·최대 1 | inline 글자 합성 경계, 미등록 FAIL_PAINT |
| geometry-scroll | 32픽셀·최대 31 | 49픽셀·최대 31 | select 화살표 경계, 이 입력 서명은 미등록 FAIL_PAINT |
| geometry-tables | 0 | 0 | strict 성공 |

수치는 각 viewport에서 같다. 수정 전 geometry-scroll의 96 DPI 차이 5,003픽셀은 32픽셀로 줄었다. 144 DPI input 위치를 포함한 1,455픽셀 차이는 49픽셀로 줄었다. 144 DPI separate table의 상자/크기 오류 12개와 1,455픽셀 차이도 제거했다. 각 수정 전 실행은 최종 `before-evidence`에 보존했다.

## 속도 유지 검증

같은 보존 Markdown과 같은 MdViewer HTML/CSS/JS 자산을 별도 자산 폴더에서 실행했다. 실제 96 DPI·1600×1000에서 변경 전/후를 번갈아 각 3회 측정했다. 페인트 36개씩의 중앙값과 실행별 주요 시간의 중앙값이다.

| 항목 | 변경 전 | 변경 후 |
| --- | ---: | ---: |
| 스크롤 페인트 | 20.67ms | 21.41ms |
| 초기 레이아웃 | 202.07ms | 209.31ms |
| 최초 페인트 | 113.77ms | 106.03ms |
| 포커스 해제 후 첫 scrollbar 클릭 | 16.01ms | 16.35ms |
| 실제 thumb 드래그 | 17.70ms | 17.15ms |

최종 스크롤 측정은 3.6% 증가했으며, 초기 레이아웃도 3.6% 증가하고 최초 페인트는 6.8% 감소했다. 앞선 같은 자산 측정은 20.81→20.65ms였다. 작은 변화는 로컬 측정의 변동 범위로 해석하며 신규 속도 향상을 주장하지 않는다. 기존 약 21ms의 스크롤 페인트 최적화는 유지했다. 세 실행의 초기/스크롤 BGRA 12쌍과 레이아웃 JSON 6쌍이 byte 단위로 동일하고, 첫 클릭/드래그는 모두 입력 메시지가 반환되기 전에 동기 페인트했다. `preview-pane.clientHeight`는 content 높이 842 대신 padding을 포함한 기준 크기 878을 반환한다. scrollTop과 실제 화면은 같다.

원본 입력·앱 자산·양쪽 측정 실행기·Skia DLL·환경 해시·반복 실행 스크립트·raw 시간·화면은 `performance-evidence`에 있다. 이전 단계의 74.3→21.6ms 개선을 이번 변경의 신규 성과로 재집계하지 않는다.

## 최종 회귀·보관·복원

정확성·추가 교정·번갈아 속도 측정이 끝난 뒤 전체 회귀 12개와 platform integrity를 실행해 모두 통과했다. 최종 실행을 ZIP/catalog와 불변 baseline으로 보관하고 압축 해제 파일 14,352개의 SHA-256을 모두 검증했다. 새 폴더에서 복원한 소스 157개·입력 20개·실행기·DLL을 확인하고 원본 120쌍을 재판정해 같은 결과를 얻었다. 이어 복원한 코드와 실행기로 원본 120쌍과 세 교정 집합 각 18쌍, 총 174쌍을 새로 렌더링했다. 기준/자체/CDP/가능한 WM_PRINTCLIENT 픽셀과 DOM·스타일·기하·raw 수치·판정이 모두 같다. 추가 크기 교정의 페인트 실패 12쌍도 같은 결과로 재현됐다.

첫 전체 회귀에서는 과거 fixed-table 테스트 두 개가 기본 간격을 0으로 가정해 실패했다. 실제 WebView2의 추가 4쌍에서 열 너비 98/196과 100/794, 외곽·열 사이 간격 2px을 확인해 엄격한 기대값을 보강했다. 이전 실패 로그와 oracle 자료는 `before-evidence/final2-test-contract`에 있다. 전체 입력 비교에서 HTML width 두 쌍은 strict 성공하고, fixed-first-row 두 쌍은 열 너비는 같지만 th 초기 정렬과 긴 셀의 최소 행 높이 차이가 남아 별도 실패로 보존했다. 이 조사 문서를 원본/교정 수량에 합산하지 않는다.

전체 회귀 로그와 순서·시각은 `regression`에 보존했다. 복원한 계측/비교 소스로 174쌍을 실제 다시 렌더링한 증거를 별도 4,865개 파일 ZIP으로 보관하고 각 파일 해시도 검증했다. 기존 실행·ZIP·삭제된 사용자 파일 상태는 유지했다.

- 최종 실행: `TWebFrame2/tests/rendering/runs/20261004-cssom-v9-final3`
- [최종 archive catalog](../TWebFrame2/tests/rendering/archives/20261004-cssom-v9-final3.json): 168,132,382 bytes, SHA-256 `E248B338F1D051CF83E74CA30C8CD504E3658BE2385E1BC7C451AC0470B67D7B`
- [복원 증거 catalog](../TWebFrame2/tests/rendering/archives/20261004-cssom-v9-final3-recovery-evidence.json): 14,308,202 bytes, 174쌍·모든 픽셀/진단/판정 동일
- [복원 결과](../TWebFrame2/tests/rendering/archives/20261004-cssom-v9-final3-recovery-validation.json)
- 불변 baseline: `154.0.4258.53-d35536483fe97ffb-20261004-cssom-v9-final3`
- 복원 폴더: `TWebFrame2/tests/rendering/restored/20261004-cssom-v9-final3-verified`

## 남은 계획

추가 크기 교정의 미등록 글자/select 페인트 차이, table 조사에서 발견한 th 초기 정렬과 긴 셀의 최소 행 높이, textarea 초기 텍스트의 내부 scroll 동기화·다중 줄 교정, 결합 scrollbar 재배치와 stable 양쪽 gutter, scroll offset/RTL/transform/pseudo/iframe/root overflow의 복합 조건, 나머지 계산 스타일, reference glyph/baseline·cluster coverage, 실제 Windows 두 DPI 및 독립 화면 캡처가 남아 있다. 다음 단계는 이 범위를 보강한 뒤 100문서, 이후 1,000문서로 확장하는 것이다. 이번의 pilot와 별도 교정 성공은 전체 계약 완료가 아니다.

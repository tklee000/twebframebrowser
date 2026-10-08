# v15~v16 통합 정확성·속도 검증 결과

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**348/348 성공(294 strict·기존 승인 54)**이다. 완료된 [v15의 318쌍](RENDERING-ACCURACY-V15-RESULTS.md)을 기준으로 v16의 5문서 30쌍을 교정했다. 변경 전 0/30, 최종 새 30쌍은 픽셀 차이 0이다. 기존 318쌍의 native/reference/CDP/WM_PRINTCLIENT 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정은 불변이고 새 예외는 없다. 정확성·속도 확인 후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원 및 복원 실행기의 348쌍 새 렌더링을 완료했다.

## 묶음 계획과 공통 수정

v15는 pre/listing/textarea 첫 LF·RCDATA/RAWTEXT·fragment 줄바꿈과 inline pre 교정을 완료한 기준이다. v16에서는 남은 script 상태와 plaintext·NULL/EOF·일부 잘못된 opener를 공통 파서에서 수정했다. v15의 기존 회귀/보관을 다시 실행 단계로 표시하지 않고 이번 묶음의 최종 검증만 마지막에 수행했다.

- script data의 escaped/double-escaped와 dash/dash-dash 상태를 구현했다. `<!--<script>` 안의 첫 `</script>`는 double escape를 종료하고 문자를 유지하며, 다음 적절한 종료 태그가 요소를 닫는다. ASCII 대소문자·delimiter·거짓 접두어·`-->` 복귀·EOF를 구분한다. 일반 script data는 다음 `<`로 건너뛰고 전체 소문자 복사를 하지 않는다.
- plaintext는 초기 LF·entity·모든 후속 마크업을 문자로 유지하고 물리 CR/CRLF만 정규화한다. 문서 text 모드의 물리 NULL은 U+FFFD로 교정한다. 실제 WebView2 innerHTML에서 RCDATA fragment NULL은 삭제, raw/script/plaintext는 U+FFFD임을 확인하고 별도로 구현했다. 직접 DOM 텍스트 문자열은 그대로 유지한다.
- data의 잘못된 `<` opener는 문자 `<`를 내보내고 다음 문자를 다시 읽어 `<<div>`의 유효한 형제를 잃지 않는다. `<!-->`·`<!--->`의 즉시 종료도 처리한다. 나머지 오류 복구/tree builder의 전체 완료로 표시하지 않는다.
- 태그별 UA display 분류 뒤 모든 태그의 `hidden`을 적용해 hidden textarea/button/span의 테두리·텍스트가 잘못 그려지는 문제를 교정했다. author display는 여전히 UA hidden display를 덮어쓸 수 있다.
- entity 검색은 기존 최대 12문자 정책 안에서만 세미콜론을 찾는다. 긴 literal `&`에서 각 위치마다 남은 문서를 다시 훑는 비용을 제거했고, `&` 없는 텍스트와 출력 문자열 capacity도 최적화했다. 지원 entity 종류/기존 의미는 유지한다.

[HTML 토큰화 규칙](https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-state)을 대조하고 고정 WebView2로 기대값을 확인했다. 문서 ID·앱 이름 분기, 새 허용치·픽셀 예외는 없다.

독립 DOMParser script/NULL 사례 175개(고정 9·결정적 토큰 조합 160·NULL text context 6)와 fragment 6개·NULL-only fragment 6개, **187/187**을 두 DPI에서 확인했다. NULL-only RCDATA fragment는 자식 0개, raw fragment는 replacement text 자식 1개임도 확인했다. 원래 발견한 5개/1개 불일치와 수정 전 출력은 별도 보존했다. 직접 회귀는 script text·후속 형제·NULL/fragment·literal DOM·plaintext·숨김 크기와 author override를 검사한다.

새 30쌍의 CSSOM 크기 504개·필수 스타일 3,150개, 누락 0이다. 기존 비교 교정 182개와 새 실제 캡처의 정상 복제·1px 좌표·누락 크기·잘못된 스타일·이전 페인트·잘못된 DOM text 60개, **242/242**를 통과했다. [최종 감사](../TWebFrame2/tests/rendering/runs/20261004-script-parser-v16-final-verified/pixel-diagnostic-invariance.json)는 소스 233개 해시와 기존 318쌍 불변을 확인한다. 입력 v1·v2·v3와 변경 전/중간 실패는 보존했다. v2의 좁은 viewport에서 표준 가로 scrollbar 페인트만 달랐던 2쌍(122/1,284픽셀)은 실패로 보존했다. 최종 파서 입력 v3는 사각 사용자 CSS scrollbar 장식을 명시해 이 변수를 고정했고 스크롤 범위/DOM/크기는 계속 검사한다. 표준 scrollbar의 새 backend 예외나 허용치는 추가하지 않았다.

## 속도와 출력 확인

동일 standalone harness와 같은 CSS helpers에서 v15 저장 DOM.cpp와 v16 최종 DOM.cpp를 각각 컴파일했다. 파일 읽기·DOM 직렬화는 제외하고 1회 warmup+9회 측정, 각 workload에서 번갈아 3회씩 총 27회/variant를 기록했다. raw-N은 style/script N쌍, amp-N은 literal `&` N×100개다. 모든 DOM 출력 18쌍은 바이트까지 동일하다.

| workload | v15 중앙값 | v16 중앙값 |
| --- | ---: | ---: |
| raw-20 | 0.0696ms | 0.0695ms |
| raw-200 | 0.6840ms | 0.6883ms |
| raw-600 | 1.9352ms | 1.9652ms |
| amp-20 | 0.4209ms | 0.0147ms |
| amp-200 | 40.3581ms | 0.1393ms |
| amp-600 | 364.2672ms | 0.3863ms |

[파싱 속도·동일 DOM 증거](../TWebFrame2/tests/rendering/runs/20261004-script-parser-v16-final-verified/optimization-and-accuracy-evidence/parser-performance/summary.json)를 보존한다. 이 수치를 일반 페이지·스크롤 향상으로 확대하지 않는다.

같은 보존 앱 HTML/CSS/JS·Markdown과 공통 View 실행기로 번갈아 3회씩, 1600×1000 실제 96 DPI에서 측정했다. 앱 추가 재빌드는 하지 않았다.

| 항목 | v15 before | v16 after |
| --- | ---: | ---: |
| 초기 레이아웃 | 207.86ms | 214.53ms |
| 최초 페인트 | 110.78ms | 109.33ms |
| 스크롤 페인트 | 20.17ms | 19.85ms |
| 외부 포커스 후 첫 클릭 | 15.62ms | 15.47ms |
| 첫 thumb 드래그 | 16.89ms | 16.30ms |

앱 BGRA 12쌍·layout JSON 6쌍은 숨겨져야 할 `dirty-indicator`의 잘못된 표시 제거 외에는 불변이다. 원래 완전 불변 gate의 false 결과를 보존했다. 실제 앱 자산을 두 DPI의 별도 WebView2에서 읽어 hidden=true·display:none·rect/client=0을 확인했고, 모든 변경 픽셀이 이전 상자의 10px 글자 영역 안에만 있으며 제거 후 header 배경으로 돌아오는 것을 확인했다. 나머지 모든 상자/픽셀과 입력 3쌍은 동일하다. [교정된 출력 검증](../TWebFrame2/tests/rendering/runs/20261004-script-parser-v16-final-verified/optimization-and-accuracy-evidence/performance-final/verified-output-correction.json)을 보존한다. 첫 클릭/드래그 동기 페인트를 유지하며 증가한 항목도 표에 기록한다. 모든 동작의 속도 향상을 주장하지 않는다. [앱 성능과 해시](../TWebFrame2/tests/rendering/runs/20261004-script-parser-v16-final-verified/optimization-and-accuracy-evidence/performance-final/summary.json)를 따른다.

## 마지막 검증

- [x] 정확성·속도·비교 교정 이후 전체 회귀 12개와 platform integrity.
- [x] 불변 ZIP/baseline·모든 파일 해시·새 폴더 복원과 원본 120쌍 재판정.
- [x] 복원 실행기의 전체 348쌍 새 렌더링·별도 증거 ZIP 해시 검증.

주 ZIP 12,366개 파일·284,176,999 bytes와 복원 증거 ZIP 8,871개 파일·21,132,973 bytes의 모든 파일 해시를 확인했다.

- [주 ZIP](../TWebFrame2/tests/rendering/archives/20261004-script-parser-v16-final-verified.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-script-parser-v16-final-verified.json)
- [복원 판정](../TWebFrame2/tests/rendering/archives/20261004-script-parser-v16-final-verified-recovery-validation.json), [복원 증거 ZIP](../TWebFrame2/tests/rendering/archives/20261004-script-parser-v16-final-verified-recovery-evidence.zip), [증거 catalog](../TWebFrame2/tests/rendering/archives/20261004-script-parser-v16-final-verified-recovery-evidence.json)

주 ZIP SHA-256: `7130121A06F1A9A85910F54C9A1F715CC406239435C48245BE9D5C42F2737DFA`. 복원 증거 ZIP SHA-256: `675B8069B657A0E9C64F0A77F157853488FFA120CB2CF576DD4B583471979B8B`.

복원 소스 233개·원본 입력 20개와 실행기/DLL/글꼴 해시·120쌍 재판정을 확인했다. 불변 baseline은 원본 120 reference 인덱스로 별도 보존한다. 복원한 실행기·입력·계측/비교 소스로 348쌍의 모든 픽셀·전체 진단·raw 수치·판정을 재현했다. 주 ZIP 문서는 보관 직전 스냅샷이며 최종 완료는 별도 복원 JSON과 이 기록을 따른다.

Windows의 긴 경로 제한을 피하기 위해 전체 작업 원본을 [work-evidence.zip](../TWebFrame2/tests/rendering/runs/20261004-script-parser-v16-final-verified/optimization-and-accuracy-evidence/work-evidence.zip)에 묶었다. 내부 15,508개 파일을 [파일별 SHA 인덱스](../TWebFrame2/tests/rendering/runs/20261004-script-parser-v16-final-verified/optimization-and-accuracy-evidence/work-evidence-index.json)와 대조했고, 새 복원 폴더에서도 모든 내부 파일의 해시를 다시 확인했다. ZIP SHA-256은 `0E7C12F05E8DF17F7A14B5623A8D626A6857A924F42BEBA9F5EA209F02899F35`다. 일반 성능/요약 링크는 풀어서 보존했다. 실패한 중복 복사본과 disposable browser profile은 이 ZIP에서 제외하며 모든 원본 실패/입력/프로그램/소스/측정 자료는 포함한다.

v15 비교 318쌍·catalog/SHA 참조는 `before-evidence/v15/`, 이번 변경 전 소스/실행기·입력·실패·oracle·속도는 `optimization-and-accuracy-evidence/`에 보존한다. 기존 ZIP은 재귀 복사하지 않았다.

## 남은 전체 계약

전체 named entity/HTML 오류 복구·tree builder, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대·원격 영구 보관은 미완료다. 이번 5문서는 corpus 수량에 합산하지 않는다. v16의 고정 조합과 187개 oracle 성공을 전체 HTML 규칙의 완료로 표시하지 않는다.

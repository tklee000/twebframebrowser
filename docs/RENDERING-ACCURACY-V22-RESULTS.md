# v22 서식 재구성·adoption·속도 통합 결과

작성일: 2026-10-05. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**552/552 성공(477 strict·기존 승인 정책의 backend 차이 75)**. 새 고정 7문서 42쌍은 before 0/42에서 구조 42/42·strict 21/42로 개선됐다. 나머지 21쌍은 독립 전체 이미지 합성으로 원인을 확인한 정확한 CPU/GPU 서명이다. 기존 510쌍의 양쪽 픽셀·전체 DOM/스타일/기하·raw 수치·판정과 이전 예외 90개는 그대로다. 허용 오차 0을 유지한다. 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 552쌍 새 렌더링을 완료했다.

## 묶어서 구현·검증한 단계

1. 활성 서식 목록과 marker, 필요할 때의 서식 재구성을 공통 tree builder에 추가했다. clone은 원본 속성·inline style/priority를 보존하며 runtime 측정 캐시는 복사하지 않는다. 재구성 후 문서/부모 포인터·중복 child 여부도 확인한다.
2. 엇갈린 종료와 중간 block의 adoption agency를 bounded outer loop와 inner loop로 처리한다. 열린 스택/서식 목록 제거·삽입·bookmark·form pointer·scope prefix와 캐시 위치를 함께 갱신한다. 문서 ID별 분기는 없다.
3. 중첩 a/nobr, 같은 tag/namespace/속성의 Noah's Ark 제한, object/applet/marquee/caption/td/th marker와 scope 실패, raw/EOF·첫 문자·반복 document/div fragment 경계를 검증했다. 전체 table insertion mode와 foster parenting은 완료로 표시하지 않는다.
4. inline 아래 여러 단계의 inline을 지나 있는 block도 찾아 줄을 분리하고 마지막 inline 줄 뒤의 형제 텍스트를 이어 배치한다. solid 배경은 block을 포함한 큰 union 사각형 대신 실제 inline 조각에 칠한다. 복잡한 배경/gradient/image/shadow/border의 일반 split-inline 처리는 남아 있다.
5. 정상 current end tag의 빠른 pop과 활성 서식의 stack-index 캐시를 유지했다. 레이아웃의 in-flow block-descendant 여부·inline continuation·배경 조각을 캐시한다. 재레이아웃/스타일 변경의 무효화와 ancestor scroll translation을 함께 검증했다.

[WHATWG 활성 서식 목록](https://html.spec.whatwg.org/multipage/parsing.html#the-list-of-active-formatting-elements), [adoption agency](https://html.spec.whatwg.org/multipage/parsing.html#adoption-agency-algorithm), [in-body](https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody)를 대조하고 독립 고정 WebView2 결과로 확인했다.

독립 document/일반 div fragment oracle **1,444/1,444**가 두 DPI에서 exact DOM에 일치한다. before 206 통과·1,238 실패를 원본 그대로 보존했다. 이전 tokenizer/comment/body/implied/boundary oracle **9,050개**도 모두 불변이다. 매 파싱 결과의 ownerDocument·parent/children 일관성·중복 노드를 확인한다. 새 화면의 필수 스타일 1200개·CSSOM 크기 192개는 누락 0이다.

기존 비교기 교정 182개와 새 정상/DOM text·attribute/1px 위치/누락 기하/잘못된 스타일/1채널 1픽셀 paint 오류 주입 **294개**를 통과했다. backend 서명이 있는 화면에도 DOM·레이아웃·스타일 오류와 새 한 픽셀 차이를 허용하지 않는다. 두 DPI의 중첩 inline 스크롤·full relayout 픽셀 일치와 하위 display 변경/복원 후 흐름·페인트 복원을 별도 확인했다. 초기 좁은 테스트 상자의 정상 줄바꿈을 잘못 기대했던 실패 로그도 보존하고 충분한 폭으로 캐시 무효화 여부를 분리했다.

최초 최종 화면 552쌍은 통과했으나 새 complete 오류 주입 테스트가 원래 문서 ID 대신 mode 폴더명을 사용해 backend 서명을 찾지 못했다. 테스트 입력의 leaf 폴더에 원래 ID를 유지하도록 고치고 294개 교정을 먼저 재검증한 뒤 `final2`에서 전체 552쌍을 새로 수집했다. 첫 `final`의 원본 소스/픽셀/진단과 테스트 실패도 별도 보존한다.

## 정확한 CPU/GPU 차이 검증

7문서의 HTML/CSS·색을 수정하지 않았다. 별도 diagnostic 입력에서 글자만 투명하게 한 native/reference 배경이 전체 이미지에서 같고, 원본과 모든 text geometry가 같은지 먼저 확인했다. 원본 DOM/CSS의 ink와 native의 glyph/font/좌표·동일 LCD mask를 고정한 독립 실행기에서 CPU/GPU 합성만 바꿨다. **42/42 전체 CPU 이미지가 원래 native에, 전체 NVIDIA D3D11 이미지가 원래 reference에 픽셀 단위로 완전히 일치**했다. 비교 결과를 만들기 위해 native pixels를 교체하지 않는다.

차이가 있는 21개 조합만 기존 사용자 CPU/GPU 예외 정책으로 등록했다. 두 이미지의 decoded pixel SHA-256·raw differentPixels/maximumChannelDelta·문서/DPI/viewport·probe/analysis 해시를 묶었다. `-StrictPixels`에서는 원래 21개 `FAIL_PAINT`가 유지된다. 변경 전·중간·최종 strict 진단과 투명 배경 입력/결과·독립 소스/실행기는 모두 보존한다. 엔진은 CPU 합성을 유지한다.

초기 기본 reference adapter가 Intel로 선택되어 v21과 다름을 발견했다. 고정 NVIDIA GeForce GTX 1080 Ti, driver `32.0.15.8266`, 관측 LUID `0,56278`로 reference와 diagnostic probe를 고정했다. 새 graphics 진단 fingerprint는 `A1427B7E1016A54001B4A210D0B6B96B0D11870043EFCC1C2BDCC59BBCEC5AC8`이며 v21의 예전 진단 fingerprint와 다르다. 환경을 동일하다고 숨기지 않고 browser arguments를 저장하여 복원 실행에도 적용한다. 기존 510쌍의 실제 픽셀/JSON 불변을 별도로 검증한다.

[전체 이미지 독립 검증](C:/twf-v22/blend-proof/summary.json), [정확한 서명 등록 증거](C:/twf-v22/blend-proof/formatting-backend-verification.json).

## 같은 출력에서의 속도

동시 빌드/캡처 없이 보존한 동일 앱 HTML/CSS/JS·Markdown을 사용했다. parser 13 workload는 번갈아 3회씩, round당 warmup 1회·측정 9회로 variant당 27개 중앙값이다. 정상 서식 6,000개와 깊은 정상 서식의 workload를 추가했다. 앱은 before/after 3회와 after/before 3회의 순서를 균형 있게 측정했다. 첫 3회 summary도 보존했다.

parser DOM 39쌍·앱 BGRA 24쌍·layout JSON 12쌍·입력 6쌍이 같다. 최초 페인트는 감소했으나 초기 레이아웃·스크롤 등은 증가했다. 캐시로 반복 탐색을 제한했으며 모든 입력의 속도 향상을 주장하지 않는다. 앱 자체는 추가 재빌드하지 않고 공통 View 실행기로 측정했다.

| parser workload | v21 before ms | v22 after ms | 시간 변화 |
| --- | ---: | ---: | ---: |
| plain-2m | 2.2919 | 2.3491 | +2.5% |
| tags-6000 | 6.8725 | 6.5796 | -4.3% |
| attributes-6000 | 12.4401 | 12.5736 | +1.1% |
| comments-6000 | 10.2471 | 10.0241 | -2.2% |
| deep-div-128 | 5.3833 | 5.5934 | +3.9% |
| deep-div-512 | 5.3449 | 5.5098 | +3.1% |
| deep-div-1024 | 5.5568 | 5.3708 | -3.3% |
| nested-span-256 | 5.5473 | 5.5259 | -0.4% |
| explicit-list-6000 | 6.1533 | 6.5324 | +6.2% |
| explicit-forms-6000 | 11.2647 | 11.4398 | +1.6% |
| explicit-options-6000 | 6.3597 | 6.4431 | +1.3% |
| explicit-formatting-6000 | 13.7438 | 13.8044 | +0.4% |
| formatted-deep-span-256 | 3.8781 | 4.0376 | +4.1% |

| 앱 항목 | v21 before ms | v22 after ms | 시간 변화 |
| --- | ---: | ---: | ---: |
| 초기 레이아웃 | 210.64 | 218.20 | +3.6% |
| 최초 페인트 | 114.10 | 108.03 | -5.3% |
| 첫 클릭 동기 페인트 | 14.39 | 14.85 | +3.2% |
| 첫 드래그 동기 페인트 | 15.12 | 15.99 | +5.7% |
| 스크롤 페인트 | 19.80 | 20.55 | +3.8% |

[성능·출력 불변·실행기 해시](C:/twf-v22/performance-final/summary.json).

## 마지막 전체 회귀·보관·복원

- [x] 정확성·속도·오류 주입 이후 전체 회귀 12개·platform integrity.
- [x] 불변 ZIP/baseline·새 폴더 복원·소스/입력/실행기/DLL 해시·원본 120쌍 재판정.
- [x] 복원 실행기로 전체 552쌍 새 렌더링·별도 증거 ZIP 해시 검증.

주 ZIP `20261005-formatting-tree-v22-final2.zip`: 227,752,675 bytes, SHA-256 `C243281A67D099B89FA1E58A9D6AF40A403499E570D524E9A421D2BCFD89CD7E`. indexed 파일 20,018개·소스 329개·원본 입력 20개를 새 폴더에 복원하고 해시를 확인했다. 복원 비교 코드로 원본 120쌍을 재판정했다. **복원 실행기·DLL·입력·측정/비교 코드의 552쌍 새 렌더링**에서 전체 디코딩 픽셀·DOM/스타일/기하 JSON·raw 차이·판정이 모두 같다.

별도 복원 증거 ZIP은 29,265,822 bytes, SHA-256 `17D3A8410E0DBF37748982E283C31E3ACCC728A1E2EA412EC820357EBBEC7A85`이며 13,537개 entry를 다시 해시 검증했다. 주 ZIP의 nested work/v21 증거 ZIP도 모든 파일을 검증했다. 주 ZIP 문서는 보관 직전 스냅샷이며 최종 완료 증거는 [복원 검증](C:/twf-v22/archives/20261005-formatting-tree-v22-final2-recovery-validation.json)을 따른다.

## 남은 범위

전체 HTML insertion modes/table foster parenting·template contents·폼 소유자·특수 fragment context·DOCTYPE/PI·foreign-content, 실제 ruby 조판·일반 inline wrapping/gradient/image/shadow/border·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서·원격 영구 보관은 미완료다. corpus는 기존 20문서이며 이번 7문서는 별도 고정 교정이다. 대용량 증거는 `C:\twf-v22`에 보존한다.

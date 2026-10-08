# v19 주석·선언·HTML 데이터 정확성·속도 검증 결과

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**438/438 성공(384 strict·기존 승인 54)**이다. 새 고정 5문서 30쌍은 변경 전 0/30에서 최종 DOM·스타일·기하 일치 및 픽셀 차이 0으로 교정했다. 기존 v18의 408쌍은 양쪽 PNG·CDP·WM_PRINTCLIENT 픽셀, 전체 DOM/스타일/기하 JSON, raw 수치·판정이 불변이다. 새 backend 예외와 허용치 변경은 없다. 정확성·속도 검증 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 438쌍 새 렌더링까지 완료했다.

## 함께 수정한 다섯 단계

1. 주석 시작·dash·less-than/bang·종료 상태를 선형 scanner로 처리한다. `-->`와 `--!>` 종료, nested opener 복구, abrupt empty 종료를 지원하고 EOF에서는 상태에 보류한 `-`/`--`/`--!`를 데이터에 붙이지 않는다. 물리 NULL은 U+FFFD로 바꾸며 참조는 literal로 유지한다. CR/CRLF는 기존 입력 전처리를 따른다.
2. 알 수 없는 `<!...>`와 invalid end opener가 같은 bogus comment scanner를 사용한다. 첫 `>`에서 종료하며 quote는 종료를 보호하지 않는다. NULL 치환·EOF·literal 참조를 보존하고 ASCII case-insensitive DOCTYPE prefix는 기존 처리로 보낸다. 기존 DOCTYPE/quirks 전체 구현을 확장했다고 표시하지 않는다.
3. HTML DATA의 물리 NULL을 무시하고 빈 text node를 만들지 않는다. 숫자 참조 `&#0;`와 raw/RCDATA의 NULL은 U+FFFD를 유지한다. NULL을 무시한 뒤 `pre`/`listing` 첫 LF를 처리하여 고정 브라우저와 같은 결과를 만든다. 직접 DOM 값은 재토큰화하지 않는다.
4. 일반 SVG foreign DATA에서 `<![CDATA[`를 literal text로 읽어 첫 `]]>`까지 유지하며 EOF의 `]`도 보존한다. 일반 SVG NULL은 U+FFFD이고 SVG `title`/`desc`/`foreignobject` 통합점은 HTML 데이터·bogus 선언 동작을 사용한다. MathML과 전체 foreign tree builder는 범위 밖이다.
5. 새 SVG 입력에서 드러난 `hidden` namespace 오류도 교정했다. HTML hidden UA 규칙은 HTML에만 적용하고 inline SVG는 replaced viewport/크기/기준선을 유지한다. SVG text 채움·viewport clip을 기존 glyph 페인트 경로로 처리하고 단순 직접 text의 DOM range를 같은 글꼴·기준선으로 측정한다. Tspan/textPath·변환/viewBox range·stroke text는 검증 범위 밖이다. 주석 DATA와 SVG text 입력은 수정하지 않았다.

[WHATWG 주석 상태](https://html.spec.whatwg.org/multipage/parsing.html#comment-start-state), [bogus 선언](https://html.spec.whatwg.org/multipage/parsing.html#markup-declaration-open-state), [DATA/foreign tree 처리](https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inforeign)를 대조하고 고정 WebView2의 독립 결과로 경계를 확인했다. 문서 ID·앱별 분기는 없다. SVG 통합점 CDATA는 고정 WebView2에서 bogus comment로 나타난 실제 동작을 따른다. 최신 표준의 non-HTML adjusted-node 조건과의 전체 동등성을 주장하지 않는다. processing instruction, 전체 DOCTYPE·tree builder는 이번에 바꾸지 않았다.

## 독립 입력과 엄격 비교

고정 document/fragment oracle **5,350개를 두 DPI 모두 정확한 실제 자식 노드 구조까지 확인**했다. 작은 alphabet의 주석 조합을 전수 생성하고 긴 dash/nested 입력·malformed 선언·NULL/CR/LF·HTML/SVG 경계를 추가했다. 두 DPI의 reference도 서로 같다. 변경 전 exact DOM 출력 3,778개 불일치와 렌더링 30개를 보존했다. before/after serializer는 동일하며 노드를 합치거나 기대값을 완화하지 않는다.

fragment oracle은 v18에서 확인한 WebView2 `innerHTML` 빠른 경로 차이를 피하도록 제거되는 leading comment를 포함한다. 일반 tokenizer 경로를 선택한다는 설명은 입력별 결과에서 추정한 것이며 내부 경로를 직접 추적한 것은 아니다. DOMParser와 fragment의 최종 자식 노드를 각각 비교한다. 최초 수정의 84개 경계 불일치도 별도 원본으로 보존했다.

SVG NULL 치환으로 나타난 U+FFFD는 작성 글꼴에 glyph가 없을 때 설치된 Tahoma로 대체한다. 독립 WebView2 canvas advance 13.5966796875px와 설치 글꼴의 cmap/advance를 대조했고 최종 양쪽 range/페인트를 확인했다. 글꼴 선택은 이 출력과 glyph 보유 여부에서 확인한 것이며 browser 내부 fallback 호출을 직접 추적하지 않았다. [Chromium Windows fallback 코드](https://raw.githubusercontent.com/chromium/chromium/main/third_party/blink/renderer/platform/fonts/win/font_fallback_win.cc)는 정책 참고이며 이 글꼴 선택의 직접 증거로 인용하지 않는다. 별도 font oracle·글꼴 해시·각 수정의 원본 실패 캡처를 보존한다.

새 30쌍의 CSSOM 크기 336개·필수 스타일 2,100개와 누락 0을 확인했다. 기존 비교기 교정 182개와 새 캡처의 정상 복제·DOM text/attribute·1px 좌표·누락 크기·잘못된 스타일·1채널 1픽셀 paint 오류 주입 210개를 통과했다. DOM-only 수정의 before PNG를 paint 오류라고 가정하지 않고 실제 픽셀을 바꿔 검증한다. [최종 감사](C:/twf-v19/runs/20261004-comment-data-v19-final/pixel-diagnostic-invariance.json)는 소스 280개와 기존 408쌍 불변을 확인한다.

## 속도와 동일 출력

DATA에서 문자열을 한 번 추출하고 ampersand가 있을 때만 디코더를 호출한다. NULL이 없으면 치환/삭제를 생략한다. 주석은 일반 문자와 긴 dash 구간을 묶어 복사하여 상태 경계만 검사한다. 전체 문서를 반복 소문자로 만들거나 앞부분을 재탐색하지 않는다.

다른 캡처·빌드 없이 parser와 같은 보존 앱 자산을 before/after 번갈아 3회씩 단독 측정했다. parser는 1회 warmup+9회 측정, variant당 27개 표본의 중앙값이며 파일 읽기·DOM 직렬화는 시간에서 제외한다. plain/span/dashes는 약 0.2M 또는 2M 문자, tags/attributes/comments는 반복 요소 수다. **동일 입력의 parser DOM 27쌍은 바이트까지 같고 앱 BGRA 12쌍·layout JSON 6쌍·입력 3쌍도 같다.** 앱 추가 재빌드는 하지 않았다.

| workload | v18 before ms | v19 after ms | 감소 |
| --- | ---: | ---: | ---: |
| plain-200k | 0.1640 | 0.1831 | -11.6% |
| plain-2m | 2.4817 | 2.2882 | 7.8% |
| tags-6000 | 6.2792 | 6.4389 | -2.5% |
| attributes-6000 | 12.2565 | 11.9837 | 2.2% |
| comments-2000 | 2.9654 | 2.8895 | 2.6% |
| comments-6000 | 9.5818 | 9.5634 | 0.2% |
| comment-span-200k | 0.0928 | 0.1337 | -44.1% |
| comment-span-2m | 1.2851 | 1.7312 | -34.7% |
| comment-dashes-2m | 4.1434 | 2.3442 | 43.4% |

| 앱 항목 | v18 before ms | v19 after ms |
| --- | ---: | ---: |
| 초기 레이아웃 | 207.54 | 204.20 |
| 최초 페인트 | 116.64 | 107.67 |
| 첫 클릭 동기 페인트 | 14.41 | 15.46 |
| 첫 드래그 동기 페인트 | 16.14 | 16.65 |
| 스크롤 페인트 | 19.99 | 20.38 |

음수 감소율과 앱 항목의 증가도 함께 기록한다. parser 일부 입력의 개선을 모든 페이지·페인트 속도 향상으로 확대하지 않는다. [성능 원본·실행기 해시](C:/twf-v19/performance-final2/summary.json)를 따른다.

## 마지막 검증과 보존

- [x] 정확성·속도·비교기 교정 이후 전체 회귀 12개와 platform integrity.
- [x] 불변 ZIP/baseline·파일 해시·새 폴더 복원과 원본 120쌍 재판정.
- [x] 복원 실행기의 전체 438쌍 새 렌더링·별도 증거 ZIP 해시.

주 ZIP `20261004-comment-data-v19-final.zip`은 142,734,296 bytes, SHA-256 `32B6F0FC54121775DE3345AC698DA9C993C55C08AF5E368F794614C9A136E575`이다. indexed 파일 15,607개, 소스 280개, 원본 입력 20개를 복원해 해시를 확인했다. 복원한 비교 코드로 원본 120쌍을 재판정하고, 복원한 실행기·DLL·측정/비교 소스로 전체 **438쌍을 새로 렌더링**했다. 양쪽 PNG/CDP/WM_PRINTCLIENT 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정이 모두 같다.

별도 복원 증거 ZIP은 24,200,855 bytes, SHA-256 `DDF398C7C2D52474F538171FE637870E367D908641F609733902BF320FB051A6`이다. 주 ZIP·복원 증거 ZIP의 모든 indexed entry를 풀어 해시를 확인했으며 nested work/v18 ZIP도 확인했다. 주 ZIP의 문서는 보관 직전 스냅샷이며 최종 완료 증거는 [복원 검증](C:/twf-v19/archives/20261004-comment-data-v19-final-recovery-validation.json)을 따른다.

대용량 자료는 D 드라이브 여유 공간을 고려해 `C:\twf-v19`에 저장한다. 이전 v18 실행·ZIP과 작업 증거는 보존한다.

## 남은 계약

전체 HTML tree builder·DOCTYPE/PI/foreign-content와 나머지 tokenizer 오류 복구, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대와 원격 영구 보관은 미완료다. 이번 5문서는 corpus 수량에 합산하지 않는다.

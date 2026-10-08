# v21 폼·생략 종료·스택 처리 통합 결과

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**510/510 성공(456 strict·기존 승인 54)**. 새 고정 6문서 36쌍은 보존된 v20 before 0/36에서 strict 36/36으로 개선됐다. 기존 474쌍의 양쪽 픽셀·전체 DOM/스타일/기하·raw 수치·판정은 불변이다. 새 예외와 허용치 변경은 없다. 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 510쌍 새 렌더링을 완료했다.

## 함께 진행한 단계

1. 중첩 form 시작은 현재 paragraph를 닫거나 새 노드를 만들지 않고 무시한다. form pointer는 다른 종료 태그로 스택에서 제거돼도 유지하며, scope 실패를 포함한 form 종료에서 해제한다. 종료는 생략 가능한 항목을 정리한 뒤 form만 스택에서 제거해 열린 자식을 유지한다.
2. 중간 스택 제거 후 기존 paragraph/general/button/list-item prefix를 앞에서부터 갱신한다. select도 general scope 경계에 포함하고, select/ruby/template 존재 상태는 기존 Entry의 unsigned 빈 공간에 저장한다. 반복 fragment parsing·scope 실패 후 재시작·깊이 128/256 제거·SVG 통합점까지 독립 검사한다. form ancestor가 있는 특수 fragment context·form-associated ownership·template contents는 미완료다.
3. option/optgroup·중첩 select·input에 따른 select 종료·hr와 ruby rb/rtc/rp/rt 생략 종료를 공통 처리한다. foreign 종료 태그가 HTML 상위에서 재처리될 때 SVG 통합점 스코프를 유지하고 `foreignObject`의 실제 DOM localName/tagName을 canonical qualifiedName으로 노출한다. 전체 foreign-content 지원으로 표시하지 않는다.
4. 데이터/type 없는 object의 fallback 콘텐츠를 일반 흐름으로 측정하고 UA clip을 적용한다. option nowrap·optgroup 내부 label 행, appearance:none select의 저자 line-height와 inline baseline을 교정한다. option 표시 텍스트는 내부 label 레이아웃으로 만들고 원본 DOM textContent를 유지하여 내부 label과 DOM Range 좌표를 구분한다. 닫힌 menu select는 선택한 label을 그리고 DOM의 popup 자식 스타일 조회와 화면 출력을 구분한다. 실제 ruby 조판 대신 명시적 block CSS를 사용하는 교정이다.
5. matching current HTML end tag는 분류와 상위 탐색 없이 pop하고 form은 pointer 경로를 유지한다. 무시하는 nested form token은 노드·속성 저장/디코딩을 생략하며 EOF pending token의 mutation도 생략한다. 일반 입력의 반복 optional-start 비교는 공유 분류 flags로 제한한다.

[WHATWG in-body](https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody), [scope](https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-scope), [Chromium UA stylesheet](https://raw.githubusercontent.com/chromium/chromium/main/third_party/blink/renderer/core/html/resources/html.css)를 대조하고 고정 WebView2 결과로 검증했다. 문서 ID·앱별 분기는 없다.

독립 document/일반 div fragment oracle **918/918**가 두 DPI 모두 exact child structure에 일치한다. before 594 통과·324 실패를 보존했다. 추가 select/form/cache/SVG 경계 **166/166**, before 50 통과도 원본과 함께 보존했다. 이전 v18/v19/v20 oracle **7,966개 전체 불변**. leading comment를 제거한 fragment tokenizer 경로를 사용하며 결과의 오류를 정규화로 지우지 않는다.

새 36쌍은 CSSOM 크기 624개·필수 스타일 3900개, 누락 0이다. 기존 비교기 교정 182개와 새 정상/DOM text·attribute/1px 위치/누락 기하/잘못된 스타일/1채널 1픽셀 paint 오류 주입 252개가 통과한다. 최초 18/36·후속 24/36·30/36, 픽셀 일치 후 남았던 option DOM Range 차이와 경계 검사 중간 실패 원본을 보존한다.

## 같은 출력에서의 속도

다른 캡처·빌드 없이 보존 앱 자산과 같은 입력을 before/after 번갈아 3회씩 단독 실행했다. parser는 warmup 1회+측정 9회, variant당 27개 표본의 중앙값이며 파일 읽기/직렬화는 시간에서 제외한다. 기존 9 workload에 올바르게 닫힌 form·option 6,000개씩을 추가했다. 앱 초기 레이아웃은 첫 3회 중앙값 207.13→222.91ms와 큰 실행 편차를 보여 순서를 뒤집은 after/before 3회를 추가했다. 최초 측정은 `summary-initial3.json`에 보존하고 아래 앱 표는 variant당 6회 중앙값을 사용한다.

parser DOM **33쌍 바이트 불변**, 앱 BGRA 24쌍·layout JSON 12쌍·입력 6쌍도 같다. 동일 보존 HTML/CSS/JS·Markdown을 공통 View에서 측정했고 MdViewer 앱은 추가 재빌드하지 않았다. 증가는 그대로 기록하며 모든 페이지·페인트의 속도 개선을 주장하지 않는다.

| workload | v20 before ms | v21 after ms | 감소 |
| --- | ---: | ---: | ---: |
| plain-2m | 2.2838 | 2.2941 | -0.5% |
| tags-6000 | 6.6275 | 6.5188 | 1.6% |
| attributes-6000 | 12.7290 | 12.3047 | 3.3% |
| comments-6000 | 9.6066 | 9.6893 | -0.9% |
| deep-div-128 | 5.5746 | 5.3196 | 4.6% |
| deep-div-512 | 5.4818 | 5.1672 | 5.7% |
| deep-div-1024 | 5.5297 | 5.4048 | 2.3% |
| nested-span-256 | 5.3818 | 5.2711 | 2.1% |
| explicit-list-6000 | 6.1471 | 5.9927 | 2.5% |
| explicit-forms-6000 | 11.3354 | 11.0584 | 2.4% |
| explicit-options-6000 | 6.1443 | 6.1573 | -0.2% |

| 앱 항목 | v20 before ms | v21 after ms |
| --- | ---: | ---: |
| 초기 레이아웃 | 205.85 | 215.90 |
| 최초 페인트 | 111.11 | 105.79 |
| 첫 클릭 동기 페인트 | 14.72 | 15.55 |
| 첫 드래그 동기 페인트 | 15.83 | 17.13 |
| 스크롤 페인트 | 20.18 | 20.40 |

[측정 원본·실행기 해시](C:/twf-v21/performance-final/summary.json).

## 마지막 전체 회귀·보관·복원

- [x] 정확성·속도·비교기 이후 전체 회귀 12개·platform integrity.
- [x] 불변 ZIP/baseline·새 폴더 복원·소스/입력/실행기/DLL 해시·원본 120쌍 재판정.
- [x] 복원 실행기의 전체 510쌍 새 렌더링·별도 증거 ZIP 검증.

주 ZIP `20261004-implied-tree-v21-final.zip`: 163,718,586 bytes, SHA-256 `B7C30560B0A406B5E0A6F398E74F8570EF39965303559FC9BA144FD1B453CC55`. indexed 파일 18,159개·소스 310개·원본 입력 20개를 새 폴더에서 복원하고 해시 확인했다. 복원 비교 코드로 원본 120쌍을 재판정하고, **복원 실행기·DLL·입력·측정/비교 소스로 510쌍을 새로 렌더링**했다. 양쪽 PNG/CDP/WM_PRINTCLIENT 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정은 최종 실행과 같다.

별도 복원 증거 ZIP: 27,745,233 bytes, SHA-256 `199439BF2DCA1AED2BE1BC2A3813076176A28A4AF8B5A44F24DF3F41D73A877A`. 주/증거 ZIP의 모든 indexed entry와 nested work/v20 증거 ZIP도 해시 검증했다. 주 ZIP 문서는 보관 직전 스냅샷이며 최종 완료 증거는 [복원 검증](C:/twf-v21/archives/20261004-implied-tree-v21-final-recovery-validation.json)을 따른다.

## 남은 계약

전체 HTML insertion modes·active formatting/adoption agency·table foster parenting·template contents·폼 소유자·특수 fragment context·DOCTYPE/PI·foreign-content, 실제 ruby 조판·일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI·독립 화면 캡처, 100→1,000문서·원격 영구 보관은 미완료다. corpus는 20문서이며 이번 6문서는 별도 고정 교정이다. 대용량 증거는 `C:\twf-v21`에 보존한다.

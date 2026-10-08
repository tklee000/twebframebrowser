# v20 본문 트리 복구·스코프·속도 통합 결과

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**474/474 성공(420 strict·기존 승인 54)**. 새 고정 6문서 36쌍은 before 0/36에서 최종 strict 36/36으로 개선됐다. 기존 438쌍의 양쪽 픽셀·전체 DOM/스타일/기하·raw 수치·판정은 불변이며 새 예외와 허용치 변경은 없다. 정확성·속도 작업 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 474쌍 새 렌더링을 완료했다.

## 함께 완료한 단계

1. paragraph 자동 닫힘에 center/details/dialog/dir/figure/figcaption/search/summary/listing·li/dt/dd·xmp/plaintext를 포함하고, button/general 스코프 밖 문단을 닫지 않는다. stray `</p>`는 빈 p를 생성하며 `</br>`는 br로 처리한다. pending EOF token의 tree mutation은 계속 생략한다.
2. 생략된 li/dt/dd 종료, 중첩 제목 시작·다른 제목 이름의 종료, 중첩 button과 일반 종료 태그의 special 경계를 공통 처리한다. ul/ol 안 잘못된 li 종료는 바깥 목록 항목을 닫지 않는다. SVG 통합점의 namespace 스코프를 유지한다.
3. 열린 paragraph와 general/button/list-item 경계를 parser-local prefix 상태로 캐시하고 pop 시 이전 상태를 복원한다. 토큰의 분류를 공유해 같은 태그를 재분류하지 않는다. paragraph가 없는 깊은 block마다 상위 stack 전체를 다시 걷지 않는다.
4. block button의 자동 폭과 블록 자식이 있는 버튼의 정상 흐름 높이를 교정한다. block을 가로지르는 inline 요소의 실제 조각 경계를 레이아웃 때 캐시하고 상대 offset으로 보존한다. DOM 기하·실제 View JavaScript 기하가 같은 `ReadElementRect`를 사용한다. scroll·restyle·재활성화 회귀는 두 DPI에서 검사한다.

[WHATWG in-body 규칙](https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody)과 [scope 정의](https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-scope)를 대조했다. 문서 ID나 앱별 분기는 없다. 전체 insertion mode·adoption agency·table foster parenting을 구현했다고 표시하지 않는다.

독립 WebView2 DOMParser 문서/일반 tokenizer fragment oracle **1596/1596**가 두 DPI 모두 일치한다. 변경 전 통과 492개·불일치 1104개와 원본 캡처를 보존했다. 추가 SVG 통합점 종료·형제·본문 꼬리 oracle 32개도 두 DPI에서 일치한다. 이전 v18 988개·v19 5350개도 정확한 child node 구조를 유지한다. fragment oracle은 leading comment를 제거한 뒤 실제 노드 구조를 비교하며 파서 오류를 정규화로 지우지 않는다.

새 36쌍의 CSSOM 크기 744개·필수 스타일 4650개, 누락 0. 기존 비교기 교정 182개와 새 정상/DOM text/attribute/1px 위치/누락 크기/잘못된 스타일/1채널 1픽셀 paint 오류 주입 252개를 통과했다. 최초 focused 24/36·후속 36/36과 최종 캐시 실행 자료를 보존한다. SVG title/desc 종료 경계의 중간 실패 32개를 추가 독립 검사로 확인해 교정한 뒤 final3 소스 snapshot으로 다시 실행했다. 중단한 final/final2 실행과 원본 실패도 보존한다.

## 속도와 동일 출력

다른 캡처·빌드 없이 동일 입력과 보존 앱 자산을 before/after 번갈아 3회씩 단독 실행했다. parser는 1회 warmup+9회 측정, variant당 27개 표본의 중앙값이며 파일 읽기·DOM 직렬화는 시간에서 제외한다. deep-div 입력은 깊이 128×64·512×16·1024×8로 각 8,192개 div를 포함한다. nested-span은 깊이 256×32이며 명시적 종료 목록은 li 6,000개다.

**parser DOM 27쌍은 바이트까지 같고 앱 BGRA 12쌍·layout JSON 6쌍·입력 3쌍도 같다.** 같은 보존 HTML/CSS/JS·Markdown을 공통 View 실행기에서 측정했고 MdViewer 앱은 추가 재빌드하지 않았다.

| workload | v19 before ms | v20 after ms | 감소 |
| --- | ---: | ---: | ---: |
| plain-2m | 2.2932 | 2.2672 | 1.1% |
| tags-6000 | 6.4964 | 6.6417 | -2.2% |
| attributes-6000 | 11.7500 | 12.3138 | -4.8% |
| comments-6000 | 9.6703 | 9.5908 | 0.8% |
| deep-div-128 | 5.5714 | 5.3205 | 4.5% |
| deep-div-512 | 6.2498 | 5.3075 | 15.1% |
| deep-div-1024 | 7.5206 | 5.2963 | 29.6% |
| nested-span-256 | 5.6324 | 5.3278 | 5.4% |
| explicit-list-6000 | 5.9974 | 6.0226 | -0.4% |

| 앱 항목 | v19 before ms | v20 after ms |
| --- | ---: | ---: |
| 초기 레이아웃 | 208.69 | 201.09 |
| 최초 페인트 | 107.18 | 107.88 |
| 첫 클릭 동기 페인트 | 15.81 | 14.85 |
| 첫 드래그 동기 페인트 | 16.53 | 15.90 |
| 스크롤 페인트 | 19.70 | 20.32 |

음수 감소율과 앱 항목의 증가도 그대로 기록한다. 깊은 tree 파싱 개선을 모든 페이지·페인트의 속도 개선으로 확대하지 않는다. [측정 원본·실행기 해시](C:/twf-v20/performance-final/summary.json)를 따른다.

## 마지막 회귀·보관·복원

- [x] 정확성·성능·비교기 검증 이후 전체 회귀 12개·platform integrity.
- [x] 불변 ZIP/baseline·새 폴더 복원·소스/입력/실행기/DLL 해시·원본 120쌍 재판정.
- [x] 복원 실행기의 전체 474쌍 새 렌더링·별도 복원 증거 ZIP 검증.

주 ZIP `20261004-body-tree-v20-final3.zip`: 162,109,369 bytes, SHA-256 `5D6B74F9EDA2E8CDCA5273BE5E68435DE05EF21A35A59740F5049C92C8710F41`. indexed 파일 17,334개·소스 295개·원본 입력 20개를 새 폴더에서 복원하고 해시를 확인했다. 복원 비교 코드로 원본 120쌍을 재판정하고, 복원 실행기·DLL·입력·측정/비교 소스로 **474쌍을 새로 렌더링**했다. 양쪽 PNG/CDP/WM_PRINTCLIENT 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정이 같다.

별도 복원 증거 ZIP: 26,109,083 bytes, SHA-256 `598547A5B8279D7777734654EA59438D65D519C83A18729F8D0219D89A6109CD`. 주/증거 ZIP의 모든 indexed entry와 nested work/v19 증거 ZIP을 해시 검증했다. 주 ZIP 문서는 보관 직전 스냅샷이며 최종 완료 증거는 [복원 검증](C:/twf-v20/archives/20261004-body-tree-v20-final3-recovery-validation.json)을 따른다.

## 남은 계약

전체 HTML tree builder·active formatting/adoption agency·table foster parenting·DOCTYPE/PI/foreign-content, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI·독립 화면 캡처, 100→1,000문서 확대·원격 영구 보관은 미완료다. 이번 6문서는 corpus 수량에 합산하지 않는다. 대용량 증거는 `C:\twf-v20`에 보존한다.

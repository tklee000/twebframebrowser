# v18 HTML 태그·속성 토큰화 정확성·속도 검증 결과

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**408/408 성공(354 strict·기존 승인 54)**이다. 새 고정 5문서 30쌍은 변경 전 0/30에서 최종 DOM·스타일·기하 일치 및 픽셀 차이 0으로 교정했다. 기존 v17의 378쌍은 native/reference/CDP/WM_PRINTCLIENT 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정이 불변이다. 새 backend 예외와 허용치 변경은 없다. 정확성·속도 검증 이후 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원 및 복원 실행기의 408쌍 새 렌더링까지 완료했다.

## 함께 수정한 네 단계

1. 태그와 속성의 이름을 각각의 HTML 구분자까지 읽고 ASCII 대문자만 소문자로 바꾼다. 이름 안 구두점·비ASCII 문자는 보존하며 물리 NULL은 U+FFFD로 바꾼다. tokenizer 공백은 space/tab/LF/FF뿐이고 물리 CR/CRLF는 기존 입력 전처리를 따른다. Unicode space와 vertical tab을 속성 값의 구분자로 처리하지 않는다.
2. quoted/unquoted 값·첫 `=`가 포함된 잘못된 이름·quote/`<`가 들어간 이름·quote 뒤 missing whitespace·unexpected slash를 복구한다. unquoted 값 안 quote/`<`/backtick/`=`/slash는 literal이다. 물리 NULL을 치환한 뒤 v17 문자 참조를 한 번만 디코딩하며 중복 속성은 첫 값을 유지한다.
3. 시작·종료 태그 모두 `>`까지 완성돼야 DOM에 적용한다. pending token이 EOF에서 끝나면 폐기하고 paragraph 종료/암시적 tbody 같은 tree mutation도 생략한다. 정상 종료 태그의 quoted `>`를 보존하고 invalid end opener를 bogus comment로 만든다. script/RAWTEXT/RCDATA 종료 태그가 같은 무할당 attribute scanner를 사용해 malformed 상태도 일치한다.
4. 무시된 토큰이나 invalid opener로 나뉜 문자 구간은 인접 DOM text node에 이어 붙인다. 새로운 text node 할당을 줄이고 `</>` 뒤 정확한 노드 구조를 유지한다. 직접 DOM 값은 재토큰화하지 않는다.

[WHATWG 태그 이름](https://html.spec.whatwg.org/multipage/parsing.html#tag-name-state), [속성 상태](https://html.spec.whatwg.org/multipage/parsing.html#before-attribute-name-state), [EOF/종료 opener](https://html.spec.whatwg.org/multipage/parsing.html#end-tag-open-state)를 대조했다. 문서 ID·앱별 분기는 없다. 전체 tree builder나 전체 tokenizer 상태를 구현했다고 표시하지 않는다.

독립 DOMParser와 div fragment oracle **988개를 두 DPI 모두 정확한 최종 자식 노드 구조까지 확인**했다. 고정 입력의 이름/값/구분자/NULL/EOF 및 raw 종료 상태를 포함한다. 변경 전 normalized DOM 출력의 686개 불일치와 새 렌더링 before 30개를 보존했다. 변경 전 보조 실행기는 인접 text token을 합쳐 직렬화하며 최종 실행기는 실제 자식 노드를 그대로 직렬화한다. strict 렌더링 비교기도 노드를 합치거나 기대값을 완화하지 않는다.

최초 916개 비교에서 WebView2의 주석 없는 `innerHTML` 경로가 유효한 `data-v="&notit;"`를 `¬it;`로 바꾸는 한 사례를 확인했다. DOMParser 문서 경로는 `&notit;`를 유지했고, 앞에 주석을 넣어 일반 tokenizer를 거친 fragment 경로도 동일했다. isolated 반복 10개·양쪽 경로 원본 출력을 보존했다. 빠른 fragment parser 경로의 차이로 추정하지만 내부 실행 경로를 직접 추적한 것은 아니다. 최종 fragment oracle에는 독립 leading comment를 넣은 후 제거하며 DOMParser의 표준 토큰화 결과와 일치함을 확인한다. 이 WebView2 fast-path 차이는 새 렌더링 예외로 등록하지 않았고 전체 browser parity 완료로 표시하지 않는다.

새 30쌍은 CSSOM 크기 360개·필수 스타일 2,250개, 누락 0이다. 기존 비교기 교정 182개와 새 캡처의 정상 복제·DOM text/attribute·1px 좌표·누락 크기·잘못된 스타일·before 페인트 오류 주입 210개를 통과했다. [최종 감사](C:/twf-v18/runs/20261004-tokenizer-v18-final/pixel-diagnostic-invariance.json)는 소스 267개 해시와 기존 378쌍 불변을 확인한다.

## 속도와 출력

ASCII 스캔으로 locale 분류/소문자 변환을 제거하고 이름/값을 move로 넣는다. 기존에도 중복 속성의 entity 디코딩은 생략했으며, 새 scanner는 그 값의 임시 문자열 복사와 치환도 생략한다. 일반 값에 ampersand가 없으면 디코더의 반환 복사도 생략한다. raw 종료 태그도 같은 scanner의 이름/값 할당 없는 경로를 사용한다. 각 입력에서 1회 warmup+9회 측정하고 before/after를 번갈아 3회씩 실행했다. 최종 표본은 각 variant 27개이며 파일 읽기/DOM 직렬화를 시간에서 제외했다. 최초 탐색 성능 측정은 캡처와 겹쳤으므로 보존만 하고 아래 표는 다른 캡처·빌드 없이 단독 실행한 최종 측정이다.

`tags-N`은 단순 div N개, `attributes-N`은 속성 8개 div N개, `duplicates-N`은 뒤 중복 속성에 긴 ignored 값이 있는 div N개다. **같은 입력의 parser DOM 27쌍은 바이트까지 동일**하다.

| workload | v17 before ms | v18 after ms | 감소 |
| --- | ---: | ---: | ---: |
| tags-200 | 0.1985 | 0.1898 | 4.4% |
| attributes-200 | 0.4690 | 0.3821 | 18.5% |
| duplicates-200 | 0.2997 | 0.2622 | 12.5% |
| tags-2000 | 2.1493 | 1.9879 | 7.5% |
| attributes-2000 | 4.8808 | 4.0789 | 16.4% |
| duplicates-2000 | 3.0832 | 2.6898 | 12.8% |
| tags-6000 | 6.6219 | 6.1852 | 6.6% |
| attributes-6000 | 14.7457 | 12.3616 | 16.2% |
| duplicates-6000 | 9.7340 | 8.5832 | 11.8% |

속성 다량 입력의 파싱은 약 16~19% 감소했다. 이 수치를 일반 페이지·페인트 속도 향상으로 확대하지 않는다. 보존한 앱 HTML/CSS/JS·Markdown을 같은 View 실행기에서 번갈아 3회씩, 1600×1000 실제 96 DPI로 확인했다. 앱 추가 재빌드는 하지 않았다. **BGRA 12쌍·layout JSON 6쌍·입력 3쌍은 동일**하다.

| 앱 항목 | v17 before ms | v18 after ms |
| --- | ---: | ---: |
| 초기 레이아웃 | 201.37 | 202.35 |
| 최초 페인트 | 106.33 | 118.72 |
| 첫 클릭 동기 페인트 | 15.59 | 15.46 |
| 첫 드래그 동기 페인트 | 16.68 | 15.69 |
| 스크롤 페인트 | 19.96 | 20.42 |

최초 페인트 106.33→118.72ms·스크롤 19.96→20.42ms의 증가를 함께 기록한다. 이번 수정이 모든 앱 동작의 속도를 개선했다고 주장하지 않는다. [성능 원본·실행기 해시](C:/twf-v18/performance-final/summary.json)를 따른다.

## 마지막 검증과 보존

- [x] 정확성·속도·비교 교정 이후 전체 회귀 12개와 platform integrity.
- [x] 불변 ZIP/baseline·파일 해시·새 폴더 복원과 원본 120쌍 재판정.
- [x] 복원 실행기의 전체 408쌍 새 렌더링·별도 증거 ZIP 해시.

주 ZIP `20261004-tokenizer-v18-final.zip`은 121,637,987 bytes, SHA-256 `D24368E9A24323FD26820113CAD1D90FD09320DAAC1A184DF6E62B2D72EA0F4D`이다. 복원 파일 14,913개·소스 267개·원본 입력 20개를 확인했다. 복원한 비교 코드의 원본 120쌍 재판정과 복원 실행기의 전체 **408쌍 새 렌더링**에서 모든 양쪽 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정이 같다.

별도 복원 증거 ZIP은 23,249,365 bytes, SHA-256 `0159003DC62D342429195E705C0A99CF989BDEDAE54C4EDAFB4704FC2F165348`이다. 주/복원 증거 ZIP의 모든 indexed entry를 풀어 해시를 확인했으며 nested work/v17 ZIP도 별도로 확인했다. 주 ZIP 안 문서는 보관 직전 스냅샷이다. 최종 완료 증거는 [복원 검증](C:/twf-v18/archives/20261004-tokenizer-v18-final-recovery-validation.json)을 따른다.

새 대용량 자료는 D 드라이브 여유 공간을 고려해 `C:\twf-v18`에 저장한다. 기존 v17 실행·ZIP과 이전 증거는 보존한다. 초기 보조 캡처의 runtime license 누락은 렌더링 전 실패했으며 원래 로그와 후속 정상 실행을 모두 보존했다.

## 남은 계약

전체 HTML tree builder·주석/DOCTYPE/CDATA/foreign-content 및 나머지 tokenizer 오류 복구, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI/독립 화면 캡처, 100→1,000문서 확대와 원격 영구 보관은 미완료다. 이번 5문서는 corpus 수량에 합산하지 않는다.

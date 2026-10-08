# v23 표 오류 복구·자동 너비·caption·속도 통합 결과

작성일: 2026-10-05. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**594/594 성공(519 strict·기존 검증된 backend 차이 75)**. 새 고정 7문서 42쌍은 before 0/42에서 strict 42/42로 개선됐다. 기존 552쌍의 양쪽 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정은 모두 그대로다. 예외 registry 전체 111개와 허용 오차 0도 불변이며 새 예외는 없다. 마지막 전체 회귀 12개·platform integrity → 불변 보관·새 폴더 복원 → 복원 실행기의 594쌍 새 렌더링까지 완료했다.

## 함께 진행한 단계

1. 연속 table 문자 버퍼의 ASCII 공백 판정과 foster parenting을 구현했다. 일반 텍스트·요소·서식 재구성과 adoption이 같은 조정된 삽입 위치를 사용한다. 표 앞의 기존 텍스트 노드를 합치며 comment는 현재 부모에 유지한다. `&nbsp;`·다른 Unicode 공백·NULL·잘못된 `<`·무시된 form·EOF 경계도 검증한다.
2. table/section/row/cell/caption/column 상태를 스택 prefix로 관리한다. table·tbody·thead·tfoot·tr·colgroup·caption·td·th의 innerHTML 문맥은 실제 context를 열린 스택에 넣지 않고 fake root에서 mode를 초기화한다. 생략된 tbody/tr, 다음 셀·행·그룹을 시작할 때의 복구, 잘못된 종료와 table scope, 중첩 표·form pointer/hidden input·select의 표 경계를 처리했다. caption/cell의 서식 marker도 같이 닫는다. 문서 ID별 분기는 없다.
3. 실제 화면에서 드러난 자동 표 너비와 caption 누락을 공통 수정했다. intrinsic cell/column contribution과 기존 측정 캐시로 자동 너비를 정하고 top caption의 높이·배치·흐름을 반영한다. 새 입력의 HTML/CSS를 통과용으로 수정하지 않았다. 두 DPI에서 viewport 변경과 caption 스타일 변경/복원 후 크기·다음 형제 위치를 별도 검증한다.
4. 일반 텍스트/요소의 append 빠른 경로와 일반 end tag의 불필요한 추가 분류를 줄였다. 표 모드·scope·clear-to-context의 prefix 캐시를 사용하며 모든 prefix를 크기 제한을 검사하는 compact index로 보존한다. 첫 구현과 중간 compact 구현의 더 느린 측정도 별도로 보존했다.

[WHATWG table tree builder](https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intable)와 [CSS 표 너비/caption 규칙](https://www.w3.org/TR/CSS22/tables.html#auto-table-layout)을 대조했다. 실제 결과는 고정 WebView2를 독립 oracle로 수집했다.

## 정확성·비교기

독립 document/일반 div fragment **856개**가 96/144 DPI에서 exact DOM에 모두 일치한다. before 146 통과·710 실패를 보존했다. 기존 tokenizer/comment/body/implied/formatting oracle **10,494개**도 불변이다. 매 파싱 결과의 ownerDocument·parent/children·중복 노드를 확인했다.

표 fragment 문맥 9종 **306개**도 두 DPI에서 독립 WebView2와 일치하여 새 DOM 검증은 총 **1,162개**다. 이전 CLI에는 context mode가 없어 이 306개의 native before 점수는 수집하지 않았다. 첫 `final`의 594쌍은 모두 통과했으나 마지막 전체 회귀에서 JavaScript가 만든 tr의 innerHTML 셀 1개가 실패했다. 문맥 전달과 fake-root의 종료/scope/column 공백 경계를 고치고 `final2`에서 정확성·성능·전체 캡처를 다시 수행했다. 첫 최종 소스·캡처·실패 로그도 보존한다.

새 화면 필수 스타일 1650개·CSSOM 크기 264개는 누락 0, 문자 기하와 두 reference capture route도 일치한다. 기존 비교기 교정 182개에 새 정상/DOM text·attribute/1px 위치/누락 기하/잘못된 스타일/1채널 1픽셀 paint 오류 주입 **294개**가 통과했다.

reference는 v22와 동일한 NVIDIA GeForce GTX 1080 Ti, driver `32.0.15.8266`, 관측 LUID `0,56278`로 고정했다. graphics fingerprint는 `A1427B7E1016A54001B4A210D0B6B96B0D11870043EFCC1C2BDCC59BBCEC5AC8`이다. browser arguments를 저장하고 복원에도 적용했다. 기존 75 backend 차이는 raw 실패와 정확한 서명을 유지한다.

## 같은 출력에서의 성능

동시 빌드/캡처 없이 parser 15 workload를 교대 3회씩, round당 warmup 1회·측정 9회로 variant당 27개 중앙값을 측정했다. 정상 6,000셀 표·2,000개 표를 추가했다. 앱은 before/after 3회와 after/before 3회로 순서를 균형 있게 측정했다. parser DOM 45쌍·앱 BGRA 24쌍·layout JSON 12쌍·입력 6쌍이 같다. 모든 입력의 속도 향상을 주장하지 않으며 증가한 항목도 아래에 남긴다. 앱 자체를 추가 재빌드하지 않고 공통 View 실행기로 측정했다.

| parser workload | v22 before ms | v23 after ms | 시간 변화 |
| --- | ---: | ---: | ---: |
| plain-2m | 2.3101 | 2.3133 | +0.1% |
| tags-6000 | 6.4399 | 6.8297 | +6.1% |
| attributes-6000 | 12.5628 | 12.4288 | -1.1% |
| comments-6000 | 9.7464 | 10.0807 | +3.4% |
| deep-div-128 | 5.3161 | 5.8244 | +9.6% |
| deep-div-512 | 5.2656 | 5.6201 | +6.7% |
| deep-div-1024 | 5.2942 | 5.7583 | +8.8% |
| nested-span-256 | 5.3337 | 5.6628 | +6.2% |
| explicit-list-6000 | 6.1993 | 6.4012 | +3.3% |
| explicit-forms-6000 | 11.1887 | 11.9991 | +7.2% |
| explicit-options-6000 | 6.1376 | 6.4551 | +5.2% |
| explicit-formatting-6000 | 13.6252 | 14.2814 | +4.8% |
| formatted-deep-span-256 | 4.0382 | 4.2902 | +6.2% |
| explicit-table-cells-6000 | 8.5183 | 9.1299 | +7.2% |
| separate-tables-2000 | 6.3525 | 6.7593 | +6.4% |

| 앱 항목 | v22 before ms | v23 after ms | 시간 변화 |
| --- | ---: | ---: | ---: |
| 초기 레이아웃 | 200.21 | 207.74 | +3.8% |
| 최초 페인트 | 101.62 | 103.93 | +2.3% |
| 첫 클릭 동기 페인트 | 14.16 | 14.17 | +0.1% |
| 첫 드래그 동기 페인트 | 15.15 | 15.79 | +4.2% |
| 스크롤 페인트 | 19.93 | 20.43 | +2.5% |

[성능·출력 불변·실행기 해시](C:/twf-v23/performance-final/summary.json), [픽셀·진단 불변](C:/twf-v23/runs/20261005-table-tree-v23-final2/pixel-diagnostic-invariance.json).

## 마지막 전체 회귀·보관·복원

- [x] 정확성·속도·오류 주입 이후 전체 회귀 12개·platform integrity.
- [x] 불변 ZIP/baseline·새 폴더 복원·소스/입력/실행기/DLL 해시·원본 120쌍 재판정.
- [x] 복원 실행기로 전체 594쌍 새 렌더링·별도 증거 ZIP 해시 검증.

주 ZIP `20261005-table-tree-v23-final2.zip`: 284,272,216 bytes, SHA-256 `77401F5239A647C03AFAEC2D3B51A5034DA38B651BB6511AB24E8E3CF6B30E8A`. indexed 20,978개 파일·소스 346개·원본 입력 20개를 새 폴더에 복원하고 해시를 확인했다. 복원 비교 코드로 원본 120쌍을 재판정했다.

**복원 실행기·DLL·입력·측정/비교 코드로 전체 594쌍을 새로 렌더링**하여 디코딩 픽셀·전체 DOM/스타일/기하 JSON·raw 차이·판정이 모두 같음을 확인했다. 복원 증거 ZIP 14,488개 entry·30,741,316 bytes, SHA-256 `7240B9C0FC2209E022D8B9AEA3AF8D4DD28DA66F7E5CFD4AF0FD93382554CD70`도 검증했다. nested work/v22 증거 ZIP의 모든 파일 해시도 확인했다. 주 ZIP 문서는 보관 직전 스냅샷이며 마지막 완료는 [복원 증거](C:/twf-v23/archives/20261005-table-tree-v23-final2-recovery-validation.json)를 따른다.

## 남은 범위

전체 HTML insertion modes·template contents·select/foreign/ancestor-form fragment context·폼 소유자·DOCTYPE/PI·foreign-content, 전체 CSS 자동 표/percentage/column-group 너비 계약·bottom/복잡한 caption wrapper·일반 split-inline 배경, 실제 Windows 두 DPI·나머지 computed style/reference glyph/baseline/cluster·100→1,000문서·원격 영구 보관은 미완료다. corpus는 기존 20문서이며 새 7문서는 별도 고정 교정이다. 대용량 증거는 `C:\twf-v23`에 보존한다.

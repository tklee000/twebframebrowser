# 상대 위치·RTL 넘침·HTML 줄바꿈 정확성 개선 v14

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이며 실제 두 Windows DPI 검증으로 표시하지 않는다.

최종 실행은 `TWebFrame2/tests/rendering/runs/20261004-position-parser-v14-final`이며 **294/294 성공(240 strict·기존 승인 54)**이다. 기존 252쌍의 모든 픽셀·전체 진단·raw 수치와 판정은 그대로다. 새 고정 7문서의 42쌍은 변경 전 전부 실패했으며 최종 픽셀 차이 0으로 교정했다. 정확성·속도 검증 이후 마지막 전체 회귀 12개·platform integrity, 보관·복원과 복원 실행기의 294쌍 새 렌더링을 완료했다. 전체 1,000문서 계약은 미완료다.

## 공통 엔진 수정

- HTML 토큰화 전에 물리 CR/CRLF를 LF로 정규화한다. 텍스트·속성·주석·raw style/script와 fragment가 같은 경로를 사용한다. `&#13;` 및 직접 DOM 텍스트 변경의 CR은 유지한다. CR이 없는 입력은 원래 문자열을 참조하여 전체 복사를 생략한다.
- 상대 위치의 left/right/top/bottom, LTR/RTL 양쪽 inset 우선순위, containing block의 폭/높이 백분율과 불확정 높이의 auto 처리를 교정했다. 정상 흐름을 배치한 뒤 해당 자식만 이동하여 형제의 자리와 크기를 유지한다. nested/absolute 자식은 따라 이동하고 viewport-fixed 자식은 유지한다.
- auto z-index의 positioned box를 일반 흐름 뒤에 그리되 새 stacking context로 만들지 않는다. 실제 stacking context와 해당 positioned 자식의 소속 scope를 구분하여 후손을 중복 그리지 않고 바깥 z-index 순서를 유지한다.
- 넓은 RTL 블록의 음수 end margin, hidden/clip/강제 scroll 및 stable/both-edges 여백의 고정 조합을 교정했다. 상대 이동 전 영역과 실제 이동 영역을 스크롤 크기에 반영하고 RTL 왼쪽 넘침을 metadata에 보존한다. clip은 별도로 처리한다.
- 레이아웃에 상대 이동이 필요한 직접 자식 목록과 실제 stacking context 여부를 캐시한다. restyle/animation에서 갱신하고 페인트·스크롤 때 inset과 stacking 조건을 다시 계산하지 않는다. gutter 폭은 이미 계산한 물리 예약값을 재사용한다.
- 고정 WebView2는 HTML entity로 남은 CR을 pre 텍스트에서 추가 줄바꿈 없이 표시한다. native에서도 줄바꿈을 추가하던 오류를 없애고 DOM 문자 오프셋을 보존했다. [CSS Text 3](https://www.w3.org/TR/css-text-3/)의 최신 CR 규칙과 고정 브라우저의 동작을 같은 것으로 단정하지 않는다.

정규화 범위는 [HTML 입력 전처리](https://html.spec.whatwg.org/multipage/parsing.html#preprocessing-the-input-stream), 상대 위치는 [CSS Positioned Layout](https://www.w3.org/TR/css-position-3/#relative-position)을 대조했다. 실제 좌표·크기·픽셀 기대값은 보존한 WebView2 캡처를 따른다. 문서 ID나 앱 이름에 따른 엔진 분기는 없다.

## 독립 정확성 검사

- v13 저장 소스 194개 해시와 실행기·DLL을 확인한 뒤 별도 before 실행기를 보존했다. 기존 exploratory 입력 4개를 바이트 그대로 복사하고 block/inline-block/flex/grid/nested/백분율 및 parser 교정 3문서를 추가했다. corpus 20문서에는 합산하지 않는다.
- 새 42/42 strict, CSSOM 크기 1,704개·필수 스타일 10,650개·누락 0. 변경 전 0/42와 중간 실패·수정 빌드 기록도 유지한다. reference/CDP 픽셀과 reference 전체 진단은 변경 전후 동일하다.
- 원본 및 기존 교정 252/252의 native/reference/CDP/가능한 WM_PRINTCLIENT 픽셀, 전체 native/reference JSON, raw 수치·coverage·판정은 동일하다. 기존 backend 서명 90개는 그대로이며 새 예외는 0개다.
- 실제 WebView2 `elementFromPoint` 5개 좌표 × 두 배율에서 parent/first/inner/second/viewport-fixed 결과를 확인했다. focused C++ 회귀는 같은 기대값으로 relayout·스타일 변경·relative 활성/해제를 검사한다.
- 기존 비교기 교정 182개와 실제 새 캡처 오류 주입 32개, 총 214개를 통과했다. 1px 상대 좌표·CSSOM 누락·계산 스타일·이전 잘못된 페인트·미처리 DOM 줄바꿈을 실패로 검출한다. 정상 복제본은 strict로 통과한다.
- source snapshot 210개 해시와 최종 실행기·DLL을 확인했다. [전체 감사](../TWebFrame2/tests/rendering/runs/20261004-position-parser-v14-final/pixel-diagnostic-invariance.json)에 그룹별 집계와 before 실패를 보존한다.

## 성능과 실제 앱 출력 변화

v13/v14 공통 View 실행기로 같은 보존 Markdown·앱 HTML/CSS/JS를 번갈아 3회씩 측정했다. 1600×1000, 실제 96 DPI다. MdViewer 앱 추가 재빌드는 하지 않았다.

| 항목 | v13 before | 최종 v14 after |
| --- | ---: | ---: |
| 초기 레이아웃 | 209.19ms | 212.95ms |
| 최초 페인트 | 106.06ms | 106.28ms |
| 스크롤 페인트 | 19.90ms | 20.22ms |
| 외부 포커스 후 첫 클릭 | 14.96ms | 15.50ms |
| 첫 thumb 드래그 | 16.29ms | 16.63ms |

스크롤 페인트는 19.90→20.22ms로 약 1.6% 늘었다. 초기 레이아웃도 약 1.8% 늘어 성능 향상을 주장하지 않는다. 이번 캐시는 새 정확성 처리가 매 프레임 반복되지 않도록 제한하며 측정의 소폭 변화를 그대로 기록한다. 첫 클릭/드래그의 동기 페인트는 유지했다.

출력 전체를 불변으로 표시하지 않는다. 보존 앱의 `.find-button{position:relative;top:2px}`가 이제 적용되어 버튼과 SVG의 y가 정확히 2 CSS px 증가했다. 두 DPI의 독립 WebView2 앱 CSS 교정도 같은 이동을 보인다. BGRA 12쌍은 기존 19×19 SVG 픽셀을 2px 이동한 결과와 정확히 같고 나머지 픽셀은 불변이다. layout JSON 6쌍은 이 두 상자의 y 외의 모든 값이 동일하며 입력 3쌍도 동일하다. 앱의 전체 WebView2 일치 검증으로 확대 해석하지 않는다.

[최종 성능과 출력 교정](../TWebFrame2/tests/rendering/runs/20261004-position-parser-v14-final/optimization-and-accuracy-evidence/performance-final/verified-output-corrections.json)에 raw 시간·픽셀/레이아웃 증거·독립 oracle·자산/입력/실행기/DLL/소스 해시를 보존한다. 첫 paired 측정은 스크롤 19.89→19.72ms였으나 최종 캐시를 반영한 새 측정과 섞어 향상으로 주장하지 않는다.

## 마지막 회귀·보관·복원

- [x] 정확성·속도·214개 비교 교정 이후 마지막 전체 회귀 12개와 platform integrity.
- [x] 불변 baseline 및 새 주 ZIP, 모든 파일 해시 확인과 새 폴더 복원.
- [x] 복원 실행기·DLL·입력·계측/비교 소스로 294쌍 새 렌더링 및 별도 복원 증거 ZIP 해시 확인.

주 ZIP은 12,620개 파일·155,191,228 bytes이며 모든 파일 해시를 검증했다. 복원 증거 ZIP은 7,622개 파일·19,271,160 bytes다.

- [주 ZIP](../TWebFrame2/tests/rendering/archives/20261004-position-parser-v14-final.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-position-parser-v14-final.json)
- [불변 baseline](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-3f0e9e06840cc63f-20261004-position-parser-v14-final/manifest.json)은 원본 120 reference 쌍 인덱스로 보존한다.
- 복원 폴더: `TWebFrame2/tests/rendering/restored/20261004-position-parser-v14-final-verified`
- [복원 판정](../TWebFrame2/tests/rendering/archives/20261004-position-parser-v14-final-recovery-validation.json), [복원 증거 ZIP](../TWebFrame2/tests/rendering/archives/20261004-position-parser-v14-final-recovery-evidence.zip), [증거 catalog](../TWebFrame2/tests/rendering/archives/20261004-position-parser-v14-final-recovery-evidence.json)

주 ZIP SHA-256: `6CBAC4064E0B1B02960F43585DEF9E35516D5267DBA11BF98C252205D0938A1F`. 복원 증거 ZIP SHA-256: `802B475B662F417F18275DE94C8F10C20D85A906C66EC34E7CC3414581F64DDF`.

복원 소스 210개·원본 입력 20개 해시와 원본 120쌍 재판정을 검증했다. 복원 실행기·DLL·입력·계측/비교 코드로 294쌍을 새로 렌더링하여 픽셀·전체 DOM/스타일/문자/크기 진단·raw 차이·판정을 재현했다. ZIP 안의 문서는 보관 직전 스냅샷이며 최종 완료 판정은 복원 JSON과 이 문서를 따른다.

기존 ZIP은 그대로 두고 새 ZIP에 재귀 복사하지 않았다. v13의 비교 증거와 catalog/SHA 참조를 `before-evidence/v13/`, 변경 전 실행기·소스 snapshot·탐색 실패·중간 수정·focused 테스트·성능 자료를 `optimization-and-accuracy-evidence/`에 보존한다. 원본 run과 profile은 삭제하거나 덮어쓰지 않는다.

## 남은 전체 계획

rounded/transformed/deferred clip과 외부 containing block, wide auto-overflow·auto margin·writing mode/table 등 나머지 positioned/RTL 조합과 음수 RTL scroll offset, intrinsic/nested/textarea/root gutter, transform/pseudo/iframe/root overflow, 나머지 계산 스타일·reference per-character glyph/baseline/cluster는 남아 있다. pre/textarea 초기 개행 등 나머지 HTML 파서, 실제 Windows 두 DPI·독립 화면 캡처, 100→1,000문서 확대 및 원격 영구 보관도 미완료다. 이번 고정 7문서 성공으로 이 범위를 완료 처리하지 않는다.

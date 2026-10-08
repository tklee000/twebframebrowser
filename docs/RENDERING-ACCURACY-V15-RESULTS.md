# HTML 텍스트 모드·첫 줄바꿈 정확성 개선 v15

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**318/318 성공(264 strict·기존 승인 54)**이다. 새 고정 4문서 24쌍은 변경 전 모두 실패했으며 최종 픽셀 차이 0으로 교정했다. 기존 294쌍의 모든 픽셀·전체 진단·raw 수치·판정은 불변이며 새 예외는 없다. 정확성·속도 검사 후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원 및 복원 실행기의 318쌍 새 렌더링을 완료했다. 전체 1,000문서 계약은 미완료다.

## 공통 엔진 수정과 독립 검증

- pre/listing/textarea 시작 태그 바로 다음 LF 하나만 무시한다. 물리 CR/CRLF와 `&#10;`를 포함하며 두 번째 LF·앞 공백·주석/하위 요소 뒤 LF·`&#13;`는 유지한다. fragment의 pre/textarea 자체 context는 첫 LF를 유지하고 새 pre 시작 태그만 이 규칙을 적용한다.
- title/textarea의 RCDATA는 마크업을 문자로 유지하고 entity를 디코딩한다. script/style/xmp/noembed/noframes 등의 RAWTEXT는 마크업과 entity를 그대로 유지한다. HTML 비void 태그의 self-closing slash는 무시하고 SVG의 self-closing은 유지한다.
- raw/RCDATA 종료 이름은 ASCII 대소문자와 HTML delimiter로 대조한다. `</stylex>` 같은 접두어는 종료하지 않는다. 따옴표 안의 `>`와 unquoted 값 안의 quote를 구분한다. 각 요소의 텍스트 구간만 전진 탐색하여 매번 전체 문서를 소문자로 만드는 복사를 제거했다.
- raw/RCDATA innerHTML context도 HTML 입력의 물리 CR/CRLF를 정규화하고 entity CR 및 직접 DOM 텍스트 변경의 CR은 유지한다. 두 DPI의 별도 WebView2 fragment oracle 4개와 집중 C++ 회귀가 동일 문자열·자식 유형을 확인했다. 광범위한 JavaScript DOM 검사의 완료로 표시하지 않는다.
- 명시적/preserved break를 가진 non-atomic inline의 line origin과 바깥 half-leading을 교정했다. pre 안 span의 문자 rect와 client/scroll 높이·재배치가 독립 reference와 일치한다. 일반 wrapping inline로 범위를 확대하지 않았다.
- 내용이 실제로 넘치는 양축 hidden textarea만 internal scrolling layer의 grayscale 모드를 사용한다. 내용이 들어맞으면 LCD 모드를 유지한다. 기존 table/form 교정 30쌍(그중 textarea 12쌍)은 불변이다. 이미 계산된 scroll cache를 읽어 추가 크기 조회를 생략한다.

[HTML 파싱 규칙](https://html.spec.whatwg.org/multipage/parsing.html)과 대조했고 정확한 DOM/좌표/픽셀 기대값은 고정 WebView2에서 얻었다. 문서 ID·앱 이름 분기나 새 픽셀 예외/비교 허용치 변경은 없다.

새 24쌍의 CSSOM 크기 456개·필수 스타일 2,850개, 누락 0이다. 기존 182개 비교 교정에 실제 새 캡처의 정상 복제/1px 위치/누락 크기/잘못된 스타일/이전 페인트/잘못된 DOM 줄바꿈 48개를 더해 총 230개를 확인했다. [전체 감사](../TWebFrame2/tests/rendering/runs/20261004-text-parser-v15-final/pixel-diagnostic-invariance.json)는 source snapshot 221개 해시와 이전 294쌍 불변을 검증한다. 변경 전 0/24·중간 실패와 수정 빌드도 보존한다.

## 속도 측정

같은 standalone harness와 CSS helpers에 v14 저장 DOM.cpp와 최종 v15 DOM.cpp를 각각 컴파일했다. 파일 읽기와 DOM 직렬화는 시간에서 제외하고 1회 warmup+9회 측정, 각 입력에서 번갈아 3회씩 총 27회/variant를 기록했다. ASCII raw text를 가진 style/script 쌍 입력이며 모든 결과 DOM은 바이트까지 같다.

| style/script 쌍 | v14 파싱 중앙값 | v15 파싱 중앙값 |
| --- | ---: | ---: |
| 20 | 0.9056ms | 0.0682ms |
| 200 | 85.1277ms | 0.6680ms |
| 600 | 736.4765ms | 2.0348ms |

이 수치는 반복 raw 요소에서 전체 복사의 quadratic 비용을 제거한 결과이며 일반 페이지·스크롤 속도의 수치로 확대하지 않는다. [파서 시간과 동일 DOM 증거](../TWebFrame2/tests/rendering/runs/20261004-text-parser-v15-final/optimization-and-accuracy-evidence/parser-performance/summary.json)를 보존한다.

같은 보존 앱 HTML/CSS/JS·Markdown과 공통 View 실행기로 번갈아 3회씩, 1600×1000 실제 96 DPI에서 측정했다. MdViewer 앱 추가 재빌드는 하지 않았다.

| 항목 | v14 before | 최종 v15 after |
| --- | ---: | ---: |
| 초기 레이아웃 | 213.04ms | 203.94ms |
| 최초 페인트 | 110.06ms | 106.52ms |
| 스크롤 페인트 | 19.49ms | 20.73ms |
| 외부 포커스 후 첫 클릭 | 14.93ms | 15.47ms |
| 첫 thumb 드래그 | 15.90ms | 17.14ms |

초기 레이아웃은 약 4.3% 줄었지만 스크롤 페인트는 약 6.3% 늘었다. 첫 클릭·드래그의 증가도 그대로 기록하며 전체 앱의 모든 동작이 빨라졌다고 주장하지 않는다. 첫 클릭/드래그 동기 페인트는 유지했다. BGRA 12쌍·layout JSON 6쌍·입력 3쌍 모두 불변이고 자산/입력/실행기/DLL/소스 해시를 기록했다. [앱 최종 성능](../TWebFrame2/tests/rendering/runs/20261004-text-parser-v15-final/optimization-and-accuracy-evidence/performance-final/summary.json)을 따른다. 앞선 넓은 inline 교정에서 발생한 앱 출력 변화는 별도 실패 자료로 보존했으며 최종 교정은 명시적 break 범위로 제한했다.

## 마지막 회귀·보관·복원

- [x] 정확성·속도·230개 비교 교정 후 전체 회귀 12개와 platform integrity.
- [x] 새 주 ZIP·불변 baseline·모든 파일 해시·별도 폴더 복원.
- [x] 복원 실행기·DLL·입력·소스로 318쌍 새 렌더링 및 별도 복원 증거 ZIP 검증.

주 ZIP 12,837개 파일·158,354,488 bytes와 복원 증거 ZIP 8,180개 파일·20,286,350 bytes의 모든 파일 해시를 검증했다.

- [주 ZIP](../TWebFrame2/tests/rendering/archives/20261004-text-parser-v15-final.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-text-parser-v15-final.json)
- [불변 baseline](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-ff3eb2bee7a97264-20261004-text-parser-v15-final/manifest.json)은 원본 120 reference 쌍 인덱스로 보존한다.
- [복원 판정](../TWebFrame2/tests/rendering/archives/20261004-text-parser-v15-final-recovery-validation.json), [복원 증거 ZIP](../TWebFrame2/tests/rendering/archives/20261004-text-parser-v15-final-recovery-evidence.zip), [증거 catalog](../TWebFrame2/tests/rendering/archives/20261004-text-parser-v15-final-recovery-evidence.json)

주 ZIP SHA-256: `939A3BD2C35D2FB7AB6F8525526E3179CE3EE28FA972EB5E4C4B31B4CB2C1DD3`. 복원 증거 ZIP SHA-256: `6B138540AB0E60A96D5F046320477F54FCED9F007FD4A997025B819BFCE8B584`.

복원 소스 221개·원본 입력 20개 해시와 120쌍 재판정을 확인했다. 복원 실행기·DLL·입력·계측/비교 코드로 318쌍의 픽셀·전체 진단·raw 차이·판정을 재현했다. 복원 폴더는 `TWebFrame2/tests/rendering/restored/20261004-text-parser-v15-final-verified`다. 주 ZIP의 문서는 보관 직전 스냅샷이며 최종 완료는 별도 복원 JSON과 이 문서를 따른다.

v14의 294쌍 비교 자료·catalog/SHA 참조는 `before-evidence/v14/`에, v15 변경 전 실행기/소스·중간 실패·정확성/성능/독립 oracle은 `optimization-and-accuracy-evidence/`에 보존한다. 기존 ZIP은 새 ZIP에 재귀 복사하지 않았으며 원본 run·profile은 유지한다.

## 남은 전체 계획

script의 escaped/double-escaped 상태·전체 entity/오류 복구·plaintext 및 나머지 HTML tree builder, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대·원격 영구 보관은 미완료다. 이번 4문서는 corpus 수량에 합산하지 않으며 고정 조합의 성공을 전체 HTML/inline/textarea 지원으로 표시하지 않는다.

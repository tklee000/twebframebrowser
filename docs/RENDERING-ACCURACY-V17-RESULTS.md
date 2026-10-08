# v17 문자 참조 정확성·속도 검증 결과

작성일: 2026-10-04. Windows x64 Release, 고정 WebView2 `154.0.4258.53`, 명시적 96/144 DPI × CSS viewport 384×768·800×600·1280×800. 실제 Windows DPI는 96이다.

**378/378 성공(324 strict·기존 승인 54)**이다. 새 고정 5문서 30쌍은 변경 전 0/30에서 최종 픽셀 차이 0으로 교정했다. 기존 v16의 348쌍은 native/reference/CDP/WM_PRINTCLIENT 픽셀·전체 DOM/스타일/기하 JSON·raw 수치·판정이 불변이다. 새 예외와 허용치 변경은 없다. 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원 및 복원 실행기의 378쌍 새 렌더링까지 완료했다.

## 함께 수정한 세 단계

1. WHATWG 이름 참조 2,231개를 정적 trie로 조회하며 가장 긴 일치·대소문자·legacy 세미콜론 생략·두 코드 포인트·보조 평면 UTF-16을 처리한다. 명명 표와 생성기를 저장해 빌드 시 네트워크에 의존하지 않는다. 저장 공간은 96,944 bytes이며 동적 초기화 map은 없다.
2. 숫자 참조는 decimal/hex와 optional semicolon을 읽고, NULL·surrogate·범위 초과를 U+FFFD로 보정하며 0x80~0x9F의 Windows-1252 대응을 적용한다. 유효한 noncharacter/기타 control은 문자로 유지한다. 임의 길이 digit run을 포화 누산으로 선형 처리하여 overflow를 방지한다.
3. 속성 문맥에서는 세미콜론 없는 이름 뒤 ASCII alphanumeric/`=`가 오면 literal 값을 유지한다. quoted/unquoted 입력에서 공통 문맥을 전달한다. script/raw·직접 DOM 값은 디코딩하지 않고 CSS 문자열/`attr()`의 불필요한 두 번째 디코딩도 제거한다.

[HTML 토큰화 규칙](https://html.spec.whatwg.org/multipage/parsing.html#character-reference-state)과 [WHATWG 이름 표](https://html.spec.whatwg.org/entities.json)를 대조했다. 저장한 표 SHA-256은 `D741D877AC77C4194C4AD526B5B4A19AEF8DFE411AB840A466891CDBB9F362E6`다. 문서 ID·앱 이름 분기는 없다.

독립 DOMParser/textarea fragment oracle **11,525개를 두 DPI에서 모두 확인**했다. 2,231개의 모든 이름을 text/속성/fragment에서 확인하고 legacy suffix·다른 quote/unquoted·숫자 scalar와 malformed/EOF·raw/직접 DOM 문맥을 추가했다. 변경 전 불일치 9,929개와 원본 출력도 보존했다. 일반 rendering fixture scripts는 비활성화하며 detached DOMParser는 별도 read-only oracle 계측에서만 사용했다. 전체 JavaScript DOM 적합성 완료로 표시하지 않는다.

새 30쌍은 CSSOM 크기 360개·필수 스타일 2,250개, 누락 0이다. 기존 비교기 교정 182개와 새 실제 캡처의 정상 복제·DOM text/attribute 오류·1px 좌표·누락 크기·잘못된 스타일·이전 페인트 오류 주입 210개를 통과했다. [최종 감사](C:/twf-v17/runs/20261004-entities-v17-final2/pixel-diagnostic-invariance.json)는 소스 252개 해시와 기존 348쌍 불변을 확인한다.

## 속도와 출력 확인

같은 C++ benchmark와 입력에서 1회 warmup+9회 측정, 각 workload/variant를 번갈아 3회씩 실행했다. 각 중앙값은 27개 표본이다. `valid-N`은 기존에 지원하던 6개 이름·2개 숫자 참조를 N×100회, `literal-N`은 literal `&!`를 N×100회, `raw-N`은 style/script N쌍이다. 파일 읽기와 DOM 직렬화는 시간에서 제외했고 **DOM 출력 24쌍은 바이트까지 동일**하다.

| workload | v16 before | 최종 v17 after |
| --- | ---: | ---: |
| valid-20 | 0.3501ms | 0.2025ms |
| literal-20 | 0.0168ms | 0.0130ms |
| valid-200 | 3.3827ms | 1.9067ms |
| literal-200 | 0.1752ms | 0.1426ms |
| valid-600 | 9.9138ms | 5.5237ms |
| literal-600 | 0.4795ms | 0.3889ms |
| raw-200 | 0.4457ms | 0.4443ms |
| raw-600 | 1.3431ms | 1.3486ms |

처음 trie-only 측정에서 짧은 참조 다량 입력이 느려지는 것을 확인해 세미콜론으로 끝나는 흔한 이름의 직접 처리 경로를 추가했다. 이전 측정도 보존한다. 최종 해당 입력의 파싱은 약 42~44% 감소했으며 이 수치를 일반 페이지·스크롤 개선으로 확대하지 않는다.

같은 보존 앱 HTML/CSS/JS·Markdown과 공통 View 실행기로 번갈아 3회씩, 1600×1000 실제 96 DPI에서 측정했다. 앱 추가 재빌드는 하지 않았다. **앱 BGRA 12쌍·layout JSON 6쌍·입력 3쌍은 불변**이다.

| 항목 | v16 before | 최종 v17 after |
| --- | ---: | ---: |
| 초기 레이아웃 | 215.84ms | 210.86ms |
| 최초 페인트 | 108.16ms | 109.43ms |
| 외부 포커스 후 첫 클릭 | 15.18ms | 15.27ms |
| 첫 thumb 드래그 | 16.41ms | 16.92ms |
| 스크롤 페인트 | 19.89ms | 20.99ms |

스크롤·최초 페인트·첫 입력의 증가도 기록하며 모든 동작의 속도 향상을 주장하지 않는다. 첫 클릭/드래그 동기 페인트도 유지했다. [성능과 실행기 해시](C:/twf-v17/runs/20261004-entities-v17-final2/optimization-and-accuracy-evidence/performance-summary.json)를 따른다.

## 마지막 검증과 보존

- [x] 정확성·속도·비교 교정 이후 전체 회귀 12개와 platform integrity.
- [x] 불변 ZIP/baseline·파일 해시·새 폴더 복원과 원본 120쌍 재판정.
- [x] 복원 실행기의 전체 378쌍 새 렌더링·별도 증거 ZIP 파일 해시.

주 ZIP 14,219개 파일·149,361,489 bytes와 복원 증거 ZIP 9,564개 파일·22,333,908 bytes의 모든 파일 해시를 확인했다. 복원 소스 252개·원본 입력 20개·원본 120쌍 재판정, 복원 실행기의 전체 378쌍 새 렌더링에서 모든 픽셀·전체 진단·raw 수치·판정이 동일하다.

- [주 ZIP](C:/twf-v17/archives/20261004-entities-v17-final2.zip), [catalog](C:/twf-v17/archives/20261004-entities-v17-final2.json)
- [복원 판정](C:/twf-v17/archives/20261004-entities-v17-final2-recovery-validation.json), [복원 증거 ZIP](C:/twf-v17/archives/20261004-entities-v17-final2-recovery-evidence.zip), [증거 catalog](C:/twf-v17/archives/20261004-entities-v17-final2-recovery-evidence.json)

주 ZIP SHA-256: `50253678A4128475BCB845A428FE490A769912A6271F8D581A386AAFC948860D`. 복원 증거 ZIP SHA-256: `7FA12870E0105B857AD270FA2A8549725CBC45E6FD47AFFF37EC2B5A96D41952`.

작업 원본과 중간 실패는 `optimization-and-accuracy-evidence/work-evidence.zip`, v16의 이전 348쌍·소스·진단은 `before-evidence/v16-evidence.zip`에 파일별 SHA 인덱스와 함께 보존했다. 새 복원 폴더에서 내부 파일의 모든 해시를 다시 확인했다. 원본 장문 경로를 다시 만들지 않아 Windows 경로 제한을 피한다. 주 ZIP 문서는 보관 직전 스냅샷이며 최종 완료는 별도 복원 JSON과 이 기록을 따른다.

D 드라이브 공간 부족으로 중간 캡처/빌드가 실패한 기록은 보존했고 최종 대용량 자료는 `C:\twf-v17`로 옮겼다. 새 임시 browser profile은 삭제 없이 별도 C 폴더로 옮겼다. 기존 v16 실행·ZIP은 변경하지 않았다. 소스·생성기·고정 교정 입력은 저장소 안에 있다.

## 남은 전체 계약

전체 HTML 오류 복구/tree builder·attribute tokenizer, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대·원격 영구 보관은 미완료다. 이번 5문서는 corpus 수량에 합산하지 않는다. 이름 표 전체와 고정 oracle의 성공을 전체 HTML parser 완료로 표시하지 않는다.

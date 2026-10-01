2026-10-02 보안검사 미지원 API 및 지연 진단

실제 초록색 완료는 아직 확인되지 않았다. `2.png`의 성공 표시를 완료 기준으로 삼으며, HTTP 200 응답이나 DOM 안의 숨겨진 성공 문구를 성공으로 판정하지 않았다.

후속 수정과 빌드 출력은 `D:\ai_works\miniwebbrowser\TWebFrame2` 아래에 있다. 엔진은 공통 JavaScript 실행기, DOM 인터페이스와 CSS 레이아웃 규칙을 수정했다. 특정 URL, DOM ID, 화면, 난독화 함수 이름을 검사하는 엔진 분기는 추가하지 않았다. 최신 확인용 실행 파일은 `tests/bin/verified-common/TWebFrameBrowser.exe`다.

확인한 공통 결함과 수정:

- EventTarget 및 Event/CustomEvent/MessageEvent/UIEvent/MouseEvent/KeyboardEvent/InputEvent/PointerEvent의 실제 프로토타입과 상속, 생성자·수신자 검사, 이벤트 데이터와 취소 처리를 보완했다. dispatchEvent는 전달받은 이벤트의 정체성을 유지한다.
- HTML 이벤트 핸들러 속성이 없던 Document/Window/HTMLElement/SVGElement에 표준 기본값과 getter/setter를 제공했다. inline 속성과 IDL 속성의 중복 실행을 없애고, 동일한 속성을 다시 설정하거나 제거할 때 핸들러 상태를 갱신한다. Worker 전역 onmessage도 실제 Worker 전역으로 전달한다.
- getter/setter의 따옴표·숫자·계산된 키 문법과 인수 개수 검사를 추가했다. 계산된 키는 값 초기화 전에 ToPropertyKey를 수행하며 변환 부수 효과와 원래 예외를 유지한다.
- Reflect.construct/Proxy 및 일반 생성자 검사를 보완하고, Date의 프로토타입·브랜드 검사·TimeClip·숫자 변환·toJSON 처리를 수정했다. Navigator/Screen/Performance에는 실제 엔진·디스플레이·시계 값을 반환하는 인터페이스 프로토타입을 제공한다.
- Array.prototype.push를 안정된 공통 프로토타입 함수로 제공한다. 안전한 밀집 배열은 실제 저장소에 직접 추가하고, 그 외에는 length getter와 숫자 변환, 인덱스 setter, 상속된 setter, 쓰기 순서와 예외를 유지한다. 순수 숫자/읽기 함수의 Function.call은 기존 최적화 경로를 사용할 수 있다.
- Intl.RelativeTimeFormat에 빠져 있던 resolvedOptions와 실제 프로토타입을 추가했다. 단순 영문/한글 문자열 조합을 Windows ICU 상대 시간·숫자 데이터로 대체했다. numeric/style/numberingSystem 옵션, 음수 0의 방향, 생성자·수신자·인수 검사, 옵션 getter와 원래 예외를 검증했다. ICU 서비스가 없는 시스템에서는 해당 기능을 노출하지 않는다.
- inline-flex의 외부 inline 성질을 내부 flex 방향으로 잘못 해석하던 고유 너비 계산을 수정했다. column/column-reverse에서는 자식 너비의 최댓값을 사용하고 수평 gap을 더하지 않는다. min-content와 max-content 경로를 함께 수정했다. row/row-reverse는 기존 합산 규칙을 유지한다.

Release x64 검증 자료:

- 전체 엔진 검사: `artifacts/security-api-final-suite.log`. inline-flex 수정 후 `artifacts/security-common-final-suite.log`에서도 전체 통과했다.
- 상대 시간 API: `artifacts/security-api-final-relative.log`. 인터프리터/JIT 임계값 0/2 × DPR 1/1.5의 네 조합에서 PASS.
- 배열·멤버 읽기·값 접근·캡처 읽기·숫자 연산: `artifacts/security-api-final-js.log`.
- inline-flex의 네 방향과 좁은 컨테이너: `artifacts/security-inline-flex-tests.log`. 100%/150%에서 column 너비 80px, row 너비 157px 확인.
- 장시간 순환 수집과 살아 있는 DOM/async 콜백: `artifacts/security-api-final-heap.log`. 1200개 스크립트 뒤 프로토타입 슬롯 117.
- 타이틀 메시지, 입력·타이머 실행 순서, 종료 및 DPI RECT 수명: `artifacts/security-api-final-responsiveness.log`.
- 실제 96/144 DPI Canvas 체크 표시 픽셀: `artifacts/security-api-final-canvas.log`. 두 배율 통과.

성능 측정의 범위:

30만 글자를 charCodeAt/fromCharCode/push로 처리하는 공통 로컬 시험에서 배열 처리 변경 전에는 3045~3130ms, 변경 후에는 1524~1594ms였다. 네 조합 모두 결과 문자열이 같았다. 이 수치를 실제 보안검사의 완료 시간이나 통과율로 해석하지 않는다. 실제 사이트는 방문마다 다른 진단 작업을 내려주므로 여러 실행의 최장 작업 시간을 동일한 작업의 전후 비교로 사용하지 않았다. 선택적 실행 프로파일의 instruction sample은 CPU 비율이 아니다. 네이티브 PDB 손상 경고가 있는 빌드의 함수명 샘플도 원인 판정에 사용하지 않았다.

실제 페이지 관찰:

`artifacts/security-push-production.log`의 120초 관찰에서는 최장 작업 CPU 12500ms/벽시계 12515ms, overrunBegin → overrunEnd → interactiveBegin을 관측했다. 완료 메시지는 없었다.

상대 시간 API 수정 뒤 실제 브라우저의 기본 로그인 URL로 실행한 `artifacts/security-api-final-production.log`에서는 최장 작업 CPU 10265ms/벽시계 10360ms였다. 전체 JavaScript CPU 시간은 37515ms였다. 수정한 이벤트 속성과 resolvedOptions 누락은 기록되지 않았다. POST 두 건이 HTTP 200이었지만 최종 화면은 “사람인지 확인하십시오”이며, 완료 메시지와 초록색 성공은 없었다. RUNTIME_ERROR는 비어 있고 EXECUTION_TIME_LIMIT는 0이다. 따라서 진단 호스트의 강제 중단으로 실패한 실행은 아니다. 처리 지연 메시지는 실제로 발생했으나 서버가 지연만으로 최종 실패를 판정했다는 증거는 확보하지 못했다.

inline-flex 수정까지 포함한 최종 120초 관찰은 `artifacts/security-common-final-production.log`다. 이번에는 선택적 실행 프로파일을 끄고 실행했다. 최장 작업 CPU 13234ms/벽시계 13313ms, 전체 JavaScript CPU 44500ms였다. 75.5초에 overrunBegin, 91.4초에 overrunEnd와 interactiveBegin을 관측했다. 수정한 API의 누락은 없었고 호스트 중단도 없었지만, 실제 완료 메시지는 없었다. 최종 `page-script-render.bmp`에서 글자는 한 줄로 표시되며 브랜드 열 너비도 126px에서 실제 SVG 고유 너비 73px로 수정됐다. 화면은 수동 확인 체크박스 단계이며 `2.png`의 초록색 성공은 재현하지 못했다.

프로덕션 로그의 선택적 WebGL/WebGPU/AudioContext/FontFace/Intl 추가 서비스 등 feature probe 누락과, catch된 예외는 개별적으로 최종 실패 원인이라고 단정하지 않았다. 원래 실행기에는 여전히 미구현 기능과 표준 적합성 차이가 있다. 이번 수정으로 모든 Web API/ECMAScript/Intl 적합성이 완성된 것은 아니다. 예를 들어 RelativeTimeFormat의 formatToParts 및 전체 로케일 협상, 이벤트 핸들러 등록 순서 등은 별도 구현 과제다.

별도로 확인한 환경 차이:

- 읽기 전용으로 확인한 Browser/HttpClient.cpp의 HTML·스크립트 로더는 Chrome/124 문자열을 사용한다. 엔진 ScriptHttpSession의 POST와 navigator.userAgent는 실제 TWebFrame/1.0 문자열이다. 요청 경로별 User-Agent 불일치가 있다. Cloudflare는 세션 중 User-Agent 변경을 WebView 실패 가능 원인으로 설명한다. 이번 실패의 단독 원인이라는 증거는 아직 없다. 제한 범위 밖의 Browser 코드는 수정하지 않았다.
- document.cookie는 실제 브라우저 쿠키 저장소와 연결되어 있지 않으며 navigator.cookieEnabled는 false다. 자원 로더와 스크립트 전송의 세션도 다르다. WebView2는 별도 영구 사용자 데이터 폴더를 사용하므로 사용자가 관찰한 1~2초 결과와 새 엔진 진단은 저장 상태까지 동일한 비교가 아니다. 쿠키나 검증 토큰을 다른 브라우저에서 가져오지 않았다.
- Cloudflare 공식 문서는 사용자 정의·수정된 엔진과 임베디드 브라우저를 제한 지원 대상으로 설명한다. 따라서 공통 API 수정과 로컬 검사 통과만으로 실제 서비스의 자동 완료를 보장할 수 없다.

파일 범위 확인은 `artifacts/security-common-scope-verification.json`에 있다. Browser/main.cpp와 WebView2Browser/main.cpp, 원래 WebView2 실행 파일은 10월 1일 보관한 해시와 같다. 원래 Browser 실행 파일은 그 기준과 다르며 파일 수정 시각은 10월 2일 01:10이다. 이 후속 빌드의 출력 경로는 모두 TWebFrame2 아래다. 원본 실행 파일을 되돌리거나 덮어쓰지 않았다. 최종 엔진·실행 파일 해시는 `artifacts/security-common-final-hashes.json`에 있다.

참고한 기본 규칙: [DOM Events](https://dom.spec.whatwg.org/#interface-event), [HTML event handlers](https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-idl-attributes), [ECMAScript ToPropertyKey](https://tc39.es/ecma262/multipage/abstract-operations.html#sec-topropertykey), [Array.push](https://tc39.es/ecma262/multipage/indexed-collections.html#sec-array.prototype.push), [Intl RelativeTimeFormat](https://tc39.es/ecma402/#sec-intl-relativetimeformat-prototype-resolvedoptions), [ICU Relative Date Time](https://unicode-org.github.io/icu-docs/apidoc/released/icu4c/ureldatefmt_8h.html), [Flex intrinsic cross sizes](https://www.w3.org/TR/css-flexbox-1/#intrinsic-cross-sizes), [Cloudflare solve issues](https://developers.cloudflare.com/cloudflare-challenges/troubleshooting/challenge-solve-issues/), [Cloudflare supported browsers](https://developers.cloudflare.com/cloudflare-challenges/reference/supported-browsers/).

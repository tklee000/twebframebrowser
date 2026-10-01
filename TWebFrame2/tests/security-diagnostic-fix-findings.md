# 보안 검사 엔진 진단 및 수정 기록

2026-10-01 기준, 공식 진단의 잘못된 서버 연결 실패는 수정했다. 실제 로그인 페이지에서 `2.png`처럼 초록색 체크가 나오는 것까지는 확인되지 않았다. 최신 120초 관찰에서는 사람 확인 단계에 머물렀고 완료 메시지는 없었다.

공식 진단 스크립트는 서버 연결을 확인하기 위해 `fetch(url, {method: 'HEAD', mode: 'no-cors'})`를 사용한다. 기존 엔진은 요청 모드를 전달하지 않고 모든 교차 출처 요청에 CORS 검사를 적용했다. 서버는 실제로 HTTP 404를 반환했지만 CORS 허용 헤더가 없어 엔진은 네트워크 실패로 처리했다. 이 때문에 공식 진단이 `Cannot reach servers`를 표시했다. HTTP 404도 서버에 연결했다는 증거이며, Fetch의 연결 성공과 HTTP 성공 상태는 별개다.

공통 Fetch 실행 경로에 요청 모드와 불투명 응답을 구현했다. 성공한 교차 출처 `no-cors` 요청은 Promise를 완료하며, 페이지에는 `type: 'opaque'`, `status: 0`, 빈 URL·헤더·본문 및 null body를 제공한다. 일반 CORS 요청은 기존 허용 검사를 거치고, `same-origin`의 교차 출처 요청은 보내기 전에 거부한다. `no-cors`의 메서드·요청 헤더 제한도 적용했다. 수정 후 공식 진단의 `Server Connection` 결과가 **Can reach Cloudflare**로 바뀐 것을 확인했다. 근거는 [Fetch 표준](https://fetch.spec.whatwg.org/)과 로컬 `artifacts/security-nocors-verification/page-script-text.txt`, `page-script-layout.json`이다.

별도로 JavaScript가 실행되는 문서에서 `<noscript>`를 일반 자식 요소로 파싱하고 렌더링하는 오류가 있었다. 공식 진단의 `JavaScript is Required` 화면이 실제 실행 상태와 달리 화면을 덮었다. 문서의 스크립트 설정을 HTML 파서와 기본 CSS 규칙에 전달하고, 활성 문서는 raw text로 파싱해 기본적으로 숨긴다. 비활성 문서와 DOMParser의 inert 문서는 대체 마크업을 파싱한다. 비동기 문서 전달과 innerHTML 직렬화도 설정을 보존한다. 이는 [HTML 표준](https://html.spec.whatwg.org/dev/scripting.html)의 공통 noscript 규칙이다.

실행 지연도 측정했다. 런타임 진단에 JavaScript 작업의 경과 시간과 Windows 스레드 CPU 시간을 함께 추가했다. 최신 실제 페이지의 가장 긴 작업은 경과 16,750ms, CPU 16,671ms였다. 통신 대기만으로 설명할 수 있는 지연이 아니다. 사이트의 10초 완료 대기 기준을 넘고 재초기화가 반복되는 상황과 일치한다. 다만 보안 서버가 어떤 최종 실패 코드로 판정했는지는 확인하지 못했다.

공통 실행 비용을 줄이기 위해 캡처된 배열 조회 계획은 매 호출에서 값을 복사하는 대신 짧은 수명의 읽기 슬롯과 숫자를 사용한다. 캡처 변수·함수 교체를 매번 확인하며, getter·Proxy·형 변환·외부 realm 등은 기존 실행 경로를 사용한다. VM의 숫자 산술·비교·단항 연산은 이미 평가된 스택 피연산자를 재사용한다. 문자열, BigInt, Reference와 외부 realm은 일반 경로를 유지한다.

`Function.apply`가 배열 저장소를 그대로 복사해 인덱스 getter를 건너뛰는 오류도 재현하고 수정했다. 공통 배열 유사 객체 처리에서 길이를 한 번 읽고 변환한 뒤 인덱스를 순서대로 읽는다. 원래 예외와 getter·Proxy의 관찰 순서를 보존하며 Reflect.apply와 Reflect.construct에도 같은 처리를 사용한다.

| 검증 | 결과 |
|---|---|
| 공식 서버 연결 진단 | 실패에서 `Can reach Cloudflare`로 변경 |
| 캡처 조회 50만 회, 최적화 실행 | 100%: 975→723ms, 150%: 952→735ms |
| 혼합 숫자 연산 30만 회, 최적화 실행 | 100%: 565→433ms, 150%: 531→417ms |
| 같은 숫자 시험, 인터프리터 실행 | 100%: 784→651ms, 150%: 752→630ms |
| 실제 로그인 페이지, 최신 120초 관찰 | HTTP 200 응답 2회, 완료 없음, 사람 확인 단계 |

마이크로 시험의 개선율은 실제 보안 검사 전체 완료 시간의 개선율을 의미하지 않는다. 실사이트의 검사 내용은 매 요청마다 달라지므로 서로 다른 요청의 작업 시간만으로 속도 개선율을 단정하지 않았다.

재현·회귀 기록은 `artifacts/security-scalar-suite.log`, `security-scalar-before.log`, `security-scalar-after.log`, `security-read-guards-final.log`, `security-array-like-before.log`, `security-array-like-after.log`, `security-noscript.log`, `security-responsiveness-final.log`에 저장했다. 새 시험은 `captured-read-plan-regression.js`, `array-like-call-regression.js`, `scalar-operation-regression.js` 및 TWebFrameTests의 noscript·Fetch 시험이다. 숫자·호출 시험은 JIT 사용 여부와 DPI 100%/150% 조합을 검사한다. 마지막 전체 엔진 시험, 네이티브 창 응답·페이지 이벤트 순서 시험, 장시간 메모리 회귀 시험과 96/144 DPI 실제 Canvas 픽셀 시험이 모두 통과했다. 메모리·Canvas 결과는 `security-heap-final.log`, `security-canvas-final.log`에 있다.

실제 관찰은 `artifacts/security-scalar-production.log` 및 `security-scalar-production/page-script-layout.json`, `page-script-render.bmp`에 있다. `RUNTIME_ERROR`는 비어 있고 `EXECUTION_TIME_LIMIT`는 0이다. 관찰 종료가 호스트의 실행 중단 때문이었던 것은 아니다. 예외 생성 기록에는 검사 중 잡힌 예외도 포함되므로 그 목록만으로 최종 실패 원인을 단정하지 않았다. DOM에 숨겨진 `성공!` 문구가 존재하는 것 또한 실제 성공 증거로 사용하지 않았다.

소스와 빌드 출력은 `D:\ai_works\miniwebbrowser\TWebFrame2` 안에 한정했다. 화면 ID·사이트별 보정·검사 성공 상태나 토큰의 생성은 추가하지 않았다. 확인용 실행 파일은 `TWebFrame2\tests\bin\x64\Release\TWebFrameBrowser.exe`다. 원래 Browser/WebView2Browser 소스와 두 실행 파일의 SHA256 비교는 `artifacts/security-final-scope-verification.json`에 있으며 모두 동일하다.

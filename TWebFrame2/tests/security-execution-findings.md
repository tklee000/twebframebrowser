2026-10-01 공통 실행기 후속 개선

이번 변경은 TWebFrame2/src/JavaScript.cpp, TWebFrame2/tests/TWebFrameTests.cpp와 새 회귀검사 파일 execution-read-regression.js에 한정했다. 기존 작업은 유지했다. 사이트 주소, DOM ID, 화면 문구나 난독화된 함수 이름을 검사하는 엔진 분기는 추가하지 않았다. Browser 및 WebView2Browser 소스와 원래 실행 파일은 이번 작업에서 수정하지 않았다. 빌드 출력과 진단 자료도 TWebFrame2/tests 아래에 저장했다.

JavaScript 값 복사는 실제 타입에 해당하는 저장소만 복사한다. 내부 순환 수집기의 타입 없는 참조 스냅샷은 별도 처리해 참조 그래프를 보존한다. 즉시 값을 읽는 연산 앞에서는 변수/속성의 임시 Reference 할당을 줄인다. 메서드 호출, 쓰기, delete, duplicate와 중단 가능한 연산은 필요한 Reference를 유지한다.

짧은 함수의 공통 슬롯 실행 경로는 메서드 호출, 비교와 단항 연산까지 처리한다. 메서드 조회는 인수 평가 전에 수행하며 getter는 한 번만 호출한다. 호출 수신자(this), Proxy, getter 예외와 comma 연산으로 수신자를 제거하는 동작을 회귀검사했다. 슬롯/스택 제한과 지원하는 명령 형태 검사는 유지하고 다른 함수는 기존 실행기를 사용한다. 이미 소유한 임시 값은 이동하며 즉시 소비되는 GetValue/Pop 명령의 중복 작업을 줄인다.

동기 실행이 끝난 프레임의 스택 용량을 최대 64개까지 재사용한다. 재사용 전에 환경, 스택, 완료 값과 비동기 상태를 비운다. 다른 소유자가 있는 프레임이나 중단되어 저장된 프레임은 재사용하지 않는다.

검증:

- Release x64 빌드 성공. 전체 TWebFrameTests 통과: artifacts/security-execution-full-tests.log.
- 실행기 회귀검사를 JIT 임계값 0/2 및 devicePixelRatio 1/1.5의 네 조합에서 통과: artifacts/security-execution-scale-tests.log. 조회/호출의 체크섬은 모두 1700000이다. 이 작은 벤치마크 수치는 실제 보안검사 완료 시간으로 해석하지 않는다.
- 기존 값/인덱스 접근, 함수 프로토타입, 스코프, 인코딩/Canvas 회귀검사 통과: artifacts/security-execution-api-tests.log. 로그의 TypeError는 잘못된 수신자를 전달해 catch로 확인하는 검사에서 발생했다.
- CanvasRegression의 실제 96/144 DPI 렌더 타깃 검사 통과: artifacts/security-execution-dpi-tests.log. CSS 좌표와 픽셀 배율을 분리한 기존 엔진 동작을 유지했다.
- RuntimeHeapRegression의 장시간 순환 수집, 살아 있는 DOM 리스너/비동기 콜백, 중단된 async 프레임 보존 검사 통과: artifacts/security-execution-heap-tests.log.

실제 페이지 결과:

WebView2에서 초록색 성공 체크를 관찰했고 artifacts/security-current-baseline/webview-success.png에 저장했다. 최신 엔진의 200초 PageScriptProbe 관찰에서는 검증 POST 두 번이 모두 HTTP 200을 받았고 각각 798300/117964 문자 응답을 받았다. 이후 후속 실행과 widgetStale 이벤트를 관측했다. 종료 시 instructions=846099292, fastScopeCalls=17161512, collections=329, EXECUTION_TIME_LIMIT=0이었다. 따라서 이 진단은 호스트의 강제 중단 없이 후속 작업까지 실행했지만 성공 토큰이나 자동 성공 표시를 확인한 결과가 아니다. 개별 caught 예외를 곧바로 검사 실패 원인으로 단정하지 않는다.

확인용 GUI 빌드: D:\ai_works\miniwebbrowser\TWebFrame2\tests\bin\x64\Release\ExecutionBrowser.exe. 실제 GUI는 '사람인지 확인하십시오' 체크박스 단계를 표시했다: artifacts/security-after-execution-final/browser-current.png. 사용자 승인 없이 CAPTCHA를 클릭하지 않았다. 1~2초 자동 완료와 초록색 체크라는 목표는 아직 달성하지 못했다. 원래 bin/Browser/x64/Release/TWebFrameBrowser.exe는 이번 변경을 포함하지 않는다.

작업 전 실행기와 테스트 실행 파일은 artifacts/security-execution-before/에 있다. 기타 진단 자료는 artifacts/security-current-baseline/, artifacts/security-current-trace/, artifacts/security-after-execution/, artifacts/security-after-execution-final/에 있다. 진단 HTML과 로그에는 실제 서버가 발급한 일시적인 URL이 있으므로 그대로 외부에 게시하지 않는다.

공통 호출 규칙은 [ECMAScript EvaluateCall](https://tc39.es/ecma262/multipage/ecmascript-language-expressions.html#sec-evaluatecall)을 참고했다. Cloudflare는 [공식 브라우저 지원 문서](https://developers.cloudflare.com/cloudflare-challenges/reference/supported-browsers/)에서 사용자 정의/크게 수정된 엔진과 임베디드 브라우저를 제한적인 지원 대상으로 분류한다. 이 문구는 현재 실패의 단독 원인이 입증됐다는 뜻이 아니다.

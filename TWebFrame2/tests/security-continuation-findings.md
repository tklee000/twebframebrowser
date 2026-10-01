2026-10-01 보안확인 후속 진단

이번 후속 작업의 소스, 테스트, 실행 파일, 화면 캡처와 로그는 모두 `D:\ai_works\miniwebbrowser\TWebFrame2` 안에 저장했다. 기존 수정 사항을 유지했다. 실제 엔진 변경은 공통 JavaScript 실행기와 Canvas 경로/픽셀 렌더링에 한정하며, 사이트 주소나 DOM ID, 난독화 함수 이름에 따른 보정은 추가하지 않았다.

사용자가 수동 확인을 클릭한 기존 브라우저의 상태를 `artifacts/security-device-time-current.png`에 캡처했다. 이 화면에서는 보안확인 영역이 비어 있었으므로 접근성 트리도 `artifacts/security-device-time-accessibility.txt`에 함께 저장했다. 트리에는 “장치에 날짜와 시간이 올바르게 설정되어 있는지 확인합니다”와 후속 문제 해결 안내가 있었다. 기존 브라우저 창은 보존했다.

시각 확인 당시 Windows 시각은 `2026-10-01T15:55:47+09:00`, UTC 기준 시각은 `2026-10-01 06:55:47 UTC`로 초 단위까지 일치했다. `Date.now()`, `new Date().getTime()`, `toISOString()`, `performance.timeOrigin + performance.now()`의 정렬도 확인했다. 따라서 화면 문구만으로 운영체제 시간이 잘못됐다고 결론 내릴 수 없다. Cloudflare 문서도 시간 관련 실패의 원인에 잘못된 장치 시각과 오래 걸린 검증을 함께 열거한다. 이번 화면의 정확한 오류 번호는 확인하지 못했다.

공통 엔진 변경:

- `src/JavaScript.cpp`: TextEncoder와 UTF-8 encode/encodeInto, 실제 Uint8Array 저장소 쓰기, UTF-16 단위 read 및 바이트 단위 written, 수신자/대상 검증을 추가했다. Unicode 문자열 이스케이프에서 서로게이트 코드 단위를 잘못 치환하던 Lexer 오류도 수정했다.
- `src/JavaScript.cpp`, `src/Canvas.h`, `src/Layout.cpp`: Canvas quadraticCurveTo/bezierCurveTo를 실제 Direct2D 경로로 처리한다. 제어점과 끝점에는 경로 생성 시점의 변환을 적용한다. getContextAttributes와 첫 getContext 호출의 alpha/willReadFrequently 설정을 제공하며, 불투명 컨텍스트의 초기화/clearRect는 검정색 불투명 픽셀로 처리한다. 너비/높이 재설정에도 컨텍스트 설정은 유지한다.
- `src/JavaScript.cpp`: 기존 ISO 날짜 파서에 표준 GMT HTTP 날짜 및 toUTCString 형식 해석을 추가했다. Unix epoch 이전 날짜를 NaN으로 거부하던 검사도 제거했다. 수정 전에는 `Date.parse('Thu, 01 Oct 2026 06:55:47 GMT')`가 NaN이었다. 수정 후에는 1790837747000을 반환한다. 이 호환성 결함이 실제 보안확인 실패의 단독 원인이라는 증거는 없다.
- `src/JavaScript.cpp`: 값 변환의 불필요한 복사, 바로 읽히는 속성의 임시 Reference 할당, 짧은 함수의 매 호출 슬롯 이름 검색/참조용 Value 생성을 줄였다. 호출 수신자, getter 부수 효과/예외, 대입/delete 참조는 보존한다. 일반 배열/속성/함수 호출 벤치마크의 체크섬은 600000으로 같았고, 측정은 839ms에서 768ms로 줄었다. 이 작은 벤치마크의 결과를 실제 보안확인의 속도 개선치로 해석하지 않는다.

최종 Release x64 검증:

- `TWebFrameTests` 전체 통과: `artifacts/security-final-full-tests.log`.
- 날짜/속성, 인코딩/Canvas, 기존 스코프 회귀검사 모두 PASS: `artifacts/security-final-api-tests.log`. 로그에 있는 TypeError는 잘못된 API 수신자/대상을 전달하고 catch로 확인하는 의도적인 테스트다.
- `CanvasRegression`의 96/144 DPI 렌더 타깃에서 실제 곡선 픽셀/위치 검사 통과: `artifacts/security-final-dpi-tests.log`. CSS 좌표와 Canvas 저장소 좌표를 유지하고 렌더 타깃에 DPI를 적용했다.
- `RuntimeHeapRegression` 통과: `artifacts/security-final-heap-tests.log`. 장시간 순환 수집에서도 살아 있는 DOM 리스너/비동기 콜백을 유지하고, 1200개 스크립트 뒤 프로토타입 슬롯 수는 115였다.
- PatchedBrowser, PageScriptProbe, CanvasRegression, RuntimeHeapRegression 빌드 통과: `artifacts/security-final-*-build.log`.

실제 페이지 진단은 아직 최종 성공을 확인하지 못했다. 누락됐던 TextEncoder/Canvas 함수의 호출 오류는 사라졌다. 날짜 수정 후 재현 자료 `artifacts/security-after-time/page-script-progress.json`에는 검증 POST status=200, responseChars=779328이 있다. 이후 긴 JavaScript 작업과 `overrunBegin` 메시지를 관측했다. 110초 진단의 `EXECUTION_TIME_LIMIT 1`과 AbortError는 진단 호스트가 설정한 관찰 한도에 따른 중단이며, 서버의 최종 실패 판정이 아니다.

최신 엔진을 사용하는 `tests/bin/x64/Release/PatchedBrowser.exe`도 직접 실행했다. 첫 캡처 `artifacts/security-final-patched-current.png`는 “확인 중” 상태였다. 긴 실행 이후의 `artifacts/security-final-patched-later.png`에서는 “사람인지 확인하십시오” 단계가 표시됐다. 직접 클릭 후 결과는 사용자 확인 대기 중이다. 보안확인이 1~2초 안에 완료되는 동작이나 초록색 성공 표시는 아직 검증되지 않았다. 계정 로그인은 수행하지 않았다.

원래 `bin/Browser/x64/Release/TWebFrameBrowser.exe`는 이번 후속 엔진 변경을 포함하지 않는다. 수정 범위 제한에 따라 확인용 바이너리는 TWebFrame2 아래에 따로 빌드했다. 변경 전 후속 작업 소스 사본은 `artifacts/security-continuation-before/`에 있다.

표준/문서: [Encoding TextEncoder](https://encoding.spec.whatwg.org/#interface-textencoder), [HTML Canvas](https://html.spec.whatwg.org/multipage/canvas.html), [ECMAScript Date.parse](https://tc39.es/ecma262/multipage/numbers-and-dates.html#sec-date.parse), [WinHttpTimeToSystemTime](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttptimetosystemtime), [Cloudflare 오류 코드](https://developers.cloudflare.com/turnstile/troubleshooting/client-side-errors/error-codes/).

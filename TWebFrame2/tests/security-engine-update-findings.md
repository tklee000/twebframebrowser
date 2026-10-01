# 공통 엔진 수정과 실제 보안확인 결과 — 2026-10-01

수정 및 빌드 출력은 모두 `D:\ai_works\miniwebbrowser\TWebFrame2` 안에 있다. 기존 작업은 보존했다. 이번 변경은 공통 실행기, DOM Canvas API와 CSS 레이아웃, 그 회귀검사에 한정했다. 사이트 주소·위젯 ID·화면 문구를 검사하는 엔진 분기, 성공 토큰 생성, 재시도 강제 차단은 추가하지 않았다. Browser 및 WebView2Browser 소스와 원래 bin 실행 파일의 수정 시각은 작업 시작 때와 같다.

최신 확인용 실행 파일은 `tests/bin/x64/Release/EngineBrowser.exe`다. 변경된 공통 엔진에 기존 Browser 소스를 그대로 연결한 Release x64 빌드다. 원래 `bin/Browser/x64/Release/TWebFrameBrowser.exe`는 이 변경을 포함하지 않는다.

## 확인한 원인과 수정

1. **긴 JavaScript 작업이 Windows 창 응답을 막음.** 기존 융합 명령은 정확한 명령 수 배수에만 걸린 중단 지점을 건너뛸 수 있었다. 실행량의 다음 임계값을 넘으면 중단 지점을 확인하도록 바꿨다. 모든 View와 iframe이 기본 메시지 서비스를 사용한다. 창 이동과 페인트를 처리하며 JavaScript 타이머, 입력 및 로딩 완료 작업은 현재 JavaScript 작업이 끝난 뒤 실행해 재진입을 방지한다. 반복 타이머를 합치고 DPI 메시지의 임시 RECT를 복사한다. 작업 종료 훅은 보류 메시지를 다시 예약한다. 종료 메시지, iframe 전달 및 동기 대화상자 입력도 회귀검사했다. 실제 확인용 GUI의 ‘확인 중’ 상태에서 드래그 호출은 191ms에 반환했고 창 원점이 (687,296)에서 (737,346)으로 이동했다.

2. **Canvas 내보내기 API 누락.** 공식 `debug.challenges.cloudflare.com` 페이지에서 `detectCanvasNoise`/`detectAllInterference`가 `TypeError: toDataURL is not a function`으로 중단된 것을 재현했다. HTMLCanvasElement의 공통 `toDataURL`과 `toBlob`을 추가했다. 실제 Canvas 비트맵을 WIC PNG로 인코딩하며 intrinsic 크기, 투명도와 96 DPI 메타데이터를 사용한다. 지원하지 않는 형식은 PNG로 내보낸다. 빈 Canvas는 `data:,` 또는 비동기 null 콜백을 반환한다. 수정 후 공식 페이지의 해당 누락 오류가 사라졌다. 이를 실제 사이트의 성공이 입증됐다는 뜻으로 해석하지 않는다.

3. **반복적인 캡처 배열 조회 비용.** 짧은 함수의 매개변수·숫자 연산·부작용 없는 캡처 조회·마지막 인덱스 읽기를 검증 후 공통 실행 계획으로 처리한다. 변경된 캡처 값과 함수는 매 호출에서 읽는다. 숫자 이외의 변환, 일반 호출 등은 기존 실행기로 돌아간다. 마지막 getter/Proxy 읽기는 한 번 수행하고 예외를 그대로 전달한다. 50만 조회 회귀검사의 최적화 구간은 기존 약 1.6초에서 약 1.0초로 줄었고 체크섬 1500000을 유지했다. 실제 보안확인 시간을 이 수치로 대신하지 않는다.

4. **공통 이진 요청 본문 오류.** XHR·fetch는 ArrayBufferView의 offset/length 대신 전체 backing buffer를 보내거나 16비트 원소당 한 바이트만 보냈다. ArrayBuffer 및 Blob도 문자열로 변환될 수 있었다. Blob 생성과 HTTP 본문이 같은 실제 바이트 스냅샷을 사용하도록 수정했다. ArrayBuffer, 8/16비트 배열과 부분 뷰, DataView, Blob 및 Blob MIME을 검사해 수정 전 실패/수정 후 통과를 확인했다. 실제 사이트의 마지막 검증 POST는 둘 다 문자열이었으므로 이 결함을 해당 사이트 실패의 직접 원인으로 단정하지 않는다.

5. **체크 표시의 좌표 기준 오류.** `transform: rotate(0deg) scale(1)`인 요소도 CSS 기준상 자식의 containing block을 만든다. 기존 레이아웃은 `position`만 확인해 체크 표시의 절대 위치를 바깥 요소 기준으로 계산했다. 이제 변환이 적용되는 가장 가까운 조상의 padding box를 absolute/fixed 자식의 기준으로 사용한다. 변환이 없는 fixed 요소는 계속 viewport를 기준으로 한다. block, inline-block, flex, grid, 생성된 `::after`, fixed inset 갱신을 100%와 150% DPI에서 검사했다. 체크 표시의 실제 픽셀 중심은 100%에서 (30,30.5), 150%에서 (30.3333,30.6667)이었고 박스 중심은 두 경우 모두 (30,30)이었다. 특정 화면이나 ID 보정은 없다.

## 실제 보안확인의 남은 문제

`security-final-observation.log`의 180초 관찰은 호스트의 강제 중단 없이 종료됐다(`EXECUTION_TIME_LIMIT 0`). 검증 POST 두 번은 HTTP 200과 779260/117968 문자 응답을 받았다. 요청 본문은 각각 String 3970/65346 바이트였다. 마지막 iframe은 943071264 명령, 17228100 공통 조회 계획 호출을 실행했다. 약 0.5초, 35.6초, 58.0초에 위젯 초기화를 관찰했고 120.9초에 수동 확인 단계, 178.0초에 widgetStale 이벤트를 받았다. 성공 토큰 및 초록색 성공 체크는 확인하지 못했다.

읽기 전용으로 확인한 현재 사이트의 `sec-turnstile.js`에는 `RESOLVE_DEADLINE_MS = 10000`과 `watchResolve()`가 있다. 10초 후 토큰도 수동 확인 상태도 없으면 `redraw()`가 위젯을 제거하고 다시 만든다. 엔진의 긴 실행이 이 시간 창을 넘는 것이 반복 초기화와 연결된다. 이 사이트 스크립트는 수정하지 않았다. 공식 오류 문서의 110600도 잘못된 시계 또는 너무 긴 검증 시간을 원인으로 제시한다. 시계 회귀검사는 통과했지만 실행 성능을 WebView2의 1~2초 수준으로 맞추는 목표는 아직 미달이다. 사용자 정의 엔진의 서버 측 지원 여부도 성공 실패의 단독 원인으로 입증되지 않았다.

수동 확인을 클릭하지 않은 관찰에서 interactiveTimeout은 사용자 입력 대기 시간 만료를 의미한다. 이를 서버 검증 실패 또는 성공으로 기록하지 않는다. 관찰 도구의 시간 제한 때문에 생긴 AbortError는 별도 `EXECUTION_TIME_LIMIT`으로 구분한다. 보안확인의 자동 성공 문제가 모두 해결됐다고 보고하지 않는다.

## 검증 자료

- `artifacts/transform-containing-block-full-tests.log`: 최종 전체 엔진 검사 통과.
- `artifacts/transform-containing-block-before-tests.log`, `transform-containing-block-final-tests.log`: 공통 transform 좌표 규칙의 수정 전 실패/수정 후 100%·150% 통과.
- `artifacts/checkbox-transform-raster-tests.log`, `checkbox-transform-100.png`, `checkbox-transform-150.png`: 실제 96/144 DPI 픽셀 검사와 이미지.
- `artifacts/security-final-responsiveness.log`: 선택적 호스트 콜백 유무, iframe 작업 순서, 창 이동 메시지, DPI 복사, 무한 작업 종료와 WM_QUIT 검사 통과.
- `artifacts/security-final-read-plan.log`, `security-final-export.log`: 인터프리터/최적화 임계값 0/2 × DPR 1/1.5 검사 통과.
- `artifacts/body-serialization-before-tests.log`, `body-serialization-after-tests.log`: 공통 HTTP 본문 수정 전 실패/수정 후 통과.
- `artifacts/security-final-heap-tests.log`: 살아 있는 DOM/비동기 콜백 및 순환 수집 검사 통과. 최종 JavaScript.cpp는 이 검증 때의 파일과 SHA256이 같다.
- `artifacts/security-compatibility-check/`, `security-compatibility-after-export/`: 공식 페이지의 Canvas API 누락 오류와 수정 후 관찰.
- `artifacts/security-final-observation/`, `security-final-observation.log`: 실제 서버 관찰. 일시적인 서버 URL이 들어 있으므로 원본 로그를 그대로 게시하지 않는다.
- `artifacts/engine-browser-final-build.log`: 최종 확인용 GUI 빌드 성공.
- `artifacts/window-response-before/`: 각 수정 전 소스 스냅샷과 비교용 실행 파일.

공통 동작은 [HTML Canvas 내보내기](https://html.spec.whatwg.org/multipage/canvas.html#dom-canvas-todataurl), [Fetch BodyInit](https://fetch.spec.whatwg.org/#concept-bodyinit-extract), [Blob 생성](https://w3c.github.io/FileAPI/#constructorBlob), [CSS transform 좌표 규칙](https://www.w3.org/TR/css-transforms-1/#transform-rendering)을 참고했다. 서버 오류의 의미는 [Cloudflare 오류 코드](https://developers.cloudflare.com/turnstile/troubleshooting/client-side-errors/error-codes/)를 확인했다.

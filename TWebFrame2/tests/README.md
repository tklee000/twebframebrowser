# 회귀검사

`CSSCompatibilityRegression`은 CSS 구문·선택자·중첩·레이어·변수·수학·색상의
네이티브 계산값과 레이아웃을 100%·150% DPI에서 검사한다. `--webview2`를
지정하면 설치된 WebView2에서 같은 HTML/CSS를 실행해 비교하고 버전과 결과를
`artifacts/css-webview2-*`에 저장한다. 이 fixture 집합은 전체 CSS 지원율이
아니다. [구현 범위와 남은 차이](../../docs/CSS-WEBVIEW2-COMPATIBILITY.md)를 참조한다.

공통 엔진 기능을 검증하는 C++ 검사, JavaScript 회귀 fixture 및 지원 파일을 유지한다. 일회성 사이트 probe·trace·audit·벤치마크 실행기와 임시 브라우저 빌드, 캡처 이미지·프로필·진단 로그를 정리했다.

저장소 루트에서 VS 2019 C++ 빌드 도구로 실행한다.

```powershell
& .\TWebFrame2\tests\Run-SupportCompatibility.ps1 -FullRegression
```

이 명령은 SupportCompatibilityRegression, TWebFrameTests, CSSCompatibilityRegression, ScrollRenderingRegression, TableSpanRegression, FormControlRegression, RuntimeHeapRegression, BrowserContextRegression, StandaloneScriptRegression, ScriptHttpRegression, CanvasRegression, PointerEventRegression의 12개 실행기를 빌드하고 실행한다. TWebFrameTests 다음에는 `platform-integrity-regression.js`도 실행한다. 빌드와 실행 결과는 `tests/bin`, `tests/obj`, `tests/artifacts` 아래에 생성한다. 나머지 `*Regression.vcxproj`는 필요에 따라 개별 실행할 수 있다.

SupportCompatibilityRegression은 문법·동작 28개, 기존 의미 비교 29개, API 동작 30개(WebAssembly 활성화 시 35개)를 100%·150% DPI와 두 JIT 설정에서 확인한다. 참조 fixture는 저장소에 포함되며 과거 진단 수집 데이터에 의존하지 않는다. 실제 GC cleanup, WebAssembly 플래그에 따른 전역 API 노출과 Worker 일관성, OPFS 재개·origin 격리 및 렌더링·hit test도 확인한다. Node.js가 있으면 실행 후 `node .\TWebFrame2\tests\Verify-SupportResults.cjs`로 결과를 참조와 다시 비교할 수 있다.

정규식은 기본적으로 C++ 표준 라이브러리의 `std::wregex`를 사용한다. `src/JavaScript.h`의 `// #define SUPPORT_PCRE2` 주석을 해제하거나 라이브러리와 검사 프로젝트의 전처리기 정의에 `SUPPORT_PCRE2`를 추가하면 PCRE2를 사용한다. 각 엔진의 기대 결과를 별도로 검사하며, 선택된 엔진은 `tests/artifacts/support-regex-backend.txt`에 기록한다. 표준 라이브러리 모드에서는 이름 있는 캡처와 후방 탐색이 SyntaxError가 되고, Unicode 모드는 UTF-16 코드 단위로 동작한다. VS 2019의 표준 정규식은 `m` 없이도 줄바꿈 경계에서 앵커가 일치하는 구현 차이가 있다.

WebAssembly는 기본 비활성화다. `src/JavaScript.h`의 `// #define SUPPORT_WEB_ASSEMBLY` 주석을 해제하거나 라이브러리와 검사 프로젝트의 전처리기 정의에 `SUPPORT_WEB_ASSEMBLY`를 추가한 뒤 다시 빌드하면 활성화된다. 활성화된 검사에서는 Wasm 실행 중단, 모듈 검증·컴파일·실행, JS import 및 메모리 공유도 확인한다.

StandaloneScriptRegression은 기존 standalone 검사 중 회귀검사만 실행한다. 임의 스크립트의 성능 측정 기능은 제거했다. PageIntegrationRegression은 `Run-TurnstileIntegration.ps1`의 공식 테스트 키 통합 검사용으로 유지한다. 수집·archive·임의 DOM 진단 기능은 제거했다. 이 통합 검사는 네트워크에 의존하므로 위 로컬 검사 묶음에는 포함하지 않는다.

ScriptHttpRegression은 로컬 HTTP 서버에서 403·404·503 HTML 문서의 iframe 동기/비동기 로드와 자체 최상위 탐색을 확인한다. 공통 BrowserContext와 브라우저 HttpClient 어댑터 양쪽을 검사하며, HTTP 상태를 유지하는 fetch, 오류 상태의 script/CSS 거절, 최종 리다이렉트 URL 및 실제 전송 실패 시 이전 문서 보존도 확인한다. Cloudflare 원본을 변경하거나 외부 Challenge 서버를 호출하지 않는다.

`platform-integrity-regression.js`는 native 함수 디스크립터, Navigator, Performance, Crypto, Event 신뢰 속성 및 Canvas 픽셀을 100%·150% DPI와 두 JIT 설정에서 검사한다. CanvasRegression은 PNG를 디코딩해 합성 결과를 확인하고, 표시 픽셀이 script readback과 일치하는지도 두 DPI에서 확인한다. PointerEventRegression은 실제 호스트 입력과 script dispatch/click의 신뢰 값 및 좌표를 검사한다. 화면 크기와 작업 영역은 SupportCompatibilityRegression의 DPI 검사에 포함된다.

`cloudflare_gemini.md`의 항목별 판정과 수정 범위는 [검증 결과](../Cloudflare_문서_검증결과.md)에 기록한다. 로컬 표준 호환성 검사 성공을 운영 Cloudflare Challenge 통과로 해석하지 않는다.

보존한 Cloudflare 원본·Wasm·메타데이터·응답 자료의 위치와 해시 검증 결과는 [보존 목록 안내](artifacts/PRESERVED.md)에 있다. Git에는 대용량 수집 자료를 추가하지 않는다. 실제 엔진에 필요한 native vendor와 WebView2 빌드용 NuGet 패키지는 임시 검사 산출물에서 제외한다.

`RuntimeBenchmark.vcxproj`는 같은 `benchmarkRun()` 함수를 native TWebFrame2와 숨겨진 WebView2에서 실행하는 별도의 성능 측정 도구다. `RuntimeBenchmark.exe twebframe|webview2 setup.js output.json [runs] [jit-threshold] [--async]`로 실행하며, 준비 코드를 실행한 뒤 함수 호출의 `performance.now()` 차이와 반환값을 저장한다. `--async`는 Promise 완료까지 기다리고 native 호스트는 View와 같은 Worker 알림/타이머 방식으로 이벤트 루프를 구동한다. 두 엔진 측정 중에만 Windows 타이머 해상도를 1ms로 맞추고 종료 시 복원한다. 로컬 원본·고정 입력 fixture·전후 측정은 `artifacts/long-job-20261002/RESULT.md`와 `artifacts/registered-scripts-20261003/RESULT.md`에 설명한다. 원래 검사 작업의 전체 시간을 추출한 함수의 벤치마크 시간과 혼동하지 않는다.

StandaloneScriptRegression은 추가로 `StringTransformRegressions.h`의 문자 변환 최적화를 확인한다. JIT를 끈 결과와 전체 출력이 같은지 비교하고, UTF-16·속성 접근자·Proxy·교체된 native 함수·배열 외부 참조·정수 범위·취소·호스트 재진입을 검사한다. 문자열 변환 최적화는 안전성이 확인된 바이트코드에 적용하며 사이트 이름이나 보안 스크립트의 특정 함수 이름에 의존하지 않는다.

`EvalWorkerRegressions.h`는 eval 캐시의 strict 구분, 실제 호출자 바인딩, 새 함수/정규식 객체, 프로토타입 수집 및 캐시 축출, TrustedScript의 불변 데이터와 정책 콜백 의미를 확인한다. Worker는 메시지 순서, UTF-16/NUL/고립 surrogate/`__proto__` 데이터, 알림을 통한 전달과 비동기 종료, realm 초기화를 검사한다. ScriptHttpRegression은 실제 View의 Windows 메시지 루프에서 Worker와 타이머 응답도 확인한다.

실제 로그인 페이지의 보안 검사에는 `Run-BoundedSecurity.ps1 -Url '로그인 URL' -OutputDirectory '결과 폴더'`를 사용한다. 먼저 `PageIntegrationRegression.vcxproj`를 빌드한다. 이 모드는 최초 로그인 HTTP 요청 직전부터 10초를 계산하고, 그 안에 부모 창에 실제 `complete` 메시지가 도착하지 않으면 실패(exit 4)로 종료한다. 실행 중인 JavaScript도 호스트 중단 콜백으로 취소하며, 실패 후 스크립트 실행·타이머 처리·추가 대기 시간은 허용하지 않는다. 네이티브 진단 저장과 정리를 포함한 프로세스 watchdog은 11초다. 각 실행은 새 프로세스에서 한 번만 검사하고 `budget-result.json`, `run.log`, 레이아웃과 화면을 저장한다. 실패 후에는 첫 10초의 자료를 분석하고 공통 엔진을 변경한 다음 재검사한다.

`SwitchDispatchRegressions.h`, `StringAppendRegressions.h`, `ByteCopyRegressions.h`는 각각 리터럴 switch 분기, 별칭이 없는 문자열 누적, 순수 문자 읽기와 Uint8Array 복사의 최적화를 검사한다. JIT를 끈 결과와 비교하며 중복 case·fallthrough·예외·문자열 별칭·접근자·Proxy·함수 교체·UTF-16·공유 버퍼·중단·호스트 재진입을 확인한다. ScriptHttpRegression은 iframe을 삽입한 같은 스크립트 안에서 동기/비동기 초기 `about:blank` 주소와 HTTPS 부모의 secure context 및 Crypto API 상속을 검사한다.

`FastPropertyRegressions.h`는 공통 VM의 속성 쓰기와 증감, 멤버 호출 준비 및 숫자 연산 경로를 JIT 0/16에서 검사한다. 키 변환과 RHS의 순서, getter·setter·Proxy·공유 typed buffer, 실제 호출 receiver, 호스트 재진입과 중단을 확인한다. 고정 입력으로 같은 보존 원본 함수를 TWebFrame2와 WebView2에서 실행한 비교와 전체 반환값 검증은 `artifacts/vm-benchmark-20261003/RESULT.md`에 있다. 이 벤치마크를 운영 보안검사 완료 시간으로 해석하지 않는다.

`NativeLoopRegressions.h`는 실행 중 반복된 일반 루프를 x64 기계어로 바꾸는 `src/NativeLoop.inl`을 검사한다. 숫자·비트 연산, 문자 테이블, 작은 함수의 본문 통합, 배열 읽기·숫자 쓰기, 실제 Function.call receiver를 지원한다. 값·배열 범위·객체 참조를 검사하고, 조건이 달라지면 완료한 반복만 반영한 뒤 VM으로 돌아간다. 기계어는 최대 4,096회 반복마다 VM으로 제어를 돌려준다. 키·길이·객체 참조는 실행 프레임에 전달하므로 값이 바뀔 때마다 다시 컴파일하지 않는다. 동적 데이터, 배열 쓰기 후 읽기, 지역 변수 교환, 예외·접근자·호스트 재진입과 중단을 JIT 0/16에서 검증한다. 보존한 서로 다른 원본 스크립트와 WebView2 비교는 `artifacts/native-loop-20261003/RESULT.md`에 기록한다.

보안 검사에서 관측한 반복문의 추가 개선은 `src/NativeLoopCache.inl`과 `artifacts/security-jit-20261003/RESULT.md`에 기록한다. 컴파일된 for/while 루프는 시작 지점에서 JIT에 진입하며 긴 본문은 명령량에 따라 일찍 컴파일한다. 준비 IR은 함수·클로저·속성·실제로 조회한 테이블 항목을 검증한 뒤 재사용한다. 변경이 발견되면 다시 분석하고 안전한 변형을 컴파일한다. `<`, `<=`, `>`, `>=`와 NaN 비교, 새 호출의 변수 재연결, 숫자 데이터 변경·중첩 조회·별칭 쓰기·접근자 교체·재진입·취소를 검사한다. 계획 256개 제한과 실행 코드/준비 IR 각각 8MiB 예산은 유지하며 오래 사용하지 않은 계획을 교체한다. 서로 다른 원문 300개로 제한 이후에도 새 컴파일이 계속되는지 확인한다. 고정 입력 함수의 속도와 실제 10초 보안 완료 여부는 구분한다.

Uint8Array 복사는 SSE2로 UTF-16의 하위 바이트를 변환한다. 호스트 취소 콜백과 Worker 종료 신호가 없는 256KiB 이상 복사만 최대 4개 스레드가 서로 겹치지 않는 숫자 슬롯에 기록한다. 실행 엔진과 JavaScript 객체 생성은 이 스레드에 전달하지 않는다. 호스트 콜백이 있으면 4,096바이트 단위로 제어를 돌려주며, ByteCopyRegressions는 전체 출력 바이트와 재진입·중단을 확인한다.

RuntimeBenchmark의 동기 측정은 선택적인 `benchmarkNormalize(result)`를 시간 측정이 끝난 뒤 두 엔진에서 동일하게 호출한다. typed array의 JSON 표현 차이를 제거하면서 반환한 모든 바이트를 비교할 수 있다. 초기 실행도 버리지 않고 중앙값과 범위를 보고한다.

`RenderingComparisonRegression.vcxproj`와 `Run-RenderingComparison.ps1`은 페이지 JavaScript를 끈 영구 HTML/CSS 문서를 실제 WebView2와 자체 `View` 공통 paint 경로로 비교한다. 현재 20개 pilot을 96/144 DPI와 세 CSS viewport에서 120개 비교하며, DOM·box 좌표·필수 계산 스타일 25개(숨김 요소 포함)·UTF-16 글자 좌표·엄격한 장치 픽셀 차이를 저장한다. native baseline·대표 glyph index/advance/offset·실제 font 이름과 reference 노드별 CDP platform font usage를 보존한다. 기준 per-character baseline/glyph, widget 내부 텍스트·전체 glyph coverage와 AA 교정은 보류한다. 실제 창 DPI와 일치하는 경우 `WM_PRINTCLIENT` 결과도 검사한다. 실패 시 종료 코드 1이며 일반 엔진 회귀검사의 성공과 구별한다. 절차는 [렌더링 검증 안내](rendering/README.md), 최신 결과는 [계산 스타일 검증 결과](../../docs/RENDERING-ACCURACY-STYLE-RESULTS.md)에 있다. `-FullRegression`은 렌더링 비교를 자동 실행하지 않는다.

`-FullRegression`에는 `TableSpanRegression`과 `FormControlRegression`을 포함한다. 12개 실행기와 platform integrity를 실행하며 표의 outer half-border/트랙, native 및 작성자 form appearance와 DPI를 확인한다. TWebFrameTests는 kerning·normal 줄 높이·폼 크기·줄바꿈과 DPI cache 왕복도 검사한다. 판정 도구는 40개 교정으로 검출력을 확인하며 문자 좌표 이동/누락/내용/advance/줄/원본 위치, 중복·매핑 오류, null/NaN/무한 좌표, `.notdef`·미수집 glyph·미해결 font 정보를 거절한다.


정적 페인트 검증은 [rendering 실행 안내](rendering/README.md)와 [전체 CPU 전환 결과](../../docs/RENDERING-ALL-CPU-RESULTS.md)를 따른다. 폰트·그라디언트·도형·그림자와 View 화면/출력 타깃을 모두 CPU 경로로 전환했다. 픽셀 완전 일치를 기본으로 하되 확인·등록된 폰트·그라디언트·도형 안티앨리어싱 CPU/GPU 차이는 허용하며 raw 차이와 strict 판정도 보존한다. 새 차이와 DOM·스타일·레이아웃·문자 기하 오류는 실패한다. `-StrictPixels`로 예외를 끌 수 있다. 판정 교정은 40개다. GPU 내부 최종 View 합성은 후속 업그레이드다. FormControlRegression은 공유 View 래스터 경로에서 최신 WebView2의 select 화살표 빈 공간·양쪽 선·아래 여백을 두 DPI로 검사한다.

ScrollRenderingRegression은 패딩이 있는 일반 상자·pre·textarea에서 짧은 내용의 세로 스크롤바/스크롤 범위가 생기지 않고, 긴 내용의 전체 스크롤 범위가 유지되는지 100%·150% DPI로 검사한다. MdViewer는 이 엔진 회귀검사에서 재빌드하지 않는다.

최신 정적 비교는 WebView2의 CapturePreview/CDP PNG를 120쌍에서 정확히 대조하며, 같은 브라우저의 GPU/색 프로필을 실행 전후에 기록한다. 기본 120/120(96 완전 일치·24 승인 AA), 별도 색상/DPI/viewport 교정 18/18·표본 357/357, 비교 교정 47개와 환경 예외 거절 교정 6개가 통과했다. 이전 기준을 유지하고 새 그래픽 환경을 분리 보존했으며 공통 엔진 소스는 변경하지 않았다. [독립 캡처·환경 교정 결과](../../docs/RENDERING-ACCURACY-CAPTURE-CALIBRATION-RESULTS.md)를 따른다.

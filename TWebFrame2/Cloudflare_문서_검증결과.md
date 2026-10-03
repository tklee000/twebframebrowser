# cloudflare_gemini.md 검증 결과

2026-10-02 현재 소스, 재현 가능한 로컬 회귀검사, 웹 표준 및 Cloudflare 공식 문서를 대조했다. 입력 문서는 특정 실패 요청의 URL·응답·실행 예외·서버 판정 로그를 제시하지 않으므로, 지적한 호환성 문제와 실제 Cloudflare 차단 원인은 따로 판단해야 한다.

이번 수정은 `TWebFrame2`의 공통 JavaScript/DOM/Canvas/레이아웃 엔진과 회귀검사에 한정했다. 앞서 허용받아 수정한 `Browser/HttpClient.cpp`, `Browser/main.cpp`의 403 HTML 문서 로드 변경은 유지한다. 저장한 Cloudflare 원본 스크립트와 Wasm은 수정하거나 삭제하지 않았다.

| 문서 항목 | 검증 결과 | 처리 |
| --- | --- | --- |
| 시스템 TLS 라이브러리와 Chrome의 JA3/JA4 차이 | WinHTTP 사용은 사실이다. 지문이 다르면 반드시 즉시 봇으로 판정된다는 결론은 입증되지 않았다. Cloudflare는 JA3/JA4를 규칙에 사용할 수 있고, JA4는 확장 목록을 정렬한다. | TLS 지문을 Chrome 것으로 바꾸지 않았다. 차단 원인 판정에는 해당 요청의 서버 측 정보가 필요하다. |
| HTTP/2 헤더 순서·대소문자 차이만으로 즉시 차단 | 제공 문서에 실제 HTTP/2 캡처나 판정 근거가 없다. HTTP/2 필드명은 표준상 소문자다. Chrome 고유 전송 순서와의 일치가 HTTP 표준 요구사항은 아니다. | 근거 없이 네트워크 전송 순서를 변경하지 않았다. |
| native 함수 toString·이름·length·프로토타입 불일치 | 검사한 EventTarget native 함수의 identity, native source, name/length 디스크립터 및 Navigator prototype/tag는 수정 전부터 통과했다. 특정 V8 공백까지 같아야 한다는 웹 표준 요구사항은 없다. | 통과한 바인딩은 유지하고 회귀검사에 포함했다. |
| 속성 디스크립터가 기본값으로 열려 있음 | `Event.isTrusted`는 수정 전 writable/configurable 데이터 속성이었다. `Object.defineProperty`도 일반 객체의 non-configurable 제한을 강제하지 않았다. `Reflect.set`은 쓰기가 거절되어도 true를 반환했다. | Event trust를 C++ 내부 상태와 non-configurable getter로 관리한다. 공통 descriptor 처리에 변경 제한, SameValue, 상속 필드/getter 및 accessor 검증을 적용하고 Reflect.set의 실패·receiver·setter 예외 처리를 수정했다. |
| crypto.subtle·PerformanceObserver가 없음 | 현재 구현과 다르다. 네이티브 SHA-1/256/384/512 digest, secure-context 노출 규칙, Performance timeline/observer가 이미 있고 기존 회귀검사도 있다. | 기존 구현과 실제 결과 검사를 유지했다. Web Crypto 전체 알고리즘 지원을 의미하지는 않는다. |
| userAgentData·Permissions·MediaDevices·WebRTC | 현재 엔진에 해당 API 구현이 없는 것은 사실이다. 그러나 이 중 하나가 undefined이면 모든 Challenge에서 즉시 실패한다는 주장에는 근거가 없다. 문서는 어떤 API 호출이 실패했는지 특정하지 않는다. | 미지원 기능으로 남는다. 실제 권한·미디어·통신 기능 없이 지원한다고 광고하는 객체를 추가하지 않았다. 특정 필수 호출이 확인되면 별도의 기능 구현 범위를 정할 수 있다. |
| Canvas가 빈 버퍼/가짜 픽셀을 반환 | 기존 Direct2D/DirectWrite 그리기 및 실제 PNG/RGBA 출력이 확인된다. 다만 `globalAlpha`, `globalCompositeOperation`은 실제 그리기에 적용되지 않았다. | alpha의 기본값·검증·정밀도·save/restore/reset과 26개 표준 합성/블렌딩 모드를 구현했다. 화면 표시, getImageData 및 PNG가 같은 합성 결과를 사용한다. 무효/0 면적 사각형과 숫자 변환도 표준대로 처리한다. |
| 폰트/이모지/서브픽셀 해시가 Chrome과 다름 | 폰트·운영체제·래스터라이저에 따른 차이는 가능한 사실이다. 특정 해시와의 불일치를 표준 위반이나 이번 차단 원인으로 판단할 자료는 없다. | 실제 glyph/pixel 렌더링을 유지한다. Chrome의 픽셀 해시를 흉내 내지 않았다. WebGL 및 모든 Canvas 기능의 완전 구현을 주장하지 않는다. |
| viewport·DOMRect·offsetWidth·DPR·screen 불일치 | 기존 CSS 좌표·픽셀·hit test는 두 DPI에서 통과한다. `screen.availWidth/availHeight`는 창 크기/전체 화면으로 덮어써지고 실제 모니터 작업 영역이 반영되지 않는 결함이 있었다. | 모니터 전체 영역과 작업 영역을 분리하고 DPI를 CSS pixel로 변환한다. 화면 속성은 정수 CSS pixel로 노출하며 resize/runtime reset 후에도 유지한다. |
| Promise/MutationObserver/microtask/timer/rAF 미지원 또는 잘못된 순서 | 이미 큐·체크포인트·타이머·animation-frame 구현과 동작 검사가 있다. 문서에는 실제 순서 오류 재현 사례가 없다. | 기존 비동기 동작 검사로 재검증했다. 실제 요청의 지연 문제까지 부정하는 결론은 아니다. |
| iframe 격리·postMessage·origin/lifecycle 미지원 | 별도 runtime, 동일/교차 출처 검사, 메시지 복제·targetOrigin 검증이 이미 구현되어 있다. 기존 검사와 403 iframe 문서 로드 검사로 확인한다. | 기존 일반 프레임 규칙을 유지하고 검사했다. |
| navigator.webdriver·isTrusted가 조작 가능 | webdriver getter가 누락되어 있었다. 현재 엔진에는 WebDriver 원격 세션 구현이 없다. isTrusted는 실제로 script에서 변경 가능했다. | Navigator prototype에 readonly webdriver=false getter를 추가했다. 호스트 입력은 trusted, script click/dispatch는 untrusted로 유지하며 trust 속성의 쓰기·삭제·재정의를 거절한다. |
| performance.now 정밀도/지터가 Chrome과 달라 실패 | Chrome의 지터 패턴 복제는 표준 요구사항이 아니다. 그러나 일반 문맥에서 native clock의 과도한 정밀도를 그대로 노출하는 문제는 확인됐다. | 공개 시간에 표준의 비격리 문맥 기준인 100µs 정밀도 제한을 적용했다. performance.now/timeOrigin, native Event timestamp, mark/resource/rAF가 공통 시계를 사용한다. 내부 task deadline은 steady clock을 그대로 사용한다. |

## 회귀검사

초기 플랫폼 fixture 14개 중 5개 assertion이 두 DPI·두 JIT 설정 모두에서 실패했다. 이후 고정 픽셀 기준, descriptor 재정의, alpha/composite 상태, 화면 작업 영역 및 공개 시간 정밀도 검사를 추가했다. 시간 정밀도 및 무효 사각형 처리도 수정 전에 실패를 재현했다.

```powershell
& .\TWebFrame2\tests\Run-SupportCompatibility.ps1 -FullRegression
node .\TWebFrame2\tests\Verify-SupportResults.cjs
```

플랫폼 fixture는 65개 항목을 100%·150% DPI와 interpreter/JIT 두 설정에서 검사한다. 전체 묶음은 기존 문법/의미/API, GC, HTTP 문서 로드, iframe·origin, scroll, Canvas PNG·표시 픽셀 및 pointer 입력 검사를 포함한다. 150% 검사는 엔진 DPR, layout scale 및 Direct2D raster target DPI를 1.5배로 설정한다. 이 실행 환경의 실제 모니터 DPI는 96이었다.

최종 결과: 플랫폼 65개 × 4 설정, 전체 회귀 묶음 9개 실행기, Node 참조 비교(문법 24개·의미 29개 × 4 설정) 및 브라우저 Release x64 빌드가 모두 통과했다. 보존 목록의 7,345개 파일은 누락 없이 기존 길이를 유지한다. 실행 로그는 `tests/artifacts/platform-integrity-full-regression.log`, 브라우저 빌드 로그는 `tests/artifacts/platform-integrity-browser-build.log`에 있다.

Cloudflare 공식 안내는 자체/변형 엔진과 embedded browser를 제한 지원 환경으로 설명한다. 로컬 검사 통과는 운영 Managed Challenge/IUAM/Turnstile 통과의 확인이 아니다. 문서의 “복합 차이 때문에 반드시 실패한다”는 결론도 이 소스와 로컬 검사만으로 확정할 수 없다.

## 근거

- [Cloudflare JA3/JA4](https://developers.cloudflare.com/bots/additional-configurations/ja3-ja4-fingerprint/): 지문 사용, JA4 정렬, 규칙 적용.
- [Cloudflare 지원 브라우저](https://developers.cloudflare.com/cloudflare-challenges/reference/supported-browsers/): 자체 엔진·embedded browser의 제한 지원.
- [HTTP/2 필드](https://www.rfc-editor.org/rfc/rfc9113.html#section-8.2): 필드명 소문자 규칙.
- [ECMAScript Function.prototype.toString](https://tc39.es/ecma262/multipage/fundamental-objects.html#sec-function.prototype.tostring): native source 표현.
- [DOM Event.isTrusted](https://dom.spec.whatwg.org/#dom-event-istrusted), [Web IDL LegacyUnforgeable](https://webidl.spec.whatwg.org/#LegacyUnforgeable): 읽기 전용·재정의 제한.
- [WebDriver Navigator 인터페이스](https://w3c.github.io/webdriver/#interface): 원격 세션 상태와 webdriver 값.
- [HTML Canvas 합성](https://html.spec.whatwg.org/multipage/canvas.html#compositing), [Compositing and Blending](https://drafts.csswg.org/compositing-1/): alpha·합성 모드 및 픽셀 계산.
- [CSSOM Screen](https://drafts.csswg.org/cssom-view/#the-screen-interface): 화면과 가용 작업 영역.
- [High Resolution Time](https://w3c.github.io/hr-time/#dfn-coarsen-time): 시간 정밀도 제한과 구현별 지터 선택.

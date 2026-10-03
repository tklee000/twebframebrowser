# Cloudflare 보안 검사 실패 원인 및 브라우저 엔진 결함 분석

Cloudflare의 봇 관리(Bot Management) 및 인터스티셜 보안 검사(Turnstile, Managed Challenge)는 단순한 자바스크립트 실행 여부만을 검사하지 않습니다. 네트워크(L4/L7), 런타임 환경(JS/DOM), 렌더링 파이프라인(Canvas/CSS/Layout), 동작 신호(Event/Timing)에 걸친 수천 개의 지표를 복합 대조하여 실제 주요 브라우저(Chromium, WebKit, Gecko)와의 불일치를 탐지합니다.

제공해주신 소스 파일 구조(`BrowserContext`, `DOM`, `JavaScript`, `Canvas`, `Layout`, `ScriptHttp` 등)를 바탕으로, 자체 구현 브라우저 엔진이 탐지될 수밖에 없는 핵심 취약점과 구조적 한계를 5개 계층으로 나누어 정리합니다.

---

## 1. 네트워크 및 암호화 계층 (TLS / HTTP Fingerprinting)

Cloudflare는 자바스크립트 코드가 실행되기도 전인 **TCP/TLS 핸드셰이크 단계**에서 클라이언트의 진위 여부를 상당 부분 판별합니다.

### 1.1 JA3 / JA4 TLS 지문 불일치
* **동작 원리:** 클라이언트가 `Client Hello` 패킷을 전송할 때 사용하는 TLS 버전, Cipher Suite 순서, Extensions 목록, 타원 곡선(Elliptic Curves), Point Formats 등의 조합으로 해시값(JA3/JA4)을 계산합니다.
* **구현 엔진의 문제:** `ScriptHttp.h`, 소켓 통신 모듈이 시스템 기본 라이브러리(OpenSSL, Schannel, WinINet 등)를 직접 사용하거나 기본 설정값을 그대로 사용할 경우, Chrome이나 Safari와 일치하지 않는 독자적인 TLS 지문이 생성되어 즉시 비정상 클라이언트로 플래깅됩니다.

### 1.2 HTTP/2 및 헤더 지문
* **SETTINGS 프레임 및 윈도우 크기:** 최신 브라우저는 고유한 HTTP/2 초기 설정값(SETTINGS_HEADER_TABLE_SIZE, ENABLE_PUSH, MAX_CONCURRENT_STREAMS 등)과 헤더 압축(HPACK) 패턴을 갖습니다.
* **헤더 순서 및 수도 헤더(`:method`, `:path`, `:authority`, `:scheme`):** 브라우저마다 수도 헤더의 전송 순서가 엄격히 고정되어 있으며, 일반 헤더(`User-Agent`, `Accept-Language`, `Sec-Ch-Ua-*`)의 순서와 대소문자 표기법이 Chrome/Firefox의 표준 시퀀스와 조금만 달라도 봇으로 판정됩니다.

---

## 2. 자바스크립트 환경 및 프로토타입 체인 무결성

Cloudflare의 난독화된 챌린지 스크립트는 런타임의 프로토타입 체인, 네이티브 함수 구현 형태, 속성 디스크립터(Property Descriptors)를 심층 검사합니다.

### 2.1 네이티브 함수 위장 검사 (`toString()` 및 Symbol.toStringTag)
* **네이티브 시그니처 검증:**
  ```javascript
  Function.prototype.toString.call(window.addEventListener);
  // 기대값: "function addEventListener() { [native code] }"
  ```
  자체 바인딩된 C++ JS 래퍼나 경량 엔진의 경우, `toString` 호출 시 공백, 개행 문자 처리, 또는 프로토타입 체인 상의 속성이 실제 V8/JavaScriptCore와 일치하지 않아 탐지됩니다.
* **디스크립터 검증 (`Object.getOwnPropertyDescriptor`):**
  주요 브라우저의 DOM/BOM API는 `writable`, `enumerable`, `configurable` 값이 웹 표준 명세에 정확히 부합해야 합니다. 수동 바인딩된 객체들은 이 플래그가 누락되거나 기본값(`true`)으로 열려 있는 경우가 많습니다.

### 2.2 웹 표준 최신 API 부재
* Cloudflare 스크립트는 다음과 같은 최신 표준 API 세트의 존재와 동작을 전방위로 확인합니다:
  * `window.crypto.subtle` (Web Crypto API)
  * `navigator.userAgentData` (User-Agent Client Hints)
  * `PerformanceObserver`, `performance.getEntriesByType`
  * `Permissions API` (`navigator.permissions.query`)
  * `MediaDevices`, `WebRTC` 객체 및 코덱 열거
* 구현 파일 목록에 해당 API들의 바인딩이 누락되어 있다면 `undefined` 반환 즉시 봇으로 격리됩니다.

---

## 3. 렌더링 파이프라인 및 그래픽 핑거프린팅

Cloudflare는 Canvas, WebGL, 폰트 렌더링 서브시스템을 호출하여 실제 GPU 및 운영체제 래스터라이저가 동작하는지 확인합니다.

### 3.1 Canvas 2D 래스터라이제이션 차이
* **동작 원리:** 챌린지 코드는 숨겨진 `<canvas>`를 생성하고 특정 텍스트, 그라데이션, 이모지(Emoji), 블렌딩 모드를 그린 뒤 `toDataURL()` 또는 `getImageData()`를 통해 픽셀 해시를 추출합니다.
* **구현 엔진의 문제 (`Canvas.h`, `RasterImage.cpp`):**
  * 자체 스포트 렌더러 또는 Skia/Cairo 등의 경량 연동은 OS 표준 글립 앤티앨리어싱(FreeType, DirectWrite, CoreText) 처리 방식과 미세한 서브픽셀 렌더링 차이를 발생시킵니다.
  * Canvas API의 일부 메서드가 스텁(Stub) 처리되어 있거나 빈 버퍼(all-zero)를 반환하면 즉시 실패합니다.

### 3.2 Viewport 및 레이아웃 메트릭 (`Layout.cpp`, `View.cpp`)
* **스크립트의 질의:** `element.getBoundingClientRect()`, `offsetWidth`, `innerHeight`, `devicePixelRatio`, `screen.availWidth` 등을 교차 검증합니다.
* **레이아웃 불일치:** 완벽한 CSS 박스 모델 및 텍스트 메트릭 계산이 지원되지 않아 요소의 크기가 `0x0`으로 계산되거나 스크롤 좌표, 뷰포트 크기가 불일치할 경우 헤드리스(Headless) 환경으로 분류됩니다.

---

## 4. DOM/CSS 및 비동기 이벤트 루프 정밀도

Cloudflare Turnstile은 단순 연산 외에도 실제 브라우징 환경에서 나타나는 타이머 및 마이크로태스크 큐의 정밀성을 측정합니다.

### 4.1 마이크로태스크/매크로태스크 스케줄링 (`Event Loop`)
* `Promise.resolve().then(...)`, `MutationObserver`, `queueMicrotask`, `setTimeout(..., 0)`, `requestAnimationFrame` 간의 실행 순서와 틱(Tick) 지연 시간을 정밀 측정합니다.
* 단일 스레드 루프에서 C++ 이벤트 루프와 JS 엔진 틱 간의 연동이 표준 사양과 미세하게 다를 경우(예: `requestAnimationFrame`이 비활성 탭처럼 멈추거나 간격이 불규칙할 때) 비정상 판정을 받습니다.

### 4.2 인라인 프레임(iframe) 및 격리 컨텍스트 (`BrowserContext.cpp`)
* Turnstile 위젯은 `<iframe>` 내부에서 독립된 실행 컨텍스트를 생성하여 부모 창(`window.parent`)과의 `postMessage` 통신 및 교차 출처(Cross-Origin) 보안 정책을 검사합니다.
* 프레임 간 메시징 전달 지연, Origin 검증 오류, 독립적인 DOM 트리의 라이프사이클 미지원 시 챌린지 토큰 생성이 중단됩니다.

---

## 5. 행동 및 신호 감지 (Interaction & Automation Leak)

* **`navigator.webdriver`:** 자동화 플래그가 `true`이거나, `delete navigator.webdriver` 등을 통해 부자연스럽게 조작된 흔적(프로토타입 체인에 getter가 누락된 경우 등).
* **신뢰할 수 있는 이벤트(`isTrusted`):** 사용자 클릭/터치 시뮬레이션 시 `event.isTrusted` 플래그가 엔진 내부에서 네이티브로 보장되지 않고 스크립트로 조작된 경우 즉각 차단됩니다.
* **고해상도 타이머 노이즈:** Spectre 완화 정책에 따른 `performance.now()`의 부동소수점 정밀도 및 지터(Jitter) 패턴이 Chrome/Safari의 스펙과 다르면 가상화 환경으로 의심받습니다.

---

## 요약 및 결론

| 계층 | 주요 결함 요인 | Cloudflare의 감지 방식 |
| :--- | :--- | :--- |
| **네트워크** | 표준 브라우저와 다른 TLS 스택 / HTTP/2 설정 | JA3/JA4 해시, HTTP/2 프레임 순서 및 헤더 대소문자 대조 |
| **JS 런타임** | Web API 구현 누락 및 프로토타입/디스크립터 불일치 | `crypto.subtle`, Client Hints, `Function.prototype.toString` 검증 |
| **렌더링** | 폰트/서브픽셀 래스터라이징 및 캔버스 해시 불일치 | 숨김 Canvas에 이모지/복합 텍스트 드로잉 후 픽셀 체크섬 비교 |
| **DOM/레이아웃** | 뷰포트 메트릭 계산 오류, CSS 레이아웃 부정확 | `getBoundingClientRect()` 연산 결과 및 요소 가시성 검증 |
| **이벤트 루프** | rAF, MutationObserver, Microtask 타이밍 오차 | 비동기 태스크 스케줄링 시퀀스 및 틱 간격 벤치마킹 |

자체 제작 브라우저 엔진이 Cloudflare를 통과하지 못하는 것은 특정 버그 하나 때문이 아니라, **TLS 지문부터 JS 프로토타입 체인, Canvas 래스터라이저, CSS 레이아웃 엔진 전반이 Chromium/WebKit과 미세하게 다른 복합적인 차이점**들이 누적되어 감지되기 때문입니다.
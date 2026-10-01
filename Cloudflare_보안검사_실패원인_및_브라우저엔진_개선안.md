# Cloudflare 보안검사 실패 원인 분석 및 브라우저 엔진 개선안

## 1. 문서 목적

본 문서는 현재 구현된 TWebFrame 계열 자체 브라우저 엔진이 Cloudflare 보안검사를 통과하지 못하는 주요 원인을 정리하고, 웹 표준 호환성 관점에서 개선해야 할 항목을 우선순위별로 제시하기 위한 문서이다.

핵심 결론은 다음과 같다.

> Cloudflare 보안검사 실패의 원인은 단일한 값이나 특정 헤더 하나의 문제가 아니라, 쿠키·스토리지·iframe·네트워크 세션·브라우저 API가 실제 일반 브라우저와 일관된 형태로 동작하지 않는 구조적 차이 때문이다.

특히 현재 소스에서 가장 직접적으로 확인되는 문제는 `navigator.cookieEnabled = false`이며, 이것만 수정해서는 충분하지 않다.

---

## 2. 핵심 문제 요약

| 중요도 | 문제 | 영향 |
|---|---|---|
| 치명적 | `navigator.cookieEnabled = false` | 브라우저가 쿠키를 사용할 수 없는 환경으로 직접 노출됨 |
| 치명적 | `document.cookie`와 실제 HTTP Cookie Jar가 연결되지 않음 | Challenge 쿠키 및 세션 쿠키 처리 불완전 |
| 치명적 | fetch/XHR과 iframe/script/resource 로더의 네트워크 세션이 분리됨 | 동일 세션·동일 쿠키 컨텍스트 유지가 어려움 |
| 매우 큼 | localStorage/sessionStorage가 실제 브라우저처럼 유지되지 않음 | origin별 상태 유지 실패 가능 |
| 매우 큼 | `postMessage()`가 표준 Structured Clone 방식이 아님 | iframe ↔ parent 통신 호환성 문제 |
| 큼 | WebAssembly, WebGL, IndexedDB 등 주요 Web API 미구현 | 일반 브라우저와 API surface 차이가 큼 |
| 큼 | `TWebFrame/1.0` UA와 `window.chrome.webview`가 동시에 존재 | 브라우저 특성이 서로 모순됨 |
| 중간~큼 | WinHTTP 기반 네트워크 특성이 일반 Chrome/Edge와 다름 | HTTP/TLS/헤더 특성이 실제 브라우저와 다름 |
| 중간 | module script, Worker 관련 지원 부족 | 최신 Challenge 스크립트 호환성 저하 가능 |

---

## 3. 가장 직접적인 문제: navigator.cookieEnabled

현재 구현에서는 다음과 같이 쿠키 지원 여부를 false로 고정하고 있다.

```cpp
navigator.object->props[L"userAgent"] =
    Value::String(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) TWebFrame/1.0");

navigator.object->props[L"platform"] =
    Value::String(L"Win32");

navigator.object->props[L"languages"] =
    ArrayValue({Value::String(language)});

navigator.object->props[L"cookieEnabled"] =
    Value::Bool(false);
```

웹 페이지에서는 다음 코드로 해당 상태를 즉시 확인할 수 있다.

```javascript
navigator.cookieEnabled
```

현재 상태에서는 항상 `false`가 반환된다.

이는 Cloudflare뿐 아니라 일반적인 로그인 시스템, 인증 시스템, 세션 기반 웹 서비스에서도 큰 호환성 문제를 유발할 수 있다.

### 단순히 true로 바꾸면 안 되는 이유

다음과 같이 값만 바꾸는 것은 올바른 해결책이 아니다.

```cpp
navigator.object->props[L"cookieEnabled"] = Value::Bool(true);
```

실제 엔진 내부에 정상적인 쿠키 저장소가 존재하고 다음 기능이 함께 동작해야 한다.

- `Set-Cookie` 응답 헤더 처리
- 요청 시 `Cookie` 헤더 자동 생성
- `document.cookie` getter
- `document.cookie` setter
- Domain
- Path
- Secure
- HttpOnly
- SameSite
- Expires
- Max-Age
- origin 및 site 정책

즉 `cookieEnabled=true`는 실제 기능이 정상 구현된 뒤 그 결과로 노출되어야 한다.

---

## 4. document.cookie 구현 문제

현재 `document.cookie`가 실제 브라우저의 Cookie Jar와 연결되지 않은 구조로 보인다.

예를 들어 다음과 같이 단순 JavaScript 객체 property로 만들어져 있다.

```cpp
documentValue.object->props[L"cookie"] =
    Value::String(L"");
```

이 경우 페이지가 다음 코드를 실행하더라도

```javascript
document.cookie = "test=123";
```

JavaScript 객체의 문자열 값만 바뀔 가능성이 높다.

정상 브라우저에서는 다음 요청 시 자동으로

```http
Cookie: test=123
```

와 같은 헤더가 만들어져야 한다.

따라서 다음 구조가 필요하다.

```text
document.cookie
      │
      ▼
  CookieJar
      │
      ├─ Domain
      ├─ Path
      ├─ Secure
      ├─ SameSite
      ├─ Expiry
      └─ HttpOnly
      │
      ▼
HTTP Request Cookie Header
```

또한 서버의

```http
Set-Cookie: ...
```

응답도 동일한 Cookie Jar로 저장되어야 한다.

---

## 5. 가장 큰 구조적 문제: 네트워크 컨텍스트 분리

현재 브라우저 엔진은 리소스 종류에 따라 서로 다른 네트워크 경로를 사용하는 것으로 보인다.

대략적인 현재 구조는 다음과 같다.

```text
                         ┌─ fetch/XHR ─ ScriptHttpSession ─ WinHTTP
웹 페이지 ──────────────┤
                         ├─ script src ─ ResourceLoader
                         ├─ iframe src ─ ResourceLoader
                         ├─ CSS ───────── ResourceLoader
                         └─ image ─────── BinaryResourceLoader
```

이 구조의 가장 큰 문제는 다음과 같다.

- iframe이 받은 쿠키를 fetch가 모를 수 있음
- script 요청에서 생성된 세션 상태가 XHR에 전달되지 않을 수 있음
- top-level navigation과 iframe이 동일한 쿠키 정책을 공유하지 않을 수 있음
- origin별 인증 상태가 분산될 수 있음

정상적인 브라우저는 개념적으로 다음 구조를 가진다.

```text
                    BrowserNetworkContext
                             │
        ┌────────────┬───────┼────────┬──────────┐
     Navigation    iframe   script   fetch      XHR
        │             │       │        │          │
        └─────────────┴───────┴────────┴──────────┘
                             │
                       Shared CookieJar
```

따라서 장기적으로는 `BrowserNetworkContext` 같은 공통 계층을 만드는 것이 가장 중요하다.

---

## 6. 권장 BrowserNetworkContext 구조

예시 구조는 다음과 같다.

```cpp
class BrowserNetworkContext
{
public:
    HttpResponse Request(const HttpRequest& request);

    CookieJar& Cookies();
    HttpCache& Cache();

    void SetUserAgent(const std::wstring& ua);
    void SetAcceptLanguage(const std::wstring& language);

private:
    CookieJar cookieJar_;
    HttpCache cache_;
    HttpSession session_;
};
```

그리고 다음 모든 기능이 동일한 컨텍스트를 사용하도록 변경한다.

```text
Navigate
iframe
script
CSS
image
fetch
XMLHttpRequest
Worker
```

예:

```cpp
View
 └─ BrowserNetworkContext
       ├─ CookieJar
       ├─ HTTP Session
       ├─ Cache
       └─ Security Policy

JavaScript Runtime
 ├─ fetch
 └─ XMLHttpRequest
       │
       └────────────── BrowserNetworkContext

ResourceLoader
 ├─ script
 ├─ iframe
 ├─ image
 └─ stylesheet
       │
       └────────────── BrowserNetworkContext
```

---

## 7. localStorage / sessionStorage 문제

현재 `localStorage`와 `sessionStorage`는 새 JavaScript 객체로 만들어지는 형태이다.

예:

```cpp
auto storage = ObjectValue(ObjectKind::Storage);
global->values[L"localStorage"] = storage;

auto sessionStorage = ObjectValue(ObjectKind::Storage);
global->values[L"sessionStorage"] = sessionStorage;
```

이 방식에서는 navigation이나 runtime 재생성 시 storage가 초기화될 가능성이 높다.

정상 브라우저에서는 `localStorage`가 origin별로 유지되어야 한다.

예:

```text
https://example.com
    └─ localStorage

https://cloudflare.com
    └─ localStorage
```

### 필요한 구조

```cpp
class StoragePartition
{
public:
    LocalStorage& GetLocalStorage(const Origin& origin);
    SessionStorage& GetSessionStorage(
        const BrowsingContextId& context,
        const Origin& origin);
};
```

`localStorage`는 navigation을 넘어 유지되어야 하며,
`sessionStorage`는 브라우징 컨텍스트 단위로 관리되어야 한다.

---

## 8. Storage API 완전성

최소한 다음 API가 필요하다.

```javascript
localStorage.length
localStorage.key(index)
localStorage.getItem(key)
localStorage.setItem(key, value)
localStorage.removeItem(key)
localStorage.clear()
```

`sessionStorage`도 동일하다.

추가적으로 다음 부분도 고려해야 한다.

- origin isolation
- quota
- storage event
- cross-frame 동작
- navigation 이후 유지 여부

---

## 9. postMessage 구현 문제

현재 `postMessage()` 처리 과정에서 메시지를 JSON 형태로 변환하는 방식이 사용되는 것으로 보인다.

개념적으로 다음과 같다.

```cpp
const auto data = r.Json(a[0]);

r.EnqueueTask([&r, node, object, data, origin] {
    if (node) {
        if (r.frameMessageSink)
            r.frameMessageSink(node, data, origin);
    }
});
```

하지만 웹 표준의 `window.postMessage()`는 JSON serialize/deserialize 기반이 아니다.

표준 브라우저는 **Structured Clone Algorithm**을 사용한다.

따라서 다음 타입들도 고려해야 한다.

- Array
- Object
- Date
- ArrayBuffer
- TypedArray
- Blob
- Map
- Set
- transferable object

또한 다음 속성도 중요하다.

```javascript
event.data
event.origin
event.source
```

그리고 호출 측에서는

```javascript
window.postMessage(message, targetOrigin)
```

의 `targetOrigin` 검증이 정확해야 한다.

---

## 10. iframe 구조 개선 필요

Cloudflare Turnstile과 같은 시스템에서는 cross-origin iframe 통신이 매우 중요하다.

필요한 핵심 개념은 다음과 같다.

```text
Window
 ├─ Document
 ├─ Location
 └─ WindowProxy
```

cross-origin iframe에서는 부모 페이지가 iframe 내부 DOM을 직접 접근할 수 없어야 하지만,
허용되는 `postMessage()` 통신은 정상적으로 동작해야 한다.

따라서 다음 항목의 표준 호환성이 중요하다.

- WindowProxy
- iframe.contentWindow
- parent
- top
- frames
- postMessage
- MessageEvent
- origin
- same-origin policy

---

## 11. 브라우저 API surface 부족

현재 엔진은 이미 상당히 많은 기능을 구현하고 있으나,
현대 일반 브라우저와 비교하면 Web API surface 차이가 크다.

현재 부족하거나 구현 여부를 확인해야 할 대표 API는 다음과 같다.

```text
WebAssembly
WebGL
WebGLRenderingContext
WebGL2RenderingContext
IndexedDB
AudioContext
RTCPeerConnection
navigator.permissions
navigator.userAgentData
navigator.plugins
navigator.mimeTypes
navigator.maxTouchPoints
navigator.deviceMemory
navigator.mediaDevices
ServiceWorker
SharedWorker
BroadcastChannel
MessageChannel
visualViewport
PerformanceObserver
```

반대로 이미 구현된 것으로 보이는 기능에는 다음이 있다.

```text
Canvas 2D
WebCrypto 일부
Promise
fetch
XMLHttpRequest
Worker 일부
MutationObserver
IntersectionObserver
ResizeObserver
requestAnimationFrame
Intl 일부
```

---

## 12. User-Agent와 브라우저 특성 불일치

현재 User-Agent는 다음과 비슷하다.

```text
Mozilla/5.0 (Windows NT 10.0; Win64; x64) TWebFrame/1.0
```

동시에 JavaScript 환경에서는 다음 객체가 노출된다.

```javascript
window.chrome
window.chrome.webview
window.twebframe
```

이 조합은 Chrome, Edge, 일반 WebView2, Firefox, Safari 중 어느 환경과도 완전히 일치하지 않는다.

### 중요

User-Agent를 Chrome 문자열로 바꾼다.

예:

```text
Mozilla/5.0 ... Chrome/...
```

로 표시하면서 실제 환경이

```javascript
WebGL === undefined
WebAssembly === undefined
indexedDB === undefined
navigator.userAgentData === undefined
```

이면 더욱 큰 불일치가 발생한다.

따라서 브라우저를 가장하는 방향보다는,
실제 웹 표준 지원 범위를 확장하는 방향이 바람직하다.

---

## 13. WinHTTP 기반 네트워크 차이

현재 fetch/XHR 계층에서 WinHTTP를 사용한다.

예:

```cpp
WinHttpOpen(
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) TWebFrame/1.0",
    ...
);
```

WinHTTP는 일반 Chromium의 네트워크 스택과 다르다.

차이가 발생할 수 있는 영역은 다음과 같다.

- TLS handshake 특성
- HTTP/2 구현
- connection reuse
- header ordering
- compression negotiation
- HTTP cache
- proxy 처리
- certificate handling
- Fetch Metadata headers
- Client Hints
- redirect 정책

이것은 단순히 HTTP 헤더 몇 개를 추가하는 것으로 완전히 같아지는 문제가 아니다.

따라서 자체 엔진에서는 목표를
"Chrome과 동일하게 보이기"보다
"HTTP 및 Fetch 표준을 정확하게 구현하기"로 두는 것이 적절하다.

---

## 14. 개선 우선순위

### 1단계: CookieJar 구현

가장 먼저 구현해야 한다.

필수 기능:

```text
Set-Cookie parser
Cookie request header
document.cookie getter
document.cookie setter
Domain
Path
Secure
HttpOnly
SameSite
Expires
Max-Age
```

그리고:

```cpp
navigator.cookieEnabled == true
```

는 실제 CookieJar가 정상적으로 동작한 이후 활성화한다.

---

### 2단계: BrowserNetworkContext 통합

다음 모든 요청을 동일한 네트워크 세션으로 통합한다.

```text
Navigate
iframe
script
stylesheet
image
fetch
XHR
Worker
```

구조:

```text
             BrowserNetworkContext
                    │
          ┌─────────┼─────────┐
          │         │         │
       CookieJar   Cache   HttpSession
          │
          ├─ Navigation
          ├─ iframe
          ├─ script
          ├─ CSS
          ├─ image
          ├─ fetch
          └─ XHR
```

---

### 3단계: StoragePartition 구현

origin 기반으로 storage를 분리한다.

```text
StoragePartition
 ├─ LocalStorage
 │    └─ origin별 지속
 │
 └─ SessionStorage
      └─ browsing context별 지속
```

---

### 4단계: iframe / postMessage 표준화

필요 기능:

```text
Structured Clone
targetOrigin
MessageEvent.origin
MessageEvent.source
WindowProxy
same-origin policy
cross-origin iframe
```

---

### 5단계: 최신 JavaScript/Web Platform 기능 확장

우선순위가 높은 기능:

1. WebAssembly
2. IndexedDB
3. WebGL
4. MessageChannel
5. Performance API
6. Permissions API
7. Worker 보완
8. module script
9. Service Worker
10. WebSocket 세부 호환성

---

## 15. 추천 클래스 구조

전체 엔진을 다음과 같이 계층화하는 것을 권장한다.

```text
BrowserContext
│
├─ BrowserNetworkContext
│   ├─ HttpSession
│   ├─ CookieJar
│   ├─ HttpCache
│   ├─ CertificateManager
│   └─ ProxyManager
│
├─ StoragePartition
│   ├─ LocalStorageManager
│   ├─ SessionStorageManager
│   └─ IndexedDBManager
│
├─ SecurityContext
│   ├─ Origin
│   ├─ SameOriginPolicy
│   ├─ CSP
│   ├─ CORS
│   └─ MixedContent
│
└─ BrowsingContext
    ├─ Window
    ├─ Document
    ├─ Frame
    └─ WindowProxy
```

---

## 16. 개발 순서 제안

실제 개발 시 다음 순서가 효율적이다.

### Phase 1

```text
CookieJar
document.cookie
navigator.cookieEnabled
```

### Phase 2

```text
BrowserNetworkContext
Navigation
iframe
script
fetch/XHR 통합
```

### Phase 3

```text
localStorage
sessionStorage
StoragePartition
```

### Phase 4

```text
postMessage
MessageEvent
WindowProxy
same-origin policy
```


## 17. Cloudflare 대응 관점에서 중요한 원칙

Cloudflare 통과 자체를 목표로 특정 탐지 로직을 우회하려 하기보다는,
일반 웹 브라우저로서 다음 표준 동작을 정확히 구현하는 것이 바람직하다.

```text
Cookie
Storage
Origin
CORS
iframe
postMessage
Navigation
Fetch
HTTP cache
Web APIs
```

이 부분이 정상 구현되면 Cloudflare뿐 아니라
다음 종류의 사이트 호환성도 동시에 개선된다.

- 로그인 사이트
- OAuth 인증
- 금융 사이트
- CDN 기반 사이트
- SPA
- iframe 기반 인증
- 결제 페이지
- SSO 시스템

---



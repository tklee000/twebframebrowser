# Cloudflare 보안검사 실패 원인 분석 및 브라우저 엔진 개선안

> 2026-10-02 구현 상태: 사용자와 확정한 이번 범위는 16절의 Phase 1~4이다.
> 아래 1~17절은 개선 전 진단과 설계 제안으로 보존한다.
> 실제 반영 내용, 검증 결과와 남은 범위는 18절에 기록한다.

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

## 18. Phase 1~4 구현 및 검증 결과 (2026-10-02)

### 18.1 이번 반영 범위

16절의 네 단계에 해당하는 쿠키, 공통 네트워크, 저장소, 프레임 통신을 구현했다.
14절의 WebAssembly·IndexedDB·WebGL·Service Worker 등 신규 플랫폼 API는 별도 장기 확장 범위로 남긴다.

| 단계 | 반영한 동작 | 주요 코드 |
|---|---|---|
| Phase 1 | 실제 CookieJar와 document.cookie를 연결하고 cookieEnabled를 활성화 | [CookieJar.h](TWebFrame2/src/CookieJar.h), [BrowserContext.cpp](TWebFrame2/src/BrowserContext.cpp) |
| Phase 2 | navigation, iframe, script, CSS, image, fetch/XHR가 같은 HTTP 세션과 쿠키 저장소 사용 | [BrowserContext.h](TWebFrame2/include/TWebFrame/BrowserContext.h), [ScriptHttp.h](TWebFrame2/src/ScriptHttp.h), [HttpClient.cpp](Browser/HttpClient.cpp), [ResourceScheduler.cpp](Browser/ResourceScheduler.cpp), [View.cpp](TWebFrame2/src/View.cpp) |
| Phase 3 | origin별 localStorage와 탭별 sessionStorage, navigation 이후 유지, storage 이벤트 | [BrowserContext.cpp](TWebFrame2/src/BrowserContext.cpp), [JavaScript.cpp](TWebFrame2/src/JavaScript.cpp) |
| Phase 4 | Structured Clone 기반 postMessage, targetOrigin, 실제 MessageEvent.source, WindowProxy와 origin 접근 검사 | [JavaScript.cpp](TWebFrame2/src/JavaScript.cpp), [View.cpp](TWebFrame2/src/View.cpp) |

공개 `View::SetBrowserContext`로 프로필을 공유하고, 같은 탭의 하위 프레임은 동일한 sessionStorage namespace를 사용한다.
HTTP 리소스 콜백 `SetNetworkResourceLoader`에는 요청을 시작한 문서의 origin, referrer, 부모 프레임을 포함한 siteForCookies가 전달된다.
기존 로컬 파일·압축 리소스용 콜백은 계속 사용할 수 있다.
HTTP용 기존 콜백을 직접 제공하는 외부 호스트는 새 콜백으로 이전하거나 동일한 BrowserContext를 사용해야 한다.

### 18.2 쿠키와 네트워크

- 여러 Set-Cookie 응답 필드와 document.cookie 쓰기가 동일한 CookieJar를 갱신한다.
- host-only/Domain, default Path와 경계 매칭, Secure, HttpOnly, SameSite, Expires, Max-Age, 쿠키 prefix를 처리한다.
- ICANN과 PRIVATE 영역을 포함한 Mozilla Public Suffix List 10,334개 규칙으로 공용 도메인에 대한 쿠키 설정을 제한한다.
- SameSite는 scheme과 등록 가능 도메인으로 비교한다. 다른 사이트를 포함한 프레임의 ancestor chain은 opaque site 정책을 사용한다.
- script는 HttpOnly 쿠키를 읽거나 덮어쓰지 못하며, 비보안 요청이 기존 Secure 쿠키를 같은 경로에 덮어쓰는 것도 제한한다.
- Expires는 HTTP 날짜의 일반 형식과 과거 형식을 처리하며, Max-Age가 우선한다. 만료 시 저장소와 cache용 쿠키 generation을 함께 갱신한다.
- fetch의 omit/same-origin/include와 XHR의 withCredentials를 적용한다. Set-Cookie는 CORS 응답 필터링 전에 처리하되 script에는 노출하지 않는다.
- WinHTTP 자체 쿠키와 자동 redirect 처리를 끄고 엔진이 직접 관리한다. 상대 redirect URL, POST→GET 전환, 최대 20회 redirect, origin 변경 시 Authorization 제거를 적용한다.
- iframe redirect는 최종 응답 URL을 문서 URL과 리소스 base로 사용한다. redirect 이전 URL로 origin 검사를 통과시키지 않는다.
- HTTP와 navigator가 동일한 TWebFrame UA를 사용한다. standalone 브라우저는 embedded 앱의 chrome.webview/twebframe 브리지를 비활성화한다.

리소스 캐시는 명시적인 max-age만 사용하며 no-store/no-cache, Vary, 만료된 Age/Date, 잘못된 freshness 값은 재사용하지 않는다.
쿠키 generation, 요청 origin과 ancestor site 정책을 cache key에 포함해 인증 상태가 바뀐 응답을 재사용하지 않는다.
최상위 navigation은 리소스 캐시에서 제공하지 않는다.

PSL 갱신은 Windows PowerShell에서 실행할 수 있으며, 생성 파일에 원본 SHA256과 MPL 2.0 라이선스 정보를 남긴다.

~~~powershell
.\tools\Update-PublicSuffixList.ps1
~~~

### 18.3 저장소와 프레임 통신

localStorage는 scheme/host/port를 정규화한 origin별로 공유한다. IDN과 IPv6도 동일한 origin 정규화 경로를 사용한다.
standalone 브라우저의 프로필 위치는 `%LOCALAPPDATA%\TWebFrameBrowser\Profile`이며,
localStorage는 `LocalStorage` 하위의 origin hash 파일에 저장한다.
저장한 origin을 파일 내부에서도 확인하고 임시 파일 교체 방식으로 기록한다.
프로필 경로 없이 생성한 BrowserContext는 메모리 저장소를 사용한다.

sessionStorage는 origin과 최상위 탭 namespace로 분리하고 reload와 같은 탭의 navigation 뒤에도 유지한다.
탭을 닫으면 해당 namespace를 해제한다. length/key/getItem/setItem/removeItem/clear,
named property 접근, Object.keys와 삭제를 지원한다. origin별 용량은 key/value의 UTF-16 크기 합계 5 MiB이며
초과 쓰기는 기존 값을 바꾸지 않고 QuotaExceededError를 반환한다.
opaque origin의 저장소 접근은 SecurityError로 제한한다.
다른 문서에는 비동기 StorageEvent를 보내며, 같은 값 재설정과 쓰기를 실행한 문서에는 이벤트를 보내지 않는다.

postMessage는 JSON 변환 대신 메시지를 호출 시점에 복제한다.
순환 참조, Date, Map/Set, ArrayBuffer, TypedArray/DataView의 공유 backing buffer와 offset, Blob/File 등 엔진이 지원하는 clone 타입을 보존한다.
ArrayBuffer transfer는 수신 데이터가 준비된 뒤 송신 측 buffer/view를 detach하며, 실패한 clone/중복 transfer는 기존 buffer를 유지한다.
targetOrigin은 전송 호출 시 검증하고 실제 전달 시 수신 문서의 origin과 다시 비교한다.
MessageEvent의 origin/source/ports와 prototype을 설정하며, 중첩 iframe에서도 실제 발신 창의 WindowProxy를 사용한다.

다른 origin의 document/name 읽기·쓰기·삭제를 제한하고 iframe.contentDocument는 null을 반환한다.
같은 origin의 parent 창에는 실제 문서·전역 값·메서드 접근을 전달한다.
창 종료 뒤의 proxy와 navigation으로 origin이 바뀐 realm 값도 검사한다.

### 18.4 빌드와 회귀검사

Release|x64 기준으로 엔진 solution과 standalone 브라우저를 빌드했다.
기존 `TWebFrameTests`, 신규 `BrowserContextRegression`, 실제 loopback HTTP 서버를 사용하는
`ScriptHttpRegression`, 장시간 GC 검사 `RuntimeHeapRegression`으로 검증한다.

최종 실행 결과는 BrowserContextRegression 51건과 ScriptHttpRegression 36건 모두 통과,
기존 TWebFrameTests 전체 통과, RuntimeHeapRegression 통과다.
아래 명령은 저장소 루트에서 실행하며, 개별 회귀검사 프로젝트는 solution 빌드가 만든 동일한 엔진 라이브러리를 사용한다.

`BrowserContextRegression`은 쿠키 속성/PSL/SameSite, origin·탭 격리, profile localStorage 재생성,
storage 이벤트/예외/용량, JIT threshold 0과 2의 typed-buffer clone, transfer 실패 시 원자성,
WindowProxy의 origin 제한과 메시지 identity를 포함한다.
`ScriptHttpRegression`은 실제 navigation→쿠키→XHR/fetch, credential/CORS,
redirect 쿠키와 URL, 동기/비동기 iframe redirect의 최종 origin, script/CSS/image 요청 정책과 cache를 검사한다.

~~~powershell
$solutionRoot = (Get-Location).Path + '\'
msbuild .\TWebFrame.sln /t:Build /p:Configuration=Release /p:Platform=x64 /m
.\bin\x64\Release\TWebFrameTests.exe
.\TWebFrame2\tests\bin\x64\Release\BrowserContextRegression.exe

msbuild .\TWebFrame2\tests\ScriptHttpRegression.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$solutionRoot" /p:BuildProjectReferences=false /m
.\TWebFrame2\tests\bin\x64\Release\ScriptHttpRegression.exe

msbuild .\TWebFrame2\tests\RuntimeHeapRegression.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$solutionRoot" /p:BuildProjectReferences=false /m
.\TWebFrame2\tests\bin\x64\Release\RuntimeHeapRegression.exe
~~~

### 18.5 Cloudflare 공식 테스트 키 통합 검증

[Cloudflare 공식 testing 문서](https://developers.cloudflare.com/turnstile/troubleshooting/testing/)의
always-pass/always-fail 공개 테스트 키만 사용한다.
브리지를 끈 `--native` 환경에서 실제 api.js와 iframe을 로드해 다음 결과를 확인했다.

| 테스트 키 | 콜백 | iframe 메시지 |
|---|---|---|
| 1x00000000000000000000AA | TEST_ONLY\|pass\|success\|dummy=true | init → requestExtraParams → translationInit → food → complete |
| 2x00000000000000000000AB | TEST_ONLY\|fail\|error\|code=600010 | init → requestExtraParams → translationInit → food → fail |

두 경우 모두 document.readyState=complete, 런타임 오류 없음, 실행 제한 미도달, pendingPromises=0을 확인했다.
iframe.document 진단 접근에 발생한 SecurityError는 다른 origin의 문서를 보호하는 정상 동작이다.
재실행 스크립트는 프로세스 시작 여부뿐 아니라 DOM 콜백 값, 실제 iframe 메시지와 런타임 상태까지 검증하며 테스트 서버를 종료한다.

~~~powershell
$solutionRoot = (Get-Location).Path + '\'
msbuild .\TWebFrame2\tests\PageScriptProbe.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$solutionRoot" /p:BuildProjectReferences=false /m
.\TWebFrame2\tests\Run-TurnstileIntegration.ps1
~~~

### 18.6 현재 한계와 다음 범위

공식 테스트 키의 성공은 실제 서비스의 Cloudflare 보안검사 통과를 뜻하지 않는다.
실제 보호 사이트의 통과 여부는 이번 검증에서 확정하지 않았다.
UA·TLS를 다른 엔진으로 위장하거나 탐지값을 강제로 바꾸는 동작은 추가하지 않았다.

현재 쿠키는 BrowserContext 수명 동안 메모리에 공유하며, 브라우저 재시작 이후의 쿠키 파일 복원은 추가하지 않았다.
localStorage의 디스크 지속과 sessionStorage의 탭 수명은 구현되어 있다.
MessagePort transfer, WebAssembly, IndexedDB, WebGL, Permissions, module script,
Service Worker 및 WebSocket 등 14절의 신규 API 확장은 후속 범위다.
WinHTTP의 TLS/프록시/인증서 처리는 Windows 동작을 사용한다.
이 검증 결과를 전체 HTML/Fetch/Web Platform 표준 적합성의 증명으로 해석해서는 안 된다.

구현 기준: [HTTP Cookies](https://httpwg.org/http-extensions/draft-ietf-httpbis-rfc6265bis.html),
[Public Suffix List](https://publicsuffix.org/list/public_suffix_list.dat),
[Fetch](https://fetch.spec.whatwg.org/),
[Web Storage](https://html.spec.whatwg.org/multipage/webstorage.html),
[Structured Data](https://html.spec.whatwg.org/multipage/structured-data.html),
[Web Messaging](https://html.spec.whatwg.org/multipage/web-messaging.html).



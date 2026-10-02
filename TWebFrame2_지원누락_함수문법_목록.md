# TWebFrame2 지원 누락 함수·문법 목록

검사일: 2026-10-02. 현재 작업 폴더의 JavaScript 엔진으로 검사하고 같은 검사 코드를 WebView2 154.0.4258.48에서 비교했습니다.

저장된 `.js` 파일 3,790개를 확인했습니다. WebAssembly 메타데이터에 대응하는 빈 파일 9개를 제외한 JavaScript 원본은 3,781개, 고유 SHA-256은 1,286개입니다. 동일한 원본도 페이지/Worker 환경이 다르면 따로 검사하여 총 1,289건을 검사했습니다. 최신 10회 수집본과 이전 수집본, 저장된 진단·벤치마크 원본을 포함합니다.

**확인된 결과: 문법 미지원 2항목, 동작 오류 6항목, 미구현 공개 함수·API 경로 19개입니다.** API 경로는 `window.File`과 `File`처럼 같은 항목의 별칭을 합친 수입니다. 단순히 미정의 속성이 조회됐다는 이유만으로 누락 목록에 넣지 않았습니다.

## 문법 미지원

|항목|최소 재현 코드|현재 엔진|WebView2|
|---|---|---|---|
|배열 구조 분해의 나머지 요소|`var [a, ...rest] = [1, 2, 3];`|`Expected binding name near '...'` 파싱 오류|`a=1`, `rest=[2,3]`|
|클래스 메서드 이름으로 `async` 사용|`class A { async(){ return 5; } } new A().async();`|`Expected class member name near '(' after 'async'` 파싱 오류|`5`|

두 번째 항목은 `async function` 지원 여부와 다른 문제입니다. 이름이 `async`인 일반 메서드를 수정자로 해석합니다. 저장된 WebView2 내부 브리지 소스에서도 같은 실패를 재현했습니다. 이 내부 브리지 소스가 Cloudflare 페이지 코드라는 뜻은 아닙니다. 일반적인 async/await 검사는 통과했습니다.

## 문법은 처리하지만 동작이 다른 항목

|항목|재현 코드 또는 조건|현재 엔진|WebView2|
|---|---|---|---|
|미정의 변수 참조의 예외|선언하지 않은 `__corpusDefinitelyMissingName` 읽기|`undefined`를 반환|`ReferenceError` 발생|
|`const` 재할당 금지|`const value=1; value=2;`|재할당 허용|`TypeError` 발생|
|상속 클래스의 `super` 메서드 호출|`class B extends A { get(){ return super.get()+1; } }`|`get is not a function` 예외|기반 메서드를 호출해 `5` 반환|
|정규식 `u` 플래그의 Unicode 문자 처리|`/^.$/u.test('\ud83d\ude00')`|`false`|`true`|
|정규식 이름 있는 캡처 그룹|`new RegExp('(?<word>a)').exec('a').groups.word`|매칭 결과가 `null`이라 속성 읽기 예외|`"a"`|
|정규식 후방 탐색|`new RegExp('(?<=a)b').test('ab')`|`false`|`true`|

`super` 재현의 전체 코드:

```javascript
class A {
    constructor(v) { this.v = v; }
    get() { return this.v; }
}
class B extends A {
    get() { return super.get() + 1; }
}
new B(4).get();
```

## 미구현 JavaScript 표준 함수·속성

다음 항목은 비교한 WebView2 환경에 존재하지만 현재 엔진에서는 `undefined`입니다.

|함수·속성|확인된 누락|
|---|---|
|`Object.getOwnPropertyDescriptors`|함수 미구현|
|`Reflect.getOwnPropertyDescriptor`|함수 미구현|
|`FinalizationRegistry`|생성자 미구현|
|`RegExp.$1`|기존 정규식 정적 캡처 속성 미구현|

`RegExp.$1`은 함수가 아닌 기존 호환성 속성입니다. `$2`~`$9`까지 별도로 검사했다는 의미는 아닙니다.

## 미구현 브라우저 함수·API

|함수·API 경로|확인된 누락|
|---|---|
|`AudioContext`|생성자|
|`File`|생성자; `window.File`도 동일|
|`FileList`|생성자/인터페이스|
|`MessageChannel`|생성자|
|`OffscreenCanvas`|생성자|
|`WebAssembly`|전역 객체|
|`WebAssembly.Module`|생성자|
|`WebAssembly.compile`|함수|
|`WebAssembly.instantiate`|함수|
|`document.featurePolicy`|기존 정책 API 객체|
|`navigator.sendBeacon`|함수|
|`navigator.storage`|StorageManager 객체|
|`navigator.storage.getDirectory`|OPFS 디렉터리 접근 함수|
|`trustedTypes`|전역 정책 객체; `window`/`self` 별칭도 동일|
|`trustedTypes.createPolicy`|정책 생성 함수|

여기에는 WebView2에서 제공하는 플랫폼 API와 기존 호환성 API가 포함됩니다. 모든 브라우저가 반드시 같은 API를 제공하거나, 이 목록의 모든 API가 Cloudflare 검증에서 필수라는 뜻은 아닙니다. `window.chrome`, 브라우저 UI의 `toolbar`, 수집기가 만든 `__stageMessages`는 위 공개 API 누락 수에서 제외했습니다.

**실제 보안 스크립트에서 직접 확인한 누락:** Cloudflare Worker 원본 3종이 `navigator.storage.getDirectory()`를 호출하다 `Cannot read properties of undefined` 예외를 발생시켰습니다. 빈 페이지의 DOM이나 jQuery 의존성과 구분하여 실제 Worker 환경에서 재현했습니다.

재현 원본 예시: [Cloudflare Worker 원본](<D:/ai_works/miniwebbrowser/TWebFrame2/tests/artifacts/cloudflare-recapture-20261002-123000/webview2/visit-01/scripts/47-950173ED84624982E4ECC3E4A569C230-4.js>).

이 API만 구현하면 보안확인을 통과한다는 의미는 아닙니다. 독립 실행 검사는 서버의 최종 판정 기준까지 증명하지 않습니다.

## 검사 범위와 결과를 읽는 방법

- 실제 엔진 Compiler로 전체 원본과 함수 본문을 컴파일했습니다. 고유 원본/환경 1,289건 중 1,287건이 컴파일됐습니다. 실패 2종 중 1종은 위 `async()` 파싱 문제이며, 다른 1종은 V8에서도 구문 오류가 나는 기존 진단 원본입니다.
- 독립 실행 완료 1,275건, API 누락을 재현한 Worker 3건, 환경·라이브러리 의존 예외 9건입니다. jQuery 미주입, 앞선 검사에서 만든 변수 부재, Node의 `require`, Turnstile 로더의 `currentScript` 부재 등을 엔진 표준 함수 누락으로 계산하지 않았습니다.
- 604개 정적 API 경로를 비교했습니다. 공개 API 누락은 별칭 포함 22경로, 별칭을 합치면 위 표의 19경로입니다.
- 의미 검사 29개 중 위 문법·동작 차이 8개를 확인했습니다. 나머지 21개 비교 검사는 통과했습니다.
- 이전 수집의 Worker 원본 4개는 캐시에 없어 검사하지 못했습니다. 최신 10회 수집의 원본 누락과 구분했습니다.
- 미호출 함수의 모든 분기, 모든 이벤트 입력, 추가 서버 응답까지 실행한 것은 아닙니다. 모든 저장 원본을 검사했다는 사실이 모든 실행 경로의 완전한 호환성을 보장하지는 않습니다.

## WebAssembly 빈 파일과 수집기 수정

기존 수집기는 CDP `Debugger.getScriptSource` 응답에서 `scriptSource`만 저장했습니다. CDP는 WebAssembly의 경우 이 문자열을 비우고 별도 `bytecode` 필드에 Base64 바이너리를 반환하므로, 저장된 빈 `.js`는 모듈이 비어 있다는 뜻이 아닙니다. [CDP 공식 응답 정의](https://raw.githubusercontent.com/ChromeDevTools/devtools-protocol/master/json/js_protocol.json).

수집기를 수정하여 WebAssembly를 `.wasm` 바이너리와 별도 메타데이터로 저장합니다. 로컬 페이지와 Worker에서 각각 8바이트 모듈을 컴파일해 2개 모두 정상 저장되는 것을 검증했습니다. 기존 방문의 빈 `.js` 파일은 원본 기록으로 보존했으며, 과거 바이너리를 자동 복구했다는 의미는 아닙니다.

## 상세 증거

- [전수 검사 보고서](<D:/ai_works/miniwebbrowser/TWebFrame2/tests/artifacts/script-corpus-audit-20261002/README.md>)
- [파일별 검사 결과](<D:/ai_works/miniwebbrowser/TWebFrame2/tests/artifacts/script-corpus-audit-20261002/file-results.json>)
- [원본별 결과와 실패 호출](<D:/ai_works/miniwebbrowser/TWebFrame2/tests/artifacts/script-corpus-audit-20261002/source-results.json>)
- [문법·동작 비교 결과](<D:/ai_works/miniwebbrowser/TWebFrame2/tests/artifacts/script-corpus-audit-20261002/semantic-differences.json>)
- [함수·API 경로별 결과와 참조 원본](<D:/ai_works/miniwebbrowser/TWebFrame2/tests/artifacts/script-corpus-audit-20261002/api-differences.json>)
- [수집기 WebAssembly 저장 검증](<D:/ai_works/miniwebbrowser/TWebFrame2/tests/artifacts/script-corpus-audit-20261002/wasm-capture-check/summary.json>)

이 파일은 확인된 지원 누락과 동작 오류 목록입니다. 해당 기능들의 구현 수정은 포함하지 않습니다.

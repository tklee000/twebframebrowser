# 2026-10-02 입력 및 Shadow DOM 후속 진단

실제 뽐뿌 사이트에서 `2.png`의 성공 상태는 아직 검증하지 못했다. 이번에 확인한 성공은 Cloudflare가 공개한 **공식 테스트 키**를 사용하는 별도 로컬 페이지의 성공이다. 테스트 토큰은 운영 서비스의 인증에 사용할 수 없다.

## 스크립트 진행을 막던 API 누락

`turnstile-integration-test.html?case=pass`를 최신 엔진으로 실행했을 때, 첫 35초 관찰에서는 완료 콜백이 없었다. 자식 문서의 로그에는 다음 호출 실패가 있었다.

```text
TypeError: getElementById is not a function
getElementById: undefined | receiver=type=5,kind=11,tag=#document-fragment
```

실행 시간 제한은 0이었고 자식 문서의 최장 작업 CPU 시간은 46ms였다. 따라서 이 테스트에서 스크립트 진행이 멈춘 문제는 느린 계산이나 진단 호스트의 강제 중단으로 설명되지 않는다.

공통 DOM 바인딩에 `DocumentFragment.prototype.getElementById`를 구현했다. `ShadowRoot`는 이를 상속한다. 해당 트리의 실제 자식을 전위 순회하고, 첫 번째 일치 항목을 반환한다. ID를 선택자로 해석하지 않고 대소문자를 구분한다. 중첩 ShadowRoot 경계는 넘지 않는다. 인수 개수, DOMString 변환, 원래 변환 예외와 수신자 검사를 적용했다. 문서의 기존 `getElementById`에도 누락됐던 필수 인수 및 문자열 변환 검사를 추가했다.

20개 의미 검사가 인터프리터/JIT 임계값 0/2 × DPR 1/1.5에서 통과했다. 변경 전에는 네 조합 모두 메서드 누락으로 실패했다. 자료는 `artifacts/security-shadow-id-before.log`와 `security-shadow-id-after.log`다.

## 실제 입력 경로 수정

수정 전 실행 파일에서 마우스 버튼을 누르는 순간 `pointerdown,mousedown,click` 순서가 나타났다. 버튼을 놓기 전에 클릭이 실행됐으며 클릭 좌표도 입력 위치를 전달받지 않았다. JavaScript의 `.click()`은 `isTrusted=true`를 반환하고 포커스를 옮겼다. 취소된 체크박스 클릭도 체크 값을 변경했다.

`View.cpp`와 `JavaScript.cpp`의 공통 동작을 수정했다.

- 마우스 클릭은 `pointerdown → mousedown → pointerup → mouseup → click` 순서로 발생한다. 실제 클릭에 CSS 좌표, 화면 좌표, 수정 키와 클릭 횟수를 전달한다.
- 누른 대상과 놓은 대상이 다를 때에는 공통 조상에 클릭을 전달하며, 포인터 캡처가 있으면 캡처 대상을 사용한다. `pointerup` 콜백에서 제거된 대상은 활성화하지 않는다. 두 번째 클릭의 `dblclick`도 버튼을 놓은 후 발생한다.
- 포커스, 텍스트 선택 시작과 선택 팝업은 누르기 단계에서 처리한다. 놓기 단계에서 텍스트 선택을 다시 초기화하지 않는다.
- `.click()`은 신뢰되지 않은 클릭이고, 포커스를 옮기지 않는다. 비활성화된 컨트롤 및 같은 요소의 재귀 활성화를 검사한다.
- 클릭 취소 시 체크박스·라디오 상태를 복원하고 change 이벤트를 발생시키지 않는다.
- 클릭의 포인터 전용 필드는 표준 기본값을 사용하고 실제 마우스에서 온 pointerId/pointerType은 보존한다. 키보드 컨텍스트 메뉴는 마우스 포인터로 기록하지 않는다.
- 누름 대상은 weak_ptr로 보관한다. 페이지 교체·포커스 이탈·캡처 해제 때 상태를 정리한다.

이전 작업에서 미완료 상태로 남아 있던 공통 이벤트 프로토타입, timeStamp, composed, 마우스 압력·크기 값도 이번 최신 소스 빌드로 검증했다. 원래 실행 파일을 사용한 첫 비교에는 이전 엔진 버전 차이가 포함되어 있으므로 모든 실패 항목을 이번 소스 변경만의 전후 효과로 해석하지 않는다.

## 공식 테스트 키 검증

공식 API 스크립트와 iframe을 그대로 로드하고, 로컬 fixture에서 문서화된 공개 테스트 키를 사용했다. 실제 서비스의 페이지나 키는 변경하지 않았다.

| 시험 | 결과 |
| --- | --- |
| 성공 테스트 키 | 1942ms에 `TEST_ONLY\|pass\|success\|dummy=true` 및 complete 이벤트 |
| 실패 테스트 키 | 1917ms에 예상된 `TEST_ONLY\|fail\|error\|code=600010` 및 fail 이벤트 |
| 두 시험의 호스트 실행 제한 | 모두 0 |
| 수정한 자식 문서의 API 호출 실패 | 두 시험 모두 없음 |

성공 화면은 `artifacts/security-official-test-pass-after/page-script-render.bmp`다. 화면 자체에 공식 테스트라는 안내가 표시되며, 초록색 성공 상태와 dummy 토큰 완료 콜백을 함께 확인했다. 실패 자료는 `security-official-test-fail-after.log`다. 테스트 키의 결과로 실제 사이트의 보안검사 통과율이나 완료 시간을 추정하지 않는다.

로컬 서버는 시험 후 종료했다. 필요하면 다음 명령으로 같은 fixture를 실행할 수 있다.

```powershell
node D:\ai_works\miniwebbrowser\TWebFrame2\tests\turnstile-test-server.cjs 8768
```

주소는 `http://127.0.0.1:8768/turnstile-integration-test.html?case=pass` 또는 `?case=fail`이다. 이 페이지는 테스트 전용이다.

## 처리 지연에 대한 증거와 한계

사용자가 수동 확인 후 실패했던 기존 실제 실행의 최장 작업은 CPU 12687ms / 벽시계 12719ms였다. 자식 런타임의 전체 스크립트 CPU 시간은 56781ms이고 실행 명령 수는 990748415였다. CPU 시간과 실제 경과 시간이 매우 가까우므로 이 작업은 주로 실행기의 계산 비용을 소비했다. 단순 네트워크 대기만으로 설명할 수 없다.

동일 로그에는 overrunBegin, overrunEnd, interactiveEnd 뒤 600010이 있고 호스트 실행 제한은 0이다. 따라서 이 실패는 진단 호스트의 관찰 마감으로 중단된 결과가 아니다. Cloudflare 문서에서 600*는 일반 검사 실패이며, 110600의 검사 시간 초과 및 110620의 사용자 상호작용 시간 초과와 구분한다. 처리 지연은 확인됐지만 최종 600010의 유일한 원인이라는 증거는 없다. 공식 실패 테스트 키에서도 짧은 실행 후 같은 코드가 반환된다.

`summarize-page-diagnostic.cjs`를 추가해 이벤트, 최종 실패 코드, 호스트 중단 여부, 프레임별 CPU·벽시계 시간, 호출 실패를 정리하도록 했다. HTTP 200, 숨겨진 DOM 성공 문자열과 운영 서비스의 성공을 동일하게 판단하지 않는다. 기존 실사이트 요약은 `artifacts/security-input-previous-summary.json`이다.

## 최종 검증 및 산출물

- 전체 엔진 검사 통과: `artifacts/security-shadow-id-suite.log`.
- DOM ID 20개 검사 × 4조합 통과: `artifacts/security-shadow-id-after.log`.
- Event 인터페이스 × 4조합 통과: `artifacts/security-input-event-interface.log`.
- 실제 Win32 클릭 순서·캡처·포커스·취소·재귀 활성화 시험 통과: `artifacts/security-input-final-tests.log`. 실제 모니터 DPI는 96이었다. 공통 레이아웃과 이벤트 메타데이터는 별도로 DPR 1/1.5에서 검사했다.
- 장시간 힙 및 살아 있는 DOM/비동기 콜백 검사 통과: `artifacts/security-input-heap.log`. 1200개 스크립트 후 prototypeSlots=117.
- 최신 확인용 실행 파일: `D:\ai_works\miniwebbrowser\TWebFrame2\tests\bin\verified-input\TWebFrameBrowser.exe`.
- 최종 소스·라이브러리·실행 파일 해시: `artifacts/security-input-final-hashes.json`.

소스 수정과 산출물은 모두 TWebFrame2 아래에 한정했다. Browser/HttpClient.cpp, Browser/main.cpp, WebView2Browser/main.cpp의 이번 작업 시작 시점 대비 해시는 동일하다. 기존 다른 수정 사항은 유지했다.

실제 뽐뿌 로그인 페이지를 최신 브라우저로 여는 동작은 도구의 자동 승인 검토에서 차단됐다. 반환된 이유는 `blocked by policy`이며, 구체적인 사유는 제공되지 않았다. 해당 동작은 다시 실행하지 않았다. 따라서 최신 수정본의 실제 사이트 성공 여부는 미검증 상태다.

기존에 확인한 요청 경로별 User-Agent 불일치, 서로 다른 네트워크 세션과 쿠키 저장소의 제한도 남아 있다. 또한 사용자 정의 엔진은 Cloudflare의 제한 지원 대상이다. 이번 공통 API 및 입력 수정만으로 실제 서비스의 성공을 보장할 수 없다.

참고: [DOM NonElementParentNode](https://dom.spec.whatwg.org/#interface-nonelementparentnode), [Pointer Events](https://www.w3.org/TR/pointerevents3/#event-attributes), [공식 테스트 키](https://developers.cloudflare.com/turnstile/troubleshooting/testing/), [오류 코드](https://developers.cloudflare.com/turnstile/troubleshooting/client-side-errors/error-codes/), [지원 브라우저](https://developers.cloudflare.com/cloudflare-challenges/reference/supported-browsers/).

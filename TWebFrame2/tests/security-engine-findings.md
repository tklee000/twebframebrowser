2026-10-01 보안확인 실행기 진단 및 수정 결과

이번 작업의 소스와 빌드 출력은 모두 `TWebFrame2` 안에 있습니다. 기존 작업 내용을 유지하고, 사이트 주소·화면 ID·보안확인 함수 이름에 따른 엔진 분기를 추가하지 않았습니다. 로그인 주소는 저장되어 있던 재현 자료의 `https://www.ppomppu.co.kr/zboard/login.php`를 사용했습니다.

공통 JavaScript 컴파일러에서 닫는 블록의 깊이를 한 단계 낮게 기록하는 오류를 재현했습니다. 조건 분기가 `EndBlock` 명령으로 이동하면 점프 처리에서 해당 블록을 먼저 제거하고, `EndBlock`에서 바깥 블록까지 제거했습니다. 중첩 블록의 `let` 값이 `42`에서 `undefined`로 바뀌는 작은 프로그램으로 변경 전 실패를 확인했습니다. 실제 보안확인 진단에도 `$for_values`와 `$for_index` 변수가 사라진 기록이 있었습니다. 명령 실행 전 깊이를 기록하도록 수정해 중첩 분기, 재귀 직렬화, 반복문, catch/finally의 스코프를 보존했습니다.

후속 단계에서 실제로 누락된 `Node.COMMENT_NODE`, `CanvasRenderingContext2D.prototype.measureText`, `createRadialGradient` 호출도 확인했습니다. Node 표준 상수를 생성자와 프로토타입에 제공하고, 일반 JavaScript 대입 규칙으로 읽기 전용 데이터 속성과 setter 없는 접근자를 처리했습니다. 명시적으로 정의한 자체 속성은 정상적으로 갱신됩니다.

Canvas 글자 측정은 DirectWrite의 글리프 배치, 진행 폭, 잉크 경계와 글꼴 지표를 사용합니다. 측정과 그리기는 같은 글꼴·공백 처리·기준선 계산을 공유합니다. `TextMetrics`는 실제 네이티브 측정값을 보관하고 프로토타입의 읽기 전용 접근자와 수신자 검증을 제공합니다. 방사형 그라데이션은 두 원의 기하, 생성 시 변환, 색상 정지점과 투명 영역을 계산해 실제 Canvas 픽셀에 그립니다. 좌표는 Canvas 고유 공간에서 계산한 뒤 CSS 크기와 모니터 DPI를 적용합니다.

검증 결과:

- Release x64 빌드와 전체 `TWebFrameTests` 1,178개 검사 통과.
- `security-runtime-regression.js`의 변경 전 실패 및 변경 후 통과 확인.
- 100%·150% 배율에서 분기/스코프, API 수신자·반사·읽기 전용 속성 검사 통과.
- `CanvasRegression`에서 96·144 DPI 렌더 타깃의 방사형 색상·위치와 측정된 글리프 경계 대 실제 픽셀 경계 비교 통과.
- `RuntimeHeapRegression`의 장시간 순환 수집과 살아 있는 DOM·비동기 콜백 보존 검사 통과.

실제 보안확인은 아직 최종 성공을 확인하지 못했습니다. 변경 전에는 검증 POST가 HTTP 400을 받고 `600010` 실패 이벤트가 발생했습니다. 스코프 수정 후에는 POST 200과 다음 검증 단계 진행을 관측했고, Canvas API 추가 후 누락된 두 Canvas 함수의 호출 오류도 사라졌습니다. 그러나 긴 후속 실행과 재시도가 140초 관찰 한도를 넘었습니다. 진단 프로그램은 이때 호스트 실행 중단으로 종료하며, 이 중단을 보안확인 성공이나 서버의 최종 판정으로 해석할 수 없습니다. 통과 토큰과 실제 계정 로그인은 검증되지 않았습니다.

확인용 브라우저 빌드: `bin/x64/Release/PatchedBrowser.exe` (이 문서가 있는 tests 디렉터리 기준).

진단 자료는 `artifacts/security-runtime-before.log`, `artifacts/security-release-tests.log`, `artifacts/security-dpi-raster-tests.log`, `artifacts/security-heap-tests.log`, `artifacts/security-baseline/`, `artifacts/security-after-block/`, `artifacts/security-after-canvas/`에 보관했습니다. 작업 시작 시점의 해당 소스 사본은 `artifacts/security-before/`에 있습니다.

공통 규칙 확인에 사용한 표준: [ECMAScript 블록 평가](https://tc39.es/ecma262/multipage/ecmascript-language-statements-and-declarations.html#sec-block-runtime-semantics-evaluation), [HTML Canvas 및 TextMetrics](https://html.spec.whatwg.org/multipage/canvas.html).

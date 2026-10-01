2026-10-01 공통 JavaScript 호출 경로 개선

이번 변경의 엔진 소스는 `TWebFrame2/src/JavaScript.cpp`다. 특정 URL, 화면, DOM ID, 보안검사 함수 이름을 검사하는 분기를 추가하지 않았다. 기존 DOM/CSS/레이아웃 수정은 보존했다. 테스트·빌드 설정·출력·진단 자료도 TWebFrame2 안에 있다.

공통 변경:

- 같은 타입의 JavaScript 값을 슬롯에 대입할 때 활성 저장소만 갱신한다. 문자열의 짧은/긴 저장소 전환, 원래 객체의 정체성, 외부 realm 소유권과 순환 수집기의 내부 참조 스냅샷을 유지한다.
- 일반 함수 호출과 생성자 호출의 인수 벡터 용량을 재사용한다. 재귀마다 별도 저장소를 대여하며 반환 및 예외 경로에서 값을 전부 지운다. 용량 64 이하인 빈 벡터를 최대 64개만 보관한다. 콜백과 클로저에 전달된 객체는 기존 소유권으로 유지된다.
- 기존 숫자 JIT가 컴파일한 호출 없는 함수는 VM 스택의 숫자 인수를 바로 사용한다. getter는 인수 평가 전에 얻은 값을 사용하며 다시 호출하지 않는다. 루프, 문자열/객체 변환, 생성자, 외부 realm, generator, 진단 추적 및 지원하지 않는 함수는 기존 호출 경로를 따른다. 호출 깊이 제한과 호스트 체크포인트를 유지한다.
- 확인용 `tests/PatchedBrowser.vcxproj`의 실행 파일 이름을 `TWebFrameBrowser`로 고정했다. 전역 TargetName을 명령줄에서 변경하던 방식은 라이브러리 이름도 변경하고 증분 빌드가 기존 실행 파일을 그대로 남길 수 있어 사용하지 않는다. 전체 재빌드에서 TWebFrame.lib를 연결한 새 실행 파일 생성과 수정 시각을 확인했다.

실행 파일: `D:\ai_works\miniwebbrowser\TWebFrame2\tests\bin\x64\Release\TWebFrameBrowser.exe`

Release x64 검증:

- 전체 TWebFrameTests 통과: `artifacts/common-call-final-full-tests.log`.
- getter 호출 순서, 숫자/Boolean 반환, NaN/Infinity/-0, 문자열 및 객체 변환, 함수 교체, Proxy, this, 재귀, 예외, 생성자, arguments와 기본 인수 검사 통과. JIT 임계값 0/2 × DPR 1/1.5의 네 조합에서 체크섬 57294624 유지: `artifacts/common-call-paired-benchmark.log`.
- 변경 전/후를 순차로 두 번 비교한 로컬 반복 호출 측정에서 최적화 경로 평균은 100%에서 1651.5 → 1485.5ms, 150%에서 1661 → 1455.5ms였다. 약 10~12% 감소다. 순수 인터프리터 경로에서는 일정한 속도 개선을 확인하지 못했다. 이 벤치마크를 실제 보안검사 완료 시간으로 해석하지 않는다.
- 기존 메서드 호출·인덱스 조회 회귀검사 네 조합 통과: `artifacts/common-call-method-tests.log`.
- 숫자 JIT 호출이 반복되는 작업의 호스트 체크포인트, 창 타이틀 메시지, iframe/타이머/입력 순서, 종료 및 DPI RECT 수명 검사 통과: `artifacts/common-call-responsiveness.log`.
- RuntimeHeapRegression의 장시간 순환 수집, 살아 있는 DOM 리스너와 async 콜백, 1200개 스크립트 뒤 프로토타입 슬롯 회수 검사 통과: `artifacts/common-call-heap-tests.log`.
- CanvasRegression의 실제 96/144 DPI 렌더 타깃과 체크 표시 좌표 검사 통과: `artifacts/common-call-dpi-raster-tests.log`. 전체 검사에서도 block/inline-block/flex/grid의 transform containing block 규칙이 100%와 150%에서 통과했다.
- 확인용 GUI 전체 재빌드 성공: `artifacts/common-call-browser-build.log`. 최종 실행 파일 크기는 7204864바이트이고 수정 시각은 2026-10-01 21:20:36 KST다. 최종 소스·실행 파일 SHA256은 `artifacts/common-call-final-hashes.json`에 있다.
- Browser/main.cpp, WebView2Browser/main.cpp 및 사용자가 지정한 원래 두 실행 파일의 SHA256은 작업 시작과 같다: `artifacts/common-call-scope-verification.json`.

실제 보안검사의 1~2초 완료 및 초록색 성공 체크는 이번 작업에서 확인하지 못했다. 외부 페이지를 실행하는 PageScriptProbe 백그라운드 진단이 자동 승인 검토에서 “blocked by policy”로 거부됐으며 구체적인 이유는 제공되지 않았다. 이후 검증은 로컬 엔진 검사로 수행했다. 이전 진단에서 오래 걸린 JavaScript와 재초기화를 관찰한 자료는 그대로 보존했다. 이번 변경만으로 실제 보안검사가 해결됐다고 결론 내리지 않는다.

공통 호출 순서는 [ECMAScript EvaluateCall](https://tc39.es/ecma262/multipage/ecmascript-language-expressions.html#sec-evaluatecall)을 참고했다. 메시지 처리 관련 확인은 [Microsoft PeekMessageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-peekmessagew)를 참고했다.

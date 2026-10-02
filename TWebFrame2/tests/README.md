# 회귀검사

공통 엔진 기능을 검증하는 C++ 검사, JavaScript 회귀 fixture 및 지원 파일을 유지한다. 일회성 사이트 probe·trace·audit·벤치마크 실행기와 임시 브라우저 빌드, 캡처 이미지·프로필·진단 로그를 정리했다.

저장소 루트에서 VS 2019 C++ 빌드 도구로 실행한다.

```powershell
& .\TWebFrame2\tests\Run-SupportCompatibility.ps1 -FullRegression
```

이 명령은 SupportCompatibilityRegression, TWebFrameTests, ScrollRenderingRegression, RuntimeHeapRegression, BrowserContextRegression, StandaloneScriptRegression을 빌드하고 실행한다. 빌드와 실행 결과는 `tests/bin`, `tests/obj`, `tests/artifacts` 아래에 생성한다. 나머지 `*Regression.vcxproj`는 필요에 따라 개별 실행할 수 있다.

SupportCompatibilityRegression은 문법·동작 24개, 기존 의미 비교 29개, API 동작 34개를 100%·150% DPI와 두 JIT 설정에서 확인한다. 참조 fixture는 저장소에 포함되며 과거 진단 수집 데이터에 의존하지 않는다. 실제 GC cleanup, Wasm 실행 중단, OPFS 재개·origin 격리 및 렌더링·hit test도 확인한다. Node.js가 있으면 실행 후 `node .\TWebFrame2\tests\Verify-SupportResults.cjs`로 결과를 참조와 다시 비교할 수 있다.

StandaloneScriptRegression은 기존 standalone 검사 중 회귀검사만 실행한다. 임의 스크립트의 성능 측정 기능은 제거했다. PageIntegrationRegression은 `Run-TurnstileIntegration.ps1`의 공식 테스트 키 통합 검사용으로 유지한다. 수집·archive·임의 DOM 진단 기능은 제거했다. 이 통합 검사는 네트워크에 의존하므로 위 로컬 검사 묶음에는 포함하지 않는다.

보존한 Cloudflare 원본·Wasm·메타데이터·응답 자료의 위치와 해시 검증 결과는 [보존 목록 안내](artifacts/PRESERVED.md)에 있다. Git에는 대용량 수집 자료를 추가하지 않는다. 실제 엔진에 필요한 native vendor와 WebView2 빌드용 NuGet 패키지는 임시 검사 산출물에서 제외한다.

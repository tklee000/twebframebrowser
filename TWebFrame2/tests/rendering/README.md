# 정적 렌더링 비교 실행 안내

현재 corpus는 변경하지 않고 보존한 HTML/CSS 문서 **20개**다. 각 문서를 실제 WebView2와 TWebFrame `View`에서 96/144 DPI, 세 CSS viewport로 렌더링한다. 페이지 JavaScript는 끈다. WebView2의 읽기 전용 계측과 CDP는 기준 수집에만 쓰며 JS DOM 제어 검증에 포함하지 않는다.

현재 판정은 DOM, 추적 요소의 border rect, 필수 계산 스타일 25개(새 schema v4), 원본 UTF-16 위치별 공백 이외 글자의 font-box rect, 장치 픽셀 전체의 정확한 BGRA 비교다. 새 캡처는 `text-diff.json`, native 줄 baseline 및 대표 shaped glyph/font 정보를 보존하며 미수집·미해결·`.notdef`와 비정상 좌표를 거절한다. `reference-fonts.json`에는 CDP의 실제 노드별 font usage를 보존한다. reference per-character glyph/기준 baseline, 다중 glyph cluster와 widget 내부 텍스트 coverage는 남아 있다. 픽셀 완전 일치를 기본으로 하되 사용자 승인에 따라 확인된 폰트·그라디언트·도형 안티앨리어싱 CPU/GPU 차이는 [backend-pixel-exceptions.json](backend-pixel-exceptions.json)에 등록하여 허용한다. 비페인트 검사가 통과하고 양쪽 디코딩 픽셀 해시·차이 수치·문서·DPI·viewport가 등록값과 같아야 `PASS_BACKEND_DIFFERENCE`로 통과한다. raw 차이는 보존하고 새 차이나 비페인트 오류는 실패한다. 전체 계약은 [comparison-contract.json](comparison-contract.json), 이전 독립 캡처 실행의 **120/120 성공: 픽셀 완전 일치 96쌍·승인 도형/그라디언트/그림자 AA 24쌍, 문자 기하 24/24·WebView2 캡처 경로 120/120 일치**는 [독립 캡처·환경 교정 결과](../../../docs/RENDERING-ACCURACY-CAPTURE-CALIBRATION-RESULTS.md)에 있다. [전체 CPU 전환 결과](../../../docs/RENDERING-ALL-CPU-RESULTS.md), [앞선 래스터 결과](../../../docs/RENDERING-ACCURACY-RASTER-RESULTS.md), [최초 78/120](../../../docs/RENDERING-ACCURACY-PILOT-RESULTS.md), [table/DPI 결과](../../../docs/RENDERING-ACCURACY-CONTINUATION-RESULTS.md)도 유지한다. 통과는 현재 활성 검사 범위이며 WebView2의 모든 CSS 지원을 뜻하지 않는다.

이전 래스터 실행 `20261003-232442-397-6a3e5f39`은 **102/120 통과·18 페인트 실패**, 문자 기하 **24/24 통과**다. 이전 통과 84쌍을 유지하고 Latin·혼합 문자·대시보드 18쌍이 새로 통과했다. 추가 재현은 27문서/54쌍이며 20 통과·34 실패다. [최신 래스터 결과](../../../docs/RENDERING-ACCURACY-RASTER-RESULTS.md)의 범위와 남은 실패를 따른다. 아래의 이전 실행 자료는 그대로 보존한다.

이전 `20261004-050655-292-8ce5100e`는 CPU 그라디언트 생성·합성 경로이며 strict 114/120과 그라디언트 6쌍의 일회성 승인 기록을 그대로 보존한다. 이전 `20261004-cpu-font-final-1791061810315`에서는 폰트도 CPU에서 합성한다. 최신 `20261004-all-cpu-scroll-final`은 엔진의 GPU 업로드·합성·읽기 경로를 제거하고 소프트웨어 화면 타깃을 사용하며, 코드 보기의 불필요한 세로 스크롤 범위도 수정한다. 사용자의 최신 지시로 확인된 폰트·그라디언트 차이를 등록하여 새 실행에서 같은 차이가 재현되면 성공 처리하도록 변경했다. GPU 내부 최종 합성은 후속 업그레이드다. 저장 실행기와 Skia DLL/라이선스의 SHA-256은 `environment.json`에서 검사한다. 별도 최소 재현 54쌍의 이전 strict 23 통과/31 실패는 이번 120쌍에 합산하지 않는다.

## 최신 계산 스타일 검증

`20261004-required-styles-v8-final2`는 schema v4에서 숨겨진 요소를 포함한 필수 계산 스타일 25개를 비교한다. 계약 버전이나 값이 빠지면 실패한다. 원본 120/120 성공(84 exact·기존 승인 36), 필수 값 17,400개·누락 0, 고의 오류 134개, 전체 회귀 12개와 platform integrity가 통과했다. 자체 픽셀은 이전 120쌍과 같다. 최신 reference는 NVIDIA GTX 1080 Ti이며 이전 Intel UHD 770 기준은 보존했다.

별도 초기값/상속/숨김/UA overflow 18쌍의 스타일·DOM·상자와 기대값 660개는 통과했다. 전체 strict 비교는 12/18로, 새 폼 페인트 6쌍은 FAIL_PAINT로 남는다. 이 문서는 corpus 수량에 합산하지 않는다. [상세 결과·아카이브·다음 작업](../../../docs/RENDERING-ACCURACY-STYLE-RESULTS.md)을 따른다.

```powershell
# 새 스타일 교정: 미등록 폼 페인트 차이 6쌍 때문에 현재 종료 코드 1
& .\TWebFrame2\tests\rendering\harness\Test-RenderingStyleCaptures.ps1 `
  -RunPath .\TWebFrame2\tests\rendering\runs\<new-run-id>

# 소스·입력·이전 native 픽셀·reference 진단 보존과 필수 스타일 검사 감사
& <python.exe> .\TWebFrame2\tests\rendering\harness\Audit-RenderingStyleRun.py `
  .\TWebFrame2\tests\rendering\runs\<new-run-id> `
  .\TWebFrame2\tests\rendering\runs\20261004-capture-calibration-v7-final `
  .\TWebFrame2\tests\rendering\runs\20261004-all-cpu-scroll-final
```

## 실행

저장소 루트에서 Windows x64, VS2019 v142 C++ 도구와 Windows SDK, WebView2 Runtime, NuGet SDK `Microsoft.Web.WebView2.1.0.3595.46`가 설치된 환경으로 실행한다. 프로젝트는 기존 `packages`를 사용한다. 폰트는 Windows의 Arial/Malgun Gothic/Segoe UI와 추가 진단의 Tahoma/Times New Roman이며 해시를 실행 환경에 기록한다. Windows 폰트 파일은 재배포하지 않는다.

```powershell
# 기존 입력이 바뀌면 거절한다. 현재 지원하는 생성 수량은 pilot 20개다.
& .\TWebFrame2\tests\rendering\generator\New-RenderingCorpus.ps1 -Count 20
& .\TWebFrame2\tests\rendering\generator\Update-RenderingCoverage.ps1

# 기본: 빌드 + 비교 교정 134개 검사 + 20 × 2 DPI × 3 viewport
& .\TWebFrame2\tests\Run-RenderingComparison.ps1

# 해당 소스로 이미 빌드한 실행기를 쓸 때
& .\TWebFrame2\tests\Run-RenderingComparison.ps1 -SkipBuild

# 저장 실행기와 교정 입력으로 RGB/alpha/DPI/viewport 18쌍 추가 확인
& .\TWebFrame2\tests\rendering\harness\Test-RenderingCaptureRoutes.ps1 `
  -RunPath .\TWebFrame2\tests\rendering\runs\<new-run-id>

# 승인 예외를 끄고 원래 픽셀 차이를 실패로 보는 진단
& .\TWebFrame2\tests\Run-RenderingComparison.ps1 -CompareOnly -StrictPixels `
  -RunPath .\TWebFrame2\tests\rendering\runs\<run-id>

# 특정 문서 재현
& .\TWebFrame2\tests\Run-RenderingComparison.ps1 -SkipBuild `
  -CaseId 0008-grid-spanning -Dpi 96,144 -Viewports 800x600

# 캡처를 유지하며 현재 비교 코드로 재판정: 새 comparisons 하위 폴더 생성
& .\TWebFrame2\tests\Run-RenderingComparison.ps1 -CompareOnly `
  -RunPath .\TWebFrame2\tests\rendering\runs\<run-id>

# 같은 실행의 보관 실행기로 최소 재현 27개 × 두 DPI 추가 검사
& .\TWebFrame2\tests\rendering\harness\Run-RenderingRepros.ps1 `
  -RunPath .\TWebFrame2\tests\rendering\runs\<new-run-id>
```

비교 실패가 있으면 종료 코드 1이다. 실패를 건너뛰지 않고 `summary.json/html`, `failures.json`, 문서별 `result.json`, `layout-diff.json`, `diff.png`에 저장한다. 캡처 자체의 실패·불안정은 별도로 기록한다. corpus 수보다 큰 `-Count`와 잘못된 문서 ID, 중복 DPI/viewport, 입력 해시 변경은 거절한다.

## DPI와 캡처

schema v3는 `reference-cdp.png`와 원래 viewport의 CDP clip 인자를 추가한다. `CapturePreview`와 디코딩 픽셀이 완전히 같아야 한다. 이미지 누락·손상·다른 크기/픽셀은 실행기 오류이며 승인 backend 차이로 통과시키지 않는다. `reference-route.json`과 차이 이미지를 저장한다. 이전 캡처는 미실행으로 표시하여 새 교정을 했다고 간주하지 않는다.

실행기는 첫 fixture 전·마지막 fixture 후에 같은 WebView2의 내부 GPU 페이지를 읽어 `reference-graphics-before/after.json`을 보존한다. 실제 활성 GPU, ANGLE/Skia, feature status와 display/color 정보의 fingerprint가 전체 행렬에서 같아야 한다. 내부 진단 페이지에서만 스크립트를 잠시 켜며 fixture는 계속 비활성화한다. 이전 독립 캡처 기준은 Intel UHD 770의 ANGLE/D3D11·Skia GaneshGL 환경이다. 이전 기준 GPU는 미계측이므로 새 기준을 별도 환경에 보존했다. 기준 환경이 바뀌었을 때 같은 runtime 버전이라는 이유로 이전 캡처를 덮어쓰지 않는다.

CSS viewport는 384×768, 800×600, 1280×800이고 페이지 확대는 1.0이다. 144 DPI에서 PNG 크기는 각각 576×1152, 1200×900, 1920×1200이다. WebView2는 raw device bounds와 명시적 RasterizationScale을 사용한다. Native는 `View::CaptureRenderingSnapshot`에서 실제 View의 `RenderSurface`를 호출한다.

각 native 캡처는 다른 DPI를 거친 뒤 재촬영하여 픽셀과 진단 JSON의 안정성을 확인한다. 요청 DPI가 실제 창 DPI와 같을 때는 `WM_PRINTCLIENT` 경로와 픽셀 전체를 비교한다. 최신 실행에서 실제 창 DPI는 96으로 이 검사는 60/60 일치했다. 앞선 실제 144 DPI 실행의 일치 자료도 보존한다. **명시적 96 DPI 캡처는 실제 Windows 100% 표시 설정 검증을 대신하지 않는다.** 시스템 표시 설정을 자동 변경하지 않는다. 동일 실행에서 실제 Windows 두 DPI를 전환하는 검사, 모니터 전환 및 화면 back-buffer 독립 캡처 검사는 남아 있다.

## 보존·복원

문서 ID, HTML, CSS, case metadata와 corpus manifest는 영구 입력이다. 생성기는 기존 내용 변경을 거절한다. 새 run 경로만 사용하고 이전 캡처·판정을 덮어쓰지 않는다. 참조 캡처의 반복 시도도 각각 보존한다. `runs/`와 `restored/`는 원본을 디스크에 유지하면서 Git 작업 트리의 대량 결과를 제외하는 경로다. 결과는 별도의 ZIP과 SHA-256 catalog로 보관한다.

```powershell
& .\TWebFrame2\tests\rendering\harness\Archive-RenderingRun.ps1 `
  -RunPath .\TWebFrame2\tests\rendering\runs\<run-id>

& .\TWebFrame2\tests\rendering\harness\Save-RenderingBaselineIndex.ps1 `
  -RunPath .\TWebFrame2\tests\rendering\runs\<run-id> `
  -CatalogPath .\TWebFrame2\tests\rendering\archives\<run-id>.json

& .\TWebFrame2\tests\rendering\harness\Restore-RenderingArchive.ps1 `
  -CatalogPath .\TWebFrame2\tests\rendering\archives\<run-id>.json `
  -Destination .\TWebFrame2\tests\rendering\restored\<new-id>

# 복원한 소스 해시·입력·실행기 확인 및 당시 비교 코드로 동일 재판정
& .\TWebFrame2\tests\rendering\harness\Verify-RenderingRecovery.ps1 `
  -RestoredRoot .\TWebFrame2\tests\rendering\restored\<new-id>
```

소스 커밋에는 입력·계측/비교 코드, `archives/`의 JSON catalog·복원 검증 기록과 `baselines/` 불변 인덱스를 포함한다. 총 약 291 MB의 검증 아카이브는 원본 ZIP 그대로 로컬 `archives/`에 보존하며 이번 소스 푸시에 포함하지 않는다. 새 checkout에서 과거 실행을 복원하려면 catalog에 지정된 ZIP을 별도로 확보하여 같은 경로에 두어야 한다. ZIP의 원격 영구 보관은 미완료이며 보관 위치와 분할 정책은 별도로 정해야 한다. ZIP에는 원본 입력, 모든 이미지/JSON/로그, 소스 snapshot, 실제 실행 파일을 담는다. 후속 아카이브에는 최소 재현 및 수정 전후 증거·회귀 로그도 포함한다. WebView2 profile 캐시만 아카이브에서 제외하며 원본 run의 캐시는 삭제하지 않는다. ZIP의 모든 파일을 풀어 SHA-256을 검증하고 새 복원 경로에서도 다시 검증한다. 같은 ID나 기존 복원 경로는 거절한다.

복원 후 `run/`에서 전체 PNG를 다시 비교할 수 있다. `run/source-snapshot.zip`을 새 폴더에 풀면 당시 소스와 계측/비교 코드가 복원된다. 보관한 `run/RenderingComparisonRegression.exe`를 `--cases`, `--measure`, `--width`, `--height`, `--dpi`, `--out` 인자로 실행하면 당시 입력을 다시 렌더링할 수 있다. `cases.txt`는 **복원한 `inputs/<case-id>`의 절대 경로**로 새로 작성한다. 새 출력 폴더를 사용한다. 기록된 OS·WebView2 Runtime·폰트가 설치돼 있어야 같은 기준을 기대할 수 있다.

최초 아카이브는 1,859개 파일 복원과 78/120 재판정을 확인했다. 후속 `20261003-162837-939-d2a5124f`는 2,361개 파일 복원과 **84/120 동일 재판정**, 보관 실행기로 표/Latin 문서를 두 DPI에서 재렌더링한 네 결과의 기준·native 픽셀 및 text geometry 일치까지 확인했다. 복원 증거는 별도 ZIP/catalog에 보존한다. 전체 1,000개 및 실제 Windows 두 표시 설정의 복원 재실행은 완료되지 않았다.

최신 `20261003-174213-996-9c018c59`는 3,167개 파일 복원과 84/120 동일 재판정, table/Latin/mixed/form/dashboard의 새 렌더링 10쌍에서 양쪽 픽셀·문자 좌표·reference font usage의 재현을 검증했다. 기준 인덱스에도 `reference-fonts.json`을 포함한다. 상세 기록은 [복원 검증](archives/20261003-174213-996-9c018c59-recovery-validation.json)을 따른다.

## 다음 단계

최신 `20261004-capture-calibration-v7-final`은 독립 교정 18/18·표본 357/357, 비교 교정 47개·기준 환경 예외 거절 교정 6개가 통과했다. 아카이브 3,258개 파일·소스 129개를 복원하고 120쌍의 모든 raw 수치와 판정을 재현했다. 원본 corpus는 20개이며 교정 3문서는 별도 수량이다. 상세 환경 변화와 미완료 계약은 [교정 결과](../../../docs/RENDERING-ACCURACY-CAPTURE-CALIBRATION-RESULTS.md)를 따른다.

복원한 실행기·DLL·입력·계측/비교 코드로 전체 120쌍을 새로 렌더링한 결과도 기준/자체/CDP 픽셀, DOM/스타일/문자 기하와 GPU fingerprint가 모두 같았다. [복원 검증](archives/20261004-capture-calibration-v7-final-recovery-validation.json)과 [복원 증거 catalog](archives/20261004-capture-calibration-v7-final-recovery-evidence.json)에 재실행 스크립트와 2,494개 증거 파일을 보존한다.

계획의 수량은 20→100→300→600→1,000이다. 현재 생성기의 `Count`는 20만 허용한다. 100개로 확대하기 전에 비교 계약의 보류 항목을 구현하고 표/텍스트/페인트/폼의 공통 실패를 해결한다. 원본을 단순화하거나 픽셀 오차 허용치를 넓혀 통과시키지 않는다. [feature-inventory.json](feature-inventory.json), [coverage.json](coverage.json)은 작성된 기능 태그와 113개 태그 쌍을 기록하며 전체 pairwise·삼중 조합·복잡도 수량 검증은 아직 미완료다.

기존 v1 캡처에는 글자 좌표 계측이 없다. `CompareOnly`는 그런 캡처에 새 계측을 있다고 간주하지 않으며, 기존 strict 판정 범위를 유지한다. 새 v2 캡처에서는 어느 한쪽 계측 누락을 실행기 오류로 처리한다. baseline fingerprint에는 계측 JS와 비교 계약의 해시도 포함한다. 새 아카이브는 원본 20개 외에 `repros/` 최소 재현도 보존한다.

`repro-evidence/`의 최신 추가 40쌍은 기본 corpus 120개와 따로 집계한다. 같은 증거 폴더의 재실행·덮어쓰기는 거절한다. 최신 추가 결과는 4 통과/36 실패이며 이전 22쌍도 보존한다. 자세한 입력과 버전 구분은 [최소 재현 안내](repros/README.md)를 따른다.

후속 커닝·플랫폼 글꼴 선택 실행은 `20261003-184127-080-f8191e3e`이며 기본 84/120, 문자 기하 24/24를 유지한다. 별도 재현은 20개/40쌍이다. 실제 한국어 monospace의 GulimChe 파일과 UI locale을 baseline 환경에 추가했다. 사용자 요청으로 기존 monospace/Consolas 동일성 회귀검사를 삭제하고 다시 검증했다. [최신 결과](../../../docs/RENDERING-ACCURACY-FONT-SELECTION-RESULTS.md)를 따른다.

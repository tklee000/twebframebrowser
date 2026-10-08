# 렌더링 정확성 v10 보존 상태와 v11·v12·v13 검증 기록

저장일: 2026-10-04  
작업 경로: `D:\ai_works\miniwebbrowser`  
현재 상태: **v13의 사각형 overflow 클립·hit-test·RTL 강제 scrollbar 개선과 정확성·성능 검사를 마쳤다.** 최신 실행은 `20261004-overflow-v13-final`이며 [v13 결과](RENDERING-ACCURACY-V13-RESULTS.md)를 따른다. 252/252쌍 성공(198 strict·기존 승인 54), 새 24쌍 strict·변경 전 실패 18쌍 교정·새 예외 0·소스 194개 일치다. 초기 레이아웃 212.42→203.26ms·첫 드래그 16.81→15.74ms, 최초 페인트는 107.30→117.37ms로 증가했다. 기존 228쌍의 화면·계약 대상 진단·raw 판정은 불변이고 내부 폭 6쌍의 교정은 별도 검증했다. 마지막 전체 회귀 12개·platform integrity와 9,835개 파일 보관·복원·252쌍 새 렌더링·복원 증거 6,672개 파일 해시 검증도 완료했다. 아래 v11·v12는 이전 완료 단계이며 v10의 미실행 설명은 당시 중단 기록이다.

사용자 지시는 [검증 계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)을 진행하면서 정확성과 속도를 함께 개선하고, 전체 회귀검사와 보관·복원 검증을 마지막에 수행하는 것이다. 이후 “계속할수 있게 문서에 현재 상태만 저장해두고 중단해주세요” 요청을 받아 문서만 갱신하고 중단했다.

## v11 재개 완료

- 150% DPI의 DOM 텍스트 조각 재줄바꿈·caret·hit-test·선택 영역 오류를 공통 경로에서 수정했다. CPU glyph 클립 계산과 grayscale 캐시 실제 할당/집계를 개선했다.
- geometry 글자·select 페인트 12쌍은 모든 DPI/viewport의 동일 입력 CPU/GPU 독립 원인 실험을 통과했다. 계약 ID는 `pilot-cpu-rendering-with-verified-geometry-backends-v11`, 정확한 backend 서명은 90개다. raw 차이와 strict 실패를 유지한다.
- 최종 소스 해시 174개와 전체 204쌍의 기존 픽셀·DOM·스타일·기하 불변을 확인했다. 번갈아 3회씩 측정한 스크롤 페인트는 약 5.5% 감소했고 최초 페인트는 약 1.2% 증가했다. 앱 추가 재빌드는 하지 않았다.
- 정확성·속도 이후 마지막 전체 회귀 → 주 ZIP/불변 baseline → 새 폴더 복원/120쌍 재판정 → 204쌍 새 렌더링 → 복원 증거 ZIP 해시 검증을 완료했다. 주 ZIP 20,239개 파일·복원 증거 5,558개 파일을 보존했다.
- 최종 ZIP/catalog는 `archives/20261004-table-form-v11-final2*`, 복원 위치는 `restored/20261004-table-form-v11-final2-verified`다. 정확한 SHA와 링크는 위 v11 결과에 있다. 기존 실패와 v10/v11 preflight 자료는 그대로 유지한다.

다음 정확성 범위는 전체 계획의 나머지 스타일·reference glyph/baseline·cluster·고정 교정 밖의 gutter/scroll/RTL/transform/pseudo/iframe/root overflow와 실제 Windows 두 DPI 검증이며, 이를 보강한 뒤 100→1,000문서로 확대한다. 이번 단계의 성공을 전체 계약 완료로 표시하지 않는다. 아래 재개 명령은 v10 중단 당시 기록이며 현재 작업에는 최신 v13 결과와 새 실행 ID를 사용한다.

## 구현 완료

| 파일 | 이번 단계 변경 |
| --- | --- |
| `TWebFrame2/src/CSS.cpp` | th 기본 굵기·중앙 정렬과 명시적 상속 정렬의 우선순위, textarea UA overflow:auto. |
| `TWebFrame2/src/DOM.cpp` | HTML table 직속 tr의 암시적 tbody 생성, body/html 뒤 인접 텍스트를 본문 마지막 텍스트에 병합. HTML 파서 전체 완성으로 간주하지 않는다. |
| `TWebFrame2/src/Layout.cpp`, `Layout.h` | table-cell 지정 높이를 콘텐츠 최소 높이로 적용. colspan/rowspan 계산에 내부 border-spacing 포함. textarea 초기 InnerText의 양축 scrollbar 결합을 최대 3회 계산으로 안정화하고 client/scroll 크기 캐시·스크롤 clamp·텍스트 원점·caret·hit-test를 동기화. |
| 같은 Layout 파일 | textarea의 실제 축별 overflow 상태로 scrollbar 표시. custom thumb의 기본 폭·모서리·최소 길이·장치 픽셀 반올림 및 스크롤 범위 0의 thumb 생략 교정. DPI별 advance를 64×deviceScale 단위로 반올림하고 부모가 배치한 inline 텍스트의 중복 정렬 방지. |
| `TWebFrame2/src/RasterSurface.h` | 양축 overflow:hidden textarea에서 grayscale 텍스트 합성. 원시 LCD 채널 평균 후 gamma 보정으로 기준 픽셀 재현. 기존 CPU 글꼴·glyph 마스크·배치 합성 최적화 유지, grayscale 마스크는 픽셀당 1바이트 캐시. glyph 캐시는 16MiB/4,096개 제한, 색/모드 보정표는 최대 약 22MiB. |
| `TableSpanRegression.cpp`, `FormControlRegression.cpp` | 독립 기준 좌표의 최소 높이·span·암시적 tbody·텍스트 병합 검사와 textarea 양축 scroll 크기·offset·thumb hit-test·caret 검사. 최종 소스 빌드/실행 통과. |
| `tests/rendering/table-form-calibration/` | 새 고정 입력 5문서와 11개 파일 SHA manifest. 원본 corpus 20문서와 기존 교정 입력은 보존. |
| `harness/Test-RenderingTableFormCaptures.ps1` | 실행기·소스·입력·런타임 해시, GPU/DPI/viewport, DOM/텍스트/스타일/크기와 strict RGBA 검사. 새 예외를 허용하지 않는 독립 교정 그룹. |
| `Run-RenderingComparison.ps1`, `harness/Test-RenderingRecoveryCaptures.ps1` | 새 입력 그룹을 소스 snapshot과 향후 새 렌더링 복원 검사에 포함. |

비교 계약 ID는 `pilot-cpu-rendering-with-table-form-scrolling-v10`이다. 캡처 schema 5, 스타일·기하 계약 버전 1을 유지한다. backend 예외는 기존 78개이며 이번 단계에서 추가하지 않았다. 이번 단계에서 MdViewer 앱을 재빌드하지 않았다.

## 최종 렌더링 검증 결과

실행 폴더: `TWebFrame2/tests/rendering/runs/20261004-table-form-v10-final`  
소스 snapshot: `source-snapshot.zip`, 환경 기록: `environment.json`, 소스 해시 169개.  
저장한 `RenderingComparisonRegression.exe` SHA256: `3E66A879A073E7C33F9FB6BF27AD6E8B974AC2AEFC3643954D86FC2E0A502E7E`.

96/144 DPI × 384×768·800×600·1280×800 CSS viewport를 검사했다. 실제 Windows DPI는 96이고, 144는 명시적 target scale이다. `actualWindowsBothDpiValidated=false` 상태를 유지한다.

| 그룹 / 결과 파일 | 결과 |
| --- | --- |
| 원본 `summary.json` | 120/120 성공. RGBA 완전 일치 84, 기존 승인 backend 차이 36. |
| `table-form-calibration/summary.json` | 30/30 RGBA 완전 일치 및 구조 통과. 크기 792개·스타일 4,950개·누락 0. 새 예외 0. |
| `style-calibration/summary.json` | 18/18 성공, strict 12·기존 승인 6. 기대 스타일 검사 660개. |
| `geometry-calibration/summary.json` | 구조·크기 18/18, 크기 648개·누락 0. strict 6, 미등록 `FAIL_PAINT` 12. |
| `capture-calibration/summary.json` | 독립 RGB/DPI 캡처 18/18 strict. |
| `comparison-controls/` | 비교기 고의 오류 교정 총 182개 통과. |

총 204쌍을 캡처했다. **204쌍 전체 성공으로 표시하지 않는다.** 남은 크기 교정 페인트 차이는 geometry-boxes의 96 DPI 21픽셀·144 DPI 22픽셀(최대 채널 차이 1), geometry-scroll의 96 DPI 32픽셀·144 DPI 49픽셀(최대 차이 31)이며 각각 세 viewport에서 반복된다. 원인 분류·예외 등록을 추가하지 않았고 raw 실패를 보존했다.

TableSpanRegression·FormControlRegression은 최종 grayscale 구현을 포함한 소스에서 통과했다. 전체 회귀검사는 이번 단계에서 아직 실행하지 않았다. CRLF를 고려한 `git diff --check`도 통과했으며 기존 파일의 줄바꿈은 일괄 변경하지 않았다.

## 속도 검증 준비 상태

`20261004-table-form-v10-final/performance-evidence/`에 다음만 준비했다.

- `before-bin/`: 직전 완료한 v9-final3의 최적화된 CPU `ViewRenderingPerformance.exe`와 런타임.
- `after-bin/`: 이번 v10 소스로 빌드한 실행기와 같은 런타임.
- `app-assets/`, `input.md`, `environment.json`: 동일한 보존 앱 자산·문서와 준비 해시.
- `Run-PairedPerformance.ps1`, `Summarize-Performance.py`: before/after를 번갈아 각 3회 실행하고 시간 중앙값·BGRA 12쌍·레이아웃 JSON 6쌍·입력 3쌍을 비교하는 도구.

**아직 `asset-workspace`, `before-1`/`after-1` 등의 측정 폴더와 성능 `summary.json`은 없다. v10 속도 영향은 미측정이다.** 기존 문서의 20.67→21.41ms는 v9 결과이며 이번 변경의 속도 결과로 사용할 수 없다.

## 보존한 변경 전·중간 증거

최종 run의 `before-evidence/`에 이전 실행과 실행기·런타임·소스 snapshot을 복사했다. 원래 실행 폴더도 유지한다.

- `20261004-table-form-before`: 최초 4문서 × 2 DPI의 변경 전 실패 8쌍.
- 개별 iteration과 `20261004-table-form-v10-preflight`: 최소 높이·span·정렬·scroll 교정의 중간 결과. preflight는 중간 소스 snapshot/실행기로 식별한다.
- `20261004-table-form-grayscale`: DirectWrite Factory2 grayscale 시도의 실패(308/576픽셀, 최대 차이 63). 실패를 덮어쓰지 않았다.
- `20261004-table-form-gray-lcd`: LCD 채널 평균 방식으로 coupled textarea의 두 DPI 차이 0 확인.
- `20261004-table-form-coupled-before`: 같은 coupled 입력을 v9 실행기로 검사한 실패 3,202/6,796픽셀. 입력 복사 폴더 이름 때문에 capture ID는 `inputs`이며 실제 HTML/CSS는 현재 `textarea-coupled-scroll` 입력과 같다.

grayscale 원인 확인에 사용한 공식 구현: [Skia DirectWrite scaler](https://skia.googlesource.com/skia/+/refs/heads/main/src/ports/SkScalerContext_win_dw.cpp)의 `RGBToA8` 채널 평균. custom scrollbar 확인 자료: [Chromium custom scrollbar](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/blink/renderer/core/layout/custom_scrollbar.cc), [custom scrollbar theme](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/blink/renderer/core/paint/custom_scrollbar_theme.cc). 자료와 실패 증거는 재개 때 참고용이다.

## 당시 재개 순서 — v10 중단 시점 기록

1. 저장한 실행기·소스 해시와 현재 변경을 확인한다. 새로운 코드 수정 또는 실패 수정이 필요하면 현재 run을 보존하고 별도 run ID로 최종 캡처를 다시 만든다. 기존 실패·결과·아카이브를 덮어쓰지 않는다.
2. 먼저 정확성·속도 작업을 이어간다. 현재 준비 자료를 그대로 사용할 경우 저장한 실행기로 아래 속도 명령을 실행하고 픽셀·레이아웃 불변 및 시간 변화를 평가한다.

```powershell
Set-Location D:\ai_works\miniwebbrowser
$run = Join-Path (Get-Location) 'TWebFrame2/tests/rendering/runs/20261004-table-form-v10-final'
& (Join-Path $run 'performance-evidence/Run-PairedPerformance.ps1')
& 'C:/Users/tklee/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe' (Join-Path $run 'performance-evidence/Summarize-Performance.py')
```

측정 스크립트는 `asset-workspace`가 이미 있으면 거절한다. 재측정 시 기존 결과를 지우지 않고 별도 증거 위치를 사용한다. 성능 입력은 같은 MdViewer 자산을 공통 View 실행기에 넣는 방식이며 앱 추가 재빌드는 필요하지 않다.

3. 정확성·속도 검증을 마친 뒤 **마지막에** 전체 회귀 12개와 platform integrity를 실행하고 로그를 최종 run에 보존한다.

```powershell
& ./TWebFrame2/tests/Run-SupportCompatibility.ps1 -FullRegression
```

4. 그 다음 보관·baseline·복원·재판정·새 렌더링·복원 증거 보관을 순서대로 수행한다. 아래는 현재 run을 최종으로 유지할 경우의 경로다. v10 ZIP/catalog/baseline/복원 폴더는 아직 만들지 않았다. 동일 ID가 이미 생겼다면 새 ID·새 복원 위치를 사용한다.

```powershell
$harness = Join-Path (Get-Location) 'TWebFrame2/tests/rendering/harness'
$archiveRoot = Join-Path (Get-Location) 'TWebFrame2/tests/rendering/archives'
$catalog = Join-Path $archiveRoot '20261004-table-form-v10-final.json'
$restored = Join-Path (Get-Location) 'TWebFrame2/tests/rendering/restored/20261004-table-form-v10-final-verified'
& (Join-Path $harness 'Archive-RenderingRun.ps1') -RunPath $run
& (Join-Path $harness 'Save-RenderingBaselineIndex.ps1') -RunPath $run -CatalogPath $catalog
& (Join-Path $harness 'Restore-RenderingArchive.ps1') -CatalogPath $catalog -Destination $restored
& (Join-Path $harness 'Verify-RenderingRecovery.ps1') -RestoredRoot $restored
$savedHarness = Join-Path $restored 'recovery-verification/source/TWebFrame2/tests/rendering/harness'
& (Join-Path $savedHarness 'Test-RenderingRecoveryCaptures.ps1') -RestoredRoot $restored
& (Join-Path $savedHarness 'Save-RenderingRecoveryEvidence.ps1') -RestoredRoot $restored -ArchiveRoot $archiveRoot
```

복원 검증은 저장한 소스의 비교·계측 코드와 실행기를 사용한다. 예상 새 렌더링 수량은 원본 120 + style 18 + geometry 18 + capture 18 + table/form 30 = 204쌍이며, 미등록 페인트 실패 12쌍도 같은 결과로 재현하는지 확인한다.

5. 속도 측정·전체 회귀·복원 결과를 확인한 뒤 이 상태 기록과 계획의 체크 항목을 갱신한다.

## 재개 시 제약과 남은 계획

기존 미커밋 변경·미추적 파일이 많다. 이번 작업 외의 변경, 기존 삭제 상태, `src.zip`, `mdviewer_tinyversion/`, 과거 run/ZIP/catalog/baseline을 보존하고 reset·일괄 삭제·결과 덮어쓰기를 하지 않는다. 이번 단계에서 커밋·PR을 만들지 않았다.

reference glyph/baseline·cluster, 나머지 계산 스타일, RTL·stable gutter·transform·pseudo·iframe·root overflow 조합, 실제 Windows 두 DPI·화면 캡처, 100→1,000문서 확장은 미완료다. 이번 30쌍의 strict 성공을 계획 전체의 완료로 표시하지 않는다. 직전 v9의 전체 회귀·보관·복원 성공은 [CSSOM 검증 결과](RENDERING-ACCURACY-GEOMETRY-RESULTS.md)에 기록되어 있으며 v10의 검증을 대체하지 않는다.

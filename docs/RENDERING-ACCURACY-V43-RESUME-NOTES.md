# v43 중단 지점과 재개 메모

2026-10-06, 사용자 요청으로 중단한 당시 기록. 이후 같은 날짜에 사용자가 계획 문서의 여러 단계를 묶어 진행하도록 요청하여 재개했다. 아래 수량과 중단 상태는 당시 이력이며 최신 상태는 [계획 문서](RENDERING-ACCURACY-VALIDATION-PLAN.md)와 [재개 실행기](../TWebFrame2/tests/rendering/work/v43/Resume.ps1)를 따른다.

작업 폴더: `D:/ai_works/miniwebbrowser`
증거·실행 폴더: `C:/twf-v43`
실행 스크립트: `TWebFrame2/tests/rendering/work/v43`
중단 기록: `C:/twf-v43/stop-request.json`

진행 중이던 v43 PowerShell 실행과 해당 콘솔 프로세스를 종료했고, v43 실행 경로를 사용하는 남은 프로세스가 없음을 확인했다. 전체 회귀·최종 반디집 보관·새 폴더 복원 검증은 아직 시작하지 않았다. 소스·입력·로그·중간 캡처·빌드 결과는 그대로 남겼으며 정리하지 않았다.

## 완료한 변경과 검사

- 일반 회색조 글자에 색 alpha × 브러시 opacity × clip 합성을 지원했다. 일반 글자의 source/backdrop 별도 반올림과 native 아이콘의 single-round 규칙을 구분한다.
- LCD는 실제 opacity가 정확히 1일 때만 CPU 경로를 허용한다. 종전 `.9999` tolerance로 작은 투명도를 잃던 경로를 막았다.
- 같은 색/opacity가 8회 반복되면 준비하는 thread-local 합성 조회표를 추가했다. 크기는 256 KiB이며 키가 바뀌면 직접 계산으로 복귀한다.
- v42 동결 소스 457개 중 이번 변경은 `TWebFrame2/src/RasterSurface.h`와 `TWebFrame2/tests/ScrollRenderingRegression.cpp` 두 파일이다. 기존 미커밋 변경은 유지한다. `C:/twf-v43/source-change-audit.json` 참조.
- 독립 byte-backed DirectWrite oracle: 픽셀 13,824건·거부 304건·거부 후 복귀 288건, 변경 후 모두 통과. 조회표를 끈 대조군도 모두 통과.
- 변경 전: CPU 미지원 6,912건, 실제 받아들인 경로의 픽셀 불일치 3,144건, LCD guard 실패 8건. 미지원은 기존 fallback이며 모두 기존 최종 화면 오류라는 뜻은 아니다.
- 새 opacity·기존 text axis/gray alpha/clip cache/clip transform/native theme, 총 6개 명령 통과. `C:/twf-v43/*-focused-final.log` 참조.
- 이전 실행기와 현재 실행기의 환경 대조 48쌍 각각 픽셀·전체 진단·raw·판정 일치. 그래픽 환경 변화 0, focused source 457개 동결.
- live 보호 입력 2,799개 확인, 새 손실 0. 이전부터 없던 `C:/twf-v24/cleanup-archives/artifact-history.7z`의 입력 4,544개 실물 검증은 미완료다.

## 성능 증거

- 6회 직렬 교대, 단일/반복 BGRA hash 144쌍 일치. 반투명 반복 페인트는 동일 구현의 조회표 없는 직접 계산 대조군보다 약 66~93% 빨랐다. v42 fallback 대비 속도라는 뜻은 아니다.
- 기존 불투명 paint 조건별 변화는 약 −4~+7%; 일괄 속도 개선으로 해석하지 않는다. 측정한 warm C++ operator new 할당은 양쪽 0.
- 새 키·8번째 조회표 생성·다음 1,000회도 6회 교대 측정, BGRA hash 72쌍 일치. 8번째 준비 비용은 약 0.46~0.84ms. 한 glyph는 비용 회수에 추가 약 317~385 paint, 9 glyph는 약 30~38, 32 glyph는 약 6~11 paint로 추정된다. 짧은 paint의 초기 비용을 결과에 공개한다.
- 동일 앱 자산 6회 비교: BGRA 48쌍·layout 24쌍 일치. 일반 문서 최초 paint +1.05%, 표 문서 −8.87%; 나머지 구간은 약 ±2% 안팎. helper 개선율을 앱 전체 개선율로 주장하지 않는다.
- 원값: `C:/twf-v43/diagnostics/gray-evidence.json`, `cold-evidence.json`, 각 CSV·정확성 로그, `C:/twf-v43/performance-final/summary.json`.
- 최초 oracle에서 기존 불투명 fractional alpha의 double 반올림을 float으로 옮겨 생긴 두 건의 기준 계산 차이를 수정했다. 최초 oracle·로그는 `diagnostics/oracle-first-draft`에 남겼고 수정한 같은 기준으로 before/after/direct를 재검사했다.

## 정확한 중단 지점

`Run-Final.ps1` → `Capture-Final.ps1` → 원본 pilot 120쌍의 비교 도중 중단했다.

- `C:/twf-v43/runs/20261006-gray-opacity-v43-final`: native 캡처 120쌍 생성, `result.json` 비교 106쌍 생성.
- corpus `summary.json`은 아직 없다. 따라서 pilot 및 전체 828쌍의 완료·불변을 판정하지 않는다.
- 추가 20개 calibration 그룹과 capture-route 검사, 전체 Audit, 오류 주입, 전체 회귀 12개·platform integrity, 최종 보관·복원·828쌍 새 렌더링은 미실행이다.
- `Capture-Final.ps1 -Resume`은 완료된 corpus summary 120/120을 요구하므로 현재 상태에 바로 사용할 수 없다.
- `Run-Final.ps1`을 처음부터 그대로 실행하면 기존 환경/캡처 폴더와 충돌할 수 있다. 완료한 단계의 증거를 보존하면서 아래 순서로 이어간다.

## 다음 실행 순서

1. 현재 source 457개·renderer·runtime·입력 해시와 실제 그래픽/DPI 환경이 동결 상태와 같은지 먼저 확인한다. 환경이 바뀌었으면 이전 로그를 보존하고 48쌍 환경 대조·앱 성능을 새 폴더에서 다시 검증한다.
2. 미완료 corpus 폴더를 `C:/twf-v43/runs/20261006-gray-opacity-v43-final-interrupted-<시간>` 같은 별도 이름으로 보존한다. 이동 전에 두 절대 경로가 모두 `C:/twf-v43/runs/` 바로 아래인지 확인하고 PowerShell `Move-Item -LiteralPath`로 처리한다. 삭제하거나 덮어쓰지 않는다.
3. 원래 run 경로가 빈 상태에서 `Capture-Final.ps1`을 실행해 pilot부터 828쌍을 다시 완결한다. 기존 source/성능 결과는 동일 환경·동일 해시일 때 재사용한다. 캡처에는 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--use-angle=d3d11 --use-adapter-luid=0,56278`, Python에는 `PYTHONUTF8=1`을 사용한다.
4. `Audit.py`로 828쌍의 이미지·전체 JSON·raw·판정 불변을 확인한다. 기존 예상값은 strict 727·기존 승인 75, 총 802/828이며 화살표 FAIL_PAINT 26은 실패로 유지한다. registry 111개·허용치 0·입력 변경 없음.
5. `Write-Results.py`로 성능 및 미완료 상태를 기록한 뒤 `Finish.ps1`을 실행한다. 이 단계에서 오류 주입 182+336건 → 마지막 전체 회귀 12개·platform integrity → 반디집 최종 ZIP → 새 폴더 복원·내부 모든 entry/live 입력 해시 → 828쌍 새 렌더링 → 여섯 focused 명령 로그 해시 → 반디집 복원 증거 보관 순으로 진행한다.
6. 모든 복원 검증이 끝난 뒤에만 `Complete-After-Recovery.ps1`로 generated/cache 정리·retention 검사·최종 문서·반디집 final-records 보관/복원을 수행한다. `Finish.ps1` 단계 중 실패하면 성공으로 표시하지 않고 그 위치부터 안전하게 재개한다.

주요 명령(재개 요청을 받은 뒤 실행):

```powershell
Set-Location -LiteralPath 'D:/ai_works/miniwebbrowser'
$env:PYTHONUTF8='1'
$env:WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS='--use-angle=d3d11 --use-adapter-luid=0,56278'
$python='C:/Users/tklee/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$work='D:/ai_works/miniwebbrowser/TWebFrame2/tests/rendering/work/v43'
# 먼저 해시/환경 확인 및 미완료 run 폴더 보존 이동을 수행한다.
& "$work/Capture-Final.ps1" *> 'C:/twf-v43/capture-final-resumed.log'
# 위 실행 성공을 확인한 뒤 순서대로 진행한다.
& $python -X utf8 "$work/Audit.py"
& $python -X utf8 "$work/Write-Results.py"
& "$work/Finish.ps1" *> 'C:/twf-v43/finish-resumed.log'
# Finish 전체 성공 및 복원 증거를 확인한 뒤에만 실행한다.
& "$work/Complete-After-Recovery.ps1" *> 'C:/twf-v43/completion-resumed.log'
```

새 압축은 반드시 `C:/Program Files/Bandizip/bz.exe` 사용. Python/.NET ZIP 읽기·해시·복원은 가능하지만 다른 압축 도구로 새 보관본을 만들지 않는다. MdViewer 자동 재빌드·커밋·브랜치·PR 생성은 하지 않는다. 실제 Windows 144 DPI/두 모니터·전체 HTML/CSS/DOM/paint 계약·1,000문서는 별도 미완료다.

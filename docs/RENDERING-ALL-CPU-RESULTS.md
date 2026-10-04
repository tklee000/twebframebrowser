# 전체 CPU 렌더링 전환과 패딩 스크롤 회귀 결과

검사일: 2026-10-04  
실행: `20261004-all-cpu-scroll-final`, Windows x64 Release, WebView2 `154.0.4258.53`

사용자 지시에 따라 TWebFrame2의 폰트·그라디언트·SVG/폼 도형·모서리·그림자 합성과 View 화면/출력 타깃을 CPU 경로로 전환했다. 코드 보기에서 내용이 패딩 안에 들어가는 경우에도 생기던 세로 스크롤 범위를 공통 엔진에서 수정했다. 실제 앱 CSS를 복사한 엔진 재현과 회귀검사를 사용했으며 MdViewer 추가 빌드는 하지 않았다.

## 비교 결과

| 검사 | 결과 |
| --- | --- |
| 원본 20문서 × 2 DPI × 3 viewport | **120/120 성공** |
| 픽셀 완전 일치 | 84쌍 |
| 승인 CPU/GPU 차이 | 36쌍: 폰트 12, 도형·그라디언트·그림자 AA 24 |
| 기본 DOM·활성 스타일·상자·문자 기하 오류 | 0 |
| 문자 기하가 있는 비교 | 24/24 성공, native 대표 glyph 924개 |
| 기준/자체 반복 캡처 및 DPI 왕복 | 120/120 안정 |
| 실제 창 96 DPI의 `WM_PRINTCLIENT` | 60/60 픽셀 일치 |
| 고의 오류 및 승인 정책 교정 | 40개 통과 |
| 전체 엔진 회귀 | 12개 실행기 및 platform integrity 통과 |

승인 차이는 문서·DPI·viewport, 양쪽 디코딩 픽셀 해시와 raw 차이 수치를 등록값과 비교한다. DOM·스타일·레이아웃·문자 기하 오류를 승인으로 통과시키지 않으며, 새 픽셀 차이는 실패한다. 원본 입력 20개와 최초 WebView2 기준 120개는 그대로 유지했다.

패딩이 있는 일반 상자·`pre`·`textarea`에서 짧은 내용의 스크롤바 hit target과 숨은 세로 범위가 없고, 긴 내용의 전체 스크롤 범위는 유지되는 것을 96/144 DPI에서 검사했다. 이 변경은 TWebFrame2 공통 레이아웃/스크롤 계산에 적용했다.

## CPU 경로 확인

소스와 실행 파일 import에서 D3D11 장치 생성·shader 컴파일, WGL, Skia GPU surface/readback 및 기존 `RasterGpu` 진입점을 찾지 않았다. Direct2D WIC·HWND·DC 타깃은 software를 지정한다. Skia는 CPU raster-direct C ABI로 사용한다. vendor DLL에 포함된 사용하지 않는 GPU 내부 코드의 존재를 엔진의 GPU 경로로 집계하지 않는다. [경로 조사](../TWebFrame2/tests/rendering/runs/20261004-all-cpu-scroll-final/gpu-path-audit.json)를 보존했다.

전체 화면의 최종 합성을 GPU 표면에서 끝내는 구조는 후속 업그레이드로 남긴다. 현재 CPU 결과를 비교 기준으로 보존한다.

## 보존과 복원

[120쌍 보고서](../TWebFrame2/tests/rendering/runs/20261004-all-cpu-scroll-final/summary.html), [입력·기준·소스 검사](../TWebFrame2/tests/rendering/runs/20261004-all-cpu-scroll-final/continuation-audit.json), [전체 회귀 로그](../TWebFrame2/tests/rendering/runs/20261004-all-cpu-scroll-final/full-regression.log)를 보관했다.

아카이브는 **20,144,877 bytes, 2,285개 파일**이다. [SHA-256 catalog](../TWebFrame2/tests/rendering/archives/20261004-all-cpu-scroll-final.json)로 모든 파일을 복원·검증했고, 소스 snapshot의 **118개 파일 해시**를 확인했다. 복원한 당시 비교 코드로 **120쌍을 재판정**하여 84 완전 일치·36 승인 차이와 모든 raw 수치·상태가 같았다. [복원 검증 기록](../TWebFrame2/tests/rendering/archives/20261004-all-cpu-scroll-final-recovery-validation.json)을 보존했다. 이번 복원은 보관 캡처의 재판정이며 새 렌더링 실행과 구분한다.

전체 1,000문서·6,000쌍, 전체 computed style·reference per-character baseline/glyph·실제 Windows 두 DPI 전환·독립 화면 캡처 등은 [계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)의 남은 항목이다. 이 결과를 전체 계약 완료로 표시하지 않는다.

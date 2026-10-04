# 독립 캡처와 기준 그래픽 환경 교정 결과

검사일: 2026-10-04  
최종 실행: `20261004-capture-calibration-v7-final`, Windows x64 Release, WebView2 `154.0.4258.53`

WebView2의 `CapturePreview`와 CDP `Page.captureScreenshot`을 모든 비교에서 독립적으로 캡처·디코딩하여 검사했다. 새 캡처의 CDP 이미지 누락·손상·크기 차이·픽셀 차이는 실행기 오류이며 CPU/GPU 예외로 통과시킬 수 없다. [공식 Page 프로토콜](https://raw.githubusercontent.com/ChromeDevTools/devtools-protocol/master/pdl/domains/Page.pdl)과 [WebView2 호출 API](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.3595.46#calldevtoolsprotocolmethod)를 기준으로, 페이지 확대 1.0·원래 viewport·`clip.scale=1`·`captureBeyondViewport=false`를 사용했다. 캡처 크기와 기준 도형으로 실제 96/144 DPI 변환을 확인했다.

## 최종 검사

| 검사 | 결과 |
| --- | --- |
| 원본 20문서 × 2 DPI × 3 viewport | **120/120 성공** |
| 픽셀 완전 일치 | **96쌍** |
| 승인 CPU/GPU 도형·그라디언트·그림자 AA | **24쌍**, raw 차이 보존 |
| WebView2 두 캡처 경로 | **120/120 완전 일치** |
| 기준/자체 반복 캡처·자체 DPI 왕복 | 120/120 안정 |
| 실제 창 96 DPI의 `WM_PRINTCLIENT` | 60/60 일치 |
| 문자 기하가 있는 비교 | 24/24 성공 |
| 픽셀·문자·승인·CDP 오류 교정 | **47개 통과** |
| 기준 환경 예외의 거절 교정 | **6개 통과** |
| 독립 색상·DPI·viewport 교정 | **18/18 strict 성공**, 표본 **357/357** 일치 |
| 그래픽 환경 전후 기록 | 6개 프로세스의 12개 기록에서 같은 fingerprint |

독립 교정 3문서는 원본 corpus 수량에 합산하지 않는다. RGB 원색·서로 다른 회색 채널·alpha 0/128/255·색상 위의 source-over 합성, 1/2/100 CSS px의 실제 장치 픽셀 폭, 소수 상자, 긴 문서의 viewport clip과 fixed 위치를 검사했다. `CapturePreview`·CDP·자체 PNG와 가능한 96 DPI `WM_PRINTCLIENT`에 같은 예상 표본을 적용했다. 교정 문서의 창 출력도 9/9 일치했다. 검사 대상은 불투명 최종 화면이며 투명 출력 전체, 다른 색 프로필과 wide-gamut 검증은 남아 있다.

## 기준 환경 변화의 발견과 처리

첫 새 실행은 96 strict 통과·24 페인트 실패였다. 두 WebView2 캡처 경로는 이미 120/120 같았지만, 최초 기준과 비교하면 36쌍의 기준 픽셀이 달랐다. 이전 실행에서 열거된 Microsoft Remote Display Adapter가 새 실행에는 없었다. 보관된 이전 실행기로 두 DPI의 대표 8쌍을 새로 렌더링해도 새 기준과 같았고 자체 픽셀은 이전과 같았다. 따라서 CDP 검사 추가가 원인이라는 가설은 배제했다.

같은 WebView2 브라우저/프로필에서 첫 fixture 전에, 마지막 fixture 후에 `edge://gpu`의 상태를 읽어 보존하도록 보강했다. 현재 실제 선택은 **Intel UHD Graphics 770, ANGLE/D3D11, Skia GaneshGL**, GPU rasterization/compositing 활성, BT709/SRGB·BGRA_8888 출력이다. 활성 GPU·backend·feature status·compositor·display/color 정보의 fingerprint가 실행 도중 바뀌면 기준 환경 오류로 거절한다. 내부 진단 페이지에서만 스크립트를 잠시 켜며 모든 corpus fixture에서는 다시 끈다.

이전 실행의 실제 선택 GPU는 기록하지 않았으므로 이전 GPU 모델이나 어댑터 변화만을 단독 원인으로 단정하지 않는다. 새 기준은 **별도 그래픽 환경**으로 보존했다. 기준 DOM·전체 보관 computed style·상자·문자 좌표는 120쌍 모두 이전과 같았고, 자체 디코딩 픽셀도 120쌍 모두 같았다. 새 환경에서 기존에 승인된 기능의 CPU/GPU 차이 24쌍만 새 서명으로 추가했다. 이 분류는 위 관측과 현재 GPU/CPU 경로에 근거한다. 원래 입력·기준 이미지·48개 기존 예외를 유지하여 registry는 72개 서명을 갖는다.

등록기는 동일 엔진 소스·빌드 설정·OS·폰트·WebView2 버전·입력, 자체 120쌍 픽셀 불변, 기준 구조/스타일/좌표 불변, 두 캡처 경로 120쌍 일치, 독립 교정 18쌍과 12개 그래픽 기록을 요구한다. 이전에 승인되지 않은 기능은 이 경로로 등록하지 않는다. 엔진 변경·교정 누락·캡처 누락·software 기준·기준 좌표 변경·자체 픽셀 변경의 6개 거절 검사는 registry를 쓰기 전에 실패했다. 전역 픽셀 허용치는 계속 0이다.

## 수정과 보존

이번 변경은 비교 실행기·교정/등록/복원 도구와 계약/안내에 적용했다. 공통 엔진/빌드 관련 **66개 파일 해시**는 [전체 CPU 전환 실행](RENDERING-ALL-CPU-RESULTS.md)과 같다. 그 엔진의 전체 회귀 12개와 platform integrity 통과 자료를 연결했으며, 엔진 회귀를 다시 실행한 것으로 표시하지 않는다. MdViewer 소스 변경과 재빌드는 하지 않았다.

[최종 120쌍 보고서](../TWebFrame2/tests/rendering/runs/20261004-capture-calibration-v7-final/summary.html), [교정 18쌍](../TWebFrame2/tests/rendering/runs/20261004-capture-calibration-v7-final/capture-calibration/summary.json), [원본·소스 검사](../TWebFrame2/tests/rendering/runs/20261004-capture-calibration-v7-final/continuation-audit.json), [분류 근거](../TWebFrame2/tests/rendering/runs/20261004-capture-calibration-v7-final/reference-environment-classification.json)를 보존했다. 최초 입력 20개는 그대로이며 최초 기준 120개도 보관한다. 현재 기준과 최초 기준의 픽셀 일치 수량 84/120은 현재 자체 엔진 통과 수량과 다른 지표다.

최종 아카이브는 **21,368,358 bytes, 3,258개 파일**이다. [catalog](../TWebFrame2/tests/rendering/archives/20261004-capture-calibration-v7-final.json)의 모든 파일과 snapshot 소스 **129개**를 복원·검증했고, 복원한 비교 코드로 **120쌍의 판정과 모든 raw 수치를 재현**했다. 미등록 상태의 최초 96/120 실행과 환경 계측을 보강한 raw 실행도 별도 ZIP/catalog로 보존한다. 이전 기준과 실패 결과를 덮어쓰지 않는다.

복원한 실행기·DLL·20개 입력·계측/비교 코드만으로 새 프로세스에서 **전체 120쌍을 다시 렌더링**했다. 기준·자체·CDP 이미지의 디코딩 픽셀 서명, 양쪽 DOM/스타일/문자 기하 진단, 96 exact·24 승인 판정이 모두 같았다. 기준 GPU fingerprint도 유지했다. [복원 및 새 렌더링 검증](../TWebFrame2/tests/rendering/archives/20261004-capture-calibration-v7-final-recovery-validation.json)과 별도 **2,494개 파일·5,065,572 bytes**의 [복원 증거 catalog](../TWebFrame2/tests/rendering/archives/20261004-capture-calibration-v7-final-recovery-evidence.json)를 보존하고 모든 ZIP entry를 해시 검증했다.

## 남은 범위

현재 corpus는 계속 20문서다. native computed style의 초기값과 누락 검출, client/scroll 정규화, reference per-character glyph/baseline과 cluster coverage, 실제 Windows 두 DPI의 전체 행렬·모니터 전환·독립 화면/back-buffer 캡처는 남아 있다. 실제 창 96 DPI의 출력 비교를 전체 두 표시 설정의 완료로 간주하지 않는다. 이를 보완한 뒤 100문서 단계로 확대한다. 전체 1,000문서·6,000쌍 계약은 미완료다.

# 계산 스타일 초기값·누락 검출 검증 결과

2026-10-04, TWebFrame2 Windows x64 Release. 최종 실행은 `20261004-required-styles-v8-final2`다. 원본 corpus 20개는 변경하지 않았다. MdViewer를 변경하거나 재빌드하지 않았다.

원본 **120/120 성공**을 유지하며 계산 스타일 비교를 10개 부분 검사에서 **25개 필수 속성**으로 확장했다. 필수 값 **17,400개**를 비교했고 누락·스타일·DOM·상자·문자 기하 실패는 0이다. 픽셀 완전 일치는 84쌍, 기존 서명으로 승인된 CPU/GPU 차이는 36쌍이다. 전체 계약과 1,000문서 검증은 완료되지 않았다.

## 변경과 판정 범위

이전 대표 viewport의 추적 요소 116개 중 113개는 native `white-space` 값이 없어 비교에서 건너뛰었다. 새 캡처 schema v4는 양쪽 `styleContractVersion:1`과 모든 필수 `computedStyles` 값을 요구한다. 누락·빈 값은 `FAIL_STYLE`, 계약 버전 누락·미지원은 `HARNESS_ERROR`다. backend 픽셀 예외는 이러한 오류를 통과시키지 않는다.

필수 속성은 display, position, visibility, box-sizing, white-space, direction, text-align, font-style, font-weight, font-kerning, pointer-events, overflow-x/y, flex-direction/wrap/grow/shrink, list-style-position/type, opacity, letter-spacing, line-height, font-size, color, background-color다. enum/논리 정렬, 숫자 표기, CSS 길이와 sRGB RGBA8 색 표기를 정규화한다. 숫자는 0.000001, 길이는 기존 1/64 CSS px 한계이며 픽셀 허용치는 계속 0이다.

native 진단은 실제 layout/paint의 초기값·폰트 크기·줄 높이·배경색 해석 함수를 사용한다. 원래의 sparse 스타일 맵도 별도로 보존한다. `display:none` 요소 및 그 하위 요소의 계산 스타일을 기록하고 실제 상자 유무와 분리한다. `visibility:hidden`은 상자가 있는 요소로 비교한다. 이전 캡처는 기존 부분 검사와 건너뛴 값 수를 그대로 표시하며 새 계약을 통과한 것으로 표시하지 않는다.

이 계측으로 드러난 공통 오류를 수정했다.

- inline SVG의 UA overflow는 hidden이며 author 규칙으로 바꿀 수 있다. [Chromium SVG UA 스타일](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/blink/renderer/core/css/svg.css)을 참고하고 현재 WebView2의 값을 직접 검증했다.
- checkbox/radio/range를 제외한 input과 메뉴 select의 UA overflow clipping을 cascade 이후 적용했다. author `!important`에도 clip을 유지하는 현재 런타임 동작과 [Chromium HTML UA 스타일](https://raw.githubusercontent.com/chromium/chromium/main/third_party/blink/renderer/core/html/resources/html.css)을 확인했다. 다른 form 외형이나 listbox 지원을 완료한 것은 아니다.
- `background:currentColor`를 위치 토큰으로 잘못 분류하던 shorthand 파서를 수정했다.
- RTL 컨테이너에서 여유 폭이 있는 고정 폭 블록을 오른쪽에 배치했다. auto margin과 기존 HTML alignment 처리를 유지했다.
- GPU 진단은 초기화 직후의 빈 GPU0/renderer/Skia 정보를 준비 완료로 처리하지 않고 실제 활성 GPU·renderer·backend·display 정보를 기다린다.

## 검증 결과

| 검사 | 결과 |
| --- | --- |
| 원본 20 × 2 DPI × 3 viewport | 120/120 성공: 84 exact + 기존 승인 36 |
| 필수 스타일 값 | 17,400개, 누락 0 |
| WebView2 CapturePreview/CDP 독립 캡처 | 120/120 픽셀 일치 |
| 실제 96 DPI WM_PRINTCLIENT | 60/60 일치 |
| 비교 고의 오류·정규화 교정 | 134개 통과: 기존 47 + 스타일 87 |
| 초기값·상속·숨김·UA overflow 3문서 × 6행렬 | 스타일/DOM/상자 18/18, 기대값 660/660 |
| 위 별도 문서의 전체 strict 비교 | 12/18 통과, 폼 페인트 6쌍 실패 유지 |
| RGB·알파·DPI·viewport 독립 교정 | 18/18 strict, 표본 357/357 |
| 기존 전체 회귀 | 12개 실행기 + platform integrity 통과 |
| 원본 입력·소스 검증 | 20문서와 소스 140개 해시 확인 |

별도 교정 3문서는 corpus 수량에 합산하지 않는다. `style-initial`, `style-inheritance`는 12쌍 모두 strict 통과했다. `style-ua-overflow`의 6쌍은 스타일·기하·캡처가 통과해도 페인트가 달라 **FAIL_PAINT**다. 96 DPI에서 125픽셀, 144 DPI에서 275픽셀이 다르고 최대 채널 차이는 31이다. checkbox, select 화살표, button 경계에 회색 차이가 집중되지만 이 사실만으로 CPU/GPU 원인을 확정하지 않았다. 새 예외를 등록하지 않았고 기존 registry 72개는 유지했다.

[별도 교정 전체 결과](../TWebFrame2/tests/rendering/runs/20261004-required-styles-v8-final2/style-calibration/summary.json), [노드별 페인트 분석](../TWebFrame2/tests/rendering/runs/20261004-required-styles-v8-final2/ua-control-paint-analysis.json), [페인트 확대 비교](../TWebFrame2/tests/rendering/runs/20261004-required-styles-v8-final2/ua-control-paint-inspection.png)에 실패를 보존한다. 확대 이미지는 왼쪽 reference, 가운데 native, 오른쪽 raw diff다.

## 기준 환경과 이전 결과

WebView2는 `154.0.4258.53`이며 최신 실제 활성 GPU는 NVIDIA GeForce GTX 1080 Ti, ANGLE/D3D11, Skia GaneshGL이다. 행렬 전후 GPU 기록 12개의 fingerprint는 `BADDA38943711751B7B10FF65B6C5B3DA88E488AA556FFA0AA26A98459CB4F27`로 안정적이다. 실제 창 DPI는 96이고 remote session이다.

이전 Intel UHD 770 환경의 96 exact/24 승인 기준을 그대로 보존했다. 최신 native 픽셀은 이전 120쌍과 모두 같고, reference DOM·raw style·글자 기하도 모두 같다. reference 픽셀 36쌍은 이전 Intel 기준과 다르지만 앞선 `20261004-all-cpu-scroll-final`의 reference 픽셀과는 120쌍 모두 같다. 이번 84/36 판정은 기존 승인 서명을 재사용한 결과다. 과거 GPU가 미계측이었던 실행의 GPU를 소급하여 NVIDIA로 단정하지 않는다.

[최종 summary](../TWebFrame2/tests/rendering/runs/20261004-required-styles-v8-final2/summary.json), [연속성·소스 감사](../TWebFrame2/tests/rendering/runs/20261004-required-styles-v8-final2/style-continuation-audit.json), [회귀 기록](../TWebFrame2/tests/rendering/runs/20261004-required-styles-v8-final2/regression/validation.json)을 함께 보존했다.

## 보존과 다음 작업

최종 ZIP은 21,773,963 bytes, 3,867개 파일이며 SHA-256은 `BD2071D1D39DD5B7C39258F49F19F6E9884EB6EC293DE9C30DADDE88F82FB88C`다. [catalog](../TWebFrame2/tests/rendering/archives/20261004-required-styles-v8-final2.json), [불변 baseline](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-c0cb0c35362866a8-20261004-required-styles-v8-final2/manifest.json)을 생성했다. 수정 전 교정 실패와 GPU 초기화 오류·예비 실행도 `20261004-required-styles-v8-final.zip`에 별도로 보존했다. 기존 ZIP·입력·기준을 덮어쓰거나 삭제하지 않았다.

최종 아카이브의 3,867개 파일과 소스 140개를 새 경로에서 복원했다. 복원한 비교 코드로 원본 120쌍의 84 exact/36 승인 판정과 raw 수치를 재현했다. 이어서 보관한 실행기·DLL·입력·계측 코드로 **원본 120쌍과 추가 스타일 교정 18쌍을 새로 렌더링**했다. reference/native/CDP 픽셀, DOM·raw/필수 스타일·문자 좌표와 GPU fingerprint가 모두 같았다. 추가 교정의 strict 12 통과/폼 페인트 6 실패도 그대로 재현했다.

[복원·새 렌더링 검증](../TWebFrame2/tests/rendering/archives/20261004-required-styles-v8-final2-recovery-validation.json)과 [복원 증거 catalog](../TWebFrame2/tests/rendering/archives/20261004-required-styles-v8-final2-recovery-evidence.json)를 별도로 보존했다. 증거 ZIP은 13,058,222 bytes, 4,014개 파일이며 SHA-256은 `52FDFF92CCB6CFA5EF222A9D5178146DB46A239BC26392EFD58F3E76D9B474F7`다. 모든 보관 entry의 해시를 확인했다. 원본 최종 ZIP은 수정하지 않았다.

다음은 새 폼 페인트 6쌍의 원인 분류/공통 수정과 client/scroll 정규화다. 이어서 나머지 계산 스타일, reference per-character glyph/baseline·cluster coverage, 실제 Windows 두 DPI/화면 캡처 통합을 보강한 뒤 100문서 단계로 확대한다.

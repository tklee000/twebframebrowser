# 커닝·플랫폼 글꼴 선택 후속 결과

실행일: 2026-10-03, Windows x64 Release, 한국어 UI, WebView2 `154.0.4258.53`. 보존 실행 ID는 `20261003-184127-080-f8191e3e`다. [계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)의 다음 글꼴 실행 단위를 진행했고 [앞선 글꼴·폼 결과](RENDERING-ACCURACY-TEXT-CONTROLS-RESULTS.md)는 유지한다.

기본 문서는 **20개**다. 두 DPI와 세 화면 크기의 **120쌍 중 84 통과·36 실패**이며 기존 통과의 퇴행은 없다. DOM·활성 스타일·추적 상자·글자 기하 실패는 0이고 36쌍은 최종 픽셀 차이로 실패한다. 글자 계측이 있는 24쌍도 모두 좌표 판정을 통과했다. 전체 계약과 1,000개 단계는 미완료다.

## 공통 엔진 수정

Segoe UI는 GPOS `kern`과 legacy `kern`을 함께 가지고 있었다. 별도 DirectWrite probe로 pair kerning과 typography feature를 분리해 측정했다. `Latin text AV To`의 24px advance는 둘 다 켰을 때 163.441406px, GPOS만 사용했을 때 163.628906px였다. 기준 inline-block 폭은 163.640625px이고 `font-kerning:none`은 167.625px다. 기존 1/64 layout 단위 판정과 허용치를 유지했다.

실제 shaped run의 face에 GPOS `kern`이 있으면 legacy pair kerning을 켜지 않는다. legacy-only face에서는 pair kerning을 유지하고 `none`은 양쪽을 끈다. 글꼴 이름이나 문서·문자열별 우회는 넣지 않았다. [Microsoft의 legacy pair API](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_1/nf-dwrite_1-idwritefontface1-getkerningpairadjustments)는 legacy table과 GPOS가 별개임을 설명한다.

이 환경의 WebView2 platform font usage에서 기본 `monospace`는 **GulimChe(굴림체)**였지만 native는 Consolas를 선택했다. 한국어 Windows UI의 generic monospace 선택을 굴림체로 맞추고 해당 파일 해시와 실행 locale을 보존한다. 작성자가 Consolas를 명시하면 그 face를 사용한다. 실측과 [Chromium 한국어 Windows font 설정](https://chromium.googlesource.com/chromium/src/+/3ae0858daf3a68dfb7926691b8f403563b002b32%5E%21/)을 함께 확인했다. 다른 UI 언어·사용자 변경 font preferences는 이번 검증 범위에 포함하지 않는다.

실제 선택 face의 EBLC bitmap 크기 목록에 해당하는 경우에만 GDI compatible advance를 적용한다. bitmap 크기가 없는 작은 글자는 natural advance를 유지하고 작성자 letter-spacing을 더한다. font-box·normal line-height도 실제 face에서 얻으며 기존 generic monospace의 임의 메트릭을 제거했다. [GetGdiCompatibleMetrics](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritefontface-getgdicompatiblemetrics)의 장치 배율을 사용하고 폭·높이·paint layout cache에 DPI를 반영했다. 다중 문자/glyph cluster를 독립 문자로 나누어 보정하지 않는다.

Hangul fallback 전에 선택 face의 실제 glyph 존재 여부를 확인해 굴림체에 있는 한글이 Noto Sans KR로 바뀌던 문제도 수정했다. 본문 폭 측정과 paint는 공통 character spacing 경로를 사용한다.

## 독립 재현

[최소 재현](../TWebFrame2/tests/rendering/repros/README.md)은 17 → **20개**다. 기존 17개 HTML/CSS와 metadata 이력은 보존했다. 새 문서는 `monospace-platform-fonts`, `kerning-fallback-runs`, `kerning-fallback-runs-v2`다. v2는 부모 font strut을 Segoe UI로 명시하고 intrinsic width를 추가한 별도 입력이다. 기본 글꼴 선택 차이가 있는 v1도 유지했다.

| 검사 | 이번 결과 |
| --- | --- |
| 기존 `font-kerning-pairs-v2`의 Segoe UI 15/24px auto/normal/none | 두 DPI의 문자 좌표 모두 통과 |
| 기존 `normal-font-lines`의 12 font/size 조합 | 두 DPI의 상자·문자 좌표 모두 통과 |
| 기존 `normal-regression-layouts-v2`의 grid와 monospace inline padding | 두 DPI의 상자·문자 좌표 모두 통과 |
| 새 GPOS/legacy 혼합·none·상속·intrinsic width v2 | 두 DPI의 상자·문자 좌표 모두 통과 |
| 새 monospace 진단 27개 행 | 두 DPI 각각 21개 행의 문자 좌표·actual font 일치; ui-monospace 6개 행은 실패 유지 |

새 monospace 진단은 9/11/13/16/20/24px, 공백, missing family, letter-spacing, 혼합 한글을 포함한다. 13px 굴림체 Latin 한 글자 폭은 96 DPI에서 7px, 144 DPI에서 6⅔ CSS px다. `ui-monospace`는 기준에서 Noto Sans KR로 fallback하고 native는 Consolas를 선택해 각 DPI에서 402개 좌표 차이가 남는다. 부모의 Noto Sans KR/Segoe UI strut 차이도 최초 혼합 재현의 144 DPI 실패로 보존했다.

글자 좌표가 맞아도 strict 페인트는 아직 다르다. 최소 재현 20개 × 두 DPI의 **추가 40쌍은 4 통과·36 실패**다. 이 수량을 기본 120쌍에 합산하지 않는다. 원본 입력과 픽셀 허용치는 변경하지 않았다.

## 검사·보존

전체 회귀 실행기 **12개와 platform integrity가 통과**했다. 사용자 요청으로 기존 monospace/Consolas 동일성 회귀검사와 전용 fixture를 삭제하고 TWebFrameTests·platform integrity를 다시 실행해 통과했다. 새 독립 reference 기반 GPOS 폭·bitmap DPI cache·기존 glyph 보존 검사는 잘못된 동일성 가정을 사용하지 않는다.

고의 오류 28개와 schema 22개가 통과했다. 최초 20개 입력 해시, 최초 120개 reference PNG의 디코딩 픽셀, 앞선 3개 아카이브는 동일하다. 수정 전후 별도 재현 10쌍의 reference 픽셀도 동일했다. [실행 감사](../TWebFrame2/tests/rendering/runs/20261003-184127-080-f8191e3e/continuation-audit.json)는 source 95개·font 파일 23개 해시, stable capture/DPI 왕복 120쌍, 실제 창 DPI 144의 `WM_PRINTCLIENT` 60쌍 일치를 기록한다. 소스 snapshot에서도 요청한 회귀검사 삭제를 확인했다.

기본 행렬은 약 133.95초, 최대 문서 캡처 1.36초, 실행기 peak working set 145,707,008 bytes였다. WebView2 자식 프로세스 메모리는 포함하지 않는다. 실행 전후 진단·probe·실패 및 성공 회귀 로그를 함께 보존한다. 커밋·원격 업로드는 수행하지 않았다.

## 남은 작업

기본 36쌍의 페인트, ui-monospace/default fallback, widget 내부 텍스트, reference per-character glyph·baseline·다중 cluster coverage 및 AA 교정이 남는다. radius/gradient/shadow/SVG, 전체 스타일·scroll/client·pseudo/iframe 대응, 실제 Windows 96 DPI/모니터 전환, 독립 화면·색 교정도 미완료다. `fullContractComplete`와 `actualWindowsBothDpiValidated`는 false다. 계약과 pilot 차이를 해결한 뒤 100 → 1,000개로 확대한다.

# 글꼴·폼 후속 수정 및 검증 결과

실행일: 2026-10-03, Windows x64 Release, WebView2 `154.0.4258.53`. 보존 실행 ID는 `20261003-174213-996-9c018c59`다. [계획](RENDERING-ACCURACY-VALIDATION-PLAN.md)의 mixed writing과 form 분석을 진행했으며, [앞선 84/120 결과](RENDERING-ACCURACY-CONTINUATION-RESULTS.md)도 유지한다.

기본 행렬은 **20개 × 96/144 DPI × 3 viewport = 120쌍, 84 통과·36 실패**다. 기존 통과 84쌍은 유지했다. 글자 계측이 있는 24쌍은 **12 통과·12 실패 → 24 통과·0 실패**로 바뀌었다. 활성 판정의 DOM·스타일·추적 상자·글자 기하 실패는 없어졌고, 남은 기본 행렬 실패는 모두 strict 페인트다. 최종 통과 수량에는 변화가 없으며 전체 계약과 1,000개 단계는 미완료다.

| 기본 문서 | 이번 활성 기하·스타일 판정 | 최종 판정 |
| --- | --- | --- |
| 0017 Latin inline | 60개 문자 × 6쌍의 좌표 통과 유지 | 0/6, 페인트 실패 |
| 0018 mixed writing/RTL | 48개 문자 × 6쌍의 좌표 통과 | 0/6, 페인트 실패 |
| 0019 form initial state | 추적 상자·활성 스타일·버튼 6개 문자 × 6쌍 통과 | 0/6, widget·텍스트 페인트 실패 |
| 0020 dashboard | 40개 문자 × 6쌍의 좌표 통과 유지 | 0/6, 페인트 실패 |
| 0012 radius/gradient, 0013 SVG | 기존 활성 구조 판정 유지 | 각각 0/6, 페인트 실패 |
| 기존 통과 14개 문서 | 기하와 정확한 장치 픽셀 통과 유지 | 84/84 |

폼의 `input` 값과 `select` option의 widget 내부 텍스트는 현재 authored DOM 문자 Range 대응에 포함되지 않는다. 위 글자 수량은 현재 계측 범위이며 모든 표시 글자의 정확성 완료를 뜻하지 않는다. [전체 결과](../TWebFrame2/tests/rendering/runs/20261003-174213-996-9c018c59/summary.html)와 [소스·원본·기준 감사](../TWebFrame2/tests/rendering/runs/20261003-174213-996-9c018c59/continuation-audit.json)를 보존했다.

## 공통 수정

`font-kerning`의 auto/normal/none과 상속을 처리하고, DirectWrite의 pair kerning 및 OpenType `kern` 기능을 text measurement와 실제 text layout에 적용했다. 관련 캐시 키에도 이 속성을 포함했다. [SetPairKerning](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_1/nf-dwrite_1-idwritetextlayout1-setpairkerning)과 [OpenType font feature](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/ns-dwrite-dwrite_font_feature)의 두 경로를 검사했다. Arial·Malgun Gothic·Times New Roman의 auto/normal/none 및 상속 재현은 두 DPI 모두 현재 문자 좌표 판정을 통과한다.

Malgun Gothic의 legacy `kern`에서는 DirectWrite와 HarfBuzz가 같은 조정을 logical 문자 advance에 배분하는 방식이 다르다. [GetKerningPairAdjustments](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_1/nf-dwrite_1-idwritefontface1-getkerningpairadjustments)와 [HarfBuzz legacy kern 구현](https://github.com/harfbuzz/harfbuzz/blob/main/src/hb-kern.hh)을 확인했다. GPOS `kern`이 없는 face의 LTR legacy pair에 한해 측정 JSON의 문자 경계를 정규화했다. 실제 DirectWrite `rawHitRect`, advance, placement offset, cluster 위치도 함께 보존한다. 문자 경계는 장치 layout unit의 바깥 경계로 표현하며 좌표 허용치는 기존 **1/64 CSS px**다. 픽셀은 계속 원본 전체를 엄격히 비교한다.

`line-height: normal`은 ascent·descent·line gap을 장치 공간에서 각각 반올림한 뒤 CSS 좌표로 변환한다. baseline과 generic monospace의 normal baseline도 같은 DPI 메트릭을 사용한다. [Blink font metrics 구현](https://chromium.googlesource.com/chromium/src/+/HEAD/third_party/blink/renderer/platform/fonts/simple_font_data.cc)과 실제 WebView2 재현으로 확인했다. 예를 들어 Arial 16px의 normal 높이는 96 DPI에서 18px, 144 DPI에서 18⅔ CSS px다. Segoe UI 13px은 각각 17px, 17⅓ CSS px다. 정상적인 DPI 차이를 같은 CSS 높이로 강제하던 기존 회귀 기대값을 교정했다.

UA input/select/button의 padding·border·color·white-space를 보완했다. 버튼은 normal whitespace로 자동 줄바꿈하며 natural glyph advance로 폭을 계산한다. submit/button/reset input의 intrinsic 폭에도 label과 decoration을 사용한다. select의 높이는 rounded font-box, border/padding 및 native appearance의 물리 픽셀 여유를 반영한다. 작성자 스타일의 override와 `appearance:none`도 재현으로 확인했다.

14px Arial 재현에서 input/button은 두 DPI 모두 높이 22px, 기본 select는 96 DPI에서 20px·144 DPI에서 18px다. 70px 폭의 3줄 버튼은 두 DPI 모두 높이 54px다. 원본 form의 800×600 픽셀 차이는 96 DPI에서 **5,335 → 2,685**, 144 DPI에서 **9,930 → 5,779**로 줄었지만 여전히 실패다.

## 계측과 검출력

실제 `IDWriteTextLayout::Draw`의 shaped run에서 대표 glyph index·advance·placement offset·cluster와 family/face/PostScript 이름을 수집한다. 이번 기본 행렬의 **924개 계측 문자**는 glyph와 플랫폼 글꼴 정보를 읽었으며 `.notdef`가 없다. 실제 PostScript face는 ArialMT, Arial-BoldMT, MalgunGothic이다. 다중 glyph cluster 전체 coverage는 아직 이 대표 glyph 계측과 구분한다.

WebView2에는 읽기 전용 [CSS.getPlatformFontsForNode](https://chromedevtools.github.io/devtools-protocol/tot/CSS/#method-getPlatformFontsForNode)를 추가해 **696개 추적 노드**의 실제 글꼴 usage를 `reference-fonts.json`으로 보존했다. 결과는 `reference.boxes`와 같은 문서 순서의 노드 집계이며 per-character glyph나 기준 baseline 자체는 아니다. `DOM.getDocument`는 depth 0의 document node를 조회해 doctype의 nodeId와 혼동하지 않도록 했다. 페이지 스크립트는 계속 비활성화한다.

고의 오류 검사는 **22 → 28개, 전부 검출 기대값 통과**다. null/NaN/무한 좌표, `.notdef`, 미수집 glyph, 미해결 font 정보를 추가로 거절한다. 기존 1 장치 픽셀 이동·글자 폭/줄/내용/원본 위치 변경 검사도 유지했다. `.notdef`의 대표 glyph 검사와 실제 기준 glyph face 일치·전체 coverage 검사는 서로 다른 범위다. AA 허용치와 허용 픽셀 수는 계속 0이다.

`Run-SupportCompatibility.ps1 -FullRegression`에 FormControlRegression을 추가해 **12개 실행기와 platform integrity 모두 통과**했다. CSS native 290개도 0 실패다. 버튼 normal whitespace, normal 줄 높이와 DPI cache 왕복, UA 크기·줄바꿈·작성자 override, 실제 glyph face 기록을 회귀검사했다. grid 버튼의 메트릭 검사는 Segoe UI를 명시한 WebView2 재현과 맞췄다. 기본 글꼴 선택이 다른 최초 재현도 별도로 보존했다. [회귀·schema 기록](../TWebFrame2/tests/rendering/runs/20261003-174213-996-9c018c59/verification/checks.json)은 schema 22개 통과도 포함한다.

## 추가 재현과 남은 차이

[최소 재현](../TWebFrame2/tests/rendering/repros/README.md)은 11 → **17개**이며 이전 manifest와 입력은 유지했다. kerning, normal 줄 높이, UA control 크기 및 기존 regression의 정적 대응을 추가했다. 같은 보관 실행기로 96/144 DPI·960×660에서 추가 **34쌍: 4 통과·30 실패**를 기록했다. 이 수량을 기본 corpus에 합산하지 않는다.

UA control 크기 재현은 추적 좌표와 계측 문자가 통과해도 widget 페인트 때문에 실패한다. normal 줄 높이 재현은 12개 font/size 조합의 상자 높이가 맞으며 Segoe UI의 일부 문자 advance 차이가 남는다. 확장 kerning 재현에서는 Segoe UI auto/normal의 15/24px 행에 각각 8개 좌표 차이가 남고 none 행은 통과한다. GPOS/legacy pair 선택·배분을 더 분석해야 한다. generic monospace의 폭, 기본 글꼴 Noto Sans KR와 native Segoe UI의 선택 차이, 일부 hidden/inline/image 조합도 실패로 보존한다.

보관 실행기 수정 전후의 800×700 독립 재현 3개 × 두 DPI에서 기준 PNG의 디코딩 픽셀은 동일했다. [별도 검증 기록](../TWebFrame2/tests/rendering/archives/20261003-174213-996-9c018c59-recovery-validation.json)에 비교 범위와 남은 실패를 기록했다.

## 보존과 복원

최초 HTML/CSS 20개 해시와 최초 기준 PNG 120개의 디코딩 픽셀은 동일했다. 앞선 두 아카이브도 변경되지 않았다. 이번 소스·계측 파일 **94개**, 폰트 파일 **22개**의 해시를 검증했다. 120쌍 모두 양쪽 캡처 및 native DPI 왕복이 안정적이고, 실제 창 DPI 144의 `WM_PRINTCLIENT` 60쌍도 일치했다.

새 [catalog](../TWebFrame2/tests/rendering/archives/20261003-174213-996-9c018c59.json)와 [ZIP](../TWebFrame2/tests/rendering/archives/20261003-174213-996-9c018c59.zip)은 **3,167개 파일, 12,826,188 bytes**다. 원본 입력·재현, 수정 전후 진단, 모든 PNG/JSON/로그, 소스 snapshot과 실행기를 포함한다. SHA-256은 `E7B3D62DDAF765F488B8A398476C46E44827C1838CF73153E710D56947ECFEB9`다. [불변 기준 인덱스](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-9aabf582f5d96aab-20261003-174213-996-9c018c59/manifest.json)는 새 reference font 자료도 가리킨다.

3,167은 해시 검증한 보관 파일 수다. 테스트 문서는 원본 20개와 별도 재현 17개이며, 아카이브에는 PNG 1,019개, JSON 1,842개, HTML/CSS·텍스트·로그·소스 ZIP·실행기 등 나머지 306개가 들어 있다. DPI·viewport별 양쪽 결과, 반복 캡처, diff와 수정 전후 증거를 따로 보존하므로 문서 수보다 파일 수가 많다. ZIP 자체의 `archive-manifest.json` 목록은 이 검증 대상 수량과 별도다.

새 위치 `restored/20261003-text-controls-9c018c59`에서 모든 entry 해시를 다시 검증했다. 복원한 소스의 비교 코드로 120쌍을 재판정해 **84/120 및 전체 실패 목록이 동일**했다. 보관 실행기로 table/Latin/mixed/form/dashboard를 두 DPI에서 새로 렌더링한 **10쌍**은 원래 실행과 reference/native 픽셀, 문자 좌표, reference font usage가 모두 동일했다. 해당 문서의 strict 페인트 실패도 같은 결과로 재현됐다. [복원 검증](../TWebFrame2/tests/rendering/archives/20261003-174213-996-9c018c59-recovery-validation.json)과 [별도 복원 증거 ZIP](../TWebFrame2/tests/rendering/archives/20261003-174213-996-9c018c59-recovery-evidence.zip)의 656개 파일·1,023,577 bytes도 해시 검증해 보존한다.

기본 행렬 실행은 약 132.17초, 최대 문서 캡처 1.34초, 실행기 peak working set 146,153,472 bytes였다. WebView2 자식 프로세스 메모리를 포함하지 않는다. 원 run과 profile은 유지하며 커밋·원격 업로드는 수행하지 않았다.

다음 작업은 남은 Segoe UI/monospace/fallback 메트릭과 widget 내부 텍스트를 보완하고, 실제 glyph coverage·기준 baseline·고의 오류 검출력을 연결해 글꼴 페인트의 AA 교정을 진행하는 것이다. radius/gradient/shadow/SVG 페인트, 전체 스타일·scroll/client·pseudo/iframe 대응, 실제 Windows 96 DPI/모니터 전환, 독립 화면/색 교정, 100→1,000개 확대는 남아 있다. `fullContractComplete`와 `actualWindowsBothDpiValidated`는 모두 false다.

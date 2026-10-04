# 렌더링 최소 재현

원본 pilot 20개와 분리한 정적 진단 문서 27개다. 입력과 설명은 `manifest.json`에 기록한다. 기존 버전은 유지하며, 내용이 달라진 진단은 v2/v3 새 디렉터리로 추가했다. 각 HTML/CSS SHA-256과 목록 수량을 실행 전에 확인한다. 최신 `20261004-050655-292-8ce5100e`의 추가 54쌍은 **23 통과·31 strict 실패**다. 이전 `20261003-232442-397-6a3e5f39`의 20 통과·34 실패도 유지한다. 원본 radius/gradient의 사용자 승인 예외는 이 진단 문서들에 적용하지 않는다. 기본 120쌍과 별도로 집계한다.

metadata v2는 재캡처 후 quirks min-height 설명을 기존 유지 동작으로 교정했다. HTML/CSS 해시는 바꾸지 않았으며 최초 metadata는 `manifest-v1.json`과 실행 아카이브의 input manifest에 보존한다.

metadata v3는 kerning·normal 줄 높이·UA control 크기 3개를 추가했고, 현재 v4는 Segoe UI kerning과 기존 normal/grid 회귀의 대응 3개를 더 보존한다. 이전 목록은 `manifest-v2.json`과 `manifest-v3.json`에 유지한다. `font-kerning-pairs-v2`는 Segoe UI 15/24px 행을 추가한 별도 입력이고, `normal-regression-layouts-v2`는 기본 글꼴 차이를 분리하기 위해 Segoe UI를 명시한 별도 입력이다. v1의 기본 글꼴 선택 차이도 그대로 실패로 남긴다.

`collapsed-table-border`는 공유 경계와 colspan, `inline-wrapped-leading-space`는 inline 형제 사이 자동 줄바꿈을 분리한다. DPI 문서는 글꼴별 font-box, inline badge/label/image의 baseline, 이미지 뒤 `<br>`의 줄 높이를 검사한다. hidden 속성의 첫 진단과 `display:none` 후속 진단을 구분한다. 빈 `src`의 이미지 상자는 원 회귀검사의 정적 대응이며, 실제 URL이 선언된 이미지는 로드 완료를 계속 요구한다.

서로 다른 모서리 radius는 남은 실패를 보존하고, quirks 백분율 min-height는 기존에 확인한 동작을 유지하는 재현이다. 이 진단 문서의 성공을 원본 문서나 전체 페인트 계약의 성공으로 처리하지 않는다. 글자의 기하가 맞아도 AA·다른 페인트가 다르면 strict 비교에서 실패한다.

기본 행렬을 캡처한 뒤 같은 실행의 보관 실행기로 추가 증거를 만든다.

```powershell
& .\TWebFrame2\tests\rendering\harness\Run-RenderingRepros.ps1 `
  -RunPath .\TWebFrame2\tests\rendering\runs\<new-run-id>
```

96/144 DPI, 960×660 CSS px의 추가 54개 비교는 `repro-evidence/`에 저장하고 기본 120개와 따로 집계한다. 실행기·계측 JS·입력 해시를 검사하며 기존 증거가 있으면 덮어쓰기를 거절한다. 아카이브는 이 결과와 `repros/` 원본 전부를 포함한다. 이전 40쌍의 결과는 4 통과·36 실패였으며 이전 22쌍도 유지한다. kerning의 Arial/Malgun/Times 문자 좌표와 UA control의 활성 상자·문자 좌표가 통과해도 strict 페인트 차이는 실패다. 기본 fallback 선택과 hidden/image 조합의 차이는 남는다. [글꼴·폼 결과](../../../../docs/RENDERING-ACCURACY-TEXT-CONTROLS-RESULTS.md)를 따른다.

metadata v5/v6는 실제 플랫폼 monospace, GPOS/legacy 혼합 runs 및 부모 font strut을 명시한 v2를 추가했다. 직전 목록을 manifest-v4/v5.json에 보존했다. Segoe UI 확장 kerning·normal-font-lines와 normal-regression-layouts-v2의 활성 상자·문자 좌표는 두 DPI 모두 통과한다. monospace-platform-fonts는 각각 21개 행의 문자 좌표·실제 font가 일치하고 ui-monospace 6개 행의 fallback 차이는 남는다. 최신 상세는 [커닝·글꼴 선택 결과](../../../../docs/RENDERING-ACCURACY-FONT-SELECTION-RESULTS.md)를 따른다.

metadata v7/v8은 네 모서리·타원형 radius, gradient origin, shadow DPI, native control와 author paint의 재현 5개를 추가했다. 이전 manifest-v6/v7.json과 입력 20개를 유지했다. 기본 corpus와 분리한 50쌍은 4 통과·46 실패다. corner v2는 grid와 명시적 body 높이로 페인트를 분리하며, v1의 flex body 자동 높이와 별도 select intrinsic width 차이도 실패로 유지한다. [페인트 수정 결과](../../../../docs/RENDERING-ACCURACY-PAINT-RESULTS.md)를 따른다.

metadata v9는 `msaa-path-coverage`와 `centered-control-text`를 추가하고 변경 전 목록을 `manifest-v8.json`에 보존했다. 이전 목록의 `documentCount=20`은 실제 25개와 불일치했으므로 새 목록에서는 27로 바로잡았다. 모든 기존 HTML/CSS는 유지한다. MSAA 문서는 경로 길이·방향·크기, 원형 ring, 소수 좌표 rect와 closed stroke를 검사한다. 버튼 문서는 Arial 14/18/24px, 자연/고정/소수 폭과 세 글자 색의 중앙 정렬을 검사한다. 두 문서는 원본 성공을 대신하지 않으며, 새 버튼 문서도 144 DPI strict 실패를 유지한다. [최신 래스터 결과](../../../../docs/RENDERING-ACCURACY-RASTER-RESULTS.md)를 따른다.

# 표·textarea 교정 재개와 CPU/GPU 페인트 원인 검증 v11

작성일: 2026-10-04. Windows x64 Release, WebView2 `154.0.4258.53`, 명시적 96/144 DPI와 세 CSS viewport. 현재 Windows 창 DPI는 96이다. 실제 Windows 두 DPI 전환과 전체 1,000문서 계약은 미완료다.

중단했던 v10의 소스 169개와 저장 실행기 해시를 확인하고 재개했다. v10의 표·textarea 공통 수정은 유지하고, CPU 글자 합성의 반복 클립 계산과 캐시 메모리 집계를 개선했다. 크기 교정에 남은 글자·select 페인트 12쌍은 동일 입력의 독립 GPU 실험으로 원인을 확인했다. 일반 픽셀 허용치는 0이며 strict 실패 수치와 이전 실행은 보존한다.

최종 수정 실행은 `TWebFrame2/tests/rendering/runs/20261004-table-form-v11-final2`이다. 앞선 `v11-final`의 204쌍 성공과 마지막 회귀 중 발견한 실패는 그대로 보존했다. 정확성·성능 검증 이후 마지막에 전체 회귀 12개와 platform integrity → 주 ZIP·불변 baseline → 새 폴더 복원·120쌍 재판정 → 204쌍 새 렌더링 → 복원 증거 ZIP 검증을 모두 완료했다.

## 유지·개선한 구현

- v10의 th 기본/상속 정렬, table-cell 최소 높이와 span 간격, 암시적 tbody와 인접 본문 텍스트 병합을 유지했다.
- textarea 초기 값의 양축 scrollbar 결합, client/scroll 캐시, offset·caret·hit-test·thumb, DPI별 advance와 grayscale 합성 교정을 유지했다.
- `RasterSurface.h`에서 glyph가 클립 안에 완전히 들어가면 기존 coverage·blend 조회표로 바로 합성한다. 소수 클립 경계의 기존 계산은 유지한다. coverage 표 768바이트를 매 glyph run마다 복사하던 작업도 제거했다.
- grayscale 마스크의 LCD 원본 3바이트 버퍼를 정확한 1바이트 버퍼로 교체했다. 벡터의 크기만 줄여 기존 할당 용량이 남던 문제를 고쳤고, 16MiB 캐시 한도는 실제 `capacity()`로 계산한다. 항목 수 4,096개 제한과 face 수명 보호는 유지한다.
- 크기 교정도 원본과 같은 승인 정책으로 비교하되 `-StrictPixels`를 지원하고 strict 성공 수를 따로 기록한다. 복원 실행기는 저장된 교정의 strict 정책을 읽으며 이전 v10 이하 실행의 strict 판정도 유지한다.
- 마지막 전체 회귀에서 150% DPI의 여러 줄 DOM 선택 영역 실패를 발견했다. 부모가 한 줄로 측정한 bitmap 글꼴 조각을 DirectWrite가 다시 줄바꿈해 phantom line과 잘못된 caret를 만들었다. 이미 한 줄로 측정한 DOM 조각은 추가 줄바꿈을 막아 공통 페인트·caret·hit-test·selection 경로를 일치시켰다. 기존 검사 조건은 유지하고 두 DPI를 모두 실행하며 실패 때 조각/선택 좌표를 기록하도록 보강했다. 수정 후 TWebFrameTests 전체는 통과했다.

MdViewer 앱은 추가 재빌드하지 않았다. 같은 보존 앱 자산·문서를 공통 View 성능 실행기에 넣었다. 현재 엔진은 CPU 합성과 소프트웨어 화면 출력을 유지한다.

## 페인트 차이의 독립 원인 실험

크기 교정 12쌍에 대해 두 DPI·세 viewport 각각 독립 실험을 했다. 실험 실행기는 기준 PNG를 렌더링 입력으로 읽지 않는다. 원래 glyph/도형·좌표·색을 렌더링하고 별도 분석기가 결과 PNG와 비교한다.

| 문서 | raw 96 DPI | raw 144 DPI | 확인한 원인 |
| --- | ---: | ---: | --- |
| geometry-boxes | 21픽셀·최대 1 | 22픽셀·최대 1 | 동일 LCD 마스크의 CPU/GPU 합성 정밀도 |
| geometry-scroll | 32픽셀·최대 31 | 49픽셀·최대 31 | select 화살표 경계의 CPU/GPU AA |

`GlyphBlendProbe.cpp`는 실제 엔진의 DirectWrite glyph 마스크를 고정한다. 동일 마스크·글자 위치·색에서 CPU 조회표 합성은 native 픽셀과 완전히 일치하고, 보존한 이전 D3D11 dual-source UNORM 합성은 WebView2 픽셀과 완전히 일치한다. 텍스트 영역 바깥에는 차이가 없다. 각 실험 입력, CPU/GPU 출력, 마스크, 분석 JSON, 실행기와 이전 GPU 헤더를 보존했다.

select는 보존한 `ControlPathProbe.exe`의 동일 geometry·color·device position과 D3D11 8×MSAA로 기준 픽셀을 완전히 재현했다. 화살표 영역 바깥 차이는 0이다. 분석기는 이제 기록한 opaque computed color를 사용하며, 원래 스타일 교정의 select ID도 지원한다.

처음 시도한 일반 Skia CPU/Ganesh 글꼴 호출은 기준을 재현하지 못했다. 이 실패도 `glyph-probe-file-*`와 `glyph-probe-f32-*`에 보존했으며 승인 근거로 사용하지 않았다. 마지막의 **동일 엔진 LCD 마스크를 고정한 합성 실험**이 승인 근거다. JSON float baseline의 장치 픽셀 반올림에는 실제 renderer와 같은 float32 연산을 사용한다.

계획에 이미 기록된 사용자 CPU/GPU 예외 승인 정책에 따라 정확한 12개 서명만 추가했다. 기존 78개를 유지하고 총 90개다. DOM·스타일·크기·문자·캡처 검사를 먼저 통과해야 하며, 양쪽 디코딩 픽셀 해시·차이 수치가 같아야 승인된다. 새 차이에는 적용되지 않는다. `Register-RenderingGeometryExceptions.ps1`는 모든 행렬의 원인 실험·입력/실행기 해시와 영역 바깥 차이 0을 확인한 뒤 등록한다.

API 선언과 비교한 공식 자료: [고정 SkiaSharp 4.153.1 C ABI](https://github.com/mono/SkiaSharp/blob/v4.153.1/binding/SkiaSharp/SkiaApi.generated.cs), [Skia DirectWrite scaler](https://skia.googlesource.com/skia/+/refs/heads/main/src/ports/SkScalerContext_win_dw.cpp), [Chromium native theme](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/ui/native_theme/native_theme_base.cc). 실제 원인 판정은 저장된 실행 환경의 독립 실험 결과에 근거한다.

## 속도와 불변 검증

v10 준비 자료의 미실행 측정을 먼저 완료했다. v9/v10를 번갈아 각 3회 실행했을 때 스크롤 페인트 중앙값은 21.47→22.06ms이고 BGRA 12쌍·레이아웃 JSON 6쌍이 동일했다. 이 결과를 보존하고 후속 최적화는 v10을 새로운 before로 사용했다.

v10/v11은 같은 Markdown·HTML/CSS/JS와 actual 96 DPI·1600×1000을 사용하고 번갈아 각 3회 실행했다. 스크롤 페인트는 각 버전 36개의 중앙값이다. 클립 최적화 직후 측정은 21.46→20.25ms였다. 선택 영역 수정까지 포함한 최종 소스를 다시 번갈아 3회씩 측정한 수치는 다음과 같다.

| 항목 | v10 before | v11 after |
| --- | ---: | ---: |
| 스크롤 페인트 중앙값 | 21.09ms | 19.94ms |
| 최초 페인트 중앙값 | 106.12ms | 107.41ms |
| WM_KILLFOCUS 후 첫 scrollbar 클릭 | 16.67ms | 14.80ms |
| 실제 thumb 드래그 | 18.34ms | 16.19ms |

최종 로컬 측정에서 스크롤 페인트는 약 5.5% 감소했다. 최초 페인트는 약 1.2% 증가해 모든 항목의 속도 향상을 주장하지 않는다. 초기/스크롤 BGRA 12쌍·레이아웃 JSON 6쌍·문서 입력 3쌍이 byte 단위로 동일하고, 첫 클릭·드래그는 모두 입력 처리 중에 동기 페인트했다. raw 시간과 입력·자산·실행기·DLL 해시는 최종 실행의 `optimization-and-backend-evidence/performance-final`에 있다. 이전 단계의 74.3→21.6ms 개선은 이번 성과로 재집계하지 않는다.

## 정확성 검사 및 마지막 검증

- [x] 원본 120/120 성공: strict 84·기존 승인 36, 비교기 고의 오류 182개 통과.
- [x] 스타일 교정 18/18: strict 12·기존 승인 6, 기대 스타일 값 660개.
- [x] 크기 교정 18/18: strict 6·새 승인 12, 크기 648개·누락 0. 이전 strict 실패 12쌍의 raw 차이는 그대로다.
- [x] 새 예외 12쌍에서 strict 거절과 native pixel·client width·computed color·DOM 고의 오류를 실제 비교기에 넣은 72개 통과.
- [x] 독립 색/DPI 교정 18/18과 table/form 교정 30/30 strict 성공.
- [x] 전체 204쌍의 native/reference/CDP/가능한 WM_PRINTCLIENT 픽셀 및 DOM·스타일·문자·크기 진단이 v10과 동일. 소스 해시 174개 일치. 150 strict·승인 54이며 raw 차이와 strict 판정 불변.
- [x] 위 검증 이후 전체 회귀 12개와 platform integrity. `regression/full-regression.log`와 `regression/summary.json`에 보존.
- [x] 주 ZIP의 20,239개 파일 해시와 불변 baseline, 새 폴더 복원, 소스 174개·원본 입력 20개·실행기/DLL 해시 검증. 복원한 비교 코드의 원본 120쌍 재판정에서 raw 수치와 판정 동일.
- [x] 복원한 코드·실행기·DLL·입력으로 204쌍 새 렌더링. 204/204 성공, strict 150·승인 54. 양쪽/CDP/가능한 WM_PRINTCLIENT 픽셀·DOM·스타일·문자·크기·raw 수치와 판정 모두 동일. 복원 증거 ZIP 5,558개 파일도 각각 해시 검증.

## 보관·복원 증거

| 자료 | 결과와 위치 |
| --- | --- |
| 주 아카이브 | [ZIP](../TWebFrame2/tests/rendering/archives/20261004-table-form-v11-final2.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-table-form-v11-final2.json). 20,239개 파일, 238,811,986 bytes. |
| 불변 기준 인덱스 | [manifest](../TWebFrame2/tests/rendering/baselines/154.0.4258.53-e07b447f037feb12-20261004-table-form-v11-final2/manifest.json). 원본 reference 120쌍, 환경 ID `e07b447f037feb12`. 교정 문서는 별도 그룹으로 주 ZIP에 포함. |
| 새 복원 폴더 | `TWebFrame2/tests/rendering/restored/20261004-table-form-v11-final2-verified`. 소스 174개·원본 20문서와 교정 입력·실행기·DLL을 확인. |
| 최종 복원 판정 | [recovery-validation.json](../TWebFrame2/tests/rendering/archives/20261004-table-form-v11-final2-recovery-validation.json). 재판정 120쌍과 새 렌더링 204쌍의 그룹별 결과·raw 수치 포함. |
| 복원 증거 | [ZIP](../TWebFrame2/tests/rendering/archives/20261004-table-form-v11-final2-recovery-evidence.zip), [catalog](../TWebFrame2/tests/rendering/archives/20261004-table-form-v11-final2-recovery-evidence.json). 5,558개 파일, 16,742,532 bytes, 모든 파일 해시 일치. |

주 ZIP SHA-256은 `FEB2E7F3C1C608C318EB5448FC43AF02ACB49FC120DB81D380813FEF007EC1E9`, 복원 증거 ZIP은 `51824947735D95DB0AC8E7702191739DFC46EA039E98F4F61ECD16C169A8C35D`다. ZIP 내부 `documentation/`은 보관 직전 상태의 스냅샷이며 최종 완료 판정은 위 복원 JSON과 이 문서를 따른다. 기존 실행·실패·아카이브를 덮어쓰지 않았다.

## 남은 전체 계획

현재 pilot 원본은 20문서이며 교정 문서를 corpus 수량에 합산하지 않는다. reference per-character glyph/baseline·cluster, 나머지 계산 스타일, RTL·stable gutter·scroll offset·transform·pseudo·iframe·root overflow 조합, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대는 남아 있다. 이번 단계의 204쌍 성공을 전체 계약 완료로 표시하지 않는다. ZIP의 원격 영구 보관도 기존 계획대로 미완료다.

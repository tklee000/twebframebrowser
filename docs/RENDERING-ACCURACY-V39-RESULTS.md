# v39 native 캡슐 정밀 판정·마스크 조회 결과

2026-10-06 완료. Windows x64 Release. 전체 회귀·반디집 보관·새 복원·828쌍 새 렌더링·native 로그 검증 완료.

- 새 독립 캡슐/clip 검사 6,912건: 변경 전 판정·픽셀 검사 실패 2,304건 → 수정 후 0. micro-fractional AA clip의 잘못된 native 수락을 포함하며 2,304개 서로 다른 픽셀이라는 뜻은 아니다.
- 미세 비균일 축·소수 경계·비정상 descriptor/DPI 거부·정상 복귀 112건: 변경 전 assertion 실패 104건 → 수정 후 0.
- 기존 glyph 배율 248,832건·guard 34건, narrow 61,440건·opacity 32,256건·guard 14건·face/factory 8건, capsule 방향 24,576건·경계 16,384건, glyph clip 3,072건·기본 192건, 일반 텍스트 23,068,672조합·clip/layer/DPI·scroll/drag/style 복귀 유지.
- 전체 828쌍이 v38의 네 이미지·전체 JSON·raw·판정과 같다. strict 727·기존 backend 승인 75, 총 802/828. 기존 화살표 FAIL_PAINT 26은 최대 채널 차이 1·raw 차이 합계 182로 유지. 비교기 182+336건 통과. 허용치 0·registry 111개 유지, 새 예외 0.
- 캡슐 성능 28조건 중앙값 변화 -7.94~+5.68% (음수는 시간 감소). 증가 조건과 매 회차 원값도 보존한다.

## 구현과 독립 검증

native 캡슐의 circular axis permutation은 device 축 절댓값이 실제로 같을 때만 허용한다. 1e-5 절대 epsilon은 1e-6/2e-6 같은 큰 상대 배율 차이나 1 ULP 축 차이를 수락하므로 제거했다. geometry는 정수 device float 또는 정수의 한 ULP 이내이며 절대 오차가 1e-4 미만인 경우만 native binary 경로를 쓴다. 화면 안의 AA clip edge는 각 변환된 edge의 기여 항 절댓값 합을 기준으로 한 ULP 이내이면서 절대 오차 1e-4 미만인 경우만 허용한다. 이는 반전/회전 clip의 cancellation 오차를 처리하기 위한 수치 범위다. 더 큰 소수 경계는 일반 Direct2D 경로로 복귀한다. bitmap 밖 clip edge는 fractional coverage에 기여하지 않으므로 bitmap 범위로 clamp한 뒤 판정한다. 이러한 ULP 오차 범위 안의 의도적 소수와 DPI 산술 오차는 float 입력만으로 구분할 수 없으며 유지하는 수치 한계다. min/max 이전에 saved clip rect 4개·matrix 6개 좌표의 유한성을 확인하고, DPI는 유한한 양수만 허용한다. invalid descriptor는 출력 sentinel과 cache entry를 변경하지 않는다. 정상 clip cache hit는 재사용한다.

signed 축 8개·배율 1/2/2^-20·thickness 2/6/10/14·RGB 2개·opacity 3개·backdrop alpha 2개·clip 3개·DPI 2개를 조합한다. authored dyadic 좌표는 두 DPI에서 정확한 정수 device 좌표로 변환된다. 기대값은 독립 double corner mapping·중앙 선분과의 거리로 구한 원형 coverage·pixel-center aliased clip·PBGRA UNORM source-over로 계산하며 엔진 cache/clip helper를 사용하지 않는다. micro-fractional AA clip은 native 거부·무잠금·모든 backdrop byte 불변을 검사한다. guard는 DPI당 비균일 축 8개·소수 geometry 8개·descriptor NaN/±infinity 30개·잘못된 DPI 10개이며 정상 clip 복귀도 확인한다. 비정상 입력은 실제 Direct2D에 전달하지 않는다.

캡슐 mask cache는 thickness 1..256의 직접 slot 조회로 tree search와 map node allocation을 줄였다. shared ownership을 유지하며 payload 상한 1 MiB·최대 live mask 32개·기존 coverage와 ink 반올림·eviction 정책은 같다. slot metadata는 x64에서 4,096 bytes이며 이전 map metadata와 동일 크기라고 주장하지 않는다. 이는 thread-local metadata 비용과 조회 비용의 교환이다. 정확한 정수 geometry는 eligibility 단계에서 바로 처리해 수치 helper 호출을 피하며 ULP 계산은 표현 오차가 있는 edge만 수행한다. 정수에도 ULP를 계산한 중간 측정에서는 일부 불투명 조건이 최대 24.59% 증가했고, helper 내부 빠른 반환만 둔 시도는 최대 11.45% 증가해 캡처 전에 중단했다. 정확한 헤더·실행기·6회 원값은 diagnostics/attempt-4/5에 보존했다.

정수 일치만 허용한 첫 시도는 기존 144 DPI capsule boundary 1,024건을 잘못 거부했다. 최종 edge의 한 ULP만 허용한 두 번째 시도에는 반전 clip의 cancellation으로 512건, 화면 밖 edge를 clamp한 세 번째 시도에는 90도 clip의 cancellation으로 128건의 잘못된 거부가 남았다. 각 edge 기여 항의 크기에 따른 오차 범위를 적용해 기존 16,384건 결과를 복구했다. 앞선 헤더·실행기·성능 원값·실패 로그는 diagnostics/attempt-1/2/3에 보존한다. glyph cold/changing 옵션이 전달되지 않은 최초 호출 원값도 별도로 보존하고 올바른 옵션으로 다시 측정했다. 최종 측정은 focused 빌드가 끝난 뒤 수행해 빌드와의 CPU 경쟁을 피했다.

v38 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp 두 파일만 변경했고 다른 455개는 같다. 정확한 변경 전 헤더·변경 후 측정 헤더·실행기 SHA-256·원값을 보존한다. 실제 graphics identity를 확인하고 frozen v38 실행기와 수정 실행기로 고정 48쌍씩 현재 환경에서 재현한다. 기존 fingerprint guard를 유지한다.

실제 화면은 이전 reference의 원격 세션 2560×1440에서 로컬 세션 1920×1080으로 바뀌었으며 최초 환경 검사가 이를 감지해 캡처 전에 중단했다. GPU LUID·driver version·backend·window DPI·scale/rotation·색·feature/compositor 상태는 같다. display bounds와 session flag가 같다고 기록하지 않는다. 모든 실제 비교 fingerprint guard는 유지하며 frozen v38 renderer의 고정 48쌍 네 이미지·전체 JSON·raw·판정이 새 환경에서도 같다는 필수 control을 통과한 뒤 수정 renderer 캡처와 앱 측정을 수행한다. 변경 전후 identity와 검증 원값을 보존한다.

## 6회 직렬 교대 성능

3 before-after·3 after-before 순서, 병렬 측정 없음. 캡슐 pixel hash 168쌍·glyph warm 120쌍/cold 48쌍·앱 BGRA 48쌍/layout 24쌍 일치. warm capsule과 glyph의 측정 구간 C++ new 0. 64-key capsule eviction은 실제 할당 수를 기록한다. WIC 반복 호출 결과이며 앱 전체 개선율을 뜻하지 않는다.

캡슐: WIC 96×96·가로/세로 길이 80·thickness 2/6/10/14·opaque/half/매 호출 RGB+opacity 교대, 40,000회. 32-key warm 조회 40,000회·64-key eviction 2,048회도 포함한다. 각 프로필 시작 backdrop은 RGBA .2/.4/.6/.5이며 매 호출 초기화하지 않는다.

| 프로필 | 방향 | 두께 | before ms | after ms | 변화 | 빠른 회차 | C++ new |
|---|---|---:|---:|---:|---:|---:|---:|
| fixed opaque | 가로 | 2 | 4.756 | 4.720 | -0.74% | 3/6 | 0→0 |
| fixed half opacity | 가로 | 2 | 19.263 | 18.928 | -1.74% | 4/6 | 0→0 |
| alternating RGB and opacity | 가로 | 2 | 21.431 | 20.908 | -2.44% | 5/6 | 0→0 |
| fixed opaque | 가로 | 6 | 9.510 | 10.050 | +5.68% | 1/6 | 0→0 |
| fixed half opacity | 가로 | 6 | 55.532 | 53.154 | -4.28% | 4/6 | 0→0 |
| alternating RGB and opacity | 가로 | 6 | 60.608 | 60.030 | -0.95% | 3/6 | 0→0 |
| fixed opaque | 가로 | 10 | 15.604 | 16.045 | +2.83% | 2/6 | 0→0 |
| fixed half opacity | 가로 | 10 | 87.092 | 90.020 | +3.36% | 2/6 | 0→0 |
| alternating RGB and opacity | 가로 | 10 | 109.866 | 110.107 | +0.22% | 3/6 | 0→0 |
| fixed opaque | 가로 | 14 | 22.919 | 23.548 | +2.74% | 2/6 | 0→0 |
| fixed half opacity | 가로 | 14 | 121.798 | 121.290 | -0.42% | 3/6 | 0→0 |
| alternating RGB and opacity | 가로 | 14 | 162.192 | 161.443 | -0.46% | 2/6 | 0→0 |
| 32 warm thickness keys | 가로 | 6 | 147.132 | 149.398 | +1.54% | 1/6 | 0→0 |
| 64 thickness keys with eviction | 가로 | 6 | 37.191 | 36.558 | -1.70% | 4/6 | 8064→6048 |
| fixed opaque | 세로 | 2 | 9.231 | 9.056 | -1.89% | 5/6 | 0→0 |
| fixed half opacity | 세로 | 2 | 25.037 | 24.492 | -2.18% | 4/6 | 0→0 |
| alternating RGB and opacity | 세로 | 2 | 28.447 | 26.189 | -7.94% | 6/6 | 0→0 |
| fixed opaque | 세로 | 6 | 12.796 | 12.969 | +1.35% | 2/6 | 0→0 |
| fixed half opacity | 세로 | 6 | 61.504 | 64.148 | +4.30% | 2/6 | 0→0 |
| alternating RGB and opacity | 세로 | 6 | 67.924 | 66.687 | -1.82% | 5/6 | 0→0 |
| fixed opaque | 세로 | 10 | 19.348 | 19.685 | +1.75% | 1/6 | 0→0 |
| fixed half opacity | 세로 | 10 | 91.943 | 91.972 | +0.03% | 4/6 | 0→0 |
| alternating RGB and opacity | 세로 | 10 | 112.058 | 114.687 | +2.35% | 2/6 | 0→0 |
| fixed opaque | 세로 | 14 | 28.909 | 26.913 | -6.90% | 5/6 | 0→0 |
| fixed half opacity | 세로 | 14 | 128.442 | 124.228 | -3.28% | 4/6 | 0→0 |
| alternating RGB and opacity | 세로 | 14 | 165.453 | 162.655 | -1.69% | 5/6 | 0→0 |
| 32 warm thickness keys | 세로 | 6 | 157.485 | 159.842 | +1.50% | 2/6 | 0→0 |
| 64 thickness keys with eviction | 세로 | 6 | 35.520 | 35.364 | -0.44% | 3/6 | 8064→6048 |

기존 glyph collateral: .25/.5/1과 .25/.5 교대, glyph 1/4/8/9/32, 20,000회. RGB key 변경·표 생성·재사용 cold 8조건 원값도 glyph-evidence.json에 보존한다.

| opacity | glyph | before ms | after ms | 변화 |
|---|---:|---:|---:|---:|
| 0.25 | 1 | 3.687 | 3.773 | +2.33% |
| 0.25 | 4 | 8.347 | 8.378 | +0.37% |
| 0.25 | 8 | 12.516 | 12.540 | +0.19% |
| 0.25 | 9 | 12.099 | 11.942 | -1.30% |
| 0.25 | 32 | 19.351 | 19.525 | +0.90% |
| 0.5 | 1 | 3.870 | 3.863 | -0.17% |
| 0.5 | 4 | 8.362 | 9.025 | +7.93% |
| 0.5 | 8 | 12.991 | 12.812 | -1.38% |
| 0.5 | 9 | 12.309 | 12.219 | -0.73% |
| 0.5 | 32 | 19.844 | 19.192 | -3.28% |
| 1.0 | 1 | 4.965 | 4.393 | -11.53% |
| 1.0 | 4 | 10.053 | 10.055 | +0.02% |
| 1.0 | 8 | 14.884 | 15.225 | +2.29% |
| 1.0 | 9 | 14.560 | 14.670 | +0.76% |
| 1.0 | 32 | 22.347 | 21.906 | -1.97% |
| 교대 .25/.5 | 1 | 10.068 | 10.077 | +0.09% |
| 교대 .25/.5 | 4 | 26.177 | 26.108 | -0.26% |
| 교대 .25/.5 | 8 | 38.571 | 38.524 | -0.12% |
| 교대 .25/.5 | 9 | 37.555 | 37.377 | -0.47% |
| 교대 .25/.5 | 32 | 44.616 | 45.958 | +3.01% |

앱은 보존한 동일 일반 Markdown·250개 표 자산으로 측정한다.

| 문서 | 구간 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 209.058 | 209.754 | +0.33% | 3/6 |
| normal | firstPaintMs | 107.626 | 107.130 | -0.46% | 4/6 |
| normal | interactiveDownMs | 14.613 | 15.899 | +8.79% | 0/6 |
| normal | interactiveDragMs | 16.043 | 16.420 | +2.35% | 2/6 |
| normal | scrollPaintMs | 20.415 | 20.411 | -0.02% | 2/6 |
| tables | initialLayoutMs | 460.913 | 471.091 | +2.21% | 1/6 |
| tables | firstPaintMs | 91.877 | 93.614 | +1.89% | 1/6 |
| tables | interactiveDownMs | 26.211 | 26.648 | +1.67% | 1/6 |
| tables | interactiveDragMs | 28.377 | 29.048 | +2.36% | 2/6 |
| tables | scrollPaintMs | 31.672 | 32.229 | +1.76% | 1/6 |

## 마지막 회귀·반디집 보관·복원·정리

전체 회귀 12개·platform integrity 통과 뒤 반디집 ZIP fast level 1로 본 보관본 207,470,486 bytes와 복원 증거 41,124,350 bytes를 만들었다. 모든 entry SHA-256, 새 폴더 소스 457개·비교기 120쌍·내부 증거를 확인했다. 복원 실행기로 전체 828쌍을 새로 렌더링했고 네 이미지·전체 진단·raw·판정이 원래 실행과 같다. 새 정밀 검사와 기존 일곱 native 명령의 로그 SHA-256도 같다. 최종 완료 문서와 정리 기록은 별도 반디집 final-cleanup-records.zip에 보관한다.

복원 검증 후 해시 일치 사본 77,967개·컴파일 생성물 295개·캐시 39,125개 정리. 소스 457개·최신 828쌍·실패 diff·보호 입력·최소 증거·반디집 보관본 유지. 새 보호 입력 손실 0, 검사 대상 생성물/캐시 0. 과거 두 파일 누락은 기존 기록대로 유지한다.

원값: C:/twf-v39/runs/20261006-capsule-precision-v39-final, C:/twf-v39/archives. 작업 코드·변경 전 실패·성능 원값·소스는 내부 optimization-and-accuracy-evidence/work-evidence.zip에 보존한다. native 명령: --capsule-precision, --glyph-axis-scale, --glyph-narrow-clip, --glyph-opacity, --capsule-axes, --capsule-boundaries, --glyph-clip-transforms, --horizontal-native.

전체 HTML/CSS/DOM/paint 계약·일반 affine·fractional group/layer·실제 Windows 144 DPI/두 모니터·1,000문서는 미완료다. MdViewer 자동 재빌드 없음.

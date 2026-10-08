# v37 native 글리프 반전·반투명 합성 조회 개선 결과

2026-10-06 완료. Windows x64 Release. 전체 회귀 12개·platform integrity·반디집 보관·새 폴더 복원·828쌍 새 렌더링·native 로그 검증 완료.

- 독립 DirectWrite 픽셀 검사 221,184건: v36 CPU 경로 미지원·픽셀 비교 실패 152,064 → v37 0. 이전 fallback 장면 전체의 오렌더링 건수를 뜻하지 않는다.
- 기존 양의 비균일 배율 69,120건과 좌우·상하·양축 반전 152,064건, 무쓰기 fallback·정상 복귀 guard 26건 통과.
- 좁은/빈 clip 61,440건·opacity 32,256건·입력 guard 14건·face/factory 전환 8건, capsule 방향 24,576건·경계 16,384건·guarded fallback 4,096+3,328건, glyph 192+3,072건·일반 텍스트 23,068,672조합·clip/layer/DPI·scroll/drag/style 복귀 유지.
- 828쌍의 네 decoded RGBA 이미지·전체 native/reference JSON·raw·판정이 v36와 일치했다. strict 727·기존 backend 승인 75, 총 802/828 성공. 기존 미등록 화살표 FAIL_PAINT 26은 최대 채널 차이 1·raw 차이 합계 182로 계속 실패한다. 비교기 기본 182개와 오류 주입 336/336 통과. 입력·허용치 0·registry 111개 유지. 새 예외 0.
- 고정 반투명 warm 10조건은 16.03~33.00% 빨라졌다. 전체 warm 조건의 최대 증가는 14.09%(opacity -1.0, glyph 4)이고, 앱 단계의 최대 증가는 5.11%다. 아래 원값과 모든 증가 조건을 함께 기록한다.

## 구현과 독립 검증

display ID·refresh rate(60→32)와 선택적 DRIVER_VENDOR 메타데이터가 v36 때와 달라 첫 고정 캡처의 fingerprint guard가 중단했다. GPU·driver version·backend·DPI·색·bounds는 같음을 확인했고, 원래 guard를 유지한 채 v36 frozen renderer를 현재 환경에서 다시 캡처해 고정 48쌍의 네 이미지·전체 JSON·raw·판정 불변을 확인했다. 실패한 첫 캡처도 focused-attempt-1에 보존했다.

native grayscale 글리프의 signed 대각 변환을 지원한다. font em에는 세로 배율의 절댓값을 적용하고 analysis matrix에 signed 가로 비율과 세로 방향을 적용한다. 기존 양의 균일 변환은 원래 matrix를 유지한다. cache key의 em 부호는 세로 반전을 구분하고 가로 비율 부호는 좌우 반전을 구분한다. 실제 DirectWrite em은 항상 양수다. cache는 16 MiB·4,096 entry 제한, immutable face 소유·run shared ownership·마지막 native key 조회를 유지한다. [Microsoft CreateGlyphRunAnalysis 문서](https://learn.microsoft.com/en-us/windows/win32/api/dwrite_2/nf-dwrite_2-idwritefactory2-createglyphrunanalysis)의 em 이후 transform 적용 순서를 사용했다.

32개 변환은 기존 10개와 비균일 6종의 좌우/상하/양축 반전 18개, 반전한 비율 경계 .125/8 두 개와 작은 음수 축×큰 em의 경계 두 개다. 이를 saved clip 변환 3개·brush alpha 2개·opacity 3개·RGB 2개·backdrop alpha 4개·clip 3개·AA 2개·run 길이 1/8/9/32·DPI 배율 1/1.5와 조합했다. 반전 전후와 마지막 양의 .5 배율 복귀를 검사한다. 각 glyph의 mask·phase는 별도 DirectWrite 호출로 분석하며 기대값은 독립 double gamma/source-over와 기하학 clip 교집합으로 계산한다. aliased clip은 별도 Direct2D oracle을 사용한다. 엔진 mask/clip cache·blend lookup을 기대값 계산에 쓰지 않는다. 모든 PBGRA byte와 premultiplied 범위를 검사한다.

13 guard/DPI는 skew 2·90도 회전·절댓값 비율 밖인 반전 2·0축 2·NaN·무한 translation·양의 비율 밖 2·em overflow·일반 텍스트 비균일 변환이다. 지원된 음수 축을 guard에서 제외하고 범위 밖 반전으로 대체했다. 모두 무쓰기 fallback과 정상 후속 paint를 확인한다. 일반 텍스트의 음수/비균일 변환, 일반 회전/기울임과 layer는 기존 fallback을 유지한다.

반투명 mask 전체가 saved clip 안에 있고 합성 table이 준비되면 별도 루프로 paint해 per-pixel clip 기하 계산과 분기를 줄인다. 기존 gamma/합성 table의 입력과 row 배치는 유지한다. fractional clip과 키가 자주 바뀌는 경우는 기존 계산을 유지한다. 첫 시도의 raw coverage 결합 방식은 여러 warm 조건을 5~11% 늦춰 최종 구현에서 제외했다. 시도 1의 정확한 헤더·실행기·207,360건 검사·6회 원값은 diagnostics/attempt-1에 보존했다. 작은 음수 축의 부호 판정도 별도 보강했다. table은 thread당 한 RGB/opacity 조합·256 KiB이고 반복 visible paint 8번째에 생성한다. 정확한 opacity와 source-over 마지막 byte 반올림을 유지하며 추가 heap 할당은 없다.

v36의 소스 457개와 비교해 RasterSurface.h와 ScrollRenderingRegression.cpp 두 파일만 바뀌었고 다른 455개는 같다. 측정 헤더와 최종 소스 SHA-256 일치를 확인한다.

## 직렬 교대 성능

6회 직렬 교대(3 before-after, 3 after-before)의 중앙값·각 회차 원값·paired 변화·빠른 회차를 보존한다. warm 20조건의 pixel hash 120쌍과 cold 8조건 48쌍이 같고 양쪽 측정 구간 C++ new 할당 0이다. 이 결과는 WIC 64×32 microbenchmark이며 앱 전체 개선율을 뜻하지 않는다. 증가값도 그대로 기록한다.

warm: 같은 #8b8b8b glyph 1/4/8/9/32, 고정 opacity .25/.5/1 및 매 호출 .25/.5 교대, 10회 준비 후 20,000회 반복. backdrop #fcfcfc는 처음 한 번만 지운다.

| opacity | glyph | before ms | after ms | 변화 | 빠른 회차 |
|---|---:|---:|---:|---:|---:|
| 0.25 | 1 | 5.016 | 4.108 | -18.10% | 5/6 |
| 0.25 | 4 | 13.318 | 9.301 | -30.16% | 6/6 |
| 0.25 | 8 | 19.877 | 14.066 | -29.24% | 6/6 |
| 0.25 | 9 | 19.638 | 13.571 | -30.90% | 6/6 |
| 0.25 | 32 | 27.768 | 20.833 | -24.98% | 6/6 |
| 0.5 | 1 | 4.936 | 4.144 | -16.03% | 5/6 |
| 0.5 | 4 | 13.410 | 9.963 | -25.70% | 5/6 |
| 0.5 | 8 | 20.022 | 13.937 | -30.39% | 6/6 |
| 0.5 | 9 | 20.607 | 13.808 | -33.00% | 6/6 |
| 0.5 | 32 | 27.140 | 21.093 | -22.28% | 6/6 |
| 1.0 | 1 | 4.596 | 4.357 | -5.22% | 3/6 |
| 1.0 | 4 | 10.527 | 10.156 | -3.53% | 3/6 |
| 1.0 | 8 | 15.690 | 15.527 | -1.04% | 4/6 |
| 1.0 | 9 | 14.620 | 15.742 | +7.68% | 1/6 |
| 1.0 | 32 | 21.888 | 22.147 | +1.19% | 1/6 |
| 교대 .25/.5 | 1 | 10.061 | 10.025 | -0.36% | 2/6 |
| 교대 .25/.5 | 4 | 25.788 | 29.421 | +14.09% | 0/6 |
| 교대 .25/.5 | 8 | 38.047 | 39.578 | +4.02% | 2/6 |
| 교대 .25/.5 | 9 | 38.417 | 38.698 | +0.73% | 1/6 |
| 교대 .25/.5 | 32 | 45.170 | 46.574 | +3.11% | 2/6 |

cold: font/mask는 준비한 상태에서 RGB 64키를 교대하고 키마다 10회, 총 640회. 첫 호출·8번째 table 생성·9/10번째 재사용·전체를 구분한다. 회색 #8b8b8b/#8c8c8c·컬러 #2763bd/#2864be, backdrop RGBA .2/.4/.6/.5를 사용한다. 전체 process/font 최초 시작 시간이 아니다.

| RGB | opacity | glyph | 구간 | before ms | after ms | 변화 | 빠른 회차 |
|---|---:|---:|---|---:|---:|---:|---:|
| gray | 0.25 | 1 | 첫 호출 | 0.040 | 0.038 | -5.70% | 6/6 |
| gray | 0.25 | 1 | 8번째 표 생성 | 22.078 | 21.443 | -2.88% | 6/6 |
| gray | 0.25 | 1 | 9/10번째 재사용 | 0.050 | 0.038 | -23.63% | 6/6 |
| gray | 0.25 | 1 | 전체 640회 | 22.385 | 21.737 | -2.90% | 6/6 |
| gray | 0.25 | 9 | 첫 호출 | 0.185 | 0.180 | -2.68% | 5/6 |
| gray | 0.25 | 9 | 8번째 표 생성 | 21.905 | 21.473 | -1.97% | 5/6 |
| gray | 0.25 | 9 | 9/10번째 재사용 | 0.134 | 0.100 | -24.82% | 6/6 |
| gray | 0.25 | 9 | 전체 640회 | 23.299 | 22.834 | -2.00% | 5/6 |
| gray | 0.5 | 1 | 첫 호출 | 0.034 | 0.034 | +2.53% | 3/6 |
| gray | 0.5 | 1 | 8번째 표 생성 | 22.053 | 21.858 | -0.89% | 3/6 |
| gray | 0.5 | 1 | 9/10번째 재사용 | 0.049 | 0.044 | -9.58% | 4/6 |
| gray | 0.5 | 1 | 전체 640회 | 22.316 | 22.112 | -0.91% | 3/6 |
| gray | 0.5 | 9 | 첫 호출 | 0.163 | 0.148 | -9.15% | 4/6 |
| gray | 0.5 | 9 | 8번째 표 생성 | 23.785 | 22.042 | -7.32% | 4/6 |
| gray | 0.5 | 9 | 9/10번째 재사용 | 0.151 | 0.105 | -30.62% | 6/6 |
| gray | 0.5 | 9 | 전체 640회 | 25.043 | 23.140 | -7.60% | 4/6 |
| color | 0.25 | 1 | 첫 호출 | 0.062 | 0.060 | -2.42% | 4/6 |
| color | 0.25 | 1 | 8번째 표 생성 | 44.161 | 43.992 | -0.38% | 3/6 |
| color | 0.25 | 1 | 9/10번째 재사용 | 0.051 | 0.048 | -6.76% | 5/6 |
| color | 0.25 | 1 | 전체 640회 | 44.616 | 44.447 | -0.38% | 3/6 |
| color | 0.25 | 9 | 첫 호출 | 0.334 | 0.317 | -5.00% | 5/6 |
| color | 0.25 | 9 | 8번째 표 생성 | 47.330 | 44.966 | -4.99% | 4/6 |
| color | 0.25 | 9 | 9/10번째 재사용 | 0.163 | 0.119 | -26.57% | 6/6 |
| color | 0.25 | 9 | 전체 640회 | 49.787 | 47.322 | -4.95% | 4/6 |
| color | 0.5 | 1 | 첫 호출 | 0.057 | 0.057 | +0.79% | 2/6 |
| color | 0.5 | 1 | 8번째 표 생성 | 44.249 | 45.419 | +2.65% | 2/6 |
| color | 0.5 | 1 | 9/10번째 재사용 | 0.057 | 0.052 | -9.47% | 4/6 |
| color | 0.5 | 1 | 전체 640회 | 44.682 | 45.848 | +2.61% | 2/6 |
| color | 0.5 | 9 | 첫 호출 | 0.280 | 0.284 | +1.68% | 3/6 |
| color | 0.5 | 9 | 8번째 표 생성 | 44.773 | 44.307 | -1.04% | 2/6 |
| color | 0.5 | 9 | 9/10번째 재사용 | 0.142 | 0.116 | -18.45% | 6/6 |
| color | 0.5 | 9 | 전체 640회 | 47.015 | 46.341 | -1.43% | 3/6 |

앱: 동일 보존 자산의 일반 Markdown·250개 표 문서 6회 직렬 교대, BGRA 48쌍·layout JSON 24쌍 일치. 원값·실행기/source SHA-256을 보존한다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 208.173 | 207.750 | -0.20% | 3/6 |
| normal | firstPaintMs | 109.089 | 109.050 | -0.04% | 3/6 |
| normal | interactiveDownMs | 15.396 | 14.932 | -3.01% | 5/6 |
| normal | interactiveDragMs | 16.485 | 16.187 | -1.81% | 4/6 |
| normal | scrollPaintMs | 20.501 | 20.488 | -0.06% | 2/6 |
| tables | initialLayoutMs | 485.150 | 481.475 | -0.76% | 4/6 |
| tables | firstPaintMs | 94.440 | 92.613 | -1.94% | 5/6 |
| tables | interactiveDownMs | 25.767 | 27.085 | +5.11% | 1/6 |
| tables | interactiveDragMs | 27.932 | 29.315 | +4.95% | 1/6 |
| tables | scrollPaintMs | 31.946 | 32.280 | +1.05% | 2/6 |

## 마지막 회귀·반디집 보관·복원·정리

전체 회귀 12개·platform integrity 통과 후 반디집 ZIP fast level 1로 본 보관본 202,192,283 bytes, 복원 증거 41,147,059 bytes를 만들었다. 보관본 SHA-256과 모든 entry를 확인했다. 새 폴더 소스 457개·비교기 120쌍, 전체 828쌍 새 렌더링의 네 이미지·전체 진단·raw·판정이 원래 실행과 같다. 새 배율/guard 및 기존 여섯 native 명령의 로그 SHA-256도 모두 같다.

복원 검증 후 해시 일치 사본 77,882개·컴파일 생성물 295개·캐시 39,173개를 정리했다. 소스 457개·최신 828쌍과 실패 diff·보호 입력·최소 증거·반디집 보관본 유지. 새 보호 입력 손실 0, 검사 대상 생성물/캐시 0. 과거 두 파일 누락은 기존 보호 기록대로 남긴다.

원값: C:/twf-v37/runs/20261006-glyph-reflect-v37-final, C:/twf-v37/archives. 작업 scripts·변경 전 헤더/미지원·독립 검사·성능 원값은 내부 optimization-and-accuracy-evidence/work-evidence.zip에 보존한다. native 명령: --glyph-axis-scale, --glyph-narrow-clip, --glyph-opacity, --capsule-axes, --capsule-boundaries, --glyph-clip-transforms, --horizontal-native.

전체 HTML/CSS/DOM/paint 계약·일반 affine·fractional group/layer·실제 Windows 144 DPI/두 모니터·1,000문서는 미완료다. MdViewer는 자동 재빌드하지 않았다.

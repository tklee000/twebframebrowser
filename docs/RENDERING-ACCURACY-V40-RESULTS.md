# v40 공유 clip cache 검증·hot path 결과

2026-10-06. Windows x64 Release. 전체 회귀·반디집 보관·새 복원·828쌍 새 렌더링·native 로그 검증 완료. 이전 보호 보관본 실물 확인은 별도 미완료.

- 독립 clip cache 1,152구성·좌표 질의 17,548건·guard/복귀 164건, 변경 전후 실패 0. 기존에 통과하던 계약을 유지하며 state transition 검사 범위를 늘렸다.
- 기존 capsule precision 6,912건·guard 112건, glyph 배율 248,832건·guard 34건 유지.
- narrow 61,440건·opacity 32,256건·guard 14건·face/factory 8건·capsule 방향 24,576건·경계 16,384건·glyph clip 3,072건·기본 192건·일반 텍스트 23,068,672조합 및 clip/layer/DPI·scroll/drag/style 복귀 유지.
- 전체 828쌍의 네 이미지·전체 JSON·raw·판정이 v39와 같다. strict 727·기존 backend 승인 75, 총 802/828. 기존 화살표 FAIL_PAINT 26 유지. 비교기 오류 주입 182+336건 통과. 허용치 0·registry 111개·새 예외 0.
- clip 측정 24조건 시간 중앙값 변화 -45.15~+1.44%. 음수는 시간 감소다. 아래에 증가 조건도 기록한다.

## 구현과 독립 검증

DeviceGlyphClip은 저장된 finite positive DPI·크기 key가 일치하면 바로 반환한다. NaN은 어떤 key와도 같지 않고 ±infinity·0·음수는 게시된 유효 key와 같을 수 없으므로 DPI 유효성 검사는 miss/upgrade에 남긴다. 잘못된 입력 거부와 출력 sentinel·이전 cache 내용 보존을 실제 채워진 cache에서 확인했다. clip rect/matrix 입력 10개의 유한성 검사 및 변환된 bounds의 유한성 검사는 유지한다.

정수 AA edge 판정의 기여 항 magnitude는 requireBinary=true이고 AA clip일 때만 계산한다. ordinary glyph와 aliased clip은 이 값을 사용하지 않아 해당 계산을 건너뛴다. geometry·AA threshold·반올림·clip 교집합·pixel-center aliased rounding·cache key/binary upgrade·push/pop invalidation 정책은 같다. 새 cache entry나 metadata·payload를 추가하지 않는다. 기존 capsule mask는 최대 32개·payload 1 MiB·256 slot을 유지한다.

독립 기대값은 double 정밀도로 네 모서리를 직접 변환하고 clip 교집합과 pixel-center alias 범위를 계산한다. 8개 반전/회전/기울임 행렬·6개 정수/소수/화면 밖/zero-width/빈 clip·2개 AA·2개 크기·3개 비대칭 DPI 조합을 96/144 scale에서 검사한다. dyadic 입력으로 기대값과 float 변환의 표현 오차를 배제한다. 각 구성에서 miss→hit→binary upgrade→binary hit→ordinary reuse, size/DPI key 변경·복귀, 실제 PushPaintClip/PopPaintClip nested invalidation을 검사한다. 유효 cache 뒤 DPI 0/음수/NaN/±infinity 20건, descriptor 10개 필드의 NaN/±infinity 60건, 유한 descriptor의 계산 overflow 2건을 각 scale에서 검사한다. 잘못된 descriptor는 Direct2D에 전달하지 않는다. before/after 모두 같은 검사를 통과한다.

v39 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp만 변경했고 다른 455개 해시는 같다. 정확한 before/after 헤더·실행기·원값을 보존한다. 현재 graphics identity를 확인하고 frozen v39 renderer의 고정 48쌍과 수정 renderer의 고정 48쌍을 원래 출력과 대조했다. 환경 비교 guard는 유지한다.

## 6회 직렬 교대 성능

3 before-after·3 after-before 순서. clip 결과 hash 144쌍·capsule pixel hash 168쌍·glyph warm 120쌍/cold 48쌍·앱 BGRA 48쌍/layout 24쌍이 일치한다. clip 측정 구간 C++ new 0. capsule/glyph warm allocation 0. 64-key eviction allocation·각 회차 원값·paired change는 JSON에 남겼다.

clip은 warm hit 1,000,000회, 나머지 100,000회·depth 1/8·binary false/true로 측정한다. non-inline 호출과 매번 output/hash 사용으로 삭제를 막으며 hash 비용도 측정 시간에 포함된다. upgrade는 ordinary 조회 후 binary 조회 두 호출, DPI/size 조건은 key를 번갈아 바꾼다. helper 측정 개선율을 앱 전체 개선율로 해석하지 않는다.

| 조건 | binary | depth | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|---:|
| warm cache hit | False | 1 | 7.518 | 4.178 | -44.43% | 6/6 |
| warm cache hit | False | 8 | 7.441 | 4.121 | -44.62% | 6/6 |
| warm cache hit | True | 1 | 7.554 | 4.143 | -45.15% | 6/6 |
| warm cache hit | True | 8 | 7.574 | 4.191 | -44.67% | 6/6 |
| AA miss / ordinary-to-binary upgrade | False | 1 | 3.139 | 3.033 | -3.39% | 6/6 |
| AA miss / ordinary-to-binary upgrade | False | 8 | 20.355 | 19.220 | -5.58% | 6/6 |
| AA miss / ordinary-to-binary upgrade | True | 1 | 8.885 | 8.171 | -8.04% | 6/6 |
| AA miss / ordinary-to-binary upgrade | True | 8 | 59.271 | 56.789 | -4.19% | 6/6 |
| aliased miss | False | 1 | 3.594 | 3.537 | -1.59% | 5/6 |
| aliased miss | False | 8 | 23.594 | 23.933 | +1.44% | 3/6 |
| aliased miss | True | 1 | 3.617 | 3.526 | -2.53% | 4/6 |
| aliased miss | True | 8 | 23.575 | 23.169 | -1.72% | 5/6 |
| skewed AA miss | False | 1 | 3.615 | 3.123 | -13.63% | 6/6 |
| skewed AA miss | False | 8 | 23.654 | 19.681 | -16.79% | 6/6 |
| skewed AA miss | True | 1 | 4.322 | 4.246 | -1.76% | 4/6 |
| skewed AA miss | True | 8 | 31.550 | 31.623 | +0.23% | 2/6 |
| axis-swapped AA miss | False | 1 | 3.619 | 3.175 | -12.28% | 6/6 |
| axis-swapped AA miss | False | 8 | 24.701 | 20.141 | -18.46% | 6/6 |
| axis-swapped AA miss | True | 1 | 6.185 | 5.850 | -5.41% | 5/6 |
| axis-swapped AA miss | True | 8 | 43.202 | 41.135 | -4.78% | 5/6 |
| DPI and size transition | False | 1 | 3.233 | 3.116 | -3.62% | 3/6 |
| DPI and size transition | False | 8 | 20.811 | 19.383 | -6.86% | 6/6 |
| DPI and size transition | True | 1 | 5.608 | 5.352 | -4.57% | 6/6 |
| DPI and size transition | True | 8 | 34.994 | 34.176 | -2.34% | 5/6 |

기존 capsule: 28조건·opaque/half/교대 color+opacity·가로/세로 T2/6/10/14 40,000회·32-key warm 40,000회·64-key eviction 2,048회.

| 조건 | 방향 | 두께 | before ms | after ms | 변화 |
|---|---|---:|---:|---:|---:|
| fixed opaque | 가로 | 2 | 4.989 | 4.703 | -5.73% |
| fixed half opacity | 가로 | 2 | 19.956 | 19.366 | -2.95% |
| alternating RGB and opacity | 가로 | 2 | 20.698 | 20.897 | +0.96% |
| fixed opaque | 가로 | 6 | 9.511 | 9.605 | +0.98% |
| fixed half opacity | 가로 | 6 | 52.773 | 53.214 | +0.84% |
| alternating RGB and opacity | 가로 | 6 | 62.251 | 60.222 | -3.26% |
| fixed opaque | 가로 | 10 | 15.200 | 15.256 | +0.36% |
| fixed half opacity | 가로 | 10 | 88.330 | 86.982 | -1.53% |
| alternating RGB and opacity | 가로 | 10 | 108.416 | 106.740 | -1.55% |
| fixed opaque | 가로 | 14 | 23.437 | 21.264 | -9.27% |
| fixed half opacity | 가로 | 14 | 126.667 | 122.975 | -2.91% |
| alternating RGB and opacity | 가로 | 14 | 160.799 | 159.052 | -1.09% |
| 32 warm thickness keys | 가로 | 6 | 157.653 | 145.021 | -8.01% |
| 64 thickness keys with eviction | 가로 | 6 | 36.610 | 34.809 | -4.92% |
| fixed opaque | 세로 | 2 | 9.404 | 9.162 | -2.58% |
| fixed half opacity | 세로 | 2 | 26.412 | 23.851 | -9.70% |
| alternating RGB and opacity | 세로 | 2 | 27.584 | 26.171 | -5.12% |
| fixed opaque | 세로 | 6 | 12.615 | 12.383 | -1.84% |
| fixed half opacity | 세로 | 6 | 64.538 | 57.622 | -10.72% |
| alternating RGB and opacity | 세로 | 6 | 69.303 | 66.457 | -4.11% |
| fixed opaque | 세로 | 10 | 20.069 | 20.150 | +0.41% |
| fixed half opacity | 세로 | 10 | 100.508 | 94.305 | -6.17% |
| alternating RGB and opacity | 세로 | 10 | 111.332 | 109.894 | -1.29% |
| fixed opaque | 세로 | 14 | 27.507 | 28.646 | +4.14% |
| fixed half opacity | 세로 | 14 | 130.541 | 124.580 | -4.57% |
| alternating RGB and opacity | 세로 | 14 | 166.351 | 160.951 | -3.25% |
| 32 warm thickness keys | 세로 | 6 | 154.876 | 153.526 | -0.87% |
| 64 thickness keys with eviction | 세로 | 6 | 35.033 | 34.827 | -0.59% |

기존 glyph .25/.5/1·교대 .25/.5 및 1/4/8/9/32개 20,000회와 cold table 8조건은 glyph-evidence.json에 보존한다.

앱은 이전 보관본에서 복원한 동일 일반 Markdown·250개 표 자산으로 측정했다. 증가한 구간도 함께 기록하며 전 구간 속도 향상을 주장하지 않는다.

| 문서 | 구간 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 210.448 | 205.478 | -2.36% | 3/6 |
| normal | firstPaintMs | 103.886 | 103.570 | -0.31% | 4/6 |
| normal | interactiveDownMs | 15.229 | 15.174 | -0.36% | 3/6 |
| normal | interactiveDragMs | 17.080 | 16.313 | -4.49% | 4/6 |
| normal | scrollPaintMs | 20.957 | 20.865 | -0.44% | 3/6 |
| tables | initialLayoutMs | 479.715 | 472.686 | -1.47% | 4/6 |
| tables | firstPaintMs | 90.295 | 91.542 | +1.38% | 2/6 |
| tables | interactiveDownMs | 26.839 | 25.965 | -3.26% | 4/6 |
| tables | interactiveDragMs | 28.841 | 28.512 | -1.14% | 3/6 |
| tables | scrollPaintMs | 32.051 | 31.187 | -2.70% | 5/6 |

## 마지막 회귀·반디집 보관·복원·정리

전체 회귀 12개·platform integrity 통과 뒤 반디집 ZIP fast level 1로 본 보관본 217,154,075 bytes와 복원 증거 44,567,913 bytes를 만들었다. 모든 entry SHA-256·새 폴더 소스 457개·비교기 120쌍·내부 증거를 확인했다. 현재 live 보호 입력 2,799개도 내부 보관본에 추가하고 새 폴더로 실제 복원해 각 해시를 검증했다. 복원 실행기로 전체 828쌍을 새로 렌더링해 네 이미지·전체 진단·raw·판정 일치를 검증했다. 새 --clip-cache와 기존 여덟 native 명령의 로그 SHA-256도 같다. 최종 문서·정리 기록은 별도 반디집 final-cleanup-records.zip에 보관한다.

복원 증거 포장 중 보호 입력의 긴 경로를 PowerShell HardLink staging이 처리하지 못해 중단했다. work 전용 포장 helper의 staging을 .NET File.Copy로 바꿔 같은 파일·entry·SHA-256을 유지하며 재개했다. 동결된 소스 457개·본 보관본·완료된 검사는 바꾸지 않았고 전체 회귀·캡처·native 검사를 다시 실행하지 않았다. 수정 helper와 중단·재개 기록은 복원 증거 ZIP의 pipeline-recovery-v40 아래에 보존한다.

해시 일치 사본 80,537개·컴파일 생성물 295개·캐시 39,121개 정리. 소스 457개·최신 828쌍·실패 diff·현재 보호 입력·최소 증거·반디집 보관본 유지. 작업 중 새 live 보호 입력 손실 0, 검사 대상 생성물/캐시 0.

v39 보관본 SHA-256 및 그 안의 보호 입력 기록을 검증했고 현재 live 보호 입력 2,799개는 모두 원래 해시와 같다. 그러나 작업 시작 전 C:/twf-v24/cleanup-archives/artifact-history.7z와 관련 작업 폴더가 이미 없어, 그 안에 있던 4,544개 입력의 현재 가용성·실물 해시는 확인하지 못했다. 사용자에게 이동 위치를 요청했다. 과거 기록을 근거로 현재 보관본이 있다고 판정하지 않는다. 과거 security-profile HTML 두 파일 누락도 유지한다. 이번 작업 중 live 입력 손실은 0이며 예전 보호 입력의 완전한 보관 검증은 미완료다.

원값: C:/twf-v40/runs/20261006-clip-cache-v40-final, C:/twf-v40/archives. 작업 코드·before/after 소스·성능 원값은 내부 optimization-and-accuracy-evidence/work-evidence.zip에 보존한다. native 명령은 --clip-cache 및 기존 --capsule-precision, --glyph-axis-scale, --glyph-narrow-clip, --glyph-opacity, --capsule-axes, --capsule-boundaries, --glyph-clip-transforms, --horizontal-native다.

전체 HTML/CSS/DOM/paint 계약·일반 affine·fractional group/layer·실제 Windows 144 DPI/두 모니터·1,000문서는 미완료다. MdViewer 자동 재빌드 없음.

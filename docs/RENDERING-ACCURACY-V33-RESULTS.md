# v33 native 캡슐 방향·반전·중앙 span 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 새 렌더링과 native 방향·경계·상태 복원 검증 완료.

- 캡슐 자체 방향·변환 경계 24,576건: native 지원 반환·픽셀·상태 probe의 v32 실패 19,200 → v33 실패 0. 20,480건은 CPU native 지원·무변경 처리, 4,096건은 fractional AA clip의 fallback 검증이다. 엔진 전체 화면 비교는 아래 828쌍 수량을 따른다.
- 세로/가로·8개 signed axis permutation·두 배율에서 independent double segment-distance 기대값과 모든 PBGRA byte를 비교했다. 일반 회전·skew·비균일/퇴화 transform·소수/뒤집힌/빈/과대 geometry와 layer fallback 18건도 확인했다.
- 기존 캡슐 16,384건·glyph 192+3,072건·일반 텍스트 23,068,672조합·clip/layer/DPI 상태 복원·스크롤/드래그/스타일 복귀도 통과했다.
- 기존 828쌍의 네 decoded RGBA 이미지·전체 native/reference JSON·raw 수치·판정이 v32와 일치했다. 총 802/828 성공(strict 727·기존 backend 승인 75), 미등록 FAIL_PAINT 26 유지.
- 비교기 기본 182개·스크롤바 오류 주입 336/336 통과. 원본 입력·허용치 0·기존 예외 registry 111개 유지, 새 예외 0.

## 정확성 수정

`RasterSurface::FillNativeCapsule`이 유한 device-space 축 변환의 부호와 축 교환을 검사하고 변환된 두 모서리의 min/max로 device rect를 만든다. 가로·세로 중 짧은 축을 두께로 쓰며, 같은 원형 cap 마스크를 방향에 맞게 조회한다. 반전·정확한 직교 축 교환은 원형 cap를 보존한다. 일반 회전/skew·비균일 transform은 기존 general renderer를 사용한다. 원래 rect의 순서를 검증하므로 잘못된 뒤집힌/빈 입력이 min/max 처리로 임의의 정상 도형이 되지 않는다. fractional AA clip·layer·소수 device geometry·256px 초과 두께도 기존 fallback이다.

독립 검사는 모든 authored corner를 double로 변환하고, 원형 cap를 중심 선분까지의 거리로 계산한다. 엔진의 cap index·cache·방향 분기 함수를 기대값에 쓰지 않는다. 별도 Direct2D fill로 alias/정수 clip의 포함을 구하고, premultiplied ink 양자화와 UNORM source-over를 계산한다. 방향 2개×transform 8개×두께 4개×색 2개×brush alpha/opacity 4개×backdrop alpha 4개×clip 3개×AA 2개×배율 2개, 총 24,576건이다. 각 페인트 뒤 target transform·전체 BGRA·premultiplied 채널 범위·클립 밖 보존과 fallback 무변경을 검사했다. 이전 헤더로 같은 검사를 실행한 실패도 보존했다.

기존 화살표 미등록 FAIL_PAINT 26쌍의 최대 채널 차이 1과 raw 차이 합계 182픽셀은 남아 있다. 캡슐 방향 개선을 화살표 차이 해결로 주장하지 않는다.

## 반복 비용과 실제 앱 성능

가로 캡슐의 각 행을 왼쪽 cap·중앙 span·오른쪽 cap으로 나눠 중앙에서 cap index와 alpha 분기를 반복하지 않는다. 세로 캡슐도 중앙 행은 같은 span 처리를 사용한다. 불투명 중앙은 같은 4byte ink를 복사하며 WIC 버퍼의 정렬을 가정하지 않는다. 반투명 중앙과 cap는 기존 byte source-over를 유지한다. 캐시 key는 orientation이 아닌 두께이며 RGB·effective opacity가 바뀌면 ink를 다시 계산한다. 위치·길이·clip·backdrop·target 변환은 live 입력이다. 32개/1 MiB 캐시 한도를 유지한다.

기존에 지원하던 가로 캡슐의 warm paint 20,000회를 6회 직렬 교대(3회 before-after, 3회 after-before) 측정했다. 미지원이던 세로/반전 경로를 before 속도 비교로 사용하지 않는다. 12개 조건의 시간 변화율은 -57.75~-2.68%이며 최종 픽셀 해시 72쌍과 임시 heap 할당 0회를 유지했다. 감소/증가 원값·paired 변화·실행기와 최종 소스 SHA-256을 보존했다. 아래 값은 작은 WIC 표면의 해당 페인트 비용이다.

| clip 깊이 | 길이 px | 두께 px | before ms | after ms | 변화 | 빠른 회차 |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 16 | 5 | 3.463 | 2.760 | -20.28% | 6/6 |
| 0 | 16 | 13 | 6.857 | 6.673 | -2.68% | 4/6 |
| 0 | 96 | 5 | 10.112 | 4.396 | -56.52% | 6/6 |
| 0 | 96 | 13 | 25.211 | 10.884 | -56.83% | 6/6 |
| 2 | 16 | 5 | 3.467 | 2.883 | -16.85% | 6/6 |
| 2 | 16 | 13 | 7.046 | 6.814 | -3.29% | 5/6 |
| 2 | 96 | 5 | 10.639 | 4.696 | -55.86% | 6/6 |
| 2 | 96 | 13 | 25.893 | 10.939 | -57.75% | 6/6 |
| 8 | 16 | 5 | 3.534 | 2.845 | -19.50% | 6/6 |
| 8 | 16 | 13 | 7.228 | 6.939 | -4.00% | 4/6 |
| 8 | 96 | 5 | 10.509 | 4.667 | -55.59% | 6/6 |
| 8 | 96 | 13 | 25.786 | 11.190 | -56.60% | 6/6 |

같은 보존 앱 자산의 일반 Markdown·250개 표 문서를 각각 6회 직렬 교대 측정했다. BGRA 48쌍·layout JSON 24쌍이 같다. 작은 capsule probe 수치를 앱 전체 개선율로 해석하지 않으며 증가한 단계도 그대로 기록한다. parser와 문서 입력은 바꾸지 않았다.

최초 span 구현에서 길이 16px·두께 13px의 중앙부가 거의 없는 조건은 최대 +4.72% 비용 증가를 관측했다. 단일 루프를 유지하고 축 변환의 0 항 곱셈을 피한 두 번째 시도는 오히려 최대 +25.79%였다. compiler symbol에서 per-pixel 합성 lambda가 별도 함수로 남는 것을 확인해 작은 합성 함수를 강제 인라인했다. 그 뒤 최종 소스로 정확성·고정 캡처·성능을 다시 측정했다. 최초 원값·실행기·헤더·고정 캡처·앱 측정은 attempt-1에, 두 번째 시도의 원값·헤더·실행기·symbol은 diagnostics/attempt-2에 보존한다. 앱 최초 drag +6.06% 등 증가도 보존하며 최적화가 모든 앱 측정 변동의 원인이라고 단정하지 않는다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 205.702 | 205.224 | -0.23% | 3/6 |
| normal | firstPaintMs | 105.507 | 112.563 | +6.69% | 3/6 |
| normal | interactiveDownMs | 15.763 | 14.891 | -5.53% | 4/6 |
| normal | interactiveDragMs | 16.590 | 16.710 | +0.72% | 2/6 |
| normal | scrollPaintMs | 20.581 | 20.616 | +0.17% | 2/6 |
| tables | initialLayoutMs | 491.217 | 481.467 | -1.98% | 5/6 |
| tables | firstPaintMs | 90.552 | 91.488 | +1.03% | 2/6 |
| tables | interactiveDownMs | 25.434 | 26.157 | +2.84% | 1/6 |
| tables | interactiveDragMs | 28.155 | 28.338 | +0.65% | 1/6 |
| tables | scrollPaintMs | 32.115 | 31.592 | -1.63% | 4/6 |

## 마지막 회귀·보관·복원

정확성·전체 828쌍·비교기·교대 성능 이후 마지막 전체 회귀 12개·platform integrity를 수행한다. 그 뒤 반디집 ZIP fast level 1로 소스·실행기·입력·캡처·전체 진단·이전 실패·성능 원값을 보관하고 새 폴더에서 복원 렌더링과 native 검사를 다시 실행한다. 기존 미등록 실패도 같은 raw·판정으로 재현해야 한다.

반디집 본 보관본 224,100,496 bytes·복원 증거 41,135,977 bytes의 SHA-256과 모든 entry를 확인했다. 소스 457개와 복원 비교기 120쌍 재판정 후, 새 렌더링 828쌍의 픽셀·전체 진단·raw·판정 및 새 캡슐 방향 24,576건·기존 캡슐 16,384건·글리프 192+3,072건·상태 복원의 로그가 모두 일치했다.

증거: `C:/twf-v33/runs/20261005-axis-capsule-v33-final`, `C:/twf-v33/archives`. 재검사 명령은 `ScrollRenderingRegression.exe --capsule-axes`, `--capsule-boundaries`, `--glyph-clip-transforms`, `--horizontal-native`다. 작업 script·수정 전 헤더와 실패·독립 검사·성능 원값은 보관본 내부 `optimization-and-accuracy-evidence/work-evidence.zip`에 보존한다.

실제 Windows DPI는 96이며 144 검사는 renderer 배율 1.5다. 일반 affine 캡슐·fractional AA group/layer·글리프 자체 회전/비균일 변환·전체 HTML/CSS/DOM/paint 계약·실제 Windows 두 DPI·1,000문서는 미완료다.

## 마지막 정리

복원 검증 뒤 해시가 일치하는 보관 사본 77,936개, 워크스페이스 컴파일 생성물 295개, 렌더링 캐시 39,031개를 정리했다. 소스 457개·보호 입력·최신 828쌍 PNG/JSON·실패 diff·최소 증거·반디집 보관본을 재확인했다. 새 보호 입력 손실 0, 워크스페이스의 남은 컴파일 생성물 0·검사 대상 렌더링 캐시 0이다.

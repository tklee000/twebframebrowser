# v35 좁은·빈 글리프 클립과 반투명 합성 재사용 결과

2026-10-06 완료. Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 새 렌더링과 새/기존 native 로그 검증 완료.

- 독립 subpixel·빈·역전 clip 검사 61,440건: v34 픽셀 실패 8,786 → v35 실패 0. 배율 1·1.5 각각 30,720건이다.
- DirectWrite의 원본 grayscale mask·독립 double gamma/source-over·pixel/clip 교집합과 별도 Direct2D aliased 포함 oracle로 PBGRA byte·premultiplied 범위·클립 밖 보존을 비교했다.
- 기존 불투명도 32,256건·입력 guard 14건·face/factory 전환 8건·방향 24,576건·경계 16,384건·glyph 192+3,072건·일반 텍스트 23,068,672조합·clip/layer/DPI 상태 복원·스크롤/드래그/스타일 복귀도 통과했다.
- 기존 828쌍의 네 decoded RGBA 이미지·전체 native/reference JSON·raw 수치·판정이 v34와 일치했다. 총 802/828 성공(strict 727·기존 backend 승인 75), 미등록 FAIL_PAINT 26 유지.
- 비교기 기본 182개·스크롤바 오류 주입 336/336 통과. 원본 입력·허용치 0·기존 registry 111개 유지, 새 예외 0.

## 정확성 수정과 검사 범위

한 픽셀 안에 clip의 양쪽 경계가 들어오면 이전의 독립 edge factor 곱이 실제 교집합 넓이보다 커졌다. `RasterSurface::DrawGlyphRun`은 X·Y 각각 `max(0, min(pixelEnd, clipEnd) - max(pixelStart, clipStart))`를 구해 곱한다. 폭·높이 0 또는 역전된 device clip은 floor/ceil 이전에 빈 영역으로 종료해 fractional 좌표가 가짜 한 픽셀 영역으로 확장되지 않는다. 부분 opacity·불투명 native·일반 텍스트의 fractional 경계가 같은 교집합을 사용한다.

저장 clip의 좌표는 DPI/96 배율을 먼저 구해 transform 뒤 공통 적용한다. 기존 `point * DPI / 96`은 1.5배 반전 clip에서 기대 19.75를 19.74999809로 만들어 최종 byte 반올림을 바꿨다. cache key와 stack/DPI/surface 무효화 조건은 유지한다.

검사 구성은 brush alpha 4개×opacity 4개×RGB 2개×backdrop alpha 4개×clip 10개×저장 clip transform 3개×AA 2개×glyph 수 4개(1/8/9/32)×배율 2개다. clip은 일반 fractional·X/Y/양축 subpixel·폭 0·높이 0·빈 점·X/Y 역전·viewport 밖이며 transform은 identity·X 반전·Y 반전이다. 같은 조합으로 변경 전 헤더를 실행해 8,786 실패와 원값을 보존했다.

Direct2D의 저장 transform과 축 정렬 bbox 의미는 [공식 PushAxisAlignedClip 문서](https://learn.microsoft.com/en-us/windows/win32/direct2d/id2d1rendertarget-pushaxisalignedclip)를 참고한다. 여러 primitive가 겹치는 fractional AA group/layer의 전체 합성 계약은 이 단일 glyph/서로 겹치지 않는 run 검사에서 검증하지 않는다. 실제 Windows DPI 144 전환과 두 모니터 검증도 미완료다.

기존 화살표 FAIL_PAINT 26쌍의 최대 채널 차이 1·raw 차이 합계 182픽셀은 남아 있다. 허용치를 완화하거나 예외를 추가하지 않았다.

첫 캡처는 reference 환경 fingerprint 변화로 중단돼 원값을 보존했다. 원격 세션 표시 장치 ID·주사율(32 → 60) 및 선택적 DRIVER_VENDOR 문자열 누락이 바뀌었고, GPU vendor/device/LUID·driver version·renderer/backend·DPI·색 공간·해상도·feature 설정은 같았다. 현재 환경 fingerprint를 별도로 동결해 기존 엄격한 환경 검사를 유지했다. frozen v34 실행기로 현재 환경의 48쌍을 다시 캡처해 이전 픽셀·전체 진단·raw·판정과 일치함을 먼저 확인했고, 이후 변경 후 48쌍과 전체 828쌍도 별도로 대조했다. 환경 변화 기록·첫 실패·변경 전 환경 대조 증거는 diagnostics에 보존한다.

## 반복 비용과 실제 앱 성능

부분 opacity의 native 합성은 최종 gamma coverage byte와 backdrop byte로 RGB·alpha 결과를 조회한다. 한 thread당 마지막 RGB/유효 opacity 한 조합만 보관하며 table payload는 256 KiB다. brush opacity는 양자화하지 않고 이전 double source-over와 같은 최종 반올림 결과를 저장한다. 연속 8회 같은 조합이 보이는 때만 table을 만들고 바뀌는 opacity는 direct 경로를 사용한다. fractional coverage도 direct 식으로 처리한다. 회색 foreground·backdrop은 RGB 결과 하나를 공유하며 backdrop alpha 255의 불필요한 반올림을 생략한다. 부분 opacity 루프와 table 생성은 별도 noinline 함수로 두어 불투명/일반 페인트의 코드 크기를 줄였다.

20,000회 paint를 각 조건마다 6회 직렬 교대(3회 before-after·3회 after-before) 측정했다. 64×32 WIC 표면·회색 foreground #8b8b8b·처음 한 번 지운 불투명 backdrop #fcfcfc에 같은 run을 반복해서 그린다. 고정 opacity 0.25/0.5/1 및 매 paint 0.25/0.5 교대×glyph 수 1/4/8/9/32의 120쌍 최종 픽셀 해시가 같고 warm heap 할당은 양쪽 모두 0이다. 표는 중앙값 변화이며 증가값도 그대로 보존한다. 초기 8회와 table 생성 비용은 아래 warm 측정에 포함되지 않는다. 작은 WIC 표면의 개선율을 앱 전체 개선율로 해석하지 않는다.

| opacity | glyph 수 | before ms | after ms | 변화 | 빠른 회차 | heap 할당 |
|---|---:|---:|---:|---:|---:|---:|
| 0.25 | 1 | 17.213 | 4.872 | -71.70% | 6/6 | 0 → 0 |
| 0.25 | 4 | 63.505 | 12.650 | -80.08% | 6/6 | 0 → 0 |
| 0.25 | 8 | 94.995 | 19.105 | -79.89% | 6/6 | 0 → 0 |
| 0.25 | 9 | 96.386 | 18.686 | -80.61% | 6/6 | 0 → 0 |
| 0.25 | 32 | 105.308 | 26.428 | -74.90% | 6/6 | 0 → 0 |
| 0.5 | 1 | 17.501 | 4.860 | -72.23% | 6/6 | 0 → 0 |
| 0.5 | 4 | 63.105 | 12.924 | -79.52% | 6/6 | 0 → 0 |
| 0.5 | 8 | 96.717 | 19.359 | -79.98% | 6/6 | 0 → 0 |
| 0.5 | 9 | 100.740 | 18.805 | -81.33% | 6/6 | 0 → 0 |
| 0.5 | 32 | 101.454 | 25.710 | -74.66% | 6/6 | 0 → 0 |
| 1.0 | 1 | 4.229 | 4.395 | +3.91% | 2/6 | 0 → 0 |
| 1.0 | 4 | 9.904 | 10.318 | +4.18% | 1/6 | 0 → 0 |
| 1.0 | 8 | 14.501 | 15.782 | +8.83% | 0/6 | 0 → 0 |
| 1.0 | 9 | 14.289 | 15.359 | +7.49% | 2/6 | 0 → 0 |
| 1.0 | 32 | 21.154 | 22.421 | +5.99% | 0/6 | 0 → 0 |
| 0.25/0.5 교대 | 1 | 19.466 | 10.391 | -46.62% | 6/6 | 0 → 0 |
| 0.25/0.5 교대 | 4 | 64.814 | 26.478 | -59.15% | 6/6 | 0 → 0 |
| 0.25/0.5 교대 | 8 | 97.308 | 38.413 | -60.52% | 6/6 | 0 → 0 |
| 0.25/0.5 교대 | 9 | 97.339 | 38.087 | -60.87% | 6/6 | 0 → 0 |
| 0.25/0.5 교대 | 32 | 105.551 | 45.089 | -57.28% | 6/6 | 0 → 0 |

첫 시도의 overlap 교정은 1.5배 반전에서 76건 실패했고 DPI 계산 순서를 고쳐 모두 통과했다. eager table 시도 이후 변화하는 opacity 비용을 고려해 연속 재사용 정책을 추가했으며, 불투명 경로 비용을 줄이기 위해 부분 합성 루프를 분리했다. 이전 3개 시도의 정확한 헤더·실행기·독립 검사·6회 성능 원값을 diagnostics/attempt-1·attempt-2·attempt-3에 보존했다. attempt-1의 reflected clip debug 실행기·출력도 보존한다.

같은 보존 앱 자산의 일반 Markdown·250개 표 문서를 각각 6회 직렬 교대 측정했다. BGRA 48쌍·layout JSON 24쌍이 같고 최초 렌더링·스크롤·드래그의 증가 단계와 원값·실행기/source SHA-256도 보존한다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 210.163 | 210.580 | +0.20% | 4/6 |
| normal | firstPaintMs | 102.126 | 103.860 | +1.70% | 3/6 |
| normal | interactiveDownMs | 14.648 | 15.107 | +3.13% | 3/6 |
| normal | interactiveDragMs | 15.606 | 15.493 | -0.72% | 4/6 |
| normal | scrollPaintMs | 20.609 | 20.515 | -0.46% | 2/6 |
| tables | initialLayoutMs | 481.212 | 479.210 | -0.42% | 4/6 |
| tables | firstPaintMs | 89.795 | 86.693 | -3.46% | 4/6 |
| tables | interactiveDownMs | 25.832 | 26.769 | +3.62% | 2/6 |
| tables | interactiveDragMs | 28.877 | 28.790 | -0.30% | 2/6 |
| tables | scrollPaintMs | 31.855 | 31.819 | -0.11% | 1/6 |

## 마지막 회귀·보관·복원

정확성·속도·전체 828쌍·비교기 검증 뒤 마지막 전체 회귀 12개·platform integrity를 수행한다. 이후 반디집 ZIP fast level 1로 소스·실행기·입력·캡처·전체 진단·실패·성능 원값을 보관하고 새 폴더에서 소스 해시·비교기·새 렌더링·native 검사를 재검증한다. 기존 미등록 실패도 같은 raw와 판정으로 재현한다.

반디집 본 보관본 207,921,797 bytes·복원 증거 41,102,046 bytes의 SHA-256과 모든 entry를 확인했다. 소스 457개·복원 비교기 120쌍 재판정 후 새 렌더링 828쌍의 픽셀·전체 진단·raw·판정이 같았다. 새 좁은/빈 clip 61,440건·기존 불투명도 32,256건·입력 guard 14건·face/factory 전환 8건·방향 24,576건·캡슐 16,384건·glyph 192+3,072건·상태 복원 로그 SHA-256도 모두 같았다.

첫 work evidence 압축은 실행 중인 coordinator 로그에 대한 반디집 writable-file 검사로 중단됐다. 경고·요청 manifest·당시 보관 script와 닫힌 실패 로그를 diagnostics/packing-attempt-1에 보존하고, 열린 coordinator/packing 로그를 제외해 완료된 회귀 이후 보관 단계부터 재개했다. 실패한 생성용 scratch ZIP은 SHA-256·크기·사유를 기록하고 정리했다.

증거: `C:/twf-v35/runs/20261006-glyph-clip-v35-final`, `C:/twf-v35/archives`. native 명령은 `ScrollRenderingRegression.exe --glyph-narrow-clip`, `--glyph-opacity`, `--capsule-axes`, `--capsule-boundaries`, `--glyph-clip-transforms`, `--horizontal-native`다. 작업 script·변경 전 헤더와 실패·독립 검사·성능 원값은 보관본 내부 `optimization-and-accuracy-evidence/work-evidence.zip`에 보존한다.

일반 affine 캡슐·여러 primitive의 fractional AA group/layer·글리프 자체 회전/비균일 변환·전체 HTML/CSS/DOM/paint 계약·실제 Windows 두 DPI·1,000문서는 미완료다. MdViewer는 자동 재빌드하지 않았다.

## 마지막 정리

복원 검증 뒤 해시가 일치하는 보관 사본 77,900개, 워크스페이스 컴파일 생성물 295개, 렌더링 캐시 39,130개를 정리했다. 소스 457개·보호 입력·최신 828쌍 PNG/JSON·실패 diff·최소 증거·반디집 보관본을 확인했다. 새 보호 입력 손실 0, 남은 워크스페이스 컴파일 생성물 0·검사 대상 렌더링 캐시 0이다. 과거 두 파일 누락은 기존 보호 기록대로 남긴다.

# v34 native 글리프 불투명도·긴 run 검증 결과

2026-10-05 시작, 2026-10-06 완료. Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 새 렌더링과 새/기존 native 로그 검증 완료.

- 독립 brush alpha×opacity·배경·clip·긴 run 검사 32,256건: v33의 native 지원 반환·픽셀 probe 실패 30,240 → v34 실패 0. 변경 전 일반 fallback으로 완성한 화면 전체의 오류 수를 뜻하지 않는다.
- 원본 DirectWrite grayscale mask와 독립 double gamma/source-over 계산으로 모든 PBGRA byte·premultiplied 범위·클립 밖 보존을 비교했다. 무효 입력 guard 14건과 face/factory 전환·복귀 8건도 통과했다.
- 기존 캡슐 방향 24,576건·경계 16,384건·glyph 192+3,072건·일반 텍스트 23,068,672조합·clip/layer/DPI 상태 복원·스크롤/드래그/스타일 복귀도 통과했다.
- 기존 828쌍의 네 decoded RGBA 이미지·전체 native/reference JSON·raw 수치·판정이 v33과 일치했다. 총 802/828 성공(strict 727·기존 backend 승인 75), 미등록 FAIL_PAINT 26 유지.
- 비교기 기본 182개·스크롤바 오류 주입 336/336 통과. 원본 입력·허용치 0·기존 registry 111개 유지, 새 예외 0.

## 정확성 수정

`RasterSurface::DrawGlyphRun`의 grayscale native 아이콘 경로가 solid brush의 alpha×opacity를 반영한다. 0 opacity는 픽셀 표면을 잠그지 않고 무변경으로 끝낸다. 부분 opacity는 원래 gamma coverage byte에 연속적인 brush product와 clip coverage를 곱하고, RGB와 destination alpha의 premultiplied source-over를 최종 byte에서 한 번 반올림한다. opacity별 alpha 256개는 마지막 product 하나만 재사용하며 색·backdrop·clip은 live 입력이다. 불투명 아이콘의 기존 lookup과 일반 텍스트 합성은 유지한다. 부분 opacity의 ordinary text·ClearType native icon·color font·layer·글리프 자체 회전/비균일 transform은 fallback이다.

검사 구성은 brush alpha 4개×opacity 4개×RGB 2개×backdrop alpha 4개×clip 3개×저장 clip transform 3개×AA 2개×glyph 수 7개(1/4/8/9/32/256/257)×배율 2개다. mask는 engine coverage cache를 우회해 DirectWrite 분석에서 직접 읽는다. 기존 file-face 연결만 공유하고 gamma·source-over는 독립 double 식이며, aliased clip 포함은 별도 Direct2D 표면에서 얻는다. 이전 헤더도 같은 검사를 실행해 미지원 반환과 픽셀 실패를 보존했다.

null face·NaN em·0 em·NaN x·infinite y·마지막 glyph의 NaN advance/offset을 픽셀 기록 전에 거부한다. 중간에 준비한 mask reference도 종료 시 해제하며, 거부 뒤 정상 페인트와 무변경 배경을 byte 단위로 확인한다. 유한 brush/transform/DPI와 안전한 정수 좌표 범위를 검사한다. glyph finite 분류는 IEEE exponent를 memcpy로 읽어 aliasing 가정 없이 수행한다.

기존 화살표 FAIL_PAINT 26쌍의 최대 채널 차이 1·raw 차이 합계 182픽셀은 남아 있다. 전체 828쌍의 전후 수치는 위 판정을 따른다.

## 반복 비용과 실제 앱 성능

1~8개 native run은 기존 stack mask 저장소를 사용한다. 9~256개는 thread별 vector capacity를 재사용하고 매번 reference를 해제한다. 저장 capacity는 최대 256개이며 257개 이상·일반 텍스트는 local vector를 사용한다. face 상태는 owned factory/face/raster 한 쌍만 보관해 immutable color-font 질의와 기존 file-face map 재조회 비용을 줄인다. 다른 face·다른 factory·원래 face/factory 복귀를 픽셀 비교로 확인했다.

기존에 지원되던 불투명 native 글리프 paint 20,000회를 6회 직렬 교대(3회 before-after·3회 after-before) 측정했다. 새 부분 opacity 경로를 이전 미지원 경로와 속도 비교하지 않는다. 42쌍의 최종 픽셀 해시가 같고 9/32/256개 조건의 warm heap 할당은 20,000 → 0이다. 257개 조건의 20,000회 할당은 제한 정책에 따라 유지한다. 표의 증가값도 그대로 보존하며 이 작은 WIC 표면 측정을 앱 전체 개선율로 해석하지 않는다.

| glyph 수 | before ms | after ms | 변화 | 빠른 회차 | heap 할당 |
|---:|---:|---:|---:|---:|---:|
| 1 | 4.318 | 4.214 | -2.41% | 6/6 | 0 → 0 |
| 4 | 9.839 | 10.327 | +4.96% | 2/6 | 0 → 0 |
| 8 | 14.781 | 15.314 | +3.61% | 0/6 | 0 → 0 |
| 9 | 15.527 | 14.684 | -5.43% | 6/6 | 20,000 → 0 |
| 32 | 22.096 | 21.579 | -2.34% | 5/6 | 20,000 → 0 |
| 256 | 88.837 | 89.378 | +0.61% | 3/6 | 20,000 → 0 |
| 257 | 89.895 | 89.333 | -0.63% | 3/6 | 20,000 → 20,000 |

첫 구현은 할당이 줄었지만 +8.23~+27.09% 증가했다. finite 검사와 긴 run 저장소 접근을 줄인 두 번째 구현도 +0.84~+12.34%였다. 마지막 immutable face 조회 재사용을 추가한 소스로 위 수치를 다시 측정했다. 두 이전 시도의 실행기·헤더·독립 검사·6회 원값을 diagnostics/attempt-1·attempt-2에 보존했다. attempt-1 헤더는 측정 이후 바뀐 blend-table setup 한 패치를 되돌려 복원했으며 이 사실을 evidence에 명시했다.

같은 보존 앱 자산의 일반 Markdown·250개 표 문서를 각각 6회 직렬 교대 측정했다. BGRA 48쌍·layout JSON 24쌍이 같다. 증가한 단계와 paired 원값·실행기/source SHA-256도 보존한다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 229.902 | 220.692 | -4.01% | 3/6 |
| normal | firstPaintMs | 119.537 | 120.283 | +0.62% | 2/6 |
| normal | interactiveDownMs | 17.256 | 16.335 | -5.34% | 3/6 |
| normal | interactiveDragMs | 19.342 | 18.185 | -5.98% | 5/6 |
| normal | scrollPaintMs | 22.269 | 21.043 | -5.50% | 6/6 |
| tables | initialLayoutMs | 557.330 | 558.175 | +0.15% | 4/6 |
| tables | firstPaintMs | 104.517 | 106.796 | +2.18% | 1/6 |
| tables | interactiveDownMs | 26.722 | 28.796 | +7.76% | 1/6 |
| tables | interactiveDragMs | 27.960 | 30.908 | +10.54% | 2/6 |
| tables | scrollPaintMs | 33.043 | 33.394 | +1.06% | 2/6 |

## 마지막 회귀·보관·복원

정확성·속도·전체 828쌍·비교기 검증 이후 마지막 전체 회귀 12개·platform integrity를 수행한다. 이후 반디집 ZIP fast level 1로 소스·실행기·입력·캡처·전체 진단·이전 실패·성능 원값을 보관하고 새 폴더에서 소스 해시·비교기·새 렌더링·native 검사를 재검증한다. 기존 미등록 실패도 같은 raw와 판정으로 재현한다.

반디집 본 보관본 181,559,389 bytes·복원 증거 41,138,691 bytes의 SHA-256과 모든 entry를 확인했다. 소스 457개·복원 비교기 120쌍 재판정 후 새 렌더링 828쌍의 픽셀·전체 진단·raw·판정이 같았다. 새 불투명도 32,256건·입력 guard 14건·face/factory 전환 8건 및 기존 방향 24,576건·캡슐 16,384건·glyph 192+3,072건·상태 복원의 로그 SHA-256도 모두 같았다.

증거: `C:/twf-v34/runs/20261005-glyph-opacity-v34-final`, `C:/twf-v34/archives`. native 재검사 명령은 `ScrollRenderingRegression.exe --glyph-opacity`, `--capsule-axes`, `--capsule-boundaries`, `--glyph-clip-transforms`, `--horizontal-native`다. 작업 script·변경 전 헤더와 실패·독립 검사·성능 원값은 보관본 내부 `optimization-and-accuracy-evidence/work-evidence.zip`에 보존한다.

실제 Windows DPI는 96이고 144 검사는 renderer 배율 1.5다. 일반 affine 캡슐·fractional AA group/layer·글리프 자체 회전/비균일 변환·전체 HTML/CSS/DOM/paint 계약·실제 Windows 두 DPI·1,000문서는 미완료다.

## 마지막 정리

복원 검증 뒤 해시가 일치하는 보관 사본 76,607개, 워크스페이스 컴파일 생성물 295개, 렌더링 캐시 38,213개를 정리했다. 소스 457개·보호 입력·최신 828쌍 PNG/JSON·실패 diff·최소 증거·반디집 보관본을 재확인했다. 새 보호 입력 손실 0, 남은 워크스페이스 컴파일 생성물 0·검사 대상 렌더링 캐시 0이다. 과거 두 파일 누락은 기존 보호 기록대로 남긴다.

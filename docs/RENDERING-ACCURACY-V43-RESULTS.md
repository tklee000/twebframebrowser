# v43 일반 회색조 반투명 합성·조회표 결과

2026-10-06. Windows x64 Release. 전체 회귀·반디집 보관·새 폴더 복원·828쌍 새 렌더링·새/기존 검사 로그 대조 완료.

- 사용자 재개 요청 이후 소스 457개·설치 글꼴 24개·실행기/런타임·그래픽 fingerprint를 다시 확인했다. 핵심 검사 여섯 명령을 새로 실행하여 이전 로그 SHA-256 일치까지 확인한 뒤 동일 소스의 독립 정확성·교대 성능 근거를 재사용했다. 중단된 120쌍 캡처·106쌍 비교는 별도 폴더와 보관 증거에 유지하고 전체 828쌍을 새로 캡처했다.
- 독립 byte-backed DirectWrite LCD mask→회색조·gamma·source-over 기준으로 13,824건, 거부 304건, 거부 이후 복귀 288건을 검사했다. 수정 후 실패 0. 조회표를 끈 대조군도 같은 독립 검사를 통과했다.
- 수정 전 CPU 경로 미지원 6,912건·실제 받아들인 경로의 픽셀 불일치 3,144건·LCD near-one guard 실패 8건을 보존한다. 미지원은 기존 false/fallback이며 모두 최종 화면 오류라는 뜻은 아니다.
- Arial/Segoe UI·명시적 96/144 DPI·세 signed diagonal axes·quarter phase·회색/색상·full/fractional/narrow clip·LTR/RTL·glyph offset·혼합 1/9 glyph를 사용한다. 색 alpha와 브러시 opacity의 여섯 곱(0·.125·.3·near-one 두 조합·1), 투명/반투명 PBGRA backdrop, 1/9회 반복을 비교한다. 모든 프레임에서 RGB≤alpha도 확인한다.
- 일반 회색조 글자에서 coverage × 색 alpha × 브러시 opacity × clip을 합성한다. 기존 일반 글자의 source와 backdrop을 따로 반올림하는 규칙을 유지한다. native icon의 single-round 규칙과 조회표는 별도로 유지한다.
- 반투명 LCD는 alpha가 정확히 1인 경우에만 CPU 경로를 허용한다. 종전 .9999 tolerance 때문에 작은 opacity 차이를 잃던 경로를 막았다. 불가 transform·NaN advance/offset/em의 늦은 실패는 표면을 건드리지 않는다. 1/8/9/32/256/257 run 경계의 다음 valid 반투명 paint도 독립 픽셀로 검증한다.
- 색/opacity 조합이 8회 반복되어야 준비하는 thread-local 조회표는 256 KiB로 제한한다. 색/opacity가 바뀌면 즉시 직접 계산으로 돌아가고 새 키를 준비한다. 회색 foreground는 RGB 한 plane을 계산해서 복사한다. 완전히 덮인 clip과 fractional clip의 내부 픽셀에서 lookup을 사용하고 fractional 경계는 직접 계산한다.
- 기존 불투명·LCD·native 계약을 유지한다. 불투명 fractional alpha oracle에서 기존 double 반올림을 float으로 옮겼을 때 두 건의 차이가 생겨 그 기준 계산을 보완했다. 첫 oracle·로그를 보존하며 수정된 동일 oracle로 before/after/direct를 모두 재검사했다.
- v42 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp만 변경했고 나머지 455개 해시는 같다. focused/성능/전체 캡처 source freeze를 확인한다.
- 전체 828쌍의 이미지·전체 JSON·raw·판정은 v42와 같다. strict 727·기존 승인 75, 총 802/828. 기존 화살표 FAIL_PAINT 26 유지. 새 예외 0·registry 111개·허용치 0.
- 실제 Windows DPI는 96. native snapshot과 WebView2 두 reference 경로는 828쌍, WM_PRINTCLIENT는 실제 96 DPI의 414쌍을 수집한다. 명시적 144 DPI의 414쌍은 실제 창 경로 미검증 사유를 보존한다.

## 6회 직렬 교대 성능

불투명은 동결 v42 대조군과, 새 반투명 경로는 동일 구현에서 OrdinaryOpacityTables 호출만 끈 직접 계산 대조군과 비교했다. 반투명 속도 수치는 v42의 fallback 대비가 아니다. 대조군 header·변경 내용·SHA-256을 보존했다. 각 10,000회 paint, 24조건 × 6회 단일/반복 BGRA hash 144쌍이 같다. warm C++ operator new 할당은 양쪽 모두 0이며 DirectWrite 전체 내부 할당을 의미하지 않는다. 최초 조회표 준비 비용은 이 warm 측정에 포함하지 않는다.

| 경로 | 색상 | glyph 수 | 모드/clip | 대조 ms | 변경 ms | 변화 | 빠른 회차 |
|---|---|---:|---|---:|---:|---:|---:|
| opaque | #8b8b8b | 1 | gray/full | 3.681 | 3.539 | -3.85% | 5/6 |
| opaque | #8b8b8b | 1 | LCD/full | 5.676 | 5.688 | +0.21% | 2/6 |
| opaque | #8b8b8b | 9 | gray/full | 22.165 | 21.286 | -3.96% | 4/6 |
| opaque | #8b8b8b | 9 | LCD/full | 39.095 | 41.122 | +5.18% | 1/6 |
| opaque | #8b8b8b | 32 | gray/full | 73.587 | 70.939 | -3.60% | 6/6 |
| opaque | #8b8b8b | 32 | LCD/full | 139.389 | 144.417 | +3.61% | 1/6 |
| opaque | #2763bd | 1 | gray/full | 3.441 | 3.511 | +2.02% | 3/6 |
| opaque | #2763bd | 1 | LCD/full | 5.780 | 5.682 | -1.70% | 3/6 |
| opaque | #2763bd | 9 | gray/full | 18.895 | 18.732 | -0.86% | 3/6 |
| opaque | #2763bd | 9 | LCD/full | 38.304 | 40.954 | +6.92% | 1/6 |
| opaque | #2763bd | 32 | gray/full | 63.748 | 64.160 | +0.65% | 2/6 |
| opaque | #2763bd | 32 | LCD/full | 139.933 | 139.422 | -0.37% | 3/6 |
| partial | #8b8b8b | 1 | gray/full | 17.639 | 3.331 | -81.12% | 6/6 |
| partial | #8b8b8b | 1 | gray/fractional | 17.685 | 5.951 | -66.35% | 6/6 |
| partial | #8b8b8b | 9 | gray/full | 146.126 | 17.887 | -87.76% | 6/6 |
| partial | #8b8b8b | 9 | gray/fractional | 148.590 | 20.993 | -85.87% | 6/6 |
| partial | #8b8b8b | 32 | gray/full | 512.655 | 62.292 | -87.85% | 6/6 |
| partial | #8b8b8b | 32 | gray/fractional | 516.516 | 63.543 | -87.70% | 6/6 |
| partial | #2763bd | 1 | gray/full | 29.053 | 3.281 | -88.71% | 6/6 |
| partial | #2763bd | 1 | gray/fractional | 28.909 | 7.633 | -73.60% | 6/6 |
| partial | #2763bd | 9 | gray/full | 247.958 | 17.465 | -92.96% | 6/6 |
| partial | #2763bd | 9 | gray/fractional | 252.078 | 21.390 | -91.51% | 6/6 |
| partial | #2763bd | 32 | gray/full | 871.898 | 60.860 | -93.02% | 6/6 |
| partial | #2763bd | 32 | gray/fractional | 871.766 | 69.501 | -92.03% | 6/6 |

새 키의 최초·8번째 조회표 생성·다음 1,000회도 6회 교대 측정했다. 생성 전후 누적 BGRA hash 72쌍은 동일하다. 8번째 생성 비용과 회수 추정치를 아래에 공개한다. 한 글리프처럼 적은 픽셀의 짧은 paint는 준비 비용을 회수하려면 더 오래 반복해야 한다. 추정은 이후 측정한 paint당 절감 시간을 사용하며 실제 앱 보장치는 아니다.

| 색상 | glyph 수 | clip | 직접 8번째 µs | 조회표 8번째 µs | 추가 비용 회수 예상 paint 수 |
|---|---:|---|---:|---:|---:|
| #8b8b8b | 1 | full | 2.80 | 471.55 | 384.6 |
| #8b8b8b | 1 | fractional | 2.90 | 461.00 | 317.5 |
| #8b8b8b | 9 | full | 22.70 | 462.55 | 34.2 |
| #8b8b8b | 9 | fractional | 22.95 | 465.00 | 30.3 |
| #8b8b8b | 32 | full | 78.45 | 465.30 | 7.6 |
| #8b8b8b | 32 | fractional | 78.10 | 470.00 | 10.6 |
| #2763bd | 1 | full | 2.80 | 803.30 | 327.4 |
| #2763bd | 1 | fractional | 2.80 | 782.90 | 360.1 |
| #2763bd | 9 | full | 25.40 | 792.40 | 32.9 |
| #2763bd | 9 | fractional | 23.60 | 840.45 | 38.1 |
| #2763bd | 32 | full | 83.50 | 780.60 | 6.5 |
| #2763bd | 32 | fractional | 83.10 | 781.00 | 7.6 |

같은 일반 Markdown·250개 표 앱 자산을 각 6회 교대 측정했다. BGRA 48쌍·layout 24쌍이 같다. 증가한 구간도 보존하며 helper 개선율을 앱 전체 개선으로 확대하지 않는다.

| 문서 | 구간 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 209.424 | 206.452 | -1.42% | 2/6 |
| normal | firstPaintMs | 129.724 | 131.087 | +1.05% | 3/6 |
| normal | interactiveDownMs | 14.927 | 14.997 | +0.47% | 2/6 |
| normal | interactiveDragMs | 16.654 | 16.671 | +0.10% | 4/6 |
| normal | scrollPaintMs | 20.674 | 20.904 | +1.11% | 3/6 |
| tables | initialLayoutMs | 473.853 | 481.177 | +1.55% | 1/6 |
| tables | firstPaintMs | 117.164 | 106.769 | -8.87% | 5/6 |
| tables | interactiveDownMs | 26.664 | 26.709 | +0.17% | 3/6 |
| tables | interactiveDragMs | 28.077 | 28.601 | +1.87% | 2/6 |
| tables | scrollPaintMs | 32.517 | 32.634 | +0.36% | 3/6 |

## 마지막 회귀·반디집 보관·복원

정확성·성능 검증 뒤 전체 회귀 12개·platform integrity·오류 주입 182+336건 통과. Scroll 전체 검사는 기존 native 축/clip/opacity/capsule 대형 검사도 포함한다. 반디집 ZIP fast level 1 본 보관본 220,800,133 bytes·복원 증거 44,571,279 bytes의 모든 entry SHA-256을 확인했다. 새 폴더에서 source 457개·실행기·runtime·입력을 복원하고 828쌍을 새로 렌더링해 이미지·진단·raw·판정 동일성을 확인했다. 새 opacity·기존 axis/gray alpha/clip cache/clip transform/native theme 여섯 명령의 로그 SHA-256이 같다. live 보호 입력 2,799개도 내부 ZIP에 보관하고 새 폴더에 실제 복원해 모든 해시를 검사했다.

이전 보호 보관본 C:/twf-v24/cleanup-archives/artifact-history.7z는 작업 시작 전부터 없다. 그 안의 4,544개 입력 실물 가용성·해시를 확인하지 못했다. 예전 security-profile HTML 두 파일 누락도 유지한다. 이번 live 입력 손실 0.

일반 회전/skew·fractional group/layer·반투명 LCD·color font는 fallback. 전체 HTML/CSS/DOM/paint 계약·실제 Windows 두 DPI/두 모니터·1,000문서는 미완료다. MdViewer 자동 재빌드 없음.

원값·보관본: C:/twf-v43/runs/20261006-gray-opacity-v43-final, C:/twf-v43/archives. 새 검사는 ScrollRenderingRegression.exe --gray-text-opacity로 실행한다.

모든 복원 검사 뒤 생성물 328개·disposable profile cache 40,129개를 정리했다. 소스·live 입력·최신 828쌍·실행기·보관본 유지, 남은 검사 대상 생성물/캐시 0. 최종 문서·정리 기록은 v43-final-records.zip에 반디집 보관하고 새 폴더 복원·실물 해시를 검사한다.

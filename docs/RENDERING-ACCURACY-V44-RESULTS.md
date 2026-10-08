# v44 일반 반투명 조회표 전체 항목·최초 생성 비용 결과

2026-10-06. Windows x64 Release. 전체 회귀·반디집 보관·새 폴더 복원·828쌍 새 렌더링·7개 검사 로그 대조 완료.

v43에서 남긴 8번째 paint의 조회표 생성 비용을 줄였다. coverage마다 일정한 foreground RGB와 alpha 기여분을 배경 256개 루프 밖에서 한 번씩 반올림한다. float 곱·source/backdrop의 별도 반올림·255 clamp·모든 table byte·256 KiB 제한·동일 키 8회 준비 정책은 유지한다. native icon의 독립 single-round 표와 draw hot path는 변경하지 않았다.

## 정확성 검증

- 독립 binary32 비트 해석과 정수 반올림 oracle로 7색 × 10 opacity × 256 coverage × 256 backdrop × 4채널, **18,350,080항목**을 검사했다. std::lround를 기준 함수에 사용하지 않는다. 0·최소 subnormal·1/255·.125·.3·.5와 이웃 float·1과 이웃 float을 포함한다.
- 변경 전 v43·변경 후·lookup을 끈 대조군 모두 항목 실패 0, 키/8회 threshold/교체/복귀 763건 실패 0. 각 256 KiB table 전체 signature와 70개 key의 합성 signature가 같다. 예전에 준비한 키로 돌아가도 7회 직접 경로 뒤 8회에 새 표가 생성되는 것을 검증한다.
- 별도 복사 header에 source 반올림을 truncation으로 바꾸는 오류와 준비 시점을 7회로 바꾸는 오류를 주입했다. 독립 검사가 각각 잘못된 항목 3,577,304개·키 assertion 154건을 탐지하고 exit 1로 거부했다. 실제 소스는 이 대조 실험에서 변경하지 않았다.
- 기존 독립 byte-backed DirectWrite glyph oracle도 세 구현 모두 13,824프레임·거부 304건·복귀 288건 실패 0. 이번 변경 전에 알려진 픽셀 오류를 새로 발견한 것은 아니며, 미세한 반올림 변경을 방지하는 검증 범위를 확대했다.
- Arial/Segoe UI, 명시적 96/144 DPI, signed diagonal axes, fractional/narrow clip, LTR/RTL, offset, 1/9 glyph, brush alpha × opacity, 투명/반투명 PBGRA backdrop과 반복 합성을 유지한다. RGB≤alpha도 검증한다.
- 동결 v43 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp만 변경했고 나머지 455개는 해시가 같다. focused·성능·전체 캡처 소스와 실행기 freeze를 확인한다.
- 고정 48쌍의 변경 전 환경 대조와 새 실행기 대조를 통과했다. 전체 828쌍의 decoded pixel·전체 JSON·raw·판정은 v43과 같다. strict 727 + 기존 backend 승인 75 = **802/828**, 화살표 FAIL_PAINT **26건 유지**. 새 예외 0·registry 111개·허용치 0.
- 실제 Windows DPI 96의 WM_PRINTCLIENT 414쌍을 확인한다. 명시적 144 DPI 414쌍을 실제 144 DPI Windows 창 검증으로 계산하지 않는다.

## 직렬 교대 속도

이전 v43의 활성 lookup을 기준으로 첫 8회와 이후 paint를 비교한다. 별도 직접 계산 대조군은 동일 신규 구현의 OrdinaryOpacityTables draw 호출만 끈 것이다. 세 구현의 첫 8회/다음 1,000회 BGRA hash **144쌍**이 같다. 아래 helper 수치는 조건별 6회 중앙값이며 생성 비용 개선은 앱 전체 속도 개선율이 아니다.

최초 cold 측정은 before에 --cold 분기가 없고 EndDraw 전 image lock이 실패한 것을 발견하여 채택하지 않았다. 동일 측정 본문·실제 opaque glyph warmup·EndDraw 후 0이 아닌 이미지 해시로 고쳤다. 첫 시도 원값과 probe를 cold-initial-invalid에 보존했고, 본문 통일 전 warm 원값도 warm-initial-probe-revision에 유지했다. 표의 값은 수정 후 6회 교대 측정만 사용한다.

**v43 기록 보정:** 같은 이전 측정기의 hashEight도 144개 샘플 모두 0이었다. v43의 “생성 전후 72쌍” 중 첫 8회 이미지 대조는 검증 근거에서 제외한다. 다음 1,000회까지 누적한 최종 이미지 hash 72쌍과 독립 glyph 13,824프레임 검사는 유효하다. opacity 0 warmup도 mask를 준비하지 못했던 한계가 있다. v43 원값·보관본은 유지하며 이 보정은 v43-cold-hash-erratum.json과 최종 기록 ZIP에 남긴다. v44의 위 cold 수치는 실제 opaque prewarm·EndDraw 이후 0이 아닌 첫 8회 해시를 사용하고 동결 v43 실행 경로도 같은 방식으로 재검증했다.

| 색상 | glyph | clip | v43 8번째 µs | v44 8번째 µs | 변화 | 빠른 회차 | 직접 경로 대비 회수 예상 추가 paint |
|---|---:|---|---:|---:|---:|---:|---:|
| #8b8b8b | 1 | full | 464.30 | 179.80 | -61.28% | 6/6 | 136.2 |
| #8b8b8b | 1 | fractional | 464.55 | 180.45 | -61.16% | 6/6 | 243.1 |
| #8b8b8b | 9 | full | 461.65 | 180.05 | -61.00% | 6/6 | 12.2 |
| #8b8b8b | 9 | fractional | 462.05 | 181.35 | -60.75% | 6/6 | 12.6 |
| #8b8b8b | 32 | full | 465.00 | 185.05 | -60.20% | 6/6 | 1.7 |
| #8b8b8b | 32 | fractional | 471.15 | 194.10 | -58.80% | 6/6 | 0.0 |
| #2763bd | 1 | full | 774.35 | 237.15 | -69.37% | 6/6 | 97.7 |
| #2763bd | 1 | fractional | 774.15 | 231.00 | -70.16% | 6/6 | 101.2 |
| #2763bd | 9 | full | 780.00 | 236.50 | -69.68% | 6/6 | 8.8 |
| #2763bd | 9 | fractional | 780.00 | 247.60 | -68.26% | 6/6 | 9.2 |
| #2763bd | 32 | full | 780.70 | 239.85 | -69.28% | 6/6 | 1.9 |
| #2763bd | 32 | fractional | 782.90 | 242.55 | -69.02% | 6/6 | 1.4 |

최초 조회표 준비 paint의 변화 범위 -70.16%~-58.80%. 회수 추정은 직접 계산 대비 추가 준비 비용과 다음 1,000회 paint 절감 시간으로 계산하며 실제 앱의 반복 횟수를 보장하지 않는다. 짧은 run에서는 준비 비용이 남는다.

불투명/LCD와 반투명 warm paint는 모두 동결 v43 활성 lookup과 비교했다. 24조건 × 6회, 각 10,000회 paint의 단일/누적 BGRA hash 144쌍이 같다. 양쪽 warm C++ operator new 할당 0. 이는 DirectWrite 전체 내부 할당이 아니며 최초 준비 시간은 위 cold 측정에 포함한다.

| 경로 | 색상 | glyph | mode/clip | v43 ms | v44 ms | 변화 | 빠른 회차 |
|---|---|---:|---|---:|---:|---:|---:|
| opaque | #8b8b8b | 1 | gray/full | 3.550 | 3.275 | -7.74% | 4/6 |
| opaque | #8b8b8b | 1 | LCD/full | 5.561 | 5.697 | +2.45% | 1/6 |
| opaque | #8b8b8b | 9 | gray/full | 20.599 | 18.970 | -7.91% | 5/6 |
| opaque | #8b8b8b | 9 | LCD/full | 39.613 | 39.133 | -1.21% | 3/6 |
| opaque | #8b8b8b | 32 | gray/full | 73.795 | 64.142 | -13.08% | 5/6 |
| opaque | #8b8b8b | 32 | LCD/full | 138.751 | 135.278 | -2.50% | 4/6 |
| opaque | #2763bd | 1 | gray/full | 3.580 | 3.289 | -8.11% | 5/6 |
| opaque | #2763bd | 1 | LCD/full | 5.600 | 5.890 | +5.17% | 2/6 |
| opaque | #2763bd | 9 | gray/full | 19.106 | 18.568 | -2.82% | 3/6 |
| opaque | #2763bd | 9 | LCD/full | 39.797 | 38.731 | -2.68% | 5/6 |
| opaque | #2763bd | 32 | gray/full | 63.845 | 62.481 | -2.14% | 5/6 |
| opaque | #2763bd | 32 | LCD/full | 138.546 | 132.453 | -4.40% | 5/6 |
| partial | #8b8b8b | 1 | gray/full | 3.409 | 3.298 | -3.27% | 3/6 |
| partial | #8b8b8b | 1 | gray/fractional | 5.777 | 5.702 | -1.29% | 5/6 |
| partial | #8b8b8b | 9 | gray/full | 17.144 | 17.335 | +1.12% | 3/6 |
| partial | #8b8b8b | 9 | gray/fractional | 20.160 | 20.093 | -0.33% | 3/6 |
| partial | #8b8b8b | 32 | gray/full | 59.150 | 58.863 | -0.48% | 4/6 |
| partial | #8b8b8b | 32 | gray/fractional | 64.603 | 66.503 | +2.94% | 2/6 |
| partial | #2763bd | 1 | gray/full | 3.331 | 3.331 | -0.01% | 3/6 |
| partial | #2763bd | 1 | gray/fractional | 7.789 | 7.115 | -8.65% | 5/6 |
| partial | #2763bd | 9 | gray/full | 17.533 | 17.264 | -1.53% | 4/6 |
| partial | #2763bd | 9 | gray/fractional | 21.326 | 21.343 | +0.08% | 3/6 |
| partial | #2763bd | 32 | gray/full | 59.092 | 60.855 | +2.98% | 2/6 |
| partial | #2763bd | 32 | gray/fractional | 68.652 | 66.620 | -2.96% | 5/6 |

같은 일반 Markdown·250개 표 자산을 각 12회 교대 측정했다. BGRA 96쌍·layout 48쌍 동일. 증가한 구간도 기록한다.

| 문서 | 구간 | v43 ms | v44 ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 208.084 | 202.606 | -2.63% | 8/12 |
| normal | firstPaintMs | 116.004 | 124.442 | +7.27% | 3/12 |
| normal | interactiveDownMs | 15.503 | 15.440 | -0.41% | 6/12 |
| normal | interactiveDragMs | 16.477 | 16.496 | +0.11% | 7/12 |
| normal | scrollPaintMs | 21.279 | 20.605 | -3.17% | 9/12 |
| tables | initialLayoutMs | 482.010 | 464.336 | -3.67% | 11/12 |
| tables | firstPaintMs | 101.246 | 101.575 | +0.32% | 4/12 |
| tables | interactiveDownMs | 26.921 | 26.171 | -2.79% | 6/12 |
| tables | interactiveDragMs | 28.883 | 28.180 | -2.43% | 9/12 |
| tables | scrollPaintMs | 32.897 | 32.333 | -1.71% | 10/12 |

첫 6회에서 첫 paint 중앙값이 일반 +7.17%, 표 +14.71%여서 마지막 회귀·보관을 보류했다. 전체 캡처가 종료한 뒤 소스 457개 불변을 확인하고 추가 6회를 직렬 교대 실행했다. 첫 6회 원값·보고서는 summary-first-six.json에 보존하고 위 표는 합계 12회 전체를 사용한다. 표 첫 paint 증가는 +0.32%로 줄었다. **일반 문서 첫 paint는 전체 12회 +7.27%, 추가 6회 +9.56%가 남아 성능 한계로 기록한다.** 일반 전체 12회에서는 3회만 빨랐다. 조회표 준비 비용 개선을 앱 전체 개선율로 계산하지 않는다. 아래에는 추가 6회의 변화도 함께 공개한다.

| 문서 | 구간 | 전체 12회 변화 | 추가 6회 변화 | 전체 빠른 회차 | 추가 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | -2.63% | -0.91% | 8/12 | 4/6 |
| normal | firstPaintMs | +7.27% | +9.56% | 3/12 | 1/6 |
| normal | interactiveDownMs | -0.41% | +0.47% | 6/12 | 3/6 |
| normal | interactiveDragMs | +0.11% | +0.51% | 7/12 | 3/6 |
| normal | scrollPaintMs | -3.17% | -0.77% | 9/12 | 5/6 |
| tables | initialLayoutMs | -3.67% | -4.31% | 11/12 | 6/6 |
| tables | firstPaintMs | +0.32% | -3.73% | 4/12 | 3/6 |
| tables | interactiveDownMs | -2.79% | +2.04% | 6/12 | 2/6 |
| tables | interactiveDragMs | -2.43% | -1.35% | 9/12 | 4/6 |
| tables | scrollPaintMs | -1.71% | -2.25% | 10/12 | 6/6 |

## 마지막 회귀·반디집 보관·복원

정확성·성능·전체 캡처 이후 전체 회귀 12개·platform integrity·오류 주입 182+336건 통과. 반디집 ZIP fast level 1 본 보관본 224,637,188 bytes와 복원 증거 44,572,520 bytes의 모든 entry SHA-256을 확인했다. 새 폴더에서 소스 457개·실행기·런타임·입력을 복원하고 828쌍을 새로 렌더링해 이미지·전체 진단·raw·판정이 같다. 새 table와 기존 opacity/axis/gray/clip cache/clip transform/native theme 7개 명령 로그 SHA-256도 같다. live 보호 입력 2,799개를 내부 ZIP에 보관하고 새 폴더에 실제 복원해 모든 해시를 확인했다.

모든 복원 검증 후 생성물 350개·disposable profile cache 39,145개를 정리했다. 소스·live 입력·최신 828쌍·실행기·보관본 유지, 검사 대상 생성물/캐시 0. 최종 문서·정리 기록은 v44-final-records.zip에 반디집으로 보관하고 새 폴더 복원·실물 해시를 확인한다.

## 남은 범위와 근거

이전 보호 보관본 C:/twf-v24/cleanup-archives/artifact-history.7z는 이번 시작 전부터 없다. 4,544개 이전 입력 실물 가용성·해시는 미확인, 예전 security-profile HTML 두 개 누락도 유지한다. 이번 live 입력 손실 0.

일반 회전/skew·fractional group/layer·반투명 LCD·color font는 fallback. 전체 HTML/CSS/DOM/paint 계약·실제 Windows 두 DPI/두 모니터·1,000문서는 미완료. 작성된 pilot 문서는 기존 20개이며 calibration을 1,000문서로 세지 않는다. MdViewer 자동 재빌드 없음.

실행: ScrollRenderingRegression.exe --opacity-table. 원값·단계별 로그: C:/twf-v44/diagnostics, C:/twf-v44/performance-final, C:/twf-v44/runs/20261006-opacity-table-v44-final. 보관본: C:/twf-v44/archives.

# v45 글자 합성표 전체 색·초기화 지연·배경 계산 공유 결과

2026-10-06. Windows x64 Release. 전체 회귀·반디집 보관·새 폴더 복원·828쌍 새 렌더링·8개 검사 로그 대조 완료.

v44에서 확인한 일반 문서 첫 paint 증가를 추적하면서 쓰이지 않는 글자 합성표의 초기화와 색마다 반복되는 backdrop 계산을 줄였다. 먼저 전체 glyph run을 검증하고 visible mask·alpha가 있을 때만 composition 표를 준비한다. 반투명 일반 text는 사용하지 않는 불투명 RGB 표를 만들지 않는다. ordinary text의 backdrop point rounding을 5/6/8-bit별로 공유하고 검은색 표로도 재사용한다. native colored icon은 전체 source-over를 한 번 반올림하는 규칙을 유지한다. 입력·반올림·pixel·fallback·허용치 변경은 없다.

최종 구현은 cached opacity 합성에서 corrected coverage가 0이면 lookup·RGB/alpha write를 생략한다. coverage 0인 모든 table plane은 backdrop과 같은 값이므로 결과가 달라지지 않는다. 빈 coverage에서 불필요한 쓰기를 줄이며 fractional clip/direct 합성은 기존 규칙을 유지한다.

## 독립 정확성 검증

- 전체 256색 × (5/6/8-bit ordinary coverage + 8-bit native coverage) × backdrop 256개 = **39,845,888항목**을 구현과 다른 double 기반 독립 기준으로 검사했다. coverage bit를 하나씩 이어 붙이며 ideal alpha의 분모 255가 홀수이므로 half-integer와 최소 1/510 떨어져 double 반올림의 모호성이 없다. ordinary는 source/backdrop을 따로, native는 합계를 한 번 반올림한다.
- 동결 v44·신규·opacity lookup을 끈 대조군 모두 항목 실패 0·키/포인터 재사용 1,024건 실패 0. 모든 table byte를 누적한 signature 13240766707082818171가 같다. black ordinary/native plane을 공유한 후 전체 palette payload는 39,845,888 → 39,780,352 bytes다. 88 KiB의 공유 backdrop plane은 이 합계에 포함하며 캐시 metadata와 allocator overhead는 payload가 아니다.
- 복사 header에 ordinary를 single rounding으로 바꾸는 오류와 coverage bit replication을 linear scaling으로 바꾸는 오류를 주입했다. 잘못된 항목을 각각 5,612,508개·458,752개 탐지해 exit 1로 거부했다. 실제 소스 해시는 유지했다.
- 기존 binary32 opacity oracle 18,350,080항목·키 763건, 독립 byte-backed DirectWrite glyph 13,824프레임·거부 304건·복귀 288건을 세 구현 모두 실패 0으로 통과했다. 7색·10 opacity·이웃 float·signed axes·fractional/narrow clip·LTR/RTL·1/9 glyph·명시적 96/144 DPI·투명 PBGRA backdrop·반복 합성을 포함한다.
- 새로운 no-op 검사는 64 RGB palette의 512회 paint를 투명·빈 fractional clip·완전 숨김·마지막 advance NaN 거부별로 실행한다. 6회 교대 측정의 24쌍 모두 초기/최종 BGRA가 같다. surface를 열지 않으며 투명/숨김/빈 clip은 수락, late-invalid는 512회 모두 거부한다. 별도 visible palette 6쌍도 변경 전후 BGRA가 같다.
- 동결 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp 두 개를 변경하고 나머지 455개는 유지했다. 현재 렌더링 executable·소스·환경 freeze를 검사한다.
- 고정 48쌍의 이전 executable/현재 환경 대조와 신규 executable 대조를 통과했다. 전체 **828쌍** decoded pixels·전체 JSON·raw·판정은 v44와 같다. strict 727 + 기존 backend 승인 75 = **802/828**, 기존 화살표 FAIL_PAINT **26건 유지**. 새 예외 0·registry 111개·허용치 0.
- 실제 Windows DPI 96의 WM_PRINTCLIENT 414쌍과 명시적 144 DPI 414쌍을 구분한다. 실제 144 DPI Windows 창 검증으로 계산하지 않는다.
- 현재 원격 세션에서 표시 해상도 1920×1080→2560×1440·refresh 60→32·remoteSession false→true가 변경됐다. GPU/driver/backend/DPI/color는 같으며 frozen v44 executable이 현재 환경에서 고정 48쌍을 재현한 것을 먼저 확인했다. 첫 paint 성능은 두 구현을 동일한 현재 세션에서 직렬 교대 측정했다. 원래 graphics fingerprint를 무시하지 않고 새 환경과 전이 기록을 보존했다.

## 직렬 교대 속도와 할당

아래 값은 같은 측정 본문·같은 입력의 동결 v44/신규 구현을 6회 직렬 교대 실행한 중앙값이다. 초기화가 필요 없었던 경로의 개선율을 앱 전체 개선율로 계산하지 않는다. C++ operator new만 계수하며 DirectWrite DLL 내부의 모든 할당을 뜻하지 않는다.

| 경로 | v44 ms/512회 | v45 ms/512회 | 변화 | 빠른 회차 | 표 할당 v44→v45 |
|---|---:|---:|---:|---:|---|
| transparent | 12.1873 | 0.1321 | -98.92% | 6/6 | 192→0 |
| empty-clip | 11.7314 | 0.0460 | -99.61% | 6/6 | 192→0 |
| hidden | 11.7842 | 0.1297 | -98.90% | 6/6 | 192→0 |
| late-invalid | 11.8626 | 0.1269 | -98.93% | 6/6 | 192→0 |
| visible | 19.8182 | 17.3132 | -12.64% | 6/6 | 192→192 |

첫 구현의 gray #8b8b8b, 9 glyph/full warm 증가는 최초 +9.38%, 전체 12회 +6.64%, 추가 6회 +9.69%로 유지돼 마지막 회귀·보관 진입을 막았다. gamma coverage를 한 번 조회하는 후보는 이를 해결하지 못해 채택하지 않았다. coverage 0의 identity lookup/write를 생략하는 후보는 table·glyph 독립 검사와 6회 3구현 교대 측정에 통과했고 해당 조건을 +3.65%로 줄였다. 첫 구현의 동결 소스·전체 828쌍·앱 첫 6회·추가 warm 원값·두 후보를 resumption/first-implementation과 별도 first-implementation run에 보존한다. 최종 구현을 채택한 후 아래 모든 helper 성능·앱 성능·고정 48쌍·전체 828쌍을 새 소스로 다시 실행했다. 첫 구현 원값을 최종 구현의 표에 섞지 않는다.

warm paint 24조건 × 6회 × 10,000회에서 단일/누적 BGRA 144쌍이 같다. 양쪽 warm C++ 할당 0. 증가한 조건도 아래에 공개한다.

| 경로 | 색상 | glyph | mode/clip | v44 ms | v45 ms | 변화 | 빠른 회차 |
|---|---|---:|---|---:|---:|---:|---:|
| opaque | #8b8b8b | 1 | gray/full | 3.188 | 3.289 | +3.17% | 1/6 |
| opaque | #8b8b8b | 1 | LCD/full | 5.409 | 5.774 | +6.76% | 0/6 |
| opaque | #8b8b8b | 9 | gray/full | 18.270 | 18.898 | +3.44% | 1/6 |
| opaque | #8b8b8b | 9 | LCD/full | 38.843 | 39.323 | +1.24% | 3/6 |
| opaque | #8b8b8b | 32 | gray/full | 64.196 | 62.986 | -1.88% | 3/6 |
| opaque | #8b8b8b | 32 | LCD/full | 132.791 | 133.936 | +0.86% | 2/6 |
| opaque | #2763bd | 1 | gray/full | 3.327 | 3.492 | +4.96% | 2/6 |
| opaque | #2763bd | 1 | LCD/full | 5.490 | 5.795 | +5.55% | 0/6 |
| opaque | #2763bd | 9 | gray/full | 18.209 | 18.598 | +2.14% | 2/6 |
| opaque | #2763bd | 9 | LCD/full | 38.708 | 39.463 | +1.95% | 2/6 |
| opaque | #2763bd | 32 | gray/full | 62.404 | 64.895 | +3.99% | 1/6 |
| opaque | #2763bd | 32 | LCD/full | 131.412 | 133.127 | +1.30% | 1/6 |
| partial | #8b8b8b | 1 | gray/full | 3.254 | 3.108 | -4.48% | 4/6 |
| partial | #8b8b8b | 1 | gray/fractional | 5.731 | 6.081 | +6.10% | 2/6 |
| partial | #8b8b8b | 9 | gray/full | 18.308 | 17.771 | -2.93% | 5/6 |
| partial | #8b8b8b | 9 | gray/fractional | 21.401 | 20.342 | -4.95% | 6/6 |
| partial | #8b8b8b | 32 | gray/full | 62.350 | 61.923 | -0.69% | 4/6 |
| partial | #8b8b8b | 32 | gray/fractional | 67.463 | 64.628 | -4.20% | 6/6 |
| partial | #2763bd | 1 | gray/full | 3.268 | 2.976 | -8.96% | 5/6 |
| partial | #2763bd | 1 | gray/fractional | 7.237 | 7.250 | +0.18% | 2/6 |
| partial | #2763bd | 9 | gray/full | 17.705 | 16.600 | -6.24% | 4/6 |
| partial | #2763bd | 9 | gray/fractional | 21.496 | 20.901 | -2.77% | 4/6 |
| partial | #2763bd | 32 | gray/full | 58.850 | 54.314 | -7.71% | 5/6 |
| partial | #2763bd | 32 | gray/fractional | 67.125 | 63.829 | -4.91% | 6/6 |

짧은 opaque LCD의 최초 +6.76%/+5.55% 증가를 조사했다. 추가 pointer 캐시·기존 table 조기 반환·동일 채널 mutable/immutable pointer 재사용의 네 후보는 일부 회색 경로가 7~18% 느려져 채택하지 않았다. 같은 이전/최종 source control executable 해시를 확인하고 4개 3구현 교대 연구에서 얻은 control 24회도 공개한다. 후보 시간을 아래 최종 source 수치에 섞지 않는다. 추가 control BGRA 288쌍·할당 0, 총 30회와 추가 24회 중앙값을 기록하며 작은 LCD 증가를 성능 한계로 유지한다.

| 색상 | glyph | mode | 최초 6회 변화 | control 전체 30회 변화 | 추가 control 24회 변화 | control 빠른 회차 |
|---|---:|---|---:|---:|---:|---:|
| #8b8b8b | 1 | gray | +3.17% | +2.98% | +2.77% | 6/30 |
| #8b8b8b | 1 | LCD | +6.76% | +5.64% | +4.83% | 4/30 |
| #8b8b8b | 9 | gray | +3.44% | +1.58% | +0.71% | 12/30 |
| #8b8b8b | 9 | LCD | +1.24% | +1.31% | +1.28% | 12/30 |
| #8b8b8b | 32 | gray | -1.88% | -0.66% | +0.01% | 12/30 |
| #8b8b8b | 32 | LCD | +0.86% | +1.64% | +2.01% | 10/30 |
| #2763bd | 1 | gray | +4.96% | +3.46% | +3.17% | 7/30 |
| #2763bd | 1 | LCD | +5.55% | +4.41% | +4.37% | 4/30 |
| #2763bd | 9 | gray | +2.14% | +0.04% | -0.19% | 14/30 |
| #2763bd | 9 | LCD | +1.95% | +1.95% | +1.93% | 9/30 |
| #2763bd | 32 | gray | +3.99% | +0.92% | +0.77% | 10/30 |
| #2763bd | 32 | LCD | +1.30% | +1.73% | +1.97% | 4/30 |

cold 6회 counterbalanced 측정의 첫 8회/다음 1,000회 BGRA 144쌍이 v44·신규·직접 계산 대조군과 같다. 이번 prewarm은 brush color alpha와 brush opacity를 모두 1로 만들어 실제 opaque로 준비했다. v44 prewarm은 opacity만 1, color alpha .6이므로 반투명이었으나 mask를 준비했고 EndDraw 이후 비영 해시도 유효하다. “opaque prewarm”이라는 v44 기록 표현을 여기서 바로잡으며 그 원값·보관본은 유지한다. v43의 hashEight=0 보정은 v44 기록과 v43-cold-hash-erratum.json에 보존되어 있다.

| 색상 | glyph | clip | v44 최초 µs | v45 최초 µs | 첫 8회 변화 | 8번째 변화 |
|---|---:|---|---:|---:|---:|---:|
| #8b8b8b | 1 | full | 78.40 | 70.50 | -0.42% | -0.20% |
| #8b8b8b | 1 | fractional | 1097.60 | 1140.45 | +3.44% | +1.30% |
| #8b8b8b | 9 | full | 82.85 | 72.20 | -5.05% | -3.01% |
| #8b8b8b | 9 | fractional | 99.65 | 106.00 | +2.29% | +2.35% |
| #8b8b8b | 32 | full | 132.35 | 129.85 | -0.70% | +0.38% |
| #8b8b8b | 32 | fractional | 187.95 | 195.90 | +0.58% | +0.03% |
| #2763bd | 1 | full | 46.25 | 43.95 | +3.95% | -2.01% |
| #2763bd | 1 | fractional | 108.00 | 92.55 | -5.89% | -6.17% |
| #2763bd | 9 | full | 69.35 | 69.15 | -6.80% | -8.03% |
| #2763bd | 9 | fractional | 120.30 | 126.15 | -0.92% | -4.41% |
| #2763bd | 32 | full | 122.15 | 121.45 | -1.89% | -2.47% |
| #2763bd | 32 | fractional | 187.70 | 180.15 | -0.63% | -7.17% |

실제 일반 Markdown·250개 표 자산을 각 6회 직렬 교대 측정했다. BGRA 48쌍·layout 24쌍 동일. v44의 일반 첫 paint +7.27% 위험은 당시 v43 대비 수치이며, 아래는 v44 대비 신규 실행기의 독립 측정이다. 서로 다른 측정 회차의 비율을 곱해 누적 개선율로 계산하지 않는다.

| 문서 | 구간 | v44 ms | v45 ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 202.588 | 202.156 | -0.21% | 4/6 |
| normal | firstPaintMs | 113.049 | 117.398 | +3.85% | 1/6 |
| normal | interactiveDownMs | 16.145 | 14.933 | -7.51% | 5/6 |
| normal | interactiveDragMs | 16.874 | 16.537 | -2.00% | 3/6 |
| normal | scrollPaintMs | 20.866 | 21.008 | +0.68% | 1/6 |
| tables | initialLayoutMs | 464.522 | 470.006 | +1.18% | 2/6 |
| tables | firstPaintMs | 114.195 | 100.197 | -12.26% | 4/6 |
| tables | interactiveDownMs | 27.801 | 27.482 | -1.15% | 3/6 |
| tables | interactiveDragMs | 28.602 | 29.200 | +2.09% | 2/6 |
| tables | scrollPaintMs | 32.579 | 32.618 | +0.12% | 3/6 |

## 마지막 전체 회귀·반디집 보관·복원

정확성·성능·전체 캡처 이후 전체 회귀 12개·platform integrity·비교기 오류 주입 182+336건을 통과했다. 반디집 ZIP fast level 1 본 보관본 407,281,417 bytes·복원 증거 44,686,286 bytes의 모든 entry SHA-256을 확인했다. 새 폴더에서 소스 457개·런타임·실행기·입력을 복원하고 828쌍을 새 렌더링해 모든 pixel·전체 진단·raw·판정이 같다. blend/opacity table·gray opacity·axis·gray alpha·clip cache·clip transform·native theme 8개 명령 로그 SHA-256도 같다. 별도 신규 probe도 복원해 초기화 5조건을 512회씩 실행하고 pixel·할당·수락/거부 결과를 대조했다. 현재 live 보호 입력 2,799개를 내부 ZIP에 포함하고 실제 새 폴더에 복원해 해시를 확인했다.

모든 복원 검증 후 생성물 481개·disposable profile cache 60,108개 정리. 소스·현재 live 보호 입력·최신 828쌍·실행기·보관본 유지, 검사 대상 생성물/캐시 0. 최종 문서·정리 기록도 반디집 압축 후 새 폴더 복원·해시 검증한다.

## 남은 범위와 실행 근거

이전 보관본 C:/twf-v24/cleanup-archives/artifact-history.7z는 작업 시작 전부터 없다. 이전 입력 4,544개 실물 가용성·해시는 미확인이며 예전 security-profile HTML 두 개 누락도 유지한다. 이번 live 입력 손실 0.

일반 회전/skew·fractional group/layer·반투명 LCD·color font는 fallback. 전체 HTML/CSS/DOM/paint 계약·실제 Windows 두 DPI/두 모니터·1,000문서는 미완료다. 작성 pilot 문서는 기존 20개이며 calibration을 1,000문서로 세지 않는다. MdViewer 자동 재빌드 없음.

실행: ScrollRenderingRegression.exe --glyph-blend-table. 원값: C:/twf-v45/diagnostics, C:/twf-v45/performance-final. 전체 캡처: C:/twf-v45/runs/20261006-glyph-init-v45-final. 보관본: C:/twf-v45/archives.

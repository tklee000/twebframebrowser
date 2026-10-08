# v46 반투명 native 아이콘 합성표 전수 검증·최초 생성 비용 결과

2026-10-06. Windows x64 Release. 전체 회귀·반디집 보관·새 폴더 복원·828쌍 새 렌더링·10개 독립 검사 로그 대조 완료.

native 아이콘의 source-over는 합산 후 한 번 반올림한다. coverage별 source 기여와 inverse alpha를 미리 계산하고 RGB·alpha plane에서 같은 backdrop 기여를 공유했다. 생성 값이 비음수이며 255 이하인 루프에서 lround의 양수 반올림을 +0.5 뒤 정수 변환으로 계산해 CRT 호출을 제거했다. 기존 direct 합성식·8번째 paint부터 활성화하는 정책·RGB/float opacity 키·256 KiB 캐시·glyph mask·clip·일반 문서 텍스트 규칙은 유지했다.

## 독립 정확성 검증

- 7 RGB palette × 14 opacity × coverage 256 × backdrop 256 × 4 plane = **25,690,112항목**. binary32 opacity의 exponent/significand를 정수 유리수로 해석해 정확한 single source-over 반올림을 계산했다. 매우 작은 opacity는 최대 채널 변화가 1/2 미만이므로 원래 backdrop byte를 기대한다. 구현의 double 계산·lround를 oracle에서 재사용하지 않는다.
- 동결 v45·신규·draw lookup을 끈 대조군 모두 실패 0. 각 1,043건의 7회 direct/8회 활성화·정확한 인접 float 키·ready 재사용·교체·복귀 검사 실패 0. 전체 byte signature 5885979846701051615 동일, 캐시 payload 262,144 bytes 유지.
- foreground 전체 256개·backdrop 6개·coverage 1~255와 도달 가능한 모든 half-byte 경계의 바로 아래/해당/바로 위 float opacity **62,816,640건**. 원래 CRT 반올림과 빠른 변환이 정확한 정수 oracle과 일치한다. alpha plane의 다른 평가 순서도 포함한다. 이 경계 검사는 표 항목 수와 별도 집계하며 모든 가능한 float 조합을 전수검사했다고 주장하지 않는다.
- 복사 header에 separate point rounding·opacity 8-bit 양자화 오류를 주입해 각각 2,464,561개·929,821개 잘못된 항목을 탐지하고 exit 1로 거부했다. production header는 변경하지 않았다.
- 실제 native glyph의 6회 교대 측정에서 warm BGRA 72쌍·cold/direct 대조 144쌍이 같다. full/fractional clip·회색/컬러·1/9/32 glyph·처음 8회/다음 1,000회를 포함한다. 모든 hash는 EndDraw 이후 비영 값이다. warm C++ operator new 할당은 양쪽 0이며 DirectWrite 내부 전체 할당 계수는 아니다.
- 기존 ordinary opacity oracle 18,350,080항목·glyph blend 39,845,888항목·독립 일반 glyph 13,824프레임·거부 304건·복귀 288건·no-op 24쌍·visible 6쌍도 통과했다. 일반 glyph warm/cold 각각 144쌍이 같다.
- 전체 828쌍 decoded pixels·전체 JSON·raw·판정은 v45와 같다. strict 727 + 기존 backend 승인 75 = **802/828**, 기존 화살표 FAIL_PAINT **26건 유지**. 원본 입력·registry 111개·허용치 0·새 예외 0.
- 동결 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp 두 개를 변경, 나머지 455개 유지. 같은 현 display 환경의 frozen v45 실행기 고정 48쌍과 신규 48쌍을 모두 확인했다.
- 실제 Windows DPI 96 WM_PRINTCLIENT 414쌍과 명시적 144 DPI 414쌍은 구분한다. 후자를 실제 Windows 144 DPI 창 검사로 세지 않는다.

## 직렬 교대 속도

같은 측정 본문·같은 입력으로 6회 before/after, after/before 직렬 교대했다. 아래는 각 조건의 median이며 증가값도 보존한다. 표 생성은 42조건 × 회당 8재생성 × 6회 = 구현별 2,016회, 모든 table hash 252쌍 동일. 생성 이후 모든 byte를 확인하는 시간은 타이밍에서 제외했다. 이 연산의 개선율을 앱 전체 속도 개선율로 계산하지 않는다.

| 색 그룹 | 최초 생성 변화 중앙값 | 조건별 변화 범위 |
|---|---:|---:|
| 회색 | -67.96% | -69.90% ~ -65.58% |
| 컬러 | -69.83% | -72.45% ~ -68.80% |

첫 계산 공유 후보는 생성 비용 감소가 작아 채택하지 않았다. 최초 원값·해시가 일치하는 header를 diagnostics/shared-only에 보존했다. 아래와 위 표의 원값은 최종 소스의 독립 실행이며 후보 값과 섞지 않는다.

| 색 | glyph | clip | native warm 변화 | 8번째 paint 변화 | 다음 1,000회 변화 |
|---|---:|---|---:|---:|---:|
| #8b8b8b | 1 | full | +5.32% | -68.02% | +15.79% |
| #8b8b8b | 1 | fractional | +1.48% | -67.21% | +4.40% |
| #8b8b8b | 9 | full | -1.14% | -67.03% | +2.88% |
| #8b8b8b | 9 | fractional | +1.39% | -67.07% | +1.21% |
| #8b8b8b | 32 | full | +0.17% | -66.54% | +3.39% |
| #8b8b8b | 32 | fractional | +3.41% | -66.47% | +3.15% |
| #2763bd | 1 | full | -0.02% | -70.82% | -0.70% |
| #2763bd | 1 | fractional | +1.95% | -69.36% | +1.25% |
| #2763bd | 9 | full | +0.15% | -68.24% | +6.47% |
| #2763bd | 9 | fractional | -0.44% | -69.40% | +3.59% |
| #2763bd | 32 | full | +5.79% | -69.19% | +0.86% |
| #2763bd | 32 | fractional | +6.08% | -69.03% | -1.13% |

초기 native warm 3조건의 +5~6% 증가를 추가 6회 역순 교대로 확인했다. 초기 원값을 보존하고 72쌍의 BGRA 일치·할당 0을 추가 확인했다. 아래는 같은 소스의 전체 12회와 추가 6회이며 최초 결과를 대체하지 않는다.

| 색 | glyph | clip | 최초 6회 변화 | 전체 12회 변화 | 추가 6회 변화 | 전체 빠른 회차 |
|---|---:|---|---:|---:|---:|---:|
| #8b8b8b | 1 | full | +5.32% | +3.22% | +0.88% | 4/12 |
| #8b8b8b | 1 | fractional | +1.48% | +1.65% | +2.17% | 5/12 |
| #8b8b8b | 9 | full | -1.14% | -1.29% | -3.51% | 8/12 |
| #8b8b8b | 9 | fractional | +1.39% | +0.72% | -3.61% | 7/12 |
| #8b8b8b | 32 | full | +0.17% | +0.30% | +2.17% | 7/12 |
| #8b8b8b | 32 | fractional | +3.41% | -0.13% | -3.77% | 7/12 |
| #2763bd | 1 | full | -0.02% | +1.96% | +2.15% | 4/12 |
| #2763bd | 1 | fractional | +1.95% | +1.95% | +2.58% | 2/12 |
| #2763bd | 9 | full | +0.15% | -3.69% | -5.49% | 8/12 |
| #2763bd | 9 | fractional | -0.44% | -0.01% | +0.21% | 7/12 |
| #2763bd | 32 | full | +5.79% | +4.08% | +3.55% | 1/12 |
| #2763bd | 32 | fractional | +6.08% | -0.39% | -1.14% | 5/12 |

일반 glyph warm 측정의 감소·증가도 공개한다. 일반 text의 코드는 이번 최적화 대상과 분리돼 있으며 작은 변동을 개선으로 단정하지 않는다.

| 구간 | 색 | glyph | mode/clip | 변화 | 빠른 회차 |
|---|---|---:|---|---:|---:|
| opaque | #8b8b8b | 1 | gray/full | -2.14% | 6/6 |
| opaque | #8b8b8b | 1 | LCD/full | -2.55% | 4/6 |
| opaque | #8b8b8b | 9 | gray/full | -2.60% | 4/6 |
| opaque | #8b8b8b | 9 | LCD/full | -0.95% | 3/6 |
| opaque | #8b8b8b | 32 | gray/full | -0.15% | 4/6 |
| opaque | #8b8b8b | 32 | LCD/full | +1.02% | 3/6 |
| opaque | #2763bd | 1 | gray/full | -1.21% | 3/6 |
| opaque | #2763bd | 1 | LCD/full | -3.58% | 5/6 |
| opaque | #2763bd | 9 | gray/full | -0.97% | 5/6 |
| opaque | #2763bd | 9 | LCD/full | -0.15% | 4/6 |
| opaque | #2763bd | 32 | gray/full | +0.10% | 3/6 |
| opaque | #2763bd | 32 | LCD/full | -0.20% | 3/6 |
| partial | #8b8b8b | 1 | gray/full | -0.11% | 4/6 |
| partial | #8b8b8b | 1 | gray/fractional | -1.28% | 3/6 |
| partial | #8b8b8b | 9 | gray/full | +1.55% | 2/6 |
| partial | #8b8b8b | 9 | gray/fractional | -2.99% | 3/6 |
| partial | #8b8b8b | 32 | gray/full | -0.53% | 4/6 |
| partial | #8b8b8b | 32 | gray/fractional | +0.59% | 3/6 |
| partial | #2763bd | 1 | gray/full | +2.12% | 0/6 |
| partial | #2763bd | 1 | gray/fractional | -0.93% | 3/6 |
| partial | #2763bd | 9 | gray/full | +3.38% | 2/6 |
| partial | #2763bd | 9 | gray/fractional | +4.04% | 1/6 |
| partial | #2763bd | 32 | gray/full | +2.45% | 3/6 |
| partial | #2763bd | 32 | gray/fractional | +5.62% | 1/6 |

실제 일반 Markdown·250개 표 자산을 각 6회 측정했다. BGRA 48쌍·layout 24쌍 동일. v45의 기존 위험은 과거 v44 대비 수치이며 아래는 v45 대비 이번 독립 실행이다. 이전 실행 비율과 곱해 누적 개선율을 계산하지 않는다.

| 문서 | 구간 | v45 ms | v46 ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 204.703 | 211.837 | +3.48% | 1/6 |
| normal | firstPaintMs | 121.840 | 123.844 | +1.64% | 3/6 |
| normal | interactiveDownMs | 16.108 | 15.219 | -5.52% | 4/6 |
| normal | interactiveDragMs | 16.909 | 16.565 | -2.04% | 2/6 |
| normal | scrollPaintMs | 20.881 | 20.773 | -0.51% | 2/6 |
| tables | initialLayoutMs | 472.411 | 482.274 | +2.09% | 1/6 |
| tables | firstPaintMs | 112.709 | 112.679 | -0.03% | 2/6 |
| tables | interactiveDownMs | 26.817 | 26.410 | -1.52% | 3/6 |
| tables | interactiveDragMs | 29.176 | 29.703 | +1.81% | 3/6 |
| tables | scrollPaintMs | 33.032 | 33.313 | +0.85% | 2/6 |

추가 성능 검토 원값: C:/twf-v46/diagnostics/warm-performance-review.json. 초기 샘플을 유지하며 추가 회차와 전체 중앙값을 함께 기록한다.

## 마지막 전체 회귀·반디집 보관·복원

정확성·성능·전체 캡처 이후 전체 회귀 12개·platform integrity·비교기 오류 주입 182+336건 통과. 반디집 ZIP fast level 1 본 보관본 211,729,092 bytes·복원 증거 44,681,622 bytes의 모든 entry SHA-256 확인. 새 폴더에서 소스 457개·런타임·입력·실행기를 복원하고 828쌍을 다시 렌더링해 pixel·전체 진단·raw·판정 일치. native opacity table/브러시 opacity를 포함한 10개 검사 로그 SHA-256 일치. 복원 probe의 초기화 5조건도 각 512회 실행해 pixel·할당·수락/거부 동작이 같다. 보호 live 입력 2,799개를 실제 새 폴더에 복원해 해시 확인.

복원 완료 뒤 생성물 350개·profile cache 39,196개 정리. 현재 live 입력·소스·828쌍·실행기·보관본 유지. 최종 문서·정리 기록도 별도 반디집 압축 후 새 폴더 복원·모든 entry 해시 검증한다.

## 남은 범위

기존 화살표 실패 26건은 유지한다. 일반 회전/skew·fractional group/layer·반투명 LCD·color font는 fallback. 실제 Windows 두 DPI·두 모니터·전체 HTML/CSS/DOM/paint 계약·1,000문서는 미완료다. 작성 pilot 문서는 기존 20개다.

이전 C:/twf-v24/cleanup-archives/artifact-history.7z는 작업 시작 전부터 없다. 과거 입력 4,544개 실물 가용성·해시 미확인·예전 security-profile HTML 2개 누락은 이번 완료 범위로 세지 않는다. 이번 live 입력 손실 0. MdViewer 자동 재빌드 없음.

실행: ScrollRenderingRegression.exe --native-opacity-table, --glyph-opacity. 원값: C:/twf-v46/diagnostics, C:/twf-v46/performance-final. 전체 캡처: C:/twf-v46/runs/20261006-native-opacity-v46-final. 보관본: C:/twf-v46/archives.

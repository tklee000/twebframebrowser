# v47 반투명 합성표 캐시 상태·조회 비용 결과

2026-10-06 시작. 완료 2026-10-07. Windows x64 Release. 전체 회귀·반디집 보관·새 폴더 복원·828쌍 새 렌더링·11개 독립 검사 로그 대조 완료.

캐시 상태와 스레드 격리의 독립 검사로 정확성 검증 공백을 보완했다. 조회/최초 생성 분리·inline·메모리 정렬의 속도 후보 네 개는 조회 microbenchmark가 빨라도 실제 glyph 페인트 증가를 해소하지 못해 채택하지 않았다. production RasterSurface.h를 동결 v46과 byte·SHA-256가 같도록 복귀했으며 이번 production 변경은 ScrollRenderingRegression.cpp의 새 정확성 검사다. native single source-over 반올림·ordinary source/backdrop 별도 반올림·정확한 RGB/float 키·7회 direct/8회 활성화·종류별 스레드별 256 KiB payload를 유지한다.

## 정확성·캐시 상태

- 동결 v46 소스 457개·실행기·동일 성능 자산·고정 48쌍을 해시 확인 후 작업했다. ScrollRenderingRegression.cpp 한 파일 변경, 나머지 456개 유지. production 렌더러 자체는 v46과 같다.
- native/ordinary × 각 스레드 16키 × 3스레드 × coverage 256 × backdrop 256 × 4plane = **25,165,824 byte 대조/구현**. 인접 binary32 opacity·한 foreground byte 변경·준비 직전 키 중단·복귀·10,000회 ready 재사용·7회/8회 경계를 확인했다. native는 정수 유리수 oracle, ordinary는 IEEE float 독립 반올림 oracle이다.
- 동결·신규·draw lookup을 끈 대조군 모두 오류 0. 상태 검사 962,448건/구현, ready 반복을 포함하며 독립 사례 수로 세지 않는다. byte signature 13554990255478011647 일치. 실제 두 worker가 표를 바꾸는 동안 main 표의 모든 byte·포인터·repetition을 확인한 격리 검사 4건 통과. 각 cache 종류의 다른 저장소도 확인했다.
- 복사 header의 7번째 조기 활성화·foreground 세 번째 byte를 키에서 누락하는 오류를 주입해 각각 960,480건·96건의 상태 오류를 탐지하고 exit 1로 거부했다. production header는 오류 주입으로 변경하지 않았다.
- 기존 native 25,690,112항목·키 1,043건·인접 float 반올림 경계 62,816,640건, ordinary 18,350,080항목·키 763건, glyph blend 39,845,888항목·키 1,024건 모두 오류 0. native separate rounding/opacity 양자화 오류 주입도 거부했다.
- 독립 실제 일반 glyph 13,824프레임·guard 304건·복귀 288건 통과. native warm 72쌍+cold/direct 144쌍, 일반 warm/cold 각각 144쌍, no-op 24쌍+visible 6쌍 BGRA 일치. native 추가 warm 72쌍도 일치하며 측정한 warm C++ operator new 할당은 0이다. DirectWrite 내부 전체 할당을 계수했다고 주장하지 않는다.
- 전체 **828쌍**의 decoded pixels·전체 DOM/style/geometry JSON·raw·판정이 v46과 같다. strict 727 + 기존 backend 승인 75 = **802/828**, 기존 화살표 FAIL_PAINT **26건 유지**. 입력·허용치 0·registry 111개·새 예외 0.
- 실제 Windows DPI 96 WM_PRINTCLIENT 414쌍과 명시적 144 DPI 414쌍을 구분한다. 실제 Windows 144 DPI 창·두 모니터 검증은 이번 완료 범위가 아니다.

## 속도 검증

동일 본문·입력으로 6회 직렬 교대했다. 표 조회는 noinline caller에서 캐시를 호출하고 값을 읽어 반복 루프 밖으로 이동하는 최적화를 막았다. 조건당 2,000,000회 × 24조건 × 6회 = 구현별 288,000,000회이며 144 checksum 쌍이 같다. 단순 조회 비용을 실제 앱 개선율로 환산하지 않는다.

| 캐시 | 경로 | 변화 중앙값 | 조건별 범위 |
|---|---|---:|---:|
| native | ready | +0.96% | -10.81% ~ +3.93% |
| native | 매번 키 교체/direct | +1.22% | -3.10% ~ +7.55% |
| ordinary | ready | +1.08% | -13.75% ~ +5.95% |
| ordinary | 매번 키 교체/direct | -6.10% | -9.59% ~ -1.18% |

키 교체 경로에서는 표를 생성하지 않는다. 아래 native 표 최초 생성은 별도 42조건 × 회당 8재생성 × 6회 = 구현별 2,016회이며 252개 full-table hash 쌍 일치. hash 확인 시간은 타이밍에서 제외했다.

| 색 그룹 | 최초 생성 변화 중앙값 | 조건별 범위 |
|---|---:|---:|
| 회색 | +0.09% | -1.18% ~ +4.39% |
| 컬러 | +0.01% | -1.56% ~ +3.16% |

이전 v46 대비 실제 native glyph 반복 페인트를 최초 6회와 추가 6회로 대조했다. 최초 원값을 유지하고 전체 12회·추가 6회를 함께 공개한다. 지속적인 5% 초과 증가 판정은 전체/추가 중앙값 모두 +5% 초과이고 빠른 회차가 각각 2/12 이하·1/6 이하인 경우다. 증가값을 별도로 보존하며 판정 문턱 아래 증가도 표에 싣는다.

| 색 | glyph | clip | 최초 6회 | 전체 12회 | 추가 6회 | 전체 빠른 회차 | 8번째 paint 변화 |
|---|---:|---|---:|---:|---:|---:|---:|
| #8b8b8b | 1 | full | -0.35% | -0.35% | +0.48% | 8/12 | +0.00% |
| #8b8b8b | 1 | fractional | +7.41% | -0.11% | -1.43% | 5/12 | +0.50% |
| #8b8b8b | 9 | full | +1.64% | +2.27% | +2.34% | 4/12 | +0.45% |
| #8b8b8b | 9 | fractional | +5.31% | +0.91% | -0.32% | 3/12 | +1.27% |
| #8b8b8b | 32 | full | -0.99% | +0.68% | +0.43% | 6/12 | -0.83% |
| #8b8b8b | 32 | fractional | +0.84% | +0.21% | +0.15% | 8/12 | +0.22% |
| #2763bd | 1 | full | -2.17% | -1.43% | -0.95% | 9/12 | -0.14% |
| #2763bd | 1 | fractional | -4.09% | -0.96% | +0.26% | 6/12 | -0.10% |
| #2763bd | 9 | full | -1.64% | -0.94% | -0.50% | 6/12 | +0.19% |
| #2763bd | 9 | fractional | -1.48% | -0.34% | -2.00% | 6/12 | +0.07% |
| #2763bd | 32 | full | +0.06% | +1.08% | +2.61% | 5/12 | -0.02% |
| #2763bd | 32 | fractional | -3.61% | -0.11% | -0.31% | 6/12 | +0.38% |

일반 glyph warm도 같은 조건으로 확인했다.

| 구간 | 색 | glyph | mode/clip | 변화 | 빠른 회차 |
|---|---|---:|---|---:|---:|
| opaque | #8b8b8b | 1 | gray/full | +0.59% | 3/6 |
| opaque | #8b8b8b | 1 | LCD/full | -0.61% | 4/6 |
| opaque | #8b8b8b | 9 | gray/full | +0.60% | 3/6 |
| opaque | #8b8b8b | 9 | LCD/full | -1.04% | 4/6 |
| opaque | #8b8b8b | 32 | gray/full | +0.97% | 4/6 |
| opaque | #8b8b8b | 32 | LCD/full | +3.31% | 0/6 |
| opaque | #2763bd | 1 | gray/full | -2.33% | 4/6 |
| opaque | #2763bd | 1 | LCD/full | -2.43% | 6/6 |
| opaque | #2763bd | 9 | gray/full | -3.62% | 4/6 |
| opaque | #2763bd | 9 | LCD/full | -2.52% | 4/6 |
| opaque | #2763bd | 32 | gray/full | -1.41% | 5/6 |
| opaque | #2763bd | 32 | LCD/full | -7.55% | 5/6 |
| partial | #8b8b8b | 1 | gray/full | +1.15% | 1/6 |
| partial | #8b8b8b | 1 | gray/fractional | +2.94% | 1/6 |
| partial | #8b8b8b | 9 | gray/full | +16.03% | 0/6 |
| partial | #8b8b8b | 9 | gray/fractional | -1.74% | 4/6 |
| partial | #8b8b8b | 32 | gray/full | +4.56% | 3/6 |
| partial | #8b8b8b | 32 | gray/fractional | -3.29% | 4/6 |
| partial | #2763bd | 1 | gray/full | -8.85% | 4/6 |
| partial | #2763bd | 1 | gray/fractional | -0.95% | 3/6 |
| partial | #2763bd | 9 | gray/full | -1.33% | 5/6 |
| partial | #2763bd | 9 | gray/fractional | -0.56% | 3/6 |
| partial | #2763bd | 32 | gray/full | -2.11% | 4/6 |
| partial | #2763bd | 32 | gray/fractional | +0.96% | 3/6 |

실제 일반 Markdown·250개 표 자산 각 12회. BGRA 96쌍·layout 48쌍 일치. 아래는 이번 실행의 v46 대비 수치이며 과거 비율과 곱하지 않는다.

| 문서 | 구간 | v46 ms | v47 ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 212.588 | 207.550 | -2.37% | 5/12 |
| normal | firstPaintMs | 132.298 | 115.048 | -13.04% | 10/12 |
| normal | interactiveDownMs | 15.327 | 15.322 | -0.04% | 8/12 |
| normal | interactiveDragMs | 16.587 | 16.246 | -2.06% | 7/12 |
| normal | scrollPaintMs | 20.723 | 20.761 | +0.18% | 6/12 |
| tables | initialLayoutMs | 481.208 | 481.761 | +0.11% | 6/12 |
| tables | firstPaintMs | 101.814 | 108.995 | +7.05% | 6/12 |
| tables | interactiveDownMs | 26.800 | 26.492 | -1.15% | 6/12 |
| tables | interactiveDragMs | 28.091 | 27.931 | -0.57% | 6/12 |
| tables | scrollPaintMs | 32.777 | 32.826 | +0.15% | 4/12 |

추가 성능 검토: C:/twf-v47/diagnostics/warm-performance-review.json. 최초 샘플을 보존했고 증가·감소 원값 모두 포함한다.

| 일반 glyph 조건 | 최초 6회 | 전체 12회 | 추가 6회 |
|---|---:|---:|---:|
| #8b8b8b/1/0 | +1.15% | -0.31% | -0.95% |
| #8b8b8b/1/1 | +2.94% | +2.86% | +0.58% |
| #8b8b8b/9/0 | +16.03% | -0.52% | -5.13% |
| #8b8b8b/9/1 | -1.74% | -0.16% | -0.36% |
| #8b8b8b/32/0 | +4.56% | +8.49% | +9.17% |
| #8b8b8b/32/1 | -3.29% | -2.63% | -0.55% |
| #2763bd/1/0 | -8.85% | -2.82% | -0.70% |
| #2763bd/1/1 | -0.95% | +4.35% | +5.12% |
| #2763bd/9/0 | -1.33% | -3.59% | -3.88% |
| #2763bd/9/1 | -0.56% | -1.70% | -0.00% |
| #2763bd/32/0 | -2.11% | -0.14% | +0.75% |
| #2763bd/32/1 | +0.96% | -0.28% | -1.35% |

추가 성능 검토: C:/twf-v47/diagnostics/app-performance-review.json. 최초 샘플을 보존했고 증가·감소 원값 모두 포함한다.

## 마지막 전체 회귀·반디집 보관·복원

최초 inline 후보는 native fractional clip 반복 페인트가 전체 12회 +7.94%, 추가 6회 +9.31%여서 제외했다. noinline 분리 후보도 회색 native/ordinary run에서 지속적인 증가를 보여 제외했다. native-only inline·64-byte 정렬 후보의 3회 screening 역시 증가를 해소하지 못했다. C:/twf-v47/candidates 및 trials에 정확한 후보 header·진단·실행기·원값을 보존했다. 초기화와 일반 extra warm 측정이 잠시 겹친 첫 후보 샘플은 유효 성능 근거에서 제외했다. 위 표는 v46 renderer를 byte 그대로 복귀한 뒤 모든 검사를 직렬로 새로 실행한 결과이며 후보 샘플과 합치지 않는다. 이번 릴리스의 속도 개선을 주장하지 않는다.

정확성·성능·전체 캡처를 마친 뒤 전체 회귀 12개·platform integrity·비교기 오류 주입 182+336건 통과. 반디집 ZIP fast level 1 본 보관본 330,388,634 bytes·복원 증거 65,720,717 bytes의 모든 entry SHA-256 확인. 새 폴더에서 소스 457개·런타임·입력·실행기를 복원하고 **828쌍을 새로 렌더링**해 pixel·전체 진단·raw·판정 일치. 새 cache 상태 검사를 포함한 **11개 검사 로그 SHA-256 일치**. 복원 probe 초기화 5조건 각 512회도 pixel·할당·guard가 같다. 보호 live 입력 2,799개를 실제 새 폴더에 복원해 해시 확인.

복원 중 504쌍 이후 디스플레이 ID·주사율 32→60 Hz·선택적 DRIVER_VENDOR 표기로 fingerprint가 바뀌어 중단했다. GPU·드라이버·DPI·색공간·화면 크기는 같다. 새 환경에서 frozen v46의 48쌍 pixel·전체 JSON·raw·판정 일치를 먼저 확인했다. 원본 보관 환경·복원 소스·측정/비교기·registry·허용치를 변경하지 않고 별도 재개 드라이버에서 검증된 두 fingerprint만 허용했다. 이미 새로 렌더링한 완료 matrix는 새 비교 폴더에서 모든 byte/진단/판정을 다시 확인했고 부분 matrix는 재캡처했다. 최종 828쌍은 실제 복원 실행기로 생성한 결과이며 두 환경의 관측 fingerprint와 재개 provenance·frozen 48쌍 전체 증거를 복원 증거 ZIP에 포함했다.

복원 완료 뒤 생성물 548개·profile cache 40,149개 정리. 현재 live 입력·소스·828쌍·실행기·보관본 유지. 최종 문서·정리 기록도 별도 반디집 압축 후 새 폴더 복원·모든 entry 해시 검증한다.

## 남은 범위

기존 화살표 실패 26건은 유지한다. 일반 회전/skew·fractional group/layer·반투명 LCD·color font는 fallback. 실제 Windows 두 DPI·두 모니터·전체 HTML/CSS/DOM/paint 계약·1,000문서는 미완료다. 작성 pilot 문서는 기존 20개다.

이전 C:/twf-v24/cleanup-archives/artifact-history.7z는 작업 시작 전부터 없다. 과거 입력 4,544개 실물·해시 미확인 및 예전 security-profile HTML 2개 누락은 이번 완료 범위로 세지 않는다. 이번 live 입력 손실 0. MdViewer 자동 재빌드 없음.

실행: ScrollRenderingRegression.exe --opacity-cache-state. 원값: C:/twf-v47/diagnostics, C:/twf-v47/performance-final. 전체 캡처: C:/twf-v47/runs/20261006-opacity-cache-v47-final. 보관본: C:/twf-v47/archives.

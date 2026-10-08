# v41 일반 회색조 텍스트 알파·채널 재사용 결과

2026-10-06. Windows x64 Release. 전체 회귀·반디집 보관·새 폴더 복원·828쌍 새 렌더링·새/기존 검사 로그 대조 완료.

- 독립 DirectWrite LCD mask→회색조와 gamma/source-over 검사 2,048건. 변경 전 알파 실패 1,152건, 수정 후 실패 0. PBGRA 위반 0.
- Arial/Segoe UI·96/144 명시적 렌더링 배율·네 quarter phase·검정/흰색/회색/색상·네 backdrop alpha·full/fractional/narrow/empty clip·1/2회 중첩 paint를 검사했다.
- 일반 회색조 텍스트가 투명 backdrop의 알파를 무조건 255로 덮어쓰던 부분을 교정했다. 완전 clip은 정수 source-over, 소수 clip은 기존 RGB와 같은 source/backdrop 개별 point rounding을 알파에도 적용한다. LCD의 opaque 정책과 native의 단일 rounding 정책을 유지한다.
- 완전 clip의 회색 글자는 RGB가 같은 backdrop에서 한 번의 blend로 세 채널을 처리한다. 색상 회색조도 한 coverage 조회와 직접 BGRA lookup을 사용한다. texture/cache key·허용치·registry는 바꾸지 않았다.
- 기존 828쌍에서 수집된 이미지·전체 JSON·raw·판정은 v40과 같다. strict 727·기존 승인 75, 총 802/828. 화살표 FAIL_PAINT 26 유지. 새 예외 0·registry 111개·허용치 0.
- 새 검사와 clip cache·capsule precision·glyph 배율/좁은 clip/opacity·capsule 방향/경계·glyph clip transform·native theme 검사를 유지했다.
- v40 동결 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp만 수정했고 나머지 455개 해시는 같다. 최종 source freeze가 focused 캡처·성능 probe·전체 캡처의 동일 소스를 확인한다.
- reference 두 경로와 native snapshot은 828쌍에 수집한다. 실제 Windows DPI가 96이므로 WM_PRINTCLIENT는 96 DPI의 414쌍에서만 수집하고, 명시적 144 DPI의 414쌍은 window-route.json에 실제 창 경로 미검증 사유를 보존한다. 실제 Windows 144 DPI 검증으로 해석하지 않는다.

## 6회 직렬 교대 성능

3회 before/after와 3회 after/before를 직렬로 측정한다. 12조건·10,000회 페인트. 단일/반복 BGRA hash 72쌍·할당 수가 일치한다. 기존 ordinary mask vector 할당은 남아 있으며 할당 제거를 주장하지 않는다. 시간 증감은 측정 조건별로 기록한다.

| 색상 | glyph 수 | 모드 | before ms | after ms | 변화 | 빠른 회차 |
|---|---:|---|---:|---:|---:|---:|
| #8b8b8b | 1 | grayscale | 5.712 | 3.603 | -36.93% | 6/6 |
| #8b8b8b | 1 | LCD | 5.378 | 5.420 | +0.78% | 2/6 |
| #8b8b8b | 8 | grayscale | 35.010 | 20.248 | -42.16% | 6/6 |
| #8b8b8b | 8 | LCD | 37.222 | 34.370 | -7.66% | 4/6 |
| #8b8b8b | 32 | grayscale | 133.225 | 76.873 | -42.30% | 6/6 |
| #8b8b8b | 32 | LCD | 132.834 | 133.622 | +0.59% | 3/6 |
| #2763bd | 1 | grayscale | 5.577 | 3.275 | -41.28% | 6/6 |
| #2763bd | 1 | LCD | 5.594 | 5.455 | -2.48% | 3/6 |
| #2763bd | 8 | grayscale | 35.194 | 16.613 | -52.80% | 6/6 |
| #2763bd | 8 | LCD | 34.036 | 34.762 | +2.13% | 1/6 |
| #2763bd | 32 | grayscale | 132.619 | 63.038 | -52.47% | 6/6 |
| #2763bd | 32 | LCD | 132.744 | 132.526 | -0.16% | 4/6 |

일반 Markdown과 250개 표를 담은 동일 앱 자산을 각 6회 교대 측정했다. 앱 BGRA 48쌍·layout 24쌍은 같다. helper 개선율을 앱 전체 개선율로 해석하지 않는다.

| 문서 | 구간 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 209.740 | 210.960 | +0.58% | 2/6 |
| normal | firstPaintMs | 118.555 | 117.059 | -1.26% | 4/6 |
| normal | interactiveDownMs | 15.443 | 15.147 | -1.91% | 2/6 |
| normal | interactiveDragMs | 17.466 | 16.930 | -3.07% | 3/6 |
| normal | scrollPaintMs | 21.259 | 21.622 | +1.71% | 2/6 |
| tables | initialLayoutMs | 479.243 | 478.221 | -0.21% | 3/6 |
| tables | firstPaintMs | 110.315 | 112.221 | +1.73% | 3/6 |
| tables | interactiveDownMs | 25.538 | 27.051 | +5.92% | 1/6 |
| tables | interactiveDragMs | 28.006 | 28.119 | +0.40% | 3/6 |
| tables | scrollPaintMs | 32.452 | 32.422 | -0.09% | 3/6 |

## 마지막 회귀·반디집 보관·복원

정확성·성능 이후 전체 회귀 12개와 platform integrity가 통과했다. 비교기 오류 주입 182+336건도 통과했다. 반디집 ZIP fast level 1 본 보관본 193,978,643 bytes·복원 증거 44,570,096 bytes, 모든 entry SHA-256을 확인했다. 새 폴더에서 소스 457개·실행기·runtime·입력을 복원하고 828쌍을 새로 렌더링해 수집된 이미지·전체 진단·raw·판정 일치를 확인했다. 새 --gray-text-alpha 및 기존 아홉 native 명령의 로그 SHA-256이 같다. 현재 live 보호 입력 2,799개를 내부 ZIP에 보존하고 새 폴더로 실제 복원해 모든 해시를 검사했다.

첫 성능 구현에서 색상 회색조 8/32 glyph 시간이 증가해 coverage 조회를 채널마다 반복하지 않도록 수정했다. 최초 측정·중간 헤더·실행기는 diagnostics/gray-attempt-1에 보존한다. 두 번째 측정은 시작 시 focused 검사와 겹쳐 diagnostics/gray-attempt-2에 따로 보존하고, 모든 focused 검사가 끝난 뒤 최종 수치를 직렬로 재측정했다.

이전 보호 보관본 C:/twf-v24/cleanup-archives/artifact-history.7z는 작업 시작 전부터 없다. 그 안의 4,544개 입력 현재 가용성·실물 해시는 확인하지 못했다. 예전 security-profile HTML 두 파일 누락도 유지한다. 이번 작업의 live 입력 손실은 0이다. 과거 보호 입력 전체 보관 검증을 완료로 표시하지 않는다.

전체 HTML/CSS/DOM/paint 계약·일반 affine·fractional group/layer·실제 Windows 두 DPI/두 모니터·1,000문서는 미완료다. MdViewer 자동 재빌드 없음.

원값과 보관본: C:/twf-v41/runs/20261006-gray-alpha-v41-final, C:/twf-v41/archives. 새 검사는 ScrollRenderingRegression.exe --gray-text-alpha로 재검사한다.

복원 검증 이후 생성물 317개·disposable profile cache 39,184개를 정리했다. 소스·입력·최신 828쌍·diff·검사 실행기·반디집 보관본을 유지하며 남은 검사 대상 생성물/캐시는 0이다. 최종 문서와 정리 기록은 별도 v41-final-records.zip으로 반디집 보관하고 새 폴더 실물·SHA-256을 검증한다.

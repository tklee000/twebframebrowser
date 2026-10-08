# v42 일반 텍스트 대각 축·guard·run 저장소 결과

2026-10-06. Windows x64 Release. 전체 회귀·반디집 보관·새 폴더 복원·828쌍 새 렌더링·새/기존 검사 로그 대조 완료.

- 독립 DirectWrite byte-backed font LCD mask·회색조 변환·gamma/source-over oracle로 대각 축 픽셀 6,912건·거부 guard 288건·거부 이후 저장소 복귀 288건을 검사했다. 수정 후 실패는 모두 0이다.
- 수정 전 대각 축 경로 미지원 5,376건·받아들이면 안 되는 미세 기울임 48건·거부 검사의 변경된 backdrop이 후속 paint에 남은 복귀 불일치 32건을 보존한다. 미지원은 기존 DrawGlyphRun false/fallback이므로 모두 기존 화면 픽셀 오류로 해석하지 않는다.
- Arial/Segoe UI·명시적 96/144 DPI·LCD/회색조·9개 signed diagonal axes·네 quarter phase·회색/색상·full/fractional/narrow clip·LTR/RTL·glyph offset·혼합 glyph run을 검사했다. 회색조는 반투명 PBGRA backdrop, LCD는 opaque backdrop을 사용한다.
- 전체 transform의 두 off-diagonal 항이 정확히 0인 경우에만 CPU 경로를 사용한다. 미세한 기울임을 무시하던 tolerance를 제거했다. 일반 텍스트도 두 signed axes와 실제 비율을 mask에 적용해 비균일 배율·반전을 지원한다. mask의 signed em·horizontal ratio 키는 기존 구조를 사용한다.
- 1~8 glyph는 inline storage, 9~256 glyph는 thread-local bounded capacity를 재사용한다. 정상·조기 실패 모두 RAII로 texture reference를 비운다. 257개 이상은 local storage를 유지한다. 1/8/9/32/256/257 경계에서 NaN advance/offset/em·불가 transform 거부와 다음 valid run의 독립 픽셀 복귀를 검사했다.
- reference mask는 production helper를 사용하지 않는다. 원본 font file bytes를 별도 loader로 직접 읽어 face를 만들었다. DirectWrite system collection과 byte-backed face의 hinted A edge가 다를 수 있어 독립 font-loading 경로를 명시한다. 혼합 glyph 검사에서 이 차이를 확인한 중간 진단은 보존한다.
- 기존 828쌍 이미지·전체 JSON·raw·판정은 v41과 같다. strict 727·기존 승인 75, 총 802/828. 화살표 FAIL_PAINT 26 유지. 새 예외 0·registry 111개·허용치 0.
- v41 동결 소스 457개 중 RasterSurface.h·ScrollRenderingRegression.cpp만 수정했다. 나머지 455개 해시는 같다. focused 캡처·성능 probe·전체 캡처의 동일 소스를 확인한다.
- 실제 Windows DPI는 96이다. reference 두 경로/native snapshot은 828쌍, WM_PRINTCLIENT는 실제 96 DPI의 414쌍에서 수집한다. 명시적 144 DPI의 414쌍은 실제 창 경로 미검증 사유를 보존한다.

## 6회 직렬 교대 성능

3회 before/after·3회 after/before를 다른 검사와 겹치지 않게 직렬 측정했다. 24조건·각 10,000회 paint의 단일/반복 BGRA hash 144쌍이 같다. 1~256 glyph의 warm paint 할당은 회차당 10,000→0, 257 glyph는 기존 10,000회 할당을 유지한다. 이 수치는 C++ operator new 계수이며 DirectWrite 내부의 모든 메모리 할당을 의미하지 않는다.

| 색상 | glyph 수 | 모드 | before ms | after ms | 변화 | 빠른 회차 | before/after 할당 |
|---|---:|---|---:|---:|---:|---:|---:|
| #8b8b8b | 1 | grayscale | 3.582 | 3.258 | -9.04% | 4/6 | 10000/0 |
| #8b8b8b | 1 | LCD | 5.682 | 5.371 | -5.49% | 5/6 | 10000/0 |
| #8b8b8b | 8 | grayscale | 19.496 | 17.206 | -11.75% | 6/6 | 10000/0 |
| #8b8b8b | 8 | LCD | 35.742 | 34.927 | -2.28% | 3/6 | 10000/0 |
| #8b8b8b | 9 | grayscale | 21.980 | 19.108 | -13.07% | 6/6 | 10000/0 |
| #8b8b8b | 9 | LCD | 39.910 | 38.368 | -3.86% | 4/6 | 10000/0 |
| #8b8b8b | 32 | grayscale | 74.333 | 64.147 | -13.70% | 6/6 | 10000/0 |
| #8b8b8b | 32 | LCD | 133.978 | 138.155 | +3.12% | 2/6 | 10000/0 |
| #8b8b8b | 256 | grayscale | 131.599 | 116.160 | -11.73% | 6/6 | 10000/0 |
| #8b8b8b | 256 | LCD | 205.910 | 205.388 | -0.25% | 3/6 | 10000/0 |
| #8b8b8b | 257 | grayscale | 129.839 | 119.734 | -7.78% | 6/6 | 10000/10000 |
| #8b8b8b | 257 | LCD | 207.263 | 210.728 | +1.67% | 2/6 | 10000/10000 |
| #2763bd | 1 | grayscale | 3.554 | 3.621 | +1.89% | 4/6 | 10000/0 |
| #2763bd | 1 | LCD | 6.065 | 5.510 | -9.14% | 4/6 | 10000/0 |
| #2763bd | 8 | grayscale | 18.153 | 17.138 | -5.59% | 4/6 | 10000/0 |
| #2763bd | 8 | LCD | 34.838 | 35.981 | +3.28% | 2/6 | 10000/0 |
| #2763bd | 9 | grayscale | 18.971 | 18.303 | -3.52% | 6/6 | 10000/0 |
| #2763bd | 9 | LCD | 39.248 | 38.447 | -2.04% | 5/6 | 10000/0 |
| #2763bd | 32 | grayscale | 64.505 | 62.842 | -2.58% | 4/6 | 10000/0 |
| #2763bd | 32 | LCD | 137.424 | 136.775 | -0.47% | 3/6 | 10000/0 |
| #2763bd | 256 | grayscale | 117.578 | 114.856 | -2.31% | 5/6 | 10000/0 |
| #2763bd | 256 | LCD | 208.965 | 205.437 | -1.69% | 3/6 | 10000/0 |
| #2763bd | 257 | grayscale | 118.302 | 114.773 | -2.98% | 6/6 | 10000/10000 |
| #2763bd | 257 | LCD | 203.884 | 210.237 | +3.12% | 1/6 | 10000/10000 |

일반 Markdown과 250개 표를 담은 동일 앱 자산을 각 12회 교대 측정했다. 앱 BGRA 96쌍·layout 48쌍은 같다. 증가한 구간도 기록하며 helper 결과를 앱 전체 개선율로 해석하지 않는다.

| 문서 | 구간 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 213.140 | 212.435 | -0.33% | 5/12 |
| normal | firstPaintMs | 126.655 | 130.500 | +3.04% | 6/12 |
| normal | interactiveDownMs | 15.768 | 15.572 | -1.24% | 6/12 |
| normal | interactiveDragMs | 16.894 | 17.052 | +0.94% | 6/12 |
| normal | scrollPaintMs | 21.169 | 20.519 | -3.07% | 10/12 |
| tables | initialLayoutMs | 486.366 | 477.857 | -1.75% | 10/12 |
| tables | firstPaintMs | 111.126 | 110.417 | -0.64% | 6/12 |
| tables | interactiveDownMs | 26.365 | 26.882 | +1.96% | 5/12 |
| tables | interactiveDragMs | 28.549 | 28.695 | +0.51% | 7/12 |
| tables | scrollPaintMs | 32.924 | 32.570 | -1.08% | 8/12 |

최초 6회에서 최초 페인트 시간이 normal +17.80%·tables +10.44%여서 전체 캡처가 끝난 뒤 다른 검사와 겹치지 않게 6회 더 측정했다. 원래 6회와 추가 6회를 모두 보존하고, 위 표는 12회 전체 표본을 합산한다. 첫 표본을 버리거나 유리한 회차를 골라 쓰지 않는다.

첫 전체 회귀에서 기존 native 검사에 있던 ordinary nonuniform fallback 가정과 후속 상태 비교 12개가 실패했다. 이제 지원하는 diagonal transform을 거부해야 한다는 오래된 가정이었다. 해당 guard를 미세 shear transform 거부 검사로 바꿨으며 새 독립 oracle이 diagonal 지원을 검증한다. production header·renderer executable·성능 probe는 그대로다. 실패 로그·수정 전 environment·source snapshot을 보존하고, 변경된 단일 테스트 hash와 반디집 snapshot을 다시 동결한 뒤 전체 회귀를 재실행했다.

## 마지막 회귀·반디집 보관·복원

정확성·성능 이후 전체 회귀 12개와 platform integrity가 통과했다. 전체 ScrollRenderingRegression은 기존 native 대형 축/clip/opacity/capsule 검사를 포함한다. 비교기 오류 주입 182+336건도 통과했다. 반디집 ZIP fast level 1 본 보관본 214,211,262 bytes·복원 증거 44,573,688 bytes의 모든 entry SHA-256을 확인했다. 새 폴더에서 소스 457개·실행기·runtime·입력을 복원하고 828쌍을 새로 렌더링해 이미지·전체 진단·raw·판정 일치를 확인했다. 새 --text-axis 및 기존 gray alpha/clip cache/clip transform/native theme 명령의 로그 SHA-256이 같다. 현재 live 보호 입력 2,799개를 내부 ZIP에 보존하고 새 폴더로 실제 복원해 모든 해시를 검사했다.

이전 보호 보관본 C:/twf-v24/cleanup-archives/artifact-history.7z는 작업 시작 전부터 없다. 그 안의 4,544개 입력 가용성·실물 해시를 확인하지 못했다. 예전 security-profile HTML 두 파일 누락도 유지한다. 이번 작업의 live 입력 손실은 0이다.

일반 회전/skew·fractional group/layer·부분 opacity·color font는 fallback을 유지한다. 이번 검사는 RasterSurface의 일반 텍스트 API 계약을 개선한 범위다. 전체 HTML/CSS/DOM/paint 계약·실제 Windows 두 DPI/두 모니터·1,000문서는 미완료다. MdViewer 자동 재빌드 없음.

원값과 보관본: C:/twf-v42/runs/20261006-text-axis-v42-final, C:/twf-v42/archives. 새 검사는 ScrollRenderingRegression.exe --text-axis로 재검사한다.

복원 이후 생성물 317개·disposable profile cache 39,142개를 정리했다. 소스·live 보호 입력·최신 828쌍·diff·실행기·보관본을 유지하며 남은 검사 대상 생성물/캐시는 0이다. 최종 문서·정리 기록은 v42-final-records.zip에 반디집 보관하고 새 폴더 실물·SHA-256을 검증한다.

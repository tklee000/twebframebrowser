# v29 스크롤바 곡선·합성·페인트 비용 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 새 렌더링 검증 완료.

- 고정 8문서 × 2 배율 × 3 viewport: 구조 48/48, strict 22/48 → 22/48. 원본 400px 표와 HTML/CSS·reference 픽셀·전체 native/reference 진단을 유지했다.
- 총 802/828 성공, strict 727·기존 backend 승인 75·미등록 FAIL_PAINT 26. 기존 780쌍의 네 decoded RGBA 이미지·전체 진단·raw 수치·판정 불변.
- raw 픽셀 차이 합계 1,638 → 182(88.89% 감소). 기존 성공 22쌍 유지, 기존 실패 26쌍 모두 raw 감소. 새 예외 0, registry 111개·채널 허용치 0 유지.
- 기본 비교기 182개와 스크롤바 오류 주입 336/336 통과. 새 스타일 3,000개·CSSOM 기하 480개, 누락 0.

## 정확성 수정과 남은 차이

기본 Fluent 수평 thumb의 원형 끝에 거리 coverage를 사용하고, premultiplied ink와 alpha를 양자화한 뒤 현재 backdrop에 CPU 합성한다. 엔진은 캡처 PNG나 문서 ID를 읽지 않는다. 양 끝 mask는 두께만으로 결정하고 bounded cache(32개·1 MiB)에서 재사용하며 위치·클립·색상은 현재 값을 사용한다. 정수 device 경계·uniform scale·opaque solid brush에만 적용한다. 소수 경계·회전·layer·일반 custom scrollbar에는 기존 렌더링 경로를 사용한다.

근거는 [Chromium NativeThemeFluent](https://chromium.googlesource.com/chromium/src/+/main/ui/native_theme/native_theme_fluent.cc)의 원형 thumb와 [Skia CircleGeometryProcessor](https://skia.googlesource.com/skia/+/9fb7fa537d938618991922bfffa627e442db67f0/src/gpu/ops/GrOvalOpFactory.cpp)의 거리 coverage다. independent probe에서 96/144 DPI의 thumb 끝은 이 CPU 계산과 고정 reference가 일치했다. GPU 합성의 양자화 순서는 probe 결과에 따른 추론이며 모든 GPU의 동일 결과를 보장하지 않는다. 원문·probe·해시는 work evidence에 보존한다.

native icon은 source-over 합산 후 한 번 반올림하고 일반 문서 글꼴은 기존 point rounding을 유지한다. **잔여 26쌍은 여전히 미등록 FAIL_PAINT다.** 96 DPI 각 6픽셀·최대 채널 차이 1; 144 DPI 각 8픽셀·최대 채널 차이 1. 잔여 차이는 화살표에 국한되고 새 예외로 숨기지 않는다. 전체 계약 성공이나 1,000문서 완료로 표시하지 않는다.

## 속도와 변경·복귀 검증

합성표 최초 생성의 부동소수점 곱셈·lround를 정수 산술로 바꿨다. 일반 텍스트의 23,068,672개 색·coverage·backdrop 조합에서 이전 함수와 완전히 같다. cold table 생성은 before/after 3회와 after/before 3회씩 같은 프로세스에서 서로 다른 미사용 색으로 측정했고, 18개 table의 모든 byte가 같았다. 아래 median ms는 해당 연산만의 비용이다.

| 합성표 | before ms | after ms | 변화 |
|---|---:|---:|---:|
| 5 bit | 0.0220 | 0.0072 | -67.43% |
| 6 bit | 0.0449 | 0.0151 | -66.33% |
| 8 bit | 0.1854 | 0.0575 | -68.98% |

thumb와 두 화살표를 같은 WIC lock에 묶고 최외곽 batch에서 Direct2D 상태를 복원한다. fallback 전에 lock을 해제해 target 사용 가능 상태를 유지한다. 두 DPI에서 실제 CPU WIC와 DC의 drag·소수 scroll·부분 repaint·fresh layout 일치, none/thin/color/크기·pseudo stylesheet 변경·복귀를 검증했다. primitive 검사에서는 클립 밖 보존·색과 두께 cache 무효화·중첩 batch·소수/회전 fallback을 확인했다.

동일한 보존 앱 자산·일반 Markdown과 250개 표 문서를 6회씩 직렬 교대 측정했다. BGRA 48쌍·layout JSON 24쌍 불변. 아래는 median ms다. 개별 원값·paired 변화·IQR도 보존하며 증가한 단계도 그대로 기록한다. 작은 차이를 모든 문서의 속도 향상으로 단정하지 않는다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 210.980 | 211.207 | +0.11% | 3/6 |
| normal | firstPaintMs | 109.097 | 106.932 | -1.98% | 3/6 |
| normal | interactiveDownMs | 14.936 | 14.604 | -2.22% | 3/6 |
| normal | interactiveDragMs | 16.446 | 16.728 | +1.72% | 3/6 |
| normal | scrollPaintMs | 20.066 | 20.581 | +2.57% | 2/6 |
| tables | initialLayoutMs | 482.646 | 479.981 | -0.55% | 4/6 |
| tables | firstPaintMs | 96.630 | 90.428 | -6.42% | 5/6 |
| tables | interactiveDownMs | 26.639 | 26.331 | -1.15% | 5/6 |
| tables | interactiveDragMs | 28.126 | 28.215 | +0.32% | 2/6 |
| tables | scrollPaintMs | 31.761 | 31.447 | -0.99% | 4/6 |

## 검증 순서와 보관

정확성·기존 780쌍 불변·비교기 오류 주입·교대 성능 측정 뒤 마지막에 전체 회귀 12개와 platform integrity를 실행했다. 그 뒤 반디집 ZIP fast level 1로 보관하고, 새 폴더의 파일 SHA-256·복원 비교기의 120쌍 재판정·복원 실행기의 전체 828쌍 새 렌더링을 검증했다. 잔여 실패의 raw 수치와 실패 판정까지 같아야 한다.

증거: `C:/twf-v29/runs/20261005-scrollbar-v29-final`, `C:/twf-v29/archives`. 실제 Windows DPI는 96이며 144 검사는 renderer 배율 1.5다. 실제 Windows 두 DPI·독립 화면 캡처·전체 HTML/CSS/문자 기하 계약·1,000문서·원격 영구 보관은 미완료다.

## 마지막 정리

복원 검증 뒤 해시가 일치하는 보관 사본 77,626개, 컴파일 생성물 295개와 캐시 39,008개를 제거했다. 보호 입력과 최신 828쌍의 PNG·전체 JSON·실패 diff·최소 증거·반디집 불변 보관본을 유지한다. 생성물 0개·캐시 0개, artifacts 29.57 MiB. 삭제 목록은 반디집 정리 기록 ZIP에 보존했다.

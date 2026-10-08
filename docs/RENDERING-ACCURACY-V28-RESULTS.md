# v28 기본 수평 스크롤바·DPI·반복 비용 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 재렌더링 검증 완료.

- 새 고정 8문서 × 2 DPI × 3 viewport: 구조 48/48, strict 22/48 → 22/48. 원본 400px 표 두 개와 최소 thumb·소수 너비 입력을 변경하지 않았다.
- 총 802/828 성공, strict 727·기존 backend 승인 75·미등록 FAIL_PAINT 26. 기존 780쌍의 decoded RGBA 네 이미지·전체 native/reference JSON·raw 수치·판정은 모두 같다.
- 새 입력의 픽셀 차이 합계 23,968 → 1,638(93.17% 감소). 구조/스타일/기하 차이와 새 승인 예외는 0이다. 기존 registry 111개·채널 허용치 0은 그대로다.
- 기본 비교기 182개와 새 오류 주입 336/336 검증. 새 계산 스타일 3,000개·CSSOM 기하 480개, 누락 0.

## 공통 수정과 남은 차이

Fluent 기본 수평 스크롤바의 화살표는 Segoe Fluent Icons의 U+EDD9/U+EDDA를 사용한다. font face와 glyph index를 한 번 조회하고, DPI에 따른 버튼·glyph 크기와 위치를 계산한다. thumb·track 두께, 최소 길이, scroll extent와 thumb 위치는 물리 픽셀에서 양자화하고 CSS 레이아웃·스크롤 범위는 CSS 좌표를 유지한다. native icon은 별도의 DirectWrite 회색조 hinting을 사용하며 일반 문서 글꼴 경로는 유지한다.

근거는 변경 전에 고정한 WebView2 154.0.4258.53 독립 캡처와 [Chromium NativeThemeFluent](https://chromium.googlesource.com/chromium/src/+/main/ui/native_theme/native_theme_fluent.cc), [ScrollbarThemeFluent](https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/core/scroll/scrollbar_theme_fluent.cc)이다. 저장한 원문·probe·해시는 work evidence에 보존한다. 엔진은 기준 PNG를 읽거나 문서 ID에 따라 분기하지 않는다.

두 DPI에서 스크롤/drag 최대 범위·소수 offset·fresh layout과 incremental paint 일치, 크기/none/thin/color 변경 및 복귀, pseudo stylesheet 추가·제거와 복귀를 검증했다. 기존 parser·표 크기 결과는 그대로다.

**픽셀 정확성은 미완료다.** 스크롤바가 있는 새 26쌍은 96 DPI에서 각 44픽셀(max delta 10), 144 DPI에서 각 82픽셀(max delta 15)의 미등록 FAIL_PAINT다. 화살표의 회색조 합성 반올림과 thumb 둥근 끝의 CPU/GPU 안티앨리어싱에 차이가 남는다. 원본 400px 표 4쌍도 실패로 유지한다. 새 예외/허용치를 추가하지 않고 실패의 원본 이미지·진단·raw 수치·diff를 보존했다. 802/828은 전체 계약 성공을 의미하지 않는다.

## 반복 비용과 실제 앱 성능

같은 computed style의 scrollbar-width/color 분류를 기존 bounded style cache에서 재사용한다. 기본 scrollbar에는 빈 pseudo ComputedStyle 두 개를 매 조회마다 할당하지 않는다. custom pseudo rule 존재와 크기는 매번 현재 stylesheet와 layout에서 읽으며, 스타일 재계산/stylesheet 변경/viewport 변경은 기존 소유권·무효화 경로를 따른다.

동일한 보존 Markdown 앱 자산·일반 문서·250개 표 문서로 before/after 3회와 after/before 3회씩 직렬 실행했다. BGRA 48쌍·layout JSON 24쌍 불변. 아래는 median ms이며 증가도 기록한다. 원값, paired 변화와 IQR은 performance summary에 보존한다. 반복 조회/할당을 줄인 사실과 실제 측정 결과를 구분하며 작은 변화를 모든 문서의 속도 향상으로 단정하지 않는다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 210.109 | 208.428 | -0.80% | 4/6 |
| normal | firstPaintMs | 108.584 | 111.310 | +2.51% | 3/6 |
| normal | interactiveDownMs | 15.012 | 14.743 | -1.79% | 3/6 |
| normal | interactiveDragMs | 16.231 | 16.341 | +0.68% | 2/6 |
| normal | scrollPaintMs | 20.352 | 20.085 | -1.32% | 3/6 |
| tables | initialLayoutMs | 484.077 | 489.159 | +1.05% | 3/6 |
| tables | firstPaintMs | 91.742 | 95.126 | +3.69% | 2/6 |
| tables | interactiveDownMs | 26.041 | 26.035 | -0.02% | 3/6 |
| tables | interactiveDragMs | 27.473 | 28.500 | +3.74% | 2/6 |
| tables | scrollPaintMs | 31.509 | 31.522 | +0.04% | 1/6 |

normal 최초 페인트 median 변화 +2.727ms, before/after IQR 20.018/7.764ms, after가 빠른 회차 3/6 · tables 최초 페인트 median 변화 +3.384ms, before/after IQR 1.721/4.398ms, after가 빠른 회차 2/6. 최초 페인트의 증가를 숨기지 않으며 후속 profiling 대상으로 유지한다. 캐시로 제거한 조회/할당만으로 모든 앱 단계가 빨라졌다고 주장하지 않는다.

## 검증 순서와 보관

정확성·기존 780쌍 불변·오류 주입·교대 성능 측정 이후 마지막에 전체 회귀 12개와 platform integrity를 실행했다. 그 다음 반디집 ZIP fast level 1 보관, 새 폴더에 복원하여 모든 파일 SHA-256 확인, 복원 비교기로 120쌍 재판정, 복원 실행기로 전체 828쌍 새 렌더링과 픽셀·전체 진단·raw 판정 일치를 확인했다. 실패 26쌍의 실패 판정도 복원 후 같아야 한다.

증거: `C:/twf-v28/runs/20261005-scrollbar-v28-final`, `C:/twf-v28/archives`. 실제 Windows DPI 96; 144 검사는 renderer 배율 1.5다. 실제 Windows 두 DPI·독립 화면 캡처·전체 HTML/CSS/문자 기하 계약·1,000문서·원격 영구 보관은 미완료다.

## 회귀·복원 이후 정리

각 경로와 SHA-256을 확인한 보관 사본 78,773개, 컴파일 생성물 295개, 렌더링 캐시 41,242개를 제거했다. 최신 828쌍의 PNG·전체 JSON·실패 diff, 보호 입력과 최소 기록, 반디집 불변 보관본을 유지한다. 컴파일 생성물 0개, 렌더링 캐시 0개, artifacts 29.57 MiB다. 상세 삭제 목록과 복원 검증은 정리 기록 ZIP에 보존한다.

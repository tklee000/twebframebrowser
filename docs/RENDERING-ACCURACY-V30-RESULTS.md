# v30 글리프 투명도·소수 클립·반복 페인트 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 새 렌더링 검증 완료.

- native glyph 경계 192프레임: v29 성공 32/192 → v30 192/192. 같은 font face의 raw mask를 독립 DirectWrite 분석으로 읽고 double source-over로 기대값을 계산했다. 색 2개·배경 알파 4개·클립 3개·run 길이 4개·배율 2개이며 이전 경로 실패 160개도 보존했다.
- 고정 48쌍: 구조 48/48·strict 22/48 유지. 기존 780쌍의 네 decoded RGBA 이미지·전체 native/reference JSON·raw 수치·판정 불변. 고정 48쌍의 raw 픽셀 합계도 182 → 182로 유지했다.
- 총 802/828 성공, strict 727·기존 backend 승인 75·미등록 FAIL_PAINT 26. 새 예외 0, registry 111개·채널 허용치 0 유지.
- 비교기 기본 182개·스크롤바 오류 주입 336/336. 고정 입력과 원본 400px 표·reference 픽셀·전체 진단 유지.

## 정확성 수정과 잔여 차이

native 회색조 글리프가 투명·반투명 PBGRA 표면의 기존 알파를 보존하며 source-over로 갱신한다. 이전 경로는 coverage 0인 마스크 픽셀도 알파 255로 덮었다. 소수 클립은 ink/backdrop을 따로 반올림하던 계산을 합산 후 한 번 반올림하도록 교정했다. clipped coverage를 byte 단위에서 계산해 반올림 경계의 불필요한 float 오차도 줄였다. 일반 문서 글꼴의 기존 합성은 유지한다. native icon에 LCD 모드를 요청하면 기존 fallback으로 넘긴다.

소수 클립의 면적 coverage와 [Skia 감마표 원문](https://raw.githubusercontent.com/google/skia/main/src/core/SkMaskGamma.cpp)에 따른 같은 감마 입력을 독립 검사에 사용했다. 192프레임의 모든 BGRA byte·클립 밖 보존·premultiplied 채널 범위·target 상태 복원을 확인했다. 이전 경로로 동일 검사를 실행하면 160프레임이 실패한다. 일반 텍스트의 기존 point rounding은 23,068,672조합에서 유지한다.

**기존 화살표 26쌍의 최대 채널 차이 1은 남아 있다.** 96 배율 각 6픽셀·144 배율 각 8픽셀이다. 감마 대표 색과 같은 adapter의 별도 D3D11 source-over probe를 확인했으나 잔여 차이를 충분히 설명하지 못했다. 임의 감마 조정·픽셀별 보정·예외 추가는 하지 않았다. GPU probe는 조사 증거다. 엔진은 GPU 업로드·동기 readback·reference PNG 읽기를 추가하지 않았다.

## 반복 비용과 앱 성능

8개 이하 native glyph run은 stack mask 배열을 사용해 매 페인트의 임시 heap 할당을 제거했다. 일반 문서 글꼴과 긴 run은 기존 reserve/push 경로를 유지한다. 사용하지 않는 inline 배열을 일반 텍스트마다 초기화·해제하지 않도록 했다. 마스크와 font face의 수명·위치·색·클립·cache 무효화는 유지한다. opaque native glyph는 기존 알파 255를 재계산하지 않는다. 회색 native icon은 감마를 한 번 조회하고, 회색 backdrop은 세 채널의 같은 합성 결과를 재사용한다.

warm paint 20,000회를 원본/수정 probe로 직렬 교대 6회 측정했다. 3회 before-after, 3회 after-before. 아래는 median ms와 C++ heap 할당 수이며 opaque 최종 픽셀 해시 30쌍이 같다. 원값과 paired 변화도 보존했다. 해당 작은 WIC surface의 연산 비용이며 증가 수치도 그대로 기록한다. 최초 수정안의 +6~15% 비용 증가도 attempt-1에 보존하고, 채널 재사용 뒤 최종 수치를 다시 측정했다.

| glyph 수 | before ms | after ms | 변화 | 임시 할당 | 빠른 회차 |
|---|---:|---:|---:|---:|---:|
| 1 | 5.376 | 4.319 | -19.66% | 20,000 → 0 | 5/6 |
| 4 | 14.969 | 9.703 | -35.18% | 20,000 → 0 | 6/6 |
| 8 | 23.374 | 14.347 | -38.62% | 20,000 → 0 | 6/6 |
| 9 | 23.134 | 14.933 | -35.45% | 20,000 → 20,000 | 6/6 |
| 32 | 30.412 | 22.680 | -25.43% | 20,000 → 20,000 | 6/6 |

실제 CPU WIC/DC의 두 배율에서 scroll·drag·부분 repaint·fresh layout, none/thin/color/크기·pseudo stylesheet 변경과 복귀를 검증했다. 최초 앱 측정에서 일반 클릭 +7.35%·drag +9.61% 증가를 발견해 일반 텍스트 경로의 inline 초기화를 제거했고, 이전 원값은 performance-attempt-1에 보존했다. 같은 보존 앱 자산의 일반 Markdown과 250개 표 문서도 각 6회 직렬 교대 측정했다. BGRA 48쌍·layout JSON 24쌍 불변. 증가한 단계도 그대로 기록한다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 204.893 | 203.690 | -0.59% | 4/6 |
| normal | firstPaintMs | 103.723 | 103.721 | -0.00% | 2/6 |
| normal | interactiveDownMs | 15.190 | 15.072 | -0.77% | 4/6 |
| normal | interactiveDragMs | 15.817 | 16.286 | +2.97% | 2/6 |
| normal | scrollPaintMs | 20.350 | 20.178 | -0.84% | 4/6 |
| tables | initialLayoutMs | 482.403 | 481.432 | -0.20% | 2/6 |
| tables | firstPaintMs | 89.281 | 86.383 | -3.25% | 5/6 |
| tables | interactiveDownMs | 26.453 | 26.883 | +1.62% | 2/6 |
| tables | interactiveDragMs | 28.470 | 28.300 | -0.60% | 3/6 |
| tables | scrollPaintMs | 31.152 | 31.091 | -0.20% | 4/6 |

## 마지막 검증과 보관

정확성·기존 캡처·비교기·교대 속도 측정 이후 마지막에 전체 회귀 12개·platform integrity를 실행했다. 그 뒤 반디집 ZIP fast level 1로 보관하고 새 폴더에서 모든 파일 SHA-256·복원 비교기 120쌍 재판정·복원 실행기 828쌍의 새 렌더링·192프레임 glyph/scroll 재검사를 검증했다. 잔여 실패의 raw 수치·판정도 일치해야 한다.

증거: `C:/twf-v30/runs/20261005-scrollbar-v30-final`, `C:/twf-v30/archives`. 실제 Windows DPI 96, 144 검사는 renderer 배율 1.5다. 전체 HTML/CSS/DOM·paint 계약·실제 Windows 두 DPI·1,000문서는 미완료다.

## 마지막 정리

복원 뒤 해시가 일치하는 보관 사본 109,585개, 컴파일 생성물 295개와 캐시 60,066개를 정리했다. 보호 입력과 최신 828쌍의 PNG·JSON·실패 diff·최소 증거·반디집 보관본을 유지한다. 생성물 0개·캐시 0개, artifacts 29.57 MiB.

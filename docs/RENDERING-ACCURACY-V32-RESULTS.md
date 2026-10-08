# v32 native 캡슐 투명도·클립·합성 재사용 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 새 렌더링과 native 경계·상태 복원 검증 완료.

- native 캡슐 지원·경계 검사 16,384건: v31 실패 12,544 → v32 실패 0. 13,056건은 CPU native 경로(투명 brush의 무변경 처리 포함), 3,328건은 fractional AA clip의 기존 fallback·무변경 검증이다.
- 같은 검사를 수정 전/후 헤더로 실행해 이전 실패를 보존했다. 기존 native glyph 192+3,072건·clip/layer/DPI 상태 복원·일반 텍스트 23,068,672조합·스크롤/드래그/스타일 복귀도 통과했다.
- 기존 828쌍의 네 decoded RGBA 이미지·전체 native/reference JSON·raw 수치·판정이 v31과 일치했다. 총 802/828 성공(strict 727·기존 backend 승인 75), 미등록 FAIL_PAINT 26 유지.
- 비교기 기본 182개·스크롤바 오류 주입 336/336 통과. 원본 400px 표·허용치 0·기존 예외 registry 111개 유지, 새 예외 0.

## 정확성 수정

native 캡슐이 saved clip의 반전·회전·기울임과 소수 aliased 경계를 공통 device clip cache로 처리한다. aliased 경계는 픽셀 중심 포함을 사용하며 Direct2D의 별도 WIC fill 결과로 확인했다. 폭 0 authored clip도 회전/기울임 후 축 정렬 bounding box에 면적이 생길 수 있음을 같은 독립 oracle로 확인하고, 변환 전에 무조건 비우던 경로를 교정했다. clip push 시점의 변환된 축 정렬 경계를 사용한다는 규칙은 [Microsoft transform 문서](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-transforms-overview)와 [PushAxisAlignedClip 문서](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/nf-d2d1-id2d1rendertarget-pushaxisalignedclip%28constd2d1_rect_f__d2d1_antialias_mode%29)를 따른다.

brush color alpha와 brush opacity를 곱하고, coverage와 결합한 premultiplied ink·alpha를 byte로 양자화한 뒤 기존 PBGRA backdrop에 source-over한다. 완전 투명 brush는 픽셀을 보존한다. 독립 double signed-distance 기하·byte premultiplication·UNORM 합성과 전체 픽셀을 비교했다. 색 2개×brush alpha/opacity 4개×backdrop alpha 4개×두께 4개×clip 4개×saved transform 8개×AA 모드 2개×배율 2개, 총 16,384건이다. cache나 엔진 capsule 함수를 기대값 계산에 사용하지 않는다. 모든 픽셀의 premultiplied 채널 범위·클립 밖 보존·target 복귀도 확인한다.

fractional AA clip에는 group 합성이 필요하므로 기존 Direct2D fallback을 유지한다. cache에 binary 여부를 함께 저장하며 clip stack·DPI·표면 크기 변경에 따라 경계와 이 여부를 무효화한다. capsule 자체의 fractional geometry·회전/비균일 transform·layer는 기존 fallback 범위다.

**기존 화살표 미등록 FAIL_PAINT 26쌍의 채널 값 1 차이는 남아 있다.** 고정 48쌍의 raw 차이 합계 182픽셀·strict 22/48·구조 48/48을 유지했다. 이번 캡슐 경계 개선과 기존 화살표 차이는 구분한다.

## 반복 비용과 실제 앱 성능

두께별 signed-distance mask에 현재 RGB·effective opacity의 premultiplied edge/center ink를 저장한다. 색·불투명도가 바뀌면 다시 계산하며 위치·길이·clip·backdrop은 live 입력이다. coverage와 ink를 합쳐 저장 캐시 32개/1 MiB 제한을 유지한다. 매 edge pixel의 세 채널 float 반올림을 반복하지 않고 byte 합성만 수행하며, alpha 0은 건너뛰고 alpha 255는 그대로 복사한다.

작은 WIC 표면의 warm capsule paint 20,000회를 6회 직렬 교대(3회 before-after, 3회 after-before) 측정했다. 아래 median은 이 연산의 비용이다. 12개 조건의 감소율은 39.16~85.41%이며 최종 픽셀 해시 72쌍과 임시 heap 할당 0회를 유지했다. 원값·paired 변화·실행기와 최종 소스 SHA-256을 보존했다.

| clip 깊이 | 길이 px | 두께 px | before ms | after ms | 변화 | 빠른 회차 |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 16 | 5 | 9.006 | 3.282 | -63.56% | 6/6 |
| 0 | 16 | 13 | 45.131 | 6.852 | -84.82% | 6/6 |
| 0 | 96 | 5 | 16.781 | 10.209 | -39.16% | 6/6 |
| 0 | 96 | 13 | 68.466 | 24.204 | -64.65% | 6/6 |
| 2 | 16 | 5 | 9.537 | 3.438 | -63.95% | 6/6 |
| 2 | 16 | 13 | 47.553 | 6.939 | -85.41% | 6/6 |
| 2 | 96 | 5 | 17.472 | 10.400 | -40.48% | 6/6 |
| 2 | 96 | 13 | 65.035 | 24.557 | -62.24% | 6/6 |
| 8 | 16 | 5 | 11.485 | 3.501 | -69.52% | 6/6 |
| 8 | 16 | 13 | 47.983 | 7.191 | -85.01% | 6/6 |
| 8 | 96 | 5 | 19.050 | 10.471 | -45.04% | 6/6 |
| 8 | 96 | 13 | 67.708 | 24.460 | -63.87% | 6/6 |

같은 보존 앱 자산의 일반 Markdown과 250개 표 문서를 각각 6회 직렬 교대 측정했다. BGRA 48쌍과 layout JSON 24쌍이 같다. 증가한 단계도 아래에 기록하며 작은 capsule probe의 감소율을 앱 전체의 감소율로 해석하지 않는다. 모든 원값·IQR·paired 변화와 실행기 hash를 보존한다.

최초 앱 측정에서는 일반 문서 first paint +11.69%·drag +9.27%를 관측했다. AA clip의 binary 판정을 일반 glyph에서도 계산하던 경로를 capsule 요청 때만 계산·캐시하도록 조정한 뒤 최종 소스로 다시 측정했다. 최초 원값·실행기·소스·부분 캡처는 attempt-1에 보존한다. 비용 분리가 코드에서 확인되지만 측정 변동의 전체 원인을 단정하지 않는다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 209.185 | 208.634 | -0.26% | 4/6 |
| normal | firstPaintMs | 104.438 | 106.602 | +2.07% | 2/6 |
| normal | interactiveDownMs | 15.095 | 15.721 | +4.15% | 2/6 |
| normal | interactiveDragMs | 17.073 | 16.974 | -0.58% | 4/6 |
| normal | scrollPaintMs | 20.631 | 20.820 | +0.91% | 3/6 |
| tables | initialLayoutMs | 478.368 | 494.125 | +3.29% | 0/6 |
| tables | firstPaintMs | 91.217 | 90.443 | -0.85% | 4/6 |
| tables | interactiveDownMs | 25.858 | 26.850 | +3.84% | 1/6 |
| tables | interactiveDragMs | 27.959 | 28.803 | +3.02% | 0/6 |
| tables | scrollPaintMs | 31.951 | 32.331 | +1.19% | 2/6 |

## 마지막 회귀·보관·복원

정확성·전체 828쌍·비교기·교대 성능 검증 뒤 전체 회귀 12개·platform integrity를 실행했다. 그 뒤 반디집 ZIP fast level 1로 소스·실행기·입력·캡처·전체 진단·이전 실패·측정 자료를 보관하고, 새 폴더에서 SHA-256·소스 457개·복원 비교기 120쌍 재판정·828쌍 새 렌더링·native 16,384건과 이전 glyph/상태 검사를 검증했다. 미등록 실패도 같은 raw 수치와 판정으로 재현해야 한다.

반디집 본 보관본 244,354,407 bytes·복원 증거 41,131,704 bytes의 SHA-256과 모든 entry를 확인했다. 복원 실행기 828쌍의 픽셀·전체 진단·raw·판정 및 native 캡슐 16,384건·글리프 192+3,072건·상태 복원의 로그가 모두 일치했다.

증거: `C:/twf-v32/runs/20261005-capsule-v32-final`, `C:/twf-v32/archives`. 재검사 명령은 `ScrollRenderingRegression.exe --capsule-boundaries`, `--glyph-clip-transforms`, `--horizontal-native`다. 작업·측정 script와 초기 경계 조사·이전 실패·원값은 보관본 내부 `optimization-and-accuracy-evidence/work-evidence.zip`에 보존한다.

실제 Windows DPI는 96이며 144 검사는 renderer 배율 1.5다. fractional AA 내부 중첩 group/layer 합성·캡슐/글리프 자체의 회전/비균일 transform·HTML/CSS/DOM·paint 전체 계약·실제 Windows 두 DPI·1,000문서는 미완료다.

## 마지막 정리

복원 검증 뒤 해시가 일치하는 보관 사본 80,919개, 워크스페이스 컴파일 생성물 295개, 렌더링 캐시 39,918개를 정리했다. 소스 457개·보호 입력·최신 828쌍 PNG/JSON·실패 diff·최소 증거·반디집 보관본을 재확인했다. 새 보호 입력 손실 0, 워크스페이스의 남은 컴파일 생성물 0·검사 대상 렌더링 캐시 0이다.

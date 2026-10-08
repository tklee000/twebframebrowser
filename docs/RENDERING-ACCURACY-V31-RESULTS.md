# v31 글리프 클립·상태 복원·반복 비용 검증 결과

2026-10-05, Windows x64 Release. 전체 회귀 12개·platform integrity, 반디집 보관·새 폴더 복원·828쌍 새 렌더링과 native 경계·상태 복원 검증 완료.

- native 글리프 변환 클립 경계: v30 성공 576/3,072 → v31 3,072/3,072. 이전 실패 2,496개와 실패한 상태 전환을 보존했다.
- 두 배율에서 각 8개 배치/클립/레이어 전환 scope의 batched/sequential 전체 BGRA와 target 사용·현재 transform·상태 복원이 일치했다. 기존 native 알파/소수 클립 192프레임과 일반 텍스트 23,068,672조합도 유지한다.
- 기존 828쌍의 native/reference/reference-cdp/native-window decoded RGBA·전체 native/reference JSON·raw 수치·판정 불변. 총 802/828 성공(strict 727·기존 backend 승인 75), 미등록 FAIL_PAINT 26 유지.
- 비교기 기본 182개·스크롤바 오류 주입 336/336 통과.
- 원본 입력·400px 표·채널 허용치 0·기존 예외 registry 111개 유지, 새 backend 예외 0.

## 정확성 수정

저장된 클립에 반전 transform이 있으면 endpoint의 min/max를 구하고, 회전·기울임이면 네 모서리의 변환된 축 정렬 경계를 구한다. 클립을 push할 때의 transform을 사용한다. Direct2D가 변환된 직사각형의 축 정렬 bounding box를 저장한다는 규칙을 [Microsoft transform 문서](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-transforms-overview)와 [PushAxisAlignedClip 문서](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/nf-d2d1-id2d1rendertarget-pushaxisalignedclip%28constd2d1_rect_f__d2d1_antialias_mode%29)에서 확인했다.

aliased 클립은 면적 coverage 대신 픽셀 중심의 포함 여부를 적용한다. 정확한 half-pixel 경계도 Direct2D 자체의 별도 WIC fill 결과와 비교했다. 독립 DirectWrite raw mask·네 모서리 기하 계산·double source-over를 사용해 색 2개, 배경 알파 4개, 클립 3개, run 길이 4개, 배율 2개, transform 8개, antialias 모드 2개를 검사했다. native 함수나 엔진 clip cache를 기대값 계산에 사용하지 않는다.

배치 중 clip/layer push·pop 전에 표면을 flush해 Direct2D 상태와 CPU bitmap 잠금의 순서를 맞췄다. 저장된 clip을 재생할 때 호출자의 현재 transform을 복원한다. DPI 변경·중첩 및 빈 clip·batch 중첩·opacity layer·push/pop 이후 그리기까지 전체 BGRA를 순차 그리기 결과와 비교한다.

**기존 화살표 26쌍의 채널 값 1 차이는 남아 있다.** 고정 48쌍의 raw 차이 합계 182픽셀과 판정을 유지한다. 이번 경계 검사의 정확성 개선과 기존 화살표 차이는 구분한다.

## 반복 비용과 실제 앱 성능

device clip 경계를 clip 스택·DPI·표면 크기마다 재사용하고 push/pop에서 무효화한다. 클립 없는 run은 cache 검사를 생략한다. 최초 수정안에서 clip 없는 glyph 1개가 +8.48% 느려진 측정도 attempt-1에 보존하고, 이 분기를 수정한 뒤 최종 소스로 다시 측정했다.

작은 WIC 표면의 warm paint 20,000회를 6회 직렬 교대(3회 before-after, 3회 after-before) 측정했다. 아래 median은 이 연산의 비용이다. 12개 조건 모두 3.43~14.19% 줄었으며 최종 픽셀 해시 72쌍과 heap 할당 0회는 유지했다. 모든 원값·paired 변화·실행기와 소스 SHA-256을 보존했다.

| 클립 깊이 | glyph 수 | before ms | after ms | 변화 | 빠른 회차 |
|---|---:|---:|---:|---:|---:|
| 0 | 1 | 4.612 | 4.359 | -5.48% | 6/6 |
| 0 | 4 | 10.515 | 9.814 | -6.67% | 5/6 |
| 0 | 8 | 15.806 | 14.552 | -7.94% | 6/6 |
| 2 | 1 | 4.638 | 4.387 | -5.42% | 6/6 |
| 2 | 4 | 10.799 | 9.866 | -8.64% | 6/6 |
| 2 | 8 | 17.076 | 14.652 | -14.19% | 6/6 |
| 8 | 1 | 4.725 | 4.464 | -5.52% | 6/6 |
| 8 | 4 | 10.767 | 10.398 | -3.43% | 5/6 |
| 8 | 8 | 16.312 | 14.788 | -9.34% | 6/6 |
| 16 | 1 | 5.119 | 4.408 | -13.88% | 5/6 |
| 16 | 4 | 11.055 | 9.921 | -10.26% | 6/6 |
| 16 | 8 | 16.675 | 14.880 | -10.77% | 5/6 |

같은 보존 앱 자산의 일반 Markdown과 250개 표 문서를 각 6회 직렬 교대 측정했다. BGRA 48쌍과 layout JSON 24쌍이 같다. 일반 문서 drag는 -9.29%지만 first paint +5.11%, 표 drag +5.08%·scroll +3.28%도 관측했다. 앱 전체의 일관된 속도 개선을 주장하지 않는다. 단일 실행에서 나타난 변동을 확정 원인으로 해석하지 않고 모든 6회 원값·IQR·paired 변화를 보존한다.

| 문서 | 측정 | before ms | after ms | 변화 | 빠른 회차 |
|---|---|---:|---:|---:|---:|
| normal | initialLayoutMs | 232.204 | 225.201 | -3.02% | 3/6 |
| normal | firstPaintMs | 110.907 | 116.570 | +5.11% | 2/6 |
| normal | interactiveDownMs | 17.280 | 16.203 | -6.23% | 4/6 |
| normal | interactiveDragMs | 19.600 | 17.779 | -9.29% | 5/6 |
| normal | scrollPaintMs | 21.604 | 21.695 | +0.42% | 3/6 |
| tables | initialLayoutMs | 548.799 | 550.008 | +0.22% | 3/6 |
| tables | firstPaintMs | 99.035 | 99.061 | +0.03% | 2/6 |
| tables | interactiveDownMs | 26.992 | 26.744 | -0.92% | 3/6 |
| tables | interactiveDragMs | 28.605 | 30.058 | +5.08% | 3/6 |
| tables | scrollPaintMs | 32.307 | 33.366 | +3.28% | 1/6 |

## 마지막 회귀·보관·복원

정확성·828쌍 캡처·비교기·교대 성능 검증을 마친 뒤 전체 회귀 12개·platform integrity를 실행했다. 그 다음 반디집 ZIP fast level 1로 소스·실행기·입력·캡처·진단·이전 실패·측정 자료를 보관하고 새 폴더에서 SHA-256과 복원 비교기 120쌍 재판정·828쌍 새 렌더링·3,072프레임 및 상태 복원·기존 192프레임을 검증했다. 미등록 실패도 동일한 raw 수치·판정으로 재현해야 한다.

반디집 보관본 181,294,542 bytes와 별도 복원 증거 41,129,148 bytes의 SHA-256·모든 entry를 확인했다. 복원 실행기의 828쌍 전체 픽셀·진단·raw·판정이 일치하고, native 변환 클립 3,072프레임 및 기존 알파/클립 192프레임·스크롤·드래그·스타일 복귀의 로그도 원본과 일치했다.

최초 측정 증거 묶음의 반디집 압축이 오류로 중단됐으나 재시도한 2,618개 파일의 SHA-256이 모두 일치했다. 완료된 전체 회귀는 유지하고 보관 단계부터 재개했다. 오류 로그와 재개 스크립트를 보존하며, 실행 중인 파이프라인 로그는 다음 묶음의 압축 입력에서 제외하도록 보완했다.

증거: `C:/twf-v31/runs/20261005-glyph-clip-v31-final`, `C:/twf-v31/archives`. 재검사 명령은 `ScrollRenderingRegression.exe --glyph-clip-transforms`와 `--horizontal-native`다. 작업/측정 script는 보관본 내부 `optimization-and-accuracy-evidence/work-evidence.zip`에 보존한다.

실제 Windows DPI는 96이며 144 검사는 renderer 배율 1.5다. 글리프 자체의 회전/비균일 변환은 기존 fallback을 사용한다. fractional AA clip 내부 중첩 합성 전체·HTML/CSS/DOM 및 paint 전체 계약·실제 Windows 두 DPI·1,000문서는 미완료다.

## 마지막 정리

복원 검증 후 해시가 일치하는 보관 사본 76,584개, 워크스페이스 컴파일 생성물 295개, 캐시 38,181개를 정리했다. 소스 457개·보호 입력·최신 828쌍의 PNG/JSON·실패 diff·최소 증거·반디집 보관본을 다시 확인했다. 새 보호 입력 손실 0, 워크스페이스의 남은 컴파일 생성물 0·검사 대상 렌더링 캐시 0이다.

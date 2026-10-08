# MdViewer CPU 렌더링·스크롤 성능 최적화 결과

검증일: 2026-10-04. 대상: TWebFrame2 공통 View, 현재 MdViewer x64 Release.

실제 MdViewer의 HTML/CSS/JavaScript에 `RENDERING-ACCURACY-VALIDATION-PLAN.md`를 넣어 검사했다. 커닝, 글꼴 선택, glyph 위치, LCD 위상 및 색 보정 계산은 유지하고 반복되는 래스터 분석·반올림·픽셀 복사·표면 생성을 줄였다. 스크롤바 입력 뒤 페인트가 메시지 큐에 밀리지 않도록 바로 갱신한다.

## 성능

같은 입력, 1600×1000 장치 픽셀, 실제 96 DPI, 12번의 thumb 이동과 WM_PRINTCLIENT 페인트를 사용했다. 변경 전 엔진은 `c5a94a5`의 CPU 경로다. 입력은 [보존 문서](../TWebFrame2/tests/rendering/performance/20261004-cpu-scroll/input.md)에 고정했다.

| 측정 | 변경 전 | 변경 후 |
| --- | ---: | ---: |
| 첫 페인트 | 194.6ms | 118.5ms |
| 스크롤 페인트 중앙값 | 74.3ms | 21.6ms |
| 스크롤 페인트 평균 | 75.7ms | 22.4ms |
| 스크롤 페인트 범위 | 64.9~93.3ms | 19.5~34.0ms |
| 새 WM_PAINT 경로의 외부 포커스 후 첫 클릭 | 별도 측정 없음 | 16.3ms |
| 새 WM_PAINT 경로의 첫 드래그 | 별도 측정 없음 | 16.5ms |

페인트 중앙값은 약 **3.4배**, 평균은 약 3.4배 개선됐다. 같은 실행기를 다시 실행했을 때 첫 클릭 17.9ms·첫 드래그 18.5ms였고 화면도 동일했다. 실제 WM_PAINT 검사에서는 입력 함수가 반환되기 전에 페인트가 발생하고 갱신 영역이 비어 있는지 확인한다. 현재 앱에서도 이 문서를 열어 탐색기에 포커스를 둔 후 첫 세로 스크롤 드래그와 연속 드래그를 확인했다.

구 `D:\ai_works\mdviewer_tinyversion\x64\Release\MdViewer.exe`와의 정량 비교는 하지 않았다. 사용자가 관찰한 2~3초의 지연을 같은 크기로 수치 재현한 결과도 아니다. 문서 열기와 레이아웃 계산은 약 0.15~0.22초로 남아 있으며, 이번 개선의 핵심은 반복 페인트와 스크롤 갱신이다. 현재 하드웨어의 한 문서 측정이므로 다른 화면 크기·글꼴·문서의 프레임 시간을 보장하지 않는다.

## 공통 엔진 변경

- `RasterSurface.h`: 원본 face를 유지하는 LCD glyph 마스크 캐시. face·물리 em 크기·glyph 번호·렌더 모드·1/4픽셀 위상을 키로 사용하고 텍스처 16MiB/4096개로 제한한다. 위치·색·클립은 매번 적용하므로 캐시가 스크롤이나 색상 변경을 고정하지 않는다.
- 동일한 8개 sRGB 보정표를 재사용한다. 불투명 클립 내부는 원래 5/6/5비트 커버리지와 부동소수 반올림을 미리 계산한 합성표를 사용한다. 모든 색을 사용하는 경우에도 합성표는 스레드당 최대 6MiB다. 소수 클립 경계는 원래 수식을 사용한다.
- `Layout.cpp`: 한 텍스트 레이아웃의 짧은 glyph 구간들에서 픽셀 잠금을 재사용한다. 밑줄·취소선·inline object·color font 등의 대체 페인트 전에 D2D 상태를 복구한다.
- `RasterSkia.h`: 모든 리소스를 준비한 후 잠긴 CPU 표면에 직접 그린다. 모서리 하나마다 전체 viewport를 복사하던 임시 버퍼를 제거했다.
- `View.cpp`: WIC 표면과 해당 타깃의 브러시/비트맵을 재사용한다. 크기·DPI 변경 또는 오류 때 소유 리소스를 먼저 해제한다. 클릭과 드래그는 휠 경로처럼 최상위 합성 View를 즉시 갱신한다.

## 정확도와 배포

- 실제 문서 초기 화면, 스크롤 1/6/12번째 지점의 BGRA 4개와 레이아웃 JSON 2개가 변경 전과 바이트 단위로 같다. 재실행도 같다.
- 원본 20문서×2 DPI×3 viewport = 120개 성공. 84개 strict, 기존 등록된 CPU/GPU 차이 36개. 필수 계산 스타일 17,400개 검사·누락 0, 비교 교정 134개 통과.
- 원본 자체 PNG 120개와 추가 스타일 교정 PNG 18개가 이전 최종 실행과 138/138 동일하다. 기존 CPU/GPU 예외 목록을 수정하거나 오차 허용치를 넓히지 않았다.
- 추가 스타일/구조 18/18, 기대값 660개 통과. 기존 폼 페인트 6쌍은 같은 FAIL_PAINT로 유지하며 전체 비교는 12/18이다.
- 독립 색/DPI 캡처 교정 18/18 및 기존 전체 회귀 실행 파일 12개·platform integrity 통과.
- 현재 실행 파일: `D:\ai_works\miniwebbrowser\mdviewer_tinyversion\x64\Release\MdViewer.exe`. 현재 TWebFrame2 소스로 재빌드하고 고정된 Skia DLL 및 라이선스를 배포했다. 수정 전 현재 앱 EXE와 구 TWebFrame 앱은 별도로 보존했다. 앱 HTML/CSS/JavaScript는 변경하지 않았다.

## 재실행과 증거

[수치·픽셀 해시](../TWebFrame2/tests/rendering/performance/20261004-cpu-scroll/)에 작은 증거 파일을 보존했다. 전체 실행 자료는 로컬 `TWebFrame2/tests/rendering/runs/20261004-performance-before-v7`, `20261004-performance-final-v1`, `20261004-performance-final-v2`, `20261004-performance-accuracy-final`에 있다. 새 출력 폴더를 사용하며 이전 결과를 덮어쓰지 않는다.

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe' `
  TWebFrame2/tests/ViewRenderingPerformance.vcxproj /t:Build /p:Configuration=Release /p:Platform=x64 /m:2
& TWebFrame2/tests/bin/x64/Release/ViewRenderingPerformance.exe `
  D:\ai_works\miniwebbrowser D:\ai_works\miniwebbrowser\TWebFrame2\tests\rendering\runs\new-performance-run `
  D:\ai_works\miniwebbrowser\TWebFrame2\tests\rendering\performance\20261004-cpu-scroll\input.md
```

MdViewer 재빌드는 로컬 기존 CMake 3.31.8 실행기로 수행했다. 별도 새 브랜치/PR은 만들지 않았다. 전체 정확도 계약, 실제 Windows 두 DPI의 통합 화면 검사 및 1,000문서 확대는 기존 계획의 미완료 상태를 유지한다.

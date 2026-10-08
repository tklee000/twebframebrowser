# WebView2 기준 HTML·CSS·DOM·레이아웃·페인트 정확성 검증 계획

작성일: 2026-10-03  
대상: TWebFrame 자체 엔진, Windows x64 Release  
현재 상태: **2026-10-07 v49: 주 corpus 20→100 확장·Skia 페인트 좌표 정확성·반복 자원 비용을 묶어 진행 중. 전체 회귀·반디집 보관·복원 검증은 마지막에 진행한다.**
직전 완료 상태: **2026-10-07 v48: 둥근 상자·그림자 클립 560프레임·비정상 좌표 60건 통과, 802/828·기존 실패 26 유지. 전체 회귀 12개·platform integrity·반디집 보관·새 폴더 복원·828쌍 새 렌더링·12개 독립 검사 로그 대조 완료.** [v48 기록](RENDERING-ACCURACY-V48-RESULTS.md). 전체 계약·1,000문서는 미완료다.

## 2026-10-07 v49 corpus 100문서·페인트 좌표·자원 비용 통합 실행 계획

- [x] v48 동결 소스 458개·보호 live 입력·동일 성능 자산 확인 및 변경 전 증거 보존.
- [ ] 기능별 10% 목표 비율로 주 corpus 20→100 확장, 기존 20개 불변·seed·hash·경계값·coverage 기록.
- [ ] 독립 페인트 좌표·guard 검사와 신규 문서 비교로 공통 정확성 교정, bounded 자원 재사용 검증.
- [ ] 직렬 교대 전후 성능·pixel·layout 확인, 신규 문서 실패와 증가한 조건 기록.
- [ ] 100문서 600쌍 및 기존 보정 708쌍 실행·기존 828쌍 불변·비교기 오류 주입 확인.
- [ ] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 복원·1,308쌍 새 렌더링·독립 검사 로그 대조.
- [ ] 최종 문서 수·통과 수·남은 900문서·계약·실패 공개, 소스·보호 입력·증거 보존 확인.

확장 순서: **20→100→300→1,000문서**. 문서 생성 수와 실행·통과 수를 구분한다. 100문서 기본 비교는 600쌍이며, 실제 Windows DPI 검증은 별도 미완료다.

입력·채널 허용치 0·registry 111개 유지. 신규 실패를 예외로 숨기지 않는다.

## 2026-10-07 v48 화살표 원인 분리·Skia 클립 정확성·페인트 비용 통합 실행 계획

- [x] v47 동결 소스 457개·보호 live 입력·동일 성능 자산 확인 및 변경 전 증거 보존.
- [x] 화살표 mask·감마·합성 후보를 독립적으로 분리 검증, 26건 잔여 원인 미확정 기록.
- [x] 공통 렌더러 정확성 개선과 반복 페인트 비용 최적화, 색·opacity·clip·변환 경계 검증.
- [x] 직렬 교대 전후 성능·pixel·layout 확인, 증가한 조건과 미채택 후보도 기록.
- [x] 전체 828쌍 새 캡처·진단·raw 판정 및 비교기 오류 주입 검사.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 복원·새 렌더링·독립 검사 로그 대조.
- [x] 최종 결과와 남은 계약을 문서화하고 소스·보호 입력·증거 보존 확인.

입력·채널 허용치 0·registry 111개 유지. 실패를 새 예외나 캡처별 보정으로 숨기지 않는다.





## 2026-10-06 v47 반투명 합성표 캐시 상태·조회 비용 통합 실행 계획

- [x] v46 동결 소스 457개·보호 live 입력·고정 48쌍·동일 앱 자산 확인.
- [x] native/ordinary 캐시의 7/8회 경계·인접 float/RGB 키·교체·복귀·스레드 격리 독립 검증.
- [x] 조회/생성 분리·inline·정렬 후보의 실제 페인트 검토, 퇴행 후보 제외 및 v46 렌더러 유지.
- [x] 직렬 교대 조회·표 생성·glyph warm/cold·앱 속도 측정, 증가 조건 추가 검토.
- [x] 전체 828쌍 pixel·전체 진단·raw·판정과 비교기 오류 주입 확인.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 복원·828쌍 새 렌더링·독립 검사 로그 대조.
- [x] 복원 완료 후 생성물·캐시 정리, 보호 입력·소스·캡처 보존 및 최종 기록 반디집 압축·복원.

입력·채널 허용치 0·registry 111개 유지. 기존 화살표 실패 26건·과거 보관본 미확인을 추적한다.

## 2026-10-06 v46 반투명 native 아이콘 합성표 전수 검증·최초 생성 비용 통합 실행 계획

- [x] v45 동결 소스·현재 live 보호 입력·고정 48쌍·동일 앱 자산 확인.
- [x] binary32 opacity를 정수 유리수로 해석하는 독립 native source-over oracle, 전 항목·키·교체·복귀 검증.
- [x] 단일 반올림 규칙을 유지하며 native 합성표의 source 계산을 backdrop 루프 밖으로 공유.
- [x] 6회 직렬 교대 최초 생성·glyph warm/cold·앱 성능 측정, 감소와 증가를 모두 기록.
- [x] 전체 828쌍 pixel·전체 진단·raw·판정 불변과 비교기 오류 주입 확인.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 복원·828쌍 새 렌더링·10개 독립 검사 로그 대조.
- [x] 복원 완료 후 생성물·캐시 정리, 보호 입력·소스·캡처 보존 확인과 최종 기록 반디집 압축·복원.

입력·채널 허용치 0·registry 111개 유지. 기존 화살표 실패 26건·과거 보관본 미확인을 별도로 추적한다.

## 2026-10-06 v45 글자 합성표 전체 색·초기화 지연·배경 계산 공유 통합 실행 계획

- [x] v44 동결 소스·현재 live 보호 입력·고정 48쌍·동일 앱 자산 확인.
- [x] 전체 256색·5/6/8-bit text·8-bit native 표의 독립 기준과 숨김·투명·늦은 거부 경로를 변경 전후 검증.
- [x] 사용할 paint의 표만 초기화하고 backdrop 계산을 공유, byte·포인터 재사용·메모리 한도 검증.
- [x] 6회 직렬 교대 warm/cold·불필요 초기화·실제 앱 첫 paint/scroll 성능 및 증가값 기록.
- [x] 전체 828쌍 픽셀·전체 진단·raw·판정과 비교기 오류 주입 확인, 기존 실패 기록.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 복원·828쌍 새 렌더링·8개 검사 로그 대조.
- [x] 소스·현재 보호 입력·최신 캡처·보관본 유지 및 최종 결과 기록.

입력·허용치 0·registry 111개 유지. 기존 화살표 실패 26건·이전 보호 보관본 4,544개 실물 확인 미완료를 추적한다.






## 2026-10-06 v44 일반 반투명 조회표 전체 항목·최초 생성 비용 통합 실행 계획

- [x] v43 동결 소스·현재 live 보호 입력·고정 48쌍·동일 앱 자산 확인.
- [x] 독립 IEEE float 반올림 기준으로 색·opacity·coverage·배경 전체 조회표와 키 전환을 변경 전후 검증.
- [x] coverage에만 의존하는 채널 반올림 계산을 루프 밖으로 이동, 조회표 byte·메모리 제한·8회 정책 유지.
- [x] 새/기존 검사·6회 직렬 교대 warm/cold·앱 BGRA/layout 성능 검증 및 증가값 기록.
- [x] 전체 828쌍 픽셀·진단·raw·판정과 오류 주입 확인, 기존 실패 기록.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 복원·828쌍 새 렌더링·7개 검사 로그 대조.
- [x] 소스·현재 보호 입력·최신 캡처·보관본 유지 및 결과 기록.

입력·허용치 0·registry 111개 유지. 기존 화살표 실패 26건과 이전 보호 보관본 4,544개 실물 확인 미완료를 추적한다.








## 2026-10-06 v43 일반 회색조 반투명 합성·조회표 통합 실행 계획

- [x] v42 동결 소스·현재 live 보호 입력·고정 48쌍·동일 앱 자산 확인.
- [x] 독립 byte-backed DirectWrite oracle로 반투명 색/브러시·clip·축·반복 합성의 변경 전 실패 보존.
- [x] 일반 회색조 opacity 합성과 LCD exact guard 수정, bounded 조회표·키 전환·거부/복귀 검증.
- [x] 새/기존 검사·6회 직렬 교대 페인트·캐시 대조군·앱 BGRA/layout 성능 검증.
- [x] 전체 828쌍 픽셀·진단·raw·판정과 오류 주입 확인, 기존 실패 기록.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 복원·828쌍 새 렌더링·검사 로그 대조.
- [x] 소스·현재 보호 입력·최신 캡처·보관본 유지 및 결과 기록.

입력·허용치 0·registry 111개 유지. 일반 회전·기울임·layer·반투명 LCD는 fallback. 이전 보호 보관본 4,544개 실물 확인은 미완료다.





## 2026-10-06 v42 일반 텍스트 대각 축·guard·run 저장소 통합 실행 계획

- [x] v41 동결 소스·현재 live 보호 입력 확인, 고정 48쌍·실행기·동일 앱 자산 선택 복원.
- [x] 독립 DirectWrite LCD/회색조 mask·gamma 합성 oracle로 비균일 배율·반전·미세 변환의 변경 전 실패 보존.
- [x] 일반 텍스트의 대각 축 변환과 정확한 affine guard 교정, 짧고 긴 run 저장소 재사용 및 조기 실패·복귀 검증.
- [x] 새 검사·기존 native 검사와 6회 직렬 교대 페인트·앱 BGRA/layout 비교, 할당과 증가한 시간도 기록.
- [x] 전체 828쌍의 픽셀·전체 진단·raw·판정과 비교기 오류 주입 확인, 기존 실패 26 유지 여부 기록.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 실제 복원·828쌍 새 렌더링·새/기존 검사 로그 대조.
- [x] 소스·현재 보호 입력·최신 캡처·최소 증거·반디집 보관본 유지 및 결과 기록.

입력·허용치 0·registry 111개 유지. 기울임·일반 affine·layer는 fallback을 유지하며 전체 계약으로 확대 해석하지 않는다. 이전 보호 보관본 4,544개 실물 확인은 별도 미완료다.






## 2026-10-06 v41 일반 회색조 텍스트 알파·채널 재사용 통합 실행 계획

- [x] v40 동결 소스·현재 live 보호 입력 확인, 고정 48쌍·실행기·동일 앱 자산 선택 복원.
- [x] 독립 DirectWrite LCD→회색조 mask와 source-over oracle로 투명/반투명 배경·색·clip·중첩 run의 변경 전 실패 보존.
- [x] 일반 회색조 텍스트의 알파 합성 교정과 회색 글자의 동일 채널 계산 재사용, opaque/LCD 계약 유지.
- [x] 새 검사·기존 native 검사와 6회 직렬 교대 페인트·앱 BGRA/layout 비교, 증가한 시간도 기록.
- [x] 전체 828쌍의 픽셀·전체 진단·raw·판정과 비교기 오류 주입 확인, 기존 실패 26 유지 여부 기록.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관 → 새 폴더 실제 복원·828쌍 새 렌더링·새/기존 native 로그 대조.
- [x] 소스·현재 보호 입력·최신 캡처·최소 증거·반디집 보관본 유지 및 결과 기록.

입력·허용치 0·registry 111개 유지. 이전 보호 보관본 4,544개 실물 확인은 별도 미완료다.





## 2026-10-06 v40 공유 clip cache 검증·hot path 통합 실행 계획

- [ ] v39 소스 457개·live 보호 입력 2,799개·정리 상태 확인, 고정 48쌍·실행기·동일 앱 자산 복원. 이전 보호 보관본 4,544개 실물 확인.
- [x] 공유 clip cache 질의 17,548건·guard/복귀 164건 독립 검사, 변경 전후 실패 0.
- [x] cache hit 중복 DPI 검사·미사용 magnitude 계산 축소, 기존 clipping 계약 유지.
- [x] 기존 native 검사·6회 직렬 교대 결과 144+168+120+48쌍·allocation·앱 BGRA 48쌍/layout 24쌍 비교.
- [x] 전체 828쌍·비교기 오류 주입 182+336건 검증, 성능 증가값과 남은 실패 기록.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·native 로그 대조.
- [x] 복원 검증 뒤 해시 일치 사본·생성물·캐시 정리, 소스·현재 live 보호 입력·최신 캡처·최소 증거 확인.

입력·허용치 0·registry 111개 유지. 화살표 26건은 실패로 추적한다.  이전 보호 보관본 4,544개 실물 확인 미완료. 전체 계약은 별도 미완료다.

## 2026-10-06 v39 native 캡슐 정밀 판정·마스크 조회 통합 실행 계획

- [x] v38 소스 457개·보호 입력·정리 상태 확인, 고정 48쌍·실행기·같은 앱 자산 선택 복원.
- [x] 독립 캡슐/clip 검사 6,912건·변경 전 실패 2,304건·guard assertion 실패 104건 보존.
- [x] 미세 비균일 축·소수 경계·clip descriptor/DPI 판정 보강과 bounded 마스크 조회 최적화, guard/복귀 112건 통과.
- [x] 기존 native 검사·6회 직렬 교대 pixel hash 168+120+48쌍·할당·앱 BGRA 48쌍·layout 24쌍 검증.
- [x] 전체 828쌍·비교기 오류 주입 182+336건 검증, 성능 증가값과 남은 실패 기록.
- [x] 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·native 로그 대조.
- [x] 복원 검증 뒤 해시 일치 사본·생성물·캐시 정리, 소스·보호 입력·최신 캡처·최소 증거 확인.

입력·허용치 0·registry 111개 유지. 화살표 26건은 실패로 추적한다. 전체 계약은 별도 미완료다.

## 2026-10-06 v38 native 글리프 미세 변환·반투명 합성 경로 통합 실행 계획

- [x] v37 소스 457개·보호 입력·정리 상태 확인, 고정 48쌍·실행기·같은 앱 자산 선택 복원.
- [x] 미세 변환 포함 독립 248,832건·변경 전 픽셀 실패 23,224건 보존·guard/복귀 34건 검증.
- [x] native 미세 축 비교·기울임 판정 수정, 캐시 준비된 반투명 합성 루프 분리.
- [x] 기존 native 정확성·6회 직렬 교대 pixel hash 120+48쌍·앱 BGRA 48쌍·layout 24쌍 검증.
- [x] 전체 828쌍 불변·비교기 오류 주입 182+336건 검증, 증가한 성능 수치와 한계 기록.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·native 로그 대조.
- [x] 복원 검증 뒤 해시가 일치하는 사본·생성물·캐시 정리, 소스·보호 입력·최신 캡처·최소 증거 확인.

원본 입력·허용치 0·registry 111개 유지. 화살표 26건은 실패로 추적한다. 일반 회전/기울임·fractional group/layer·전체 계약은 이번 완료 범위가 아니다.

## 2026-10-06 v37 native 글리프 반전·반투명 합성 조회 통합 실행 계획

- [x] v36 소스 457개·보호 입력·정리 상태 확인, 고정 48쌍·실행기·같은 앱 자산 선택 복원.
- [x] 반전/비균일 배율 독립 221,184건·변경 전 미지원 152,064건 보존·guard/복귀 26건 검증.
- [x] signed native mask 배율·cache 방향 구분, 완전히 덮인 clip 전용 루프로 per-pixel 기하 계산·분기 축소.
- [x] 기존 native 정확성·6회 직렬 교대 pixel hash 120+48쌍·앱 BGRA 48쌍·layout 24쌍 검증.
- [x] 전체 828쌍 불변·비교기 오류 주입 182+336건 검증, 증가한 성능 수치와 한계 기록.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·native 로그 대조.
- [x] 복원 검증 뒤 해시가 일치하는 사본·생성물·캐시 정리, 소스·보호 입력·최신 캡처·최소 증거 확인.

원본 입력·허용치 0·registry 111개 유지. 화살표 26건은 실패로 추적한다. 일반 회전/기울임·fractional group/layer·전체 계약은 이번 완료 범위가 아니다.

## 2026-10-06 v36 native 글리프 비균일 배율·최초 캐시 비용 통합 실행 계획

- [x] v35 소스 457개·보호 입력·정리 상태 확인, 고정 48쌍·실행기·같은 앱 자산 선택 복원.
- [x] 독립 비균일 배율 검사에서 이전 CPU 미지원·픽셀 비교 실패 62,208건 보존, 69,120건·guard/복귀 26건 통과.
- [x] native mask 가로/세로 비율·제한된 cache key 적용, 회색 opacity table 중복 RGB 계산 제거.
- [x] 최초 키·8번째 표 생성·키 변경/복귀·반복 페인트 6회 직렬 교대, pixel hash 48+120쌍·앱 BGRA 48쌍·layout 24쌍 및 증가값 기록.
- [x] 새 배율·기존 clip/opacity/native/상태 복원·전체 828쌍 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·새/기존 native 로그 대조.
- [x] 모든 검증 뒤 해시가 일치하는 사본·생성물·캐시 정리, 소스·보호 입력·최신 캡처·최소 증거 확인.

원본 입력·허용치 0·기존 registry 111개 유지. 화살표 26건은 실패로 추적한다. 회전/기울임/음수 글리프 변환·fractional group/layer 합성의 전체 계약은 이번 완료 범위가 아니다.

## 2026-10-06 v35 subpixel 글리프 클립·합성 재사용 통합 실행 계획

- [x] v34 소스 457개·보호 입력·정리 상태 확인, 고정 48쌍·실행기·같은 앱 자산 선택 복원.
- [x] 독립 subpixel·빈·역전 clip 검사에서 이전 픽셀 실패 8,786건 보존, overlap·DPI 좌표 교정 61,440건 통과.
- [x] 한 thread당 RGB/opacity 1개·256 KiB table·연속 8회 재사용 정책, 색/opacity/clip/face 변경 및 복귀 검증.
- [x] 고정/변화하는 opacity 반복 페인트 6회 직렬 교대·120 pixel hash쌍, 앱 BGRA 48쌍·layout 24쌍 비교와 증가값 기록.
- [x] 새 좁은/빈 clip·기존 opacity/방향/경계/clip·상태 복원·전체 828쌍 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·새/기존 native 로그 대조.
- [x] 모든 검증 뒤 해시가 일치하는 사본·생성물·캐시 정리, 소스·보호 입력·최신 캡처·최소 증거 확인.

원본 입력·허용치 0·기존 registry 111개 유지. 화살표 26건은 실패로 남긴다. 여러 fractional AA group/layer와 글리프 자체 일반 affine 변환의 전체 계약은 이번 완료 범위로 주장하지 않는다.

## 2026-10-05 v34 native 글리프 불투명도·긴 run 통합 실행 계획

- [x] v33 소스 457개·보호 입력·정리 상태 검증, 고정 48쌍·실행기·같은 앱 자산 선택 복원.
- [x] 독립 DirectWrite mask·double gamma/source-over로 이전 native 지원·픽셀 실패 30,240건 보존, brush alpha×opacity 교정 32,256건 통과.
- [x] 긴 native run capacity와 immutable face 재사용, 입력 guard 14건·face/factory 전환과 복귀 8건·기존 native 검사 통과.
- [x] 반복 페인트 6회 직렬 교대·42 pixel hash쌍, 같은 앱 자산 BGRA 48쌍·layout 24쌍 비교, 증가 단계도 기록.
- [x] 새 opacity·기존 native 방향/경계/clip·상태 복원·전체 828쌍 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·새/기존 native 로그 대조.
- [x] 모든 검증 뒤 해시가 일치하는 사본·생성물·캐시 정리, 소스·보호 입력·최신 캡처·최소 증거 재확인.

원본 입력·허용치 0·기존 registry 111개 유지. 기존 화살표 26건은 실패로 남긴다. 일반 텍스트 부분 불투명도·color font·글리프 자체 회전/비균일 변환·layer는 fallback을 유지한다.

## 2026-10-05 v33 캡슐 방향·반전·중앙 span 통합 실행 계획

- [x] v32 소스 457개·보호 입력·생성물 정리 상태 확인, 고정 48쌍·실행기·같은 앱 자산 선택 복원.
- [x] 독립 segment-distance 검사로 이전 실패 19,200건 보존, 세로·반전·축 교환 device 캡슐 교정.
- [x] 중앙 불투명 span 처리와 방향 간 같은 두께 캐시 재사용, 색·불투명도·두께·clip·transform 변경 및 복귀 검증.
- [x] 반복 페인트 6회 직렬 교대 측정, 같은 앱 자산 BGRA 48쌍·layout 24쌍 불변과 단계별 속도 변화 기록.
- [x] 방향 24,576건·기존 캡슐 16,384건·글리프 192+3,072건·상태 복원·전체 828쌍·비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·방향/기존 native 검사 로그 대조.
- [x] 검증 이후 해시가 일치하는 사본·생성물·렌더링 캐시 정리, 소스·보호 입력·최신 캡처·최소 증거 재확인.

일반 회전·skew·비균일 transform·소수 geometry·fractional AA clip·layer는 기존 fallback을 유지한다. 허용치 0·기존 예외 registry 111개·원본 입력을 유지하고 기존 화살표 26건은 실패로 기록한다.

## 2026-10-05 v32 native 캡슐 투명도·클립·합성 재사용 통합 실행 계획

- [x] v31 소스 457개·보호 입력 확인, 반디집 보관본에서 고정 48쌍·실행기·앱 자산 선택 복원.
- [x] native 캡슐 소수 aliased·반전/회전/기울임 클립의 픽셀 중심 포함과 폭 0 변환 경계 교정, 독립 이전 실패 보존.
- [x] brush alpha×opacity·투명 backdrop의 premultiplied 합성과 색/불투명도/두께 변경·복귀 검증. 16,384건 중 fractional AA fallback 3,328건 유지.
- [x] bounded 캐시에 edge/center premultiplied ink를 재사용하고 반복 페인트·같은 앱 자산을 직렬 교대 측정. 증가한 값도 보존.
- [x] 기존 native 글리프 192+3,072건·상태 복원·고정 48쌍·기존 전체 828쌍의 픽셀/전체 진단/raw/판정 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·16,384건 및 기존 native 경계·상태 복원 재검사.
- [x] 모든 검증 이후 해시가 일치하는 사본·워크스페이스 생성물·렌더링 캐시 정리, 보호 입력·최신 캡처·최소 증거 유지.

캡슐 자체의 소수 geometry·회전/비균일 transform·fractional AA group/layer 합성은 기존 fallback을 유지한다. 기존 화살표 미등록 FAIL_PAINT 26쌍·채널 허용치 0·원본 입력·기존 예외 registry 111개를 유지한다.

## 2026-10-05 v31 글리프 클립·상태 복원·반복 비용 통합 실행 계획

- [x] 생성물·캐시 0 확인, v30 소스 457개·보호 입력 검증, 반디집 보관본에서 고정 48쌍·실행기·앱 자산 선택 복원.
- [x] 반전·회전·기울임 클립의 world-space 경계와 aliased 픽셀 중심 포함 규칙을 공통 교정, 독립 3,072프레임에서 이전 실패 2,496개 보존.
- [x] 배치 중 클립·레이어 변경 전 표면 잠금 해제와 현재 transform 보존, 두 배율의 DPI·중첩·빈 클립·상태 복원 비교.
- [x] 클립 경계를 스택·DPI·표면 크기에 따라 재사용하고 반복 페인트 및 같은 앱 자산의 성능을 직렬 교대 측정. 증가한 값도 보존.
- [x] 고정 48쌍 및 기존 828쌍의 픽셀·전체 진단·raw 수치·판정 불변과 오류 주입 비교기 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링·3,072프레임과 상태 복원 재검사.
- [x] 검증 완료 후 해시가 일치하는 생성물·캐시·사본만 정리, 보호 입력·최신 캡처·최소 증거 유지.

허용치 0·원본 입력·기존 예외 목록을 유지한다. 기존 화살표 FAIL_PAINT 26쌍은 해결되지 않으면 실패로 남긴다. 글리프 자체의 회전/비균일 변환과 fractional AA clip 내부 중첩 합성 전체는 이번 완료 범위로 주장하지 않는다.

## 2026-10-05 v30 글리프 감마·합성·반복 페인트 통합 실행 계획

- [x] 생성물·캐시 0, 보호 입력과 v29 소스 457개 해시 확인, 고정 48쌍·실행기·앱 자산 선택 복원.
- [x] 화살표 잔여 감마·합성 조사, 투명 알파·소수 클립 공통 교정과 이전 경로 실패 160개 확인.
- [x] 짧은 글리프 run의 임시 할당 제거, 두 배율의 클립·스타일 변경·복귀·독립 경계 192프레임 검증.
- [x] 독립 경계 192프레임 개선·고정 48쌍/기존 780쌍 불변·오류 주입 비교기 검증.
- [x] 같은 앱 자산 직렬 교대 성능 측정, 증가한 수치도 보존.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링.
- [x] 모든 검증 뒤 보관 해시가 일치하는 생성물·캐시·사본 정리, 입력·최신 캡처·최소 증거 유지.

허용치 0·기존 예외 111개·원본 400px 표를 유지한다. 잔여 실패를 새 예외로 숨기지 않는다.

## 2026-10-05 v29 스크롤바 곡선·합성·페인트 비용 통합 실행 계획

- [x] 생성물·캐시 0 확인, 보호 입력과 v28 소스 457개 해시 검증, 고정 48쌍·실행기·앱 자산 선택 복원.
- [x] 원본 400px 표와 기존 입력을 유지한 채 곡선 coverage·회색조 합성 경로를 공통 교정.
- [x] 반복 페인트 비용 감소, 두 배율의 스크롤·클립·크기/스타일 변경과 복귀 검증.
- [x] 기존 780쌍 불변·고정 48쌍의 strict/raw 개선·비교기 오류 주입 검증.
- [x] 같은 앱 자산의 교대 측정과 최초 페인트 비용 분석, 증가한 시간도 기록.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 828쌍 새 렌더링.
- [x] 회귀·복원 완료 후 검증된 생성물·캐시·사본 정리, 보호 입력·최신 캡처·최소 증거 유지.

허용치 0·기존 backend registry 111개를 유지한다. 잔여 실패를 새 예외로 등록하거나 입력 크기를 바꿔 숨기지 않는다. 전체 계약·실제 Windows 두 DPI·1,000문서는 미완료다.

## 2026-10-05 v28 기본 수평 스크롤바·DPI·반복 비용 통합 실행 계획

- [x] 생성물·캐시 0 확인, 보호 입력과 v27 소스 438개 검증·반디집 기준 최소 복원.
- [x] 원본 400px 표 2개를 그대로 포함한 새 8문서·48쌍 입력 고정과 변경 전 실패 보존.
- [x] 기본 수평 스크롤바 Fluent 글리프·물리 픽셀 thumb/track·최소 길이 공통 교정.
- [x] 스크롤·drag·크기/스타일 변경과 복귀 검증, 반복 조회 감소와 교대 성능 측정.
- [x] 새 48쌍·기존 780쌍 픽셀/전체 진단/raw 및 비교기 고의 오류 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 전체 새 렌더링.
- [x] 회귀·복원 완료 후 생성물·캐시·검증된 사본 정리, 최소 캡처·기록과 보호 입력 유지.

새 48쌍은 strict 22/48·구조 48/48, raw 픽셀 차이 93.17% 감소. 남은 26쌍은 미등록 FAIL_PAINT이며 예외나 허용치 추가 0이다. 전체 계약·실제 Windows 144 DPI·1,000문서는 미완료다.

## 2026-10-05 v27 자동 표 셀 제약·열 계산 재사용 통합 실행 계획

- [x] 생성물·캐시 정리 상태와 보호 입력 확인, v26 반디집 보관본에서 최소 기준 복원·새 8문서 before 보존.
- [x] 자동 표 셀의 글꼴 단위·padding/border·box-sizing·HTML width 제약을 공통 교정.
- [x] colspan 내부 간격·중첩 표 격리·viewport 단위 열/셀 경계 검증.
- [x] viewport 의존성을 구분해 같은 열 너비 계산 재사용, 변경·복귀 무효화와 같은 앱 자산 교대 성능 측정.
- [x] 새 v2 48쌍 두 DPI strict·기존 732쌍 픽셀/전체 진단/raw 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 전체 780쌍 재렌더링.
- [x] 회귀·복원 이후 생성물·캐시·검증된 사본 정리, 보호 입력·최신 캡처·최소 기록 유지.

초기 v1의 400px 표 넘침 4쌍은 기본 수평 스크롤바 FAIL_PAINT로 보존하며 성공·승인 예외로 처리하지 않는다. v2는 두 viewport 제어 표만 320px로 고정한 별도 입력이다.

전체 자동 표 분배·복잡한 caption wrapper·HTML insertion modes·실제 Windows 두 DPI·1,000문서는 미완료다. 증거는 `C:/twf-v27`에 보존한다.

## 2026-10-05 v26 표 너비 경계·측정 재사용 통합 실행 계획

- [x] 작업 전 생성물·캐시 정리 상태와 보호 입력 확인, v25 반디집 보관본에서 최소 기준 복원·새 8문서 before 보존.
- [x] 명시적 0 열·셀과 남은 너비, percentage/absolute 혼합과 전체 백분율 경계 교정.
- [x] 빈 표·간격·percentage 셀 decoration·예약 열이 있는 colspan의 공통 처리 검증.
- [x] 실제 열 너비에 따른 행 측정 재사용·불필요한 임시 배열 제거와 무효화 검증, 같은 앱 자산 교대 성능 측정.
- [x] 새 48쌍 두 DPI strict·기존 684쌍 픽셀/전체 진단/raw 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 보관·새 폴더 복원 → 전체 732쌍 재렌더링.
- [x] 회귀·복원 완료 후 생성물·캐시·검증된 사본 정리, 보호 입력·최신 캡처·최소 기록 유지.

전체 자동 표 알고리즘·복잡한 caption wrapper·HTML insertion modes·실제 Windows 두 DPI·1,000문서는 미완료다. 증거는 `C:/twf-v26`에 보존한다.

## 2026-10-05 v25 고정 표 우선순위·폭 경계·측정 비용 통합 실행 계획

- [x] 작업 전 생성물 확인·최소 정리, 검증된 v24 실행기/소스/앱 자산 선택 복원과 새 8문서 before 보존.
- [x] col·첫 행 셀·colspan·명시적 0 너비의 우선순위와 간격/box decoration 공통 교정.
- [x] 지정 표 너비를 넘는 절대 열·자식 col이 있는 colgroup의 폭 적용·fixed + auto 너비 경계 교정.
- [x] 반복 열/행 측정 비용 감소와 크기·스타일·DOM 변경 무효화, 같은 앱 자산 교대 성능 측정.
- [x] 두 DPI 새 48쌍·기존 636쌍 픽셀/전체 진단/raw 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 반디집 불변 보관·새 폴더 복원 → 전체 684쌍 새 렌더링.
- [x] 회귀·복원 완료 후 생성물·캐시·검증된 보관 사본 정리, 보호 입력/최신 캡처/최소 기록 유지.

전체 자동 표 알고리즘·복잡한 caption wrapper·HTML insertion modes·실제 Windows 두 DPI·1,000문서는 미완료로 유지한다. 증거는 `C:/twf-v25`에 보존한다.

## 2026-10-05 v24 표 열 크기·하단 caption·캐시 통합 실행 계획

- [x] v23 소스·실행기와 새 7문서의 변경 전 실패 보존.
- [x] stylesheet 열 너비·colgroup span·백분율·자동 최소 너비·중첩 표 격리를 공통 처리.
- [x] caption-side 상속과 하단 caption 배치·흐름, 크기/스타일 변경 경계 검증.
- [x] 반복 표 모델 구축·너비 측정 비용 감소, 같은 DOM/앱 자산 교대 성능 측정.
- [x] 두 DPI 새 42쌍·기존 594쌍 불변과 비교기 오류 주입 검증.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 불변 보관·새 폴더 복원 → 전체 636쌍 새 렌더링.

- [x] 회귀·복원 완료 후 컴파일 생성물·캐시·해시가 일치하는 보관 사본 정리, 최신 PNG/JSON·최소 기록·불변 보관본 유지.

전체 자동 표 알고리즘·복잡한 caption wrapper·HTML insertion modes·실제 Windows 두 DPI·1,000문서는 미완료로 유지한다. 증거는 `C:\twf-v24`에 보존한다.

## 2026-10-05 v23 표 오류 복구·범위·속도 통합 실행 기록

- [x] v22 소스·실행기와 새 7문서 before 0/42, 독립 oracle before 146/856 보존.
- [x] table foster parenting·연속 ASCII 공백 버퍼·공통 삽입 위치·서식 재구성/marker 경계.
- [x] 생략된 tbody/tr/cell·section/column/caption·중첩 표·form/select 복구와 자동 너비/top caption 공통 배치·캐시 무효화 검증.
- [x] 표 mode/scope prefix·일반 append/end fast path·compact index, 같은 DOM 15 workload 교대 3회·앱 6회씩 성능 측정. 증가한 시간도 상세 기록.
- [x] 두 DPI 독립 DOM 1,162개·기존 10,494개 불변. 총 594/594(519 strict·기존 backend 75), 새 42 strict·기존 552의 픽셀/전체 JSON/raw/판정 불변. 새 예외 0, registry 111개/허용치 0 유지. 비교기 182+294개 교정.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity.
- [x] 불변 보관·새 폴더 복원/120쌍 재판정 → 전체 594쌍 새 렌더링·별도 복원 증거 ZIP 검증.

마지막 전체 회귀 12개·platform integrity → 불변 보관·새 폴더 복원 → 복원 실행기의 594쌍 새 렌더링까지 완료했다. 증거는 `C:\twf-v23`에 보존한다. 전체 insertion modes/template/나머지 fragment context·전체 CSS 표 너비 계약·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 2026-10-05 v22 서식 재구성·adoption·경계·속도 통합 실행 기록

- [x] v21 소스·실행기와 새 7문서 before 0/42·oracle before 실패 1,238개 보존.
- [x] active formatting 재구성·엇갈린 종료·block adoption/inner loop, 중첩 a/nobr·Noah's Ark·marker/scope·raw/EOF를 공통 처리.
- [x] 중첩 inline 블록 흐름·solid 배경 조각·continuation을 교정하고 parser-local stack index/정상 end fast path·레이아웃 캐시/무효화·스크롤 검증.
- [x] 두 DPI exact oracle 1,444개·이전 9,050개 불변. 총 552/552(477 strict·backend 차이 75), 새 구조 42/42·strict 21/42. 독립 전체 CPU/GPU 합성으로 새 차이 21개만 기존 정책에 등록, 허용치 0·이전 90개 예외 불변. 비교기 182+294개 교정.
- [x] 같은 DOM 13 workload 교대 3회, 앱 순서를 뒤집어 총 6회씩 측정. parser DOM 39쌍·앱 BGRA 24쌍/layout 12쌍 불변. 최초 페인트 114.10→108.03ms, 초기 레이아웃 210.64→218.20ms·스크롤 19.80→20.55ms 증가도 기록.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity.
- [x] 불변 보관·새 폴더 복원/120쌍 재판정 → 전체 552쌍 새 렌더링·별도 복원 증거 ZIP 검증.

정확성·속도 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 552쌍 새 렌더링을 완료했다. 증거는 `C:\twf-v22`에 보존한다. table foster parenting·전체 insertion modes/template/특수 fragment context·일반 split-inline 복잡한 배경·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 2026-10-04 v21 폼·생략 종료·스택 처리 통합 실행 기록

- [x] v20 소스·실행기 보존, 새 6문서 before 0/36·독립 oracle before 324 실패와 경계 원본 보존.
- [x] form pointer·비최상위 제거·scope/재시작, option/optgroup·select·ruby 생략 종료, object fallback·label·스타일/기하 공통 교정.
- [x] 두 DPI exact oracle 918개·추가 경계 166개·이전 7,966개 불변. 총 510/510·새 36 strict·기존 474 전체 불변·새 예외 0. 비교기 182+252개 교정 통과.
- [x] parser prefix/context·matching current end fast path·ignored form 할당/디코딩 생략. 같은 DOM 11 workload는 번갈아 3회씩, 앱은 순서를 뒤집은 추가 3회를 포함해 6회씩 단독 측정, DOM 33쌍·앱 BGRA 24쌍/layout 12쌍 불변. 증가 수치도 기록.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity.
- [x] 불변 보관·새 폴더 복원/120쌍 재판정 → 복원 실행기의 510쌍 새 렌더링·별도 증거 ZIP 검증.

정확성·속도 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 510쌍 새 렌더링을 완료했다. 대용량 증거는 `C:\twf-v21`에 보존한다. 전체 tree builder/adoption agency/table foster parenting·template/폼 소유자·특수 fragment context·실제 ruby 조판·DOCTYPE/PI·나머지 기하/스타일·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 2026-10-04 v20 본문 트리 복구·스코프·속도 통합 실행 기록

- [x] v19 소스·실행기 보존, 새 고정 6문서 before 0/36·독립 oracle before 불일치 1,104개 보존.
- [x] paragraph 자동 닫힘·li/dt/dd·heading·button·일반 종료 태그 스코프 경계 공통 수정. button 블록 콘텐츠·split inline 조각 기하와 실제 View 조회 교정.
- [x] 독립 document/fragment oracle 1,596개·추가 SVG 통합점 32개 두 DPI·이전 oracle 6,338개 불변, 총 474/474·새 36 strict·기존 438 전체 불변·새 예외 0. 기존 비교 교정 182개와 새 오류 주입 252개 통과.
- [x] parser prefix 스코프·paragraph 캐시, 레이아웃 split inline 상대 기하 캐시. 같은 DOM 9 workload·앱을 단독으로 번갈아 3회씩 측정, DOM 27쌍·앱 BGRA 12쌍/layout 6쌍 불변. 증가 수치도 함께 기록.
- [x] 위 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity.
- [x] 불변 보관·새 폴더 복원/120쌍 재판정 → 복원 실행기의 474쌍 새 렌더링·별도 증거 ZIP 검증.

정확성·속도 작업 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 474쌍 새 렌더링을 완료했다. 대용량 증거는 `C:\twf-v20`에 보존한다. 전체 HTML tree builder/adoption agency/table foster parenting·DOCTYPE/PI·나머지 기하/스타일/실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 2026-10-04 v19 주석·선언·데이터 통합 실행 기록

- [x] v18 소스·실행기 보존, 새 고정 5문서 before 0/30·독립 exact DOM 불일치 3,778개 보존.
- [x] 주석 종료/EOF·bogus 선언·HTML DATA NULL·기본 SVG CDATA와 통합점 경계 공통 수정. SVG hidden namespace·inline viewport·text 채움/clip·단순 직접 text range 보강.
- [x] 독립 document/fragment oracle 5,350개 두 DPI·총 438/438·새 30 strict·기존 408 전체 불변·새 예외 0. 기존 비교기 교정 182개와 새 오류 주입 210개 통과.
- [x] 선형 주석 span/dash 스캔·일반 텍스트 복사 감소·NULL 없는 경로 생략. 같은 DOM 9 workload와 앱을 단독으로 번갈아 3회씩 측정; DOM 27쌍·앱 BGRA 12쌍/layout 6쌍 불변. 개선과 증가 수치 모두 결과 문서에 기록.
- [x] 위 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity.
- [x] 불변 보관·새 폴더 복원/120쌍 재판정 → 복원 실행기의 438쌍 새 렌더링·별도 증거 ZIP 검증.

정확성·속도 검증 이후 마지막 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원과 복원 실행기의 438쌍 새 렌더링까지 완료했다. 대용량 증거는 `C:\twf-v19`에 보존한다. 전체 HTML tree builder/DOCTYPE/PI/foreign-content·wrapping/clip/나머지 스타일·문자 기하·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 2026-10-04 v18 태그·속성 토큰화 통합 실행 기록

- [x] v17 소스·실행기 보존, 새 고정 5문서 before 0/30 및 독립 normalized DOM 불일치 686개 보존.
- [x] ASCII 이름·HTML 공백·malformed 구분자·NULL·EOF 공통 수정. raw 종료 scanner 공유와 인접 텍스트 병합.
- [x] 독립 document/fragment oracle 988개 두 DPI·총 408/408·새 30 strict·기존 378 전체 불변·새 예외 0. 기존 비교 교정 182개와 새 오류 주입 210개 통과. WebView2 innerHTML fast-path 차이는 독립 원본/일반 tokenizer 경로로 검증해 별도 기록.
- [x] 중복 속성 value 처리 생략·move 삽입·ASCII 선형 스캔. 같은 DOM 9 workload와 앱을 단독으로 번갈아 3회씩 측정. 속성 다량 입력 파싱 약 16~19% 감소·DOM 27쌍·앱 BGRA 12쌍/layout 6쌍 불변. 최초 페인트 106.33→118.72ms·스크롤 19.96→20.42ms 증가도 기록.
- [x] 위 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity.
- [x] 불변 보관·새 폴더 복원/120쌍 재판정 → 복원 실행기의 408쌍 새 렌더링·별도 증거 ZIP 검증.

정확성·속도 검증 이후 전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원 및 복원 실행기의 408쌍 새 렌더링까지 완료했다. 대용량 증거는 `C:\twf-v18`에 보존한다. 전체 HTML tree builder/나머지 tokenizer·wrapping/clip/스타일·문자 기하·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 2026-10-04 v17 이름·숫자·속성 문자 참조 묶음

- [x] v16 소스·실행기와 새 고정 5문서의 before 0/30, 독립 11,525개 입력의 9,929개 불일치 보존.
- [x] WHATWG 이름 2,231개·longest match·legacy/속성 규칙, 숫자 optional semicolon·Unicode/Windows-1252·오류 보정 공통 수정. raw/CSS/직접 DOM의 literal 값과 단일 디코딩 유지.
- [x] 독립 WebView2 oracle 11,525/11,525를 두 DPI에서 확인. 총 378/378·324 strict·기존 승인 54; 새 30 strict·기존 348의 픽셀/전체 진단/raw 불변·새 예외 0. 기존 비교 교정 182개와 새 오류 주입 210개 통과.
- [x] 정적 trie·literal 구간 복사·무할당 숫자 스캔과 짧은 참조 직접 처리. 동일 DOM 8 workload를 번갈아 3회씩 측정; 짧은 참조 다량 입력 파싱 약 42~44% 감소. 앱 BGRA 12쌍·layout JSON 6쌍 불변. 스크롤 19.89→20.99ms 증가는 함께 기록하며 모든 동작의 속도 향상을 주장하지 않음. 앱 추가 재빌드 없음.
- [x] 위 정확성·속도 검증 후 마지막 전체 회귀 12개·platform integrity.
- [x] 불변 보관·baseline·새 폴더 복원/120쌍 재판정 → 복원 실행기의 전체 378쌍 새 렌더링·별도 증거 ZIP 검증.

전체 회귀 12개·platform integrity, 불변 보관·새 폴더 복원 및 복원 실행기의 378쌍 새 렌더링까지 완료했다. D 드라이브 공간 부족으로 새 대용량 증거는 `C:\twf-v17`에 저장하며 기존 증거는 보존한다. [v17 결과](RENDERING-ACCURACY-V17-RESULTS.md)를 따른다. 전체 HTML tree builder·wrapping/clip/나머지 스타일·문자 기하·실제 Windows 두 DPI·1,000문서·원격 영구 보관은 미완료다.

## 2026-10-04 v15~v16 통합 실행 계획·v16 완료 기록

- [x] 완료된 v15 소스 221개·318쌍을 확인하고 실행기/DLL/성능 실행기 보존. v15의 첫 LF·RCDATA/RAWTEXT·inline pre 결과 유지.
- [x] 새 고정 입력 v1/v2/v3의 before 0/30 보존. script escaped/double-escaped·plaintext·NULL/EOF·NULL-only fragment 자식·잘못된 opener/짧은 주석·hidden UA display 공통 교정. 두 DPI 새 입력 30쌍 strict, 독립 oracle 187/187.
- [x] 선형 script 탐색 유지·bounded entity scan과 capacity 최적화. 같은 DOM인 raw/amp 6 workload와 같은 앱 자산을 번갈아 3회씩 측정. 두 DPI의 실제 앱 oracle로 hidden indicator 제거 확인; 그 외 BGRA 12쌍·layout JSON 6쌍 불변. 앱 추가 재빌드 없음.
- [x] 총 348/348·294 strict·기존 승인 54. 기존 318 픽셀/전체 진단/raw 불변·소스 233개·비교 교정 242개·새 예외 0.
- [x] 위 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 불변 보관/새 폴더 복원/120쌍 재판정 → 복원 실행기의 348쌍 새 렌더링·별도 증거 ZIP 검증.

전체 named entity/HTML 오류 복구·tree builder, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대·원격 영구 보관은 미완료다. 세부 수치·범위·증거는 [v16 기록](RENDERING-ACCURACY-V16-RESULTS.md)을 따른다.

## 2026-10-04 HTML 텍스트 모드·첫 개행 v15

- [x] v14 소스 210개·실행기·DLL을 확인하고 새 고정 4문서 before 0/24를 보존.
- [x] pre/listing/textarea 첫 LF, RCDATA/RAWTEXT·종료 이름/quote·HTML nonvoid slash·fragment 줄바꿈을 공통 수정. inline pre의 line origin/scroll 크기와 fitting hidden textarea LCD 교정. 두 DPI focused/독립 fragment oracle 통과.
- [x] raw 요소마다 전체 소문자 복사 제거. 600 style/script 쌍 파싱 736.48→2.03ms, 동일 DOM. 최종 앱 초기 레이아웃 213.04→203.94ms·스크롤 19.49→20.73ms. BGRA 12쌍·layout JSON 6쌍 불변; 모든 동작의 향상을 주장하지 않음. 앱 추가 재빌드 없음.
- [x] 총 318/318·264 strict·기존 승인 54. 새 24 strict·기존 294 픽셀/전체 진단/raw 불변·소스 221개·비교 교정 230개. 새 예외 없음.
- [x] 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity → 불변 보관/새 폴더 복원/120쌍 재판정 → 복원 실행기의 318쌍 새 렌더링 및 별도 증거 ZIP 검증.

script의 escaped/double-escaped 상태·전체 entity/오류 복구·plaintext 및 나머지 HTML tree builder, 일반 wrapping inline·복잡한 clip/RTL/scroll/gutter, 나머지 계산 스타일·reference glyph/baseline/cluster, 실제 Windows 두 DPI와 독립 화면 캡처, 100→1,000문서 확대·원격 영구 보관은 미완료다. 세부 제한·속도·증거는 [v15 기록](RENDERING-ACCURACY-V15-RESULTS.md)을 따른다.

## 2026-10-04 상대 위치·RTL 넘침·HTML 줄바꿈 v14

- [x] v13 소스 194개·실행기·DLL을 확인하고 새 고정 7문서의 before 0/42를 보존.
- [x] HTML 물리 CR/CRLF 정규화와 entity/직접 DOM CR 구분. 상대 inset·백분율·정상 흐름/후손 좌표·auto z-index 순서·RTL 넓은 상자와 스크롤 크기를 공통 수정.
- [x] 상대 자식 목록·stacking 조건을 캐시하고 restyle/animation에서 갱신. 우선 14쌍 strict, 두 DPI의 독립 pointer/앱 CSS oracle과 relayout·스타일 활성/해제 회귀 통과.
- [x] 최종 v13/v14 번갈아 성능 3회씩: 스크롤 19.90→20.22ms·초기 레이아웃 209.19→212.95ms. 속도 향상을 주장하지 않는다. 실제 앱 `top:2px` 교정 외의 BGRA 12쌍·layout JSON 6쌍 불변을 검증. 앱 추가 재빌드 없음.
- [x] 총 294/294 성공·240 strict·기존 승인 54. 새 42쌍 strict·기존 252쌍 전체 불변·소스 210개 해시·비교 교정 214개 통과. 새 픽셀 예외 없음.
- [x] 위 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity, 주 ZIP·불변 baseline·새 복원 폴더 해시와 120쌍 재판정, 복원 실행기의 294쌍 새 렌더링 및 별도 증거 ZIP 검증.

상대 위치의 고정 조합과 HTML 입력 줄바꿈을 전체 positioned/RTL/파서 지원으로 표시하지 않는다. 세부 제한·성능·증거는 [v14 기록](RENDERING-ACCURACY-V14-RESULTS.md)을 따른다.

## 2026-10-04 사각형 overflow 클립·RTL 강제 scrollbar v13

- [x] v12 저장 소스 184개 해시·실행기·DLL 확인. 별도 고정 4문서의 before 6/24 strict·18 실패와 탐색 입력의 relative/RTL 큰 상자/CRLF 실패를 별도 보존.
- [x] 빈 stable 여백과 실제 scrollbar track을 구분한 사각형 클립을 일반/deferred 페인트·일반/stacking/텍스트 hit-test가 공유. box rect에 상대적인 값으로 캐시하고 스크롤·sticky 이동에서 재사용, 재배치·restyle 때 갱신.
- [x] RTL 강제 세로 scrollbar의 물리 여백을 레이아웃에서 한 번 예약하고 block/grid/textarea 중복 차감 방지. overflow-y 재사용으로 metadata 및 페인트의 스타일 재조회 감소.
- [x] 우선 8쌍 strict와 독립 WebView2 pointer 21개 위치 × 두 DPI 기준·스크롤·재배치 검사 통과. 기존 backend 서명 90개 불변, 새 예외 없음.
- [x] v12/v13 같은 보존 앱 자산에서 번갈아 3회씩 최종 성능 검사. BGRA 12쌍·layout JSON 6쌍·문서 3쌍 불변. 초기 레이아웃 212.42→203.26ms, 첫 드래그 16.81→15.74ms. 스크롤 20.35→20.30ms는 큰 개선으로 해석하지 않으며 최초 페인트 107.30→117.37ms 증가도 기록. 앱 추가 재빌드 없음.
- [x] 총 252/252 성공·198 strict·기존 승인 54. 새 overflow 24쌍 strict, 크기 720개·스타일 4,500개·누락 0. 기존 228쌍의 모든 픽셀·계약 대상 진단·raw 판정 불변; 내부 content/scroll 폭만 강제 상자 6쌍에서 120→108px로 교정하고 WebView2 client − padding과 실제 track 폭으로 검증. 소스 194개·비교기 고의 오류 182개 확인.
- [x] 위 정확성·성능 이후 마지막 전체 회귀 12개·platform integrity 통과. 주 ZIP 9,835개 파일·불변 baseline·새 폴더 복원·소스 194개/입력/실행기/DLL 해시 및 120쌍 재판정 확인. 복원 실행기의 252쌍 새 렌더링에서 최종 v13 픽셀·전체 진단·raw 수치·판정 동일. 복원 증거 ZIP 6,672개 파일도 각각 해시 검증.

사각형 고정 교정을 전체 rounded/transformed/deferred clip이나 나머지 positioned/RTL/HTML 파서 조합의 완료로 표시하지 않는다. 초기 탐색 실패는 승인 예외로 바꾸지 않고 후속 재현 입력으로 보존한다.

## 2026-10-04 stable gutter·RTL 배치 v12

- [x] v11 저장 실행기·DLL·소스 해시를 확인하고 별도 고정 4문서의 변경 전 실패 24쌍을 보존.
- [x] auto/hidden/scroll의 stable 여백과 both-edges의 양쪽 여백을 같은 content 좌표·폭·client 크기·일반 overflow clip에 적용. clip/visible과 폭 0은 유지. RTL scrollbar와 grid column·flex row 방향 공통 수정.
- [x] gutter token flags를 기존 소유권 검증 캐시에 보존하고 일반 visible 상자는 새 계산을 생략. 소스 snapshot과 복원 재렌더링에 새 strict 교정 그룹 포함.
- [x] 같은 보존 앱 자산에서 v11/v12 번갈아 3회씩 측정. BGRA 12쌍·layout JSON 6쌍·문서 3쌍 불변. 스크롤 페인트 20.75→20.18ms, 초기 레이아웃 201.47→207.83ms·최초 페인트 109.29→108.28ms로 모든 항목의 개선을 주장하지 않음. 앱 추가 재빌드 없음.
- [x] 기존 204쌍의 픽셀·진단·raw 수치 불변과 새 gutter 24쌍 strict, 총 228/228 성공·174 strict·기존 승인 54·소스 해시 184개 일치. 새 교정의 크기 864개·스타일 5,400개·누락 0, 새 예외 0. 비교기 고의 오류 182개 통과.
- [x] 위 정확성·속도 이후 마지막 전체 회귀 12개·platform integrity 통과. 주 ZIP 7,143개 파일·불변 baseline·새 폴더 복원·소스 184개/입력/실행기/DLL 해시 확인과 120쌍 재판정 일치. 복원 실행기의 228쌍 새 렌더링에서 픽셀·진단·raw 수치·판정 동일. 복원 증거 ZIP 6,115개 파일도 각각 해시 검증.

고정 교정은 corpus 수량에 합산하지 않는다. intrinsic/nested/textarea/root gutter, positioned/stacking/rounded clip과 나머지 scroll/RTL 조합은 미완료로 유지한다.

## 2026-10-04 재개 v11 — 정확성·속도·마지막 회귀·복원 완료

- [x] v10 저장 소스 169개와 실행기 해시를 확인하고, 미측정이던 v9/v10 성능을 같은 입력·자산으로 번갈아 3회씩 측정. BGRA 12쌍·레이아웃 6쌍 불변.
- [x] glyph가 클립 안에 있으면 반복 coverage 계산을 생략하고 조회표를 직접 참조. grayscale 마스크의 실제 1바이트 할당과 캐시 capacity 한도 보강.
- [x] 마지막 회귀에서 발견한 150% DPI의 DOM 텍스트 재줄바꿈·caret·hit-test·선택 영역 오류를 수정. 기존 기대값을 유지하고 두 DPI 모두 검사하며 실패 좌표 진단 보강.
- [x] 수정한 최종 v10/v11 실행기를 번갈아 3회씩 측정: 스크롤 페인트 21.09→19.94ms(약 5.5% 감소), BGRA 12쌍·레이아웃 6쌍 불변. 최초 페인트는 106.12→107.41ms. 앱 추가 재빌드는 하지 않음.
- [x] 기존 geometry 글자/select 12쌍을 두 DPI·세 viewport 각각의 독립 GPU 실험으로 완전 재현. 동일 LCD 마스크의 CPU 결과는 native, GPU 결과는 reference와 완전히 일치. 승인 정책에 따라 12개 정확한 서명만 추가; raw 21/22·32/49픽셀과 strict 실패 유지.
- [x] 새 실행 원본 120 + style 18 + geometry 18 + capture 18 + table/form 30 = 204/204 성공. 픽셀 완전 일치 150·승인 54. 모든 양쪽 캡처와 DOM·스타일·기하·strict/raw 수치는 v10과 동일.
- [x] 기존 비교기 교정 182개 및 실제 새 예외의 strict/픽셀/크기/스타일/DOM 오류 주입 72개 통과.
- [x] 위 정확성·속도 검증 이후 마지막 전체 회귀 12개·platform integrity 통과. 주 ZIP 20,239개 파일과 불변 baseline, 소스 174개·원본 20문서 해시/복원·120쌍 재판정 동일. 복원 실행기의 204쌍 새 렌더링에서 픽셀·DOM·스타일·기하·strict/raw 수치와 판정 동일. 복원 증거 ZIP 5,558개 파일도 해시 검증.

## 2026-10-04 table·textarea 교정 v10 — 당시 중단 기록

- [x] th 기본 정렬·상속, table-cell 최소 높이와 colspan/rowspan 내부 간격, 암시적 tbody와 본문 끝 인접 텍스트 병합 교정.
- [x] textarea 초기 값의 양축 scrollbar 결합, client/scroll 크기 캐시, 스크롤 좌표·caret·hit-test와 custom thumb 교정.
- [x] DPI별 글자 advance 반올림과 inline 텍스트 중복 정렬 교정. overflow 양축 hidden textarea의 grayscale 합성을 LCD 마스크 평균 후 gamma 보정으로 구현하고 기존 glyph 캐시를 유지.
- [x] 새 5문서 × 2 DPI × 3 viewport = 30쌍 strict 통과. 크기 792개·스타일 4,950개·누락 0, 새 페인트 예외 0.
- [x] 원본 120/120(84 strict·기존 승인 36), 스타일 교정 18/18(12 strict·기존 승인 6), 독립 캡처 교정 18/18 strict, 비교 교정 182개 통과.
- [x] 기존 크기 교정 18/18 구조·크기 통과, strict 6/18. 미등록 페인트 실패 12쌍은 유지하고 성공으로 바꾸지 않음.
- [x] 최종 소스 상태에서 TableSpanRegression·FormControlRegression 빌드/실행 통과. 변경 전·중간 실패와 최종 실행 자료 보존.
- [ ] 준비한 before/after 실행기로 번갈아 3회씩 속도 측정하고 BGRA·레이아웃 불변 확인. 아직 측정 결과 없음.
- [ ] 정확성·속도 작업을 마친 뒤 마지막에 전체 회귀 12개·platform integrity → 보관 → 복원 → 복원한 자료로 204쌍 새 렌더링 검증.

최종 실행은 `TWebFrame2/tests/rendering/runs/20261004-table-form-v10-final`이다. 실제 Windows DPI는 96이며 144는 명시적 렌더링 배율 검증이다. 사용자 중단 요청 이후에는 문서만 갱신했다. 다음 v9 기록의 속도·전체 회귀·복원 성공은 이전 단계 결과다.

## 2026-10-04 CSSOM 크기 정규화·폼 원인 검증

이번 사용자 지시는 정확성을 개선하면서 속도도 고려하고, 전체 회귀검사와 보관·복원 검증은 마지막에 수행하는 것이다. 기존 입력·실행·ZIP은 보존한다. MdViewer 앱을 추가 재빌드하지 않고 같은 앱 자산과 보존한 문서를 공통 View 실행기로 측정한다.

- [x] `clientWidth/clientHeight/scrollWidth/scrollHeight`를 비음수 정수 CSS px로 수집하고 정확히 비교. schema v5에서 계약 버전·값 누락/손상은 HARNESS_ERROR, 한 픽셀 차이는 FAIL_LAYOUT. 숨김 요소와 하위 요소도 검사하며 페인트 예외로 우회하지 않음.
- [x] View의 JavaScript 크기 조회와 렌더링 진단이 같은 공통 계산을 사용. padding/border, scrollbar, inline/hidden/root/table/form을 구분하고 내부 scroll 값은 별도 보존. 하위 overflow를 레이아웃 때 캐시하여 각 조회의 하위 트리 순회를 제거.
- [x] table separate border-spacing과 장치 픽셀 반올림, 같은 줄의 tall inline-block 앞 텍스트 baseline, bare `::-webkit-scrollbar` 선택자, custom scrollbar의 기본 투명 페인트/화살표, 스크롤 descendant clip을 공통 수정.
- [x] textarea 초기 DOM 값·UA 스타일·대체 콘텐츠 처리와 padding 영역의 텍스트 클립, input의 홀수 장치 픽셀 중앙 정렬 수정.
- [x] 기존 폼 6쌍: 동일 좌표·색의 GaneshGL checkbox/button과 D3D11 8×MSAA select 화살표가 두 DPI에서 기준 픽셀과 완전히 일치. CPU 엔진 유지, 기존 72개 예외에 정확한 6개 서명 추가. raw 125/275픽셀·최대 채널 차이 31·strict 실패 유지.
- [x] 원본 120/120와 스타일 교정 18/18, 크기·스타일 누락 0. 기존 비교 교정 134개에 크기 오류 교정 48개 추가, 총 182개 통과.
- [x] 추가 크기 교정 18/18 구조·크기(648개·누락 0), strict 6/18·미등록 페인트 실패 12 유지. 독립 RGB/DPI 캡처 18/18, 최종 번갈아 속도 측정 3회씩과 BGRA 12쌍·레이아웃 6쌍 불변.
- [x] 정확성·교정·속도 검증 후 최종 전체 회귀 12개와 platform integrity 통과. 기본 간격을 0으로 가정하던 과거 table 기대값은 실제 WebView2의 기본 2px과 열 너비로 보강하고 이전 실패도 보존.
- [x] 최종 ZIP 14,352개 파일·소스 157개·원본 입력 20개 해시/복원 및 원본 120쌍 재판정 일치. 복원한 실행기·DLL·입력·계측/비교 코드로 원본 120 + 스타일/크기/색 교정 각 18, 총 174쌍 새 렌더링의 픽셀·DOM·스타일·기하·판정 동일. raw 페인트 실패 12쌍도 재현. 복원 증거 ZIP 4,865개 파일 추가 검증.
- [x] 새 크기 교정의 글자 합성 및 select 경계 페인트 차이 판정 보강은 위 v11에서 완료. 동일 입력 CPU/GPU 원인 실험을 통과한 정확한 12개 서명만 승인하며 원래 strict 실패와 raw 차이를 유지.
- [ ] 양쪽 stable gutter, scroll offset/RTL/transform/pseudo/iframe/root overflow 조합, 나머지 계산 스타일과 reference glyph/baseline·cluster, 실제 Windows 두 DPI·화면 캡처, 100→1,000문서 확장. 추가 table 조사의 th 정렬·긴 셀 최소 높이와 textarea 초기 텍스트의 내부 scroll 동기화·양축 scrollbar 결합은 위 v10 단계에서 교정했다. v10 중단 때 미실행한 속도·전체 회귀·보관·복원은 위 v11 재개 단계에서 완료했다.

진단 문서 3개는 기존 corpus 20개와 별도로 보존·집계한다. 현재 크기 계약의 구현과 해당 교정 성공을 모든 CSSOM 조합의 완료로 표시하지 않는다.

## 2026-10-04 MdViewer 스크롤 성능 최적화

사용자가 TWebFrame2 MdViewer에서 이 문서의 렌더링 및 외부 포커스 후 첫 세로 스크롤 지연을 보고하여 성능 최적화를 다시 요청했다. 이번 요청에는 현재 MdViewer 재빌드와 실제 문서 열기·스크롤 검증을 포함한다. 이전 단계의 성능 조사 취소 및 추가 앱 재빌드 제외는 이 요청에 대해 대체한다.

- [x] 글꼴 face·크기·glyph·래스터 모드·가로 1/4픽셀 위상의 LCD 마스크 캐시. 커닝·advance·오프셋·클립·색상·배율 계산 유지.
- [x] 기존 sRGB 보정표와 5/6/5비트 LCD 합성의 정확한 반올림을 조회표로 재사용. 소수 클립 경계는 기존 수식 유지.
- [x] 한 텍스트 레이아웃의 글자 구간들을 한 CPU 픽셀 잠금에서 합성. 밑줄·취소선·inline object·대체 페인트 전에 잠금 해제 및 원래 순서 복구.
- [x] Skia 모서리/그림자마다 전체 화면을 두 번 복사하던 작업 제거. WIC 표면 및 타깃 소유 리소스 재사용, 크기/DPI 변경·오류 때 무효화.
- [x] 첫 스크롤바 클릭과 thumb 드래그에서 변경 영역을 즉시 WM_PAINT로 갱신. 입력 메시지가 쌓여 페인트를 미루는 경로 보강.
- [x] 실제 앱 HTML/CSS/JS와 변경 전 문서 입력 보존. 1600×1000·96 DPI에서 스크롤 페인트 74.3→21.6ms, 새 WM_PAINT 경로의 첫 클릭/드래그 16~19ms. 최초 페인트 194.6→118.5ms.
- [x] 초기 화면 및 스크롤 3개 지점의 BGRA, 초기/스크롤 레이아웃 JSON 불변. 원본 120개와 추가 스타일 교정 18개의 자체 PNG도 138/138 불변.
- [x] 원본 120/120·필수 스타일 17,400개·누락 0, 비교 교정 134개, 전체 회귀 12개·platform integrity, 독립 색/DPI 교정 18개 통과. 기존 폼 페인트 실패 6쌍 유지.
- [x] 현재 TWebFrame2 MdViewer x64 Release 재빌드 및 런타임 배포. 실제 앱에서 이 문서를 열고 외부 창 포커스 후 첫 드래그와 연속 드래그 확인. 구 TWebFrame MdViewer 실행 파일은 보존.

변경 전 수치는 같은 현재 앱 자산을 넣은 공통 View 실행기의 최적화 직전 CPU 엔진에서 측정했다. 구 TWebFrame 실행 파일과의 정량 비교나, 사용자가 관찰한 2~3초의 지연을 그대로 재현한 수치는 아니다. 전체 계약의 잔여 정확도 작업은 계속 미완료로 유지한다.

## 2026-10-04 사용자 승인에 따른 이번 작업 종료 조건

사용자는 그라디언트와 폰트의 GPU 합성을 CPU로 전환하고, CPU/GPU 렌더링 차이는 기록하여 예외로 허용하고 성공으로 처리하도록 지시했다. 최신 지시는 “최대한 픽셀 100%를 조건으로 하지만 폰트렌더링, 그라디에이션과 같은 CPU/GPU 렌더링에서 불일치가 오는 경우는 예외로 허용”이다. GPU 내부에서 전체 화면 합성을 끝내는 구조는 후속 업그레이드로 남긴다.

이 지시는 이전의 픽셀 완전 일치만 허용하는 종료 조건과 그라디언트 6쌍만의 일회성 승인을 대체한다. 기본 목표는 RGBA 완전 일치이며, 확인된 폰트·그라디언트 CPU/GPU 차이는 `backend-pixel-exceptions.json`에 원인·양쪽 디코딩 픽셀 해시·차이 수치를 등록한다. DOM·스타일·레이아웃·문자 기하·캡처 검사를 통과하고 등록된 차이가 그대로 재현되면 `PASS_BACKEND_DIFFERENCE`로 성공 판정한다. `differentPixels`, 최대 채널 차이와 `strictPass`는 그대로 기록한다. 새 차이가 추가되거나 비페인트 오류가 있으면 실패한다. `-StrictPixels`로 예외를 끈 진단도 가능하다. 수량·기능 coverage·실제 DPI 등 나머지 전체 계약은 유지한다.

최종 순서는 CPU 합성 적용 → 원본 120쌍의 raw 차이와 비페인트 검사 확인 → CPU/GPU 차이 등록 및 재판정 → 전체 회귀검사 → 다른 GPU 경로 조사 → 실행 자료 보존·복원이다.

MdViewer 재빌드는 이번 작업에만 한정한다. 사용자의 별도 요청 없이 이후 TWebFrame2 변경 때 MdViewer를 자동으로 계속 재빌드하지 않는다.

이후 사용자 지시: 모든 GPU 경로를 CPU로 복구하고 도형의 안티앨리어싱 차이도 같은 방식으로 성공·예외 처리한다. 폰트 성능 조사 요청은 사용자가 정상 동작을 확인한 뒤 취소했고 MdViewer 건은 제외했다. 이후 코드 보기의 세로 스크롤바 버그는 TWebFrame2에서 수정하도록 요청했다. 앱 소스 변경과 MdViewer 추가 재빌드는 하지 않고 실제 앱 CSS를 복사한 엔진 재현으로 검증한다.

후속 업그레이드는 View의 최종 합성 표면을 GPU에 유지하고 gradient·clip·border·shadow·text를 같은 표면에서 합성한 뒤 화면에 제시하는 구조다. 도형별 GPU→CPU 읽어오기를 없애고, CPU 읽어오기는 snapshot/출력 등 필요한 시점에만 수행한다. 변경 영역, 표면/리소스 재사용, 화면 크기·스크롤·연속 갱신의 프레임 시간과 전송량을 검증한다. 지금의 CPU 그라디언트와 현재 정확도 자료를 비교 기준으로 보존한다.

## 2026-10-04 계산 스타일 초기값·누락 검출

- [x] schema v4에서 25개 필수 계산 스타일과 계약 버전 검사. 빈 값·누락은 실패이며 기존 캡처의 건너뛴 값도 명시적으로 기록.
- [x] 실제 layout/paint 초기값·길이·색 해석으로 native 진단 보강. raw 스타일 보존, display:none 및 하위 요소의 상속과 상자 유무 분리.
- [x] SVG/input/select UA overflow, background currentColor 파서, RTL 고정 폭 블록 배치를 공통 수정.
- [x] 원본 120/120 성공(84 exact·기존 승인 36). 필수 스타일 17,400개·누락 0, 자체 픽셀 120쌍 불변, 독립 reference 캡처 120/120 일치.
- [x] 초기값/상속/숨김/UA 교정의 스타일·구조 18/18와 기대값 660/660, 별도 색/DPI 캡처 교정 18/18·357 표본, 고의 오류 134개 통과.
- [x] 기존 전체 회귀 12개와 platform integrity 통과. 실제 NVIDIA 기준 GPU 기록 12개 안정성 확인, Intel 기준 보존.
- [x] 소스 140개와 원본 20개 해시, 최종 3,867개 파일 ZIP 및 불변 baseline 보존.
- [x] 최종 파일/소스 복원과 120쌍 재판정. 복원한 실행기로 원본 120쌍 + 스타일 교정 18쌍 새 렌더링, 픽셀/DOM/스타일/기하/GPU/판정 동일. strict 교정의 실패 6쌍도 동일하게 재현, 별도 증거 4,014개 파일 보존/검증.
- [x] 새 폼 교정의 페인트 6쌍 원인 분류는 다음 CSSOM 단계에서 독립 GPU 실험으로 완료. strict 12/18은 보존하며 확인된 6쌍만 승인. 회색 경계 차이만으로 AA 예외를 추가하지 않음.
- [ ] 나머지 계산 스타일, 추가 client/scroll 조합과 reference glyph/baseline·cluster·실제 Windows 두 DPI·화면 캡처 통합. 기본 크기 4개 계약은 다음 CSSOM 단계에서 구현.

현재 범위와 새 실패, 복원 증거는 [스타일 검증 결과](RENDERING-ACCURACY-STYLE-RESULTS.md)를 따른다. 25개 필수 속성 완료를 전체 계산 스타일 계약 완료로 표시하지 않는다.

## 2026-10-04 독립 캡처·기준 그래픽 환경 교정

- [x] 모든 새 캡처에서 `CapturePreview`와 CDP PNG를 비교. schema v3의 누락·손상·다른 크기/픽셀은 실행기 오류이며 backend 예외로 통과시키지 않음.
- [x] 같은 브라우저의 내부 GPU 페이지에서 프로세스 전후의 실제 활성 GPU·ANGLE/Skia·feature status·display/color 정보를 수집. 12개 fingerprint 안정성을 검사하고 기준 환경 식별자에 포함.
- [x] 독립 RGB·source-over·1/2/100 CSS px·소수 상자·viewport/fixed 교정 3문서 × 6행렬, 18/18 strict 통과·표본 357/357 일치. corpus 수량에 합산하지 않음.
- [x] 기준 픽셀 환경 변화 조사: 보관 실행기의 대표 8쌍이 새 기준을 재현. 자체 120쌍 픽셀과 기준 DOM/스타일/좌표는 불변. 이전 기준 GPU는 미계측으로 남기고 새 환경을 별도로 보존.
- [x] 이미 승인한 기능의 새 환경 서명 24개만 추가. 원래 48개 유지, 일반 오차 허용치 0. 위험한 환경 전이의 거절 교정 6개 통과.
- [x] 최종 120/120 성공(96 exact·24 승인), 캡처 경로 120/120·문자 기하 24/24·실제 96 DPI 출력 60/60·비교 교정 47개 통과.
- [x] 최종 아카이브 3,258개 파일·소스 129개 복원/해시 확인, 복원한 코드로 120쌍 동일 재판정. 이전 raw 실패 실행도 별도 아카이브로 보존.
- [x] 복원한 실행기·DLL·입력·계측/비교 코드로 전체 120쌍 새 렌더링. 기준/자체/CDP 픽셀·DOM/스타일/문자 기하·GPU fingerprint·판정 동일. 복원 증거 2,494개 파일을 별도 ZIP/catalog로 보존·검증.

이 단계 이후 native 필수 계산 스타일 25개의 초기값/누락 검출을 보강했다. 다음은 새 폼 페인트 6쌍 원인 분류/수정과 client/scroll 정규화 → 나머지 계산 스타일과 reference 글자별 glyph/baseline·cluster coverage → 실제 Windows 두 DPI 및 화면 캡처 통합 → 100문서 단계다. 전체 계약의 미완료 항목을 현재 pilot 통과로 완료 처리하지 않는다. 상세 수치와 환경 변화의 한계는 [교정 결과](RENDERING-ACCURACY-CAPTURE-CALIBRATION-RESULTS.md)를 따른다.

## 1. 목표와 이번 범위

동일한 HTML·CSS·리소스를 WebView2와 TWebFrame에서 렌더링하고, DOM 구조, 스타일, 레이아웃, 최종 화면을 비교한다. **최대한 모든 장치 픽셀의 RGBA 100% 일치를 목표로 하되, 확인·등록된 폰트·그라디언트·도형 안티앨리어싱 CPU/GPU 렌더링 차이는 허용한다.** 문서는 누적 1,000개를 만들며, 100%와 150% DPI에서 모두 검사한다. 실패와 허용 예외는 문서별로 추적하되 수정은 공통 엔진에 반영한다.

| 항목 | 이번 검증 계약 |
| --- | --- |
| 기준 엔진 | 실행 환경에 고정한 실제 WebView2 런타임. Chromium 일반 브라우저의 결과로 대체하지 않는다. |
| 입력 | 스크립트 없는 정적 HTML·CSS, 로컬 글꼴·이미지·SVG·iframe 문서. 두 엔진에 동일한 파일과 URL 의미를 제공한다. |
| 복잡도 | 단일 기능 진단 문서와 자유로운 복합 레이아웃을 함께 구성한다. 중첩 flex/grid/table/block, 긴 텍스트, 겹침, overflow, 변형, 반응형 조건을 조합한다. |
| 비교 | DOM → 계산 스타일 → CSS 좌표의 레이아웃 → 실제 장치 픽셀의 페인트 순서로 원인을 찾는다. 최종 통과에는 레이아웃과 페인트가 모두 일치해야 한다. |
| 배율 | 기본 배율 두 개는 96 DPI(100%), 144 DPI(150%). 페이지 확대는 두 환경 모두 `ZoomFactor = 1.0`. 브라우저 페이지 확대 검사는 별도 항목으로 관리한다. |
| 보존 | 생성 문서, 리소스, seed, 생성기 버전, manifest, 기준 결과, 실패 결과와 최소 재현 문서를 보존한다. 재실행 시 삭제하거나 덮어쓰지 않는다. |
| 수정 | HTML 파서·DOM, 공통 CSS 처리, 레이아웃, 글꼴·이미지·페인트, View의 DPI 경로를 수정한다. 문서 ID나 특정 문자열에 따른 우회 처리를 넣지 않는다. |
| JavaScript | 페이지 스크립트와 JavaScript에 의한 DOM 생성·변경·이벤트 동작은 이번 corpus에서 제외하고 후속 단계로 둔다. 공통 JavaScript 실행기는 이번 실패의 원인이 실제 공유 코드로 확인된 경우에만 수정하고 기존 회귀검사로 확인한다. |

1,000개는 무한한 HTML/CSS 조합을 모두 열거하는 수량이 아니다. WebView2 지원 기능 목록을 바탕으로 **값의 경계 조건, 기능 쌍의 조합, 중요한 세 기능 조합, 깊은 중첩 구조**를 체계적으로 선정한다. 검사한 범위와 검사하지 않은 범위를 별도로 공개하며, 1,000개 통과를 전체 CSS 표준의 100% 지원으로 표현하지 않는다.

## 2. 기존 자산과 보완할 부분

| 기존 자산 | 활용 방법과 한계 |
| --- | --- |
| `TWebFrame2/tests/CSSCompatibilityRegression.cpp` | WebView2 연결·런타임 버전 기록·공통 CSS 사례를 재사용한다. 현재 비교는 일부 요소의 가로 크기와 색상이며 전체 화면의 픽셀 비교가 아니다. 기존 70개 사례와 새 문서 수량을 구분한다. |
| `TWebFrame2/tests/ScrollRenderingRegression.cpp` | 명시적 DPI의 Direct2D/DirectWrite 래스터 타깃, 재배치, 부분 갱신 검사를 참고한다. 내부 Layout 경로만의 성공을 실제 View 전체 성공으로 간주하지 않는다. |
| `TWebFrame2/include/TWebFrame/TWebFrame.h` | `SetPageScriptsEnabled(false)`로 페이지 스크립트를 비활성화한다. `DumpLayoutJson()`은 정수 좌표 중심이므로 소수 좌표·텍스트 조각을 제공하는 별도 진단 출력을 준비한다. |
| `tools/Capture-TWebFrame.ps1` | 기존 특정 GUI의 Win32 캡처 절차를 참고한다. 고정 대기 시간·화면 좌표 클릭에 의존하므로 1,000개 문서를 처리하는 전용 실행기가 필요하다. |
| `tools/Compare-Screenshots.ps1` | 진단용 이미지 차이 표시를 참고한다. 기본 RGB 허용치 24를 화면 전체에 적용하는 현재 방식은 이번 통과 판정에 사용하지 않는다. |
| `TWebFrame2/tests/Run-SupportCompatibility.ps1` | 엔진 수정 뒤 기존 전체 회귀검사를 실행한다. 새 비교 실행기 연동은 단계적으로 추가한다. |

현재 구현과 남은 기능은 [CSS 엔진과 WebView2 비교](CSS-WEBVIEW2-COMPATIBILITY.md)에 기록되어 있다. 기존 `tests/artifacts`는 기본적으로 Git에서 제외되므로 영구 corpus를 그 아래에 만들지 않는다. 과거 보존 자료와 `TWebFrame2/src.zip`도 건드리지 않는다.

## 3. 재현 환경과 DPI 계약

### 3.1 기준 환경 고정

- 기존 비교에서 확인한 런타임은 `154.0.4258.53`이다. 새 실행기의 시작 시점에 실제 버전을 다시 읽고 그 버전을 기준으로 등록한다.
- 가능하면 Fixed Version WebView2 런타임을 사용한다. Evergreen을 사용하면 실행 전후 버전이 같은지 검사하고, 바뀐 버전의 결과는 별도 환경으로 분리한다. 기존 기준 이미지를 자동으로 갱신하지 않는다.
- 환경 manifest에는 WebView2/SDK 버전, TWebFrame Git SHA와 미커밋 변경의 해시, 실행기 버전, Windows 빌드, GPU·드라이버, 하드웨어/소프트웨어 렌더링 모드, 테마, 색 프로필, DirectWrite·ClearType 설정을 기록한다.
- 사용 글꼴의 파일 해시·버전과 대체 글꼴을 기록한다. Windows 텍스트 크기는 100%로 고정하며 locale, 색상 테마, `prefers-*` 관련 설정도 고정한다.
- 동일한 URL 구조의 로컬 리소스를 사용한다. 외부 네트워크, 날짜·난수에 따른 입력, 원격 글꼴, 동영상, 움직이는 이미지에 의존하지 않는다. 캐시·사용자 프로필·방문 이력·스크롤 복원 상태는 실행별로 격리한다.
- 기준 이미지가 반복 캡처에서 변하면 엔진 불일치로 판단하기 전에 환경 불안정으로 기록한다. 불안정 사례도 삭제하거나 통과 수량에 포함하지 않는다.

### 3.2 CSS 픽셀과 장치 픽셀 분리

| 설정 | 100% | 150% |
| --- | ---: | ---: |
| DPI | 96 | 144 |
| 래스터 배율 | 1.0 | 1.5 |
| 페이지 확대 | 1.0 | 1.0 |
| 예시 CSS viewport | 800 × 600 | 800 × 600 |
| 예시 캡처 크기, 장치 픽셀 | 800 × 600 | 1,200 × 900 |

WebView2 전용 호스트는 Per-Monitor DPI Awareness V2를 사용한다. `ICoreWebView2Controller3`의 자동 모니터 배율 감지를 끄고 명시적 `RasterizationScale`을 설정하며, raw-pixel Bounds에는 CSS viewport에 배율을 곱한 크기를 넣는다. `BoundsMode`, 래스터 배율과 페이지 확대는 서로 구분하여 기록한다. 이 설정의 의미는 [Microsoft의 Controller3 문서](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2controller3?view=webview2-1.0.3595.46)에 따른다.

설정값만 믿지 않고 viewport 메트릭, `devicePixelRatio`, PNG 크기, 1 CSS px 기준선과 100 CSS px 기준 상자의 실제 픽셀 폭을 확인한다. 좌표에 배율을 두 번 적용하거나 150% 이미지를 100% 크기로 리사이즈해서 비교하지 않는다. 소수 경계의 최종 픽셀 스냅도 검사한다.

TWebFrame은 CSS 좌표로 레이아웃하고 최종 타깃에 96/144 DPI를 적용한다. 실제 `View`의 `GetDpiForWindow`, `WM_DPICHANGED`, Direct2D DPI, 클라이언트 크기, 스크롤·클리핑 좌표가 같은 배율을 사용하는지 확인한다. 내부 오프스크린 타깃의 명시적 DPI 검사는 원인 분리에 활용하고, 최종 View 검증은 Windows 배율이 실제 100%·150%인 환경에서 수행한다. 에뮬레이션 결과와 실제 Windows DPI 결과를 구분한다.

같은 CSS viewport의 두 DPI에서 배치가 유지되는지 검사하되, `resolution` 미디어 쿼리·`srcset` 등 DPI에 따라 의도적으로 달라지는 입력은 **각 DPI의 WebView2 결과**와 비교한다. DPI 사이의 PNG를 단순 확대해 기준으로 삼지 않는다.

## 4. 검사 행렬

기본 viewport는 `384 × 768`, `800 × 600`, `1,280 × 800` CSS px로 정한다. 모바일 기기 에뮬레이션은 켜지 않고 viewport 폭에 따른 정적 반응형 동작을 검사한다.

```text
1,000개 문서 × 2개 DPI × 3개 CSS viewport = 기본 6,000개 비교 쌍
각 비교 쌍 = WebView2 기준 캡처 + TWebFrame 캡처 + DOM/스타일/레이아웃 비교
```

- 스크롤 문서는 상단 외에 지정한 중간·하단 및 내부 스크롤 상태를 추가 검사한다. 호스트 입력/스크롤 기능으로 같은 CSS 좌표에 맞추고 측정값을 확인한다. JavaScript로 `scrollTop`이나 DOM을 변경하지 않는다.
- fixed/sticky/overflow는 원래 viewport 크기를 유지한 상태로 캡처한다. 전체 페이지 촬영을 위해 viewport 높이를 늘려 배치를 바꾸지 않는다. 긴 문서는 실제 viewport의 여러 상태로 검사한다.
- 필수 속성별 경계값, 예를 들어 미디어 조건 직전·경계·직후 폭은 사례 manifest에 추가한다. 추가 상태와 viewport 검사 수량은 기본 6,000개와 따로 보고한다.
- 캡처 안정성 반복 검사, 새 프로세스/재사용 프로세스 검사, 100%↔150% 전환 검사는 추가 행렬이다. DPI 전환 후 폰트·이미지·레이아웃 캐시가 올바르게 갱신되는지도 확인한다.
- 마지막 단계의 6,000개 비교는 실제 View 경로와 두 Windows DPI 환경을 포함한다. 오프스크린 내부 검사만 실행한 환경에는 최종 완료 표시를 하지 않는다.

## 5. 영구 문서 1,000개 구성

### 5.1 주 분류별 목표 수량

각 문서는 주 분류를 하나만 갖고, 조합한 모든 기능은 복수 태그로 기록한다. 아래 수량은 정확히 1,000개이며, 초기 발견 결과에 따라 분류 간 수량을 조정하면 manifest와 계획에 변경 이유를 남긴다.

| 주 분류 | 문서 수 | 대표 검사 내용 |
| --- | ---: | --- |
| HTML·DOM·선택자·캐스케이드 | 100 | HTML 오류 복구, 암시적 요소, 공백·엔티티, 목록·폼의 초기 상태, 상속, 우선순위, 변수, layer, nesting, 의사 요소 |
| block·box·고유 크기 | 120 | margin collapse, box sizing, auto/min/max, 백분율, calc, aspect-ratio, float/clear, 이미지의 고유 크기 |
| inline·글꼴·텍스트 | 110 | 한글·라틴·CJK·RTL 혼합, 줄바꿈, whitespace, baseline, line-height, fallback, bidi, writing-mode, decoration |
| flex | 130 | 중첩, wrap, grow/shrink, auto 최소 크기, gap, order, align/baseline, intrinsic 크기와 overflow |
| grid | 140 | auto-placement, minmax/fr, spanning, implicit track, intrinsic track, subgrid 및 다른 배치 문맥과의 결합 |
| table | 80 | auto/fixed layout, colspan/rowspan, caption, collapsed border, 빈 셀, 테이블 안의 flex/grid·긴 텍스트 |
| position·stacking·clip·scroll | 110 | absolute/fixed/sticky, containing block, z-index, 중첩 스크롤, clip/overflow, transform으로 생성한 문맥 |
| 배경·border·이미지·페인트 | 90 | 다중 배경, gradient, radius, shadow, opacity, transform, SVG, object-fit, mask/filter/blend 등 실제 기준 런타임 지원 기능 |
| 반응형·조건·단위 | 70 | media/supports/container 조건, 논리 속성, viewport/container/글꼴 단위, DPI 조건, 선언 fallback |
| 종합 화면 | 50 | 대시보드·문서·카드·사이드바·상품 목록 등 여러 배치 문맥과 복합 페인트를 함께 사용하는 화면 |
| **합계** | **1,000** | |

기준 WebView2에서 지원하지 않는 기능은 별도 목록에 두고 두 엔진의 fallback 동작을 확인한다. TWebFrame에서 미지원이라는 이유로 WebView2가 지원하는 사례를 corpus에서 제거하지 않는다. SVG·정적 iframe·폼 기본 모습·UA 스타일도 기록하며, 지원이 어려운 기능의 실패를 숨기지 않는다.

### 5.2 복잡도와 조합 원칙

| 복잡도 | 문서 수 | 구성 원칙 |
| --- | ---: | --- |
| 단일 원인 진단 | 120 | 실패 원인을 빠르게 구분하는 작은 문서. 소수 크기, 0/auto, 경계값 포함 |
| 복합 기본 | 280 | 두세 기능의 상호작용, 다양한 HTML 구조와 텍스트 길이 |
| 복잡한 레이아웃 | 500 | 서로 다른 기능군 3개 이상과 3단계 이상의 중첩 배치, 다수 요소·페인트 겹침 |
| 스트레스·종합 복합 | 100 | 깊은 중첩, 수백 요소, 긴 콘텐츠, 다중 문맥·스크롤·변형 |
| **합계** | **1,000** | 주 분류 수량과 교차하는 별도 축 |

전체 중 600개는 복잡한 레이아웃 또는 스트레스 문서다. 색상이나 문자열만 바꾼 동일 템플릿 1,000개로 수량을 채우지 않는다. DOM 구조, 배치 문맥, intrinsic sizing, 클리핑·페인트 관계가 실제로 달라지도록 생성한다.

기능 목록에는 구문, 값/단위, 레이아웃 문맥, 부모·자식 관계, 페인트, DPI를 독립 축으로 둔다. 유효한 조합의 pairwise coverage를 생성하고, `grid × intrinsic text × overflow`, `flex × percentage/min-size × wrap`, `transform × sticky × clip`, `writing-mode × logical sizing × table` 같은 주요 세 기능 조합은 명시적으로 추가한다. coverage 분모와 불가능한 조합의 제외 이유를 보존한다.

고정 seed의 제약 있는 생성기와 사람이 작성한 종합 문서를 함께 사용한다. 정상 HTML 중심의 문서와 의도적인 HTML/CSS 오류 복구 문서를 구분한다. 모든 문서에는 UTF-8, doctype/quirks 모드, viewport, 글꼴, 기능 태그, seed, 리소스 해시, 기대하는 관측 항목을 기록한다. 생성된 문서는 파일로 확정하고, 반복 실행 중 다시 생성하지 않는다.

CSS animation/transition은 작성 단계에서 고정 시점의 정적 상태로 만들거나 별도 보류 항목으로 둔다. 전역 CSS 덮어쓰기로 검사 기능을 없애지 않는다. 사용자 입력에 따른 상태 전환, JS DOM 조작, Custom Elements의 스크립트 동작, 동적 Shadow DOM 등은 후속 범위로 기록한다. 이런 보류 범위를 이번 통과율의 분모에 몰래 합치지 않는다.

## 6. 캡처·측정 실행기 설계

전용 네이티브 실행기 `RenderingComparisonRegression`과 PowerShell 실행 스크립트를 만든다. WebView2는 기준을 제공하고, TWebFrame은 자체 엔진으로 렌더링한다. 다음 순서를 문서·DPI·viewport마다 실행한다.

1. 환경과 입력 파일 해시를 확인하고 새 문서/격리된 리소스 상태를 준비한다.
2. 같은 viewport·DPI·배경·테마·글꼴·로컬 URL로 양쪽을 설정한다. 페이지 스크립트는 비활성화한다.
3. HTML 파싱, 스타일시트·이미지·글꼴·iframe 로딩, 레이아웃 및 최종 페인트 완료를 확인한다. 고정 `Sleep`만으로 완료를 결정하지 않는다.
4. 리소스 실패가 없고 DOM/레이아웃 및 연속 캡처가 안정적인지 확인한다. 제한 시간 초과, 잘못된 PNG 크기, 이전 페이지 촬영은 실행기 실패로 처리한다.
5. WebView2 기준 PNG와 DOM/스타일/레이아웃 데이터를 먼저 보존한다. TWebFrame의 같은 데이터를 캡처한다.
6. 정규화와 비교를 수행하고 모든 차이를 파일로 저장한다. 여러 문서 중 하나가 실패해도 전체 실패 목록을 수집한다.
7. 결과를 원자적으로 확정하며, 중단 후에는 완료된 동일 입력/환경 항목만 재사용한다. 실패·미완료를 성공으로 재사용하지 않는다.

WebView2 화면 캡처는 PNG `CapturePreview`를 우선 검증하고, 필요하면 CDP `Page.captureScreenshot`을 비교 검증한다. 캡처 전 현재 navigation의 로딩 완료를 확인하고, 완료 콜백을 기다린다. API와 CDP 호출 순서는 [Microsoft의 ICoreWebView2 문서](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2?view=webview2-1.0.3595.46#capturepreview)에 따른다. 두 캡처 경로의 DPI·색·알파 차이를 교정 문서로 확인한 뒤 정식 경로를 고정한다.

TWebFrame 캡처는 실제 `View`의 공통 페인트 결과를 읽는 방식으로 구현한다. 화면 가림이나 `PrintWindow` 지원 여부에 영향을 받는 캡처는 검증 없이 사용하지 않는다. 테스트용 캡처 훅이 필요하면 같은 페인트·클립·리소스 코드를 사용하고 최종 타깃의 픽셀을 읽는다. 내부 `LayoutEngine` 래스터 캡처는 추가 원인 진단에 사용한다. 두 경로의 렌더링 차이가 있으면 View 통합 오류로 남긴다.

WebView2의 구조 측정에는 `DOMSnapshot.captureSnapshot`과 필요한 DOM/CSS CDP 조회를 사용한다. computed style 목록, DOM rect, paint order, text box는 [공식 DOMSnapshot 프로토콜 정의](https://github.com/ChromeDevTools/devtools-protocol/blob/master/pdl/domains/DOMSnapshot.pdl)에 맞춰 수집하며, 실제 고정 런타임의 프로토콜에서 지원되는지 확인한다. 텍스트 조각 표현은 런타임 버전별로 정규화한다.

계측은 읽기 전용으로 한정한다. CDP만으로 필요한 값이나 글꼴 준비 상태를 얻지 못하면 `getBoundingClientRect`, `getComputedStyle`, `document.fonts.ready` 등 읽기 전용 WebView2 계측을 별도로 허용한다. fixture에는 스크립트를 추가하지 않고 DOM·스타일·스크롤을 변경하지 않는다. 이 계측은 TWebFrame JavaScript 실행기나 JS DOM 제어의 적합성 검사에 포함하지 않는다.

## 7. 일치 판정 기준

### 7.1 DOM·스타일·레이아웃

| 계층 | 판정 및 기록 |
| --- | --- |
| DOM | 정적 파싱 결과의 요소 순서·부모 관계·속성·텍스트·엔티티·암시적 HTML 요소를 비교한다. 스크립트 없이 생성되는 의사 요소와 자식 문서도 대응시킨다. |
| 스타일 | 영향을 주는 계산 스타일, 상속, 변수 해석, 기본 스타일과 cascade 결과를 비교한다. 문자열 표기 차이는 속성 의미를 유지하며 정규화한다. |
| box geometry | border/content rect, margin/padding, 소수 좌표, 변형 전후 경계, scroll/client 크기, clipping 경계를 비교한다. CSS px와 장치 px를 별도 저장한다. |
| 텍스트 | 내용, 사용 글꼴, 줄 수, 줄바꿈 위치, 텍스트 조각의 논리 범위와 위치·advance·baseline을 비교한다. 계측 가능한 값은 직접 비교하고 나머지는 별도 기준 문서/픽셀로 확인한다. |
| 페인트 관계 | stacking context, 상대적인 앞뒤 순서, 의사 요소, 겹침, 가림, 클리핑 결과를 비교한다. 두 엔진 내부의 paint-order 숫자가 같아야 한다고 가정하지 않는다. |

요소에 안정된 `data-case-node` 식별자를 넣고 텍스트 노드·의사 요소·iframe은 경로를 조합하여 대응시킨다. 브라우저별 익명 box의 내부 구조가 달라도 보이는 결과와 의미 있는 텍스트 조각을 비교할 수 있도록 정규화한다. 엔진의 DOM 오류를 정규화로 지우지 않는다.

좌표 JSON의 표현 오차 상한은 **1/64 CSS px**다. 최종 픽셀 차이가 등록된 CPU/GPU 예외에 해당하지 않으면 페인트 실패다. 실제 1px 이동, 잘못된 줄바꿈·글꼴·baseline·overflow는 CPU/GPU 차이로 인정하지 않는다.

### 7.2 픽셀 100% 기본 목표와 CPU/GPU 예외 판정

**텍스트·비텍스트의 RGBA 완전 일치를 기본 목표로 하며, 확인된 폰트·그라디언트·도형 안티앨리어싱 CPU/GPU 차이를 허용 예외로 제외한다.** PNG 파일의 압축 바이트 대신 디코딩한 픽셀을 비교한다. 이미지 이동·리사이즈·블러로 차이를 없애지 않는다.

글꼴은 CPU에서 합성한다. GPU와의 coverage·AA·양자화·합성 차이를 수치와 이미지로 보존하고 원인을 확인한 뒤 등록한다.

- 텍스트 내용·실제 글꼴·줄바꿈·위치·advance·baseline·색상·opacity를 일치시킨다.
- 글리프 coverage·양자화와 배경 합성의 CPU/GPU 차이는 허용 예외로 추적한다.
- 경계·내부·주변 픽셀을 모두 측정한다. 등록된 양쪽 픽셀 해시·문서/DPI/viewport·차이 수치가 재현될 때만 예외를 적용한다.
- 글꼴 교체, 글자 이동·누락, 잘못된 clip, 새 배경·border·그림자 오류는 실패다.

그라디언트·단색 모서리·그림자는 Skia CPU raster-direct, 도형·SVG·폼은 소프트웨어 Direct2D, 폰트는 CPU LCD 합성을 사용한다. 전환으로 확인된 도형/그림자 안티앨리어싱 차이는 `CPU_GPU_RASTER_ANTIALIASING`으로 같은 증거 등록 절차를 적용한다. mask·transform·border·shadow·SVG·이미지도 기본은 픽셀 완전 일치이며, 등록되지 않은 차이는 원인 확인 전까지 실패다. 스크롤 범위 등 실제 레이아웃 오류를 CPU/GPU 예외로 분류하지 않는다.

결과에는 전체 다른 픽셀 수, 최대 채널 차이, 차이 경계, 구조 불일치 목록을 저장한다. `allowedFontAAPixels`와 `allowedBackendDifferencePixels`에 허용 수를 기록하고 `disallowedDifferentPixels`에는 승인 예외를 제외한 차이를 기록한다. **통과 조건은 실행 환경/리소스 검증 성공, DOM·스타일·레이아웃·문자 기하 일치, 예외를 제외한 다른 픽셀 수 0이다.** 차이 0은 `PASS`, 승인된 차이만 있는 결과는 `PASS_BACKEND_DIFFERENCE`로 구분한다. `strictPass`와 raw 차이는 진단으로 보존한다.

### 7.3 비교 도구 자체의 검증

교정 문서는 단색·1px 선·소수 경계·반투명 합성·이미지·텍스트·radius/gradient/clip을 포함한다. 같은 결과끼리 비교하면 통과하고, 다음 고의 오류를 넣으면 반드시 실패해야 한다.

- 상자나 글자 1 장치 px 이동, 한 줄의 줄바꿈 변경.
- 배경·border·글자 색 한 채널 변경, 글꼴 교체.
- 요소/글자 누락, 이미지 잘못된 크기, 잘못된 z-index/clip.
- 알파 잘못된 합성, 150% 캡처의 잘못된 크기 또는 이중 배율 적용.
- 글꼴 AA 경계의 한 픽셀·한 채널을 1 변경한 오류와 텍스트 사각형 안의 비텍스트 오류.

승인된 CPU/GPU 샘플의 성공과 PNG 인코딩 독립성을 확인한다. 같은 승인 샘플에 구조 오류를 추가하거나 색·다른 픽셀·문서/DPI가 달라지면 예외로 통과하지 않아야 한다.

교정·고의 오류 검사가 통과하기 전에는 corpus 통과율을 신뢰할 수 있는 수치로 발표하지 않는다.

## 8. 파일과 결과 보존

다음은 목표 구조다. 현재 실행기, generator, fixtures 20개, coverage, schemas, repros, runs, archives와 불변 baseline 인덱스를 구현했다. 아직 생성하지 않은 1,000개 문서와 진단 overlay 등의 항목은 목표로 남아 있다. 실행 결과의 실제 디렉터리 배치는 [실행 안내](../TWebFrame2/tests/rendering/README.md)를 따른다.

```text
TWebFrame2/tests/rendering/
  README.md
  corpus-manifest.json
  feature-inventory.json
  coverage.json
  schemas/
  generator/                  # 고정 seed, 생성기 버전, 조합 제약
  assets/                     # 글꼴·PNG·SVG·리소스 라이선스와 해시
  fixtures/
    0001-basic-box/
      index.html
      style.css
      case.json
    ...
    1000-complex-layout/
  repros/                     # 실패에서 추출한 최소 재현; 원본은 유지
  baselines/
    <environment-id>/         # WebView2 버전·환경별 불변 기준
      manifest.json
      <case-id>/<dpi>/<viewport>/<state>/
        reference.png
        dom.json
        styles.json
        layout.json
  runs/
    <run-id>/
      environment.json
      input-manifest.json
      summary.json
      summary.html
      failures.json
      <case-id>/<dpi>/<viewport>/<state>/
        native.png
        reference.png
        diff.png
        overlay.png
        font-aa-mask.png
        layout-diff.json
        result.json
        logs.txt

TWebFrame2/tests/RenderingComparisonRegression.cpp
TWebFrame2/tests/RenderingComparisonRegression.vcxproj
TWebFrame2/tests/Run-RenderingComparison.ps1
```

HTML·CSS·manifest·생성기·필수 로컬 리소스는 Git에서 버전 관리한다. 기준 PNG와 실행 결과는 크기를 pilot에서 측정한 뒤 Git LFS 또는 해시 manifest를 갖춘 영구 아카이브로 관리하고, 복원 방법과 보관 위치를 README에 기록한다. 로컬 Git ignore만 설정한 상태를 영구 보존 완료로 간주하지 않는다. 아카이브 방식과 무관하게 자동 삭제·자동 덮어쓰기 정책을 넣지 않는다.

문서 ID는 재사용하지 않는다. 생성 후 입력을 변경해야 하면 이유와 새 해시·버전을 남기고 기존 버전도 보존한다. 최소 재현 문서는 원본을 대체하지 않는다. WebView2 업그레이드 시 새 기준 디렉터리를 만들고 구버전 결과를 유지한다. 수동 기준 갱신도 변경 사유와 전후 차이를 기록한다.

개별 결과는 `PASS`, `FAIL_DOM`, `FAIL_STYLE`, `FAIL_LAYOUT`, `FAIL_TEXT`, `FAIL_PAINT`, `HARNESS_ERROR`, `UNSTABLE_REFERENCE`, `PENDING` 등으로 구분하고 복수 실패 원인도 보존한다. 현재 비교기는 복수 불일치가 있는 결과의 최상위 상태를 `FAIL`로 쓰고 `failures[].kind`에 원인을 기록한다. 미실행·불안정·시간 초과·알려진 미지원은 통과가 아니다. 보고서에는 전체 수량, 실행 수량, 실패, 미실행과 coverage를 함께 표시한다.

## 9. 단계별 실행 계획

문서 수는 **누적 수량**이다. 앞 단계 문서는 다음 단계에서 유지하고 다시 검사한다. 각 단계의 완료 조건이 충족되기 전에는 다음 단계가 완료되었다고 표시하지 않는다.

| 단계 | 작업 | 산출물 | 완료 조건 |
| --- | --- | --- | --- |
| 0. 계약·환경 | 기준 런타임/폰트/색/DPI/viewport/AA 정책 고정, 실제 WebView2 기능 목록과 현재 엔진 차이 정리 | 환경 schema, 기능 목록, 비교 계약 | 기준 버전과 보류 범위가 명확하며 입력·환경을 재현할 수 있음 |
| 1. 캡처·DPI 교정 | 양쪽 실제 렌더 경로 캡처, 로딩 안정성, 소수 좌표 출력, 96/144 DPI와 실제 View 경로 검증 | 실행기 골격, 교정 문서, 캡처/좌표 JSON | 반복 캡처가 안정적이고 픽셀 크기·DPR·CSS 좌표·DPI 전환이 올바름 |
| 2. 판정 도구 | DOM/스타일/레이아웃 정규화, raw pixel diff와 CPU/GPU 예외, HTML 보고서 | diff 도구, 판정 설정, 고의 오류 검사 | 예외를 제외한 다른 픽셀 수 0이며 고의 오류 검사가 통과함 |
| 3. pilot 20개 | 모든 주요 분류의 대표 문서 20개 작성, 복합 화면 포함, 2 DPI × 3 viewport 실행 | 영구 문서 20개, 기본 120 비교 쌍, 최초 실패 목록 | 각 문서에 신뢰할 수 있는 기준·결과·재현 정보가 있음. 환경/실행기 오류가 해결됨 |
| 4. 기초 100개 | 80개 추가, HTML/cascade/box/text/flex/grid/table 기초 상호작용 우선 수정 | 누적 100개, 기본 600 비교 쌍, 공통 엔진 수정과 최소 재현 | 기초 기능의 남은 차이가 해결되고 기존 회귀검사가 통과함 |
| 5. 복합 300개 | 200개 추가, pairwise 생성·중첩·intrinsic sizing·소수 좌표·scroll/clip 확장 | 누적 300개, 기본 1,800 비교 쌍, coverage 및 원인별 목록 | 새 조합의 차이를 공통 수정으로 해결하고 앞 단계의 결과가 유지됨 |
| 6. 고난도 600개 | 300개 추가, writing-mode·고급 grid·조건·transform·SVG·filter 등 확장 | 누적 600개, 기본 3,600 비교 쌍, 기능별 수정 | 고난도 기능의 실패를 해결하고 실제 View·DPI 통합 차이가 해소됨 |
| 7. 전체 1,000개 | 400개 추가, 정해진 분류·복잡도 충족, 실제 Windows 두 DPI에서 전체 실행 | 누적 1,000개, 기본 6,000 비교 쌍과 추가 상태 결과 | 필수 행렬의 미실행·불안정·판정 불가·실패가 0이고 승인된 CPU/GPU 예외를 제외한 픽셀 차이가 0임 |
| 8. 영구 회귀 운영 | 빠른 대표 묶음과 전체 실행 분리, 재실행/복원/업그레이드 절차 검증 | 실행 안내, 보존 검증, 회귀 연동 | 보관한 파일만으로 같은 환경의 전체 검증을 다시 수행하고 동일 결과를 얻음 |
| 후속. JS DOM 동작 | DOM 생성·변경·이벤트·비동기·스타일 변경과 reflow/invalidation 검증 | 별도 계획과 별도 동적 corpus | 현재 정적 검증과 분리하여 추후 진행 |

단계 3의 pilot에서는 실패가 있는 그대로 최초 기준을 확정한다. 이후에는 문서 하나씩 비교하되 동일 원인의 실패를 묶어서 공통 코드를 고친다. 장기 수정이 필요한 기능은 실패 목록에 유지하고 다른 독립 기능의 분석을 진행할 수 있다. 수량이 늘어났다는 이유로 이전 단계의 완료 조건을 충족한 것으로 처리하지 않는다.

단계 3에서 캡처 시간·비교 시간·최대 메모리·PNG/JSON 크기를 측정하여 전체 실행 시간과 보관 용량을 계산한다. 단계 4 이후의 일정은 이 실측과 실패 종류를 기준으로 산정한다. 최초부터 임의의 완료 날짜나 전체 일치 보장을 제시하지 않는다.

## 10. 실패를 공통 엔진 수정으로 연결하는 절차

1. 실패 문서의 HTML/CSS, WebView2 버전, DPI, viewport, 상태, 양쪽 PNG와 차이 이미지를 고정하여 보존한다.
2. 입력/폰트/리소스/환경이 동일한지 확인하고 새 프로세스에서도 재현되는지 확인한다.
3. DOM → computed style → box/text geometry → paint/clip → DPI/View 순서로 최초 차이를 찾는다. 픽셀 차이의 경계와 관련 노드 ID를 연결한다.
4. 원본에서 작은 재현 문서를 추출하고 별도 파일로 보존한다. 관련 기능 조합과 다른 DPI에도 같은 원인이 있는지 확인한다.
5. `DOM.cpp`, 공통 `CSS.cpp` 및 관련 파서, `Layout.cpp`, `View.cpp`의 실제 공통 원인을 수정한다. 공유 JavaScript/DOM 바인딩 코드가 원인이면 그 경로도 수정하되 JS DOM 조작 검증 범위는 확대하지 않는다.
6. 수정 중에는 관련 문서부터 검사한다. 최신 사용자 지시에 따라 폰트·그라디언트의 CPU 합성과 확인된 CPU/GPU 차이의 예외 승인을 적용한다. 원본 모든 DPI/viewport와 기존 통과 확인 → 예외 등록·재판정 → 기존 회귀검사 → 나머지 GPU 경로 조사 → 보존·복원 순서로 수행한다. 기본 100% 목표와 raw 차이는 유지한다.
7. 수정 전후 차이, 영향 기능, 검증 결과, 남은 실패를 기록한다. 해결된 문서를 빠른 회귀 묶음에도 포함한다.

기준 WebView2 이미지를 TWebFrame 결과로 바꾸거나, 실패 문서의 CSS를 단순화하거나, 일반 픽셀 허용치를 높여 통과시키지 않는다. 실제 WebView2 기능 차이와 엔진 기능 차이를 분리하며, 원본 입력이 잘못된 경우에도 기준 엔진의 실제 오류 복구 결과를 먼저 확인한다.

기존 전체 검증은 `TWebFrame2/tests/Run-SupportCompatibility.ps1 -FullRegression`, 관련 개별 회귀검사, x64 Release 빌드로 확인한다. 사용자 지시에 따라 새 branch나 PR을 만들지 않는다. 본 계획 작성은 자동 커밋·푸시를 수행하지 않는다.

## CPU 폰트·그라디언트 작업 완료 (`20261004-cpu-font-final-1791061810315`)

- [x] 폰트 GPU 합성 호출 제거, CPU LCD 합성 적용. 기존 CPU 그라디언트 유지.
- [x] 원본 120쌍: 완전 일치 96, 승인 폰트 18, 승인 그라디언트 6, 실패 0. raw 차이와 strict 판정 보존.
- [x] 비교 교정 36개·문자 기하 24쌍·전체 회귀 12개와 platform integrity·schema 22개 통과.
- [x] MdViewer Release·Debug 이번 재빌드와 core/통합 검사 통과. 이후 자동 재빌드하지 않음.
- [x] 전체 회귀 완료 뒤 도형·모서리·그림자 및 최종 화면의 GPU 경로와 픽셀 회수 조사. 추가 전환은 하지 않음.
- [x] 2,680개 파일과 소스 117개 해시 복원 확인, 복원한 코드로 120쌍 동일 재판정. 이전 기준 유지.

상세 수치, 승인 정책과 후속 GPU 합성 업그레이드는 [CPU 합성 결과](RENDERING-CPU-COMPOSITION-RESULTS.md)를 따른다.

## 이전 CPU 그라디언트 작업 완료 (`20261004-050655-292-8ce5100e`)

- [x] gradient GPU shader/readback 제거와 CPU 생성·합성 적용.
- [x] 원본 120쌍 재검사: strict 114쌍 차이 0, radius/gradient 6쌍 수치·이미지·해시 보존 및 사용자 승인 완료. 이번 잔여 수정 대상 0쌍.
- [x] 최신 WebView2 두 DPI의 select 화살표 기대값과 공유 View 검사 경로 반영.
- [x] 전체 회귀 12개와 platform integrity, 문자 기하 24쌍, 고의 오류 28개 통과.
- [x] source/runtime/폰트 해시와 최초 입력 20개·기준 이미지 120개 보존 확인.
- [x] 3,349개 보관 파일 복원·120쌍 재판정·주요 12쌍 새 렌더링 재현 확인.
- [x] 추가 재현 54쌍의 23 통과/31 strict 실패를 별도 기록하고 승인 예외를 확장하지 않음.
- [x] GPU 안에서 최종 View 합성을 끝내는 구조를 후속 업그레이드로 명시.

아래 체크리스트는 1,000개 전체 계약의 완료 조건이며 이번 사용자 승인 완료와 구분한다.

## 11. 최종 완료 체크리스트

- [ ] 영구 HTML/CSS 문서 1,000개와 로컬 리소스가 존재하고 파일 해시·seed·생성기 버전이 기록되어 있다.
- [ ] 주 분류·복잡도 수량과 기능 조합 coverage가 manifest에 맞으며, 보류·미검사 범위를 공개한다.
- [ ] 실제 View 경로에서 Windows 100%·150% DPI, 페이지 확대 1.0, 세 viewport의 기본 6,000개 비교를 모두 실행했다.
- [ ] 각 문서의 추가 viewport/스크롤 상태도 빠짐없이 실행했다.
- [ ] DOM·스타일·레이아웃·문자 기하가 계약에 맞고, 승인된 CPU/GPU 예외를 제외한 다른 픽셀 수가 모든 필수 결과에서 0이다. raw 차이와 예외 원인은 보존한다.
- [ ] 잘못된 글꼴/색/위치/clip을 넣는 고의 오류 검사가 전부 실패하여 판정 도구의 검출력을 확인했다.
- [ ] 실제 DPI 전환과 반복 실행에서도 결과가 안정적이고 기존 회귀검사가 통과한다.
- [ ] 미지원·실패·미실행·불안정 결과를 성공으로 처리하지 않았으며 남은 필수 실패가 없다.
- [ ] 기준·실행 결과·최소 재현·환경 manifest를 영구 보관했고 새 위치에서 복원하여 재실행했다.
- [ ] JavaScript DOM 제어 검사는 후속 계획으로 유지하며 이번 정적 검증의 완료와 구분한다.

**앞선 table/DPI 실행은 120개 캡처·84 통과/36 실패·보존/복원 검증을 수행했다.** collapsed table 6개가 새로 통과하고 이전 통과 78개는 유지됐다. 원본 입력 20개와 기준 이미지 120개의 해시/픽셀을 유지했다. 그 실행에서 UTF-16 위치·내용·font-box rect 판정과 총 22개 교정 검사를 추가하고 DPI 폰트/inline/grid 경로를 보완했다. 당시 최소 재현 11개 × 2 DPI의 22쌍은 4 통과/18 실패였으며 회귀 실행기 11개 및 platform integrity가 통과했다. 아래 최신 실행은 이 자료를 유지하며 검증 범위를 확장했다.

앞선 실행 `20261003-174213-996-9c018c59`에서는 mixed writing의 kerning과 문자 Range 경계, normal 줄 높이·baseline, 폼 UA 크기·스타일·줄바꿈을 공통 수정했다. 글자 기하 판정은 12/24 → 24/24, 기본 DOM·활성 스타일·추적 상자·글자 기하 실패는 0이며 strict 페인트 36쌍은 남는다. 대표 native glyph/font 정보와 696개 reference 노드의 실제 font usage를 보존했고, 고의 오류 28개·회귀 실행기 12개·platform integrity·schema 22개가 통과했다. 최소 재현은 17개/추가 34쌍이며 4 통과·30 실패를 별도로 보존했다. 3,167개 파일의 아카이브 해시·복원과 전체 120쌍 재판정, 주요 문서 10쌍의 새 렌더링 재현도 검증했다. [상세 결과와 범위](RENDERING-ACCURACY-TEXT-CONTROLS-RESULTS.md)를 따른다.

최신 실행 `20261003-184127-080-f8191e3e`는 GPOS/legacy 우선순위, 한국어 monospace의 실제 굴림체 선택·bitmap 메트릭·DPI cache와 기존 glyph fallback을 수정했다. Segoe UI 및 기존 monospace/grid 재현의 활성 상자·문자 좌표가 두 DPI 모두 통과했다. 기존 monospace/Consolas 동일성 회귀검사는 사용자 요청으로 삭제했고 삭제 후 재검사도 통과했다. 기본 84/120·글자 좌표 24/24를 유지하며 36 페인트 실패는 남는다. 별도 재현 20개/40쌍은 4 통과·36 실패로 보존했다. [상세 결과](RENDERING-ACCURACY-FONT-SELECTION-RESULTS.md)를 따른다.

최신 페인트 실행 `20261003-193005-619-d5320f8d`는 네 모서리 radius와 cascade, gradient padding-box origin, shadow bitmap의 144 DPI source 단위, actual glyph face별 힌팅 모드와 폼 외형·텍스트 위치·select 화살표를 공통 수정했다. 84/120·문자 좌표 24/24를 유지했고 36 실패 중 21쌍의 차이가 감소했다. 총 차이는 412,030 → 305,246픽셀이다. 최소 재현 25개/추가 50쌍은 4 통과·46 실패로 별도 보존했다. 최종 회귀 12개·platform integrity·고의 오류 28개·schema 22개가 통과했다. 원본 입력·기준 PNG·기존 아카이브를 유지했다. [상세 결과와 남은 원인](RENDERING-ACCURACY-PAINT-RESULTS.md)을 따른다.

이전 래스터 실행 `20261003-232442-397-6a3e5f39`는 공유 WIC·DirectWrite LCD·GPU 합성 경로를 검증하고 SVG MSAA coverage·원형 stroke·반투명 native control·select 화살표·button 중앙 정렬·grid 누적 표현 오차를 공통 수정했다. 기본 **102/120**, 문자 기하 **24/24**이며 이전 통과 84개를 유지하고 Latin·혼합 문자·대시보드 18쌍이 새로 통과했다. 총 차이는 305,246 → 40,199픽셀이다. SVG 경계 10/33픽셀, checkbox 경계 1/3픽셀 및 radius/gradient/shadow의 18페인트 실패가 남는다. 최초 작업 상태의 소스·실행기·12쌍도 보존한다. 추가 재현은 27개/54쌍이며 원본과 따로 집계한다. 전체 회귀검사는 10.6에 따라 아직 실행하지 않았다. [상세 결과와 보존 검증](RENDERING-ACCURACY-RASTER-RESULTS.md)을 따른다.

이전 남았던 18쌍은 SVG·폼 12쌍의 strict 수정과 radius/gradient 6쌍의 CPU 전환·승인으로 마무리했다. 그 실행은 strict 114/120과 승인 120/120이며, 최신 CPU 폰트 전환은 완전 일치 96쌍·승인 차이 24쌍을 합쳐 120/120이다. GPU 내부 최종 합성은 후속 업그레이드로 남긴다. ui-monospace/default fallback, 전체 스타일·reference per-character glyph/baseline·다중 cluster coverage·독립 글꼴 AA 교정·실제 Windows 96 DPI/모니터 전환·독립 페인트/색 교정도 남아 있다. 계약과 pilot 차이를 보완한 뒤 100개로 확대한다.

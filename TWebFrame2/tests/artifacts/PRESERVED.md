# 보존한 Cloudflare 수집 스크립트

2026-10-02 임시 검사 자료 정리 시 보안 검사 스크립트의 원본 경로를 유지했다.

- JavaScript 및 Wasm: **3,795개**.
- 스크립트 메타데이터·원본 응답을 포함한 보호 파일: **7,345개**, 약 **1,441.9 MiB**.
- 정리 전후 모든 보호 파일의 길이와 SHA-256이 동일함을 확인했다.
- 파일별 목록과 해시: 이 폴더의 `preserved-scripts-manifest.json`.

주요 경로는 `cloudflare-recapture-20261002-123000`, `cloudflare-stage-comparison-20261002`와 기존 `security-*`, `turnstile-*` 폴더다. 캡처별 `scripts`·`responses`와 식별용 manifest·events·network 메타데이터를 보존했다. 과거의 원본 HTML 응답 및 보안 스크립트 사본도 보존 대상에 포함했다.

회귀검사의 새 출력은 이 폴더에 생성된다. 캡처 스크립트 및 대용량 응답은 로컬 보존 자료로 유지하며 Git에서 제외한다.

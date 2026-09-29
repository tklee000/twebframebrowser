# WebView2 Browser

`TWebFrameBrowser`와 렌더링 결과 및 동작을 비교하기 위한 Win32 WebView2 호스트입니다.
창의 초기 크기, 탭 표시줄, 주소 도구 모음, 상태 표시줄과 키보드 단축키는
`TWebFrameBrowser`와 동일하며 본문 영역만 Microsoft Edge WebView2로 렌더링합니다.

## 빌드

Visual Studio 2019에서 루트의 `Webview2Browser.sln`을 열어 빌드합니다. 프로젝트는
NuGet의 `Microsoft.Web.WebView2` 1.0.3595.46 패키지를 사용하며 WebView2 Runtime이
설치되어 있어야 합니다.

## 실행

주소를 생략하면 `TWebFrameBrowser`와 동일한 비교용 시작 페이지가 열립니다.

```text
WebView2Browser.exe
WebView2Browser.exe https://example.com/
```

지원 단축키: `Ctrl+L`, `Ctrl+T`, `Ctrl+W`, `F5`, `Ctrl+R`, `Alt+Left`, `Alt+Right`.

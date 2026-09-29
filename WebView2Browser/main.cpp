#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kClassName[] = L"WebView2ComparisonBrowser";
constexpr wchar_t kHomeUrl[] =
    L"https://www.ppomppu.co.kr/zboard/view.php?id=freeboard&page=1&divpage=1893&category=2&no=10133266";

enum ControlId : int {
    ID_TABS = 100, ID_BACK, ID_FORWARD, ID_REFRESH, ID_HOME,
    ID_ADDRESS, ID_GO, ID_NEW_TAB, ID_CLOSE_TAB, ID_STATUS,
    ID_FOCUS_ADDRESS, ID_SHORTCUT_NEW_TAB, ID_SHORTCUT_CLOSE_TAB,
    ID_SHORTCUT_BACK, ID_SHORTCUT_FORWARD
};

std::wstring WindowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::wstring Trim(const std::wstring& input) {
    const auto first = std::find_if_not(input.begin(), input.end(), iswspace);
    const auto last = std::find_if_not(input.rbegin(), input.rend(), iswspace).base();
    return first < last ? std::wstring(first, last) : L"";
}

std::wstring SearchUrl(const std::wstring& query) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, query.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 1) return L"https://www.bing.com/";
    std::string utf8(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, query.c_str(), -1, utf8.data(), count,
                        nullptr, nullptr);
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result = L"https://www.bing.com/search?q=";
    for (size_t i = 0; i + 1 < utf8.size(); ++i) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') {
            result.push_back(static_cast<wchar_t>(byte));
        } else {
            result.push_back(L'%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 15]);
        }
    }
    return result;
}

std::wstring ResolveAddress(const std::wstring& input) {
    std::wstring address = Trim(input);
    if (address.size() >= 2 && address.front() == L'"' && address.back() == L'"')
        address = address.substr(1, address.size() - 2);
    if (address.empty()) return L"";
    std::wstring lower = address;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if (lower.rfind(L"https://", 0) == 0 || lower.rfind(L"http://", 0) == 0 ||
        lower.rfind(L"file://", 0) == 0 || lower == L"about:blank")
        return address;
    if (lower.find(L"://") != std::wstring::npos ||
        address.find(L' ') != std::wstring::npos ||
        (address.find(L'.') == std::wstring::npos && lower.rfind(L"localhost", 0) != 0))
        return SearchUrl(address);
    return L"https://" + address;
}

std::wstring TakeCoTaskString(LPWSTR value) {
    std::wstring result = value ? value : L"";
    CoTaskMemFree(value);
    return result;
}

std::wstring WebViewUserDataFolder() {
    PWSTR localAppData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                       nullptr, &localAppData))) {
        path = localAppData;
        CoTaskMemFree(localAppData);
        path += L"\\TWebFrameComparison\\WebView2";
        SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    }
    return path;
}

struct Tab {
    unsigned id = 0;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webView;
    std::wstring pendingUrl = L"about:blank";
    std::wstring url = L"about:blank";
    std::wstring title = L"새 탭";
    std::wstring status = L"WebView2 초기화 중…";
    bool loading = false;
    bool controllerPending = false;
};

class BrowserApp {
public:
    bool Create(HINSTANCE instance, const std::wstring& initial) {
        instance_ = instance;
        initial_ = initial.empty() ? kHomeUrl : ResolveAddress(initial);

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &BrowserApp::WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        HDC screen = GetDC(nullptr);
        const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : USER_DEFAULT_SCREEN_DPI;
        if (screen) ReleaseDC(nullptr, screen);
        const int workWidth = static_cast<int>(std::max<LONG>(1, workArea.right - workArea.left));
        const int workHeight = static_cast<int>(std::max<LONG>(1, workArea.bottom - workArea.top));
        const int windowWidth = std::max(620, std::min(
            MulDiv(1200, dpi, USER_DEFAULT_SCREEN_DPI), workWidth * 85 / 100));
        const int windowHeight = std::max(420, std::min(
            MulDiv(800, dpi, USER_DEFAULT_SCREEN_DPI), workHeight * 85 / 100));
        const int windowX = workArea.left + (workWidth - windowWidth) / 2;
        const int windowY = workArea.top + (workHeight - windowHeight) / 2;

        hwnd_ = CreateWindowExW(0, kClassName, L"WebView2 Browser",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, windowX, windowY,
            windowWidth, windowHeight, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        return true;
    }

    int Run() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!TranslateAcceleratorW(hwnd_, accelerators_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<BrowserApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<BrowserApp*>(
                reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->Handle(message, wparam, lparam) :
                     DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND Button(int id, const wchar_t* label) {
        return CreateWindowExW(0, L"BUTTON", label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 32, 30, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    }

    void InitControls() {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_TAB_CLASSES};
        InitCommonControlsEx(&common);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        tabsControl_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | TCS_FOCUSNEVER, 0, 0, 100, 30,
            hwnd_, reinterpret_cast<HMENU>(ID_TABS), instance_, nullptr);
        back_ = Button(ID_BACK, L"←");
        forward_ = Button(ID_FORWARD, L"→");
        refresh_ = Button(ID_REFRESH, L"⟳");
        home_ = Button(ID_HOME, L"⌂");
        address_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 100, 30, hwnd_, reinterpret_cast<HMENU>(ID_ADDRESS), instance_, nullptr);
        go_ = Button(ID_GO, L"이동");
        newTab_ = Button(ID_NEW_TAB, L"+");
        closeTab_ = Button(ID_CLOSE_TAB, L"×");
        status_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 100, 22, hwnd_, reinterpret_cast<HMENU>(ID_STATUS), instance_, nullptr);
        for (HWND control : {tabsControl_, back_, forward_, refresh_, home_, address_,
                             go_, newTab_, closeTab_, status_})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SetWindowSubclass(address_, &BrowserApp::AddressProc, 1,
                          reinterpret_cast<DWORD_PTR>(this));
        ACCEL keys[] = {
            {FVIRTKEY | FCONTROL, 'L', ID_FOCUS_ADDRESS},
            {FVIRTKEY | FCONTROL, 'T', ID_SHORTCUT_NEW_TAB},
            {FVIRTKEY | FCONTROL, 'W', ID_SHORTCUT_CLOSE_TAB},
            {FVIRTKEY, VK_F5, ID_REFRESH},
            {FVIRTKEY | FCONTROL, 'R', ID_REFRESH},
            {FVIRTKEY | FALT, VK_LEFT, ID_SHORTCUT_BACK},
            {FVIRTKEY | FALT, VK_RIGHT, ID_SHORTCUT_FORWARD}
        };
        accelerators_ = CreateAcceleratorTableW(keys, _countof(keys));
        Layout();
    }

    static LRESULT CALLBACK AddressProc(HWND hwnd, UINT message, WPARAM wparam,
                                        LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto* app = reinterpret_cast<BrowserApp*>(data);
        if (message == WM_KEYDOWN && wparam == VK_RETURN) {
            app->NavigateAddress();
            return 0;
        }
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
            app->RefreshChrome();
            app->FocusPage();
            return 0;
        }
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    RECT PageBounds() const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        return RECT{0, 78, std::max(1L, client.right),
                    std::max(79L, client.bottom - 23)};
    }

    void Layout() {
        if (!tabsControl_) return;
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int width = client.right, height = client.bottom, y = 39;
        MoveWindow(tabsControl_, 4, 4, std::max(100, width - 8), 30, TRUE);
        MoveWindow(back_, 8, y, 32, 31, TRUE);
        MoveWindow(forward_, 44, y, 32, 31, TRUE);
        MoveWindow(refresh_, 80, y, 32, 31, TRUE);
        MoveWindow(home_, 116, y, 32, 31, TRUE);
        MoveWindow(address_, 156, y, std::max(80, width - 290), 31, TRUE);
        MoveWindow(go_, width - 126, y, 42, 31, TRUE);
        MoveWindow(newTab_, width - 78, y, 32, 31, TRUE);
        MoveWindow(closeTab_, width - 42, y, 32, 31, TRUE);
        MoveWindow(status_, 8, height - 22, std::max(1, width - 16), 20, TRUE);
        const RECT page = PageBounds();
        for (const auto& tab : tabs_)
            if (tab->controller) tab->controller->put_Bounds(page);
    }

    std::shared_ptr<Tab> ActiveTab() const {
        const int index = TabCtrl_GetCurSel(tabsControl_);
        return index >= 0 && static_cast<size_t>(index) < tabs_.size() ?
               tabs_[static_cast<size_t>(index)] : nullptr;
    }

    void RefreshTab(const std::shared_ptr<Tab>& tab) {
        const auto found = std::find(tabs_.begin(), tabs_.end(), tab);
        if (found != tabs_.end()) {
            std::wstring label = tab->title.empty() ? L"새 탭" : tab->title;
            if (label.size() > 24) label = label.substr(0, 23) + L"…";
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(label.c_str());
            TabCtrl_SetItem(tabsControl_, static_cast<int>(found - tabs_.begin()), &item);
        }
        if (ActiveTab() == tab) RefreshChrome();
    }

    void RefreshChrome() {
        const auto tab = ActiveTab();
        if (!tab) return;
        BOOL canGoBack = FALSE, canGoForward = FALSE;
        if (tab->webView) {
            tab->webView->get_CanGoBack(&canGoBack);
            tab->webView->get_CanGoForward(&canGoForward);
        }
        EnableWindow(back_, canGoBack);
        EnableWindow(forward_, canGoForward);
        SetWindowTextW(refresh_, tab->loading ? L"×" : L"⟳");
        if (GetFocus() != address_) SetWindowTextW(address_, tab->url.c_str());
        SetWindowTextW(status_, tab->status.c_str());
        const std::wstring title = tab->title.empty() ? L"WebView2 Browser" :
            tab->title + L" - WebView2 Browser";
        SetWindowTextW(hwnd_, title.c_str());
    }

    void Activate(size_t index) {
        if (index >= tabs_.size()) return;
        TabCtrl_SetCurSel(tabsControl_, static_cast<int>(index));
        for (size_t i = 0; i < tabs_.size(); ++i)
            if (tabs_[i]->controller)
                tabs_[i]->controller->put_IsVisible(i == index ? TRUE : FALSE);
        RefreshChrome();
    }

    void BeginWebViewEnvironment() {
        const auto dataFolder = WebViewUserDataFolder();
        const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, dataFolder.empty() ? nullptr : dataFolder.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (closing_) return S_OK;
                    if (FAILED(result) || !environment) {
                        SetEnvironmentError(result);
                        return S_OK;
                    }
                    environment_ = environment;
                    for (const auto& tab : tabs_) CreateController(tab);
                    return S_OK;
                }).Get());
        if (FAILED(started)) SetEnvironmentError(started);
    }

    void SetEnvironmentError(HRESULT error) {
        wchar_t code[32]{};
        swprintf_s(code, L"0x%08X", static_cast<unsigned>(error));
        for (const auto& tab : tabs_) {
            tab->loading = false;
            tab->status = L"WebView2 초기화 실패 (" + std::wstring(code) + L")";
            RefreshTab(tab);
        }
    }

    void AddTab(const std::wstring& value, bool focusAddress = false) {
        auto tab = std::make_shared<Tab>();
        tab->id = ++nextTabId_;
        tab->pendingUrl = ResolveAddress(value.empty() ? L"about:blank" : value);
        if (tab->pendingUrl.empty()) tab->pendingUrl = L"about:blank";
        tab->url = tab->pendingUrl;
        tab->title = tab->url == L"about:blank" ? L"새 탭" : tab->url;

        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tab->title.c_str());
        TabCtrl_InsertItem(tabsControl_, static_cast<int>(tabs_.size()), &item);
        tabs_.push_back(tab);
        Activate(tabs_.size() - 1);
        if (environment_) CreateController(tab);
        if (focusAddress) {
            SetFocus(address_);
            SendMessageW(address_, EM_SETSEL, 0, -1);
        }
    }

    void CreateController(const std::shared_ptr<Tab>& tab) {
        if (!environment_ || !tab || tab->controller || tab->controllerPending) return;
        tab->controllerPending = true;
        std::weak_ptr<Tab> weak = tab;
        const HRESULT started = environment_->CreateCoreWebView2Controller(
            hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    auto current = weak.lock();
                    if (!current || closing_) {
                        if (controller) controller->Close();
                        return S_OK;
                    }
                    current->controllerPending = false;
                    if (FAILED(result) || !controller) {
                        current->status = L"WebView2 컨트롤 생성 실패";
                        current->loading = false;
                        RefreshTab(current);
                        return S_OK;
                    }
                    current->controller = controller;
                    if (FAILED(controller->get_CoreWebView2(&current->webView)) ||
                        !current->webView) {
                        current->status = L"WebView2 본문 생성 실패";
                        current->controller->Close();
                        current->controller.Reset();
                        RefreshTab(current);
                        return S_OK;
                    }
                    current->controller->put_Bounds(PageBounds());
                    current->controller->put_IsVisible(
                        ActiveTab() == current ? TRUE : FALSE);
                    AttachEvents(current);
                    Navigate(current, current->pendingUrl);
                    return S_OK;
                }).Get());
        if (FAILED(started)) {
            tab->controllerPending = false;
            tab->status = L"WebView2 컨트롤 요청 실패";
            RefreshTab(tab);
        }
    }

    void AttachEvents(const std::shared_ptr<Tab>& tab) {
        std::weak_ptr<Tab> weak = tab;
        EventRegistrationToken token{};
        tab->webView->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this, weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    const auto current = weak.lock();
                    if (!current || closing_) return S_OK;
                    LPWSTR uri = nullptr;
                    if (args && SUCCEEDED(args->get_Uri(&uri)))
                        current->url = TakeCoTaskString(uri);
                    current->loading = true;
                    current->status = L"페이지 로드 중…";
                    RefreshTab(current);
                    return S_OK;
                }).Get(), &token);
        tab->webView->add_SourceChanged(
            Callback<ICoreWebView2SourceChangedEventHandler>(
                [this, weak](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                    const auto current = weak.lock();
                    if (!current || closing_) return S_OK;
                    LPWSTR source = nullptr;
                    if (SUCCEEDED(sender->get_Source(&source)))
                        current->url = TakeCoTaskString(source);
                    RefreshTab(current);
                    return S_OK;
                }).Get(), &token);
        tab->webView->add_DocumentTitleChanged(
            Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [this, weak](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                    const auto current = weak.lock();
                    if (!current || closing_) return S_OK;
                    LPWSTR title = nullptr;
                    if (SUCCEEDED(sender->get_DocumentTitle(&title))) {
                        current->title = TakeCoTaskString(title);
                        if (current->title.empty()) current->title = current->url;
                    }
                    RefreshTab(current);
                    return S_OK;
                }).Get(), &token);
        tab->webView->add_HistoryChanged(
            Callback<ICoreWebView2HistoryChangedEventHandler>(
                [this, weak](ICoreWebView2*, IUnknown*) -> HRESULT {
                    const auto current = weak.lock();
                    if (current && !closing_) RefreshTab(current);
                    return S_OK;
                }).Get(), &token);
        tab->webView->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this, weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    const auto current = weak.lock();
                    if (!current || closing_) return S_OK;
                    BOOL success = FALSE;
                    if (args) args->get_IsSuccess(&success);
                    current->loading = false;
                    if (success) {
                        current->status = L"완료";
                    } else {
                        COREWEBVIEW2_WEB_ERROR_STATUS error = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                        if (args) args->get_WebErrorStatus(&error);
                        current->status = L"페이지 로드 실패 (WebView2 오류 " +
                            std::to_wstring(static_cast<int>(error)) + L")";
                    }
                    RefreshTab(current);
                    return S_OK;
                }).Get(), &token);
        tab->webView->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this, weak](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    if (!weak.lock() || closing_ || !args) return S_OK;
                    LPWSTR uri = nullptr;
                    if (SUCCEEDED(args->get_Uri(&uri))) {
                        const auto target = TakeCoTaskString(uri);
                        args->put_Handled(TRUE);
                        AddTab(target);
                    }
                    return S_OK;
                }).Get(), &token);
    }

    void Navigate(const std::shared_ptr<Tab>& tab, const std::wstring& target) {
        if (!tab) return;
        const std::wstring url = ResolveAddress(target);
        if (url.empty()) return;
        tab->pendingUrl = url;
        tab->url = url;
        if (!tab->webView) {
            tab->status = L"WebView2 초기화 중…";
            RefreshTab(tab);
            return;
        }
        tab->loading = true;
        tab->status = L"페이지 로드 중…";
        const HRESULT result = tab->webView->Navigate(url.c_str());
        if (FAILED(result)) {
            tab->loading = false;
            tab->status = L"탐색 요청 실패";
        }
        RefreshTab(tab);
    }

    void NavigateAddress() {
        const auto tab = ActiveTab();
        if (tab) Navigate(tab, WindowText(address_));
        FocusPage();
    }

    void FocusPage() {
        const auto tab = ActiveTab();
        if (tab && tab->controller)
            tab->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

    void StopOrReload() {
        const auto tab = ActiveTab();
        if (!tab || !tab->webView) return;
        if (tab->loading) {
            tab->webView->Stop();
            tab->loading = false;
            tab->status = L"요청 중지됨";
        } else {
            tab->webView->Reload();
        }
        RefreshTab(tab);
    }

    void CloseTab() {
        const int index = TabCtrl_GetCurSel(tabsControl_);
        if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) return;
        auto tab = tabs_[static_cast<size_t>(index)];
        if (tab->controller) tab->controller->Close();
        tab->webView.Reset();
        tab->controller.Reset();
        tabs_.erase(tabs_.begin() + index);
        TabCtrl_DeleteItem(tabsControl_, index);
        if (tabs_.empty()) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return;
        }
        Activate(std::min(static_cast<size_t>(index), tabs_.size() - 1));
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            InitControls();
            AddTab(initial_);
            BeginWebViewEnvironment();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = POINT{620, 420};
            return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == ID_TABS &&
                reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
                const int index = TabCtrl_GetCurSel(tabsControl_);
                if (index >= 0) Activate(static_cast<size_t>(index));
            }
            return 0;
        case WM_COMMAND: {
            const auto tab = ActiveTab();
            switch (LOWORD(wparam)) {
            case ID_BACK:
            case ID_SHORTCUT_BACK: {
                BOOL canGoBack = FALSE;
                if (tab && tab->webView &&
                    SUCCEEDED(tab->webView->get_CanGoBack(&canGoBack)) && canGoBack)
                    tab->webView->GoBack();
                return 0;
            }
            case ID_FORWARD:
            case ID_SHORTCUT_FORWARD: {
                BOOL canGoForward = FALSE;
                if (tab && tab->webView &&
                    SUCCEEDED(tab->webView->get_CanGoForward(&canGoForward)) && canGoForward)
                    tab->webView->GoForward();
                return 0;
            }
            case ID_REFRESH: StopOrReload(); return 0;
            case ID_HOME: if (tab) Navigate(tab, kHomeUrl); return 0;
            case ID_GO: NavigateAddress(); return 0;
            case ID_NEW_TAB:
            case ID_SHORTCUT_NEW_TAB: AddTab(L"about:blank", true); return 0;
            case ID_CLOSE_TAB:
            case ID_SHORTCUT_CLOSE_TAB: CloseTab(); return 0;
            case ID_FOCUS_ADDRESS:
                SetFocus(address_);
                SendMessageW(address_, EM_SETSEL, 0, -1);
                return 0;
            }
            break;
        }
        case WM_DESTROY:
            closing_ = true;
            for (const auto& tab : tabs_)
                if (tab->controller) tab->controller->Close();
            tabs_.clear();
            environment_.Reset();
            if (accelerators_) DestroyAcceleratorTable(accelerators_);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tabsControl_ = nullptr, back_ = nullptr, forward_ = nullptr, refresh_ = nullptr;
    HWND home_ = nullptr, address_ = nullptr, go_ = nullptr, newTab_ = nullptr;
    HWND closeTab_ = nullptr, status_ = nullptr;
    HACCEL accelerators_ = nullptr;
    std::wstring initial_;
    unsigned nextTabId_ = 0;
    bool closing_ = false;
    ComPtr<ICoreWebView2Environment> environment_;
    std::vector<std::shared_ptr<Tab>> tabs_;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDPIAware();
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        MessageBoxW(nullptr, L"COM을 초기화할 수 없습니다.", L"WebView2 Browser",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    BrowserApp app;
    if (!app.Create(instance, commandLine ? commandLine : L"")) {
        MessageBoxW(nullptr, L"브라우저 창을 만들 수 없습니다.", L"WebView2 Browser",
                    MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }
    const int result = app.Run();
    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}

#include "HttpClient.h"
#include "ResourceScheduler.h"
#include <TWebFrame/TWebFrame.h>

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kClassName[] = L"TWebFrameNativeBrowser";
//constexpr wchar_t kHomeUrl[] = L"https://www.ppomppu.co.kr/zboard/view.php?id=freeboard&page=1&divpage=1893&category=2&no=10133266";
//constexpr wchar_t kHomeUrl[] = L"https://cdn4.ppomppu.co.kr/banner/kakao_ad_728x90.html?v=3";
constexpr wchar_t kHomeUrl[] = L"https://www.ppomppu.co.kr/zboard/login.php?s_url=zboard%2Fview.php%3Fid%3Dfreeboard%26page%3D1%26divpage%3D1893%26no%3D10133266%26focus_target%3Dcomment_write_form";
constexpr UINT kFetchFinished = WM_APP + 1;
constexpr UINT kDeferredTabSelection = WM_APP + 2;

bool HasDocumentResponse(const ResourceScheduler::Response& response) {
    return response && response->status >= 200 && response->status < 600 &&
           response->error.empty();
}

enum ControlId : int {
    ID_TABS = 100, ID_BACK, ID_FORWARD, ID_REFRESH, ID_HOME,
    ID_ADDRESS, ID_GO, ID_NEW_TAB, ID_CLOSE_TAB, ID_STATUS,
    ID_FOCUS_ADDRESS, ID_FOCUS_PAGE, ID_SHORTCUT_NEW_TAB, ID_SHORTCUT_CLOSE_TAB,
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

struct LaunchOptions {
    std::wstring initialAddress;
    bool enablePageScripts = true;
};

LaunchOptions ParseLaunchOptions(const std::wstring& commandLine) {
    LaunchOptions options;
    auto remaining = Trim(commandLine);
    constexpr wchar_t disableScripts[] = L"--disable-scripts";
    constexpr size_t disableScriptsLength =
        sizeof(disableScripts) / sizeof(disableScripts[0]) - 1;
    constexpr wchar_t enableScripts[] = L"--enable-scripts";
    constexpr size_t enableScriptsLength =
        sizeof(enableScripts) / sizeof(enableScripts[0]) - 1;
    if (remaining.compare(0, disableScriptsLength, disableScripts) == 0 &&
        (remaining.size() == disableScriptsLength ||
         iswspace(remaining[disableScriptsLength]))) {
        options.enablePageScripts = false;
        remaining = Trim(remaining.substr(disableScriptsLength));
    } else if (remaining.compare(0, enableScriptsLength, enableScripts) == 0 &&
        (remaining.size() == enableScriptsLength ||
         iswspace(remaining[enableScriptsLength]))) {
        options.enablePageScripts = true;
        remaining = Trim(remaining.substr(enableScriptsLength));
    }
    options.initialAddress = std::move(remaining);
    return options;
}

std::wstring SearchUrl(const std::wstring& query) {
    int count = WideCharToMultiByte(CP_UTF8, 0, query.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return L"https://www.bing.com/";
    std::string utf8(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, query.c_str(), -1, utf8.data(), count, nullptr, nullptr);
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    std::wstring result = L"https://www.bing.com/search?q=";
    for (size_t i = 0; i + 1 < utf8.size(); ++i) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~') result.push_back(static_cast<wchar_t>(byte));
        else { result.push_back(L'%'); result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
    }
    return result;
}

std::wstring ResolveAddress(const std::wstring& input) {
    std::wstring address = Trim(input);
    if (address.empty()) return L"";
    std::wstring lower = address;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if (lower.rfind(L"https://", 0) == 0 || lower.rfind(L"http://", 0) == 0 ||
        lower == L"about:blank") return address;
    if (lower.find(L"://") != std::wstring::npos ||
        address.find(L' ') != std::wstring::npos ||
        (address.find(L'.') == std::wstring::npos && lower.rfind(L"localhost", 0) != 0))
        return SearchUrl(address);
    return L"https://" + address;
}

std::wstring ResolveResourceUrl(const std::wstring& base, const std::wstring& reference) {
    auto value = Trim(reference);
    if (value.empty() || value.front() == L'#') return L"";
    std::wstring lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if (lower.rfind(L"data:", 0) == 0 || lower.rfind(L"javascript:", 0) == 0 ||
        lower.rfind(L"about:", 0) == 0) return L"";
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0) return value;
    std::vector<wchar_t> combined(32768, L'\0');
    DWORD length = static_cast<DWORD>(combined.size());
    return SUCCEEDED(UrlCombineW(base.c_str(), value.c_str(), combined.data(), &length, 0)) ?
        std::wstring(combined.data()) : L"";
}

std::wstring TagAttribute(const std::wstring& tag, const std::wstring& name) {
    std::wstring lower = tag;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    size_t position = 0;
    while ((position = lower.find(name, position)) != std::wstring::npos) {
        const bool left = position == 0 || iswspace(lower[position - 1]) || lower[position - 1] == L'<';
        size_t cursor = position + name.size();
        const bool right = cursor >= lower.size() || iswspace(lower[cursor]) || lower[cursor] == L'=';
        if (!left || !right) { position = cursor; continue; }
        while (cursor < lower.size() && iswspace(lower[cursor])) ++cursor;
        if (cursor >= lower.size() || lower[cursor] != L'=') { position = cursor; continue; }
        do { ++cursor; } while (cursor < lower.size() && iswspace(lower[cursor]));
        if (cursor >= tag.size()) return L"";
        wchar_t quote = 0;
        if (tag[cursor] == L'\'' || tag[cursor] == L'"') quote = tag[cursor++];
        const auto start = cursor;
        while (cursor < tag.size() && (quote ? tag[cursor] != quote :
               !iswspace(tag[cursor]) && tag[cursor] != L'>')) ++cursor;
        auto result = tag.substr(start, cursor - start);
        size_t entity = 0;
        while ((entity = result.find(L"&amp;", entity)) != std::wstring::npos)
            result.replace(entity, 5, L"&");
        return result;
    }
    return L"";
}

void AddResource(const std::wstring& base, const std::wstring& value,
                 std::vector<std::wstring>& output,
                 std::unordered_set<std::wstring>& seen) {
    const auto resolved = ResolveResourceUrl(base, value);
    if (!resolved.empty() && seen.insert(resolved).second) output.push_back(resolved);
}

void DiscoverCssUrls(const std::wstring& css, const std::wstring& base,
                     std::vector<std::wstring>& output,
                     std::unordered_set<std::wstring>& seen) {
    std::wstring lower = css;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    size_t position = 0;
    while ((position = lower.find(L"url(", position)) != std::wstring::npos) {
        size_t cursor = position + 4;
        while (cursor < css.size() && iswspace(css[cursor])) ++cursor;
        wchar_t quote = 0;
        if (cursor < css.size() && (css[cursor] == L'\'' || css[cursor] == L'"'))
            quote = css[cursor++];
        const auto start = cursor;
        while (cursor < css.size() && (quote ? css[cursor] != quote : css[cursor] != L')')) ++cursor;
        AddResource(base, css.substr(start, cursor - start), output, seen);
        const auto close = css.find(L')', cursor);
        position = close == std::wstring::npos ? css.size() : close + 1;
    }
}

struct DiscoveredResources {
    std::vector<std::wstring> styles;
    std::vector<std::wstring> scripts;
    std::vector<std::wstring> images;
    std::vector<std::wstring> frames;
};

DiscoveredResources DiscoverResources(const std::wstring& html, const std::wstring& base,
                                      bool includeScripts) {
    DiscoveredResources result;
    std::unordered_set<std::wstring> styleSeen, scriptSeen, imageSeen, frameSeen;
    std::wstring lower = html;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    size_t position = 0;
    while ((position = lower.find(L'<', position)) != std::wstring::npos) {
        const auto end = lower.find(L'>', position + 1);
        if (end == std::wstring::npos) break;
        const auto tag = html.substr(position, end - position + 1);
        size_t nameStart = position + 1;
        while (nameStart < end && iswspace(lower[nameStart])) ++nameStart;
        size_t nameEnd = nameStart;
        while (nameEnd < end && iswalnum(lower[nameEnd])) ++nameEnd;
        const auto name = lower.substr(nameStart, nameEnd - nameStart);
        if (name == L"script") {
            if (includeScripts)
                AddResource(base, TagAttribute(tag, L"src"), result.scripts, scriptSeen);
        }
        else if (name == L"link") {
            auto rel = TagAttribute(tag, L"rel");
            std::transform(rel.begin(), rel.end(), rel.begin(), towlower);
            if (rel.find(L"stylesheet") != std::wstring::npos)
                AddResource(base, TagAttribute(tag, L"href"), result.styles, styleSeen);
        } else if (name == L"iframe")
            AddResource(base, TagAttribute(tag, L"src"), result.frames, frameSeen);
        else if (name == L"img" || name == L"source" || name == L"video" || name == L"audio")
            AddResource(base, TagAttribute(tag, L"src"), result.images, imageSeen);

        const auto style = TagAttribute(tag, L"style");
        if (!style.empty()) DiscoverCssUrls(style, base, result.images, imageSeen);
        if (name == L"style") {
            const auto close = lower.find(L"</style", end + 1);
            if (close != std::wstring::npos) {
                DiscoverCssUrls(html.substr(end + 1, close - end - 1), base,
                                result.images, imageSeen);
                position = close;
                continue;
            }
        }
        position = end + 1;
    }
    return result;
}

std::wstring EscapeHtml(const std::wstring& text) {
    std::wstring result;
    for (wchar_t ch : text) {
        if (ch == L'&') result += L"&amp;";
        else if (ch == L'<') result += L"&lt;";
        else if (ch == L'>') result += L"&gt;";
        else if (ch == L'"') result += L"&quot;";
        else result.push_back(ch);
    }
    return result;
}

struct Tab {
    unsigned id = 0;
    unsigned sequence = 0;
    unsigned renderSequence = 0;
    std::unique_ptr<TWebFrame::View> view;
    std::wstring url = L"about:blank";
    std::wstring title = L"새 탭";
    std::wstring status = L"완료";
    std::vector<std::wstring> history;
    size_t historyIndex = 0;
    bool loading = false;
    ResourceScheduler::CancellationToken navigationCancellation;
    std::shared_ptr<const std::wstring> resourceReferer =
        std::make_shared<const std::wstring>(L"about:blank");
};

struct FetchResult {
    unsigned tabId = 0;
    unsigned sequence = 0;
    std::shared_ptr<const HttpResponse> response;
    std::wstring html;
};

class BrowserApp {
public:
    bool Create(HINSTANCE instance, const std::wstring& initial, bool enablePageScripts) {
        instance_ = instance;
        initial_ = initial.empty() ? kHomeUrl : ResolveAddress(initial);
        pageScriptsEnabled_ = enablePageScripts;
        network_ = std::make_shared<ResourceScheduler>();
        alive_ = std::make_shared<std::atomic_bool>(true);
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
        // Start as a normal, centered window. Use CSS-like logical dimensions
        // at the system DPI, but keep enough desktop visible on smaller screens.
        const int windowWidth = std::max(620, std::min(MulDiv(1200, dpi, USER_DEFAULT_SCREEN_DPI),
                                                       workWidth * 85 / 100));
        const int windowHeight = std::max(420, std::min(MulDiv(800, dpi, USER_DEFAULT_SCREEN_DPI),
                                                        workHeight * 85 / 100));
        const int windowX = workArea.left + (workWidth - windowWidth) / 2;
        const int windowY = workArea.top + (workHeight - windowHeight) / 2;
        hwnd_ = CreateWindowExW(0, kClassName, L"TWebFrame Browser",
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
            app = static_cast<BrowserApp*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    HWND Button(int id, const wchar_t* label) {
        return CreateWindowExW(0, L"BUTTON", label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 32, 30, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
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
        SetWindowSubclass(address_, &BrowserApp::AddressProc, 1, reinterpret_cast<DWORD_PTR>(this));
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

    static LRESULT CALLBACK AddressProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                        UINT_PTR, DWORD_PTR data) {
        auto* app = reinterpret_cast<BrowserApp*>(data);
        if (message == WM_KEYDOWN && wparam == VK_RETURN) {
            if(app->servicingChrome_){
                PostMessageW(app->hwnd_,WM_COMMAND,ID_GO,0);app->pendingChromeAction_=true;return 0;
            }
            app->NavigateAddress();
            return 0;
        }
        if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
            if(app->servicingChrome_){
                PostMessageW(app->hwnd_,WM_COMMAND,ID_FOCUS_PAGE,0);app->pendingChromeAction_=true;return 0;
            }
            app->RefreshChrome();
            app->FocusPage();
            return 0;
        }
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    RECT PageBounds() const {
        RECT client{};
        GetClientRect(hwnd_, &client);
        return RECT{0, 78, std::max(1L, client.right), std::max(79L, client.bottom - 23)};
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
        for (auto& tab : tabs_) if (tab->view) tab->view->SetBounds(page);
    }

    std::shared_ptr<Tab> ActiveTab() const {
        int index = TabCtrl_GetCurSel(tabsControl_);
        return index >= 0 && static_cast<size_t>(index) < tabs_.size() ? tabs_[index] : nullptr;
    }

    void RefreshTab(const std::shared_ptr<Tab>& tab) {
        auto found = std::find(tabs_.begin(), tabs_.end(), tab);
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
        auto tab = ActiveTab();
        if (!tab) return;
        EnableWindow(back_, tab->historyIndex > 0);
        EnableWindow(forward_, tab->historyIndex + 1 < tab->history.size());
        SetWindowTextW(refresh_, tab->loading ? L"×" : L"⟳");
        if (GetFocus() != address_) SetWindowTextW(address_, tab->url.c_str());
        SetWindowTextW(status_, tab->status.c_str());
        std::wstring title = tab->title.empty() ? L"TWebFrame Browser" :
            tab->title + L" - TWebFrame Browser";
        SetWindowTextW(hwnd_, title.c_str());
    }

    void Activate(size_t index) {
        if (index >= tabs_.size()) return;
        TabCtrl_SetCurSel(tabsControl_, static_cast<int>(index));
        for (size_t i = 0; i < tabs_.size(); ++i)
            if (tabs_[i]->view) tabs_[i]->view->SetVisible(i == index);
        RefreshChrome();
    }

    void AddTab(const std::wstring& url, bool focusAddress = false,const std::wstring& initiator=L"") {
        auto tab = std::make_shared<Tab>();
        tab->id = ++nextTabId_;
        std::weak_ptr<Tab> weak = tab;
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tab->title.c_str());
        TabCtrl_InsertItem(tabsControl_, static_cast<int>(tabs_.size()), &item);
        tabs_.push_back(tab);
        tab->view = TWebFrame::View::Create(hwnd_, PageBounds());
        if (!tab->view) {
            tab->status = L"TWebFrame 보기를 만들 수 없습니다.";
            RefreshTab(tab);
            return;
        }
        tab->view->SetParallelResourceLoading(true);
        tab->view->SetBrowserContext(network_->Context());
        tab->view->SetCompatibilityBridgeEnabled(false);
        tab->view->SetExecutionYieldHandler([this]{return ServiceChromeDuringScript();});
        // Script execution is enabled for normal browser compatibility. The
        // --disable-scripts launch option disables page scripts without
        // changing URL, stylesheet, iframe or form behavior.
        tab->view->SetPageScriptsEnabled(pageScriptsEnabled_);
        auto network = network_;
        tab->view->SetNetworkResourceLoader([network](const TWebFrame::NetworkRequest& request){
            const auto response=network->Get(request);TWebFrame::NetworkResponse result;
            if(response){
                result.status=response->status;result.url=response->url;
                result.headers=response->headers;result.contentType=response->contentType;
                result.body=HttpClient::DecodeText(*response);result.bytes=response->body;result.error=response->error;
            }
            return result;
        });
        tab->view->SetResourceLoader([network, weak](const std::wstring& target, std::wstring& text) {
            auto current = weak.lock();
            if (!current) return false;
            const auto referer = std::atomic_load(&current->resourceReferer);
            auto response = network->Get(target, referer ? *referer : L"");
            if (!response || !response->Ok()) return false;
            text = HttpClient::DecodeText(*response);
            return true;
        });
        tab->view->SetBinaryResourceLoader([network, weak](const std::wstring& target,
                                                          std::vector<unsigned char>& bytes) {
            auto current = weak.lock();
            if (!current) return false;
            const auto referer = std::atomic_load(&current->resourceReferer);
            auto response = network->Get(target, referer ? *referer : L"");
            if (!response || !response->Ok()) return false;
            bytes = response->body;
            return true;
        });
        tab->view->SetNavigationHandler([this, weak](const std::wstring& target, bool newWindow) {
            if (auto current = weak.lock()){
                const auto initiator=current->url;
                if(newWindow)AddTab(target,false,initiator);
                else Navigate(current,target,true,initiator);
            }
        });
        tab->view->SetHistoryChangedHandler([this,weak](const std::wstring& target,bool replace,int delta){
            if(const auto current=weak.lock()){
                if(delta){
                    const auto index=static_cast<long long>(current->historyIndex)+delta;
                    if(index>=0&&index<static_cast<long long>(current->history.size()))current->historyIndex=static_cast<size_t>(index);
                }else if(replace){
                    if(!current->history.empty())current->history[current->historyIndex]=target;
                }else{
                    current->history.resize(current->historyIndex+1);current->history.push_back(target);++current->historyIndex;
                }
                current->url=target;
                std::atomic_store(&current->resourceReferer,std::make_shared<const std::wstring>(target));
                if(ActiveTab()==current)RefreshChrome();
            }
        });
        tab->view->SetHistoryTraversalHandler([this,weak](int delta){
            if(const auto current=weak.lock()){
                const auto index=static_cast<long long>(current->historyIndex)+delta;
                if(index>=0&&index<static_cast<long long>(current->history.size())){
                    current->historyIndex=static_cast<size_t>(index);Navigate(current,current->history[current->historyIndex],false);
                }
            }
        });
        tab->view->SetLoadHandler([this, weak](bool success, const std::wstring& error) {
            if (auto current = weak.lock()) {
                if (current->renderSequence != current->sequence) return;
                current->loading = false;
                if (!success) current->status = error;
                else {
                    current->title = current->view->DocumentTitle();
                    if (current->title.empty()) current->title = current->url;
                    current->status = L"완료";
                }
                RefreshTab(current);
            }
        });
        Activate(tabs_.size() - 1);
        Navigate(tab, url.empty() ? L"about:blank" : url, true,initiator);
        if (focusAddress) {
            SetFocus(address_);
            SendMessageW(address_, EM_SETSEL, 0, -1);
        }
    }

    void Navigate(const std::shared_ptr<Tab>& tab, const std::wstring& target, bool addHistory,const std::wstring& initiator=L"") {
        if (!tab || !tab->view) return;
        std::wstring url = ResolveAddress(target);
        if (url.empty()) return;
        if (tab->navigationCancellation)
            tab->navigationCancellation->store(true, std::memory_order_relaxed);
        tab->view->CancelPendingLoads();
        tab->navigationCancellation = std::make_shared<std::atomic_bool>(false);
        const auto cancellation = tab->navigationCancellation;
        if (addHistory) {
            if (tab->historyIndex + 1 < tab->history.size())
                tab->history.erase(tab->history.begin() + tab->historyIndex + 1, tab->history.end());
            tab->history.push_back(url);
            tab->historyIndex = tab->history.size() - 1;
        }
        tab->url = url;
        std::atomic_store(&tab->resourceReferer,
                          std::make_shared<const std::wstring>(url));
        ++tab->sequence;
        tab->loading = url != L"about:blank";
        tab->status = tab->loading ? L"HTTPS 페이지 요청 중…" : L"완료";
        tab->title = url == L"about:blank" ? L"새 탭" : url;
        RefreshTab(tab);
        if (!tab->loading) {
            tab->renderSequence = tab->sequence;
            tab->view->NavigateToString(L"<html><head><title>새 탭</title></head><body></body></html>", url);
            return;
        }
        HWND destination = hwnd_;auto network = network_;auto alive = alive_;
        const bool pageScriptsEnabled = pageScriptsEnabled_;
        unsigned id = tab->id, sequence = tab->sequence;
        const auto referer=initiator;
        const auto publish = [destination, alive](std::unique_ptr<FetchResult> result) {
            if (alive->load() && PostMessageW(destination, kFetchFinished, 0,
                                               reinterpret_cast<LPARAM>(result.get())))
                result.release();
        };
        network->Fetch(url, referer, ResourcePriority::High,
            [network, alive, id, sequence, publish, cancellation,
             pageScriptsEnabled](ResourceScheduler::Response response) {
                auto initial = std::make_unique<FetchResult>();
                initial->tabId = id;initial->sequence = sequence;initial->response = response;
                if (!HasDocumentResponse(response) || !alive->load()) {
                    publish(std::move(initial));return;
                }
                initial->html = HttpClient::DecodeText(*response);
                const auto resources = DiscoverResources(initial->html, response->url,
                                                         pageScriptsEnabled);
                for (const auto& image : resources.images)
                    network->Prefetch(image, response->url, ResourcePriority::Low, cancellation);
                for (const auto& frame : resources.frames)
                    network->Prefetch(frame, response->url, ResourcePriority::Normal, cancellation);
                // Rendering is gated only by styles requested by TWebFrame.
                // Script prefetch remains useful, but a slow analytics or ad
                // server must not prevent the main HTML from reaching the
                // renderer and producing its first frame.
                for (const auto& resource : resources.styles) {
                    network->Fetch(resource, response->url, ResourcePriority::High,
                        [network, resource, cancellation](ResourceScheduler::Response item) {
                            if (item && item->Ok()) {
                                std::wstring type = item->contentType;
                                std::transform(type.begin(), type.end(), type.begin(), towlower);
                                if (type.find(L"text/css") != std::wstring::npos ||
                                    resource.find(L".css") != std::wstring::npos) {
                                    std::vector<std::wstring> nested;
                                    std::unordered_set<std::wstring> seen;
                                    DiscoverCssUrls(HttpClient::DecodeText(*item), item->url,
                                                    nested, seen);
                                    for (const auto& url : nested)
                                        network->Prefetch(url, item->url, ResourcePriority::Low,
                                                          cancellation);
                                }
                            }
                        }, cancellation);
                }
                for (const auto& resource : resources.scripts)
                    network->Prefetch(resource, response->url, ResourcePriority::Normal,
                                      cancellation);
                publish(std::move(initial));
            }, cancellation,true);
    }

    void FetchFinished(std::unique_ptr<FetchResult> result) {
        auto found = std::find_if(tabs_.begin(), tabs_.end(),
            [&](const std::shared_ptr<Tab>& tab) { return tab->id == result->tabId; });
        if (found == tabs_.end()) return;
        auto tab = *found;
        if (tab->sequence != result->sequence || !tab->view) return;
        const auto response = result->response;
        if (!HasDocumentResponse(response)) {
            tab->loading = false;
            tab->status = !response || response->error.empty() ? L"페이지 로드 실패" : response->error;
            std::wstring html = L"<html><head><title>페이지 로드 실패</title></head>"
                L"<body style='font:16px Segoe UI;padding:32px'><h1>페이지를 열 수 없습니다</h1><p>" +
                EscapeHtml(tab->status) + L"</p><p>" + EscapeHtml(tab->url) + L"</p></body></html>";
            tab->renderSequence = result->sequence;
            tab->view->NavigateToString(html, L"about:blank");
            tab->title = L"페이지 로드 실패";
        } else {
            tab->url = response->url;
            std::atomic_store(&tab->resourceReferer,
                              std::make_shared<const std::wstring>(response->url));
            if (!tab->history.empty()) tab->history[tab->historyIndex] = response->url;
            tab->status = L"페이지 렌더링 중…";
            tab->loading = true;
            RefreshTab(tab);
            tab->renderSequence = result->sequence;
            tab->view->NavigateToStringAsync(result->html, response->url);
        }
        RefreshTab(tab);
    }

    void StopOrReload() {
        auto tab = ActiveTab();
        if (!tab) return;
        if (tab->loading) {
            ++tab->sequence;
            if (tab->navigationCancellation)
                tab->navigationCancellation->store(true, std::memory_order_relaxed);
            if (tab->view) tab->view->CancelPendingLoads();
            tab->loading = false;
            tab->status = L"요청 중지됨";
            RefreshTab(tab);
        } else Navigate(tab, tab->url, false);
    }

    void CloseTab() {
        int index = TabCtrl_GetCurSel(tabsControl_);
        if (index < 0 || static_cast<size_t>(index) >= tabs_.size()) return;
        auto tab = tabs_[index];
        ++tab->sequence;
        if (tab->navigationCancellation)
            tab->navigationCancellation->store(true, std::memory_order_relaxed);
        if (tab->view) tab->view->CancelPendingLoads();
        if (tab->view) tab->view->SetVisible(false);
        tabs_.erase(tabs_.begin() + index);
        TabCtrl_DeleteItem(tabsControl_, index);
        tab->view.reset();
        if (tabs_.empty()) { PostMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        Activate(std::min(static_cast<size_t>(index), tabs_.size() - 1));
    }

    void NavigateAddress() {
        auto tab = ActiveTab();
        if (!tab) return;
        Navigate(tab, WindowText(address_), true);
        FocusPage();
    }

    void FocusPage() {
        auto tab = ActiveTab();
        if (tab && tab->view) SetFocus(tab->view->Window());
    }

    bool ServiceChromeDuringScript(){
        if(pendingChromeAction_)return false;
        servicingChrome_=true;std::vector<MSG> deferred;deferred.reserve(128);
        MSG message{};
        for(unsigned count=0;count<128&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++count){
            const bool chrome=message.hwnd==hwnd_||message.hwnd==tabsControl_||message.hwnd==back_||
                message.hwnd==forward_||message.hwnd==refresh_||message.hwnd==home_||message.hwnd==address_||
                message.hwnd==go_||message.hwnd==newTab_||message.hwnd==closeTab_||message.hwnd==status_;
            if(message.message==WM_QUIT){PostQuitMessage(static_cast<int>(message.wParam));pendingChromeAction_=true;break;}
            if(!chrome){
                bool rendered=false;
                for(const auto& tab:tabs_)if(tab->view&&tab->view->ServiceRenderingMessage(message)){rendered=true;break;}
                if(!rendered)deferred.push_back(message);
                continue;
            }
            if(!TranslateAcceleratorW(hwnd_,accelerators_,&message)){
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            if(pendingChromeAction_)break;
        }
        servicingChrome_=false;
        // Page messages remain ordinary queued tasks. They cannot dispatch
        // input handlers, timers or another script inside the current job.
        for(const auto& queued:deferred)PostMessageW(queued.hwnd,queued.message,queued.wParam,queued.lParam);
        return !pendingChromeAction_;
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if(servicingChrome_){
            if(message==WM_COMMAND||message==WM_CLOSE||message==WM_SIZE||message==kFetchFinished){
                PostMessageW(hwnd_,message,wparam,lparam);pendingChromeAction_=true;return 0;
            }
            if(message==WM_NOTIFY&&reinterpret_cast<NMHDR*>(lparam)->idFrom==ID_TABS&&
               reinterpret_cast<NMHDR*>(lparam)->code==TCN_SELCHANGE){
                PostMessageW(hwnd_,kDeferredTabSelection,TabCtrl_GetCurSel(tabsControl_),0);
                pendingChromeAction_=true;return 0;
            }
        }else pendingChromeAction_=false;
        switch (message) {
        case WM_CREATE:
            InitControls();
            AddTab(initial_);
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = POINT{620, 420};
            return 0;
        case kFetchFinished:
            FetchFinished(std::unique_ptr<FetchResult>(reinterpret_cast<FetchResult*>(lparam)));
            return 0;
        case kDeferredTabSelection:
            if(static_cast<size_t>(wparam)<tabs_.size())Activate(static_cast<size_t>(wparam));
            return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == ID_TABS &&
                reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
                int index = TabCtrl_GetCurSel(tabsControl_);
                if (index >= 0) Activate(static_cast<size_t>(index));
            }
            return 0;
        case WM_COMMAND: {
            auto tab = ActiveTab();
            switch (LOWORD(wparam)) {
            case ID_BACK: case ID_SHORTCUT_BACK:
                if(tab&&tab->view->CanTraverseHistory(-1))tab->view->TraverseHistory(-1);
                else if (tab && tab->historyIndex > 0) Navigate(tab, tab->history[--tab->historyIndex], false);
                return 0;
            case ID_FORWARD: case ID_SHORTCUT_FORWARD:
                if(tab&&tab->view->CanTraverseHistory(1))tab->view->TraverseHistory(1);
                else if (tab && tab->historyIndex + 1 < tab->history.size())
                    Navigate(tab, tab->history[++tab->historyIndex], false);
                return 0;
            case ID_REFRESH: StopOrReload(); return 0;
            case ID_HOME: if (tab) Navigate(tab, kHomeUrl, true); return 0;
            case ID_GO: NavigateAddress(); return 0;
            case ID_NEW_TAB: case ID_SHORTCUT_NEW_TAB: AddTab(L"about:blank", true); return 0;
            case ID_CLOSE_TAB: case ID_SHORTCUT_CLOSE_TAB: CloseTab(); return 0;
            case ID_FOCUS_ADDRESS:
                SetFocus(address_); SendMessageW(address_, EM_SETSEL, 0, -1); return 0;
            case ID_FOCUS_PAGE: FocusPage(); return 0;
            }
            break;
        }
        case WM_DESTROY:
            alive_->store(false);
            for (const auto& tab : tabs_)
                if (tab->navigationCancellation)
                    tab->navigationCancellation->store(true, std::memory_order_relaxed);
            tabs_.clear();
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
    bool pageScriptsEnabled_ = true;
    bool servicingChrome_=false,pendingChromeAction_=false;
    unsigned nextTabId_ = 0;
    std::vector<std::shared_ptr<Tab>> tabs_;
    std::shared_ptr<ResourceScheduler> network_;
    std::shared_ptr<std::atomic_bool> alive_;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    SetProcessDPIAware();
    const auto options = ParseLaunchOptions(commandLine ? commandLine : L"");
    BrowserApp app;
    if (!app.Create(instance, options.initialAddress, options.enablePageScripts)) {
        MessageBoxW(nullptr, L"브라우저 창을 만들 수 없습니다.", L"TWebFrame Browser", MB_OK | MB_ICONERROR);
        return 1;
    }
    return app.Run();
}

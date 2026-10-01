#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "BrowserContext.h"

namespace TWebFrame {

inline constexpr unsigned int VersionMajor = 1;
inline constexpr unsigned int VersionMinor = 3;
inline constexpr wchar_t VersionString[] = L"1.03";

// TWebFrame is a small, embeddable HTML/CSS/JavaScript surface.  The API is
// intentionally small and keeps a compatibility bridge for existing local pages
// while keeping ownership and threading rules explicit.
class View final {
public:
    using MessageHandler = std::function<void(const std::wstring&)>;
    using LoadHandler = std::function<void(bool, const std::wstring&)>;
    // Resolves page-relative text resources such as stylesheets, scripts, and
    // non-HTTP data requested by fetch() or XMLHttpRequest. Script HTTP requests
    // use the engine transport to preserve response metadata and CORS. Return
    // false when a resource is unavailable.
    using ResourceLoader = std::function<bool(const std::wstring&, std::wstring&)>;
    // HTTP resource requests carry the initiating document and ancestor site
    // policy. Local/archive resources continue to use the existing callbacks.
    using NetworkResourceLoader = std::function<NetworkResponse(const NetworkRequest&)>;
    // Resolves byte resources without a text transcoding step. Image hosts use
    // this callback so JPEG/PNG/GIF bytes, including archive-backed resources,
    // reach the shared decoder unchanged.
    using BinaryResourceLoader = std::function<bool(
        const std::wstring&, std::vector<unsigned char>&)>;
    // Called for non-fragment link and script navigation. A browser host can
    // load the target, update its address bar, and maintain history.
    using NavigationHandler = std::function<void(const std::wstring&, bool newWindow)>;
    // Reports push/replaceState and same-document traversal without reloading.
    // A nonzero delta identifies a traversal; zero identifies push/replace.
    using HistoryChangedHandler = std::function<void(const std::wstring&,bool replace,int delta)>;
    using HistoryTraversalHandler = std::function<void(int)>;
    // Optional owner-thread callback in addition to the View's built-in native
    // chrome/render safepoint. Document jobs wait for the active script to end.
    // Do not call View methods here. Return false to interrupt the current job.
    using ExecutionYieldHandler = std::function<bool()>;

    static std::unique_ptr<View> Create(HWND parent, const RECT& bounds);
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    HWND Window() const noexcept;
    void SetBounds(const RECT& bounds);
    void SetVisible(bool visible);
    void SetMessageHandler(MessageHandler handler);
    // The legacy embedded-app bridge is enabled by default for compatibility.
    // Standalone browsers can disable its chrome.webview and twebframe globals.
    void SetCompatibilityBridgeEnabled(bool enabled);
    void SetLoadHandler(LoadHandler handler);
    void SetBrowserContext(std::shared_ptr<BrowserContext> context,std::uint64_t session=0);
    void SetResourceLoader(ResourceLoader loader);
    void SetNetworkResourceLoader(NetworkResourceLoader loader);
    void SetBinaryResourceLoader(BinaryResourceLoader loader);
    void SetNavigationHandler(NavigationHandler handler);
    void SetHistoryChangedHandler(HistoryChangedHandler handler);
    void SetHistoryTraversalHandler(HistoryTraversalHandler handler);
    bool CanTraverseHistory(int delta) const;
    bool TraverseHistory(int delta);
    void SetExecutionYieldHandler(ExecutionYieldHandler handler);
    // Paint and CSS timeline messages can be serviced at a script instruction
    // boundary without dispatching another page script or rebuilding the DOM.
    bool ServiceRenderingMessage(const MSG& message);
    // Load text/binary resources, parse asynchronous documents and CSS, and
    // decode raster resources on a bounded worker pool. Host loaders used with
    // this mode must be safe to call concurrently from worker threads.
    void SetParallelResourceLoading(bool enabled);
    // Controls automatic execution of script elements and inline event
    // handlers. This does not provide a browser sandbox or security boundary.
    void SetPageScriptsEnabled(bool enabled);

    bool Navigate(const std::wstring& filePath);
    bool NavigateToString(const std::wstring& html, const std::wstring& basePath = L"");
    // Parses HTML on the bounded CPU worker pool and publishes the resulting
    // document on the view's UI thread. Completion is reported by LoadHandler.
    bool NavigateToStringAsync(const std::wstring& html,
                               const std::wstring& basePath = L"");
    // Invalidates queued document parses and image jobs without replacing the
    // currently displayed document. In-flight host I/O may finish, but its
    // result will not be published to this view.
    void CancelPendingLoads();

    // Source is always parsed and compiled by TWebFrame's JavaScript compiler.
    // The returned string is the JavaScript result converted to a string.
    bool ExecuteScript(const std::wstring& source, std::wstring* result = nullptr,
                       std::wstring* error = nullptr);
    bool ExecuteScript(const wchar_t* source, std::wstring* result = nullptr,
                       std::wstring* error = nullptr) {
        return ExecuteScript(std::wstring(source ? source : L""), result, error);
    }

    // Delivers a host message through
    // window.twebframe.addEventListener('message', ...). The compatibility
    // window.chrome.webview bridge receives the same message. JSON messages
    // expose the decoded JavaScript value as event.data.
    bool PostWebMessageAsJson(const std::wstring& json, std::wstring* error = nullptr);
    void PostWebMessageAsString(const std::wstring& message);

    // Test/diagnostic helpers. Layout JSON contains stable integer pixel bounds.
    std::wstring DumpLayoutJson() const;
    std::wstring DumpLayoutJson(bool includeChildFrames) const;
    // Accessibility JSON exposes the same stable automation ids used by the
    // UI Automation provider. Prefer an explicit data-automation-id or id;
    // otherwise TWebFrame emits a deterministic DOM path.
    std::wstring DumpAccessibilityJson() const;
    std::wstring LastError() const;
    std::wstring DocumentTitle() const;

private:
    struct Impl;
    explicit View(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace TWebFrame

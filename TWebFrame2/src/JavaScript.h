#pragma once

// Uncomment to enable WebAssembly, or define it in the library build settings.
// #define SUPPORT_WEB_ASSEMBLY

// Uncomment to use PCRE2 instead of the C++ standard regular expression library.
// #define SUPPORT_PCRE2

#include "EditingCommand.h"
#include "ScriptRequest.h"

#include <functional>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

struct LayoutRect;

class JavaScriptRuntime {
public:
    struct HeapStatistics {
        size_t objects = 0;
        size_t functions = 0;
        size_t nativeFunctions = 0;
        size_t environments = 0;
        size_t nodeListeners = 0;
        size_t prototypeSlots = 0;
        size_t collections = 0;
    };
    struct JitStatistics {
        size_t compiledFunctions = 0;
        size_t generatedCodeBytes = 0;
        size_t nativeCalls = 0;
        size_t guardFallbacks = 0;
        size_t unsupportedFunctions = 0;
    };
    enum class MutationKind { Paint, Style, Layout, Tree };
    struct Mutation {
        MutationKind kind = MutationKind::Paint;
        std::vector<std::shared_ptr<Node>> targets;
        bool liveRegionMembershipChanged = false;
        // Inline declaration updates normally change computed values without
        // changing which selectors match. Keep that distinction so the view
        // does not conservatively invalidate unrelated sibling subtrees.
        bool selectorMatchingChanged = false;
    };
    using MessageSink = std::function<void(const std::wstring&)>;
    using MutationSink = std::function<void(const Mutation&)>;
    using FrameScheduler = std::function<void()>;
    using TimerScheduler = std::function<void(unsigned)>;
    // Called at interpreter safepoints on the owner thread. The host may
    // service its own UI here, but must not enter or mutate this View/realm.
    // Returning false interrupts the current script job.
    using ExecutionYieldHandler = std::function<bool()>;
    using ResourceLoader = std::function<bool(const std::wstring&, std::wstring&)>;
    using AsyncResourceLoader = std::function<void(
        const std::wstring&, std::function<void(bool, std::wstring)>)>;
    using NavigationSink = std::function<void(const std::wstring&)>;
    using SameDocumentNavigationSink = std::function<void(const std::wstring&,bool,int)>;
    using HistoryTraversalSink = std::function<void(int)>;
    using DialogSink = std::function<void(const std::wstring&)>;
    using ConfirmSink = std::function<bool(const std::wstring&)>;
    using DocumentWriteSink = std::function<void(const std::wstring&)>;
    using FrameMessageSink = std::function<void(const std::shared_ptr<Node>&,
                                                const std::wstring&,
                                                const std::wstring&)>;
    using FrameDocumentSink = std::function<void(const std::shared_ptr<Node>&,
                                                 const std::wstring&)>;
    using FrameDocumentProvider = std::function<std::shared_ptr<Document>(const std::shared_ptr<Node>&)>;
    using FrameRuntimeProvider = std::function<std::shared_ptr<JavaScriptRuntime>(const std::shared_ptr<Node>&)>;
    using ParentMessageSink = std::function<void(const std::wstring&,
                                                 const std::wstring&)>;
    using TopMessageSink = ParentMessageSink;
    using FocusSink = std::function<void(const std::shared_ptr<Node>&)>;
    using WindowFocusSink = std::function<void(const std::shared_ptr<Node>&,bool)>;
    using DocumentFocusProvider = std::function<bool()>;
    using ActivationSink = std::function<void(const std::shared_ptr<Node>&)>;
    using PointerCaptureSink = std::function<void(bool)>;
    using SelectionProvider = std::function<bool(const std::shared_ptr<Node>&, size_t&, size_t&)>;
    using SelectionSetter = std::function<void(const std::shared_ptr<Node>&, size_t, size_t)>;
    using DomSelection = EditingSelection;
    using DomSelectionProvider = std::function<bool(DomSelection&)>;
    using DomSelectionSetter = std::function<void(const DomSelection&)>;
    using FrameSelectionProvider = std::function<bool(const std::shared_ptr<Node>&,DomSelection&)>;
    using FrameSelectionSetter = std::function<void(const std::shared_ptr<Node>&,const DomSelection&)>;
    struct EventInit {
        std::wstring key;
        std::wstring data;
        std::wstring inputType;
        std::shared_ptr<Node> relatedTarget;
        double clientX = 0;
        double clientY = 0;
        double pageX = 0;
        double pageY = 0;
        double screenX = 0;
        double screenY = 0;
        double movementX = 0;
        double movementY = 0;
        int button = 0;
        int buttons = 0;
        int detail = 0;
        bool ctrlKey = false;
        bool shiftKey = false;
        bool altKey = false;
        bool metaKey = false;
        bool isComposing = false;
        bool isTrusted = true;
        bool bubbles = true;
        bool cancelable = true;
    };
    struct NodeGeometry {
        double x=0,y=0,width=0,height=0;
        double clientWidth=0,clientHeight=0,scrollWidth=0,scrollHeight=0;
    };
    using GeometryProvider = std::function<NodeGeometry(const std::shared_ptr<Node>&)>;
    using SvgTextLengthProvider = std::function<bool(const std::shared_ptr<Node>&,float&,std::vector<LayoutRect>*)>;
    using StylePropertyProvider = std::function<std::wstring(const std::shared_ptr<Node>&,
                                                             const std::wstring&)>;

    explicit JavaScriptRuntime(Document& document);
    void SetBrowserContext(std::shared_ptr<TWebFrame::BrowserContext> context,std::uint64_t session=0);
    std::wstring SiteForCookies(const std::wstring& documentUrl=L"") const;
    ~JavaScriptRuntime();
    JavaScriptRuntime(const JavaScriptRuntime&) = delete;
    JavaScriptRuntime& operator=(const JavaScriptRuntime&) = delete;

    void SetMessageSink(MessageSink sink);
    void SetCompatibilityBridgeEnabled(bool enabled);
    void SetMutationSink(MutationSink sink);
    void SetFrameScheduler(FrameScheduler scheduler);
    void SetTimerScheduler(TimerScheduler scheduler);
    // Called from a Worker thread; schedule host work only, without throwing
    // or entering the realm. Empty handler retains timer polling for embedders.
    void SetWorkerWakeHandler(std::function<void()> handler);
    void SetExecutionYieldHandler(ExecutionYieldHandler handler);
    // Scheduling only: called when the last interpreter frame unwinds.
    // The callback must not throw or enter/mutate a JavaScript realm.
    void SetExecutionCompletionHandler(std::function<void()> handler);
    bool IsExecuting() const noexcept;
    void SetSecureContextAncestors(std::function<bool()> provider);
    bool IsSecureContext() const;
    void SetGeometryProvider(GeometryProvider provider);
    void SetSvgGeometryProvider(GeometryProvider provider);
    void SetSvgTextLengthProvider(SvgTextLengthProvider provider);
    void SetStylePropertyProvider(StylePropertyProvider provider);
    void SetResourceLoader(ResourceLoader loader);
    void SetAsyncResourceLoader(AsyncResourceLoader loader);
    void SetRequestLoader(ScriptRequestLoader loader);
    void SetAsyncRequestLoader(AsyncScriptRequestLoader loader);
    void SetNavigationSink(NavigationSink sink);
    void SetSameDocumentNavigationSink(SameDocumentNavigationSink sink);
    void SetHistoryTraversalSink(HistoryTraversalSink sink);
    bool CanTraverseHistory(int delta) const;
    bool TraverseHistory(int delta);
    void SetDialogSink(DialogSink sink);
    void SetConfirmSink(ConfirmSink sink);
    void SetDocumentWriteSink(DocumentWriteSink sink);
    void SetFrameMessageSink(FrameMessageSink sink);
    void SetFrameDocumentSink(FrameDocumentSink sink);
    void SetFrameDocumentProvider(FrameDocumentProvider provider);
    void SetFrameRuntimeProvider(FrameRuntimeProvider provider);
    void SetFrameSelectionProvider(FrameSelectionProvider provider,FrameSelectionSetter setter);
    bool ReadDomSelection(DomSelection& selection) const;
    void WriteDomSelection(const DomSelection& selection);
    void SetParentMessageSink(ParentMessageSink sink);
    void SetEmbeddingFrame(JavaScriptRuntime* parent,const std::shared_ptr<Node>& frame);
    void SetTopMessageSink(TopMessageSink sink);
    void SetFocusSink(FocusSink sink);
    void SetWindowFocusSink(WindowFocusSink sink);
    void SetDocumentFocusProvider(DocumentFocusProvider provider);
    void SetActivationSink(ActivationSink sink);
    void SetPointerCaptureSink(PointerCaptureSink sink);
    void SetSelectionProvider(SelectionProvider provider);
    void SetSelectionSetter(SelectionSetter setter);
    void SetDomSelectionProvider(DomSelectionProvider provider);
    void SetDomSelectionSetter(DomSelectionSetter setter);
    void SetInlineEventHandlersEnabled(bool enabled);
    void SetViewportSize(double width, double height);
    void SetDevicePixelRatio(double ratio);
    void SetDisplaySize(double width,double height);
    void SetDisplaySize(double width,double height,double availableWidth,double availableHeight);
    void SetLocation(const std::wstring& location);
    void SetWindowName(const std::wstring& name);
    void SetCurrentScript(const std::shared_ptr<Node>& script);
    std::wstring FrameWindowName(const std::shared_ptr<Node>& node);
    void SetDocumentReadyState(const std::wstring& state);
    void NavigateToFragment(const std::wstring& fragment);
    bool Load(const std::wstring& source, std::wstring* error = nullptr);
    bool Execute(const std::wstring& source, std::wstring* result = nullptr,
                 std::wstring* error = nullptr);
    // Diagnostic compilation in a temporary module, without executing source.
    bool ValidateSyntax(const std::wstring& source, std::wstring* error = nullptr) const;
    // Zero disables JIT compilation and execution for this runtime.
    void SetJitCompilationThreshold(size_t calls);
    JitStatistics GetJitStatistics() const;
    HeapStatistics GetHeapStatistics() const;
    std::wstring LastError() const;
    std::wstring LastCreatedError() const;
    std::wstring CreatedErrorTrace() const;
    std::wstring DiagnosticsJson() const;
    void DispatchDocumentEvent(const std::wstring& eventName);
    void DispatchWindowEvent(const std::wstring& eventName);
    void RunAnimationFrame();
    void RunTimers();
    bool DispatchNodeEvent(const std::shared_ptr<Node>& node, const std::wstring& eventName,
                           const EventInit& init = {});
    void CompleteImageRequest(const std::shared_ptr<Node>& node);
    void CancelPendingImageRequests();
    bool DispatchClipboardEvent(const std::shared_ptr<Node>& node,
                                const std::wstring& eventName,
                                const std::wstring& text,
                                const std::vector<Node::FileInfo>& files = {});
    std::shared_ptr<Node> CapturedPointerTarget() const;
    void ClearPointerCapture();
    void DispatchFileDrop(const std::shared_ptr<Node>& node,
                          const std::vector<Node::FileInfo>& files);
    bool DispatchWebMessageAsJson(const std::wstring& json, std::wstring* error = nullptr);
    void DispatchWebMessageAsString(const std::wstring& message);
    bool DispatchWindowMessageAsJson(const std::wstring& json,
                                     const std::shared_ptr<Node>& sourceFrame = {},
                                     const std::wstring& sourceOrigin = L"null",
                                     std::wstring* error = nullptr);
    void Clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace TWebFrame::Internal

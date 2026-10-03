#include <TWebFrame/TWebFrame.h>

#include "Accessibility.h"
#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"
#include "NumericParser.h"
#include "RasterImage.h"
// Keep the decoder in this long-standing translation unit as well as in its
// own source file.  Some downstream TWebFrame consumers maintain a fixed
// source list and would otherwise omit the new raster implementation.
#include "RasterImage.cpp"
#include "ScriptDialog.h"
#include "ScriptHttp.h"
#include "TextInput.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace TWebFrame {
using Microsoft::WRL::ComPtr;
using namespace Internal;

namespace {
constexpr wchar_t kWindowClass[] = L"TWebFrame.View.1";
constexpr UINT_PTR kAnimationFrameTimer = 0x5746;
constexpr UINT_PTR kJavaScriptTimer = 0x5747;
constexpr UINT_PTR kCssTransitionTimer = 0x5748;
constexpr UINT_PTR kCaretBlinkTimer = 0x5749;
constexpr UINT_PTR kImageAnimationTimer = 0x574a;
constexpr UINT kAnimationFrameFallbackMessage = WM_APP + 0x57;
constexpr UINT kNavigationMessage = WM_APP + 0x59;
constexpr UINT kHistoryTraversalMessage = WM_APP + 0x64;
constexpr UINT kAsyncImageReadyMessage = WM_APP + 0x5a;
constexpr UINT kAsyncDocumentReadyMessage = WM_APP + 0x5b;
constexpr UINT kAsyncTextReadyMessage = WM_APP + 0x5c;
constexpr UINT kAsyncFrameReadyMessage = WM_APP + 0x5d;
constexpr UINT kAsyncFrameSourceReadyMessage = WM_APP + 0x5e;
constexpr UINT kSyncChildFramesMessage = WM_APP + 0x5f;
constexpr UINT kViewportResizeMessage = WM_APP + 0x60;
constexpr UINT kDeferredExecutionMessage = WM_APP + 0x65;
constexpr UINT kWorkerReadyMessage = WM_APP + 0x66;
thread_local unsigned executionUiServiceDepth=0;
thread_local std::vector<HWND> executionReplayWindows;
constexpr UINT_PTR kTooltipToolId = 0x5750;

class BackgroundWorkQueue {
public:
    using Task = std::function<void()>;
    enum class Priority : std::size_t { Critical, Normal, Low };
    BackgroundWorkQueue() {
        const auto logical=std::max(2u,std::thread::hardware_concurrency());
        const auto count=std::max(2u,std::min(6u,logical>2?logical-2:logical));
        workers_.reserve(count);
        for(unsigned index=0;index<count;++index)workers_.emplace_back([this]{Run();});
    }
    ~BackgroundWorkQueue(){
        {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;}
        ready_.notify_all();
        for(auto& worker:workers_)if(worker.joinable())worker.join();
    }
    void Submit(Priority priority,Task task){
        {std::lock_guard<std::mutex> lock(mutex_);if(stopping_)return;
            tasks_[static_cast<std::size_t>(priority)].push_back(std::move(task));}
        ready_.notify_one();
    }
private:
    bool HasTasks()const{
        for(const auto& queue:tasks_)if(!queue.empty())return true;
        return false;
    }
    void Run(){
        for(;;){Task task;{
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock,[this]{return stopping_||HasTasks();});
            if(stopping_&&!HasTasks())return;
            for(auto& queue:tasks_)if(!queue.empty()){
                task=std::move(queue.front());queue.pop_front();break;
            }
        }try{task();}catch(...){}}
    }
    std::mutex mutex_;std::condition_variable ready_;
    std::array<std::deque<Task>,3> tasks_;
    std::vector<std::thread> workers_;bool stopping_=false;
};

BackgroundWorkQueue& ImageWorkers(){static BackgroundWorkQueue workers;return workers;}

struct AsyncViewLifetime {
    std::mutex mutex;
    HWND hwnd=nullptr;
    bool alive=true;
    std::uint64_t navigationGeneration=0;
    std::uint64_t resourceGeneration=0;
};

struct AsyncImageResult {
    std::uint64_t generation=0;
    std::wstring key;
    std::shared_ptr<RasterImage> image;
};

struct AsyncDocumentResult {
    std::uint64_t generation=0;
    std::wstring base;
    std::wstring location;
    std::wstring error;
    std::unique_ptr<Document> document;
    std::unique_ptr<StyleSheet> styles;
    std::unordered_map<std::wstring,std::wstring> textResources;
    std::unordered_set<std::wstring> failedTextResources;
};

struct AsyncTextResult {
    std::uint64_t generation=0;
    bool loaded=false;
    std::wstring content;
    std::function<void(bool,std::wstring)> completion;
    ScriptResponse response;
    std::function<void(ScriptResponse)> requestCompletion;
};

struct AsyncFrameResult {
    std::uint64_t generation=0;
    std::uint64_t request=0;
    bool success=false;
    std::shared_ptr<Node> node;
};

struct AsyncFrameSourceResult {
    std::uint64_t generation=0;
    std::uint64_t request=0;
    bool loaded=false;
    std::shared_ptr<Node> node;
    std::wstring html;
    std::wstring base;
    std::wstring location;
};

std::wstring Utf8ToWide(const std::string& value) {
    if(value.empty())return {};int count=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);std::wstring out(count,L'\0');MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),out.data(),count);return out;
}

std::wstring ResolveResourceForBase(const std::wstring& base,
                                    const std::wstring& reference) {
    if(reference.rfind(L"data:",0)==0)return reference;
    if(base.find(L"://")!=std::wstring::npos){
        std::vector<wchar_t> combined(32768,L'\0');
        DWORD length=static_cast<DWORD>(combined.size());
        if(SUCCEEDED(UrlCombineW(base.c_str(),reference.c_str(),combined.data(),&length,0)))
            return combined.data();
    }
    auto resolved=reference;
    const auto baseScheme=base.find(L"://");
    if(baseScheme!=std::wstring::npos&&reference.find(L"://")==std::wstring::npos){
        auto cleanBase=base;const auto fragment=cleanBase.find(L'#');
        if(fragment!=std::wstring::npos)cleanBase.resize(fragment);
        const auto originEnd=cleanBase.find(L'/',baseScheme+3);
        if(reference.rfind(L"//",0)==0)resolved=cleanBase.substr(0,baseScheme+1)+reference;
        else if(!reference.empty()&&reference.front()==L'/')
            resolved=(originEnd==std::wstring::npos?cleanBase:cleanBase.substr(0,originEnd))+reference;
        else{
            const auto slash=cleanBase.find_last_of(L'/');
            resolved=(slash==std::wstring::npos?cleanBase+L'/':cleanBase.substr(0,slash+1))+reference;
        }
    }
    return resolved;
}

bool LoadTextResourceForBase(const View::ResourceLoader& loader,
                             const std::wstring& base,const std::wstring& reference,
                             std::wstring& content){
    const auto resolved=ResolveResourceForBase(base,reference);
    const bool externalResource=base.find(L"://")!=std::wstring::npos||
                                resolved.find(L"://")!=std::wstring::npos;
    auto resource=reference;
    const auto suffix=resource.find_first_of(L"?#");
    if(suffix!=std::wstring::npos)resource.resize(suffix);
    if(loader&&loader(resolved,content))return true;
    if(resolved!=reference&&loader&&loader(reference,content))return true;
    if(!externalResource&&loader){
        auto withoutQuery=resolved;
        const auto query=withoutQuery.find_first_of(L"?#");
        if(query!=std::wstring::npos){
            withoutQuery.resize(query);
            if(loader(withoutQuery,content))return true;
        }
    }
    if(externalResource)return false;
    std::filesystem::path path(resource);
    if(path.is_relative()&&!base.empty())path=std::filesystem::path(base)/path;
    std::ifstream input(path,std::ios::binary);if(!input)return false;
    std::ostringstream bytes;bytes<<input.rdbuf();content=Utf8ToWide(bytes.str());return true;
}

std::wstring ResolveStylesheetUrls(const std::wstring& source,const std::wstring& location){
    std::wstring result;size_t copied=0;wchar_t quote=0;bool comment=false;
    for(size_t index=0;index<source.size();++index){
        const auto c=source[index];
        if(comment){if(c==L'*'&&index+1<source.size()&&source[index+1]==L'/'){comment=false;++index;}continue;}
        if(quote){if(c==L'\\')++index;else if(c==quote)quote=0;continue;}
        if(c==L'/'&&index+1<source.size()&&source[index+1]==L'*'){comment=true;++index;continue;}
        if(c==L'\''||c==L'"'){quote=c;continue;}
        if((c!=L'u'&&c!=L'U')||index+4>source.size()||ToLower(source.substr(index,4))!=L"url(")continue;
        size_t end=index+4;wchar_t urlQuote=0;
        for(;end<source.size();++end){
            const auto value=source[end];
            if(value==L'\\'){++end;continue;}
            if(urlQuote){if(value==urlQuote)urlQuote=0;}
            else if(value==L'\''||value==L'"')urlQuote=value;
            else if(value==L')')break;
        }
        if(end==source.size())break;
        auto reference=Trim(source.substr(index+4,end-index-4));
        if(reference.size()>1&&(reference.front()==L'\''||reference.front()==L'"'))reference=reference.substr(1,reference.size()-2);
        if(!reference.empty()&&reference.front()!=L'#'&&reference.find(L':')==std::wstring::npos){
            std::wstring resolved(4096,L'\0');DWORD size=static_cast<DWORD>(resolved.size());
            if(SUCCEEDED(UrlCombineW(location.c_str(),reference.c_str(),resolved.data(),&size,URL_DONT_ESCAPE_EXTRA_INFO)))resolved.resize(size);
            else resolved=ResolveResourceForBase(location,reference);
            result+=source.substr(copied,index-copied)+L"url(\""+resolved+L"\")";copied=end+1;
        }
        index=end;
    }
    return copied?result+source.substr(copied):source;
}

template<class Loader>
std::wstring CollectDocumentStyles(const Document& document,const std::wstring& base,Loader&& load,bool strict,std::wstring& error){
    std::wstring css;
    for(const auto& node:document.QuerySelectorAll(L"style,link")){
        if(node->attributes.count(L"disabled"))continue;
        std::wstring source;
        if(node->tag==L"style")source=node->InnerText();
        else{
            if(ToLower(Trim(node->Attribute(L"rel")))!=L"stylesheet")continue;
            const auto href=node->Attribute(L"href");if(href.empty())continue;
            if(node->stylesheetOverride)source=node->stylesheetText;
            else if(!load(href,source)){
                if(strict){error=L"Cannot load stylesheet: "+href;return L"";}
                continue;
            }
            source=ResolveStylesheetUrls(source,ResolveResourceForBase(base,href));
        }
        const auto media=Trim(node->Attribute(L"media"));
        if(!media.empty())css+=L"@media "+media+L" {\n"+source+L"\n}\n";
        else css+=source+L"\n";
    }
    return css;
}

std::wstring EncodeFormComponent(const std::wstring& value,UINT codePage){
    const int size=WideCharToMultiByte(codePage,0,value.c_str(),static_cast<int>(value.size()),
        nullptr,0,nullptr,nullptr);
    if(size<=0)return L"";
    std::string bytes(static_cast<size_t>(size),'\0');
    WideCharToMultiByte(codePage,0,value.c_str(),static_cast<int>(value.size()),
        bytes.data(),size,nullptr,nullptr);
    constexpr wchar_t digits[]=L"0123456789ABCDEF";
    std::wstring encoded;
    for(unsigned char byte:bytes){
        if((byte>=L'A'&&byte<=L'Z')||(byte>=L'a'&&byte<=L'z')||
            (byte>=L'0'&&byte<=L'9')||byte==L'*'||byte==L'-'||byte==L'.'||byte==L'_')
            encoded.push_back(static_cast<wchar_t>(byte));
        else if(byte==L' ')encoded.push_back(L'+');
        else{encoded.push_back(L'%');encoded.push_back(digits[byte>>4]);encoded.push_back(digits[byte&15]);}
    }
    return encoded;
}

ATOM EnsureWindowClass(HINSTANCE instance,const wchar_t* className,WNDPROC proc) {
    WNDCLASSEXW existing{};if(GetClassInfoExW(instance,className,&existing))return 1;
    // WM_SIZE explicitly invalidates the view. Avoid the class-wide redraw
    // flags, which add redundant full-window invalidations during live resize.
    WNDCLASSEXW wc{sizeof(wc)};wc.style=CS_DBLCLKS;wc.lpfnWndProc=proc;wc.hInstance=instance;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=nullptr;wc.lpszClassName=className;return RegisterClassExW(&wc);
}

bool IsTextInput(const std::shared_ptr<Node>& node) {
    if(!node||node->tag!=L"input")return false;
    const auto type=ToLower(node->Attribute(L"type"));
    return type.empty()||type==L"text"||type==L"search"||type==L"tel"||
           type==L"url"||type==L"email"||type==L"password"||type==L"number";
}

bool IsTextControl(const std::shared_ptr<Node>& node) {
    return IsTextInput(node)||(node&&node->tag==L"textarea");
}

bool IsEditableTextControl(const std::shared_ptr<Node>& node) {
    return IsTextControl(node)&&!node->disabled&&!node->attributes.count(L"readonly");
}

bool IsContentEditable(const std::shared_ptr<Node>& node) {
    if(!node||!node->attributes.count(L"contenteditable"))return false;
    const auto value=ToLower(Trim(node->Attribute(L"contenteditable")));
    return value.empty()||value==L"true"||value==L"plaintext-only";
}

bool IsAtomicEditingElement(const std::shared_ptr<Node>& node) {
    if(!node||node->type!=NodeType::Element)return false;
    const auto& tag=node->tag;
    return tag==L"img"||tag==L"br"||tag==L"hr"||tag==L"input"||
           tag==L"canvas"||tag==L"svg"||tag==L"iframe"||tag==L"video"||
           tag==L"audio"||tag==L"embed"||tag==L"object";
}

bool IsClassicJavaScriptType(const std::wstring& authoredType) {
    auto type=ToLower(Trim(authoredType));
    const auto parameters=type.find(L';');
    if(parameters!=std::wstring::npos)type=Trim(type.substr(0,parameters));
    return type.empty()||type==L"text/javascript"||type==L"application/javascript"||
           type==L"text/ecmascript"||type==L"application/ecmascript"||
           type==L"text/jscript"||type==L"text/livescript";
}

std::shared_ptr<Node> EditableRoot(const std::shared_ptr<Node>& node) {
    for(auto current=node;current;current=current->parent.lock()){
        if(current->attributes.count(L"contenteditable"))
            return IsContentEditable(current)?current:std::shared_ptr<Node>{};
    }
    return {};
}

bool IsFocusable(const std::shared_ptr<Node>& node) {
    if(!node||node->disabled||ToLower(node->Attribute(L"aria-disabled"))==L"true")return false;
    if(node->attributes.count(L"tabindex"))return true;
    if(IsContentEditable(node))return true;
    if(node->tag==L"input"||node->tag==L"button"||node->tag==L"select"||node->tag==L"textarea")return true;
    if(node->tag==L"a"&&!node->Attribute(L"href").empty())return true;
    const auto role=ToLower(node->Attribute(L"role"));
    return role==L"button"||role==L"menuitem"||role==L"checkbox"||role==L"radio"||role==L"switch"||role==L"tab"||role==L"option";
}

int SequentialTabIndex(const std::shared_ptr<Node>& node){
    if(!IsFocusable(node))return -1;
    if(!node->attributes.count(L"tabindex"))return 0;
    const auto value=Trim(node->Attribute(L"tabindex"));size_t used=0;int parsed=0;
    return TryParseInteger(value,parsed,&used)&&used==value.size()?parsed:0;
}

bool IsKeyboardActivatable(const std::shared_ptr<Node>& node){
    if(!node||node->disabled||ToLower(node->Attribute(L"aria-disabled"))==L"true")return false;
    if(node->tag==L"button"||node->tag==L"a")return true;
    if(node->tag==L"input"){
        const auto type=ToLower(node->Attribute(L"type"));
        return type==L"button"||type==L"submit"||type==L"reset"||type==L"checkbox"||
               type==L"radio"||type==L"file"||type==L"image";
    }
    const auto role=ToLower(node->Attribute(L"role"));
    return role==L"button"||role==L"menuitem"||role==L"checkbox"||role==L"radio"||
           role==L"switch"||role==L"tab"||role==L"option";
}

bool FileInfoForPath(const std::wstring& path,Node::FileInfo& result){
    WIN32_FILE_ATTRIBUTE_DATA metadata{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&metadata))return false;
    const bool directory=(metadata.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
    const std::filesystem::path selected(path);
    const auto extension=ToLower(selected.extension().wstring());
    result.name=selected.filename().wstring();
    if(result.name.empty())result.name=selected.root_name().wstring();
    result.path=selected.lexically_normal().wstring();
    result.type=directory?L"application/x-directory":extension==L".csv"?L"text/csv":extension==L".json"?L"application/json":
        extension==L".txt"||extension==L".log"?L"text/plain":extension==L".png"?L"image/png":
        extension==L".jpg"||extension==L".jpeg"?L"image/jpeg":extension==L".gif"?L"image/gif":
        extension==L".webp"?L"image/webp":extension==L".bmp"||extension==L".dib"?L"image/bmp":
        extension==L".svg"?L"image/svg+xml":L"application/octet-stream";
    result.size=directory?0:(static_cast<unsigned long long>(metadata.nFileSizeHigh)<<32)|metadata.nFileSizeLow;
    return true;
}

std::vector<std::shared_ptr<Node>> SelectOptions(const std::shared_ptr<Node>& select) {
    std::vector<std::shared_ptr<Node>> result;
    if(!select||select->tag!=L"select")return result;
    std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& parent){
        for(const auto& child:parent->children){
            if(child->tag==L"option")result.push_back(child);
            else if(child->tag==L"optgroup")collect(child);
        }
    };
    collect(select);return result;
}

std::wstring OptionValue(const std::shared_ptr<Node>& option) {
    return option&&option->attributes.count(L"value")?option->Attribute(L"value"):
           (option?option->InnerText():L"");
}

std::wstring KeyValue(WPARAM key) {
    switch(key){
    case VK_ESCAPE:return L"Escape";case VK_RETURN:return L"Enter";case VK_TAB:return L"Tab";
    case VK_BACK:return L"Backspace";case VK_DELETE:return L"Delete";case VK_SPACE:return L" ";
    case VK_LEFT:return L"ArrowLeft";case VK_RIGHT:return L"ArrowRight";
    case VK_UP:return L"ArrowUp";case VK_DOWN:return L"ArrowDown";
    case VK_HOME:return L"Home";case VK_END:return L"End";case VK_PRIOR:return L"PageUp";case VK_NEXT:return L"PageDown";
    default:break;
    }
    if(key>=L'A'&&key<=L'Z')return std::wstring(1,static_cast<wchar_t>(((GetKeyState(VK_SHIFT)&0x8000)!=0)?key:key-L'A'+L'a'));
    if(key>=L'0'&&key<=L'9')return std::wstring(1,static_cast<wchar_t>(key));
    return L"Unidentified";
}

size_t SelectedOptionIndex(const std::shared_ptr<Node>& select,
                           const std::vector<std::shared_ptr<Node>>& options) {
    if(options.empty())return 0;
    if(select&&select->attributes.count(L"value")){
        const auto value=select->Attribute(L"value");
        for(size_t index=0;index<options.size();++index)
            if(OptionValue(options[index])==value)return index;
    }
    for(size_t index=0;index<options.size();++index)
        if(options[index]->attributes.count(L"selected"))return index;
    return 0;
}

size_t PreviousTextPosition(const std::wstring& value,size_t position) {
    if(position==0)return 0;--position;
    if(position>0&&value[position]>=0xdc00&&value[position]<=0xdfff&&
       value[position-1]>=0xd800&&value[position-1]<=0xdbff)--position;
    return position;
}

size_t NextTextPosition(const std::wstring& value,size_t position) {
    if(position>=value.size())return value.size();
    if(value[position]>=0xd800&&value[position]<=0xdbff&&position+1<value.size()&&
       value[position+1]>=0xdc00&&value[position+1]<=0xdfff)return position+2;
    return position+1;
}

unsigned int CompositeOver(unsigned int foreground,unsigned int background) {
    const unsigned int alpha=(foreground>>24)&0xff;
    if(alpha==0)return background;
    if(alpha==255)return 0xff000000u|(foreground&0x00ffffffu);
    auto channel=[&](int shift){
        const unsigned int front=(foreground>>shift)&0xff,back=(background>>shift)&0xff;
        return (front*alpha+back*(255-alpha)+127)/255;
    };
    return 0xff000000u|(channel(16)<<16)|(channel(8)<<8)|channel(0);
}

unsigned int EffectiveBackgroundColor(const LayoutBox* box) {
    std::vector<const LayoutBox*> ancestors;
    for(auto* current=box;current;current=current->parent)ancestors.push_back(current);
    // This is also the clear color used for a TWebFrame document.
    unsigned int result=0xffffffffu;
    constexpr unsigned int invalid=0x01020304u;
    for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
        auto value=(*it)->style.Get(L"background-color");
        if(value.empty())value=(*it)->style.Get(L"background");
        if(value.empty())continue;
        if(ToLower(Trim(value))==L"currentcolor")value=(*it)->style.Get(L"color",L"#000000");
        const auto color=StyleSheet::Color(value,invalid);
        if(color!=invalid)result=CompositeOver(color,result);
    }
    return result;
}
}

struct View::Impl {
    HWND hwnd=nullptr;
    std::shared_ptr<Document> documentOwner=std::make_shared<Document>();
    Document& document=*documentOwner;
    StyleSheet styles;
    JavaScriptRuntime javascript{document};
    LayoutEngine layout{document,styles};
    MessageHandler messageHandler;
    LoadHandler loadHandler;
    View::ResourceLoader resourceLoader;
    View::NetworkResourceLoader networkResourceLoader;
    std::shared_ptr<BrowserContext> browserContext=std::make_shared<BrowserContext>();
    std::uint64_t storageSession=browserContext->NewBrowsingContext();
    bool ownsStorageSession=true;
    View::BinaryResourceLoader binaryResourceLoader;
    View::NavigationHandler navigationHandler;
    View::HistoryChangedHandler historyChangedHandler;
    View::HistoryTraversalHandler historyTraversalHandler;
    View::ExecutionYieldHandler javascriptExecutionYieldHandler;
    struct DeferredExecutionMessage {
        MSG message{};
        RECT dpiBounds{};
        bool hasDpiBounds=false;
    };
    std::vector<DeferredExecutionMessage> deferredExecutionMessages;
    bool deferredExecutionMessagePosted=false;
    bool executionChromeInterrupted=false;
    bool pageScriptsEnabled=true;
    bool javascriptCompatibilityBridgeEnabled=true;
    bool parallelResourceLoading=false;
    std::uint64_t navigationGeneration=0;
    std::uint64_t resourceGeneration=0;
    struct InitialDocumentScript {
        std::shared_ptr<Node> node;
        std::wstring resource;
        std::wstring program;
        bool asynchronous=false;
        bool deferred=false;
        bool ready=false;
        bool loaded=false;
        bool finished=false;
    };
    std::vector<std::shared_ptr<InitialDocumentScript>> initialDocumentScripts;
    std::vector<std::pair<std::shared_ptr<Node>,std::wstring>> initialImageEvents;
    size_t nextOrderedInitialScript=0;
    size_t nextDeferredInitialScript=0;
    size_t remainingInitialScripts=0;
    std::uint64_t initialScriptGeneration=0;
    bool initialBlockingScriptsDrained=false;
    bool initialDomContentLoaded=false;
    bool initialLoadDispatched=false;
    std::unordered_map<std::wstring,std::shared_ptr<RasterImage>> rasterImageCache;
    std::unordered_set<std::wstring> pendingRasterImages;
    std::unordered_map<std::wstring,std::vector<std::shared_ptr<Node>>> pendingImageNodes;
    std::shared_ptr<AsyncViewLifetime> asyncLifetime=std::make_shared<AsyncViewLifetime>();
    struct ChildFrame {
        std::shared_ptr<Node> node;
        std::shared_ptr<View> view;
        std::wstring source;
        std::uint64_t request=0;
    };
    std::vector<ChildFrame> childFrames;
    Impl* compositionParent=nullptr;
    // Cross-realm functions can keep a detached frame alive beyond this view.
    // Remember weak owners so teardown can sever every parent/UI bridge.
    std::vector<std::weak_ptr<View>> createdChildViews;
    float frameViewportWidth=0,frameViewportHeight=0;
    HWND pointerFrame=nullptr;
    bool painting=false;
    unsigned childFrameDestructionDepth=0;
    JavaScriptRuntime::TopMessageSink topMessageRelay;
    std::uint64_t nextChildFrameRequest=0;
    bool childFrameSyncPending=false;
    std::wstring lastError;
    std::wstring basePath;
    std::wstring currentLocation;
    std::wstring installedStylesheetText;
    std::unordered_map<std::wstring,std::wstring> stylesheetResources;
    bool stylesheetSourcesDirty=true;
    std::wstring pendingNavigation;
    bool pendingNavigationNewWindow=false;
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> writeFactory;
    ComPtr<ID2D1HwndRenderTarget> renderTarget;
    ComPtr<ID2D1BitmapRenderTarget> backBuffer;
    D2D1_SIZE_U backBufferPixelSize{};
    float backBufferDpi=0;
    std::shared_ptr<AccessibilityHost> accessibility;
    std::vector<std::weak_ptr<Node>> liveRegions;
    std::unordered_map<const Node*,std::wstring> liveRegionText;
    struct AccessibilityPosition {
        std::weak_ptr<Node> parent;
        const Node* parentKey = nullptr;
        size_t index = 0;
    };
    std::unordered_map<const Node*,std::vector<std::shared_ptr<Node>>> accessibilityChildrenCache;
    std::unordered_map<const Node*,AccessibilityPosition> accessibilityPositionCache;
    std::unordered_map<const Node*,std::wstring> accessibilityAutomationIdCache;
    bool accessibilityTreeDirty = true;
    std::shared_ptr<Node> focused;
    // Script-driven focus follows the most recent input modality. A focus()
    // call made by a pointer handler must not manufacture :focus-visible,
    // while the same call made during keyboard navigation must retain it.
    bool focusVisibleFromKeyboard = true;
    std::shared_ptr<Node> hovered;
    std::weak_ptr<Node> primaryPointerDownTarget;
    int primaryClickDetail=0;
    std::vector<std::shared_ptr<Node>> hoverPath;
    std::shared_ptr<Node> scrollbarDragNode;
    float scrollbarDragOffset=0;
    bool scrollbarDragHorizontal=false;
    std::shared_ptr<Node> openSelectPopup;
    int selectPopupHotIndex=-1;
    float selectPopupScrollOffset=0;
    bool selectPopupShowAll=false;
    bool selectPopupScrollDragging=false;
    float selectPopupScrollDragOffset=0;
    std::unique_ptr<ScriptDialog> scriptDialog;
    std::shared_ptr<Node> editingNode;
    // A collapsed contenteditable selection may live between child nodes
    // (notably immediately after an img).  Keep that DOM boundary as a real
    // selection instead of manufacturing an empty text node that normalize()
    // is required to remove.
    std::shared_ptr<Node> editingBoundaryContainer;
    size_t editingBoundaryOffset=0;
    bool textSelectionDragging=false;
    size_t selectionAnchor=0;
    size_t caretPosition=0;
    float verticalCaretX=0;
    bool verticalCaretXValid=false;
    bool caretVisible=true;
    bool caretBlinkTimerActive=false;
    bool textEditDirty=false;
    bool compositionActive=false;
    std::wstring compositionBase;
    std::wstring compositionText;
    std::vector<unsigned char> compositionAttributes;
    size_t compositionReplaceStart=0;
    size_t compositionReplaceEnd=0;
    size_t compositionCursor=0;
    struct EditSnapshot {
        std::wstring value;
        size_t anchor=0;
        size_t caret=0;
    };
    std::vector<EditSnapshot> undoHistory;
    std::vector<EditSnapshot> redoHistory;
    bool layoutDirty=true;
    bool viewportOnlyDirty=false;
    bool trackingMouseLeave=false;
    bool hasPointerPosition=false;
    float pointerX=0;
    float pointerY=0;
    bool cssTransitionTimerActive=false;
    bool imageAnimationTimerActive=false;
    std::chrono::steady_clock::time_point cssTransitionTick{};
    HWND tooltip=nullptr;
    HFONT tooltipFont=nullptr;
    TOOLINFOW tooltipTool{};
    std::shared_ptr<Node> tooltipOwner;
    std::wstring tooltipText;
    bool tooltipVisible=false;
    TextInput textInput;

    Impl():textInput(TextInput::Client{
        [this]{return CanEditText();},
        [this]{BeginTextComposition();},
        [this](const std::wstring& text,const std::vector<unsigned char>& attributes,size_t cursor){
            UpdateTextComposition(text,attributes,cursor);
        },
        [this](const std::wstring& text){CommitTextComposition(text);},
        [this]{CancelTextComposition();},
        [this]{return CaretClientRect();}
    }){
        // Every View, including iframe realms, has a safepoint even when its
        // embedding application has not installed a custom UI callback.
        javascript.SetExecutionYieldHandler([this]{return ServiceExecutionMessages();});
        javascript.SetExecutionCompletionHandler([this]{ScheduleExecutionMessages();});
    }

    void ScheduleDeferredExecutionMessages(){
        if(!deferredExecutionMessagePosted&&!deferredExecutionMessages.empty()&&hwnd)
            deferredExecutionMessagePosted=PostMessageW(hwnd,kDeferredExecutionMessage,0,0)!=FALSE;
    }
    void ScheduleDeferredExecutionTree(){
        ScheduleDeferredExecutionMessages();
        if(childFrameDestructionDepth)return;
        for(const auto& frame:childFrames)if(frame.view&&frame.view->impl_)
            frame.view->impl_->ScheduleDeferredExecutionTree();
    }
    void ScheduleExecutionMessages(){
        auto* root=this;while(root->compositionParent)root=root->compositionParent;
        root->ScheduleDeferredExecutionTree();
        auto pending=std::move(executionReplayWindows);executionReplayWindows.clear();
        for(const HWND window:pending)if(IsWindow(window)){
            wchar_t name[64]{};
            if(GetClassNameW(window,name,64)&&wcscmp(name,kWindowClass)==0)
                if(auto* view=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA)))
                    view->ScheduleDeferredExecutionMessages();
        }
    }
    void DeferExecutionMessage(HWND target,UINT message,WPARAM wParam,LPARAM lParam){
        // Windows timers repeat until killed. Keep a single queued JavaScript
        // timer task while the active job runs, rather than accumulating ticks.
        if(message==WM_TIMER&&(wParam==kAnimationFrameTimer||wParam==kJavaScriptTimer)){
            wchar_t name[64]{};
            if(GetClassNameW(target,name,64)&&wcscmp(name,kWindowClass)==0)KillTimer(target,wParam);
        }
        DeferredExecutionMessage queued;queued.message={target,message,wParam,lParam};
        // WM_DPICHANGED's suggested rectangle belongs to the sender's stack.
        if(message==WM_DPICHANGED&&lParam){
            queued.dpiBounds=*reinterpret_cast<const RECT*>(lParam);queued.hasDpiBounds=true;
        }
        // Coalesce viewport updates; keep input and completion tasks in order.
        if(message==WM_SIZE||message==WM_MOVE||message==WM_DPICHANGED||message==WM_DPICHANGED_AFTERPARENT||
           (message==WM_TIMER&&(wParam==kAnimationFrameTimer||wParam==kJavaScriptTimer)))
            for(auto& pending:deferredExecutionMessages)
                if(pending.message.hwnd==target&&pending.message.message==message&&
                   (message!=WM_TIMER||pending.message.wParam==wParam)){pending=queued;return;}
        deferredExecutionMessages.push_back(queued);ScheduleDeferredExecutionMessages();
    }
    void ReleaseDeferredExecutionMessages(bool deliverDpi=true){
        auto pending=std::move(deferredExecutionMessages);deferredExecutionMessages.clear();
        // Posting restores the ordinary host/page task ordering. DPI messages
        // require synchronous delivery of our owned rectangle while idle.
        for(auto& queued:pending){
            if(!queued.message.hwnd){
                PostThreadMessageW(GetCurrentThreadId(),queued.message.message,
                    queued.message.wParam,queued.message.lParam);continue;
            }
            if(!IsWindow(queued.message.hwnd))continue;
            if(queued.hasDpiBounds){
                if(deliverDpi)SendMessageW(queued.message.hwnd,queued.message.message,queued.message.wParam,
                    reinterpret_cast<LPARAM>(&queued.dpiBounds));
            }
            else PostMessageW(queued.message.hwnd,queued.message.message,
                queued.message.wParam,queued.message.lParam);
        }
    }
    static LRESULT CALLBACK ExecutionChromeProc(HWND window,UINT message,WPARAM wParam,
                                                LPARAM lParam,UINT_PTR,DWORD_PTR data){
        auto* self=reinterpret_cast<Impl*>(data);
        // DefWindowProc runs a nested message loop while moving or sizing a
        // window. Protect the host's page-owning actions throughout that loop.
        // Registered messages (0xC000+) include OS/accessibility queries with
        // borrowed pointers. They must never be mistaken for queued host jobs.
        const bool hostJob=message>=WM_APP&&message<0xc000;
        if(message==WM_SIZE||message==WM_MOVE||message==WM_DPICHANGED||message==WM_DPICHANGED_AFTERPARENT||
           message==WM_COMMAND||message==WM_CLOSE||message==WM_TIMER||hostJob){
            self->DeferExecutionMessage(window,message,wParam,lParam);
            if(message==WM_COMMAND||message==WM_CLOSE||hostJob)
                self->executionChromeInterrupted=true;
            return 0;
        }
        if(message==WM_SYSCOMMAND&&(wParam&0xfff0)==SC_CLOSE){
            self->DeferExecutionMessage(window,message,wParam,lParam);
            self->executionChromeInterrupted=true;return 0;
        }
        return DefSubclassProc(window,message,wParam,lParam);
    }
    bool ServiceExecutionMessages(){
        executionChromeInterrupted=false;
        struct ServiceScope {
            ServiceScope(){++executionUiServiceDepth;}
            ~ServiceScope(){--executionUiServiceDepth;}
        } serviceScope;
        const HWND root=hwnd?GetAncestor(hwnd,GA_ROOT):nullptr;
        const UINT_PTR guardId=reinterpret_cast<UINT_PTR>(this);
        const bool guarded=root&&SetWindowSubclass(root,ExecutionChromeProc,guardId,
                                                   reinterpret_cast<DWORD_PTR>(this));
        struct ChromeGuard {
            HWND window;UINT_PTR id;bool installed;
            ~ChromeGuard(){if(installed&&IsWindow(window))RemoveWindowSubclass(window,ExecutionChromeProc,id);}
        } guard{root,guardId,guarded};
        bool keepRunning=!javascriptExecutionYieldHandler||javascriptExecutionYieldHandler();
        // Drain a bounded slice of the thread queue, dispatching only chrome
        // and render snapshots. Holding other tasks until the job ends also
        // lets the low-priority WM_QUIT flag materialize with page jobs pending.
        MSG message{};
        for(unsigned count=0;keepRunning&&!executionChromeInterrupted&&count<64&&
            PeekMessageW(&message,nullptr,0,0,PM_REMOVE);++count){
            if(message.message==WM_QUIT){
                PostQuitMessage(static_cast<int>(message.wParam));
                executionChromeInterrupted=true;break;
            }
            const bool nativeChrome=guarded&&message.hwnd==root&&
                ((message.message>=WM_NCMOUSEMOVE&&message.message<=WM_NCXBUTTONDBLCLK)||
                 message.message==WM_SYSCOMMAND||message.message==WM_CLOSE);
            wchar_t targetClass[64]{};
            const bool frameMessage=(message.message==kDeferredExecutionMessage||
                (message.message==WM_TIMER&&message.wParam==kCssTransitionTimer))&&
                GetClassNameW(message.hwnd,targetClass,64)&&wcscmp(targetClass,kWindowClass)==0;
            const bool ownDrain=frameMessage&&message.message==kDeferredExecutionMessage;
            const bool cssTimeline=frameMessage&&message.message==WM_TIMER;
            if(ownDrain||message.message==WM_PAINT||nativeChrome||cssTimeline||message.message>=0xc000)
                DispatchMessageW(&message);
            else DeferExecutionMessage(message.hwnd,message.message,message.wParam,message.lParam);
        }
        ScheduleExecutionMessages();
        return keepRunning&&!executionChromeInterrupted;
    }

    void PublishAsyncGenerations(){
        std::lock_guard<std::mutex> lock(asyncLifetime->mutex);
        asyncLifetime->navigationGeneration=navigationGeneration;
        asyncLifetime->resourceGeneration=resourceGeneration;
    }

    NetworkRequest ResourceRequest(const std::wstring& url,const std::wstring& location)const{
        NetworkRequest request;request.url=url;request.referrer=location;
        request.origin=BrowserContext::Origin(location);request.siteForCookies=javascript.SiteForCookies(location);
        request.mode=NetworkRequest::Mode::Navigation;request.credentials=NetworkRequest::Credentials::Include;
        return request;
    }
    View::ResourceLoader TextLoader(const std::wstring& location)const{
        const auto loader=resourceLoader;const auto network=networkResourceLoader;
        const auto context=browserContext;const auto policy=ResourceRequest(L"",location);
        return [loader,network,context,policy](const std::wstring& url,std::wstring& content){
            if(BrowserContext::Origin(url)!=L"null"&&(network||!loader)){
                auto request=policy;request.url=url;
                const auto response=network?network(request):context->Request(request);
                if(response.status<200||response.status>=300)return false;
                content=response.body;return true;
            }
            return loader&&loader(url,content);
        };
    }
    std::function<bool(const std::wstring&,std::wstring&,std::wstring&)> DocumentLoader(bool topLevel=false)const{
        const auto loader=resourceLoader;const auto network=networkResourceLoader;
        const auto context=browserContext;const auto base=basePath;
        auto policy=ResourceRequest(L"",currentLocation);policy.topLevelNavigation=topLevel;
        return [loader,network,context,base,policy](const std::wstring& source,std::wstring& html,std::wstring& finalUrl){
            const auto resolved=ResolveResourceForBase(base,source);
            if(BrowserContext::Origin(resolved)!=L"null"&&(network||!loader)){
                auto request=policy;request.url=resolved;
                const auto response=network?network(request):context->Request(request);
                // An HTTP error page is still a navigation document. Reject
                // transport failures, rather than discarding 4xx/5xx HTML.
                if(response.status<200||response.status>=600||!response.error.empty()||response.opaque)return false;
                html=response.body;finalUrl=BrowserContext::Origin(response.url)==L"null"?resolved:response.url;return true;
            }
            return LoadTextResourceForBase(loader,base,source,html);
        };
    }
    View::BinaryResourceLoader BinaryLoader()const{
        const auto loader=binaryResourceLoader;const auto network=networkResourceLoader;
        const auto context=browserContext;const auto policy=ResourceRequest(L"",currentLocation);
        return [loader,network,context,policy](const std::wstring& url,std::vector<unsigned char>& bytes){
            if(BrowserContext::Origin(url)!=L"null"&&(network||!loader)){
                auto request=policy;request.url=url;
                const auto response=network?network(request):context->Request(request);
                if(response.status<200||response.status>=300)return false;
                bytes=response.bytes;return true;
            }
            return loader&&loader(url,bytes);
        };
    }
    bool LoadTextResource(const std::wstring& reference,std::wstring& content)const{
        return LoadTextResourceForBase(TextLoader(currentLocation),basePath,reference,content);
    }

    static int HexDigit(wchar_t value){
        if(value>=L'0'&&value<=L'9')return value-L'0';
        if(value>=L'a'&&value<=L'f')return value-L'a'+10;
        if(value>=L'A'&&value<=L'F')return value-L'A'+10;
        return -1;
    }
    static std::wstring PercentDecode(const std::wstring& value){
        std::wstring result;std::string encoded;
        const auto flush=[&]{if(!encoded.empty()){result+=Utf8ToWide(encoded);encoded.clear();}};
        for(size_t index=0;index<value.size();++index){
            if(value[index]==L'%'&&index+2<value.size()){
                const int high=HexDigit(value[index+1]),low=HexDigit(value[index+2]);
                if(high>=0&&low>=0){encoded.push_back(static_cast<char>((high<<4)|low));index+=2;continue;}
            }
            flush();result.push_back(value[index]);
        }
        flush();return result;
    }
    std::wstring ResolveResourceReference(const std::wstring& reference)const{
        return ResolveResourceForBase(basePath,reference);
    }
    bool LoadBinaryResource(const std::wstring& reference,
                            std::vector<unsigned char>& bytes)const{
        if(DecodeImageDataUrl(reference,bytes))return true;
        const auto resolved=ResolveResourceReference(reference);
        const auto loader=BinaryLoader();
        if(loader(resolved,bytes))return true;
        if(resolved!=reference&&loader(reference,bytes))return true;
        if(resolved.find(L"://")!=std::wstring::npos&&resolved.rfind(L"file://",0)!=0)return false;
        auto local=resolved;
        if(local.rfind(L"file:///",0)==0)local=PercentDecode(local.substr(8));
        else if(local.rfind(L"file://",0)==0)local=PercentDecode(local.substr(7));
        const auto suffix=local.find_first_of(L"?#");if(suffix!=std::wstring::npos)local.resize(suffix);
        std::filesystem::path path(local);
        if(path.is_relative()&&!basePath.empty()&&basePath.find(L"://")==std::wstring::npos)
            path=std::filesystem::path(basePath)/path;
        std::ifstream input(path,std::ios::binary);if(!input)return false;
        input.seekg(0,std::ios::end);const auto size=input.tellg();
        if(size<=0)return false;input.seekg(0,std::ios::beg);
        bytes.resize(static_cast<size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()),size);
        return input.good()||input.eof();
    }

    std::shared_ptr<RasterImage> ResolveRasterImage(const std::wstring& reference){
        const auto key=ResolveResourceReference(reference);
        const auto cached=rasterImageCache.find(key);
        if(cached!=rasterImageCache.end())return cached->second;
        if(parallelResourceLoading&&(binaryResourceLoader||BrowserContext::Origin(key)!=L"null")){
            if(pendingRasterImages.insert(key).second){
                const auto loader=BinaryLoader();
                const auto fallback=reference;
                const auto lifetime=asyncLifetime;
                const auto generation=resourceGeneration;
                ImageWorkers().Submit(BackgroundWorkQueue::Priority::Low,
                    [loader,key,fallback,lifetime,generation]{
                    {
                        std::lock_guard<std::mutex> lock(lifetime->mutex);
                        if(!lifetime->alive||!lifetime->hwnd||
                           lifetime->resourceGeneration!=generation)return;
                    }
                    std::vector<unsigned char> bytes;std::wstring error;
                    bool loaded=loader(key,bytes);
                    if(!loaded&&key!=fallback){bytes.clear();loaded=loader(fallback,bytes);}
                    {
                        std::lock_guard<std::mutex> lock(lifetime->mutex);
                        if(!lifetime->alive||!lifetime->hwnd||
                           lifetime->resourceGeneration!=generation)return;
                    }
                    auto image=loaded?DecodeRasterImage(bytes,key,&error):std::shared_ptr<RasterImage>{};
                    auto result=std::make_unique<AsyncImageResult>();
                    result->generation=generation;result->key=key;result->image=std::move(image);
                    std::lock_guard<std::mutex> lock(lifetime->mutex);
                    if(!lifetime->alive||!lifetime->hwnd||
                       lifetime->resourceGeneration!=generation)return;
                    if(PostMessageW(lifetime->hwnd,kAsyncImageReadyMessage,0,
                                    reinterpret_cast<LPARAM>(result.get())))result.release();
                });
            }
            return {};
        }
        std::vector<unsigned char> bytes;std::wstring decodeError;
        std::shared_ptr<RasterImage> image;
        if(LoadBinaryResource(reference,bytes))image=DecodeRasterImage(bytes,key,&decodeError);
        if(image&&image->animated){
            const auto now=GetTickCount64();
            image->nextFrameTick=now+std::max(1u,image->frames.front().delayMs);
        }
        rasterImageCache.emplace(key,image);
        if(image&&image->animated)SyncImageAnimationTimer();
        return image;
    }

    std::vector<std::shared_ptr<RasterImage>> ActiveRasterImages()const{
        std::vector<std::shared_ptr<RasterImage>> result;
        std::unordered_set<const RasterImage*> seen;
        const auto append=[&](const std::shared_ptr<RasterImage>& image){
            if(image&&seen.insert(image.get()).second)result.push_back(image);
        };
        for(const auto& item:rasterImageCache)append(item.second);
        for(const auto& node:document.QuerySelectorAll(L"img"))append(node->image);
        return result;
    }

    bool LoadImageNode(const std::shared_ptr<Node>& node,bool dispatchEvent){
        if(!node||node->tag!=L"img")return false;
        const auto source=node->Attribute(L"src");
        if(node->imageSource==source&&node->imageComplete)return false;
        node->imageSource=source;node->image.reset();node->imageComplete=source.empty();
        if(source.empty())return true;
        const auto key=ResolveResourceReference(source);
        node->image=ResolveRasterImage(source);
        node->imageComplete=!parallelResourceLoading||rasterImageCache.count(key)>0;
        if(!node->imageComplete){
            auto& nodes=pendingImageNodes[key];
            if(std::find(nodes.begin(),nodes.end(),node)==nodes.end())nodes.push_back(node);
        }
        if(dispatchEvent&&node->imageComplete){JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
            javascript.DispatchNodeEvent(node,node->image?L"load":L"error",event);
            javascript.CompleteImageRequest(node);}
        return true;
    }
    void SyncImageAnimationTimer(){
        KillTimer(hwnd,kImageAnimationTimer);imageAnimationTimerActive=false;
        const auto now=GetTickCount64();std::uint32_t delay=0;
        for(const auto& image:ActiveRasterImages()){
            const auto candidate=image->TimeUntilNextFrame(now);
            if(candidate&&(!delay||candidate<delay))delay=candidate;
        }
        if(delay)imageAnimationTimerActive=
            SetTimer(hwnd,kImageAnimationTimer,std::max(1u,delay),nullptr)!=0;
    }
    bool LoadImages(bool dispatchEvents){
        bool changed=false;
        for(const auto& node:document.QuerySelectorAll(L"img"))
            changed=LoadImageNode(node,dispatchEvents)||changed;
        SyncImageAnimationTimer();return changed;
    }
    void ClearPendingImageLoads(){
        pendingImageNodes.clear();
        javascript.CancelPendingImageRequests();
    }
    void CompleteAsyncImage(std::unique_ptr<AsyncImageResult> result){
        if(!result||result->generation!=resourceGeneration)return;
        pendingRasterImages.erase(result->key);
        if(result->image&&result->image->animated){
            const auto now=GetTickCount64();
            result->image->nextFrameTick=now+
                std::max(1u,result->image->frames.front().delayMs);
        }
        rasterImageCache[result->key]=result->image;
        std::vector<std::shared_ptr<Node>> completed;
        std::unordered_set<const Node*> seen;
        const auto append=[&](const std::shared_ptr<Node>& node){
            if(!node||!seen.insert(node.get()).second)return;
            const auto source=node->Attribute(L"src");
            if(source.empty()||ResolveResourceReference(source)!=result->key)return;
            node->image=result->image;node->imageSource=source;node->imageComplete=true;
            completed.push_back(node);
        };
        const auto pending=pendingImageNodes.find(result->key);
        if(pending!=pendingImageNodes.end()){
            auto nodes=std::move(pending->second);pendingImageNodes.erase(pending);
            for(const auto& node:nodes)append(node);
        }
        for(const auto& node:document.QuerySelectorAll(L"img"))append(node);
        layoutDirty=true;viewportOnlyDirty=false;SyncImageAnimationTimer();
        InvalidateView();
        for(const auto& node:completed){
            if(result->generation!=resourceGeneration)break;
            JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
            javascript.DispatchNodeEvent(node,result->image?L"load":L"error",event);
            javascript.CompleteImageRequest(node);
        }
    }
    void AdvanceImageAnimations(){
        KillTimer(hwnd,kImageAnimationTimer);imageAnimationTimerActive=false;
        const auto now=GetTickCount64();bool changed=false;
        for(const auto& image:ActiveRasterImages())changed=image->Advance(now)||changed;
        if(changed)InvalidateView();
        SyncImageAnimationTimer();
    }

    float DpiScale()const{
        if(compositionParent)return compositionParent->DpiScale();
        const UINT dpi=hwnd?GetDpiForWindow(hwnd):USER_DEFAULT_SCREEN_DPI;
        return std::max(1.0f,static_cast<float>(dpi)/static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    }
    float PixelToDip(float value)const{return value/DpiScale();}
    LONG DipToPixel(float value)const{return static_cast<LONG>(std::lround(value*DpiScale()));}
    void UpdateTooltipMetrics(){
        if(!tooltip)return;
        if(tooltipFont){DeleteObject(tooltipFont);tooltipFont=nullptr;}
        tooltipFont=CreateFontW(-DipToPixel(12.0f),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
            DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        if(tooltipFont)SendMessageW(tooltip,WM_SETFONT,reinterpret_cast<WPARAM>(tooltipFont),FALSE);
        RECT margin{DipToPixel(6.0f),DipToPixel(3.0f),DipToPixel(6.0f),DipToPixel(3.0f)};
        SendMessageW(tooltip,TTM_SETMARGIN,0,reinterpret_cast<LPARAM>(&margin));
        SendMessageW(tooltip,TTM_SETTIPBKCOLOR,RGB(255,255,255),0);
        SendMessageW(tooltip,TTM_SETTIPTEXTCOLOR,RGB(0,0,0),0);
        SendMessageW(tooltip,TTM_SETMAXTIPWIDTH,0,DipToPixel(360.0f));
    }
    bool InitializeTooltip(HINSTANCE instance){
        INITCOMMONCONTROLSEX commonControls{sizeof(commonControls),ICC_WIN95_CLASSES};
        InitCommonControlsEx(&commonControls);
        tooltip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,
            WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,
            CW_USEDEFAULT,CW_USEDEFAULT,hwnd,nullptr,instance,nullptr);
        if(!tooltip)return false;
        SetWindowPos(tooltip,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        // Chromium's plain title tooltip is rectangular and uses the page's
        // neutral white palette instead of the themed Windows help balloon.
        SetWindowTheme(tooltip,L"",L"");
        // The host application may use either comctl32 v5 or v6. V2 is the
        // newest TOOLINFO layout accepted by both versions on supported Windows.
        tooltipTool.cbSize=TTTOOLINFOW_V2_SIZE;
        tooltipTool.uFlags=TTF_TRACK|TTF_ABSOLUTE|TTF_TRANSPARENT;
        tooltipTool.hwnd=hwnd;tooltipTool.uId=kTooltipToolId;
        GetClientRect(hwnd,&tooltipTool.rect);
        tooltipTool.lpszText=const_cast<LPWSTR>(L" ");
        if(!SendMessageW(tooltip,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tooltipTool))){
            DestroyWindow(tooltip);tooltip=nullptr;return false;
        }
        UpdateTooltipMetrics();return true;
    }
    void CancelTooltipHover(){
        if(!hwnd)return;
        TRACKMOUSEEVENT cancel{sizeof(cancel),TME_CANCEL|TME_HOVER,hwnd,0};
        TrackMouseEvent(&cancel);
    }
    void HideTooltip(bool clearTarget=true){
        CancelTooltipHover();
        if(tooltip&&tooltipVisible)
            SendMessageW(tooltip,TTM_TRACKACTIVATE,FALSE,reinterpret_cast<LPARAM>(&tooltipTool));
        tooltipVisible=false;
        if(clearTarget){tooltipOwner.reset();tooltipText.clear();}
    }
    static std::pair<std::shared_ptr<Node>,std::wstring> TooltipForNode(
        const std::shared_ptr<Node>& node){
        for(auto current=node;current;current=current->parent.lock()){
            if(current->attributes.count(L"title")){
                const auto text=current->Attribute(L"title");
                // An explicitly empty title suppresses a title inherited from
                // an ancestor, matching the HTML title advisory-text rule.
                return text.empty()?std::pair<std::shared_ptr<Node>,std::wstring>{}:
                    std::make_pair(current,text);
            }
        }
        return {};
    }
    void UpdateTooltipTarget(const std::shared_ptr<Node>& hit){
        const auto candidate=TooltipForNode(hit);
        if(candidate.first==tooltipOwner&&candidate.second==tooltipText)return;
        HideTooltip(false);tooltipOwner=candidate.first;tooltipText=candidate.second;
        if(!tooltipOwner||tooltipText.empty())return;
        TRACKMOUSEEVENT hover{sizeof(hover),TME_HOVER,hwnd,HOVER_DEFAULT};
        TrackMouseEvent(&hover);
    }
    void ShowTooltipAt(LPARAM position){
        if(!tooltip||!tooltipOwner||tooltipText.empty())return;
        const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(position)));
        const float y=PixelToDip(static_cast<float>(GET_Y_LPARAM(position)));
        if(layoutDirty)Rebuild();
        const auto hit=layout.HitTest(x,y);const auto candidate=TooltipForNode(hit);
        if(candidate.first!=tooltipOwner||candidate.second!=tooltipText){
            UpdateTooltipTarget(hit);return;
        }
        POINT screenPoint{GET_X_LPARAM(position),GET_Y_LPARAM(position)};
        ClientToScreen(hwnd,&screenPoint);
        const UINT dpi=GetDpiForWindow(hwnd);
        const int pointerOffset=std::max(1,GetSystemMetricsForDpi(SM_CYCURSOR,dpi)/2);
        const POINT pointer=screenPoint;screenPoint.y+=pointerOffset;
        tooltipTool.lpszText=const_cast<LPWSTR>(tooltipText.c_str());
        SendMessageW(tooltip,TTM_UPDATETIPTEXTW,0,reinterpret_cast<LPARAM>(&tooltipTool));
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const HMONITOR monitor=MonitorFromPoint(pointer,MONITOR_DEFAULTTONEAREST);
        const bool hasMonitor=monitor&&GetMonitorInfoW(monitor,&monitorInfo);
        SendMessageW(tooltip,TTM_TRACKPOSITION,0,MAKELPARAM(screenPoint.x,screenPoint.y));
        SendMessageW(tooltip,TTM_TRACKACTIVATE,TRUE,reinterpret_cast<LPARAM>(&tooltipTool));
        RECT tooltipBounds{};
        if(hasMonitor&&GetWindowRect(tooltip,&tooltipBounds)){
            const RECT& work=monitorInfo.rcWork;
            const int tooltipWidth=tooltipBounds.right-tooltipBounds.left;
            const int tooltipHeight=tooltipBounds.bottom-tooltipBounds.top;
            screenPoint.x=std::max(work.left,std::min(screenPoint.x,work.right-tooltipWidth));
            if(screenPoint.y+tooltipHeight>work.bottom)
                screenPoint.y=pointer.y-pointerOffset-tooltipHeight;
            screenPoint.y=std::max(work.top,std::min(screenPoint.y,work.bottom-tooltipHeight));
            SendMessageW(tooltip,TTM_TRACKPOSITION,0,MAKELPARAM(screenPoint.x,screenPoint.y));
        }
        tooltipVisible=true;
    }
    static void IncludeBounds(LayoutRect& bounds,bool& initialized,const LayoutRect& item){
        if(item.width<=0||item.height<=0)return;
        if(!initialized){bounds=item;initialized=true;return;}
        const float left=std::min(bounds.x,item.x),top=std::min(bounds.y,item.y);
        const float right=std::max(bounds.x+bounds.width,item.x+item.width);
        const float bottom=std::max(bounds.y+bounds.height,item.y+item.height);
        bounds={left,top,right-left,bottom-top};
    }
    void InvalidateView(const RECT* dirty=nullptr){
        InvalidateRect(hwnd,dirty,FALSE);
        if(compositionParent){
            RECT bounds{};
            if(dirty)bounds=*dirty;else GetClientRect(hwnd,&bounds);
            MapWindowPoints(hwnd,compositionParent->hwnd,reinterpret_cast<POINT*>(&bounds),2);
            InflateRect(&bounds,1,1);
            compositionParent->InvalidateView(&bounds);
        }
    }
    void InvalidateDipBounds(const LayoutRect& bounds){
        const float scale=DpiScale();
        RECT dirty{static_cast<LONG>(std::floor(bounds.x*scale)),
            static_cast<LONG>(std::floor(bounds.y*scale)),
            static_cast<LONG>(std::ceil((bounds.x+bounds.width)*scale)),
            static_cast<LONG>(std::ceil((bounds.y+bounds.height)*scale))};
        InflateRect(&dirty,1,1);RECT client{};GetClientRect(hwnd,&client);
        RECT clipped{};if(IntersectRect(&clipped,&dirty,&client))InvalidateView(&clipped);
    }
    void InvalidateScrollViewport(const std::shared_ptr<Node>& node){
        // Replaced elements backed by child HWNDs participate in the same
        // scroll coordinate space as painted layout boxes.  LayoutEngine moves
        // those boxes as scroll offsets change, so mirror the new coordinates
        // before repainting the exposed viewport.
        UpdateFrameBounds();
        if(!layoutDirty)if(const auto* box=layout.BoxFor(node)){
            // Scrolled descendants are clipped to the scroll container's border box.
            // Invalidate only that box, converting CSS pixels to device pixels in
            // InvalidateDipBounds so the rule remains correct at every monitor DPI.
            InvalidateDipBounds(box->rect);return;
        }
        InvalidateView();
    }
    void UpdateJavaScriptViewport(){
        RECT bounds{};GetClientRect(hwnd,&bounds);const float scale=DpiScale();
        javascript.SetViewportSize(frameViewportWidth>0?frameViewportWidth:
            static_cast<double>(std::max(1L,bounds.right-bounds.left))/scale,
            frameViewportHeight>0?frameViewportHeight:
            static_cast<double>(std::max(1L,bounds.bottom-bounds.top))/scale);
        javascript.SetDevicePixelRatio(scale);
        MONITORINFO monitor{sizeof(monitor)};
        if(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)){
            const float displayWidth=(monitor.rcMonitor.right-monitor.rcMonitor.left)/scale;
            const float displayHeight=(monitor.rcMonitor.bottom-monitor.rcMonitor.top)/scale;
            javascript.SetDisplaySize(displayWidth,displayHeight,
                (monitor.rcWork.right-monitor.rcWork.left)/scale,
                (monitor.rcWork.bottom-monitor.rcWork.top)/scale);
            styles.SetDisplay(displayWidth,displayHeight,scale);
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wParam,LPARAM lParam){
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(lParam);self=static_cast<Impl*>(cs->lpCreateParams);self->hwnd=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        return self?self->HandleMessage(message,wParam,lParam):DefWindowProcW(hwnd,message,wParam,lParam);
    }
    bool Initialize(HWND parent,const RECT& bounds){
        auto instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent,GWLP_HINSTANCE));if(!instance)instance=GetModuleHandleW(nullptr);if(!EnsureWindowClass(instance,kWindowClass,&Impl::WindowProc)){lastError=L"Failed to register TWebFrame window class";return false;}
        hwnd=CreateWindowExW(0,kWindowClass,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS|WS_CLIPCHILDREN,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,parent,nullptr,instance,this);if(!hwnd){lastError=L"Failed to create TWebFrame child window";return false;}
        {std::lock_guard<std::mutex> lock(asyncLifetime->mutex);asyncLifetime->hwnd=hwnd;}
        InitializeTooltip(instance);
        DragAcceptFiles(hwnd,TRUE);
        HRESULT hr=D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,d2dFactory.ReleaseAndGetAddressOf());if(FAILED(hr)){lastError=L"D2D1CreateFactory failed";return false;}
        hr=DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf()));if(FAILED(hr)){lastError=L"DWriteCreateFactory failed";return false;}
        layout.SetRasterImageResolver([this](const std::wstring& reference){
            return ResolveRasterImage(reference);
        });
        layout.SetFramePainter([this](ID2D1RenderTarget* target,const LayoutBox& box){
            PaintChildFrame(target,box);
        });
        javascript.SetBrowserContext(browserContext,storageSession);
        javascript.SetMessageSink([this](const std::wstring& msg){if(messageHandler)messageHandler(msg);});
        javascript.SetMutationSink([this](const JavaScriptRuntime::Mutation& mutation){HandleMutation(mutation);});
        javascript.SetFocusSink([this](const std::shared_ptr<Node>& node){
            if(node&&node->ownerDocument&&node->ownerDocument!=&document)
                if(const auto child=FindDocumentView(node->ownerDocument)){
                    child->impl_->SetFocusedNode(node,focusVisibleFromKeyboard,true);return;
                }
            SetFocusedNode(node,focusVisibleFromKeyboard,true);
        });
        javascript.SetDocumentFocusProvider([this]{
            const auto focus=GetFocus();
            return focus==hwnd||(focus&&IsChild(hwnd,focus));
        });
        javascript.SetWindowFocusSink([this](const auto& frame,bool focus){
            SetBrowsingContextFocus(frame,focus);
        });
        javascript.SetActivationSink([this](const std::shared_ptr<Node>& node){
            if(!node)return;JavaScriptRuntime::EventInit click{};click.isTrusted=false;
            Activate(node,0,0,false,click,false);
        });
        javascript.SetPointerCaptureSink([this](bool capture){
            if(capture){if(GetCapture()!=hwnd)SetCapture(hwnd);}else if(GetCapture()==hwnd)ReleaseCapture();
        });
        javascript.SetSelectionProvider([this](const std::shared_ptr<Node>& node,size_t& start,size_t& end){
            if(!IsTextControl(node))return false;
            if(node==focused&&editingNode==node){start=std::min(selectionAnchor,caretPosition);end=std::max(selectionAnchor,caretPosition);}
            else{start=node->selectionStart;end=node->selectionEnd;}
            return true;
        });
        javascript.SetSelectionSetter([this](const std::shared_ptr<Node>& node,size_t start,size_t end){
            if(node!=focused||!IsTextControl(node))return;editingNode=node;
            const auto length=node->Attribute(L"value").size();const auto first=std::min(start,length),last=std::min(end,length);
            if(node->selectionDirection==L"backward"){selectionAnchor=last;caretPosition=first;}
            else{selectionAnchor=first;caretPosition=last;}ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);
        });
        javascript.SetDomSelectionProvider([this](JavaScriptRuntime::DomSelection& selection){
            if(!focused||(!editingNode&&!editingBoundaryContainer)||
               (!IsTextControl(focused)&&!IsContentEditable(focused)))return false;
            if(editingBoundaryContainer){
                const auto offset=std::min(editingBoundaryOffset,
                                           editingBoundaryContainer->children.size());
                selection.anchorNode=selection.focusNode=editingBoundaryContainer;
                selection.anchorOffset=selection.focusOffset=offset;
                return true;
            }
            selection.anchorNode=editingNode;selection.anchorOffset=std::min(selectionAnchor,EditingValue().size());
            selection.focusNode=editingNode;selection.focusOffset=std::min(caretPosition,EditingValue().size());
            return true;
        });
        javascript.SetDomSelectionSetter([this](const JavaScriptRuntime::DomSelection& selection){
            if(!selection.anchorNode||!selection.focusNode)return;
            const auto root=IsTextControl(selection.anchorNode)?selection.anchorNode:EditableRoot(selection.anchorNode);
            if(!root)return;
            // addRange() supplies the exact editing position, so focusing here
            // must not create a surrogate text node before that position is
            // installed.
            SetFocusedNode(root,true,false);
            auto target=selection.anchorNode;
            size_t anchorOffset=selection.anchorOffset,focusOffset=selection.focusOffset;
            if(IsContentEditable(root)&&target->type!=NodeType::Text&&
               selection.focusNode==target&&selection.focusOffset==selection.anchorOffset){
                const auto boundary=EditableBoundary(target,selection.anchorOffset);
                if(!boundary.first)return;
                editingNode.reset();editingBoundaryContainer=boundary.first;
                editingBoundaryOffset=boundary.second;
                selectionAnchor=caretPosition=0;verticalCaretXValid=false;
                ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return;
            }else{
                if(target->type!=NodeType::Text&&target!=root)target=EnsureEditableTextNode(target);
                if(target==root&&IsContentEditable(root))target=EnsureEditableTextNode(root);
            }
            if(!target)return;
            ClearEditingBoundary();
            editingNode=target;const auto length=EditingValue().size();
            selectionAnchor=std::min(anchorOffset,length);
            caretPosition=selection.focusNode==target?
                std::min(focusOffset,length):selectionAnchor;
            verticalCaretXValid=false;
            StoreControlSelection();ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);
        });
        javascript.SetFrameScheduler([this]{
            if(!SetTimer(hwnd,kAnimationFrameTimer,16,nullptr))
                PostMessageW(hwnd,kAnimationFrameFallbackMessage,0,0);
        });
        javascript.SetFrameDocumentSink([this](const std::shared_ptr<Node>& node,
                                               const std::wstring& html){
            CommitChildFrameDocument(node,html);
        });
        javascript.SetFrameDocumentProvider([this](const std::shared_ptr<Node>& node){
            auto found=std::find_if(childFrames.begin(),childFrames.end(),[&](const ChildFrame& frame){return frame.node==node;});
            if(found==childFrames.end()){
                StartChildFrame(node);
                found=std::find_if(childFrames.begin(),childFrames.end(),[&](const ChildFrame& frame){return frame.node==node;});
            }
            if(found==childFrames.end())return std::shared_ptr<Document>{};
            // An initial about:blank document is available synchronously to
            // scripts immediately after inserting the iframe.
            if(!found->view->impl_->document.Body())
                found->view->impl_->document.Parse(L"<!doctype html><html><head></head><body></body></html>");
            return found->view->impl_->documentOwner;
        });
        javascript.SetFrameRuntimeProvider([this](const std::shared_ptr<Node>& node){
            auto found=std::find_if(childFrames.begin(),childFrames.end(),[&](const ChildFrame& frame){return frame.node==node;});
            if(found==childFrames.end()){
                StartChildFrame(node);
                found=std::find_if(childFrames.begin(),childFrames.end(),[&](const ChildFrame& frame){return frame.node==node;});
            }
            if(found==childFrames.end())return std::shared_ptr<JavaScriptRuntime>{};
            // Retained functions keep their originating browsing context alive
            // even after its iframe is removed from the parent document.
            auto child=found->view;
            return std::shared_ptr<JavaScriptRuntime>(std::move(child),&found->view->impl_->javascript);
        });
        javascript.SetFrameSelectionProvider([this](const auto& node,auto& selection){
            for(const auto& frame:childFrames)if(frame.node==node)return frame.view->impl_->javascript.ReadDomSelection(selection);
            return false;
        },[this](const auto& node,const auto& selection){
            for(const auto& frame:childFrames)if(frame.node==node){auto child=frame.view;child->impl_->javascript.WriteDomSelection(selection);break;}
        });
        javascript.SetTimerScheduler([this](unsigned delay){
            KillTimer(hwnd,kJavaScriptTimer);
            if(delay)SetTimer(hwnd,kJavaScriptTimer,std::max(1u,delay),nullptr);
        });
        javascript.SetWorkerWakeHandler([lifetime=asyncLifetime]{
            std::lock_guard<std::mutex> lock(lifetime->mutex);
            if(lifetime->alive&&lifetime->hwnd)PostMessageW(lifetime->hwnd,kWorkerReadyMessage,0,0);
        });
        javascript.SetResourceLoader([this](const std::wstring& resource,std::wstring& content){
            return LoadTextResource(resource,content);
        });
        const auto sendRequest=[](const ScriptRequest& request,const View::ResourceLoader& loader,
                                  const std::wstring& base,const std::shared_ptr<BrowserContext>& http){
            // The text resource callback cannot represent HTTP status, headers,
            // redirects or CORS. All script HTTP methods use the full transport.
            if(request.url.rfind(L"https://",0)==0||request.url.rfind(L"http://",0)==0)return http->Request(request);
            ScriptResponse response;response.url=request.url;
            if(LoadTextResourceForBase(loader,base,request.url,response.body)){
                response.status=200;response.statusText=L"OK";
            }return response;
        };
        javascript.SetRequestLoader([this,sendRequest](const ScriptRequest& request){
            return sendRequest(request,resourceLoader,basePath,browserContext);
        });
        javascript.SetAsyncRequestLoader([this,sendRequest](const ScriptRequest& request,std::function<void(ScriptResponse)> complete){
            if(!parallelResourceLoading){complete(sendRequest(request,resourceLoader,basePath,browserContext));return;}
            const auto loader=resourceLoader;const auto base=basePath;const auto http=browserContext;
            const auto lifetime=asyncLifetime;const auto generation=resourceGeneration;
            ImageWorkers().Submit(BackgroundWorkQueue::Priority::Critical,
                [request,complete=std::move(complete),sendRequest,loader,base,http,lifetime,generation]() mutable {
                {
                    std::lock_guard<std::mutex> lock(lifetime->mutex);
                    if(!lifetime->alive||!lifetime->hwnd||lifetime->resourceGeneration!=generation)return;
                }
                auto result=std::make_unique<AsyncTextResult>();result->generation=generation;
                result->requestCompletion=std::move(complete);result->response=sendRequest(request,loader,base,http);
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                if(!lifetime->alive||!lifetime->hwnd||lifetime->resourceGeneration!=generation)return;
                if(PostMessageW(lifetime->hwnd,kAsyncTextReadyMessage,0,reinterpret_cast<LPARAM>(result.get())))result.release();
            });
        });
        javascript.SetAsyncResourceLoader([this](const std::wstring& resource,
                                                  std::function<void(bool,std::wstring)> complete){
            if(!parallelResourceLoading){
                std::wstring content;const bool loaded=LoadTextResource(resource,content);
                complete(loaded,std::move(content));return;
            }
            const auto loader=TextLoader(currentLocation);const auto base=basePath;
            const auto lifetime=asyncLifetime;
            const auto generation=resourceGeneration;
            ImageWorkers().Submit(BackgroundWorkQueue::Priority::Critical,
                [resource,complete=std::move(complete),loader,base,
                 lifetime,generation]() mutable {
                {
                    std::lock_guard<std::mutex> lock(lifetime->mutex);
                    if(!lifetime->alive||!lifetime->hwnd||
                       lifetime->resourceGeneration!=generation)return;
                }
                auto result=std::make_unique<AsyncTextResult>();
                result->generation=generation;result->completion=std::move(complete);
                result->loaded=LoadTextResourceForBase(loader,base,resource,result->content);
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                if(!lifetime->alive||!lifetime->hwnd||
                   lifetime->resourceGeneration!=generation)return;
                if(PostMessageW(lifetime->hwnd,kAsyncTextReadyMessage,0,
                                reinterpret_cast<LPARAM>(result.get())))result.release();
            });
        });
        javascript.SetNavigationSink([this](const std::wstring& target){
            pendingNavigation=target;
            pendingNavigationNewWindow=false;
            PostMessageW(hwnd,kNavigationMessage,0,0);
        });
        javascript.SetSameDocumentNavigationSink([this](const std::wstring& target,bool replace,int delta){
            currentLocation=target;basePath=target;
            if(historyChangedHandler)historyChangedHandler(target,replace,delta);
        });
        javascript.SetHistoryTraversalSink([this](int delta){
            PostMessageW(hwnd,kHistoryTraversalMessage,static_cast<WPARAM>(static_cast<INT_PTR>(delta)),0);
        });
        javascript.SetDialogSink([this](const std::wstring& message){RunScriptDialog(message,false);});
        javascript.SetConfirmSink([this](const std::wstring& message){
            return RunScriptDialog(message,true);
        });
        javascript.SetDocumentWriteSink([this](const std::wstring& html){
            // document.open()/write()/close() creates a replacement Document
            // in the same browsing context.  Keep the JavaScript Window and
            // its globals (for example SafeFrame's bootstrap configuration)
            // while rebuilding DOM, CSS, resources, layout, and child frames.
            LoadHtmlInternal(&html,{},{},{},{},basePath,currentLocation,true);
        });
        javascript.SetFrameMessageSink([this](const std::shared_ptr<Node>& node,const std::wstring& data,
                                              const std::wstring& origin){
            for(auto& frame:childFrames)if(frame.node==node){
                frame.view->impl_->javascript.DispatchWindowMessageAsJson(data,{},origin);
                frame.view->impl_->layoutDirty=true;
                frame.view->impl_->InvalidateView();
                break;
            }
        });
        javascript.SetGeometryProvider([this](const std::shared_ptr<Node>& node){
            return ReadNodeGeometry(node);
        });
        javascript.SetSvgGeometryProvider([this](const std::shared_ptr<Node>& node){
            auto* owner=this;auto child=node?FindDocumentView(node->ownerDocument):std::shared_ptr<View>{};
            if(child)owner=child->impl_.get();
            LayoutRect bounds;JavaScriptRuntime::NodeGeometry result;
            if(SvgObjectBoundingBox(node,owner->styles,bounds,owner->d2dFactory.Get(),owner->writeFactory.Get())){
                result.x=bounds.x;result.y=bounds.y;result.width=bounds.width;result.height=bounds.height;
            }
            return result;
        });
        javascript.SetSvgTextLengthProvider([this](const std::shared_ptr<Node>& node,float& length,std::vector<LayoutRect>* characters){
            auto* owner=this;auto child=node?FindDocumentView(node->ownerDocument):std::shared_ptr<View>{};
            if(child)owner=child->impl_.get();
            return SvgTextAdvanceLength(node,owner->styles,length,owner->writeFactory.Get(),characters);
        });
        javascript.SetStylePropertyProvider([this](const std::shared_ptr<Node>& node,const std::wstring& property){
            auto* owner=this;auto child=node?FindDocumentView(node->ownerDocument):std::shared_ptr<View>{};
            if(child)owner=child->impl_.get();
            if(owner->layoutDirty)owner->Rebuild();
            if(const auto* box=owner->layout.BoxFor(node))return box->style.Get(property);
            return owner->styles.Compute(node).Get(property);
        });
        InitializeAccessibility();
        return true;
    }
    std::shared_ptr<View> FindDocumentView(const Document* owner) const {
        if(!owner||owner==&document)return {};
        for(const auto& frame:childFrames){
            if(frame.view->impl_->documentOwner.get()==owner)return frame.view;
            if(const auto nested=frame.view->impl_->FindDocumentView(owner))return nested;
        }
        return {};
    }
    void SetBrowsingContextFocus(const std::shared_ptr<Node>& frame,bool focus){
        if(frame){
            if(frame->ownerDocument&&frame->ownerDocument!=&document){
                if(const auto owner=FindDocumentView(frame->ownerDocument))
                    owner->impl_->SetBrowsingContextFocus(frame,focus);
                return;
            }
            auto found=std::find_if(childFrames.begin(),childFrames.end(),
                [&](const ChildFrame& child){return child.node==frame;});
            if(found==childFrames.end()){
                StartChildFrame(frame);
                found=std::find_if(childFrames.begin(),childFrames.end(),
                    [&](const ChildFrame& child){return child.node==frame;});
            }
            if(found!=childFrames.end()){
                auto retained=found->view;
                retained->impl_->SetBrowsingContextFocus({},focus);
            }
            return;
        }
        if(focus)SetFocus(hwnd);
        else if(GetFocus()==hwnd)SetFocus(GetParent(hwnd));
    }
    JavaScriptRuntime::NodeGeometry ReadNodeGeometry(const std::shared_ptr<Node>& node){
            if(node&&node->ownerDocument&&node->ownerDocument!=&document)
                if(const auto child=FindDocumentView(node->ownerDocument))return child->impl_->ReadNodeGeometry(node);
            JavaScriptRuntime::NodeGeometry geometry;
            if(layoutDirty)Rebuild();
            if(const auto* box=layout.BoxFor(node)){
                geometry.x=box->rect.x;geometry.y=box->rect.y;
                geometry.width=box->rect.width;geometry.height=box->rect.height;
                geometry.clientWidth=box->content.width;geometry.clientHeight=box->content.height;
                geometry.scrollWidth=std::max(box->content.width,box->scrollWidth);
                geometry.scrollHeight=std::max(box->content.height,box->scrollHeight);
            }
            // In standards mode the root element's client box represents the
            // viewport, even though the layout root itself is an internal
            // wrapper rather than an ordinary CSS content box.
            if(node&&node==document.QuerySelector(L"html")){
                RECT client{};GetClientRect(hwnd,&client);const float scale=DpiScale();
                geometry.clientWidth=static_cast<float>(std::max(1L,client.right))/scale;
                geometry.clientHeight=static_cast<float>(std::max(1L,client.bottom))/scale;
                const auto* scrollingBox=layout.Root();
                geometry.scrollWidth=std::max(
                    static_cast<double>(scrollingBox?scrollingBox->scrollWidth:0.0f),
                    geometry.clientWidth);
                geometry.scrollHeight=std::max(
                    static_cast<double>(scrollingBox?scrollingBox->scrollHeight:0.0f),
                    geometry.clientHeight);
            }
            return geometry;
    }
    void DiscardCompositedResources(){
        layout.DiscardDeviceResources();
        if(scriptDialog)scriptDialog->layout.DiscardDeviceResources();
        for(const auto& frame:childFrames)if(frame.view)
            frame.view->impl_->DiscardCompositedResources();
    }
    void ResetRenderTargets(){DiscardCompositedResources();backBuffer.Reset();backBufferPixelSize={};backBufferDpi=0;renderTarget.Reset();}
    void EnsureTarget(){
        if(renderTarget||!d2dFactory)return;
        DiscardCompositedResources();backBuffer.Reset();backBufferPixelSize={};backBufferDpi=0;
        RECT r{};GetClientRect(hwnd,&r);const auto size=D2D1::SizeU(
            static_cast<UINT32>(std::max(1L,r.right-r.left)),
            static_cast<UINT32>(std::max(1L,r.bottom-r.top)));
        // Interactive HTML surfaces already pace animation through their
        // requestAnimationFrame timer.  Present editor and pointer updates as
        // soon as the dirty back buffer is ready instead of making EndDraw
        // wait for the desktop compositor's next presentation interval.
        const auto properties=D2D1::HwndRenderTargetProperties(
            hwnd,size,D2D1_PRESENT_OPTIONS_IMMEDIATELY);
        if(SUCCEEDED(d2dFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),properties,
            renderTarget.ReleaseAndGetAddressOf()))){const float dpi=USER_DEFAULT_SCREEN_DPI*DpiScale();renderTarget->SetDpi(dpi,dpi);}
    }
    bool EnsureBackBuffer(D2D1_SIZE_U required,float dpi){
        if(!renderTarget)return false;
        const bool dpiChanged=std::abs(backBufferDpi-dpi)>0.01f;
        const bool tooSmall=backBufferPixelSize.width<required.width||backBufferPixelSize.height<required.height;
        const bool excessivelyLarge=backBufferPixelSize.width>required.width*2u||backBufferPixelSize.height>required.height*2u;
        if(backBuffer&&!dpiChanged&&!tooSmall&&!excessivelyLarge)return true;
        D2D1_SIZE_U capacity=required;
        if(backBuffer&&!dpiChanged&&tooSmall){
            capacity.width=std::max(required.width,backBufferPixelSize.width+std::max(64u,backBufferPixelSize.width/4u));
            capacity.height=std::max(required.height,backBufferPixelSize.height+std::max(64u,backBufferPixelSize.height/4u));
        }
        DiscardCompositedResources();backBuffer.Reset();backBufferPixelSize={};backBufferDpi=0;
        const auto dipSize=D2D1::SizeF(capacity.width*USER_DEFAULT_SCREEN_DPI/dpi,
            capacity.height*USER_DEFAULT_SCREEN_DPI/dpi);
        const auto pixelFormat=D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN,D2D1_ALPHA_MODE_IGNORE);
        if(FAILED(renderTarget->CreateCompatibleRenderTarget(&dipSize,&capacity,&pixelFormat,
            D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,backBuffer.ReleaseAndGetAddressOf())))return false;
        backBuffer->SetDpi(dpi,dpi);backBufferPixelSize=capacity;backBufferDpi=dpi;return true;
    }
    void UpdateFrameBounds(){
        const float scale=DpiScale();
        struct FramePlacement { HWND window=nullptr;RECT bounds{};bool visible=false; };
        std::vector<FramePlacement> placements;placements.reserve(childFrames.size());
        // Moving or showing a child HWND can synchronously dispatch messages
        // that mutate the DOM and resynchronize childFrames. Compute the full
        // placement list first so vector erasure/reallocation cannot invalidate
        // the loop that is currently applying layout.
        for(const auto& frame:childFrames){
            const auto* box=layout.BoxFor(frame.node);
            FramePlacement placement;placement.window=frame.view?frame.view->Window():nullptr;
            if(!box||!box->visible||box->content.width<=0||box->content.height<=0){
                placements.push_back(placement);continue;
            }
            auto* child=frame.view->impl_.get();
            if(child->frameViewportWidth!=box->content.width||child->frameViewportHeight!=box->content.height){
                child->frameViewportWidth=box->content.width;child->frameViewportHeight=box->content.height;
                const bool viewportOnly=!child->layoutDirty||child->viewportOnlyDirty;
                child->layoutDirty=true;child->viewportOnlyDirty=viewportOnly;child->UpdateJavaScriptViewport();
            }
            RECT bounds{static_cast<LONG>(std::floor(box->content.x*scale)),
                static_cast<LONG>(std::floor(box->content.y*scale)),
                static_cast<LONG>(std::ceil((box->content.x+box->content.width)*scale)),
                static_cast<LONG>(std::ceil((box->content.y+box->content.height)*scale))};
            placement.bounds=bounds;placement.visible=true;placements.push_back(placement);
        }
        for(const auto& placement:placements){
            if(!placement.window||!IsWindow(placement.window))continue;
            if(!placement.visible){ShowWindow(placement.window,SW_HIDE);continue;}
            MoveWindow(placement.window,placement.bounds.left,placement.bounds.top,
                placement.bounds.right-placement.bounds.left,
                placement.bounds.bottom-placement.bounds.top,FALSE);
            if(IsWindow(placement.window))ShowWindow(placement.window,SW_SHOW);
        }
    }
    void SyncCssTransitionTimer(){
        if(layout.HasActiveTransitions()){
            if(!cssTransitionTimerActive){
                cssTransitionTick=std::chrono::steady_clock::now();
                cssTransitionTimerActive=SetTimer(hwnd,kCssTransitionTimer,16,nullptr)!=0;
            }
        }else if(cssTransitionTimerActive){
            KillTimer(hwnd,kCssTransitionTimer);cssTransitionTimerActive=false;
        }
    }
    void SyncDocumentStyles(){
        if(!stylesheetSourcesDirty)return;
        stylesheetSourcesDirty=false;
        std::unordered_map<std::wstring,std::wstring> used;
        std::wstring error;
        const auto css=CollectDocumentStyles(document,basePath,[&](const auto& reference,auto& content){
            const auto key=ResolveResourceReference(reference);
            const auto cached=stylesheetResources.find(key);
            if(cached!=stylesheetResources.end())content=cached->second;
            else LoadTextResource(reference,content);
            used.emplace(key,content);return true;
        },false,error);
        stylesheetResources.swap(used);
        if(css==installedStylesheetText)return;
        if(styles.Parse(css,&error)){
            installedStylesheetText=css;UpdateJavaScriptViewport();
            layoutDirty=true;viewportOnlyDirty=false;
        }
    }
    void Rebuild(){SyncDocumentStyles();RECT r{};GetClientRect(hwnd,&r);const float scale=DpiScale();const float width=frameViewportWidth>0?frameViewportWidth:static_cast<float>(std::max(1L,r.right))/scale,height=frameViewportHeight>0?frameViewportHeight:static_cast<float>(std::max(1L,r.bottom))/scale;javascript.SetViewportSize(width,height);if(viewportOnlyDirty)layout.Relayout(width,height,scale);else layout.Layout(width,height,scale);layoutDirty=false;viewportOnlyDirty=false;accessibilityTreeDirty=true;if(accessibility)accessibility->Invalidate();UpdateFrameBounds();SyncCssTransitionTimer();}
    bool AccessibilityHidden(const std::shared_ptr<Node>& node)const{
        for(auto current=node;current;current=current->parent.lock())
            if(ToLower(current->Attribute(L"aria-hidden"))==L"true"||current->attributes.count(L"hidden"))return true;
        return false;
    }
    bool IsAccessibilityNode(const std::shared_ptr<Node>& node){
        if(!node||node->type==NodeType::Document||AccessibilityHidden(node))return false;
        if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);
        if(node->tag==L"option"){
            const auto container=OwningSelect(node);const auto* selectBox=container?layout.BoxFor(container):nullptr;
            return selectBox&&selectBox->visible;
        }
        if(!box||!box->visible)return false;
        if(node->type==NodeType::Text){
            if(Trim(node->text).empty())return false;const auto parent=node->parent.lock();
            return !parent||!(IsFocusable(parent)||parent->tag==L"button"||parent->tag==L"label"||
                parent->tag==L"option"||!parent->Attribute(L"aria-label").empty());
        }
        const auto role=ToLower(node->Attribute(L"role"));
        if(role==L"none"||role==L"presentation")return IsFocusable(node);
        if(IsFocusable(node)||!role.empty()||!node->Attribute(L"aria-label").empty()||
           !node->Attribute(L"aria-labelledby").empty()||!node->Attribute(L"aria-live").empty())return true;
        const auto& tag=node->tag;
        return tag==L"h1"||tag==L"h2"||tag==L"h3"||tag==L"h4"||tag==L"h5"||tag==L"h6"||
            tag==L"img"||tag==L"ul"||tag==L"ol"||tag==L"li"||tag==L"table"||tag==L"tr"||
            tag==L"th"||tag==L"td"||tag==L"nav"||tag==L"main"||tag==L"section"||tag==L"aside";
    }
    void EnsureAccessibilityTree(){
        if(!accessibilityTreeDirty)return;
        if(layoutDirty)Rebuild();
        accessibilityChildrenCache.clear();accessibilityPositionCache.clear();
        accessibilityAutomationIdCache.clear();
        std::function<void(const std::shared_ptr<Node>&,const std::wstring&)> cacheAutomationIds=
            [&](const std::shared_ptr<Node>& parent,const std::wstring& parentPath){
                std::unordered_map<std::wstring,size_t> indexes;
                for(const auto& child:parent->children){
                    if(!child)continue;
                    const auto name=child->type==NodeType::Text?L"text":
                        (child->tag.empty()?L"node":child->tag);
                    const auto index=++indexes[name];
                    const auto path=parentPath.empty()?name+L"["+std::to_wstring(index)+L"]":
                        parentPath+L"/"+name+L"["+std::to_wstring(index)+L"]";
                    std::wstring automationId;
                    for(const auto* attribute:{L"data-automation-id",L"id",L"name"}){
                        automationId=child->Attribute(attribute);if(!automationId.empty())break;
                    }
                    accessibilityAutomationIdCache.emplace(
                        child.get(),automationId.empty()?path:std::move(automationId));
                    cacheAutomationIds(child,path);
                }
            };
        const auto root=document.Root();if(root)cacheAutomationIds(root,L"");
        std::function<void(const std::shared_ptr<Node>&,const std::shared_ptr<Node>&)> collect=
            [&](const std::shared_ptr<Node>& current,const std::shared_ptr<Node>& accessibleParent){
                if(!current||current->attributes.count(L"hidden")||
                   ToLower(current->Attribute(L"aria-hidden"))==L"true")return;
                auto parent=accessibleParent;
                if(IsAccessibilityNode(current)){
                    auto& siblings=accessibilityChildrenCache[parent.get()];
                    accessibilityPositionCache[current.get()]={parent,parent.get(),siblings.size()};
                    siblings.push_back(current);parent=current;
                }
                for(const auto& child:current->children)collect(child,parent);
            };
        if(root)for(const auto& child:root->children)collect(child,{});
        accessibilityTreeDirty=false;
    }
    std::vector<std::shared_ptr<Node>> AccessibilityChildren(const std::shared_ptr<Node>& parent){
        EnsureAccessibilityTree();const auto found=accessibilityChildrenCache.find(parent.get());
        return found==accessibilityChildrenCache.end()?std::vector<std::shared_ptr<Node>>{}:found->second;
    }
    std::shared_ptr<Node> AccessibilityParent(const std::shared_ptr<Node>& node){
        EnsureAccessibilityTree();const auto found=accessibilityPositionCache.find(node.get());
        return found==accessibilityPositionCache.end()?std::shared_ptr<Node>{}:found->second.parent.lock();
    }
    std::shared_ptr<Node> AccessibilitySibling(const std::shared_ptr<Node>& node,bool next){
        EnsureAccessibilityTree();const auto found=accessibilityPositionCache.find(node.get());
        if(found==accessibilityPositionCache.end())return {};
        const auto siblings=accessibilityChildrenCache.find(found->second.parentKey);
        if(siblings==accessibilityChildrenCache.end())return {};
        const auto index=found->second.index;
        if(next)return index+1<siblings->second.size()?siblings->second[index+1]:std::shared_ptr<Node>{};
        return index>0&&index<=siblings->second.size()?siblings->second[index-1]:std::shared_ptr<Node>{};
    }
    std::wstring AccessibilityName(const std::shared_ptr<Node>& node){
        if(!node)return L"TWebFrame";if(node->type==NodeType::Text)return Trim(node->text);
        auto label=node->Attribute(L"aria-label");if(!label.empty())return label;
        const auto labelledBy=node->Attribute(L"aria-labelledby");
        if(!labelledBy.empty()){
            std::wistringstream ids(labelledBy);std::wstring id,name;
            while(ids>>id)if(auto source=document.GetElementById(id)){if(!name.empty())name+=L" ";name+=Trim(source->InnerText());}
            if(!name.empty())return name;
        }
        const auto id=node->Attribute(L"id");
        if(!id.empty())for(const auto& candidate:document.QuerySelectorAll(L"label"))
            if(candidate->Attribute(L"for")==id&&!Trim(candidate->InnerText()).empty())return Trim(candidate->InnerText());
        if(node->tag==L"img"&&!node->Attribute(L"alt").empty())return node->Attribute(L"alt");
        if(!node->Attribute(L"title").empty())return node->Attribute(L"title");
        const auto type=ToLower(node->Attribute(L"type"));
        if(node->tag==L"input"&&(type==L"button"||type==L"submit"||type==L"reset"||type==L"file")&&!node->Attribute(L"value").empty())return node->Attribute(L"value");
        if(IsTextInput(node)&&!node->Attribute(L"placeholder").empty())return node->Attribute(L"placeholder");
        if(node->tag==L"select"){
            const auto options=SelectOptions(node);if(!options.empty())return Trim(options[SelectedOptionIndex(node,options)]->InnerText());
        }
        const auto role=ToLower(node->Attribute(L"role"));
        const bool roleNamesFromContent=role==L"button"||role==L"menuitem"||role==L"option"||
            role==L"tab"||role==L"checkbox"||role==L"radio"||role==L"switch"||
            role==L"status"||role==L"alert"||role==L"link"||role==L"heading"||
            role==L"treeitem"||role==L"cell"||role==L"columnheader"||role==L"rowheader";
        const auto& tag=node->tag;
        const bool tagNamesFromContent=tag==L"button"||tag==L"a"||tag==L"label"||
            tag==L"option"||tag==L"li"||tag==L"th"||tag==L"td"||tag==L"summary"||
            tag==L"legend"||(tag.size()==2&&tag[0]==L'h'&&tag[1]>=L'1'&&tag[1]<=L'6');
        return roleNamesFromContent||tagNamesFromContent?Trim(node->InnerText()):L"";
    }
    std::wstring StableAutomationId(const std::shared_ptr<Node>& node)const{
        if(!node)return L"twebframe-root";
        const auto cached=accessibilityAutomationIdCache.find(node.get());
        if(cached!=accessibilityAutomationIdCache.end())return cached->second;
        for(const auto* attribute:{L"data-automation-id",L"id",L"name"}){
            const auto value=node->Attribute(attribute);if(!value.empty())return value;
        }
        std::vector<std::wstring> parts;
        for(auto current=node;current&&current!=document.Root();current=current->parent.lock()){
            const auto parent=current->parent.lock();size_t index=1;
            if(parent)for(const auto& sibling:parent->children){
                if(sibling==current)break;if(sibling->type==current->type&&sibling->tag==current->tag)++index;
            }
            const auto name=current->type==NodeType::Text?L"text":(current->tag.empty()?L"node":current->tag);
            parts.push_back(name+L"["+std::to_wstring(index)+L"]");
        }
        std::wstring result;
        for(auto it=parts.rbegin();it!=parts.rend();++it){if(!result.empty())result+=L"/";result+=*it;}
        return result.empty()?L"twebframe-element":result;
    }
    std::shared_ptr<Node> OwningSelect(const std::shared_ptr<Node>& node)const{
        auto current=node;
        while(current&&current->tag!=L"select")current=current->parent.lock();return current;
    }
    AccessibilityNodeInfo AccessibilityInfo(const std::shared_ptr<Node>& node){
        AccessibilityNodeInfo info;info.root=!node;info.valid=info.root||IsAccessibilityNode(node);if(!info.valid)return info;
        if(layoutDirty)Rebuild();
        if(info.root){
            info.controlType=UIA_PaneControlTypeId;info.name=L"TWebFrame";info.automationId=L"twebframe-root";
            info.className=L"TWebFrame.View";info.focusable=true;info.focused=GetFocus()==hwnd||IsChild(hwnd,GetFocus());
            RECT rect{};GetClientRect(hwnd,&rect);POINT origin{0,0};ClientToScreen(hwnd,&origin);
            info.bounds={static_cast<double>(origin.x),static_cast<double>(origin.y),
                         static_cast<double>(rect.right),static_cast<double>(rect.bottom)};return info;
        }
        EnsureAccessibilityTree();
        info.name=AccessibilityName(node);info.automationId=StableAutomationId(node);
        info.className=node->type==NodeType::Text?L"#text":node->tag;info.focusable=IsFocusable(node);
        info.focused=node->focused;info.enabled=!node->disabled&&ToLower(node->Attribute(L"aria-disabled"))!=L"true";
        info.readOnly=node->attributes.count(L"readonly")!=0||ToLower(node->Attribute(L"aria-readonly"))==L"true";info.password=ToLower(node->Attribute(L"type"))==L"password";
        info.helpText=node->Attribute(L"aria-description");if(info.helpText.empty())info.helpText=node->Attribute(L"title");
        info.ariaRole=ToLower(node->Attribute(L"role"));const auto role=info.ariaRole;
        const auto checked=ToLower(node->Attribute(L"aria-checked"));
        info.mixed=node->indeterminate||checked==L"mixed";info.checked=node->checked||checked==L"true";
        info.expanded=IsDatalistInput(node)?openSelectPopup==node:ToLower(node->Attribute(L"aria-expanded"))==L"true";
        const auto live=ToLower(node->Attribute(L"aria-live"));info.liveSetting=live==L"assertive"?Assertive:live==L"polite"?Polite:Off;
        std::vector<std::wstring> aria;
        if(node->attributes.count(L"aria-checked"))aria.push_back(L"checked="+checked);
        if(node->disabled||node->attributes.count(L"aria-disabled"))aria.push_back(L"disabled="+std::wstring(info.enabled?L"false":L"true"));
        if(node->attributes.count(L"aria-expanded"))aria.push_back(L"expanded="+std::wstring(info.expanded?L"true":L"false"));
        if(!live.empty())aria.push_back(L"live="+live);
        for(const auto& item:aria){if(!info.ariaProperties.empty())info.ariaProperties+=L";";info.ariaProperties+=item;}
        const auto type=ToLower(node->Attribute(L"type"));
        if(role==L"button")info.controlType=UIA_ButtonControlTypeId;
        else if(role==L"menu")info.controlType=UIA_MenuControlTypeId;
        else if(role==L"menubar")info.controlType=UIA_MenuBarControlTypeId;
        else if(role==L"menuitem")info.controlType=UIA_MenuItemControlTypeId;
        else if(role==L"checkbox"||role==L"switch")info.controlType=UIA_CheckBoxControlTypeId;
        else if(role==L"radio")info.controlType=UIA_RadioButtonControlTypeId;
        else if(role==L"tab")info.controlType=UIA_TabItemControlTypeId;
        else if(role==L"tablist")info.controlType=UIA_TabControlTypeId;
        else if(role==L"option")info.controlType=UIA_ListItemControlTypeId;
        else if(role==L"listbox")info.controlType=UIA_ListControlTypeId;
        else if(role==L"textbox")info.controlType=UIA_EditControlTypeId;
        else if(role==L"status"||role==L"alert")info.controlType=UIA_TextControlTypeId;
        else if(node->tag==L"button")info.controlType=UIA_ButtonControlTypeId;
        else if(node->tag==L"a")info.controlType=UIA_HyperlinkControlTypeId;
        else if(node->tag==L"select"||IsDatalistInput(node))info.controlType=UIA_ComboBoxControlTypeId;
        else if(node->tag==L"option")info.controlType=UIA_ListItemControlTypeId;
        else if(node->tag==L"input"&&(type==L"checkbox"||type==L"radio"))info.controlType=type==L"radio"?UIA_RadioButtonControlTypeId:UIA_CheckBoxControlTypeId;
        else if(IsTextInput(node)||node->tag==L"textarea")info.controlType=UIA_EditControlTypeId;
        else if(node->tag==L"img")info.controlType=UIA_ImageControlTypeId;
        else if(node->tag==L"ul"||node->tag==L"ol")info.controlType=UIA_ListControlTypeId;
        else if(node->tag==L"li")info.controlType=UIA_ListItemControlTypeId;
        else if(node->tag==L"table")info.controlType=UIA_TableControlTypeId;
        else if(node->tag==L"th")info.controlType=UIA_HeaderItemControlTypeId;
        else if(node->tag==L"tr"||node->tag==L"td")info.controlType=UIA_DataItemControlTypeId;
        else if(node->type==NodeType::Text||(!node->tag.empty()&&node->tag[0]==L'h'&&node->tag.size()==2))info.controlType=UIA_TextControlTypeId;
        else info.controlType=UIA_GroupControlTypeId;
        info.toggle=(node->tag==L"input"&&(type==L"checkbox"||type==L"radio"))||role==L"checkbox"||role==L"radio"||role==L"switch";
        info.invoke=IsKeyboardActivatable(node)&&!info.toggle;info.valuePattern=IsTextInput(node)||node->tag==L"textarea"||role==L"textbox";
        info.selection=node->tag==L"select"||role==L"listbox";info.selectionItem=node->tag==L"option"||role==L"option";
        info.expandCollapse=node->attributes.count(L"aria-expanded")!=0||IsDatalistInput(node);
        if(IsTextInput(node)||node->tag==L"textarea"||role==L"textbox")info.value=node->Attribute(L"value");
        else if(node->tag==L"select")info.value=node->Attribute(L"value");
        if(info.selectionItem){
            const auto select=OwningSelect(node);if(select){const auto options=SelectOptions(select);if(!options.empty())info.selected=options[SelectedOptionIndex(select,options)]==node;}
            else info.selected=node->attributes.count(L"aria-selected")&&ToLower(node->Attribute(L"aria-selected"))==L"true";
        }
        auto boundsNode=node;if(node->tag==L"option")if(auto select=OwningSelect(node))boundsNode=select;
        const auto* box=layout.BoxFor(boundsNode);RECT client{};GetClientRect(hwnd,&client);POINT origin{0,0};ClientToScreen(hwnd,&origin);
        if(box&&box->visible){const float scale=DpiScale();info.bounds={origin.x+box->rect.x*scale,origin.y+box->rect.y*scale,box->rect.width*scale,box->rect.height*scale};
            info.offscreen=box->rect.width<=0||box->rect.height<=0||box->rect.x+box->rect.width<=0||box->rect.y+box->rect.height<=0||box->rect.x>=client.right/scale||box->rect.y>=client.bottom/scale;
        }else info.offscreen=true;
        return info;
    }
    void InitializeAccessibility(){
        accessibility=std::make_shared<AccessibilityHost>(hwnd);
        accessibility->SetCallbacks(
            [this](const auto& node){return AccessibilityInfo(node);},
            [this](const auto& node){return AccessibilityChildren(node);},
            [this](const auto& node){return AccessibilityParent(node);},
            [this](const auto& node,bool next){return AccessibilitySibling(node,next);},
            [this](double screenX,double screenY){POINT point{static_cast<LONG>(std::lround(screenX)),static_cast<LONG>(std::lround(screenY))};ScreenToClient(hwnd,&point);if(layoutDirty)Rebuild();auto node=layout.HitTest(PixelToDip(static_cast<float>(point.x)),PixelToDip(static_cast<float>(point.y)));while(node&&!IsAccessibilityNode(node))node=node->parent.lock();return node;},
            [this]{return focused;},
            [this](const auto& node)->HRESULT{if(!IsFocusable(node))return UIA_E_NOTSUPPORTED;SetFocus(hwnd);SetFocusedNode(node,true,true);return S_OK;},
            [this](const auto& node)->HRESULT{if(!node||!AccessibilityInfo(node).enabled)return UIA_E_ELEMENTNOTENABLED;if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);if(!box)return UIA_E_ELEMENTNOTAVAILABLE;Activate(node,box->rect.x+box->rect.width/2.0f,box->rect.y+box->rect.height/2.0f);return S_OK;},
            [this](const auto& node,const std::wstring& value)->HRESULT{if(!node||(!IsTextInput(node)&&node->tag!=L"textarea"&&ToLower(node->Attribute(L"role"))!=L"textbox"))return UIA_E_NOTSUPPORTED;if(node->disabled||node->attributes.count(L"readonly"))return UIA_E_ELEMENTNOTENABLED;const auto old=node->Attribute(L"value");if(old==value)return S_OK;node->SetAttribute(L"value",value);javascript.DispatchNodeEvent(node,L"input");javascript.DispatchNodeEvent(node,L"change");RefreshTextSelectionFromDom();layoutDirty=true;InvalidateView();return S_OK;},
            [this](const auto& node)->HRESULT{const auto select=OwningSelect(node);if(select){const auto options=SelectOptions(select);const auto it=std::find(options.begin(),options.end(),node);if(it==options.end()||node->disabled)return UIA_E_ELEMENTNOTENABLED;SelectOption(select,static_cast<size_t>(std::distance(options.begin(),it)));return S_OK;}if(node&&ToLower(node->Attribute(L"role"))==L"option"){const auto parent=node->parent.lock();if(!parent)return UIA_E_NOTSUPPORTED;for(const auto& sibling:parent->children)if(ToLower(sibling->Attribute(L"role"))==L"option")sibling->SetAttribute(L"aria-selected",sibling==node?L"true":L"false");javascript.DispatchNodeEvent(node,L"click");javascript.DispatchNodeEvent(node,L"change");layoutDirty=true;InvalidateView();return S_OK;}return UIA_E_NOTSUPPORTED;},
            [this](const auto& node)->HRESULT{if(!node)return UIA_E_ELEMENTNOTAVAILABLE;const auto role=ToLower(node->Attribute(L"role")),type=ToLower(node->Attribute(L"type"));const bool native=node->tag==L"input"&&(type==L"checkbox"||type==L"radio");if(!native&&role!=L"checkbox"&&role!=L"radio"&&role!=L"switch")return UIA_E_NOTSUPPORTED;const auto before=ToLower(node->Attribute(L"aria-checked"));if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);Activate(node,box?box->rect.x:0,box?box->rect.y:0);if(!native&&ToLower(node->Attribute(L"aria-checked"))==before)node->SetAttribute(L"aria-checked",before==L"true"?L"false":L"true");layoutDirty=true;InvalidateView();return S_OK;},
            [this](const auto& node,bool expand)->HRESULT{
                if(!node)return UIA_E_ELEMENTNOTAVAILABLE;
                if(IsDatalistInput(node)){
                    if(expand){SetFocus(hwnd);SetFocusedNode(node,true,true);OpenSelectPopup(node,true,false);}
                    else if(openSelectPopup==node)CloseSelectPopup();
                    return S_OK;
                }
                if(!node->attributes.count(L"aria-expanded"))return UIA_E_NOTSUPPORTED;const auto before=node->Attribute(L"aria-expanded");if(layoutDirty)Rebuild();const auto* box=layout.BoxFor(node);Activate(node,box?box->rect.x:0,box?box->rect.y:0);if(ToLower(node->Attribute(L"aria-expanded"))==ToLower(before))node->SetAttribute(L"aria-expanded",expand?L"true":L"false");layoutDirty=true;InvalidateView();return S_OK;
            });
    }
    static bool IsWithin(const std::shared_ptr<Node>& node,const std::shared_ptr<Node>& ancestor){
        for(auto current=node;current;current=current->parent.lock())if(current==ancestor)return true;
        return false;
    }
    bool IsConnectedToDocument(const std::shared_ptr<Node>& node)const{
        if(!node)return false;
        auto current=node;while(auto parent=current->ComposedParent())current=std::move(parent);
        return current==document.Root();
    }
    void RefreshLiveRegionIndex(const JavaScriptRuntime::Mutation& mutation,bool reset){
        std::vector<std::shared_ptr<Node>> indexed;
        auto add=[&](const std::shared_ptr<Node>& node){
            if(!node||node->attributes.count(L"aria-live")==0)return;
            if(std::find(indexed.begin(),indexed.end(),node)==indexed.end())indexed.push_back(node);
        };
        if(!reset){
            for(const auto& weak:liveRegions)if(auto node=weak.lock())
                if(IsConnectedToDocument(node)&&node->attributes.count(L"aria-live"))add(node);
        }
        if(reset||mutation.targets.empty()){
            indexed=document.QuerySelectorAll(L"[aria-live]");
        }else if(mutation.kind==JavaScriptRuntime::MutationKind::Tree||
                 mutation.liveRegionMembershipChanged){
            for(const auto& target:mutation.targets)
                for(const auto& node:document.QuerySelectorAll(L"[aria-live]",target))add(node);
        }
        liveRegions.clear();liveRegions.reserve(indexed.size());
        for(const auto& node:indexed)liveRegions.push_back(node);
    }
    void NotifyAccessibilityMutation(const JavaScriptRuntime::Mutation& mutation,bool reset=false){
        if(!accessibility)return;
        if(reset||mutation.kind==JavaScriptRuntime::MutationKind::Tree||
           mutation.liveRegionMembershipChanged)RefreshLiveRegionIndex(mutation,reset);
        std::unordered_set<const Node*> active;
        for(const auto& weak:liveRegions)if(auto node=weak.lock())active.insert(node.get());
        for(auto item=liveRegionText.begin();item!=liveRegionText.end();)
            if(active.count(item->first)==0)item=liveRegionText.erase(item);else ++item;
        if(liveRegions.empty()||(!reset&&mutation.kind!=JavaScriptRuntime::MutationKind::Tree&&
           !mutation.liveRegionMembershipChanged))return;
        for(const auto& weak:liveRegions){
            const auto node=weak.lock();if(!node)continue;
            bool affected=reset||mutation.liveRegionMembershipChanged||mutation.targets.empty();
            if(!affected)for(const auto& target:mutation.targets)
                if(IsWithin(target,node)||IsWithin(node,target)){affected=true;break;}
            if(!affected)continue;
            const auto text=Trim(node->InnerText());
            const auto previous=liveRegionText.find(node.get());
            if(previous!=liveRegionText.end()&&previous->second!=text)
                accessibility->RaiseLiveRegionChanged(node);
            liveRegionText[node.get()]=text;
        }
    }
    std::wstring ChildFrameSource(const std::shared_ptr<Node>& node)const{
        if(!node)return {};
        const auto srcdoc=node->attributes.find(L"srcdoc");
        if(srcdoc!=node->attributes.end())return L"srcdoc:"+srcdoc->second;
        const auto source=Trim(node->Attribute(L"src"));
        return source.empty()||ToLower(source)==L"about:blank"?
            L"about:blank":ResolveResourceReference(source);
    }
    void DispatchChildFrameEvent(const std::shared_ptr<Node>& node,bool success){
        JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
        javascript.DispatchNodeEvent(node,success?L"load":L"error",event);
        layoutDirty=true;viewportOnlyDirty=false;InvalidateView();
    }
    void StartChildFrame(const std::shared_ptr<Node>& node,
                         const std::unordered_map<std::wstring,std::wstring>* preparedText=nullptr,
                         const std::unordered_set<std::wstring>* failedText=nullptr){
        if(!node||node->tag!=L"iframe"||!IsConnectedToDocument(node))return;
        const auto descriptor=ChildFrameSource(node);
        RECT bounds{0,0,1,1};auto child=View::Create(hwnd,bounds);
        if(!child){DispatchChildFrameEvent(node,false);return;}
        // Keep the child HWND for focus, IME, accessibility and capture. Its
        // pixels are composited by the common layout painter rather than by
        // the Windows child-window order, which cannot express CSS z-index.
        child->impl_->compositionParent=this;
        child->impl_->javascript.SetSecureContextAncestors([frame=child->impl_.get(),creatorSecure=javascript.IsSecureContext()]{
            return frame->compositionParent?frame->compositionParent->javascript.IsSecureContext():creatorSecure;
        });
        if(const auto region=CreateRectRgn(0,0,0,0))
            if(!SetWindowRgn(child->Window(),region,FALSE))DeleteObject(region);
        child->SetResourceLoader(resourceLoader);
        child->SetNetworkResourceLoader(networkResourceLoader);
        child->SetBrowserContext(browserContext,storageSession);
        child->SetBinaryResourceLoader(binaryResourceLoader);
        child->SetParallelResourceLoading(parallelResourceLoading);
        child->SetPageScriptsEnabled(pageScriptsEnabled);
        child->impl_->javascript.SetCompatibilityBridgeEnabled(javascriptCompatibilityBridgeEnabled);
        child->SetExecutionYieldHandler(javascriptExecutionYieldHandler);
        child->SetNavigationHandler(navigationHandler);
        child->SetMessageHandler([this](const std::wstring& message){
            if(messageHandler)messageHandler(message);
        });
        // The iframe name initializes the child browsing context's Window
        // name and persists across that context's navigations.
        child->impl_->javascript.SetWindowName(javascript.FrameWindowName(node));
        JavaScriptRuntime::ParentMessageSink deliverToParent=
            [this,node](const std::wstring& data,const std::wstring& origin){
            javascript.DispatchWindowMessageAsJson(data,node,origin);
            layoutDirty=true;InvalidateView();
        };
        child->impl_->javascript.SetParentMessageSink(deliverToParent);
        child->impl_->javascript.SetEmbeddingFrame(&javascript,node);
        // The initial about:blank realm is observable before an asynchronous
        // document commits, including its inherited secure context.
        child->impl_->basePath=basePath;
        child->impl_->currentLocation=L"about:blank";
        child->impl_->javascript.SetLocation(L"about:blank");
        if(topMessageRelay)child->impl_->javascript.SetTopMessageSink(topMessageRelay);
        child->impl_->topMessageRelay=topMessageRelay?topMessageRelay:deliverToParent;
        const auto request=++nextChildFrameRequest;
        childFrames.push_back({node,std::move(child),descriptor,request});
        if(createdChildViews.size()%32==0)createdChildViews.erase(
            std::remove_if(createdChildViews.begin(),createdChildViews.end(),[](const auto& frame){return frame.expired();}),createdChildViews.end());
        createdChildViews.push_back(childFrames.back().view);
        if(parallelResourceLoading){
            const auto parentLifetime=asyncLifetime;const auto parentGeneration=resourceGeneration;
            childFrames.back().view->SetLoadHandler(
                [node,parentLifetime,parentGeneration,request](bool success,const std::wstring&){
                    auto result=std::make_unique<AsyncFrameResult>();
                    result->generation=parentGeneration;result->request=request;
                    result->success=success;result->node=node;
                    std::lock_guard<std::mutex> lock(parentLifetime->mutex);
                    if(!parentLifetime->alive||!parentLifetime->hwnd||
                       parentLifetime->resourceGeneration!=parentGeneration)return;
                    if(PostMessageW(parentLifetime->hwnd,kAsyncFrameReadyMessage,0,
                                    reinterpret_cast<LPARAM>(result.get())))result.release();
                });
        }

        std::wstring childHtml,frameBase,frameLocation,finalFrameUrl;
        bool loaded=false,deferSource=false;
        const auto srcdoc=node->attributes.find(L"srcdoc");
        if(srcdoc!=node->attributes.end()){
            childHtml=srcdoc->second;frameBase=basePath;frameLocation=L"about:srcdoc";loaded=true;
        }else{
            const auto source=Trim(node->Attribute(L"src"));
            if(source.empty()||ToLower(source)==L"about:blank"){
                childHtml=L"<!doctype html><html><head></head><body></body></html>";
                frameBase=basePath;frameLocation=L"about:blank";loaded=true;
            }else{
                const auto sourceKey=ResolveResourceReference(source);
                const auto prepared=preparedText?preparedText->find(sourceKey):
                    std::unordered_map<std::wstring,std::wstring>::const_iterator{};
                if(preparedText&&prepared!=preparedText->end()&&
                   !(BrowserContext::Origin(sourceKey)!=L"null"&&(networkResourceLoader||!resourceLoader))){
                    childHtml=prepared->second;loaded=true;
                }else if(failedText&&failedText->count(sourceKey)>0){
                    loaded=false;
                }else if(parallelResourceLoading){
                    deferSource=true;
                }else loaded=DocumentLoader()(source,childHtml,finalFrameUrl);
                const auto resolvedSource=ResolveResourceReference(source);
                if(resolvedSource.find(L"://")!=std::wstring::npos){
                    frameBase=resolvedSource;frameLocation=resolvedSource;
                }else{
                    auto framePath=std::filesystem::path(source);
                    const auto suffix=source.find_first_of(L"?#");
                    if(suffix!=std::wstring::npos)
                        framePath=std::filesystem::path(source.substr(0,suffix));
                    if(framePath.is_relative())framePath=std::filesystem::path(basePath)/framePath;
                    frameBase=framePath.parent_path().wstring();frameLocation=framePath.wstring();
                }
                if(!finalFrameUrl.empty())frameBase=frameLocation=finalFrameUrl;
                if(deferSource){
                    const auto loader=DocumentLoader();
                    const auto generation=resourceGeneration;const auto lifetime=asyncLifetime;
                    ImageWorkers().Submit(BackgroundWorkQueue::Priority::Normal,
                        [source,node,loader,generation,lifetime,
                         frameBase,frameLocation,request]{
                        {
                            std::lock_guard<std::mutex> lock(lifetime->mutex);
                            if(!lifetime->alive||!lifetime->hwnd||
                               lifetime->resourceGeneration!=generation)return;
                        }
                        auto result=std::make_unique<AsyncFrameSourceResult>();
                        result->generation=generation;result->request=request;result->node=node;
                        result->base=frameBase;result->location=frameLocation;
                        std::wstring finalUrl;
                        result->loaded=loader(source,result->html,finalUrl);
                        if(!finalUrl.empty())result->base=result->location=finalUrl;
                        std::lock_guard<std::mutex> lock(lifetime->mutex);
                        if(!lifetime->alive||!lifetime->hwnd||
                           lifetime->resourceGeneration!=generation)return;
                        if(PostMessageW(lifetime->hwnd,kAsyncFrameSourceReadyMessage,0,
                                        reinterpret_cast<LPARAM>(result.get())))result.release();
                    });
                    return;
                }
            }
        }
        auto found=std::find_if(childFrames.begin(),childFrames.end(),
            [&](const ChildFrame& frame){return frame.node==node&&frame.request==request;});
        if(found==childFrames.end())return;
        if(!loaded){childFrames.erase(found);DispatchChildFrameEvent(node,false);return;}
        auto& frame=*found->view;
        if(parallelResourceLoading){
            frame.impl_->LoadHtmlAsync(childHtml,frameBase,frameLocation);return;
        }
        if(!frame.impl_->LoadHtml(childHtml,frameBase,frameLocation)){
            childFrames.erase(found);DispatchChildFrameEvent(node,false);return;
        }
        DispatchChildFrameEvent(node,true);
    }
    void CommitChildFrameDocument(const std::shared_ptr<Node>& node,
                                  const std::wstring& html){
        if(node&&node->ownerDocument&&node->ownerDocument!=&document){
            if(const auto owner=FindDocumentView(node->ownerDocument))owner->impl_->CommitChildFrameDocument(node,html);
            return;
        }
        if(!node||node->tag!=L"iframe"||!IsConnectedToDocument(node))return;
        auto found=std::find_if(childFrames.begin(),childFrames.end(),
            [&](const ChildFrame& frame){return frame.node==node;});
        if(found==childFrames.end()){
            StartChildFrame(node);
            found=std::find_if(childFrames.begin(),childFrames.end(),
                [&](const ChildFrame& frame){return frame.node==node;});
        }
        if(found==childFrames.end())return;
        constexpr auto location=L"about:blank";
        auto child=found->view;
        // document.close() exposes its newly written DOM before returning.
        // Preserve the browsing context's Window and runtime across the write.
        child->impl_->CancelPendingLoads();
        const bool loaded=child->impl_->LoadHtmlInternal(&html,nullptr,nullptr,{}, {},basePath,location,true);
        DispatchChildFrameEvent(node,loaded);
    }
    void SyncChildFrames(const std::unordered_map<std::wstring,std::wstring>* preparedText=nullptr,
                         const std::unordered_set<std::wstring>* failedText=nullptr){
        std::vector<std::shared_ptr<Node>> nodes;
        std::function<void(const std::shared_ptr<Node>&)> collect=[&](const auto& current){
            if(current->tag==L"iframe")nodes.push_back(current);
            for(const auto& child:current->RenderChildren())collect(child);
        };
        collect(document.Root());
        std::unordered_set<const Node*> live;
        for(const auto& node:nodes)live.insert(node.get());
        childFrames.erase(std::remove_if(childFrames.begin(),childFrames.end(),
            [&](const ChildFrame& frame){
                return live.count(frame.node.get())==0||frame.source!=ChildFrameSource(frame.node);
            }),childFrames.end());
        for(const auto& node:nodes){
            const auto found=std::find_if(childFrames.begin(),childFrames.end(),
                [&](const ChildFrame& frame){return frame.node==node;});
            if(found==childFrames.end())StartChildFrame(node,preparedText,failedText);
            else found->view->impl_->javascript.SetWindowName(javascript.FrameWindowName(node));
        }
        UpdateFrameBounds();
    }
    void ScheduleChildFrameSync(){
        if(childFrameSyncPending)return;
        childFrameSyncPending=true;
        if(!PostMessageW(hwnd,kSyncChildFramesMessage,0,0))childFrameSyncPending=false;
    }
    void HandleMutation(const JavaScriptRuntime::Mutation& mutation){
        if(std::any_of(mutation.targets.begin(),mutation.targets.end(),[&](const auto& target){
            return target&&target->ownerDocument&&target->ownerDocument!=&document;})){
            JavaScriptRuntime::Mutation local=mutation;local.targets.clear();
            for(const auto& target:mutation.targets){
                if(!target||!target->ownerDocument||target->ownerDocument==&document){local.targets.push_back(target);continue;}
                if(const auto child=FindDocumentView(target->ownerDocument)){
                    auto forwarded=mutation;forwarded.targets={target};child->impl_->HandleMutation(forwarded);
                }
            }
            if(local.targets.empty())return;
            HandleMutation(local);return;
        }
        bool stylesMayHaveChanged=mutation.kind==JavaScriptRuntime::MutationKind::Tree;
        for(const auto& target:mutation.targets)
            for(auto node=target;node;node=node->parent.lock())
                if(node->tag==L"style"||node->tag==L"link"){stylesMayHaveChanged=true;break;}
        if(stylesMayHaveChanged){stylesheetSourcesDirty=true;layoutDirty=true;viewportOnlyDirty=false;}
        bool framesMayHaveChanged=mutation.kind==JavaScriptRuntime::MutationKind::Tree;
        if(!framesMayHaveChanged)for(const auto& target:mutation.targets)
            if(target&&target->tag==L"iframe"){framesMayHaveChanged=true;break;}
        if(framesMayHaveChanged)ScheduleChildFrameSync();
        bool imageChanged=false;
        if(mutation.kind==JavaScriptRuntime::MutationKind::Tree||
           mutation.kind==JavaScriptRuntime::MutationKind::Layout){
            imageChanged=LoadImages(true);
            // Image() and document.createElement('img') may begin fetching
            // before they are connected to the document tree.
            for(const auto& target:mutation.targets)
                imageChanged=LoadImageNode(target,true)||imageChanged;
        }
        if(imageChanged){layoutDirty=true;viewportOnlyDirty=false;}
        const bool declarationOnly=mutation.kind==JavaScriptRuntime::MutationKind::Style&&
            !mutation.selectorMatchingChanged&&!styles.AttributeAffectsStyle(L"style");
        LayoutRect dirtyBounds{};bool hasDirtyBounds=false,canTarget=!layoutDirty&&
            !mutation.targets.empty()&&(mutation.kind==JavaScriptRuntime::MutationKind::Paint||
                                        mutation.kind==JavaScriptRuntime::MutationKind::Style);
        if(canTarget&&mutation.kind==JavaScriptRuntime::MutationKind::Style&&!declarationOnly&&
           (styles.HasPseudoRules(L"before")||styles.HasPseudoRules(L"after")||
            styles.MutationRequiresBroadInvalidation()))canTarget=false;
        if(canTarget)for(const auto& target:mutation.targets){
            if(mutation.kind==JavaScriptRuntime::MutationKind::Paint){
                const auto* box=layout.BoxFor(target);if(!box){canTarget=false;break;}
                IncludeBounds(dirtyBounds,hasDirtyBounds,box->rect);
            }else{
                LayoutRect visual{};if(!layout.VisualBounds(target,visual)){canTarget=false;break;}
                IncludeBounds(dirtyBounds,hasDirtyBounds,visual);
            }
        }
        if(mutation.kind==JavaScriptRuntime::MutationKind::Tree||
           mutation.kind==JavaScriptRuntime::MutationKind::Layout){
            layoutDirty=true;viewportOnlyDirty=false;
        }else if(mutation.kind==JavaScriptRuntime::MutationKind::Style&&!layoutDirty){
            bool layoutChanged=false;
            // A selector change can add or remove generated boxes. Restyling
            // updates existing boxes, so generated-content styles retain the
            // conservative tree-rebuild path.
            if(!declarationOnly&&
               (styles.HasPseudoRules(L"before")||styles.HasPseudoRules(L"after"))){
                layoutChanged=true;
            }else if((!declarationOnly&&styles.MutationRequiresBroadInvalidation())||
                     mutation.targets.empty()){
                const auto* root=layout.Root();
                layoutChanged=!root||!root->node||layout.Restyle(root->node);
            }else{
                bool geometryChanged=false;
                for(const auto& target:mutation.targets){
                    bool targetGeometryChanged=false;
                    layoutChanged=layout.Restyle(target,&targetGeometryChanged)||layoutChanged;
                    geometryChanged=geometryChanged||targetGeometryChanged;
                }
                if(geometryChanged)UpdateFrameBounds();
            }
            layoutDirty=layoutChanged;
            if(layoutDirty)viewportOnlyDirty=false;
            else SyncCssTransitionTimer();
        }
        bool scrollChanged=false;
        if(!layoutDirty&&(mutation.kind==JavaScriptRuntime::MutationKind::Paint||
                          mutation.kind==JavaScriptRuntime::MutationKind::Style))
            for(const auto& target:mutation.targets)
                scrollChanged=layout.SyncScroll(target)||scrollChanged;
        if(scrollChanged)UpdateFrameBounds();
        if(canTarget&&!layoutDirty&&mutation.kind==JavaScriptRuntime::MutationKind::Style)
            for(const auto& target:mutation.targets){
                LayoutRect visual{};if(!layout.VisualBounds(target,visual)){canTarget=false;break;}
                IncludeBounds(dirtyBounds,hasDirtyBounds,visual);
            }
        accessibilityTreeDirty=true;if(accessibility)accessibility->Invalidate();
        if(canTarget&&!layoutDirty&&hasDirtyBounds)InvalidateDipBounds(dirtyBounds);
        else InvalidateView();
        if(hovered){
            if(IsConnectedToDocument(hovered))UpdateTooltipTarget(hovered);
            else UpdateTooltipTarget({});
        }
        NotifyAccessibilityMutation(mutation);
    }
    void RestyleDocument(){
        if(layoutDirty)return;
        const auto* root=layout.Root();
        layoutDirty=!root||!root->node||layout.Restyle(root->node);
        if(layoutDirty)viewportOnlyDirty=false;else SyncCssTransitionTimer();
    }
    static std::wstring AccessibilityJsonEscape(const std::wstring& value){
        std::wstring out;for(const auto character:value){if(character==L'\\'||character==L'"')out+=L'\\';if(character==L'\n')out+=L"\\n";else if(character!=L'\r')out+=character;}return out;
    }
    std::wstring DumpAccessibilityJson(){
        if(layoutDirty)Rebuild();std::wstring output=L"[";bool first=true;
        std::function<void(const std::shared_ptr<Node>&)> append=[&](const std::shared_ptr<Node>& parent){
            for(const auto& node:AccessibilityChildren(parent)){
                const auto info=AccessibilityInfo(node);if(!first)output+=L",";first=false;
                std::wostringstream item;item<<L"{\"automationId\":\""<<AccessibilityJsonEscape(info.automationId)
                    <<L"\",\"name\":\""<<AccessibilityJsonEscape(info.name)<<L"\",\"role\":\""
                    <<AccessibilityJsonEscape(info.ariaRole)<<L"\",\"controlType\":"<<info.controlType
                    <<L",\"enabled\":"<<(info.enabled?L"true":L"false")<<L",\"focusable\":"
                    <<(info.focusable?L"true":L"false")<<L",\"x\":"<<std::lround(info.bounds.left)
                    <<L",\"y\":"<<std::lround(info.bounds.top)<<L",\"width\":"<<std::lround(info.bounds.width)
                    <<L",\"height\":"<<std::lround(info.bounds.height)<<L"}";output+=item.str();append(node);
            }
        };
        append({});return output+L"]";
    }
    bool ScriptsExecuting()const{
        if(javascript.IsExecuting())return true;
        for(const auto& frame:childFrames)if(frame.view&&frame.view->impl_&&frame.view->impl_->ScriptsExecuting())return true;
        return false;
    }
    bool RenderingScriptBusy()const{
        if(executionUiServiceDepth)return true;
        const auto* root=this;while(root->compositionParent)root=root->compositionParent;
        // HWND teardown can send messages while a frame-vector entry is being
        // erased. Treat that interval as busy instead of traversing the entry.
        if(root->childFrameDestructionDepth||childFrameDestructionDepth)return true;
        return root->ScriptsExecuting();
    }
    void PaintChildFrame(ID2D1RenderTarget* target,const LayoutBox& box){
        const auto found=std::find_if(childFrames.begin(),childFrames.end(),
            [&](const ChildFrame& frame){return frame.node==box.node;});
        if(found==childFrames.end()||!found->view)return;
        // Rebuilding a child can run resize callbacks. Hold its owner and a
        // copy of the geometry across callbacks that may remove the iframe.
        const auto childView=found->view;const auto bounds=box.content;
        auto* child=childView->impl_.get();
        if(child->layoutDirty&&!RenderingScriptBusy())child->Rebuild();
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        const auto content=D2D1::RectF(bounds.x,bounds.y,bounds.x+bounds.width,bounds.y+bounds.height);
        target->PushAxisAlignedClip(content,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target->SetTransform(D2D1::Matrix3x2F::Translation(bounds.x,bounds.y)*transform);
        ComPtr<ID2D1SolidColorBrush> background;
        if(SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),&background)))
            target->FillRectangle(D2D1::RectF(0,0,bounds.width,bounds.height),background.Get());
        const LayoutRect viewport{0,0,bounds.width,bounds.height};
        child->layout.Paint(target,child->writeFactory.Get(),&viewport);
        child->PaintTextEditing(target);child->PaintSelectPopup(target);
        target->SetTransform(transform);target->PopAxisAlignedClip();
        // Child redraws are satisfied by this shared composition pass.
        ValidateRect(child->hwnd,nullptr);
    }
    HRESULT RenderSurface(ID2D1RenderTarget* target,const LayoutRect& dirty){
        if(!target)return E_INVALIDARG;target->BeginDraw();target->SetTransform(D2D1::IdentityMatrix());
        const auto dirtyRect=D2D1::RectF(dirty.x,dirty.y,dirty.x+dirty.width,dirty.y+dirty.height);
        target->PushAxisAlignedClip(dirtyRect,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ComPtr<ID2D1SolidColorBrush> background;
        // The initial HTML canvas is white; authored root/body backgrounds
        // are propagated by LayoutEngine::Paint over this opaque base.
        if(SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),&background)))
            target->FillRectangle(dirtyRect,background.Get());
        layout.Paint(target,writeFactory.Get(),&dirty);PaintTextEditing(target);PaintSelectPopup(target);
        if(scriptDialog&&scriptDialog->active)
            scriptDialog->layout.Paint(target,writeFactory.Get(),&dirty);
        target->PopAxisAlignedClip();
        return target->EndDraw();
    }
    void PrintClient(HDC dc){
        if(!dc||!d2dFactory||!writeFactory||painting)return;
        if(layoutDirty&&!RenderingScriptBusy())Rebuild();
        RECT client{};GetClientRect(hwnd,&client);
        const float scale=DpiScale();
        ComPtr<ID2D1DCRenderTarget> target;
        const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
        if(FAILED(d2dFactory->CreateDCRenderTarget(&properties,&target))||
           FAILED(target->BindDC(dc,&client)))return;
        target->SetDpi(USER_DEFAULT_SCREEN_DPI*scale,USER_DEFAULT_SCREEN_DPI*scale);
        DiscardCompositedResources();painting=true;
        RenderSurface(target.Get(),{0,0,(client.right-client.left)/scale,(client.bottom-client.top)/scale});
        painting=false;DiscardCompositedResources();
    }
    void Paint(){
        if(compositionParent){
            PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);
            RECT dirty=ps.rcPaint;
            if(IsRectEmpty(&dirty))GetClientRect(hwnd,&dirty);
            MapWindowPoints(hwnd,compositionParent->hwnd,reinterpret_cast<POINT*>(&dirty),2);
            compositionParent->InvalidateView(&dirty);return;
        }
        PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);
        // Moving/focusing nested HWNDs can synchronously request a parent
        // paint. Direct2D drawing and traversal cannot be entered recursively.
        if(painting||childFrameDestructionDepth){EndPaint(hwnd,&ps);return;}
        painting=true;EnsureTarget();
        if(renderTarget){
            // Build and paint the complete frame off-screen first. The HWND
            // target is resized only when the finished bitmap is ready, so a
            // live resize never exposes partially painted document content.
            if(layoutDirty&&!RenderingScriptBusy())Rebuild();
            RECT client{};GetClientRect(hwnd,&client);const auto pixelSize=D2D1::SizeU(
                static_cast<UINT32>(std::max(1L,client.right-client.left)),
                static_cast<UINT32>(std::max(1L,client.bottom-client.top)));
            const float dpi=USER_DEFAULT_SCREEN_DPI*DpiScale();
            auto* previousBackBuffer=backBuffer.Get();
            if(EnsureBackBuffer(pixelSize,dpi)){
                const float scale=DpiScale();LayoutRect dirty;
                if(previousBackBuffer!=backBuffer.Get()||IsRectEmpty(&ps.rcPaint))
                    dirty={0,0,pixelSize.width/scale,pixelSize.height/scale};
                else{
                    const float left=std::floor(ps.rcPaint.left/scale),top=std::floor(ps.rcPaint.top/scale);
                    const float right=std::ceil(ps.rcPaint.right/scale),bottom=std::ceil(ps.rcPaint.bottom/scale);
                    dirty={left,top,std::max(0.0f,right-left),std::max(0.0f,bottom-top)};
                }
                const HRESULT rendered=RenderSurface(backBuffer.Get(),dirty);
                if(SUCCEEDED(rendered)){
                    ComPtr<ID2D1Bitmap> bitmap;
                    if(SUCCEEDED(backBuffer->GetBitmap(&bitmap))){
                        const auto currentSize=renderTarget->GetPixelSize();
                        const HRESULT resized=currentSize.width==pixelSize.width&&currentSize.height==pixelSize.height?
                            S_OK:renderTarget->Resize(pixelSize);
                        if(SUCCEEDED(resized)){
                            renderTarget->SetDpi(dpi,dpi);
                            const auto dirtyBounds=D2D1::RectF(dirty.x,dirty.y,
                                dirty.x+dirty.width,dirty.y+dirty.height);
                            renderTarget->BeginDraw();renderTarget->SetTransform(D2D1::IdentityMatrix());
                            renderTarget->PushAxisAlignedClip(dirtyBounds,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                            renderTarget->DrawBitmap(bitmap.Get(),dirtyBounds,1.0f,
                                D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,dirtyBounds);
                            renderTarget->PopAxisAlignedClip();
                            const HRESULT result=renderTarget->EndDraw();
                            if(result==D2DERR_RECREATE_TARGET)ResetRenderTargets();
                        }else ResetRenderTargets();
                    }
                }else if(rendered==D2DERR_RECREATE_TARGET)ResetRenderTargets();
            }
        }
        painting=false;EndPaint(hwnd,&ps);
    }
    std::shared_ptr<Node> FocusTarget(const std::shared_ptr<Node>& node)const{
        if(IsTextControl(node))return node;
        if(auto root=EditableRoot(node))return root;
        return node;
    }
    bool CanEditText()const{
        return IsEditableTextControl(focused)||(focused&&IsContentEditable(focused)&&editingNode&&
            editingNode->type==NodeType::Text)||(focused&&IsContentEditable(focused)&&
            editingBoundaryContainer);
    }
    std::wstring EditingValue()const{
        if(!editingNode)return {};
        return editingNode->type==NodeType::Text?editingNode->text:editingNode->Attribute(L"value");
    }
    void SetEditingValue(const std::wstring& value){
        if(!editingNode)return;
        if(editingNode->type==NodeType::Text)editingNode->text=value;
        else editingNode->SetAttribute(L"value",value);
    }
    void StoreControlSelection(){
        if(IsTextControl(focused)){
            focused->selectionStart=std::min(selectionAnchor,caretPosition);
            focused->selectionEnd=std::max(selectionAnchor,caretPosition);
            focused->selectionDirection=selectionAnchor==caretPosition?L"none":selectionAnchor>caretPosition?L"backward":L"forward";
        }
    }
    std::shared_ptr<Node> FirstEditableText(const std::shared_ptr<Node>& parent)const{
        if(!parent)return {};
        if(parent->type==NodeType::Text)return parent;
        if(parent!=focused&&parent->attributes.count(L"contenteditable")&&!IsContentEditable(parent))return {};
        for(const auto& child:parent->children)if(auto text=FirstEditableText(child))return text;
        return {};
    }
    std::shared_ptr<Node> EnsureEditableTextNode(const std::shared_ptr<Node>& target){
        if(IsTextControl(focused))return focused;
        auto candidate=target;
        while(candidate&&candidate->type!=NodeType::Text&&candidate!=focused)candidate=candidate->parent.lock();
        if(candidate&&candidate->type==NodeType::Text&&EditableRoot(candidate)==focused)return candidate;
        if(auto text=FirstEditableText(target&&EditableRoot(target)==focused?target:focused))return text;
        if(!focused||!IsContentEditable(focused))return {};
        auto text=std::make_shared<Node>();text->type=NodeType::Text;text->text=L"";text->parent=focused;
        focused->children.push_back(text);document.Reindex();layoutDirty=true;return text;
    }
    void ClearEditingBoundary(){
        editingBoundaryContainer.reset();editingBoundaryOffset=0;
    }
    std::pair<std::shared_ptr<Node>,size_t> EditableBoundary(
        const std::shared_ptr<Node>& requestedContainer,size_t requestedOffset)const{
        if(!focused||!IsContentEditable(focused))return {};
        auto container=requestedContainer;size_t offset=requestedOffset;
        if(IsAtomicEditingElement(container)){
            const auto parent=container->parent.lock();if(!parent)return {};
            const auto position=std::find(parent->children.begin(),parent->children.end(),container);
            if(position==parent->children.end())return {};
            offset=static_cast<size_t>(position-parent->children.begin())+(requestedOffset?1:0);
            container=parent;
        }
        if(!container||container->type==NodeType::Text||
           (container!=focused&&EditableRoot(container)!=focused))return {};
        return {container,std::min(offset,container->children.size())};
    }
    std::pair<std::shared_ptr<Node>,size_t> EnsureEditableBoundaryText(
        const std::shared_ptr<Node>& requestedContainer,size_t requestedOffset){
        const auto boundary=EditableBoundary(requestedContainer,requestedOffset);
        auto container=boundary.first;size_t offset=boundary.second;
        if(!container)return {};
        // A lone <br> is the browser-compatible placeholder for an otherwise
        // empty editable line (including an empty table cell).  Once typing
        // begins it must be replaced by the real text node, rather than left
        // behind as an extra line break after the inserted text.
        bool reindex=false;
        if(container->children.size()==1&&container->children.front()->tag==L"br"){
            const auto placeholder=container->children.front();
            reindex=!document.UnindexSubtree(placeholder);
            container->children.clear();placeholder->parent.reset();offset=0;
        }
        if(offset<container->children.size()&&
           container->children[offset]->type==NodeType::Text)
            return {container->children[offset],0};
        if(offset>0&&container->children[offset-1]->type==NodeType::Text){
            const auto text=container->children[offset-1];return {text,text->text.size()};
        }
        auto text=std::make_shared<Node>();text->type=NodeType::Text;text->tag=L"#text";
        text->ownerDocument=container->ownerDocument;
        text->parent=container;
        container->children.insert(container->children.begin()+static_cast<std::ptrdiff_t>(offset),text);
        if(reindex||!document.IndexSubtree(text))document.Reindex();
        layoutDirty=true;return {text,0};
    }
    bool MaterializeEditingBoundary(){
        if(!editingBoundaryContainer)return editingNode!=nullptr;
        const auto text=EnsureEditableBoundaryText(editingBoundaryContainer,editingBoundaryOffset);
        if(!text.first)return false;
        editingNode=text.first;selectionAnchor=caretPosition=text.second;
        ClearEditingBoundary();return true;
    }
    bool ParticipatesInFocusedEditing(const std::shared_ptr<Node>& node)const{
        if(!node||!focused||!IsContentEditable(focused))return false;
        if(node==focused)return true;
        if(node->attributes.count(L"contenteditable")&&!IsContentEditable(node))return false;
        return EditableRoot(node)==focused;
    }
    std::shared_ptr<Node> FirstEditingLeaf(const std::shared_ptr<Node>& node)const{
        if(!ParticipatesInFocusedEditing(node))return {};
        if(node->type==NodeType::Text||IsAtomicEditingElement(node))return node;
        for(const auto& child:node->children)
            if(auto leaf=FirstEditingLeaf(child))return leaf;
        return {};
    }
    std::shared_ptr<Node> LastEditingLeaf(const std::shared_ptr<Node>& node)const{
        if(!ParticipatesInFocusedEditing(node))return {};
        if(node->type==NodeType::Text||IsAtomicEditingElement(node))return node;
        for(auto child=node->children.rbegin();child!=node->children.rend();++child)
            if(auto leaf=LastEditingLeaf(*child))return leaf;
        return {};
    }
    std::shared_ptr<Node> AdjacentEditingLeaf(const std::shared_ptr<Node>& node,
                                              bool backward)const{
        for(auto current=node;current&&current!=focused;){
            const auto parent=current->parent.lock();if(!parent)return {};
            const auto position=std::find(parent->children.begin(),parent->children.end(),current);
            if(position==parent->children.end())return {};
            if(backward){
                for(auto item=position;item!=parent->children.begin();){
                    --item;if(auto leaf=LastEditingLeaf(*item))return leaf;
                }
            }else{
                for(auto item=position+1;item!=parent->children.end();++item)
                    if(auto leaf=FirstEditingLeaf(*item))return leaf;
            }
            current=parent;
        }
        return {};
    }
    std::shared_ptr<Node> EditingLeafAtBoundary(bool backward)const{
        if(!editingBoundaryContainer)return {};
        const auto& children=editingBoundaryContainer->children;
        const auto offset=std::min(editingBoundaryOffset,children.size());
        if(backward){
            for(size_t index=offset;index>0;--index)
                if(auto leaf=LastEditingLeaf(children[index-1]))return leaf;
        }else{
            for(size_t index=offset;index<children.size();++index)
                if(auto leaf=FirstEditingLeaf(children[index]))return leaf;
        }
        return AdjacentEditingLeaf(editingBoundaryContainer,backward);
    }
    bool SetEditingTextPosition(const std::shared_ptr<Node>& text,size_t offset){
        if(!text||text->type!=NodeType::Text||EditableRoot(text)!=focused)return false;
        ClearEditingBoundary();editingNode=text;
        selectionAnchor=caretPosition=std::min(offset,text->text.size());verticalCaretXValid=false;
        ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return true;
    }
    bool SetEditingBoundaryPosition(const std::shared_ptr<Node>& container,size_t offset){
        const auto boundary=EditableBoundary(container,offset);if(!boundary.first)return false;
        editingNode.reset();editingBoundaryContainer=boundary.first;
        editingBoundaryOffset=boundary.second;selectionAnchor=caretPosition=0;verticalCaretXValid=false;
        ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return true;
    }
    bool MoveAcrossEditingLeaf(bool backward){
        if(!focused||!IsContentEditable(focused))return false;
        auto leaf=editingBoundaryContainer?EditingLeafAtBoundary(backward):
            AdjacentEditingLeaf(editingNode,backward);
        std::shared_ptr<Node> atomic;while(leaf){
            if(leaf->type==NodeType::Text)
                return SetEditingTextPosition(leaf,backward?leaf->text.size():0);
            if(IsAtomicEditingElement(leaf)&&!atomic)atomic=leaf;
            leaf=AdjacentEditingLeaf(leaf,backward);
        }
        if(!atomic)return false;
        const auto parent=atomic->parent.lock();if(!parent)return false;
        const auto position=std::find(parent->children.begin(),parent->children.end(),atomic);
        if(position==parent->children.end())return false;
        const auto index=static_cast<size_t>(position-parent->children.begin());
        return SetEditingBoundaryPosition(parent,index+(backward?0:1));
    }
    const LayoutBox* EditingLayoutBox()const{
        return editingNode?layout.BoxFor(editingNode):nullptr;
    }
    const LayoutBox* EditingStyleBox()const{
        auto source=editingNode?editingNode:editingBoundaryContainer;
        if(!source)return nullptr;
        for(auto current=source;current;current=current->parent.lock())
            if(const auto* box=layout.BoxFor(current))return box;
        return nullptr;
    }
    void BeginEditingAt(const std::shared_ptr<Node>& target,float x,float y){
        ClearEditingBoundary();verticalCaretXValid=false;
        if(layoutDirty)Rebuild();std::shared_ptr<Node> hitNode;size_t hitOffset=0;
        auto scope=IsTextControl(focused)?focused:
            (target&&EditableRoot(target)==focused?target:focused);
        bool hit=layout.HitTestText(scope,x,y,hitNode,hitOffset);
        // Prefer the clicked editable subtree over unrelated nearby text.  An
        // empty block or table cell has no text layout to hit-test, but its DOM
        // boundary is still a valid caret position and must remain observable
        // through Selection.anchorNode for generic editor scripts.
        if(!hit&&!IsTextControl(focused)&&scope&&scope!=focused&&
           SetEditingBoundaryPosition(scope,0))return;
        if(!hit&&scope!=focused)hit=layout.HitTestText(focused,x,y,hitNode,hitOffset);
        if(hit&&(hitNode==focused||EditableRoot(hitNode)==focused)){
            editingNode=std::move(hitNode);selectionAnchor=caretPosition=hitOffset;
        }else{
            editingNode=EnsureEditableTextNode(target);if(!editingNode)return;
            selectionAnchor=caretPosition=EditingValue().size();
        }
        StoreControlSelection();
        textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();
    }
    bool UpdateTextSelectionAt(float x,float y){
        if(!textSelectionDragging||!editingNode||!focused)return false;
        if(layoutDirty)Rebuild();
        std::shared_ptr<Node> hitNode;size_t hitOffset=0;
        const auto scope=IsTextControl(focused)?focused:editingNode;
        if(!layout.HitTestText(scope,x,y,hitNode,hitOffset)||hitNode!=editingNode)return false;
        const auto next=std::min(hitOffset,EditingValue().size());
        if(caretPosition==next)return true;
        caretPosition=next;verticalCaretXValid=false;StoreControlSelection();
        textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();return true;
    }
    bool CanBlinkCaret()const{
        return hwnd&&(editingNode||editingBoundaryContainer)&&focused&&GetFocus()==hwnd&&
            (selectionAnchor==caretPosition||compositionActive);
    }
    void StopCaretBlink(){
        if(caretBlinkTimerActive){KillTimer(hwnd,kCaretBlinkTimer);caretBlinkTimerActive=false;}
        const bool changed=!caretVisible;caretVisible=true;
        if(changed&&hwnd)InvalidateView();
    }
    void ResetCaretBlink(){
        if(caretBlinkTimerActive){KillTimer(hwnd,kCaretBlinkTimer);caretBlinkTimerActive=false;}
        caretVisible=true;
        if(CanBlinkCaret()){
            UINT interval=GetCaretBlinkTime();
            if(interval==0)interval=530;
            if(interval!=INFINITE)
                caretBlinkTimerActive=SetTimer(hwnd,kCaretBlinkTimer,
                    std::max(interval,static_cast<UINT>(USER_TIMER_MINIMUM)),nullptr)!=0;
        }
        if(hwnd)InvalidateView();
    }
    bool CaretDipRect(D2D1_RECT_F& result){
        if(!editingNode&&!editingBoundaryContainer)return false;LayoutRect caret{};
        if(!layout.TextCaretRect(editingNode?editingNode:editingBoundaryContainer,
            editingNode?caretPosition:editingBoundaryOffset,caret))return false;
        result=D2D1::RectF(caret.x,caret.y,caret.x+caret.width,caret.y+caret.height);return true;
    }
    RECT CaretClientRect(){
        if(layoutDirty)Rebuild();D2D1_RECT_F caret{};if(!CaretDipRect(caret))return RECT{};
        return RECT{DipToPixel(caret.left),DipToPixel(caret.top),
                    DipToPixel(caret.right),DipToPixel(caret.bottom)};
    }
    void InvalidateCaret(){
        if(!hwnd)return;
        RECT caret=CaretClientRect();
        if(IsRectEmpty(&caret)){InvalidateView();return;}
        InflateRect(&caret,2,2);InvalidateView(&caret);
    }
    void PaintTextEditing(ID2D1RenderTarget* target){
        if(!target||(!editingNode&&!editingBoundaryContainer)||GetFocus()!=hwnd)return;
        const auto* styleBox=EditingStyleBox();if(!styleBox||!styleBox->visible)return;
        const size_t valueLength=EditingValue().size();
        const size_t begin=std::min({selectionAnchor,caretPosition,valueLength});
        const size_t end=std::min(valueLength,std::max(selectionAnchor,caretPosition));
        ComPtr<ID2D1SolidColorBrush> brush;
        std::vector<LayoutRect> rangeRects;
        if(editingNode&&end>begin&&layout.TextRangeRects(editingNode,begin,end-begin,rangeRects)){
            target->CreateSolidColorBrush(D2D1::ColorF(0x3b82f6,0.36f),&brush);
            for(const auto& rect:rangeRects)
                target->FillRectangle(D2D1::RectF(rect.x,rect.y,rect.x+rect.width,
                                                  rect.y+rect.height),brush.Get());
        }
        rangeRects.clear();
        if(editingNode&&compositionActive&&!compositionText.empty()&&
           layout.TextRangeRects(editingNode,compositionReplaceStart,
                                 compositionText.size(),rangeRects)){
            target->CreateSolidColorBrush(D2D1::ColorF(0xff2563eb),&brush);
            for(const auto& rect:rangeRects)
                target->DrawLine(D2D1::Point2F(rect.x,rect.y+rect.height-1.0f/DpiScale()),
                    D2D1::Point2F(rect.x+rect.width,rect.y+rect.height-1.0f/DpiScale()),
                    brush.Get(),1.0f/DpiScale());
        }
        if((selectionAnchor==caretPosition||compositionActive)&&caretVisible){D2D1_RECT_F caret{};if(CaretDipRect(caret)){
            const auto color=StyleSheet::Color(styleBox->style.Get(L"caret-color",styleBox->style.Get(L"color",L"#000")),0xff000000);
            target->CreateSolidColorBrush(D2D1::ColorF(color&0x00ffffff),&brush);target->FillRectangle(caret,brush.Get());
        }}
    }
    void RefreshTextSelectionFromDom(){
        if(!editingNode||compositionActive)return;const auto length=EditingValue().size();
        selectionAnchor=std::min(selectionAnchor,length);caretPosition=std::min(caretPosition,length);
        StoreControlSelection();
        textInput.UpdateCandidateWindow(hwnd);
    }
    void BeginTextComposition(){
        if(!CanEditText()||compositionActive)return;compositionActive=true;compositionBase=EditingValue();
        compositionReplaceStart=std::min(selectionAnchor,caretPosition);
        compositionReplaceEnd=std::min(compositionBase.size(),std::max(selectionAnchor,caretPosition));
        compositionText.clear();compositionAttributes.clear();compositionCursor=0;
        JavaScriptRuntime::EventInit event{};event.isComposing=true;
        javascript.DispatchNodeEvent(focused,L"compositionstart",event);
    }
    void UpdateTextComposition(const std::wstring& text,const std::vector<unsigned char>& attributes,
                               size_t cursor){
        if(!compositionActive)BeginTextComposition();if(!compositionActive)return;
        auto display=compositionBase;display.replace(compositionReplaceStart,
            compositionReplaceEnd-compositionReplaceStart,text);SetEditingValue(display);
        compositionText=text;compositionAttributes=attributes;compositionCursor=std::min(cursor,text.size());
        selectionAnchor=caretPosition=compositionReplaceStart+compositionCursor;
        verticalCaretXValid=false;
        StoreControlSelection();
        JavaScriptRuntime::EventInit event{};event.data=text;event.isComposing=true;
        javascript.DispatchNodeEvent(focused,L"compositionupdate",event);
        layoutDirty=true;ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);
    }
    void CommitTextComposition(const std::wstring& text){
        if(!compositionActive){ReplaceSelection(text,L"insertCompositionText",true);return;}
        SetEditingValue(compositionBase);selectionAnchor=compositionReplaceStart;caretPosition=compositionReplaceEnd;
        StoreControlSelection();
        compositionActive=false;compositionText.clear();compositionAttributes.clear();
        JavaScriptRuntime::EventInit ended{};ended.data=text;
        javascript.DispatchNodeEvent(focused,L"compositionend",ended);
        ReplaceSelection(text,L"insertCompositionText",false);
    }
    void CancelTextComposition(){
        if(!compositionActive)return;SetEditingValue(compositionBase);
        selectionAnchor=compositionReplaceStart;caretPosition=compositionReplaceEnd;compositionActive=false;
        StoreControlSelection();
        compositionText.clear();compositionAttributes.clear();
        javascript.DispatchNodeEvent(focused,L"compositionend");layoutDirty=true;
        InvalidateView();
    }
    void CommitTextEdit(){if(IsTextControl(focused)&&textEditDirty){javascript.DispatchNodeEvent(focused,L"change");textEditDirty=false;}}
    bool SetHoveredNode(const std::shared_ptr<Node>& next,LayoutRect* dirtyBounds=nullptr,
                        bool* dirtyBoundsValid=nullptr){
        if(dirtyBoundsValid)*dirtyBoundsValid=false;
        if(next==hovered)return false;
        std::vector<std::shared_ptr<Node>> nextPath;
        for(auto current=next;current;current=current->parent.lock())nextPath.push_back(current);
        size_t previousUnique=hoverPath.size(),nextUnique=nextPath.size();
        while(previousUnique&&nextUnique&&hoverPath[previousUnique-1]==nextPath[nextUnique-1]){
            --previousUnique;--nextUnique;
        }
        std::vector<std::shared_ptr<Node>> affected;
        auto collectAffected=[&](const std::vector<std::shared_ptr<Node>>& path,size_t count){
            std::shared_ptr<Node> outermost;
            for(size_t index=0;index<count;++index)
                if(styles.HoverStateAffects(path[index]))outermost=path[index];
            if(outermost&&std::find(affected.begin(),affected.end(),outermost)==affected.end())
                affected.push_back(std::move(outermost));
        };
        collectAffected(hoverPath,previousUnique);
        collectAffected(nextPath,nextUnique);
        for(size_t index=0;index<previousUnique;++index)hoverPath[index]->hovered=false;
        for(size_t index=0;index<nextUnique;++index)nextPath[index]->hovered=true;

        hovered=next;hoverPath=std::move(nextPath);
        if(affected.empty())return false;
        if(styles.HoverRequiresBroadInvalidation()){
            const auto* root=layout.Root();
            return root&&root->node?layout.Restyle(root->node):false;
        }
        LayoutRect dirty{};bool hasDirty=false,safe=true;
        if(dirtyBounds)for(const auto& node:affected){
            LayoutRect visual{};if(!layout.VisualBounds(node,visual)){safe=false;break;}
            IncludeBounds(dirty,hasDirty,visual);
        }
        bool layoutChanged=false;
        for(const auto& node:affected)
            if(layout.BoxFor(node))layoutChanged=layout.Restyle(node)||layoutChanged;
        if(dirtyBounds&&safe&&!layoutChanged)for(const auto& node:affected){
            LayoutRect visual{};if(!layout.VisualBounds(node,visual)){safe=false;break;}
            IncludeBounds(dirty,hasDirty,visual);
        }
        if(dirtyBounds&&dirtyBoundsValid&&safe&&hasDirty&&!layoutChanged){
            *dirtyBounds=dirty;*dirtyBoundsValid=true;
        }
        return layoutChanged;
    }
    JavaScriptRuntime::EventInit PointerEventAt(WPARAM keyState,float x,float y,
                                                 int button=-1,int detail=0)const{
        JavaScriptRuntime::EventInit event{};
        event.clientX=x;event.clientY=y;event.pageX=x;event.pageY=y;
        POINT screenPoint{DipToPixel(x),DipToPixel(y)};ClientToScreen(hwnd,&screenPoint);
        const float scale=DpiScale();
        event.screenX=static_cast<double>(screenPoint.x)/scale;
        event.screenY=static_cast<double>(screenPoint.y)/scale;
        event.button=button;event.detail=detail;
        if(keyState&MK_LBUTTON)event.buttons|=1;
        if(keyState&MK_RBUTTON)event.buttons|=2;
        if(keyState&MK_MBUTTON)event.buttons|=4;
        if(keyState&MK_XBUTTON1)event.buttons|=8;
        if(keyState&MK_XBUTTON2)event.buttons|=16;
        event.ctrlKey=(keyState&MK_CONTROL)!=0;
        event.shiftKey=(keyState&MK_SHIFT)!=0;
        event.altKey=(GetKeyState(VK_MENU)&0x8000)!=0;
        event.metaKey=(GetKeyState(VK_LWIN)&0x8000)!=0||
                      (GetKeyState(VK_RWIN)&0x8000)!=0;
        return event;
    }
    static std::vector<std::shared_ptr<Node>> PointerPath(const std::shared_ptr<Node>& node){
        std::vector<std::shared_ptr<Node>> path;
        for(auto current=node;current;current=current->parent.lock())
            if(current->type!=NodeType::Document)path.push_back(current);
        return path;
    }
    void DispatchPointerTransition(const std::shared_ptr<Node>& previous,
                                   const std::shared_ptr<Node>& next,
                                   const JavaScriptRuntime::EventInit& base){
        if(previous==next)return;
        const auto previousPath=PointerPath(previous),nextPath=PointerPath(next);
        size_t previousUnique=previousPath.size(),nextUnique=nextPath.size();
        while(previousUnique&&nextUnique&&
              previousPath[previousUnique-1]==nextPath[nextUnique-1]){
            --previousUnique;--nextUnique;
        }
        if(previous){
            auto out=base;out.relatedTarget=next;
            javascript.DispatchNodeEvent(previous,L"pointerout",out);
            out.bubbles=false;out.cancelable=false;
            for(size_t index=0;index<previousUnique;++index)
                javascript.DispatchNodeEvent(previousPath[index],L"pointerleave",out);
        }
        if(next){
            auto over=base;over.relatedTarget=previous;
            javascript.DispatchNodeEvent(next,L"pointerover",over);
            over.bubbles=false;over.cancelable=false;
            for(size_t index=nextUnique;index>0;--index)
                javascript.DispatchNodeEvent(nextPath[index-1],L"pointerenter",over);
        }
    }
    void SetFocusedNode(const std::shared_ptr<Node>& node,bool focusVisible=false,
                        bool openTextEditor=false){
        const auto candidate=FocusTarget(node);const auto next=IsFocusable(candidate)?candidate:std::shared_ptr<Node>{};
        if(openSelectPopup&&next!=openSelectPopup)CloseSelectPopup();
        if(next==focused){
            const bool desired=focused&&(focusVisible||IsTextControl(focused)||IsContentEditable(focused));
            if(focused&&desired!=focused->focusVisible){focused->focusVisible=desired;RestyleDocument();if(accessibility)accessibility->Invalidate();InvalidateView();}
            if(focused&&openTextEditor&&!editingNode&&!editingBoundaryContainer&&
               (IsTextControl(focused)||IsContentEditable(focused)))
                editingNode=EnsureEditableTextNode(focused);
            if(editingNode||editingBoundaryContainer)ResetCaretBlink();
            return;
        }
        textInput.Cancel(hwnd);StoreControlSelection();editingNode.reset();ClearEditingBoundary();StopCaretBlink();
        if(focused){for(auto current=focused;current;current=current->parent.lock())current->focusWithin=false;CommitTextEdit();focused->focused=false;focused->focusVisible=false;
            javascript.DispatchNodeEvent(focused,L"blur");javascript.DispatchNodeEvent(focused,L"focusout");}
        focused=next;textEditDirty=false;selectionAnchor=caretPosition=0;verticalCaretXValid=false;
        undoHistory.clear();redoHistory.clear();
        if(focused){focused->focused=true;focused->focusVisible=focusVisible||IsTextControl(focused)||IsContentEditable(focused);for(auto current=focused;current;current=current->parent.lock())current->focusWithin=true;
            if(IsTextControl(focused)){editingNode=focused;const auto length=EditingValue().size();const auto start=std::min(focused->selectionStart,length),end=std::min(focused->selectionEnd,length);if(focused->selectionDirection==L"backward"){selectionAnchor=end;caretPosition=start;}else{selectionAnchor=start;caretPosition=end;}}
            else if(IsContentEditable(focused)){editingNode=EnsureEditableTextNode(focused);selectionAnchor=caretPosition=EditingValue().size();}
            javascript.DispatchNodeEvent(focused,L"focus");javascript.DispatchNodeEvent(focused,L"focusin");}
        RestyleDocument();if(accessibility)accessibility->Invalidate();InvalidateView();
        if(focused&&accessibility)accessibility->RaiseFocusChanged(focused);
        if(focused&&openTextEditor&&(IsTextControl(focused)||IsContentEditable(focused)))
            textInput.UpdateCandidateWindow(hwnd);
        ResetCaretBlink();
    }
    std::vector<std::shared_ptr<Node>> SequentialFocusOrder(){
        if(layoutDirty)Rebuild();
        struct Candidate { std::shared_ptr<Node> node; int tabIndex=0; size_t order=0; };
        std::vector<Candidate> candidates;size_t order=0;
        std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& parent){
            if(!parent)return;
            if(parent->type==NodeType::Element){
                const int tabIndex=SequentialTabIndex(parent);const auto* box=layout.BoxFor(parent);
                if(tabIndex>=0&&box&&box->visible&&box->rect.width>0&&box->rect.height>0)
                    candidates.push_back({parent,tabIndex,order});
                ++order;
            }
            for(const auto& child:parent->children)visit(child);
        };
        visit(document.Root());
        std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate& left,const Candidate& right){
            const bool leftPositive=left.tabIndex>0,rightPositive=right.tabIndex>0;
            if(leftPositive!=rightPositive)return leftPositive;
            if(leftPositive&&left.tabIndex!=right.tabIndex)return left.tabIndex<right.tabIndex;
            return left.order<right.order;
        });
        std::vector<std::shared_ptr<Node>> result;result.reserve(candidates.size());
        for(auto& candidate:candidates)result.push_back(std::move(candidate.node));
        return result;
    }
    bool MoveSequentialFocus(bool reverse){
        auto order=SequentialFocusOrder();if(order.empty())return false;
        auto current=std::find(order.begin(),order.end(),focused);size_t index=0;
        if(current==order.end())index=reverse?order.size()-1:0;
        else{
            const size_t old=static_cast<size_t>(std::distance(order.begin(),current));
            index=reverse?(old==0?order.size()-1:old-1):(old+1)%order.size();
        }
        SetFocus(hwnd);SetFocusedNode(order[index],true,true);return true;
    }
    bool MoveMenuFocus(WPARAM key){
        if(!focused)return false;
        auto associatedMenu=[this](const std::shared_ptr<Node>& trigger){
            const auto controls=trigger->Attribute(L"aria-controls");if(!controls.empty())if(auto menu=document.GetElementById(controls))return menu;
            const auto parent=trigger->parent.lock();if(parent)for(const auto& child:parent->children)
                if(ToLower(child->Attribute(L"role"))==L"menu")return child;
            return std::shared_ptr<Node>{};
        };
        auto menuTrigger=[&](const std::shared_ptr<Node>& menu){
            const auto labelled=menu?menu->Attribute(L"aria-labelledby"):L"";if(!labelled.empty())if(auto trigger=document.GetElementById(labelled))return trigger;
            const auto parent=menu?menu->parent.lock():std::shared_ptr<Node>{};if(parent)for(const auto& child:parent->children)
                if(child!=menu&&IsKeyboardActivatable(child)&&ToLower(child->Attribute(L"role"))!=L"menuitem")return child;
            return std::shared_ptr<Node>{};
        };
        auto itemsFor=[this](const std::shared_ptr<Node>& menu){
            std::vector<std::shared_ptr<Node>> items;
            std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& parent){
            for(const auto& child:parent->children){
                if(ToLower(child->Attribute(L"role"))==L"menuitem"&&IsFocusable(child)){
                    const auto* box=layout.BoxFor(child);if(box&&box->visible)items.push_back(child);
                }
                visit(child);
            }
            };
            if(menu)visit(menu);return items;
        };
        auto openAndFocus=[&](const std::shared_ptr<Node>& trigger,const std::shared_ptr<Node>& menu,bool last){
            if(!trigger||!menu)return false;if(layoutDirty)Rebuild();auto items=itemsFor(menu);
            if(items.empty()){
                const auto* box=layout.BoxFor(trigger);Activate(trigger,box?box->rect.x:0,box?box->rect.y:0,true);
                if(layoutDirty)Rebuild();items=itemsFor(menu);
            }
            if(items.empty())return false;SetFocusedNode(last?items.back():items.front(),true,false);return true;
        };
        const auto role=ToLower(focused->Attribute(L"role"));
        if(role!=L"menuitem"){
            const auto menu=associatedMenu(focused);
            if(menu&&(key==VK_DOWN||key==VK_UP||key==VK_HOME||key==VK_END))
                return openAndFocus(focused,menu,key==VK_UP||key==VK_END);
            return false;
        }
        auto menu=focused->parent.lock();while(menu&&ToLower(menu->Attribute(L"role"))!=L"menu"&&ToLower(menu->Attribute(L"role"))!=L"menubar")menu=menu->parent.lock();
        if(key==VK_ESCAPE){if(auto trigger=menuTrigger(menu)){SetFocusedNode(trigger,true,false);return true;}return false;}
        if(!menu)return false;if(layoutDirty)Rebuild();auto items=itemsFor(menu);if(items.empty())return false;
        if((key==VK_LEFT||key==VK_RIGHT)&&ToLower(menu->Attribute(L"role"))!=L"menubar"){
            const auto root=menu->parent.lock(),container=root?root->parent.lock():std::shared_ptr<Node>{};
            if(container){std::vector<std::pair<std::shared_ptr<Node>,std::shared_ptr<Node>>> roots;
                for(const auto& child:container->children)for(const auto& descendant:child->children)
                    if(ToLower(descendant->Attribute(L"role"))==L"menu"){if(auto trigger=menuTrigger(descendant))roots.push_back({child,trigger});break;}
                const auto found=std::find_if(roots.begin(),roots.end(),[&](const auto& item){return item.first==root;});
                if(found!=roots.end()&&roots.size()>1){size_t index=static_cast<size_t>(std::distance(roots.begin(),found));index=key==VK_LEFT?(index==0?roots.size()-1:index-1):(index+1)%roots.size();const auto trigger=roots[index].second;return openAndFocus(trigger,associatedMenu(trigger),false);}}
            return false;
        }
        auto current=std::find(items.begin(),items.end(),focused);
        size_t index=current==items.end()?0:static_cast<size_t>(std::distance(items.begin(),current));
        if(key==VK_HOME)index=0;else if(key==VK_END)index=items.size()-1;
        else if(key==VK_UP||key==VK_LEFT)index=index==0?items.size()-1:index-1;
        else if(key==VK_DOWN||key==VK_RIGHT)index=(index+1)%items.size();else return false;
        SetFocusedNode(items[index],true,false);return true;
    }
    std::shared_ptr<Node> DatalistForInput(const std::shared_ptr<Node>& input)const{
        if(!IsTextInput(input))return {};
        const auto id=input->Attribute(L"list");if(id.empty())return {};
        const auto datalist=document.GetElementById(id);
        return datalist&&datalist->tag==L"datalist"?datalist:std::shared_ptr<Node>{};
    }
    bool IsDatalistInput(const std::shared_ptr<Node>& input)const{
        return static_cast<bool>(DatalistForInput(input));
    }
    std::vector<std::shared_ptr<Node>> DatalistOptions(const std::shared_ptr<Node>& input,
                                                        bool showAll)const{
        std::vector<std::shared_ptr<Node>> result;const auto datalist=DatalistForInput(input);
        if(!datalist)return result;
        const auto query=ToLower(input->Attribute(L"value"));
        std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& parent){
            for(const auto& child:parent->children){
                if(child->tag!=L"option"){collect(child);continue;}
                const auto value=OptionValue(child),label=child->Attribute(L"label");
                if(showAll||query.empty()||ToLower(value).find(query)==0||
                   (!label.empty()&&ToLower(label).find(query)==0))result.push_back(child);
            }
        };
        collect(datalist);return result;
    }
    std::vector<std::shared_ptr<Node>> PopupOptions(const std::shared_ptr<Node>& control)const{
        return control&&control->tag==L"select"?SelectOptions(control):
            DatalistOptions(control,selectPopupShowAll);
    }
    int PopupSelectedIndex(const std::shared_ptr<Node>& control,
                           const std::vector<std::shared_ptr<Node>>& options)const{
        if(options.empty())return -1;
        if(control&&control->tag==L"select")return static_cast<int>(SelectedOptionIndex(control,options));
        const auto value=control?control->Attribute(L"value"):L"";
        for(size_t index=0;index<options.size();++index)
            if(OptionValue(options[index])==value)return static_cast<int>(index);
        return -1;
    }
    bool SelectOption(const std::shared_ptr<Node>& select,size_t index,bool dispatchEvents=true){
        const auto options=SelectOptions(select);if(!select||index>=options.size()||options[index]->disabled)return false;
        const size_t previous=SelectedOptionIndex(select,options);if(previous==index)return false;
        for(auto& option:options)option->RemoveAttribute(L"selected");
        options[index]->SetAttribute(L"selected",L"");
        select->SetAttribute(L"value",OptionValue(options[index]));
        if(dispatchEvents){javascript.DispatchNodeEvent(select,L"input");javascript.DispatchNodeEvent(select,L"change");}
        layoutDirty=true;InvalidateView();return true;
    }
    bool ChoosePopupOption(const std::shared_ptr<Node>& control,size_t index){
        const auto options=PopupOptions(control);if(!control||index>=options.size()||options[index]->disabled)return false;
        if(control->tag==L"select"){
            const auto all=SelectOptions(control);const auto found=std::find(all.begin(),all.end(),options[index]);
            return found!=all.end()&&SelectOption(control,static_cast<size_t>(std::distance(all.begin(),found)));
        }
        if(!IsDatalistInput(control))return false;
        const auto value=OptionValue(options[index]);const bool changed=value!=control->Attribute(L"value");
        control->SetAttribute(L"value",value);selectionAnchor=caretPosition=value.size();
        if(focused==control)editingNode=control;StoreControlSelection();RefreshTextSelectionFromDom();textEditDirty=false;
        if(changed){javascript.DispatchNodeEvent(control,L"input");javascript.DispatchNodeEvent(control,L"change");}
        layoutDirty=true;InvalidateView();return changed;
    }
    bool MoveSelectOption(const std::shared_ptr<Node>& select,int direction,bool toEdge=false){
        const auto options=SelectOptions(select);if(options.empty())return false;
        size_t index=SelectedOptionIndex(select,options);
        if(toEdge)index=direction<0?0:options.size()-1;
        else{
            const auto next=static_cast<long long>(index)+direction;
            index=static_cast<size_t>(std::max<long long>(0,std::min<long long>(
                static_cast<long long>(options.size()-1),next)));
        }
        while(index<options.size()&&options[index]->disabled){
            if(direction<0){if(index==0)return false;--index;}
            else{if(index+1>=options.size())return false;++index;}
        }
        return SelectOption(select,index);
    }
    struct SelectPopupGeometry {
        LayoutRect bounds;
        float rowHeight=0;
        float borderWidth=0;
        float textInset=0;
        float contentHeight=0;
        float viewportHeight=0;
        float scrollOffset=0;
        float scrollbarWidth=0;
    };
    bool GetSelectPopupGeometry(const std::shared_ptr<Node>& control,SelectPopupGeometry& geometry)const{
        if(!control)return false;const auto* box=layout.BoxFor(control);if(!box||!box->visible)return false;
        const auto options=PopupOptions(control);if(options.empty())return false;
        RECT client{};GetClientRect(hwnd,&client);const float scale=DpiScale();
        const float viewportWidth=std::max(1.0f,static_cast<float>(client.right-client.left)/scale);
        const float viewportHeight=std::max(1.0f,static_cast<float>(client.bottom-client.top)/scale);
        auto metric=[&](const wchar_t* property,float reference,float fallback){
            const auto raw=box->style.Get(property);return raw.empty()?fallback:
                StyleSheet::Length(raw,reference,viewportWidth,fallback);
        };
        const bool editable=IsDatalistInput(control);
        geometry.rowHeight=std::max(1.0f,metric(L"--select-menu-row-height",box->rect.height,
            editable?std::max(36.0f,box->rect.height*1.25f):box->rect.height*0.75f));
        geometry.borderWidth=std::max(0.0f,metric(L"--select-menu-border-width",box->rect.width,1.0f));
        geometry.textInset=std::max(0.0f,metric(L"--select-menu-text-inset",box->rect.width,box->rect.height*0.3125f));
        const float gap=std::max(0.0f,metric(L"--select-menu-gap",box->rect.height,box->rect.height*0.125f));
        const float width=std::max(1.0f,metric(L"--select-menu-width",box->rect.width,box->rect.width));
        float maxRows=editable?5.0f:8.0f;const auto rawMaxRows=box->style.Get(L"--select-menu-max-rows");
        float parsedMaxRows=0;
        if(TryParseFloat(rawMaxRows,parsedMaxRows))maxRows=std::max(1.0f,parsedMaxRows);
        geometry.contentHeight=geometry.rowHeight*static_cast<float>(options.size());
        geometry.viewportHeight=std::min(geometry.contentHeight,geometry.rowHeight*maxRows);
        float height=geometry.viewportHeight+geometry.borderWidth*2.0f;
        float left=std::max(0.0f,std::min(box->rect.x,viewportWidth-width));
        float top=box->rect.y+box->rect.height+gap;
        const float roomBelow=std::max(0.0f,viewportHeight-top),roomAbove=std::max(0.0f,box->rect.y-gap);
        if(height>roomBelow&&roomAbove>roomBelow){height=std::min(height,roomAbove);top=box->rect.y-gap-height;}
        else height=std::min(height,roomBelow);
        geometry.viewportHeight=std::max(1.0f,height-geometry.borderWidth*2.0f);
        geometry.scrollOffset=std::max(0.0f,std::min(selectPopupScrollOffset,
            std::max(0.0f,geometry.contentHeight-geometry.viewportHeight)));
        geometry.scrollbarWidth=geometry.contentHeight>geometry.viewportHeight+0.5f?10.0f:0.0f;
        geometry.bounds={left,top,std::min(width,viewportWidth),height};return height>geometry.borderWidth*2.0f;
    }
    int SelectPopupIndexAt(float x,float y)const{
        SelectPopupGeometry geometry;if(!GetSelectPopupGeometry(openSelectPopup,geometry)||!geometry.bounds.Contains(x,y))return -1;
        if(geometry.scrollbarWidth>0&&x>=geometry.bounds.x+geometry.bounds.width-geometry.borderWidth-geometry.scrollbarWidth)return -1;
        const float local=y-geometry.bounds.y-geometry.borderWidth;
        if(local<0||local>=geometry.viewportHeight)return -1;
        const int index=static_cast<int>(std::floor((local+geometry.scrollOffset)/geometry.rowHeight));
        return index>=0&&static_cast<size_t>(index)<PopupOptions(openSelectPopup).size()?index:-1;
    }
    void CloseSelectPopup(){
        if(!openSelectPopup)return;openSelectPopup.reset();selectPopupHotIndex=-1;selectPopupScrollOffset=0;
        selectPopupShowAll=false;selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;
        if(GetCapture()==hwnd)ReleaseCapture();InvalidateView();
    }
    void EnsureSelectPopupHotVisible(){
        if(selectPopupHotIndex<0)return;SelectPopupGeometry geometry;
        if(!GetSelectPopupGeometry(openSelectPopup,geometry))return;
        const float top=geometry.rowHeight*selectPopupHotIndex,bottom=top+geometry.rowHeight;
        if(top<geometry.scrollOffset)selectPopupScrollOffset=top;
        else if(bottom>geometry.scrollOffset+geometry.viewportHeight)
            selectPopupScrollOffset=bottom-geometry.viewportHeight;
    }
    bool MoveSelectPopupHot(int direction,bool toEdge=false){
        const auto options=PopupOptions(openSelectPopup);if(options.empty())return false;
        int index=selectPopupHotIndex;
        if(toEdge)index=direction<0?0:static_cast<int>(options.size()-1);
        else if(index<0)index=direction<0?static_cast<int>(options.size()-1):0;
        else index=std::max(0,std::min(static_cast<int>(options.size()-1),index+direction));
        while(index>=0&&static_cast<size_t>(index)<options.size()&&options[index]->disabled){
            index+=direction<0?-1:1;
        }
        if(index<0||static_cast<size_t>(index)>=options.size())return false;
        if(index!=selectPopupHotIndex){selectPopupHotIndex=index;EnsureSelectPopupHotVisible();InvalidateView();}return true;
    }
    bool ScrollSelectPopup(float amount){
        SelectPopupGeometry geometry;if(!GetSelectPopupGeometry(openSelectPopup,geometry))return false;
        const float maximum=std::max(0.0f,geometry.contentHeight-geometry.viewportHeight);
        const float previous=selectPopupScrollOffset;
        selectPopupScrollOffset=std::max(0.0f,std::min(maximum,previous+amount));
        if(std::abs(previous-selectPopupScrollOffset)<0.01f)return false;
        InvalidateView();return true;
    }
    void PaintSelectPopup(ID2D1RenderTarget* target){
        SelectPopupGeometry geometry;if(!target||!writeFactory||!GetSelectPopupGeometry(openSelectPopup,geometry)){openSelectPopup.reset();selectPopupHotIndex=-1;return;}
        const auto options=PopupOptions(openSelectPopup);if(options.empty())return;
        const auto* box=layout.BoxFor(openSelectPopup);if(!box)return;
        auto d2d=[](unsigned int value){return D2D1::ColorF(((value>>16)&255)/255.0f,((value>>8)&255)/255.0f,(value&255)/255.0f,((value>>24)&255)/255.0f);};
        // A native select popup belongs to the control's rendered color scheme.
        // Use the computed control surface unless the author explicitly supplies
        // one of the popup customization properties.
        auto popupSurface=EffectiveBackgroundColor(box);
        if(IsDatalistInput(openSelectPopup)){
            auto scheme=ToLower(Trim(box->style.Get(L"color-scheme",L"light")));
            const auto separator=scheme.find_first_of(L" \t");if(separator!=std::wstring::npos)scheme.resize(separator);
            popupSurface=scheme==L"dark"?0xff202020u:0xffffffffu;
        }
        const auto palette=ResolveSelectPopupPalette(box->style,popupSurface);
        ComPtr<ID2D1SolidColorBrush> brush;
        const auto outer=D2D1::RectF(geometry.bounds.x,geometry.bounds.y,
            geometry.bounds.x+geometry.bounds.width,geometry.bounds.y+geometry.bounds.height);
        const auto rawShadowSize=box->style.Get(L"--select-menu-shadow-size");
        const float shadowSize=std::max(0.0f,rawShadowSize.empty()?8.0f:
            StyleSheet::Length(rawShadowSize,box->rect.height,geometry.bounds.width,8.0f));
        const auto rawShadowColor=box->style.Get(L"--select-menu-shadow-color");
        const unsigned int shadow=StyleSheet::Color(rawShadowColor,0x30000000);
        if(shadowSize>0&&((shadow>>24)&0xff)){
            const unsigned int alpha=(shadow>>24)&0xff;
            const int layers=std::max(1,static_cast<int>(std::ceil(shadowSize)));
            for(int layer=layers;layer>=1;--layer){
                const unsigned int layerAlpha=std::max(1u,alpha*static_cast<unsigned int>(layers-layer+1)/
                    static_cast<unsigned int>(layers*2));
                target->CreateSolidColorBrush(d2d((shadow&0x00ffffffu)|(layerAlpha<<24)),&brush);
                target->FillRectangle(D2D1::RectF(outer.left+1,outer.bottom,outer.right-1,
                    outer.bottom+static_cast<float>(layer)),brush.Get());
            }
        }
        target->CreateSolidColorBrush(d2d(palette.border),&brush);
        target->FillRectangle(outer,brush.Get());
        const auto inner=D2D1::RectF(outer.left+geometry.borderWidth,outer.top+geometry.borderWidth,
            outer.right-geometry.borderWidth,outer.bottom-geometry.borderWidth);
        target->CreateSolidColorBrush(d2d(palette.background),&brush);target->FillRectangle(inner,brush.Get());
        auto family=box->style.Get(L"font-family",L"Segoe UI");const auto comma=family.find(L',');if(comma!=std::wstring::npos)family=Trim(family.substr(0,comma));
        family.erase(std::remove(family.begin(),family.end(),L'\''),family.end());family.erase(std::remove(family.begin(),family.end(),L'"'),family.end());
        int weight=400;const auto rawWeight=box->style.Get(L"font-weight",L"400");size_t usedWeight=0;
        if(!TryParseInteger(rawWeight,weight,&usedWeight)||usedWeight!=rawWeight.size())
            weight=box->style.Is(L"font-weight",L"bold")?700:400;
        const float fontSize=StyleSheet::Length(box->style.Get(L"font-size",L"16px"),16,16,16);
        ComPtr<IDWriteTextFormat> format;
        if(FAILED(writeFactory->CreateTextFormat(family.c_str(),nullptr,static_cast<DWRITE_FONT_WEIGHT>(std::max(1,std::min(999,weight))),
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,fontSize,L"ko-kr",&format)))return;
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextFormat> secondaryFormat;
        if(IsDatalistInput(openSelectPopup)&&SUCCEEDED(writeFactory->CreateTextFormat(family.c_str(),nullptr,DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,std::max(8.0f,fontSize*0.82f),L"ko-kr",&secondaryFormat))){
            secondaryFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            secondaryFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        const int selected=PopupSelectedIndex(openSelectPopup,options);
        const float contentRight=inner.right-geometry.scrollbarWidth;
        target->PushAxisAlignedClip(D2D1::RectF(inner.left,inner.top,contentRight,inner.bottom),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for(size_t index=0;index<options.size();++index){
            const bool highlighted=static_cast<int>(index)==selectPopupHotIndex||
                (selectPopupHotIndex<0&&static_cast<int>(index)==selected&&openSelectPopup->tag==L"select");
            const float top=inner.top+geometry.rowHeight*static_cast<float>(index)-geometry.scrollOffset;
            const auto row=D2D1::RectF(inner.left,top,contentRight,top+geometry.rowHeight);
            if(row.bottom<=inner.top||row.top>=inner.bottom)continue;
            if(highlighted){target->CreateSolidColorBrush(d2d(palette.selectedBackground),&brush);target->FillRectangle(row,brush.Get());}
            const auto foreground=options[index]->disabled?palette.disabledColor:(highlighted?palette.selectedColor:palette.color);
            target->CreateSolidColorBrush(d2d(foreground),&brush);
            std::wstring label,secondary;
            if(IsDatalistInput(openSelectPopup)){
                label=OptionValue(options[index]);secondary=options[index]->Attribute(L"label");
                if(secondary==label)secondary.clear();
            }else{label=options[index]->Attribute(L"label");if(label.empty())label=Trim(options[index]->InnerText());}
            const auto textRect=D2D1::RectF(row.left+geometry.textInset,row.top,
                row.right-geometry.textInset,secondary.empty()?row.bottom:row.top+geometry.rowHeight*0.62f);
            target->DrawTextW(label.c_str(),static_cast<UINT32>(label.size()),format.Get(),textRect,brush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,DWRITE_MEASURING_MODE_NATURAL);
            if(!secondary.empty()&&secondaryFormat){
                const auto secondaryRect=D2D1::RectF(row.left+geometry.textInset,row.top+geometry.rowHeight*0.48f,
                    row.right-geometry.textInset,row.bottom-2.0f);
                target->DrawTextW(secondary.c_str(),static_cast<UINT32>(secondary.size()),secondaryFormat.Get(),
                    secondaryRect,brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP,DWRITE_MEASURING_MODE_NATURAL);
            }
        }
        target->PopAxisAlignedClip();
        if(geometry.scrollbarWidth>0){
            const auto track=D2D1::RectF(contentRight,inner.top,inner.right,inner.bottom);
            target->CreateSolidColorBrush(d2d((palette.border&0x00ffffffu)|0x30000000u),&brush);target->FillRectangle(track,brush.Get());
            const float thumbHeight=std::max(20.0f,geometry.viewportHeight*geometry.viewportHeight/geometry.contentHeight);
            const float maximum=std::max(0.0f,geometry.contentHeight-geometry.viewportHeight);
            const float travel=std::max(0.0f,geometry.viewportHeight-thumbHeight);
            const float thumbTop=inner.top+(maximum>0?geometry.scrollOffset/maximum*travel:0);
            target->CreateSolidColorBrush(d2d((palette.color&0x00ffffffu)|0x66000000u),&brush);
            target->FillRectangle(D2D1::RectF(contentRight+2,thumbTop,inner.right-2,thumbTop+thumbHeight),brush.Get());
        }
    }
    void OpenSelectPopup(const std::shared_ptr<Node>& control,bool showAll=false,bool toggle=true){
        if(!control||control->disabled)return;
        if(openSelectPopup==control&&toggle){CloseSelectPopup();return;}
        openSelectPopup=control;selectPopupShowAll=showAll;selectPopupScrollOffset=0;
        selectPopupHotIndex=control->tag==L"select"?PopupSelectedIndex(control,PopupOptions(control)):-1;
        if(layoutDirty)Rebuild();SelectPopupGeometry geometry;if(!GetSelectPopupGeometry(control,geometry)){CloseSelectPopup();return;}
        EnsureSelectPopupHotVisible();
        InvalidateView();
    }
    void OpenFilePicker(const std::shared_ptr<Node>& input){
        if(!input||input->disabled||input->tag!=L"input"||
           ToLower(input->Attribute(L"type"))!=L"file"||!IsWindowVisible(hwnd))return;
        std::wstring patterns;
        const auto accept=input->Attribute(L"accept");size_t start=0;
        while(start<accept.size()){
            const auto comma=accept.find(L',',start);
            const auto extension=Trim(accept.substr(start,comma==std::wstring::npos?std::wstring::npos:comma-start));
            if(!extension.empty()&&extension.front()==L'.'){
                if(!patterns.empty())patterns+=L';';patterns+=L'*'+extension;
            }
            if(comma==std::wstring::npos)break;start=comma+1;
        }
        if(patterns.empty())patterns=L"*.*";
        std::wstring filter=L"Accepted files";filter+=L'\0';filter+=patterns;filter+=L'\0';
        filter+=L"All files";filter+=L'\0';filter+=L"*.*";filter+=L'\0';filter+=L'\0';
        std::vector<wchar_t> path(32768,L'\0');
        OPENFILENAMEW options{sizeof(options)};options.hwndOwner=GetAncestor(hwnd,GA_ROOT);
        options.lpstrFile=path.data();options.nMaxFile=static_cast<DWORD>(path.size());
        options.lpstrFilter=filter.c_str();options.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if(!GetOpenFileNameW(&options))return;
        Node::FileInfo info;
        if(!FileInfoForPath(path.data(),info))return;
        input->files={info};
        input->SetAttribute(L"value",L"C:\\fakepath\\"+info.name);
        javascript.DispatchNodeEvent(input,L"input");javascript.DispatchNodeEvent(input,L"change");
        layoutDirty=true;InvalidateView();
    }
    std::shared_ptr<Node> NearestForm(const std::shared_ptr<Node>& node)const{
        for(auto current=node;current;current=current->parent.lock())if(current->tag==L"form")return current;return {};
    }
    bool SubmitNearestForm(const std::shared_ptr<Node>& control){
        const auto form=NearestForm(control);if(!form)return false;
        if(javascript.DispatchNodeEvent(form,L"submit"))return true;
        const auto method=ToLower(form->Attribute(L"method"));
        if(method==L"dialog")if(const auto dialog=form->Closest(L"dialog");dialog&&dialog->attributes.count(L"open")){
            dialog->RemoveAttribute(L"open");dialog->modal=false;dialog->SetAttribute(L"returnvalue",control?control->Attribute(L"value"):L"");
            JavaScriptRuntime::EventInit close{};close.bubbles=false;close.cancelable=false;javascript.DispatchNodeEvent(dialog,L"close",close);
            layoutDirty=true;InvalidateView();
        }
        if(navigationHandler&&method!=L"dialog"){
            if(!method.empty()&&method!=L"get"){
                lastError=L"POST form submission is not supported";
                if(loadHandler)loadHandler(false,lastError);
                return true;
            }
            UINT codePage=CP_UTF8;
            for(const auto& meta:document.QuerySelectorAll(L"meta")){
                const auto declaration=ToLower(meta->Attribute(L"charset")+L" "+meta->Attribute(L"content"));
                if(declaration.find(L"euc-kr")!=std::wstring::npos||
                   declaration.find(L"windows-949")!=std::wstring::npos){codePage=949;break;}
            }
            std::wstring query;
            std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& node){
                if(!node||node->disabled)return;
                if(node!=form&&(node->tag==L"input"||node->tag==L"textarea"||
                                node->tag==L"select"||node->tag==L"button")){
                    const auto name=node->Attribute(L"name");
                    const auto type=ToLower(node->Attribute(L"type"));
                    bool include=!name.empty();
                    if(type==L"button"||type==L"reset"||type==L"file")include=false;
                    if(node->tag==L"button"&&node!=control)include=false;
                    if((type==L"submit"||type==L"image")&&node!=control)include=false;
                    if((type==L"checkbox"||type==L"radio")&&!node->checked)include=false;
                    if(include){
                        std::wstring value=node->Attribute(L"value");
                        if((type==L"checkbox"||type==L"radio")&&
                           !node->attributes.count(L"value"))value=L"on";
                        if(node->tag==L"textarea"&&!node->attributes.count(L"value"))
                            value=node->InnerText();
                        if(node->tag==L"select"&&!node->attributes.count(L"value")){
                            auto options=SelectOptions(node);
                            auto selected=std::find_if(options.begin(),options.end(),
                                [](const std::shared_ptr<Node>& option){return option->attributes.count(L"selected")>0;});
                            if(selected==options.end())selected=options.begin();
                            if(selected!=options.end())value=OptionValue(*selected);
                        }
                        if(!query.empty())query+=L"&";
                        query+=EncodeFormComponent(name,codePage)+L"="+
                               EncodeFormComponent(value,codePage);
                    }
                }
                for(const auto& child:node->children)collect(child);
            };
            collect(form);
            const auto action=form->Attribute(L"action");
            std::wstring target=action.empty()?basePath:ResolveResourceReference(action);
            const auto hash=target.find(L'#');if(hash!=std::wstring::npos)target.resize(hash);
            if(!query.empty())target+=(target.find(L'?')==std::wstring::npos?L"?":L"&")+query;
            pendingNavigation=target;pendingNavigationNewWindow=false;
            PostMessageW(hwnd,kNavigationMessage,0,0);
        }
        return true;
    }
    void PrepareActivation(const std::shared_ptr<Node>& node,float clickX,float clickY,bool keyboardFocusVisible=false){
        if(node&&(node->disabled||ToLower(node->Attribute(L"aria-disabled"))==L"true"))return;
        SetFocusedNode(node,keyboardFocusVisible,false);
        const auto focusTarget=FocusTarget(node);
        if(IsTextControl(focusTarget)||IsContentEditable(focusTarget)){
            BeginEditingAt(node,clickX,clickY);
            if(IsDatalistInput(focusTarget)){
                const auto* box=layout.BoxFor(focusTarget);
                const bool indicator=box&&clickX>=box->rect.x+box->rect.width-box->rect.height;
                OpenSelectPopup(focusTarget,indicator,false);
            }
        }else if(node&&node->tag==L"select")OpenSelectPopup(node);
    }
    void Activate(const std::shared_ptr<Node>& node,float clickX,float clickY,bool keyboardFocusVisible=false,
                  const JavaScriptRuntime::EventInit& click={},bool prepareInput=true){
        if(node&&(node->disabled||ToLower(node->Attribute(L"aria-disabled"))==L"true"))return;
        if(prepareInput)PrepareActivation(node,clickX,clickY,keyboardFocusVisible);
        if(!node){layoutDirty=true;InvalidateView();return;}
        const auto type=ToLower(node->Attribute(L"type"));const auto role=ToLower(node->Attribute(L"role"));bool changed=false;
        const bool oldChecked=node->checked,oldIndeterminate=node->indeterminate;
        const auto oldAriaChecked=node->Attribute(L"aria-checked");
        std::vector<std::pair<std::shared_ptr<Node>,bool>> oldRadioStates;
        if(node->tag==L"input"&&type==L"checkbox"&&!node->disabled){node->indeterminate=false;node->checked=!node->checked;changed=true;}
        else if(node->tag==L"input"&&type==L"radio"&&!node->disabled){auto name=node->Attribute(L"name");for(auto& other:document.GetElementsByName(name)){oldRadioStates.emplace_back(other,other->checked);other->checked=(other==node);}changed=true;}
        else if(role==L"checkbox"||role==L"switch"){const auto value=ToLower(node->Attribute(L"aria-checked"));node->SetAttribute(L"aria-checked",value==L"true"?L"false":L"true");changed=true;}
        else if(role==L"radio"){node->SetAttribute(L"aria-checked",L"true");changed=true;}
        const bool clickCanceled=javascript.DispatchNodeEvent(node,L"click",click);
        if(clickCanceled&&changed){
            node->checked=oldChecked;node->indeterminate=oldIndeterminate;
            for(const auto& state:oldRadioStates)state.first->checked=state.second;
            if(role==L"checkbox"||role==L"switch"||role==L"radio")node->SetAttribute(L"aria-checked",oldAriaChecked);
        }else if(changed){JavaScriptRuntime::EventInit change{};change.cancelable=false;javascript.DispatchNodeEvent(node,L"change",change);}
        if(!clickCanceled){
            const auto anchor=node->Closest(L"a[href]");
            if(anchor){const auto href=Trim(anchor->Attribute(L"href"));
                if(!href.empty()&&href.front()==L'#')javascript.NavigateToFragment(href);
                else if(!href.empty()&&href.rfind(L"javascript:",0)!=0){
                    pendingNavigation=ResolveResourceReference(href);
                    pendingNavigationNewWindow=ToLower(anchor->Attribute(L"target"))==L"_blank";
                    PostMessageW(hwnd,kNavigationMessage,0,0);
                }
            }
            auto submitter=node;if(node->tag!=L"button"&&node->tag!=L"input")submitter=node->Closest(L"button");
            if(submitter&&!submitter->disabled){const auto submitterType=ToLower(submitter->Attribute(L"type"));
                if((submitter->tag==L"button"&&(submitterType.empty()||submitterType==L"submit"))||
                   (submitter->tag==L"input"&&(submitterType==L"submit"||submitterType==L"image")))SubmitNearestForm(submitter);
            }
        }
        layoutDirty=true;
        if(!clickCanceled&&node->tag==L"input"&&type==L"file")OpenFilePicker(node);
        else if(!clickCanceled)if(auto label=node->Closest(L"label")){
            auto control=document.GetElementById(label->Attribute(L"for"));
            if(!control)control=document.QuerySelector(L"input",label);
            if(control&&control->tag==L"input"&&ToLower(control->Attribute(L"type"))==L"file"){
                SetFocusedNode(control,false,false);OpenFilePicker(control);
            }
        }
        InvalidateView();
    }
    void ReleasePrimaryPointer(WPARAM keyState,float x,float y){
        if(layoutDirty)Rebuild();
        const auto captured=javascript.CapturedPointerTarget();
        const auto target=captured?captured:layout.HitTest(x,y);
        const auto pressed=primaryPointerDownTarget.lock();primaryPointerDownTarget.reset();
        const int detail=primaryClickDetail;primaryClickDetail=0;
        auto pointer=PointerEventAt(keyState,x,y,0,detail);
        javascript.DispatchNodeEvent(target,L"pointerup",pointer);
        if(javascript.CapturedPointerTarget())javascript.ClearPointerCapture();
        if(GetCapture()==hwnd)ReleaseCapture();
        std::shared_ptr<Node> clickTarget;
        if(pressed){
            if(captured)clickTarget=target;
            else for(auto candidate=target;candidate&&!clickTarget;candidate=candidate->ComposedParent())
                for(auto down=pressed;down;down=down->ComposedParent())if(candidate==down){clickTarget=candidate;break;}
        }
        // A release does not activate a node removed by a pointerup listener.
        if(IsConnectedToDocument(clickTarget)){
            Activate(clickTarget,x,y,false,pointer,false);
            if(detail==2)javascript.DispatchNodeEvent(clickTarget,L"dblclick",pointer);
        }
    }
    void RecordUndo(){
        if(!editingNode)return;
        undoHistory.push_back({EditingValue(),selectionAnchor,caretPosition});
        if(undoHistory.size()>512)undoHistory.erase(undoHistory.begin());
        redoHistory.clear();
    }
    bool ApplyHistory(bool redo){
        auto& source=redo?redoHistory:undoHistory;
        auto& destination=redo?undoHistory:redoHistory;
        if(!CanEditText()||source.empty())return false;
        JavaScriptRuntime::EventInit before{};
        before.inputType=redo?L"historyRedo":L"historyUndo";
        if(javascript.DispatchNodeEvent(focused,L"beforeinput",before))return false;
        destination.push_back({EditingValue(),selectionAnchor,caretPosition});
        const auto snapshot=std::move(source.back());source.pop_back();
        SetEditingValue(snapshot.value);selectionAnchor=std::min(snapshot.anchor,snapshot.value.size());
        caretPosition=std::min(snapshot.caret,snapshot.value.size());verticalCaretXValid=false;
        StoreControlSelection();
        if(IsTextControl(focused))textEditDirty=true;
        javascript.DispatchNodeEvent(focused,L"input",before);layoutDirty=true;
        ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return true;
    }
    bool CopySelectionToClipboard(){
        if(!editingNode)return false;const auto value=EditingValue();
        const size_t begin=std::min({selectionAnchor,caretPosition,value.size()});
        const size_t end=std::min(value.size(),std::max(selectionAnchor,caretPosition));
        if(begin==end||!OpenClipboard(hwnd))return false;
        const auto selected=value.substr(begin,end-begin);
        const SIZE_T bytes=(selected.size()+1)*sizeof(wchar_t);
        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!memory){CloseClipboard();return false;}
        void* target=GlobalLock(memory);if(!target){GlobalFree(memory);CloseClipboard();return false;}
        std::memcpy(target,selected.c_str(),bytes);GlobalUnlock(memory);
        EmptyClipboard();
        if(!SetClipboardData(CF_UNICODETEXT,memory)){GlobalFree(memory);CloseClipboard();return false;}
        CloseClipboard();return true;
    }
    bool PasteFromClipboard(){
        if(!OpenClipboard(hwnd))return false;
        std::wstring value;bool hasText=false;std::vector<Node::FileInfo> files;
        if(HANDLE data=GetClipboardData(CF_UNICODETEXT))if(const auto* text=static_cast<const wchar_t*>(GlobalLock(data))){value=text;GlobalUnlock(data);hasText=true;}
        if(HANDLE data=GetClipboardData(CF_HDROP)){
            const auto drop=static_cast<HDROP>(data);const UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);
            for(UINT index=0;index<count;++index){const UINT length=DragQueryFileW(drop,index,nullptr,0);std::wstring path(length+1,L'\0');DragQueryFileW(drop,index,path.data(),length+1);path.resize(length);Node::FileInfo info;if(FileInfoForPath(path,info))files.push_back(std::move(info));}
        }
        CloseClipboard();if(!hasText&&files.empty())return false;
        if(javascript.DispatchClipboardEvent(focused,L"paste",value,files))return true;
        if(!CanEditText()||!hasText)return false;
        if(IsTextInput(focused)){
            value.erase(std::remove(value.begin(),value.end(),L'\r'),value.end());
            const auto lineBreak=value.find(L'\n');if(lineBreak!=std::wstring::npos)value.resize(lineBreak);
        }else{
            size_t position=0;while((position=value.find(L"\r\n",position))!=std::wstring::npos)value.replace(position,2,L"\n");
            std::replace(value.begin(),value.end(),L'\r',L'\n');
        }
        return ReplaceSelection(value,L"insertFromPaste");
    }
    bool ReplaceSelection(const std::wstring& replacement,const std::wstring& inputType=L"insertText",
                           bool isComposing=false){
        if(!CanEditText())return false;
        const bool atBoundary=editingBoundaryContainer!=nullptr;
        auto value=EditingValue();size_t begin=atBoundary?0:std::min(selectionAnchor,caretPosition);
        size_t end=atBoundary?0:std::min(value.size(),std::max(selectionAnchor,caretPosition));
        begin=std::min(begin,value.size());std::wstring inserted=replacement;
        if(IsTextControl(focused)){const auto maximum=focused->Attribute(L"maxlength");unsigned long long parsed=0;size_t used=0;
            if(TryParseUnsignedInteger(maximum,parsed,&used)&&used==maximum.size()&&
               parsed<=(std::numeric_limits<size_t>::max)()){
                const size_t limit=static_cast<size_t>(parsed),kept=value.size()-(end-begin);
                if(kept>=limit)inserted.clear();else if(inserted.size()>limit-kept)inserted.resize(limit-kept);
            }
        }
        if(begin==end&&inserted.empty())return false;
        JavaScriptRuntime::EventInit before{};before.data=inserted;before.inputType=inputType;before.isComposing=isComposing;
        if(javascript.DispatchNodeEvent(focused,L"beforeinput",before))return false;
        if(atBoundary){
            if(!MaterializeEditingBoundary())return false;
            value=EditingValue();begin=std::min(selectionAnchor,caretPosition);
            end=std::min(value.size(),std::max(selectionAnchor,caretPosition));
            begin=std::min(begin,value.size());
        }
        RecordUndo();
        value.replace(begin,end-begin,inserted);selectionAnchor=caretPosition=begin+inserted.size();SetEditingValue(value);
        verticalCaretXValid=false;
        StoreControlSelection();
        if(IsTextControl(focused))textEditDirty=true;
        JavaScriptRuntime::EventInit input=before;javascript.DispatchNodeEvent(focused,L"input",input);
        if(openSelectPopup==focused&&IsDatalistInput(focused)){
            selectPopupShowAll=false;selectPopupHotIndex=-1;selectPopupScrollOffset=0;
            if(PopupOptions(focused).empty())CloseSelectPopup();
        }
        layoutDirty=true;ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return true;
    }
    std::shared_ptr<Node> CloneEditingShell(const std::shared_ptr<Node>& source)const{
        if(!source||source->type!=NodeType::Element)return {};
        auto clone=std::make_shared<Node>();clone->type=source->type;clone->tag=source->tag;
        clone->attributes=source->attributes;clone->attributes.erase(L"id");
        clone->inlineStyle=source->inlineStyle;clone->ownerDocument=source->ownerDocument;
        return clone;
    }
    static void MoveEditingChildren(const std::shared_ptr<Node>& source,size_t offset,
                                    const std::shared_ptr<Node>& destination){
        if(!source||!destination)return;offset=std::min(offset,source->children.size());
        auto first=source->children.begin()+static_cast<std::ptrdiff_t>(offset);
        for(auto item=first;item!=source->children.end();++item)(*item)->parent=destination;
        destination->children.insert(destination->children.end(),first,source->children.end());
        source->children.erase(first,source->children.end());
    }
    bool CollapseEditingSelectionToBoundary(std::shared_ptr<Node>& container,size_t& offset){
        if(editingBoundaryContainer){
            container=editingBoundaryContainer;
            offset=std::min(editingBoundaryOffset,container->children.size());return true;
        }
        if(!editingNode||editingNode->type!=NodeType::Text)return false;
        const auto parent=editingNode->parent.lock();if(!parent)return false;
        const auto position=std::find(parent->children.begin(),parent->children.end(),editingNode);
        if(position==parent->children.end())return false;
        const size_t begin=std::min({selectionAnchor,caretPosition,editingNode->text.size()});
        const size_t end=std::min(editingNode->text.size(),
                                  std::max(selectionAnchor,caretPosition));
        if(end>begin)editingNode->text.erase(begin,end-begin);
        const size_t index=static_cast<size_t>(position-parent->children.begin());
        container=parent;
        if(begin==0)offset=index;
        else if(begin>=editingNode->text.size())offset=index+1;
        else{
            auto trailing=std::make_shared<Node>();trailing->type=NodeType::Text;
            trailing->tag=L"#text";trailing->text=editingNode->text.substr(begin);
            trailing->ownerDocument=editingNode->ownerDocument;trailing->parent=parent;
            editingNode->text.resize(begin);
            parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(index+1),
                                    trailing);
            offset=index+1;
        }
        return true;
    }
    std::shared_ptr<Node> NearestEditingBlock(const std::shared_ptr<Node>& container)const{
        for(auto current=container;current&&current!=focused;current=current->parent.lock()){
            const auto* box=layout.BoxFor(current);if(!box)continue;
            const auto display=ToLower(box->style.Get(L"display"));
            if(display==L"block"||display==L"flow-root"||display==L"list-item")
                return current;
        }
        return {};
    }
    static std::shared_ptr<Node> AppendEmptyEditingText(const std::shared_ptr<Node>& parent){
        if(!parent)return {};
        auto text=std::make_shared<Node>();text->type=NodeType::Text;text->tag=L"#text";
        text->ownerDocument=parent->ownerDocument;text->parent=parent;
        parent->children.push_back(text);return text;
    }
    bool InsertEditingParagraph(){
        if(!focused||!IsContentEditable(focused))return false;
        // HTML pre elements are line-oriented editing hosts.  Enter inserts a
        // preserved newline inside the current pre (including the common
        // <pre><code>...</code></pre> shape) instead of cloning the pre into a
        // second block.  This also keeps preview serializers from interpreting
        // one code fence as two adjacent code blocks.
        for(auto current=editingBoundaryContainer?editingBoundaryContainer:editingNode;
            current;current=current->parent.lock()){
            if(current->tag==L"pre")return ReplaceSelection(L"\n",L"insertParagraph");
            if(current==focused)break;
        }
        JavaScriptRuntime::EventInit before{};before.inputType=L"insertParagraph";
        if(javascript.DispatchNodeEvent(focused,L"beforeinput",before))return true;
        if(layoutDirty)Rebuild();
        std::shared_ptr<Node> container;size_t offset=0;
        if(!CollapseEditingSelectionToBoundary(container,offset))return false;
        auto block=NearestEditingBlock(container);
        if(!block)block=focused;
        auto trailing=CloneEditingShell(container);if(!trailing)return false;
        auto caretHost=trailing;MoveEditingChildren(container,offset,trailing);
        for(auto original=container;original!=block;){
            const auto parent=original->parent.lock();if(!parent)return false;
            const auto position=std::find(parent->children.begin(),parent->children.end(),original);
            if(position==parent->children.end())return false;
            auto parentTrailing=CloneEditingShell(parent);if(!parentTrailing)return false;
            trailing->parent=parentTrailing;parentTrailing->children.push_back(trailing);
            const auto siblingOffset=static_cast<size_t>(position-parent->children.begin())+1;
            MoveEditingChildren(parent,siblingOffset,parentTrailing);
            trailing=parentTrailing;original=parent;
        }
        std::shared_ptr<Node> newBlock;
        if(block==focused){
            newBlock=document.CreateElement(L"div");
            MoveEditingChildren(trailing,0,newBlock);
            if(caretHost==trailing)caretHost=newBlock;
            newBlock->parent=focused;focused->children.push_back(newBlock);
        }else{
            newBlock=trailing;
            const auto parent=block->parent.lock();if(!parent)return false;
            const auto position=std::find(parent->children.begin(),parent->children.end(),block);
            if(position==parent->children.end())return false;
            newBlock->parent=parent;
            parent->children.insert(position+1,newBlock);
        }
        if(caretHost->children.empty())AppendEmptyEditingText(caretHost);
        if(block!=focused&&block->children.empty())AppendEmptyEditingText(block);
        document.Reindex();layoutDirty=true;verticalCaretXValid=false;
        if(accessibility)accessibility->Invalidate();
        if(!SetEditingBoundaryPosition(caretHost,0))return false;
        javascript.DispatchNodeEvent(focused,L"input",before);
        layoutDirty=true;ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);return true;
    }
    std::shared_ptr<Node> AdjacentAtomicEditingNode(bool backward)const{
        if(!focused)return {};
        if(editingBoundaryContainer){
            const auto& children=editingBoundaryContainer->children;
            auto index=std::min(editingBoundaryOffset,children.size());
            if(backward){
                while(index>0){
                    const auto candidate=children[--index];
                    if(candidate->type==NodeType::Text&&candidate->text.empty())continue;
                    return IsAtomicEditingElement(candidate)?candidate:std::shared_ptr<Node>{};
                }
            }else{
                while(index<children.size()){
                    const auto candidate=children[index++];
                    if(candidate->type==NodeType::Text&&candidate->text.empty())continue;
                    return IsAtomicEditingElement(candidate)?candidate:std::shared_ptr<Node>{};
                }
            }
            return {};
        }
        if(!editingNode||editingNode==focused)return {};
        auto current=editingNode;
        while(current&&current!=focused){
            const auto parent=current->parent.lock();if(!parent)return {};
            const auto position=std::find(parent->children.begin(),parent->children.end(),current);
            if(position==parent->children.end())return {};
            if(backward){
                auto index=static_cast<size_t>(position-parent->children.begin());
                while(index>0){
                    const auto candidate=parent->children[--index];
                    if(candidate->type==NodeType::Text&&candidate->text.empty())continue;
                    return IsAtomicEditingElement(candidate)?candidate:std::shared_ptr<Node>{};
                }
            }else{
                auto index=static_cast<size_t>(position-parent->children.begin())+1;
                while(index<parent->children.size()){
                    const auto candidate=parent->children[index++];
                    if(candidate->type==NodeType::Text&&candidate->text.empty())continue;
                    return IsAtomicEditingElement(candidate)?candidate:std::shared_ptr<Node>{};
                }
            }
            current=parent;
        }
        return {};
    }
    bool DeleteAdjacentAtomic(bool backward,const std::wstring& inputType){
        if(IsTextControl(focused)||selectionAnchor!=caretPosition)return false;
        const auto target=AdjacentAtomicEditingNode(backward);if(!target)return false;
        JavaScriptRuntime::EventInit before{};before.inputType=inputType;
        if(javascript.DispatchNodeEvent(focused,L"beforeinput",before))return false;
        const auto parent=target->parent.lock();if(!parent)return false;
        const auto position=std::find(parent->children.begin(),parent->children.end(),target);
        if(position==parent->children.end())return false;
        const auto removedOffset=static_cast<size_t>(position-parent->children.begin());
        const bool indexOk=document.UnindexSubtree(target);
        parent->children.erase(position);target->parent.reset();
        if(editingBoundaryContainer==parent&&removedOffset<editingBoundaryOffset)
            --editingBoundaryOffset;
        if(!indexOk)document.Reindex();
        javascript.DispatchNodeEvent(focused,L"input",before);
        layoutDirty=true;verticalCaretXValid=false;if(accessibility)accessibility->Invalidate();
        ResetCaretBlink();textInput.UpdateCandidateWindow(hwnd);InvalidateView();
        return true;
    }
    bool Backspace(){
        if(!CanEditText())return false;
        if(editingBoundaryContainer)return DeleteAdjacentAtomic(true,L"deleteContentBackward");
        const auto value=EditingValue();if(selectionAnchor==caretPosition){if(caretPosition==0)return DeleteAdjacentAtomic(true,L"deleteContentBackward");selectionAnchor=PreviousTextPosition(value,std::min(caretPosition,value.size()));}return ReplaceSelection(L"",L"deleteContentBackward");
    }
    bool DeleteForward(){
        if(!CanEditText())return false;
        if(editingBoundaryContainer)return DeleteAdjacentAtomic(false,L"deleteContentForward");
        const auto value=EditingValue();if(selectionAnchor==caretPosition){if(caretPosition>=value.size())return DeleteAdjacentAtomic(false,L"deleteContentForward");selectionAnchor=NextTextPosition(value,caretPosition);}return ReplaceSelection(L"",L"deleteContentForward");
    }
    void MoveCaret(size_t position,bool extend){
        if(!editingNode)return;const auto length=EditingValue().size();caretPosition=std::min(position,length);if(!extend)selectionAnchor=caretPosition;verticalCaretXValid=false;StoreControlSelection();textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();
    }
    bool MoveHorizontalCaret(bool backward,bool extend){
        verticalCaretXValid=false;
        if(IsTextControl(focused)){
            const auto value=EditingValue();
            const auto position=caretPosition==selectionAnchor?
                (backward?PreviousTextPosition(value,caretPosition):
                          NextTextPosition(value,caretPosition)):
                (backward?std::min(caretPosition,selectionAnchor):
                          std::max(caretPosition,selectionAnchor));
            MoveCaret(position,extend);return true;
        }
        if(!focused||!IsContentEditable(focused))return false;
        if(editingBoundaryContainer)return !extend&&MoveAcrossEditingLeaf(backward);
        if(!editingNode||editingNode->type!=NodeType::Text)return false;
        const auto value=EditingValue();
        if(selectionAnchor!=caretPosition){
            MoveCaret(backward?std::min(caretPosition,selectionAnchor):
                               std::max(caretPosition,selectionAnchor),extend);
            return true;
        }
        const auto position=backward?PreviousTextPosition(value,caretPosition):
                                     NextTextPosition(value,caretPosition);
        if(position!=caretPosition){MoveCaret(position,extend);return true;}
        return !extend&&MoveAcrossEditingLeaf(backward);
    }
    bool MoveVerticalCaret(bool upward,bool extend){
        if(!focused||IsTextInput(focused)||(!editingNode&&!editingBoundaryContainer))return false;
        if(layoutDirty)Rebuild();
        const auto current=editingNode?editingNode:editingBoundaryContainer;
        const size_t currentOffset=editingNode?caretPosition:editingBoundaryOffset;
        LayoutRect caret{};if(!layout.TextCaretRect(current,currentOffset,caret))return false;
        const float preferred=verticalCaretXValid?verticalCaretX:caret.x;
        std::shared_ptr<Node> target;size_t targetOffset=0;
        if(!layout.VerticalCaretPosition(focused,current,currentOffset,preferred,upward,
                                        target,targetOffset))return false;
        bool moved=false;
        if(IsTextControl(focused)){
            if(target!=focused)return false;
            MoveCaret(targetOffset,extend);moved=true;
        }else if(target->type==NodeType::Text){
            if(extend){
                if(target!=editingNode)return false;
                MoveCaret(targetOffset,true);moved=true;
            }else moved=SetEditingTextPosition(target,targetOffset);
        }else if(!extend)moved=SetEditingBoundaryPosition(target,targetOffset);
        if(moved){verticalCaretX=preferred;verticalCaretXValid=true;}
        return moved;
    }
    HCURSOR CursorAtCurrentPosition(){
        POINT point{};GetCursorPos(&point);ScreenToClient(hwnd,&point);
        if(layoutDirty&&!RenderingScriptBusy())Rebuild();
        const auto node=layout.HitTest(PixelToDip(static_cast<float>(point.x)),PixelToDip(static_cast<float>(point.y)));
        for(const auto& frame:childFrames)if(frame.node==node&&frame.view)
            return frame.view->impl_->CursorAtCurrentPosition();
        std::wstring cursor;if(node)if(const auto* box=layout.BoxFor(node))cursor=ToLower(box->style.Get(L"cursor",L"auto"));
        if(cursor==L"ns-resize"||cursor==L"n-resize"||cursor==L"s-resize"||cursor==L"row-resize")return LoadCursorW(nullptr,IDC_SIZENS);
        if(cursor==L"ew-resize"||cursor==L"e-resize"||cursor==L"w-resize"||cursor==L"col-resize")return LoadCursorW(nullptr,IDC_SIZEWE);
        if(cursor==L"nwse-resize"||cursor==L"nw-resize"||cursor==L"se-resize")return LoadCursorW(nullptr,IDC_SIZENWSE);
        if(cursor==L"nesw-resize"||cursor==L"ne-resize"||cursor==L"sw-resize")return LoadCursorW(nullptr,IDC_SIZENESW);
        if(cursor==L"move"||cursor==L"all-scroll")return LoadCursorW(nullptr,IDC_SIZEALL);
        if(cursor==L"crosshair")return LoadCursorW(nullptr,IDC_CROSS);
        if(cursor==L"not-allowed"||cursor==L"no-drop")return LoadCursorW(nullptr,IDC_NO);
        if(cursor==L"wait")return LoadCursorW(nullptr,IDC_WAIT);
        if(cursor==L"progress")return LoadCursorW(nullptr,IDC_APPSTARTING);
        if(cursor==L"text")return LoadCursorW(nullptr,IDC_IBEAM);
        if(cursor==L"pointer")return LoadCursorW(nullptr,IDC_HAND);
        return LoadCursorW(nullptr,(IsTextControl(node)||EditableRoot(node))?IDC_IBEAM:
            (node&&(node->tag==L"button"||node->tag==L"select"||!node->Attribute(L"onclick").empty())?IDC_HAND:IDC_ARROW));
    }
    LRESULT ParentResizeHit(POINT screenPoint)const{
        const HWND parent=GetParent(hwnd);if(!parent)return HTCLIENT;
        return SendMessageW(parent,WM_NCHITTEST,0,
            MAKELPARAM(static_cast<WORD>(screenPoint.x),static_cast<WORD>(screenPoint.y)));
    }
    static HCURSOR ResizeCursor(LRESULT hit){
        if(hit==HTLEFT||hit==HTRIGHT)return LoadCursorW(nullptr,IDC_SIZEWE);
        if(hit==HTTOP||hit==HTBOTTOM)return LoadCursorW(nullptr,IDC_SIZENS);
        if(hit==HTTOPLEFT||hit==HTBOTTOMRIGHT)return LoadCursorW(nullptr,IDC_SIZENWSE);
        if(hit==HTTOPRIGHT||hit==HTBOTTOMLEFT)return LoadCursorW(nullptr,IDC_SIZENESW);
        return nullptr;
    }
    HWND FrameAtPoint(float x,float y)const{
        const auto hit=layout.HitTest(x,y);
        for(const auto& frame:childFrames)
            if(frame.node==hit&&frame.view)return frame.view->Window();
        return nullptr;
    }
    void LeavePointerFrame(){
        const HWND previous=pointerFrame;pointerFrame=nullptr;
        if(previous&&IsWindow(previous))SendMessageW(previous,WM_MOUSELEAVE,0,0);
    }
    bool HasPointerTooltip()const{
        if(tooltipOwner&&!tooltipText.empty())return true;
        for(const auto& frame:childFrames)
            if(frame.view&&frame.view->Window()==pointerFrame)return frame.view->impl_->HasPointerTooltip();
        return false;
    }
    bool ForwardFramePointer(UINT message,WPARAM wParam,LPARAM lParam){
        if(layoutDirty)Rebuild();
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        const bool screenCoordinates=message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL||
            message==WM_CONTEXTMENU;
        if(screenCoordinates){if(lParam==static_cast<LPARAM>(-1))return false;ScreenToClient(hwnd,&point);}
        const float x=PixelToDip(static_cast<float>(point.x)),y=PixelToDip(static_cast<float>(point.y));
        const HWND child=FrameAtPoint(x,y);
        if(message==WM_MOUSEMOVE&&pointerFrame!=child){LeavePointerFrame();pointerFrame=child;}
        if(!child||!IsWindow(child))return false;
        const auto found=std::find_if(childFrames.begin(),childFrames.end(),
            [&](const ChildFrame& frame){return frame.view&&frame.view->Window()==child;});
        if(found==childFrames.end())return false;
        const auto childView=found->view;
        if(message==WM_MOUSEHOVER&&!childView->impl_->HasPointerTooltip())return false;
        if(!screenCoordinates){MapWindowPoints(hwnd,child,&point,1);lParam=MAKELPARAM(point.x,point.y);}
        SendMessageW(child,message,wParam,lParam);
        if(message==WM_MOUSEMOVE&&childView->impl_->HasPointerTooltip()){
            // The physical pointer belongs to the outer compositing HWND.
            // Track its hover timer there, then route the advisory title to
            // the browsing context whose content is actually under it.
            TRACKMOUSEEVENT hover{sizeof(hover),TME_HOVER,hwnd,HOVER_DEFAULT};TrackMouseEvent(&hover);
        }
        return true;
    }
    bool HandleWheel(UINT message,WPARAM wParam,LPARAM lParam,bool descendIntoFrames=true){
        HideTooltip();if(layoutDirty)Rebuild();
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(hwnd,&point);
        const float x=PixelToDip(static_cast<float>(point.x)),y=PixelToDip(static_cast<float>(point.y));
        const bool horizontal=message==WM_MOUSEHWHEEL;
        const float delta=static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam));
        if(openSelectPopup){
            SelectPopupGeometry popup;
            if(!horizontal&&GetSelectPopupGeometry(openSelectPopup,popup)&&popup.bounds.Contains(x,y)){
                ScrollSelectPopup(-delta/WHEEL_DELTA*popup.rowHeight*3.0f);return true;
            }
            CloseSelectPopup();
        }
        if(descendIntoFrames){
            const auto hit=layout.HitTest(x,y);
            const auto found=std::find_if(childFrames.begin(),childFrames.end(),
                [&](const ChildFrame& frame){return frame.node==hit;});
            if(found!=childFrames.end()&&found->view){
                const auto child=found->view;
                if(child->impl_->HandleWheel(message,wParam,lParam))return true;
            }
        }
        std::shared_ptr<Node> scrolled;bool chainStopped=false;
        if(!layout.ScrollAt(x,y,horizontal?-delta:delta,&scrolled,horizontal,&chainStopped))
            return chainStopped;
        if(accessibility)accessibility->Invalidate();
        textInput.UpdateCandidateWindow(hwnd);InvalidateScrollViewport(scrolled);
        auto* surface=this;while(surface->compositionParent)surface=surface->compositionParent;
        UpdateWindow(surface->hwnd);
        javascript.DispatchNodeEvent(scrolled,L"scroll");return true;
    }
    void LayoutScriptDialog(){
        if(!scriptDialog)return;
        RECT client{};GetClientRect(hwnd,&client);
        const float scale=DpiScale();
        scriptDialog->Layout(std::max(1.0f,(client.right-client.left)/scale),
            std::max(1.0f,(client.bottom-client.top)/scale),scale);
    }
    void CloseScriptDialog(bool accepted){
        if(!scriptDialog||!scriptDialog->active)return;
        scriptDialog->accepted=accepted;scriptDialog->active=false;
        if(GetCapture()==hwnd)ReleaseCapture();
        InvalidateView();
    }
    void DispatchViewportResize(){
        auto* owner=this;
        while(owner->compositionParent)owner=owner->compositionParent;
        if(owner->scriptDialog&&owner->scriptDialog->active)
            PostMessageW(hwnd,kViewportResizeMessage,0,0);
        else javascript.DispatchWindowEvent(L"resize");
    }
    bool HandleScriptDialogMessage(UINT message,WPARAM wParam,LPARAM lParam){
        auto* owner=this;
        while(owner->compositionParent)owner=owner->compositionParent;
        if(!owner->scriptDialog)return false;
        if(message==WM_KILLFOCUS)return true;
        if(message==WM_CAPTURECHANGED){
            // Native capture can change synchronously while opening/closing
            // the overlay; defer the page's pointercancel callback as well.
            PostMessageW(hwnd,message,wParam,lParam);return true;
        }
        if(!owner->scriptDialog->active)return false;
        auto& dialog=*owner->scriptDialog;
        switch(message){
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:case WM_LBUTTONUP:case WM_MOUSEMOVE:{
            POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            if(owner!=this)MapWindowPoints(hwnd,owner->hwnd,&point,1);
            const float x=owner->PixelToDip(static_cast<float>(point.x));
            const float y=owner->PixelToDip(static_cast<float>(point.y));
            const int button=dialog.ButtonAt(x,y);
            if(message==WM_MOUSEMOVE){
                if(dialog.hoveredButton!=button){
                    dialog.Hover(button);owner->InvalidateView();
                }
            }else if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK){
                dialog.pressedButton=button;
                if(button>=0){
                    dialog.Focus(button,false);
                    owner->InvalidateView();
                }
            }else{
                if(button>=0&&button==dialog.pressedButton)owner->CloseScriptDialog(button==0);
                else dialog.pressedButton=-1;
            }
            return true;
        }
        case WM_KEYDOWN:case WM_SYSKEYDOWN:
            if(message==WM_SYSKEYDOWN&&wParam==VK_F4){owner->CloseScriptDialog(false);return true;}
            if(wParam==VK_ESCAPE){owner->CloseScriptDialog(false);return true;}
            if(wParam==VK_RETURN||wParam==VK_SPACE){
                owner->CloseScriptDialog(dialog.focusedButton==0);return true;
            }
            if(dialog.confirm&&(wParam==VK_TAB||wParam==VK_LEFT||wParam==VK_RIGHT)){
                dialog.Focus(1-dialog.focusedButton,true);
                owner->InvalidateView();
            }
            return true;
        case WM_KEYUP:case WM_SYSKEYUP:case WM_CHAR:case WM_SYSCHAR:
        case WM_IME_STARTCOMPOSITION:case WM_IME_COMPOSITION:case WM_IME_ENDCOMPOSITION:
        case WM_IME_CHAR:
        case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_MBUTTONDOWN:case WM_MBUTTONUP:
        case WM_CONTEXTMENU:
        case WM_MOUSEHOVER:
            return true;
        case WM_MOUSELEAVE:
            dialog.Hover(-1);owner->InvalidateView();return true;
        case WM_DROPFILES:DragFinish(reinterpret_cast<HDROP>(wParam));return true;
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:{
            POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
            ScreenToClient(owner->hwnd,&point);
            if(dialog.layout.ScrollAt(owner->PixelToDip(static_cast<float>(point.x)),
                owner->PixelToDip(static_cast<float>(point.y)),
                static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)),nullptr,message==WM_MOUSEHWHEEL))
                owner->InvalidateView();
            return true;
        }
        case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_ARROW));return true;
        default:return false;
        }
    }
    void ReleaseDialogInput(HWND owner){
        for(auto queued=deferredExecutionMessages.begin();queued!=deferredExecutionMessages.end();){
            const auto& message=queued->message;
            const bool input=(message.message>=WM_KEYFIRST&&message.message<=WM_KEYLAST)||
                (message.message>=WM_MOUSEFIRST&&message.message<=WM_MOUSELAST);
            if(input&&(message.hwnd==owner||IsChild(owner,message.hwnd))){
                PostMessageW(message.hwnd,message.message,message.wParam,message.lParam);
                queued=deferredExecutionMessages.erase(queued);
            }else ++queued;
        }
        for(const auto& frame:childFrames)if(frame.view&&frame.view->impl_)
            frame.view->impl_->ReleaseDialogInput(owner);
    }
    bool RunScriptDialog(const std::wstring& message,bool confirm,const std::wstring& site={}){
        auto* owner=this;
        while(owner->compositionParent)owner=owner->compositionParent;
        const auto senderSite=site.empty()?ScriptDialog::SiteName(currentLocation):site;
        if(owner!=this)return owner->RunScriptDialog(message,confirm,senderSite);
        if(scriptDialog||!hwnd||!IsWindow(hwnd))return false;
        auto dialog=std::make_unique<ScriptDialog>();
        const bool korean=PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_KOREAN;
        if(!dialog->Initialize(senderSite,message,confirm,korean))return false;
        const HWND previousFocus=GetFocus();
        scriptDialog=std::move(dialog);
        LayoutScriptDialog();
        HideTooltip();CloseSelectPopup();
        InvalidateView();UpdateWindow(hwnd);
        SetFocus(hwnd);
        SetCapture(hwnd);
        // A synchronous browser dialog explicitly pauses the current job.
        // Input collected at an earlier safepoint belongs to this same modal
        // loop; withholding its dismissal keys would prevent the job ending.
        ReleaseDialogInput(hwnd);
        MSG pending{};
        std::vector<MSG> deferred;
        while(scriptDialog&&scriptDialog->active&&IsWindow(hwnd)){
            const BOOL received=GetMessageW(&pending,nullptr,0,0);
            if(received<=0){
                if(received==0)PostQuitMessage(static_cast<int>(pending.wParam));
                break;
            }
            if(pending.hwnd==GetAncestor(hwnd,GA_ROOT)&&
               (pending.message==WM_CLOSE||(pending.message==WM_SYSCOMMAND&&
                (pending.wParam&0xfff0)==SC_CLOSE))){
                CloseScriptDialog(false);deferred.push_back(pending);continue;
            }
            // A synchronous simple dialog pauses document jobs. Keep loading
            // completions and JavaScript timers queued until the caller resumes;
            // rendering, resizing and the dialog's own input remain responsive.
            if(pending.hwnd&&(pending.hwnd==hwnd||IsChild(hwnd,pending.hwnd))){
                const bool scriptTimer=pending.message==WM_TIMER&&
                    (pending.wParam==kAnimationFrameTimer||pending.wParam==kJavaScriptTimer);
                const bool scriptJob=pending.message==kAnimationFrameFallbackMessage||
                    pending.message==kNavigationMessage||pending.message==kAsyncImageReadyMessage||
                    pending.message==kAsyncDocumentReadyMessage||pending.message==kAsyncTextReadyMessage||
                    pending.message==kAsyncFrameReadyMessage||pending.message==kAsyncFrameSourceReadyMessage||
                    pending.message==kSyncChildFramesMessage||pending.message==kViewportResizeMessage||
                    pending.message==WM_CAPTURECHANGED;
                if(scriptTimer||scriptJob){
                    if(scriptTimer)KillTimer(pending.hwnd,pending.wParam);
                    deferred.push_back(pending);continue;
                }
            }
            const bool dialogKey=(pending.message==WM_KEYDOWN||pending.message==WM_SYSKEYDOWN)&&
                (pending.hwnd==hwnd||IsChild(hwnd,pending.hwnd));
            if(!dialogKey)TranslateMessage(&pending);
            DispatchMessageW(&pending);
        }
        const bool accepted=scriptDialog&&scriptDialog->accepted;
        if(GetCapture()==hwnd)ReleaseCapture();
        if(previousFocus&&IsWindow(previousFocus))SetFocus(previousFocus);
        scriptDialog.reset();InvalidateView();
        for(const auto& job:deferred)if(IsWindow(job.hwnd))
            PostMessageW(job.hwnd,job.message,job.wParam,job.lParam);
        return accepted;
    }
    LRESULT HandleMessage(UINT message,WPARAM wParam,LPARAM lParam){
        if(message==kDeferredExecutionMessage){
            deferredExecutionMessagePosted=false;
            if(!RenderingScriptBusy())ReleaseDeferredExecutionMessages();
            else if(std::find(executionReplayWindows.begin(),executionReplayWindows.end(),hwnd)==executionReplayWindows.end())
                executionReplayWindows.push_back(hwnd);
            return 0;
        }
        if(HandleScriptDialogMessage(message,wParam,lParam))return 0;
        // A native move/resize loop may dispatch page messages without going
        // through the host's main loop. Preserve JavaScript run-to-completion
        // across the entire frame tree, including asynchronous load callbacks.
        if(RenderingScriptBusy()){
            // UI Automation dispatch borrows a function on the caller's stack.
            // It cannot be queued. Keep its default unavailable result while
            // a script owns the DOM, rather than reentering an action callback.
            if(message==kAccessibilityDispatchMessage)return 0;
            const bool scriptTimer=message==WM_TIMER&&
                (wParam==kAnimationFrameTimer||wParam==kJavaScriptTimer);
            const bool scriptJob=message==kAnimationFrameFallbackMessage||
                message==kNavigationMessage||message==kHistoryTraversalMessage||
                message==kAsyncImageReadyMessage||message==kAsyncDocumentReadyMessage||
                message==kAsyncTextReadyMessage||message==kAsyncFrameReadyMessage||
                message==kAsyncFrameSourceReadyMessage||message==kSyncChildFramesMessage||
                message==kViewportResizeMessage||message==kWorkerReadyMessage;
            const bool input=(message>=WM_KEYFIRST&&message<=WM_KEYLAST)||
                (message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||
                message==WM_MOUSELEAVE||message==WM_CONTEXTMENU||message==WM_CAPTURECHANGED||
                message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_DROPFILES||
                message==WM_COPY||message==WM_CUT||message==WM_PASTE||message==WM_UNDO||
                message==WM_UNICHAR||message==WM_IME_STARTCOMPOSITION||
                message==WM_IME_COMPOSITION||message==WM_IME_ENDCOMPOSITION;
            if(scriptTimer||scriptJob||input||message==WM_SIZE||message==WM_DPICHANGED||message==WM_DPICHANGED_AFTERPARENT){
                if(scriptTimer)KillTimer(hwnd,wParam);
                DeferExecutionMessage(hwnd,message,wParam,lParam);return 0;
            }
        }
        LRESULT textResult=0;if(textInput.HandleMessage(hwnd,message,wParam,lParam,textResult))return textResult;
        if(message==kWorkerReadyMessage){javascript.RunTimers();return 0;}
        // The document hit test, including higher stacking contexts, owns
        // pointer routing. A transparent iframe HWND must never steal an
        // overlaid menu's input; unobscured frames still receive native input.
        if(!openSelectPopup&&!textSelectionDragging&&!scrollbarDragNode&&
           !selectPopupScrollDragging&&!javascript.CapturedPointerTarget()){
            switch(message){
            case WM_LBUTTONDOWN:case WM_LBUTTONUP:case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:case WM_RBUTTONUP:case WM_MBUTTONDOWN:case WM_MBUTTONUP:
            case WM_CONTEXTMENU:
                if(ForwardFramePointer(message,wParam,lParam))return 0;
                break;
            }
        }
        switch(message){
        case kAccessibilityDispatchMessage:return accessibility?accessibility->HandleDispatch(lParam):0;
        case kAsyncImageReadyMessage:
            CompleteAsyncImage(std::unique_ptr<AsyncImageResult>(
                reinterpret_cast<AsyncImageResult*>(lParam)));
            return 0;
        case kAsyncDocumentReadyMessage:
            CompleteAsyncDocument(std::unique_ptr<AsyncDocumentResult>(
                reinterpret_cast<AsyncDocumentResult*>(lParam)));
            return 0;
        case kAsyncTextReadyMessage:{
            std::unique_ptr<AsyncTextResult> result(
                reinterpret_cast<AsyncTextResult*>(lParam));
            if(result&&result->generation==resourceGeneration&&result->completion)
                result->completion(result->loaded,std::move(result->content));
            if(result&&result->generation==resourceGeneration&&result->requestCompletion)
                result->requestCompletion(std::move(result->response));
            return 0;
        }
        case kAsyncFrameReadyMessage:{
            std::unique_ptr<AsyncFrameResult> result(
                reinterpret_cast<AsyncFrameResult*>(lParam));
            if(result&&result->generation==resourceGeneration&&result->node){
                const auto found=std::find_if(childFrames.begin(),childFrames.end(),
                    [&](const ChildFrame& frame){return frame.node==result->node&&
                                                        frame.request==result->request;});
                if(found==childFrames.end())return 0;
                if(!result->success)childFrames.erase(found);
                JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
                javascript.DispatchNodeEvent(result->node,result->success?L"load":L"error",event);
                layoutDirty=true;InvalidateView();
            }
            return 0;
        }
        case kAsyncFrameSourceReadyMessage:{
            std::unique_ptr<AsyncFrameSourceResult> result(
                reinterpret_cast<AsyncFrameSourceResult*>(lParam));
            if(!result||result->generation!=resourceGeneration||!result->node)return 0;
            const auto found=std::find_if(childFrames.begin(),childFrames.end(),
                [&](const ChildFrame& frame){return frame.node==result->node&&
                                                    frame.request==result->request;});
            if(found==childFrames.end())return 0;
            if(result->loaded){
                found->view->impl_->LoadHtmlAsync(result->html,result->base,result->location);
            }else{
                childFrames.erase(found);
                JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
                javascript.DispatchNodeEvent(result->node,L"error",event);
                layoutDirty=true;InvalidateView();
            }
            return 0;
        }
        case kSyncChildFramesMessage:
            childFrameSyncPending=false;SyncChildFrames();return 0;
        case kViewportResizeMessage:DispatchViewportResize();return 0;
        case kHistoryTraversalMessage:
            if(historyTraversalHandler)historyTraversalHandler(static_cast<int>(static_cast<INT_PTR>(wParam)));
            return 0;
        case kNavigationMessage:{
            auto target=std::move(pendingNavigation);pendingNavigation.clear();
            if(target.empty())return 0;
            if(navigationHandler){navigationHandler(target,pendingNavigationNewWindow);return 0;}
            std::wstring html,finalUrl;
            if(!DocumentLoader(compositionParent==nullptr)(target,html,finalUrl)){
                lastError=L"Cannot load HTML resource: "+target;
                if(loadHandler)loadHandler(false,lastError);
                return 0;
            }
            if(!finalUrl.empty())target=finalUrl;
            LoadHtml(html,target,target);return 0;
        }
        case WM_GETOBJECT:if(lParam==static_cast<LPARAM>(UiaRootObjectId)&&accessibility)return accessibility->ReturnRawProvider(wParam,lParam);break;
        case WM_NCHITTEST:{
            const HWND parent=GetParent(hwnd);if(parent){const LRESULT hit=SendMessageW(parent,WM_NCHITTEST,wParam,lParam);
                // Child HWND non-client resize results are not promoted to the
                // top-level sizing loop. Keep the child hit client-owned; the
                // cursor and button handlers below explicitly route the same
                // parent hit to the top-level window.
                if(hit>=HTLEFT&&hit<=HTBOTTOMRIGHT)return HTCLIENT;}
            break;
        }
        case WM_NCLBUTTONDOWN:
            if(wParam>=HTLEFT&&wParam<=HTBOTTOMRIGHT){const HWND parent=GetParent(hwnd);if(parent){ReleaseCapture();return SendMessageW(parent,message,wParam,lParam);}}
            break;
        case WM_PAINT:Paint();return 0;case WM_ERASEBKGND:return 1;
        case WM_PRINTCLIENT:PrintClient(reinterpret_cast<HDC>(wParam));return 0;
        case WM_IME_SETCONTEXT:return DefWindowProcW(hwnd,message,wParam,lParam&~ISC_SHOWUICOMPOSITIONWINDOW);
        case WM_SETFOCUS:textInput.UpdateCandidateWindow(hwnd);ResetCaretBlink();return 0;
        case WM_TIMER:
            if(wParam==kAnimationFrameTimer){KillTimer(hwnd,kAnimationFrameTimer);javascript.RunAnimationFrame();return 0;}
            if(wParam==kJavaScriptTimer){KillTimer(hwnd,kJavaScriptTimer);javascript.RunTimers();return 0;}
            if(wParam==kCssTransitionTimer){
                const auto now=std::chrono::steady_clock::now();
                const float elapsed=std::max(1.0f,std::chrono::duration<float,std::milli>(now-cssTransitionTick).count());
                cssTransitionTick=now;const bool scriptBusy=RenderingScriptBusy();
                if(layoutDirty&&!scriptBusy)Rebuild();
                const bool remains=layout.AdvanceTransitions(elapsed);
                if(!scriptBusy)UpdateFrameBounds();
                InvalidateView();
                if(!remains){KillTimer(hwnd,kCssTransitionTimer);cssTransitionTimerActive=false;}
                return 0;
            }
            if(wParam==kCaretBlinkTimer){if(CanBlinkCaret()){caretVisible=!caretVisible;InvalidateCaret();}else StopCaretBlink();return 0;}
            if(wParam==kImageAnimationTimer){AdvanceImageAnimations();return 0;}break;
        case kAnimationFrameFallbackMessage:javascript.RunAnimationFrame();return 0;
        case WM_SIZE:{HideTooltip();const bool viewportOnly=!layoutDirty||viewportOnlyDirty;layoutDirty=true;viewportOnlyDirty=viewportOnly;UpdateJavaScriptViewport();LayoutScriptDialog();DispatchViewportResize();InvalidateView();return 0;}
        case WM_DPICHANGED:case WM_DPICHANGED_AFTERPARENT:{HideTooltip();UpdateTooltipMetrics();
            // Child windows receive AFTERPARENT in a per-monitor-v2 host. Both
            // notifications must invalidate device resources before updating
            // CSS viewport and input geometry for the new monitor scale.
            ResetRenderTargets();layout.DiscardDeviceResources();
            const bool viewportOnly=!layoutDirty||viewportOnlyDirty;layoutDirty=true;viewportOnlyDirty=viewportOnly;
            UpdateJavaScriptViewport();LayoutScriptDialog();DispatchViewportResize();InvalidateView();return 0;}
        case WM_DROPFILES:{
            const auto drop=reinterpret_cast<HDROP>(wParam);
            POINT point{};DragQueryPoint(drop,&point);
            std::vector<Node::FileInfo> files;
            const UINT count=DragQueryFileW(drop,0xffffffff,nullptr,0);
            for(UINT index=0;index<count;++index){
                const UINT length=DragQueryFileW(drop,index,nullptr,0);
                std::wstring path(length+1,L'\0');
                DragQueryFileW(drop,index,path.data(),length+1);path.resize(length);
                Node::FileInfo info;if(FileInfoForPath(path,info))files.push_back(std::move(info));
            }
            DragFinish(drop);
            if(!files.empty()){
                if(layoutDirty)Rebuild();
                javascript.DispatchFileDrop(layout.HitTest(PixelToDip(static_cast<float>(point.x)),
                                                       PixelToDip(static_cast<float>(point.y))),files);
                layoutDirty=true;InvalidateView();
            }
            return 0;
        }
        case WM_LBUTTONDOWN:{
            HideTooltip();
            primaryPointerDownTarget.reset();primaryClickDetail=0;
            focusVisibleFromKeyboard=false;
            POINT screenPoint{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ClientToScreen(hwnd,&screenPoint);
            const LRESULT resizeHit=ParentResizeHit(screenPoint);if(resizeHit>=HTLEFT&&resizeHit<=HTBOTTOMRIGHT){ReleaseCapture();PostMessageW(GetParent(hwnd),WM_NCLBUTTONDOWN,static_cast<WPARAM>(resizeHit),MAKELPARAM(static_cast<WORD>(screenPoint.x),static_cast<WORD>(screenPoint.y)));return 0;}
            const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam))),y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));if(layoutDirty)Rebuild();
            if(openSelectPopup){
                SelectPopupGeometry popup;const auto control=openSelectPopup;const auto* controlBox=layout.BoxFor(control);
                const int optionIndex=SelectPopupIndexAt(x,y);
                if(optionIndex>=0){const auto options=PopupOptions(control);if(static_cast<size_t>(optionIndex)<options.size()&&!options[optionIndex]->disabled)ChoosePopupOption(control,static_cast<size_t>(optionIndex));CloseSelectPopup();return 0;}
                if(GetSelectPopupGeometry(control,popup)&&popup.bounds.Contains(x,y)){
                    const float scrollbarLeft=popup.bounds.x+popup.bounds.width-popup.borderWidth-popup.scrollbarWidth;
                    if(popup.scrollbarWidth>0&&x>=scrollbarLeft){
                        const float innerTop=popup.bounds.y+popup.borderWidth;
                        const float thumbHeight=std::max(20.0f,popup.viewportHeight*popup.viewportHeight/popup.contentHeight);
                        const float maximum=std::max(0.0f,popup.contentHeight-popup.viewportHeight);
                        const float travel=std::max(0.0f,popup.viewportHeight-thumbHeight);
                        const float thumbTop=innerTop+(maximum>0?popup.scrollOffset/maximum*travel:0);
                        if(y>=thumbTop&&y<=thumbTop+thumbHeight){selectPopupScrollDragging=true;selectPopupScrollDragOffset=y-thumbTop;SetCapture(hwnd);}
                        else ScrollSelectPopup(y<thumbTop?-popup.viewportHeight*0.9f:popup.viewportHeight*0.9f);
                    }
                    return 0;
                }
                if(controlBox&&controlBox->rect.Contains(x,y)){
                    if(control->tag==L"select")CloseSelectPopup();
                    else OpenSelectPopup(control,x>=controlBox->rect.x+controlBox->rect.width-controlBox->rect.height,false);
                    return 0;
                }
                CloseSelectPopup();
            }
            if(layout.BeginScrollbarInteraction(x,y,scrollbarDragNode,scrollbarDragOffset,scrollbarDragHorizontal)){if(scrollbarDragNode)SetCapture(hwnd);else javascript.DispatchNodeEvent(layout.HitTest(x,y),L"scroll");if(accessibility)accessibility->Invalidate();UpdateFrameBounds();InvalidateView();return 0;}
            SetFocus(hwnd);auto target=layout.HitTest(x,y);auto pointer=PointerEventAt(wParam,x,y,0,1);
            primaryPointerDownTarget=target;primaryClickDetail=1;
            if(javascript.DispatchNodeEvent(target,L"pointerdown",pointer))return 0;
            PrepareActivation(target,x,y);layoutDirty=true;InvalidateView();
            if(editingNode&&focused&&FocusTarget(target)==focused&&
               (IsTextControl(focused)||IsContentEditable(focused))){
                textSelectionDragging=true;SetCapture(hwnd);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK:{focusVisibleFromKeyboard=false;const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam))),y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));if(layoutDirty)Rebuild();auto target=layout.HitTest(x,y);auto pointer=PointerEventAt(wParam,x,y,0,2);primaryPointerDownTarget=target;primaryClickDetail=2;if(!javascript.DispatchNodeEvent(target,L"pointerdown",pointer))PrepareActivation(target,x,y);return 0;}
        case WM_CONTEXTMENU:{float x=0,y=0;std::shared_ptr<Node> target;const bool keyboard=lParam==static_cast<LPARAM>(-1);if(keyboard){target=focused;if(layoutDirty)Rebuild();if(const auto* box=layout.BoxFor(target)){x=box->content.x+box->content.width/2.0f;y=box->content.y+box->content.height/2.0f;}}else{POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};ScreenToClient(hwnd,&point);x=PixelToDip(static_cast<float>(point.x));y=PixelToDip(static_cast<float>(point.y));if(layoutDirty)Rebuild();target=layout.HitTest(x,y);}auto context=PointerEventAt(0,x,y,2,keyboard?0:1);javascript.DispatchNodeEvent(target,L"contextmenu",context);return 0;}
        case WM_LBUTTONUP:
            if(selectPopupScrollDragging){selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;if(GetCapture()==hwnd)ReleaseCapture();return 0;}
            if(scrollbarDragNode){InvalidateView();scrollbarDragNode.reset();scrollbarDragOffset=0;scrollbarDragHorizontal=false;if(GetCapture()==hwnd)ReleaseCapture();return 0;}
            if(textSelectionDragging){
                const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam)));
                const float y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));
                UpdateTextSelectionAt(x,y);textSelectionDragging=false;
                ReleasePrimaryPointer(wParam,x,y);return 0;
            }
            {
                const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam))),y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));
                ReleasePrimaryPointer(wParam,x,y);return 0;
            }
            break;
        case WM_CAPTURECHANGED:{HideTooltip();primaryPointerDownTarget.reset();primaryClickDetail=0;textSelectionDragging=false;selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;scrollbarDragNode.reset();scrollbarDragOffset=0;scrollbarDragHorizontal=false;if(auto captured=javascript.CapturedPointerTarget()){auto pointer=PointerEventAt(0,pointerX,pointerY);javascript.DispatchNodeEvent(captured,L"pointercancel",pointer);if(javascript.CapturedPointerTarget())javascript.ClearPointerCapture();}return 0;}
        case WM_MOUSEWHEEL:case WM_MOUSEHWHEEL:
            // An exhausted or non-scrolling browsing context continues the
            // wheel's scroll chain in its containing document. Screen-space
            // coordinates remain valid across nested frames and monitor DPI.
            for(auto* receiver=this;receiver;receiver=receiver->compositionParent)
                if(receiver->HandleWheel(message,wParam,lParam,receiver==this))break;
            return 0;
        case WM_MOUSEMOVE:{
            if(!trackingMouseLeave){TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT),TME_LEAVE,hwnd,0};trackingMouseLeave=TrackMouseEvent(&tracking)!=FALSE;}
            const float x=PixelToDip(static_cast<float>(GET_X_LPARAM(lParam))),y=PixelToDip(static_cast<float>(GET_Y_LPARAM(lParam)));
            if(selectPopupScrollDragging){HideTooltip();SelectPopupGeometry popup;if(GetSelectPopupGeometry(openSelectPopup,popup)){
                const float thumbHeight=std::max(20.0f,popup.viewportHeight*popup.viewportHeight/popup.contentHeight);
                const float travel=std::max(0.0f,popup.viewportHeight-thumbHeight),maximum=std::max(0.0f,popup.contentHeight-popup.viewportHeight);
                const float thumbTop=std::max(0.0f,std::min(travel,y-(popup.bounds.y+popup.borderWidth)-selectPopupScrollDragOffset));
                const float previous=selectPopupScrollOffset;selectPopupScrollOffset=travel>0?thumbTop/travel*maximum:0;
                if(std::abs(previous-selectPopupScrollOffset)>0.01f)InvalidateView();
            }return 0;}
            if(scrollbarDragNode){HideTooltip();if(layout.DragScrollbar(scrollbarDragNode,x,y,scrollbarDragOffset,scrollbarDragHorizontal)){javascript.DispatchNodeEvent(scrollbarDragNode,L"scroll");if(accessibility)accessibility->Invalidate();textInput.UpdateCandidateWindow(hwnd);InvalidateScrollViewport(scrollbarDragNode);}return 0;}
            if(textSelectionDragging){HideTooltip();UpdateTextSelectionAt(x,y);return 0;}
            if(layoutDirty)Rebuild();if(openSelectPopup){int hot=SelectPopupIndexAt(x,y);const auto options=PopupOptions(openSelectPopup);if(hot>=0&&(static_cast<size_t>(hot)>=options.size()||options[hot]->disabled))hot=-1;if(hot!=selectPopupHotIndex){selectPopupHotIndex=hot;InvalidateView();}SelectPopupGeometry popup;if(GetSelectPopupGeometry(openSelectPopup,popup)&&popup.bounds.Contains(x,y)){HideTooltip();return 0;}}
            auto n=layout.HitTest(x,y);UpdateTooltipTarget(n);
            auto pointer=PointerEventAt(wParam,x,y);if(hasPointerPosition){pointer.movementX=x-pointerX;pointer.movementY=y-pointerY;}
            const auto previous=hovered;LayoutRect dirty{};bool targeted=false;
            if(n!=previous){const bool hoverLayoutChanged=SetHoveredNode(n,&dirty,&targeted);layoutDirty=layoutDirty||hoverLayoutChanged;DispatchPointerTransition(previous,n,pointer);}
            if(auto target=javascript.CapturedPointerTarget())javascript.DispatchNodeEvent(target,L"pointermove",pointer);
            else if(n)javascript.DispatchNodeEvent(n,L"pointermove",pointer);
            pointerX=x;pointerY=y;hasPointerPosition=true;
            if(n!=previous){if(!layoutDirty&&targeted)InvalidateDipBounds(dirty);else InvalidateView();}
            ForwardFramePointer(message,wParam,lParam);return 0;
        }
        case WM_MOUSEHOVER:if(!ForwardFramePointer(message,wParam,lParam))ShowTooltipAt(lParam);return 0;
        case WM_MOUSELEAVE:{LeavePointerFrame();trackingMouseLeave=false;HideTooltip();const auto previous=hovered;if(previous){
            auto pointer=PointerEventAt(0,pointerX,pointerY);LayoutRect dirty{};bool targeted=false;
            const bool hoverLayoutChanged=SetHoveredNode({},&dirty,&targeted);layoutDirty=layoutDirty||hoverLayoutChanged;
            DispatchPointerTransition(previous,{},pointer);
            if(!layoutDirty&&targeted)InvalidateDipBounds(dirty);else InvalidateView();
        }hasPointerPosition=false;return 0;}
        case WM_GETDLGCODE:return DLGC_WANTTAB|DLGC_WANTARROWS|
            ((CanEditText()||focused&&focused->tag==L"select")?DLGC_WANTCHARS:0);
        case WM_KEYUP:{JavaScriptRuntime::EventInit key{};key.key=KeyValue(wParam);key.ctrlKey=(GetKeyState(VK_CONTROL)&0x8000)!=0;key.shiftKey=(GetKeyState(VK_SHIFT)&0x8000)!=0;key.altKey=(GetKeyState(VK_MENU)&0x8000)!=0;key.metaKey=(GetKeyState(VK_LWIN)&0x8000)!=0||(GetKeyState(VK_RWIN)&0x8000)!=0;javascript.DispatchNodeEvent(focused,L"keyup",key);return 0;}
        case WM_KEYDOWN:{HideTooltip();JavaScriptRuntime::EventInit key{};key.key=KeyValue(wParam);key.ctrlKey=(GetKeyState(VK_CONTROL)&0x8000)!=0;key.shiftKey=(GetKeyState(VK_SHIFT)&0x8000)!=0;key.altKey=(GetKeyState(VK_MENU)&0x8000)!=0;key.metaKey=(GetKeyState(VK_LWIN)&0x8000)!=0||(GetKeyState(VK_RWIN)&0x8000)!=0;if(!key.ctrlKey&&!key.altKey&&!key.metaKey)focusVisibleFromKeyboard=true;if(javascript.DispatchNodeEvent(focused,L"keydown",key))return 0;}
        if(textInput.IsComposing())break;
        if(wParam==VK_ESCAPE)if(const auto dialog=document.QuerySelector(L"dialog[open]")){JavaScriptRuntime::EventInit cancel{};cancel.bubbles=false;cancel.cancelable=true;if(!javascript.DispatchNodeEvent(dialog,L"cancel",cancel)&&dialog->attributes.count(L"open")){dialog->RemoveAttribute(L"open");dialog->modal=false;JavaScriptRuntime::EventInit close{};close.bubbles=false;close.cancelable=false;javascript.DispatchNodeEvent(dialog,L"close",close);layoutDirty=true;InvalidateView();}return 0;}
        if(openSelectPopup){
            if(wParam==VK_ESCAPE||wParam==VK_F4){CloseSelectPopup();return 0;}
            if(wParam==VK_UP){MoveSelectPopupHot(-1);return 0;}
            if(wParam==VK_DOWN){MoveSelectPopupHot(1);return 0;}
            if(wParam==VK_HOME){MoveSelectPopupHot(-1,true);return 0;}
            if(wParam==VK_END){MoveSelectPopupHot(1,true);return 0;}
            if(wParam==VK_SPACE||wParam==VK_RETURN){const auto control=openSelectPopup;const int option=selectPopupHotIndex;if(option>=0)ChoosePopupOption(control,static_cast<size_t>(option));CloseSelectPopup();return 0;}
            if(wParam==VK_TAB)CloseSelectPopup();
        }
        if(wParam==VK_TAB){MoveSequentialFocus((GetKeyState(VK_SHIFT)&0x8000)!=0);return 0;}
        if(MoveMenuFocus(wParam))return 0;
        if(focused&&focused->tag==L"select"){
            const bool alt=(GetKeyState(VK_MENU)&0x8000)!=0;
            if(wParam==VK_F4||(alt&&wParam==VK_DOWN)||wParam==VK_SPACE||wParam==VK_RETURN){OpenSelectPopup(focused);return 0;}
            if(wParam==VK_UP){MoveSelectOption(focused,-1);return 0;}
            if(wParam==VK_DOWN){MoveSelectOption(focused,1);return 0;}
            if(wParam==VK_HOME){MoveSelectOption(focused,-1,true);return 0;}
            if(wParam==VK_END){MoveSelectOption(focused,1,true);return 0;}
        }
        if(IsKeyboardActivatable(focused)&&(wParam==VK_SPACE||wParam==VK_RETURN)){
            if(layoutDirty)Rebuild();float x=0,y=0;if(const auto* box=layout.BoxFor(focused)){
                x=box->content.x+box->content.width/2.0f;y=box->content.y+box->content.height/2.0f;
            }
            Activate(focused,x,y,true);return 0;
        }
        if(CanEditText()){
            const bool extend=(GetKeyState(VK_SHIFT)&0x8000)!=0,control=(GetKeyState(VK_CONTROL)&0x8000)!=0;const auto value=EditingValue();
            if(control&&(wParam==L'C'||wParam==L'c')){CopySelectionToClipboard();return 0;}
            if(control&&(wParam==L'X'||wParam==L'x')){if(CopySelectionToClipboard())ReplaceSelection(L"",L"deleteByCut");return 0;}
            if(control&&(wParam==L'V'||wParam==L'v')){PasteFromClipboard();return 0;}
            if(control&&(wParam==L'Z'||wParam==L'z')){ApplyHistory(extend);return 0;}
            if(control&&(wParam==L'Y'||wParam==L'y')){ApplyHistory(true);return 0;}
            if(control&&(wParam==L'A'||wParam==L'a')){selectionAnchor=0;caretPosition=value.size();verticalCaretXValid=false;StoreControlSelection();ResetCaretBlink();return 0;}
            if(wParam==VK_LEFT){MoveHorizontalCaret(true,extend);return 0;}
            if(wParam==VK_RIGHT){MoveHorizontalCaret(false,extend);return 0;}
            if(wParam==VK_UP){MoveVerticalCaret(true,extend);return 0;}
            if(wParam==VK_DOWN){MoveVerticalCaret(false,extend);return 0;}
            if(wParam==VK_HOME){MoveCaret(0,extend);return 0;}if(wParam==VK_END){MoveCaret(value.size(),extend);return 0;}if(wParam==VK_DELETE){DeleteForward();return 0;}if(wParam==VK_RETURN&&IsTextInput(focused)){SubmitNearestForm(focused);return 0;}
        }break;
        case WM_COPY:CopySelectionToClipboard();return 0;
        case WM_CUT:if(CopySelectionToClipboard())ReplaceSelection(L"",L"deleteByCut");return 0;
        case WM_PASTE:PasteFromClipboard();return 0;
        case WM_UNDO:ApplyHistory(false);return 0;
        case WM_CHAR:if(textInput.IsComposing())return 0;else if(CanEditText()){if(wParam==VK_BACK)Backspace();else if(wParam==L'\r'){
            if(IsTextControl(focused)){if(!IsTextInput(focused))ReplaceSelection(L"\n",L"insertLineBreak");}
            else InsertEditingParagraph();
        }else if(wParam>=32&&wParam!=127)ReplaceSelection(std::wstring(1,static_cast<wchar_t>(wParam)));return 0;}break;
        case WM_UNICHAR:if(wParam==UNICODE_NOCHAR)return TRUE;else if(CanEditText()&&wParam>=32&&wParam<=0x10ffff){std::wstring text;if(wParam<=0xffff)text.push_back(static_cast<wchar_t>(wParam));else{const auto value=static_cast<unsigned>(wParam)-0x10000;text.push_back(static_cast<wchar_t>(0xd800+(value>>10)));text.push_back(static_cast<wchar_t>(0xdc00+(value&0x3ff)));}ReplaceSelection(text);return 0;}break;
        case WM_KILLFOCUS:HideTooltip();primaryPointerDownTarget.reset();primaryClickDetail=0;textSelectionDragging=false;if(GetCapture()==hwnd)ReleaseCapture();CloseSelectPopup();textInput.Cancel(hwnd);SetFocusedNode({});StopCaretBlink();InvalidateView();return 0;
        case WM_SETCURSOR:{POINT screenPoint{};GetCursorPos(&screenPoint);const LRESULT parentHit=ParentResizeHit(screenPoint);if(const HCURSOR resize=ResizeCursor(parentHit)){SetCursor(resize);return TRUE;}const UINT hit=LOWORD(lParam);
            if(const HCURSOR resize=ResizeCursor(hit)){SetCursor(resize);return TRUE;}
            if(hit==HTCLIENT){SetCursor(CursorAtCurrentPosition());return TRUE;}break;}
        case WM_DESTROY:{
            javascript.SetWorkerWakeHandler({});
            javascript.SetExecutionYieldHandler({});javascript.SetExecutionCompletionHandler({});
            ReleaseDeferredExecutionMessages(false);
            if(scriptDialog)scriptDialog->active=false;
            {std::lock_guard<std::mutex> lock(asyncLifetime->mutex);
                asyncLifetime->alive=false;asyncLifetime->hwnd=nullptr;}
            MSG pending{};
            while(PeekMessageW(&pending,hwnd,kAsyncImageReadyMessage,
                               kAsyncImageReadyMessage,PM_REMOVE))
                delete reinterpret_cast<AsyncImageResult*>(pending.lParam);
            while(PeekMessageW(&pending,hwnd,kAsyncDocumentReadyMessage,
                               kAsyncDocumentReadyMessage,PM_REMOVE))
                delete reinterpret_cast<AsyncDocumentResult*>(pending.lParam);
            while(PeekMessageW(&pending,hwnd,kAsyncTextReadyMessage,
                               kAsyncTextReadyMessage,PM_REMOVE))
                delete reinterpret_cast<AsyncTextResult*>(pending.lParam);
            while(PeekMessageW(&pending,hwnd,kAsyncFrameReadyMessage,
                               kAsyncFrameReadyMessage,PM_REMOVE))
                delete reinterpret_cast<AsyncFrameResult*>(pending.lParam);
            while(PeekMessageW(&pending,hwnd,kAsyncFrameSourceReadyMessage,
                               kAsyncFrameSourceReadyMessage,PM_REMOVE))
                delete reinterpret_cast<AsyncFrameSourceResult*>(pending.lParam);
            pendingNavigation.clear();HideTooltip();if(tooltip){DestroyWindow(tooltip);tooltip=nullptr;}if(tooltipFont){DeleteObject(tooltipFont);tooltipFont=nullptr;}KillTimer(hwnd,kAnimationFrameTimer);KillTimer(hwnd,kJavaScriptTimer);KillTimer(hwnd,kCssTransitionTimer);KillTimer(hwnd,kCaretBlinkTimer);KillTimer(hwnd,kImageAnimationTimer);caretBlinkTimerActive=false;cssTransitionTimerActive=false;imageAnimationTimerActive=false;childFrames.clear();textInput.Cancel(nullptr);if(accessibility)accessibility->Disconnect();ResetRenderTargets();return 0;}
        default:break;}return DefWindowProcW(hwnd,message,wParam,lParam);}
    void ResetInitialDocumentLoad(){
        initialDocumentScripts.clear();initialImageEvents.clear();
        nextOrderedInitialScript=0;nextDeferredInitialScript=0;
        remainingInitialScripts=0;initialScriptGeneration=0;
        initialBlockingScriptsDrained=false;initialDomContentLoaded=false;
        initialLoadDispatched=false;
    }
    void DispatchReadyInitialImages(){
        auto images=std::move(initialImageEvents);initialImageEvents.clear();
        for(const auto& item:images)if(item.first->imageSource==item.second&&
            !item.second.empty()&&item.first->imageComplete){
            JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
            javascript.DispatchNodeEvent(item.first,item.first->image?L"load":L"error",event);
        }
    }
    void FinishInitialDocumentLoad(){
        if(initialLoadDispatched||!initialDomContentLoaded||remainingInitialScripts)return;
        initialLoadDispatched=true;
        javascript.SetDocumentReadyState(L"complete");
        javascript.DispatchWindowEvent(L"load");
        layoutDirty=true;InvalidateView();
        if(loadHandler)loadHandler(true,L"");
    }
    bool ExecuteInitialDocumentScript(const std::shared_ptr<InitialDocumentScript>& script,
                                      std::uint64_t generation){
        if(!script||script->finished||generation!=resourceGeneration||
           generation!=initialScriptGeneration)return false;
        bool executed=script->loaded;
        if(executed&&!script->program.empty()){
            std::wstring scriptError;
            javascript.SetCurrentScript(script->node);
            executed=javascript.Execute(script->program,nullptr,&scriptError);
            javascript.SetCurrentScript({});
            if(generation!=resourceGeneration||generation!=initialScriptGeneration)return false;
        }
        if(!executed){
            JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
            javascript.DispatchNodeEvent(script->node,L"error",event);
        }else if(!script->resource.empty()){
            JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
            javascript.DispatchNodeEvent(script->node,L"load",event);
        }
        script->finished=true;
        if(remainingInitialScripts)--remainingInitialScripts;
        FinishInitialDocumentLoad();
        return true;
    }
    void DrainOrderedInitialScripts(){
        const auto generation=initialScriptGeneration;
        if(!initialBlockingScriptsDrained){
            while(nextOrderedInitialScript<initialDocumentScripts.size()){
                const auto script=initialDocumentScripts[nextOrderedInitialScript];
                if(script->asynchronous||script->deferred||script->finished){
                    ++nextOrderedInitialScript;continue;
                }
                if(!script->ready)return;
                if(!ExecuteInitialDocumentScript(script,generation))return;
                ++nextOrderedInitialScript;
            }
            initialBlockingScriptsDrained=true;
        }
        while(nextDeferredInitialScript<initialDocumentScripts.size()){
            const auto script=initialDocumentScripts[nextDeferredInitialScript];
            if(!script->deferred||script->finished){
                ++nextDeferredInitialScript;continue;
            }
            if(!script->ready)return;
            if(!ExecuteInitialDocumentScript(script,generation))return;
            ++nextDeferredInitialScript;
        }
        if(initialDomContentLoaded||generation!=resourceGeneration||
           generation!=initialScriptGeneration)return;
        initialDomContentLoaded=true;
        javascript.SetDocumentReadyState(L"interactive");
        DispatchReadyInitialImages();
        javascript.DispatchDocumentEvent(L"DOMContentLoaded");
        if(generation!=resourceGeneration||generation!=initialScriptGeneration)return;
        SyncChildFrames();
        FinishInitialDocumentLoad();
    }
    void CompleteInitialScriptLoad(const std::shared_ptr<InitialDocumentScript>& script,
                                   std::uint64_t generation,bool loaded,
                                   std::wstring program){
        if(!script||generation!=resourceGeneration||generation!=initialScriptGeneration)return;
        script->loaded=loaded;script->program=std::move(program);script->ready=true;
        if(script->asynchronous){
            ExecuteInitialDocumentScript(script,generation);
            DrainOrderedInitialScripts();
        }else DrainOrderedInitialScripts();
    }
    void BeginInitialDocumentScripts(
        const std::unordered_map<std::wstring,std::wstring>& preparedText,
        const std::unordered_set<std::wstring>& failedText){
        initialScriptGeneration=resourceGeneration;
        const auto generation=initialScriptGeneration;
        const auto scripts=pageScriptsEnabled?document.QuerySelectorAll(L"script"):
                                              std::vector<std::shared_ptr<Node>>{};
        for(const auto& node:scripts){
            if(!IsClassicJavaScriptType(node->Attribute(L"type")))continue;
            node->scriptStarted=true;
            auto script=std::make_shared<InitialDocumentScript>();
            script->node=node;script->resource=node->Attribute(L"src");
            script->asynchronous=!script->resource.empty()&&
                                 node->attributes.count(L"async")!=0;
            script->deferred=!script->asynchronous&&!script->resource.empty()&&
                             node->attributes.count(L"defer")!=0;
            if(script->resource.empty()){
                script->program=node->InnerText();script->loaded=true;script->ready=true;
            }else{
                const auto key=ResolveResourceReference(script->resource);
                const auto ready=preparedText.find(key);
                if(ready!=preparedText.end()){
                    script->program=ready->second;script->loaded=true;script->ready=true;
                }else if(failedText.count(key)>0)script->ready=true;
            }
            initialDocumentScripts.push_back(std::move(script));
        }
        remainingInitialScripts=initialDocumentScripts.size();
        for(const auto& script:initialDocumentScripts)if(script->asynchronous&&script->ready)
            ExecuteInitialDocumentScript(script,generation);
        for(const auto& script:initialDocumentScripts){
            if(script->ready||script->resource.empty())continue;
            const auto resource=script->resource;const auto loader=TextLoader(currentLocation);
            const auto base=basePath;const auto lifetime=asyncLifetime;
            ImageWorkers().Submit(BackgroundWorkQueue::Priority::Critical,
                [this,script,resource,loader,base,lifetime,generation]{
                    {
                        std::lock_guard<std::mutex> lock(lifetime->mutex);
                        if(!lifetime->alive||!lifetime->hwnd||
                           lifetime->resourceGeneration!=generation)return;
                    }
                    auto result=std::make_unique<AsyncTextResult>();
                    result->generation=generation;
                    result->loaded=LoadTextResourceForBase(
                        loader,base,resource,result->content);
                    result->completion=[this,script,generation]
                        (bool loaded,std::wstring program){
                        CompleteInitialScriptLoad(script,generation,loaded,
                                                  std::move(program));
                    };
                    std::lock_guard<std::mutex> lock(lifetime->mutex);
                    if(!lifetime->alive||!lifetime->hwnd||
                       lifetime->resourceGeneration!=generation)return;
                    if(PostMessageW(lifetime->hwnd,kAsyncTextReadyMessage,0,
                                    reinterpret_cast<LPARAM>(result.get())))result.release();
                });
        }
        DrainOrderedInitialScripts();
    }
    bool LoadHtmlInternal(const std::wstring* html,std::unique_ptr<Document> parsed,
                          std::unique_ptr<StyleSheet> parsedStyles,
                          std::unordered_map<std::wstring,std::wstring> preparedText,
                          std::unordered_set<std::wstring> failedText,
                          const std::wstring& base,const std::wstring& location,
                          bool preserveJavaScriptWindow=false,
                          bool deferInitialScripts=false){
        primaryPointerDownTarget.reset();primaryClickDetail=0;
        HideTooltip();lastError.clear();childFrames.clear();childFrameSyncPending=false;basePath=base;currentLocation=location;++resourceGeneration;ResetInitialDocumentLoad();PublishAsyncGenerations();rasterImageCache.clear();pendingRasterImages.clear();ClearPendingImageLoads();textInput.Cancel(nullptr);focused.reset();editingNode.reset();ClearEditingBoundary();textSelectionDragging=false;if(GetCapture()==hwnd)ReleaseCapture();StopCaretBlink();hovered.reset();hoverPath.clear();hasPointerPosition=false;pointerX=pointerY=0;scrollbarDragNode.reset();scrollbarDragOffset=0;scrollbarDragHorizontal=false;openSelectPopup.reset();selectPopupHotIndex=-1;selectPopupScrollOffset=0;selectPopupShowAll=false;selectPopupScrollDragging=false;selectPopupScrollDragOffset=0;selectionAnchor=caretPosition=0;textEditDirty=false;liveRegions.clear();liveRegionText.clear();KillTimer(hwnd,kCssTransitionTimer);KillTimer(hwnd,kImageAnimationTimer);cssTransitionTimerActive=false;imageAnimationTimerActive=false;layout.ClearTransitions();if(!preserveJavaScriptWindow)javascript.Clear();javascript.SetDocumentReadyState(L"loading");UpdateJavaScriptViewport();if(parsed)document.AdoptParsed(*parsed);else if(!html||!document.Parse(*html,&lastError)){if(loadHandler)loadHandler(false,lastError);return false;}
        const bool externalPage=base.find(L"://")!=std::wstring::npos;
        const auto loadText=[&](const std::wstring& reference,std::wstring& content){
            const auto key=ResolveResourceReference(reference);
            const auto found=preparedText.find(key);
            if(found!=preparedText.end()){content=found->second;return true;}
            if(failedText.count(key)>0)return false;
            return LoadTextResource(reference,content);
        };
        if(parsedStyles){styles=std::move(*parsedStyles);UpdateJavaScriptViewport();}
        else{
            const auto css=CollectDocumentStyles(document,basePath,loadText,!externalPage,lastError);
            if(!lastError.empty()){if(loadHandler)loadHandler(false,lastError);return false;}
            if(!styles.Parse(css,&lastError)){
                if(!externalPage){if(loadHandler)loadHandler(false,lastError);return false;}
                styles.Parse(L"",nullptr);lastError.clear();
            }
        }
        installedStylesheetText.clear();stylesheetResources.clear();stylesheetSourcesDirty=true;
        for(const auto& link:document.QuerySelectorAll(L"link[rel='stylesheet']")){
            const auto key=ResolveResourceReference(link->Attribute(L"href"));
            const auto prepared=preparedText.find(key);
            if(prepared!=preparedText.end())stylesheetResources.emplace(key,prepared->second);
        }
        // Page scripts can synchronously query layout while the document is
        // still loading.  The previous navigation's layout boxes are no
        // longer valid once the new DOM and style sheet have been installed,
        // so make the geometry provider rebuild them before the first such
        // query instead of exposing stale (or missing) coordinates.
        layoutDirty=true;viewportOnlyDirty=false;
        LoadImages(false);std::vector<std::pair<std::shared_ptr<Node>,std::wstring>> initialImages;
        for(const auto& image:document.QuerySelectorAll(L"img"))
            initialImages.push_back({image,image->imageSource});
        javascript.SetLocation(location);
        if(deferInitialScripts){
            initialImageEvents=std::move(initialImages);
            layoutDirty=true;InvalidateView();
            JavaScriptRuntime::Mutation initialMutation;
            initialMutation.kind=JavaScriptRuntime::MutationKind::Tree;
            initialMutation.targets.push_back(document.Root());
            NotifyAccessibilityMutation(initialMutation,true);
            // Publish a complete DOM/CSS frame before any initial page script
            // can monopolize the UI thread. Script execution begins only after
            // the render target has had this explicit first-paint checkpoint.
            RedrawWindow(hwnd,nullptr,nullptr,
                         RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
            BeginInitialDocumentScripts(preparedText,failedText);
            return true;
        }
        // Each classic script is its own JavaScript Script Record. A load or
        // syntax/runtime error in one script must not prevent later scripts
        // from running, and function declarations are hoisted only within the
        // script that contains them. Hosts may disable automatic page scripts
        // while retaining normal URL, stylesheet, iframe and form behavior.
        for(const auto& script:pageScriptsEnabled?document.QuerySelectorAll(L"script"):
                                                  std::vector<std::shared_ptr<Node>>{}){
            if(!IsClassicJavaScriptType(script->Attribute(L"type")))continue;
            const auto source=script->Attribute(L"src");std::wstring program;
            if(source.empty())program=script->InnerText();
            else if(!loadText(source,program)){
                JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
                javascript.DispatchNodeEvent(script,L"error",event);continue;
            }
            std::wstring scriptError;
            javascript.SetCurrentScript(script);
            const bool executed=javascript.Execute(program,nullptr,&scriptError);
            javascript.SetCurrentScript({});
            if(!executed){
                JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
                javascript.DispatchNodeEvent(script,L"error",event);continue;
            }
            if(!source.empty()){
                JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
                javascript.DispatchNodeEvent(script,L"load",event);
            }
        }
        lastError.clear();
        for(const auto& item:initialImages)if(item.first->imageSource==item.second&&
            !item.second.empty()&&item.first->imageComplete){
            JavaScriptRuntime::EventInit event;event.bubbles=false;event.cancelable=false;
            javascript.DispatchNodeEvent(item.first,item.first->image?L"load":L"error",event);
        }
        javascript.SetDocumentReadyState(L"interactive");
        javascript.DispatchDocumentEvent(L"DOMContentLoaded");
        SyncChildFrames(&preparedText,&failedText);
        javascript.SetDocumentReadyState(L"complete");
        javascript.DispatchWindowEvent(L"load");
        layoutDirty=true;InvalidateView();JavaScriptRuntime::Mutation initialMutation;
        initialMutation.kind=JavaScriptRuntime::MutationKind::Tree;initialMutation.targets.push_back(document.Root());
        NotifyAccessibilityMutation(initialMutation,true);
        if(loadHandler)loadHandler(true,L"");return true;
    }
    bool LoadHtml(const std::wstring& html,const std::wstring& base,
                  const std::wstring& location){
        ++navigationGeneration;
        PublishAsyncGenerations();
        return LoadHtmlInternal(&html,{},{},{},{},base,location);
    }
    bool LoadHtmlAsync(const std::wstring& html,const std::wstring& base,
                       const std::wstring& location){
        const auto generation=++navigationGeneration;
        ++resourceGeneration;ResetInitialDocumentLoad();
        pendingRasterImages.clear();ClearPendingImageLoads();
        PublishAsyncGenerations();
        const auto lifetime=asyncLifetime;
        const auto loader=TextLoader(location);
        const bool loadStylesInWorker=parallelResourceLoading;
        const bool scriptingEnabled=pageScriptsEnabled;
        const bool externalPage=base.find(L"://")!=std::wstring::npos;
        ImageWorkers().Submit(BackgroundWorkQueue::Priority::Critical,
            [html,base,location,generation,lifetime,loader,
             loadStylesInWorker,externalPage,scriptingEnabled]{
            {
                std::lock_guard<std::mutex> lock(lifetime->mutex);
                if(!lifetime->alive||!lifetime->hwnd||
                   lifetime->navigationGeneration!=generation)return;
            }
            auto result=std::make_unique<AsyncDocumentResult>();
            result->generation=generation;result->base=base;result->location=location;
            result->document=std::make_unique<Document>();
            result->document->SetScriptingEnabled(scriptingEnabled);
            if(!result->document->Parse(html,&result->error))result->document.reset();
            if(result->document&&loadStylesInWorker){
                const auto loadPrepared=[&](const std::wstring& reference,std::wstring& content){
                    const auto key=ResolveResourceForBase(base,reference);
                    const auto found=result->textResources.find(key);
                    if(found!=result->textResources.end()){content=found->second;return true;}
                    if(result->failedTextResources.count(key)>0)return false;
                    const bool loaded=LoadTextResourceForBase(loader,base,reference,content);
                    if(loaded)result->textResources.emplace(key,content);
                    else result->failedTextResources.insert(key);
                    return loaded;
                };
                const auto css=CollectDocumentStyles(*result->document,base,loadPrepared,!externalPage,result->error);
                if(!result->error.empty())result->document.reset();
                if(result->document){
                    result->styles=std::make_unique<StyleSheet>();
                    if(!result->styles->Parse(css,&result->error)){
                        if(!externalPage)result->document.reset();
                        else{result->styles->Parse(L"",nullptr);result->error.clear();}
                    }
                }
            }
            std::lock_guard<std::mutex> lock(lifetime->mutex);
            if(!lifetime->alive||!lifetime->hwnd||
               lifetime->navigationGeneration!=generation)return;
            if(PostMessageW(lifetime->hwnd,kAsyncDocumentReadyMessage,0,
                            reinterpret_cast<LPARAM>(result.get())))result.release();
        });
        return true;
    }
    void CancelPendingLoads(){
        ++navigationGeneration;++resourceGeneration;
        ResetInitialDocumentLoad();pendingRasterImages.clear();ClearPendingImageLoads();
        PublishAsyncGenerations();
    }
    void CompleteAsyncDocument(std::unique_ptr<AsyncDocumentResult> result){
        if(!result||result->generation!=navigationGeneration)return;
        if(!result->document){
            lastError=result->error.empty()?L"HTML parsing failed":result->error;
            if(loadHandler)loadHandler(false,lastError);
            return;
        }
        LoadHtmlInternal(nullptr,std::move(result->document),std::move(result->styles),
                         std::move(result->textResources),
                         std::move(result->failedTextResources),
                         result->base,result->location,false,true);
    }
};

View::View(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
View::~View(){
    if(impl_&&impl_->ownsStorageSession)impl_->browserContext->ReleaseBrowsingContext(impl_->storageSession);
    auto* compositionOwner=impl_?impl_->compositionParent:nullptr;
    if(compositionOwner)++compositionOwner->childFrameDestructionDepth;
    if(impl_)++impl_->childFrameDestructionDepth;
    if(impl_){impl_->javascript.SetExecutionYieldHandler({});impl_->javascript.SetExecutionCompletionHandler({});}
    if(impl_)impl_->ReleaseDeferredExecutionMessages(false);
    if(impl_&&impl_->asyncLifetime){
        {std::lock_guard<std::mutex> lock(impl_->asyncLifetime->mutex);
            impl_->asyncLifetime->alive=false;impl_->asyncLifetime->hwnd=nullptr;}
        if(impl_->hwnd){MSG message{};while(PeekMessageW(&message,impl_->hwnd,
            kAsyncImageReadyMessage,kAsyncImageReadyMessage,PM_REMOVE))
            delete reinterpret_cast<AsyncImageResult*>(message.lParam);}
        if(impl_->hwnd){MSG message{};while(PeekMessageW(&message,impl_->hwnd,
            kAsyncDocumentReadyMessage,kAsyncDocumentReadyMessage,PM_REMOVE))
            delete reinterpret_cast<AsyncDocumentResult*>(message.lParam);}
        if(impl_->hwnd){MSG message{};while(PeekMessageW(&message,impl_->hwnd,
            kAsyncTextReadyMessage,kAsyncTextReadyMessage,PM_REMOVE))
            delete reinterpret_cast<AsyncTextResult*>(message.lParam);}
        if(impl_->hwnd){MSG message{};while(PeekMessageW(&message,impl_->hwnd,
            kAsyncFrameReadyMessage,kAsyncFrameReadyMessage,PM_REMOVE))
            delete reinterpret_cast<AsyncFrameResult*>(message.lParam);}
        if(impl_->hwnd){MSG message{};while(PeekMessageW(&message,impl_->hwnd,
            kAsyncFrameSourceReadyMessage,kAsyncFrameSourceReadyMessage,PM_REMOVE))
            delete reinterpret_cast<AsyncFrameSourceResult*>(message.lParam);}
    }
    // Disconnect UI Automation before DestroyWindow begins its synchronous
    // message teardown. UiaDisconnectProvider cannot make the required COM
    // call while handling a SendMessage-delivered WM_DESTROY.
    if(impl_&&impl_->accessibility)impl_->accessibility->Disconnect();
    if(impl_&&impl_->hwnd&&IsWindow(impl_->hwnd))DestroyWindow(impl_->hwnd);
    if(impl_)for(const auto& weak:impl_->createdChildViews)if(const auto child=weak.lock()){
        child->impl_->compositionParent=nullptr;child->impl_->topMessageRelay={};
        child->impl_->javascript.SetParentMessageSink({});child->impl_->javascript.SetTopMessageSink({});
        child->impl_->javascript.SetEmbeddingFrame(nullptr,{});
        child->SetMessageHandler({});child->SetNavigationHandler({});
        child->SetResourceLoader({});child->SetNetworkResourceLoader({});child->SetBinaryResourceLoader({});child->SetLoadHandler({});
    }
    // DestroyWindow/accessibility teardown can synchronously paint the parent
    // while its frame vector is erasing this entry. Release the implementation
    // before allowing the parent to traverse the stable frame list again.
    impl_.reset();
    if(compositionOwner){--compositionOwner->childFrameDestructionDepth;compositionOwner->InvalidateView();}
}
std::unique_ptr<View> View::Create(HWND parent,const RECT& bounds){auto impl=std::make_unique<Impl>();if(!impl->Initialize(parent,bounds))return {};return std::unique_ptr<View>(new View(std::move(impl)));}
HWND View::Window()const noexcept{return impl_->hwnd;}
void View::SetBounds(const RECT& bounds){MoveWindow(impl_->hwnd,bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top,TRUE);}
void View::SetVisible(bool visible){if(!visible)impl_->HideTooltip();ShowWindow(impl_->hwnd,visible?SW_SHOW:SW_HIDE);}
void View::SetMessageHandler(MessageHandler handler){impl_->messageHandler=std::move(handler);}
void View::SetCompatibilityBridgeEnabled(bool enabled){
    impl_->javascriptCompatibilityBridgeEnabled=enabled;impl_->javascript.SetCompatibilityBridgeEnabled(enabled);
    for(auto& frame:impl_->childFrames)frame.view->SetCompatibilityBridgeEnabled(enabled);
}
void View::SetLoadHandler(LoadHandler handler){impl_->loadHandler=std::move(handler);}
void View::SetBrowserContext(std::shared_ptr<BrowserContext> context,std::uint64_t session){
    if(!context)return;
    if(impl_->ownsStorageSession)impl_->browserContext->ReleaseBrowsingContext(impl_->storageSession);
    impl_->browserContext=std::move(context);impl_->storageSession=session?session:impl_->browserContext->NewBrowsingContext();
    impl_->ownsStorageSession=session==0;
    impl_->javascript.SetBrowserContext(impl_->browserContext,impl_->storageSession);
    for(auto& frame:impl_->childFrames)frame.view->SetBrowserContext(impl_->browserContext,impl_->storageSession);
}
void View::SetResourceLoader(ResourceLoader loader){impl_->resourceLoader=std::move(loader);}
void View::SetNetworkResourceLoader(NetworkResourceLoader loader){
    impl_->networkResourceLoader=std::move(loader);
    for(auto& frame:impl_->childFrames)frame.view->SetNetworkResourceLoader(impl_->networkResourceLoader);
}
void View::SetBinaryResourceLoader(BinaryResourceLoader loader){impl_->binaryResourceLoader=std::move(loader);}
void View::SetNavigationHandler(NavigationHandler handler){impl_->navigationHandler=std::move(handler);}
void View::SetHistoryChangedHandler(HistoryChangedHandler handler){impl_->historyChangedHandler=std::move(handler);}
void View::SetHistoryTraversalHandler(HistoryTraversalHandler handler){impl_->historyTraversalHandler=std::move(handler);}
bool View::CanTraverseHistory(int delta)const{return impl_->javascript.CanTraverseHistory(delta);}
bool View::TraverseHistory(int delta){return impl_->javascript.TraverseHistory(delta);}
void View::SetParallelResourceLoading(bool enabled){impl_->parallelResourceLoading=enabled;}
void View::SetPageScriptsEnabled(bool enabled){
    impl_->pageScriptsEnabled=enabled;
    impl_->document.SetScriptingEnabled(enabled);
    impl_->javascript.SetInlineEventHandlersEnabled(enabled);
    impl_->layoutDirty=true;impl_->viewportOnlyDirty=false;impl_->InvalidateView();
}
bool View::Navigate(const std::wstring& filePath){auto path=filePath;const auto suffix=path.find_first_of(L"?#");if(suffix!=std::wstring::npos)path.resize(suffix);std::ifstream input(path,std::ios::binary);if(!input){impl_->lastError=L"Cannot open HTML file: "+path;if(impl_->loadHandler)impl_->loadHandler(false,impl_->lastError);return false;}std::ostringstream bytes;bytes<<input.rdbuf();auto slash=path.find_last_of(L"\\/");return impl_->LoadHtml(Utf8ToWide(bytes.str()),slash==std::wstring::npos?L"":path.substr(0,slash),filePath);}
bool View::NavigateToString(const std::wstring& html,const std::wstring& basePath){return impl_->LoadHtml(html,basePath,basePath);}
bool View::NavigateToStringAsync(const std::wstring& html,const std::wstring& basePath){return impl_->LoadHtmlAsync(html,basePath,basePath);}
void View::CancelPendingLoads(){impl_->CancelPendingLoads();}
void View::SetExecutionYieldHandler(ExecutionYieldHandler handler){
    impl_->javascriptExecutionYieldHandler=std::move(handler);
    for(auto& frame:impl_->childFrames)frame.view->SetExecutionYieldHandler(impl_->javascriptExecutionYieldHandler);
}
bool View::ServiceRenderingMessage(const MSG& message){
    if(message.hwnd==impl_->hwnd){
        if(message.message!=WM_PAINT&&(message.message!=WM_TIMER||message.wParam!=kCssTransitionTimer))return false;
        DispatchMessageW(&message);return true;
    }
    for(const auto& frame:impl_->childFrames)
        if(frame.view&&frame.view->ServiceRenderingMessage(message))return true;
    return false;
}
bool View::ExecuteScript(const std::wstring& source,std::wstring* result,std::wstring* error){const bool ok=impl_->javascript.Execute(source,result,error);if(!ok)impl_->lastError=error?*error:L"JavaScript execution failed";impl_->RefreshTextSelectionFromDom();return ok;}
bool View::PostWebMessageAsJson(const std::wstring& json,std::wstring* error){const bool ok=impl_->javascript.DispatchWebMessageAsJson(json,error);if(!ok)impl_->lastError=error?*error:L"Invalid JSON web message";impl_->RefreshTextSelectionFromDom();return ok;}
void View::PostWebMessageAsString(const std::wstring& message){impl_->javascript.DispatchWebMessageAsString(message);impl_->RefreshTextSelectionFromDom();}
std::wstring View::DumpLayoutJson()const{if(impl_->layoutDirty)const_cast<Impl*>(impl_.get())->Rebuild();return impl_->layout.DumpJson();}
std::wstring View::DumpLayoutJson(bool includeChildFrames)const{
    if(!includeChildFrames)return DumpLayoutJson();
    const auto quote=[](const std::wstring& text){
        std::wstring result=L"\"";
        for(const auto ch:text){if(ch==L'\"'||ch==L'\\'){result+=L'\\';result+=ch;}
            else if(ch==L'\n')result+=L"\\n";else if(ch==L'\r')result+=L"\\r";
            else if(ch==L'\t')result+=L"\\t";else if(ch>=32)result+=ch;}
        return result+L'\"';
    };
    const auto body=impl_->document.Body();
    std::wstring shadowText,shadowStyles;
    const std::function<void(const std::shared_ptr<Node>&)> collectShadowText=[&](const std::shared_ptr<Node>& node){
        if(!node||node->tag==L"style"||node->tag==L"script")return;
        if(node->type==NodeType::Text)shadowText+=node->text;
        for(const auto& child:node->RenderChildren())collectShadowText(child);
    };
    if(body&&body->shadowRoot)collectShadowText(body->shadowRoot);
    const std::function<void(const std::shared_ptr<Node>&)> collectShadowStyles=[&](const std::shared_ptr<Node>& node){
        if(!node)return;
        if(node->tag==L"style")shadowStyles+=node->InnerText()+L'\n';
        for(const auto& child:node->RenderChildren())collectShadowStyles(child);
    };
    if(body&&body->shadowRoot)collectShadowStyles(body->shadowRoot);
    if(impl_->layoutDirty)const_cast<Impl*>(impl_.get())->Rebuild();
    std::wstring output=L"{\"document\":"+impl_->layout.DumpJson(true)+
        L",\"bodyChildCount\":"+std::to_wstring(body?body->children.size():0)+
        L",\"bodyShadowChildCount\":"+std::to_wstring(body&&body->shadowRoot?body->shadowRoot->children.size():0)+
        L",\"bodyShadowText\":"+quote(shadowText)+
        L",\"bodyShadowStyles\":"+quote(shadowStyles)+
        L",\"createdErrors\":"+quote(impl_->javascript.CreatedErrorTrace())+
        L",\"runtime\":"+impl_->javascript.DiagnosticsJson()+L",\"frames\":[";bool first=true;
    for(const auto& frame:impl_->childFrames)if(frame.view){
        if(!first)output+=L',';first=false;output+=frame.view->DumpLayoutJson(true);
    }
    return output+L"]}";
}
std::wstring View::DumpAccessibilityJson()const{return const_cast<Impl*>(impl_.get())->DumpAccessibilityJson();}
std::wstring View::LastError()const{
    if(!impl_->lastError.empty())return impl_->lastError;
    const auto error=impl_->javascript.LastError();if(!error.empty())return error;
    for(const auto& frame:impl_->childFrames){
        const auto childError=frame.view->LastError();
        if(!childError.empty())return L"Iframe: "+childError;
    }
    return {};
}
std::wstring View::DocumentTitle()const{const auto title=impl_->document.QuerySelector(L"title");return title?title->InnerText():L"";}

} // namespace TWebFrame

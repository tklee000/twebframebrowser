#include <TWebFrame/TWebFrame.h>

#include "DOM.h"
#include "CSS.h"
#include "JavaScript.h"
#include "Layout.h"
#include "ScriptDialog.h"
#include "TextInput.h"

#include <windows.h>
#include <imm.h>
#include <commctrl.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <thread>
#include <vector>

using namespace TWebFrame::Internal;

namespace {
int failures = 0;
volatile LONG firstChanceCppExceptions = 0;
bool SaveRasterBmp(const std::filesystem::path& path,UINT width,UINT height,const void* pixels){
    if(!pixels||!width||!height)return false;
    BITMAPFILEHEADER fileHeader{};fileHeader.bfType=0x4d42;
    fileHeader.bfOffBits=sizeof(fileHeader)+sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize=fileHeader.bfOffBits+width*height*4;
    BITMAPINFOHEADER header{};header.biSize=sizeof(header);header.biWidth=width;
    header.biHeight=-static_cast<LONG>(height);header.biPlanes=1;header.biBitCount=32;
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&fileHeader),sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&header),sizeof(header));
    output.write(static_cast<const char*>(pixels),static_cast<std::streamsize>(width)*height*4);
    return output.good();
}
struct ScriptDialogProbe {
    HWND window=nullptr;
    unsigned opened=0;
    bool painted=false;
    bool blueButton=false;
    bool clickBlueButton=false;
    COLORREF surface=CLR_INVALID;
    POINT blue{-1,-1};
    bool fallbackUsed=false;
    bool nativeDialog=false;
};
ScriptDialogProbe scriptDialogProbe;
void ObserveScriptDialog(){
    if(!IsWindow(scriptDialogProbe.window))return;
    ++scriptDialogProbe.opened;
    EnumThreadWindows(GetWindowThreadProcessId(scriptDialogProbe.window,nullptr),[](HWND window,LPARAM)->BOOL{
        wchar_t className[32]{};GetClassNameW(window,className,32);
        if(std::wstring(className)==L"#32770")scriptDialogProbe.nativeDialog=true;
        return TRUE;
    },0);
    RECT client{};GetClientRect(scriptDialogProbe.window,&client);
    BITMAPINFO bitmapInfo{};bitmapInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth=client.right;bitmapInfo.bmiHeader.biHeight=-client.bottom;
    bitmapInfo.bmiHeader.biPlanes=1;bitmapInfo.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HDC memory=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(memory,&bitmapInfo,DIB_RGB_COLORS,&pixels,nullptr,0);
    HGDIOBJ previous=bitmap?SelectObject(memory,bitmap):nullptr;
    DWORD_PTR printed=0;
    const bool copied=bitmap&&SendMessageTimeoutW(scriptDialogProbe.window,WM_PRINTCLIENT,
        reinterpret_cast<WPARAM>(memory),PRF_CLIENT,SMTO_ABORTIFHUNG,5000,&printed)!=0;
    GdiFlush();
    if(copied)SaveRasterBmp(std::filesystem::path(L"TWebFrame2/tests/artifacts")/
        (L"script-dialog-native-"+std::to_wstring(scriptDialogProbe.opened)+L".bmp"),
        client.right,client.bottom,pixels);
    auto colorAt=[&](int x,int y)->COLORREF{
        if(!copied||x<0||y<0||x>=client.right||y>=client.bottom)return CLR_INVALID;
        const auto pixel=static_cast<const std::uint32_t*>(pixels)[static_cast<size_t>(y)*client.right+x];
        return RGB((pixel>>16)&255,(pixel>>8)&255,pixel&255);
    };
    const float scale=static_cast<float>(GetDpiForWindow(scriptDialogProbe.window))/96.0f;
    const int left=static_cast<int>(std::lround((client.right/scale-
        std::min(448.0f,std::max(1.0f,client.right/scale-24.0f)))/2.0f*scale));
    const int paintX=left+static_cast<int>(std::lround(5.0f*scale));
    const int paintY=static_cast<int>(std::lround(50.0f*scale));
    const COLORREF surface=colorAt(paintX,paintY);
    scriptDialogProbe.surface=surface;
    scriptDialogProbe.painted=surface!=CLR_INVALID&&GetRValue(surface)>235&&
        GetGValue(surface)>235&&GetBValue(surface)>235;
    POINT blue{-1,-1};
    for(int y=0;y<client.bottom&&blue.x<0;++y)
        for(int x=0;x<client.right;++x){
            const COLORREF color=colorAt(x,y);
            if(color!=CLR_INVALID&&GetRValue(color)>=15&&GetRValue(color)<=40&&
                GetGValue(color)>=85&&GetGValue(color)<=120&&
                GetBValue(color)>=195&&GetBValue(color)<=225){blue={x,y};break;}
        }
    scriptDialogProbe.blueButton=blue.x>=0;
    scriptDialogProbe.blue=blue;
    if(previous)SelectObject(memory,previous);
    if(bitmap)DeleteObject(bitmap);
    if(memory)DeleteDC(memory);
    if(scriptDialogProbe.clickBlueButton&&blue.x>=0){
        const LPARAM point=MAKELPARAM(blue.x+3,blue.y+3);
        PostMessageW(scriptDialogProbe.window,WM_LBUTTONDOWN,MK_LBUTTON,point);
        PostMessageW(scriptDialogProbe.window,WM_LBUTTONUP,0,point);
    }else{
        if(scriptDialogProbe.clickBlueButton)scriptDialogProbe.fallbackUsed=true;
        PostMessageW(scriptDialogProbe.window,WM_KEYDOWN,
            scriptDialogProbe.clickBlueButton?VK_RETURN:VK_ESCAPE,0);
    }
}
LONG CALLBACK CountFirstChanceCppExceptions(EXCEPTION_POINTERS* exception) {
    if(exception&&exception->ExceptionRecord&&
       exception->ExceptionRecord->ExceptionCode==0xe06d7363UL)
        InterlockedIncrement(&firstChanceCppExceptions);
    return EXCEPTION_CONTINUE_SEARCH;
}
LRESULT resizeHostHit=HTCLIENT;
WPARAM resizeHostButtonDown=0;
LRESULT CALLBACK ResizeHostWindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
    if(message==WM_NCHITTEST)return resizeHostHit;
    if(message==WM_NCLBUTTONDOWN){resizeHostButtonDown=wParam;return 0;}
    return DefWindowProcW(window,message,wParam,lParam);
}
struct ConstantHash { std::size_t operator()(int) const { return 1; } };
void CheckAt(bool condition, const wchar_t* message, int line) {
    static std::size_t checkIndex = 0;
    ++checkIndex;
    static const bool traceChecks=[](){wchar_t flag[2]{};return GetEnvironmentVariableW(L"TWEBFRAME_TRACE_CHECKS",flag,2)==1&&flag[0]==L'1';}();
    if(traceChecks)std::wcerr<<L"CHECK["<<checkIndex<<L", line "<<line<<L"] "<<message<<std::endl;
    if (!condition) {
        std::wcerr << L"FAIL[" << checkIndex << L", line " << line << L"]: "
                   << message << L"\n";
        ++failures;
    }
}
#define Check(condition, message) CheckAt((condition), (message), __LINE__)
const LayoutBox* FindLayout(const LayoutBox* box, const std::wstring& id, const std::wstring& className=L"") {
    if (!box) return nullptr;
    if ((!id.empty() && box->node->Attribute(L"id") == id) ||
        (!className.empty() && box->node->HasClass(className))) return box;
    for (const auto& child : box->children) if (const auto* found=FindLayout(child.get(),id,className)) return found;
    return nullptr;
}
const LayoutBox* FindPseudo(const LayoutBox* box, const std::wstring& pseudo) {
    if (!box) return nullptr;
    if (box->pseudo == pseudo) return box;
    for (const auto& child : box->children)
        if (const auto* found=FindPseudo(child.get(),pseudo)) return found;
    return nullptr;
}

struct FrameStats {
    double mean=0;
    double p50=0;
    double p95=0;
    double p99=0;
    double maximum=0;
};

FrameStats SummarizeFrames(std::vector<double> samples) {
    FrameStats result;
    if(samples.empty())return result;
    std::sort(samples.begin(),samples.end());
    const auto percentile=[&](double value){
        const size_t index=std::min(samples.size()-1,
            static_cast<size_t>(std::ceil(value*static_cast<double>(samples.size())))-1);
        return samples[index];
    };
    result.mean=std::accumulate(samples.begin(),samples.end(),0.0)/samples.size();
    result.p50=percentile(0.50);result.p95=percentile(0.95);result.p99=percentile(0.99);
    result.maximum=samples.back();return result;
}

bool MeasureFrames(HWND window,size_t sampleCount,
                   const std::function<bool(size_t)>& action,FrameStats& result) {
    using Clock=std::chrono::steady_clock;
    constexpr size_t warmupCount=8;
    for(size_t index=0;index<warmupCount;++index){
        if(!action(index))return false;
        UpdateWindow(window);
    }
    std::vector<double> samples;samples.reserve(sampleCount);
    for(size_t index=0;index<sampleCount;++index){
        const auto start=Clock::now();
        if(!action(index+warmupCount))return false;
        UpdateWindow(window);
        samples.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
    }
    result=SummarizeFrames(std::move(samples));return true;
}

int RunFrameBenchmark() {
    using Clock=std::chrono::steady_clock;
    constexpr size_t nodeCount=10000;
    constexpr size_t sampleCount=120;
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP|WS_VISIBLE,
        -10000,-10000,1280,720,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!host){std::wcerr<<L"benchmark host creation failed\n";return 1;}
    RECT bounds{0,0,1280,720};auto view=TWebFrame::View::Create(host,bounds);
    if(!view){DestroyWindow(host);std::wcerr<<L"benchmark view creation failed\n";return 1;}
    std::wstring html;
    html.reserve(nodeCount*72);
    html+=LR"HTML(<style>*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;font:14px Segoe UI}#viewport{height:680px;overflow:auto}.item{height:22px;padding:2px 8px;border-bottom:1px solid #eee;color:#222}.item:hover,.item.hot{color:#b91c1c;background:#fef2f2}.hidden{display:none}</style><div id="viewport">)HTML";
    for(size_t index=0;index<nodeCount;++index)
        html+=L"<div class='item' id='row-"+std::to_wstring(index)+L"'>Row "+
            std::to_wstring(index)+L"</div>";
    html+=L"</div><div id='benchmark-menu' class='hidden'></div>";
    const auto loadStart=Clock::now();
    if(!view->NavigateToString(html)){std::wcerr<<view->LastError()<<L'\n';view.reset();DestroyWindow(host);return 1;}
    UpdateWindow(view->Window());
    const double initialLoad=std::chrono::duration<double,std::milli>(Clock::now()-loadStart).count();

    FrameStats hover,style,scroll,scrollNoopClass,scrollHeavySibling,scrollNoopStyle,
        scrollChangingStyle;
    const bool hoverOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const int y=11+static_cast<int>(index%28)*22;
        SendMessageW(view->Window(),WM_MOUSEMOVE,0,MAKELPARAM(80,y));return true;
    },hover);
    SendMessageW(view->Window(),WM_MOUSELEAVE,0,0);UpdateWindow(view->Window());
    std::wstring error;
    const bool styleOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const auto id=std::to_wstring(index%nodeCount);
        return view->ExecuteScript(L"document.getElementById('row-"+id+L"').classList.toggle('hot');",
                                   nullptr,&error);
    },style);
    view->ExecuteScript(L"document.getElementById('viewport').scrollTop=0;",nullptr,&error);
    UpdateWindow(view->Window());
    POINT wheelPoint{100,100};ClientToScreen(view->Window(),&wheelPoint);
    const bool scrollOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const short delta=index%2?WHEEL_DELTA:-WHEEL_DELTA;
        SendMessageW(view->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,delta),
            MAKELPARAM(wheelPoint.x,wheelPoint.y));return true;
    },scroll);
    const bool scrollListenerOk=view->ExecuteScript(
        L"document.getElementById('viewport').addEventListener('scroll',()=>document.getElementById('benchmark-menu').classList.add('hidden'));",
        nullptr,&error);
    view->ExecuteScript(L"document.getElementById('viewport').scrollTop=0;",nullptr,&error);
    UpdateWindow(view->Window());
    const bool scrollNoopClassOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const short delta=index%2?WHEEL_DELTA:-WHEEL_DELTA;
        SendMessageW(view->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,delta),
            MAKELPARAM(wheelPoint.x,wheelPoint.y));return true;
    },scrollNoopClass);
    std::wstring splitPaneHtml=LR"HTML(<style>*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;font:12px Segoe UI}#history{height:300px;overflow:auto}.commit{height:22px;padding:2px 8px;border-bottom:1px solid #ddd}#detail{height:380px;overflow:hidden;border-top:1px solid #bbb}#diff{height:100%;overflow:auto;font:11px Consolas}.diff-line{display:grid;grid-template-columns:46px 46px minmax(0,1fr);height:17px}.diff-line span{overflow:hidden;white-space:pre}</style><div id='history'>)HTML";
    for(size_t index=0;index<240;++index)
        splitPaneHtml+=L"<div class='commit'>Commit "+std::to_wstring(index)+L"</div>";
    splitPaneHtml+=L"</div><div id='detail'><div id='diff'>";
    for(size_t index=0;index<5000;++index)
        splitPaneHtml+=L"<div class='diff-line'><span>"+std::to_wstring(index)+
            L"</span><span>"+std::to_wstring(index+1)+
            L"</span><span>Localization payload line "+std::to_wstring(index)+L"</span></div>";
    splitPaneHtml+=L"</div></div>";
    const bool splitPaneOk=view->NavigateToString(splitPaneHtml);
    UpdateWindow(view->Window());
    const bool scrollHeavySiblingOk=splitPaneOk&&MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const short delta=index%2?WHEEL_DELTA:-WHEEL_DELTA;
        SendMessageW(view->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,delta),
            MAKELPARAM(wheelPoint.x,wheelPoint.y));return true;
    },scrollHeavySibling);

    // Large production stylesheets commonly contain a sibling combinator or
    // :has(), which deliberately puts real style changes on the conservative
    // whole-document restyle path. Reassigning an already-identical inline
    // declaration from a scroll listener is not a style change and must not
    // make that cost proportional to a dynamically appended list.
    std::wstring commentLikeHtml=LR"HTML(<style>
        *{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;font:12px Segoe UI}
        #viewport{height:680px;overflow:auto}.comment{min-height:24px;border-bottom:1px solid #ddd}
        .comment + .comment{color:#222}#navigator{position:fixed;top:8px;right:8px}
        </style><div id='viewport'>)HTML";
    for(size_t index=0;index<3000;++index)
        commentLikeHtml+=L"<div class='comment'><span>Comment "+std::to_wstring(index)+
            L"</span><span> dynamically inserted reply text</span></div>";
    commentLikeHtml+=L"</div><div id='navigator'>navigation</div><script>"
        L"const viewport=document.getElementById('viewport'),navigator=document.getElementById('navigator');"
        L"viewport.addEventListener('scroll',()=>{navigator.style.position='fixed';navigator.style.top='8px';});"
        L"</script>";
    const bool commentLikeOk=view->NavigateToString(commentLikeHtml);
    UpdateWindow(view->Window());
    const bool scrollNoopStyleOk=commentLikeOk&&MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const short delta=index%2?WHEEL_DELTA:-WHEEL_DELTA;
        SendMessageW(view->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,delta),
            MAKELPARAM(wheelPoint.x,wheelPoint.y));return true;
    },scrollNoopStyle);
    const bool changingStyleListenerOk=view->ExecuteScript(
        L"viewport.addEventListener('scroll',()=>{navigator.style.top=(8+viewport.scrollTop)+'px';});",
        nullptr,&error);
    const bool scrollChangingStyleOk=changingStyleListenerOk&&
        MeasureFrames(view->Window(),sampleCount,[&](size_t index){
            const short delta=index%2?WHEEL_DELTA:-WHEEL_DELTA;
            SendMessageW(view->Window(),WM_MOUSEWHEEL,MAKEWPARAM(0,delta),
                MAKELPARAM(wheelPoint.x,wheelPoint.y));return true;
        },scrollChangingStyle);

    std::wcout<<std::fixed<<std::setprecision(3)
        <<L"TWebFrame 10k-node frame benchmark (milliseconds)\n"
        <<L"initial_load_and_paint="<<initialLoad<<L"\n"
        <<L"scenario,samples,mean,p50,p95,p99,max\n";
    const auto print=[&](const wchar_t* name,const FrameStats& value){
        std::wcout<<name<<L','<<sampleCount<<L','<<value.mean<<L','<<value.p50<<L','
                  <<value.p95<<L','<<value.p99<<L','<<value.maximum<<L'\n';
    };
    print(L"hover",hover);print(L"style_toggle",style);print(L"wheel_scroll",scroll);
    print(L"wheel_scroll_noop_class",scrollNoopClass);
    print(L"wheel_scroll_heavy_sibling",scrollHeavySibling);
    print(L"wheel_scroll_noop_style_large_dom",scrollNoopStyle);
    print(L"wheel_scroll_changing_inline_style_large_dom",scrollChangingStyle);
    view.reset();DestroyWindow(host);
    if(!hoverOk||!styleOk||!scrollOk||!scrollListenerOk||!scrollNoopClassOk||
       !splitPaneOk||!scrollHeavySiblingOk||!commentLikeOk||!scrollNoopStyleOk||
       !changingStyleListenerOk||!scrollChangingStyleOk){
        std::wcerr<<L"benchmark action failed: "<<error<<L'\n';return 1;
    }
    return 0;
}

int RunEditingBenchmark() {
    constexpr size_t sampleCount=120;
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP|WS_VISIBLE,
        -10000,-10000,1000,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!host){std::wcerr<<L"editing benchmark host creation failed\n";return 1;}
    RECT bounds{0,0,1000,700};auto view=TWebFrame::View::Create(host,bounds);
    if(!view){DestroyWindow(host);std::wcerr<<L"editing benchmark view creation failed\n";return 1;}

    std::wstring html=LR"HTML(<style>
        *{box-sizing:border-box}html,body{margin:0;width:100%;height:100%;font:14px Segoe UI}
        #toolbar{height:48px;display:flex;gap:4px;padding:8px;border-bottom:1px solid #ddd}
        button{width:28px;height:28px}.active{background:#dbeafe}
        #editor{height:calc(100% - 48px);padding:20px;overflow:auto;white-space:pre-wrap}
    </style><div id="toolbar">)HTML";
    for(size_t index=0;index<32;++index)
        html+=L"<button data-command='command-"+std::to_wstring(index)+L"'>"+
              std::to_wstring(index%10)+L"</button>";
    html+=LR"HTML(</div><article id="editor" contenteditable="true"></article><script>
        const editor=document.getElementById('editor');
        const history=[];
        let currentText='';
        function inlineText(node){
            if(node.nodeType===3)return node.nodeValue||'';
            if(node.nodeType!==1)return '';
            return [...node.childNodes].map(inlineText).join('');
        }
        function serialize(){
            return [...editor.childNodes]
                .map((node)=>inlineText(node).trim())
                .filter((block)=>block!=='')
                .join('\n\n').replace(/\n{3,}/g,'\n\n');
        }
        function updateChrome(){
            document.querySelectorAll('[data-command]').forEach((button,index)=>{
                button.disabled=history.length===0;
                button.setAttribute('aria-pressed',String(index===0&&currentText.length>0));
            });
        }
        editor.addEventListener('beforeinput',()=>{
            history.push({text:serialize(),html:editor.innerHTML});
        });
        editor.addEventListener('input',()=>{
            editor.querySelectorAll('span[style]').forEach((span)=>{
                if(span.style.length===1&&span.style.fontSize)span.replaceWith(...span.childNodes);
            });
            editor.querySelectorAll('li > li').forEach(()=>{});
            editor.normalize();
            currentText=serialize();
            updateChrome();
        });
    </script>)HTML";
    if(!view->NavigateToString(html)){
        std::wcerr<<view->LastError()<<L'\n';view.reset();DestroyWindow(host);return 1;
    }
    UpdateWindow(view->Window());std::wstring error;
    if(!view->ExecuteScript(L"document.getElementById('editor').focus();",nullptr,&error)){
        std::wcerr<<error<<L'\n';view.reset();DestroyWindow(host);return 1;
    }
    UpdateWindow(view->Window());

    FrameStats typing;
    const bool typingOk=MeasureFrames(view->Window(),sampleCount,[&](size_t index){
        const wchar_t character=L'a'+static_cast<wchar_t>(index%26);
        SendMessageW(view->Window(),WM_CHAR,character,1);return true;
    },typing);
    std::wstring result;
    const bool resultOk=view->ExecuteScript(
        L"return document.getElementById('editor').textContent.length;",&result,&error);
    std::wcout<<std::fixed<<std::setprecision(3)
        <<L"TWebFrame contenteditable benchmark (milliseconds)\n"
        <<L"scenario,samples,mean,p50,p95,p99,max\n"
        <<L"contenteditable_input,"<<sampleCount<<L','<<typing.mean<<L','<<typing.p50<<L','
        <<typing.p95<<L','<<typing.p99<<L','<<typing.maximum<<L'\n';
    view.reset();DestroyWindow(host);
    if(!typingOk||!resultOk||result!=std::to_wstring(sampleCount+8)){
        std::wcerr<<L"editing benchmark failed: "<<error<<L" result="<<result<<L'\n';return 1;
    }
    return 0;
}

struct TextRasterSample {
    bool rendered=false;
    bool grayscaleMode=false;
    bool webRenderingParams=false;
    size_t grayscaleEdgePixels=0;
    size_t colorFringePixels=0;
};

TextRasterSample CaptureTextRaster(LayoutEngine& layout,float scale,bool transparent=false) {
    TextRasterSample sample;
    const UINT width=static_cast<UINT>(std::lround(240.0f*scale));
    const UINT height=static_cast<UINT>(std::lround(80.0f*scale));
    BITMAPINFO bitmapInfo{};bitmapInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth=static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight=-static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes=1;bitmapInfo.bmiHeader.biBitCount=32;
    bitmapInfo.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HDC memory=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(memory,&bitmapInfo,DIB_RGB_COLORS,&pixels,nullptr,0);
    HGDIOBJ previous=bitmap?SelectObject(memory,bitmap):nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory;
    const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            transparent?D2D1_ALPHA_MODE_PREMULTIPLIED:D2D1_ALPHA_MODE_IGNORE));
    RECT bounds{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    D2D1_FACTORY_OPTIONS factoryOptions{};
    const bool ready=memory&&bitmap&&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),&factoryOptions,
            reinterpret_cast<void**>(d2dFactory.ReleaseAndGetAddressOf())))&&
        SUCCEEDED(d2dFactory->CreateDCRenderTarget(&properties,&target))&&
        SUCCEEDED(target->BindDC(memory,&bounds))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf())));
    if(ready){
        layout.DiscardDeviceResources();layout.Layout(240,80,scale);
        target->SetDpi(USER_DEFAULT_SCREEN_DPI*scale,USER_DEFAULT_SCREEN_DPI*scale);
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::White));
        layout.Paint(target.Get(),writeFactory.Get());
        sample.rendered=SUCCEEDED(target->EndDraw());
        sample.grayscaleMode=target->GetTextAntialiasMode()==D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
        Microsoft::WRL::ComPtr<IDWriteRenderingParams> renderingParams;
        target->GetTextRenderingParams(&renderingParams);
        Microsoft::WRL::ComPtr<IDWriteRenderingParams1> renderingParams1;
        if(renderingParams)renderingParams.As(&renderingParams1);
        sample.webRenderingParams=renderingParams&&
            std::abs(renderingParams->GetEnhancedContrast())<0.001f&&
            std::abs(renderingParams->GetClearTypeLevel()-(transparent?0.0f:1.0f))<0.001f&&
            renderingParams->GetPixelGeometry()==(transparent?DWRITE_PIXEL_GEOMETRY_FLAT:DWRITE_PIXEL_GEOMETRY_RGB)&&
            renderingParams->GetRenderingMode()==DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC&&
            renderingParams1&&
            std::abs(renderingParams1->GetGrayscaleEnhancedContrast())<0.001f;
        GdiFlush();
        const auto* values=static_cast<const std::uint32_t*>(pixels);
        for(size_t index=0;index<static_cast<size_t>(width)*height;++index){
            const auto pixel=values[index];
            const auto blue=static_cast<unsigned char>(pixel);
            const auto green=static_cast<unsigned char>(pixel>>8);
            const auto red=static_cast<unsigned char>(pixel>>16);
            if(red!=green||green!=blue)++sample.colorFringePixels;
            else if(red>0&&red<255)++sample.grayscaleEdgePixels;
        }
    }
    layout.DiscardDeviceResources();
    if(previous)SelectObject(memory,previous);
    if(bitmap)DeleteObject(bitmap);
    if(memory)DeleteDC(memory);
    return sample;
}

struct BoxRasterSample {
    bool rendered=false;
    LONG left=std::numeric_limits<LONG>::max();
    LONG top=std::numeric_limits<LONG>::max();
    LONG right=std::numeric_limits<LONG>::min();
    LONG bottom=std::numeric_limits<LONG>::min();
    std::uint32_t topEdge=0;
    std::uint32_t belowTopEdge=0;
    std::uint32_t aboveBottomEdge=0;
    std::uint32_t bottomEdge=0;
    UINT width=0;
    std::vector<std::uint32_t> pixels;
    std::uint32_t ColorAt(float x,float y,float scale)const{
        const auto column=static_cast<UINT>(std::floor(x*scale));
        const auto row=static_cast<UINT>(std::floor(y*scale));
        const size_t offset=static_cast<size_t>(row)*width+column;
        return column<width&&offset<pixels.size()?pixels[offset]&0x00ffffffu:0xffffffffu;
    }
};

BoxRasterSample CaptureBoxRaster(LayoutEngine& layout,float scale,float cssWidth=80,float cssHeight=50) {
    BoxRasterSample sample;
    const UINT width=static_cast<UINT>(std::lround(cssWidth*scale));
    const UINT height=static_cast<UINT>(std::lround(cssHeight*scale));
    BITMAPINFO bitmapInfo{};bitmapInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth=static_cast<LONG>(width);
    bitmapInfo.bmiHeader.biHeight=-static_cast<LONG>(height);
    bitmapInfo.bmiHeader.biPlanes=1;bitmapInfo.bmiHeader.biBitCount=32;
    bitmapInfo.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;
    HDC memory=CreateCompatibleDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(memory,&bitmapInfo,DIB_RGB_COLORS,&pixels,nullptr,0);
    HGDIOBJ previous=bitmap?SelectObject(memory,bitmap):nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory;
    const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
    RECT bounds{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    D2D1_FACTORY_OPTIONS factoryOptions{};
    const bool ready=memory&&bitmap&&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),&factoryOptions,
            reinterpret_cast<void**>(d2dFactory.ReleaseAndGetAddressOf())))&&
        SUCCEEDED(d2dFactory->CreateDCRenderTarget(&properties,&target))&&
        SUCCEEDED(target->BindDC(memory,&bounds))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf())));
    if(ready){
        layout.DiscardDeviceResources();layout.Layout(cssWidth,cssHeight,scale);
        target->SetDpi(USER_DEFAULT_SCREEN_DPI*scale,USER_DEFAULT_SCREEN_DPI*scale);
        target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::White));
        layout.Paint(target.Get(),writeFactory.Get());
        sample.rendered=SUCCEEDED(target->EndDraw());GdiFlush();
        const auto* values=static_cast<const std::uint32_t*>(pixels);
        sample.width=width;sample.pixels.assign(values,values+static_cast<size_t>(width)*height);
        for(LONG y=0;y<static_cast<LONG>(height);++y)
            for(LONG x=0;x<static_cast<LONG>(width);++x)
                if((values[static_cast<size_t>(y)*width+x]&0x00ffffffu)!=0x00ffffffu){
                    sample.left=std::min(sample.left,x);sample.top=std::min(sample.top,y);
                    sample.right=std::max(sample.right,x);sample.bottom=std::max(sample.bottom,y);
                }
        if(sample.left<=sample.right&&sample.top<=sample.bottom){
            const LONG centerX=(sample.left+sample.right)/2;
            const auto pixel=[&](LONG y){
                return values[static_cast<size_t>(y)*width+centerX]&0x00ffffffu;
            };
            sample.topEdge=pixel(sample.top);
            sample.belowTopEdge=pixel(std::min(sample.top+1,sample.bottom));
            sample.aboveBottomEdge=pixel(std::max(sample.top,sample.bottom-1));
            sample.bottomEdge=pixel(sample.bottom);
        }
    }
    layout.DiscardDeviceResources();
    if(previous)SelectObject(memory,previous);
    if(bitmap)DeleteObject(bitmap);
    if(memory)DeleteDC(memory);
    return sample;
}

int RunJQueryCompatibility(const std::wstring& sourcePath) {
    std::ifstream input(std::filesystem::path(sourcePath),std::ios::binary);
    if(!input){std::wcerr<<L"Cannot open jQuery source: "<<sourcePath<<L'\n';return 2;}
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),
                                         static_cast<int>(bytes.size()),nullptr,0);
    if(length<=0){std::wcerr<<L"jQuery source is not valid UTF-8\n";return 2;}
    std::wstring source(static_cast<size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),
                        static_cast<int>(bytes.size()),source.data(),length);

    Document document;std::wstring error;
    if(!document.Parse(
        L"<!doctype html><html><head></head><body>"
        L"<div id='fixture' class='item'><span class='label'>alpha</span>"
        L"<ul id='list'><li class='first'>one</li><li class='second'>two</li></ul></div>"
        L"<div class='item secondary'></div>"
        L"<form id='form'><input name='query' value='hello world'>"
        L"<input type='checkbox' name='enabled' value='yes' checked></form>"
        L"</body></html>",&error)){
        std::wcerr<<L"jQuery fixture parse failed: "<<error<<L'\n';return 2;
    }
    JavaScriptRuntime runtime(document);
    runtime.SetLocation(L"https://jquery.test/index.html");
    runtime.SetResourceLoader([](const std::wstring& resource,std::wstring& body){
        if(resource!=L"payload.json")return false;
        body=L"{\"value\":9}";return true;
    });
    const auto jqueryExceptionHandler=AddVectoredExceptionHandler(1,CountFirstChanceCppExceptions);
    struct NativeExceptionObserverScope {
        PVOID handler;
        ~NativeExceptionObserverScope(){if(handler)RemoveVectoredExceptionHandler(handler);}
    } jqueryExceptionObserver{jqueryExceptionHandler};
    InterlockedExchange(&firstChanceCppExceptions,0);
    if(!runtime.Execute(source,nullptr,&error)){
        std::wcerr<<L"jQuery 1.11.2 initialization failed: "<<error<<L'\n';return 1;
    }
    std::wstring result;
    if(!runtime.Execute(LR"JS(
        function check(value,label){if(!value)throw new Error(label);}
        check(jQuery.fn.jquery==='1.11.2','version');
        check(jQuery.find.support.getElementsByClassName&&jQuery.find.support.qsa&&
              jQuery.find.support.matchesSelector,'native selector feature detection');
        check(jQuery('.item').length===2,'class selector');
        const target=jQuery('#fixture');
        check(target.find('li').length===2&&target.find('li').first().text()==='one',
              'find/first/text');
        check(target.find('li').last().is('.second'),'last/is');
        check(target.find('.second').closest('#fixture').length===1,'closest');
        check(target.children('#list').length===1&&jQuery('#list').children().length===2,
              'children');
        check(jQuery('.first').next().hasClass('second')&&
              jQuery('.second').prev().hasClass('first'),'next/prev');

        target.addClass('active').attr('data-ready','yes').css('color','red')
              .append('<b class="added">beta</b>').prepend('<i class="prefix">zero</i>');
        check(target.hasClass('active')&&target.attr('data-ready')==='yes','class/attr');
        check(target.find('.added').text()==='beta'&&target.find('.prefix').text()==='zero',
              'append/prepend');
        target.toggleClass('active').removeAttr('data-ready');
        check(!target.hasClass('active')&&target.attr('data-ready')===undefined,
              'toggleClass/removeAttr');
        target.data('count',3);
        check(target.data('count')===3,'data');
        target.hide();check(target.css('display')==='none','hide');
        target.show();check(target.css('display')!=='none','show');

        let clickCount=0,delegated=0,oneCount=0;
        const firstNode=target.find('.first')[0];
        const seedMatch=jQuery.find('li',target[0],null,[firstNode]).length;
        const directMatch=jQuery.find.matches('li',[firstNode]).length;
        const selectorMatch=jQuery.find.matchesSelector(firstNode,'li');
        check(seedMatch===1&&directMatch===1&&selectorMatch,
              'Sizzle seed '+seedMatch+'|'+directMatch+'|'+selectorMatch+'|'
              +firstNode.nodeType+'|'+firstNode.nodeName);
        target.on('click.test',function(){clickCount++;});
        target.on('click.test','li',function(){delegated++;});
        target.one('custom',function(){oneCount++;});
        target.trigger('click');
        target.find('.first').trigger('click');
        target.trigger('custom').trigger('custom');
        target.off('.test');target.trigger('click');
        check(clickCount===2&&delegated===1&&oneCount===1,
              'events '+clickCount+'|'+delegated+'|'+oneCount);

        const copied=jQuery.extend(true,{}, {nested:{a:1}}, {nested:{b:2}});
        check(copied.nested.a===1&&copied.nested.b===2,'extend');
        check(jQuery.map([1,2,3],function(value){return value*2;}).join(',')==='2,4,6',
              'map');
        check(jQuery.grep([1,2,3],function(value){return value>1;}).join(',')==='2,3',
              'grep');
        const callbacks=jQuery.Callbacks('memory');let callbackValue=0;
        callbacks.add(function(value){callbackValue+=value;}).fire(2);
        callbacks.add(function(value){callbackValue+=value;});
        check(callbackValue===4,'Callbacks memory');
        let deferredValue=0;
        jQuery.Deferred().resolve(5).then(function(value){deferredValue=value;});
        check(deferredValue===5,'Deferred');
        let queueValue='';
        target.queue(function(next){queueValue+='a';next();})
              .queue(function(next){queueValue+='b';next();});
        check(queueValue==='ab','queue');
        check(typeof XMLHttpRequest==='function','XMLHttpRequest constructor');
        const manualRequest=new XMLHttpRequest();
        manualRequest.open('GET','payload.json',false);manualRequest.send();
        check(manualRequest.status===200&&manualRequest.responseText==='{"value":9}',
              'XMLHttpRequest '+manualRequest.status+'|'+manualRequest.responseText);
        check(jQuery.support.ajax,'jQuery AJAX support');
        let ajaxValue=0,readyCount=0;
        jQuery(function(){readyCount++;});
        jQuery.getJSON('payload.json',function(data){ajaxValue=data.value;});

        const serialized=jQuery('#form').serialize();
        check(serialized==='query=hello+world&enabled=yes','serialize '+serialized);
        jQuery('#form input[name=query]').val('updated');
        check(jQuery('#form input[name=query]').val()==='updated','val');
        const clone=target.clone(true);clone.attr('id','clone').appendTo('body');
        check(jQuery('#clone').length===1&&jQuery('#clone .added').length===1,'clone/appendTo');
        jQuery('#clone').remove();check(jQuery('#clone').length===0,'remove');
        return 'jquery-complete';
    )JS",&result,&error)){
        std::wcerr<<L"jQuery smoke operations failed: "<<error<<L'\n';return 1;
    }
    if(result!=L"jquery-complete"){
        std::wcerr<<L"jQuery smoke result mismatch: "<<result<<L'\n';return 1;
    }
    // Ready and XHR completion are separate timer jobs. Drive the event loop
    // through both callbacks instead of assuming one timer tick drains all jobs.
    bool completed=false;
    for(unsigned tick=0;tick<16&&!completed;++tick){
        runtime.RunTimers();
        if(!runtime.Execute(L"return ajaxValue+'|'+readyCount;",&result,&error))break;
        completed=result==L"9|1";
    }
    if(!completed){
        std::wcerr<<L"jQuery async/ready completion failed: "<<error<<L" result="<<result<<L'\n';
        return 1;
    }
    if(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)!=0){
        std::wcerr<<L"jQuery compatibility raised native C++ exceptions\n";return 1;
    }
    std::wcout<<L"jQuery 1.11.2 full source parsed, initialized, and passed the compatibility suite\n";
    return 0;
}

int RunJavaScriptFiles(int count,wchar_t** paths,bool iframeContext=false) {
    Document document;std::wstring error;
    if(!document.Parse(L"<html><head></head><body></body></html>",&error)){
        std::wcerr<<L"diagnostic document parse failed: "<<error<<L'\n';return 1;
    }
    JavaScriptRuntime runtime(document);
    if(iframeContext){
        runtime.SetParentMessageSink([](const std::wstring&,const std::wstring&){
            std::wcout<<L"Frame posted a message to its parent\n";
        });
        runtime.Clear();
        runtime.SetDocumentReadyState(L"loading");
    }
    runtime.SetLocation(L"https://example.test/index.html?ad_test=1");
    runtime.SetViewportSize(1280,720);
    std::vector<std::filesystem::path> searchDirectories;
    for(int index=0;index<count;++index)
        searchDirectories.push_back(std::filesystem::absolute(paths[index]).parent_path());
    runtime.SetResourceLoader([searchDirectories](const std::wstring& resource,std::wstring& body){
        auto clean=resource;const auto suffix=clean.find_first_of(L"?#");
        if(suffix!=std::wstring::npos)clean.resize(suffix);
        const auto slash=clean.find_last_of(L"/\\");
        const auto filename=clean.substr(slash==std::wstring::npos?0:slash+1);
        for(const auto& directory:searchDirectories){
            std::ifstream input(directory/filename,std::ios::binary);if(!input)continue;
            const std::string bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
            const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),
                                                  static_cast<int>(bytes.size()),nullptr,0);
            if(length<=0)return false;body.assign(static_cast<size_t>(length),L'\0');
            MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),
                                static_cast<int>(bytes.size()),body.data(),length);return true;
        }
        return false;
    });
    std::wstring previousErrorTrace;
    for(int index=0;index<count;++index){
        std::ifstream input(std::filesystem::path(paths[index]),std::ios::binary);
        if(!input){std::wcerr<<L"cannot open JavaScript file: "<<paths[index]<<L'\n';return 1;}
        const std::string bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),
                                       static_cast<int>(bytes.size()),nullptr,0);
        if(length<=0){std::wcerr<<L"invalid UTF-8 JavaScript file: "<<paths[index]<<L'\n';return 1;}
        std::wstring source(static_cast<size_t>(length),L'\0');
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),
                            static_cast<int>(bytes.size()),source.data(),length);
        std::wstring result;
        if(!runtime.Execute(source,&result,&error)){
            std::wcerr<<L"JavaScript file "<<index+1<<L" failed: "<<error<<L'\n'
                      <<runtime.LastCreatedError()<<L'\n';return 1;
        }
        if(iframeContext&&index==0){
            runtime.SetDocumentReadyState(L"interactive");runtime.DispatchDocumentEvent(L"DOMContentLoaded");
            runtime.SetDocumentReadyState(L"complete");runtime.DispatchWindowEvent(L"load");
        }
        runtime.RunTimers();
        std::wcout<<L"JavaScript file "<<index+1<<L" result: "<<result;
        const auto createdError=runtime.LastCreatedError();
        if(!createdError.empty())std::wcout<<L" last-error: "<<createdError;
        std::wcout<<L'\n';
        const auto errorTrace=runtime.CreatedErrorTrace();
        if(errorTrace.size()>previousErrorTrace.size())
            std::wcout<<L"new errors:\n"<<errorTrace.substr(previousErrorTrace.size())<<L'\n';
        previousErrorTrace=errorTrace;
    }
    return 0;
}
}

int RunJavaScriptExceptionRegression(){
    const int initialFailures=failures;
    const auto handler=AddVectoredExceptionHandler(1,CountFirstChanceCppExceptions);
    Check(handler!=nullptr,L"JavaScript exception regression installs its native exception observer");
    InterlockedExchange(&firstChanceCppExceptions,0);
    {
        Document document;document.Parse(L"<!doctype html><body></body>");
        JavaScriptRuntime runtime(document);std::wstring result,error;
        Check(runtime.Execute(LR"JS(
            var thrownValue={message:'caught'};var trace='';
            function raise(){throw thrownValue;}
            function middle(){try{raise();}finally{trace+='finally|';}}
            function outer(){return middle();}
            try{outer();}catch(error){trace+=String(error===thrownValue)+'|';}
            try{throw 'local';}catch(error){trace+=error+'|';}
            function defaultThrow(value=raise()){trace+='bad';}
            try{defaultThrow();}catch(error){trace+=String(error===thrownValue)+'|';}
            function ConstructorThrow(){throw thrownValue;}
            try{new ConstructorThrow();}catch(error){trace+=String(error===thrownValue);}
            try{raise.call(null);}catch(error){trace+='|'+String(error===thrownValue);}
            try{raise.apply(null,[]);}catch(error){trace+='|'+String(error===thrownValue);}
            try{raise.bind(null)();}catch(error){trace+='|'+String(error===thrownValue);}
            return trace;
        )JS",&result,&error)&&result==L"finally|true|local|true|true|true|true|true",
            L"JavaScript throws preserve identity through calls, finally, defaults and constructors");
        Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
              L"handled JavaScript throws do not raise first-chance native C++ exceptions");

        InterlockedExchange(&firstChanceCppExceptions,0);
        Check(runtime.Execute(LR"JS(
            var mathMarker={},mathIdentity=false,mathTypeError='',canvasSizeError='';
            try{Math.sqrt({valueOf:function(){throw mathMarker;}});}catch(error){mathIdentity=error===mathMarker;}
            try{Math.imul(1n,1);}catch(error){mathTypeError=error.name;}
            var exceptionCanvas=document.createElement('canvas').getContext('2d');
            try{exceptionCanvas.getImageData(0,0,0,1);}catch(error){canvasSizeError=error.name;}
            return mathIdentity+'|'+mathTypeError+'|'+canvasSizeError;
        )JS",&result,&error)&&result==L"true|TypeError|IndexSizeError"&&
              InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
              L"Math conversion and Canvas validation preserve JavaScript errors without native C++ first-chance exceptions");

        InterlockedExchange(&firstChanceCppExceptions,0);
        Check(runtime.Execute(LR"JS(
            var asyncTrace='';
            async function asyncThrow(){try{await Promise.reject('rejected');}finally{asyncTrace+='finally';}}
            asyncThrow().catch(function(error){asyncTrace+='|'+error;});
        )JS",nullptr,&error)&&runtime.Execute(L"return asyncTrace;",&result,&error)&&result==L"finally|rejected",
            L"async rejection runs finally and reaches the JavaScript promise handler");
        Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
              L"async rejection propagation does not raise first-chance native C++ exceptions");
        Check(runtime.Execute(LR"JS(
            var finallyTrace='';
            Promise.resolve().finally(function(){throw 'finally-error';})
                .catch(function(error){finallyTrace=error;});
        )JS",nullptr,&error)&&runtime.Execute(L"return finallyTrace;",&result,&error)&&result==L"finally-error"&&
            InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
            L"Promise finally propagates callback failures without native exceptions");

        InterlockedExchange(&firstChanceCppExceptions,0);
        Check(runtime.Execute(LR"JS(
            function invalidNative(){document.body.dispatchEvent(null);}
            function nativeMiddle(){return invalidNative();}
            function nativeOuter(){return nativeMiddle();}
            try{nativeOuter();}catch(error){return error.name;}
        )JS",&result,&error)&&result==L"TypeError",
            L"native DOM validation errors still reach the JavaScript catch handler");
        Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
              L"native validation reaches JavaScript without first-chance C++ exceptions");

        InterlockedExchange(&firstChanceCppExceptions,0);
        Check(runtime.Execute(LR"JS(
            var nativeErrors=[];
            function probe(callback){try{callback();}catch(error){nativeErrors.push(error.name);}}
            probe(function(){[].reduce(null);});
            probe(function(){''.repeat(-1);});
            probe(function(){new DOMParser().parseFromString('','invalid/type');});
            probe(function(){new MutationObserver(null);});
            probe(function(){queueMicrotask(null);});
            probe(function(){String.fromCodePoint(-1);});
            probe(function(){decodeURIComponent('%');});
            probe(function(){decodeURI('%FF');});
            probe(function(){Promise();});
            probe(function(){new Promise(null);});
            probe(function(){new DataView();});
            probe(function(){nonexistentFunction();});
            return nativeErrors.join('|');
        )JS",&result,&error)&&result==L"TypeError|RangeError|TypeError|TypeError|TypeError|RangeError|URIError|URIError|TypeError|TypeError|TypeError|ReferenceError",
            L"native API argument errors retain their JavaScript error types");
        Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
              L"native feature probes do not raise first-chance C++ exceptions");

        Check(runtime.Execute(L"window.addEventListener('unload',function(){throw 'unload-error';});setTimeout(function(){throw 'timer-error';},0);",
              nullptr,&error),L"window shutdown and timer exception fixture loads");
        runtime.RunTimers();
        Check(runtime.LastError()==L"Uncaught timer-error",
              L"timer errors keep their JavaScript diagnostics");
        runtime.DispatchWindowEvent(L"unload");
        Check(runtime.LastError()==L"Uncaught unload-error"&&
              InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
              L"window shutdown callbacks report JavaScript errors without native exceptions");

        InterlockedExchange(&firstChanceCppExceptions,0);
        Check(!runtime.Execute(L"throw new Error('uncaught-regression');",&result,&error)&&
              error==L"Uncaught Error: uncaught-regression"&&runtime.LastError()==error,
              L"uncaught JavaScript errors still report failure and retain their diagnostics");
        Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
              L"uncaught JavaScript completion reports failure without a native exception");
        InterlockedExchange(&firstChanceCppExceptions,0);
        runtime.Clear();
        Check(runtime.Execute(L"return 1;",&result,&error)&&result==L"1",
              L"runtime reset remains usable after handled and uncaught exceptions");
    }
    Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
          L"runtime reset and destruction do not raise native exceptions");
    if(handler)RemoveVectoredExceptionHandler(handler);
    return failures==initialFailures?0:1;
}

int RunEmbeddedScriptCompatibility(){
    const auto before=failures;std::wstring error,result;Document document;
    Check(document.Parse(L"<!doctype html><html><head><title>  Embedded\n script </title>"
        L"<style>html,body{margin:0}#host{width:80px;height:40px}.panel{background:red}</style>"
        L"</head><body><div id='host'><span>light</span></div></body></html>",&error),error.c_str());
    JavaScriptRuntime runtime(document);runtime.SetLocation(L"https://embedded.test/page");
    for(const auto ratio:{1.0,1.5}){
        runtime.SetDevicePixelRatio(ratio);
        for(const auto threshold:{size_t{0},size_t{1}}){
            runtime.SetJitCompilationThreshold(threshold);
            Check(runtime.Execute(LR"JS(
                function blockBranches(){
                    {
                        let retained=42;
                        {if(false){}}
                        {true||'unused';}
                        {false&&'unused';}
                        {true?'first':'second';}
                        {null??'fallback';}
                        return retained;
                    }
                }
                function serializeNested(value){
                    if(value===null)return 'null';
                    if(typeof value!=='object')return String(value);
                    {
                        let output='{',separator='';
                        for(var key in value)if(Object.prototype.hasOwnProperty.call(value,key)){
                            {if(false){}}
                            output+=separator+key+':'+serializeNested(value[key]);separator=',';
                        }
                        return output+'}';
                    }
                }
                function blockCompletions(){
                    let trace='';
                    outer:for(let index of [1,2,3]){
                        let retained=index;
                        try{throw index;}catch(error){
                            {if(false){}}
                            trace+=retained+':'+error+';';
                            if(index<3)continue outer;
                            break outer;
                        }finally{
                            {if(false){}}
                            trace+='f'+retained+';';
                        }
                    }
                    return trace;
                }
                return blockBranches()+'|'+serializeNested({first:{a:1,b:2},second:3})+'|'+blockCompletions();
            )JS",&result,&error)&&result==L"42|{first:{a:1,b:2},second:3}|1:1;f1;2:2;f2;3:3;f3;",
                L"branches into closing blocks unwind each scope once and preserve recursive iteration, catch and finally at both DPIs and JIT thresholds");
        }
        runtime.SetJitCompilationThreshold(512);
        std::srand(1);
        Check(runtime.Execute(L"return [Math.random(),Math.random(),Math.random()].join(',');",&result,&error),error.c_str());
        const auto randomSequence=result;std::srand(1);
        Check(runtime.Execute(L"return [Math.random(),Math.random(),Math.random()].join(',');",&result,&error)&&result!=randomSequence,
            L"host CRT seed changes do not replay a realm's Math.random sequence");
        Check(runtime.Execute(LR"JS(
            var validRandomRange=true,fullRandomPrecision=false;
            for(var sample=0;sample<512;sample++){
                var randomNumber=Math.random();
                validRandomRange=validRandomRange&&randomNumber>=0&&randomNumber<1;
                fullRandomPrecision=fullRandomPrecision||randomNumber*32768!==Math.floor(randomNumber*32768);
            }
            return validRandomRange+'|'+fullRandomPrecision;
        )JS",&result,&error)&&result==L"true|true",
            L"Math.random supplies fractions in [0,1) beyond the Windows CRT's 15-bit precision at both DPIs");
        Check(runtime.Execute(LR"JS(
            var noFunctionPrototypeConstruction=false;
            try{new Function.prototype();}catch(e){noFunctionPrototypeConstruction=e instanceof TypeError;}
            return typeof Function.prototype+'|'+(Function.prototype instanceof Function)+'|'+
                (Function.prototype()===undefined)+'|'+Function.prototype.toString()+'|'+
                Object.prototype.toString.call(Function.prototype)+'|'+
                (Object.getPrototypeOf(Function.prototype)===Object.prototype)+'|'+
                Object.keys(Function.prototype).length+'|'+noFunctionPrototypeConstruction;
        )JS",&result,&error)&&result==L"function|true|true|function () { [native code] }|[object Function]|true|0|true",
            L"Function.prototype is callable without being constructable and has coherent type, source, prototype and property reflection at both DPIs");
        for(const auto threshold:{size_t{0},size_t{1}}){
            runtime.SetJitCompilationThreshold(threshold);
            Check(runtime.Execute(LR"JS(
                function sequencing(initial){var value=initial;return value+(value=7);}
                var fn=function(value){return value;};var callResult=fn(fn='toString');
                var object={x:3,method:function(){return this.x;}},other={x:9,method:function(){return this.x;}};
                var methodResult=object.method(object=other);object={x:3};
                var indexResult=object[(object=other,'x')];object={x:3};object.x+=(object.x=4);
                var ctor=function(value){this.value=value;},original=ctor;
                var instance=new ctor(ctor=7);
                return callResult+'|'+methodResult+'|'+indexResult+'|'+object.x+'|'+instance.value+'|'+
                    (instance instanceof original)+'|'+sequencing(3)+'|'+sequencing(5)+'|'+fn;
            )JS",&result,&error)&&result==L"toString|3|3|7|7|true|10|12|toString",
                L"callee, receiver, constructor, computed base and left operand are captured before subsequent side effects in interpreter and JIT at both DPIs");
            if(threshold)Check(runtime.GetJitStatistics().nativeCalls>0,
                L"JIT execution also materializes left operand values before assignment");
            const auto bitwiseNativeBefore=runtime.GetJitStatistics().nativeCalls;
            Check(runtime.Execute(LR"JS(
                function bitAnd(a,b){return a&b;}function bitOr(a,b){return a|b;}function bitXor(a,b){return a^b;}
                function bitLeft(a,b){return a<<b;}function bitRight(a,b){return a>>b;}function bitUnsigned(a,b){return a>>>b;}
                function bitNot(a){return ~a;}
                var inputs=[0,-0,NaN,Infinity,-Infinity,.5,-1.5,2147483647,2147483648,4294967295,
                    4294967296,9007199254740991,9223372036854775808,9223372036854777856,-9223372036854777856,
                    19342813113834064647782400,1e100,-1e100],bitsAgree=true;
                for(var first=0;first<inputs.length;first++){
                    var left=inputs[first];bitsAgree=bitsAgree&&bitNot(left)===(~left);
                    for(var second=0;second<inputs.length;second++){
                        var right=inputs[second];
                        bitsAgree=bitsAgree&&bitAnd(left,right)===(left&right)&&bitOr(left,right)===(left|right)&&
                            bitXor(left,right)===(left^right)&&bitLeft(left,right)===(left<<right)&&
                            bitRight(left,right)===(left>>right)&&bitUnsigned(left,right)===(left>>>right);
                    }
                }
                return bitsAgree+'|'+bitXor('5',3)+'|'+bitUnsigned(-1,0);
            )JS",&result,&error)&&result==L"true|6|4294967295",
                L"numeric bitwise JIT agrees with interpreter modulo conversion for fractions, huge doubles, NaN, infinities and signed shifts at both DPIs");
            if(threshold)Check(runtime.GetJitStatistics().nativeCalls>bitwiseNativeBefore,
                L"bitwise helpers execute as guarded native leaf code");
        }
        runtime.SetJitCompilationThreshold(512);
        Check(runtime.Execute(LR"JS(
            var order=[],optionalReads=0,unusedReads=0;
            var receiver={x:3,get method(){order.push('get');return function(){order.push('call');return this.x;};}};
            var alternate={x:9};
            var called=receiver.method((order.push('arg'),receiver=alternate));
            var optional={get method(){optionalReads++;return function(){return this===optional;};}};
            var optionalResult=optional.method?.();
            var discarded={get value(){unusedReads++;return 1;}};discarded.value;void discarded.value;
            var iteration=0;for(discarded.value;iteration<1;iteration++,discarded.value){}
            var methods={strict:function(){'use strict';return this===undefined;}};
            var commaResult=(0,methods.strict)(),conditionalResult=(true?methods.strict:null)(),logicalResult=(false||methods.strict)();
            var logicalReads=0,logical={get value(){logicalReads++;return 3;}};var logicalResultValue=(logical.value??=4);
            var inferName=function(){};
            return called+'|'+order.join(',')+'|'+optionalResult+'|'+optionalReads+'|'+unusedReads+'|'+
                commaResult+'|'+conditionalResult+'|'+logicalResult+'|'+logicalResultValue+'|'+logicalReads+'|'+inferName.name;
        )JS",&result,&error)&&result==L"3|get,arg,call|true|1|4|true|true|true|3|1|inferName",
            L"getters run once at the required evaluation boundary and optional calls retain receivers while value-producing expressions release them");
        Check(runtime.Execute(LR"JS(
            var retainedCallable=function(){return 'retained';};
            function discardDuringArguments(){retainedCallable=null;for(var i=0;i<20000;i++){var cycle={};cycle.self=cycle;}return 1;}
            return retainedCallable(discardDuringArguments());
        )JS",&result,&error)&&result==L"retained",
            L"a prepared callee stays reachable through cycle collection during argument evaluation");
        Check(runtime.Execute(LR"JS(
            function reflectable(first,second=7,...rest){}
            var lengthDescriptor=Object.getOwnPropertyDescriptor(reflectable,'length');
            var nameDescriptor=Object.getOwnPropertyDescriptor(reflectable,'name');
            var nativeDescriptor=Object.getOwnPropertyDescriptor(Math.pow,'length');
            reflectable.length=9;Math.pow.length=9;
            var ordinary={};Object.defineProperty(ordinary,'value',{value:4});ordinary.value=8;
            return lengthDescriptor.value+'|'+lengthDescriptor.writable+'|'+lengthDescriptor.enumerable+'|'+lengthDescriptor.configurable+'|'+
                nameDescriptor.value+'|'+nativeDescriptor.value+'|'+nativeDescriptor.writable+'|'+
                reflectable.length+'|'+Math.pow.length+'|'+Object.keys(reflectable).length+'|'+ordinary.value+'|'+
                (Object.getOwnPropertyNames(reflectable).indexOf('length')>=0);
        )JS",&result,&error)&&result==L"1|false|false|true|reflectable|2|false|1|2|0|4|true",
            L"function metadata is reflected as actual nonenumerable properties with standard writable flags at both DPIs");
        Check(runtime.Execute(LR"JS(
            var readOnlyErrors=[];
            function rejectReadOnly(){'use strict';
                try{reflectable.length=9;}catch(e){readOnlyErrors.push(e instanceof TypeError);}
                try{Math.pow.length++;}catch(e){readOnlyErrors.push(e instanceof TypeError);}
                try{ordinary.value+=1;}catch(e){readOnlyErrors.push(e instanceof TypeError);}
            }
            rejectReadOnly();return readOnlyErrors.join(',')+'|'+reflectable.length+'|'+Math.pow.length+'|'+ordinary.value;
        )JS",&result,&error)&&result==L"true,true,true|1|2|4",
            L"strict assignments and updates reject readonly data properties without changing their values");
        Check(runtime.Execute(LR"JS(
            var savedName=reflectable.name;
            Object.defineProperty(reflectable,'name',{value:'changed',writable:true});reflectable.name='edited';
            var edited=reflectable.name;delete reflectable.name;
            return savedName+'|'+edited+'|'+Object.prototype.hasOwnProperty.call(reflectable,'name')+'|'+
                (Object.getOwnPropertyNames(reflectable).indexOf('name')<0)+'|'+reflectable.name;
        )JS",&result,&error)&&result==L"reflectable|edited|false|true|",
            L"redefining and deleting function metadata stays consistent with property reads and reflection");
        Check(runtime.Execute(LR"JS(
            var trimEnd=String.prototype.trimEnd,trimStart=String.prototype.trimStart;
            var trimNull='',trimMarker={},trimSame=false;
            try{trimEnd.call(null);}catch(error){trimNull=error.name;}
            try{trimEnd.call({toString:function(){throw trimMarker;}});}catch(error){trimSame=error===trimMarker;}
            return '\ufeff\u00a0x\u202f\u3000'.trim()+'|'+trimEnd.call(' a \ufeff')+'|'+trimStart.call('\u2000 a ')+'|'+
                ('\u0085x\u0085'.trim()==='\u0085x\u0085')+'|'+trimEnd.call({toString:function(){return 'object ';}})+'|'+
                ('x'.trimEnd===trimEnd)+'|'+(String.prototype.trimRight===trimEnd)+'|'+
                (String.prototype.trimLeft===trimStart)+'|'+trimEnd.length+'|'+trimNull+'|'+trimSame;
        )JS",&result,&error)&&result==L"x| a|a |true|object|true|true|true|0|TypeError|true",
            L"string trimming handles ECMAScript whitespace, borrowed receivers, aliases, identity and thrown values at both DPIs");
        Check(runtime.Execute(LR"JS(
            var canvas=document.createElement('canvas'),context=canvas.getContext('2d');
            var tag=Object.getOwnPropertyDescriptor(HTMLCanvasElement.prototype,Symbol.toStringTag);
            return (Object.getPrototypeOf(Math)===Object.prototype)+'|'+(Object.getPrototypeOf(JSON)===Object.prototype)+'|'+
                Object.prototype.toString.call(Math)+'|'+Object.prototype.toString.call(canvas)+'|'+
                Object.prototype.toString.call(context)+'|'+Object.prototype.toString.call(document)+'|'+
                tag.enumerable+'|'+tag.writable+'|'+devicePixelRatio;
        )JS",&result,&error)&&result==(ratio==1.0?L"true|true|[object Math]|[object HTMLCanvasElement]|[object CanvasRenderingContext2D]|[object Document]|false|false|1":
            L"true|true|[object Math]|[object HTMLCanvasElement]|[object CanvasRenderingContext2D]|[object Document]|false|false|1.5"),
            L"intrinsic prototypes and DOM interface tags are coherent independently of DPI");
        Check(runtime.Execute(LR"JS(
            var comment=document.createComment('node'),textNode=document.createTextNode('text');
            Node.COMMENT_NODE=99;Node.prototype.TEXT_NODE=99;comment.COMMENT_NODE=99;
            var ownConstant=Object.create(Node.prototype);
            Object.defineProperty(ownConstant,'COMMENT_NODE',{value:12,writable:true});ownConstant.COMMENT_NODE=13;
            var descriptor=Object.getOwnPropertyDescriptor(Node,'COMMENT_NODE');
            return Node.COMMENT_NODE+'|'+comment.COMMENT_NODE+'|'+comment.nodeType+'|'+
                Node.TEXT_NODE+'|'+textNode.TEXT_NODE+'|'+document.DOCUMENT_NODE+'|'+
                Node.CDATA_SECTION_NODE+'|'+Node.PROCESSING_INSTRUCTION_NODE+'|'+
                descriptor.writable+'|'+descriptor.configurable+'|'+ownConstant.COMMENT_NODE;
        )JS",&result,&error)&&result==L"8|8|8|3|3|9|4|7|false|false|13",
            L"DOM node constants match node types and are inherited read-only constants at both DPIs");
        Check(runtime.Execute(LR"JS(
            var measureContext=document.createElement('canvas').getContext('2d');measureContext.font='20px Arial';
            var measured=measureContext.measureText('Mg'),smallWidth=measured.width;
            var narrow=measureContext.measureText('ii').width,wide=measureContext.measureText('WW').width;
            measureContext.scale(3,4);var unchangedWidth=measureContext.measureText('Mg').width;
            measureContext.font='40px Arial';var largeWidth=measureContext.measureText('Mg').width;
            var whiteSpace=measureContext.measureText('A\tB').width===measureContext.measureText('A B').width;
            var trailing=measureContext.measureText('A  ').width>measureContext.measureText('A').width;
            var emptyMetrics=measureContext.measureText('');measured.width=0;
            var receiverError=false,getterError=false,coercionError=false,strictWrite=false,marker={};
            try{(function(){'use strict';measured.width=0;})();}catch(e){strictWrite=e instanceof TypeError;}
            try{CanvasRenderingContext2D.prototype.measureText.call({},'text');}catch(e){receiverError=e instanceof TypeError;}
            try{Object.getOwnPropertyDescriptor(TextMetrics.prototype,'width').get.call({});}catch(e){getterError=e instanceof TypeError;}
            try{measureContext.measureText({toString:function(){throw marker;}});}catch(e){coercionError=e===marker;}
            return (measured instanceof TextMetrics)+'|'+(measured.width===smallWidth&&smallWidth>0&&wide>narrow)+'|'+
                (unchangedWidth===smallWidth&&Math.abs(largeWidth-2*smallWidth)<0.01)+'|'+whiteSpace+'|'+trailing+'|'+
                (measured.actualBoundingBoxAscent>0&&measured.actualBoundingBoxDescent>0&&measured.fontBoundingBoxAscent>0)+'|'+
                (emptyMetrics.width===0&&emptyMetrics.actualBoundingBoxRight===0)+'|'+receiverError+'|'+getterError+'|'+coercionError+'|'+
                (measureContext.measureText===CanvasRenderingContext2D.prototype.measureText)+'|'+measureContext.measureText.length+'|'+strictWrite+'|'+
                (Object.getOwnPropertyNames(measured).length===0);
        )JS",&result,&error)&&result==L"true|true|true|true|true|true|true|true|true|true|true|1|true|true",
            L"Canvas text metrics use shaped glyph advances and ink, font and alignment metrics in intrinsic units at both DPIs");
        Check(runtime.Execute(LR"JS(
            var radialContext=document.createElement('canvas').getContext('2d');
            var radial=radialContext.createRadialGradient(10,10,2,10,10,8);
            radial.addColorStop(0,'red');radial.addColorStop(1,'blue');radialContext.fillStyle=radial;
            radialContext.fillRect(0,0,20,20);var center=radialContext.getImageData(10,10,1,1).data;
            var edge=radialContext.getImageData(19,10,1,1).data,between=radialContext.getImageData(14,10,1,1).data;
            radialContext.clearRect(0,0,300,150);radialContext.scale(2,1);
            var transformed=radialContext.createRadialGradient(5,10,0,5,10,4);
            transformed.addColorStop(0,'red');transformed.addColorStop(1,'blue');radialContext.resetTransform();
            radialContext.fillStyle=transformed;radialContext.fillRect(0,0,20,20);
            var horizontal=radialContext.getImageData(15,10,1,1).data,vertical=radialContext.getImageData(10,15,1,1).data;
            radialContext.clearRect(0,0,300,150);
            radialContext.fillStyle=radialContext.createRadialGradient(0,0,0,0,0,5);radialContext.fillRect(0,0,20,20);
            var emptyAlpha=radialContext.getImageData(1,1,1,1).data[3];
            var identical=radialContext.createRadialGradient(10,10,2,10,10,2);identical.addColorStop(0,'red');
            radialContext.fillStyle=identical;radialContext.fillRect(0,0,20,20);var identicalAlpha=radialContext.getImageData(10,10,1,1).data[3];
            var negativeRadius=false,nonfinite=false,radialReceiver=false;
            try{radialContext.createRadialGradient(0,0,-1,0,0,5);}catch(e){negativeRadius=e.name==='IndexSizeError';}
            try{radialContext.createRadialGradient(0,0,1,0,0,Infinity);}catch(e){nonfinite=e instanceof TypeError;}
            try{CanvasRenderingContext2D.prototype.createRadialGradient.call({},0,0,1,0,0,5);}catch(e){radialReceiver=e instanceof TypeError;}
            return (radial instanceof CanvasGradient&&radialContext.createRadialGradient===CanvasRenderingContext2D.prototype.createRadialGradient)+'|'+
                (center[0]>240&&center[2]<10&&edge[2]>240&&between[0]>30&&between[2]>30)+'|'+
                (horizontal[0]>30&&horizontal[2]>30&&vertical[2]>240)+'|'+emptyAlpha+'|'+identicalAlpha+'|'+
                negativeRadius+'|'+nonfinite+'|'+radialReceiver+'|'+radialContext.createRadialGradient.length;
        )JS",&result,&error)&&result==L"true|true|true|0|0|true|true|true|6",
            L"radial gradients paint interpolated two-circle colors with creation transforms, transparency and standard errors at both DPIs");
        Check(runtime.Execute(LR"JS(
            var svgNamespace='http://www.w3.org/2000/svg';
            var textRoot=document.createElementNS(svgNamespace,'svg');textRoot.style.fontFamily='Arial';textRoot.style.fontSize='20px';
            var svgLabel=document.createElementNS(svgNamespace,'text');textRoot.appendChild(svgLabel);svgLabel.textContent='Actual glyph measurement';
            var svgAdvance=svgLabel.getComputedTextLength(),svgBox=svgLabel.getBBox();
            svgLabel.setAttribute('transform','scale(3)');
            var sameLength=svgLabel.getComputedTextLength()===svgAdvance;
            svgLabel.style.fontSize='40px';var doubleSize=svgLabel.getComputedTextLength();
            svgLabel.style.fontSize='20px';svgLabel.textContent='  A   B  ';var collapsedLength=svgLabel.getComputedTextLength();
            svgLabel.textContent='A B';var normalizedLength=svgLabel.getComputedTextLength();
            svgLabel.textContent='  A   B  ';svgLabel.style.whiteSpace='pre';var preservedLength=svgLabel.getComputedTextLength();
            svgLabel.textContent='';var emptyLength=svgLabel.getComputedTextLength();
            var textSpan=document.createElementNS(svgNamespace,'tspan');textSpan.textContent='WW';textSpan.style.fontSize='40px';svgLabel.appendChild(textSpan);
            var nestedLength=svgLabel.getComputedTextLength(),childLength=textSpan.getComputedTextLength();
            textRoot.style.display='none';var hiddenLength=svgLabel.getComputedTextLength();
            var invalidTextReceiver=false;
            try{SVGTextContentElement.prototype.getComputedTextLength.call(document.body);}catch(e){invalidTextReceiver=e instanceof TypeError;}
            var svgLengthDescriptor=Object.getOwnPropertyDescriptor(SVGTextContentElement.prototype,'getComputedTextLength');
            var textPath=document.createElementNS(svgNamespace,'textPath');textPath.textContent='path glyphs';
            return (svgAdvance>0&&Math.abs(svgBox.width-svgAdvance)<0.01)+'|'+sameLength+'|'+
                (Math.abs(doubleSize-2*svgAdvance)<0.01)+'|'+(Math.abs(collapsedLength-normalizedLength)<0.01)+'|'+
                (preservedLength>normalizedLength)+'|'+emptyLength+'|'+(Math.abs(nestedLength-childLength)<0.01)+'|'+hiddenLength+'|'+
                invalidTextReceiver+'|'+(svgLabel.getComputedTextLength===SVGTextContentElement.prototype.getComputedTextLength)+'|'+
                (svgLabel instanceof SVGTextElement&&textSpan instanceof SVGTSpanElement&&textSpan instanceof SVGTextContentElement)+'|'+
                svgLengthDescriptor.enumerable+'|'+svgLengthDescriptor.value.length+'|'+
                (typeof document.createElement('div').getComputedTextLength==='undefined')+'|'+
                (textPath instanceof SVGTextPathElement&&textPath instanceof SVGTextContentElement&&textPath.getComputedTextLength()>0);
        )JS",&result,&error)&&result==L"true|true|true|true|true|0|true|0|true|true|true|true|0|true|true",
            L"SVG text advances use shaped glyphs, inherited CSS, nested fonts and whitespace in user units at both DPIs with validated prototype receivers");
        Check(runtime.Execute(LR"JS(
            var textGeometry=document.createElementNS('http://www.w3.org/2000/svg','text');
            textGeometry.style.fontSize='20px';textGeometry.textContent='AB';
            textGeometry.setAttribute('x','13');textGeometry.setAttribute('y','29');
            var firstCell=textGeometry.getExtentOfChar(0),secondCell=textGeometry.getExtentOfChar(1);
            var invalidIndex=false,invalidCellReceiver=false;
            try{textGeometry.getExtentOfChar(2);}catch(e){invalidIndex=e.name==='IndexSizeError';}
            try{SVGTextContentElement.prototype.getExtentOfChar.call(document.body,0);}catch(e){invalidCellReceiver=e instanceof TypeError;}
            textGeometry.setAttribute('transform','scale(1.5)');
            var unchangedCell=textGeometry.getExtentOfChar(0);
            textGeometry.style.fontSize='40px';var largeCell=textGeometry.getExtentOfChar(0);
            textGeometry.textContent='\ud83d\ude00';var surrogateCell=textGeometry.getExtentOfChar(1);
            var context=document.createElement('canvas').getContext('2d'),gradient=context.createLinearGradient(0,0,40,0);
            var another=context.createLinearGradient(0,0,40,0),borrowedStop=gradient.addColorStop;
            borrowedStop.call(another,0,'#000000');borrowedStop.call(another,1,'#ffffff');context.fillStyle=another;
            context.fillRect(0,0,40,1);var pixels=context.getImageData(0,0,40,1).data;
            var stopRange=false,stopReceiver=false;
            try{gradient.addColorStop(2,'red');}catch(e){stopRange=e.name==='IndexSizeError';}
            try{borrowedStop.call({},0,'red');}catch(e){stopReceiver=e instanceof TypeError;}
            return (firstCell.x===13&&firstCell.y<29&&firstCell.width>0&&firstCell.height>0)+'|'+
                (secondCell.x>=firstCell.x+firstCell.width-0.01)+'|'+invalidIndex+'|'+invalidCellReceiver+'|'+
                (unchangedCell.width===firstCell.width)+'|'+(Math.abs(largeCell.width-firstCell.width*2)<0.01)+'|'+
                (textGeometry.getNumberOfChars()===2&&surrogateCell.width>0)+'|'+
                (gradient instanceof CanvasGradient&&Object.getPrototypeOf(gradient)===CanvasGradient.prototype)+'|'+
                (borrowedStop===CanvasGradient.prototype.addColorStop&&borrowedStop.length===2)+'|'+
                (pixels[0]<pixels[156]&&pixels[3]===255&&pixels[159]===255)+'|'+stopRange+'|'+stopReceiver;
        )JS",&result,&error)&&result==L"true|true|true|true|true|true|true|true|true|true|true|true",
            L"SVG character cells use shaped UTF-16 clusters and CanvasGradient prototype methods retain real pixel output at both DPIs");
        Check(runtime.Execute(LR"JS(
            function take(a,b){return a+'|'+b;}
            var argumentValue=1,ordered=take(argumentValue,argumentValue=2);
            var values={get x(){argumentValue=7;return 3;}};
            var getterOrder=take(values.x,argumentValue),spreadOrder=take(...[4,5]);
            function localEval(){var scoped=11;return eval('scoped+1');}
            function ArgumentCtor(a,b){this.pair=take(a,b);}
            var objectCtor=new ArgumentCtor(6,7),spreadCtor=new ArgumentCtor(...[8,9]);
            function recursive(n){return n?take(n,recursive(n-1)):0;}
            return ordered+';'+getterOrder+';'+spreadOrder+';'+localEval()+';'+objectCtor.pair+';'+spreadCtor.pair+';'+recursive(2);
        )JS",&result,&error)&&result==L"1|2;3|7;4|5;12;6|7;8|9;2|1|0",
            L"stack argument calls preserve left-to-right getters, spread, constructors, direct eval and recursion at both DPIs");
        Check(runtime.Execute(LR"JS(
            var badRadix=false;try{(19).toString(37);}catch(e){badRadix=e instanceof RangeError;}
            var paddingCoercions=0,fill={toString(){paddingCoercions++;return 'ab';}};
            var untouched='abcd'.padStart(2,fill),padded='x'.padStart(4,fill);
            return (186).toString(16)+'|'+(35).toString(36)+'|'+(0.5).toString(2)+'|'+
                Number.prototype.toString.call(new Number(-15),16)+'|'+
                ((19).toString===Number.prototype.toString)+'|'+badRadix+'|'+untouched+'|'+padded+'|'+paddingCoercions+'|'+
                'x'.padEnd(4,'ab')+'|'+('x'.padStart(9,'')==='x');
        )JS",&result,&error)&&result==L"ba|z|0.1|-f|true|true|abcd|abax|1|xaba|true",
            L"radix conversion and padding retain values, standard receivers and fill evaluation order at both DPIs");
        Check(runtime.Execute(LR"JS(
            var octets=new Uint8Array(3),signedWords=new Int32Array(1),clamped=new Uint8ClampedArray(3),single=new Float32Array(1);
            octets[0]=257;octets[1]=-1;octets[2]=3.9;octets[3]=99;octets[-1]=9;octets.length=20;
            signedWords[0]=4294967295;clamped[0]=1.5;clamped[1]=2.5;clamped[2]=400;single[0]=1+Math.pow(2,-24);
            var bigIntStore=false;try{octets[0]=1n;}catch(e){bigIntStore=e instanceof TypeError;}
            return octets.join(',')+'|'+octets.length+'|'+(typeof octets[3]==='undefined'&&typeof octets[-1]==='undefined')+'|'+
                signedWords[0]+'|'+clamped.join(',')+'|'+single[0]+'|'+bigIntStore;
        )JS",&result,&error)&&result==L"1,255,3|3|true|-1|2,2,255|1|true",
            L"typed array stores apply integer width, signedness, clamping and float precision without resizing at both DPIs");
        Check(runtime.Execute(LR"JS(
            var shared=new ArrayBuffer(24),bytes=new Uint8Array(shared),words=new Uint32Array(shared),writer=new DataView(shared,4,8);
            writer.setFloat64(0,1,true);var floatBytes=bytes[10]===240&&bytes[11]===63;
            words[0]=0x01020304;var wordBytes=bytes[0]===4&&bytes[1]===3&&bytes[2]===2&&bytes[3]===1;
            bytes[4]=7;var sharedInteger=writer.getUint32(0,true)===7;
            DataView.prototype.setFloat64.call(writer,0,-1.5,false);
            var doubleEndian=writer.getFloat64(0,false)===-1.5&&bytes[4]===191&&bytes[5]===248;
            writer.setInt16(0,-2,true);var signedEndian=writer.getInt16(0,true)===-2&&writer.getUint16(0,false)===65279;
            writer.setFloat32(0,0.5,true);var singleEndian=writer.getFloat32(0,true)===0.5;
            var outside=false,badReceiver=false;try{writer.setFloat64(1,2);}catch(e){outside=e instanceof RangeError;}
            try{DataView.prototype.getFloat64.call({});}catch(e){badReceiver=e instanceof TypeError;}
            var offsetBytes=new Uint8Array(shared,4,8);offsetBytes[0]=129;
            return floatBytes&&wordBytes&&sharedInteger&&doubleEndian&&signedEndian&&singleEndian&&outside&&badReceiver&&
                bytes[4]===129&&offsetBytes.buffer===shared&&offsetBytes.byteOffset===4&&writer.byteOffset===4&&writer.byteLength===8&&
                writer instanceof DataView&&!Array.isArray(bytes)&&ArrayBuffer.isView(writer)&&ArrayBuffer.isView(bytes)&&
                Object.prototype.toString.call(bytes)==='[object Uint8Array]'&&Object.prototype.toString.call(writer)==='[object DataView]'&&
                writer.setFloat64===DataView.prototype.setFloat64&&writer.setFloat64.length===2;
        )JS",&result,&error)&&result==L"true",
            L"DataView and typed views share byte storage with float and integer encoding, bounds, offsets, endian order and prototype receiver validation at both DPIs");
        Check(runtime.Execute(LR"JS(
            var hashes=[],hashErrors=[],hashOrder=[];var hashInput=new Uint8Array([97,98,99]);
            function digestHex(buffer){return Array.from(new Uint8Array(buffer),x=>x.toString(16).padStart(2,'0')).join('');}
            crypto.subtle.digest('sHa-256',hashInput).then(function(buffer){hashes[0]=digestHex(buffer);hashes[4]=buffer instanceof ArrayBuffer;hashOrder.push('hash');});
            crypto.subtle.digest({name:'SHA-1'},hashInput.buffer).then(function(buffer){hashes[1]=digestHex(buffer);});
            crypto.subtle.digest('SHA-384',hashInput).then(function(buffer){hashes[2]=digestHex(buffer);});
            crypto.subtle.digest('SHA-512',hashInput).then(function(buffer){hashes[3]=digestHex(buffer);});
            var offsetHashBuffer=new ArrayBuffer(8),offsetHashBytes=new Uint8Array(offsetHashBuffer);
            offsetHashBytes[3]=97;offsetHashBytes[4]=98;offsetHashBytes[5]=99;
            crypto.subtle.digest('SHA-256',new DataView(offsetHashBuffer,3,3)).then(function(buffer){hashes[5]=digestHex(buffer);});
            crypto.subtle.digest('MD5',hashInput).catch(function(e){hashErrors.push(e.name);});
            crypto.subtle.digest('SHA-256',[1,2,3]).catch(function(e){hashErrors.push(e.name);});
            hashInput[0]=120;hashOrder.push('sync');
            return isSecureContext+'|'+(crypto.subtle instanceof SubtleCrypto)+'|'+
                (crypto.subtle.digest===SubtleCrypto.prototype.digest)+'|'+crypto.subtle.digest.length+'|'+hashes.length;
        )JS",&result,&error)&&result==L"true|true|true|2|0",L"WebCrypto digest snapshots input and returns a pending promise at both DPIs");
        for(int pending=0;pending<4;++pending)runtime.RunTimers();
        Check(runtime.Execute(LR"JS(
            return hashes[0]==='ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'&&
                hashes[1]==='a9993e364706816aba3e25717850c26c9cd0d89d'&&
                hashes[2]==='cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7'&&
                hashes[3]==='ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f'&&
                hashes[4]&&hashes[5]===hashes[0]&&hashOrder.join(',')==='sync,hash'&&hashErrors.join(',')==='NotSupportedError,TypeError';
        )JS",&result,&error)&&result==L"true",L"native SHA digests match known vectors, preserve snapshots and reject unsupported inputs at both DPIs");
        runtime.SetLocation(L"http://untrusted.test/");
        Check(runtime.Execute(L"return !isSecureContext&&typeof crypto.subtle==='undefined'&&typeof SubtleCrypto==='undefined';",&result,&error)&&result==L"true",
            L"insecure HTTP contexts do not expose SubtleCrypto");
        runtime.SetLocation(L"http://localhost/");
        Check(runtime.Execute(L"return isSecureContext&&typeof crypto.subtle.digest==='function';",&result,&error)&&result==L"true",L"loopback contexts can use native digests");
        runtime.SetLocation(L"https://embedded.test/page");runtime.SetSecureContextAncestors([]{return false;});
        Check(runtime.Execute(L"return !isSecureContext&&typeof crypto.subtle==='undefined';",&result,&error)&&result==L"true",L"an insecure ancestor prevents HTTPS child secure-context exposure");
        runtime.SetSecureContextAncestors({});
        for(const auto threshold:{size_t(0),size_t(1)}){
            runtime.SetJitCompilationThreshold(threshold);
            Check(runtime.Execute(LR"JS(
                function strictReceiver(){'use strict';return this===undefined;}
                function sloppyReceiver(){return this===globalThis;}
                var relay={invoke:function(fn){return fn();}};
                var receiverBefore=relay.invoke(strictReceiver)&&relay.invoke(sloppyReceiver);
                for(var warm=0;warm<200;warm++)relay.invoke(strictReceiver);
                return receiverBefore+'|'+relay.invoke(strictReceiver)+'|'+relay.invoke(sloppyReceiver);
            )JS",&result,&error)&&result==L"true|true|true",
                L"bare calls inside warmed methods preserve strict and sloppy receivers in interpreter and optimized scopes at both DPIs");
            Check(runtime.Execute(LR"JS(
                var scopeReads=0,scopeSentinel={value:19},scopeTable={get value(){scopeReads++;return 5;},get failure(){throw scopeSentinel;}};
                Object.defineProperty(scopeTable,'0',Object.getOwnPropertyDescriptor(scopeTable,'value'));
                Object.defineProperty(scopeTable,'1',Object.getOwnPropertyDescriptor(scopeTable,'failure'));
                function scopeSource(){return scopeTable;}
                function scopeLookup(i,t,v){i=i-100;t=scopeSource();v=t[i];return v;}
                var scopeValid=true;for(var i=0;i<100;i++)scopeValid=scopeValid&&scopeLookup(100)===5;
                var scopeCaught=false;try{scopeLookup(101);}catch(e){scopeCaught=e===scopeSentinel;}
                scopeTable=[8,9];scopeValid=scopeValid&&scopeLookup(100)===8&&scopeLookup(101)===9;
                function scopeEscaped(a){return function(){return a;};}var scopeClosures=[];
                for(var i=0;i<100;i++)scopeClosures.push(scopeEscaped(i));
                for(var i=0;i<100;i++)scopeValid=scopeValid&&scopeClosures[i]()===i;
                return scopeValid+'|'+scopeCaught+'|'+scopeReads;
            )JS",&result,&error)&&result==L"true|true|100",L"optimized and interpreter scopes preserve getters, thrown identity, capture updates and escaping closures at both DPIs");
        }
    }
    runtime.SetDevicePixelRatio(1);
    std::vector<ScriptRequest> scriptRequests;
    runtime.SetRequestLoader([&](const ScriptRequest& request){
        scriptRequests.push_back(request);ScriptResponse response;response.url=request.url;
        if(request.url.find(L"offline")!=std::wstring::npos)return response;
        response.status=422;response.statusText=L"Unprocessable Content";response.contentType=L"application/json";
        response.headers=L"Content-Type: application/json\r\nX-Result: rejected\r\n";response.body=L"{\"ok\":false}";return response;
    });
    Check(runtime.Execute(LR"JS(
        var request=new XMLHttpRequest(),requestEvent='';request.open('POST','/submit',false);
        request.setRequestHeader('X-Test','retained');request.responseType='json';
        request.onload=function(){requestEvent='load';};request.onerror=function(){requestEvent='error';};
        request.send('caf\u00e9');
        return request.status+'|'+request.statusText+'|'+request.response.ok+'|'+request.getResponseHeader('x-result')+'|'+requestEvent+'|'+request.responseURL;
    )JS",&result,&error)&&result==L"422|Unprocessable Content|false|rejected|load|https://embedded.test/submit",
        L"XHR retains HTTP error status, headers and JSON body and fires load when the server responds");
    Check(scriptRequests.size()==1&&scriptRequests[0].method==L"POST"&&scriptRequests[0].origin==L"https://embedded.test"&&
        scriptRequests[0].body==std::vector<unsigned char>({'c','a','f',0xc3,0xa9})&&
        std::find(scriptRequests[0].headers.begin(),scriptRequests[0].headers.end(),std::make_pair(std::wstring(L"x-test"),std::wstring(L"retained")))!=scriptRequests[0].headers.end(),
        L"the common request loader receives the POST method, UTF-8 body, origin and custom headers");
    Check(runtime.Execute(LR"JS(
        var fetchResult='',fetchFailure='';
        fetch('/submit',{method:'PUT',headers:new Headers({'Content-Type':'application/octet-stream'}),body:new Uint8Array([0,128,255])})
            .then(function(response){fetchResult=response.status+'|'+response.ok+'|'+response.headers.get('x-result');});
        fetch('/offline').catch(function(error){fetchFailure=error.name;});
    )JS",&result,&error),error.c_str());
    Check(runtime.Execute(L"return fetchResult+'|'+fetchFailure;",&result,&error)&&result==L"422|false|rejected|TypeError"&&
        scriptRequests.size()==3&&scriptRequests[1].method==L"PUT"&&scriptRequests[1].body==std::vector<unsigned char>({0,128,255}),
        L"fetch forwards methods and byte bodies, resolves HTTP errors and rejects transport failures");
    scriptRequests.clear();
    Check(runtime.Execute(LR"JS(
        var opaqueFetch='',sameOriginFetch='',invalidModeFetch='',invalidMethodFetch='';
        fetch('https://cross-origin.test/reachability',{method:'HEAD',mode:'no-cors',headers:{'X-Private':'secret','Accept':'text/plain'}}).then(function(response){
            opaqueFetch=response.type+'|'+response.status+'|'+response.ok+'|'+response.url+'|'+response.statusText+'|'+
                response.headers.get('x-result')+'|'+(response.body===null);
            return response.text();
        }).then(function(text){opaqueFetch+='|'+text;});
        fetch('https://cross-origin.test/reachability',{mode:'same-origin'}).catch(function(error){sameOriginFetch=error.name;});
        fetch('/submit',{mode:'unknown'}).catch(function(error){invalidModeFetch=error.name;});
        fetch('/submit',{method:'PUT',mode:'no-cors'}).catch(function(error){invalidMethodFetch=error.name;});
    )JS",&result,&error),error.c_str());
    Check(runtime.Execute(L"return opaqueFetch+'|'+sameOriginFetch+'|'+invalidModeFetch+'|'+invalidMethodFetch;",&result,&error)&&
        result==L"opaque|0|false|||null|true||TypeError|TypeError|TypeError"&&
        scriptRequests.size()==1&&scriptRequests[0].mode==ScriptRequest::Mode::NoCors&&scriptRequests[0].method==L"HEAD"&&
        scriptRequests[0].headers.size()==1&&scriptRequests[0].headers[0].first==L"accept"&&scriptRequests[0].headers[0].second==L"text/plain",
        L"no-cors fetch resolves a filtered opaque response and invalid modes reject without sending requests");
    scriptRequests.clear();
    const bool wireExecuted=runtime.Execute(LR"JS(
        var wireBytes=new Uint8Array([9,0,128,255,8]);
        var wireWords=new Uint16Array([0x1234,0xabcd]);
        var wireSlice=new Uint8Array(wireBytes.buffer,1,3);
        var wireBodies=[wireSlice,wireBytes.buffer,new DataView(wireBytes.buffer,1,3),
            wireWords,new Uint8Array(wireWords.buffer,1,2),
            new Blob([wireSlice,wireWords],{type:'application/octet-stream'})];
        for(var wireIndex=0;wireIndex<wireBodies.length;wireIndex++){
            var wireRequest=new XMLHttpRequest();wireRequest.open('POST','/wire-bytes',false);wireRequest.send(wireBodies[wireIndex]);
        }
        fetch('/wire-bytes',{method:'POST',body:wireSlice});
        return true;
    )JS",&result,&error);
    if(!wireExecuted||result!=L"true")std::wcerr<<L"Body serialization fixture: "<<error<<L" result="<<result<<L'\n';
    Check(wireExecuted&&result==L"true",L"binary request fixture executes");
    const std::vector<std::vector<unsigned char>> wireExpected={
        {0,128,255},{9,0,128,255,8},{0,128,255},{0x34,0x12,0xcd,0xab},{0x12,0xcd},
        {0,128,255,0x34,0x12,0xcd,0xab},{0,128,255}};
    bool wireCorrect=scriptRequests.size()==wireExpected.size();
    for(size_t index=0;wireCorrect&&index<wireExpected.size();++index)
        wireCorrect=scriptRequests[index].body==wireExpected[index];
    Check(wireCorrect,L"XHR and fetch snapshot the exact ArrayBuffer, typed-view, DataView and Blob bytes");
    Check(scriptRequests.size()>5&&std::find(scriptRequests[5].headers.begin(),scriptRequests[5].headers.end(),
        std::make_pair(std::wstring(L"content-type"),std::wstring(L"application/octet-stream")))!=scriptRequests[5].headers.end(),
        L"Blob request bodies use the Blob MIME type");
    runtime.SetRequestLoader({});
    Check(runtime.Execute(LR"JS(
        var hyperbolic=Math.tanh,coercions=0;
        var coercion={valueOf:function(){coercions++;return 2;}};
        var nanMinimum=Math.min(NaN,coercion);
        return [hyperbolic.call(null,Infinity),Object.is(hyperbolic(-0),-0),Math.acosh(1),Math.asinh(-Infinity),Math.atanh(1),
            Math.hypot(3,4),Math.hypot(Infinity,NaN),Math.abs(Math.hypot(1e308,1e308)/1e308-Math.SQRT2)<1e-15,
            Math.imul(0xffffffff,5),Math.clz32(1),Math.clz32(),Math.fround(1.337)===1.3370000123977661,
            Math.round(-1.5),Object.is(Math.round(-0.5),-0),1/Math.min(0,-0),1/Math.max(-0,0),Math.min(),Math.max(),
            Number.isNaN(nanMinimum),coercions,Number.isNaN(Math.pow(-1,Infinity)),Number.isNaN(Math.pow(1,NaN)),Number.isNaN(Math.sin()),
            Math.tanh===hyperbolic,hyperbolic.name,hyperbolic.length,Object.getOwnPropertyNames(Math).indexOf('tanh')>=0,Object.keys(Math).length].join('|');
    )JS",&result,&error)&&result==L"1|true|0|-Infinity|Infinity|5|Infinity|true|-5|31|32|true|-1|true|-Infinity|Infinity|Infinity|-Infinity|true|1|true|true|true|true|tanh|1|true|0",
        L"Math intrinsics retain callable identity and metadata, coerce all min arguments, handle signed zero and non-finite values, and calculate 32-bit products and large vector norms correctly");
    Check(runtime.Execute(LR"JS(
        var conversionMarker={},conversionTrace='',conversionCaught=false,symbolHint='';
        try{Math.min(NaN,{valueOf:function(){conversionTrace+='throw';throw conversionMarker;}},{valueOf:function(){conversionTrace+='later';return 0;}});}
        catch(error){conversionCaught=error===conversionMarker;}
        var numericPrimitive={};numericPrimitive[Symbol.toPrimitive]=function(hint){symbolHint=hint;return 9;};
        var bigintMathError='',symbolMathError='';
        try{Math.sqrt(1n);}catch(error){bigintMathError=error.name;}
        try{Math.abs(Symbol('number'));}catch(error){symbolMathError=error.name;}
        return conversionCaught+'|'+conversionTrace+'|'+Math.sqrt(numericPrimitive)+'|'+symbolHint+'|'+bigintMathError+'|'+symbolMathError;
    )JS",&result,&error)&&result==L"true|throw|3|number|TypeError|TypeError",
        L"Math conversion uses the number hint and propagates the original JavaScript throw before evaluating later arguments");
    Check(runtime.Execute(LR"JS(
        var pixelCanvas=document.createElement('canvas');pixelCanvas.width=3;pixelCanvas.height=2;
        pixelCanvas.style.width='30px';pixelCanvas.style.height='20px';
        var pixelContext=pixelCanvas.getContext('2d');pixelContext.fillStyle='#ff0000';pixelContext.fillRect(0,0,1,1);
        pixelContext.translate(1,0);pixelContext.fillStyle='#00ff00';pixelContext.fillRect(0,0,1,1);
        var pixelRead=pixelContext.getImageData(0,0,3,1),pixelOutside=pixelContext.getImageData(-1,0,2,1);
        var pixelReverse=pixelContext.getImageData(2,1,-2,-1),pixelSizeError='',pixelBrandError='';
        try{pixelContext.getImageData(0,0,0,1);}catch(error){pixelSizeError=error.name;}
        try{pixelContext.getImageData.call({},0,0,1,1);}catch(error){pixelBrandError=error.name;}
        pixelRead.data[0]=0;var retainedPixel=pixelContext.getImageData(0,0,1,1).data[0];
        pixelContext.clearRect(0,0,1,1);var clearedPixel=pixelContext.getImageData(1,0,1,1).data[3];
        return pixelRead.width+'|'+pixelRead.height+'|'+(pixelRead.data instanceof Uint8ClampedArray)+'|'+Array.prototype.join.call(pixelRead.data,',')+'|'+
            Array.prototype.join.call(pixelOutside.data,',')+'|'+Array.prototype.join.call(pixelReverse.data,',')+'|'+retainedPixel+'|'+clearedPixel+'|'+pixelSizeError+'|'+pixelBrandError;
    )JS",&result,&error)&&result==L"3|1|true|0,0,0,255,0,255,0,255,0,0,0,0|0,0,0,0,255,0,0,255|255,0,0,255,0,255,0,255|255|0|IndexSizeError|TypeError",
        L"Canvas getImageData reads the rendered intrinsic pixels, ignores drawing transforms for its coordinates, preserves transparent out-of-bounds pixels and returns an independent clamped byte array");
    Check(runtime.Execute(LR"JS(
        var strokeCanvas=document.createElement('canvas');strokeCanvas.width=100;strokeCanvas.height=70;
        var strokeContext=strokeCanvas.getContext('2d');strokeContext.font='48px Arial';strokeContext.strokeStyle='#ff0000';strokeContext.lineWidth=2;
        CanvasRenderingContext2D.prototype.strokeText.call(strokeContext,'O',10,52);
        var strokePixels=strokeContext.getImageData(0,0,100,70).data,redCoverage=0;
        for(var p=0;p<strokePixels.length;p+=4)if(strokePixels[p]>200&&strokePixels[p+1]===0&&strokePixels[p+2]===0&&strokePixels[p+3]>0)++redCoverage;
        var centerAlpha=strokeContext.getImageData(28,34,1,1).data[3];
        strokeContext.clearRect(0,0,100,70);strokeContext.strokeText('OOOO',0,52,20);
        var limited=strokeContext.getImageData(25,0,75,70).data,outsideCoverage=0;
        for(var q=3;q<limited.length;q+=4)outsideCoverage+=limited[q];
        return (redCoverage>100)+'|'+centerAlpha+'|'+outsideCoverage;
    )JS",&result,&error)&&result==L"true|0|0",
        L"Canvas strokeText renders actual glyph outlines using stroke color and width, preserves unpainted glyph interiors and respects maxWidth");
    Check(runtime.Execute(LR"JS(
        var reflectedCanvas=document.createElement('canvas'),reflectedContext=reflectedCanvas.getContext('2d');
        var contextPrototype=CanvasRenderingContext2D.prototype,canvasBrandError='',contextBrandError='',illegalContext='';
        try{HTMLCanvasElement.prototype.getContext.call(document.createElement('div'),'2d');}catch(e){canvasBrandError=e.name;}
        try{contextPrototype.fillRect.call({},0,0,1,1);}catch(e){contextBrandError=e.name;}
        try{new CanvasRenderingContext2D();}catch(e){illegalContext=e.name;}
        reflectedCanvas.width=2;reflectedCanvas.height=2;reflectedContext.fillStyle='#0000ff';
        contextPrototype.fillRect.call(reflectedContext,0,0,1,1);
        var bluePixel=contextPrototype.getImageData.call(reflectedContext,0,0,1,1).data;
        var canvasAccessor=Object.getOwnPropertyDescriptor(contextPrototype,'canvas');
        return (reflectedCanvas instanceof HTMLCanvasElement)+'|'+(reflectedContext instanceof CanvasRenderingContext2D)+'|'+
            (Object.getPrototypeOf(reflectedContext)===contextPrototype)+'|'+(reflectedContext.fillRect===contextPrototype.fillRect)+'|'+
            (HTMLCanvasElement.prototype.getContext.call(reflectedCanvas,'2d')===reflectedContext)+'|'+
            (canvasAccessor.get.call(reflectedContext)===reflectedCanvas)+'|'+(reflectedCanvas.getContext(' 2D ')===null)+'|'+
            bluePixel[2]+'|'+canvasBrandError+'|'+contextBrandError+'|'+illegalContext;
    )JS",&result,&error)&&result==L"true|true|true|true|true|true|true|255|TypeError|TypeError|TypeError",
        L"Canvas interfaces expose shared prototype methods which operate on the borrowed receiver's actual backing store and reject incompatible receivers");
    Check(runtime.Execute(LR"JS(
        var inlineElement=document.createElement('div'),inlineStyle=inlineElement.style;
        var styleAccessor=Object.getOwnPropertyDescriptor(HTMLElement.prototype,'style'),styleBrandError='',styleMarker={},styleCaught=false;
        inlineElement.style='width: 37px; color: red';
        try{styleAccessor.get.call({});}catch(e){styleBrandError=e.name;}
        try{styleAccessor.set.call(inlineElement,{toString:function(){throw styleMarker;}});}catch(e){styleCaught=e===styleMarker;}
        var inlineSvg=document.createElementNS('http://www.w3.org/2000/svg','rect'),svgStyle=inlineSvg.style;
        Object.getOwnPropertyDescriptor(SVGElement.prototype,'style').set.call(inlineSvg,'fill: blue');
        return (inlineStyle===inlineElement.style)+'|'+(styleAccessor.get.call(inlineElement)===inlineStyle)+'|'+inlineStyle.width+'|'+
            styleBrandError+'|'+styleCaught+'|'+(svgStyle===inlineSvg.style)+'|'+svgStyle.fill+'|'+Object.keys(inlineElement).includes('$style');
    )JS",&result,&error)&&result==L"true|true|37px|TypeError|true|true|blue|false",
        L"HTML and SVG inline style accessors preserve object identity, forward string assignment to cssText, validate receivers and propagate string conversion errors");
    if(result!=L"true|true|37px|TypeError|true|true|blue|false")std::wcerr<<L"Style reflection result: "<<result<<L"; error: "<<error<<L'\n';
    Check(runtime.Execute(LR"JS(
        var clonedLeaf={value:7},cloneSource={left:clonedLeaf,right:clonedLeaf};cloneSource.self=cloneSource;
        cloneSource.map=new Map();cloneSource.map.set(clonedLeaf,cloneSource);cloneSource.set=new Set([clonedLeaf,cloneSource]);
        var cloneGetterCalls=0;Object.defineProperty(cloneSource,'computed',{enumerable:true,get:function(){++cloneGetterCalls;return clonedLeaf;}});
        cloneSource[Symbol('ignored')]=function(){};
        var cloneResult=structuredClone(cloneSource),cloneFunctionError='',cloneNodeError='';
        try{structuredClone({callback:function(){}});}catch(e){cloneFunctionError=e.name;}
        try{structuredClone(document.body);}catch(e){cloneNodeError=e.name;}
        cloneResult.left.value=9;
        var cloneBytes=new Uint8Array([3,8]),clonedBytes=structuredClone(cloneBytes);clonedBytes[0]=11;
        return (cloneResult!==cloneSource)+'|'+(cloneResult.self===cloneResult)+'|'+(cloneResult.left===cloneResult.right)+'|'+
            (cloneResult.map.get(cloneResult.left)===cloneResult)+'|'+cloneResult.set.has(cloneResult)+'|'+
            (cloneResult.computed===cloneResult.left)+'|'+cloneGetterCalls+'|'+clonedLeaf.value+'|'+cloneFunctionError+'|'+cloneNodeError+'|'+
            (clonedBytes instanceof Uint8Array)+'|'+cloneBytes[0]+'|'+clonedBytes[0]+'|'+(clonedBytes.buffer!==cloneBytes.buffer);
    )JS",&result,&error)&&result==L"true|true|true|true|true|true|1|7|DataCloneError|DataCloneError|true|3|11|true",
        L"Structured cloning preserves cycles and shared graph identity, clones collections and typed bytes independently, evaluates enumerable getters once and rejects nonserializable values");
    Check(runtime.Execute(LR"JS(
        var valueOfObject={},valueOfFunction=function(){},valueOfNullError='',valueOfGetterCalls=0;
        var valueOfNumber=Object.prototype.valueOf.call(3),taggedObject={};
        Object.defineProperty(taggedObject,Symbol.toStringTag,{get:function(){++valueOfGetterCalls;return 'Tagged';}});
        try{Object.prototype.valueOf.call(null);}catch(e){valueOfNullError=e.name;}
        return (valueOfObject.valueOf()===valueOfObject)+'|'+(valueOfFunction.valueOf()===valueOfFunction)+'|'+
            typeof valueOfNumber+'|'+valueOfNumber.valueOf()+'|'+valueOfNullError+'|'+
            Object.prototype.toString.call(taggedObject)+'|'+valueOfGetterCalls+'|'+
            (typeof Object.getOwnPropertyDescriptor(Object.prototype,'valueOf').value)+'|'+Object.keys(Object.prototype).includes('valueOf');
    )JS",&result,&error)&&result==L"true|true|object|3|TypeError|[object Tagged]|1|function|false",
        L"Object prototype conversion methods preserve receiver identity, box primitives, reject null, expose stable descriptors and evaluate custom string tags");
    Check(runtime.Execute(LR"JS(
        var boxedNumber=new Number(7),numberBrandError='';
        try{Number.prototype.valueOf.call({});}catch(e){numberBrandError=e.name;}
        return typeof boxedNumber+'|'+(boxedNumber instanceof Number)+'|'+boxedNumber.valueOf()+'|'+boxedNumber.toFixed(2)+'|'+
            (Object.getPrototypeOf(Object(3))===Number.prototype)+'|'+Number.prototype.valueOf()+'|'+numberBrandError;
    )JS",&result,&error)&&result==L"object|true|7|7.00|true|0|TypeError",
        L"Number wrappers retain primitive values through their shared prototype, distinguish construction from conversion and reject incompatible receivers");
    Check(runtime.Execute(LR"JS(
        var reflectedStyleElement=document.createElement('div'),reflectedStyle=reflectedStyleElement.style;
        var reflectedCssText=Object.getOwnPropertyDescriptor(CSSStyleDeclaration.prototype,'cssText'),styleMethodError='',styleReadOnlyError='';
        reflectedCssText.set.call(reflectedStyle,'width: 41px; --Case: kept');
        CSSStyleDeclaration.prototype.setProperty.call(reflectedStyle,'height','13px','important');
        try{CSSStyleDeclaration.prototype.getPropertyValue.call({},'width');}catch(e){styleMethodError=e.name;}
        var readOnlyStyle=getComputedStyle(reflectedStyleElement);
        try{CSSStyleDeclaration.prototype.setProperty.call(readOnlyStyle,'width','99px');}catch(e){styleReadOnlyError=e.name;}
        return (reflectedStyle instanceof CSSStyleDeclaration)+'|'+(reflectedStyle.getPropertyValue===CSSStyleDeclaration.prototype.getPropertyValue)+'|'+
            reflectedStyle.width+'|'+reflectedStyle.getPropertyValue('--Case')+'|'+reflectedStyle.height+'|'+reflectedStyle.getPropertyPriority('height')+'|'+
            reflectedCssText.get.call(reflectedStyle).includes('41px')+'|'+styleMethodError+'|'+styleReadOnlyError+'|'+readOnlyStyle.cssText+'|'+reflectedStyle.width;
    )JS",&result,&error)&&result==L"true|true|41px|kept|13px|important|true|TypeError|NoModificationAllowedError||41px",
        L"CSS style reflection exposes real shared methods and cssText accessors, preserves custom property case and prevents computed style mutation");
    Check(runtime.Execute(LR"JS(
        var inheritedObject={},nullPrototypeObject=Object.create(null),removedPrototypeObject={};
        removedPrototypeObject.__proto__=null;
        return (Object.getPrototypeOf(inheritedObject)===Object.prototype)+'|'+
            (inheritedObject.toLocaleString===Object.prototype.toLocaleString)+'|'+inheritedObject.toLocaleString()+'|'+
            (typeof nullPrototypeObject.valueOf)+'|'+(typeof nullPrototypeObject.toString)+'|'+(typeof removedPrototypeObject.hasOwnProperty);
    )JS",&result,&error)&&result==L"true|true|[object Object]|undefined|undefined|undefined",
        L"Ordinary objects inherit stable Object prototype methods while explicitly null prototypes do not receive synthetic fallbacks");
    Check(runtime.Execute(LR"JS(
        var consoleProgress=0,cyclicConsoleValue={};cyclicConsoleValue.self=cyclicConsoleValue;
        var borrowedCount=console.count,borrowedTable=console.table,borrowedTrace=console.trace;
        borrowedCount('compatibility');borrowedCount('compatibility');console.countReset('compatibility');
        console.time('compatibility');console.timeLog('compatibility');console.timeEnd('compatibility');
        console.dir(cyclicConsoleValue);console.dirxml(document.body);borrowedTable(cyclicConsoleValue);borrowedTrace('compatibility');
        console.assert(true,'silent');console.assert(false,'recorded');console.group('outer');console.groupCollapsed('inner');
        console.clear();console.groupEnd();console.countReset('missing');console.timeEnd('missing');
        consoleProgress++;return consoleProgress+'|'+typeof console.countReset+'|'+typeof console.dirxml+'|'+typeof borrowedTable(cyclicConsoleValue);
    )JS",&result,&error)&&result==L"1|function|function|undefined",
        L"standard Console counting, timing and inspection calls remain usable when borrowed, given cyclic values, or reset before initialization");
    Check(runtime.Execute(LR"JS(
        var text=document.createTextNode('abcdef'),container=document.createElement('div');container.appendChild(text);
        var range=document.createRange();range.setStart(text,2);range.setEnd(text,5);
        CharacterData.prototype.replaceData.call(text,1,2,'XYZ');
        var adjusted=range.startOffset+'|'+range.endOffset;
        text.insertData(0,'!');text.deleteData(7,100);text.appendData('?');
        var substring=text.substringData(2,100),offsetError='',brandError='';
        try{text.deleteData(-1,0);}catch(e){offsetError=e.name;}
        try{CharacterData.prototype.appendData.call(container,'x');}catch(e){brandError=e.name;}
        range.setStart(text,5);range.setEnd(text,7);var tail=Text.prototype.splitText.call(text,3);
        var split=(tail===container.lastChild)+'|'+(range.startContainer===tail)+'|'+range.startOffset+'|'+range.endOffset;
        var detached=document.createTextNode('abcd'),detachedRange=document.createRange();detachedRange.setStart(detached,3);detachedRange.setEnd(detached,4);
        detached.splitText(2);var detachedPosition=(detachedRange.startContainer===detached)+'|'+detachedRange.startOffset+'|'+detachedRange.endOffset;
        var comment=Document.prototype.createComment.call(document,'comment');comment.appendData('!');comment.data=null;
        tail.data='replacement';tail.nodeValue='actual';
        return adjusted+'|'+substring+'|'+offsetError+'|'+brandError+'|'+split+'|'+detachedPosition+'|'+
            text.data+'|'+tail.textContent+'|'+comment.length+'|'+(text instanceof Text)+'|'+(comment instanceof CharacterData);
    )JS",&result,&error)&&result==L"1|6|XYZde?|IndexSizeError|TypeError|true|true|2|4|true|2|2|!aX|actual|0|true|true",
        L"CharacterData methods and setters change the actual node, validate receivers and offsets, and preserve live ranges across replacement and splitting");
    Check(runtime.Execute(LR"JS(
        var adjacentRoot=document.createElement('section'),adjacentTarget=document.createElement('div');
        adjacentRoot.appendChild(adjacentTarget);document.body.appendChild(adjacentRoot);
        adjacentTarget.appendChild(document.createElement('b'));var retainedChild=adjacentTarget.firstChild;
        Element.prototype.insertAdjacentHTML.call(adjacentTarget,'BEFOREBEGIN','<em>before</em>');
        adjacentTarget.insertAdjacentText('afterbegin','<literal>');
        adjacentTarget.insertAdjacentHTML('beforeend','<span id="adjacent-added">after</span><script>window.adjacentScriptRuns=1;</script>');
        var adjacentElement=document.createElement('i'),returnedElement=adjacentTarget.insertAdjacentElement('afterend',adjacentElement);
        var badPosition='',badHierarchy='';try{adjacentTarget.insertAdjacentHTML('invalid','x');}catch(e){badPosition=e.name;}
        try{adjacentTarget.insertAdjacentElement('beforeend',adjacentRoot);}catch(e){badHierarchy=e.name;}
        var adjacentResult=adjacentRoot.firstChild.tagName+'|'+adjacentTarget.firstChild.textContent+'|'+
            (retainedChild===adjacentTarget.childNodes[1])+'|'+document.getElementById('adjacent-added').textContent+'|'+
            (returnedElement===adjacentRoot.lastChild)+'|'+typeof adjacentScriptRuns+'|'+badPosition+'|'+badHierarchy;
        adjacentRoot.remove();console.groupCollapsed('compatibility');console.groupEnd();return adjacentResult;
    )JS",&result,&error)&&result==L"EM|<literal>|true|after|true|undefined|SyntaxError|HierarchyRequestError",
        L"adjacent HTML, text and element insertion preserves identity and indexes, keeps parsed scripts inert and validates positions and hierarchy");
    Check(runtime.Execute(LR"JS(
        var localized={toString:function(){return this.label;},label:'custom'};
        return 'plain'.toLocaleString()+'|'+Object.prototype.toLocaleString.call(localized)+'|'+
            Node.DOCUMENT_POSITION_FOLLOWING+'|'+document.body.DOCUMENT_POSITION_CONTAINED_BY+'|'+
            Node.DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC+'|'+
            (Node.prototype.compareDocumentPosition.call(document.body,document.getElementById('host'))===20);
    )JS",&result,&error)&&result==L"plain|custom|4|16|32|true",
        L"inherited toLocaleString uses the actual toString method and Node position constants match document ordering");
    Check(runtime.Execute(LR"JS(
        var characterReader=String.prototype.charCodeAt,oldCharacterReader=String.prototype.charAt;
        var directCharacter=characterReader.call('ABC',1),coercedCharacter=characterReader.call(123,1),nullError='';
        try{characterReader.call(null,0);}catch(error){nullError=error.name;}
        String.prototype.charAt=function(){return 'overridden';};var overriddenCharacter='ABC'.charAt(1);
        String.prototype.charAt=oldCharacterReader;
        var longText='A'.repeat(32768),checksum=0;for(var i=0;i<1024;i++)checksum+=longText.charCodeAt(i);
        return directCharacter+'|'+coercedCharacter+'|'+nullError+'|'+overriddenCharacter+'|'+checksum+'|'+
            ('ABC'.charCodeAt===characterReader)+'|'+Number.isNaN(characterReader.call('A',Infinity));
    )JS",&result,&error)&&result==L"66|50|TypeError|overridden|66560|true|true",
        L"String character methods read their actual receiver, retain prototype identity and overrides, and handle large inputs without binding copies");
    float previousGridWidth=0;
    for(const auto scale:{1.0f,1.5f}){
        Document gridDocument;StyleSheet gridStyles;
        Check(gridDocument.Parse(L"<!doctype html><html><head><style>html,body{margin:0;font:14px Arial}.row{display:flex;align-items:center;justify-content:space-between;height:65px}#status{display:grid;grid-template-columns:30px auto;gap:6px;align-items:center}#dot{width:30px;height:30px}#brand{width:80px;height:30px}</style></head><body><div class='row'><div id='status'><div id='dot'></div><div id='word'>\ud655\uc778 \uc911...</div></div><div id='brand'></div></div></body></html>",&error),error.c_str());
        Check(gridStyles.Parse(gridDocument.StyleText(),&error),error.c_str());LayoutEngine gridLayout(gridDocument,gridStyles);gridLayout.Layout(300,65,scale);
        const auto status=gridLayout.BoxFor(gridDocument.GetElementById(L"status")),word=gridLayout.BoxFor(gridDocument.GetElementById(L"word")),brand=gridLayout.BoxFor(gridDocument.GetElementById(L"brand"));
        Check(status&&word&&brand&&status->rect.width>=36+word->rect.width-0.5f&&word->rect.height<30&&word->rect.x+word->rect.width<=brand->rect.x+0.5f,
            L"a grid flex item's intrinsic width includes every column and gap so its status label stays on one line");
        Check(status&&(!previousGridWidth||std::abs(status->rect.width-previousGridWidth)<1),
            L"intrinsic grid widths remain in CSS pixels at 100% and 150% device scales");
        if(status)previousGridWidth=status->rect.width;
    }
    Check(runtime.Execute(LR"JS(
        var originalSelector=Document.prototype.querySelector,selectorCalls=0;
        var wrappedSelector=new Proxy(originalSelector,{apply:function(target,receiver,args){selectorCalls++;return Reflect.apply(target,receiver,args);}});
        Document.prototype.querySelector=wrappedSelector;
        var selected=document.querySelector('#host'),child=Element.prototype.querySelector.call(selected,'span'),wrongReceiver;
        try{Element.prototype.querySelector.call({},'span');}catch(e){wrongReceiver=e.name;}
        Document.prototype.querySelector=originalSelector;
        return selected.id+'|'+child.textContent+'|'+selectorCalls+'|'+wrongReceiver+'|'+
            (Object.getPrototypeOf(selected)===HTMLDivElement.prototype)+'|'+
            (Object.getPrototypeOf(document)===Document.prototype)+'|'+Function.prototype.toString.call(wrappedSelector);
    )JS",&result,&error)&&result==L"host|light|1|TypeError|true|true|function () { [native code] }",
        L"DOM prototype methods preserve brand checks and receivers when borrowed or wrapped in a callable Proxy");
    Document frameDocument;
    Check(frameDocument.Parse(L"<!doctype html><html><body></body></html>",&error),error.c_str());
    auto frameRuntime=std::make_shared<JavaScriptRuntime>(frameDocument);frameRuntime->SetLocation(L"about:blank");
    runtime.SetFrameRuntimeProvider([frameRuntime](const std::shared_ptr<Node>&){return frameRuntime;});
    Check(runtime.Execute(LR"JS(
        var realmFrame=document.createElement('iframe');document.body.appendChild(realmFrame);
        var frameEval=realmFrame.contentWindow.eval;
        frameEval('var frameCounter=7;function frameFunction(){return function(){return [frameCounter,this===window];};}');
        var foreignFunction=realmFrame.contentWindow.frameFunction(),foreignResult=foreignFunction();
        realmFrame.contentWindow.frameCounter=9;
        return typeof frameEval+'|'+typeof frameCounter+'|'+foreignResult.join(',')+'|'+foreignFunction().join(',')+'|'+
            (frameEval('this')===realmFrame.contentWindow)+'|'+(realmFrame.contentWindow.eval===frameEval);
    )JS",&result,&error)&&result==L"function|undefined|7,true|9,true|true|true",
        L"iframe intrinsic functions, nested closures, window identity and global writes preserve the originating realm");
    frameRuntime->SetLocation(L"https://other.test/page");
    Check(runtime.Execute(L"realmFrame.src='https://other.test/page';try{var blockedEval=realmFrame.contentWindow.eval;return typeof blockedEval;}catch(e){return e.name;}",&result,&error)&&result==L"SecurityError",
        L"frame intrinsic access rejects another origin");
    Check(runtime.Execute(L"frameEval=null;foreignFunction=null;foreignResult=null;realmFrame.remove();realmFrame=null;",&result,&error),error.c_str());
    runtime.SetFrameRuntimeProvider({});
    Check(runtime.Execute(LR"JS(
        var position=Number.NaN,iterations=0;
        while(!Number.isNaN(position)){if(++iterations>1)break;position++;}
        return typeof position+'|'+Number.isNaN(position)+'|'+iterations+'|'+
            (Number.POSITIVE_INFINITY===Infinity)+'|'+(Number.NEGATIVE_INFINITY===-Infinity)+'|'+
            (Number.MAX_VALUE>1e308)+'|'+(Number.MIN_VALUE>0)+'|'+(1+Number.EPSILON>1);
    )JS",&result,&error)&&result==L"number|true|0|true|true|true|true|true",
        L"Number constants retain numeric types and terminate NaN sentinel loops");
    Check(runtime.Execute(L"return Number.isInteger(Date.now())+'|'+Number.isInteger(new Date().getTime())+'|'+new Date(1.9).getTime()+'|'+new Date(-1.9).getTime()+'|'+Number.isNaN(new Date(8640000000000001).getTime());",&result,&error)&&result==L"true|true|1|-1|true",
        L"Date timestamps use integer milliseconds and TimeClip bounds rather than fractional performance clock values");
    Check(runtime.Execute(LR"JS(
        var borrowed=Array.prototype.join;
        var body=borrowed.call(['(',')','{return 17;}'],'');
        var dynamic=eval('(function generated'+body+')');
        return dynamic()+'|'+borrowed.call({0:'a',2:'c',length:3},'-')+'|'+borrowed.call('abc','-')+'|'+[1,2].join(undefined);
    )JS",&result,&error)&&result==L"17|a--c|a-b-c|1,2",
        L"borrowed Array.join reads the actual array-like receiver and preserves generated JavaScript source");
    Check(runtime.Execute(LR"JS(
        var append=String.prototype.concat,cut=String.prototype.slice;
        var source=append.call('function generated','(){return 23;}');
        return eval('('+source+')')()+'|'+cut.call('abcdef',1,-1)+'|'+String.prototype.charCodeAt.call('AZ',1)+'|'+
            String.prototype.replace.call('hello','l','X')+'|'+String.prototype.split.call('a,b',',').length;
    )JS",&result,&error)&&result==L"23|bcde|90|heXlo|2",
        L"borrowed String prototype methods coerce and read their actual receiver");
    Check(runtime.Execute(LR"JS(
        var repeatErrors=[];
        for(var count of [-1,Infinity]){try{'a'.repeat(count);}catch(e){repeatErrors.push(e.name);}}
        return 'a'.repeat(undefined).length+'|'+String.prototype.repeat.call('ab',NaN).length+'|'+
            'x'.repeat(-0.7).length+'|'+''.repeat(1e12).length+'|'+String.prototype.repeat.call('ab',2.9)+'|'+repeatErrors.join(',');
    )JS",&result,&error)&&result==L"0|0|0|0|abab|RangeError,RangeError",
        L"String.repeat applies integer coercion before validating counts and handles empty strings");
    Check(runtime.Execute(LR"JS(
        var takeLast=[].pop,takeFirst=[].shift,prepend=[].unshift,reverse=[].reverse,items=[1,2,3];
        var last=takeLast.call(items),first=takeFirst.call(items),size=prepend.call(items,4,5);
        reverse.call(items);return last+'|'+first+'|'+size+'|'+items.join(',')+'|'+Array.prototype.length;
    )JS",&result,&error)&&result==L"3|1|3|2,5,4|0",
        L"borrowed array mutations read and modify the actual receiver rather than the method lookup array");
    Check(runtime.Execute(LR"JS(
        var wide=BigInt('0xffffffffffffffffffffffffffffffff'),incremented=wide+1n;
        var negative=-13n,errors=[];
        for(var action of [function(){return 1n+1;},function(){return +1n;},function(){return 1n/0n;},function(){return 1n>>>1n;},function(){return BigInt(1.5);},function(){return new BigInt(1);}]){
            try{action();}catch(e){errors.push(e.name);}
        }
        return typeof wide+'|'+incremented.toString(16)+'|'+(wide*wide%97n)+'|'+
            (negative/5n)+'|'+(negative%5n)+'|'+(negative>>2n)+'|'+(~negative)+'|'+
            (-1n&wide).toString(16)+'|'+(BigInt('9007199254740993')>9007199254740992)+'|'+
            (0n==false)+'|'+Number(255n)+'|'+errors.join(',');
    )JS",&result,&error)&&result==L"bigint|100000000000000000000000000000000|89|-2|-3|-4|12|ffffffffffffffffffffffffffffffff|true|true|255|TypeError,TypeError,RangeError,TypeError,RangeError,TypeError",
        L"BigInt keeps arbitrary precision arithmetic, signed shifts, mixed numeric comparisons and exception semantics");
    Check(runtime.Execute(LR"JS(
        function arity(first,second=1,...rest){}function pair(first,second){}
        return arity.name+'|'+arity.length+'|'+pair.length+'|'+(pair.constructor===Function)+'|'+atob.name+'|'+eval.length;
    )JS",&result,&error)&&result==L"arity|1|2|true|atob|1",L"function names, declared arity, intrinsic names and the Function prototype constructor are exposed");
    Check(runtime.Execute(LR"JS(
        function addBound(a,b){return this.offset+a+b;}var bound=Function.prototype.bind.call(addBound,{offset:3},4);
        function BoundModel(value){this.value=value;}var model=new (BoundModel.bind(null,9))();
        return bound.name+'|'+bound.length+'|'+bound(5)+'|'+model.value+'|'+(model instanceof BoundModel)+'|'+addBound.hasOwnProperty('length');
    )JS",&result,&error)&&result==L"bound addBound|1|12|9|true|true",L"bound functions preserve declared arity, receivers, leading arguments and constructor behavior");
    Check(runtime.Execute(LR"JS(
        var syntax;try{eval('function incomplete');}catch(error){syntax=error;}
        return (syntax instanceof SyntaxError)+'|'+(syntax instanceof Error)+'|'+(syntax instanceof TypeError)+'|'+
            (syntax.constructor===SyntaxError)+'|'+syntax.constructor.name+'|'+Object.keys(syntax).length;
    )JS",&result,&error)&&result==L"true|true|false|true|SyntaxError|0",L"caught syntax errors retain their specific constructor, prototype and nonenumerable fields");
    Check(runtime.Execute(LR"JS(
        var timerResult='',timerObject={value:8};
        setTimeout(function(object,amount){'use strict';timerResult=object.value+amount+'|'+(object===timerObject)+'|'+(this===window);},0,timerObject,4);
    )JS",nullptr,&error),error.c_str());
    runtime.RunTimers();
    Check(runtime.Execute(L"return timerResult;",&result,&error)&&result==L"12|true|true",L"timer callbacks receive extra arguments and the global receiver even in strict mode");
    Check(runtime.Execute(L"var animationTime=-1;requestAnimationFrame(function(time){animationTime=time;});",nullptr,&error),error.c_str());
    runtime.RunAnimationFrame();
    Check(runtime.Execute(L"return animationTime>=0&&Math.abs(animationTime-performance.now())<1000;",&result,&error)&&result==L"true",
        L"animation frame timestamps share the performance clock's time origin");
    Check(runtime.Execute(LR"JS(
        var invalid=[];for(var input of ['A','A===','AA=A','AA!']){try{atob(input);}catch(e){invalid.push(e.name);}}
        var rejected=false;try{btoa('\u0100');}catch(e){rejected=e.name==='InvalidCharacterError';}
        return btoa('\x00\xffabc')+'|'+atob(' YW\nJj ').length+'|'+atob('YWI')+'|'+invalid.length+'|'+rejected+'|'+(globalThis===window);
    )JS",&result,&error)&&result==L"AP9hYmM=|3|ab|4|true|true",L"Base64 helpers preserve binary bytes, accept ASCII whitespace and reject invalid input");
    Check(runtime.Execute(LR"JS(
        function localEval(){
            var count=4;let increment=8;eval('count+=increment;var added=7;');
            var scoped=eval("'use strict';var privateValue=5;privateValue+count");
            return count+'|'+added+'|'+scoped+'|'+typeof privateValue;
        }
        function indirectEval(){let privateValue=9;return (0,eval)('typeof privateValue');}
        function strictEval(){'use strict';eval('var isolated=1');return typeof isolated;}
        var marker={},same=false;try{eval('throw marker');}catch(error){same=error===marker;}
        return localEval()+'|'+indirectEval()+'|'+strictEval()+'|'+eval(23)+'|'+(eval(marker)===marker)+'|'+same+'|'+eval('1;{2+3;}');
    )JS",&result,&error)&&result==L"12|7|17|undefined|undefined|undefined|23|true|true|5",
        L"eval preserves direct scope, strict isolation, indirect global scope, completions and thrown identity");
    Check(runtime.Execute(LR"JS(
        var outer=7;function makeDynamic(){var outer=99;return new Function('value','return outer+value');}
        return makeDynamic()(3)+'|'+Function('return 12')()+'|'+new Function()();
    )JS",&result,&error)&&result==L"10|12|undefined",L"Function constructors compile parameter and body strings in the global environment");
    Check(runtime.Execute(LR"JS(
        function serializeMe (value) { /* preserved */ return value+1; }
        var rebuilt=(0,eval)('('+serializeMe.toString()+')'),arrow=value=>value*2;
        return (serializeMe.toString()==='function serializeMe (value) { /* preserved */ return value+1; }')+'|'+rebuilt(3)+'|'+(0,eval)('('+String(arrow)+')')(5);
    )JS",&result,&error)&&result==L"true|4|10",L"function source serialization preserves executable bodies, comments and arrow syntax");
    Check(runtime.Execute(LR"JS(
        var host=document.getElementById('host'),shadow=host.attachShadow({mode:'closed'});
        shadow.innerHTML='<style>.panel{width:60px;height:30px;background:#008800}</style><div id="inside" class="panel"></div>';
        var inside=shadow.querySelector('#inside');
        return document.title+'|'+(host.shadowRoot===null)+'|'+(shadow instanceof ShadowRoot)+'|'+
            (inside.getRootNode()===shadow)+'|'+(inside.getRootNode({composed:true})===document)+'|'+inside.isConnected+'|'+
            (document.getElementById('inside')===null)+'|'+(document.querySelectorAll('div') instanceof NodeList)+'|'+
            (document.getElementsByTagName('div') instanceof HTMLCollection);
    )JS",&result,&error)&&result==L"Embedded script|true|true|true|true|true|true|true|true",
        L"closed shadow trees preserve DOM boundaries, connection, root identity and collection interfaces");
    const auto host=document.GetElementById(L"host");
    Check(host&&host->shadowRoot&&host->shadowRoot->children.size()==2,
        L"a closed shadow root owns its children without adding them to the light tree index");
    Document templateDocument;Check(templateDocument.Parse(L"<body><template id='parsed-template'><p id='inert'>Inert</p></template><main id='template-target'></main></body>",&error),error.c_str());
    JavaScriptRuntime templateRuntime(templateDocument);std::wstring templateResult;
    Check(templateRuntime.Execute(LR"JS(
        var parsed=document.getElementById('parsed-template'),content=parsed.content;
        var clone=parsed.cloneNode(true),target=document.getElementById('template-target');
        var inert=document.getElementById('inert')===null;
        target.appendChild(content);
        return inert+'|'+(content instanceof DocumentFragment)+'|'+content.nodeType+'|'+
            (content.parentNode===null)+'|'+parsed.childNodes.length+'|'+content.childNodes.length+'|'+
            target.textContent+'|'+clone.content.textContent+'|'+(clone.content!==content);
    )JS",&templateResult,&error)&&templateResult==L"true|true|11|true|0|0|Inert|Inert|true",
        L"parsed template content is inert, branded, independently cloneable and consumed as a fragment");
    Check(templateRuntime.Execute(LR"JS(
        var dynamic=document.createElement('template'),fragment=dynamic.content;
        dynamic.innerHTML='<p>Details <a href="/help">Help</a></p>';
        var html=dynamic.innerHTML,first=fragment.firstChild;
        dynamic.innerHTML='<span>Replacement</span>';
        var replaced=dynamic.content===fragment&&first.parentNode===null;
        target.appendChild(fragment);
        var light=document.createElement('span');light.textContent='Light';dynamic.appendChild(light);
        return (html.indexOf('<p>Details ')===0)+'|'+replaced+'|'+target.textContent+'|'+
            dynamic.content.childNodes.length+'|'+dynamic.childNodes.length;
    )JS",&templateResult,&error)&&templateResult==L"true|true|InertReplacement|0|1",
        L"template innerHTML preserves fragment identity and separates content from light-DOM children");
    StyleSheet sheet;Check(sheet.Parse(document.StyleText(),&error),error.c_str());
    LayoutEngine layout(document,sheet);
    for(const auto scale:{1.0f,1.5f}){
        const auto raster=CaptureBoxRaster(layout,scale,100,60);
        const auto inside=host&&host->shadowRoot?host->shadowRoot->children.back():std::shared_ptr<Node>{};
        const auto box=layout.BoxFor(inside);
        Check(raster.rendered&&raster.ColorAt(10,10,scale)==0x008800&&box&&
            std::abs(box->rect.width-60)<0.01f&&std::abs(box->rect.height-30)<0.01f&&layout.HitTest(10,10)==inside,
            L"shadow CSS stays scoped while geometry, painting and hit testing agree at 100% and 150% DPI");
    }
    Check(runtime.Execute(LR"JS(
        var iterator=document.createNodeIterator(host,NodeFilter.SHOW_ELEMENT|NodeFilter.SHOW_TEXT);
        var names=[],node;while((node=iterator.nextNode())!==null)names.push(node.nodeName);
        var last=iterator.previousNode();
        var filtered=document.createNodeIterator(host,NodeFilter.SHOW_ELEMENT,function(n){return n===host?NodeFilter.FILTER_SKIP:NodeFilter.FILTER_ACCEPT;});
        return names.join(',')+'|'+last.nodeType+'|'+iterator.pointerBeforeReferenceNode+'|'+filtered.nextNode().tagName+'|'+(filtered.nextNode()===null);
    )JS",&result,&error)&&result==L"DIV,SPAN,#text|3|true|SPAN|true",
        L"NodeIterator includes its root, filters nodes, reverses direction and stays within the light tree");
    Check(runtime.Execute(LR"JS(
        var sequence=[],source=[1,2],it=source.values();sequence.push(it.next().value);source.push(3);
        sequence.push(it.next().value,it.next().value,it.next().done);
        var params=new URLSearchParams('a=1&a=2'),values=[];params.forEach(function(v,k){values.push(k+v);});
        var symbol=Symbol('private'),object={[symbol]:7};
        return sequence.join(',')+'|'+Array.from(new Map([['x',4]])).map(e=>e.join(':')).join(',')+'|'+
            Array.from(params).map(e=>e.join(':')).join(',')+'|'+values.join(',')+'|'+(Object.getOwnPropertySymbols(object)[0]===symbol);
    )JS",&result,&error)&&result==L"1,2,3,true|x:4|a:1,a:2|a1,a2|true",
        L"iterators, URLSearchParams enumeration and symbol inspection work for embedded script inputs");
    runtime.SetResourceLoader([](const std::wstring&,std::wstring& body){body=L"fixture";return true;});
    Check(runtime.Execute(LR"JS(
        var deliveries=[],observer=new PerformanceObserver(function(list,owner){
            deliveries.push(list.getEntries().map(e=>e.entryType+':'+e.name).join(','));
        });
        performance.mark('begin',{startTime:4});performance.mark('end',{startTime:14});
        observer.observe({type:'mark',buffered:true});observer.observe({type:'measure'});
        var measure=performance.measure('elapsed','begin','end');
        return measure.duration+'|'+performance.getEntriesByType('mark').length+'|'+
            (measure instanceof PerformanceMeasure)+'|'+(performance.now()>=0)+'|'+deliveries.length;
    )JS",&result,&error)&&result==L"10|2|true|true|0",
        L"Performance API stores real entries and queues observer delivery after script execution");
    runtime.RunTimers();
    Check(runtime.Execute(L"observer.disconnect();performance.clearMarks('begin');return deliveries.join('|')+'|'+performance.getEntriesByType('mark').length;",
        &result,&error)&&result==L"mark:begin,measure:elapsed,mark:end|1",
        L"PerformanceObserver sorts buffered entries and supports disconnect and selective clearing");
    runtime.SetRequestLoader([](const ScriptRequest& request){
        ScriptResponse response;response.status=200;response.url=request.url;response.body=L"fixture";
        response.requestStartOffsetMs=0;response.responseStartOffsetMs=0;
        response.encodedBodySize=5;response.decodedBodySize=7;
        response.timingAllowed=request.url.find(L"protected")==std::wstring::npos;return response;
    });
    Check(runtime.Execute(LR"JS(
        performance.clearResourceTimings();
        var timed=new XMLHttpRequest();timed.open('GET','/measured',false);timed.send();
        var protectedRequest=new XMLHttpRequest();protectedRequest.open('GET','/protected',false);protectedRequest.send();
        var entries=performance.getEntriesByType('resource'),t=entries[0],hidden=entries[1],json=t.toJSON();
        return (t.requestStart>=t.startTime&&t.responseStart>=t.requestStart&&t.responseEnd>=t.responseStart)+'|'+
            t.encodedBodySize+'|'+t.decodedBodySize+'|'+t.responseStatus+'|'+t.transferSize+'|'+
            (hidden.requestStart===0&&hidden.responseStart===0&&hidden.encodedBodySize===0&&hidden.decodedBodySize===0)+'|'+
            (json.decodedBodySize===7&&Array.isArray(json.serverTiming)&&t.connectStart===0);
    )JS",&result,&error)&&result==L"true|5|7|200|0|true|true",
        L"resource timing reports measured phases and byte counts while masking protected or unavailable information");
    runtime.SetRequestLoader({});
    Check(runtime.Execute(LR"JS(
        function capturedReader(){var current={value:1};return {
            read:function(){return current;},write:function(v){current=v;}
        };}
        var reader=capturedReader(),sum=0;for(var hot=0;hot<2000;hot++)sum+=reader.read().value;
        var changed={value:9};reader.write(changed);
        var selfReader=function namedReader(){return namedReader;};
        function argumentsReader(){return arguments;}
        return sum+'|'+(reader.read()===changed)+'|'+(selfReader()===selfReader)+'|'+argumentsReader(7)[0];
    )JS",&result,&error)&&result==L"2000|true|true|7",
        L"short closure reads preserve live binding updates, function identity and ordinary arguments semantics");
    Check(runtime.Execute(LR"JS(
        function directArguments(x){return eval('arguments[0]');}
        function capturedArguments(x){return ()=>arguments[0];}
        function nestedArguments(x){return function(){return arguments[0];};}
        function templateArguments(x){return `${arguments[0]}`;}
        return directArguments(11)+'|'+capturedArguments(12)()+'|'+nestedArguments(0)(13)+'|'+templateArguments(14);
    )JS",&result,&error)&&result==L"11|12|13|14",
        L"arguments allocation optimization retains direct eval, closures, nested calls and template expressions");
    Check(runtime.Execute(LR"JS(
        var numericCollator=new Intl.Collator('en-US',{numeric:true,sensitivity:'base',ignorePunctuation:true});
        var ordinaryCollator=new Intl.Collator('en-US'),compare=numericCollator.compare,options=numericCollator.resolvedOptions();
        var invalidCollation=false;try{new Intl.Collator('en-US',{sensitivity:'unknown'});}catch(e){invalidCollation=e instanceof RangeError;}
        return (compare('2','10')<0)+'|'+(compare('cafe','caf\u00e9')===0)+'|'+(compare('a-b','ab')===0)+'|'+
            (ordinaryCollator.compare('2','10')>0)+'|'+(compare===numericCollator.compare)+'|'+
            (numericCollator instanceof Intl.Collator)+'|'+options.numeric+'|'+options.locale+'|'+invalidCollation;
    )JS",&result,&error)&&result==L"true|true|true|true|true|true|true|en-US|true",
        L"Intl.Collator uses OS ICU for numeric, accent and punctuation collation with stable bound comparisons");
    Document historyDocument;historyDocument.Parse(L"<!doctype html><html><body><p id='history-retained'>retained</p></body></html>",&error);
    JavaScriptRuntime historyRuntime(historyDocument);historyRuntime.SetLocation(L"https://history.example/path/start");
    std::vector<std::wstring> historyNotifications;std::wstring historyReload;
    historyRuntime.SetSameDocumentNavigationSink([&](const std::wstring& url,bool replace,int delta){
        historyNotifications.push_back(url+L"|"+(replace?L"replace":L"push")+L"|"+std::to_wstring(delta));
    });
    historyRuntime.SetNavigationSink([&](const std::wstring& url){historyReload=url;});
    Check(historyRuntime.Execute(LR"JS(
        var retainedHistoryNode=document.getElementById('history-retained'),historyEvents=[];
        addEventListener('popstate',function(event){historyEvents.push(event.state&&event.state.value);});
        var historyData={value:3};historyData.self=historyData;
        history.replaceState(historyData,'','?first=1');historyData.value=99;history.state.value=4;
        history.pushState({value:8},'','#second');history.scrollRestoration='manual';
        var historySecurity='',historyClone='',historyBrand='';
        try{history.replaceState({},'','https://example.org/');}catch(e){historySecurity=e.name;}
        try{history.pushState({fn:function(){}},'');}catch(e){historyClone=e.name;}
        try{History.prototype.pushState.call({},null,'');}catch(e){historyBrand=e.name;}
        history.length=900;return history.length+'|'+history.state.value+'|'+location.href+'|'+historySecurity+'|'+historyClone+'|'+historyBrand+'|'+(history instanceof History);
    )JS",&result,&error)&&result==L"2|8|https://history.example/path/start?first=1#second|SecurityError|DataCloneError|TypeError|true",
        L"History state methods change same-document URLs, retain independent serialized state and reject cross-origin URLs, unserializable data and incorrect receivers");
    Check(historyRuntime.Execute(L"history.back();return history.state.value+'|'+historyEvents.length;",&result,&error)&&result==L"8|0",
        L"History traversal queues a task instead of changing the document inside the current script job");
    historyRuntime.RunTimers();
    Check(historyRuntime.Execute(L"return history.state.value+'|'+(history.state.self===history.state)+'|'+location.hash+'|'+historyEvents.join(',')+'|'+(retainedHistoryNode===document.getElementById('history-retained'));",&result,&error)&&result==L"3|true||3|true"&&historyNotifications.size()==3,
        L"Same-document traversal restores a fresh state graph, dispatches popstate and preserves the existing DOM and JavaScript realm");
    historyRuntime.Execute(L"history.pushState({value:12},'','?replacement');history.go();",nullptr,&error);historyRuntime.RunTimers();
    Check(!historyRuntime.CanTraverseHistory(1)&&historyReload==L"https://history.example/path/start?replacement",
        L"A new history entry truncates forward entries and go(0) requests a real reload");
    unsigned yieldCount=0;runtime.SetExecutionYieldHandler([&]{++yieldCount;return false;});
    Check(!runtime.Execute(L"while(true){}",nullptr,&error)&&error.find(L"AbortError")!=std::wstring::npos&&yieldCount==1,
        L"host safepoints interrupt an infinite script without blocking the owner thread indefinitely");
    runtime.SetExecutionYieldHandler({});
    Check(runtime.Execute(L"return 42;",&result,&error)&&result==L"42",
        L"the runtime remains usable after a host-interrupted script unwinds");
    Check(runtime.Execute(LR"JS(
        var ns='http://www.w3.org/2000/svg',svg=document.createElementNS(ns,'svg'),group=document.createElementNS(ns,'g');
        var rectangle=document.createElementNS(ns,'rect');
        rectangle.setAttribute('x','10');rectangle.setAttribute('y','20');rectangle.setAttribute('width','30');rectangle.setAttribute('height','40');
        rectangle.setAttribute('transform','translate(400 500)');
        var own=rectangle.getBBox();rectangle.removeAttribute('transform');group.appendChild(rectangle);svg.appendChild(group);
        group.setAttribute('transform','translate(10 20) scale(2)');
        var descendants=svg.getBBox(),local=group.getBBox(),receiverError=false;
        try{SVGGraphicsElement.prototype.getBBox.call(document.body);}catch(e){receiverError=e instanceof TypeError;}
        return [own.x,own.y,own.width,own.height,descendants.x,descendants.y,descendants.width,descendants.height,local.x,local.y].join(',')+'|'+
            (rectangle instanceof SVGGraphicsElement)+'|'+(svg instanceof SVGSVGElement)+'|'+receiverError;
    )JS",&result,&error)&&result==L"10,20,30,40,30,60,60,80,10,20|true|true|true",
        L"SVG object bounds use user coordinates, descendant transforms and validated prototype receivers");
    Check(runtime.Execute(LR"JS(
        var curve=document.createElementNS(ns,'path');curve.setAttribute('d','M0 0 Q10 20 20 0');
        var cb=curve.getBBox(),label=document.createElementNS(ns,'text');
        label.setAttribute('x','100');label.setAttribute('y','50');label.textContent='Actual glyph measurement';
        label.style.fontSize='20px';var tb=label.getBBox();
        label.textContent+=' additional characters';var wider=label.getBBox();
        return (Math.abs(cb.width-20)<0.1&&Math.abs(cb.height-10)<0.1)+'|'+
            (tb.x===100&&tb.y<50&&tb.width>0&&tb.height>0&&wider.width>tb.width)+'|'+
            (typeof document.createElement('div').getBBox==='undefined');
    )JS",&result,&error)&&result==L"true|true|true",
        L"SVG curve extrema and detached text bounds are computed using Direct2D and DirectWrite");
    const auto frame=document.CreateElement(L"iframe");frame->SetAttribute(L"src",L"https://child.test/page");
    Check(runtime.Execute(L"var messages=[];window.addEventListener('message',e=>messages.push(e.isTrusted+'|'+e.origin+'|'+e.data.kind));",nullptr,&error),error.c_str());
    Check(runtime.DispatchWindowMessageAsJson(L"{\"kind\":\"ready\"}",frame,L"https://child.test",&error),error.c_str());
    Check(runtime.Execute(L"return messages.join(',')+'|'+new Event('message').isTrusted;",&result,&error)&&
        result==L"true|https://child.test|ready|false",
        L"browser-delivered frame messages are trusted while script-created events remain untrusted");
    Check(runtime.Execute(LR"JS(
        var writes=[],releaseWrite,pipeComplete=false,streamController;
        var readable=new ReadableStream({start:function(controller){streamController=controller;controller.enqueue(1);controller.enqueue(2);controller.close();}});
        var writable=new WritableStream({write:function(value){writes.push(value);if(value===1)return new Promise(function(resolve){releaseWrite=resolve;});},close:function(){writes.push('closed');}});
        readable.pipeTo(writable).then(function(){pipeComplete=true;});
        return readable.locked+'|'+writable.locked+'|'+pipeComplete;
    )JS",&result,&error)&&result==L"true|true|false",L"pipeTo locks both streams and returns an asynchronous completion");
    Check(runtime.Execute(L"return writes.join(',')+'|'+pipeComplete;",&result,&error)&&result==L"1|false",
        L"stream piping waits for a pending sink write before reading the next chunk");
    Check(runtime.Execute(L"releaseWrite();",nullptr,&error),error.c_str());
    Check(runtime.Execute(L"return writes.join(',')+'|'+pipeComplete+'|'+readable.locked+'|'+writable.locked;",
        &result,&error)&&result==L"1,2,closed|true|false|false",L"pipeTo closes the destination and releases both locks after all writes finish");
    Check(runtime.Execute(LR"JS(
        var cancelled,pipeError,streamError={kind:'sink'};
        var failedSource=new ReadableStream({start:function(c){c.enqueue(3);},cancel:function(reason){cancelled=reason;}});
        failedSource.pipeTo(new WritableStream({write:function(){throw streamError;}})).catch(function(error){pipeError=error;});
    )JS",nullptr,&error),error.c_str());
    Check(runtime.Execute(L"return (cancelled===streamError)+'|'+(pipeError===streamError)+'|'+failedSource.locked;",
        &result,&error)&&result==L"true|true|false",L"a failed sink cancels its source and preserves the original rejection value");
    Check(runtime.Execute(LR"JS(
        var xhrEvents=[],eventRequest=new XMLHttpRequest();eventRequest.open('GET','/fixture',false);
        eventRequest.addEventListener('load',function(e){xhrEvents.push(e.type+':'+(e.target===eventRequest)+':'+e.isTrusted);});
        eventRequest.addEventListener('loadend',function(e){xhrEvents.push(e.type);});eventRequest.send();
        return xhrEvents.join('|');
    )JS",&result,&error)&&result==L"load:true:true|loadend",L"XMLHttpRequest delivers registered load and loadend listeners with the request as target");
    Check(runtime.Execute(LR"JS(
        var workerMessages=[],blob=new Blob(["onmessage=function(event){postMessage({value:event.data.value+1,isolated:typeof document==='undefined'&&typeof window==='undefined',same:this===self});};"],{type:'application/javascript'});
        var workerUrl=URL.createObjectURL(blob),worker=new Worker(workerUrl);
        worker.onmessage=function(event){workerMessages.push(event.data.value+'|'+event.data.isolated+'|'+event.data.same+'|'+event.isTrusted);};
        worker.postMessage({value:41});URL.revokeObjectURL(workerUrl);
        return workerMessages.length+'|'+(worker instanceof Worker)+'|'+(blob instanceof Blob);
    )JS",&result,&error)&&result==L"0|true|true",
        L"Blob-backed workers start asynchronously and retain their source after URL revocation");
    const auto workerDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do{
        runtime.RunTimers();Sleep(1);
        Check(runtime.Execute(L"return workerMessages.join(',');",&result,&error),error.c_str());
    }while(result.empty()&&std::chrono::steady_clock::now()<workerDeadline);
    Check(result==L"42|true|true|true",L"workers execute in an isolated background realm and deliver trusted cloned messages");
    Check(runtime.Execute(L"worker.terminate();",nullptr,&error),error.c_str());
    Check(runtime.Execute(LR"JS(
        var closingMessage='',closingUrl=URL.createObjectURL(new Blob(["postMessage('finished');close();"]));
        var closingWorker=new Worker(closingUrl);closingWorker.onmessage=function(e){closingMessage=e.data;};
        URL.revokeObjectURL(closingUrl);
    )JS",nullptr,&error),error.c_str());
    const auto closingDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do{runtime.RunTimers();Sleep(1);runtime.Execute(L"return closingMessage;",&result,&error);}
    while(result.empty()&&std::chrono::steady_clock::now()<closingDeadline);
    Check(result==L"finished",L"messages posted before a worker closes are delivered after its realm exits");
    Check(runtime.Execute(LR"JS(
        var immediateUrl=URL.createObjectURL(new Blob(['onmessage=function(){};']));
        for(var i=0;i<8;i++){var immediateWorker=new Worker(immediateUrl);immediateWorker.terminate();}
        URL.revokeObjectURL(immediateUrl);return true;
    )JS",&result,&error)&&result==L"true",L"immediate worker termination safely interrupts realm initialization");
    Document loopWorkerDocument;loopWorkerDocument.Parse(L"<html><body></body></html>",&error);
    JavaScriptRuntime loopWorkerRuntime(loopWorkerDocument);
    Check(loopWorkerRuntime.Execute(LR"JS(
        var loopWorkerReady=false;
        var loopWorkerUrl=URL.createObjectURL(new Blob(["function hotLoop(n){let total=0;for(let i=0;i<n;i++){total+=i;}return total;}for(let warm=0;warm<100;warm++){hotLoop(10);}postMessage('ready');hotLoop(Infinity);"]));
        var loopWorker=new Worker(loopWorkerUrl);loopWorker.onmessage=function(){loopWorkerReady=true;};URL.revokeObjectURL(loopWorkerUrl);
    )JS",nullptr,&error),error.c_str());
    const auto loopWorkerDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do{loopWorkerRuntime.RunTimers();Sleep(1);loopWorkerRuntime.Execute(L"return loopWorkerReady;",&result,&error);}
    while(result!=L"true"&&std::chrono::steady_clock::now()<loopWorkerDeadline);
    Check(result==L"true",L"a worker reaches a warmed numeric loop before termination is tested");
    const auto workerStopStarted=std::chrono::steady_clock::now();loopWorkerRuntime.Clear();
    Check(std::chrono::steady_clock::now()-workerStopStarted<std::chrono::seconds(2),
        L"runtime teardown interrupts a worker's warmed infinite numeric loop instead of waiting inside uncancellable JIT code");
    return failures==before?0:1;
}

struct ExecutionWindowProbe {
    HWND page=nullptr;
    HWND childPage=nullptr;
    bool scriptActive=false;
    unsigned nativeDown=0,hostResize=0,hostTimer=0,hostClose=0;
    bool reenteredHost=false,dpiRectangleCopied=false,registeredQueryAnswered=false;
    UINT registeredQuery=0;
};

LRESULT CALLBACK ExecutionWindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
    auto* probe=reinterpret_cast<ExecutionWindowProbe*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){
        probe=static_cast<ExecutionWindowProbe*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(probe));
    }
    if(probe&&message==probe->registeredQuery)return 0x1234;
    if(probe)switch(message){
    case WM_NCLBUTTONDOWN:{
        ++probe->nativeDown;
        probe->registeredQueryAnswered=SendMessageW(window,probe->registeredQuery,0,0)==0x1234;
        // Simulate messages delivered by the native modal move/size loop.
        SendMessageW(window,WM_SIZE,SIZE_RESTORED,MAKELPARAM(660,420));
        SendMessageW(window,WM_TIMER,991,0);
        RECT suggested{11,22,673,444};
        SendMessageW(window,WM_DPICHANGED,MAKELONG(144,144),reinterpret_cast<LPARAM>(&suggested));
        SendMessageW(probe->page,WM_TIMER,0x5747,0);
        SendMessageW(probe->page,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,20));
        SendMessageW(probe->page,WM_LBUTTONUP,0,MAKELPARAM(20,20));
        if(probe->childPage)SendMessageW(probe->childPage,WM_TIMER,0x5747,0);
        suggested={0,0,0,0};return 0;
    }
    case WM_SIZE:if(probe->page){++probe->hostResize;probe->reenteredHost|=probe->scriptActive;}return 0;
    case WM_TIMER:++probe->hostTimer;probe->reenteredHost|=probe->scriptActive;return 0;
    case WM_CLOSE:++probe->hostClose;return 0;
    case WM_DPICHANGED:{
        const auto* rect=reinterpret_cast<const RECT*>(lParam);
        probe->dpiRectangleCopied=rect&&rect->left==11&&rect->bottom==444;
        probe->reenteredHost|=probe->scriptActive;return 0;
    }
    }
    return DefWindowProcW(window,message,wParam,lParam);
}

int RunInlineFlexIntrinsicRegression(){
    const int before=failures;
    for(const float ratio:{1.0f,1.5f})for(const auto* direction:{L"row",L"row-reverse",L"column",L"column-reverse"}){
        Document document;StyleSheet styles;std::wstring error;
        const std::wstring html=L"<style>*{box-sizing:border-box;margin:0;padding:0}.host{display:flex;width:300px}"
            L".probe{display:inline-flex;flex-direction:"+std::wstring(direction)+L";gap:7px}"
            L".first{width:80px;height:10px}.second{width:70px;height:10px}.narrow{width:90px}"
            L"</style><div class='host'><div class='probe' id='wide'><a class='first'></a><a class='second'></a></div></div>"
            L"<div class='host narrow'><div class='probe' id='narrow'><a class='first'></a><a class='second'></a></div></div>";
        Check(document.Parse(html,&error)&&styles.Parse(document.StyleText(),&error),L"inline-flex intrinsic fixture parses");
        LayoutEngine layout(document,styles);layout.Layout(400,300,ratio);
        const auto* wide=layout.BoxFor(document.GetElementById(L"wide"));
        const auto* narrow=layout.BoxFor(document.GetElementById(L"narrow"));
        const bool column=std::wstring_view(direction).find(L"column")==0;
        Check(wide&&std::abs(wide->rect.width-(column?80.0f:157.0f))<.01f,
            L"inline-flex intrinsic width follows its inner flex direction at both DPIs");
        if(column)Check(narrow&&std::abs(narrow->rect.width-80.0f)<.01f,
            L"a column's automatic minimum width is its widest item, without horizontal gaps");
        if(wide&&narrow)std::wcout<<L"Inline-flex DPI "<<ratio<<L" direction "<<direction<<L" widths "<<wide->rect.width<<L","<<narrow->rect.width<<L'\n';
    }
    return failures==before?0:1;
}

int RunTransformContainingBlockRegression(){
    for(const float ratio:{1.0f,1.5f})for(const auto* display:{L"block",L"inline-block",L"flex",L"grid"}){
        Document document;StyleSheet styles;std::wstring error;
        const std::wstring html=L"<style>*{box-sizing:border-box;margin:0;padding:0}"
            L".outer{position:relative;margin:20px 30px;width:240px;height:180px;border:4px solid black;padding:20px}"
            L".carrier{display:"+std::wstring(display)+L";transform:rotate(0deg) scale(1);margin:10px 15px;width:80px;height:60px;border:3px solid black;padding:7px}"
            L".inner{position:relative;width:30px;height:20px}"
            L".absolute{position:absolute;left:5px;top:6px;width:8px;height:9px}"
            L".fixed{position:fixed;left:5px;top:6px;width:8px;height:9px}"
            L".control{margin:0;width:24px;height:24px;border:2px solid black;transform:scale(1)}"
            L".control::after{content:'';position:absolute;left:5px;top:0;width:6px;height:12px}"
            L"</style><div class='outer' id='outer'><div class='carrier' id='carrier'>"
            L"<div class='absolute' id='absolute'></div><div class='inner'><div class='fixed' id='fixed'></div></div></div>"
            L"<div class='control' id='control'></div><div class='fixed' id='viewport-fixed'></div></div>";
        Check(document.Parse(html,&error)&&styles.Parse(document.StyleText(),&error),L"transform containing block fixture parses");
        LayoutEngine layout(document,styles);layout.Layout(400,300,ratio);
        const auto* carrier=layout.BoxFor(document.GetElementById(L"carrier"));
        const auto* absolute=layout.BoxFor(document.GetElementById(L"absolute"));
        const auto* fixed=layout.BoxFor(document.GetElementById(L"fixed"));
        const auto* viewportFixed=layout.BoxFor(document.GetElementById(L"viewport-fixed"));
        if(carrier&&absolute&&fixed)std::wcout<<L"Containing block DPI "<<ratio<<L" display "<<display
            <<L" carrier "<<carrier->rect.x<<L","<<carrier->rect.y
            <<L" absolute "<<absolute->rect.x<<L","<<absolute->rect.y
            <<L" fixed "<<fixed->rect.x<<L","<<fixed->rect.y<<L'\n';
        // Border widths snap to device pixels. Remove the authored 7px
        // padding from the content origin to locate the used padding-box edge.
        Check(carrier&&absolute&&fixed&&std::abs(absolute->rect.x-(carrier->content.x-7+5))<.01f&&
            std::abs(absolute->rect.y-(carrier->content.y-7+6))<.01f&&std::abs(fixed->rect.x-(carrier->content.x-7+5))<.01f&&
            std::abs(fixed->rect.y-(carrier->content.y-7+6))<.01f,
            L"identity transforms establish padding-box containing blocks for absolute and fixed descendants across formatting contexts and DPIs");
        Check(viewportFixed&&std::abs(viewportFixed->rect.x-5)<.01f&&std::abs(viewportFixed->rect.y-6)<.01f,
            L"fixed positioning skips relative ancestors without transforms");
        const auto* control=layout.BoxFor(document.GetElementById(L"control"));const LayoutBox* generated=nullptr;
        if(control)for(const auto& child:control->children)if(child->pseudo==L"after")generated=child.get();
        Check(control&&generated&&std::abs(generated->rect.x-control->rect.x-7)<.01f&&
            std::abs(generated->rect.y-control->rect.y-2)<.01f,
            L"generated checkmark geometry uses its transformed owner's padding box at both DPIs");
        const auto fixedNode=document.GetElementById(L"fixed");bool geometryChanged=false;
        fixedNode->inlineStyle[L"left"]=L"9px";
        const bool requiresLayout=layout.Restyle(fixedNode,&geometryChanged);
        Check(!requiresLayout&&geometryChanged&&carrier&&fixed&&
            std::abs(fixed->rect.x-(carrier->content.x-7+9))<.01f&&
            std::abs(fixed->rect.y-(carrier->content.y-7+6))<.01f,
            L"updating fixed insets retains the transformed containing block without rebuilding unrelated flow");
    }
    return failures?1:0;
}

int RunExecutionResponsiveness(){
    const int before=failures;
    {
        Document document;std::wstring error,result;document.Parse(L"<body></body>",&error);
        JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(2);
        runtime.SetExecutionYieldHandler([]{return true;});
        Check(runtime.Execute(L"var cell={value:1};function readCell(value){return value.value;}function numericIncrement(value){return value+1;}for(var i=0;i<2000;i++){readCell(cell);numericIncrement(i);}",nullptr,&error),
            L"mixed optimized/interpreted checkpoint fixture warms ordinary method reads");
        unsigned checkpoints=0;ULONGLONG previous=0,maximumGap=0;
        const auto checkpointDeadline=GetTickCount64()+1500;
        runtime.SetExecutionYieldHandler([&]{const auto now=GetTickCount64();
            if(previous)maximumGap=std::max(maximumGap,now-previous);previous=now;++checkpoints;return now<checkpointDeadline;});
        Check(runtime.Execute(L"var until=performance.now()+150;while(performance.now()<until){cell.value=numericIncrement(readCell(cell));}return cell.value>1;",&result,&error)&&
              result==L"true"&&checkpoints>=3&&maximumGap<120,
            L"fused instructions and warmed method reads cannot skip repeated host checkpoints");
        Check(runtime.GetJitStatistics().nativeCalls>2000,
            L"numeric leaf calls retain host checkpoints after native compilation");
    }
    WNDCLASSW registration{};registration.lpfnWndProc=ExecutionWindowProc;
    registration.hInstance=GetModuleHandleW(nullptr);registration.lpszClassName=L"TWebFrame.Tests.ExecutionWindow";
    RegisterClassW(&registration);
    for(const auto awareness:{DPI_AWARENESS_CONTEXT_UNAWARE,DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2})
    for(const bool customCallback:{false,true}){
        const auto previousAwareness=SetThreadDpiAwarenessContext(awareness);
        ExecutionWindowProbe probe;
        probe.registeredQuery=RegisterWindowMessageW(L"TWebFrame.Tests.ExecutionQuery");
        HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,registration.lpszClassName,L"",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            -10000,-10000,660,420,nullptr,nullptr,registration.hInstance,&probe);
        Check(host!=nullptr,L"long-script native host is created");
        RECT bounds{0,0,640,360};auto view=host?TWebFrame::View::Create(host,bounds):nullptr;
        Check(view!=nullptr,L"long-script View is created");
        if(view){
            probe.page=view->Window();
            Check(view->NavigateToString(LR"HTML(<style>body{margin:0}button{display:block;width:80px;height:40px}</style>
                <button onclick="order.push('click')">Action</button><iframe srcdoc="<p>Child</p>"></iframe>)HTML"),
                L"long-script common HTML fixture loads");
            probe.childPage=FindWindowExW(probe.page,nullptr,L"TWebFrame.View.1",nullptr);
            std::wstring result,error;
            Check(view->ExecuteScript(L"var order=[];setTimeout(function(){order.push('timer');},0);",nullptr,&error),
                L"page timer is queued before the long job");
            if(probe.childPage)
                Check(view->ExecuteScript(L"frames[0].eval(\"var childOrder=[];setTimeout(function(){childOrder.push('timer');},0);\");",nullptr,&error),
                    L"iframe timer is queued before the parent's long job");
            unsigned customYields=0;
            if(customCallback)view->SetExecutionYieldHandler([&]{
                ++customYields;MSG queued{};
                for(unsigned count=0;count<32&&PeekMessageW(&queued,nullptr,0,0,PM_REMOVE);++count)
                    DispatchMessageW(&queued);
                return true;
            });
            PostMessageW(host,WM_NCLBUTTONDOWN,HTCAPTION,0);
            probe.scriptActive=true;
            const bool completed=view->ExecuteScript(LR"JS(
                order.push('start');var deadline=performance.now()+120;
                while(performance.now()<deadline){}
                order.push('end');return order.join(',');
            )JS",&result,&error);
            probe.scriptActive=false;
            Check(completed&&result==L"start,end"&&probe.nativeDown==1,
                L"native title-bar messages are handled during a long script without reentering page timers or input");
            Check(!probe.reenteredHost&&probe.hostResize==0&&probe.hostTimer==0,
                L"native modal-loop host resize and timer actions wait for the script to finish");
            Check(probe.registeredQueryAnswered,L"registered OS-style synchronous queries are answered without being deferred or interrupting JavaScript");
            if(customCallback)Check(customYields>0,L"the embedding application's optional callback still runs");
            if(probe.childPage){
                Check(view->ExecuteScript(L"return frames[0].eval('childOrder.length');",&result,&error)&&result==L"0",
                    L"an idle iframe cannot dispatch a timer inside its parent's active job");
            }
            MSG queued{};for(unsigned count=0;count<256&&PeekMessageW(&queued,nullptr,0,0,PM_REMOVE);++count)
                DispatchMessageW(&queued);
            Check(probe.hostResize==1&&probe.hostTimer==1&&probe.dpiRectangleCopied&&!probe.reenteredHost,
                L"deferred host tasks and owned DPI rectangles are delivered after the long job");
            Check(view->ExecuteScript(L"return order.join(',');",&result,&error)&&
                result.find(L"start,end")==0&&result.find(L"timer")!=std::wstring::npos&&result.find(L"click")!=std::wstring::npos,
                L"deferred page timers and pointer input are delivered after the long job");
            std::wcout<<L"Callback "<<customCallback<<L" page order "<<result<<L" error "<<error<<L'\n';
            if(probe.childPage)Check(view->ExecuteScript(L"return frames[0].eval('childOrder.join()');",&result,&error)&&result==L"timer",
                L"deferred iframe timers resume after the parent finishes");
            if(probe.childPage)std::wcout<<L"Child order "<<result<<L" error "<<error<<L'\n';
            view->SetExecutionYieldHandler({});
            PostMessageW(host,WM_CLOSE,0,0);
            Check(!view->ExecuteScript(L"while(true){}",nullptr,&error)&&error.find(L"AbortError")!=std::wstring::npos,
                L"a native close safely interrupts an infinite script with no host callback installed");
            for(unsigned count=0;count<128&&PeekMessageW(&queued,nullptr,0,0,PM_REMOVE);++count)DispatchMessageW(&queued);
            Check(probe.hostClose==1,L"native close returns to the ordinary host loop after unwinding");
            PostQuitMessage(74);
            Check(!view->ExecuteScript(L"while(true){}",nullptr,&error)&&error.find(L"AbortError")!=std::wstring::npos,
                L"a pending thread quit interrupts a long script");
            bool retainedQuit=false;
            for(unsigned count=0;count<256&&PeekMessageW(&queued,nullptr,0,0,PM_REMOVE);++count){
                if(queued.message==WM_QUIT){retainedQuit=queued.wParam==74;break;}
                DispatchMessageW(&queued);
            }
            Check(retainedQuit,
                L"the safepoint preserves WM_QUIT and its exit code");
            Check(view->ExecuteScript(L"return 42;",&result,&error)&&result==L"42",
                L"the realm remains usable after native close/quit interruptions");
            view.reset();
        }
        if(host)DestroyWindow(host);
        SetThreadDpiAwarenessContext(previousAwareness);
    }
    return failures==before?0:1;
}

int RunNoScriptRegression(){
    const std::wstring markup=L"<!doctype html><html><head></head><body><noscript><b>fallback &amp; text</b><style>body { color: red; }</style></noscript><p>ordinary content</p></body></html>";
    for(const auto scale:{1.0f,1.5f})for(const bool enabled:{true,false}){
        Document document;document.SetScriptingEnabled(enabled);Check(document.Parse(markup),L"noscript fixture parses");
        const auto fallback=document.QuerySelector(L"noscript");
        Check(static_cast<bool>(fallback),L"noscript stays in the DOM");
        Check(static_cast<bool>(document.QuerySelector(L"noscript b"))==!enabled,
              L"noscript children are raw text with scripting enabled and elements with scripting disabled");
        Check(document.StyleText().empty()==enabled,L"fallback style is inert while scripting is enabled");
        StyleSheet styles;styles.SetDisplay(1000,760,scale);styles.Parse(L"");
        Check(styles.Compute(fallback).Is(L"display",enabled?L"none":L"block"),L"noscript default rendering follows scripting at both DPIs");
        Document adopted;adopted.AdoptParsed(document);
        Check(adopted.ScriptingEnabled()==enabled,L"background-parsed documents preserve their scripting mode");
        if(enabled){
            JavaScriptRuntime runtime(adopted);std::wstring result,error;
            Check(runtime.Execute(L"return document.querySelector('noscript').innerHTML;",&result,&error)&&
                  result==L"<b>fallback &amp; text</b><style>body { color: red; }</style>",L"noscript raw text round-trips through innerHTML");
            Check(runtime.Execute(L"return new DOMParser().parseFromString('<html><body><noscript><b>fallback</b></noscript></body></html>','text/html').querySelector('noscript b').textContent;",&result,&error)&&result==L"fallback",
                  L"DOMParser inert documents parse fallback markup");
        }
    }
    return failures?1:0;
}

int RunEmbeddingFrameRegression(){
    const int initialFailures=failures;
    for(const double ratio:{1.0,1.5}){
        Document childDocument;childDocument.Parse(L"<html><body></body></html>");
        JavaScriptRuntime child(childDocument);child.SetDevicePixelRatio(ratio);
        std::wstring result,error;
        {
            Document parentDocument;parentDocument.Parse(L"<html><body><iframe name='embedded'></iframe></body></html>");
            JavaScriptRuntime parent(parentDocument);parent.SetLocation(L"https://example.test/parent");parent.SetDevicePixelRatio(ratio);
            const auto frame=parentDocument.QuerySelector(L"iframe");child.SetEmbeddingFrame(&parent,frame);
            child.SetLocation(L"https://example.test/child");
            Check(child.Execute(L"return frameElement.tagName+'|'+frameElement.name+'|'+('frameElement' in window);",&result,&error)&&result==L"IFRAME|embedded|true",
                L"same-origin frameElement returns the actual embedding element at both DPI scales");
            Check(child.Execute(L"frameElement.name='renamed';",nullptr,&error)&&frame->Attribute(L"name")==L"renamed",
                L"embedding element writes operate on the parent DOM");
            child.SetLocation(L"https://other.test/child");
            Check(child.Execute(L"return frameElement===null;",&result,&error)&&result==L"true",
                L"cross-origin frameElement is null");
            child.SetLocation(L"about:blank");
            Check(child.Execute(L"return frameElement.name;",&result,&error)&&result==L"renamed",
                L"inherited-origin about:blank exposes its embedding element");
            child.Clear();
            Check(child.Execute(L"return frameElement.name;",&result,&error)&&result==L"renamed",
                L"runtime reset preserves the browsing context embedding");
            Check(child.Execute(L"var retainedFrame=frameElement;",nullptr,&error),
                L"embedding element can be retained independently of its parent runtime");
        }
        Check(child.Execute(L"return frameElement===null;",&result,&error)&&result==L"true",
            L"expired embedding parent returns null without retaining a destroyed runtime");
        Check(child.Execute(L"return retainedFrame.tagName+'|'+retainedFrame.name;",&result,&error)&&result==L"IFRAME|renamed",
            L"retained embedding element remains readable after parent destruction");
    }
    return failures-initialFailures;
}

int wmain(int argc,wchar_t** argv) {
    const HRESULT comInitialization=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const bool uninitializeCom=SUCCEEDED(comInitialization);
    if(argc>1&&_wcsicmp(argv[1],L"--embedding-frame-regression")==0){
        const int result=RunEmbeddingFrameRegression();if(!result)std::wcout<<L"Embedding frame regression passed at 100% and 150% DPI\n";
        if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--noscript-regression")==0){
        const int result=RunNoScriptRegression();if(!result)std::wcout<<L"Noscript regression passed at 100% and 150% DPI\n";
        if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--transform-containing-block-regression")==0){
        const int result=RunTransformContainingBlockRegression();
        if(!result)std::wcout<<L"Transform containing block regression passed at 100% and 150% DPI\n";
        if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--responsiveness-regression")==0){
        const int result=RunExecutionResponsiveness();
        if(!result)std::wcout<<L"Execution responsiveness regression passed\n";
        if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--inline-flex-intrinsic-regression")==0){
        const int result=RunInlineFlexIntrinsicRegression();
        if(!result)std::wcout<<L"Inline-flex intrinsic regression passed at 100% and 150% DPI\n";
        if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc==3&&_wcsicmp(argv[1],L"--execution-regression")==0){
        std::ifstream input(std::filesystem::path(argv[2]),std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
        if(!input||length<=0){if(uninitializeCom)CoUninitialize();return 1;}
        std::wstring script(static_cast<size_t>(length),L'\0');
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),script.data(),length);
        for(const auto ratio:{1.0,1.5})for(const size_t threshold:{size_t{0},size_t{2}}){
            Document document;std::wstring error,result;
            Check(document.Parse(L"<html><body></body></html>",&error),error.c_str());
            JavaScriptRuntime runtime(document);runtime.SetDevicePixelRatio(ratio);runtime.SetJitCompilationThreshold(threshold);
            Check(runtime.Execute(script,&result,&error)&&result.rfind(L"PASS|",0)==0,
                L"execution reads and calls preserve their semantics with interpreter/optimized execution at both DPI scales");
            std::wcout<<L"DPI "<<ratio<<L" threshold "<<threshold<<L" result "<<result<<L" error "<<error<<L'\n';
        }
        if(uninitializeCom)CoUninitialize();return failures?1:0;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--embedded-script-regression")==0){
        const auto result=RunEmbeddedScriptCompatibility();
        if(!result)std::wcout<<L"Embedded script compatibility regression passed\n";
        if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--exception-regression")==0){
        const int result=RunJavaScriptExceptionRegression();
        if(!result)std::wcout<<L"JavaScript exception regression passed\n";
        if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--benchmark")==0){
        const int result=RunFrameBenchmark();if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>1&&_wcsicmp(argv[1],L"--editing-benchmark")==0){
        const int result=RunEditingBenchmark();if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>2&&_wcsicmp(argv[1],L"--jquery")==0){
        const int result=RunJQueryCompatibility(argv[2]);if(uninitializeCom)CoUninitialize();return result;
    }
    if(argc>2&&(_wcsicmp(argv[1],L"--javascript")==0||_wcsicmp(argv[1],L"--iframe-javascript")==0)){
        const int result=RunJavaScriptFiles(argc-2,argv+2,_wcsicmp(argv[1],L"--iframe-javascript")==0);
        if(uninitializeCom)CoUninitialize();return result;
    }
    RunJavaScriptExceptionRegression();
    RunNoScriptRegression();
    RunEmbeddedScriptCompatibility();
    RunTransformContainingBlockRegression();
    RunInlineFlexIntrinsicRegression();
    FastMap<int, std::wstring, ConstantHash> fastMap;
    for (int i = 0; i < 64; ++i) fastMap[i] = std::to_wstring(i);
    fastMap[17] = L"updated";
    Check(fastMap.size() == 64 && fastMap.find(17) != fastMap.end() &&
          fastMap.find(17)->second == L"updated" && fastMap.count(99) == 0,
          L"custom map handles growth, collisions and updates");
    fastMap.clear(); fastMap[3] = L"reused";
    Check(fastMap.size() == 1 && fastMap.find(3)->second == L"reused",
          L"custom map can reuse cleared buckets");

    std::wstring error;
    Document queryDoc;
    Check(queryDoc.Parse(L"<body><section id='scope'><span class='first'></span><div><span class='deep'></span></div></section><span class='outside'></span></body>"),
          L"query optimization fixture parses");
    const auto queryScope=queryDoc.GetElementById(L"scope");
    Check(queryDoc.QuerySelector(L".first,.deep",queryScope)==queryScope->children.front(),
          L"querySelector preserves document-order early exit for selector groups");
    Check(queryDoc.QuerySelectorAll(L":scope > .first,.deep",queryScope).size()==2,
          L"compiled scoped selector groups preserve querySelectorAll behavior");
    queryScope->SetAttribute(L"DATA-MIXED",L"value");
    Check(queryScope->Attribute(L"data-mixed")==L"value"&&
          queryScope->Attribute(L"DATA-MIXED")==L"value",
          L"attribute fast path preserves case-insensitive lookup");
    queryScope->SetAttribute(L"class",L"alpha\tbeta  gamma");
    Check(queryScope->HasClass(L"beta")&&!queryScope->HasClass(L"bet"),
          L"class token scan handles whitespace without stream allocation");
    Check(queryDoc.QuerySelector(L"section.alpha")==queryScope,
          L"direct native class mutation updates the owning document candidate index");
    Document stateSelectorDoc;
    Check(stateSelectorDoc.Parse(
        L"<body><section><span id='first-type'></span><em id='only-type'></em><span id='last-type'></span></section>"
        L"<div><b id='only-child'></b></div><a id='link' href=''></a><a id='anchor'></a>"
        L"<input id='required-control' required><input id='disabled-control' disabled><div id='plain'></div></body>"),
        L"structural and state selector fixture parses");
    Check(Document::MatchesSelector(stateSelectorDoc.GetElementById(L"first-type"),L"span:first-of-type")&&
          Document::MatchesSelector(stateSelectorDoc.GetElementById(L"last-type"),L"span:last-of-type")&&
          Document::MatchesSelector(stateSelectorDoc.GetElementById(L"only-type"),L"em:only-of-type")&&
          Document::MatchesSelector(stateSelectorDoc.GetElementById(L"only-child"),L"b:only-child"),
          L"structural pseudo-classes use generic element sibling relationships");
    Check(Document::MatchesSelector(stateSelectorDoc.GetElementById(L"link"),L"a:link")&&
          Document::MatchesSelector(stateSelectorDoc.GetElementById(L"link"),L"a:any-link")&&
          !Document::MatchesSelector(stateSelectorDoc.GetElementById(L"anchor"),L"a:link"),
          L"link pseudo-classes depend on href presence, including an empty URL");
    Check(Document::MatchesSelector(stateSelectorDoc.GetElementById(L"required-control"),L"input:enabled:required")&&
          Document::MatchesSelector(stateSelectorDoc.GetElementById(L"disabled-control"),L"input:disabled:optional")&&
          !Document::MatchesSelector(stateSelectorDoc.GetElementById(L"plain"),L"div:enabled")&&
          !Document::MatchesSelector(stateSelectorDoc.GetElementById(L"plain"),L"div:optional"),
          L"form state pseudo-classes do not leak onto elements that cannot have those states");
    std::shared_ptr<Node> retainedAfterDocument;
    {
        Document ownerDoc;
        Check(ownerDoc.Parse(L"<body><div id='retained'></div></body>"),
              L"owner-document lifecycle fixture parses");
        retainedAfterDocument=ownerDoc.GetElementById(L"retained");
        Check(retainedAfterDocument&&retainedAfterDocument->ownerDocument==&ownerDoc,
              L"indexed nodes retain their owning document while connected");
    }
    Check(retainedAfterDocument&&retainedAfterDocument->ownerDocument==nullptr,
          L"document destruction disconnects retained nodes from candidate indexes");
    retainedAfterDocument->AddClass(L"safe-after-document");

    Document mutationDoc;
    Check(mutationDoc.Parse(L"<body><div id='scroll'><span></span></div></body>"),
          L"mutation classification fixture parses");
    JavaScriptRuntime mutationJs(mutationDoc);
    JavaScriptRuntime::Mutation lastMutation;int mutationNotifications=0;
    mutationJs.SetMutationSink([&](const JavaScriptRuntime::Mutation& mutation){
        lastMutation=mutation;++mutationNotifications;
    });
    Check(mutationJs.Execute(L"document.getElementById('scroll').scrollTop=5;",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Paint&&
          lastMutation.targets.size()==1&&lastMutation.targets.front()==mutationDoc.GetElementById(L"scroll"),
          L"scroll mutations remain paint-only and retain their target");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"const node=document.getElementById('scroll');node.scrollTop=8;node.style.color='red';",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Style&&
          !lastMutation.selectorMatchingChanged,
          L"batched inline declarations retain the strongest invalidation level without reporting a selector change");
    std::wstring cssomResult;
    Check(mutationJs.Execute(
              L"const style=document.getElementById('scroll').style;"
              L"style.setProperty('width','120px','important');"
              L"const removed=style.removeProperty('color');"
              L"return [removed,style.getPropertyValue('width'),style.getPropertyPriority('width'),style.item(0),style.cssText,document.getElementById('scroll').getAttribute('style')].join('|');",
              &cssomResult,&error)&&
          cssomResult==L"red|120px|important|width|width: 120px !important;|width: 120px !important;",
          L"CSSStyleDeclaration mutators preserve priority, ordering and the reflected style attribute");
    Check(mutationJs.Execute(
              L"const style=document.getElementById('scroll').style;style.width='';style.cssText='height: 45px; color: blue !important';return style.length+'|'+style.height+'|'+style.getPropertyPriority('color');",
              &cssomResult,&error)&&cssomResult==L"2|45px|important",
          L"CSSStyleDeclaration empty assignment removes declarations and cssText reparses priorities");
    mutationNotifications=0;
    Check(mutationJs.Execute(
              L"const style=document.getElementById('scroll').style;"
              L"style.height='45px';"
              L"style.setProperty('height','45px');"
              L"style.setProperty('color','blue','important');",
              nullptr,&error)&&mutationNotifications==0,
          L"reassigning identical CSSOM declarations does not invalidate a large document");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"document.getElementById('scroll').setAttribute('width','240');",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Layout,
          L"geometry-affecting attributes request layout without a tree mutation");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"const child=document.createElement('b');document.getElementById('scroll').appendChild(child);",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.kind==JavaScriptRuntime::MutationKind::Tree,
          L"tree mutations request a layout-tree rebuild");
    mutationNotifications=0;
    Check(mutationJs.Execute(L"document.getElementById('scroll').setAttribute('ARIA-LIVE','polite');",nullptr,&error)&&
          mutationNotifications==1&&lastMutation.liveRegionMembershipChanged,
          L"aria-live membership changes are identified without a document-wide scan");
    mutationNotifications=0;
    Check(mutationJs.Execute(
              L"const node=document.getElementById('scroll');node.classList.add('hidden');",
              nullptr,&error)&&mutationNotifications==1&&
          lastMutation.kind==JavaScriptRuntime::MutationKind::Style&&
          lastMutation.selectorMatchingChanged,
          L"classList reports an actual class change once");
    mutationNotifications=0;
    Check(mutationJs.Execute(
              L"const node=document.getElementById('scroll');"
              L"node.classList.add('hidden');node.classList.toggle('hidden',true);"
              L"node.classList.remove('missing');",
              nullptr,&error)&&mutationNotifications==0,
          L"no-op classList operations do not invalidate style or layout");
    mutationNotifications=0;
    Check(mutationJs.Execute(
              L"const node=document.getElementById('scroll');node.classList.remove('hidden');",
              nullptr,&error)&&mutationNotifications==1&&
          lastMutation.kind==JavaScriptRuntime::MutationKind::Style,
          L"classList reports an actual class removal once");
    Document normalizeDoc;
    Check(normalizeDoc.Parse(L"<body><div id='editor'>text</div></body>"),
          L"DOM normalization fixture parses");
    JavaScriptRuntime normalizeJs(normalizeDoc);int normalizeNotifications=0;
    JavaScriptRuntime::Mutation normalizeMutation;
    normalizeJs.SetMutationSink([&](const JavaScriptRuntime::Mutation& mutation){
        normalizeMutation=mutation;++normalizeNotifications;
    });
    const auto normalizeReindexStart=normalizeDoc.FullReindexCount();
    Check(normalizeJs.Execute(L"document.getElementById('editor').normalize();",nullptr,&error)&&
          normalizeNotifications==0&&normalizeDoc.FullReindexCount()==normalizeReindexStart,
          L"normalizing an already-normalized text subtree is mutation-free");
    Check(normalizeJs.Execute(
              L"const e=document.getElementById('editor');e.append(document.createTextNode('more'));",
              nullptr,&error),L"adjacent normalization text fixture is created");
    normalizeNotifications=0;
    Check(normalizeJs.Execute(L"document.getElementById('editor').normalize();",nullptr,&error)&&
          normalizeNotifications==1&&normalizeMutation.kind==JavaScriptRuntime::MutationKind::Tree&&
          normalizeDoc.GetElementById(L"editor")->children.size()==1&&
          normalizeDoc.GetElementById(L"editor")->InnerText()==L"textmore"&&
          normalizeDoc.FullReindexCount()==normalizeReindexStart,
          L"normalizing adjacent text nodes reports one tree change without reindexing elements");
    const auto incrementalIndexStart=mutationDoc.FullReindexCount();
    Check(mutationJs.Execute(L"const indexedChild=document.createElement('i');indexedChild.id='incremental-id';document.getElementById('scroll').appendChild(indexedChild);",nullptr,&error)&&
          mutationDoc.GetElementById(L"incremental-id")!=nullptr&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"unique subtree insertion updates the ID index without a full document walk");
    Check(mutationJs.Execute(L"indexedChild.id='renamed-id';",nullptr,&error)&&
          !mutationDoc.GetElementById(L"incremental-id")&&mutationDoc.GetElementById(L"renamed-id")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"unique ID replacement updates the index incrementally");
    Check(mutationJs.Execute(L"indexedChild.remove();",nullptr,&error)&&
          !mutationDoc.GetElementById(L"renamed-id")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"unique subtree removal updates the ID index incrementally");
    Check(mutationJs.Execute(L"const candidateChild=document.createElement('article');candidateChild.className='candidate-old';document.getElementById('scroll').appendChild(candidateChild);",nullptr,&error)&&
          mutationDoc.QuerySelector(L"article.candidate-old")==mutationDoc.QuerySelector(L".candidate-old")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"subtree insertion updates tag and class candidate indexes incrementally");
    Check(mutationJs.Execute(L"candidateChild.classList.remove('candidate-old');candidateChild.classList.add('candidate-new');",nullptr,&error)&&
          !mutationDoc.QuerySelector(L".candidate-old")&&
          mutationDoc.QuerySelector(L"article.candidate-new")!=nullptr&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"classList mutations update class candidates without a full reindex");
    Check(mutationJs.Execute(L"candidateChild.setAttribute('class','candidate-set');",nullptr,&error)&&
          !mutationDoc.QuerySelector(L".candidate-new")&&mutationDoc.QuerySelector(L".candidate-set")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"class attribute replacement updates class candidates incrementally");
    Check(mutationJs.Execute(L"candidateChild.remove();",nullptr,&error)&&
          !mutationDoc.QuerySelector(L"article.candidate-set")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"subtree removal clears tag and class candidate indexes incrementally");
    Check(mutationJs.Execute(L"const namedChild=document.createElement('input');namedChild.name='original-name';document.getElementById('scroll').appendChild(namedChild);",nullptr,&error)&&
          mutationDoc.GetElementsByName(L"original-name").size()==1&&
          mutationDoc.GetElementsByName(L"absent-name").empty()&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"subtree insertion indexes named elements and absent names without a full reindex");
    Check(mutationJs.Execute(L"namedChild.setAttribute('name','renamed-name');",nullptr,&error)&&
          mutationDoc.GetElementsByName(L"original-name").empty()&&
          mutationDoc.GetElementsByName(L"renamed-name").size()==1&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"name attribute replacement updates named lookup incrementally");
    Check(mutationJs.Execute(L"namedChild.remove();",nullptr,&error)&&
          mutationDoc.GetElementsByName(L"renamed-name").empty()&&
          mutationDoc.FullReindexCount()==incrementalIndexStart,
          L"subtree removal clears the named-element presence index incrementally");
    Check(mutationJs.Execute(L"const duplicateA=document.createElement('b');const duplicateB=document.createElement('b');duplicateA.id='duplicate-id';duplicateB.id='duplicate-id';document.getElementById('scroll').append(duplicateA,duplicateB);",nullptr,&error)&&
          mutationDoc.GetElementById(L"duplicate-id")&&
          mutationDoc.FullReindexCount()==incrementalIndexStart+1,
          L"duplicate IDs conservatively fall back to one full reindex");
    const auto duplicateMatches=mutationDoc.QuerySelectorAll(L"#duplicate-id");
    Check(duplicateMatches.size()==2&&mutationDoc.QuerySelector(L"#duplicate-id")==duplicateMatches.front(),
          L"duplicate-ID candidate fallback preserves first-in-document query order");

    Document scrollSyncDoc;
    Check(scrollSyncDoc.Parse(L"<body><div id='scroller'><div id='scroll-content'></div></div></body>"),
          L"paint-only scroll fixture parses");
    StyleSheet scrollSyncCss;
    Check(scrollSyncCss.Parse(L"#scroller{width:160px;height:40px;overflow:auto}#scroll-content{height:200px}"),
          L"paint-only scroll styles parse");
    LayoutEngine scrollSyncLayout(scrollSyncDoc,scrollSyncCss);scrollSyncLayout.Layout(240,120);
    const auto scroller=scrollSyncDoc.GetElementById(L"scroller");
    const auto scrollContent=scrollSyncDoc.GetElementById(L"scroll-content");
    const float initialContentY=scrollSyncLayout.BoxFor(scrollContent)->rect.y;
    scroller->scrollTop=30;
    Check(scrollSyncLayout.SyncScroll(scroller)&&
          std::abs(scrollSyncLayout.BoxFor(scrollContent)->rect.y-(initialContentY-30))<0.01f&&
          !scrollSyncLayout.SyncScroll(scroller),
          L"paint-only DOM scrolling translates existing layout boxes exactly once");

    const auto numericExceptionHandler=AddVectoredExceptionHandler(1,CountFirstChanceCppExceptions);
    InterlockedExchange(&firstChanceCppExceptions,0);
    Document numericFallbackDoc;
    Check(numericFallbackDoc.Parse(
        L"<body><div id='numeric'>&#xnot-a-number;</div>"
        L"<canvas id='canvas' width='auto'></canvas><img id='image' width='auto'></body>",&error),
        L"invalid authored numeric tokens remain valid HTML input");
    Check(numericFallbackDoc.QuerySelectorAll(L"div:nth-child(not-a-number)").empty(),
          L"invalid nth-child arithmetic fails matching without escaping the DOM selector rule");
    constexpr unsigned int invalidColorFallback=0x01020304u;
    Check(StyleSheet::Length(L"inherit",100,100,37)==37&&
          StyleSheet::Color(L"rgb(not-a-number, 0, 0)",invalidColorFallback)==invalidColorFallback,
          L"invalid CSS numeric tokens use their property fallback");
    StyleSheet numericFallbackCss;
    Check(numericFallbackCss.Parse(
        L"#numeric{font-weight:bold;transition:all 0.2s ease-in-out;width:inherit}"),
        L"transition and keyword-font CSS fixture parses");
    LayoutEngine numericFallbackLayout(numericFallbackDoc,numericFallbackCss);
    numericFallbackLayout.Layout(240,120);
    JavaScriptRuntime numericFallbackJs(numericFallbackDoc);std::wstring numericFallbackResult;
    Check(numericFallbackJs.Execute(
        L"return document.getElementById('canvas').width+'|'"
        L"+document.getElementById('image').width;",
        &numericFallbackResult,&error)&&numericFallbackResult==L"300|0",error.c_str());
    Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
          L"DOM, CSS, layout and JavaScript numeric fallbacks do not throw first-chance C++ exceptions");
    if(numericExceptionHandler)RemoveVectoredExceptionHandler(numericExceptionHandler);

    Document jqueryDomDoc;
    Check(jqueryDomDoc.Parse(
        L"<body><form id='jquery-form'><input name='first'><textarea name='second'></textarea>"
        L"<button name='submit'>Submit</button></form><div id='jquery-node'>text</div></body>",&error),
        L"jQuery DOM compatibility fixture parses");
    JavaScriptRuntime jqueryDomJs(jqueryDomDoc);std::wstring jqueryDomResult;
    jqueryDomJs.SetResourceLoader([](const std::wstring& resource,std::wstring& body){
        if(resource!=L"jquery-payload.json")return false;
        body=L"{\"value\":9}";return true;
    });
    Check(jqueryDomJs.Execute(
        L"const form=document.getElementById('jquery-form');"
        L"const node=document.getElementById('jquery-node');"
        L"const request=new XMLHttpRequest();request.open('GET','jquery-payload.json',false);request.send();"
        L"const typedRequest=new XMLHttpRequest();typedRequest.responseType='json';"
        L"typedRequest.open('GET','jquery-payload.json',false);typedRequest.send();"
        L"return node.nodeName+'|'+node.firstChild.nodeName+'|'+form.elements.length+'|'"
        L"+request.readyState+'|'+request.status+'|'+JSON.parse(request.responseText).value+'|'"
        L"+typedRequest.response.value+'|'+typeof typedRequest.response;",
        &jqueryDomResult,&error)&&jqueryDomResult==L"DIV|#text|3|4|200|9|9|object",error.c_str());
    Check(jqueryDomResult==L"DIV|#text|3|4|200|9|9|object",
          L"standard nodeName, form collections and text/JSON XMLHttpRequest responses support library paths");

    Document editorDomDoc;
    Check(editorDomDoc.Parse(L"<!doctype html><body><div class='item'><a></a></div></body>",&error),
          L"detached library feature-detection fixture parses");
    JavaScriptRuntime editorDomJs(editorDomDoc);std::wstring editorDomResult;
    Check(editorDomJs.Execute(LR"JS(
        var detached=document.createElement('section');
        detached.innerHTML='<a class="item" data-value="detached">link</a><input checked>';
        var link=detached.getElementsByTagName('a').item(0);
        return link.getAttribute('data-value')==='detached' &&
            detached.getElementsByClassName('item').length===1 &&
            detached.querySelector('a.item')===link &&
            detached.querySelectorAll('a[data-value]').length===1 &&
            document.querySelectorAll('a').length===1;
    )JS",&editorDomResult,&error)&&editorDomResult==L"true",
          L"detached subtree queries use their scope even when connected indexes contain the same tags and classes");
    Check(editorDomJs.Execute(LR"JS(
        var xml=new DOMParser().parseFromString(
          '<?xml version="1.0" encoding="UTF-8"?><Template Mode="Mixed"><Item><![CDATA[<p>&amp;</p>]]></Item></Template>',
          'text/xml');
        var item=xml.getElementsByTagName('Item').item(0);
        var root=xml.documentElement;
        var good=root.nodeName==='Template' && root.getAttribute('Mode')==='Mixed' &&
          root.getAttribute('mode')===null && item.firstChild.nodeType===4 &&
          item.firstChild.nodeValue==='<p>&amp;</p>' && item.textContent==='<p>&amp;</p>' &&
          item.ownerDocument===xml && xml.getElementsByTagName('item').length===0;
        root.setAttribute('Mode','updated');root.removeAttribute('Mode');
        var invalid=new DOMParser().parseFromString('<root><item></root>','application/xml');
        var empty=new DOMParser().parseFromString('','text/xml');
        return good && !root.hasAttribute('Mode') && invalid.getElementsByTagName('parsererror').length===1 &&
            empty.getElementsByTagName('parsererror').length===1;
    )JS",&editorDomResult,&error)&&editorDomResult==L"true",
          L"DOMParser preserves XML names, CDATA and ownerDocument and reports malformed XML");
    editorDomJs.SetResourceLoader([](const std::wstring& resource,std::wstring& body){
        if(resource!=L"template.data")return false;
        body=L"<Template><Item><![CDATA[editor template]]></Item></Template>";return true;
    });
    Check(editorDomJs.Execute(LR"JS(
        var xmlRequest=new XMLHttpRequest();xmlRequest.open('GET','template.data',false);
        xmlRequest.overrideMimeType('text/xml; charset=utf-8');xmlRequest.send();
        var textRequest=new XMLHttpRequest();textRequest.open('GET','template.data',false);
        textRequest.overrideMimeType('text/plain');textRequest.send();
        return xmlRequest.responseXML.getElementsByTagName('Item').item(0).textContent==='editor template' &&
          textRequest.responseXML===null;
    )JS",&editorDomResult,&error)&&editorDomResult==L"true",
          L"XHR XML responses use the MIME type rather than a URL or filename pattern");
    Check(editorDomJs.Execute(LR"JS(
        function legacy(){if(true){function blockHelper(){return 7;}}return blockHelper();}
        function untaken(){if(false){function unusedHelper(){return 9;}}return typeof unusedHelper;}
        function strict(){'use strict';if(true){function privateHelper(){return 1;}}return typeof privateHelper;}
        return legacy()===7 && untaken()==='undefined' && strict()==='undefined';
    )JS",&editorDomResult,&error)&&editorDomResult==L"true",
          L"legacy block functions bind in the outer function only for non-strict executed blocks");
    Check(editorDomJs.Execute(LR"JS(
        var parsedHtml=new DOMParser().parseFromString('<p class="parsed">inert</p><script>window.parserExecuted=true;</script>','text/html');
        var additional=parsedHtml.createElement('span');additional.appendChild(parsedHtml.createTextNode(' child'));
        parsedHtml.querySelector('p').appendChild(additional);
        return parsedHtml.getElementsByClassName('parsed').item(0).textContent==='inert child' &&
            additional.ownerDocument===parsedHtml && typeof window.parserExecuted==='undefined' &&
            document.querySelector('.parsed')===null;
    )JS",&editorDomResult,&error)&&editorDomResult==L"true",
          L"DOMParser HTML documents are inert and their DOM APIs operate on the parsed document");
    Check(editorDomJs.Execute(LR"JS(
        var importSource=new DOMParser().parseFromString('<section id="import-source"><b>copy</b><!--note--></section>','text/html');
        var original=importSource.querySelector('section'),sourceParent=original.parentNode;
        var shallow=document.importNode(original),deep=Document.prototype.importNode.call(document,original,true);
        var target=new DOMParser().parseFromString('<main></main>','text/html');
        var intoTarget=Document.prototype.importNode.call(target,deep,{selfOnly:false});
        var selfOnly=target.importNode(deep,{selfOnly:true});
        var host=document.createElement('div'),shadow=host.attachShadow({mode:'open'}),errors=[];
        for(var bad of [undefined,{},document,importSource,shadow]){
            try{document.importNode(bad);}catch(error){errors.push(error.name);}
        }
        var borrowError='';try{Document.prototype.importNode.call(host,deep);}catch(error){borrowError=error.name;}
        var before=shallow!==original && shallow.childNodes.length===0 && shallow.ownerDocument===document &&
            deep!==original && deep.parentNode===null && deep.childNodes.length===2 &&
            deep.firstChild.ownerDocument===document && deep.lastChild.ownerDocument===document &&
            original.parentNode===sourceParent && original.ownerDocument===importSource &&
            document.getElementById('import-source')===null && intoTarget.ownerDocument===target &&
            intoTarget.firstChild.ownerDocument===target && selfOnly.childNodes.length===0;
        document.body.appendChild(deep);target.querySelector('main').appendChild(intoTarget);
        deep.firstChild.textContent='changed';
        var fragment=importSource.createDocumentFragment();fragment.append('fragment');
        var copiedFragment=document.importNode(fragment,true);
        return before && document.getElementById('import-source')===deep &&
            target.getElementById('import-source')===intoTarget && original.textContent==='copy' &&
            intoTarget.textContent==='copy' && copiedFragment.nodeType===11 &&
            copiedFragment.firstChild.ownerDocument===document && fragment.childNodes.length===1 &&
            errors.join('|')==='TypeError|TypeError|NotSupportedError|NotSupportedError|NotSupportedError' && borrowError==='TypeError';
    )JS",&editorDomResult,&error)&&editorDomResult==L"true",
          L"importNode copies detached subtrees into the receiving document without moving the source and rejects invalid nodes");
    Check(editorDomJs.Execute(LR"JS(
        var dynamicStyle=document.createElement('style');document.head.appendChild(dynamicStyle);
        var dynamicSheet=dynamicStyle.sheet;
        var firstRule=dynamicSheet.insertRule('a { color:red }',0);
        dynamicSheet.insertRule('a { color:blue }',0);
        var insertionWorks=firstRule===0 && dynamicSheet.cssRules.length===2 &&
          dynamicSheet.cssRules.item(0).cssText.indexOf('blue')>=0;
        dynamicSheet.deleteRule(0);
        var bounds=false;try{dynamicSheet.insertRule('a { color:green }',3);}catch(e){bounds=e.name==='IndexSizeError';}
        return insertionWorks && bounds && dynamicSheet.cssRules.length===1 &&
          dynamicSheet.cssRules[0].cssText.indexOf('red')>=0 && document.styleSheets.length===1;
    )JS",&editorDomResult,&error)&&editorDomResult==L"true",
          L"CSSOM insertion and deletion honor rule order and expose current rules");

    Document browserCompatDoc;
    Check(browserCompatDoc.Parse(L"<body><div id='first'></div><div id='second'></div></body>",&error),
          L"browser compatibility fixture parses");
    JavaScriptRuntime browserCompatJs(browserCompatDoc);std::wstring browserCompatResult;
    browserCompatJs.SetLocation(L"https://storage.example.test/compatibility");
    Check(browserCompatJs.Execute(LR"JS(
        var first=document.getElementById('first'),second=document.getElementById('second');
        var comment=document.createComment('marker');first.appendChild(comment);
        function makeCache(){var keys=[];function cache(key,value){cache[key+' ']=value;return value;}return cache;}
        var classCache=makeCache();
        function matcher(name){var expression=classCache[name+' '];return expression||
          (expression=new RegExp('(^|\\s)'+name+'(\\s|$)'))&&
          classCache(name,function(value){return expression.test(value);});}
        var matchItem=matcher('item'),matchItemAgain=matcher('item');
        localStorage.setItem('x','1');localStorage.removeItem('x');
        sessionStorage.setItem('y','2');sessionStorage.removeItem('y');
        return comment.nodeType+'|'+comment.nodeName+'|'+first.innerHTML+'|'+
          first.compareDocumentPosition(second)+'|'+second.compareDocumentPosition(first)+'|'+
          /[a-z]+/g.source+'|'+'abcdef'.substr(-3,2)+'|'+'a'.concat('b','c')+'|'+
          matchItem('item active')+'|'+matchItemAgain('item')+'|'+
          localStorage.getItem('x')+'|'+sessionStorage.getItem('y');
    )JS",&browserCompatResult,&error)&&
          browserCompatResult==L"8|#comment|<!--marker-->|4|2|[a-z]+|de|abc|true|true|null|null",
          error.c_str());
    Check(browserCompatResult==L"8|#comment|<!--marker-->|4|2|[a-z]+|de|abc|true|true|null|null",
          L"comments, document order, strings, RegExp source, storage and inferred function names follow browser semantics");
    Check(browserCompatJs.Execute(
          L"const detached=document.createElement('div'),before=detached.isConnected;"
          L"document.body.appendChild(detached);const during=detached.isConnected;"
          L"detached.remove();return before+'|'+during+'|'+detached.isConnected;",
          &browserCompatResult,&error)&&browserCompatResult==L"false|true|false",error.c_str());
    Check(browserCompatResult==L"false|true|false",
          L"Node isConnected follows attachment to and removal from the active document");
    Check(browserCompatJs.Execute(
        L"var windowEvents=0,detachedEvents=0;"
        L"window.addEventListener('path-probe',function(){windowEvents++;},true);"
        L"var detached=document.createElement('img');"
        L"detached.addEventListener('path-probe',function(){detachedEvents++;});"
        L"detached.dispatchEvent(new Event('path-probe'));"
        L"var connected=document.createElement('img');document.body.appendChild(connected);"
        L"connected.dispatchEvent(new Event('path-probe'));"
        L"return windowEvents+'|'+detachedEvents;",
        &browserCompatResult,&error)&&browserCompatResult==L"1|1",error.c_str());
    Check(browserCompatResult==L"1|1",
          L"detached node events stay off the document and window propagation path");
    Check(browserCompatJs.Execute(
        L"var parent=document.createElement('div'),oldChild=document.createElement('span'),"
        L"replacement=document.createElement('img');oldChild.id='old-child';"
        L"replacement.id='replacement';parent.appendChild(oldChild);document.body.appendChild(parent);"
        L"var returned=parent.replaceChild(replacement,oldChild);"
        L"return (returned===oldChild)+'|'+(oldChild.parentNode===null)+'|'"
        L"+(replacement.parentNode===parent)+'|'+parent.firstChild.id;",
        &browserCompatResult,&error)&&browserCompatResult==L"true|true|true|replacement",
        error.c_str());
    Check(browserCompatResult==L"true|true|true|replacement",
          L"Node.replaceChild connects a detached replacement and returns the removed child");
    Check(browserCompatJs.Execute(LR"JS(
        var observerLog=[];var target=document.getElementById('first');
        var mutations=new MutationObserver(function(records){observerLog.push(records[0].type+':'+records[0].attributeName);});
        mutations.observe(target,{attributes:true,attributeFilter:['data-state']});
        var intersections=new IntersectionObserver(function(entries,observer){observerLog.push('intersection:'+entries[0].isIntersecting);observer.unobserve(target);});
        intersections.observe(target);target.setAttribute('data-state','ready');
    )JS",nullptr,&error),error.c_str());
    Check(browserCompatJs.Execute(L"return observerLog.join('|');",&browserCompatResult,&error)&&
          browserCompatResult==L"intersection:true|attributes:data-state",error.c_str());
    Check(browserCompatResult==L"intersection:true|attributes:data-state",
          L"intersection and mutation observers deliver generic DOM records at microtask checkpoints");
    Check(browserCompatJs.Execute(LR"JS(
        var async=1;async=2;
        var computed={['dynamic'+'Key']:3,['method'](){return 4;}};
        var marker=0,keys=[];for(var key in (marker=5,{first:1,second:2}))keys.push(key);
        class Base { constructor(value){this.baseValue=value;} inherited(){return 9;} get inheritedValue(){return 11;} set assigned(value){this.saved=value;} static get category(){return 'base';} }
        class Derived extends Base { constructor(value){super(value+1);this.derivedValue=value;} local(){return 10;} }
        class FieldBase { baseField=21; }class FieldDerived extends FieldBase { emptyField;derivedField=this.baseField+1; }
        var reversed=[1,2,3].reverse();var derived=new Derived(14);derived.assigned=12;
        var fields=new FieldDerived();
        var destructured=[];for(let [left,right] of [[1,2],[3,4]])destructured.push(left+right);
        var regexAfterControl=false;if(true)/data-/.test('data-x')&&(regexAfterControl=true);
        var symbolOne=Symbol('key'),symbolTwo=Symbol('key'),symbolObject={[symbolOne]:13};
        var bundledModules={1490(value){return value+1;},'named-module'(){return 16;}};
        function propertyCarrier(){}propertyCarrier.visible=26;
        var weakKey={},weakMap=new WeakMap([[weakKey,27]]),weakSet=new WeakSet([weakKey]);
        function proxyTarget(value){return this.base+value;}proxyTarget.answer=31;
        var proxy=new Proxy(proxyTarget,{
          get(target,key,receiver){return key==='virtual'?32:Reflect.get(target,key,receiver);},
          set(target,key,value){return Reflect.set(target,key,value+1);},
          apply(target,receiver,args){return Reflect.apply(target,receiver,args)+1;}
        });proxy.written=32;
        var dollarExport={$W:34};Object.defineProperty(dollarExport,'$getter',{enumerable:true,get(){return 35;}});
        var dollarKeys=[];for(var dollarKey in dollarExport)dollarKeys.push(dollarKey);
        let {index:destructuredDefault=18,preservedNull=19}={preservedNull:null};
        let {[symbolOne]:computedBinding=20}=symbolObject;
        let [nestedFirst,{value:nestedValue=nestedFirst+1}]=[23,{}];
        let {params:{create:nestedPatternDefault=25}={}}={};
        function templateTag(parts,value){return parts[0]+value+parts[1]+'/'+parts.raw[0];}
        return async+'|'+computed.dynamicKey+'|'+computed.method()+'|'+marker+'|'+keys.length+'|'+
          String.fromCharCode(65,66)+'|'+String.fromCodePoint(0x1f600).length+'|'+new Array(3).join('x')+'|'+
          Array.call(null,6,7).join('')+'|'+Number.call(null,'8')+'|'+reversed.join('')+'|'+
          derived.inherited()+'|'+derived.local()+'|'+derived.inheritedValue+'|'+derived.saved+'|'+derived.baseValue+'|'+derived.derivedValue+'|'+Derived.category+'|'+first.hasChildNodes()+'|'+
          unescape(escape('A B'))+'|'+templateTag`left${6}right`+'|'+destructured.join(',')+'|'+regexAfterControl+'|'+
          (symbolOne!==symbolTwo)+'|'+symbolObject[symbolOne]+'|'+(Symbol.for('shared')===Symbol.for('shared'))+'|'+Symbol.keyFor(Symbol.for('shared'))+'|'+
          bundledModules[1490](16)+'|'+bundledModules['named-module']()+'|'+destructuredDefault+'|'+preservedNull+'|'+computedBinding+'|'+
          fields.baseField+'|'+fields.derivedField+'|'+fields.hasOwnProperty('emptyField')+'|'+nestedFirst+'|'+nestedValue+'|'+nestedPatternDefault+'|'+
          Object.keys(propertyCarrier).join(',')+'|'+Object.values(propertyCarrier)[0]+'|'+Object.hasOwn(propertyCarrier,'visible')+'|'+
          weakMap.get(weakKey)+'|'+weakMap.has(weakKey)+'|'+weakSet.has(weakKey)+'|'+(window.WeakMap===WeakMap)+'|'+
          proxy.virtual+'|'+proxy.answer+'|'+proxyTarget.written+'|'+proxy.call({base:2},3)+'|'+typeof proxy+'|'+
          dollarKeys.includes('$W')+'|'+Object.keys(dollarExport).includes('$getter')+'|'+dollarExport.$getter;
    )JS",&browserCompatResult,&error)&&
          browserCompatResult==L"2|3|4|5|2|AB|2|xx|67|8|321|9|10|11|12|15|14|base|true|A B|left6right/left|3,7|true|true|13|true|shared|17|16|18|null|13|21|22|true|23|24|25|visible|26|true|27|true|true|true|32|31|33|6|function|true|true|35",error.c_str());
    Check(browserCompatResult==L"2|3|4|5|2|AB|2|xx|67|8|321|9|10|11|12|15|14|base|true|A B|left6right/left|3,7|true|true|13|true|shared|17|16|18|null|13|21|22|true|23|24|25|visible|26|true|27|true|true|true|32|31|33|6|function|true|true|35",
          L"computed object keys, async identifiers, for-in expressions, string factories and callable constructors follow browser semantics");
    Check(browserCompatJs.Execute(LR"JS(
        class ComputedMembers {
          *[Symbol.iterator](){yield 35;yield* [36];}
          ['field'+'Name']=37;
          get ['computed'+'Value'](){return this.fieldName+1;}
          static ['answer'](){return 39;}
        }
        var computedMembers=new ComputedMembers();
        var second=0,total=0;for([,second] of [[1,2],[3,4]])total+=second;
        function* generate(){yield 40;yield* [41,42];}
        var iterator=generate(),first=iterator.next(),remaining=generate();
        return [...computedMembers[Symbol.iterator]()].join(',')+'|'+computedMembers.fieldName+'|'+
          computedMembers.computedValue+'|'+ComputedMembers.answer()+'|'+total+'|'+
          first.value+'|'+first.done+'|'+[...remaining].join(',');
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"35,36|37|38|39|6|40|false|40,41,42",error.c_str());
    Check(browserCompatResult==L"35,36|37|38|39|6|40|false|40,41,42",
          L"computed class members, generators and destructuring assignment in for-of loops follow browser semantics");
    Check(browserCompatJs.Execute(LR"JS(
        var stateKey=Symbol('state'),carrier={},state={items:new Map([['first',1]]),attempts:0};
        Object.defineProperty(carrier,stateKey,{configurable:true,enumerable:false,value:state});
        var restored=Reflect.get(Object.getOwnPropertyDescriptor(carrier,stateKey),'value');
        var map=restored.items,set=new Set([2,3]),receiverError='';
        Map.prototype.set.call(map,'second',4);Set.prototype.add.call(set,5);
        try{Map.prototype.get.call({},'first');}catch(error){receiverError=error.name;}
        return map instanceof Map && set instanceof Set && !(set instanceof Map) && !(map instanceof Set) &&
            Object.getPrototypeOf(map)===Map.prototype && Object.getPrototypeOf(set)===Set.prototype &&
            map.constructor===Map && set.constructor===Set && Map.prototype.get.call(map,'second')===4 &&
            [...Map.prototype.values.call(map)].join(',')==='1,4' && Set.prototype.has.call(set,5) &&
            [...Set.prototype.values.call(set)].join(',')==='2,3,5' && receiverError==='TypeError' &&
            Object.getOwnPropertySymbols(carrier)[0]===stateKey && Object.keys(Map.prototype).length===0;
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"true",
          L"Map and Set instances preserve prototype identity and symbol-carried state across independent consumers");
    Check(browserCompatJs.Execute(LR"JS(
        var key=Symbol('state'),target={plain:1},getterCalls=0;
        Object.defineProperty(target,key,{value:2,configurable:true});
        Object.defineProperty(target,'accessor',{get:function(){getterCalls++;return 3;},configurable:true});
        Object.defineProperty(target,'fixed',{value:4,configurable:false});
        var deleted=Reflect.deleteProperty(target,key) && Reflect.deleteProperty(target,'accessor') &&
            Reflect.deleteProperty(target,'plain') && Reflect.deleteProperty(target,'absent');
        var fixed=!Reflect.deleteProperty(target,'fixed') && target.fixed===4 &&
            !Object.getOwnPropertyDescriptor(target,'fixed').configurable;
        var trapKey='',trapTarget=null,proxied=new Proxy(target,{deleteProperty:function(object,name){trapTarget=object;trapKey=name;return false;}});
        var trapped=!Reflect.deleteProperty(proxied,key) && trapTarget===target && trapKey===key;
        var errors=[];for(var input of [undefined,null,1,'text']){
            try{Reflect.deleteProperty(input,'x');}catch(error){errors.push(error.name);}
        }
        return deleted && fixed && trapped && getterCalls===0 && !Object.hasOwn(target,'accessor') &&
            Object.getOwnPropertySymbols(target).length===0 && errors.join('|')==='TypeError|TypeError|TypeError|TypeError';
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"true",
          L"Reflect.deleteProperty removes symbol and accessor state, honors configurability and forwards Proxy traps");
    Check(browserCompatJs.Execute(LR"JS(
        var dateFormatter=new Intl.DateTimeFormat('en-US',{year:'numeric',month:'2-digit',day:'2-digit'});
        var dateOptions=dateFormatter.resolvedOptions(),originalLocale=dateOptions.locale;
        dateOptions.locale='changed';var cleanDateOptions=dateFormatter.resolvedOptions();
        var numberFormatter=new Intl.NumberFormat(['en-US'],{useGrouping:false,minimumFractionDigits:2,maximumFractionDigits:2});
        var numberOptions=numberFormatter.resolvedOptions();
        var clockOptions=new Intl.DateTimeFormat('en-US',{hour:'numeric',hour12:false}).resolvedOptions();
        var defaultDate=new Intl.DateTimeFormat().resolvedOptions();
        return originalLocale==='en-US' && cleanDateOptions!==dateOptions && cleanDateOptions.locale===originalLocale &&
            cleanDateOptions.calendar==='gregory' && cleanDateOptions.year==='numeric' && cleanDateOptions.month==='2-digit' &&
            cleanDateOptions.day==='2-digit' && typeof cleanDateOptions.timeZone==='string' && cleanDateOptions.timeZone.length>0 &&
            numberOptions.locale==='en-US' && numberOptions.useGrouping===false && numberOptions.minimumFractionDigits===2 &&
            numberOptions.maximumFractionDigits===2 && numberFormatter.format(1234.5)==='1234.50' &&
            clockOptions.hour12===false && clockOptions.hourCycle==='h23' && defaultDate.year==='numeric';
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"true",
          L"Intl formatters expose fresh resolved options and the host time zone rather than omitting the standard method");
    Check(browserCompatJs.Execute(LR"JS(
        class ConstructorRoot { constructor(value){this.rootValue=value;} }
        class ConstructorMiddle extends ConstructorRoot {}
        class ConstructorLeaf extends ConstructorMiddle { constructor(value){super(value+1);} }
        return new ConstructorLeaf(40).rootValue;
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"41",error.c_str());
    Check(browserCompatResult==L"41",
          L"super calls through constructor-less derived classes reach the inherited constructor");
    Check(browserCompatJs.Execute(LR"JS(
        function Widget(){}class Panel{}var widget=new Widget(),panel=new Panel();
        return [(function(){}) instanceof Function,(()=>0) instanceof Function,
          Widget instanceof Function,Panel instanceof Function,[] instanceof Array,
          new Array(2) instanceof Array,[] instanceof Object,widget instanceof Widget,
          widget instanceof Object,panel instanceof Panel,new Date(0) instanceof Date,
          Promise.resolve(1) instanceof Promise,/x/ instanceof Object,null instanceof Object].join('|');
    )JS",&browserCompatResult,&error)&&
          browserCompatResult==L"true|true|true|true|true|true|true|true|true|true|true|true|true|false",
          error.c_str());
    Check(browserCompatResult==L"true|true|true|true|true|true|true|true|true|true|true|true|true|false",
          L"instanceof follows standard Function, Array, Object and constructor prototype relationships");
    Check(browserCompatJs.Execute(LR"JS(
        var numericObject={valueOf(){return 4;}};
        var stringObject={toString(){return 'prefix';}};
        var uuidSeed=[1e7]+-1e3+-4e3+-8e3+-1e11;
        return ([1,2]+3)+'|'+(numericObject+5)+'|'+(stringObject+5)+'|'+
          typeof uuidSeed+'|'+uuidSeed;
    )JS",&browserCompatResult,&error)&&
          browserCompatResult==L"1,23|9|prefix5|string|10000000-1000-4000-8000-100000000000",
          error.c_str());
    Check(browserCompatResult==L"1,23|9|prefix5|string|10000000-1000-4000-8000-100000000000",
          L"addition applies ordinary object-to-primitive conversion and array stringification");
    Check(browserCompatJs.Execute(
          L"return 131..toString()+'|'+0b101+'|'+0o10+'|'+0x10+'|'+.5+'|'+1.25e+2;",
          &browserCompatResult,&error)&&browserCompatResult==L"131|5|8|16|0.5|125",error.c_str());
    Check(browserCompatResult==L"131|5|8|16|0.5|125",
          L"numeric literals stop before member access and support standard radix and exponent forms");
    Check(browserCompatJs.Execute(
          L"return Object.is(NaN,NaN)+'|'+Object.is(0,-0)+'|'+Object.is('x','x')+'|'"
          L"+Object.is({},{});",
          &browserCompatResult,&error)&&browserCompatResult==L"true|false|true|false",error.c_str());
    Check(browserCompatResult==L"true|false|true|false",
          L"Object.is applies SameValue equality for numbers, primitives and object identity");
    Check(browserCompatJs.Execute(LR"JS(
        var classicGlobal=(function(global){return global===window&&global.document===document;})(this);
        return classicGlobal+'|'+(this===window);
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"true|true",error.c_str());
    Check(browserCompatResult==L"true|true",
          L"classic scripts expose Window as their top-level this value");
    Check(browserCompatJs.Execute(L"window.scriptExport={value:1};",nullptr,&error),error.c_str());
    Check(browserCompatJs.Execute(
          L"var scriptExport=scriptExport||{};scriptExport.second=2;"
          L"return (scriptExport===window.scriptExport)+'|'+scriptExport.value+'|'+window.scriptExport.second;",
          &browserCompatResult,&error)&&browserCompatResult==L"true|1|2",error.c_str());
    Check(browserCompatJs.Execute(
          L"window.scriptExport={value:3};return (scriptExport===window.scriptExport)+'|'+scriptExport.value;",
          &browserCompatResult,&error)&&browserCompatResult==L"true|3",error.c_str());
    Check(browserCompatResult==L"true|3",
          L"global var bindings and Window properties share storage across classic scripts");
    Check(browserCompatJs.Execute(
          L"var first=document.createElement('div'),rects=first.getClientRects();"
          L"return typeof first.getClientRects+'|'+rects.length;",
          &browserCompatResult,&error)&&browserCompatResult==L"function|0",error.c_str());
    Check(browserCompatResult==L"function|0",
          L"elements expose an empty DOMRectList when they have no rendered geometry");
    Check(browserCompatJs.Execute(
          L"return Date.parse('2024-01-02T03:04:05.006Z')+'|'"
          L"+Number.isNaN(Date.parse('not a date'))+'|'"
          L"+new Intl.NumberFormat('en-US').format(12345.5)+'|'"
          L"+new Intl.NumberFormat('en-US',{useGrouping:false,minimumFractionDigits:2,maximumFractionDigits:2}).format(12345.5);",
          &browserCompatResult,&error)&&
          browserCompatResult==L"1704164645006|true|12,345.5|12345.50",error.c_str());
    Check(browserCompatResult==L"1704164645006|true|12,345.5|12345.50",
          L"Date.parse and Intl.NumberFormat expose browser-compatible static formatting APIs");
    Check(browserCompatJs.Execute(
          L"var zeroEntry={A:0};return (zeroEntry.A!=null)+'|'+(0==null)+'|'"
          L"+(null==undefined)+'|'+('5'==5)+'|'+(false==0)+'|'+([1]==1);",
          &browserCompatResult,&error)&&browserCompatResult==L"true|false|true|true|true|true",error.c_str());
    Check(browserCompatResult==L"true|false|true|true|true|true",
          L"abstract equality keeps nullish values distinct from numbers and converts other primitives and objects");
    Check(browserCompatJs.Execute(L"return new Date(0).toString();",&browserCompatResult,&error)&&
          browserCompatResult==L"Thu, 01 Jan 1970 00:00:00 GMT",error.c_str());
    Check(browserCompatResult==L"Thu, 01 Jan 1970 00:00:00 GMT",
          L"Date exposes a stable general-purpose string conversion");
    Check(browserCompatJs.Execute(
          L"return [1,2].flatMap(function(value,index,array){return [value+index,array.length];}).join(',');",
          &browserCompatResult,&error)&&browserCompatResult==L"1,2,3,2",error.c_str());
    Check(browserCompatResult==L"1,2,3,2",
          L"Array flatMap invokes its callback with standard arguments and flattens one level");
    Check(browserCompatJs.Execute(LR"JS(
        var optionalCalls=0,missing=null,present={method(){optionalCalls++;return {value:7};}};
        return missing?.method()+'|'+missing?.deep.value+'|'+present?.method().value+'|'+
          optionalCalls+'|'+({}).missing?.();
    )JS",&browserCompatResult,&error)&&
          browserCompatResult==L"undefined|undefined|7|1|undefined",error.c_str());
    Check(browserCompatResult==L"undefined|undefined|7|1|undefined",
          L"optional chaining short-circuits the complete property and call chain");
    Check(browserCompatJs.Execute(LR"JS(
        var mapLog=[],setLog=[],mapValue=new Map([['a',1],['b',2]]),setValue=new Set(['x','y']);
        mapValue.forEach(function(value,key,owner){mapLog.push(key+value+(owner===mapValue));});
        setValue.forEach(function(value,key,owner){setLog.push(value+key+(owner===setValue));});
        return mapLog.join(',')+'|'+setLog.join(',')+'|'+Array.from(mapValue.keys()).join(',')+'|'+
          Array.from(setValue.entries()).map(function(entry){return entry.join('');}).join(',');
    )JS",&browserCompatResult,&error)&&
          browserCompatResult==L"a1true,b2true|xxtrue,yytrue|a,b|xx,yy",error.c_str());
    Check(browserCompatResult==L"a1true,b2true|xxtrue,yytrue|a,b|xx,yy",
          L"Map and Set expose standard key, entry and forEach iteration semantics");
    Check(browserCompatJs.Execute(LR"JS(
        var headers=new Headers({'X-Test':'one'}),headerLog=[];
        headers.append('x-test','two');headers.set('Content-Type','text/plain');
        headers.forEach(function(value,key,owner){headerLog.push(key+'='+value+':' +(owner===headers));});
        class ConstructorBase { constructor(value){this.constructed=value;} }
        class ImplicitDerived extends ConstructorBase { marker=3; }
        var implicitDerived=new ImplicitDerived(8);
        return headers.get('X-TEST')+'|'+headers.has('content-type')+'|'+headerLog.length+'|'+
          implicitDerived.constructed+'|'+implicitDerived.marker;
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"one, two|true|2|8|3",error.c_str());
    Check(browserCompatResult==L"one, two|true|2|8|3",
          L"Headers normalize names and implicit derived constructors forward arguments to their base");
    Check(browserCompatJs.Execute(LR"JS(
        const outer=[];var closure;let label='outer';
        { const outer=null;let captured=9;closure=()=>captured; }
        while(true){ { let label='inner';break; } }
        try { { const label='try';throw new Error(label); } } catch(error) { outer.push(error.message); }
        outer.push('ready');
        return outer.join(',')+'|'+closure()+'|'+label;
    )JS",&browserCompatResult,&error)&&browserCompatResult==L"try,ready|9|outer",error.c_str());
    Check(browserCompatResult==L"try,ready|9|outer",
          L"block-scoped declarations shadow without overwriting outer bindings across jumps and exceptions");

    Document dynamicScriptDoc;Check(dynamicScriptDoc.Parse(L"<html><head></head><body></body></html>",&error),
          L"dynamic script scheduling fixture parses");
    JavaScriptRuntime dynamicScriptJs(dynamicScriptDoc);
    dynamicScriptJs.SetResourceLoader([](const std::wstring& resource,std::wstring& source){
        if(resource==L"external.js"){
            source=L"dynamicOrder.push('external');callbackReady();";return true;
        }
        if(resource==L"scoped-external.js"){
            source=L"scopedLoad+=10;";return true;
        }
        return false;
    });
    const bool dynamicInlineExecuted=dynamicScriptJs.Execute(LR"JS(
        var inlineOrder=[];
        var inline=document.createElement('script');
        inline.innerHTML="if(1 < 2 && 3 > 2) inlineOrder.push('inline-innerHTML');";
        document.head.appendChild(inline);
        return inlineOrder.join(',')+'|'+inline.textContent;
    )JS",&browserCompatResult,&error);
    if(!dynamicInlineExecuted||browserCompatResult!=
       L"inline-innerHTML|if(1 < 2 && 3 > 2) inlineOrder.push('inline-innerHTML');")
        std::wcerr<<L"dynamic inline actual: executed="<<dynamicInlineExecuted
                  <<L" result=["<<browserCompatResult<<L"] error=["<<error<<L"]\n";
    Check(dynamicInlineExecuted&&
          browserCompatResult==L"inline-innerHTML|if(1 < 2 && 3 > 2) inlineOrder.push('inline-innerHTML');",
          error.c_str());
    Check(browserCompatResult==
          L"inline-innerHTML|if(1 < 2 && 3 > 2) inlineOrder.push('inline-innerHTML');",
          L"script innerHTML preserves raw text and executes when the element is connected");
    Check(dynamicScriptJs.Execute(LR"JS(
        var dynamicOrder=[];
        var external=document.createElement('script');external.src='external.js';
        external.onload=function(){dynamicOrder.push('load');};
        document.head.appendChild(external);
        dynamicOrder.push('after-append');
        window.callbackReady=function(){dynamicOrder.push('callback');};
    )JS",nullptr,&error),error.c_str());
    Check(dynamicScriptJs.Execute(L"return dynamicOrder.join(',');",&browserCompatResult,&error)&&
          browserCompatResult==L"after-append",error.c_str());
    Check(browserCompatResult==L"after-append",
          L"DOM-inserted external scripts wait for a later event-loop task");
    dynamicScriptJs.RunTimers();
    Check(dynamicScriptJs.Execute(L"return dynamicOrder.join(',');",&browserCompatResult,&error)&&
          browserCompatResult==L"after-append,external,callback,load",error.c_str());
    Check(browserCompatResult==L"after-append,external,callback,load",
          L"a later event-loop task executes the external script and dispatches load");
    Check(dynamicScriptJs.Execute(LR"JS(
        var scopedLoad=0;
        (function(){
            const script=document.createElement('script');
            script.src='scoped-external.js';
            script.onload=function(){scopedLoad++;};
            document.head.appendChild(script);
        })();
    )JS",nullptr,&error),error.c_str());
    dynamicScriptJs.RunTimers();
    Check(dynamicScriptJs.Execute(L"return scopedLoad;",&browserCompatResult,&error)&&
          browserCompatResult==L"11",error.c_str());
    Check(browserCompatResult==L"11",
          L"a connected script retains its load handler until its queued resource task completes");
    std::vector<std::pair<std::wstring,
        std::function<void(bool,std::wstring)>>> pendingScriptLoads;
    dynamicScriptJs.SetAsyncResourceLoader(
        [&](const std::wstring& resource,std::function<void(bool,std::wstring)> complete){
            pendingScriptLoads.push_back({resource,std::move(complete)});
        });
    Check(dynamicScriptJs.Execute(LR"JS(
        var orderedScripts=[];
        var first=document.createElement('script');first.async=false;first.src='ordered-first.js';
        first.onload=function(){orderedScripts.push('first-load');};
        var second=document.createElement('script');second.async=false;second.src='ordered-second.js';
        second.onload=function(){orderedScripts.push('second-load');};
        document.head.appendChild(first);document.head.appendChild(second);
    )JS",nullptr,&error),error.c_str());
    dynamicScriptJs.RunTimers();
    Check(pendingScriptLoads.size()==2,
          L"a timer slice starts independent ordered script downloads in parallel");
    if(pendingScriptLoads.size()==2){
        pendingScriptLoads[1].second(true,L"orderedScripts.push('second');");
        Check(dynamicScriptJs.Execute(L"return orderedScripts.join(',');",
                                      &browserCompatResult,&error)&&browserCompatResult.empty(),
              L"an async=false script waits when an earlier inserted script is still loading");
        pendingScriptLoads[0].second(true,L"orderedScripts.push('first');");
        Check(dynamicScriptJs.Execute(L"return orderedScripts.join(',');",
                                      &browserCompatResult,&error)&&
              browserCompatResult==L"first,first-load,second,second-load",error.c_str());
        Check(browserCompatResult==L"first,first-load,second,second-load",
              L"async=false scripts execute in insertion order after parallel loading");
    }
    dynamicScriptJs.SetAsyncResourceLoader({});

    Document jitDoc;Check(jitDoc.Parse(L"<body></body>",&error),L"baseline JIT fixture parses");
    JavaScriptRuntime jitJs(jitDoc);std::wstring jitResult;jitJs.SetJitCompilationThreshold(2);
    Check(jitJs.Execute(LR"(
        function jitSum(n){let total=0;for(let i=0;i<n;i++){total+=i;}return total;}
        function jitAbs(n){if(n<0)return -n;return n;}
        function jitGuard(n){return n+1;}
        function jitOps(a,b){let value=a+b;value=value-b;value=value*3;return value/2;}
        function jitInc(n){let x=n;let a=x++;let b=++x;let c=x--;let d=--x;return a+b+c+d+x;}
        function jitLess(a,b){return a<b;}
        function jitEqual(a,b){return a===b;}
        function jitRelation(a,b){
            let value=0;
            if(a<=b)value=value+1;
            if(a>b)value=value+2;
            if(a>=b)value=value+4;
            if(a!==b)value=value+8;
            return value;
        }
        function jitNot(n){return !n;}
        let answer=0;
        for(let i=0;i<8;i++)answer+=jitSum(1000+i);
        for(let i=0;i<8;i++)answer+=jitAbs(-i);
        for(let i=0;i<4;i++){
            jitGuard(i);jitOps(i+4,2);jitInc(i);jitLess(i,i+1);jitEqual(i,i);
            jitRelation(i+1,i);jitNot(i);
        }
        return answer+'|'+jitOps(8,2)+'|'+jitInc(5)+'|'+jitLess(1,2)+'|'
            +jitEqual(2,2)+'|'+jitRelation(3,2)+'|'+jitRelation(0/0,2)+'|'
            +jitLess(0/0,2)+'|'+jitNot(0/0)+'|'+jitNot(0)+'|'+jitNot(4)+'|'+jitGuard('x');
    )",&jitResult,&error)&&
          jitResult==L"4024084|12|29|true|true|14|8|false|true|true|false|x1",error.c_str());
#if defined(_M_X64)
    const auto jitStatistics=jitJs.GetJitStatistics();
    Check(jitStatistics.compiledFunctions>=9&&jitStatistics.generatedCodeBytes>0&&
          jitStatistics.nativeCalls>=9,
          L"hot numeric functions are promoted to x64 baseline JIT code");
    Check(jitStatistics.guardFallbacks>=1,
          L"baseline JIT type guard failures return to the interpreter");
    jitJs.SetJitCompilationThreshold(0);const auto nativeCallsBeforeDisable=jitStatistics.nativeCalls;
    Check(jitJs.Execute(L"return jitSum(10);",&jitResult,&error)&&jitResult==L"45"&&
          jitJs.GetJitStatistics().nativeCalls==nativeCallsBeforeDisable,
          L"baseline JIT can be disabled per runtime without changing execution results");
    jitJs.Clear();
    const auto clearedJitStatistics=jitJs.GetJitStatistics();
    Check(clearedJitStatistics.compiledFunctions==0&&clearedJitStatistics.generatedCodeBytes==0,
          L"runtime reset releases baseline JIT code and statistics");
#endif

    Document switchDoc;Check(switchDoc.Parse(L"<body></body>",&error),L"switch JavaScript fixture parses");
    JavaScriptRuntime switchJs(switchDoc);std::wstring switchResult;
    Check(switchJs.Load(L"function choose(value){let result='';switch(value){case 'a':result+='a';break;default:result+='d';case 'b':result+='b';}return result;}",&error),error.c_str());
    Check(switchJs.Execute(L"return choose('a')+'|'+choose('b')+'|'+choose('x');",&switchResult,&error),error.c_str());
    Check(switchResult==L"a|b|db",L"switch supports matching, break, default and fallthrough");
    Document semanticsDoc;Check(semanticsDoc.Parse(L"<!doctype html><body></body>",&error),L"JavaScript semantics fixture parses");
    JavaScriptRuntime semanticsJs(semanticsDoc);std::wstring semanticsResult;
    semanticsJs.SetViewportSize(1280,720);
    Check(semanticsJs.Execute(
        L"return innerWidth+'|'+innerHeight+'|'+window.innerWidth+'|'+window.innerHeight;",
        &semanticsResult,&error)&&semanticsResult==L"1280|720|1280|720",
        L"viewport dimensions are exposed through browser window globals");
    Check(semanticsJs.Execute(
        L"var mark=performance.mark('ready');return performance.getEntriesByType('navigation').length+'|'"
        L"+mark.name+'|'+mark.entryType+'|'+(performance.now()>0);",
        &semanticsResult,&error)&&semanticsResult==L"0|ready|mark|true",
        L"Performance entry queries and user timing methods expose browser-compatible values");
    semanticsJs.SetDevicePixelRatio(1.5);
    Check(semanticsJs.Execute(
        L"return (window.document===document)+'|'+document.compatMode+'|'+document.readyState+'|'"
        L"+screen.width+'|'+screen.height+'|'+navigator.appName+'|'"
        L"+matchMedia('(min-width: 1000px) and (orientation: landscape)').matches+'|'"
        L"+matchMedia('(max-width: 600px)').matches+'|'"
        L"+matchMedia('(min-resolution: 1.5dppx)').matches;",
        &semanticsResult,&error)&&
        semanticsResult==L"true|CSS1Compat|complete|1280|720|Netscape|true|false|true",
        L"browser globals and media queries reflect CSS viewport units and device scale");
    semanticsJs.SetLocation(L"https://example.test/path?q=1#part");
    Check(semanticsJs.Execute(
        L"return document.URL+'|'+document.documentURI+'|'+document.baseURI+'|'+document.referrer;",
        &semanticsResult,&error)&&semanticsResult==
        L"https://example.test/path?q=1#part|https://example.test/path?q=1#part|https://example.test/path?q=1#part|",
        L"document URL properties expose the active browsing-context location");
    Check(semanticsJs.Execute(
        L"var anchor=document.createElement('a');anchor.href='https://cdn.example.test:8443/ad?x=1#bid';"
        L"return location.protocol+'|'+location.host+'|'+location.hostname+'|'+location.port+'|'"
        L"+location.pathname+'|'+location.search+'|'+location.hash+'|'+location.origin+'|'"
        L"+anchor.href+'|'+anchor.protocol+'|'+anchor.host+'|'+anchor.hostname+'|'"
        L"+anchor.port+'|'+anchor.pathname+'|'+anchor.search+'|'+anchor.hash+'|'+anchor.origin;",
        &semanticsResult,&error)&&semanticsResult==
        L"https:|example.test|example.test||/path|?q=1|#part|https://example.test|"
        L"https://cdn.example.test:8443/ad?x=1#bid|https:|cdn.example.test:8443|cdn.example.test|8443|/ad|?x=1|#bid|https://cdn.example.test:8443",
        error.c_str());
    Check(semanticsResult==
        L"https:|example.test|example.test||/path|?q=1|#part|https://example.test|"
        L"https://cdn.example.test:8443/ad?x=1#bid|https:|cdn.example.test:8443|cdn.example.test|8443|/ad|?x=1|#bid|https://cdn.example.test:8443",
        L"Location and HTML anchor elements expose parsed URL components");
    Document currentScriptDoc;
    Check(currentScriptDoc.Parse(
        L"<html><head><script id='loader' src='library.js'></script></head><body></body></html>",
        &error),L"currentScript DOM fixture parses");
    JavaScriptRuntime currentScriptJs(currentScriptDoc);
    const auto currentScriptNode=currentScriptDoc.GetElementById(L"loader");
    currentScriptJs.SetCurrentScript(currentScriptNode);
    Check(currentScriptJs.Execute(
        L"return (document.currentScript===document.getElementById('loader'))+'|'"
        L"+(document.currentScript instanceof HTMLScriptElement)+'|'"
        L"+(document.currentScript instanceof HTMLElement)+'|'"
        L"+(document.currentScript instanceof Element)+'|'"
        L"+(document.currentScript instanceof Node)+'|'"
        L"+(document instanceof Document)+'|'+Node.ELEMENT_NODE;",
        &semanticsResult,&error)&&semanticsResult==L"true|true|true|true|true|true|1",
        L"classic scripts expose document.currentScript and standard DOM constructor identity");
    currentScriptJs.SetCurrentScript({});
    Check(currentScriptJs.Execute(L"return document.currentScript===null;",
                                  &semanticsResult,&error)&&semanticsResult==L"true",
          L"document.currentScript is cleared outside script evaluation");
    semanticsJs.SetGeometryProvider([](const std::shared_ptr<Node>& node){
        JavaScriptRuntime::NodeGeometry geometry;
        if(node&&node->Attribute(L"id")==L"hit-back")geometry={10,10,100,100,100,100,100,100};
        if(node&&node->Attribute(L"id")==L"hit-front")geometry={20,20,50,50,50,50,50,50};
        return geometry;
    });
    Check(semanticsJs.Execute(
        L"var back=document.createElement('div'),front=document.createElement('span');"
        L"back.id='hit-back';front.id='hit-front';back.appendChild(front);document.body.appendChild(back);"
        L"var topHit=document.elementFromPoint(30,30),hits=document.elementsFromPoint(30,30);"
        L"var result=topHit.id+'|'+hits.map(node=>node.id).join(',')+'|'"
        L"+(document.elementFromPoint(-1,30)===null);back.remove();return result;",
        &semanticsResult,&error)&&semanticsResult==L"hit-front|hit-front,hit-back|true",error.c_str());
    semanticsJs.SetGeometryProvider({});
    Check(semanticsResult==L"hit-front|hit-front,hit-back|true",
        L"document point queries return the topmost laid-out element in paint order");
    Check(semanticsJs.Execute(
        L"document.write('<div id=written-node>ready</div><script>window.documentWriteRan=7;<\\/script>');"
        L"var result=document.getElementById('written-node').textContent+'|'+documentWriteRan;"
        L"document.getElementById('written-node').remove();return result;",
        &semanticsResult,&error)&&semanticsResult==L"ready|7",error.c_str());
    Check(semanticsResult==L"ready|7",
        L"document.write appends parsed markup and executes connected inline scripts");
    Check(semanticsJs.Execute(
        L"window.libraryExport=()=>41;var declaredGlobal=1;"
        L"function assignUndeclared(){undeclaredGlobal=2;}assignUndeclared();"
        L"return libraryExport()+'|'+window.declaredGlobal+'|'+window.undeclaredGlobal+'|'"
        L"+('libraryExport' in window)+'|'+(window.Object===Object);",
        &semanticsResult,&error)&&semanticsResult==L"41|1|2|true|true",
        L"the Window object and global identifier environment share reusable browser script exports");
    Check(semanticsJs.Execute(
        L"function ordinaryThis(){return this===window;}"
        L"return ordinaryThis()+'|'+(function(){return this===window;})();",
        &semanticsResult,&error)&&semanticsResult==L"true|true",
        L"plain calls in classic scripts bind ordinary function this to Window");
    Check(semanticsJs.Execute(
        L"function strictThis(){'use strict';return this===undefined;}"
        L"function strictOuter(){'use strict';return function(){return this===undefined;};}"
        L"class StrictMethod{value(){return this===undefined;}}"
        L"var method=(new StrictMethod()).value;"
        L"return strictThis()+'|'+strictOuter()()+'|'+method();",
        &semanticsResult,&error)&&semanticsResult==L"true|true|true",
        L"strict directives, nested functions and class methods preserve an undefined this value");
    Check(semanticsJs.Execute(
        L"var outerChoice=()=>99;"
        L"function chooseLocal(){var outerChoice=outerChoice===undefined?()=>7:outerChoice;return outerChoice();}"
        L"function keepAssignment(){value=4;var value;return value;}"
        L"function blockVar(){if(true){var inside=6;}return inside;}"
        L"return chooseLocal()+'|'+keepAssignment()+'|'+blockVar();",
        &semanticsResult,&error)&&semanticsResult==L"7|4|6",
        L"var bindings are hoisted to function scope without resetting assignments at declaration sites");
    Check(semanticsJs.Execute(
        L"var shared={marker:17};"
        L"function chooseExisting(){var local;return (local=shared)!=null?local:shared={marker:99};}"
        L"var chosen=chooseExisting();return (chosen===shared)+'|'+chosen.marker+'|'+shared.marker;",
        &semanticsResult,&error)&&semanticsResult==L"true|17|17",
        L"conditional false arms retain assignment-expression precedence without capturing the whole ternary");
    Check(semanticsJs.Execute(
        L"function finiteDepth(value){return value===0?0:1+finiteDepth(value-1);}"
        L"return finiteDepth(96);",
        &semanticsResult,&error)&&semanticsResult==L"96",
        L"ordinary library call chains can exceed sixty-four interpreter frames");
    Check(semanticsJs.Execute(
        L"function recurse(){return recurse();}"
        L"try{recurse();}catch(error){return error.name+'|'+error.message;}",
        &semanticsResult,&error)&&
        semanticsResult==L"RangeError|Maximum call stack size exceeded",
        L"excessive script recursion becomes a catchable RangeError before native stack exhaustion");
    Check(semanticsJs.Execute(
        L"var applied=Function.prototype.apply.call(function(a,b){return this.base+a+b;},"
        L"{base:2},[3,4]);"
        L"var called=Function.prototype.call.call(function(a){return this.base+a;},{base:5},6);"
        L"return typeof Function+'|'+applied+'|'+called;",
        &semanticsResult,&error)&&semanticsResult==L"function|9|11",
        L"Function prototype call and apply invoke arbitrary callable receivers");
    Check(semanticsJs.Execute(
        L"var method=document.getElementsByClassName;"
        L"function userFunction(){};"
        L"return /\\[native code\\]/.test(String(method))+'|'"
        L"+/\\[native code\\]/.test(Function.prototype.toString.call(method))+'|'"
        L"+/\\[native code\\]/.test(userFunction.toString());",
        &semanticsResult,&error)&&semanticsResult==L"true|true|false",
        L"native DOM functions expose the standard source form used by feature detection");
    Check(semanticsJs.Execute(
        L"return typeof String.prototype+'|'+typeof RegExp.prototype+'|'"
        L"+RegExp.prototype.hasOwnProperty('sticky');",
        &semanticsResult,&error)&&semanticsResult==L"object|object|false",
        L"native constructors expose ordinary prototype objects");
    Check(semanticsJs.Execute(
        L"var words=new Uint32Array(4),bytes=new Uint8Array([257,-1,3]);"
        L"var same=crypto.getRandomValues(words)===words;"
        L"return typeof Uint32Array+'|'+words.length+'|'+Uint32Array.BYTES_PER_ELEMENT+'|'"
        L"+bytes.join(',')+'|'+same+'|'+(words[0]>=0&&words[0]<=4294967295)+'|'"
        L"+(window.crypto===crypto);",
        &semanticsResult,&error)&&semanticsResult==L"function|4|4|1,255,3|true|true|true",
        L"integer typed arrays and Web Crypto random filling expose browser-compatible globals");
    Check(semanticsJs.Execute(
        L"var id=crypto.randomUUID();return typeof crypto.randomUUID+'|'+id.length+'|'"
        L"+id[8]+id[13]+id[18]+id[23]+'|'+id[14]+'|'"
        L"+('89ab'.indexOf(id[19])>=0)+'|'+(/^[0-9a-f-]+$/.test(id));",
        &semanticsResult,&error)&&semanticsResult==L"function|36|----|4|true|true",
        L"crypto.randomUUID returns a lowercase RFC 4122 version 4 identifier");
    Check(semanticsJs.Execute(
        L"var bytes=new Uint8Array([1,2,3,4,5,6]);var view=new DataView(bytes.buffer);"
        L"bytes[1]=9;return view.getUint32(0)+'|'+view.getUint16(4)+'|'"
        L"+view.getUint32(0,true)+'|'+view.byteLength+'|'"
        L"+ArrayBuffer.isView(bytes)+'|'+ArrayBuffer.isView(view);",
        &semanticsResult,&error)&&semanticsResult==L"17367812|1286|67307777|6|true|true",
        L"typed-array buffers remain live through DataView integer reads and report view identity");
    Check(semanticsJs.Execute(
        L"var controller=new AbortController(),calls=0,reason='';"
        L"controller.signal.addEventListener('abort',function(){calls++;reason=controller.signal.reason;});"
        L"controller.abort('stopped');controller.abort('again');"
        L"return typeof controller.abort+'|'+controller.signal.aborted+'|'+calls+'|'+reason;",
        &semanticsResult,&error)&&semanticsResult==L"function|true|1|stopped",
        L"AbortController aborts once, records its reason and notifies signal listeners");
    Check(semanticsJs.Execute(
        L"function Inherited(){} function Base(){} Inherited.__proto__=Base;"
        L"Base.marker=7;var ordinary={};ordinary.__proto__=Base;"
        L"return (Inherited.__proto__===Base)+'|'+typeof Inherited.__proto__+'|'"
        L"+(ordinary.__proto__===Base)+'|'+ordinary.marker;",
        &semanticsResult,&error)&&semanticsResult==L"true|function|true|7",
        L"callable and ordinary objects preserve explicit __proto__ inheritance used by compiled libraries");
    Check(semanticsJs.Execute(
        L"var timestamp=new Date(1000);var returned=timestamp.setTime(timestamp.getTime()+2500);"
        L"var epoch=new Date(0);return returned+'|'+timestamp.getTime()+'|'+epoch.toUTCString()+'|'"
        L"+epoch.getUTCFullYear()+'|'+epoch.getUTCMonth()+'|'+epoch.getUTCDate()+'|'"
        L"+epoch.getUTCHours()+'|'+epoch.getUTCMinutes()+'|'+epoch.getUTCSeconds()+'|'"
        L"+typeof epoch.getTimezoneOffset()+'|'+Number.isInteger(epoch.getTimezoneOffset());",
        &semanticsResult,&error)&&semanticsResult==L"3500|3500|Thu, 01 Jan 1970 00:00:00 GMT|1970|0|1|0|0|0|number|true",
        L"Date mutation and UTC serialization follow browser timestamp semantics");
    Check(semanticsJs.Execute(
        L"Object.defineProperty(Array.prototype,'genericIterator',{value:function(){return 3;}});"
        L"var values=[];return ('genericIterator' in values)+'|'+values.genericIterator();",
        &semanticsResult,&error)&&semanticsResult==L"true|3",
        L"array instances inherit properties dynamically installed on Array.prototype");
    Check(semanticsJs.Execute(
        L"return isNaN(undefined)+'|'+isNaN('12')+'|'+isFinite(null)+'|'"
        L"+isFinite('Infinity')+'|'+parseInt('08')+'|'+parseInt('0x10')+'|'"
        L"+parseInt('-101tail',2)+'|'+Number.isNaN(parseInt('xyz'))+'|'"
        L"+(window.isNaN===isNaN)+'|'+(window.parseInt===parseInt);",
        &semanticsResult,&error)&&
        semanticsResult==L"true|false|true|false|8|16|-5|true|true|true",
        L"global numeric conversion functions follow browser coercion and radix rules");
    Check(semanticsJs.Execute(
        L"var uri='https://example.test/a b?q=\\uD55C\\uAE00&x=1#part';"
        L"return encodeURI(uri)+'|'+decodeURI('%2F%ED%95%9C%EA%B8%80')+'|'"
        L"+(window.decodeURI===decodeURI);",
        &semanticsResult,&error)&&
        semanticsResult==L"https://example.test/a%20b?q=%ED%95%9C%EA%B8%80&x=1#part|%2F\xD55C\xAE00|true",
        L"URI globals preserve reserved delimiters while encoding and decoding UTF-8");
    Check(semanticsJs.Execute(
        L"var sample='ABCZ';return sample.charAt(0)+'|'"
        L"+sample.charAt(20)+'|'+sample.charCodeAt(0)+'|'"
        L"+Number.isNaN(sample.charCodeAt(20))+'|'+sample.codePointAt(1)+'|'"
        L"+sample.substring(4,1).length+'|'+sample.substring(-2,1)+'|'"
        L"+'alpha42'.search(/\\d+/)+'|'+'alpha'.search('ph');",
        &semanticsResult,&error)&&semanticsResult==L"A||65|true|66|3|A|5|2",
        L"string character and substring methods follow browser index rules");
    Check(semanticsJs.Execute(
        L"var target={cmd:[]};function setOnce(name,value){var local=target;"
        L"local.hasOwnProperty(name)||(local[name]=value);}"
        L"setOnce('_loaded_',true);setOnce('_loaded_',false);"
        L"return target._loaded_+'|'+target.hasOwnProperty('_loaded_');",
        &semanticsResult,&error)&&semanticsResult==L"true|true",
        L"short-circuit expressions preserve computed property assignment references");
    Check(semanticsJs.Execute(
        L"function rotate(Q,q,qB,Bb,F,Oo){"
        L"for(qB={Q:751,q:606,F:628},Bb=function(value){return value;},F=Q();true;)"
        L"try{Oo=parseInt(Bb(qB.q))/2;return q+'|'+Oo+'|'+arguments[1]+'|'+qB.q;}"
        L"catch(error){return error.name;}}"
        L"return rotate(function(){return [];},954289);",
        &semanticsResult,&error)&&semanticsResult==L"954289|303|954289|606",error.c_str());
    Check(semanticsResult==L"954289|303|954289|606",
          L"object property names remain separate from same-named function parameters in for initializers");
    Check(semanticsJs.Execute(
        L"function decoderTable(value){return value='unusedB606B628'.split('B'),"
        L"decoderTable=function(){return value;},decoderTable();}"
        L"function decode(index,q,entries,result){return index=index-438,entries=decoderTable(),"
        L"result=entries[index],result;}"
        L"return (function(tableFactory,target,keys,decoder,table,computed){"
        L"for(keys={q:439},decoder=decode,table=tableFactory();true;)"
        L"try{computed=parseInt(decoder(keys.q))/2;"
        L"return target+'|'+computed+'|'+arguments[1]+'|'+decoder(keys.q);}"
        L"catch(error){return error.name;}}(decoderTable,954289));",
        &semanticsResult,&error)&&semanticsResult==L"954289|303|954289|606",error.c_str());
    Check(semanticsResult==L"954289|303|954289|606",
          L"nested decoders do not corrupt same-position parameters in an enclosing rotation function");
    Check(semanticsJs.Execute(
        L"function memoizedTable(value){return value='firstBsecondBthird'.split('B'),"
        L"memoizedTable=function(){return value;},memoizedTable();}"
        L"var first=memoizedTable();first.push(first.shift());var second=memoizedTable();"
        L"return (first===second)+'|'+second.join(',')+'|'+(memoizedTable===second);",
        &semanticsResult,&error)&&semanticsResult==L"true|second,third,first|false",error.c_str());
    Check(semanticsResult==L"true|second,third,first|false",
          L"a function declaration can replace its outer binding for self-memoizing decoders");
    Check(semanticsJs.Execute(
        L"var receiver={};receiver.hasOwnProperty=function(name){"
        L"window.methodTrace=(window.methodTrace||'')+name;return false;};"
        L"receiver.hasOwnProperty('x')||(receiver['x']=true);"
        L"return window.methodTrace+'|'+receiver.x;",
        &semanticsResult,&error)&&semanticsResult==L"x|true",
        L"method calls preserve property assignments made by nested execution frames");
    Check(semanticsJs.Execute(
        L"function Constructor(){}Constructor.cached=7;"
        L"return Constructor.hasOwnProperty('cached')+'|'"
        L"+Constructor.hasOwnProperty('missing')+'|'"
        L"+Object.prototype.hasOwnProperty.call(Constructor,'cached')+'|'"
        L"+[1].hasOwnProperty('0')+'|'+[1].hasOwnProperty('length');",
        &semanticsResult,&error)&&semanticsResult==L"true|false|true|true|true",
        L"Object prototype ownership checks apply to functions and arrays");
    Check(semanticsJs.Execute(
        L"var value={visible:1};"
        L"Object.defineProperty(value,'hidden',{value:2});"
        L"Object.defineProperty(value,'shown',{value:3,enumerable:true});"
        L"var names=Object.keys(value).sort().join(',');"
        L"return typeof Object.prototype.propertyIsEnumerable+'|'"
        L"+value.propertyIsEnumerable('visible')+'|'"
        L"+value.propertyIsEnumerable('hidden')+'|'"
        L"+Object.prototype.propertyIsEnumerable.call(value,'shown')+'|'"
        L"+[1].propertyIsEnumerable('length')+'|'"
        L"+[1].propertyIsEnumerable('0')+'|'"
        L"+Object('z').propertyIsEnumerable(0)+'|'+names;",
        &semanticsResult,&error)&&
        semanticsResult==L"function|true|false|true|false|true|true|shown,visible",
        error.c_str());
    Check(semanticsResult==L"function|true|false|true|false|true|true|shown,visible",
          L"propertyIsEnumerable and Object.keys honor own descriptor enumerability");
    Check(semanticsJs.Execute(
        L"function LegacyComponent(){}"
        L"Object.defineProperty(LegacyComponent.prototype,'componentWillMount',{"
        L"configurable:true,get:function(){return this.UNSAFE_componentWillMount;},"
        L"set:function(value){Object.defineProperty(this,'componentWillMount',{"
        L"configurable:true,writable:true,value:value});}});"
        L"var component=new LegacyComponent();"
        L"component.componentWillMount=function(){return 9;};"
        L"return component.hasOwnProperty('componentWillMount')+'|'"
        L"+component.componentWillMount()+'|'"
        L"+(LegacyComponent.prototype.componentWillMount===undefined);",
        &semanticsResult,&error)&&semanticsResult==L"true|9|true",error.c_str());
    Check(semanticsResult==L"true|9|true",
          L"defineProperty creates an own data property without re-entering an inherited setter");
    Check(semanticsJs.Execute(
        L"var jsonValue={visible:1};Object.defineProperty(jsonValue,'hidden',{value:2});"
        L"return JSON.stringify(jsonValue)+'|'"
        L"+(JSON.stringify(jsonValue).indexOf('$enumerable:')<0);",
        &semanticsResult,&error)&&semanticsResult==L"{\"visible\":1}|true",
        L"JSON.stringify omits non-enumerable properties and internal descriptor metadata");
    Check(semanticsJs.Execute(
        L"var sized=new Image(120.9,45.8),empty=Image();"
        L"return typeof Image+'|'+(window.Image===Image)+'|'"
        L"+(sized instanceof Image)+'|'+(sized instanceof HTMLImageElement)+'|'"
        L"+sized.tagName+'|'+sized.width+'|'+sized.height+'|'"
        L"+empty.complete+'|'+empty.naturalWidth;",
        &semanticsResult,&error)&&
        semanticsResult==L"function|true|true|true|IMG|120|45|true|0",
        error.c_str());
    Check(semanticsResult==L"function|true|true|true|IMG|120|45|true|0",
          L"Image constructs detached HTMLImageElement instances with optional dimensions");
    Check(semanticsJs.Execute(
        L"window.messageProbe='';window.addEventListener('message',function(event){"
        L"window.messageProbe=event.data.value+'|'+(event.source===window);});"
        L"postMessage({value:7},'*');return typeof postMessage+'|'"
        L"+(window.postMessage===postMessage);",
        &semanticsResult,&error)&&semanticsResult==L"function|true",error.c_str());
    semanticsJs.RunTimers();
    Check(semanticsJs.Execute(L"return window.messageProbe;",&semanticsResult,&error)&&
          semanticsResult==L"7|true",
          L"global postMessage queues a same-window message event with source and data");
    Check(semanticsJs.Execute(
        L"function stackOuter(){return stackInner();}"
        L"function stackInner(){return Error('boom').stack;}var value={},base={inherited:3};"
        L"var child=Object.create(base);Object.defineProperties(child,{own:{value:4}});"
        L"return Object.isExtensible(value)+'|'+Object.isFrozen(value)+'|'"
        L"+(Object.freeze(value)===value)+'|'"
        L"+(stackOuter().indexOf('at stackInner')>=0)+'|'"
        L"+(stackOuter().indexOf('at stackOuter')>=0)+'|'"
        L"+child.inherited+'|'+child.own+'|'"
        L"+(Object.getPrototypeOf(child)===base);",
        &semanticsResult,&error)&&
        semanticsResult==L"true|false|true|true|true|3|4|true",
        L"Object integrity methods and named Error stack frames follow browser semantics");
    Check(semanticsJs.Execute(
        L"function Base(){this.base=1;}Base.answer=42;"
        L"function Child(){return Reflect.construct(Base,[],Object.getPrototypeOf(this).constructor);}"
        L"Object.setPrototypeOf(Child,Base);"
        L"Child.prototype=Object.create(Base.prototype,{constructor:{value:Child}});"
        L"Child.prototype.own=function(){return this.base+6;};"
        L"function CustomError(message){return Reflect.construct(Error,[message],CustomError);}"
        L"Object.setPrototypeOf(CustomError,Error);"
        L"CustomError.prototype=Object.create(Error.prototype,{constructor:{value:CustomError}});"
        L"var reflected=Reflect.construct(Boolean,[],function(){});"
        L"return (Object.getPrototypeOf(Child)===Base)+'|'"
        L"+Child.answer+'|'+typeof Object.getPrototypeOf(Child).apply+'|'"
        L"+Boolean.prototype.valueOf.call(reflected)+'|'+new Child().own()+'|'"
        L"+(new CustomError('broken') instanceof CustomError)+'|'"
        L"+new CustomError('broken').message;",
        &semanticsResult,&error)&&semanticsResult==L"true|42|function|false|7|true|broken",
        L"callable prototype chains and Boolean reflection support transpiled inheritance helpers");
    Check(semanticsJs.Execute(
        L"function EventBase(){this.listeners={};}"
        L"EventBase.prototype.add=function(){this.listeners.ready=true;};"
        L"function finish(receiver,result){if(result&&('object'==typeof result||'function'==typeof result))"
        L"return result;if(result!==undefined)throw new TypeError();return receiver;}"
        L"function inherit(receiver,child,args){var parent=Object.getPrototypeOf(child);"
        L"return finish(receiver,Reflect.construct(parent,args||[],Object.getPrototypeOf(receiver).constructor));}"
        L"function Logger(storage){var value;return(value=inherit(this,Logger)).patterns=[],value.namespaces={},"
        L"storage&&(value.patterns=value.read(storage)),value.add(),value;}"
        L"Object.setPrototypeOf(Logger,EventBase);"
        L"Logger.prototype=Object.create(EventBase.prototype,{constructor:{value:Logger}});"
        L"Logger.prototype.read=function(){return ['one'];};"
        L"Logger.prototype.check=function(){return this.patterns.length;};"
        L"Logger.prototype.info=function(key){return key in this.namespaces||"
        L"(this.namespaces[key]={value:this.check()}),this.namespaces[key];};"
        L"var logger=new Logger({});return Object.keys(logger).sort().join(',')+'|'"
        L"+logger.patterns.join(',')+'|'+logger.listeners.ready+'|'+logger.info('ready').value;",
        &semanticsResult,&error)&&
        semanticsResult==L"listeners,namespaces,patterns|one|true|1",
        L"transpiled comma-expression constructors retain fields across inherited method calls");
    Check(semanticsJs.Execute(LR"JS(
        function defineMembers(target,members){
          for(var index=0;index<members.length;index++){
            var descriptor=members[index];descriptor.enumerable=descriptor.enumerable||false;
            descriptor.configurable=true;if('value' in descriptor)descriptor.writable=true;
            Object.defineProperty(target,descriptor.key,descriptor);
          }
        }
        function defineClass(constructor,members){
          if(members)defineMembers(constructor.prototype,members);
          Object.defineProperty(constructor,'prototype',{writable:false});return constructor;
        }
        function inherit(child,parent){
          child.prototype=Object.create(parent&&parent.prototype,
            {constructor:{value:child,writable:true,configurable:true}});
          Object.defineProperty(child,'prototype',{writable:false});
          if(parent)Object.setPrototypeOf(child,parent);
        }
        function getPrototype(value){return Object.getPrototypeOf(value);}
        function finish(receiver,result){
          if(result&&('object'==typeof result||'function'==typeof result))return result;
          if(result!==undefined)throw new TypeError();return receiver;
        }
        function constructBase(receiver){
          var parent=getPrototype(DerivedLogger);
          return finish(receiver,Reflect.construct(parent,[],getPrototype(receiver).constructor));
        }
        var EventSource=function(){return defineClass(function(){this.listeners={};},[
          {key:'add',value:function(){this.listeners.ready=true;}}
        ]);}();
        var Storage=function(){return defineClass(function(callback){this.callback=callback;},[
          {key:'read',value:function(){return ['active'];}}
        ]);}();
        var DerivedLogger=function(parent){
          function DerivedLogger(storage){var value;
            return(value=constructBase(this)).patterns=[],value.storage=storage,
              value.patterns=value.storage.read(),value.add(),value;
          }
          inherit(DerivedLogger,parent);
          return defineClass(DerivedLogger,[
            {key:'count',value:function(){return this.patterns.length;}}
          ]);
        }(EventSource);
        var logger=new DerivedLogger(new Storage(function(){}));
        return Object.keys(logger).sort().join(',')+'|'+logger.patterns.join(',')+'|'
          +logger.listeners.ready+'|'+logger.count()+'|'+(logger instanceof DerivedLogger);
    )JS",&semanticsResult,&error)&&
        semanticsResult==L"listeners,patterns,storage|active|true|1|true",error.c_str());
    Check(semanticsResult==L"listeners,patterns,storage|active|true|1|true",
          L"nested construction preserves objects returned by transpiled derived constructors");
    Check(semanticsJs.Execute(LR"JS(
        function defineMethod(target,name,value,hidden){
          function install(name,mode){
            defineMethod(target,name,function(argument){return this._invoke(name,mode,argument);});
          }
          if(name)Object.defineProperty(target,name,
            {value:value,enumerable:!hidden,configurable:!hidden,writable:!hidden});
          else {install('next',0);install('throw',1);install('return',2);}
        }
        var iteratorPrototype={};function Generator(){}function GeneratorFunction(){}
        function GeneratorFunctionPrototype(){}
        var generatorPrototype=GeneratorFunctionPrototype.prototype=Generator.prototype=
          Object.create(iteratorPrototype);
        GeneratorFunction.prototype=GeneratorFunctionPrototype;
        defineMethod(generatorPrototype);
        function mark(generatorFunction){
          Object.setPrototypeOf(generatorFunction,GeneratorFunctionPrototype);
          generatorFunction.prototype=Object.create(generatorPrototype);
          return generatorFunction;
        }
        function wrap(inner,outer){
          var constructor=outer&&outer.prototype instanceof Generator?outer:Generator;
          var generator=Object.create(constructor.prototype);
          defineMethod(generator,'_invoke',function(){return {value:7,done:true};},true);
          return generator;
        }
        function task(){return wrap(function(){},task);}
        mark(task);var generated=task.apply(null,[]);
        var step=generated.next();
        return (Object.getPrototypeOf(generated)!==null)+'|'+typeof generated.next+'|'
          +typeof generated._invoke+'|'+step.value+'|'+step.done;
    )JS",&semanticsResult,&error)&&semanticsResult==L"true|function|function|7|true",error.c_str());
    Check(semanticsResult==L"true|function|function|7|true",
          L"regenerator-style prototypes expose defineProperty-installed iterator methods");
    Check(semanticsJs.Execute(
        L"try{({}).missing();}catch(error){return error.name;}return 'missed';",
        &semanticsResult,&error)&&semanticsResult==L"TypeError",
        L"calling a non-callable value throws TypeError");
    Check(semanticsJs.Execute(
        L"const objectTag=({}).toString;"
        L"return objectTag.call(function(){})+'|'+objectTag.call([])+'|'"
        L"+objectTag.call({})+'|'+('toString' in {});",
        &semanticsResult,&error)&&
        semanticsResult==L"[object Function]|[object Array]|[object Object]|true",
        L"Object prototype tags classify functions and objects through detached call");
    Check(semanticsJs.Execute(
        L"var arrayMethods=[],detachedPush=arrayMethods.push,detachedSlice=arrayMethods.slice,"
        L"detachedIndexOf=arrayMethods.indexOf,detachedConcat=arrayMethods.concat;"
        L"var arrayLike={0:'first',length:1};detachedPush.call(arrayLike,'second');"
        L"var copied=detachedSlice.call(arrayLike,0),flattened=detachedConcat.apply([],[[1],[2]]);"
        L"return (Object(arrayLike)===arrayLike)+'|'+typeof Object+'|'"
        L"+({}).toString.call(Object)+'|'+arrayLike.length+'|'+copied.join(',')+'|'"
        L"+detachedIndexOf.call(arrayLike,'second')+'|'+flattened.join(',')+'|'"
        L"+Array.prototype.slice.call(arrayLike).join(',');",
        &semanticsResult,&error)&&
        semanticsResult==L"true|function|[object Function]|2|first,second|1|1,2|first,second",
        L"Object coercion and detached Array methods preserve generic array-like receivers");
    Check(semanticsJs.Execute(
        L"return [1,2,3].reduce((total,value)=>total+value,4)+'|'"
        L"+[1,2,3].reduceRight((total,value)=>total+value)+'|'"
        L"+[7].reduce((total,value)=>total+value);",
        &semanticsResult,&error)&&semanticsResult==L"10|6|7",
        L"Array reduce methods preserve accumulator and traversal semantics");
    Check(semanticsJs.Execute(
        L"window.classBodyExecutions=0;var singleton=new class {"
        L"value(){return 7;} dormant(){window.classBodyExecutions++;}};"
        L"return singleton.value()+'|'+classBodyExecutions;",
        &semanticsResult,&error)&&semanticsResult==L"7|0",
        L"anonymous class expressions construct instances without executing dormant method bodies");
    Check(semanticsJs.Execute(
        L"var immediate=function(){return 3;}(),"
        L"composed=(function(){return 4;})()+5,"
        L"callable=function(){return 6;}||null;"
        L"return immediate+'|'+composed+'|'+callable();",
        &semanticsResult,&error)&&semanticsResult==L"3|9|6",
        L"variable initializers retain IIFE calls and following expression operators");
    Check(semanticsJs.Execute(
        L"var placeholder=new RegExp('{memo}','g');"
        L"return 'x{memo}{memo}'.replace(placeholder,'ok')+'|'"
        L"+'a{memo}b{image}'.replace(/{[a-zA-Z0-9_]+}/g,'')+'|'"
        L"+'aaa'.replace(/a{2}/g,'x');",
        &semanticsResult,&error)&&semanticsResult==L"xokok|ab|xa",
        L"legacy regular expressions distinguish literal braces from numeric quantifiers");
    Document classifiedReadyDoc;
    Check(classifiedReadyDoc.Parse(L"<div id='ready-output'></div>",&error),
          L"classified ready callback fixture parses");
    JavaScriptRuntime classifiedReadyJs(classifiedReadyDoc);std::wstring classifiedReadyResult;
    classifiedReadyJs.SetDocumentReadyState(L"loading");
    Check(classifiedReadyJs.Execute(
        L"var readyCallbacks=[];var classify=({}).toString;"
        L"function onReady(callback){if(classify.call(callback)==='[object Function]')readyCallbacks.push(callback);}"
        L"document.addEventListener('DOMContentLoaded',function(){readyCallbacks.forEach(function(callback){callback();});});"
        L"onReady(function(){document.getElementById('ready-output').textContent='ready';});",
        nullptr,&error),error.c_str());
    classifiedReadyJs.SetDocumentReadyState(L"interactive");
    classifiedReadyJs.DispatchDocumentEvent(L"DOMContentLoaded");
    Check(classifiedReadyJs.Execute(
        L"return document.getElementById('ready-output').textContent;",
        &classifiedReadyResult,&error)&&classifiedReadyResult==L"ready",
        L"function-tagged ready callbacks run when DOMContentLoaded is dispatched");
    Check(semanticsJs.Execute(
        L"let responsiveChanges=0;let responsiveMatch='';"
        L"const responsiveMedia=matchMedia('(max-width: 900px)');"
        L"responsiveMedia.addEventListener('change',(event)=>{responsiveChanges++;responsiveMatch=String(event.matches);});",
        nullptr,&error),error.c_str());
    semanticsJs.SetViewportSize(800,720);
    Check(semanticsJs.Execute(
        L"return responsiveMedia.matches+'|'+responsiveChanges+'|'+responsiveMatch;",
        &semanticsResult,&error)&&semanticsResult==L"true|1|true",
        L"MediaQueryList updates and emits change when the CSS viewport crosses a boundary");
    semanticsJs.SetViewportSize(800,720);
    Check(semanticsJs.Execute(L"return responsiveChanges;",&semanticsResult,&error)&&semanticsResult==L"1",
          L"MediaQueryList does not emit duplicate events while the result is unchanged");
    semanticsJs.SetViewportSize(1280,720);
    semanticsJs.SetLocation(L"https://app.examples/index.html?mode=dark&lang=ko-KR#settings");
    std::wstring requestedNavigation;
    semanticsJs.SetNavigationSink([&](const std::wstring& target){requestedNavigation=target;});
    Check(semanticsJs.Load(L"",&error)&&semanticsJs.Execute(
        L"const languageUrl=new URL(location.href);"
        L"languageUrl.searchParams.set('lang','ja-JP');"
        L"location.replace(languageUrl.href);"
        L"return languageUrl.href+'|'+languageUrl.searchParams.get('lang')+'|'"
        L"+new URLSearchParams('?label=hello+world').get('label');",
        &semanticsResult,&error)&&
        semanticsResult==L"https://app.examples/index.html?mode=dark&lang=ja-JP#settings|ja-JP|hello world"&&
        requestedNavigation==L"https://app.examples/index.html?mode=dark&lang=ja-JP#settings",
        L"URL search parameters update href and location.replace requests navigation");
    Check(semanticsJs.Load(
        L"let exceptionLog='';"
        L"function throwAcrossFrame(){throw {message:'boom'};}"
        L"function catchAcrossFrame(){try{throwAcrossFrame();exceptionLog+='bad';}catch(error){exceptionLog+=error.message;}finally{exceptionLog+=':finally';}return exceptionLog;}"
        L"function finallyOverridesReturn(){try{return 'try';}finally{return 'finally';}}"
        L"function finallyPreservesReturn(){try{return 'kept';}finally{try{Math.random();}catch(ignored){}}}"
        L"function loopFinally(){let value='';for(let i=0;i<3;i++){try{if(i===1)continue;if(i===2)break;value+=i;}finally{value+='f';}}return value;}"
        L"let thrownIdentity=new TypeError('typed');let sameThrown=false;try{throw thrownIdentity;}catch(error){sameThrown=error===thrownIdentity&&error.name==='TypeError'&&error.message==='typed'&&error.stack==='TypeError: typed';}"
        L"let catchShadow='outer';let capturedCatch;try{throw 'inner';}catch(catchShadow){capturedCatch=()=>catchShadow;}",
        &error),error.c_str());
    Check(semanticsJs.Execute(L"return catchAcrossFrame()+'|'+finallyOverridesReturn()+'|'+finallyPreservesReturn()+'|'+loopFinally()+'|'+sameThrown+'|'+catchShadow+'|'+capturedCatch();",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"boom:finally|finally|kept|0fff|true|outer|inner",
          L"VM exceptions preserve values and catch scope across calls while abrupt completions run finally");
    Check(semanticsJs.Execute(
        L"let compound=3;compound*=4;compound/=2;compound%=5;compound**=3;"
        L"let logicalRuns=0;let truthy='keep';truthy||=(++logicalRuns);let falsy=0;falsy||=7;"
        L"let enabled=true;enabled&&='yes';let stopped=false;stopped&&=(++logicalRuns);"
        L"let doValues=[];let doIndex=0;do{doIndex++;if(doIndex===2)continue;doValues.push(doIndex);}while(doIndex<3);"
        L"let ownKeys=[];for(const key in {alpha:1,beta:2})ownKeys.push(key);let reused='';for(reused in {gamma:3}){}"
        L"return compound+'|'+(+\"8\")+'|'+truthy+'|'+falsy+'|'+enabled+'|'+logicalRuns+'|'+doValues.join(',')+'|'+ownKeys.length+'|'+ownKeys.includes('alpha')+'|'+ownKeys.includes('beta')+'|'+reused;",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"1|8|keep|7|yes|0|1,3|2|true|true|gamma",
          L"basic compound and logical assignments, unary plus, do-while and for-in syntax execute correctly");
    Check(semanticsJs.Execute(
        L"let labeled='';outerBlock:{labeled+='a';break outerBlock;labeled+='bad';}"
        L"outerLoop:for(let row=0;row<3;row++){for(let column=0;column<3;column++){"
        L"if(column===1)continue outerLoop;labeled+=row;}}"
        L"first:second:do{labeled+='d';break first;}while(true);"
        L"return labeled;",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"a012d",
          L"statement labels route labeled break and continue across blocks and nested loops");
    Check(semanticsJs.Execute(
        L"let bits=5;bits&=3;bits|=8;bits^=1;bits<<=2;bits>>=1;bits>>>=2;"
        L"let precedence=1|2&4;let unsigned=-1>>>0;let inverted=~0;"
        L"let present='alpha' in {alpha:1};let second=1 in [4,5];let missing=2 in [4,5];"
        L"let voidSide=0;let voidResult=void (voidSide=7);"
        L"let sequence=0;let sequenceValue=(sequence=1,sequence+=2,sequence*2);"
        L"function gather(head,...rest){return head+':'+rest.join('-')+':'+arguments.length;}"
        L"function trailing(a,b,){return a+b;}let sparse=[1,,3,];"
        L"return bits+'|'+precedence+'|'+unsigned+'|'+inverted+'|'+present+'|'+second+'|'+missing+'|'+typeof voidResult+'|'+voidSide+'|'+sequence+'|'+sequenceValue+'|'+gather(...['a','b','c'],)+'|'+trailing(2,3,)+'|'+sparse.length+'|'+typeof sparse[1];",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"4|1|4294967295|-1|true|true|false|undefined|7|3|6|a:b-c:3|5|3|undefined",
          L"bitwise, shift, in and void operators plus rest, arguments, trailing commas and array holes execute correctly");
    Check(semanticsJs.Execute(
        L"function options({enabled=true,label='fallback'}={}){return enabled+'|'+label;}"
        L"return options()+'|'+options({enabled:false,label:'custom'});",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"true|fallback|false|custom",
          L"destructured parameters support property and whole-pattern defaults");
    Check(semanticsJs.Execute(
        L"return '<p>x</p>'.replace(/^<([a-z]+)/i,'<$1 data-kind=\"block\"');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"<p data-kind=\"block\">x</p>",
          L"regular-expression replacement strings expand capture references");
    Check(semanticsJs.Execute(
        L"return /^[^:/?#]*(?:[/?#]|$)/.test('folder/file')+'|'"
        L"+/^[^:/?#]*(?:[/?#]|$)/.test('scheme:value');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"true|false",
          L"regular-expression literals retain slash characters inside character classes");
    Check(semanticsJs.Execute(
        L"const marker=`\\u0000${7}\\u0000`;"
        L"return '\\u0041\\x42'+'|'+marker.replace(/\\u0000(\\d+)\\u0000/g,(_,index)=>`token-${index}`);",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"AB|token-7",
          L"string, template and regular-expression literals decode JavaScript Unicode escapes");
    const auto regexExceptionHandler=AddVectoredExceptionHandler(1,CountFirstChanceCppExceptions);
    InterlockedExchange(&firstChanceCppExceptions,0);
    Check(semanticsJs.Execute(
        L"const localized=['&File','&\\uD30C\\uC77C','&\\u65E5\\u672C','&\\u0424\\u0430\\u0439\\u043B','&\\u6587\\u4EF6'];"
        L"return localized.map((label)=>label.replace(/&(?=\\p{L})/u,'')).join('|');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"File|\uD30C\uC77C|\u65E5\u672C|\u0424\u0430\u0439\u043B|\u6587\u4EF6",
          L"Unicode Letter property escapes match multilingual menu labels");
    Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
          L"supported Unicode property escapes compile without first-chance C++ exceptions");
    if(regexExceptionHandler)RemoveVectoredExceptionHandler(regexExceptionHandler);
    Check(semanticsJs.Execute(
        L"const fence=new RegExp('^ {0,3}```');const insensitive=RegExp('alpha','i');"
        L"const clone=new RegExp(insensitive);"
        L"return fence.test('```powershell')+'|'+fence.test('text')+'|'+clone.test('ALPHA');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"true|false|true",
          L"RegExp constructor creates dynamic expressions and clones existing expressions");
    Check(semanticsJs.Execute(
        L"let promiseOrder=[];"
        L"Promise.resolve(1).then((value)=>{promiseOrder.push('then'+value);return value+1;}).then((value)=>promiseOrder.push('chain'+value));"
        L"Promise.resolve({then:(resolve)=>resolve(7)}).then((value)=>promiseOrder.push('thenable'+value));"
        L"new Promise((resolve)=>resolve(3)).then((value)=>promiseOrder.push('constructed'+value));"
        L"Promise.reject(new Error('rejected')).catch((error)=>promiseOrder.push(error.message));"
        L"queueMicrotask(()=>promiseOrder.push('queued'));promiseOrder.push('sync');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return promiseOrder.join(',');",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"sync,then1,thenable7,constructed3,rejected,queued,chain2",
          L"Promise settlement, thenable assimilation and recursive microtask checkpoints preserve order");
    Check(semanticsJs.Execute(
        L"let promiseFinally=[];Promise.resolve('ok').finally(()=>promiseFinally.push('fulfilled-finally')).then((value)=>promiseFinally.push(value));Promise.reject('bad').finally(()=>promiseFinally.push('rejected-finally')).catch((reason)=>promiseFinally.push(reason));",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return promiseFinally.join(',');",&semanticsResult,&error)&&semanticsResult==L"fulfilled-finally,rejected-finally,ok,bad",
          L"Promise finally waits for its callback and preserves fulfillment values and rejection reasons");
    Check(semanticsJs.Execute(
        L"let aggregateOrder=[];"
        L"Promise.all([Promise.resolve(1),2]).then((values)=>aggregateOrder.push('all'+values.join('-')));"
        L"Promise.allSettled([Promise.resolve('yes'),Promise.reject('no')]).then((values)=>aggregateOrder.push(values[0].status+'-'+values[1].reason));"
        L"Promise.any([Promise.reject('first'),Promise.resolve('winner')]).then((value)=>aggregateOrder.push('any-'+value));",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return aggregateOrder.join(',');",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"all1-2,fulfilled-no,any-winner",
          L"Promise aggregate combinators retain input order and settlement semantics");
    Check(semanticsJs.Execute(
        L"let resolverResult=[];let deferred=Promise.withResolvers.call(Promise);"
        L"deferred.promise.then((value)=>resolverResult.push(value));deferred.resolve(43);",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(
        L"return resolverResult.join(',')+'|'+typeof deferred.resolve+'|'+typeof deferred.reject;",
        &semanticsResult,&error)&&semanticsResult==L"43|function|function",
          L"Promise.withResolvers exposes a callable deferred promise capability");
    Check(semanticsJs.Execute(
        L"let rejectionEvents=[];window.addEventListener('unhandledrejection',(event)=>rejectionEvents.push('unhandled-'+event.reason));window.addEventListener('rejectionhandled',()=>rejectionEvents.push('handled'));let lateRejection=Promise.reject('late');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"lateRejection.catch(()=>rejectionEvents.push('caught'));",nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return rejectionEvents.join(',');",&semanticsResult,&error)&&semanticsResult==L"unhandled-late,handled,caught",
          L"unhandled and subsequently handled promise rejections emit lifecycle events");
    Check(semanticsJs.Execute(
        L"let asyncOrder=[];"
        L"async function asyncTask(){asyncOrder.push('start');const value=await Promise.resolve(4);asyncOrder.push('after'+value);return value+1;}"
        L"async function asyncRecover(){try{await Promise.reject('no');return 'bad';}catch(error){return 'caught-'+error;}finally{asyncOrder.push('recover-finally');}}"
        L"function defaultFailure(){throw 'default';}async function asyncDefault(value=defaultFailure()){return value;}"
        L"const asyncArrow=async(value)=>await Promise.resolve(value+1);const asyncService={async load(value){return await asyncArrow(value);}};class AsyncWorker{async run(value){return await asyncService.load(value);}}"
        L"asyncTask().then((value)=>asyncOrder.push('done'+value));asyncRecover().then((value)=>asyncOrder.push(value));new AsyncWorker().run(8).then((value)=>asyncOrder.push('method'+value));asyncDefault().catch((reason)=>asyncOrder.push(reason));asyncOrder.push('sync');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return asyncOrder.join(',');",&semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"start,sync,after4,recover-finally,default,done5,caught-no,method9",
          L"async functions, arrows and methods suspend at await, resume from microtasks and route rejection through catch/finally");
    semanticsJs.SetResourceLoader([](const std::wstring& resource,std::wstring& body){if(resource!=L"fixture.json")return false;body=L"{\"value\":9}";return true;});
    Check(semanticsJs.Execute(
        L"let fetchValue='';async function loadFixture(){const response=await fetch('fixture.json');const data=await response.json();return response.ok+':'+data.value;}loadFixture().then((value)=>fetchValue=value);",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return fetchValue;",&semanticsResult,&error)&&semanticsResult==L"true:9",
          L"fetch and response.json compose as promises across consecutive await suspension points");
    semanticsJs.SetResourceLoader([](const std::wstring& resource,std::wstring& body){
        if(resource!=L"stream.txt")return false;body=L"first \uD55C\uAE00\nsecond";return true;
    });
    Check(semanticsJs.Execute(
        L"let streamValue='';fetch('stream.txt').then((response)=>{const reader=response.body.getReader();const decoder=new TextDecoder();return reader.read().then((first)=>reader.read().then((second)=>streamValue=decoder.decode(first.value)+'|'+first.done+'|'+second.done));});",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return streamValue;",&semanticsResult,&error)&&
          semanticsResult==L"first \uD55C\uAE00\nsecond|false|true",
          L"fetch response bodies expose UTF-8 readable streams with terminating reader results");
    Check(semanticsJs.Execute(
        L"let taskOrder=[];setTimeout(()=>taskOrder.push('timer'),0);Promise.resolve().then(()=>taskOrder.push('microtask'));taskOrder.push('script');",
        nullptr,&error),error.c_str());
    Check(semanticsJs.Execute(L"return taskOrder.join(',');",&semanticsResult,&error)&&semanticsResult==L"script,microtask",
          L"microtasks drain after the script task before timers");
    semanticsJs.RunTimers();
    Check(semanticsJs.Execute(L"return taskOrder.join(',');",&semanticsResult,&error)&&semanticsResult==L"script,microtask,timer",
          L"timer callbacks run as later tasks after the microtask checkpoint");
    Check(semanticsJs.Execute(
        L"taskOrder=[];setTimeout(()=>taskOrder.push('first'),0);"
        L"setTimeout(()=>taskOrder.push('second'),0);",
        nullptr,&error),error.c_str());
    semanticsJs.RunTimers();
    Check(semanticsJs.Execute(L"return taskOrder.join(',');",&semanticsResult,&error)&&
          semanticsResult==L"first",
          L"a timer wake processes one event-loop task so the host can service UI work");
    semanticsJs.RunTimers();
    Check(semanticsJs.Execute(L"return taskOrder.join(',');",&semanticsResult,&error)&&
          semanticsResult==L"first,second",
          L"the next timer wake processes the following queued task");
    Document eventDoc;Check(eventDoc.Parse(L"<button id='event-button'>go</button><button id='compat-button'>compat</button>",&error),L"event fixture parses");
    JavaScriptRuntime eventJs(eventDoc);std::wstring eventResult;bool focusRequested=false;
    eventJs.SetFocusSink([&](const std::shared_ptr<Node>& node){focusRequested=node==eventDoc.GetElementById(L"event-button");node->focused=true;});
    Check(eventJs.Load(L"let eventLog='',compatLog='';const b=document.getElementById('event-button');b.addEventListener('pointerdown',(e)=>{eventLog+=e.button+':'+e.detail;e.preventDefault();e.stopPropagation();});document.addEventListener('pointerdown',()=>eventLog+=':bubble');document.addEventListener('keydown',(e)=>eventLog+='|'+e.key+':'+e.ctrlKey+':'+e.shiftKey);const c=document.getElementById('compat-button');c.addEventListener('pointerdown',(e)=>compatLog+='pointer:'+e.button+':'+e.detail);c.addEventListener('mousedown',(e)=>compatLog+='|mouse:'+e.button+':'+e.detail);b.focus();",&error),error.c_str());
    JavaScriptRuntime::EventInit pointerInit;pointerInit.button=0;pointerInit.detail=2;
    Check(eventJs.DispatchNodeEvent(eventDoc.GetElementById(L"event-button"),L"pointerdown",pointerInit),
          L"preventDefault is reported to the native event source");
    JavaScriptRuntime::EventInit keyInit;keyInit.key=L"b";keyInit.ctrlKey=true;keyInit.shiftKey=true;
    eventJs.DispatchNodeEvent(eventDoc.GetElementById(L"event-button"),L"keydown",keyInit);
    Check(eventJs.Execute(L"return eventLog;",&eventResult,&error),error.c_str());
    Check(eventResult==L"0:2|b:true:true",L"pointer and keyboard metadata bubble with propagation controls");
    bool nativePointerCaptured=false;eventJs.SetPointerCaptureSink([&](bool captured){nativePointerCaptured=captured;});
    Check(eventJs.Execute(L"b.addEventListener('pointerdown',event=>b.setPointerCapture(event.pointerId),{once:true});",nullptr,&error),error.c_str());
    eventJs.DispatchNodeEvent(eventDoc.GetElementById(L"event-button"),L"pointerdown",pointerInit);
    Check(nativePointerCaptured&&eventJs.CapturedPointerTarget()==eventDoc.GetElementById(L"event-button")&&
          eventJs.Execute(L"return b.hasPointerCapture(1);",&eventResult,&error)&&eventResult==L"true",
          L"pointer capture routes through the common DOM and native capture bridge");
    Check(eventJs.Execute(L"b.releasePointerCapture(1);return b.hasPointerCapture(1);",&eventResult,&error)&&
          eventResult==L"false"&&!nativePointerCaptured&&!eventJs.CapturedPointerTarget(),
          L"releasing pointer capture clears both DOM and native capture state");
    Check(!eventJs.DispatchNodeEvent(eventDoc.GetElementById(L"compat-button"),L"pointerdown",pointerInit),
          L"an uncanceled pointer press keeps its native default action");
    Check(eventJs.Execute(L"return compatLog;",&eventResult,&error)&&eventResult==L"pointer:0:2|mouse:0:2",
          L"pointerdown produces the standard compatibility mousedown event");
    Check(focusRequested,L"programmatic focus is synchronized with the native view");
    Check(eventJs.Execute(L"let timerLog='';setTimeout(()=>timerLog='ran',0);",nullptr,&error),error.c_str());
    Check(eventJs.Execute(L"return timerLog;",&eventResult,&error)&&eventResult.empty(),
          L"setTimeout remains asynchronous even with a zero delay");
    eventJs.RunTimers();
    Check(eventJs.Execute(L"return timerLog;",&eventResult,&error)&&eventResult==L"ran",
          L"due timeout callbacks run through the generic timer queue");
    Check(semanticsJs.Execute(
        L"const sortable=[{name:'beta'},{name:'alpha'}];sortable.sort((left,right)=>left.name.localeCompare(right.name));"
        L"const source={nested:{value:1}};const copied=structuredClone(source);copied.nested.value=9;"
        L"const parsedDate=new Date('2024-01-02T03:04:05.006Z');const relative=new Intl.RelativeTimeFormat('en-US',{numeric:'always'}).format(-2,'hour');"
        L"return sortable.at(0).name+'|'+sortable.some((item)=>item.name==='beta')+'|'+[1,2].findIndex((item)=>item===2)+'|'+source.nested.value+'|'+parsedDate.toISOString()+'|'+Number.isNaN(new Date('invalid').getTime())+'|'+Number.isInteger(5)+'|'+Number.isInteger(5.5)+'|'+Number.isInteger('5')+'|'+relative;",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"alpha|true|1|1|2024-01-02T03:04:05.006Z|true|true|false|false|2 hours ago",
          L"generic array, string, structured clone, Date and Intl APIs compose correctly");
    Check(semanticsJs.Execute(
        L"const splitLines='one\\r\\ntwo\\nthree'.split(/\\r?\\n/);"
        L"const splitOptional='alpha--beta-gamma'.split(/--?/);"
        L"const splitLimited='a--b-c'.split(/--?/,2);"
        L"const hunk='@@ -12,2 +34,5 @@ heading'.match(/^@@ -(\\d+)(?:,\\d+)? \\+(\\d+)(?:,\\d+)? @@/);"
        L"return splitLines.slice(0,2).join('|')+'|'+splitLines.slice(-1).join('|')+'|'+splitOptional.join('|')+'|'+splitLimited.join('|')+'|'+hunk[1]+'|'+hunk[2]+'|'+hunk.index+'|'+'abc'.split().join(',')+'|'+'abc'.match(/z/);",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"one|two|three|alpha|beta|gamma|a|b|12|34|0|abc|null",
          L"Array slice and String split and match apply reusable collection and regular-expression semantics");
    Check(semanticsJs.Execute(
        L"const matches=[...String('**bold** and `code`').matchAll(/(\\*\\*[^*]+\\*\\*)|(`[^`]+`)/g)];"
        L"const frozen=Object.freeze({alpha:1});const filled=Array(3).fill('x').join('');"
        L"return Object.hasOwn(frozen,'alpha')+'|'+filled+'|'+[1,3,2,3].findLastIndex(value=>value===3)+'|'+'  value'.trimStart()+'|'+'suffix'.endsWith('fix')+'|'+'ab'.repeat(2)+'|'+'\\nalpha'.lastIndexOf('\\n',-1)+'|'+matches.length+'|'+matches[1].index+'|'+encodeURIComponent(decodeURIComponent('a%20b%2Fc'))+'|'+parseFloat('12.5px');",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"true|xxx|3|value|true|abab|0|2|13|a%20b%2Fc|12.5",
          L"MdViewer modern Object, Array and String helpers preserve browser semantics");
    Check(semanticsJs.Execute(
        L"const documentHistory=[{text:'stale'}];documentHistory.length=0;"
        L"const appendedLength=documentHistory.push({text:'current'});"
        L"documentHistory.length=3;const expandedSlot=documentHistory[2]===undefined;"
        L"documentHistory.length=1;let invalidLength=false;"
        L"try{documentHistory.length=-1;}catch(error){invalidLength=error.name==='RangeError';}"
        L"return appendedLength+'|'+documentHistory.length+'|'+documentHistory[0].text+'|'+expandedSlot+'|'+invalidLength;",
        &semanticsResult,&error),error.c_str());
    Check(semanticsResult==L"1|1|current|true|true",
          L"Array length assignment clears, expands and truncates shared JavaScript history stacks");
    Check(eventJs.Execute(L"let intervalRuns=0;const intervalId=setInterval(()=>{intervalRuns++;if(intervalRuns===2)clearInterval(intervalId);},0);",nullptr,&error),error.c_str());
    eventJs.RunTimers();eventJs.RunTimers();eventJs.RunTimers();
    Check(eventJs.Execute(L"return intervalRuns;",&eventResult,&error)&&eventResult==L"2",
          L"setInterval repeats until clearInterval removes the generic timer");
    Document modernDomDoc;Check(modernDomDoc.Parse(L"<div id='scope'><span class='direct'></span><div><span class='direct'></span></div></div><div id='choices'><label><input type='radio' checked></label><label><input type='radio'></label></div><dialog id='dialog'></dialog><form id='form'></form>",&error),L"modern DOM fixture parses");
    Check(modernDomDoc.QuerySelectorAll(L"#choices label:has(input:checked)").size()==1&&modernDomDoc.QuerySelectorAll(L"#choices label:nth-of-type(2)").size()==1,
          L"relational and type-index CSS selectors match reusable document state");
    JavaScriptRuntime modernDomJs(modernDomDoc);std::wstring modernDomResult;
    Check(modernDomJs.Load(
        L"const scope=document.getElementById('scope');const dialog=document.getElementById('dialog');const form=document.getElementById('form');let submitted=false,closed=false;form.addEventListener('submit',(event)=>{submitted=true;event.preventDefault();});dialog.addEventListener('close',()=>closed=true);dialog.showModal();const opened=dialog.open;dialog.close('accepted');form.requestSubmit();const direct=scope.querySelectorAll(':scope > .direct').length;",
        &error),error.c_str());
    Check(modernDomJs.Execute(L"return opened+'|'+dialog.open+'|'+dialog.returnValue+'|'+submitted+'|'+closed+'|'+direct;",&modernDomResult,&error),error.c_str());
    Check(modernDomResult==L"true|false|accepted|true|true|1",
          L"dialog, form submission and scoped selectors use reusable DOM behavior");
    Document compatibilityDomDoc;
    Check(compatibilityDomDoc.Parse(
        L"<body><form name='search'><input name='query' class='field primary'><span class='field'></span></form></body>",
        &error),L"browser compatibility DOM fixture parses");
    JavaScriptRuntime compatibilityDomJs(compatibilityDomDoc);std::wstring compatibilityDomResult;
    bool compatibilityDocumentFocused=false;
    compatibilityDomJs.SetDocumentFocusProvider([&]{return compatibilityDocumentFocused;});
    Check(compatibilityDomJs.Execute(
        L"const form=document.getElementsByTagName('form')[0];const input=form.query;"
        L"const marker=document.createEvent('Event');marker.initEvent('ready',true,true);"
        L"const attribute=input.getAttributeNode('name');"
        L"return [window.document===document,document.search===form,"
        L"document.getElementsByTagName('input').length,document.getElementsByClassName('field').length,"
        L"form.getElementsByTagName('*').length,form.getElementsByClassName('field').length,"
        L"input.ownerDocument===document,input.getAttribute('missing')===null,"
        L"attribute.name,attribute.value,attribute.nodeValue,attribute.nodeType,attribute.ownerElement===input,"
        L"attribute.specified,marker.type,marker.bubbles,marker.cancelable,"
        L"form.children[1].previousElementSibling===input].join('|');",
        &compatibilityDomResult,&error)&&
        compatibilityDomResult==L"true|true|1|2|2|2|true|true|name|query|query|2|true|true|ready|true|true|true",
        L"legacy document collections, named form access and event initialization compose through the common DOM bridge");
    Check(compatibilityDomJs.Execute(L"return document.hasFocus();",&compatibilityDomResult,&error)&&
          compatibilityDomResult==L"false",L"document.hasFocus reflects an unfocused host document");
    compatibilityDocumentFocused=true;
    Check(compatibilityDomJs.Execute(L"return document.hasFocus();",&compatibilityDomResult,&error)&&
          compatibilityDomResult==L"true",L"document.hasFocus reflects a focused host document");
    Check(compatibilityDomJs.Execute(LR"JS(
        var attributeMap=input.attributes,initialAttributeCount=attributeMap.length;
        var attributeIterator=attributeMap[Symbol.iterator](),attributeNames=[],attributeStep;
        while(!(attributeStep=attributeIterator.next()).done)attributeNames.push(attributeStep.value.name);
        var namedAttribute=attributeMap.getNamedItem('NAME');namedAttribute.value='term';
        input.setAttribute('data-extra','ok');var countAfterAdd=attributeMap.length;
        var extraValue=attributeMap['data-extra'].value;
        var removedAttribute=attributeMap.removeNamedItem('data-extra'),countAfterRemove=attributeMap.length;
        var createdAttribute=document.createAttribute('DATA-CREATED');createdAttribute.value='yes';
        var replacedAttribute=attributeMap.setNamedItem(createdAttribute);
        return [Object.prototype.toString.call(attributeMap),Array.isArray(attributeMap),
          initialAttributeCount,attributeMap.item(0).nodeType,attributeNames.sort().join(','),
          input.getAttribute('name'),countAfterAdd,extraValue,countAfterRemove,
          removedAttribute.ownerElement===null,removedAttribute.value,replacedAttribute===null,
          input.getAttribute('data-created'),createdAttribute.ownerElement===input].join('|');
    )JS",&compatibilityDomResult,&error)&&
        compatibilityDomResult==L"[object NamedNodeMap]|false|2|2|class,name|term|3|ok|2|true|ok|true|yes|true",
        L"element attributes expose a live iterable NamedNodeMap with mutable Attr nodes");
    Document appDomDoc;Check(appDomDoc.Parse(
        L"<div id='root'><span id='replace' class='alpha beta'>ab</span><em>cd</em></div>"
        L"<input id='source' value='abcdef'><button id='activate'>go</button>"
        L"<table id='table'><thead><tr><th>H</th></tr></thead><tbody><tr><td>A</td></tr></tbody></table>",
        &error),L"MdViewer DOM compatibility fixture parses");
    JavaScriptRuntime appDomJs(appDomDoc);std::wstring appDomResult;
    Check(appDomJs.Execute(
        L"const root=document.getElementById('root');const replaced=document.getElementById('replace');"
        L"replaced.classList.remove('alpha');const classCount=replaced.classList.length;"
        L"const fragment=document.createDocumentFragment();fragment.append(document.createTextNode('X'),document.createTextNode('Y'));replaced.replaceWith(fragment);root.normalize();"
        L"const walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);let walked='',text;while(text=walker.nextNode())walked+=text.nodeValue+':'+text.length+';';"
        L"const contents=document.createRange();contents.selectNodeContents(root);const selected=contents.toString();"
        L"const edit=document.createRange();edit.setStart(root.firstChild,1);edit.setEnd(root.firstChild,2);edit.deleteContents();const inserted=document.createTextNode('Z');edit.insertNode(inserted);edit.setStartAfter(inserted);edit.collapse(true);"
        L"const source=document.getElementById('source');source.setRangeText('ZZ',1,3,'select');source.setSelectionRange(1,3,'backward');const editedValue=source.value,selectionState=source.selectionStart+'-'+source.selectionEnd+'-'+source.selectionDirection;source.contentEditable='false';source.required=true;const missingBefore=source.reportValidity();source.value='';const missingAfter=source.reportValidity();source.setCustomValidity('bad');const customError=source.reportValidity();source.setCustomValidity('');"
        L"let clicks=0;const activate=document.getElementById('activate');activate.addEventListener('click',()=>clicks++);activate.click();"
        L"const table=document.getElementById('table').cloneNode(true);const body=table.tBodies[0];const row=body.insertRow(0);row.insertCell().append(document.createElement('br'));body.deleteRow(1);row.insertCell().textContent='B';row.deleteCell(0);"
        L"return classCount+'|'+fragment.nodeType+'|'+walked+'|'+selected+'|'+root.textContent+'|'+editedValue+'|'+selectionState+'|'+source.contentEditable+'|'+missingBefore+'-'+missingAfter+'-'+customError+'|'+clicks+'|'+table.rows.length+'-'+body.rows.length+'-'+row.cells.length;",
        &appDomResult,&error),error.c_str());
    Check(appDomResult==L"1|11|XY:2;cd:2;|XYcd|XZcd|aZZdef|1-3-backward|false|true-false-false|1|2-1-1",
          L"tree walking, fragments, ranges, form controls, activation and table DOM APIs compose for MdViewer editing");
    Check(appDomJs.Execute(
        L"const contextualRange=document.createRange(),rootNode=document.getElementById('root');"
        L"contextualRange.selectNode(rootNode);const contextual=contextualRange.createContextualFragment('<strong id=\"range-fragment\">range</strong>');"
        L"rootNode.appendChild(contextual);return contextualRange.commonAncestorContainer.tagName+'|'+contextual.nodeType+'|'+document.getElementById('range-fragment').textContent"
        L"+'|'+contextual.childNodes.length+'|'+rootNode.lastChild.tagName;",
        &appDomResult,&error)&&appDomResult==L"BODY|11|range|0|STRONG",
        L"Range.selectNode and createContextualFragment insert parsed markup through generic DOM rules");
    Check(appDomJs.Execute(LR"JS(
        var container=document.createElement('div'),marker=document.createElement('span');
        container.appendChild(marker);document.body.appendChild(container);
        var transfer=document.createDocumentFragment(),first=document.createElement('b'),second=document.createElement('i');
        first.id='fragment-first';second.id='fragment-second';first.textContent='A';second.textContent='B';
        transfer.appendChild(first);transfer.appendChild(second);
        var returned=container.insertBefore(transfer,marker);
        var beforeValid=returned===transfer&&transfer.childNodes.length===0&&transfer.parentNode===null&&
            container.childNodes.length===3&&container.firstChild===first&&first.nextSibling===second&&
            second.nextSibling===marker&&document.getElementById('fragment-second')===second;
        container.insertBefore(first,first);container.appendChild(first);
        var moveValid=container.firstChild===second&&container.lastChild===first;
        var empty=document.createDocumentFragment();container.appendChild(empty);
        var emptyValid=container.childNodes.length===3&&empty.parentNode===null;
        var missing=false,cycle=false;
        try{container.insertBefore(empty,document.createElement('div'));}catch(e){missing=e.name==='NotFoundError';}
        try{first.appendChild(container);}catch(e){cycle=e.name==='HierarchyRequestError';}
        return beforeValid&&moveValid&&emptyValid&&missing&&cycle;
    )JS",&appDomResult,&error)&&appDomResult==L"true",
        L"fragment insertion transfers ordered children, preserves DOM indexes and handles self moves and invalid references");
    Check(appDomJs.Execute(
        L"const scriptRange=document.createRange(),rootNode=document.getElementById('root');"
        L"scriptRange.selectNode(rootNode);const contextual=scriptRange.createContextualFragment("
        L"'<section id=\"range-script-parent\"><script>document.currentScript.parentElement.setAttribute(\"data-ran\",\"yes\")</script></section>');"
        L"rootNode.replaceChildren(contextual);const parent=document.getElementById('range-script-parent');"
        L"return parent.getAttribute('data-ran')+'|'+parent.children.length;",
        &appDomResult,&error)&&appDomResult==L"yes|1",
        L"scripts nested in a contextual fragment execute when replaceChildren connects the fragment");
    Document dialogDisplayDoc;Check(dialogDisplayDoc.Parse(L"<body><dialog id='closed' class='modal'></dialog><dialog id='opened' class='modal' open></dialog><div id='hidden' hidden></div></body>",&error),L"dialog display fixture parses");
    StyleSheet dialogDisplayCss;Check(dialogDisplayCss.Parse(L".modal{width:300px}",&error),L"dialog display CSS parses");
    LayoutEngine dialogDisplayLayout(dialogDisplayDoc,dialogDisplayCss);dialogDisplayLayout.Layout(800,600);
    const auto* closedDialog=FindLayout(dialogDisplayLayout.Root(),L"closed");const auto* openedDialog=FindLayout(dialogDisplayLayout.Root(),L"opened");const auto* hiddenElement=FindLayout(dialogDisplayLayout.Root(),L"hidden");
    Check(closedDialog&&!closedDialog->visible&&openedDialog&&openedDialog->visible&&openedDialog->style.Is(L"position",L"fixed")&&hiddenElement&&!hiddenElement->visible,
          L"closed dialogs and hidden elements skip layout while open dialogs enter the fixed top layer");
    JavaScriptRuntime dialogRuntime(dialogDisplayDoc);std::wstring dialogResult;
    Check(dialogRuntime.Execute(
          L"const dialog=document.getElementById('closed');dialog.showModal();return dialog.open;",
          &dialogResult,&error)&&dialogResult==L"true"&&
          dialogDisplayDoc.GetElementById(L"closed")->modal,
          L"showModal records modal top-layer state separately from the open attribute");
    Check(dialogRuntime.Execute(
          L"const dialog=document.getElementById('closed');dialog.close('done');dialog.show();return dialog.open+'|'+dialog.returnValue;",
          &dialogResult,&error)&&dialogResult==L"true|done"&&
          !dialogDisplayDoc.GetElementById(L"closed")->modal,
          L"close removes modal state while non-modal show does not create a backdrop");
    Check(eventJs.Execute(L"const timer= setTimeout(()=>timerLog='wrong',0);clearTimeout(timer);",nullptr,&error),error.c_str());
    eventJs.RunTimers();
    Check(eventJs.Execute(L"return timerLog;",&eventResult,&error)&&eventResult==L"ran",
          L"clearTimeout removes a queued timeout callback");
    Document svgDoc;Check(svgDoc.Parse(L"<style>svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:2}</style><svg style='display:none'><symbol id='icon' viewBox='0 0 24 24'><path d='M2 2L22 22'/></symbol></svg><svg id='instance'><use href='#icon'></use></svg>",&error),L"SVG use fixture parses");
    StyleSheet svgCss;Check(svgCss.Parse(svgDoc.StyleText(),&error),L"SVG use CSS parses");
    LayoutEngine svgLayout(svgDoc,svgCss);svgLayout.Layout(100,100);
    const auto* svgInstance=svgLayout.BoxFor(svgDoc.GetElementById(L"instance"));
    Check(svgInstance&&std::lround(svgInstance->rect.width)==18&&std::lround(svgInstance->rect.height)==18,
          L"SVG symbol use instances participate in generic layout");
    Document svgVariableDoc;
    Check(svgVariableDoc.Parse(
          L"<style>:root{--surface:#10151d;--edge:#ff654a}</style><svg id='variable-svg'><circle id='variable-circle' fill='var(--surface)' stroke='var(--edge)'/></svg>",
          &error),L"SVG presentation variable fixture parses");
    StyleSheet svgVariableCss;Check(svgVariableCss.Parse(svgVariableDoc.StyleText(),&error),
          L"SVG presentation variable CSS parses");
    const auto svgVariableStyle=svgVariableCss.Compute(svgVariableDoc.GetElementById(L"variable-svg"));
    const auto circleVariableStyle=svgVariableCss.Compute(
          svgVariableDoc.GetElementById(L"variable-circle"),&svgVariableStyle);
    Check(circleVariableStyle.Get(L"fill")==L"#10151d"&&
          circleVariableStyle.Get(L"stroke")==L"#ff654a",
          L"SVG presentation attributes resolve inherited custom properties");
    Document cascadeDoc;
    Check(cascadeDoc.Parse(L"<style>.x{background-color:#111;margin-left:1px;flex-grow:7;gap:3px 4px}.x{background:#fff;margin:2px;flex:1}</style><div class='x'></div>", &error),
          L"shorthand cascade fixture parses");
    StyleSheet cascadeCss; Check(cascadeCss.Parse(cascadeDoc.StyleText(), &error), L"shorthand cascade CSS parses");
    const auto cascadeStyle=cascadeCss.Compute(cascadeDoc.QuerySelector(L".x"));
    Check(cascadeStyle.Get(L"background-color")==L"#fff"&&cascadeStyle.Get(L"margin-left")==L"2px"&&
          cascadeStyle.Get(L"flex-grow")==L"1"&&cascadeStyle.Get(L"row-gap")==L"3px"&&
          cascadeStyle.Get(L"column-gap")==L"4px",L"shorthands reset canonical longhands in cascade order");
    Check(StyleSheet::Color(L"color-mix(in srgb, #d9dee8 68%, transparent)")==0xadd9dee8u,
          L"sRGB color-mix preserves opaque channels while mixing alpha with transparent");
    Document borderMixDoc;
    Check(borderMixDoc.Parse(
          L"<style>:root{--edge:#252e3b}.row{border-bottom:1px solid color-mix(in srgb,var(--edge) 65%,transparent)}</style><div id='mixed-border' class='row'></div>",
          &error),L"functional border color shorthand fixture parses");
    StyleSheet borderMixCss;Check(borderMixCss.Parse(borderMixDoc.StyleText(),&error),
          L"functional border color shorthand CSS parses");
    const auto borderMixStyle=borderMixCss.Compute(borderMixDoc.GetElementById(L"mixed-border"));
    Check(borderMixStyle.Get(L"border-bottom-color")==
              L"color-mix(in srgb,#252e3b 65%,transparent)"&&
          StyleSheet::Color(borderMixStyle.Get(L"border-bottom-color"))==0xa6252e3bu,
          L"border shorthand keeps the complete color function and its alpha");
    Document borderScaleDoc;
    Check(borderScaleDoc.Parse(
          L"<style>body{margin:0}.probe{box-sizing:border-box;width:100px;height:20px;border:1px solid #fff}</style><div id='border-scale' class='probe'></div>",
          &error),L"device-scale border fixture parses");
    StyleSheet borderScaleCss;Check(borderScaleCss.Parse(borderScaleDoc.StyleText(),&error),
          L"device-scale border CSS parses");
    LayoutEngine borderScaleLayout(borderScaleDoc,borderScaleCss);borderScaleLayout.Layout(200,80,1.0f);
    const auto* borderScaleBox=borderScaleLayout.BoxFor(borderScaleDoc.GetElementById(L"border-scale"));
    Check(borderScaleBox&&std::abs((borderScaleBox->content.x-borderScaleBox->rect.x)-1.0f)<0.001f&&
          std::abs(borderScaleBox->rect.width-100.0f)<0.001f,
          L"one CSS-pixel border remains one physical pixel at 100% scale");
    borderScaleLayout.Relayout(200,80,1.5f);
    borderScaleBox=borderScaleLayout.BoxFor(borderScaleDoc.GetElementById(L"border-scale"));
    Check(borderScaleBox&&std::abs((borderScaleBox->content.x-borderScaleBox->rect.x)-(2.0f/3.0f))<0.001f&&
          std::abs(borderScaleBox->rect.width-100.0f)<0.001f,
          L"one CSS-pixel border snaps to one physical pixel at 150% scale");

    Document viewportAbsoluteDoc;
    Check(viewportAbsoluteDoc.Parse(
          L"<style>iframe{position:absolute;top:0;left:0;width:100%;height:100%;border:0}</style>"
          L"<iframe id='viewport-absolute'></iframe>",&error),
          L"initial containing block fixture parses");
    StyleSheet viewportAbsoluteCss;
    Check(viewportAbsoluteCss.Parse(viewportAbsoluteDoc.StyleText(),&error),
          L"initial containing block CSS parses");
    LayoutEngine viewportAbsoluteLayout(viewportAbsoluteDoc,viewportAbsoluteCss);
    bool viewportAbsoluteValid=true;
    for(const float scale:{1.0f,1.5f}){
        viewportAbsoluteLayout.Relayout(728,90,scale);
        const auto* absolute=viewportAbsoluteLayout.BoxFor(
            viewportAbsoluteDoc.GetElementById(L"viewport-absolute"));
        viewportAbsoluteValid=viewportAbsoluteValid&&absolute&&
            std::abs(absolute->rect.x)<0.01f&&std::abs(absolute->rect.y)<0.01f&&
            std::abs(absolute->rect.width-728.0f)<0.01f&&
            std::abs(absolute->rect.height-90.0f)<0.01f&&
            std::abs(absolute->style.deviceScale-scale)<0.01f;
    }
    Check(viewportAbsoluteValid,
          L"an absolute percentage-sized child without a positioned ancestor uses the initial containing block at 100 and 150 percent DPI");

    Document basicCssDoc;
    Check(basicCssDoc.Parse(
        L"<style>body{margin:0}"
        L"[data-code^='prefix' i]{width:41px}[data-code$='value']{height:22px}"
        L"[data-code*='fix-v']{margin-left:3px}[data-tags~='hot']{padding-left:4px}"
        L"[lang|='en']{padding-right:5px}#selector-anchor ~ [data-code]{background:center/cover no-repeat teal}"
        L"#styled{width:10px;height:10px;font:italic 700 12px/1.5 'Segoe UI';border:4px solid rebeccapurple;border-style:none;text-decoration:underline line-through}"
        L"#solid{width:10px;height:10px;border:solid rebeccapurple 4px}</style>"
        L"<body><div id='selector-anchor'></div><div id='selector-target' data-code='Prefix-value' data-tags='new hot' lang='en-US'></div>"
        L"<div id='styled'>decorated</div><div id='solid'></div></body>",&error),
        L"basic CSS syntax fixture parses");
    StyleSheet basicCss;Check(basicCss.Parse(basicCssDoc.StyleText(),&error),L"basic CSS stylesheet parses");
    const auto selectorStyle=basicCss.Compute(basicCssDoc.GetElementById(L"selector-target"));
    Check(selectorStyle.Get(L"width")==L"41px"&&selectorStyle.Get(L"height")==L"22px"&&
          selectorStyle.Get(L"margin-left")==L"3px"&&selectorStyle.Get(L"padding-left")==L"4px"&&
          selectorStyle.Get(L"padding-right")==L"5px"&&selectorStyle.Get(L"background-color")==L"teal",
          L"attribute selector operators, case flags, general siblings and background color extraction work");
    const auto styledNode=basicCssDoc.GetElementById(L"styled");
    const auto styledCss=basicCss.Compute(styledNode);
    Check(styledCss.Get(L"font-style")==L"italic"&&styledCss.Get(L"font-weight")==L"700"&&
          styledCss.Get(L"font-size")==L"12px"&&styledCss.Get(L"line-height")==L"1.5"&&
          styledCss.Get(L"border-top-style")==L"none"&&
          styledCss.Get(L"text-decoration")==L"underline line-through",
          L"font and border shorthands retain basic style components and text decorations");
    Check(StyleSheet::Color(L"#1234")==0x44112233u&&
          StyleSheet::Color(L"#11223380")==0x80112233u&&
          StyleSheet::Color(L"rgb(100% 0% 0% / 50%)")==0x80ff0000u&&
          StyleSheet::Color(L"rebeccapurple")==0xff663399u,
          L"CSS four/eight-digit hex, percentage rgb alpha and common named colors parse");
    LayoutEngine basicCssLayout(basicCssDoc,basicCss);basicCssLayout.Layout(240,120);
    const auto* styledBox=FindLayout(basicCssLayout.Root(),L"styled");
    const auto* solidBox=FindLayout(basicCssLayout.Root(),L"solid");
    Check(styledBox&&solidBox&&std::lround(styledBox->rect.width)==10&&
          std::lround(styledBox->rect.height)==10&&std::lround(solidBox->rect.width)==18&&
          std::lround(solidBox->rect.height)==18,
          L"border-style none suppresses border geometry while a solid border contributes to the box");
    Check(styledBox&&!styledBox->children.empty()&&
          styledBox->children.front()->style.Get(L"text-decoration")==L"underline line-through",
          L"text decorations propagate to the text run that DirectWrite paints");

    Document generatedContentDoc;
    Check(generatedContentDoc.Parse(
        L"<body><div id='empty' aria-label='Edit the rendered document'></div>"
        L"<div id='occupied' aria-label='Hidden label'>text</div></body>",&error),
        L"generated-content fixture parses");
    StyleSheet generatedContentCss;
    Check(generatedContentCss.Parse(
        L"body{margin:0}#empty{height:120px;padding:20px;box-sizing:border-box}"
        L"div:empty::before{content:'[' attr(aria-label) ']'}",&error),
        L"generated-content CSS parses");
    LayoutEngine generatedContentLayout(generatedContentDoc,generatedContentCss);
    generatedContentLayout.Layout(640,240);
    const auto* emptyGenerated=FindLayout(generatedContentLayout.Root(),L"empty");
    const auto* occupiedGenerated=FindLayout(generatedContentLayout.Root(),L"occupied");
    const auto* beforeGenerated=FindPseudo(emptyGenerated,L"before");
    Check(beforeGenerated&&!beforeGenerated->children.empty()&&
          beforeGenerated->children.front()->node->text==L"[Edit the rendered document]"&&
          std::abs(beforeGenerated->children.front()->rect.y-emptyGenerated->content.y)<0.01f&&
          !FindPseudo(occupiedGenerated,L"before"),
          L":empty generated content resolves attr() at the block's top content edge");

    Document flexStaticPositionDoc;
    Check(flexStaticPositionDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}.action{display:flex;"
        L"justify-content:space-around;align-items:end;position:relative;width:80px;"
        L"height:80px;padding-bottom:13px;border:1px solid #777}.action::before{"
        L"content:'';position:absolute;top:16px;width:27px;height:25px}.edge{"
        L"position:relative;width:100px;height:60px}.edge span{position:absolute;"
        L"left:auto;right:7px;top:auto;bottom:9px;width:20px;height:10px}</style>"
        L"<span id='generated-action' class='action'>Action</span>"
        L"<div id='auto-inset-host' class='edge'><span id='auto-inset'></span></div>",
        &error),L"flex static-position fixture parses");
    StyleSheet flexStaticPositionCss;
    Check(flexStaticPositionCss.Parse(flexStaticPositionDoc.StyleText(),&error),
          L"flex static-position CSS parses");
    LayoutEngine flexStaticPositionLayout(flexStaticPositionDoc,flexStaticPositionCss);
    bool flexStaticPositionDpiValid=true;
    for(const float scale:{1.0f,1.5f}){
        flexStaticPositionLayout.Layout(320,180,scale);
        const auto* action=FindLayout(flexStaticPositionLayout.Root(),L"generated-action");
        const auto* icon=FindPseudo(action,L"before");
        const auto* insetHost=FindLayout(flexStaticPositionLayout.Root(),L"auto-inset-host");
        const auto* inset=FindLayout(flexStaticPositionLayout.Root(),L"auto-inset");
        flexStaticPositionDpiValid=flexStaticPositionDpiValid&&action&&icon&&insetHost&&inset&&
            std::abs((icon->rect.x+icon->rect.width/2)-
                     (action->rect.x+action->rect.width/2))<0.01f&&
            std::abs((icon->rect.y-action->rect.y)-
                     (16.0f+(action->content.y-action->rect.y)))<0.01f&&
            std::abs((inset->rect.x+inset->rect.width)-
                     (insetHost->rect.x+insetHost->rect.width-7.0f))<0.01f&&
            std::abs((inset->rect.y+inset->rect.height)-
                     (insetHost->rect.y+insetHost->rect.height-9.0f))<0.01f;
    }
    Check(flexStaticPositionDpiValid,
          L"automatic insets use generic absolute positioning and generated flex items keep their single-item distributed alignment at 100 and 150 percent DPI");

    Document inlineBadgeDoc;
    Check(inlineBadgeDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}.title{width:400px;"
        L"font-family:'Segoe UI';font-size:20px;line-height:30px}.popular{display:inline-block;"
        L"width:22px;height:11px;vertical-align:middle}.subject{font-size:20px}.tools{width:220px;font-size:0}"
        L".recommend{font-size:12px;vertical-align:top;margin-right:15px}.recommend::before{"
        L"content:'';display:inline-block;width:16px;height:16px;vertical-align:text-top;"
        L"margin-right:5px}.copy{display:inline-block;width:19px;height:19px}</style>"
        L"<div class='title'><img id='popular-badge' class='popular'> "
        L"<span id='subject-copy' class='subject'>Subject</span></div>"
        L"<div id='inline-tools' class='tools'><span id='recommend-label' class='recommend'>"
        L"Recommend</span><span id='copy-action' class='copy'></span></div>",
        &error),L"inline badge alignment fixture parses");
    StyleSheet inlineBadgeCss;
    Check(inlineBadgeCss.Parse(inlineBadgeDoc.StyleText(),&error),
          L"inline badge alignment CSS parses");
    LayoutEngine inlineBadgeLayout(inlineBadgeDoc,inlineBadgeCss);
    bool inlineBadgeDpiValid=true;
    float popularOffset100=0,recommendOffset100=0;
    for(const float scale:{1.0f,1.5f}){
        inlineBadgeLayout.Layout(480,160,scale);
        const auto* popular=FindLayout(inlineBadgeLayout.Root(),L"popular-badge");
        const auto* subject=FindLayout(inlineBadgeLayout.Root(),L"subject-copy");
        const auto* tools=FindLayout(inlineBadgeLayout.Root(),L"inline-tools");
        const auto* recommend=FindLayout(inlineBadgeLayout.Root(),L"recommend-label");
        const auto* copy=FindLayout(inlineBadgeLayout.Root(),L"copy-action");
        inlineBadgeDpiValid=inlineBadgeDpiValid&&popular&&subject&&tools&&recommend&&copy;
        if(!popular||!subject||!tools||!recommend||!copy)continue;
        const float popularOffset=(popular->rect.y+popular->rect.height/2.0f)-
            (subject->rect.y+subject->rect.height/2.0f);
        const float recommendOffset=recommend->rect.y-copy->rect.y;
        if(scale==1.0f){popularOffset100=popularOffset;recommendOffset100=recommendOffset;}
        inlineBadgeDpiValid=inlineBadgeDpiValid&&std::abs(popularOffset)<4.0f&&
            std::abs(recommendOffset)<0.05f&&
            std::abs(popularOffset-popularOffset100)<0.01f&&
            std::abs(recommendOffset-recommendOffset100)<0.01f;
    }
    Check(inlineBadgeDpiValid,
          L"middle, top, and text-top keep replaced badges and action icons on the visual text track at 100 and 150 percent DPI");

    Document firstLetterDoc;
    Check(firstLetterDoc.Parse(
        L"<style>button{display:inline-flex;font-size:0}button::first-letter{font-size:14px}</style>"
        L"<button id='compact'>\x21bb <span>Fetch</span></button>",&error),
        L"first-letter fixture parses");
    StyleSheet firstLetterCss;
    Check(firstLetterCss.Parse(firstLetterDoc.StyleText(),&error),
          L"first-letter CSS parses");
    LayoutEngine firstLetterLayout(firstLetterDoc,firstLetterCss);
    firstLetterLayout.Layout(320,120);
    const auto* compactButton=FindLayout(firstLetterLayout.Root(),L"compact");
    const LayoutBox* visibleFirstLetter=nullptr;
    if(compactButton)for(const auto& child:compactButton->children)
        if(child->node&&child->node->type==NodeType::Text&&
           child->node->text==L"\x21bb")visibleFirstLetter=child.get();
    Check(visibleFirstLetter&&
          std::lround(StyleSheet::Length(visibleFirstLetter->style.Get(L"font-size"),
                                        16,320,0))==14&&
          visibleFirstLetter->rect.width>1,
          L"::first-letter styles the first rendered character as a reusable text run");

    Document percentageMinHeightDoc;
    Check(percentageMinHeightDoc.Parse(L"<body><main id='fill'></main></body>",&error),
          L"percentage min-height fixture parses");
    StyleSheet percentageMinHeightCss;
    Check(percentageMinHeightCss.Parse(
        L"body{margin:0}#fill{min-height:100%;box-sizing:border-box}",&error),
        L"percentage min-height CSS parses");
    LayoutEngine percentageMinHeightLayout(percentageMinHeightDoc,percentageMinHeightCss);
    percentageMinHeightLayout.Layout(640,720);
    const auto* percentageMinHeightFill=FindLayout(percentageMinHeightLayout.Root(),L"fill");
    Check(percentageMinHeightFill&&std::abs(percentageMinHeightFill->rect.height-720)<0.01f,
          L"percentage min-height resolves against a definite containing block height");

    Document fontDoc;
    Check(fontDoc.Parse(L"<html><head><style>body{font:13px/1.42 'Segoe UI Variable','Segoe UI',sans-serif}button{font:inherit}</style></head><body id='font-body'><button id='font-child'>Text</button></body></html>",&error),
          L"font shorthand fixture parses");
    StyleSheet fontCss;Check(fontCss.Parse(fontDoc.StyleText(),&error),L"font shorthand CSS parses");
    const auto bodyFont=fontCss.Compute(fontDoc.GetElementById(L"font-body"));
    const auto childFont=fontCss.Compute(fontDoc.GetElementById(L"font-child"),&bodyFont);
    Check(bodyFont.Get(L"font-size")==L"13px"&&bodyFont.Get(L"line-height")==L"1.42"&&
          bodyFont.Get(L"font-family").find(L"Segoe UI Variable")!=std::wstring::npos&&
          childFont.Get(L"font-size")==L"13px"&&childFont.Get(L"line-height")==L"1.42"&&
          childFont.Get(L"font-family")==bodyFont.Get(L"font-family")&&
          childFont.Get(L"white-space")==L"nowrap",
          L"font shorthand and font inherit populate the longhands used by text layout");
    Document fontFallbackDoc;
    Check(fontFallbackDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}pre{display:block;white-space:pre;font:14px/1.55 '__TWebFrame Missing Font__',Consolas,monospace}code{font:inherit}#explicit{font-family:Consolas}#generic{font-family:monospace}</style>"
        L"<pre><code id='fallback'>0123456789 ABC xyz</code></pre>"
        L"<pre><code id='explicit'>0123456789 ABC xyz</code></pre>"
        L"<pre><code id='generic'>0123456789 ABC xyz</code></pre>",
        &error),L"CSS font-family fallback fixture parses");
    StyleSheet fontFallbackCss;
    Check(fontFallbackCss.Parse(fontFallbackDoc.StyleText(),&error),
          L"CSS font-family fallback styles parse");
    LayoutEngine fontFallbackLayout(fontFallbackDoc,fontFallbackCss);
    float fallbackWidth100=0,explicitWidth100=0,genericWidth100=0;
    const auto verifyFontFallback=[&](float scale){
        fontFallbackLayout.Layout(360,180,scale);
        const auto* fallback=fontFallbackLayout.BoxFor(fontFallbackDoc.GetElementById(L"fallback"));
        const auto* explicitFont=fontFallbackLayout.BoxFor(fontFallbackDoc.GetElementById(L"explicit"));
        const auto* generic=fontFallbackLayout.BoxFor(fontFallbackDoc.GetElementById(L"generic"));
        if(!fallback||!explicitFont||!generic)return false;
        if(scale==1.0f){fallbackWidth100=fallback->rect.width;
            explicitWidth100=explicitFont->rect.width;genericWidth100=generic->rect.width;}
        return std::abs(fallback->rect.width-explicitFont->rect.width)<0.01f&&
               std::abs(generic->rect.width-explicitFont->rect.width)<0.01f&&
               (scale==1.0f||(std::abs(fallback->rect.width-fallbackWidth100)<0.01f&&
                std::abs(explicitFont->rect.width-explicitWidth100)<0.01f&&
                std::abs(generic->rect.width-genericWidth100)<0.01f));
    };
    Check(verifyFontFallback(1.0f)&&verifyFontFallback(1.5f),
          L"CSS font-family lists skip missing faces and map monospace to the same installed face in stable CSS DIPs at 100 and 150 percent DPI");
    Document frameStackDoc,frameStackChildDoc;
    Check(frameStackDoc.Parse(LR"HTML(<!doctype html><style>
        html,body{margin:0;padding:0}
        nav{position:relative;height:10px;z-index:9}
        aside{position:absolute;left:10px;top:10px;width:30px;height:30px;background:#ff0000;z-index:100}
        section{position:relative;z-index:0}iframe{display:block;border:0;width:60px;height:40px}
        </style><nav><aside></aside></nav><section><iframe></iframe></section>)HTML",&error)&&
        frameStackChildDoc.Parse(LR"HTML(<!doctype html><style>
        html,body{margin:0;background:#0000ff}div{position:fixed;inset:0;background:#00ff00;z-index:999999}
        </style><div></div>)HTML",&error),L"generic frame stacking fixtures parse");
    StyleSheet frameStackCss,frameStackChildCss;
    Check(frameStackCss.Parse(frameStackDoc.StyleText(),&error)&&
        frameStackChildCss.Parse(frameStackChildDoc.StyleText(),&error),L"generic frame stacking CSS parses");
    LayoutEngine frameStackLayout(frameStackDoc,frameStackCss),frameStackChildLayout(frameStackChildDoc,frameStackChildCss);
    Microsoft::WRL::ComPtr<IDWriteFactory> frameStackWrite;
    Check(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(frameStackWrite.ReleaseAndGetAddressOf()))),L"frame composition text factory initializes");
    for(const float scale:{1.0f,1.5f}){
        frameStackLayout.SetFramePainter([&](ID2D1RenderTarget* target,const LayoutBox& owner){
            frameStackChildLayout.DiscardDeviceResources();
            frameStackChildLayout.Layout(owner.content.width,owner.content.height,scale);
            D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
            target->PushAxisAlignedClip(D2D1::RectF(owner.content.x,owner.content.y,
                owner.content.x+owner.content.width,owner.content.y+owner.content.height),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            target->SetTransform(D2D1::Matrix3x2F::Translation(owner.content.x,owner.content.y)*transform);
            frameStackChildLayout.Paint(target,frameStackWrite.Get());
            target->SetTransform(transform);target->PopAxisAlignedClip();
        });
        frameStackCss.Parse(frameStackDoc.StyleText(),&error);
        auto raster=CaptureBoxRaster(frameStackLayout,scale);
        Check(raster.rendered&&raster.ColorAt(20,20,scale)==0xff0000&&raster.ColorAt(50,20,scale)==0x00ff00&&
            frameStackLayout.HitTest(20,20)==frameStackDoc.QuerySelector(L"aside"),
            L"CSS popup paints and receives input above iframe contents at 100 and 150 percent DPI");
        frameStackCss.Parse(frameStackDoc.StyleText()+L"aside{background:rgba(255,0,0,.5)}",&error);
        raster=CaptureBoxRaster(frameStackLayout,scale);const auto mixed=raster.ColorAt(20,20,scale);
        Check(raster.rendered&&((mixed>>16)&255)>=127&&((mixed>>16)&255)<=128&&
            ((mixed>>8)&255)>=127&&((mixed>>8)&255)<=128&&(mixed&255)==0,
            L"translucent popup blends with iframe pixels instead of cutting a native-window hole at both DPIs");
        frameStackCss.Parse(frameStackDoc.StyleText()+L"section{z-index:10}",&error);
        raster=CaptureBoxRaster(frameStackLayout,scale);
        Check(raster.rendered&&raster.ColorAt(20,20,scale)==0x00ff00&&
            frameStackLayout.HitTest(20,20)==frameStackDoc.QuerySelector(L"iframe"),
            L"higher owner stacking context puts iframe above popup despite popup descendant z-index at both DPIs");
        frameStackCss.Parse(frameStackDoc.StyleText()+L"nav{overflow:hidden}",&error);
        raster=CaptureBoxRaster(frameStackLayout,scale);
        Check(raster.rendered&&raster.ColorAt(20,20,scale)==0x00ff00,
            L"ancestor overflow clipping also applies to a popup above iframe content at both DPIs");
    }
    frameStackLayout.SetFramePainter({});frameStackChildLayout.DiscardDeviceResources();

    Document textRasterDoc;
    Check(textRasterDoc.Parse(
        L"<style>html,body{margin:0;width:100%;height:100%;background:#fff}pre{margin:4px;color:#000;white-space:pre;font:400 18px Arial}</style><pre id='text-raster'>ASDF 0123</pre>",
        &error),L"text antialiasing fixture parses");
    StyleSheet textRasterCss;
    Check(textRasterCss.Parse(textRasterDoc.StyleText(),&error),
          L"text antialiasing fixture styles parse");
    LayoutEngine textRasterLayout(textRasterDoc,textRasterCss);
    const auto textRasterStyle=textRasterCss.Compute(textRasterDoc.GetElementById(L"text-raster"));
    Check(textRasterStyle.Get(L"font-weight")==L"400",
          L"normal CSS text retains computed font-weight 400 before rasterization");
    const auto textRaster100=CaptureTextRaster(textRasterLayout,1.0f);
    const auto textRaster150=CaptureTextRaster(textRasterLayout,1.5f);
    const auto isGrayscaleText=[](const TextRasterSample& sample){
        return sample.rendered&&sample.grayscaleMode&&sample.webRenderingParams&&
               sample.grayscaleEdgePixels>0&&sample.colorFringePixels==0;
    };
    const auto isSubpixelText=[](const TextRasterSample& sample){
        return sample.rendered&&!sample.grayscaleMode&&sample.webRenderingParams&&sample.colorFringePixels>0;
    };
    Check(isSubpixelText(textRaster100)&&isSubpixelText(textRaster150),
          L"opaque page text uses subpixel coverage at 100 and 150 percent DPI");
    Check(isGrayscaleText(CaptureTextRaster(textRasterLayout,1.0f,true))&&
          isGrayscaleText(CaptureTextRaster(textRasterLayout,1.5f,true)),
          L"transparent compositing surfaces use grayscale text coverage at both DPIs");
    Document fragmentIconDoc;
    Check(fragmentIconDoc.Parse(LR"HTML(
        <!doctype html>
        <style>*{margin:0;padding:0}body{font:12px/1.6 'Malgun Gothic'}
        .row{display:flex;height:29px;width:850px}.items{display:flex;align-items:center;margin-left:auto;font-size:11px}
        ul{display:flex;align-items:center;list-style:none}a{color:inherit;text-decoration:none}
        img{width:14px;height:14px;vertical-align:text-top}</style>
        <div class="row"><nav class="items"><ul><li><a>First</a></li><li><a><img id="nested-icon"></a></li></ul></nav></div>
        <div class="row"><nav class="items"><ul><li><a>Second</a></li><li><img id="direct-icon"></li></ul></nav></div>
        <div class="row"><nav class="items"><ul><li><a>Third</a></li><li><a><span><img id="deep-icon"></span></a></li></ul></nav></div>
    )HTML",&error),L"inline image and dynamic hidden menu fixture parses");
    JavaScriptRuntime fragmentIconJs(fragmentIconDoc);
    Check(fragmentIconJs.Execute(LR"JS(
        var rows=document.querySelectorAll('.row');
        for(var i=0;i<rows.length;i++){
            var item=rows[i].querySelector('li:last-child'),fragment=document.createDocumentFragment();
            fragment.appendChild(document.createElement('br'));
            var hidden=document.createElement('span');hidden.style.display='none';hidden.textContent='Hidden menu';
            fragment.appendChild(hidden);
            if(i%2)item.appendChild(fragment);else item.insertBefore(fragment,null);
        }
    )JS",nullptr,&error),error.c_str());
    StyleSheet fragmentIconCss;Check(fragmentIconCss.Parse(fragmentIconDoc.StyleText(),&error),
        L"inline image and dynamic hidden menu styles parse");
    LayoutEngine fragmentIconLayout(fragmentIconDoc,fragmentIconCss);
    bool fragmentIconsAligned=true;
    const auto fragmentIconRows=fragmentIconDoc.QuerySelectorAll(L".row");
    for(const float scale:{1.0f,1.5f}){
        fragmentIconLayout.Layout(960,660,scale);
        const wchar_t* ids[]={L"nested-icon",L"direct-icon",L"deep-icon"};
        for(size_t index=0;index<3;++index){
            const auto* row=fragmentIconLayout.BoxFor(fragmentIconRows[index]);
            const auto* image=fragmentIconLayout.BoxFor(fragmentIconDoc.GetElementById(ids[index]));
            fragmentIconsAligned=fragmentIconsAligned&&row&&image&&
                std::abs(image->rect.y-row->rect.y-6.7f)<0.04f&&
                std::abs(image->rect.width-14)<0.01f&&std::abs(image->rect.height-14)<0.01f;
        }
    }
    Check(fragmentIconsAligned,
        L"fragment-inserted hidden menus do not raise text-top icons in centered flex rows at 100 and 150 percent DPI");
    Document boxRasterDoc;
    Check(boxRasterDoc.Parse(
          L"<style>html,body{margin:0}.probe{position:absolute;box-sizing:border-box;left:10.4px;top:10.4px;width:20.4px;height:12.4px;border:1px solid #445566;background:#112233}</style><div class='probe'></div>",
          &error),L"physical-pixel box fixture parses");
    StyleSheet boxRasterCss;Check(boxRasterCss.Parse(boxRasterDoc.StyleText(),&error),
          L"physical-pixel box CSS parses");
    LayoutEngine boxRasterLayout(boxRasterDoc,boxRasterCss);
    const auto boxRaster100=CaptureBoxRaster(boxRasterLayout,1.0f);
    const auto boxRaster150=CaptureBoxRaster(boxRasterLayout,1.5f);
    const auto hasSinglePixelBorder=[](const BoxRasterSample& sample,LONG left,LONG top,
                                      LONG right,LONG bottom){
        constexpr std::uint32_t border=0x00445566u;
        constexpr std::uint32_t fill=0x00112233u;
        return sample.rendered&&sample.left==left&&sample.top==top&&
            sample.right==right&&sample.bottom==bottom&&
            sample.topEdge==border&&sample.belowTopEdge==fill&&
            sample.aboveBottomEdge==fill&&sample.bottomEdge==border;
    };
    Check(hasSinglePixelBorder(boxRaster100,10,10,30,22)&&
          hasSinglePixelBorder(boxRaster150,16,16,45,33),
          L"box backgrounds and borders snap to physical pixel edges at 100 and 150 percent DPI");

    const auto directWriteLineHeight=[](const wchar_t* family,float size){
        Microsoft::WRL::ComPtr<IDWriteFactory> factory;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(factory.ReleaseAndGetAddressOf())))||
           FAILED(factory->CreateTextFormat(family,nullptr,DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"ko-kr",&format))||
           FAILED(factory->CreateTextLayout(L"Hg",2,format.Get(),100000.0f,100000.0f,&layout)))
            return 0.0f;
        DWRITE_LINE_METRICS metrics{};UINT32 count=0;
        return SUCCEEDED(layout->GetLineMetrics(&metrics,1,&count))&&count?metrics.height:0.0f;
    };
    Document normalLineDoc;
    Check(normalLineDoc.Parse(
          L"<style>body{margin:0}#normal-line{font-family:'Segoe UI';font-size:13px}</style><div id='normal-line'>Text</div>",
          &error),L"normal line-height fixture parses");
    StyleSheet normalLineCss;Check(normalLineCss.Parse(normalLineDoc.StyleText(),&error),
          L"normal line-height CSS parses");
    LayoutEngine normalLineLayout(normalLineDoc,normalLineCss);normalLineLayout.Layout(200,80,1.0f);
    const auto expectedNormalLineHeight=directWriteLineHeight(L"Segoe UI",13.0f);
    const auto* normalLineBox=normalLineLayout.BoxFor(normalLineDoc.GetElementById(L"normal-line"));
    const float normalLineHeight100=normalLineBox?normalLineBox->rect.height:0;
    normalLineLayout.Relayout(200,80,1.5f);
    normalLineBox=normalLineLayout.BoxFor(normalLineDoc.GetElementById(L"normal-line"));
    const float normalLineHeight150=normalLineBox?normalLineBox->rect.height:0;
    Check(expectedNormalLineHeight>0&&
          std::abs(normalLineHeight100-expectedNormalLineHeight)<0.001f&&
          std::abs(normalLineHeight150-expectedNormalLineHeight)<0.001f,
          L"normal line-height follows font metrics without changing CSS geometry across DPI scales");
    Document inlinePaddingDoc;
    Check(inlinePaddingDoc.Parse(
          L"<style>body{margin:0}.header{display:flex;height:32px;align-items:center}.summary{font-family:'Segoe UI';font-size:11px}.summary code{padding:4px 7px}</style><div id='inline-header' class='header'><div id='inline-summary' class='summary'><span id='inline-path'>Path</span><code id='inline-code'>branch</code></div></div>",
          &error),L"inline padding line-box fixture parses");
    StyleSheet inlinePaddingCss;Check(inlinePaddingCss.Parse(inlinePaddingDoc.StyleText(),&error),
          L"inline padding line-box CSS parses");
    LayoutEngine inlinePaddingLayout(inlinePaddingDoc,inlinePaddingCss);inlinePaddingLayout.Layout(240,80);
    const auto* inlineSummary=inlinePaddingLayout.BoxFor(inlinePaddingDoc.GetElementById(L"inline-summary"));
    const auto* inlineCode=inlinePaddingLayout.BoxFor(
        inlinePaddingDoc.GetElementById(L"inline-code"));
    Check(inlineSummary&&
          std::abs(inlineSummary->rect.height-directWriteLineHeight(L"Segoe UI",11.0f))<0.001f,
          L"vertical padding on a non-atomic inline box does not enlarge its containing line");
    Check(inlineCode&&std::lround(inlineCode->rect.y)==8&&
          std::lround(inlineCode->rect.y+inlineCode->rect.height)==27,
          L"a padded generic-monospace inline box aligns its content baseline at 100 percent DPI");
    inlinePaddingLayout.Relayout(240,80,1.5f);
    inlineCode=inlinePaddingLayout.BoxFor(inlinePaddingDoc.GetElementById(L"inline-code"));
    Check(inlineCode&&std::lround(inlineCode->rect.y*1.5f)==11&&
          std::lround((inlineCode->rect.y+inlineCode->rect.height)*1.5f)==39,
          L"a padded generic-monospace inline box keeps browser pixel bounds at 150 percent DPI");

    Document rootFontDoc;
    Check(rootFontDoc.Parse(
        L"<html><head><style>:root{font-family:'Segoe UI';font-size:14px}*{box-sizing:border-box}.switch{display:flex;align-items:center;width:220px;height:44px}.copy{display:grid;flex:1;gap:1px;min-width:0}.copy strong{overflow:hidden;font-size:13px;text-overflow:ellipsis;white-space:nowrap}.copy small{font-size:11px}</style></head><body><div class='switch'><span class='copy'><strong id='workspace-title'>Orbit Workspace</strong><small id='workspace-subtitle'>personal</small></span></div><p id='hero-description'>Win32 host description</p></body></html>",&error),
          L"root font inheritance fixture parses");
    StyleSheet rootFontCss;Check(rootFontCss.Parse(rootFontDoc.StyleText(),&error),L"root font inheritance CSS parses");
    LayoutEngine rootFontLayout(rootFontDoc,rootFontCss);rootFontLayout.Layout(320,160);
    const auto* rootBody=rootFontLayout.Root();
    const auto* workspaceTitle=FindLayout(rootFontLayout.Root(),L"workspace-title");
    const auto* workspaceSubtitle=FindLayout(rootFontLayout.Root(),L"workspace-subtitle");
    const auto* heroDescription=FindLayout(rootFontLayout.Root(),L"hero-description");
    Check(rootBody&&heroDescription&&rootBody->style.Get(L"font-size")==L"14px"&&
          heroDescription->style.Get(L"font-size")==L"14px",
          L"a body layout root inherits the author font size from the html :root ancestor");
    Document themedRootDoc;
    Check(themedRootDoc.Parse(
          L"<html data-theme='light'><head><style>:root[data-theme='light']{--theme-ink:#123456}.marker{color:var(--theme-ink)}</style></head><body><span id='theme-marker' class='marker'>x</span></body></html>",&error),
          L"compound root selector fixture parses");
    StyleSheet themedRootCss;Check(themedRootCss.Parse(themedRootDoc.StyleText(),&error),L"compound root selector CSS parses");
    const auto themedHtmlStyle=themedRootCss.Compute(themedRootDoc.QuerySelector(L"html"));
    const auto themedBodyStyle=themedRootCss.Compute(themedRootDoc.Body(),&themedHtmlStyle);
    const auto themedMarkerStyle=themedRootCss.Compute(themedRootDoc.GetElementById(L"theme-marker"),&themedBodyStyle);
    Check(themedMarkerStyle.Get(L"color")==L"#123456",
          L"a compound :root selector applies theme variables through the normal cascade");
    const auto rowsRemainSeparateAtScale=[&](float scale){
        return workspaceTitle&&workspaceSubtitle&&
            std::ceil((workspaceTitle->rect.y+workspaceTitle->rect.height)*scale)<=
            std::floor(workspaceSubtitle->rect.y*scale);
    };
    Check(workspaceTitle&&workspaceSubtitle&&
          workspaceTitle->rect.y+workspaceTitle->rect.height<=workspaceSubtitle->rect.y+0.01f&&
          rowsRemainSeparateAtScale(1.0f)&&rowsRemainSeparateAtScale(1.5f),
          L"overflow-clipped grid labels retain separate intrinsic rows at 100 and 150 percent scaling");

    struct DpiLayoutSample { UINT dpi=0; bool loaded=false; bool clicked=false; std::vector<double> geometry; };
    const auto captureDpiLayout=[&](DPI_AWARENESS_CONTEXT context){
        DpiLayoutSample sample;
        const auto previous=SetThreadDpiAwarenessContext(context);
        HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
            0,0,2200,1500,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(host){
            sample.dpi=GetDpiForWindow(host);
            constexpr double cssWidth=1328,cssHeight=852;
            const double scale=static_cast<double>(sample.dpi)/96.0;
            RECT bounds{0,0,static_cast<LONG>(std::lround(cssWidth*scale)),
                static_cast<LONG>(std::lround(cssHeight*scale))};
            if(auto view=TWebFrame::View::Create(host,bounds)){
                view->SetMessageHandler([&](const std::wstring& message){
                    sample.clicked=message.find(L"dpi-click")!=std::wstring::npos;
                });
                sample.loaded=view->NavigateToString(
                    L"<style>*{box-sizing:border-box}html,body{width:100%;height:100%;margin:0}.probe{position:absolute;left:10vw;top:10vh;width:50vw;height:25vh;font-size:4vw}</style><button id='dpi-probe' class='probe'>DPI</button><script>const probe=document.getElementById('dpi-probe'),rect=probe.getBoundingClientRect();window.initialGeometry=[rect.x,rect.y,rect.width,rect.height,getComputedStyle(probe).fontSize,probe.offsetWidth,probe.offsetHeight,document.documentElement.clientWidth,document.documentElement.clientHeight].join(',');probe.addEventListener('click',()=>window.chrome.webview.postMessage('dpi-click'));</script>",
                    L"https://dpi.test/");
                std::wstring value,errorText;
                if(sample.loaded&&view->ExecuteScript(
                    L"return window.initialGeometry;",
                    &value,&errorText)){
                    size_t start=0;
                    while(start<=value.size()){
                        const auto comma=value.find(L',',start);
                        try{sample.geometry.push_back(std::stod(value.substr(start,
                            comma==std::wstring::npos?std::wstring::npos:comma-start)));}
                        catch(...){sample.geometry.clear();break;}
                        if(comma==std::wstring::npos)break;
                        start=comma+1;
                    }
                }
                if(sample.geometry.size()>=4){
                    const int x=static_cast<int>(std::lround(
                        (sample.geometry[0]+sample.geometry[2]/2.0)*scale));
                    const int y=static_cast<int>(std::lround(
                        (sample.geometry[1]+sample.geometry[3]/2.0)*scale));
                    SendMessageW(view->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
                    SendMessageW(view->Window(),WM_LBUTTONUP,0,MAKELPARAM(x,y));
                }
            }
            DestroyWindow(host);
        }
        SetThreadDpiAwarenessContext(previous);
        return sample;
    };
    const auto dpi100=captureDpiLayout(DPI_AWARENESS_CONTEXT_UNAWARE);
    const auto dpiMonitor=captureDpiLayout(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto matchesCssViewport=[](const DpiLayoutSample& sample){
        const double expected[]={132.8,85.2,664.0,213.0,53.12,664.0,213.0,1328.0,852.0};
        if(!sample.loaded||!sample.clicked||sample.geometry.size()!=std::size(expected))return false;
        for(size_t index=0;index<std::size(expected);++index)
            if(std::abs(sample.geometry[index]-expected[index])>0.02)return false;
        return true;
    };
    Check(dpi100.dpi==96&&dpiMonitor.dpi>=96&&matchesCssViewport(dpi100)&&
          matchesCssViewport(dpiMonitor),
          L"loading scripts see current CSS geometry and offset metrics while hit testing remains correct at 96 DPI and per-monitor DPI");

    Document basicHtmlDoc;
    Check(basicHtmlDoc.Parse(
        L"<body id='basic-body'><p id='first-paragraph'>first<br>line"
        L"<p id='second-paragraph'>second<div id='following-block'>block</div></p>"
        L"<p id='phrasing'><a id='link'><b id='bold'>bold</b> <em id='emphasis'>em</em> <code id='code'>code</code></a>&nbsp;x</body>",
        &error),L"basic HTML element fixture parses");
    const auto basicBody=basicHtmlDoc.GetElementById(L"basic-body");
    const auto firstParagraph=basicHtmlDoc.GetElementById(L"first-paragraph");
    const auto secondParagraph=basicHtmlDoc.GetElementById(L"second-paragraph");
    const auto followingBlock=basicHtmlDoc.GetElementById(L"following-block");
    const auto phrasing=basicHtmlDoc.GetElementById(L"phrasing");
    Check(firstParagraph&&secondParagraph&&followingBlock&&
          firstParagraph->parent.lock()==basicBody&&secondParagraph->parent.lock()==basicBody&&
          followingBlock->parent.lock()==basicBody,
          L"paragraphs close implicitly before another paragraph or block element");
    Check(firstParagraph&&phrasing&&firstParagraph->InnerText()==L"first\nline"&&
          phrasing->InnerText().find(static_cast<wchar_t>(0x00a0))!=std::wstring::npos,
          L"br contributes a line break to inner text and nbsp decodes to a non-breaking space");
    StyleSheet basicHtmlCss;Check(basicHtmlCss.Parse(L"",&error),L"basic HTML default styles parse");
    const auto paragraphStyle=basicHtmlCss.Compute(firstParagraph);
    const auto linkStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"link"));
    const auto boldStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"bold"));
    const auto emphasisStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"emphasis"));
    const auto codeStyle=basicHtmlCss.Compute(basicHtmlDoc.GetElementById(L"code"));
    Check(paragraphStyle.Is(L"display",L"block")&&paragraphStyle.Get(L"margin")==L"1em 0"&&
          linkStyle.Is(L"display",L"inline")&&boldStyle.Get(L"font-weight")==L"700"&&
          emphasisStyle.Is(L"font-style",L"italic")&&codeStyle.Get(L"font-family")==L"monospace",
          L"paragraph and common phrasing elements receive browser-like default styles");

    Document breakLayoutDoc;
    Check(breakLayoutDoc.Parse(
        L"<style>body{margin:0;font-size:10px;line-height:20px}p{margin:0}</style>"
        L"<body><p><span id='line-one'>one</span><br><span id='line-two'>two</span><br><br><span id='line-four'>four</span></p><p id='after-lines'>after</p><hr id='rule'></body>",
        &error),L"line break layout fixture parses");
    StyleSheet breakLayoutCss;Check(breakLayoutCss.Parse(breakLayoutDoc.StyleText(),&error),L"line break layout CSS parses");
    LayoutEngine breakLayout(breakLayoutDoc,breakLayoutCss);breakLayout.Layout(240,160);
    const auto* lineOne=FindLayout(breakLayout.Root(),L"line-one");
    const auto* lineTwo=FindLayout(breakLayout.Root(),L"line-two");
    const auto* lineFour=FindLayout(breakLayout.Root(),L"line-four");
    const auto* afterLines=FindLayout(breakLayout.Root(),L"after-lines");
    const auto* rule=FindLayout(breakLayout.Root(),L"rule");
    Check(lineOne&&lineTwo&&lineFour&&afterLines&&
          std::lround(lineTwo->rect.y-lineOne->rect.y)==20&&
          std::lround(lineFour->rect.y-lineTwo->rect.y)==40&&
          std::lround(afterLines->rect.y-lineOne->parent->content.y)==80,
          L"br creates one line break and consecutive br elements preserve an empty line");
    Check(rule&&std::lround(rule->rect.height)==1&&rule->style.Get(L"background")==L"#808080",
          L"hr receives a visible one-pixel separator default");

    Document preservedWhitespaceDoc;
    Check(preservedWhitespaceDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}body{font-size:10px;line-height:20px}"
        L"#source{display:block;width:160px;white-space:pre-wrap}"
        L"#collapsed{display:block;width:160px;white-space:normal}"
        L"#editor{display:block;width:160px;height:40px;overflow:auto;white-space:pre-wrap}</style>"
        L"<pre id='source'><code id='source-code'></code></pre>"
        L"<div id='collapsed'></div><textarea id='editor'></textarea>",
        &error),L"preserved whitespace fixture parses");
    JavaScriptRuntime preservedWhitespaceJs(preservedWhitespaceDoc);
    Check(preservedWhitespaceJs.Execute(
        L"document.getElementById('source-code').innerHTML='alpha\\nbeta\\ngamma';"
        L"document.getElementById('collapsed').textContent='alpha\\nbeta\\ngamma';"
        L"document.getElementById('editor').value='alpha\\nbeta\\ngamma';",
        nullptr,&error),error.c_str());
    StyleSheet preservedWhitespaceCss;
    Check(preservedWhitespaceCss.Parse(preservedWhitespaceDoc.StyleText(),&error),
          L"preserved whitespace CSS parses");
    LayoutEngine preservedWhitespaceLayout(preservedWhitespaceDoc,preservedWhitespaceCss);
    preservedWhitespaceLayout.Layout(240,180);
    const auto* preservedSource=FindLayout(preservedWhitespaceLayout.Root(),L"source");
    const auto* preservedSourceCode=FindLayout(preservedWhitespaceLayout.Root(),L"source-code");
    const auto* collapsedText=FindLayout(preservedWhitespaceLayout.Root(),L"collapsed");
    const auto* multilineEditor=FindLayout(preservedWhitespaceLayout.Root(),L"editor");
    Check(preservedSourceCode&&collapsedText&&
          preservedSourceCode->rect.height>collapsedText->rect.height*2.5f,
          L"pre-wrap preserves authored line breaks from JavaScript-generated DOM text");
    Check(collapsedText&&collapsedText->rect.height<30,
          L"normal white-space collapses authored line breaks into one line");
    Check(preservedSource&&preservedSourceCode&&collapsedText&&
          preservedSource->rect.height>=preservedSourceCode->rect.height&&
          collapsedText->rect.y>=preservedSourceCode->rect.y+preservedSourceCode->rect.height-0.5f,
          L"an auto-height block contains multiline inline descendants before following flow content");
    Check(multilineEditor&&multilineEditor->scrollHeight>multilineEditor->content.height,
          L"textarea overflow measures every preserved source line");
    const auto preservedSourceText=preservedWhitespaceDoc.GetElementById(L"source-code")->children.front();
    const auto verifyPreservedSelectionGeometry=[&](float scale){
        preservedWhitespaceLayout.Layout(240,180,scale);
        LayoutRect thirdLineCaret{};std::shared_ptr<Node> hitNode;size_t hitOffset=0;
        std::vector<LayoutRect> selectionRects;
        const bool caret=preservedWhitespaceLayout.TextCaretRect(
            preservedSourceText,12,thirdLineCaret);
        const bool hit=caret&&preservedWhitespaceLayout.HitTestText(preservedSourceText,
            thirdLineCaret.x+12.0f,thirdLineCaret.y+thirdLineCaret.height*0.5f,
            hitNode,hitOffset);
        const bool range=preservedWhitespaceLayout.TextRangeRects(preservedSourceText,2,
            preservedSourceText->text.size()-4,selectionRects);
        if(!caret||!hit||hitNode!=preservedSourceText||hitOffset<12||!range)return false;
        std::vector<float> lines;
        for(const auto& rect:selectionRects){
            if(std::none_of(lines.begin(),lines.end(),[&](float y){return std::abs(y-rect.y)<0.01f;}))
                lines.push_back(rect.y);
        }
        return lines.size()==3&&lines[0]<lines[1]&&lines[1]<lines[2];
    };
    Check(verifyPreservedSelectionGeometry(1.0f)&&verifyPreservedSelectionGeometry(1.5f),
          L"preserved multiline DOM text hit testing and selection rectangles cover every line at 100 and 150 percent DPI");

    Document editableCaretDoc;
    Check(editableCaretDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}article{display:block;width:320px;min-height:100px;padding:12px;font:16px/24px 'Segoe UI'}p{display:block;margin:0}</style>"
        L"<article id='editable' contenteditable='true'><p id='mixed'>alpha <strong>bravo</strong> charlie</p></article>",
        &error),L"contenteditable caret fixture parses");
    StyleSheet editableCaretCss;
    Check(editableCaretCss.Parse(editableCaretDoc.StyleText(),&error),L"contenteditable caret CSS parses");
    LayoutEngine editableCaretLayout(editableCaretDoc,editableCaretCss);editableCaretLayout.Layout(360,160);
    const auto editableRoot=editableCaretDoc.GetElementById(L"editable");
    const auto mixedParagraph=editableCaretDoc.GetElementById(L"mixed");
    const auto trailingText=mixedParagraph&&!mixedParagraph->children.empty()?mixedParagraph->children.back():std::shared_ptr<Node>{};
    const auto* trailingTextBox=editableCaretLayout.BoxFor(trailingText);
    LayoutRect unpaintedCaret{};
    Check(trailingTextBox&&editableCaretLayout.TextCaretRect(trailingText,2,unpaintedCaret)&&
          unpaintedCaret.x>trailingTextBox->rect.x,
          L"contenteditable caret geometry resolves a DOM text offset before the first paint");
    std::shared_ptr<Node> hitText;size_t hitTextOffset=0;
    Check(trailingTextBox&&editableCaretLayout.HitTestText(mixedParagraph,
              trailingTextBox->rect.x+trailingTextBox->rect.width*0.5f,
              trailingTextBox->rect.y+trailingTextBox->rect.height*0.5f,hitText,hitTextOffset)&&
          hitText==trailingText&&hitTextOffset>0&&hitTextOffset<trailingText->text.size(),
          L"contenteditable pointer placement selects the nearest DOM text offset before the first paint");

    Document paragraphFlowDoc;
    Check(paragraphFlowDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}article{display:block;width:320px;padding:12px;font:16px/24px 'Segoe UI'}p{display:block;margin:0}</style>"
        L"<article contenteditable='true'><p id='line-before'>hello</p><p id='line-after'></p></article>",
        &error),L"contenteditable paragraph flow fixture parses");
    StyleSheet paragraphFlowCss;
    Check(paragraphFlowCss.Parse(paragraphFlowDoc.StyleText(),&error),
          L"contenteditable paragraph flow CSS parses");
    LayoutEngine paragraphFlowLayout(paragraphFlowDoc,paragraphFlowCss);
    paragraphFlowLayout.Layout(360,160,1.0f);
    const auto* paragraphBefore100=paragraphFlowLayout.BoxFor(
        paragraphFlowDoc.GetElementById(L"line-before"));
    const auto* paragraphAfter100=paragraphFlowLayout.BoxFor(
        paragraphFlowDoc.GetElementById(L"line-after"));
    const float paragraphBeforeY100=paragraphBefore100?paragraphBefore100->rect.y:0.0f;
    const float paragraphBeforeHeight100=paragraphBefore100?paragraphBefore100->rect.height:0.0f;
    const float paragraphAfterY100=paragraphAfter100?paragraphAfter100->rect.y:0.0f;
    const float paragraphAfterHeight100=paragraphAfter100?paragraphAfter100->rect.height:0.0f;
    paragraphFlowLayout.Layout(360,160,1.5f);
    const auto* paragraphBefore150=paragraphFlowLayout.BoxFor(
        paragraphFlowDoc.GetElementById(L"line-before"));
    const auto* paragraphAfter150=paragraphFlowLayout.BoxFor(
        paragraphFlowDoc.GetElementById(L"line-after"));
    Check(paragraphBefore100&&paragraphAfter100&&paragraphBefore150&&paragraphAfter150&&
          paragraphAfterY100>=paragraphBeforeY100+paragraphBeforeHeight100-0.01f&&
          paragraphAfter150->rect.y>=paragraphBefore150->rect.y+paragraphBefore150->rect.height-0.01f&&
          paragraphAfterHeight100>0&&paragraphAfter150->rect.height>0&&
          std::abs(paragraphBeforeY100-paragraphBefore150->rect.y)<0.01f&&
          std::abs(paragraphAfterY100-paragraphAfter150->rect.y)<0.01f&&
          std::abs(paragraphAfterHeight100-paragraphAfter150->rect.height)<0.01f,
          L"an empty editable paragraph reserves the same distinct CSS line box at 100 and 150 percent DPI");
    const auto paragraphRoot=paragraphFlowDoc.GetElementById(L"line-before")->parent.lock();
    const auto paragraphText=paragraphFlowDoc.GetElementById(L"line-before")->children.front();
    const auto paragraphEmpty=paragraphFlowDoc.GetElementById(L"line-after");
    const auto verifyVerticalCaret=[&](float scale){
        paragraphFlowLayout.Layout(360,160,scale);
        LayoutRect start{};std::shared_ptr<Node> downTarget,upTarget;
        size_t downOffset=0,upOffset=0;
        if(!paragraphFlowLayout.TextCaretRect(paragraphText,3,start))return false;
        if(!paragraphFlowLayout.VerticalCaretPosition(paragraphRoot,paragraphText,3,start.x,
                false,downTarget,downOffset))return false;
        if(downTarget!=paragraphEmpty||downOffset!=0)return false;
        if(!paragraphFlowLayout.VerticalCaretPosition(paragraphRoot,downTarget,downOffset,start.x,
                true,upTarget,upOffset))return false;
        return upTarget==paragraphText&&upOffset==3;
    };
    Check(verifyVerticalCaret(1.0f)&&verifyVerticalCaret(1.5f),
          L"vertical caret hit testing crosses between text and an empty paragraph at the same visual column at 100 and 150 percent DPI");

    Document wrappedCaretDoc;
    Check(wrappedCaretDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}article{display:block;width:90px;font:16px/24px 'Segoe UI'}</style>"
        L"<article id='wrapped-editor' contenteditable='true'>alpha beta gamma delta epsilon</article>",
        &error),L"wrapped contenteditable caret fixture parses");
    StyleSheet wrappedCaretCss;
    Check(wrappedCaretCss.Parse(wrappedCaretDoc.StyleText(),&error),
          L"wrapped contenteditable caret CSS parses");
    LayoutEngine wrappedCaretLayout(wrappedCaretDoc,wrappedCaretCss);
    const auto wrappedEditor=wrappedCaretDoc.GetElementById(L"wrapped-editor");
    const auto wrappedText=wrappedEditor->children.front();
    const auto verifyWrappedCaret=[&](float scale){
        wrappedCaretLayout.Layout(180,180,scale);
        LayoutRect start{},downCaret{};std::shared_ptr<Node> downTarget,upTarget;
        size_t downOffset=0,upOffset=0;
        if(!wrappedCaretLayout.TextCaretRect(wrappedText,2,start))return false;
        if(!wrappedCaretLayout.VerticalCaretPosition(wrappedEditor,wrappedText,2,start.x,
                false,downTarget,downOffset)||downTarget!=wrappedText||downOffset<=2)return false;
        if(!wrappedCaretLayout.TextCaretRect(downTarget,downOffset,downCaret)||
           downCaret.y<=start.y)return false;
        if(!wrappedCaretLayout.VerticalCaretPosition(wrappedEditor,downTarget,downOffset,start.x,
                true,upTarget,upOffset))return false;
        return upTarget==wrappedText&&upOffset==2;
    };
    Check(verifyWrappedCaret(1.0f)&&verifyWrappedCaret(1.5f),
          L"vertical caret hit testing follows visual lines created by text wrapping at 100 and 150 percent DPI");

    Document trailingNewlineCaretDoc;
    Check(trailingNewlineCaretDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}article,pre{display:block;width:240px}pre{white-space:pre-wrap;font:16px/24px 'Segoe UI'}code{font:inherit}</style>"
        L"<article contenteditable='true'><pre id='trailing-pre'><code id='trailing-code'>hello</code></pre></article>",
        &error),L"trailing preformatted newline caret fixture parses");
    StyleSheet trailingNewlineCaretCss;
    Check(trailingNewlineCaretCss.Parse(trailingNewlineCaretDoc.StyleText(),&error),
          L"trailing preformatted newline caret CSS parses");
    LayoutEngine trailingNewlineCaretLayout(trailingNewlineCaretDoc,trailingNewlineCaretCss);
    const auto trailingCode=trailingNewlineCaretDoc.GetElementById(L"trailing-code");
    const auto trailingNewlineText=trailingCode&&!trailingCode->children.empty()?
        trailingCode->children.front():std::shared_ptr<Node>{};
    const auto verifyTrailingNewlineCaret=[&](float scale){
        if(!trailingNewlineText)return false;
        trailingNewlineText->text=L"hello\n";
        trailingNewlineCaretLayout.Layout(280,140,scale);
        const auto* emptyLinePre=trailingNewlineCaretLayout.BoxFor(
            trailingNewlineCaretDoc.GetElementById(L"trailing-pre"));
        LayoutRect firstLine{},emptyLine{};
        if(!emptyLinePre||
           !trailingNewlineCaretLayout.TextCaretRect(trailingNewlineText,2,firstLine)||
           !trailingNewlineCaretLayout.TextCaretRect(
               trailingNewlineText,trailingNewlineText->text.size(),emptyLine))return false;
        const float emptyLinePreHeight=emptyLinePre->rect.height;
        trailingNewlineText->text=L"hello\nX";
        trailingNewlineCaretLayout.Layout(280,140,scale);
        LayoutRect beforeTypedCharacter{};
        if(!trailingNewlineCaretLayout.TextCaretRect(
               trailingNewlineText,trailingNewlineText->text.size()-1,beforeTypedCharacter))return false;
        return emptyLine.y>=firstLine.y+firstLine.height-0.01f&&
               emptyLinePreHeight>=firstLine.height*2.0f-0.01f&&
               std::abs(emptyLine.x-beforeTypedCharacter.x)<0.01f&&
               std::abs(emptyLine.y-beforeTypedCharacter.y)<0.01f&&
               std::abs(emptyLine.height-beforeTypedCharacter.height)<0.01f&&
               std::abs(emptyLine.width-1.0f/scale)<0.01f;
    };
    Check(verifyTrailingNewlineCaret(1.0f)&&verifyTrailingNewlineCaret(1.5f),
          L"a preserved trailing newline reserves the same empty-line caret position before and after typing at 100 and 150 percent DPI");

    Document atomicCaretDoc;
    Check(atomicCaretDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}article{display:block;width:320px;min-height:100px;padding:12px;font:16px/24px 'Segoe UI'}p{display:block;margin:0}img{width:40px;height:30px}</style>"
        L"<article contenteditable='true'><p id='atomic-line'>alpha<img id='atomic-image'>Z</p></article>",
        &error),L"atomic contenteditable caret fixture parses");
    StyleSheet atomicCaretCss;
    Check(atomicCaretCss.Parse(atomicCaretDoc.StyleText(),&error),
          L"atomic contenteditable caret CSS parses");
    LayoutEngine atomicCaretLayout(atomicCaretDoc,atomicCaretCss);
    const auto atomicLine=atomicCaretDoc.GetElementById(L"atomic-line");
    const auto atomicImage=atomicCaretDoc.GetElementById(L"atomic-image");
    const auto atomicTrailingText=atomicLine&&!atomicLine->children.empty()?
        atomicLine->children.back():std::shared_ptr<Node>{};
    LayoutRect atomicCaret100{},atomicCaret150{},typedCaret100{},typedCaret150{};
    atomicCaretLayout.Layout(360,160,1.0f);
    const auto* atomicImage100=atomicCaretLayout.BoxFor(atomicImage);
    const bool hasAtomicImage100=atomicImage100!=nullptr;
    const float atomicImageRight100=atomicImage100?
        atomicImage100->rect.x+atomicImage100->rect.width:0.0f;
    const bool resolvedAtomic100=atomicCaretLayout.TextCaretRect(
        atomicLine,2,atomicCaret100);
    const bool resolvedTyped100=atomicCaretLayout.TextCaretRect(
        atomicTrailingText,0,typedCaret100);
    atomicCaretLayout.Layout(360,160,1.5f);
    const auto* atomicImage150=atomicCaretLayout.BoxFor(atomicImage);
    const bool resolvedAtomic150=atomicCaretLayout.TextCaretRect(
        atomicLine,2,atomicCaret150);
    const bool resolvedTyped150=atomicCaretLayout.TextCaretRect(
        atomicTrailingText,0,typedCaret150);
    Check(resolvedAtomic100&&resolvedAtomic150&&resolvedTyped100&&resolvedTyped150&&
          hasAtomicImage100&&atomicImage150&&
          std::abs(atomicCaret100.x-atomicImageRight100)<0.01f&&
          std::abs(atomicCaret150.x-(atomicImage150->rect.x+atomicImage150->rect.width))<0.01f&&
          std::abs(atomicCaret100.y-typedCaret100.y)<0.01f&&
          std::abs(atomicCaret150.y-typedCaret150.y)<0.01f&&
          std::abs(atomicCaret100.height-typedCaret100.height)<0.01f&&
          std::abs(atomicCaret150.height-typedCaret150.height)<0.01f&&
          std::abs(atomicCaret100.width-1.0f)<0.01f&&
          std::abs(atomicCaret150.width-(2.0f/3.0f))<0.01f,
          L"a DOM boundary after an atomic inline element matches the following text caret position with a one-physical-pixel caret at 100 and 150 percent DPI");

    Document emptyEditableDoc;
    Check(emptyEditableDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}article{display:block;width:240px;min-height:80px;padding:10px;font-size:16px;line-height:24px}article:empty::before{content:attr(aria-label)}</style>"
        L"<article id='empty-editor' contenteditable='true' aria-label='Edit here'></article>",
        &error),L"empty contenteditable fixture parses");
    const auto emptyEditable=emptyEditableDoc.GetElementById(L"empty-editor");
    auto emptyTextNode=std::make_shared<Node>();emptyTextNode->type=NodeType::Text;
    emptyTextNode->tag=L"#text";emptyTextNode->parent=emptyEditable;emptyEditable->children.push_back(emptyTextNode);
    emptyEditableDoc.Reindex();
    StyleSheet emptyEditableCss;
    Check(emptyEditableCss.Parse(emptyEditableDoc.StyleText(),&error),L"empty contenteditable CSS parses");
    LayoutEngine emptyEditableLayout(emptyEditableDoc,emptyEditableCss);emptyEditableLayout.Layout(280,140);
    LayoutRect emptyCaret{};const auto* emptyEditorBox=emptyEditableLayout.BoxFor(emptyEditable);
    Check(emptyEditorBox&&emptyEditableLayout.BoxFor(emptyTextNode)==nullptr&&
          emptyEditableLayout.TextCaretRect(emptyTextNode,0,emptyCaret)&&
          std::abs(emptyCaret.x-emptyEditorBox->content.x)<0.01f&&
          std::abs(emptyCaret.y-emptyEditorBox->content.y)<0.01f&&
          std::lround(emptyCaret.height)==24,
          L"an empty contenteditable paints a line-height caret at its content origin even while its placeholder pseudo-element is rendered");

    Document mediaDoc;
    Check(mediaDoc.Parse(L"<style>.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr))}@media (max-width:1180px){.grid{grid-template-columns:1fr}}</style><div class='grid'></div>",&error),
          L"media query fixture parses");
    StyleSheet mediaCss;Check(mediaCss.Parse(mediaDoc.StyleText(),&error),L"media query CSS parses");
    mediaCss.SetViewport(1320,700);Check(mediaCss.Compute(mediaDoc.QuerySelector(L".grid")).Get(L"grid-template-columns")==L"repeat(2,minmax(0,1fr))",
          L"max-width media rule stays inactive above its breakpoint");
    const auto wideMediaVersion=mediaCss.Version();mediaCss.SetViewport(1280,700);
    Check(mediaCss.Version()==wideMediaVersion,L"viewport changes within one media-query range preserve computed-style caches");
    mediaCss.SetViewport(1000,700);Check(mediaCss.Compute(mediaDoc.QuerySelector(L".grid")).Get(L"grid-template-columns")==L"1fr",
          L"max-width media rule participates in the normal cascade below its breakpoint");
    Check(mediaCss.Version()!=wideMediaVersion,L"crossing a media-query breakpoint invalidates computed-style caches");

    Document deviceMediaDoc;
    Check(deviceMediaDoc.Parse(
        L"<style>.panel{display:none;height:10px}"
        L"@media screen and (max-device-width:900px) and (min-device-height:600px){.panel{display:block;height:20px}}"
        L"</style><div class='panel'></div>",&error),L"device media query fixture parses");
    StyleSheet deviceMediaCss;
    Check(deviceMediaCss.Parse(deviceMediaDoc.StyleText(),&error),L"device media query CSS parses");
    deviceMediaCss.SetViewport(1200,720);
    Check(deviceMediaCss.Compute(deviceMediaDoc.QuerySelector(L".panel")).Get(L"display")==L"none",
          L"device-width media rules stay inactive above their CSS pixel boundary");
    deviceMediaCss.SetViewport(800,720);
    const auto narrowDeviceStyle=deviceMediaCss.Compute(deviceMediaDoc.QuerySelector(L".panel"));
    Check(narrowDeviceStyle.Get(L"display")==L"block"&&narrowDeviceStyle.Get(L"height")==L"20px",
          L"device media dimensions use the common CSS pixel viewport");
    deviceMediaCss.SetDisplay(1920,1080,1);
    Check(deviceMediaCss.Compute(deviceMediaDoc.QuerySelector(L".panel")).Get(L"display")==L"none",
          L"a narrow desktop viewport does not match tablet device-width rules");
    deviceMediaCss.SetDisplay(800,720,1.5f);
    Check(deviceMediaCss.Compute(deviceMediaDoc.QuerySelector(L".panel")).Get(L"display")==L"block",
          L"device media dimensions use CSS pixels independently of raster scaling");
    Check(StyleSheet::MediaQueryMatches(L"print, screen and (max-width:900px)",800,720,1920,1080,1.5)&&
          !StyleSheet::MediaQueryMatches(L"print",800,720,1920,1080,1.5)&&
          StyleSheet::MediaQueryMatches(L"(min-resolution:144dpi)",800,720,1920,1080,1.5)&&
          !StyleSheet::MediaQueryMatches(L"(-ms-high-contrast:none)",800,720,1920,1080,1.5),
          L"stylesheet media lists, media types, resolution and unsupported features share matchMedia semantics");

    Document relayoutDoc;
    Check(relayoutDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.panel{width:50vw;height:20px}@media (max-width:700px){.panel{width:25vw}}</style><div id='relayout-panel' class='panel'></div>",&error),
          L"viewport relayout fixture parses");
    StyleSheet relayoutCss;Check(relayoutCss.Parse(relayoutDoc.StyleText(),&error),L"viewport relayout CSS parses");
    LayoutEngine relayoutEngine(relayoutDoc,relayoutCss);relayoutEngine.Layout(1000,100);
    const auto* relayoutRoot=relayoutEngine.Root();const auto* initialRelayoutPanel=FindLayout(relayoutRoot,L"relayout-panel");
    relayoutEngine.Relayout(800,100);const auto* resizedRelayoutPanel=FindLayout(relayoutEngine.Root(),L"relayout-panel");
    Check(relayoutEngine.Root()==relayoutRoot&&initialRelayoutPanel==resizedRelayoutPanel&&
          resizedRelayoutPanel&&std::lround(resizedRelayoutPanel->rect.width)==400,
          L"viewport-only relayout reuses layout and text caches while resolving viewport units");
    relayoutEngine.Relayout(600,100);const auto* narrowRelayoutPanel=FindLayout(relayoutEngine.Root(),L"relayout-panel");
    Check(narrowRelayoutPanel&&std::lround(narrowRelayoutPanel->rect.width)==150,
          L"viewport relayout falls back to a full style rebuild at media-query breakpoints");

    Document viewportFontDoc;
    Check(viewportFontDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.title{font-size:clamp(20px,4vw,60px)}</style><div id='viewport-title' class='title'>Title</div>",&error),
          L"viewport font-size fixture parses");
    StyleSheet viewportFontCss;Check(viewportFontCss.Parse(viewportFontDoc.StyleText(),&error),L"viewport font-size CSS parses");
    LayoutEngine viewportFontLayout(viewportFontDoc,viewportFontCss);viewportFontLayout.Layout(1000,100);
    const auto* wideViewportTitle=FindLayout(viewportFontLayout.Root(),L"viewport-title");
    Check(wideViewportTitle&&std::abs(StyleSheet::Length(wideViewportTitle->style.Get(L"font-size"),16,16)-40)<0.01f,
          L"viewport-relative font size resolves against the CSS viewport");
    viewportFontLayout.Relayout(500,100);
    const auto* narrowViewportTitle=FindLayout(viewportFontLayout.Root(),L"viewport-title");
    Check(narrowViewportTitle&&std::abs(StyleSheet::Length(narrowViewportTitle->style.Get(L"font-size"),16,16)-20)<0.01f,
          L"viewport-relative font size recomputes after a DPI-independent viewport change");

    Document gridDoc;
    Check(gridDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.shell{width:700px;height:400px;display:grid;grid-template-rows:52px minmax(0,1fr) 28px}.named{width:1000px;height:80px;display:grid;grid-template-columns:minmax(100px,140px) minmax(100px,140px) minmax(180px,1fr) max-content minmax(180px,1fr);grid-template-areas:'first second fluid intrinsic last';column-gap:10px}.first{grid-area:first}.second{grid-area:second}.fluid{grid-area:fluid}.intrinsic{grid-area:intrinsic;width:70px}.last{grid-area:last}</style><div class='shell'><div id='grid-top'></div><div id='grid-main'></div><div id='grid-foot'></div></div><div class='named'><div id='grid-last' class='last'></div><div id='grid-first' class='first'></div><div id='grid-intrinsic' class='intrinsic'></div><div id='grid-fluid' class='fluid'></div><div id='grid-second' class='second'></div></div>",&error),
          L"explicit and named grid fixture parses");
    StyleSheet gridCss;Check(gridCss.Parse(gridDoc.StyleText(),&error),L"explicit and named grid CSS parses");
    LayoutEngine gridLayout(gridDoc,gridCss);gridLayout.Layout(1000,480);
    const auto* gridTop=FindLayout(gridLayout.Root(),L"grid-top");
    const auto* gridMain=FindLayout(gridLayout.Root(),L"grid-main");
    const auto* gridFoot=FindLayout(gridLayout.Root(),L"grid-foot");
    Check(gridTop&&gridMain&&gridFoot&&std::lround(gridTop->rect.height)==52&&
          std::lround(gridMain->rect.height)==320&&std::lround(gridMain->rect.y)==52&&
          std::lround(gridFoot->rect.height)==28&&std::lround(gridFoot->rect.y)==372,
          L"grid-template-rows preserves fixed tracks and assigns the remainder to fr");
    const auto* gridFirst=FindLayout(gridLayout.Root(),L"grid-first");
    const auto* gridSecond=FindLayout(gridLayout.Root(),L"grid-second");
    const auto* gridFluid=FindLayout(gridLayout.Root(),L"grid-fluid");
    const auto* gridIntrinsic=FindLayout(gridLayout.Root(),L"grid-intrinsic");
    const auto* gridLast=FindLayout(gridLayout.Root(),L"grid-last");
    Check(gridFirst&&gridSecond&&gridFluid&&gridIntrinsic&&gridLast&&
          gridFirst->rect.x<gridSecond->rect.x&&gridSecond->rect.x<gridFluid->rect.x&&
          gridFluid->rect.x<gridIntrinsic->rect.x&&gridIntrinsic->rect.x<gridLast->rect.x&&
          std::lround(gridFirst->rect.width)==140&&std::lround(gridSecond->rect.width)==140&&
          std::lround(gridIntrinsic->rect.width)==70,
          L"grid-template-areas places reordered children and resolves capped/content/fr tracks");

    Document gridButtonDoc;
    Check(gridButtonDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}button{font:inherit}.list{display:grid;width:620px;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}.item{display:grid;grid-template-columns:30px 1fr;padding:10px 11px;border:1px solid transparent}.copy strong,.copy small{display:block}.copy strong{font-size:12px}.copy small{margin-top:3px;font-size:10px}</style>"
          L"<div class='list'><button id='grid-button' class='item'><span>icon</span><span class='copy'><strong id='grid-button-title'>name</strong><small id='grid-button-detail'>path</small></span></button></div>",&error),
          L"grid button intrinsic-size fixture parses");
    StyleSheet gridButtonCss;Check(gridButtonCss.Parse(gridButtonDoc.StyleText(),&error),L"grid button intrinsic-size CSS parses");
    LayoutEngine gridButtonLayout(gridButtonDoc,gridButtonCss);gridButtonLayout.Layout(700,200);
    const auto* gridButton=FindLayout(gridButtonLayout.Root(),L"grid-button");
    const auto* gridButtonTitle=FindLayout(gridButtonLayout.Root(),L"grid-button-title");
    const auto* gridButtonDetail=FindLayout(gridButtonLayout.Root(),L"grid-button-detail");
    Check(gridButton&&gridButtonTitle&&gridButtonDetail&&std::lround(gridButton->rect.height)==54&&
          gridButtonTitle->rect.y+gridButtonTitle->rect.height<=gridButtonDetail->rect.y+0.01f,
          L"grid buttons size from blockified inline items and preserve their vertical content flow");

    Document gridAutoContentDoc;
    Check(gridAutoContentDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.meta{display:grid;width:520px;grid-template-columns:1fr auto;column-gap:12px;font-family:'Segoe UI';font-size:11px;line-height:14px}.meta small{color:#777}</style>"
          L"<div class='meta'><span><small>Author</small><br>DESKTOP-INVEG4R\\tklee &lt;tklee@example.com&gt;</span><span id='auto-content-time'><small>Time</small><br>8\uC6D4 7\uC77C \uC624\uD6C4 12:17</span></div>",&error),
          L"grid auto max-content fixture parses");
    StyleSheet gridAutoContentCss;
    Check(gridAutoContentCss.Parse(gridAutoContentDoc.StyleText(),&error),
          L"grid auto max-content CSS parses");
    LayoutEngine gridAutoContentLayout(gridAutoContentDoc,gridAutoContentCss);
    gridAutoContentLayout.Layout(560,120,1.0f);
    const auto* gridAutoTime100=FindLayout(gridAutoContentLayout.Root(),L"auto-content-time");
    const float gridAutoWidth100=gridAutoTime100?gridAutoTime100->rect.width:0;
    const float gridAutoHeight100=gridAutoTime100?gridAutoTime100->rect.height:0;
    gridAutoContentLayout.Layout(560,120,1.5f);
    const auto* gridAutoTime150=FindLayout(gridAutoContentLayout.Root(),L"auto-content-time");
    Check(gridAutoTime100&&gridAutoTime150&&gridAutoWidth100>=80&&gridAutoHeight100<=30&&
          gridAutoTime150->rect.width>=80&&gridAutoTime150->rect.height<=30&&
          std::abs(gridAutoTime150->rect.width-gridAutoWidth100)<0.1f,
          L"an auto grid track next to an fr track keeps fitting inline metadata on one line at 100 and 150 percent DPI");

    Document intrinsicDoc;
    Check(intrinsicDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.card{width:300px;display:grid;padding:10px;border:1px solid black}.field{display:grid;gap:5px}.label{height:16px}.control{position:relative}.editor{display:block;width:100%;height:32px}.floating{position:absolute;right:0;top:50%;height:12px}.hint{height:12px}</style><div id='intrinsic-card' class='card'><div class='field'><span class='label'>Label</span><div class='control'><input class='editor'><span class='floating'>v</span></div><span class='hint'>Hint</span></div></div>",&error),
          L"intrinsic positioned-child fixture parses");
    StyleSheet intrinsicCss;Check(intrinsicCss.Parse(intrinsicDoc.StyleText(),&error),L"intrinsic positioned-child CSS parses");
    LayoutEngine intrinsicLayout(intrinsicDoc,intrinsicCss);intrinsicLayout.Layout(400,200);
    const auto* intrinsicCard=FindLayout(intrinsicLayout.Root(),L"intrinsic-card");
    const auto* intrinsicEditor=intrinsicLayout.BoxFor(intrinsicDoc.QuerySelector(L".editor"));
    Check(intrinsicCard&&intrinsicEditor&&std::lround(intrinsicCard->rect.height)==92&&
          std::lround(intrinsicEditor->rect.width)==278,
          L"absolute descendants do not increase intrinsic height and percentage controls resolve against their containing block");

    Document absolutePaddingDoc;
    Check(absolutePaddingDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.host{position:relative;width:100px;height:60px;padding:10px 15px;border:2px solid black}.flex{display:flex}.grid{display:grid}.fill{position:absolute;inset:0}</style>"
          L"<div class='host flex'><span id='absolute-flex' class='fill'></span></div><div class='host grid'><span id='absolute-grid' class='fill'></span></div><div class='host'><span id='absolute-block' class='fill'></span></div>",&error),
          L"absolute padding-box fixture parses");
    StyleSheet absolutePaddingCss;Check(absolutePaddingCss.Parse(absolutePaddingDoc.StyleText(),&error),L"absolute padding-box CSS parses");
    LayoutEngine absolutePaddingLayout(absolutePaddingDoc,absolutePaddingCss);absolutePaddingLayout.Layout(400,240);
    const auto* absoluteFlex=FindLayout(absolutePaddingLayout.Root(),L"absolute-flex");
    const auto* absoluteGrid=FindLayout(absolutePaddingLayout.Root(),L"absolute-grid");
    const auto* absoluteBlock=FindLayout(absolutePaddingLayout.Root(),L"absolute-block");
    Check(absoluteFlex&&absoluteGrid&&absoluteBlock&&
          std::lround(absoluteFlex->rect.x)==2&&std::lround(absoluteFlex->rect.width)==96&&
          std::lround(absoluteGrid->rect.x)==2&&std::lround(absoluteGrid->rect.width)==96&&
          std::lround(absoluteBlock->rect.x)==2&&std::lround(absoluteBlock->rect.width)==96,
          L"absolute insets resolve against the padding box in flex, grid, and block containers");

    Document selectDoc;
    Check(selectDoc.Parse(L"<html><head><style>:root{color-scheme:light}*{box-sizing:border-box;margin:0;padding:0}select{width:140px;height:32px;background:#f0f4ff;color:#182131}</style></head><body><select id='choice' value='b'><option value='a'>Alpha</option><option value='b'>Beta</option><option value='c'>Gamma</option></select><datalist id='suggestions'><option value='one'></option></datalist></body></html>",&error),
          L"select control fixture parses");
    StyleSheet selectCss;Check(selectCss.Parse(selectDoc.StyleText(),&error),L"select control CSS parses");
    LayoutEngine selectLayout(selectDoc,selectCss);selectLayout.Layout(300,100);
    const auto* selectBox=FindLayout(selectLayout.Root(),L"choice");
    const auto* dataListBox=FindLayout(selectLayout.Root(),L"suggestions");
    Check(selectBox&&std::lround(selectBox->rect.width)==140&&std::lround(selectBox->rect.height)==32&&
          selectBox->style.Get(L"color-scheme")==L"light"&&dataListBox&&!dataListBox->visible,
          L"select and datalist are atomic controls instead of laying out their option children");
    const auto selectPalette=selectBox?ResolveSelectPopupPalette(selectBox->style,
        StyleSheet::Color(selectBox->style.Get(L"background-color"),0)):SelectPopupPalette{};
    Check(selectPalette.background==0xfff0f4ff&&selectPalette.border==0xffd8d8d8&&
          selectPalette.selectedBackground==0xff767676&&selectPalette.color==0xff182131&&
          selectPalette.selectedColor==0xffffffff,
          L"select popup inherits its computed control surface and common UA selection colors");
    ComputedStyle darkSelectStyle;if(selectBox)*darkSelectStyle.values=*selectBox->style.values;
    (*darkSelectStyle.values)[L"color-scheme"]=L"dark";
    const auto darkSelectPalette=ResolveSelectPopupPalette(darkSelectStyle,0xff242424);
    Check(darkSelectPalette.background==0xff242424&&darkSelectPalette.border==0xffa8a8a8&&
          darkSelectPalette.selectedBackground==0xffc6c6c6&&darkSelectPalette.selectedColor==0xff1b1b1b,
          L"select popup defaults follow the inherited dark color scheme without per-control CSS");

    Document percentGridDoc;
    Check(percentGridDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.cell{width:140px;display:grid}.control{width:100%;height:32px}</style><div class='cell'><select id='percent-grid-control' class='control'><option>Choice</option></select></div>",&error),
          L"percentage grid-control fixture parses");
    StyleSheet percentGridCss;Check(percentGridCss.Parse(percentGridDoc.StyleText(),&error),L"percentage grid-control CSS parses");
    LayoutEngine percentGridLayout(percentGridDoc,percentGridCss);percentGridLayout.Layout(300,100);
    const auto* percentGridControl=FindLayout(percentGridLayout.Root(),L"percent-grid-control");
    Check(percentGridControl&&std::lround(percentGridControl->rect.width)==140,
          L"percentage-sized grid controls resolve against the track instead of growing it to their fallback intrinsic width");

    HWND selectHost=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
        -10000,-10000,300,100,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(selectHost!=nullptr,L"select interaction host is created");
    if(selectHost){
        RECT selectBounds{0,0,300,100};auto selectView=TWebFrame::View::Create(selectHost,selectBounds);
        Check(selectView!=nullptr,L"select interaction TWebFrame view is created");
        if(selectView){
            std::vector<std::wstring> selectEvents;
            selectView->SetMessageHandler([&](const std::wstring& message){selectEvents.push_back(message);});
            auto clickControlCenter=[&](const wchar_t* id){
                std::wstring center,centerError;
                const auto script=L"const r=document.getElementById('"+std::wstring(id)+
                    L"').getBoundingClientRect();return Math.round(r.x+r.width/2)+','+"
                    L"Math.round(r.y+r.height/2);";
                Check(selectView->ExecuteScript(script,&center,&centerError),centerError.c_str());
                const auto comma=center.find(L',');
                if(comma==std::wstring::npos)return;
                SendMessageW(selectView->Window(),WM_LBUTTONDOWN,MK_LBUTTON,
                    MAKELPARAM(std::stoi(center.substr(0,comma)),std::stoi(center.substr(comma+1))));
            };
            Check(selectView->NavigateToString(
                L"<style>*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%}select{width:140px;height:32px;background:#f0f4ff;color:#182131}</style>"
                L"<select id='native-choice' value='a'><option value='a'>Alpha</option><option value='b'>Beta</option><option value='c'>Gamma</option></select>"
                L"<script>const choice=document.getElementById('native-choice');choice.addEventListener('input',()=>{window.chrome.webview.postMessage('input:'+choice.value);});choice.addEventListener('change',()=>{window.chrome.webview.postMessage('change:'+choice.value);});</script>"),
                selectView->LastError().c_str());
            clickControlCenter(L"native-choice");
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_DOWN,0);
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_RETURN,0);
            std::wstring selectionResult,selectionError;
            Check(selectView->ExecuteScript(
                L"return document.getElementById('native-choice').value;",
                &selectionResult,&selectionError),selectionError.c_str());
            Check(selectionResult==L"b"&&selectEvents.size()==2&&
                  selectEvents[0]==L"input:b"&&selectEvents[1]==L"change:b",
                  L"select keyboard selection updates value and dispatches input and change events");

            selectEvents.clear();
            Check(selectView->NavigateToString(
                L"<style>*{box-sizing:border-box}html,body{margin:0;width:100%;height:100%}body{padding-top:36px}input{width:180px;height:32px;background:#f0f4ff;color:#182131}</style>"
                L"<input id='editable-choice' list='editable-options' placeholder='Choose or type'><datalist id='editable-options'>"
                L"<option value='10M'></option><option value='100M'></option><option value='650M' label='CD'></option></datalist>"
                L"<script>const choice=document.getElementById('editable-choice');choice.addEventListener('input',()=>{window.chrome.webview.postMessage('input:'+choice.value);});choice.addEventListener('change',()=>{window.chrome.webview.postMessage('change:'+choice.value);});</script>"),
                selectView->LastError().c_str());
            clickControlCenter(L"editable-choice");
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_DOWN,0);
            SendMessageW(selectView->Window(),WM_KEYDOWN,VK_RETURN,0);
            selectionResult.clear();selectionError.clear();
            Check(selectView->ExecuteScript(
                L"return document.getElementById('editable-choice').value;",
                &selectionResult,&selectionError),selectionError.c_str());
            Check(GetWindow(selectView->Window(),GW_CHILD)==nullptr,
                  L"input list uses the TWebFrame input path without a child edit control");
            Check(selectionResult==L"10M",(L"input list commits the chosen datalist option; actual value: "+selectionResult).c_str());
            Check(selectEvents.size()==2&&selectEvents[0]==L"input:10M"&&selectEvents[1]==L"change:10M",
                  L"input list selection dispatches one input event and one change event");
            selectEvents.clear();SendMessageW(selectView->Window(),WM_CHAR,L'G',0);
            selectionResult.clear();selectionError.clear();
            Check(selectView->ExecuteScript(
                L"return document.getElementById('editable-choice').value;",
                &selectionResult,&selectionError),selectionError.c_str());
            Check(selectionResult==L"10MG"&&selectEvents.size()==1&&selectEvents[0]==L"input:10MG",
                  L"input list remains directly editable after choosing a suggestion");
            const auto editableAccessibility=selectView->DumpAccessibilityJson();
            Check(editableAccessibility.find(L"\"automationId\":\"editable-choice\"")!=std::wstring::npos&&
                  editableAccessibility.find(L"\"controlType\":"+std::to_wstring(UIA_ComboBoxControlTypeId))!=std::wstring::npos,
                  L"input list exposes editable-combobox accessibility semantics");
        }
        selectView.reset();DestroyWindow(selectHost);
    }

    HWND accessibilityHost=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
        -10000,-10000,640,360,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(accessibilityHost!=nullptr,L"accessibility interaction host is created");
    if(accessibilityHost){
        RECT bounds{0,0,640,360};auto view=TWebFrame::View::Create(accessibilityHost,bounds);
        Check(view!=nullptr,L"accessibility TWebFrame view is created");
        if(view){
            Check(view->NavigateToString(
                L"<style>*{box-sizing:border-box}body{margin:0}button,input,select,[role=menuitem]{display:block;width:180px;height:28px}.menu-closed{display:none}</style>"
                L"<button id='pointer-trigger'>Pointer trigger</button><button id='pointer-target'>Pointer target</button><button id='keyboard-target'>Keyboard target</button>"
                L"<button id='plain'>Plain</button><button id='tab-two' tabindex='2'>Two</button>"
                L"<button id='tab-one' tabindex='1' data-automation-id='primary-action' aria-label='Primary action' aria-expanded='false'>One</button>"
                L"<input id='value-field' aria-label='Value field' value='start'><input id='toggle-field' type='checkbox' aria-label='Toggle field'>"
                L"<button id='disabled-action' disabled aria-label='Disabled action'>Disabled</button>"
                L"<select id='selection' aria-label='Selection'><option id='option-a' value='a'>Alpha</option><option id='option-b' value='b'>Beta</option></select>"
                L"<div><button id='menu-trigger' aria-controls='command-menu'>Commands</button><div id='command-menu' class='menu-closed' role='menu' aria-label='Commands'><button role='menuitem' id='menu-one'>Menu one</button><button role='menuitem' id='menu-two'>Menu two</button></div></div>"
                L"<main id='content-region'><section><span>Section text</span></section></main>"
                L"<div id='live' role='status' aria-live='polite'>Ready</div>"
                L"<script>let accessibilityLog=[];document.addEventListener('focusin',(e)=>accessibilityLog.push('in:'+e.target.id));document.addEventListener('focusout',(e)=>accessibilityLog.push('out:'+e.target.id));document.addEventListener('click',(e)=>accessibilityLog.push('click:'+e.target.id));document.getElementById('menu-trigger').addEventListener('click',()=>document.getElementById('command-menu').classList.remove('menu-closed'));document.getElementById('pointer-trigger').addEventListener('click',()=>document.getElementById('pointer-target').focus());document.getElementById('pointer-target').addEventListener('keydown',(event)=>{if(event.key==='ArrowDown')document.getElementById('keyboard-target').focus();});</script>"),
                view->LastError().c_str());
            std::wstring result,scriptError;
            SendMessageW(view->Window(),WM_KEYDOWN,VK_TAB,0);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"tab-one",
                  L"Tab follows positive tabindex ordering and marks keyboard focus visible");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_TAB,0);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"tab-two",
                  L"Tab advances through equal focus groups in document order");
            BYTE oldKeyboard[256]{},shiftKeyboard[256]{};GetKeyboardState(oldKeyboard);std::copy(std::begin(oldKeyboard),std::end(oldKeyboard),std::begin(shiftKeyboard));shiftKeyboard[VK_SHIFT]=0x80;SetKeyboardState(shiftKeyboard);
            SendMessageW(view->Window(),WM_KEYDOWN,VK_TAB,0);SetKeyboardState(oldKeyboard);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"tab-one",
                  L"Shift+Tab reverses sequential focus order");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_RETURN,0);
            Check(view->ExecuteScript(L"return accessibilityLog.join(',');",&result,&scriptError)&&
                  result.find(L"in:tab-one")!=std::wstring::npos&&result.find(L"out:tab-one")!=std::wstring::npos&&
                  result.find(L"click:tab-one")!=std::wstring::npos,
                  L"focusin/focusout bubble and Enter activates a focused button");
            Check(view->ExecuteScript(L"document.getElementById('menu-trigger').focus();",nullptr,&scriptError),scriptError.c_str());
            SendMessageW(view->Window(),WM_KEYDOWN,VK_DOWN,0);
            Check(view->ExecuteScript(L"return document.activeElement.id;",&result,&scriptError)&&result==L"menu-one",
                  L"ArrowDown opens an associated menu and focuses its first item");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_DOWN,0);
            Check(view->ExecuteScript(L"return document.querySelector(':focus-visible').id;",&result,&scriptError)&&result==L"menu-two",
                  L"menu arrow navigation moves focus among menuitems");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_SPACE,0);
            Check(view->ExecuteScript(L"return accessibilityLog[accessibilityLog.length-1];",&result,&scriptError)&&result==L"click:menu-two",
                  L"Space activates a focused menuitem");
            SendMessageW(view->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,10));
            SendMessageW(view->Window(),WM_LBUTTONUP,0,MAKELPARAM(10,10));
            Check(view->ExecuteScript(
                  L"return document.activeElement.id+'|'+(document.querySelector(':focus-visible')?.id||'none');",
                  &result,&scriptError)&&result==L"pointer-target|none",
                  L"programmatic focus moved by a pointer handler does not match focus-visible");
            SendMessageW(view->Window(),WM_KEYDOWN,VK_DOWN,0);
            Check(view->ExecuteScript(
                  L"return document.activeElement.id+'|'+document.querySelector(':focus-visible').id;",
                  &result,&scriptError)&&result==L"keyboard-target|keyboard-target",
                  L"programmatic focus moved by a keyboard handler remains focus-visible");

            const auto accessibilityJson=view->DumpAccessibilityJson();
            Check(accessibilityJson.find(L"\"automationId\":\"primary-action\"")!=std::wstring::npos&&
                  accessibilityJson.find(L"\"automationId\":\"value-field\"")!=std::wstring::npos&&
                  accessibilityJson.find(L"\"automationId\":\"option-b\"")!=std::wstring::npos&&
                  accessibilityJson.find(L"\"name\":\"Primary action\"")!=std::wstring::npos,
                  L"accessibility diagnostics expose stable explicit automation ids and accessible names");
            Check(accessibilityJson.find(L"\"automationId\":\"content-region\",\"name\":\"\"")!=std::wstring::npos,
                  L"structural accessibility containers do not duplicate all descendant text in their names");

            using Microsoft::WRL::ComPtr;
            ComPtr<IUIAutomation> automation;
            HRESULT automationResult=CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,
                                                       IID_PPV_ARGS(automation.ReleaseAndGetAddressOf()));
            Check(SUCCEEDED(automationResult),L"UI Automation client is available");
            if(SUCCEEDED(automationResult)){
                ComPtr<IUIAutomationElement> root;
                automationResult=automation->ElementFromHandle(view->Window(),root.ReleaseAndGetAddressOf());
                Check(SUCCEEDED(automationResult)&&root,L"WM_GETOBJECT returns the TWebFrame UI Automation fragment root");
                auto findById=[&](const wchar_t* id){
                    ComPtr<IUIAutomationElement> element;if(!root)return element;
                    VARIANT value{};value.vt=VT_BSTR;value.bstrVal=SysAllocString(id);
                    ComPtr<IUIAutomationCondition> condition;
                    if(SUCCEEDED(automation->CreatePropertyCondition(UIA_AutomationIdPropertyId,value,condition.ReleaseAndGetAddressOf())))
                        root->FindFirst(TreeScope_Descendants,condition.Get(),element.ReleaseAndGetAddressOf());
                    VariantClear(&value);return element;
                };
                auto primary=findById(L"primary-action");Check(primary!=nullptr,L"UIA tree finds an element by stable AutomationId");
                if(primary){
                    BSTR name=nullptr;primary->get_CurrentName(&name);Check(name&&std::wstring(name)==L"Primary action",L"UIA exposes aria-label as Name");SysFreeString(name);
                    RECT rectangle{};Check(SUCCEEDED(primary->get_CurrentBoundingRectangle(&rectangle))&&rectangle.right>rectangle.left&&rectangle.bottom>rectangle.top,
                        L"UIA fragment exposes non-empty screen bounds");
                    ComPtr<IUIAutomationInvokePattern> invokePattern;
                    Check(SUCCEEDED(primary->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(invokePattern.ReleaseAndGetAddressOf())))&&invokePattern,
                        L"button exposes the Invoke pattern");
                    if(invokePattern)invokePattern->Invoke();
                    ComPtr<IUIAutomationExpandCollapsePattern> expandPattern;
                    Check(SUCCEEDED(primary->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,IID_PPV_ARGS(expandPattern.ReleaseAndGetAddressOf())))&&expandPattern,
                        L"aria-expanded exposes the ExpandCollapse pattern");
                    if(expandPattern){expandPattern->Expand();ExpandCollapseState state=ExpandCollapseState_Collapsed;expandPattern->get_CurrentExpandCollapseState(&state);Check(state==ExpandCollapseState_Expanded,L"expanded state is exposed through UIA");}
                }
                auto valueField=findById(L"value-field");
                if(valueField){ComPtr<IUIAutomationValuePattern> valuePattern;Check(SUCCEEDED(valueField->GetCurrentPatternAs(UIA_ValuePatternId,IID_PPV_ARGS(valuePattern.ReleaseAndGetAddressOf())))&&valuePattern,L"text input exposes the Value pattern");if(valuePattern){BSTR updated=SysAllocString(L"updated");valuePattern->SetValue(updated);SysFreeString(updated);}}
                auto toggleField=findById(L"toggle-field");
                if(toggleField){ComPtr<IUIAutomationTogglePattern> togglePattern;Check(SUCCEEDED(toggleField->GetCurrentPatternAs(UIA_TogglePatternId,IID_PPV_ARGS(togglePattern.ReleaseAndGetAddressOf())))&&togglePattern,L"checkbox exposes the Toggle pattern");if(togglePattern){togglePattern->Toggle();ToggleState state=ToggleState_Off;togglePattern->get_CurrentToggleState(&state);Check(state==ToggleState_On,L"checked state is exposed through UIA");}}
                auto optionB=findById(L"option-b");Check(optionB!=nullptr,L"UIA tree exposes native select options");
                if(optionB){ComPtr<IUIAutomationSelectionItemPattern> selectionItem;Check(SUCCEEDED(optionB->GetCurrentPatternAs(UIA_SelectionItemPatternId,IID_PPV_ARGS(selectionItem.ReleaseAndGetAddressOf())))&&selectionItem,L"option exposes the SelectionItem pattern");if(selectionItem)selectionItem->Select();}
                const bool actionsUpdated=view->ExecuteScript(L"return document.getElementById('value-field').value+'|'+document.getElementById('toggle-field').checked+'|'+document.getElementById('selection').value+'|'+document.getElementById('tab-one').getAttribute('aria-expanded');",&result,&scriptError)&&result==L"updated|true|b|true";
                if(!actionsUpdated)std::wcerr<<L"UIA action state: "<<result<<L"\n";
                Check(actionsUpdated,
                      L"UIA Invoke/Value/Toggle/Selection/ExpandCollapse actions update the DOM");
                auto disabled=findById(L"disabled-action");if(disabled){BOOL enabled=TRUE;disabled->get_CurrentIsEnabled(&enabled);Check(!enabled,L"disabled state is exposed through UIA");}
                auto menuItem=findById(L"menu-one");if(menuItem){VARIANT roleValue{};menuItem->GetCurrentPropertyValue(UIA_AriaRolePropertyId,&roleValue);Check(roleValue.vt==VT_BSTR&&std::wstring(roleValue.bstrVal)==L"menuitem",L"HTML role is exposed through UIA");VariantClear(&roleValue);}
                auto live=findById(L"live");if(live){VARIANT liveValue{};live->GetCurrentPropertyValue(UIA_LiveSettingPropertyId,&liveValue);Check(liveValue.vt==VT_I4&&liveValue.lVal==Polite,L"aria-live politeness is exposed through UIA");VariantClear(&liveValue);}
                auto selection=findById(L"selection");if(selection){ComPtr<IUIAutomationSelectionPattern> selectionPattern;Check(SUCCEEDED(selection->GetCurrentPatternAs(UIA_SelectionPatternId,IID_PPV_ARGS(selectionPattern.ReleaseAndGetAddressOf())))&&selectionPattern,L"select exposes the Selection pattern");if(selectionPattern){ComPtr<IUIAutomationElementArray> selected;selectionPattern->GetCurrentSelection(selected.ReleaseAndGetAddressOf());int length=0;if(selected)selected->get_Length(&length);Check(length==1,L"Selection pattern returns the selected option");}}
            }
        }
        view.reset();DestroyWindow(accessibilityHost);
    }

    const wchar_t resizeHostClass[]=L"TWebFrame.Tests.ResizeHost";
    WNDCLASSW resizeClass{};resizeClass.lpfnWndProc=ResizeHostWindowProc;
    resizeClass.hInstance=GetModuleHandleW(nullptr);resizeClass.lpszClassName=resizeHostClass;
    RegisterClassW(&resizeClass);
    HWND resizeHost=CreateWindowExW(WS_EX_TOOLWINDOW,resizeHostClass,L"",WS_POPUP|WS_THICKFRAME,
        -10000,-10000,400,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(resizeHost!=nullptr,L"resize forwarding host is created");
    if(resizeHost){
        RECT bounds{0,0,400,240};auto resizeView=TWebFrame::View::Create(resizeHost,bounds);
        Check(resizeView!=nullptr,L"resize forwarding TWebFrame view is created");
        if(resizeView){
            Check(resizeView->NavigateToString(
                L"<style>*{box-sizing:border-box;margin:0}.surface{width:100vw;height:100vh;background:#183153;color:white}</style><div class='surface'>Buffered resize</div>"),
                L"back-buffer paint fixture loads");
            for(int width=400;width<=720;width+=16){
                RECT frameBounds{0,0,width,240+(width-400)/4};resizeView->SetBounds(frameBounds);
                RedrawWindow(resizeView->Window(),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE);
            }
            Check(IsWindow(resizeView->Window()),L"off-screen back-buffer painting survives repeated live-size frames");
            const LRESULT resizeHits[]={HTLEFT,HTRIGHT,HTTOP,HTBOTTOM,HTTOPLEFT,HTTOPRIGHT,HTBOTTOMLEFT,HTBOTTOMRIGHT};
            bool allForwarded=true;
            for(const auto hit:resizeHits){
                resizeHostHit=hit;resizeHostButtonDown=0;
                SendMessageW(resizeView->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,2));
                MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                allForwarded=allForwarded&&resizeHostButtonDown==static_cast<WPARAM>(hit);
            }
            Check(allForwarded,L"TWebFrame forwards all four edges and four corner resize hits to its parent window");
            resizeHostHit=HTRIGHT;SendMessageW(resizeView->Window(),WM_SETCURSOR,
                reinterpret_cast<WPARAM>(resizeView->Window()),MAKELPARAM(HTCLIENT,WM_MOUSEMOVE));
            Check(GetCursor()==LoadCursorW(nullptr,IDC_SIZEWE),L"parent resize border selects the matching system sizing cursor");
            resizeHostHit=HTCLIENT;
            Check(resizeView->NavigateToString(
                L"<style>*{box-sizing:border-box;margin:0;padding:0}.row{display:flex;width:200px;height:40px;background:#111}.row:hover{width:220px;background:#345}.row span{display:block;width:100px;height:40px}</style>"
                L"<button id='hover-row' class='row'><span id='hover-first'><strong>Workspace</strong></span><span id='hover-second'><small>Recent</small></span></button>"),
                L"ancestor hover fixture loads");
            std::wstring hoverResult,hoverError;
            SendMessageW(resizeView->Window(),WM_MOUSEMOVE,0,MAKELPARAM(20,20));
            Check(resizeView->ExecuteScript(
                L"let row=document.querySelector('#hover-row:hover');return row.id+'|'+document.querySelector('#hover-first:hover').id+'|'+String(row.getBoundingClientRect().width);",
                &hoverResult,&hoverError)&&hoverResult==L"hover-row|hover-first|220",
                L"hover applies its style to a nested hit target and all of its ancestors");
            SendMessageW(resizeView->Window(),WM_MOUSEMOVE,0,MAKELPARAM(120,20));
            Check(resizeView->ExecuteScript(
                L"return document.querySelector('#hover-row:hover').id+'|'+document.querySelector('#hover-second:hover').id;",
                &hoverResult,&hoverError)&&hoverResult==L"hover-row|hover-second",
                L"moving between child elements preserves the parent hover state");
            SendMessageW(resizeView->Window(),WM_MOUSELEAVE,0,0);
            Check(resizeView->ExecuteScript(
                L"return document.querySelector('#hover-row:hover')===null&&document.getElementById('hover-row').getBoundingClientRect().width===200;",
                &hoverResult,&hoverError)&&hoverResult==L"true",
                L"leaving the view clears hover styles from the complete ancestor path");
            Check(resizeView->NavigateToString(LR"HTML(
                <style>
                    *{box-sizing:border-box;margin:0;padding:0}
                    .menu-bar{position:relative;width:320px;height:38px;overflow:visible}
                    .menu-entry{position:relative;width:100px;height:38px}
                    .menu-entry>a{display:block;width:100%;height:38px}
                    .dropdown-panel{display:none;position:absolute;left:0;top:38px;width:220px;height:90px;z-index:20;background:#fff}
                    .menu-entry:hover .dropdown-panel{display:block}
                    .dropdown-panel ul{display:block;width:100px}
                    .dropdown-panel li{display:block;height:24px}
                </style>
                <nav class="menu-bar"><div class="menu-entry"><a href="#">Menu</a><div class="dropdown-panel"></div></div></nav>
                <script>
                    initializeMenu(function () { populateMenu(); });
                    function initializeMenu(callback) {
                        if (document.readyState !== 'loading') callback();
                        else document.addEventListener('DOMContentLoaded', callback);
                    }
                    function populateMenu() {
                        document.querySelector('.dropdown-panel').innerHTML = '<ul><li>First</li><li>Second</li></ul>';
                    }
                </script>
            )HTML"),L"script-populated hover popup fixture loads");
            Check(resizeView->ExecuteScript(
                L"return document.querySelectorAll('.dropdown-panel li').length;",
                &hoverResult,&hoverError)&&hoverResult==L"2",
                L"a ready callback can call later function declarations and populate a generic menu");
            const float popupDpiScale=static_cast<float>(GetDpiForWindow(resizeView->Window()))/
                                      static_cast<float>(USER_DEFAULT_SCREEN_DPI);
            const auto popupPhysical=[popupDpiScale](float dip){return static_cast<int>(std::lround(dip*popupDpiScale));};
            SendMessageW(resizeView->Window(),WM_MOUSEMOVE,0,
                MAKELPARAM(popupPhysical(40),popupPhysical(19)));
            Check(resizeView->ExecuteScript(
                L"const popup=document.querySelector('.dropdown-panel');const bounds=popup.getBoundingClientRect();return getComputedStyle(popup).display==='block'&&Math.round(bounds.top)===38&&Math.round(bounds.width)===220;",
                &hoverResult,&hoverError)&&hoverResult==L"true",
                L"ancestor hover reveals a script-populated absolute popup at CSS-pixel geometry");
            SendMessageW(resizeView->Window(),WM_MOUSEMOVE,0,
                MAKELPARAM(popupPhysical(40),popupPhysical(60)));
            Check(resizeView->ExecuteScript(
                L"return document.querySelector('.menu-entry:hover')!==null&&getComputedStyle(document.querySelector('.dropdown-panel')).display==='block';",
                &hoverResult,&hoverError)&&hoverResult==L"true",
                L"moving into an overflow-visible popup preserves its ancestor hover chain at monitor DPI");
            SendMessageW(resizeView->Window(),WM_MOUSELEAVE,0,0);
            Check(resizeView->ExecuteScript(
                L"return getComputedStyle(document.querySelector('.dropdown-panel')).display;",
                &hoverResult,&hoverError)&&hoverResult==L"none",
                L"leaving a hover popup restores the common display-none rule");
            resizeView->SetResourceLoader([](const std::wstring& resource,std::wstring& body){
                if(resource.find(L"unavailable-analytics.js")!=std::wstring::npos)return false;
                if(resource.find(L"invalid-widget.js")!=std::wstring::npos){body=L"function broken( {";return true;}
                if(resource.find(L"navigation-menu.js")!=std::wstring::npos){
                    body=LR"JS(
                        onReady(function () { populateNavigation(); });
                        function onReady(callback) { callback(); }
                        function populateNavigation() {
                            var configs = [['menu-panel', buildNavigation]];
                            for (var i = 0; i < configs.length; i++) {
                                var panel = document.getElementById(configs[i][0]);
                                var builder = configs[i][1];
                                if (panel && !panel.querySelector('ul') && typeof builder === 'function')
                                    panel.innerHTML = builder();
                            }
                        }
                        function buildNavigation() { return '<ul><li>First</li><li>Last</li></ul>'; }
                    )JS";
                    return true;
                }
                return false;
            });
            Check(resizeView->NavigateToString(LR"HTML(
                <div id="menu-panel"></div>
                <script async src="unavailable-analytics.js"></script>
                <script src="invalid-widget.js"></script>
                <script type="application/ld+json">{"not":"javascript"}</script>
                <script defer src="navigation-menu.js"></script>
                <iframe src="unavailable-ad-frame.html"></iframe>
            )HTML",L"https://scripts.test/page.html"),
                L"independent scripts and frames tolerate unrelated resource and syntax failures");
            Check(resizeView->ExecuteScript(
                L"return document.querySelectorAll('#menu-panel li').length;",
                &hoverResult,&hoverError)&&hoverResult==L"2",
                L"a later DOM-populating script still runs and hoists its own declarations");
            resizeView->SetResourceLoader({});
            Check(resizeView->NavigateToString(
                L"<style>*{box-sizing:border-box;margin:0}body{background:#1b241e}.field{display:flex;width:200px;height:34px;background:rgba(0,0,0,.13)}.field:focus-within{background:rgba(0,0,0,.2)}.field input{width:100%;height:100%;border:0;background:transparent;color:#e9ecea}</style><body><label class='field'><input type='search' placeholder='Search'></label></body>"),
                L"transparent custom editor fixture loads");
            SendMessageW(resizeView->Window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(50,17));
            Check(GetWindow(resizeView->Window(),GW_CHILD)==nullptr,
                  L"focused text input does not create a native child editor");
            SendMessageW(resizeView->Window(),WM_CHAR,L'x',1);
            Check(resizeView->ExecuteScript(L"return document.querySelector('input').value;",
                                            &hoverResult,&hoverError)&&hoverResult==L"x",
                  L"the first custom-input character replaces the visible placeholder with DOM text");
        }
        resizeView.reset();DestroyWindow(resizeHost);
    }
    UnregisterClassW(resizeHostClass,GetModuleHandleW(nullptr));

    Document positionedDoc;
    Check(positionedDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}.overlay{position:fixed;inset:0;display:grid;place-items:center;opacity:0}.dialog{width:40px;height:20px;transform:translateY(-50%)}</style><div id='overlay' class='overlay'><div id='dialog' class='dialog'></div></div>",&error),
          L"positioned layout fixture parses");
    StyleSheet positionedCss;Check(positionedCss.Parse(positionedDoc.StyleText(),&error),L"positioned layout CSS parses");
    const auto overlayStyle=positionedCss.Compute(positionedDoc.GetElementById(L"overlay"));
    Check(overlayStyle.Get(L"top")==L"0"&&overlayStyle.Get(L"right")==L"0"&&
          overlayStyle.Get(L"bottom")==L"0"&&overlayStyle.Get(L"left")==L"0"&&
          overlayStyle.Get(L"align-items")==L"center"&&overlayStyle.Get(L"justify-items")==L"center",
          L"inset and place-items shorthands populate positioned layout longhands");
    LayoutEngine positionedLayout(positionedDoc,positionedCss);positionedLayout.Layout(300,200);
    const auto* overlayBox=FindLayout(positionedLayout.Root(),L"overlay");
    const auto* dialogBox=FindLayout(positionedLayout.Root(),L"dialog");
    Check(overlayBox&&dialogBox&&std::lround(overlayBox->rect.width)==300&&std::lround(overlayBox->rect.height)==200&&
          std::lround(dialogBox->rect.x)==130&&std::lround(dialogBox->rect.y)==80,
          L"fixed inset sizing, grid centering and percentage transforms compose generically");

    Check(std::abs(StyleSheet::Length(L"min(560px, calc(100vw - 48px))",1101,1101,-1)-560)<0.01f&&
          std::abs(StyleSheet::Length(L"max(240px, 30%)",1000,1000,-1)-300)<0.01f&&
          std::abs(StyleSheet::Length(L"clamp(240px, 50%, 560px)",800,800,-1)-400)<0.01f,
          L"CSS min, max and clamp math functions resolve nested lengths");
    Document responsiveDialogDoc;
    Check(responsiveDialogDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.overlay{position:fixed;inset:0;display:grid;place-items:center;padding:24px}.dialog{width:min(560px,calc(100vw - 48px));height:250px}</style>"
          L"<div class='overlay'><section id='responsive-dialog' class='dialog'></section></div>",&error),
          L"responsive dialog fixture parses");
    StyleSheet responsiveDialogCss;Check(responsiveDialogCss.Parse(responsiveDialogDoc.StyleText(),&error),L"responsive dialog CSS parses");
    LayoutEngine responsiveDialogLayout(responsiveDialogDoc,responsiveDialogCss);responsiveDialogLayout.Layout(1101,719);
    const auto* responsiveDialog=FindLayout(responsiveDialogLayout.Root(),L"responsive-dialog");
    Check(responsiveDialog&&std::abs(responsiveDialog->rect.width-560)<0.01f&&
          std::abs(responsiveDialog->rect.x-270.5f)<0.01f&&std::abs(responsiveDialog->rect.y-234.5f)<0.01f,
          L"grid items resolve responsive explicit sizes against their containing grid area");

    Document implicitScrollDoc;
    Check(implicitScrollDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.grid{display:grid;width:300px;height:100px}.scroll{min-height:0;overflow:auto;scrollbar-width:thin;scrollbar-color:#4a4a4a transparent}.scroll::-webkit-scrollbar{width:10px}.scroll::-webkit-scrollbar-track{background:transparent}.scroll::-webkit-scrollbar-thumb{min-height:36px;background:#454545;border:3px solid transparent;border-radius:99px}.row{height:60px}</style>"
          L"<div class='grid'><main id='implicit-scroll' class='scroll'><div class='row'>1</div><div class='row'>2</div><div class='row'>3</div></main></div>",&error),
          L"implicit grid scroll fixture parses");
    StyleSheet implicitScrollCss;Check(implicitScrollCss.Parse(implicitScrollDoc.StyleText(),&error),L"implicit grid scroll CSS parses");
    LayoutEngine implicitScrollLayout(implicitScrollDoc,implicitScrollCss);implicitScrollLayout.Layout(300,100);
    const auto implicitScrollNode=implicitScrollDoc.GetElementById(L"implicit-scroll");
    const auto* implicitScrollBox=implicitScrollLayout.BoxFor(implicitScrollNode);
    const auto implicitThumbStyle=implicitScrollBox?
        implicitScrollCss.Compute(implicitScrollNode,&implicitScrollBox->style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
    Check(implicitScrollBox&&std::lround(implicitScrollBox->rect.height)==100&&
          std::lround(implicitScrollBox->scrollHeight)==180,
          L"an overflow-auto item shrinks inside a definite implicit grid row");
    std::shared_ptr<Node> thinScrollbarDrag;float thinScrollbarOffset=0;
    const float implicitScrollRight=implicitScrollBox?implicitScrollBox->content.x+implicitScrollBox->content.width:0;
    const float implicitScrollTop=implicitScrollBox?implicitScrollBox->content.y:0;
    Check(implicitScrollBox&&implicitScrollBox->style.Get(L"scrollbar-width")==L"thin"&&
          implicitScrollBox->style.Get(L"scrollbar-color")==L"#4a4a4a transparent"&&
          implicitThumbStyle.Get(L"background-color")==L"#454545"&&
          implicitThumbStyle.Get(L"min-height")==L"36px"&&
          !implicitScrollLayout.BeginScrollbarInteraction(implicitScrollRight-12,implicitScrollTop+5,thinScrollbarDrag,thinScrollbarOffset)&&
          implicitScrollLayout.BeginScrollbarInteraction(implicitScrollRight-5,implicitScrollTop+5,thinScrollbarDrag,thinScrollbarOffset)&&
          !thinScrollbarDrag&&
          implicitScrollLayout.BeginScrollbarInteraction(implicitScrollRight-5,implicitScrollTop+17,thinScrollbarDrag,thinScrollbarOffset)&&
          thinScrollbarDrag==implicitScrollNode,
          L"thin scrollbar styling uses a ten-pixel track with compact triangle buttons and an inset thumb");
    Check(implicitScrollLayout.ScrollAt(150,50,-120)&&implicitScrollNode->scrollTop>0,
          L"the mouse wheel scrolls an overflow-auto item in an implicit grid row");

    Document viewportScrollDoc;
    Check(viewportScrollDoc.Parse(
          L"<!doctype html><html><head><style>.page-content{height:600px;background:#f33}</style></head>"
          L"<body><main class='page-content'></main></body></html>",&error),
          L"ordinary tall document viewport-scroll fixture parses");
    StyleSheet viewportScrollCss;Check(viewportScrollCss.Parse(viewportScrollDoc.StyleText(),&error),
          L"ordinary tall document viewport-scroll CSS parses");
    LayoutEngine viewportScrollLayout(viewportScrollDoc,viewportScrollCss);
    bool viewportScrollDpiValid=true;
    for(const float scale:{1.0f,1.5f}){
        viewportScrollDoc.Body()->scrollTop=0;
        viewportScrollLayout.Layout(300,200,scale);
        const auto raster=CaptureBoxRaster(viewportScrollLayout,scale,300,200);
        const auto* viewportRoot=viewportScrollLayout.Root();
        const float scrollbarX=std::round(292.0f*scale)/scale;
        const float thumbY=std::round(25.0f*scale)/scale;
        std::shared_ptr<Node> viewportDrag;float viewportDragOffset=0;bool horizontal=false;
        const bool outsideTrack=viewportScrollLayout.BeginScrollbarInteraction(
            std::round(280.0f*scale)/scale,thumbY,viewportDrag,viewportDragOffset,horizontal);
        viewportDrag.reset();viewportDragOffset=0;horizontal=false;
        const bool hitsThumb=viewportScrollLayout.BeginScrollbarInteraction(
            scrollbarX,thumbY,viewportDrag,viewportDragOffset,horizontal);
        viewportScrollDoc.Body()->scrollTop=0;viewportScrollLayout.SyncScroll(viewportScrollDoc.Body());
        viewportScrollDpiValid=viewportScrollDpiValid&&viewportRoot&&
            viewportRoot->viewportScrollContainer&&viewportRoot->viewportOverflowY==L"auto"&&
            viewportRoot->viewportScrollport.width==300&&viewportRoot->viewportScrollport.height==200&&
            viewportRoot->scrollHeight>590&&!outsideTrack&&hitsThumb&&!horizontal&&
            viewportDrag==viewportScrollDoc.Body()&&raster.rendered&&
            raster.ColorAt(scrollbarX,thumbY,scale)==0x008b8b8b;
        SaveRasterBmp(std::filesystem::path(L"TWebFrame2/tests/artifacts")/
            (scale==1?L"body-scrollbar-100.bmp":L"body-scrollbar-150.bmp"),
            raster.width,static_cast<UINT>(raster.pixels.size()/std::max(1u,raster.width)),
            raster.pixels.data());
        viewportScrollDoc.Body()->scrollTop=0;viewportScrollLayout.SyncScroll(viewportScrollDoc.Body());
        viewportScrollDpiValid=viewportScrollDpiValid&&
            viewportScrollLayout.ScrollAt(150,100,-120)&&viewportScrollDoc.Body()->scrollTop==90;
        viewportScrollDoc.Body()->scrollTop=0;viewportScrollLayout.SyncScroll(viewportScrollDoc.Body());
        viewportDrag.reset();viewportDragOffset=0;horizontal=false;
        const bool startsDrag=viewportScrollLayout.BeginScrollbarInteraction(
            scrollbarX,thumbY,viewportDrag,viewportDragOffset,horizontal);
        viewportScrollDpiValid=viewportScrollDpiValid&&startsDrag&&viewportDrag&&
            viewportScrollLayout.DragScrollbar(viewportDrag,scrollbarX,1000,viewportDragOffset,horizontal)&&
            std::abs(viewportScrollDoc.Body()->scrollTop-(viewportRoot->scrollHeight-200))<0.01f&&
            viewportScrollLayout.DragScrollbar(viewportDrag,scrollbarX,-1000,viewportDragOffset,horizontal)&&
            viewportScrollDoc.Body()->scrollTop==0;
        viewportScrollLayout.Relayout(300,700,scale);
        viewportDrag.reset();viewportDragOffset=0;horizontal=false;
        viewportScrollDpiValid=viewportScrollDpiValid&&
            viewportScrollLayout.Root()->viewportScrollport.height==700&&
            !viewportScrollLayout.BeginScrollbarInteraction(scrollbarX,thumbY,
                viewportDrag,viewportDragOffset,horizontal)&&
            !viewportScrollLayout.ScrollAt(150,100,-120);
    }
    Check(viewportScrollDpiValid,
          L"default body overflow paints, hits and wheel-scrolls a viewport-edge scrollbar at 100 and 150 percent DPI");
    JavaScriptRuntime viewportScrollRuntime(viewportScrollDoc);
    std::wstring viewportScriptResult,viewportScriptError;
    Check(viewportScrollRuntime.Execute(
          L"document.documentElement.scrollTop=37;return document.scrollingElement===document.documentElement&&document.body.scrollTop===37&&document.documentElement.scrollTop===37;",
          &viewportScriptResult,&viewportScriptError)&&viewportScriptResult==L"true",
          L"standards-mode documentElement and scrollingElement address the common viewport scroll state");

    const auto rootOverflow=[&](const wchar_t* css){
        Document document;StyleSheet sheet;std::wstring localError;
        const std::wstring html=L"<!doctype html><style>"+std::wstring(css)+
            L"</style><body><div style='height:500px'></div></body>";
        if(!document.Parse(html,&localError)||!sheet.Parse(document.StyleText(),&localError))
            return std::wstring{};
        LayoutEngine layout(document,sheet);layout.Layout(240,120);
        const auto* root=layout.Root();
        if(!root)return std::wstring{};
        std::shared_ptr<Node> drag;float offset=0;bool horizontal=false;
        const bool scrollbar=layout.BeginScrollbarInteraction(235,22,drag,offset,horizontal);
        return root->viewportOverflowY+L"|"+(scrollbar?L"bar":L"none");
    };
    Check(rootOverflow(L"html{overflow:hidden}body{overflow:visible}")==L"hidden|none"&&
          rootOverflow(L"html{overflow:visible}body{overflow:hidden}")==L"hidden|none"&&
          rootOverflow(L"html{overflow:clip}")==L"hidden|none"&&
          rootOverflow(L"html{overflow:auto}body{overflow:hidden}")==L"auto|bar",
          L"root and body overflow propagate to the viewport using common HTML scrolling rules");

    Document stackedViewportDoc;
    Check(stackedViewportDoc.Parse(
          L"<!doctype html><style>body{margin:0}.page{height:600px;background:#f33;position:relative;z-index:1}"
          L".pane{position:fixed;right:0;top:0;width:50px;height:200px;overflow:scroll;z-index:9999;background:#0f0;scrollbar-color:#000 #0f0}"
          L".pane>div{height:500px}</style><body><main class='page'></main><aside id='edge-pane' class='pane'><div></div></aside></body>",&error),
          L"stacked viewport scrollbar fixture parses");
    StyleSheet stackedViewportCss;
    Check(stackedViewportCss.Parse(stackedViewportDoc.StyleText(),&error),
          L"stacked viewport scrollbar CSS parses");
    LayoutEngine stackedViewportLayout(stackedViewportDoc,stackedViewportCss);
    bool stackedViewportValid=true;
    for(const float scale:{1.0f,1.5f}){
        const auto raster=CaptureBoxRaster(stackedViewportLayout,scale,300,200);
        std::shared_ptr<Node> drag;float offset=0;bool horizontal=false;
        const float x=std::round(292.0f*scale)/scale;
        const float y=std::round(25.0f*scale)/scale;
        stackedViewportValid=stackedViewportValid&&raster.rendered&&
            raster.ColorAt(x,y,scale)==0x008b8b8b&&
            raster.ColorAt(x,100,scale)==0x00fcfcfc&&
            stackedViewportLayout.BeginScrollbarInteraction(x,y,drag,offset,horizontal)&&
            drag==stackedViewportDoc.Body()&&!horizontal;
    }
    Check(stackedViewportValid,
          L"viewport scrollbar paints and receives pointer input above positioned and fixed content at both DPIs");

    Document horizontalViewportDoc;
    Check(horizontalViewportDoc.Parse(
          L"<!doctype html><style>body{margin:0}.wide{width:600px;height:20px;background:#f33}</style>"
          L"<body><main class='wide'></main></body>",&error),
          L"horizontal viewport scrollbar fixture parses");
    StyleSheet horizontalViewportCss;
    Check(horizontalViewportCss.Parse(horizontalViewportDoc.StyleText(),&error),
          L"horizontal viewport scrollbar CSS parses");
    LayoutEngine horizontalViewportLayout(horizontalViewportDoc,horizontalViewportCss);
    bool horizontalViewportValid=true;
    for(const float scale:{1.0f,1.5f}){
        horizontalViewportDoc.Body()->scrollLeft=0;
        const auto raster=CaptureBoxRaster(horizontalViewportLayout,scale,300,200);
        const float x=std::round(25.0f*scale)/scale;
        const float y=std::round(192.0f*scale)/scale;
        std::shared_ptr<Node> drag;float offset=0;bool horizontal=false;
        horizontalViewportValid=horizontalViewportValid&&raster.rendered&&
            raster.ColorAt(x,y,scale)==0x008b8b8b&&
            horizontalViewportLayout.BeginScrollbarInteraction(x,y,drag,offset,horizontal)&&
            drag==horizontalViewportDoc.Body()&&horizontal&&
            horizontalViewportLayout.DragScrollbar(drag,1000,y,offset,horizontal)&&
            horizontalViewportDoc.Body()->scrollLeft==300&&
            horizontalViewportLayout.DragScrollbar(drag,-1000,y,offset,horizontal)&&
            horizontalViewportDoc.Body()->scrollLeft==0;
    }
    Check(horizontalViewportValid,
          L"horizontal viewport scrollbar paints, drags and clamps its range at both DPIs");

    Document forcedViewportDoc;
    Check(forcedViewportDoc.Parse(L"<!doctype html><style>html{overflow:scroll}body{margin:0}</style><body></body>",&error),
          L"empty overflow-scroll viewport fixture parses");
    StyleSheet forcedViewportCss;
    Check(forcedViewportCss.Parse(forcedViewportDoc.StyleText(),&error),
          L"empty overflow-scroll viewport CSS parses");
    LayoutEngine forcedViewportLayout(forcedViewportDoc,forcedViewportCss);
    forcedViewportLayout.Layout(300,200);
    std::shared_ptr<Node> forcedViewportDrag;float forcedViewportOffset=0;bool forcedHorizontal=false;
    Check(forcedViewportLayout.BeginScrollbarInteraction(292,20,forcedViewportDrag,forcedViewportOffset,forcedHorizontal)&&
          forcedViewportLayout.BeginScrollbarInteraction(20,192,forcedViewportDrag,forcedViewportOffset,forcedHorizontal)&&
          !forcedViewportLayout.BeginScrollbarInteraction(292,192,forcedViewportDrag,forcedViewportOffset,forcedHorizontal),
          L"overflow-scroll shows both viewport tracks without content and excludes their shared corner from hit testing");

    Document gridScrollerDoc;
    Check(gridScrollerDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.scroller{display:grid;width:300px;height:100px;overflow:auto;grid-template-columns:100px 1fr}.tall{height:150px}</style>"
          L"<div id='grid-scroller' class='scroller'><div class='tall'></div><div id='grid-scroll-second'></div></div>",&error),
          L"scrolling grid fixture parses");
    StyleSheet gridScrollerCss;Check(gridScrollerCss.Parse(gridScrollerDoc.StyleText(),&error),L"scrolling grid CSS parses");
    LayoutEngine gridScrollerLayout(gridScrollerDoc,gridScrollerCss);gridScrollerLayout.Layout(400,200);
    const auto* gridScroller=FindLayout(gridScrollerLayout.Root(),L"grid-scroller");
    const auto* gridScrollSecond=FindLayout(gridScrollerLayout.Root(),L"grid-scroll-second");
    Check(gridScroller&&gridScrollSecond&&std::lround(gridScroller->scrollHeight)==150&&
          std::lround(gridScrollSecond->rect.width)==185,
          L"overflowing grid containers compute scroll height and reserve DPI-independent scrollbar space");

    Document stableGutterDoc;
    Check(stableGutterDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.gutter{width:200px;height:80px;overflow-y:auto;scrollbar-gutter:stable}.child{width:100%;height:20px}</style>"
          L"<div class='gutter'><div id='stable-gutter-child' class='child'></div></div>",&error),
          L"stable scrollbar-gutter fixture parses");
    StyleSheet stableGutterCss;Check(stableGutterCss.Parse(stableGutterDoc.StyleText(),&error),L"stable scrollbar-gutter CSS parses");
    LayoutEngine stableGutterLayout(stableGutterDoc,stableGutterCss);stableGutterLayout.Layout(240,100);
    const auto* stableGutterChild=FindLayout(stableGutterLayout.Root(),L"stable-gutter-child");
    Check(stableGutterChild&&std::lround(stableGutterChild->rect.width)==185,
          L"scrollbar-gutter stable reserves a DPI-independent gutter before overflow appears");

    Document popupMenuDoc;
    Check(popupMenuDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}#bar{display:flex;align-items:center;width:320px;height:42px;border-bottom:1px solid #444}#menu{display:flex;align-items:center;height:100%}#menu-root{position:relative;height:100%}#trigger{display:inline-grid;place-items:center;height:100%;padding:0 11px}.popup{display:none;position:absolute;top:100%;width:100px;height:30px}.open .popup{display:block}</style>"
          L"<header id='bar'><nav id='menu'><div id='menu-root'><button id='trigger'>File</button><div class='popup'></div></div></nav></header>",&error),
          L"popup menu layout fixture parses");
    StyleSheet popupMenuCss;Check(popupMenuCss.Parse(popupMenuDoc.StyleText(),&error),L"popup menu CSS parses");
    LayoutEngine popupMenuLayout(popupMenuDoc,popupMenuCss);popupMenuLayout.Layout(320,80);
    const auto popupMenuRoot=popupMenuDoc.GetElementById(L"menu-root");
    const auto* closedTrigger=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"trigger"));
    const auto* closedMenu=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"menu"));
    const bool closedMenuValid=closedTrigger&&closedMenu;
    const float closedTriggerY=closedTrigger?closedTrigger->rect.y:-1;
    const float closedTriggerHeight=closedTrigger?closedTrigger->rect.height:-1;
    if(popupMenuRoot)popupMenuRoot->AddClass(L"open");
    popupMenuLayout.Layout(320,80);
    const auto* openTrigger=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"trigger"));
    const auto* openMenu=popupMenuLayout.BoxFor(popupMenuDoc.GetElementById(L"menu"));
    Check(closedMenuValid&&openTrigger&&openMenu&&
          std::abs(closedTriggerY-openTrigger->rect.y)<0.01f&&
          std::abs(closedTriggerHeight-openTrigger->rect.height)<0.01f&&
          std::abs(openTrigger->rect.height-openMenu->content.height)<0.01f,
          L"opening an absolute popup keeps its full-height menu trigger in place");

    Document hoverPopupDoc;
    Check(hoverPopupDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.menu{position:relative;width:320px;height:38px;overflow:visible}.entry{position:relative;width:100px;height:38px}.panel{display:none;position:absolute;left:0;top:38px;width:220px;height:90px;z-index:20}.entry:hover .panel{display:block}.panel ul{display:block;width:220px}.panel li,.panel a{display:block;width:220px;height:30px}</style>"
          L"<nav class='menu'><div id='hover-popup-entry' class='entry'><a>Menu</a><div id='hover-popup-panel' class='panel'><ul><li><a id='hover-popup-link'>First</a></li><li><a>Second</a></li></ul></div></div></nav>",&error),
          L"hover popup DPI fixture parses");
    StyleSheet hoverPopupCss;Check(hoverPopupCss.Parse(hoverPopupDoc.StyleText(),&error),L"hover popup DPI CSS parses");
    LayoutEngine hoverPopupLayout(hoverPopupDoc,hoverPopupCss);
    const auto hoverPopupEntry=hoverPopupDoc.GetElementById(L"hover-popup-entry");
    const auto hoverPopupPanel=hoverPopupDoc.GetElementById(L"hover-popup-panel");
    const auto hoverPopupLink=hoverPopupDoc.GetElementById(L"hover-popup-link");
    bool hoverPopupDpiValid=hoverPopupEntry&&hoverPopupPanel&&hoverPopupLink;
    for(const float scale:{1.0f,1.5f})if(hoverPopupDpiValid){
        hoverPopupEntry->hovered=false;hoverPopupLayout.Layout(400,180,scale);
        const auto* hiddenPanel=hoverPopupLayout.BoxFor(hoverPopupPanel);
        hoverPopupDpiValid=hoverPopupDpiValid&&hiddenPanel&&!hiddenPanel->visible;
        hoverPopupEntry->hovered=true;hoverPopupLayout.Layout(400,180,scale);
        const auto* entryBox=hoverPopupLayout.BoxFor(hoverPopupEntry);
        const auto* panelBox=hoverPopupLayout.BoxFor(hoverPopupPanel);
        const auto* linkBox=hoverPopupLayout.BoxFor(hoverPopupLink);
        const auto hit=linkBox?hoverPopupLayout.HitTest(
            linkBox->rect.x+linkBox->rect.width-2,linkBox->rect.y+linkBox->rect.height/2):std::shared_ptr<Node>{};
        hoverPopupDpiValid=hoverPopupDpiValid&&entryBox&&panelBox&&panelBox->visible&&linkBox&&
            std::abs(panelBox->rect.y-entryBox->rect.y-entryBox->rect.height)<0.01f&&
            std::abs(panelBox->rect.width-220.0f)<0.01f&&
            std::abs(panelBox->style.deviceScale-scale)<0.01f&&
            (hit==hoverPopupLink||(hit&&hit->parent.lock()==hoverPopupLink));
    }
    Check(hoverPopupDpiValid,
          L"descendant hover popups remain visible, positioned and hit-testable at 100 and 150 percent DPI");

    Document floatedMenuDoc;
    Check(floatedMenuDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.popup{position:absolute;width:680px;height:auto;padding:3px 2px;border:2px solid;overflow:visible}.row{height:275px;clear:left}.column{float:left;width:95px;height:100%}.column span{display:block;height:20px}</style>"
          L"<div id='float-popup' class='popup'><div id='float-row' class='row'>"
          L"<ul id='float-first' class='column'><li><span>one</span></li></ul>"
          L"<ul class='column'></ul><ul class='column'></ul><ul class='column'></ul>"
          L"<ul class='column'></ul><ul class='column'></ul><ul id='float-last' class='column'></ul>"
          L"</div></div>",&error),L"multi-column floated menu fixture parses");
    StyleSheet floatedMenuCss;Check(floatedMenuCss.Parse(floatedMenuDoc.StyleText(),&error),
          L"multi-column floated menu CSS parses");
    LayoutEngine floatedMenuLayout(floatedMenuDoc,floatedMenuCss);
    bool floatedMenuDpiValid=true;
    for(const float scale:{1.0f,1.5f}){
        floatedMenuLayout.Layout(760,340,scale);
        const auto* popup=floatedMenuLayout.BoxFor(floatedMenuDoc.GetElementById(L"float-popup"));
        const auto* row=floatedMenuLayout.BoxFor(floatedMenuDoc.GetElementById(L"float-row"));
        const auto* first=floatedMenuLayout.BoxFor(floatedMenuDoc.GetElementById(L"float-first"));
        const auto* last=floatedMenuLayout.BoxFor(floatedMenuDoc.GetElementById(L"float-last"));
        floatedMenuDpiValid=floatedMenuDpiValid&&popup&&row&&first&&last&&
            std::fabs(popup->rect.width-680.0f)<0.01f&&
            std::fabs(popup->rect.height-285.0f)<0.01f&&
            std::fabs(first->rect.y-last->rect.y)<0.01f&&
            std::fabs(last->rect.x-first->rect.x-570.0f)<0.01f&&
            last->rect.x+last->rect.width<=row->content.x+row->content.width+0.01f&&
            std::fabs(first->rect.height-row->content.height)<0.01f;
    }
    Check(floatedMenuDpiValid,
          L"fixed-height popup rows arrange floated percentage-height columns at 100 and 150 percent DPI");

    Document floatedVoteDoc;
    Check(floatedVoteDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}table{width:500px;table-layout:fixed}"
          L"td.avatar{width:100px}.comment{padding:2px 4px 4px 10px}.vote{float:right;padding:0 5px;margin-left:3px;border:1px solid #ddd}"
          L".memo{position:relative;overflow:hidden;width:100%;padding-top:4px;min-height:40px;line-height:1.5}</style>"
          L"<table><tr><td class='avatar'>avatar</td><td id='comment-cell' class='comment'>"
          L"<span id='anti-vote' class='vote'>0</span><span id='up-vote' class='vote'>0</span>"
          L"<b id='comment-name'>name</b><div id='comment-memo' class='memo'>comment text</div>"
          L"</td></tr></table>",&error),L"floated comment vote fixture parses");
    StyleSheet floatedVoteCss;Check(floatedVoteCss.Parse(floatedVoteDoc.StyleText(),&error),
          L"floated comment vote CSS parses");
    LayoutEngine floatedVoteLayout(floatedVoteDoc,floatedVoteCss);
    bool floatedVoteDpiValid=true;
    float voteWidth100=0,cellWidth100=0;
    for(const float scale:{1.0f,1.5f}){
        floatedVoteLayout.Layout(560,140,scale);
        const auto* cell=floatedVoteLayout.BoxFor(floatedVoteDoc.GetElementById(L"comment-cell"));
        const auto* antiVote=floatedVoteLayout.BoxFor(floatedVoteDoc.GetElementById(L"anti-vote"));
        const auto* upVote=floatedVoteLayout.BoxFor(floatedVoteDoc.GetElementById(L"up-vote"));
        const auto* name=floatedVoteLayout.BoxFor(floatedVoteDoc.GetElementById(L"comment-name"));
        const auto* memo=floatedVoteLayout.BoxFor(floatedVoteDoc.GetElementById(L"comment-memo"));
        floatedVoteDpiValid=floatedVoteDpiValid&&cell&&antiVote&&upVote&&name&&memo;
        if(!cell||!antiVote||!upVote||!name||!memo)continue;
        if(scale==1.0f){voteWidth100=antiVote->rect.width;cellWidth100=cell->rect.width;}
        floatedVoteDpiValid=floatedVoteDpiValid&&
            antiVote->rect.width<40.0f&&upVote->rect.width<40.0f&&
            std::fabs(antiVote->rect.y-upVote->rect.y)<0.01f&&
            upVote->rect.x+upVote->rect.width<=antiVote->rect.x+0.01f&&
            antiVote->rect.x+antiVote->rect.width<=cell->content.x+cell->content.width+0.01f&&
            std::fabs(name->rect.y-antiVote->rect.y)<1.0f&&
            memo->rect.y<antiVote->rect.y+antiVote->rect.height+20.0f&&
            // A one-physical-pixel border is 1 CSS px at 100% and 2/3 CSS px
            // at 150%; the control must otherwise preserve its CSS geometry.
            std::fabs(antiVote->rect.width-voteWidth100)<1.0f&&
            std::fabs(cell->rect.width-cellWidth100)<0.01f;
    }
    Check(floatedVoteDpiValid,
          L"auto-width right-floated inline vote controls shrink to content inside table cells at 100 and 150 percent DPI");

    bool formattedFloatsValid=true,formattedFloatHeightsValid=true;
    for(const bool left:{false,true})for(const bool labelBefore:{false,true})
        for(const bool tableCell:{false,true}){
            Document formattedFloatDoc;
            const std::wstring label=L"<b id='float-label'>Label</b>";
            const std::wstring markup=(labelBefore?label:L"")+
                L"\n  <span id='float-first' class='badge'><img><span>0</span></span>\n\t"
                L"<!-- formatting whitespace -->\n  <span id='float-second' class='badge'>"
                L"<img><span>11</span></span>\n  "+(labelBefore?L"":label)+
                L"<p id='float-body'>Body text</p>";
            const std::wstring html=L"<!doctype html><style>html,body{margin:0;font:12px/20px Arial}"
                L"table{width:320px;table-layout:fixed;border-collapse:collapse}td{padding:0}"
                L".panel{display:flow-root;width:320px}.badge{float:"+
                std::wstring(left?L"left":L"right")+
                L";margin-left:3px;padding:0 5px;border:1px solid #ccc;font-size:13px;line-height:20px}"
                L"img{width:12px;height:10px;vertical-align:middle}p{margin:0}</style>"+
                (tableCell?L"<table><tr><td id='float-panel'>":L"<div class='panel' id='float-panel'>")+
                markup+(tableCell?L"</td></tr></table>":L"</div>");
            Check(formattedFloatDoc.Parse(html,&error),L"formatted adjacent floats fixture parses");
            StyleSheet formattedFloatCss;Check(formattedFloatCss.Parse(formattedFloatDoc.StyleText(),&error),
                L"formatted adjacent floats CSS parses");
            LayoutEngine formattedFloatLayout(formattedFloatDoc,formattedFloatCss);
            for(const float scale:{1.0f,1.5f}){
                formattedFloatLayout.Layout(400,160,scale);
                const auto* first=formattedFloatLayout.BoxFor(formattedFloatDoc.GetElementById(L"float-first"));
                const auto* second=formattedFloatLayout.BoxFor(formattedFloatDoc.GetElementById(L"float-second"));
                const auto* panel=formattedFloatLayout.BoxFor(formattedFloatDoc.GetElementById(L"float-panel"));
                const auto* text=formattedFloatLayout.BoxFor(formattedFloatDoc.GetElementById(L"float-label"));
                const auto* body=formattedFloatLayout.BoxFor(formattedFloatDoc.GetElementById(L"float-body"));
                formattedFloatsValid=formattedFloatsValid&&first&&second&&panel&&text&&body;
                formattedFloatHeightsValid=formattedFloatHeightsValid&&first&&second;
                if(!first||!second||!panel||!text||!body)continue;
                const float border=std::floor(scale)/scale;
                const float separation=left?second->rect.x-first->rect.x-first->rect.width:
                    first->rect.x-second->rect.x-second->rect.width;
                formattedFloatsValid=formattedFloatsValid&&
                    std::abs(first->rect.y-panel->content.y)<0.01f&&
                    std::abs(second->rect.y-first->rect.y)<0.01f&&
                    std::abs(separation-3)<0.01f&&
                    body->rect.y-panel->content.y<21&&
                    (left?text->rect.x>=second->rect.x+second->rect.width-0.01f:
                          text->rect.x+text->rect.width<=second->rect.x+0.01f);
                formattedFloatHeightsValid=formattedFloatHeightsValid&&
                    std::abs(first->rect.height-(20+2*border))<0.01f&&
                    std::abs(second->rect.height-(20+2*border))<0.01f;
            }
        }
    Check(formattedFloatsValid,
        L"left and right float pairs stay side by side across formatting whitespace and preceding labels in blocks and cells at both DPIs");
    Check(formattedFloatHeightsValid,
        L"floated inline elements retain their block line height plus borders at 100 and 150 percent DPI");

    Document narrowFloatDoc;
    Check(narrowFloatDoc.Parse(
        L"<!doctype html><style>html,body{margin:0}.panel{display:flow-root;width:50px}"
        L".badge{float:right;width:30px;height:16px}.clear{clear:both;height:8px}"
        L".preserved{white-space:pre;width:200px}</style>"
        L"<div class='panel'><span id='narrow-first' class='badge'></span>\n "
        L"<span id='narrow-second' class='badge'></span><div id='clear-after-floats' class='clear'></div></div>"
        L"<div class='panel preserved'><span id='preserved-first' class='badge'></span>\n"
        L"<span id='preserved-second' class='badge'></span></div>",&error),
        L"narrow and preserved-whitespace float fixture parses");
    StyleSheet narrowFloatCss;Check(narrowFloatCss.Parse(narrowFloatDoc.StyleText(),&error),
        L"narrow and preserved-whitespace float CSS parses");
    LayoutEngine narrowFloatLayout(narrowFloatDoc,narrowFloatCss);
    bool narrowFloatsValid=true;
    for(const float scale:{1.0f,1.5f}){
        narrowFloatLayout.Layout(240,200,scale);
        const auto* first=narrowFloatLayout.BoxFor(narrowFloatDoc.GetElementById(L"narrow-first"));
        const auto* second=narrowFloatLayout.BoxFor(narrowFloatDoc.GetElementById(L"narrow-second"));
        const auto* cleared=narrowFloatLayout.BoxFor(narrowFloatDoc.GetElementById(L"clear-after-floats"));
        const auto* preFirst=narrowFloatLayout.BoxFor(narrowFloatDoc.GetElementById(L"preserved-first"));
        const auto* preSecond=narrowFloatLayout.BoxFor(narrowFloatDoc.GetElementById(L"preserved-second"));
        narrowFloatsValid=narrowFloatsValid&&first&&second&&cleared&&preFirst&&preSecond&&
            std::abs(second->rect.y-first->rect.y-first->rect.height)<0.01f&&
            std::abs(cleared->rect.y-second->rect.y-second->rect.height)<0.01f&&
            preSecond->rect.y>preFirst->rect.y+0.01f;
    }
    Check(narrowFloatsValid,
        L"insufficient width, clear and preserved line breaks still move floats onto later rows at both DPIs");

    Document legacyAlignmentDoc;
    Check(legacyAlignmentDoc.Parse(
        L"<!doctype html><style>html,body{margin:0}.panel{width:300px}"
        L".nested{width:80%;height:30px}.small{width:100px;height:20px}"
        L"table{width:300px;table-layout:fixed;border-collapse:collapse}"
        L"td{padding:0;height:80px}td>div{width:20px;height:16px}</style>"
        L"<div class='panel' align='RIGHT' id='legacy-right'><div class='nested' id='legacy-inner'>"
        L"<div class='nested' id='legacy-deep'></div></div>"
        L"<div class='small' id='legacy-auto' style='margin-right:auto'></div>"
        L"<div class='small' align='left' id='legacy-reset'><div style='width:50px;height:20px' id='legacy-reset-child'></div></div></div>"
        L"<div class='panel' align='middle' id='legacy-center'><div class='small' id='legacy-center-inner'></div></div>"
        L"<div class='panel' align='right' style='text-align:right' id='legacy-css'><div class='small' id='legacy-css-inner'></div></div>"
        L"<table><tbody valign='bottom'><tr valign='TOP'>"
        L"<td id='legacy-top'><div id='legacy-top-child'></div></td>"
        L"<td valign='bottom' id='legacy-bottom'><div id='legacy-bottom-child'></div></td>"
        L"<td valign='bottom' style='vertical-align:middle' id='legacy-middle'><div id='legacy-middle-child'></div></td>"
        L"</tr><tr><td id='legacy-group'><div id='legacy-group-child'></div></td><td></td><td></td></tr></tbody></table>",&error),
        L"inherited HTML block and row alignment fixture parses");
    StyleSheet legacyAlignmentCss;Check(legacyAlignmentCss.Parse(legacyAlignmentDoc.StyleText(),&error),
        L"inherited HTML block and row alignment CSS parses");
    LayoutEngine legacyAlignmentLayout(legacyAlignmentDoc,legacyAlignmentCss);
    bool legacyBlockAlignmentValid=true,legacyRowAlignmentValid=true;
    for(const float scale:{1.0f,1.5f}){
        legacyAlignmentLayout.Layout(400,400,scale);
        const auto find=[&](const wchar_t* id){return legacyAlignmentLayout.BoxFor(legacyAlignmentDoc.GetElementById(id));};
        const auto* outer=find(L"legacy-right");const auto* inner=find(L"legacy-inner");
        const auto* deep=find(L"legacy-deep");const auto* automatic=find(L"legacy-auto");
        const auto* reset=find(L"legacy-reset");const auto* resetChild=find(L"legacy-reset-child");
        const auto* centered=find(L"legacy-center");const auto* centerChild=find(L"legacy-center-inner");
        const auto* css=find(L"legacy-css");const auto* cssChild=find(L"legacy-css-inner");
        legacyBlockAlignmentValid=legacyBlockAlignmentValid&&outer&&inner&&deep&&automatic&&reset&&resetChild&&centered&&centerChild&&css&&cssChild;
        if(outer&&inner&&deep&&automatic&&reset&&resetChild&&centered&&centerChild&&css&&cssChild)
            legacyBlockAlignmentValid=legacyBlockAlignmentValid&&
                std::abs(inner->rect.x-outer->content.x-60)<0.01f&&
                std::abs(deep->rect.x-inner->content.x-48)<0.01f&&
                std::abs(automatic->rect.x-outer->content.x)<0.01f&&
                std::abs(reset->rect.x-outer->content.x-200)<0.01f&&
                std::abs(resetChild->rect.x-reset->content.x)<0.01f&&
                std::abs(centerChild->rect.x-centered->content.x-100)<0.01f&&
                std::abs(cssChild->rect.x-css->content.x)<0.01f;
        for(const auto& cell:std::initializer_list<std::pair<const wchar_t*,const wchar_t*>>{
            {L"legacy-top",L"legacy-top-child"},{L"legacy-bottom",L"legacy-bottom-child"},
            {L"legacy-middle",L"legacy-middle-child"},{L"legacy-group",L"legacy-group-child"}}){
            const auto* parent=find(cell.first);const auto* child=find(cell.second);
            legacyRowAlignmentValid=legacyRowAlignmentValid&&parent&&child;
            if(!parent||!child)continue;
            const bool top=std::wstring(cell.first)==L"legacy-top";
            const bool middle=std::wstring(cell.first)==L"legacy-middle";
            const float expected=top?0.0f:middle?32.0f:64.0f;
            legacyRowAlignmentValid=legacyRowAlignmentValid&&
                std::abs(child->rect.y-parent->content.y-expected)<0.01f&&
                parent->style.Is(L"vertical-align",top?L"top":middle?L"middle":L"bottom");
        }
    }
    Check(legacyBlockAlignmentValid,
        L"HTML align positions nested percent-width blocks, resets and auto margins while author text-align only aligns text at both DPIs");
    Check(legacyRowAlignmentValid,
        L"table cells inherit row and row-group valign with cell and author overrides at both DPIs");

    for(const float scale:{1.0f,1.5f}){
        ScriptDialog confirmDialog;
        const std::wstring prompt=L"\ub85c\uadf8\uc778 \uc774\ud6c4\uc5d0 \uc774\uc6a9 \uac00\ub2a5\ud569\ub2c8\ub2e4. "
            L"\ub85c\uadf8\uc778 \ud398\uc774\uc9c0\ub85c \uc774\ub3d9\ud558\uc2dc\uaca0\uc2b5\ub2c8\uae4c?";
        Check(confirmDialog.Initialize(L"www.ppomppu.co.kr",prompt,true,true),
            L"the common confirm HTML/CSS surface initializes");
        confirmDialog.Layout(1182,660,scale);
        const auto raster=CaptureBoxRaster(confirmDialog.layout,scale,1182,660);
        const auto* card=confirmDialog.layout.BoxFor(confirmDialog.card);
        const auto* ok=confirmDialog.layout.BoxFor(confirmDialog.okButton);
        const auto* cancel=confirmDialog.layout.BoxFor(confirmDialog.cancelButton);
        Check(raster.rendered&&card&&ok&&cancel&&std::abs(card->rect.x-367)<0.01f&&
            std::abs(card->rect.width-448)<0.01f&&card->rect.y==12&&
            ok->rect.y+ok->rect.height<=card->rect.y+card->rect.height&&
            cancel->rect.y+cancel->rect.height<=card->rect.y+card->rect.height&&
            raster.ColorAt(card->rect.x+5,card->rect.y+60,scale)==0x00ffffff&&
            raster.ColorAt(ok->rect.x+6,ok->rect.y+6,scale)==0x001967d2,
            L"browser confirm card, CSS buttons and padding paint at 100 and 150 percent DPI");
        if(ok&&cancel){
            const auto hit=[&](const LayoutRect& rect){return confirmDialog.ButtonAt(
                std::round((rect.x+rect.width/2)*scale)/scale,
                std::round((rect.y+rect.height/2)*scale)/scale);};
            Check(hit(ok->rect)==0&&hit(cancel->rect)==1,
                L"both browser dialog buttons hit their painted device-pixel centers at both DPIs");
        }
        SaveRasterBmp(std::filesystem::path(L"TWebFrame2/tests/artifacts")/
            (scale==1?L"script-dialog-confirm-100.bmp":L"script-dialog-confirm-150.bmp"),
            raster.width,static_cast<UINT>(raster.pixels.size()/std::max(1u,raster.width)),raster.pixels.data());
        confirmDialog.document.GetElementById(L"script-dialog-message")->SetInnerText(
            L"<b>Literal text</b>\nA second line with a very long message that should remain scrollable.\n"
            L"More text\nMore text\nMore text\nMore text\nMore text\nMore text");
        confirmDialog.Layout(320,120,scale);
        card=confirmDialog.layout.BoxFor(confirmDialog.card);
        ok=confirmDialog.layout.BoxFor(confirmDialog.okButton);
        Check(card&&ok&&card->rect.x>=0&&card->rect.x+card->rect.width<=320&&
            ok->rect.y+ok->rect.height<=120&&
            confirmDialog.document.QuerySelector(L"b")==nullptr,
            L"small browser viewports retain visible dialog actions and treat message markup as text at both DPIs");
        ScriptDialog alertDialog;
        Check(alertDialog.Initialize(L"example.test",L"An alert from JavaScript",false,false),
            L"the common alert HTML/CSS surface initializes");
        alertDialog.Layout(640,280,scale);
        const auto alertRaster=CaptureBoxRaster(alertDialog.layout,scale,640,280);
        const auto* alertOk=alertDialog.layout.BoxFor(alertDialog.okButton);
        const auto* alertCancel=alertDialog.layout.BoxFor(alertDialog.cancelButton);
        Check(alertRaster.rendered&&alertOk&&
            (!alertCancel||!alertCancel->visible)&&
            alertDialog.ButtonAt(alertOk->rect.x+8,alertOk->rect.y+8)==0,
            L"alert renders a single working confirmation action at both DPIs");
        SaveRasterBmp(std::filesystem::path(L"TWebFrame2/tests/artifacts")/
            (scale==1?L"script-dialog-alert-100.bmp":L"script-dialog-alert-150.bmp"),
            alertRaster.width,static_cast<UINT>(alertRaster.pixels.size()/std::max(1u,alertRaster.width)),alertRaster.pixels.data());
    }

    {
        Document eventHost;
        auto eventChild=std::make_shared<Document>();
        Check(eventHost.Parse(L"<!doctype html><iframe id='event-frame'></iframe>",&error),
            L"cross-document event host parses");
        Check(eventChild->Parse(L"<!doctype html><style>html,body{margin:0}div{width:140px;height:50px}</style>"
            L"<body contenteditable='true'><div id='event-pad'>Editable</div></body>",&error),
            L"cross-document editable child parses");
        JavaScriptRuntime hostRuntime(eventHost),childRuntime(*eventChild);
        hostRuntime.SetFrameDocumentProvider([&](const auto&){return eventChild;});
        std::wstring eventTrace,confirmationText;
        bool acceptConfirmation=false;
        unsigned confirmationCount=0;
        const auto trace=[&](const std::wstring& value){eventTrace+=value+L"|";};
        hostRuntime.SetMessageSink(trace);childRuntime.SetMessageSink(trace);
        hostRuntime.SetConfirmSink([&](const std::wstring& value){
            confirmationText=value;++confirmationCount;return acceptConfirmation;
        });
        Check(childRuntime.Load(LR"JS(
            document.getElementById('event-pad').addEventListener('click',function(){chrome.webview.postMessage('child-node');});
            document.addEventListener('click',function(){chrome.webview.postMessage('child-document');});
        )JS",&error),L"child realm listeners load");
        Check(hostRuntime.Load(LR"JS(
            var foreignDocument=document.getElementById('event-frame').contentDocument;
            var accepted=0,hostClicks=0,lastChildEvent;
            document.addEventListener('click',function(){hostClicks++;});
            foreignDocument.addEventListener('click',function(){chrome.webview.postMessage('parent-capture');},true);
            foreignDocument.getElementById('event-pad').addEventListener('click',function(e){
                chrome.webview.postMessage('parent-node');
                if(this!==foreignDocument.getElementById('event-pad')||e.currentTarget!==this)throw new Error('node identity');
            });
            function foreignClick(e){
                lastChildEvent=e;
                if(this!==foreignDocument||e.currentTarget!==foreignDocument||e.eventPhase!==3||
                   e.target!==foreignDocument.getElementById('event-pad'))throw new Error('document identity');
                chrome.webview.postMessage('parent-document');
                if(confirm('Sign in before writing?'))accepted++;
            }
            foreignDocument.addEventListener('click',foreignClick);
        )JS",&error),L"parent realm listeners bind to the child document");
        StyleSheet childStyle;Check(childStyle.Parse(eventChild->StyleText(),&error),L"editable event child CSS parses");
        LayoutEngine childLayout(*eventChild,childStyle);
        bool sharedClicksValid=true;
        for(const float scale:{1.0f,1.5f}){
            childLayout.Layout(200,100,scale);
            hostRuntime.SetDevicePixelRatio(scale);childRuntime.SetDevicePixelRatio(scale);
            const float x=std::round(80*scale)/scale,y=std::round(30*scale)/scale;
            const auto target=childLayout.HitTest(x,y);
            eventTrace.clear();acceptConfirmation=scale==1.5f;
            JavaScriptRuntime::EventInit event;event.clientX=x;event.clientY=y;
            const bool canceled=childRuntime.DispatchNodeEvent(target,L"click",event);
            sharedClicksValid=sharedClicksValid&&target&&target->Attribute(L"id")==L"event-pad"&&!canceled&&
                eventTrace==L"parent-capture|child-node|parent-node|child-document|parent-document|"&&
                confirmationText==L"Sign in before writing?";
            std::wstring result;
            sharedClicksValid=sharedClicksValid&&hostRuntime.Execute(
                L"return lastChildEvent.target===foreignDocument.getElementById('event-pad')&&lastChildEvent.currentTarget===null;",
                &result,&error)&&result==L"true";
        }
        Check(sharedClicksValid&&confirmationCount==2&&hostRuntime.LastError().empty(),
            L"native child clicks invoke parent callbacks in registration order with correct identity and confirm results at both DPIs");
        std::wstring sharedEventResult;
        Check(hostRuntime.Execute(L"return accepted+'|'+hostClicks+'|'+(window.confirm===confirm);",&sharedEventResult,&error)&&
            sharedEventResult==L"1|0|true",L"confirm cancellation and acceptance preserve the caller's JavaScript state");
        eventTrace.clear();hostRuntime.DispatchNodeEvent(eventHost.Body(),L"click");
        Check(eventTrace.empty()&&confirmationCount==2&&hostRuntime.Execute(L"return hostClicks;",&sharedEventResult,&error)&&sharedEventResult==L"1",
            L"a parent document click never invokes listeners registered on an iframe document");
        Check(hostRuntime.Execute(LR"JS(
            foreignDocument.removeEventListener('click',foreignClick);
            foreignDocument.addEventListener('click',function(e){e.preventDefault();e.stopImmediatePropagation();
                chrome.webview.postMessage('cancel-once');},{capture:true,once:true});
        )JS",&sharedEventResult,&error),L"cross-document listener removal and once cancellation register");
        eventTrace.clear();
        const bool canceledOnce=childRuntime.DispatchNodeEvent(eventChild->GetElementById(L"event-pad"),L"click");
        Check(canceledOnce&&eventTrace==L"parent-capture|cancel-once|",
            L"cross-document cancellation stops child listeners and applies to the native default action");
        eventTrace.clear();childRuntime.DispatchNodeEvent(eventChild->GetElementById(L"event-pad"),L"click");
        Check(eventTrace==L"parent-capture|child-node|parent-node|child-document|"&&confirmationCount==2,
            L"once listeners are removed before invocation and removed document callbacks stay removed");
    }

    Document pointerHitDoc;
    Check(pointerHitDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.stage{position:relative;width:200px;height:100px}.target{width:60px;height:30px}.overlay{position:absolute;inset:0;pointer-events:none}.host{position:relative;width:40px;height:20px}.popup{position:absolute;left:0;top:20px;width:100px;height:40px}.stack{position:relative;width:100px;height:50px;margin-left:150px}.front,.back{position:absolute;inset:0}.front{z-index:2}</style>"
          L"<div class='stage'><button id='pointer-target' class='target'>Target</button><div class='overlay'></div></div><div class='host'><button id='overflow-popup' class='popup'>Popup</button></div><div class='stack'><button id='z-front' class='front'>Front</button><button id='z-back' class='back'>Back</button></div>",&error),
          L"pointer hit-testing fixture parses");
    StyleSheet pointerHitCss;Check(pointerHitCss.Parse(pointerHitDoc.StyleText(),&error),L"pointer hit-testing CSS parses");
    LayoutEngine pointerHitLayout(pointerHitDoc,pointerHitCss);pointerHitLayout.Layout(300,220);
    const auto pointerTarget=pointerHitDoc.GetElementById(L"pointer-target");
    const auto overflowPopup=pointerHitDoc.GetElementById(L"overflow-popup");
    const auto zFront=pointerHitDoc.GetElementById(L"z-front");
    const auto* pointerTargetBox=pointerHitLayout.BoxFor(pointerTarget);
    const auto* overflowPopupBox=pointerHitLayout.BoxFor(overflowPopup);
    Check(pointerTargetBox&&pointerHitLayout.HitTest(pointerTargetBox->rect.x+10,pointerTargetBox->rect.y+10)==pointerTarget,
          L"pointer-events none overlay lets pointer input reach the element underneath");
    Check(overflowPopupBox&&pointerHitLayout.HitTest(overflowPopupBox->rect.x+10,overflowPopupBox->rect.y+10)==overflowPopup,
          L"overflow-visible positioned descendants remain clickable outside the parent border");
    const auto* zFrontBox=pointerHitLayout.BoxFor(zFront);
    Check(zFrontBox&&pointerHitLayout.HitTest(zFrontBox->rect.x+10,zFrontBox->rect.y+10)==zFront,
          L"hit testing follows z-index stacking instead of raw DOM order");

    Document nestedStackingDoc;
    Check(nestedStackingDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.root{position:relative;width:200px;height:100px}.early{position:relative;width:40px;height:20px}.popup{position:absolute;left:0;top:20px;width:100px;height:40px;z-index:3}.cover{position:absolute;left:0;top:20px;width:100px;height:40px}</style>"
          L"<div class='root'><div class='early'><button id='nested-popup' class='popup'>Popup</button></div><button id='later-cover' class='cover'>Cover</button></div>",&error),
          L"nested stacking-context fixture parses");
    StyleSheet nestedStackingCss;Check(nestedStackingCss.Parse(nestedStackingDoc.StyleText(),&error),L"nested stacking-context CSS parses");
    LayoutEngine nestedStackingLayout(nestedStackingDoc,nestedStackingCss);nestedStackingLayout.Layout(200,100);
    const auto nestedPopup=nestedStackingDoc.GetElementById(L"nested-popup");
    const auto* nestedPopupBox=nestedStackingLayout.BoxFor(nestedPopup);
    Check(nestedPopupBox&&nestedStackingLayout.HitTest(nestedPopupBox->rect.x+10,nestedPopupBox->rect.y+10)==nestedPopup,
          L"a positive z-index descendant stacks above later content outside its non-stacking ancestor");

    Document popupScrollChainDoc;
    Check(popupScrollChainDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.root{position:relative;width:200px;height:100px;overflow:hidden}.early{position:relative;width:20px;height:20px}.popup{position:absolute;left:10px;top:10px;width:100px;height:50px;overflow-y:auto;z-index:3}.popup-fill{height:150px}.main-scroll{position:absolute;inset:0;overflow-y:auto}.main-fill{height:300px}</style>"
          L"<div class='root'><div class='early'><div id='popup-scroll' class='popup'><div class='popup-fill'></div></div></div><main id='covered-main-scroll' class='main-scroll'><div class='main-fill'></div></main></div>",&error),
          L"popup scroll-chain fixture parses");
    StyleSheet popupScrollChainCss;Check(popupScrollChainCss.Parse(popupScrollChainDoc.StyleText(),&error),L"popup scroll-chain CSS parses");
    LayoutEngine popupScrollChainLayout(popupScrollChainDoc,popupScrollChainCss);popupScrollChainLayout.Layout(200,100);
    const auto popupScroll=popupScrollChainDoc.GetElementById(L"popup-scroll");
    const auto coveredMainScroll=popupScrollChainDoc.GetElementById(L"covered-main-scroll");
    const auto* popupScrollBox=popupScrollChainLayout.BoxFor(popupScroll);
    const float popupScrollX=popupScrollBox?popupScrollBox->content.x+20:0;
    const float popupScrollY=popupScrollBox?popupScrollBox->content.y+20:0;
    std::shared_ptr<Node> popupScrolledNode;
    const bool popupFirstWheel=popupScrollChainLayout.ScrollAt(popupScrollX,popupScrollY,-120,&popupScrolledNode);
    const float coveredMainAfterFirstWheel=coveredMainScroll?coveredMainScroll->scrollTop:-1;
    popupScrollChainLayout.ScrollAt(popupScrollX,popupScrollY,-120,&popupScrolledNode);
    popupScrolledNode.reset();
    const bool popupBoundaryWheel=popupScrollChainLayout.ScrollAt(popupScrollX,popupScrollY,-120,&popupScrolledNode);
    Check(popupScrollBox&&popupFirstWheel&&popupScrolledNode==nullptr&&
          popupScroll->scrollTop>=popupScrollBox->scrollHeight-popupScrollBox->content.height-0.01f&&
          coveredMainAfterFirstWheel==0&&coveredMainScroll&&coveredMainScroll->scrollTop==0&&!popupBoundaryWheel,
          L"wheel input over a popup scrolls only its hit-target chain and never a covered sibling at the boundary");

    Document fixedInsetDoc;
    Check(fixedInsetDoc.Parse(L"<style>*{box-sizing:border-box;margin:0;padding:0}body{display:grid}.fixed{position:fixed;right:18px;bottom:18px;width:50px;height:20px}</style><div id='fixed-inset' class='fixed'></div>",&error),
          L"fixed right-bottom inset fixture parses");
    StyleSheet fixedInsetCss;Check(fixedInsetCss.Parse(fixedInsetDoc.StyleText(),&error),L"fixed right-bottom inset CSS parses");
    LayoutEngine fixedInsetLayout(fixedInsetDoc,fixedInsetCss);fixedInsetLayout.Layout(300,200);
    const auto* fixedInset=FindLayout(fixedInsetLayout.Root(),L"fixed-inset");
    Check(fixedInset&&std::lround(fixedInset->rect.x)==232&&std::lround(fixedInset->rect.y)==162,
          L"fixed descendants of grid containers honor right and bottom insets");
    const auto fixedInsetNode=fixedInsetDoc.GetElementById(L"fixed-inset");
    const auto* fixedInsetRoot=fixedInsetLayout.Root();bool fixedInsetGeometryChanged=false;
    if(fixedInsetNode)fixedInsetNode->inlineStyle[L"bottom"]=L"28px";
    const bool fixedInsetRequiresLayout=
        fixedInsetLayout.Restyle(fixedInsetNode,&fixedInsetGeometryChanged);
    fixedInset=FindLayout(fixedInsetLayout.Root(),L"fixed-inset");
    Check(!fixedInsetRequiresLayout&&fixedInsetGeometryChanged&&
          fixedInsetLayout.Root()==fixedInsetRoot&&fixedInset&&
          std::lround(fixedInset->rect.x)==232&&std::lround(fixedInset->rect.y)==152,
          L"changing a fixed box inset translates only that laid-out subtree");

    Document transitionDoc;
    Check(transitionDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}.workspace{width:300px;height:100px;display:grid;grid-template-columns:200px minmax(0,1fr);transition:grid-template-columns 200ms linear}.workspace.closed{grid-template-columns:0 minmax(0,1fr)}.panel{min-width:0;overflow:hidden;opacity:1;visibility:visible;transform:translateX(0);transition:opacity 100ms linear,transform 200ms linear,visibility 0s linear 0s}.closed .panel{opacity:0;visibility:hidden;transform:translateX(-20px);transition:opacity 100ms linear,transform 200ms linear,visibility 0s linear 200ms}</style>"
          L"<div id='transition-workspace' class='workspace'><aside id='transition-panel' class='panel'></aside><main></main></div>",&error),
          L"CSS transition fixture parses");
    StyleSheet transitionCss;Check(transitionCss.Parse(transitionDoc.StyleText(),&error),L"CSS transition stylesheet parses");
    LayoutEngine transitionLayout(transitionDoc,transitionCss);transitionLayout.Layout(300,100);
    auto transitionWorkspace=transitionDoc.GetElementById(L"transition-workspace");
    transitionWorkspace->SetAttribute(L"class",L"workspace closed");transitionLayout.Layout(300,100);
    const auto* transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionLayout.HasActiveTransitions()&&transitionPanel&&std::lround(transitionPanel->rect.width)==200&&
          std::lround(transitionPanel->rect.x)==0&&transitionPanel->style.Get(L"visibility")==L"visible",
          L"CSS transitions retain the starting grid, transform and delayed visibility on the first frame");
    const auto* retainedTransitionRoot=transitionLayout.Root();
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionLayout.Root()==retainedTransitionRoot&&transitionPanel&&std::abs(transitionPanel->rect.width-100)<0.1f&&
          std::abs(transitionPanel->rect.x+10)<0.1f&&
          std::abs(std::stof(transitionPanel->style.Get(L"opacity")))<0.01f&&
          transitionPanel->style.Get(L"visibility")==L"visible",
          L"CSS transitions reuse the box tree while interpolating grid, transform and opacity values");
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(!transitionLayout.HasActiveTransitions()&&transitionPanel&&transitionPanel->rect.width<0.1f&&
          transitionPanel->style.Get(L"visibility")==L"hidden",
          L"CSS transitions commit their target values and delayed discrete state at completion");
    transitionWorkspace->SetAttribute(L"class",L"workspace");transitionLayout.Layout(300,100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionLayout.HasActiveTransitions()&&transitionPanel&&transitionPanel->rect.width<0.1f&&
          transitionPanel->style.Get(L"visibility")==L"visible",
          L"reverse CSS transitions start from the collapsed geometry and reveal immediately");
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(transitionPanel&&std::abs(transitionPanel->rect.width-100)<0.1f&&
          std::abs(transitionPanel->rect.x+10)<0.1f,
          L"reverse CSS transitions interpolate back toward the expanded layout");
    transitionLayout.AdvanceTransitions(100);
    transitionPanel=transitionLayout.BoxFor(transitionDoc.GetElementById(L"transition-panel"));
    Check(!transitionLayout.HasActiveTransitions()&&transitionPanel&&std::abs(transitionPanel->rect.width-200)<0.1f&&
          std::abs(transitionPanel->rect.x)<0.1f,
          L"reverse CSS transitions finish at the expanded layout");

    Document animationDoc;
    Check(animationDoc.Parse(L"<style>@keyframes Turn{100%{transform:rotate(360deg)}}@keyframes fade{from{opacity:0}to{opacity:1}}"
        L"#moving{width:40px;height:20px;animation:Turn 1s linear infinite}#fading{animation:fade 1s .2s linear both}</style>"
        L"<div id='moving'></div><div id='fading'></div>",&error),L"CSS keyframe animation fixture parses");
    StyleSheet animationCss;Check(animationCss.Parse(animationDoc.StyleText(),&error),L"CSS keyframe rules parse");
    LayoutEngine animationLayout(animationDoc,animationCss);animationLayout.Layout(200,100);
    const auto movingNode=animationDoc.GetElementById(L"moving"),fadingNode=animationDoc.GetElementById(L"fading");
    Check(animationLayout.HasActiveTransitions()&&animationLayout.BoxFor(movingNode)->style.Get(L"transform")==L"rotate(0deg)"&&
        animationLayout.BoxFor(fadingNode)->style.Get(L"opacity")==L"0",L"CSS animations synthesize underlying endpoints and backwards fill before a delay");
    animationLayout.AdvanceTransitions(250);
    Check(std::abs(std::stof(animationLayout.BoxFor(movingNode)->style.Get(L"transform").substr(7))-90)<.01f&&
        std::abs(std::stof(animationLayout.BoxFor(fadingNode)->style.Get(L"opacity"))-.05f)<.01f,
        L"CSS keyframe transform and opacity animation advance on the rendering timeline");
    animationLayout.Layout(200,100);
    animationLayout.AdvanceTransitions(1000);
    Check(std::abs(std::stof(animationLayout.BoxFor(movingNode)->style.Get(L"transform").substr(7))-90)<.01f&&
        animationLayout.BoxFor(fadingNode)->style.Get(L"opacity")==L"1",
        L"Infinite CSS animation retains time across layout rebuilds while finite animation preserves forwards fill");
    movingNode->SetAttribute(L"style",L"animation-play-state: paused");animationLayout.Layout(200,100);
    const auto pausedTransform=animationLayout.BoxFor(movingNode)->style.Get(L"transform");
    animationLayout.AdvanceTransitions(500);
    Check(animationLayout.BoxFor(movingNode)->style.Get(L"transform")==pausedTransform&&!animationLayout.HasActiveTransitions(),
        L"Paused CSS animations preserve their current frame and stop requesting rendering ticks");

    Document buttonDoc;
    Check(buttonDoc.Parse(L"<style>#override{text-align:right}</style><div id='parent'><button id='default'>Default</button><button id='override'>Override</button></div>",&error),
          L"button alignment fixture parses");
    StyleSheet buttonCss;Check(buttonCss.Parse(buttonDoc.StyleText(),&error),L"button alignment CSS parses");
    const auto parentButtonStyle=buttonCss.Compute(buttonDoc.GetElementById(L"parent"));
    const auto defaultButtonStyle=buttonCss.Compute(buttonDoc.GetElementById(L"default"),&parentButtonStyle);
    const auto overrideButtonStyle=buttonCss.Compute(buttonDoc.GetElementById(L"override"),&parentButtonStyle);
    Check(defaultButtonStyle.Get(L"text-align")==L"center"&&defaultButtonStyle.Get(L"white-space")==L"nowrap",
          L"button user-agent alignment and no-wrap behavior override inherited defaults");
    Check(overrideButtonStyle.Get(L"text-align")==L"right",L"author text alignment overrides the button user-agent default");

    Document emptyInputCaretDoc;
    Check(emptyInputCaretDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}input{display:block;width:180px;height:32px;padding:0 9px;border:1px solid #ccc;font:13px/1.42 'Segoe UI'}</style>"
          L"<input id='empty-password' type='password'><input id='filled-password' type='password' value='x'>",
          &error),L"empty input caret fixture parses");
    StyleSheet emptyInputCaretCss;
    Check(emptyInputCaretCss.Parse(emptyInputCaretDoc.StyleText(),&error),
          L"empty input caret CSS parses");
    LayoutEngine emptyInputCaretLayout(emptyInputCaretDoc,emptyInputCaretCss);
    emptyInputCaretLayout.Layout(240,100);
    const auto emptyPassword=emptyInputCaretDoc.GetElementById(L"empty-password");
    const auto filledPassword=emptyInputCaretDoc.GetElementById(L"filled-password");
    const auto* emptyPasswordBox=emptyInputCaretLayout.BoxFor(emptyPassword);
    const auto* filledPasswordBox=emptyInputCaretLayout.BoxFor(filledPassword);
    LayoutRect emptyPasswordCaret{},filledPasswordCaret{};
    const bool hasEmptyPasswordCaret=emptyInputCaretLayout.TextCaretRect(
        emptyPassword,0,emptyPasswordCaret);
    const bool hasFilledPasswordCaret=emptyInputCaretLayout.TextCaretRect(
        filledPassword,0,filledPasswordCaret);
    Check(hasEmptyPasswordCaret&&hasFilledPasswordCaret&&emptyPasswordBox&&filledPasswordBox&&
          std::abs((emptyPasswordCaret.y-emptyPasswordBox->content.y)-
                   (filledPasswordCaret.y-filledPasswordBox->content.y))<0.01f&&
          std::abs(emptyPasswordCaret.height-filledPasswordCaret.height)<0.01f,
          L"an empty single-line input caret uses the same centered line box as entered text");

    Document intrinsicMenuDoc;
    Check(intrinsicMenuDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0}.popup{position:absolute;width:max-content;min-width:220px;padding:5px}"
        L".popup button{display:block;width:100%;padding:5px 28px 5px 10px;white-space:pre;font:12px Arial;text-align:left}"
        L".tab-probe{display:inline-block;white-space:pre;font:16px Arial}.tab-two{tab-size:2}.tab-eight{tab-size:8}</style>"
        L"<div id='intrinsic-popup' class='popup'><button>Save document under another name...\tCtrl+Shift+S</button></div>"
        L"<span id='tab-two' class='tab-probe tab-two'>A\tB</span>"
        L"<span id='tab-eight' class='tab-probe tab-eight'>A\tB</span>",
        &error),L"intrinsic menu and tab-size fixture parses");
    StyleSheet intrinsicMenuCss;
    Check(intrinsicMenuCss.Parse(intrinsicMenuDoc.StyleText(),&error),
          L"intrinsic menu and tab-size CSS parses");
    LayoutEngine intrinsicMenuLayout(intrinsicMenuDoc,intrinsicMenuCss);
    intrinsicMenuLayout.Layout(800,200);
    const auto* intrinsicPopup=FindLayout(intrinsicMenuLayout.Root(),L"intrinsic-popup");
    const auto* tabTwo=FindLayout(intrinsicMenuLayout.Root(),L"tab-two");
    const auto* tabEight=FindLayout(intrinsicMenuLayout.Root(),L"tab-eight");
    Check(intrinsicPopup&&intrinsicPopup->rect.width>300,
          L"width max-content uses the widest unwrapped child instead of the minimum width");
    if(tabTwo&&tabEight&&tabEight->rect.width<=tabTwo->rect.width+10)
        std::wcerr<<L"tab-size widths: "<<tabTwo->rect.width<<L", "<<tabEight->rect.width<<L"\n";
    Check(tabTwo&&tabEight&&tabEight->rect.width>tabTwo->rect.width+10,
          L"tab-size changes preserved-tab text measurement and inline intrinsic width");
    const auto tabText=intrinsicMenuDoc.QuerySelector(L"#tab-eight")->children.front();
    const auto tabTextStyle=intrinsicMenuCss.Compute(tabText,&tabEight->style);
    Check(tabTextStyle.Get(L"tab-size")==L"8",L"tab-size inherits into text runs");

    Document checkDoc;
    Check(checkDoc.Parse(L"<style>html,body{margin:0}*{box-sizing:border-box}.check{width:100px;height:25px;display:flex;align-items:center;gap:7px}.check input{width:15px;height:15px}</style><label class='check'><input id='check' type='checkbox'><span id='check-label'>Choice</span></label>",&error),
          L"checkbox user-agent fixture parses");
    StyleSheet checkCss;Check(checkCss.Parse(checkDoc.StyleText(),&error),L"checkbox user-agent CSS parses");
    const auto checkStyle=checkCss.Compute(checkDoc.GetElementById(L"check"));
    Check(checkStyle.Get(L"margin")==L"3px 3px 3px 4px",
          L"checkbox keeps Chromium user-agent margins when author CSS sets its size");
    LayoutEngine checkLayout(checkDoc,checkCss);checkLayout.Layout(200,60);
    const auto* checkBox=FindLayout(checkLayout.Root(),L"check");
    const auto* checkLabel=FindLayout(checkLayout.Root(),L"check-label");
    Check(checkBox&&checkLabel&&std::lround(checkBox->rect.x)==4&&
          std::lround(checkLabel->rect.x)==29,
          L"checkbox margins and flex gap place following label text like Chromium");

    Document hiddenInputDoc;
    Check(hiddenInputDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}input{display:block!important}"
        L"table{width:900px;table-layout:fixed}td{padding:0}textarea{display:inline-block}</style>"
        L"<form><input id='hidden-page' type='hidden' value='1'>"
        L"<input type='HIDDEN' value='freeboard'><input type='hidden' value='10133266'>"
        L"<table><tr><td><textarea id='comment-memo' style='width:99%;height:122px'>"
        L"Login is required.</textarea></td></tr></table></form>",
        &error),L"hidden comment-form input fixture parses");
    StyleSheet hiddenInputCss;
    Check(hiddenInputCss.Parse(hiddenInputDoc.StyleText(),&error),
          L"hidden comment-form input CSS parses");
    const auto hiddenInputStyle=hiddenInputCss.Compute(
        hiddenInputDoc.GetElementById(L"hidden-page"));
    Check(hiddenInputStyle.Is(L"display",L"none"),
          L"input type hidden remains non-rendered despite a broad author display rule");
    LayoutEngine hiddenInputLayout(hiddenInputDoc,hiddenInputCss);
    bool hiddenCommentFormValid=true;
    for(const float scale:{1.0f,1.5f}){
        hiddenInputLayout.Layout(1000,300,scale);
        const auto* hiddenInput=hiddenInputLayout.BoxFor(
            hiddenInputDoc.GetElementById(L"hidden-page"));
        const auto* commentMemo=hiddenInputLayout.BoxFor(
            hiddenInputDoc.GetElementById(L"comment-memo"));
        hiddenCommentFormValid=hiddenCommentFormValid&&hiddenInput&&commentMemo&&
            !hiddenInput->visible&&commentMemo->rect.y<5.0f&&
            commentMemo->rect.width>880.0f&&
            std::abs(commentMemo->rect.height-122.0f)<0.75f;
    }
    Check(hiddenCommentFormValid,
          L"hidden form values do not create rows and the comment textarea keeps its percentage width and fixed height at 100 and 150 percent DPI");

    Document commonEditorLayoutDoc;
    Check(commonEditorLayoutDoc.Parse(LR"HTML(
        <!doctype html><style>
        *{margin:0;padding:0}body{font:12px/160% Arial}
        table{width:600px;table-layout:fixed;border-collapse:collapse}
        #leading{width:100px;padding-right:5px}#percent-a{width:80%}#percent-b{width:45%}
        #control{width:85px;height:35px;border:1px solid;padding:2px}
        #points{font-size:8pt}#inherited-length{font-size:24px}
        #legacy-pseudo:before{content:'icon';display:inline-block;width:20px;height:18px}
        #inline-height{height:100px;line-height:20px}
        </style><table><tr><td id='leading'></td><td></td></tr></table>
        <table><tr><td id='percent-a'>a</td><td id='percent-b'>b</td></tr></table>
        <input type='submit' id='control' value='save'>
        <div id='points'>small</div><div id='inherited-length'>large</div>
        <a id='legacy-pseudo'>link</a><span id='inline-height'>text</span>
    )HTML",&error),L"common editor CSS rule fixture parses");
    StyleSheet commonEditorCss;Check(commonEditorCss.Parse(commonEditorLayoutDoc.StyleText(),&error),
          L"common editor CSS rule fixture styles parse");
    LayoutEngine commonEditorLayout(commonEditorLayoutDoc,commonEditorCss);
    bool commonEditorRulesValid=true;
    for(const float scale:{1.0f,1.5f}){
        commonEditorLayout.Layout(700,400,scale);
        const auto* leading=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"leading"));
        const auto* percentA=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"percent-a"));
        const auto* percentB=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"percent-b"));
        const auto* control=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"control"));
        const auto* points=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"points"));
        const auto* inherited=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"inherited-length"));
        const auto* legacy=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"legacy-pseudo"));
        const auto* inlineHeight=commonEditorLayout.BoxFor(commonEditorLayoutDoc.GetElementById(L"inline-height"));
        commonEditorRulesValid=commonEditorRulesValid&&leading&&percentA&&percentB&&control&&points&&inherited&&legacy&&inlineHeight&&
            std::abs(leading->rect.width-105)<0.1f&&
            std::abs(percentA->rect.width-384)<0.1f&&std::abs(percentB->rect.width-216)<0.1f&&
            std::abs(control->rect.width-85)<0.1f&&std::abs(control->rect.height-35)<0.1f&&
            std::abs(StyleSheet::Length(points->style.Get(L"font-size"),16,16)-10.666667f)<0.01f&&
            std::abs(StyleSheet::Length(inherited->style.Get(L"line-height"),24,24)-19.2f)<0.01f&&
            FindPseudo(legacy,L"before")&&inlineHeight->rect.height<30;
    }
    Check(commonEditorRulesValid,
          L"table padding, overcommitted percentage columns, control border boxes, pt fonts, inherited line lengths, legacy pseudo elements and inline heights are stable at 100 and 150 percent DPI");
    Check(std::abs(StyleSheet::Length(L"1in",0,0)-96)<0.01f&&
          std::abs(StyleSheet::Length(L"2.54cm",0,0)-96)<0.01f&&
          std::abs(StyleSheet::Length(L"25.4mm",0,0)-96)<0.01f&&
          std::abs(StyleSheet::Length(L"6pc",0,0)-96)<0.01f&&
          std::abs(StyleSheet::Length(L"101.6q",0,0)-96)<0.01f,
          L"absolute CSS lengths resolve to CSS pixels independently of device DPI");

    Document inlineEditorFlowDoc;
    Check(inlineEditorFlowDoc.Parse(LR"HTML(
        <!doctype html><style>*{margin:0;padding:0}body{font:12px/20px 'Malgun Gothic'}
        table{width:300px;table-layout:fixed;border-collapse:collapse}
        #fixed-wrapper{height:20px}#nested-tabs{height:16px}#floating-tab{float:left;height:23px;width:40px}
        #notice{margin:8px 0}#badge{float:left;height:18px;width:20px}#notice-text{display:block;margin-bottom:-2px}
        </style><table><tr><td id='bfc'><div id='fixed-wrapper'><div id='nested-tabs'><div id='floating-tab'></div></div></div></td></tr></table>
        <table><tr><td id='notice-cell'><font><div id='notice'><span id='badge'></span><span id='notice-text'>notice</span></div></font></td></tr></table>
        <div style='text-align:right'><span><label id='nbsp'><input type='checkbox'>&nbsp;text&nbsp;&nbsp;</label><label id='edge'><input type='checkbox'>&nbsp;text&nbsp;
        </label></span><input type='submit' value='save' style='width:85px;height:35px'></div>
    )HTML",&error),L"nested floats and inline whitespace fixture parses");
    StyleSheet inlineEditorFlowCss;Check(inlineEditorFlowCss.Parse(inlineEditorFlowDoc.StyleText(),&error),
          L"nested floats and inline whitespace styles parse");
    LayoutEngine inlineEditorFlow(inlineEditorFlowDoc,inlineEditorFlowCss);
    bool inlineEditorFlowValid=true;
    for(const float scale:{1.0f,1.5f}){
        inlineEditorFlow.Layout(500,300,scale);
        const auto* cell=inlineEditorFlow.BoxFor(inlineEditorFlowDoc.GetElementById(L"bfc"));
        const auto* notice=inlineEditorFlow.BoxFor(inlineEditorFlowDoc.GetElementById(L"notice-cell"));
        const auto* nbsp=inlineEditorFlow.BoxFor(inlineEditorFlowDoc.GetElementById(L"nbsp"));
        const auto* edge=inlineEditorFlow.BoxFor(inlineEditorFlowDoc.GetElementById(L"edge"));
        std::vector<LayoutRect> inlineGlyph;
        const auto labelNode=inlineEditorFlowDoc.GetElementById(L"nbsp");
        const bool glyphMeasured=inlineEditorFlow.TextRangeRects(labelNode->children.back(),1,1,inlineGlyph);
        inlineEditorFlowValid=inlineEditorFlowValid&&cell&&notice&&nbsp&&edge&&
            std::abs(cell->rect.height-23)<0.01f&&std::abs(notice->rect.height-34)<0.01f&&
            std::abs(nbsp->rect.width-edge->rect.width)<0.01f&&glyphMeasured&&!inlineGlyph.empty()&&
            inlineGlyph.front().x>nbsp->rect.x+16&&inlineGlyph.front().x<nbsp->rect.x+19;
    }
    Check(inlineEditorFlowValid,
          L"BFCs enclose nested visible floats, negative block margins are counted once, NBSP survives and nested trailing spaces precede controls at 100 and 150 percent DPI");

    Document floatDoc;
    Check(floatDoc.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}table{width:640px;table-layout:fixed}"
        L"td{height:40px;padding:6px 10px;font:14px/22px Arial}.comment-btn-layout{float:right}"
        L".vote{display:inline-block;height:22px;padding:1px 7px;border:1px solid #ccc}</style>"
        L"<table><tr><td id='comment-cell'><span>author</span>"
        L"<div id='comment-votes' class='comment-btn-layout'><span class='vote'>3</span>"
        L"<span class='vote'>0</span></div></td></tr></table>",
        &error),L"table-cell float fixture parses");
    StyleSheet floatCss;Check(floatCss.Parse(floatDoc.StyleText(),&error),
          L"table-cell float CSS parses");
    LayoutEngine floatLayout(floatDoc,floatCss);
    const auto verifyFloat=[&](float scale){
        floatLayout.Layout(700,100,scale);
        const auto* cell=floatLayout.BoxFor(floatDoc.GetElementById(L"comment-cell"));
        const auto* votes=floatLayout.BoxFor(floatDoc.GetElementById(L"comment-votes"));
        if(!cell||!votes)return false;
        return votes->style.Is(L"float",L"right")&&votes->rect.width>20&&
            votes->rect.width<160&&
            std::abs((votes->rect.x+votes->rect.width)-
                     (cell->content.x+cell->content.width))<0.75f;
    };
    Check(verifyFloat(1.0f)&&verifyFloat(1.5f),
          L"an auto-width right float in a table cell shrink-wraps at the right edge at 100 and 150 percent DPI");

    Document fixedDoc;
    Check(fixedDoc.Parse(L"<style>*{margin:0;padding:0}table{width:300px;table-layout:fixed}th:first-child{width:50px}th:last-child{width:100px}td:first-child{width:250px}</style><table><thead><tr><th>A</th><th>B</th></tr></thead><tbody><tr><td>Long second-row content</td><td>C</td></tr></tbody></table>",&error),
          L"fixed table fixture parses");
    StyleSheet fixedCss;Check(fixedCss.Parse(fixedDoc.StyleText(),&error),L"fixed table CSS parses");
    LayoutEngine fixedLayout(fixedDoc,fixedCss);fixedLayout.Layout(400,200);
    const auto* firstFixedColumn=fixedLayout.BoxFor(fixedDoc.QuerySelector(L"thead th:first-child"));
    const auto* secondFixedColumn=fixedLayout.BoxFor(fixedDoc.QuerySelector(L"thead th:last-child"));
    Check(firstFixedColumn&&secondFixedColumn&&std::lround(firstFixedColumn->rect.width)==100&&
          std::lround(secondFixedColumn->rect.width)==200,
          L"fixed table columns use the first row instead of later content widths");

    Document presentationalTableDoc;
    Check(presentationalTableDoc.Parse(
          L"<style>*{margin:0;padding:0}table{width:900px;table-layout:fixed}</style>"
          L"<table><tbody><tr><td id='avatar-column' width='100px'><img width='80'></td>"
          L"<td id='comment-column'>Comment body</td></tr></tbody></table>",&error),
          L"presentational table-cell width fixture parses");
    StyleSheet presentationalTableCss;
    Check(presentationalTableCss.Parse(presentationalTableDoc.StyleText(),&error),
          L"presentational table-cell width CSS parses");
    bool presentationalWidthsValid=true;
    LayoutEngine presentationalTableLayout(presentationalTableDoc,presentationalTableCss);
    for(const float scale:{1.0f,1.5f}){
        presentationalTableLayout.Layout(1000,200,scale);
        const auto* avatar=presentationalTableLayout.BoxFor(
            presentationalTableDoc.GetElementById(L"avatar-column"));
        const auto* comment=presentationalTableLayout.BoxFor(
            presentationalTableDoc.GetElementById(L"comment-column"));
        presentationalWidthsValid=presentationalWidthsValid&&avatar&&comment&&
            std::abs(avatar->rect.width-100.0f)<0.01f&&
            std::abs(comment->rect.width-800.0f)<0.01f&&
            std::lround((avatar->rect.x+avatar->rect.width)*scale)==
                std::lround(comment->rect.x*scale);
    }
    Check(presentationalWidthsValid,
          L"fixed tables honor HTML cell widths with contiguous columns at 100 and 150 percent DPI");

    Document doc; error.clear();
    Check(doc.Parse(L"<html><head><style>#x{color:red}.on{display:flex}</style></head><body><div id='x' class='on'>hello <span>world</span></div></body></html>", &error), L"HTML parses");
    Check(doc.GetElementById(L"x") != nullptr, L"id index");
    Check(doc.QuerySelectorAll(L"#x.on span").size() == 1, L"descendant selector");
    StyleSheet css; Check(css.Parse(doc.StyleText(), &error), L"CSS parses");
    auto style = css.Compute(doc.GetElementById(L"x"));
    Check(style.Is(L"display", L"flex"), L"CSS cascade");
    Document focusWithinDoc;Check(focusWithinDoc.Parse(L"<label class='field'><input id='query'></label>",&error),L"focus-within fixture parses");
    const auto focusWithinInput=focusWithinDoc.GetElementById(L"query");
    const auto focusWithinLabel=focusWithinDoc.QuerySelector(L".field");
    focusWithinInput->focused=true;focusWithinInput->focusWithin=true;focusWithinLabel->focusWithin=true;
    Check(Document::MatchesSelector(focusWithinLabel,L".field:focus-within"),L"focus-within matches an ancestor of the focused control");
    focusWithinInput->focused=false;focusWithinInput->focusWithin=false;focusWithinLabel->focusWithin=false;
    Check(!Document::MatchesSelector(focusWithinLabel,L".field:focus-within"),L"focus-within clears when no descendant is focused");

    struct LegacyLineCase { const wchar_t* doctype; bool quirks,limited; };
    const LegacyLineCase legacyLineCases[]={
        {L"<!doctype html>",false,false},
        {L"",true,false},
        {L"<!DOCTYPE HTML PUBLIC '-//W3C//DTD XHTML 1.0 Transitional//EN' 'legacy.dtd'>",false,true},
        {L"<!doctype html public \"-//W3C//DTD XHTML 1.0 Frameset//EN\">",false,true},
        {L"<!doctype html public '-//W3C//DTD HTML 4.01 Transitional//EN' 'legacy.dtd'>",false,true},
        {L"<!doctype html public '-//W3C//DTD HTML 4.01 Frameset//EN'>",true,false},
        {L"<!doctype html public '-//W3C//DTD XHTML 1.0 Strict//EN' 'strict.dtd'>",false,false}
    };
    bool legacyLineModesValid=true,legacyImageLinesValid=true;
    for(const auto& test:legacyLineCases){
        Document legacyLineDoc;
        const std::wstring html=std::wstring(test.doctype)+
            L"<style>html,body{margin:0}.line{font:12px/1.6 'Malgun Gothic';width:60px}"
            L"img{width:16px;height:13.5px;vertical-align:middle}</style>"
            L"<div class='line'><img></div><div class='line'><img>Text</div>";
        Check(legacyLineDoc.Parse(html,&error),L"legacy document line-height fixture parses");
        Document adoptedLineDoc;adoptedLineDoc.AdoptParsed(legacyLineDoc);
        legacyLineModesValid=legacyLineModesValid&&adoptedLineDoc.QuirksMode()==test.quirks&&
            adoptedLineDoc.LimitedQuirksMode()==test.limited;
        StyleSheet legacyLineCss;Check(legacyLineCss.Parse(adoptedLineDoc.StyleText(),&error),
            L"legacy document line-height CSS parses");
        LayoutEngine legacyLineLayout(adoptedLineDoc,legacyLineCss);
        const auto lines=adoptedLineDoc.QuerySelectorAll(L".line");
        const auto images=adoptedLineDoc.GetElementsByTagName(L"img");
        for(const float scale:{1.0f,1.5f}){
            legacyLineLayout.Layout(200,100,scale);
            const auto* imageLine=legacyLineLayout.BoxFor(lines[0]);
            const auto* image=legacyLineLayout.BoxFor(images[0]);
            const auto* mixedLine=legacyLineLayout.BoxFor(lines[1]);
            const bool legacy=test.quirks||test.limited;
            legacyImageLinesValid=legacyImageLinesValid&&imageLine&&image&&mixedLine&&
                std::abs(imageLine->rect.height-(legacy?13.5f:19.2f))<0.01f&&
                std::abs(image->rect.y-imageLine->content.y-(legacy?0.0f:4.171875f))<0.04f&&
                std::abs(mixedLine->rect.height-19.2f)<0.01f;
        }
        Check(adoptedLineDoc.Parse(L"<!doctype html><p>reset</p>",&error)&&
            !adoptedLineDoc.QuirksMode()&&!adoptedLineDoc.LimitedQuirksMode(),
            L"reparsing a standard document clears its previous legacy layout mode");
    }
    Check(legacyLineModesValid,
        L"legacy public and system identifiers distinguish limited quirks, full quirks and standards across adoption");
    Check(legacyImageLinesValid,
        L"legacy image-only inline lines omit the font strut while actual text retains its line height at both DPI scales");

    struct PaddedFlexCase {
        const wchar_t* rules;
        bool column,wrapped,borderBox;
    };
    const PaddedFlexCase paddedFlexCases[]={
        {L"",false,false,false},
        {L".pair{flex-wrap:wrap}",false,false,false},
        {L".pair{flex-direction:column}",true,false,false},
        {L".pair{flex-wrap:wrap;width:60px}",false,true,false},
        {L".pair{flex-direction:column;flex-wrap:wrap;height:60px}",true,true,false},
        {L".pair>span{box-sizing:border-box;width:31px;height:31px}",false,false,true},
        {L".pair>span{flex-basis:15px;width:9px}",false,false,false},
        {L".pair{flex-direction:column}.pair>span{flex-basis:15px;height:9px}",true,false,false}
    };
    bool paddedFlexSizesValid=true,paddedFlexPlacementValid=true;
    for(const auto& test:paddedFlexCases){
        Document paddedFlexDoc;
        const std::wstring html=L"<style>html,body{margin:0}.pair{display:flex;width:100px;height:80px;"
            L"align-items:flex-start;align-content:flex-start;font-size:0}.pair>span{"
            L"width:15px;height:15px;padding:7px;border:1px solid #ccc;border-radius:100%;"
            L"margin:0 5px 5px 0;font-size:12px;flex-shrink:0}img{width:16px;height:16px}"
            +std::wstring(test.rules)+L"</style><div class='pair'><span><img></span><span><img></span></div>";
        Check(paddedFlexDoc.Parse(html,&error),L"padded flex-item fixture parses");
        StyleSheet paddedFlexCss;Check(paddedFlexCss.Parse(paddedFlexDoc.StyleText(),&error),
            L"padded flex-item CSS parses");
        LayoutEngine paddedFlexLayout(paddedFlexDoc,paddedFlexCss);
        const auto nodes=paddedFlexDoc.QuerySelectorAll(L".pair>span");
        for(const float scale:{1.0f,1.5f}){
            paddedFlexLayout.Layout(320,240,scale);
            const auto* first=nodes.size()==2?paddedFlexLayout.BoxFor(nodes[0]):nullptr;
            const auto* second=nodes.size()==2?paddedFlexLayout.BoxFor(nodes[1]):nullptr;
            const float border=std::floor(scale)/scale;
            const float extent=test.borderBox?31.0f:15+14+2*border;
            const float content=extent-14-2*border;
            paddedFlexSizesValid=paddedFlexSizesValid&&first&&second&&
                std::abs(first->rect.width-extent)<0.01f&&std::abs(first->rect.height-extent)<0.01f&&
                std::abs(second->rect.width-extent)<0.01f&&std::abs(second->rect.height-extent)<0.01f&&
                std::abs(first->content.width-content)<0.01f&&
                std::abs(first->content.height-content)<0.01f&&
                std::abs(first->content.x-first->rect.x-7-border)<0.01f&&
                std::abs(first->content.y-first->rect.y-7-border)<0.01f;
            if(first&&second){
                const bool horizontal=test.column==test.wrapped;
                const float advance=extent+5;
                paddedFlexPlacementValid=paddedFlexPlacementValid&&
                    std::abs(second->rect.x-first->rect.x-(horizontal?advance:0))<0.01f&&
                    std::abs(second->rect.y-first->rect.y-(horizontal?0:advance))<0.01f;
            }else paddedFlexPlacementValid=false;
        }
    }
    Check(paddedFlexSizesValid,
        L"flex width, height and basis include content-box edges exactly once at 100 and 150 percent DPI");
    Check(paddedFlexPlacementValid,
        L"padded flex items advance and wrap by their outer size in row and column directions at both DPI scales");

    Document intrinsicFlexDoc;
    Check(intrinsicFlexDoc.Parse(
        L"<style>html,body{margin:0}.row{display:flex;align-items:center;width:300px;height:40px}"
        L".pair{display:flex;font-size:0}.pair>span{display:inline-block;width:15px;height:15px;"
        L"padding:7px;border:1px solid #ccc;border-radius:100%;margin-right:5px;font-size:12px}"
        L".mark{display:inline-block;width:17px;height:17px;margin-top:-1px}"
        L"img{width:16px;height:16px;vertical-align:middle}</style><div class='row'>"
        L"<div class='pair'><span><span class='mark'></span></span><span><img></span></div></div>",&error),
        L"intrinsically sized padded flex buttons fixture parses");
    StyleSheet intrinsicFlexCss;Check(intrinsicFlexCss.Parse(intrinsicFlexDoc.StyleText(),&error),
        L"intrinsically sized padded flex buttons CSS parses");
    LayoutEngine intrinsicFlexLayout(intrinsicFlexDoc,intrinsicFlexCss);
    const auto intrinsicButtons=intrinsicFlexDoc.QuerySelectorAll(L".pair>span");
    bool intrinsicFlexValid=true;
    for(const float scale:{1.0f,1.5f}){
        intrinsicFlexLayout.Layout(320,100,scale);
        const auto* pair=intrinsicFlexLayout.BoxFor(intrinsicFlexDoc.QuerySelector(L".pair"));
        const auto* first=intrinsicFlexLayout.BoxFor(intrinsicButtons[0]);
        const auto* second=intrinsicFlexLayout.BoxFor(intrinsicButtons[1]);
        const float extent=15+14+2*std::floor(scale)/scale;
        intrinsicFlexValid=intrinsicFlexValid&&pair&&first&&second&&
            std::abs(pair->rect.width-2*(extent+5))<0.01f&&
            std::abs(first->rect.width-extent)<0.01f&&std::abs(second->rect.width-extent)<0.01f&&
            std::abs(first->rect.height-extent)<0.01f&&std::abs(second->rect.height-extent)<0.01f;
    }
    Check(intrinsicFlexValid,
        L"auto-width flex parents include content-box decorations in child intrinsic contributions at both DPI scales");
    StyleSheet intrinsicColumnCss;
    Check(intrinsicColumnCss.Parse(intrinsicFlexDoc.StyleText()+L".row{height:80px}.pair{flex-direction:column}",&error),
        L"intrinsic column flex CSS parses");
    LayoutEngine intrinsicColumnLayout(intrinsicFlexDoc,intrinsicColumnCss);
    bool intrinsicColumnValid=true;
    for(const float scale:{1.0f,1.5f}){
        intrinsicColumnLayout.Layout(320,100,scale);
        const auto* pair=intrinsicColumnLayout.BoxFor(intrinsicFlexDoc.QuerySelector(L".pair"));
        const auto* first=intrinsicColumnLayout.BoxFor(intrinsicButtons[0]);
        const auto* second=intrinsicColumnLayout.BoxFor(intrinsicButtons[1]);
        const float extent=15+14+2*std::floor(scale)/scale;
        intrinsicColumnValid=intrinsicColumnValid&&pair&&first&&second&&
            std::abs(pair->rect.width-extent-5)<0.01f&&
            std::abs(first->rect.width-extent)<0.01f&&std::abs(second->rect.width-extent)<0.01f&&
            std::abs(second->rect.y-first->rect.y-extent)<0.01f;
    }
    Check(intrinsicColumnValid,
        L"column flex intrinsic width uses the widest inline-authored item instead of summing their widths");

    Document flexEdgesDoc;
    Check(flexEdgesDoc.Parse(
        L"<style>html,body{margin:0}.shrink{display:flex;width:100px;height:40px}.shrink>span{"
        L"width:50px;height:20px;min-width:0}.shrink>span:first-child{padding:0 10px}"
        L".percent{display:flex;flex-direction:column;width:100px;height:200px;align-items:flex-start}"
        L".percent>span{width:20px;height:20px;padding:10%;margin:2%;flex-shrink:0}</style>"
        L"<div class='shrink'><span></span><span></span></div>"
        L"<div class='percent'><span></span><span></span></div>",&error),
        L"flex shrinking and percentage-edge fixture parses");
    StyleSheet flexEdgesCss;Check(flexEdgesCss.Parse(flexEdgesDoc.StyleText(),&error),
        L"flex shrinking and percentage-edge CSS parses");
    LayoutEngine flexEdgesLayout(flexEdgesDoc,flexEdgesCss);
    const auto shrinkItems=flexEdgesDoc.QuerySelectorAll(L".shrink>span");
    const auto percentItems=flexEdgesDoc.QuerySelectorAll(L".percent>span");
    bool flexEdgesValid=true;
    for(const float scale:{1.0f,1.5f}){
        flexEdgesLayout.Layout(320,320,scale);
        const auto* padded=flexEdgesLayout.BoxFor(shrinkItems[0]);
        const auto* plain=flexEdgesLayout.BoxFor(shrinkItems[1]);
        const auto* percentFirst=flexEdgesLayout.BoxFor(percentItems[0]);
        const auto* percentSecond=flexEdgesLayout.BoxFor(percentItems[1]);
        flexEdgesValid=flexEdgesValid&&padded&&plain&&percentFirst&&percentSecond&&
            std::abs(padded->rect.width-60)<0.01f&&std::abs(plain->rect.width-40)<0.01f&&
            std::abs(padded->content.width-plain->content.width)<0.01f&&
            std::abs(percentFirst->rect.width-40)<0.01f&&std::abs(percentFirst->rect.height-40)<0.01f&&
            std::abs(percentSecond->rect.y-percentFirst->rect.y-44)<0.01f;
    }
    Check(flexEdgesValid,
        L"flex shrink weights exclude padding and percentage edges use the containing width on either axis");

    Document alignmentDoc;Check(alignmentDoc.Parse(
        L"<style>*{box-sizing:border-box}button{display:inline-flex;align-items:center;gap:7px;min-height:36px;padding:0 13px;font-family:'Segoe UI';font-size:14px}.icon{width:15px;height:15px}.copy{display:grid;gap:1px}.copy strong{font-size:13px}.copy small{font-size:11px}</style><body><button id='aligned-button'>\n  <svg class='icon' viewBox='0 0 24 24'></svg> Export\n</button><span id='stacked-copy' class='copy'><strong>Account</strong><small>Developer plan</small></span></body>",
        &error),L"generic flex and grid alignment fixture parses");
    StyleSheet alignmentCss;Check(alignmentCss.Parse(alignmentDoc.StyleText(),&error),L"generic flex and grid alignment CSS parses");
    LayoutEngine alignmentLayout(alignmentDoc,alignmentCss);alignmentLayout.Layout(400,200);
    const auto* alignedButton=FindLayout(alignmentLayout.Root(),L"aligned-button");
    const auto* stackedCopy=FindLayout(alignmentLayout.Root(),L"stacked-copy");
    if(alignedButton&&alignedButton->children.size()>=2){
        const auto& icon=*alignedButton->children[0];const auto& label=*alignedButton->children[1];
        Check(alignedButton->style.Is(L"white-space",L"nowrap")&&
              std::abs((icon.rect.y+icon.rect.height/2)-(label.rect.y+label.rect.height/2))<0.01f,
              L"formatted flex-button HTML collapses source whitespace and vertically centers icon and label boxes");
    }
    if(stackedCopy&&stackedCopy->children.size()>=2){
        const auto& first=*stackedCopy->children[0];const auto& second=*stackedCopy->children[1];
        Check(second.style.Is(L"display",L"inline")&&second.rect.height<19&&
              second.rect.y>=first.rect.y+first.rect.height+0.9f,
              L"small is an inline phrasing element and stacked grid text rows do not overlap");
    }

    Document baselineDoc;
    Check(baselineDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0}.line{display:flex;align-items:baseline;width:200px;height:30px}.small{font-family:'Segoe UI';font-size:10px}.large{font-family:'Segoe UI';font-size:14px}</style><div class='line'><span id='baseline-small' class='small'>SMALL</span><strong id='baseline-large' class='large'>Large</strong></div>",
          &error),L"flex baseline fixture parses");
    StyleSheet baselineCss;Check(baselineCss.Parse(baselineDoc.StyleText(),&error),
          L"flex baseline CSS parses");
    LayoutEngine baselineLayout(baselineDoc,baselineCss);baselineLayout.Layout(240,80);
    const auto* baselineSmall=FindLayout(baselineLayout.Root(),L"baseline-small");
    const auto* baselineLarge=FindLayout(baselineLayout.Root(),L"baseline-large");
    Check(baselineSmall&&baselineLarge&&baselineSmall->rect.y>baselineLarge->rect.y+2.0f&&
          baselineSmall->rect.y+baselineSmall->rect.height<=
              baselineLarge->rect.y+baselineLarge->rect.height+0.5f,
          L"row flex items align their first text baselines across font sizes");

    Document inlineDoc;
    Check(inlineDoc.Parse(L"<style>#bar,#header{display:flex;align-items:center;height:54px}#bar,#spaces{font-size:12px}#header{font-size:15px}#icon{display:inline-flex;width:15px}#spaces{display:flex;gap:8px}</style><div id='bar'><strong id='simple'>DISCONNECTED</strong><span id='mixed'>available: <strong>00</strong></span></div><div id='header'><strong id='title'><span id='icon'>x</span>Saved Data</strong></div><div id='spaces'><strong id='trail'>LINK: </strong><strong>DISCONNECTED</strong><strong id='plain'>LINK:</strong></div>", &error),
          L"inline formatting fixture parses");
    StyleSheet inlineCss; Check(inlineCss.Parse(inlineDoc.StyleText(), &error), L"inline formatting CSS parses");
    LayoutEngine inlineLayout(inlineDoc,inlineCss);inlineLayout.Layout(400,120);
    const auto* simpleInline=FindLayout(inlineLayout.Root(),L"simple");
    const auto* mixedInline=FindLayout(inlineLayout.Root(),L"mixed");
    const auto* titleInline=FindLayout(inlineLayout.Root(),L"title");
    const auto* iconInline=FindLayout(inlineLayout.Root(),L"icon");
    const auto* trailingInline=FindLayout(inlineLayout.Root(),L"trail");
    const auto* plainInline=FindLayout(inlineLayout.Root(),L"plain");
    Check(simpleInline&&mixedInline&&std::lround(simpleInline->rect.height)==16&&
          std::lround(mixedInline->rect.height)==16&&std::abs(simpleInline->rect.y-mixedInline->rect.y)<0.05f,
          L"nested inline content uses the inherited line box instead of an arbitrary minimum height");
    Check(titleInline&&iconInline&&titleInline->children.size()==2&&
          std::abs(iconInline->rect.y-titleInline->children[1]->rect.y)<0.01f&&
          iconInline->rect.x<titleInline->children[1]->rect.x,
          L"inline-flex participates in its parent's inline formatting line");
    Check(trailingInline&&plainInline&&std::abs(trailingInline->rect.width-plainInline->rect.width)<0.01f,
          L"collapsible trailing whitespace is removed at the end of an inline formatting context");

    Document leadingSpaceDoc;
    Check(leadingSpaceDoc.Parse(L"<style>label{display:block;font-size:11px}</style><label id='with-space'><input type='checkbox'> filename</label><label id='without-space'><input type='checkbox'>filename</label>",&error),
          L"leading inline whitespace fixture parses");
    StyleSheet leadingSpaceCss;Check(leadingSpaceCss.Parse(leadingSpaceDoc.StyleText(),&error),L"leading inline whitespace CSS parses");
    LayoutEngine leadingSpaceLayout(leadingSpaceDoc,leadingSpaceCss);leadingSpaceLayout.Layout(400,100);
    const auto* withSpace=FindLayout(leadingSpaceLayout.Root(),L"with-space");
    const auto* withoutSpace=FindLayout(leadingSpaceLayout.Root(),L"without-space");
    const LayoutBox* withSpaceText=withSpace&&withSpace->children.size()==2?withSpace->children[1].get():nullptr;
    const LayoutBox* withoutSpaceText=withoutSpace&&withoutSpace->children.size()==2?withoutSpace->children[1].get():nullptr;
    Check(withSpaceText&&withoutSpaceText&&withSpaceText->rect.width-withoutSpaceText->rect.width>2.5f,
          L"collapsed leading whitespace is preserved between inline siblings");

    Document commonFlowDoc;
    Check(commonFlowDoc.Parse(
        L"<style>*{margin:0}body{font:20px Arial}.line{height:30px}"
        L".stack{width:800px}.frame{width:728px;height:90px;vertical-align:bottom}"
        L".first{height:20px;margin-bottom:10px}.parent{min-height:100px}"
        L".child{height:90px;margin:15px 0}.next{height:10px;margin-top:20px}"
        L".padded{padding-top:5px}.padded>div{height:10px;margin-top:15px}"
        L".flex-flow{display:flex;flex-direction:column}.flex-flow>div{height:10px;margin:10px 0}"
        L".small-count{font:11px/30px Arial;background:#eee;vertical-align:text-top}"
        L".font-edges{font:20px/30px 'Malgun Gothic'}"
        L".font-edges span{font-size:11px;font-family:Tahoma;vertical-align:text-top;background:#eee}"
        L".tool-track{font:0px/1.6 'Malgun Gothic'}.tool-track span{font-size:12px;vertical-align:top}"
        L".tool-track i{display:inline-block;width:19px;height:19px}</style>"
        L"<div id='spaced-badge' class='line'><img width='22' height='11'> <span>Label</span></div>"
        L"<div id='tight-badge' class='line'><img width='22' height='11'><span>Label</span></div>"
        L"<div class='line'>Title <span id='small-count' class='small-count'>47</span></div>"
        L"<div id='font-line' class='font-edges'>Text <span id='font-label'>47</span></div>"
        L"<div id='tool-line' class='tool-track'><span id='tool-label'>Action</span><i></i></div>"
        L"<div id='frame-line' class='stack'><iframe class='frame'></iframe></div>"
        L"<section class='stack'><div id='margin-first' class='first'></div>"
        L"<div id='margin-parent' class='parent'><div id='margin-child' class='child'></div></div>"
        L"<div id='margin-next' class='next'></div></section>"
        L"<div id='margin-padded' class='padded'><div id='margin-padded-child'></div></div>"
        L"<div class='flex-flow'><div id='margin-flex-first'></div><div id='margin-flex-next'></div></div>",
        &error),L"common inline whitespace and block margin fixture parses");
    StyleSheet commonFlowCss;Check(commonFlowCss.Parse(commonFlowDoc.StyleText(),&error),
                                  L"common flow CSS parses");
    LayoutEngine commonFlowLayout(commonFlowDoc,commonFlowCss);
    for(const float scale:{1.0f,1.5f}){
        commonFlowLayout.Layout(960,660,scale);
        const auto* spaced=FindLayout(commonFlowLayout.Root(),L"spaced-badge");
        const auto* tight=FindLayout(commonFlowLayout.Root(),L"tight-badge");
        Check(spaced&&tight&&spaced->children.size()==3&&tight->children.size()==2&&
              spaced->children.back()->rect.x-tight->children.back()->rect.x>4.0f,
              L"whitespace-only text between replaced and inline elements retains one CSS space at both DPIs");
        const auto* count=FindLayout(commonFlowLayout.Root(),L"small-count");
        Check(count&&count->rect.height>=11.0f&&count->rect.height<=15.0f&&
              count->children.size()==1&&count->children.front()->rect.height>=29.0f,
              L"an inline label paints its font-sized background while retaining the containing line-height");
        const auto* fontLine=FindLayout(commonFlowLayout.Root(),L"font-line");
        const auto* fontLabel=FindLayout(commonFlowLayout.Root(),L"font-label");
        Check(fontLine&&fontLabel&&std::abs(fontLine->rect.height-31.0f)<0.01f&&
              std::abs(fontLabel->rect.height-13.0f)<0.01f&&
              std::abs(fontLabel->rect.y-fontLine->rect.y-9.0f)<0.01f,
              L"mixed font sizes use the browser's rounded font edges and odd half-leading at both DPIs");
        const auto* toolLine=FindLayout(commonFlowLayout.Root(),L"tool-line");
        const auto* toolLabel=FindLayout(commonFlowLayout.Root(),L"tool-label");
        Check(toolLine&&toolLabel&&std::abs(toolLabel->rect.y-toolLine->rect.y-1.0f)<0.01f,
              L"top-aligned inline backgrounds retain font half-leading beside atomic action boxes");
        const auto* frameLine=FindLayout(commonFlowLayout.Root(),L"frame-line");
        Check(frameLine&&std::abs(frameLine->rect.height-90.0f)<0.01f,
              L"a bottom-aligned replaced element determines intrinsic line height without baseline descent");
        const auto* first=FindLayout(commonFlowLayout.Root(),L"margin-first");
        const auto* parent=FindLayout(commonFlowLayout.Root(),L"margin-parent");
        const auto* child=FindLayout(commonFlowLayout.Root(),L"margin-child");
        const auto* next=FindLayout(commonFlowLayout.Root(),L"margin-next");
        Check(first&&parent&&child&&next&&std::abs(parent->rect.height-100.0f)<0.01f&&
              std::abs(parent->rect.y-first->rect.y-first->rect.height-15.0f)<0.01f&&
              std::abs(child->rect.y-parent->rect.y)<0.01f&&
              std::abs(next->rect.y-parent->rect.y-parent->rect.height-20.0f)<0.01f,
              L"adjoining sibling and parent-child margins collapse across an auto-height constrained block");
        const auto* padded=FindLayout(commonFlowLayout.Root(),L"margin-padded");
        const auto* paddedChild=FindLayout(commonFlowLayout.Root(),L"margin-padded-child");
        Check(padded&&paddedChild&&std::abs(paddedChild->rect.y-padded->rect.y-20.0f)<0.01f,
              L"padding separates parent and child margins");
        const auto* flexFirst=FindLayout(commonFlowLayout.Root(),L"margin-flex-first");
        const auto* flexNext=FindLayout(commonFlowLayout.Root(),L"margin-flex-next");
        Check(flexFirst&&flexNext&&std::abs(flexNext->rect.y-flexFirst->rect.y-30.0f)<0.01f,
              L"vertical margins of flex items remain additive");
    }

    Document standardTableDoc;
    Check(standardTableDoc.Parse(
        L"<!-- source comment --><!doctype html><style>body{margin:0}table{border-collapse:collapse}</style>"
        L"<div id='invalid-padding' style='padding:0 8 0 8'>Text</div>"
        L"<table cellpadding='0'><tr><td id='attribute-height' height='25'></td></tr></table>"
        L"<table cellpadding='0'><tr><td id='empty-cell'></td></tr></table>",&error),
        L"standards-mode HTML table fixture parses");
    StyleSheet standardTableCss;Check(standardTableCss.Parse(standardTableDoc.StyleText(),&error),
                                     L"standards-mode table styles parse");
    LayoutEngine standardTableLayout(standardTableDoc,standardTableCss);
    standardTableLayout.Layout(320,100);
    const auto* invalidPadding=FindLayout(standardTableLayout.Root(),L"invalid-padding");
    const auto* attributeHeight=FindLayout(standardTableLayout.Root(),L"attribute-height");
    const auto* emptyCell=FindLayout(standardTableLayout.Root(),L"empty-cell");
    Check(!standardTableDoc.QuirksMode()&&invalidPadding&&
          std::abs(invalidPadding->content.x-invalidPadding->rect.x)<0.01f&&
          attributeHeight&&std::abs(attributeHeight->rect.height-25.0f)<0.01f&&
          emptyCell&&std::abs(emptyCell->rect.height)<0.01f,
          L"standards mode ignores invalid unitless padding, honors HTML cell height, and gives empty rows no synthetic height");
    Check(standardTableDoc.Parse(L"<div id='legacy-padding' style='padding:0 8 0 8'>Text</div>",&error),
          L"legacy unitless-length document parses");
    standardTableLayout.Layout(320,100);
    const auto* legacyPadding=FindLayout(standardTableLayout.Root(),L"legacy-padding");
    Check(standardTableDoc.QuirksMode()&&legacyPadding&&
          std::abs(legacyPadding->content.x-legacyPadding->rect.x-8.0f)<0.01f,
          L"quirks mode retains legacy unitless CSS padding");

    Document clippedTextDoc;
    Check(clippedTextDoc.Parse(L"<style>*{box-sizing:border-box}.cell{width:120px;height:28px;padding:0 10px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.right{text-align:right}</style><div id='clipped-left' class='cell'>A very long single line</div><div id='clipped-right' class='cell right'>42</div>",&error),
          L"single-line clipped text fixture parses");
    StyleSheet clippedTextCss;Check(clippedTextCss.Parse(clippedTextDoc.StyleText(),&error),L"single-line clipped text CSS parses");
    LayoutEngine clippedTextLayout(clippedTextDoc,clippedTextCss);clippedTextLayout.Layout(300,100);
    const auto* clippedLeft=FindLayout(clippedTextLayout.Root(),L"clipped-left");
    const auto* clippedRight=FindLayout(clippedTextLayout.Root(),L"clipped-right");
    Check(clippedLeft&&clippedLeft->children.size()==1&&
          std::abs(clippedLeft->children[0]->rect.x-clippedLeft->content.x)<0.01f&&
          std::abs(clippedLeft->children[0]->rect.width-clippedLeft->content.width)<0.01f&&
          std::abs(clippedLeft->children[0]->rect.height-clippedLeft->content.height)<0.01f,
          L"single-line clipped text uses its containing block without intrinsic-width layout");
    Check(clippedRight&&clippedRight->children.size()==1&&
          std::abs(clippedRight->children[0]->rect.width-clippedRight->content.width)<0.01f,
          L"single-line clipped text preserves a full alignment rectangle");

    JavaScriptRuntime js(doc); std::wstring message;
    js.SetMessageSink([&](const std::wstring& value) { message = value; });
    const wchar_t* source = LR"JS(
        function update(data) {
            const x = document.getElementById('x');
            x.innerText = `${data.name}:${(data.value * 2).toFixed(1)}`;
            x.classList.add('changed');
        }
        document.addEventListener('DOMContentLoaded', () => {
            document.getElementById('x').addEventListener('click', function(e) {
                window.chrome.webview.postMessage('clicked:' + this.innerText);
            });
        });
    )JS";
    Check(js.Load(source, &error), error.c_str());
    js.DispatchDocumentEvent(L"DOMContentLoaded");
    Check(js.Execute(L"update({name:'A', value:2.5});", nullptr, &error), error.c_str());
    Check(doc.GetElementById(L"x")->InnerText() == L"A:5.0", L"compiled function mutates DOM");
    Check(doc.GetElementById(L"x")->HasClass(L"changed"), L"classList binding");
    js.DispatchNodeEvent(doc.GetElementById(L"x"), L"click");
    Check(message == L"clicked:A:5.0", L"host message bridge");
    Check(js.Execute(L"document.getElementById('x').innerText='pending'; const skipped=requestAnimationFrame(()=>{document.getElementById('x').innerText='wrong';}); cancelAnimationFrame(skipped); requestAnimationFrame(()=>{document.getElementById('x').innerText='done';});",nullptr,&error),error.c_str());
    Check(doc.GetElementById(L"x")->InnerText()==L"pending",L"animation callbacks wait until the next frame");
    js.RunAnimationFrame();
    Check(doc.GetElementById(L"x")->InnerText()==L"done",L"animation frame runs active callbacks and skips canceled callbacks");

    Document hoistedFunctionDoc;
    Check(hoistedFunctionDoc.Parse(
          L"<nav><div class='dropdown-panel'></div><div class='dropdown-panel'></div></nav>",&error),
          L"function declaration hoisting fixture parses");
    JavaScriptRuntime hoistedFunctionJs(hoistedFunctionDoc);
    const wchar_t* hoistedFunctionSource=LR"JS(
        runWhenReady(function () {
            populatePanels();
            window.nestedHoistResult = callNestedDeclaration();
        });

        function runWhenReady(callback) {
            if (document.readyState !== 'loading') callback();
            else document.addEventListener('DOMContentLoaded', callback);
        }
        function populatePanels() {
            document.querySelectorAll('.dropdown-panel').forEach(function (panel, index) {
                panel.innerHTML = buildList(index + 1);
            });
        }
        function buildList(number) {
            return '<ul><li>Menu ' + number + '</li></ul>';
        }
        function callNestedDeclaration() {
            return declaredAfterReturnSite();
            function declaredAfterReturnSite() { return 'nested-ok'; }
        }
    )JS";
    Check(hoistedFunctionJs.Load(hoistedFunctionSource,&error),error.c_str());
    std::wstring hoistedFunctionResult;
    Check(hoistedFunctionJs.Execute(
              L"return document.querySelectorAll('.dropdown-panel ul').length+'|'+window.nestedHoistResult+'|'+typeof populatePanels;",
              &hoistedFunctionResult,&error)&&hoistedFunctionResult==L"2|nested-ok|function",
          L"function declarations are instantiated before top-level and nested scope execution");

    Document modernJsDoc;
    Check(modernJsDoc.Parse(L"<div class='card' data-table-id='alpha'></div>",&error),L"modern JavaScript DOM fixture parses");
    JavaScriptRuntime modernJs(modernJsDoc);std::wstring modernMessage;
    modernJs.SetMessageSink([&](const std::wstring& value){modernMessage=value;});
    const wchar_t* modernSource=LR"JS(
        class Counter {
            static STEP = 2;
            constructor(root) { this.root = root; this.value = 1; this.bump = this.add.bind(this); }
            add() { this.value += Counter.STEP; return this.value; }
            schedule() { requestAnimationFrame(() => { this.root.textContent = `${this.bump()}`; this.root.dataset.state = "ready"; }); }
        }
        const base = { command: "modern" };
        const payload = { ...base, tableId: document.querySelector(".card").dataset.tableId };
        const instances = new Map();
        document.querySelectorAll(".card").forEach((card) => { const item = new Counter(card); instances.set(item.root.dataset.tableId, item); });
        for (const item of instances.values()) { item.schedule(); }
        window.chrome.webview.postMessage(JSON.stringify(payload));
    )JS";
    Check(modernJs.Load(modernSource,&error),error.c_str());
    Check(modernMessage.find(L"\"command\":\"modern\"")!=std::wstring::npos&&modernMessage.find(L"\"tableId\":\"alpha\"")!=std::wstring::npos,
          L"classes, construction, object spread and dataset work together");
    modernJs.RunAnimationFrame();
    const auto modernCard=modernJsDoc.QuerySelector(L".card");
    Check(modernCard&&modernCard->InnerText()==L"3"&&modernCard->Attribute(L"data-state")==L"ready",
          L"bound methods, lexical arrow this and for-of iterables run on animation frames");

    Document hostTableDoc;
    Check(hostTableDoc.Parse(L"<table><tbody id='rows'></tbody></table>",&error),
          L"host-fed table DOM fixture parses");
    JavaScriptRuntime hostTableJs(hostTableDoc);
    const wchar_t* hostTableSource=LR"JS(
        function escapeHtml(value) {
            return String(value)
                .replaceAll('&','&amp;')
                .replaceAll('<','&lt;')
                .replaceAll('>','&gt;')
                .replaceAll('"','&quot;')
                .replaceAll("'","&#39;");
        }
        function renderTable(rows) {
            const body = document.getElementById('rows');
            body.innerHTML = '';
            for (const row of rows) {
                const tr = document.createElement('tr');
                tr.innerHTML = `<td>${escapeHtml(row.id)}</td><td>${escapeHtml(row.name)}</td><td>${escapeHtml(row.value)}</td>`;
                body.appendChild(tr);
            }
        }
        window.twebframe?.addEventListener('message', (event) => {
            const message = event.data;
            if (message && message.type === 'cards' && Array.isArray(message.rows)) {
                renderTable(message.rows);
                console.log(`[WebMsg] received ${message.rows.length} rows`);
            }
        });
    )JS";
    Check(hostTableJs.Load(hostTableSource,&error),error.c_str());
    Check(hostTableJs.DispatchWebMessageAsJson(
              L"{\"type\":\"cards\",\"rows\":[{\"id\":\"001\",\"name\":\"<GPU>\",\"value\":\"A&B\"},{\"id\":\"002\",\"name\":\"Memory\",\"value\":\"64GB\"}]}",
              &error),error.c_str());
    const auto hostTableBody=hostTableDoc.GetElementById(L"rows");
    const auto hostTableRows=hostTableDoc.QuerySelectorAll(L"tr",hostTableBody);
    const auto firstHostTableCells=hostTableRows.empty()?std::vector<std::shared_ptr<Node>>{}:
        hostTableDoc.QuerySelectorAll(L"td",hostTableRows.front());
    Check(hostTableRows.size()==2&&firstHostTableCells.size()==3&&
          firstHostTableCells[0]->InnerText()==L"001"&&
          firstHostTableCells[1]->InnerText()==L"<GPU>"&&
          firstHostTableCells[2]->InnerText()==L"A&B"&&
          hostTableDoc.QuerySelectorAll(L"gpu",hostTableBody).empty(),
          L"host JSON messages render table rows through generic modern JavaScript and DOM bindings");

    Document fileDoc;
    Check(fileDoc.Parse(L"<input id='file' type='file'>",&error),L"file input fixture parses");
    auto selectedFile=fileDoc.GetElementById(L"file");
    selectedFile->files.push_back({L"sample.csv",L"text/csv",L"",4096});
    selectedFile->SetAttribute(L"value",L"C:\\fakepath\\sample.csv");
    JavaScriptRuntime fileJs(fileDoc);std::wstring fileResult;
    Check(fileJs.Execute(L"const picked=document.getElementById('file').files[0]; return picked.name+'|'+picked.size+'|'+picked.type;",&fileResult,&error)&&
          fileResult==L"sample.csv|4096|text/csv",L"file input exposes standard file name, size and type to JavaScript");
    Check(fileJs.Execute(L"document.getElementById('file').value=''; return document.getElementById('file').files.length;",&fileResult,&error)&&
          fileResult==L"0",L"clearing a file input clears the selected file list");
    std::wstring droppedName;
    fileJs.SetMessageSink([&](const std::wstring& value){droppedName=value;});
    Check(fileJs.Execute(L"document.getElementById('file').addEventListener('drop',event=>window.chrome.webview.postMessage(event.type+'|'+event.dataTransfer.files[0].name));",nullptr,&error),error.c_str());
    fileJs.DispatchFileDrop(selectedFile,{{L"dropped.csv",L"text/csv",L"",100}});
    Check(droppedName==L"drop|dropped.csv",L"file drop events expose a standard dataTransfer file list");
    Check(fileJs.Execute(L"document.getElementById('file').addEventListener('paste',event=>{const item=event.clipboardData.items[0];const file=item.getAsFile();window.chrome.webview.postMessage(event.clipboardData.getData('text/plain')+'|'+event.clipboardData.types.includes('Files')+'|'+item.kind+'|'+file.name+'|'+file.type);event.preventDefault();});",nullptr,&error),error.c_str());
    Check(fileJs.DispatchClipboardEvent(selectedFile,L"paste",L"plain text",{{L"image.png",L"image/png",L"C:\\image.png",321}})&&
          droppedName==L"plain text|true|file|image.png|image/png",
          L"clipboard events expose text, file types and DataTransferItem files");
    wchar_t temporaryDirectory[MAX_PATH]{},temporaryFile[MAX_PATH]{};
    const DWORD temporaryLength=GetTempPathW(MAX_PATH,temporaryDirectory);
    Check(temporaryLength>0&&temporaryLength<MAX_PATH&&GetTempFileNameW(temporaryDirectory,L"twf",0,temporaryFile)!=0,
          L"FileReader test creates a temporary file");
    if(temporaryFile[0]){
        const HANDLE file=CreateFileW(temporaryFile,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        const char bytes[]={'a','b','c'};DWORD written=0;
        Check(file!=INVALID_HANDLE_VALUE&&WriteFile(file,bytes,sizeof(bytes),&written,nullptr)&&written==sizeof(bytes),
              L"FileReader test writes its binary fixture");
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
        selectedFile->files={{L"image.png",L"image/png",temporaryFile,sizeof(bytes)}};
        Check(fileJs.Execute(L"let dataUrl='';const reader=new FileReader();reader.addEventListener('load',()=>dataUrl=reader.result,{once:true});reader.readAsDataURL(document.getElementById('file').files[0]);return dataUrl;",&fileResult,&error)&&
              fileResult==L"data:image/png;base64,YWJj",
              L"FileReader produces an image data URL from a selected file");
        DeleteFileW(temporaryFile);
    }

    Document iconButtonDoc;
    Check(iconButtonDoc.Parse(L"<style>*{box-sizing:border-box}button{display:flex;align-items:center;gap:10px;padding:0 12px}svg{width:20px;height:20px}</style><div style='display:flex;width:500px'><button id='icon-button'><svg viewBox='0 0 24 24'><path d='M4 4h16v16H4z'/></svg>Sample label</button></div>",&error),L"icon button fixture parses");
    StyleSheet iconButtonCss;Check(iconButtonCss.Parse(iconButtonDoc.StyleText(),&error),L"icon button styles parse");
    LayoutEngine iconButtonLayout(iconButtonDoc,iconButtonCss);iconButtonLayout.Layout(500,100);
    const auto* iconButton=FindLayout(iconButtonLayout.Root(),L"icon-button");
    Check(iconButton&&iconButton->children.size()==2&&
          iconButton->children.back()->rect.x+iconButton->children.back()->rect.width<=
              iconButton->content.x+iconButton->content.width+0.5f,
          L"flex button intrinsic width includes its icon, text and gap");
    Document anonymousFlexDoc;
    Check(anonymousFlexDoc.Parse(
          L"<style>button{display:inline-flex;gap:7px;padding:0}</style><button id='flex-space'>A <span>B</span></button><button id='flex-tight'>A<span>B</span></button>",
          &error),L"anonymous flex text fixture parses");
    StyleSheet anonymousFlexCss;Check(anonymousFlexCss.Parse(anonymousFlexDoc.StyleText(),&error),
          L"anonymous flex text CSS parses");
    LayoutEngine anonymousFlexLayout(anonymousFlexDoc,anonymousFlexCss);anonymousFlexLayout.Layout(300,80);
    const auto* flexSpace=anonymousFlexLayout.BoxFor(anonymousFlexDoc.GetElementById(L"flex-space"));
    const auto* flexTight=anonymousFlexLayout.BoxFor(anonymousFlexDoc.GetElementById(L"flex-tight"));
    Check(flexSpace&&flexTight&&std::abs(flexSpace->rect.width-flexTight->rect.width)<0.001f,
          L"collapsible whitespace is trimmed at anonymous flex-item boundaries");
    Document preformattedFlexDoc;
    Check(preformattedFlexDoc.Parse(
          L"<style>*{box-sizing:border-box;margin:0;padding:0}#pre-flex{display:flex;width:300px;height:30px;justify-content:space-between;white-space:pre}</style>"
          L"<div id='pre-flex'>\n  <span id='pre-flex-label'>Recent documents</span><span id='pre-flex-arrow'>&#8250;</span>\n</div>",
          &error),L"preformatted anonymous flex whitespace fixture parses");
    StyleSheet preformattedFlexCss;
    Check(preformattedFlexCss.Parse(preformattedFlexDoc.StyleText(),&error),
          L"preformatted anonymous flex whitespace CSS parses");
    LayoutEngine preformattedFlexLayout(preformattedFlexDoc,preformattedFlexCss);
    preformattedFlexLayout.Layout(400,80);
    const auto* preFlex=FindLayout(preformattedFlexLayout.Root(),L"pre-flex");
    const auto* preFlexLabel=FindLayout(preformattedFlexLayout.Root(),L"pre-flex-label");
    const auto* preFlexArrow=FindLayout(preformattedFlexLayout.Root(),L"pre-flex-arrow");
    Check(preFlex&&preFlexLabel&&preFlexArrow&&preFlex->children.size()==2&&
          std::abs(preFlexLabel->rect.x-preFlex->content.x)<0.01f&&
          std::abs(preFlexArrow->rect.x+preFlexArrow->rect.width-
                   (preFlex->content.x+preFlex->content.width))<0.01f,
          L"preformatted source indentation does not create anonymous flex items or distort space-between");

    std::vector<std::wstring> compositionStages;std::wstring compositionResult;
    TextInput compositionInput({
        []{return true;},
        [&]{compositionStages.push_back(L"start");},
        [&](const std::wstring& text,const std::vector<unsigned char>&,size_t cursor){
            compositionStages.push_back(text+L"@"+std::to_wstring(cursor));
        },
        [&](const std::wstring& text){compositionResult=text;},
        [&]{compositionStages.push_back(L"cancel");},
        []{return RECT{10,10,11,28};}
    });
    compositionInput.StartComposition();
    compositionInput.UpdateComposition(L"\u314e",{ATTR_INPUT},1);
    compositionInput.UpdateComposition(L"\ud558",{ATTR_TARGET_CONVERTED},1);
    Check(compositionInput.IsComposing()&&compositionStages.size()==3&&
          compositionStages[1]==L"\u314e@1"&&compositionStages[2]==L"\ud558@1",
          L"Hangul jamo and partial syllables are exposed during inline composition");
    compositionInput.CommitComposition(L"\ud55c");
    Check(!compositionInput.IsComposing()&&compositionResult==L"\ud55c",
          L"only the IME result commits the completed Hangul syllable");

    HWND inputHost=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP|WS_VISIBLE,-10000,-10000,320,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(inputHost!=nullptr,L"off-screen input test host is created");
    if(inputHost){
        RECT inputBounds{0,0,320,120};auto inputView=TWebFrame::View::Create(inputHost,inputBounds);
        Check(inputView!=nullptr,L"TWebFrame input test view is created");
        if(inputView){
            inputView->SetResourceLoader([](const std::wstring& path,std::wstring& css){
                if(path!=L"https://styles.test/css/editor.css")return false;
                css=L"p{color:blue;background-image:url(../images/icon.png)}";return true;
            });
            Check(inputView->NavigateToString(
                L"<style>p{color:red}</style><link rel='stylesheet' href='/css/editor.css'>"
                L"<style id='last-style'>p{color:green}</style><p>text</p>",L"https://styles.test/document.html"),
                L"stylesheet source order and relative URL fixture loads");
            std::wstring cascadeResult,cascadeError;
            Check(inputView->ExecuteScript(
                L"return getComputedStyle(document.querySelector('p')).color;",&cascadeResult,&cascadeError)&&cascadeResult==L"green",
                L"inline and linked stylesheets cascade in document order");
            Check(inputView->ExecuteScript(
                L"document.getElementById('last-style').remove();"
                L"var addedStyle=document.createElement('style');document.head.appendChild(addedStyle);"
                L"addedStyle.sheet.insertRule('p{color:purple}',0);"
                L"return getComputedStyle(document.querySelector('p')).color;",&cascadeResult,&cascadeError)&&cascadeResult==L"purple",
                L"inserted style elements and CSSOM rules affect computed style before script returns");
            Check(inputView->ExecuteScript(
                L"addedStyle.remove();return getComputedStyle(document.querySelector('p')).color+'|'"
                L"+getComputedStyle(document.querySelector('p')).backgroundImage;",&cascadeResult,&cascadeError)&&
                cascadeResult.find(L"blue|url(\"https://styles.test/images/icon.png\")")==0,
                L"stylesheet removal restores the linked rule and relative image URLs use the stylesheet location");
            const DWORD asyncUiThread=GetCurrentThreadId();
            std::atomic<DWORD> asyncResourceThread{0};
            std::atomic<DWORD> asyncFetchThread{0};
            std::atomic<DWORD> asyncScriptThread{0};
            std::atomic<DWORD> initialScriptThread{0};
            bool asyncCompleted=false,asyncSucceeded=false;DWORD asyncCallbackThread=0;
            int asyncCompletionCount=0;
            inputView->SetParallelResourceLoading(true);
            inputView->SetResourceLoader([&](const std::wstring& path,std::wstring& source){
                if(path==L"https://async.test/theme.css"){
                    asyncResourceThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                    source=L"#async-winner{color:rgb(1,2,3)}";return true;
                }
                if(path==L"fixture://async.test/payload.txt"){
                    asyncFetchThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                    Sleep(20);source=L"worker-fetch";return true;
                }
                if(path==L"https://async.test/dynamic.js"){
                    asyncScriptThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                    Sleep(40);source=L"window.dynamicWorkerValue='worker-script';";return true;
                }
                if(path==L"https://async.test/ordered-slow.js"){
                    initialScriptThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                    Sleep(120);source=L"initialOrder.push('slow');";return true;
                }
                if(path==L"https://async.test/async-fast.js"){
                    initialScriptThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                    Sleep(5);source=L"initialOrder.push('async');";return true;
                }
                if(path==L"https://async.test/defer-fast.js"){
                    initialScriptThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                    Sleep(10);source=L"initialOrder.push('defer');";return true;
                }
                return false;
            });
            inputView->SetLoadHandler([&](bool success,const std::wstring&){
                asyncSucceeded=success;asyncCompleted=true;++asyncCompletionCount;
                asyncCallbackThread=GetCurrentThreadId();
            });
            std::wstring superseded=L"<title>superseded</title>";
            for(int index=0;index<5000;++index)
                superseded+=L"<div>stale "+std::to_wstring(index)+L"</div>";
            Check(inputView->NavigateToStringAsync(superseded,L"https://async.test/old.html"),
                  L"asynchronous HTML parse is accepted");
            Check(inputView->NavigateToStringAsync(
                L"<title>async-winner</title><link rel='stylesheet' href='theme.css'>"
                L"<main id='async-winner'>ready</main>",L"https://async.test/index.html"),
                L"newer asynchronous navigation supersedes an older parse");
            Check(!asyncCompleted,L"asynchronous navigation does not complete inline on the UI thread");
            const auto asyncDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!asyncCompleted&&std::chrono::steady_clock::now()<asyncDeadline){
                MSG asyncMessage{};
                if(PeekMessageW(&asyncMessage,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&asyncMessage);DispatchMessageW(&asyncMessage);
                }else Sleep(1);
            }
            Check(asyncCompleted&&asyncSucceeded&&inputView->DocumentTitle()==L"async-winner",
                  L"the newest worker-parsed document is adopted on the view");
            Check(asyncCompletionCount==1&&asyncCallbackThread==asyncUiThread,
                  L"stale parse completion is discarded and load completion stays on the UI thread");
            Check(asyncResourceThread.load(std::memory_order_relaxed)!=0&&
                  asyncResourceThread.load(std::memory_order_relaxed)!=asyncUiThread,
                  L"external stylesheet loading and parsing run on the CPU worker path");
            std::wstring asyncFetchMessage;
            inputView->SetMessageHandler([&](const std::wstring& value){asyncFetchMessage=value;});
            std::wstring asyncFetchError;
            Check(inputView->ExecuteScript(
                L"fetch('fixture://async.test/payload.txt').then(response=>response.text()).then(value=>window.chrome.webview.postMessage(value));",
                nullptr,&asyncFetchError),asyncFetchError.c_str());
            Check(asyncFetchMessage.empty(),L"fetch returns a pending promise without blocking the UI thread");
            const auto fetchDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(asyncFetchMessage.empty()&&std::chrono::steady_clock::now()<fetchDeadline){
                MSG fetchMessage{};
                if(PeekMessageW(&fetchMessage,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&fetchMessage);DispatchMessageW(&fetchMessage);
                }else Sleep(1);
            }
            Check(asyncFetchMessage==L"worker-fetch"&&
                  asyncFetchThread.load(std::memory_order_relaxed)!=0&&
                  asyncFetchThread.load(std::memory_order_relaxed)!=asyncUiThread,
                  L"fetch I/O resolves its promise on the UI thread after worker completion");
            std::wstring asyncScriptMessage;
            inputView->SetMessageHandler([&](const std::wstring& value){asyncScriptMessage=value;});
            Check(inputView->ExecuteScript(
                L"const script=document.createElement('script');script.src='dynamic.js';"
                L"script.onload=()=>window.chrome.webview.postMessage(window.dynamicWorkerValue+'|load');"
                L"document.head.appendChild(script);",
                nullptr,&asyncFetchError),asyncFetchError.c_str());
            Check(asyncScriptMessage.empty(),
                  L"a DOM-inserted external script does not complete inside appendChild");
            const auto scriptDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(asyncScriptMessage.empty()&&std::chrono::steady_clock::now()<scriptDeadline){
                MSG scriptMessage{};
                if(PeekMessageW(&scriptMessage,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&scriptMessage);DispatchMessageW(&scriptMessage);
                }else Sleep(1);
            }
            Check(asyncScriptMessage==L"worker-script|load"&&
                  asyncScriptThread.load(std::memory_order_relaxed)!=0&&
                  asyncScriptThread.load(std::memory_order_relaxed)!=asyncUiThread,
                  L"DOM-inserted external scripts load on a worker and execute on the UI thread");
            asyncCompleted=false;asyncSucceeded=false;
            std::vector<std::wstring> initialScriptMessages;
            inputView->SetMessageHandler([&](const std::wstring& value){
                initialScriptMessages.push_back(value);
            });
            Check(inputView->NavigateToStringAsync(
                LR"HTML(<title>progressive-script-page</title>
                <style>html,body{margin:0;background:#123456}</style>
                <script>
                var initialOrder=['inline'];
                document.addEventListener('DOMContentLoaded',()=>window.chrome.webview.postMessage('D:'+initialOrder.join(',')));
                window.addEventListener('load',()=>window.chrome.webview.postMessage('L:'+initialOrder.join(',')));
                </script>
                <script src="ordered-slow.js"></script>
                <script async src="async-fast.js"></script>
                <script defer src="defer-fast.js"></script>
                <script>initialOrder.push('tail');</script>)HTML",
                L"https://async.test/progressive.html"),
                L"progressive initial-script navigation is accepted");
            const auto adoptionDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(inputView->DocumentTitle()!=L"progressive-script-page"&&
                  std::chrono::steady_clock::now()<adoptionDeadline){
                MSG adoptionMessage{};
                if(PeekMessageW(&adoptionMessage,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&adoptionMessage);DispatchMessageW(&adoptionMessage);
                }else Sleep(1);
            }
            Check(inputView->DocumentTitle()==L"progressive-script-page"&&!asyncCompleted,
                  L"DOM and CSS publish before a slow initial external script completes");
            const auto initialScriptDeadline=std::chrono::steady_clock::now()+
                std::chrono::seconds(5);
            while(!asyncCompleted&&std::chrono::steady_clock::now()<initialScriptDeadline){
                MSG initialMessage{};
                if(PeekMessageW(&initialMessage,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&initialMessage);DispatchMessageW(&initialMessage);
                }else Sleep(1);
            }
            std::wstring initialOrder,initialOrderError;
            Check(inputView->ExecuteScript(L"return initialOrder.join(',');",
                                           &initialOrder,&initialOrderError)&&
                  initialOrder==L"inline,async,slow,tail,defer",initialOrderError.c_str());
            Check(asyncCompleted&&asyncSucceeded&&
                  initialScriptThread.load(std::memory_order_relaxed)!=0&&
                  initialScriptThread.load(std::memory_order_relaxed)!=asyncUiThread&&
                  initialScriptMessages.size()==2&&
                  initialScriptMessages[0]==L"D:inline,async,slow,tail,defer"&&
                  initialScriptMessages[1]==L"L:inline,async,slow,tail,defer",
                  L"initial async scripts run on completion while ordered and defer scripts preserve DOM order");
            inputView->SetLoadHandler({});inputView->SetResourceLoader({});
            inputView->SetParallelResourceLoading(false);
            std::wstring inputMessage;inputView->SetMessageHandler([&](const std::wstring& value){inputMessage=value;});
            const wchar_t* inputHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}input{width:120px;height:32px;padding:0 4px;border:1px solid #ccc;font-size:13px}</style><input id="editor" type="text" value="abc" maxlength="5"><script>document.addEventListener('DOMContentLoaded',function(){const editor=document.getElementById('editor');editor.addEventListener('input',function(){window.chrome.webview.postMessage('input:'+this.value);});editor.addEventListener('change',function(){window.chrome.webview.postMessage('change:'+this.value);});});</script>)HTML";
            Check(inputView->NavigateToString(inputHtml),L"text input fixture loads in a real view");
            const HWND inputWindow=inputView->Window();
            Check((GetWindowLongPtrW(inputWindow,GWL_STYLE)&WS_TABSTOP)!=0,L"embedded view participates in dialog keyboard focus");
            std::wstring inputCenter,selectionError;
            Check(inputView->ExecuteScript(
                L"const r=document.getElementById('editor').getBoundingClientRect();"
                L"return Math.round(r.x+5)+','+Math.round(r.y+r.height/2);",
                &inputCenter,&selectionError),selectionError.c_str());
            const auto inputCenterComma=inputCenter.find(L',');
            Check(inputCenterComma!=std::wstring::npos,L"text input geometry can be queried");
            if(inputCenterComma!=std::wstring::npos){
                const auto scale=static_cast<double>(GetDpiForWindow(inputWindow))/96.0;
                const int x=static_cast<int>(std::lround(std::stod(inputCenter.substr(0,inputCenterComma))*scale));
                const int y=static_cast<int>(std::lround(std::stod(inputCenter.substr(inputCenterComma+1))*scale));
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
            }
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"focused text input is rendered and edited without a child Win32 Edit");
            const auto dialogCode=SendMessageW(inputWindow,WM_GETDLGCODE,static_cast<WPARAM>(L'A'),0);
            Check((dialogCode&DLGC_WANTCHARS)!=0,L"TWebFrame's custom editor requests character messages from a dialog host");
            std::wstring selectionResult;
            Check(inputView->ExecuteScript(L"const e=document.getElementById('editor');e.setSelectionRange(0,0);return e.selectionStart+'|'+e.selectionEnd;",&selectionResult,&selectionError)&&selectionResult==L"0|0",
                  L"DOM selection APIs control the custom caret");
            inputMessage.clear();SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'X'),1);
            Check(inputMessage==L"input:Xabc",L"typing inserts text at the clicked caret and dispatches input");
            SendMessageW(inputWindow,WM_CHAR,VK_BACK,1);
            Check(inputMessage==L"input:abc",L"backspace edits the focused text input");
            Check(inputView->ExecuteScript(L"document.getElementById('editor').setSelectionRange(2,2);",nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'Y'),1);
            Check(inputMessage==L"input:abYc",L"typing inserts text in the middle at the custom caret");
            Check(inputView->ExecuteScript(L"document.getElementById('editor').setSelectionRange(2,2);",nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'\uAC00'),1);
            Check(inputMessage==L"input:ab\uAC00Yc",L"custom Unicode editing synchronizes Korean text at the caret");
            SendMessageW(inputWindow,WM_CHAR,VK_BACK,1);
            Check(inputMessage==L"input:abYc",L"Korean text can be edited through the same custom caret path");
            SendMessageW(inputWindow,WM_UNDO,0,0);
            Check(inputMessage==L"input:ab\uAC00Yc",L"custom text input undo restores Unicode edits");
            SendMessageW(inputWindow,WM_CHAR,VK_BACK,1);
            Check(inputMessage==L"input:abYc",L"editing continues from the caret restored by undo");
            inputMessage.clear();SendMessageW(inputWindow,WM_CHAR,VK_RETURN,1);
            Check(inputMessage.empty(),L"single-line HTML input rejects a custom-editor line break");
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(250,80));
            Check(inputMessage==L"change:abYc",L"leaving an edited text input dispatches change");
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,L"text editing never creates a Win32 Edit child");
            inputMessage.clear();
            Check(inputView->ExecuteScript(L"window.chrome.webview.postMessage(document.getElementById('editor').value);"),L"edited input value is readable through the DOM");
            Check(inputMessage==L"abYc",L"DOM value reflects custom keyboard editing");
            const wchar_t* contenteditableHtml=LR"HTML(<style>*{box-sizing:border-box;margin:0}article{display:block;width:280px;height:90px;padding:10px;font:16px/24px "Segoe UI"}p{display:block;margin:0}</style><article id="rich-editor" contenteditable="true"><p>alpha <strong id="middle-run">bravo</strong> charlie</p></article>)HTML";
            Check(inputView->NavigateToString(contenteditableHtml),L"contenteditable pointer fixture loads in a real view");
            UpdateWindow(inputWindow);
            std::wstring trailingTextPoint;
            Check(inputView->ExecuteScript(
                L"const r=document.getElementById('middle-run').getBoundingClientRect();return Math.round(r.x+r.width+8)+','+Math.round(r.y+r.height/2);",
                &trailingTextPoint,&selectionError),selectionError.c_str());
            const auto trailingPointComma=trailingTextPoint.find(L',');
            if(trailingPointComma!=std::wstring::npos){
                const auto scale=static_cast<double>(GetDpiForWindow(inputWindow))/96.0;
                const int x=static_cast<int>(std::lround(std::stod(trailingTextPoint.substr(0,trailingPointComma))*scale));
                const int y=static_cast<int>(std::lround(std::stod(trailingTextPoint.substr(trailingPointComma+1))*scale));
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
                SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'X'),1);
            }
            Check(inputView->ExecuteScript(L"return document.getElementById('rich-editor').textContent;",
                &selectionResult,&selectionError)&&selectionResult.find(L"bravo")<selectionResult.find(L'X')&&
                !selectionResult.empty()&&selectionResult.back()!=L'X',
                L"clicking a later inline text run inserts at its pointed offset instead of a text-node boundary");
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"contenteditable pointer editing remains in the childless common input path");
            const wchar_t* paragraphInputHtml=LR"HTML(<style>*{box-sizing:border-box;margin:0}article{display:block;width:280px;min-height:90px;padding:10px;font:16px/24px "Segoe UI"}p{display:block;margin:0}</style><article id="paragraph-editor" contenteditable="true"><p>helloworld</p></article>)HTML";
            Check(inputView->NavigateToString(paragraphInputHtml),
                  L"contenteditable paragraph input fixture loads in a real view");
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');"
                L"e.addEventListener('beforeinput',function(event){this.setAttribute('data-before',event.inputType);});"
                L"e.addEventListener('input',function(event){this.setAttribute('data-input',event.inputType);});"
                L"e.focus();const r=document.createRange();r.setStart(e.firstChild.firstChild,5);r.collapse(true);"
                L"const s=getSelection();s.removeAllRanges();s.addRange(r);",
                nullptr,&selectionError),selectionError.c_str());
            SetFocus(inputWindow);SendMessageW(inputWindow,WM_CHAR,VK_RETURN,1);
            std::wstring paragraphResult;
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');const s=getSelection();"
                L"return e.children.length+'|'+e.children[0].textContent+'|'+e.children[1].textContent+'|'"
                L"+(s.anchorNode===e.children[1])+'|'+s.anchorOffset+'|'"
                L"+e.getAttribute('data-before')+'|'+e.getAttribute('data-input');",
                &paragraphResult,&selectionError)&&
                paragraphResult==L"2|hello|world|true|0|insertParagraph|insertParagraph",
                L"Enter splits a contenteditable block at the DOM caret and dispatches standard paragraph input events");
            SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'X'),1);
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');return e.children[0].textContent+'|'+e.children[1].textContent;",
                &paragraphResult,&selectionError)&&paragraphResult==L"hello|Xworld",
                L"typing after Enter continues at the start of the newly created paragraph");
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');const t=e.children[1].firstChild;"
                L"const r=document.createRange();r.setStart(t,t.length);r.collapse(true);"
                L"const s=getSelection();s.removeAllRanges();s.addRange(r);",
                nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_CHAR,VK_RETURN,1);
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');const s=getSelection();"
                L"const previous=e.children[1].getBoundingClientRect();const empty=e.children[2].getBoundingClientRect();"
                L"return e.children.length+'|'+e.children[2].textContent+'|'"
                L"+(s.anchorNode===e.children[2])+'|'+s.anchorOffset+'|'"
                L"+(empty.y>=previous.y+previous.height-0.01)+'|'+(empty.height>0);",
                &paragraphResult,&selectionError)&&paragraphResult==L"3||true|0|true|true",
                L"Enter at the end creates a visible empty line and places the caret on that line");
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');const t=e.children[0].firstChild;"
                L"const r=document.createRange();r.setStart(t,3);r.collapse(true);"
                L"const s=getSelection();s.removeAllRanges();s.addRange(r);",
                nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_KEYDOWN,VK_DOWN,1);
            std::wstring firstDown;
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');const s=getSelection();"
                L"return (s.anchorNode===e.children[1].firstChild)+'|'+s.anchorOffset;",
                &firstDown,&selectionError)&&firstDown.rfind(L"true|",0)==0,
                L"Down Arrow moves the DOM caret from one editable paragraph to the next visual line");
            SendMessageW(inputWindow,WM_KEYDOWN,VK_DOWN,1);
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');const s=getSelection();"
                L"return (s.anchorNode===e.children[2])+'|'+s.anchorOffset;",
                &paragraphResult,&selectionError)&&paragraphResult==L"true|0",
                L"Down Arrow reaches an empty editable paragraph using its DOM boundary");
            SendMessageW(inputWindow,WM_KEYDOWN,VK_UP,1);
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('paragraph-editor');const s=getSelection();"
                L"return (s.anchorNode===e.children[1].firstChild)+'|'+s.anchorOffset;",
                &paragraphResult,&selectionError)&&paragraphResult==firstDown,
                L"Up Arrow returns to the previous line while preserving the preferred visual column");
            const wchar_t* codeBlockInputHtml=LR"HTML(<style>*{box-sizing:border-box;margin:0}article{display:block;width:280px;min-height:90px;padding:10px;font:16px/24px "Segoe UI"}pre{display:block;white-space:pre-wrap;margin:0}code{font:inherit}</style><article id="code-editor" contenteditable="true"><pre><code>helloworld</code></pre></article>)HTML";
            Check(inputView->NavigateToString(codeBlockInputHtml),
                  L"contenteditable code-block input fixture loads in a real view");
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('code-editor');const t=e.querySelector('code').firstChild;"
                L"e.addEventListener('beforeinput',function(event){this.setAttribute('data-before',event.inputType);});"
                L"e.addEventListener('input',function(event){this.setAttribute('data-input',event.inputType);});"
                L"e.focus();const r=document.createRange();r.setStart(t,5);r.collapse(true);"
                L"const s=getSelection();s.removeAllRanges();s.addRange(r);",
                nullptr,&selectionError),selectionError.c_str());
            SetFocus(inputWindow);SendMessageW(inputWindow,WM_CHAR,VK_RETURN,1);
            std::wstring codeBlockResult;
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('code-editor');const p=e.querySelector('pre');"
                L"const t=p.querySelector('code').firstChild;const s=getSelection();"
                L"return e.children.length+'|'+e.querySelectorAll('pre').length+'|'"
                L"+p.textContent.replace('\\n','<LF>')+'|'+(s.anchorNode===t)+'|'"
                L"+s.anchorOffset+'|'+e.getAttribute('data-before')+'|'"
                L"+e.getAttribute('data-input');",
                &codeBlockResult,&selectionError)&&
                codeBlockResult==L"1|1|hello<LF>world|true|6|insertParagraph|insertParagraph",
                L"Enter in PRE inserts a preserved newline without creating another code block");
            SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(L'X'),1);
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('code-editor');return e.children.length+'|'"
                L"+e.firstElementChild.textContent.replace('\\n','<LF>');",
                &codeBlockResult,&selectionError)&&codeBlockResult==L"1|hello<LF>Xworld",
                L"typing after Enter remains inside the original PRE code block");
            const wchar_t* atomicSelectionHtml=LR"HTML(<style>*{box-sizing:border-box;margin:0}article{display:block;width:280px;min-height:90px;padding:10px;font:16px/24px "Segoe UI"}p{display:block;margin:0}img{width:32px;height:24px}</style><article id="image-editor" contenteditable="true"><p id="image-line">alpha</p></article>)HTML";
            Check(inputView->NavigateToString(atomicSelectionHtml),
                  L"atomic contenteditable selection fixture loads in a real view");
            std::wstring atomicSelection;
            Check(inputView->ExecuteScript(
                L"const e=document.getElementById('image-editor');const p=document.getElementById('image-line');"
                L"e.focus();const r=document.createRange();r.setStart(p.firstChild,p.firstChild.length);r.collapse(true);"
                L"const s=getSelection();s.removeAllRanges();s.addRange(r);"
                L"document.execCommand('insertHTML',false,'<img src=\"missing.png\" alt=\"sample\">');"
                L"e.normalize();const current=getSelection();"
                L"return (current.anchorNode===p)+'|'+current.anchorOffset+'|'+current.isCollapsed;",
                &atomicSelection,&selectionError)&&atomicSelection==L"true|2|true",
                L"normalizing an editable tree preserves its collapsed DOM boundary immediately after an inserted image");
            SetFocus(inputWindow);
            for(const auto character:std::wstring(L"world"))
                SendMessageW(inputWindow,WM_CHAR,static_cast<WPARAM>(character),1);
            std::wstring atomicHtml;
            Check(inputView->ExecuteScript(
                L"return document.getElementById('image-line').innerHTML;",
                &atomicHtml,&selectionError)&&atomicHtml.find(L"<img")!=std::wstring::npos&&
                atomicHtml.size()>=5&&atomicHtml.substr(atomicHtml.size()-5)==L"world",
                L"typing at an image boundary materializes text after the image without an element-specific workaround");
            for(int index=0;index<6;++index)SendMessageW(inputWindow,WM_KEYDOWN,VK_LEFT,1);
            Check(inputView->ExecuteScript(
                L"const s=getSelection();return s.anchorNode.nodeValue+'|'+s.anchorOffset;",
                &atomicSelection,&selectionError)&&atomicSelection==L"alpha|5",
                L"left-arrow navigation crosses an atomic inline image from the following text node");
            SendMessageW(inputWindow,WM_KEYDOWN,VK_RIGHT,1);
            Check(inputView->ExecuteScript(
                L"const s=getSelection();return s.anchorNode.nodeValue+'|'+s.anchorOffset;",
                &atomicSelection,&selectionError)&&atomicSelection==L"world|0",
                L"right-arrow navigation crosses an atomic inline image from the preceding text node");
            const wchar_t* emptyInputsHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}body{font:13px/1.42 "Segoe UI Variable","Segoe UI",sans-serif}input{display:block;width:120px;height:32px;padding:0 4px;border:1px solid #ccc;font:inherit}</style><input type="text"><input type="password"><input type="number">)HTML";
            Check(inputView->NavigateToString(emptyInputsHtml),L"empty input types fixture loads in a real view");
            for(int index=0;index<3;++index){
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(10,16+32*index));
                SendMessageW(inputWindow,WM_CHAR,index==2?L'1':L'a',1);
            }
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"text, password and number inputs share the childless custom input implementation");
            Check(inputView->ExecuteScript(L"const a=document.querySelectorAll('input');return a[0].value+'|'+a[1].value+'|'+a[2].value;",&selectionResult,&selectionError)&&selectionResult==L"a|a|1",
                  L"custom editing updates every supported input type");
            const wchar_t* fractionalInputHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}input{position:absolute;left:10.5px;top:10.5px;width:120px;height:31px;padding:0;border:1px solid #ccc}</style><input type="text" placeholder="Fractional input">)HTML";
            Check(inputView->NavigateToString(fractionalInputHtml),L"fractional input geometry fixture loads in a real view");
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,20));
            Check(GetWindow(inputWindow,GW_CHILD)==nullptr,
                  L"fractional CSS geometry remains in the common renderer without an overlay HWND");
            const wchar_t* scrollHtml=LR"HTML(<style>*{margin:0;padding:0;box-sizing:border-box}#scroll{width:200px;height:80px;overflow-y:scroll}.row{height:30px}</style><div id="scroll"><div id="first" class="row">1</div><div class="row">2</div><div class="row">3</div><div class="row">4</div><div class="row">5</div><div id="last" class="row">6</div></div>)HTML";
            Check(inputView->NavigateToString(scrollHtml),L"scrollbar drag fixture loads in a real view");const auto beforeDragLayout=inputView->DumpLayoutJson();
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(192,28));SendMessageW(inputWindow,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(192,65));SendMessageW(inputWindow,WM_LBUTTONUP,0,MAKELPARAM(192,65));const auto afterDragLayout=inputView->DumpLayoutJson();
            Check(beforeDragLayout!=afterDragLayout,L"view mouse capture and thumb dragging update overflowing content layout");
            inputMessage.clear();
            Check(inputView->NavigateToString(L"<style>*{margin:0}#drop-zone{width:200px;height:80px}</style><div id='drop-zone'></div><script>document.getElementById('drop-zone').addEventListener('drop',event=>window.chrome.webview.postMessage(event.dataTransfer.files[0].name))</script>"),
                  L"native file drop fixture loads in a real view");
            wchar_t modulePath[MAX_PATH]{};
            const DWORD modulePathLength=GetModuleFileNameW(nullptr,modulePath,MAX_PATH);
            Check(modulePathLength>0&&modulePathLength<MAX_PATH,L"test executable path is available for native file drop");
            if(modulePathLength>0&&modulePathLength<MAX_PATH){
                const std::wstring dropPath(modulePath,modulePathLength);
                const SIZE_T bytes=sizeof(DROPFILES)+(dropPath.size()+2)*sizeof(wchar_t);
                HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT,bytes);
                if(memory){auto* header=static_cast<DROPFILES*>(GlobalLock(memory));
                    header->pFiles=sizeof(DROPFILES);header->pt={10,10};header->fWide=TRUE;
                    auto* name=reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(header)+sizeof(DROPFILES));
                    std::copy(dropPath.begin(),dropPath.end(),name);GlobalUnlock(memory);
                    SendMessageW(inputWindow,WM_DROPFILES,reinterpret_cast<WPARAM>(memory),0);
                }
                Check(inputMessage==L"TWebFrameTests.exe",L"native file drop reaches the DOM drop event at the pointer location");
            }
            Check(inputView->NavigateToString(L"<iframe id='sized-frame'></iframe>"),
                  L"replaced iframe sizing fixture loads in a real view");
            Check(inputView->ExecuteScript(
                L"const frame=document.getElementById('sized-frame');frame.width=240;frame.height=180;"
                L"frame.srcdoc='<main>inline frame</main>';const rect=frame.getBoundingClientRect();"
                L"return frame.width+'|'+frame.height+'|'+frame.srcdoc+'|'+rect.width+'|'+rect.height;",
                &selectionResult,&selectionError)&&
                selectionResult==L"240|180|<main>inline frame</main>|240|180",
                L"iframe width, height and srcdoc properties reflect into layout and child-frame navigation");
            if(selectionResult!=L"240|180|<main>inline frame</main>|240|180")
                std::wcerr<<L"iframe reflected sizing actual: "<<selectionResult<<L'\n';
            inputMessage.clear();
            Check(inputView->NavigateToString(LR"HTML(<!doctype html><style>
                html,body{margin:0}nav{position:relative;height:20px;z-index:9}
                aside{display:none;position:absolute;top:20px;width:60px;height:40px;background:#fff;z-index:100}
                nav:hover aside{display:block}section{position:relative;z-index:0}
                iframe{display:block;width:120px;height:80px;border:0}
                </style><nav>Menu<aside onclick="window.chrome.webview.postMessage('popup-click')">Action</aside></nav>
                <section><iframe srcdoc="<style>html,body{margin:0}button{width:120px;height:80px}</style><button onclick='window.chrome.webview.postMessage(&quot;frame-click&quot;)'>Child</button>"></iframe></section>)HTML"),
                L"generic native iframe popup input fixture loads");
            const auto framePointerScale=static_cast<double>(GetDpiForWindow(inputWindow))/96.0;
            const auto framePoint=[&](int x,int y){return MAKELPARAM(static_cast<int>(std::lround(x*framePointerScale)),
                static_cast<int>(std::lround(y*framePointerScale)));};
            SendMessageW(inputWindow,WM_MOUSEMOVE,0,framePoint(10,10));
            SendMessageW(inputWindow,WM_MOUSEMOVE,0,framePoint(20,30));
            inputMessage.clear();SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(20,30));
            SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(20,30));
            Check(inputMessage==L"popup-click",L"a popup overlapping iframe receives its own click through common CSS hit testing");
            SendMessageW(inputWindow,WM_MOUSEMOVE,0,framePoint(100,30));
            inputMessage.clear();SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(100,30));
            SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(100,30));
            Check(inputMessage==L"frame-click"&&GetFocus()==FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr),
                L"unobscured iframe contents retain pointer routing and native focus");
            Check(inputView->ExecuteScript(L"return document.querySelector('nav:hover')===null;",
                &selectionResult,&selectionError)&&selectionResult==L"true",L"moving out of popup into iframe clears its ancestor hover state");
            Check(inputView->NavigateToString(LR"HTML(<style>html,body{margin:0}</style><body><script>
                var shadowClicks=[],shadowRoot=document.body.attachShadow({mode:'closed'});
                shadowRoot.innerHTML='<style>input{display:block;width:24px;height:24px;margin:0}</style><input type="checkbox">';
                var shadowCheckbox=shadowRoot.querySelector('input');
                shadowCheckbox.addEventListener('pointerdown',e=>shadowClicks.push(e.type));
                shadowCheckbox.addEventListener('pointerup',e=>shadowClicks.push(e.type));
                shadowCheckbox.addEventListener('click',e=>shadowClicks.push(e.type+':'+e.isTrusted+':'+e.detail));
                </script>)HTML"),L"closed shadow checkbox pointer fixture loads");
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(12,12));
            SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(12,12));
            Check(inputView->ExecuteScript(L"return shadowCheckbox.checked+'|'+shadowClicks.join(',')+'|'+(document.body.shadowRoot===null);",
                &selectionResult,&selectionError)&&selectionResult==L"true|pointerdown,pointerup,click:true:1|true",
                L"a native pointer release activates a checkbox in a closed shadow tree with a trusted click");
            Check(inputView->ExecuteScript(L"shadowClicks=[];shadowCheckbox.addEventListener('click',e=>e.preventDefault(),{once:true});",
                nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(12,12));
            SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(12,12));
            Check(inputView->ExecuteScript(L"return shadowCheckbox.checked+'|'+shadowClicks.join(',');",
                &selectionResult,&selectionError)&&selectionResult==L"true|pointerdown,pointerup,click:true:1",
                L"canceling a shadow checkbox click restores the previous checked state");
            Check(inputView->ExecuteScript(L"shadowClicks=[];shadowCheckbox.addEventListener('pointerup',()=>shadowCheckbox.remove(),{once:true});",
                nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(12,12));
            SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(12,12));
            Check(inputView->ExecuteScript(L"return shadowCheckbox.checked+'|'+shadowClicks.join(',');",
                &selectionResult,&selectionError)&&selectionResult==L"true|pointerdown,pointerup",
                L"a checkbox removed during pointerup remains unactivated in a shadow tree");
            Check(inputView->NavigateToString(
                L"<style>*{margin:0}body{background:#28496a}iframe{width:160px;height:90px;border:0}</style>"
                L"<iframe id='friendly-frame'></iframe>"),
                L"friendly iframe document fixture loads in a real view");
            Check(inputView->ExecuteScript(
                L"const frame=document.getElementById('friendly-frame');"
                L"const childDocument=frame.contentWindow.document;"
                L"const sameDocument=childDocument===frame.contentDocument;"
                L"childDocument.open('text/html','replace');"
                L"childDocument.write('<style>html,body{margin:0;background:#0a5}</style>'"
                L"+'<main>painted child</main><script>window.chrome.webview.postMessage(\"friendly-frame-painted\")<\\/script>');"
                L"childDocument.close();return sameDocument+'|'+typeof childDocument.write;",
                &selectionResult,&selectionError)&&selectionResult==L"true|function",
                selectionError.c_str());
            Check(inputMessage==L"friendly-frame-painted"&&
                      FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr)!=nullptr,
                  L"contentWindow.document write and close load and render a friendly iframe document");
            Check(inputView->ExecuteScript(
                L"for(var i=0;i<20;i++)frame.contentWindow.focus();"
                L"childDocument.body.innerHTML='initial editor contents';"
                L"frame.contentWindow.blur();window.focus();window.blur();"
                L"return childDocument.body.textContent;",
                &selectionResult,&selectionError)&&selectionResult==L"initial editor contents",
                L"browsing context focus and blur APIs allow scripts to initialize iframe editor contents");
            const auto focusedFrameWindow=FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr);
            Check(focusedFrameWindow&&
                FindWindowExW(inputWindow,focusedFrameWindow,L"TWebFrame.View.1",nullptr)==nullptr,
                L"repeated iframe window focus reuses its existing browsing context");
            Check(inputView->ExecuteScript(LR"JS(
                childDocument.body.innerHTML='<div class="inside" contenteditable="true">text</div>';
                var inside=childDocument.querySelector('.inside');
                var fragment=childDocument.createDocumentFragment();
                var appended=childDocument.createElement('span');appended.className='inserted';
                appended.appendChild(childDocument.createTextNode(' appended'));fragment.appendChild(appended);
                inside.appendChild(fragment);
                childDocument.styleSheets[0].insertRule('.inside { width:100px; height:24px; color:rgb(10,20,30) }',0);
                var rect=inside.getBoundingClientRect();
                return inside.textContent==='text appended' &&
                    appended.ownerDocument===inside.ownerDocument &&
                    childDocument.getElementsByClassName('inserted').length===1 &&
                    document.querySelector('.inside')===null &&
                    Math.abs(rect.width-100)<0.01 && Math.abs(rect.height-24)<0.01 &&
                    getComputedStyle(inside).color==='rgb(10,20,30)';
            )JS",&selectionResult,&selectionError)&&selectionResult==L"true",
                  L"parent scripts update the child document's DOM, stylesheet, indexes and CSS geometry synchronously");
            Check(inputView->ExecuteScript(LR"JS(
                var nativeConfirmChoices=[];
                childDocument.addEventListener('click',function(e){
                    if(e.currentTarget!==childDocument)throw new Error('wrong child document');
                    var choice=confirm('Sign in before writing?');
                    nativeConfirmChoices.push(choice?'ok':'cancel');
                    chrome.webview.postMessage(choice?'confirmation-ok':'confirmation-cancel');
                });
            )JS",&selectionResult,&selectionError),L"parent editor callback registers on the native child document");
            scriptDialogProbe={};scriptDialogProbe.window=inputWindow;
            {
                std::atomic<bool> dialogDone=false;
                std::thread observer([&]{Sleep(40);ObserveScriptDialog();});
                inputMessage.clear();
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(20,10));
                SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(20,10));
                dialogDone=true;observer.join();
                Check(scriptDialogProbe.opened==1&&scriptDialogProbe.painted&&
                    scriptDialogProbe.blueButton&&!scriptDialogProbe.nativeDialog&&inputMessage==L"confirmation-cancel",
                    L"an iframe editor click paints a browser confirm card and Escape returns false");
                if(!scriptDialogProbe.painted||!scriptDialogProbe.blueButton)
                    std::wcerr<<L"script dialog paint: opened="<<scriptDialogProbe.opened
                        <<L" surface="<<scriptDialogProbe.surface<<L" blue="
                        <<scriptDialogProbe.blue.x<<L","<<scriptDialogProbe.blue.y<<L'\n';
                scriptDialogProbe.clickBlueButton=true;inputMessage.clear();
                dialogDone=false;
                std::thread acceptObserver([&]{
                    Sleep(40);ObserveScriptDialog();Sleep(150);
                    if(!dialogDone.load()){
                        scriptDialogProbe.fallbackUsed=true;
                        PostMessageW(inputWindow,WM_KEYDOWN,VK_RETURN,0);
                    }
                });
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(20,10));
                SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(20,10));
                dialogDone=true;acceptObserver.join();
                Check(scriptDialogProbe.opened==2&&!scriptDialogProbe.fallbackUsed&&
                    inputMessage==L"confirmation-ok"&&inputView->ExecuteScript(
                    L"return nativeConfirmChoices.join('|');",&selectionResult,&selectionError)&&selectionResult==L"cancel|ok",
                    L"clicking the CSS-rendered confirm button returns true exactly once");
                SendMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(200,120));
                SendMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(200,120));
                Check(scriptDialogProbe.opened==2,L"clicking outside the editor never opens its iframe confirmation");
                scriptDialogProbe.clickBlueButton=true;
                dialogDone=false;scriptDialogProbe.fallbackUsed=false;
                std::thread alertObserver([&]{
                    Sleep(40);ObserveScriptDialog();Sleep(150);
                    if(!dialogDone.load()){
                        scriptDialogProbe.fallbackUsed=true;
                        PostMessageW(inputWindow,WM_KEYDOWN,VK_RETURN,0);
                    }
                });
                const bool alertExecuted=inputView->ExecuteScript(
                    L"var alertResult=alert('A rendered alert');return String(alertResult)+'|'+(1+1);",
                    &selectionResult,&selectionError);
                dialogDone=true;alertObserver.join();
                Check(alertExecuted&&selectionResult==L"undefined|2"&&
                    scriptDialogProbe.opened==3&&scriptDialogProbe.painted&&
                    scriptDialogProbe.blueButton&&!scriptDialogProbe.fallbackUsed,
                    L"alert paints the common browser dialog and resumes JavaScript after its button is clicked");
                if(selectionResult!=L"undefined|2"||!scriptDialogProbe.painted||!scriptDialogProbe.blueButton)
                    std::wcerr<<L"script alert: result="<<selectionResult<<L" error="<<selectionError
                        <<L" opened="<<scriptDialogProbe.opened<<L" surface="<<scriptDialogProbe.surface
                        <<L" blue="<<scriptDialogProbe.blue.x<<L","<<scriptDialogProbe.blue.y<<L'\n';
                const HWND previousDialogFocus=GetFocus();
                RECT dialogClient{};GetClientRect(inputWindow,&dialogClient);
                PostMessageW(inputWindow,WM_LBUTTONDOWN,MK_LBUTTON,framePoint(20,10));
                PostMessageW(inputWindow,WM_LBUTTONUP,0,framePoint(20,10));
                PostMessageW(focusedFrameWindow,WM_KEYDOWN,L'A',0);
                PostMessageW(focusedFrameWindow,WM_CHAR,L'x',0);
                PostMessageW(inputWindow,WM_SIZE,SIZE_RESTORED,MAKELPARAM(dialogClient.right,dialogClient.bottom));
                PostMessageW(inputWindow,WM_TIMER,0x5747,0);
                PostMessageW(inputWindow,WM_KEYDOWN,VK_TAB,0);
                PostMessageW(inputWindow,WM_KEYDOWN,VK_RETURN,0);
                Check(inputView->ExecuteScript(LR"JS(
                    var modalResizeCount=0,modalKeyCount=0,modalTimerCount=0,modalCancelCount=0;
                    var modalBody=childDocument.body.innerHTML;
                    function modalResize(){modalResizeCount++;}
                    function modalKey(){modalKeyCount++;}
                    function modalCancel(){modalCancelCount++;}
                    window.addEventListener('resize',modalResize);
                    document.addEventListener('keydown',modalKey);
                    childDocument.addEventListener('keydown',modalKey);
                    document.body.addEventListener('pointercancel',modalCancel);
                    document.body.setPointerCapture(1);
                    setTimeout(function(){modalTimerCount++;},0);
                    var modalAnswer=confirm('Keyboard cancellation');
                    return modalAnswer+'|'+nativeConfirmChoices.length+'|'+modalResizeCount+'|'+
                        modalKeyCount+'|'+modalTimerCount+'|'+(modalBody===childDocument.body.innerHTML)+'|'+
                        (document.getElementById('script-dialog')===null)+'|'+modalCancelCount;
                )JS",&selectionResult,&selectionError)&&selectionResult==L"false|2|0|0|0|true|true|0"&&
                    GetFocus()==previousDialogFocus,
                    L"Tab and Enter cancel confirm, page input/jobs pause, and dialog DOM and focus remain isolated");
                MSG dialogMessage{};
                while(PeekMessageW(&dialogMessage,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&dialogMessage);DispatchMessageW(&dialogMessage);
                }
                Check(inputView->ExecuteScript(LR"JS(
                    window.removeEventListener('resize',modalResize);
                    document.removeEventListener('keydown',modalKey);
                    childDocument.removeEventListener('keydown',modalKey);
                    document.body.removeEventListener('pointercancel',modalCancel);
                    return (modalResizeCount>0)+'|'+modalKeyCount+'|'+modalTimerCount+'|'+
                        (modalBody===childDocument.body.innerHTML)+'|'+modalCancelCount+'|'+
                        document.body.hasPointerCapture(1);
                )JS",&selectionResult,&selectionError)&&selectionResult==L"true|0|1|true|1|false",
                    L"deferred resize and timer jobs resume after confirm without leaking the dismissal key into the page");
            }
            Check(inputView->ExecuteScript(
                L"var retainedChildBody=childDocument.body;frame.remove();return retainedChildBody.textContent;",
                &selectionResult,&selectionError)&&selectionResult==L"text appended",
                L"a retained iframe DOM node remains valid after the iframe is removed");
            inputMessage.clear();
            Check(inputView->NavigateToString(L"<main id='old-document'>old</main>"),
                  L"document replacement fixture loads in a real view");
            Check(inputView->ExecuteScript(
                L"window.replacementState='preserved';"
                L"document.open('text/html','replace');"
                L"document.write('<style>html,body{margin:0;background:#174}</style>'"
                L"+'<main id=\"new-document\">new</main>'"
                L"+'<script>window.chrome.webview.postMessage(window.replacementState)<\\/script>');"
                L"document.close();"
                L"return document.getElementById('new-document').textContent+'|'"
                L"+window.replacementState+'|'+(document.getElementById('old-document')===null);",
                &selectionResult,&selectionError)&&
                selectionResult==L"new|preserved|true"&&inputMessage==L"preserved",
                selectionError.c_str());
            if(selectionResult!=L"new|preserved|true")
                std::wcerr<<L"document replacement actual: "<<selectionResult<<L'\n';
            Check(inputView->NavigateToString(
                L"<style>*{margin:0;padding:0;box-sizing:border-box}"
                L"#frame-scroll{width:300px;height:100px;overflow-y:scroll}"
                L"#frame-spacer{height:180px}iframe{display:block;width:120px;height:60px;border:0}</style>"
                L"<div id='frame-scroll'><div id='frame-spacer'></div><iframe srcdoc='<main>scroll child</main>'></iframe></div>"),
                L"scrolling iframe fixture loads in a real view");
            (void)inputView->DumpLayoutJson();
            const HWND scrollingFrame=FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr);
            RECT frameBeforeScroll{},frameAfterScroll{};
            if(scrollingFrame)GetWindowRect(scrollingFrame,&frameBeforeScroll);
            Check(scrollingFrame!=nullptr&&inputView->ExecuteScript(
                L"document.getElementById('frame-scroll').scrollTop=140;",
                nullptr,&selectionError),selectionError.c_str());
            if(scrollingFrame)GetWindowRect(scrollingFrame,&frameAfterScroll);
            const auto frameScrollScale=static_cast<double>(GetDpiForWindow(inputWindow))/96.0;
            if(!scrollingFrame||frameAfterScroll.top>=
                   frameBeforeScroll.top-static_cast<LONG>(100.0*frameScrollScale)){
                std::wstring scrollState;
                inputView->ExecuteScript(
                    L"const s=document.getElementById('frame-scroll'),f=s.querySelector('iframe'),r=f.getBoundingClientRect();"
                    L"return s.scrollTop+'|'+s.scrollHeight+'|'+s.clientHeight+'|'+r.y;",
                    &scrollState,&selectionError);
                std::wcerr<<L"iframe scroll placement actual: before="<<frameBeforeScroll.top
                          <<L" after="<<frameAfterScroll.top<<L" scale="<<frameScrollScale
                          <<L" dom="<<scrollState<<L'\n';
            }
            Check(scrollingFrame&&frameAfterScroll.top<
                      frameBeforeScroll.top-static_cast<LONG>(100.0*frameScrollScale),
                  L"iframe child windows follow common CSS scroll coordinates at the active DPI");
            const auto wheelAtFrame=[&](HWND receiver,UINT message=WM_MOUSEWHEEL){
                POINT point{static_cast<LONG>(std::lround(20*frameScrollScale)),
                    static_cast<LONG>(std::lround(20*frameScrollScale))};
                ClientToScreen(inputWindow,&point);
                SendMessageW(receiver,message,MAKEWPARAM(0,message==WM_MOUSEWHEEL?-WHEEL_DELTA:WHEEL_DELTA),
                    MAKELPARAM(point.x,point.y));
            };
            const wchar_t* wheelParent=LR"HTML(<!doctype html><style>
                html,body{margin:0}main{width:200px;height:80px;overflow:auto}
                iframe{display:block;width:120px;height:60px;border:0}footer{height:240px}
                </style><main><iframe srcdoc="<!doctype html><style>html,body{margin:0}</style>short"></iframe><footer></footer></main>)HTML";
            Check(inputView->NavigateToString(wheelParent),L"non-scrolling iframe wheel chain fixture loads");
            (void)inputView->DumpLayoutJson();wheelAtFrame(inputWindow);
            Check(inputView->ExecuteScript(L"return document.querySelector('main').scrollTop;",
                &selectionResult,&selectionError)&&selectionResult==L"90",
                L"wheel over a non-scrolling iframe scrolls its parent document");
            inputView->ExecuteScript(L"document.querySelector('main').scrollTop=0;",nullptr,&selectionError);
            const HWND wheelChild=FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr);
            wheelAtFrame(wheelChild);
            Check(inputView->ExecuteScript(L"return document.querySelector('main').scrollTop;",
                &selectionResult,&selectionError)&&selectionResult==L"90",
                L"wheel initially delivered to a child HWND also continues in its containing document");
            Check(inputView->NavigateToString(LR"HTML(<!doctype html><style>
                html,body{margin:0}main{width:200px;height:80px;overflow:auto}
                iframe{display:block;width:120px;height:60px;border:0}footer{height:240px}
                </style><main><iframe srcdoc="<!doctype html><style>html,body{margin:0}section{height:60px;overflow:auto}div{height:300px}</style><section><div></div></section>"></iframe><footer></footer></main>)HTML"),
                L"scrollable iframe wheel chain fixture loads");
            (void)inputView->DumpLayoutJson();wheelAtFrame(inputWindow);
            Check(inputView->ExecuteScript(L"return document.querySelector('main').scrollTop;",
                &selectionResult,&selectionError)&&selectionResult==L"0",
                L"scrollable iframe consumes wheel input before its parent");
            for(int index=0;index<3;++index)wheelAtFrame(inputWindow);
            Check(inputView->ExecuteScript(L"return document.querySelector('main').scrollTop;",
                &selectionResult,&selectionError)&&selectionResult==L"90",
                L"wheel continues in the parent once iframe scrolling reaches its boundary");
            Check(inputView->NavigateToString(LR"HTML(<!doctype html><style>
                html,body{margin:0}main{width:200px;height:80px;overflow:auto}
                iframe{display:block;width:120px;height:60px;border:0}footer{height:240px}
                </style><main><iframe srcdoc="<!doctype html><style>html,body{margin:0}section{height:60px;overflow:auto;overscroll-behavior:contain}div{height:300px}</style><section><div></div></section>"></iframe><footer></footer></main>)HTML"),
                L"iframe CSS scroll containment fixture loads");
            (void)inputView->DumpLayoutJson();for(int index=0;index<5;++index)wheelAtFrame(inputWindow);
            Check(inputView->ExecuteScript(L"return document.querySelector('main').scrollTop;",
                &selectionResult,&selectionError)&&selectionResult==L"0",
                L"explicit CSS overscroll containment stops iframe boundary chaining");
            inputView->SetResourceLoader([](const std::wstring& resource,std::wstring& content){
                if(resource.find(L"middle-wheel.html")==std::wstring::npos)return false;
                content=LR"HTML(<!doctype html><style>html,body{margin:0}iframe{display:block;width:120px;height:60px;border:0}</style><iframe srcdoc="<!doctype html><style>html,body{margin:0}</style>leaf"></iframe>)HTML";
                return true;
            });
            Check(inputView->NavigateToString(LR"HTML(<!doctype html><style>
                html,body{margin:0}main{width:200px;height:80px;overflow:auto}
                iframe{display:block;width:120px;height:60px;border:0}footer{height:240px}
                </style><main><iframe src="middle-wheel.html"></iframe><footer></footer></main>)HTML"),
                L"nested iframe wheel chain fixture loads");
            (void)inputView->DumpLayoutJson();wheelAtFrame(inputWindow);
            Check(inputView->ExecuteScript(L"return document.querySelector('main').scrollTop;",
                &selectionResult,&selectionError)&&selectionResult==L"90",
                L"wheel chains through multiple non-scrolling browsing contexts to its scrollable ancestor");
            inputView->SetResourceLoader({});
            Check(inputView->NavigateToString(LR"HTML(<!doctype html><style>
                html,body{margin:0}main{width:80px;height:100px;overflow:auto}
                iframe{display:block;width:60px;height:60px;border:0}footer{width:260px;height:10px}
                </style><main><iframe srcdoc="<!doctype html><style>html,body{margin:0}</style>short"></iframe><footer></footer></main>)HTML"),
                L"horizontal iframe wheel chain fixture loads");
            (void)inputView->DumpLayoutJson();wheelAtFrame(inputWindow,WM_MOUSEHWHEEL);
            Check(inputView->ExecuteScript(L"return document.querySelector('main').scrollLeft;",
                &selectionResult,&selectionError)&&selectionResult==L"90",
                L"horizontal wheel over non-scrolling iframe also chains to parent");
            inputMessage.clear();
            Check(inputView->NavigateToString(LR"HTML(<!doctype html><style>
                html,body{margin:0}iframe{display:block;width:120.25px;height:60.5px;border:0}
                </style><iframe srcdoc="<!doctype html><script>window.addEventListener('resize',function(){window.chrome.webview.postMessage(window.innerWidth+'|'+window.innerHeight);});</script>"></iframe>)HTML"),
                L"fractional iframe viewport fixture loads");
            (void)inputView->DumpLayoutJson();
            Check(inputMessage==L"120.25|60.5",
                L"iframe viewport retains CSS-pixel dimensions independently of rounded native-window bounds at active DPI");
            Check(inputView->NavigateToString(L"<main id='frame-host'></main>"),
                  L"generic iframe attribute assignment fixture loads");
            Check(inputView->ExecuteScript(
                L"function mergeAttributes(target){for(var key,source,index=1;index<arguments.length;index++){source=arguments[index];for(key in source)target[key]=source[key];}}"
                L"function assignAttributes(target,attributes){for(var key in attributes)target[key]=attributes[key];}"
                L"const frame=document.createElement('iframe'),attributes={frameborder:0,style:'border:0',src:'about:blank'};"
                L"mergeAttributes(attributes,{id:'safe-frame',title:'creative',name:'serialized-content',scrolling:'no',width:'240',height:'600',src:'child.html'});"
                L"assignAttributes(frame,attributes);"
                L"document.getElementById('frame-host').appendChild(frame);"
                L"return frame.id+'|'+frame.name+'|'+frame.getAttribute('name')+'|'+frame.width+'|'+frame.height;",
                &selectionResult,&selectionError)&&
                selectionResult==L"safe-frame|serialized-content|serialized-content|240|600",
                L"generic property enumeration reflects iframe startup attributes");
            if(selectionResult!=L"safe-frame|serialized-content|serialized-content|240|600")
                std::wcerr<<L"iframe enumerated attributes actual: "<<selectionResult<<L'\n';
            Check(inputView->NavigateToString(
                L"<iframe id='first-frame' name='named-frame'></iframe><iframe id='second-frame'></iframe>"),
                L"Window frames collection fixture loads in a real view");
            Check(inputView->ExecuteScript(
                L"const first=document.getElementById('first-frame');const second=document.getElementById('second-frame');"
                L"return (frames===window)+'|'+frames.length+'|'+(frames[0]===first.contentWindow)+'|'"
                L"+(frames[1]===second.contentWindow)+'|'+(frames['named-frame']===first.contentWindow)+'|'"
                L"+(window['named-frame']===first.contentWindow)+'|'+('named-frame' in window);",
                &selectionResult,&selectionError)&&selectionResult==L"true|2|true|true|true|true|true",
                L"window.frames exposes a live indexed and named WindowProxy collection");
            if(selectionResult!=L"true|2|true|true|true|true|true")
                std::wcerr<<L"Window frames collection actual: "<<selectionResult<<L'\n';
            std::vector<std::wstring> frameEvents;
            inputView->SetMessageHandler([&](const std::wstring& value){frameEvents.push_back(value);});
            inputView->SetResourceLoader([](const std::wstring& path,std::wstring& source){
                if(path!=L"https://child.frames.test/child.html")return false;
                source=L"<script>window.addEventListener('message',event=>{if(event.source===parent&&event.data.kind==='reply')parent.postMessage({kind:'ack',replyOrigin:event.origin,nested:event.data.nested,contextName:window.name},'*');});parent.postMessage({kind:'ready',nested:top!==window&&top===parent,contextName:window.name},'*');</script>";
                return true;
            });
            const wchar_t* frameHtml=LR"HTML(<style>*{margin:0}iframe{width:150px;height:80px;border:0}</style><main id="frame-host"></main><script>const frame=document.createElement('iframe');frame.id='child';frame.name='serialized-context';frame.contentWindow.name='serialized-payload';window.addEventListener('message',event=>{if(event.source!==frame.contentWindow)return;if(event.data.kind==='ready')frame.contentWindow.postMessage({kind:'reply',nested:event.data.nested},'*');if(event.data.kind==='ack')window.chrome.webview.postMessage('iframe-ok|'+event.origin+'|'+event.data.replyOrigin+'|'+event.data.nested+'|'+event.data.contextName);});frame.addEventListener('load',()=>window.chrome.webview.postMessage('iframe-loaded'));frame.src='https://child.frames.test/child.html';document.getElementById('frame-host').appendChild(frame);</script>)HTML";
            Check(inputView->NavigateToString(frameHtml,L"https://parent.frames.test/index.html"),inputView->LastError().c_str());
            const auto messageDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(frameEvents.size()<2&&std::chrono::steady_clock::now()<messageDeadline){
                MSG message{};if(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}else Sleep(1);
            }
            Check(frameEvents.size()==2&&
                      frameEvents[0]==L"iframe-loaded"&&
                      frameEvents[1]==L"iframe-ok|https://child.frames.test|https://parent.frames.test|true|serialized-payload",
                  L"iframe messages expose source identity and serialized sender origins");
            if(frameEvents.size()!=2||
               frameEvents[1]!=L"iframe-ok|https://child.frames.test|https://parent.frames.test|true|serialized-payload"){
                std::wcerr<<L"iframe message events:";
                for(const auto& event:frameEvents)std::wcerr<<L" ["<<event<<L"]";
                std::wcerr<<L'\n';
            }
            Check(FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr)!=nullptr,
                  L"iframe has a separately rendered TWebFrame child window");
            frameEvents.clear();
            inputView->SetResourceLoader([](const std::wstring& path,std::wstring& source){
                if(path==L"https://middle.frames.test/child.html"){
                    source=L"<iframe src='https://deep.frames.test/grandchild.html'></iframe>";
                    return true;
                }
                if(path==L"https://deep.frames.test/grandchild.html"){
                    source=L"<script>top.postMessage({kind:'top-route',separate:top!==parent},'*');parent.parent.postMessage({kind:'chain-route'},'*');</script>";
                    return true;
                }
                return false;
            });
            Check(inputView->NavigateToString(
                L"<iframe id='middle' src='https://middle.frames.test/child.html'></iframe><script>const middle=document.getElementById('middle');window.addEventListener('message',event=>{if(event.source===middle.contentWindow.frames[0]&&event.source!==middle.contentWindow)window.chrome.webview.postMessage(event.data.kind+(event.data.separate===undefined?'':'|'+event.data.separate));});</script>",
                L"https://top.frames.test/index.html"),
                L"nested iframe messaging fixture loads");
            const auto nestedMessageDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(frameEvents.size()<2&&std::chrono::steady_clock::now()<nestedMessageDeadline){
                MSG message{};if(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}else Sleep(1);
            }
            Check(std::find(frameEvents.begin(),frameEvents.end(),L"top-route|true")!=frameEvents.end()&&
                      std::find(frameEvents.begin(),frameEvents.end(),L"chain-route")!=frameEvents.end(),
                  L"nested WindowProxy top and parent chains route messages to the top context");
            frameEvents.clear();
            inputView->SetResourceLoader([](const std::wstring& path,std::wstring& source){
                if(path==L"https://dynamic-frames.test/first.html"||
                   path==L"https://dynamic-frames.test/second.html"){
                    source=L"<main>dynamic child</main>";return true;
                }
                return false;
            });
            Check(inputView->NavigateToString(
                L"<main id='frame-host'></main><script>window.dynamicFrameEvents=[];</script>",
                L"https://dynamic-frames.test/index.html"),
                L"dynamic iframe parent fixture loads");
            Check(inputView->ExecuteScript(
                L"const frame=document.createElement('iframe');frame.id='dynamic-frame';"
                L"frame.addEventListener('load',()=>window.chrome.webview.postMessage(frame.src));"
                L"frame.src='first.html';document.getElementById('frame-host').appendChild(frame);",
                nullptr,&selectionError),selectionError.c_str());
            MSG dynamicFrameMessage{};
            while(PeekMessageW(&dynamicFrameMessage,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&dynamicFrameMessage);DispatchMessageW(&dynamicFrameMessage);
            }
            Check(frameEvents.size()==1&&frameEvents[0]==L"first.html"&&
                  FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr)!=nullptr,
                  L"a script-appended iframe loads and creates a rendered child view");
            Check(inputView->ExecuteScript(
                L"document.getElementById('dynamic-frame').src='second.html';",
                nullptr,&selectionError),selectionError.c_str());
            while(PeekMessageW(&dynamicFrameMessage,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&dynamicFrameMessage);DispatchMessageW(&dynamicFrameMessage);
            }
            Check(frameEvents.size()==2&&frameEvents[1]==L"second.html",
                  L"changing a connected iframe source reloads its child view");
            Check(inputView->ExecuteScript(L"document.getElementById('dynamic-frame').remove();",
                                           nullptr,&selectionError),selectionError.c_str());
            while(PeekMessageW(&dynamicFrameMessage,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&dynamicFrameMessage);DispatchMessageW(&dynamicFrameMessage);
            }
            Check(FindWindowExW(inputWindow,nullptr,L"TWebFrame.View.1",nullptr)==nullptr,
                  L"removing a dynamic iframe destroys its child view");
            frameEvents.clear();std::atomic<DWORD> asyncFrameLoaderThread{0};
            bool asyncFrameParentLoaded=false;
            inputView->SetParallelResourceLoading(true);
            inputView->SetLoadHandler([&](bool success,const std::wstring&){
                asyncFrameParentLoaded=success;
            });
            inputView->SetResourceLoader([&](const std::wstring& path,std::wstring& source){
                if(path!=L"https://frames.test/child.html")return false;
                asyncFrameLoaderThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                source=L"<main id='async-child'>child</main>";return true;
            });
            Check(inputView->NavigateToStringAsync(
                L"<iframe id='async-frame' src='child.html'></iframe>"
                L"<script>document.getElementById('async-frame').addEventListener('load',()=>window.chrome.webview.postMessage('async-frame-loaded'));</script>",
                L"https://frames.test/index.html"),
                L"parallel iframe parent navigation starts");
            const auto frameDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while((!asyncFrameParentLoaded||frameEvents.empty())&&
                  std::chrono::steady_clock::now()<frameDeadline){
                MSG frameMessage{};
                if(PeekMessageW(&frameMessage,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&frameMessage);DispatchMessageW(&frameMessage);
                }else Sleep(1);
            }
            Check(asyncFrameParentLoaded&&frameEvents.size()==1&&
                  frameEvents[0]==L"async-frame-loaded"&&
                  asyncFrameLoaderThread.load(std::memory_order_relaxed)!=0&&
                  asyncFrameLoaderThread.load(std::memory_order_relaxed)!=GetCurrentThreadId(),
                  L"iframe source loading and child parsing complete through the worker pipeline");
            inputView->SetLoadHandler({});inputView->SetParallelResourceLoading(false);
            inputView->SetResourceLoader([](const std::wstring& path,std::wstring& source){
                if(path.rfind(L"https://app.examples/index.html",0)!=0)return false;
                source=L"<main id='language-view'></main><script>document.getElementById('language-view').textContent=new URLSearchParams(location.search).get('lang');</script>";
                return true;
            });
            Check(inputView->NavigateToString(L"<main id='language-view'>ko-KR</main>",
                                               L"https://app.examples/index.html?lang=ko-KR"),
                  L"URL navigation fixture loads in a real view");
            Check(inputView->ExecuteScript(
                L"const next=new URL(location.href);next.searchParams.set('lang','ja-JP');location.replace(next.href);",
                nullptr,&selectionError),selectionError.c_str());
            MSG navigationMessage{};
            while(PeekMessageW(&navigationMessage,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&navigationMessage);DispatchMessageW(&navigationMessage);
            }
            Check(inputView->ExecuteScript(
                L"return document.getElementById('language-view').textContent+'|'+location.search;",
                &selectionResult,&selectionError)&&selectionResult==L"ja-JP|?lang=ja-JP",
                L"location.replace reloads the current resource with updated URL search parameters");
            std::wstring submittedUrl;
            inputView->SetPageScriptsEnabled(false);
            const auto scriptsDisabledExceptionHandler=
                AddVectoredExceptionHandler(1,CountFirstChanceCppExceptions);
            InterlockedExchange(&firstChanceCppExceptions,0);
            Check(inputView->NavigateToString(
                L"<main id='script-mode'>safe</main>"
                L"<script>const broken = ;</script>"
                L"<script>throw new Error('must not run');</script>"
                L"<button id='inline-mode' "
                L"onclick=\"document.getElementById('script-mode').textContent='unsafe'\">press</button>",
                L"https://app.examples/untrusted.html"),
                L"scripts-disabled mode accepts a page containing unsupported scripts");
            Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
                  L"scripts-disabled mode skips script elements without first-chance C++ exceptions");
            InterlockedExchange(&firstChanceCppExceptions,0);
            Check(inputView->ExecuteScript(
                L"return document.getElementById('script-mode').textContent;",
                &selectionResult,&selectionError)&&selectionResult==L"safe",
                L"scripts-disabled mode leaves script elements unexecuted");
            Check(InterlockedCompareExchange(&firstChanceCppExceptions,0,0)==0,
                  L"scripts-disabled DOM inspection does not throw first-chance C++ exceptions");
            if(scriptsDisabledExceptionHandler)
                RemoveVectoredExceptionHandler(scriptsDisabledExceptionHandler);
            Check(inputView->ExecuteScript(
                L"document.getElementById('inline-mode').click();"
                L"return document.getElementById('script-mode').textContent;",
                &selectionResult,&selectionError)&&selectionResult==L"safe",
                L"scripts-disabled mode dispatches native clicks without running inline handlers");
            inputView->SetNavigationHandler([&](const std::wstring& url,bool){submittedUrl=url;});
            Check(inputView->NavigateToString(
                L"<meta charset='euc-kr'><form method='get' action='/find'>"
                L"<input id='query' name='q' value='\xD55C\xAE00'>"
                L"<input type='hidden' name='id' value='freeboard'></form>",
                L"https://app.examples/search?existing=1"),
                L"scripts-disabled GET form fixture loads");
            Check(inputView->ExecuteScript(L"document.getElementById('query').focus()",
                                           nullptr,&selectionError),selectionError.c_str());
            SendMessageW(inputView->Window(),WM_KEYDOWN,VK_RETURN,0);
            while(PeekMessageW(&navigationMessage,nullptr,0,0,PM_REMOVE)){
                TranslateMessage(&navigationMessage);DispatchMessageW(&navigationMessage);
            }
            Check(submittedUrl.find(L"https://app.examples/find?")==0&&
                  submittedUrl.find(L"q=%C7%D1%B1%DB")!=std::wstring::npos&&
                  submittedUrl.find(L"id=freeboard")!=std::wstring::npos,
                  L"scripts-disabled GET form submits named controls using the page charset");
            inputView->SetPageScriptsEnabled(true);
            Check(inputView->NavigateToString(
                L"<link rel='stylesheet' href='/unavailable.css'>"
                L"<main id='external-script-mode'>waiting</main>"
                L"<script>document.getElementById('external-script-mode').textContent='executed';</script>",
                L"https://app.examples/scripts-enabled.html"),
                L"scripts-enabled external page tolerates an unavailable stylesheet");
            Check(inputView->ExecuteScript(
                L"return document.getElementById('external-script-mode').textContent;",
                &selectionResult,&selectionError)&&selectionResult==L"executed",
                L"page script execution is independent of external-page resource handling");
        }
        DestroyWindow(inputHost);
    }

    if(uninitializeCom)CoUninitialize();
    if(failures){std::wcerr<<failures<<L" test(s) failed\n";return 1;}
    std::wcout << L"All TWebFrame tests passed\n";return 0;
}

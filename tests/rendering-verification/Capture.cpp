#include <TWebFrame/TWebFrame.h>
#include "CaptureIO.h"
#include <objbase.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <psapi.h>
#include <wincrypt.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace fs=std::filesystem;
namespace {
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT hr,const char* message){Require(SUCCEEDED(hr),message);}
void PumpMessages(){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
int captureTimeoutMs=10000;
template<class Predicate> bool PumpUntil(Predicate predicate,int milliseconds=0){
    if(!milliseconds)milliseconds=captureTimeoutMs;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(!predicate()&&std::chrono::steady_clock::now()<deadline){PumpMessages();std::this_thread::sleep_for(std::chrono::milliseconds(2));}
    return predicate();
}
void PumpFor(int milliseconds){const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<end){PumpMessages();std::this_thread::sleep_for(std::chrono::milliseconds(2));}}
struct Image{UINT width=0,height=0;std::vector<unsigned char> pixels;};
ComPtr<IWICImagingFactory> imaging;
Image ReadPng(const fs::path& path){
    ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converter;
    Hr(imaging->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder),"decode PNG");
    Hr(decoder->GetFrame(0,&frame),"PNG frame");Hr(imaging->CreateFormatConverter(&converter),"PNG converter");
    Hr(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"convert PNG");
    Image image;Hr(converter->GetSize(&image.width,&image.height),"PNG size");
    Require(static_cast<std::uint64_t>(image.width)*image.height<=64000000,"PNG too large");
    image.pixels.resize(static_cast<size_t>(image.width)*image.height*4);
    Hr(converter->CopyPixels(nullptr,image.width*4,static_cast<UINT>(image.pixels.size()),image.pixels.data()),"PNG pixels");return image;
}
void SavePng(const fs::path& path,const Image& image){
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    Hr(imaging->CreateStream(&stream),"PNG stream");Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG destination");
    Hr(imaging->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");
    Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"initialize encoder");
    Hr(encoder->CreateNewFrame(&frame,nullptr),"encode frame");Hr(frame->Initialize(nullptr),"initialize frame");
    Hr(frame->SetSize(image.width,image.height),"set PNG size");WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    Hr(frame->SetPixelFormat(&format),"PNG format");Require(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA),"unexpected encoder format");
    Hr(frame->WritePixels(image.height,image.width*4,static_cast<UINT>(image.pixels.size()),const_cast<BYTE*>(image.pixels.data())),"write PNG");
    Hr(frame->Commit(),"commit frame");Hr(encoder->Commit(),"commit PNG");
}
struct Completion{bool done=false;HRESULT hr=E_PENDING;std::wstring text;};
struct WindowOwner {
    HWND handle=nullptr;
    ~WindowOwner(){if(handle)DestroyWindow(handle);}
    WindowOwner(const WindowOwner&)=delete;
    WindowOwner& operator=(const WindowOwner&)=delete;
    explicit WindowOwner(HWND value):handle(value){}
};
Image WindowPaint(HWND window,UINT width,UINT height){
    struct Surface{
        HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;
        ~Surface(){if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
    } surface;
    Require(surface.dc!=nullptr,"window-paint DC");
    RECT client{};Require(GetClientRect(window,&client)&&client.right==static_cast<LONG>(width)&&client.bottom==static_cast<LONG>(height),"window-paint client dimensions");
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-static_cast<LONG>(height);info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* data=nullptr;surface.bitmap=CreateDIBSection(surface.dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
    Require(surface.bitmap!=nullptr,"window-paint bitmap");surface.previous=SelectObject(surface.dc,surface.bitmap);
    Require(surface.previous&&surface.previous!=HGDI_ERROR,"window-paint selected bitmap");
    const size_t bytes=static_cast<size_t>(width)*height*4;std::memset(data,0x4d,bytes);
    SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(surface.dc),PRF_CLIENT);GdiFlush();
    Image image;image.width=width;image.height=height;const auto begin=static_cast<const unsigned char*>(data);
    image.pixels.assign(begin,begin+bytes);for(size_t i=3;i<bytes;i+=4)image.pixels[i]=255;return image;
}
struct Browser{
    bool softwareRequested=false;
    HWND host=nullptr;
    ComPtr<ICoreWebView2Environment> environment;ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2Controller3> scaling;ComPtr<ICoreWebView2> view;
    std::shared_ptr<Completion> navigation=std::make_shared<Completion>();EventRegistrationToken navToken{};
    std::wstring version;
    Browser(const fs::path& profile,UINT width,UINT height,double scale,bool software=false):softwareRequested(software){
        host=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"Rendering reference",WS_POPUP,-20000,-20000,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        Require(host!=nullptr,"reference host");ShowWindow(host,SW_SHOWNOACTIVATE);
        struct Initialization:Completion{
            bool cancelled=false;
            HWND host=nullptr;
            ComPtr<ICoreWebView2Environment> environment;
            ComPtr<ICoreWebView2Controller> controller;
            ComPtr<ICoreWebView2> view;
        };
        auto completion=std::make_shared<Initialization>();completion->host=host;
        try{
        auto options=Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if(software)Hr(options->put_AdditionalBrowserArguments(L"--disable-gpu --disable-gpu-compositing"),"software arguments");
        Hr(CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([completion](HRESULT hr,ICoreWebView2Environment* env)->HRESULT{
                if(completion->cancelled)return S_OK;
                if(FAILED(hr)||!env){completion->hr=FAILED(hr)?hr:E_FAIL;completion->done=true;return S_OK;}completion->environment=env;
                const auto started=env->CreateCoreWebView2Controller(completion->host,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [completion](HRESULT result,ICoreWebView2Controller* value)->HRESULT{
                        if(completion->cancelled){if(value)value->Close();return S_OK;}
                        completion->hr=FAILED(result)?result:(value?S_OK:E_FAIL);
                        if(SUCCEEDED(completion->hr)){completion->controller=value;completion->hr=value->get_CoreWebView2(&completion->view);}completion->done=true;return S_OK;
                    }).Get());if(FAILED(started)){completion->hr=started;completion->done=true;}return S_OK;
            }).Get()),"create environment");
        Require(PumpUntil([&]{return completion->done;}),"WebView2 initialization timeout");Hr(completion->hr,"WebView2 initialization");
        environment=completion->environment;controller=completion->controller;view=completion->view;
        Hr(controller.As(&scaling),"Controller3 is required");Hr(scaling->put_ShouldDetectMonitorScaleChanges(FALSE),"disable automatic scale");
        Hr(scaling->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS),"raw bounds");
        Hr(scaling->put_RasterizationScale(scale),"reference scale");Hr(controller->put_ZoomFactor(1.0),"page zoom");
        RECT bounds{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};Hr(controller->put_Bounds(bounds),"reference bounds");
        Hr(controller->put_IsVisible(TRUE),"reference visible");
        ComPtr<ICoreWebView2Settings> settings;Hr(view->get_Settings(&settings),"settings");Hr(settings->put_IsScriptEnabled(FALSE),"disable page scripts");
        Hr(settings->put_AreDefaultScriptDialogsEnabled(FALSE),"disable dialogs");
        LPWSTR runtime=nullptr;Hr(environment->get_BrowserVersionString(&runtime),"runtime version");version=runtime;CoTaskMemFree(runtime);
        auto nav=navigation;Hr(view->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([nav](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT{
            BOOL ok=FALSE;args->get_IsSuccess(&ok);nav->hr=ok?S_OK:E_FAIL;nav->done=true;return S_OK;
        }).Get(),&navToken),"navigation event");
        }catch(...){completion->cancelled=true;if(completion->controller)completion->controller->Close();DestroyWindow(host);host=nullptr;throw;}
    }
    ~Browser(){if(view)view->remove_NavigationCompleted(navToken);if(controller)controller->Close();if(host)DestroyWindow(host);}
    void Abort(){UINT32 pid=0;if(view&&SUCCEEDED(view->get_BrowserProcessId(&pid))&&pid){HANDLE process=OpenProcess(PROCESS_TERMINATE,FALSE,pid);if(process){TerminateProcess(process,9);CloseHandle(process);}}}
    void Navigate(const fs::path& path){
        wchar_t url[32768]{};DWORD size=32768;Hr(UrlCreateFromPathW(path.c_str(),url,&size,0),"file URL");
        NavigateUrl(url);
    }
    void NavigateUrl(const wchar_t* url){
        navigation->done=false;navigation->hr=E_PENDING;Hr(view->Navigate(url),"navigate URL");
        Require(PumpUntil([&]{return navigation->done;}),"navigation timeout");Hr(navigation->hr,"navigation failure");
    }
    std::wstring Measure(const std::wstring& script){
        auto completion=std::make_shared<Completion>();
        Hr(view->ExecuteScript(script.c_str(),Callback<ICoreWebView2ExecuteScriptCompletedHandler>([completion](HRESULT hr,LPCWSTR text)->HRESULT{
            completion->hr=hr;completion->text=text?text:L"null";completion->done=true;return S_OK;
        }).Get()),"read-only measurement");
        Require(PumpUntil([&]{return completion->done;}),"measurement timeout");Hr(completion->hr,"measurement callback");
        Require(completion->text!=L"null","read-only measurement returned null");return completion->text;
    }
    void Snapshot(const fs::path& path){
        ComPtr<IStream> stream;Hr(SHCreateStreamOnFileEx(path.c_str(),STGM_CREATE|STGM_WRITE|STGM_SHARE_EXCLUSIVE,FILE_ATTRIBUTE_NORMAL,TRUE,nullptr,&stream),"capture stream");
        auto completion=std::make_shared<Completion>();
        Hr(view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,stream.Get(),Callback<ICoreWebView2CapturePreviewCompletedHandler>([completion](HRESULT hr)->HRESULT{
            completion->hr=hr;completion->done=true;return S_OK;
        }).Get()),"capture preview");Require(PumpUntil([&]{return completion->done;}),"capture timeout");Hr(completion->hr,"capture callback");
    }
    std::wstring DevTools(const wchar_t* method,const std::wstring& parameters=L"{}"){
        auto completion=std::make_shared<Completion>();
        const auto handler=Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>([completion](HRESULT hr,LPCWSTR json)->HRESULT{
                completion->hr=hr;completion->text=json?json:L"null";completion->done=true;return S_OK;
            });
        Hr(view->CallDevToolsProtocolMethod(method,parameters.c_str(),handler.Get()),"DevTools request");
        Require(PumpUntil([&]{return completion->done;}),"DevTools timeout");Hr(completion->hr,"DevTools result");
        Require(completion->text!=L"null"&&completion->text.find(L"\"error\"")==std::wstring::npos,"DevTools returned an error");
        return completion->text;
    }
    void GraphicsSnapshot(const fs::path& path){
        DWORD sessionId=0;ProcessIdToSessionId(GetCurrentProcessId(),&sessionId);
        std::wstring info=L"null",error;ComPtr<ICoreWebView2Settings> settings;
        try{
            // Inspect the runtime's internal page in this same browser/profile,
            // before the first fixture and after the last. Fixture scripts stay off.
            Hr(view->get_Settings(&settings),"graphics settings");Hr(settings->put_IsScriptEnabled(TRUE),"internal GPU page scripts");
            NavigateUrl(L"edge://gpu");bool ready=false;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            do{
                info=Measure(L"(()=>{const v=document.querySelector('info-view');const info=v&&v.browserBridge&&v.browserBridge.gpuInfo;const rows=info&&info.basicInfo||[];const active=rows.filter(r=>/^GPU\\d+$/.test(r.description)&&String(r.value).includes('*ACTIVE*'));const renderer=rows.find(r=>r.description==='GL_RENDERER');const skia=rows.find(r=>r.description==='Skia Backend');const features=info&&info.featureStatus&&info.featureStatus.featureStatus;const software="+std::wstring(softwareRequested?L"true":L"false")+L";const backendReady=software?features&&features.gpu_compositing==='disabled_software'&&features.rasterization==='disabled_software':active.length===1&&renderer&&renderer.value&&skia&&skia.value&&skia.value!=='None'&&features;return {ready:!!(backendReady&&info.displayInfo&&info.displayInfo.length),url:location.href,info:info||null};})()");
                if(info.find(L"\"ready\":true")!=std::wstring::npos){ready=true;break;}PumpFor(20);
            }while(std::chrono::steady_clock::now()<deadline);
            Require(ready,"internal GPU diagnostics unavailable");
        }catch(const std::exception& failure){const std::string message=failure.what();error.assign(message.begin(),message.end());}
        if(settings)Hr(settings->put_IsScriptEnabled(FALSE),"restore disabled fixture scripts");
        RegressionIO::WriteText(path,L"{\"schemaVersion\":1,\"remoteSession\":"+(GetSystemMetrics(SM_REMOTESESSION)?std::wstring(L"true"):std::wstring(L"false"))+
            L",\"windowsSessionId\":"+std::to_wstring(sessionId)+L",\"windowDpi\":"+std::to_wstring(GetDpiForWindow(host))+
            L",\"available\":"+(error.empty()?L"true":L"false")+L",\"error\":"+RegressionIO::Quote(error)+L",\"gpuPage\":"+info+L"}");
        Require(error.empty(),"reference graphics environment unavailable");
    }
    void Screenshot(const fs::path& path,UINT cssWidth,UINT cssHeight){
        // Keep the original viewport. The clip uses CSS pixels and scale 1;
        // Controller3's rasterization scale determines the device pixels.
        const auto parameters=L"{\"format\":\"png\",\"fromSurface\":true,\"captureBeyondViewport\":false,\"clip\":{\"x\":0,\"y\":0,\"width\":"+
            std::to_wstring(cssWidth)+L",\"height\":"+std::to_wstring(cssHeight)+L",\"scale\":1}}";
        const auto response=DevTools(L"Page.captureScreenshot",parameters);
        const auto key=response.find(L"\"data\"");Require(key!=std::wstring::npos,"CDP screenshot data missing");
        const auto colon=response.find(L':',key+6),begin=response.find(L'\"',colon);
        Require(colon!=std::wstring::npos&&begin!=std::wstring::npos,"invalid CDP screenshot data");
        const auto end=response.find(L'\"',begin+1);
        Require(end!=std::wstring::npos&&end>begin+1&&end-begin-1<256000000,"invalid CDP screenshot length");
        const auto encoded=response.substr(begin+1,end-begin-1);DWORD bytes=0;
        Require(CryptStringToBinaryW(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,nullptr,&bytes,nullptr,nullptr)!=FALSE,"CDP screenshot base64 size");
        std::vector<BYTE> decoded(bytes);
        Require(CryptStringToBinaryW(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,decoded.data(),&bytes,nullptr,nullptr)!=FALSE,"CDP screenshot base64 decode");
        std::ofstream output(path,std::ios::binary);Require(output.is_open(),"CDP screenshot destination");
        output.write(reinterpret_cast<const char*>(decoded.data()),bytes);Require(output.good(),"CDP screenshot write");
        RegressionIO::WriteText(path.parent_path()/L"reference-cdp-parameters.json",parameters);
    }
    void DomSnapshot(const fs::path& path){
        RegressionIO::WriteText(path,DevTools(L"DOMSnapshot.captureSnapshot",LR"({"computedStyles":["display","position","font-family","font-size","color"],"includePaintOrder":true,"includeDOMRects":true})"));
    }
    void FontSnapshot(const fs::path& path){
        DevTools(L"DOM.enable");DevTools(L"CSS.enable");
        const auto document=DevTools(L"DOM.getDocument",L"{\"depth\":0}");std::wsmatch match;
        Require(std::regex_search(document,match,std::wregex(LR"("nodeId"\s*:\s*([1-9][0-9]*))")),"CDP document node missing");
        const auto nodes=DevTools(L"DOM.querySelectorAll",L"{\"nodeId\":"+match[1].str()+L",\"selector\":\"[data-case-node], [data-probe]\"}");
        Require(std::regex_search(nodes,match,std::wregex(LR"("nodeIds"\s*:\s*\[([0-9,\s]*)\])")),"CDP tracked nodes missing");
        // querySelectorAll and the read-only box walker use the same document
        // order. CDP fonts are node aggregates, not per-character baselines.
        const auto ids=match[1].str();const std::wregex number(L"[1-9][0-9]*");
        std::wostringstream out;out<<L"{\"schemaVersion\":1,\"scope\":\"[data-case-node], [data-probe] in document order; CDP platform font aggregates\",\"nodes\":[";
        size_t index=0;
        for(auto it=std::wsregex_iterator(ids.begin(),ids.end(),number);it!=std::wsregex_iterator();++it){
            if(index)out<<L',';const auto nodeId=it->str();
            out<<L"{\"trackedIndex\":"<<index++<<L",\"nodeId\":"<<nodeId<<L",\"usage\":"
               <<DevTools(L"CSS.getPlatformFontsForNode",L"{\"nodeId\":"+nodeId+L"}")<<L'}';
        }
        out<<L"]}";RegressionIO::WriteText(path,out.str());
    }
};
std::wstring Option(int argc,wchar_t** argv,const wchar_t* key,const std::wstring& fallback=L""){
    for(int i=1;i+1<argc;++i)if(std::wstring(argv[i])==key)return argv[i+1];return fallback;
}
}

int wmain(int argc,wchar_t** argv){
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))return 2;
    int errors=0;
    try{
        Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)),"WIC factory");
        captureTimeoutMs=std::stoi(Option(argc,argv,L"--timeout-ms",L"30000"));
        Require(captureTimeoutMs>=1000&&captureTimeoutMs<=300000,"invalid capture timeout");
        const UINT width=std::stoul(Option(argc,argv,L"--width",L"800")),height=std::stoul(Option(argc,argv,L"--height",L"600"));
        const float dpi=std::stof(Option(argc,argv,L"--dpi",L"96"));
        Require(width&&height&&width<=8192&&height<=8192&&(dpi==96||dpi==144),"invalid CSS viewport/DPI");
        const double scale=dpi/96.0;
        const UINT pw=static_cast<UINT>(std::lround(width*scale)),ph=static_cast<UINT>(std::lround(height*scale));
        const fs::path output=fs::absolute(Option(argc,argv,L"--out")),list=fs::absolute(Option(argc,argv,L"--cases"));
        const bool native=Option(argc,argv,L"--engine")==L"twebframe2";
        const bool software=Option(argc,argv,L"--software")==L"true";
        const bool hostMeasure=Option(argc,argv,L"--host-measure")==L"true";
        const bool fullDiagnostics=Option(argc,argv,L"--full-diagnostics")==L"true";
        UINT actualWindowDpi=GetDpiForSystem();
        Require(fs::is_regular_file(list),"case list missing");fs::create_directories(output);
        std::unique_ptr<Browser> browser;
        unsigned browserGeneration=0;
        std::wstring runtime;
        auto script=RegressionIO::ReadText(fs::absolute(Option(argc,argv,L"--measure")));
        auto referenceScript=script;
        const auto stringify=referenceScript.find(L"JSON.stringify(");
        if(stringify!=std::wstring::npos)referenceScript.replace(stringify,15,L"(");
        std::wistringstream lines(RegressionIO::ReadText(list));std::wstring line;
        unsigned completed=0,attempted=0;
        while(std::getline(lines,line)){
            if(!line.empty()&&line.back()==L'\r')line.pop_back();if(line.empty())continue;
            const fs::path fixture=line,caseOutput=output/fixture.filename();
            Require(!fs::exists(caseOutput),"case output already exists; use a new output folder");
            fs::create_directories(caseOutput);++attempted;
            RegressionIO::WriteText(output/L"current-case.txt",fixture.filename().wstring());
            const auto started=std::chrono::steady_clock::now();
            try{
                if(native){
                    WindowOwner parent(CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"TWebFrame capture",WS_POPUP,-20000,-20000,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr));
                    Require(parent.handle!=nullptr,"native host window");
                    actualWindowDpi=GetDpiForWindow(parent.handle);
                    RECT bounds{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
                    auto view=TWebFrame::View::Create(parent.handle,bounds);Require(view!=nullptr,"native View create");
                    view->SetPageScriptsEnabled(false);view->SetParallelResourceLoading(false);view->SetVisible(false);
                    Require(view->Navigate((fixture/L"index.html").wstring()),"native document load");
                    Image image;image.width=pw;image.height=ph;std::wstring diagnostics;
                    Require(view->CaptureRenderingSnapshot(width,height,dpi,image.pixels,diagnostics),"native snapshot");
                    Require(image.pixels.size()==static_cast<size_t>(pw)*ph*4,"native pixel byte count");
                    SavePng(caseOutput/L"render.png",image);
                    if(!fullDiagnostics){
                        // Keep the snapshot's actual viewport/DPI contract.
                        // Large DOM/text dumps can be requested on a diagnostic
                        // rerun; projecting metadata never changes PNG bytes.
                        const auto dom=diagnostics.find(L",\"dom\":");
                        Require(dom!=std::wstring::npos,"snapshot metadata contract missing");
                        diagnostics=diagnostics.substr(0,dom)+L",\"diagnosticsScope\":\"capture-metadata\"}";
                    }
                    RegressionIO::WriteText(caseOutput/L"layout.json",diagnostics);
                    if(hostMeasure){
                        std::wstring value,error;
                        if(view->ExecuteScript(L"return "+script+L";",&value,&error))RegressionIO::WriteText(caseOutput/L"host-measure.json",value);
                        else RegressionIO::WriteText(caseOutput/L"host-measure-error.txt",error);
                    }
                    // The host measurement is at actual window DPI. Explicit-dpi snapshot diagnostics remain authoritative.
                    RegressionIO::WriteText(caseOutput/L"resource-error.txt",view->LastError());
                }else{
                    if(!browser){
                        browser=std::make_unique<Browser>(output/(L"profile-"+std::to_wstring(++browserGeneration)),pw,ph,scale,software);
                        runtime=browser->version;
                        actualWindowDpi=GetDpiForWindow(browser->host);
                        if(browserGeneration==1)browser->GraphicsSnapshot(output/L"graphics.json");
                    }
                    browser->Navigate(fixture/L"index.html");PumpFor(60);
                    const auto measured=browser->Measure(referenceScript);
                    RegressionIO::WriteText(caseOutput/L"layout.json",measured);
                    browser->Snapshot(caseOutput/L"render.png");
                    if(fullDiagnostics){
                        browser->DomSnapshot(caseOutput/L"dom-snapshot.json");
                        browser->FontSnapshot(caseOutput/L"platform-fonts.json");
                    }
                    const auto decoded=ReadPng(caseOutput/L"render.png");
                    Require(decoded.width==pw&&decoded.height==ph,"WebView2 physical PNG dimensions mismatch");
                }
                ++completed;
                const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
                RegressionIO::WriteText(caseOutput/L"status.json",L"{\"status\":\"captured\",\"cssViewport\":["+std::to_wstring(width)+L","+std::to_wstring(height)+L"],\"dpi\":"+std::to_wstring(dpi)+L",\"pixelSize\":["+std::to_wstring(pw)+L","+std::to_wstring(ph)+L"],\"milliseconds\":"+std::to_wstring(ms)+L",\"pageScriptsEnabled\":false}");
            }catch(const std::exception& e){
                ++errors;const std::string message=e.what();
                RegressionIO::WriteText(caseOutput/L"error.txt",std::wstring(message.begin(),message.end()));
                if(browser){browser->Abort();browser.reset();PumpFor(50);}
            }
            RegressionIO::WriteText(output/L"progress.txt",std::to_wstring(attempted)+L" attempted / "+std::to_wstring(completed)+L" captured / "+std::to_wstring(errors)+L" errors");
            if(attempted%25==0){std::wcout<<attempted<<L" attempted; "<<completed<<L" captured; "<<errors<<L" errors\n"<<std::flush;}
            // Release same-origin back/forward cached documents at a bounded interval.
            if(browser&&attempted%25==0){browser.reset();PumpFor(30);}
        }
        RegressionIO::WriteText(output/L"summary.json",L"{\"schemaVersion\":1,\"engine\":"+RegressionIO::Quote(native?L"twebframe2":L"webview2")+L",\"runtime\":"+RegressionIO::Quote(runtime)+L",\"softwareRequested\":"+(software?L"true":L"false")+L",\"dpi\":"+std::to_wstring(dpi)+L",\"scale\":"+std::to_wstring(scale)+L",\"actualWindowDpi\":"+std::to_wstring(actualWindowDpi)+L",\"attempted\":"+std::to_wstring(attempted)+L",\"captured\":"+std::to_wstring(completed)+L",\"errors\":"+std::to_wstring(errors)+L"}");
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";++errors;}
    imaging.Reset();CoUninitialize();return errors?1:0;
}

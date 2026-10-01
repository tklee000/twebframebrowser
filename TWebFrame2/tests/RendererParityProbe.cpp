#include <TWebFrame/TWebFrame.h>
#include "DOM.h"
#include "JavaScript.h"
#include "../../Browser/HttpClient.h"
#include <ole2.h>
#include <WebView2.h>
#include <wrl.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using namespace TWebFrame::Internal;

static bool PumpUntil(const std::function<bool()>& ready, int seconds=60) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    do {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if(ready())return true;
        Sleep(5);
    } while(std::chrono::steady_clock::now()<end);
    return false;
}
static void WriteText(const std::filesystem::path& path,const std::wstring& text) {
    const int count=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(count,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),bytes.data(),count,nullptr,nullptr);
    std::ofstream file(path,std::ios::binary);file.write(bytes.data(),bytes.size());
}
static std::wstring ReadText(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(file)),{});
    const int count=MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
    std::wstring text(count,L'\0');MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),text.data(),count);return text;
}
static std::wstring Evaluate(ICoreWebView2* web,const wchar_t* script) {
    bool done=false;std::wstring result;
    web->ExecuteScript(script,Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
        [&](HRESULT status,LPCWSTR json)->HRESULT {if(SUCCEEDED(status)&&json)result=json;done=true;return S_OK;}).Get());
    PumpUntil([&]{return done;});return result;
}
static bool Capture(HWND window,const std::filesystem::path& path) {
    RECT rect{};GetClientRect(window,&rect);const UINT width=rect.right,height=rect.bottom;
    HDC dc=GetDC(window),memory=CreateCompatibleDC(dc);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-static_cast<LONG>(height);info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;void* pixels=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    auto previous=SelectObject(memory,bitmap);const bool copied=BitBlt(memory,0,0,width,height,dc,0,0,SRCCOPY)!=0;
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    bool ok=copied&&SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))&&
        SUCCEEDED(factory->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))&&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&
        SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))&&
        SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&
        SUCCEEDED(frame->SetSize(width,height));
    if(copied){auto* colors=static_cast<unsigned int*>(pixels);
        for(size_t index=0;index<static_cast<size_t>(width)*height;++index)colors[index]|=0xff000000u;}
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(ok)ok=SUCCEEDED(frame->SetPixelFormat(&format))&&
        SUCCEEDED(frame->WritePixels(height,width*4,width*height*4,static_cast<BYTE*>(pixels)))&&
        SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit());
    SelectObject(memory,previous);DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window,dc);return ok;
}

int wmain(int argc,wchar_t** argv) {
    if(argc<3)return 2;
    SetProcessDPIAware();CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const std::filesystem::path output=std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(output);
    const double scale=argc>3?std::wcstod(argv[3],nullptr):1.0;
    const bool production=argc>4&&std::wstring(argv[4])==L"production";
    const bool scripts=argc>4&&(std::wstring(argv[4])==L"scripts"||production);
    const bool bottom=argc>5&&std::wstring(argv[5])==L"bottom";
    const bool editor=argc>5&&std::wstring(argv[5])==L"editor";
    const bool editorClick=argc>5&&std::wstring(argv[5])==L"editor-click";
    const bool top=argc>5&&std::wstring(argv[5])==L"top";
    const bool attachments=argc>5&&std::wstring(argv[5])==L"attachments";
    const bool comments=argc>5&&std::wstring(argv[5])==L"comments";
    const int width=(bottom||editor||editorClick||top||attachments||comments)?1182:960,height=660;
    RECT bounds{0,0,width,height};
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Renderer parity probe",WS_POPUP|WS_VISIBLE,
        40,40,width,height,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ComPtr<ICoreWebView2Controller> controller;ComPtr<ICoreWebView2> web;
    bool created=false;
    CreateCoreWebView2EnvironmentWithOptions(nullptr,(output/L"profile").c_str(),nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [&](HRESULT status,ICoreWebView2Environment* environment)->HRESULT {
            if(FAILED(status)){created=true;return status;}
            return environment->CreateCoreWebView2Controller(host,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [&](HRESULT status,ICoreWebView2Controller* value)->HRESULT {
                    if(SUCCEEDED(status)&&value){controller=value;controller->get_CoreWebView2(&web);
                        controller->put_Bounds(bounds);controller->put_ZoomFactor(scale);}
                    created=true;return S_OK;
                }).Get());
        }).Get());
    if(!PumpUntil([&]{return created;})||!web)return 3;
    ComPtr<ICoreWebView2Settings> probeSettings;web->get_Settings(&probeSettings);
    probeSettings->put_AreDefaultScriptDialogsEnabled(FALSE);
    EventRegistrationToken dialogToken{};
    web->add_ScriptDialogOpening(Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
        [&](ICoreWebView2*,ICoreWebView2ScriptDialogOpeningEventArgs* args)->HRESULT {
            LPWSTR message=nullptr;args->get_Message(&message);
            if(message){WriteText(output/L"webview-dialog.txt",message);CoTaskMemFree(message);}
            args->Accept();return S_OK;
        }).Get(),&dialogToken);
    const std::wstring input=argv[1];
    const bool live=input.rfind(L"https://",0)==0||input.rfind(L"http://",0)==0;
    bool loaded=false;EventRegistrationToken token{};
    web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [&](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*)->HRESULT {loaded=true;return S_OK;}).Get(),&token);
    std::wstring html;
    if(live)web->Navigate(input.c_str());
    else {html=ReadText(input);ComPtr<ICoreWebView2Settings> settings;
        web->get_Settings(&settings);settings->put_IsScriptEnabled(scripts);
        if(scripts){
            ComPtr<ICoreWebView2_3> virtualHost;web.As(&virtualHost);
            const auto file=std::filesystem::absolute(input);
            virtualHost->SetVirtualHostNameToFolderMapping(L"renderer-fixture.test",file.parent_path().c_str(),COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            const auto url=L"https://renderer-fixture.test/"+file.filename().wstring();web->Navigate(url.c_str());
        }else web->NavigateToString(html.c_str());}
    if(!PumpUntil([&]{return loaded;}))return 4;
    const auto settle=std::chrono::steady_clock::now()+std::chrono::seconds(live?20:2);
    PumpUntil([&]{return std::chrono::steady_clock::now()>=settle;},25);
    if(bottom)Evaluate(web.Get(),L"window.scrollTo(0,document.body.scrollHeight)");
    const std::wstring alignEditor=LR"JS((function(){var frame=document.querySelector('iframe.cheditor-editarea');
        if(frame)window.scrollTo(0,window.pageYOffset+frame.getBoundingClientRect().top-100);})())JS";
    if(editor||editorClick)Evaluate(web.Get(),alignEditor.c_str());
    const std::wstring captureConfirm=LR"JS(window.probeConfirmMessages=[];
        window.confirm=function(message){window.probeConfirmMessages.push(String(message));return false;};)JS";
    if(editorClick){
        Evaluate(web.Get(),captureConfirm.c_str());
        Evaluate(web.Get(),L"document.querySelector('iframe.cheditor-editarea').contentDocument.body.click()");
        WriteText(output/L"webview-click-confirm.json",Evaluate(web.Get(),L"JSON.stringify(window.probeConfirmMessages)"));
        WriteText(output/L"webview-click-gate.json",Evaluate(web.Get(),L"String(checkLoginCount)"));
    }
    const std::wstring alignAttachments=LR"JS((function(){var buttons=document.querySelector('.buttonBox-cover');
        if(buttons)window.scrollTo(0,window.pageYOffset+buttons.getBoundingClientRect().top-200);})())JS";
    if(attachments)Evaluate(web.Get(),alignAttachments.c_str());
    const std::wstring alignComments=LR"JS((function(){var vote=document.querySelector('.comment_template_depth1_vote .cmd_box');
        if(vote)window.scrollTo(0,window.pageYOffset+vote.getBoundingClientRect().top-100);})())JS";
    if(comments)Evaluate(web.Get(),alignComments.c_str());
    const std::wstring frameContents=LR"JS((function(){var frames=document.querySelectorAll('iframe'),out=[];
        for(var i=0;i<frames.length;i++){try{var d=frames[i].contentDocument;
            if(d&&d.body)out.push({className:frames[i].className,html:d.body.innerHTML,text:d.body.textContent});}
            catch(e){}}return JSON.stringify(out);})())JS";
    WriteText(output/L"webview-frame-contents.json",Evaluate(web.Get(),frameContents.c_str()));
    static const wchar_t* metrics=LR"JS((function(){var out=[];var nodes=document.querySelectorAll('*');
        for(var i=0;i<nodes.length;i++){var n=nodes[i];var r=n.getBoundingClientRect();var s=getComputedStyle(n);
            if(r.width>0&&r.height>0&&r.top<660&&r.bottom>0)out.push([n.tagName,n.id,n.className,
                r.x,r.y,r.width,r.height,s.display,s.color,s.backgroundColor,s.fontSize,s.lineHeight,
                s.marginTop,s.marginBottom,s.paddingTop,s.paddingBottom,s.overflow,s.verticalAlign,s.fontFamily].join('|'));}
        return out.join('\n');})())JS";
    WriteText(output/L"webview-metrics.json",Evaluate(web.Get(),metrics));
    WriteText(output/L"webview-fixture-messages.json",Evaluate(web.Get(),L"JSON.stringify(window.probeErrors)"));
    ComPtr<IStream> preview;SHCreateStreamOnFileEx((output/L"webview.png").c_str(),STGM_CREATE|STGM_WRITE|STGM_SHARE_EXCLUSIVE,
        FILE_ATTRIBUTE_NORMAL,TRUE,nullptr,&preview);
    bool captured=false;
    web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,preview.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([&](HRESULT)->HRESULT {captured=true;return S_OK;}).Get());
    PumpUntil([&]{return captured;});preview.Reset();
    if(live&&production){
        HttpClient sourceClient;const auto response=sourceClient.Get(input,input);
        if(!response.Ok())return 5;
        html=HttpClient::DecodeText(response);
    }else if(live){
        auto snapshot=Evaluate(web.Get(),LR"JS((function(){var clone=document.documentElement.cloneNode(true);
            var frames=clone.querySelectorAll('iframe');for(var i=0;i<frames.length;i++){
                frames[i].setAttribute('src','about:blank');frames[i].removeAttribute('srcdoc');}
            var scripts=clone.querySelectorAll('script');for(var j=0;j<scripts.length;j++)scripts[j].remove();
            return clone.outerHTML;})())JS");
        Document decoderDocument;JavaScriptRuntime decoder(decoderDocument);std::wstring error;
        if(!decoder.Execute(L"return "+snapshot+L";",&html,&error))return 5;
        html=L"<!doctype html><base href='"+input+L"'>"+html;
        WriteText(output/L"snapshot.html",html);
    }
    controller->Close();controller.Reset();web.Reset();
    // Direct LayoutEngine raster probes exercise non-system scales separately;
    // the view comparison here uses the host's real Windows monitor DPI.
    auto view=TWebFrame::View::Create(host,bounds);
    std::wstring sourceLocation=live?input:L"";
    if(!live){Document parsed;std::wstring parseError;parsed.Parse(html,&parseError);
        const auto bases=parsed.QuerySelectorAll(L"base");
        if(!bases.empty())sourceLocation=bases.front()->Attribute(L"href");}
    HttpClient client;
    view->SetPageScriptsEnabled(production);
    std::wstring messages;
    view->SetMessageHandler([&](const std::wstring& message){messages+=message+L"\n";});
    view->SetResourceLoader([&](const std::wstring& url,std::wstring& content){
        const auto response=client.Get(url,input);if(!response.Ok())return false;
        content=HttpClient::DecodeText(response);return true;
    });
    view->SetBinaryResourceLoader([&](const std::wstring& url,std::vector<unsigned char>& bytes){
        const auto response=client.Get(url,input);if(!response.Ok())return false;bytes=response.body;return true;
    });
    view->SetParallelResourceLoading(true);bool adopted=false,loadSuccess=false;std::wstring loadError;
    view->SetLoadHandler([&](bool success,const std::wstring& error){loadSuccess=success;loadError=error;adopted=true;});
    if(!view->NavigateToStringAsync(html,sourceLocation))return 6;
    if(!PumpUntil([&]{return adopted;},120))return 8;
    if(!loadSuccess){std::wcerr<<L"load failed: "<<loadError<<L'\n';return 9;}
    if(scripts&&!production){
        Document parsed;parsed.Parse(html);std::wstring log;
        for(const auto& script:parsed.GetElementsByTagName(L"script")){
            const auto type=script->Attribute(L"type");
            if(!type.empty()&&type!=L"text/javascript"&&type!=L"application/javascript")continue;
            const auto src=script->Attribute(L"src");std::wstring code=script->InnerText();
            if(!src.empty())code=HttpClient::DecodeText(client.Get(src,sourceLocation));
            std::wstring scriptError;
            const bool ok=view->ExecuteScript(code,nullptr,&scriptError);
            log+=(src.empty()?L"inline":src)+L": "+(ok?L"OK":scriptError)+L"\n";
        }
        WriteText(output/L"script-execution.txt",log);
    }
    const auto paintEnd=std::chrono::steady_clock::now()+std::chrono::seconds(production&&live?20:3);
    PumpUntil([&]{return std::chrono::steady_clock::now()>=paintEnd;},25);
    std::wstring result,error;
    if(bottom){view->ExecuteScript(L"window.scrollTo(0,document.body.scrollHeight)",nullptr,&error);
        PumpUntil([]{return false;},1);}
    if(editor||editorClick){view->ExecuteScript(alignEditor,nullptr,&error);PumpUntil([]{return false;},1);}
    if(attachments){view->ExecuteScript(alignAttachments,nullptr,&error);PumpUntil([]{return false;},1);}
    if(comments){view->ExecuteScript(alignComments,nullptr,&error);PumpUntil([]{return false;},1);}
    if(editorClick){
        view->ExecuteScript(captureConfirm,nullptr,&error);
        view->ExecuteScript(L"var probeFrame=document.querySelector('iframe.cheditor-editarea');"
            L"var probeRect=probeFrame.getBoundingClientRect();return (probeRect.x+24)+'|'+(probeRect.y+24);",&result,&error);
        const auto separator=result.find(L'|');
        if(separator!=std::wstring::npos){
            const double dpiScale=static_cast<double>(GetDpiForWindow(view->Window()))/96.0;
            const int x=static_cast<int>(std::lround(std::wcstod(result.c_str(),nullptr)*dpiScale));
            const int y=static_cast<int>(std::lround(std::wcstod(result.c_str()+separator+1,nullptr)*dpiScale));
            const auto point=MAKELPARAM(x,y);
            SendMessageW(view->Window(),WM_MOUSEMOVE,0,point);
            SendMessageW(view->Window(),WM_LBUTTONDOWN,MK_LBUTTON,point);
            SendMessageW(view->Window(),WM_LBUTTONUP,0,point);
        }
        view->ExecuteScript(L"return JSON.stringify(window.probeConfirmMessages);",&result,&error);
        WriteText(output/L"twebframe-click-confirm.json",result);
        view->ExecuteScript(L"return String(checkLoginCount);",&result,&error);
        WriteText(output/L"twebframe-click-gate.txt",result);
    }
    view->ExecuteScript(L"return "+frameContents+L";",&result,&error);
    WriteText(output/L"twebframe-frame-contents.json",result);
    const bool inspected=view->ExecuteScript(L"return "+std::wstring(metrics)+L";",&result,&error);
    WriteText(output/L"twebframe-metrics.txt",result);WriteText(output/L"layout.json",view->DumpLayoutJson());
    std::wstring dom;view->ExecuteScript(L"return document.body.innerHTML;",&dom,&error);
    WriteText(output/L"twebframe-dom.html",dom);
    WriteText(output/L"twebframe-errors.txt",view->LastError()+L"\n"+messages);
    view->ExecuteScript(L"return JSON.stringify(window.probeErrors);",&result,&error);
    WriteText(output/L"fixture-messages.json",result);
    const bool saved=Capture(view->Window(),output/L"twebframe.png");
    std::wcout<<L"metrics="<<inspected<<L" capture="<<saved<<L" error="<<error<<L'\n';
    view.reset();DestroyWindow(host);CoUninitialize();return inspected&&saved?0:7;
}

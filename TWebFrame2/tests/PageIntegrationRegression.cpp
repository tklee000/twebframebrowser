#include "../../Browser/HttpClient.h"
#include <TWebFrame/TWebFrame.h>
#include <objbase.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>


bool SaveViewPixels(HWND window,const wchar_t* filename){
    RECT bounds{};if(!GetClientRect(window,&bounds))return false;
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=bounds.right;
    info.bmiHeader.biHeight=-bounds.bottom;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!bitmap){DeleteDC(dc);return false;}const auto previous=SelectObject(dc,bitmap);
    SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN);
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info.bmiHeader);
    const auto bytes=static_cast<DWORD>(bounds.right*bounds.bottom*4);file.bfSize=file.bfOffBits+bytes;
    std::ofstream output(std::filesystem::path(filename),std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file),sizeof(file));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels),bytes);const bool saved=static_cast<bool>(output);
    SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);return saved;
}

void SaveTextSnapshot(const std::wstring& text,const std::filesystem::path& filename){
    const int length=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string utf8(static_cast<size_t>(length),'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),utf8.data(),length,nullptr,nullptr);
    std::ofstream output(filename,std::ios::binary);output.write(utf8.data(),utf8.size());
}
void SaveLayoutSnapshot(TWebFrame::View& view,const std::filesystem::path& filename){
    SaveTextSnapshot(view.DumpLayoutJson(true),filename);
}

int wmain(int argc,wchar_t** argv){
    if(argc<2){std::wcerr<<L"Usage: PageIntegrationRegression https://host/path [observation-seconds] [output-directory] [--visible] [--native] [--security-budget] [--click=x,y]\n";return 2;}
    bool visible=false,native=false,clickRequested=false,securityBudget=false;int clickX=0,clickY=0;
    for(int i=4;i<argc;++i){
        const std::wstring option=argv[i];
        if(option==L"--visible")visible=true;
        else if(option==L"--native")native=true;
        else if(option==L"--security-budget")securityBudget=true;
        else if(option.rfind(L"--click=",0)==0&&swscanf_s(option.c_str()+8,L"%d,%d",&clickX,&clickY)==2&&clickX>=0&&clickY>=0&&clickX<32768&&clickY<32768)clickRequested=true;
        else {std::wcerr<<L"Unknown or invalid option\n";return 2;}
    }
    const auto observationSeconds=argc>=3?std::max(1,std::min(600,_wtoi(argv[2]))):60;
    const auto outputDirectory=argc>=4?std::filesystem::path(argv[3]):std::filesystem::path(L"TWebFrame2/tests/artifacts");
    std::filesystem::create_directories(outputDirectory);
    std::wcout<<std::unitbuf;
    std::wcout<<L"PROCESS "<<GetCurrentProcessId()<<L'\n';
    const auto observationStart=std::chrono::steady_clock::now();
    const auto elapsed=[&]{return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-observationStart).count();};
    HttpClient client;const std::wstring url=argv[1];
    const auto page=client.Get(url,L"",true);
    std::wcout<<L"PAGE "<<page.status<<L" bytes="<<page.body.size()<<L'\n';
    if(!page.Ok())return 1;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"TWebFrame integration regression",visible?WS_OVERLAPPEDWINDOW:WS_POPUP,
                             0,0,1000,760,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    RECT bounds{0,0,1000,760};auto view=TWebFrame::View::Create(host,bounds);
    if(!view){DestroyWindow(host);return 1;}
    view->SetBrowserContext(client.Context());
    if(native)view->SetCompatibilityBridgeEnabled(false);
    if(visible){ShowWindow(host,SW_SHOWNORMAL);SetForegroundWindow(host);SetFocus(view->Window());}
    auto outputMutex=std::make_shared<std::mutex>();
    view->SetPageScriptsEnabled(true);view->SetParallelResourceLoading(true);
    view->SetMessageHandler([&](const std::wstring& message){
        {
            std::lock_guard<std::mutex> lock(*outputMutex);std::wcout<<L"SCRIPT_MESSAGE ms="<<elapsed()<<L' '<<message<<L"|error="<<view->LastError()<<L'\n';
        }
    });
    view->SetLoadHandler([&](bool ok,const std::wstring& error){
        {
            std::lock_guard<std::mutex> lock(*outputMutex);
            std::wcout<<L"LOAD "<<ok<<L" error="<<error<<L'\n';
        }
        if(ok&&!native)view->ExecuteScript(LR"JS(
            window.addEventListener('message',function(event){
                var data=event.data;
                window.chrome.webview.postMessage('origin='+event.origin+'|trusted='+event.isTrusted+'|event='+(data&&data.event)+'|source='+(data&&data.source)+'|mode='+(data&&data.mode)+'|widget='+(data&&data.widgetId)+'|code='+(data&&data.code));
            });
        )JS");
    });
    view->SetNetworkResourceLoader([context=client.Context(),outputMutex,outputDirectory](const TWebFrame::NetworkRequest& request){
        auto result=context->Request(request);
        // URLs are printed without query strings; response cookies are not logged.
        {std::lock_guard<std::mutex> lock(*outputMutex);
            std::wcout<<L"RESOURCE "<<result.status<<L" bytes="<<result.bytes.size()<<L' '<<request.url.substr(0,request.url.find_first_of(L"?#"))<<L'\n';}
        return result;
    });
    view->SetResourceLoader([&](const std::wstring& target,std::wstring& text){
        const auto response=client.Get(target,page.url);
        if(response.Ok())text=HttpClient::DecodeText(response);
        std::lock_guard<std::mutex> lock(*outputMutex);
        // Keep URL query values out of diagnostic logs.
        auto printableError=response.error;for(auto& character:printableError)if(character>127)character=L'?';
        std::wcout<<L"TEXT "<<response.status<<L" chars="<<text.size()<<L" error="<<printableError<<L' '
                  <<target.substr(0,target.find_first_of(L"?#"))<<L'\n';
        return response.Ok();
    });
    view->SetBinaryResourceLoader([&](const std::wstring& target,std::vector<unsigned char>& data){
        const auto response=client.Get(target,page.url);if(response.Ok())data=response.body;
        return response.Ok();
    });
    // The security run is bounded from login navigation, including preparation.
    // Its deadline never moves when a callback yields or catches AbortError.
    auto deadline=securityBudget?observationStart+std::chrono::seconds(10):
        std::chrono::steady_clock::now()+std::chrono::seconds(observationSeconds);
    bool executionTimeLimit=false;
    view->SetExecutionYieldHandler([&]{
        if(std::chrono::steady_clock::now()<deadline)return true;
        executionTimeLimit=true;return false;
    });
    auto html=HttpClient::DecodeText(page);
    const bool started=view->NavigateToStringAsync(html,page.url);
    bool settling=false,clicked=false,securitySuccess=false;
    auto nextSecuritySnapshot=std::chrono::steady_clock::now();
    while(started&&std::chrono::steady_clock::now()<deadline){
        MSG message{};while(std::chrono::steady_clock::now()<deadline&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(clickRequested&&!clicked&&elapsed()>=10000){
            clicked=true;SaveLayoutSnapshot(*view,outputDirectory/L"page-script-before-click.json");
            const auto scale=static_cast<double>(GetDpiForWindow(view->Window()))/96.0;
            const auto point=MAKELPARAM(static_cast<int>(std::lround(clickX*scale)),static_cast<int>(std::lround(clickY*scale)));
            SendMessageW(view->Window(),WM_MOUSEMOVE,0,point);
            SendMessageW(view->Window(),WM_LBUTTONDOWN,MK_LBUTTON,point);
            SendMessageW(view->Window(),WM_LBUTTONUP,0,point);
            std::wcout<<L"POINTER_CLICK "<<clickX<<L','<<clickY<<L" error="<<view->LastError()<<L'\n';
            SaveLayoutSnapshot(*view,outputDirectory/L"page-script-after-click.json");
        }
        if(securityBudget&&std::chrono::steady_clock::now()<deadline&&
           std::chrono::steady_clock::now()>=nextSecuritySnapshot){
            const auto snapshot=view->DumpLayoutJson(true);
            SaveTextSnapshot(snapshot,outputDirectory/L"page-script-layout.json");
            nextSecuritySnapshot=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
            const auto messages=snapshot.find(L"\"receivedMessages\":[");
            if(messages!=std::wstring::npos){
                const auto end=snapshot.find(L']',messages);
                const auto complete=snapshot.find(L"\"complete\"",messages);
                securitySuccess=std::chrono::steady_clock::now()<deadline&&end!=std::wstring::npos&&complete<end;
                if(securitySuccess)break;
            }
        }
        // A long script job may finish after the initial observation period.
        // Allow its ordinary queued frame messages to reach the parent view.
        if(!securityBudget&&!settling&&std::chrono::steady_clock::now()>=deadline){
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);settling=true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::wcout<<L"RUNTIME_ERROR "<<view->LastError()<<L'\n';
    std::wcout<<L"EXECUTION_TIME_LIMIT "<<executionTimeLimit<<L'\n';
    if(securityBudget)std::wcout<<L"SECURITY_RESULT "<<(securitySuccess?L"complete":L"timeout")<<L" elapsed_ms="<<elapsed()<<L'\n';
    SaveLayoutSnapshot(*view,outputDirectory/L"page-script-layout.json");
    SaveViewPixels(view->Window(),(outputDirectory/L"page-script-render.bmp").c_str());
    // Do not run another page script, deliver timers, or extend observation
    // after a security timeout. The snapshot above is native diagnostic data.
    if(securityBudget){
        view.reset();DestroyWindow(host);if(SUCCEEDED(com))CoUninitialize();
        return !started?1:securitySuccess?0:4;
    }
    view->SetExecutionYieldHandler({});
    std::wstring result;
    const bool textInspected=view->ExecuteScript(LR"JS(
        var walker=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT),node,parts=[];
        while(node=walker.nextNode()){
            var parent=node.parentNode;
            if(parent && parent.closest('script,style,noscript,template'))continue;
            parts.push(node.textContent);
        }
        return parts.join('\n').slice(0,16000);
    )JS",&result);
    SaveTextSnapshot(result,outputDirectory/L"page-script-text.txt");
    std::wcout<<L"BODY_TEXT "<<textInspected<<L" chars="<<result.size()<<L'\n';
    view.reset();DestroyWindow(host);if(SUCCEEDED(com))CoUninitialize();return started?0:1;
}

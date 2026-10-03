#include "../src/DOM.h"
#include "../src/JavaScript.h"
#include "RegressionIO.h"
#include <ole2.h>
#include <mmsystem.h>
#pragma comment(lib,"winmm.lib")
#include <WebView2.h>
#include <wrl.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using namespace TWebFrame::Internal;

// Match View's notification-driven Worker host. No artificial polling delay
// is included in measured message delivery; ordinary timers keep their due time.
struct NativeHostWake {
    std::mutex mutex;
    std::condition_variable changed;
    bool pending=false;
    std::chrono::steady_clock::time_point due=std::chrono::steady_clock::time_point::max();
    void Notify(){std::lock_guard<std::mutex> lock(mutex);pending=true;changed.notify_one();}
    void Schedule(unsigned delay){
        std::lock_guard<std::mutex> lock(mutex);
        due=delay?std::chrono::steady_clock::now()+std::chrono::milliseconds(delay):std::chrono::steady_clock::time_point::max();
        changed.notify_one();
    }
    void Wait(std::chrono::steady_clock::time_point deadline){
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_until(lock,std::min(due,deadline),[&]{return pending;});pending=false;
    }
};

static bool PumpUntil(const bool& done) {
    const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    while(!done&&std::chrono::steady_clock::now()<limit) {
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done;
}

int wmain(int argc,wchar_t** argv) {
    if(argc<4) {
        std::wcerr<<L"Usage: RuntimeBenchmark twebframe|webview2 setup.js output.json [runs] [jit-threshold] [--async]\n";
        return 2;
    }
    const std::wstring engine=argv[1],source=RegressionIO::ReadText(argv[2]);
    const auto highResolution=timeBeginPeriod(1);
    struct TimerScope {bool active;~TimerScope(){if(active)timeEndPeriod(1);}} timerScope{highResolution==TIMERR_NOERROR};
    const int runs=argc>4?std::max(1,_wtoi(argv[4])):3;
    bool asynchronous=false;for(int index=5;index<argc;++index)if(std::wstring(argv[index])==L"--async")asynchronous=true;
    const std::wstring probe=LR"JS((function(){var t=performance.now();var result=benchmarkRun();var ms=performance.now()-t;if(typeof benchmarkNormalize==='function')result=benchmarkNormalize(result);return JSON.stringify({ms:ms,result:result});})())JS";
    const std::wstring asyncProbe=LR"JS((function(){window.__benchmarkPending=true;window.__benchmarkResult=undefined;var t=performance.now();try{Promise.resolve(benchmarkRun()).then(function(result){window.__benchmarkResult=JSON.stringify({ms:performance.now()-t,result:result});window.__benchmarkPending=false;},function(error){window.__benchmarkResult=JSON.stringify({error:String(error&&error.stack||error)});window.__benchmarkPending=false;});}catch(error){window.__benchmarkResult=JSON.stringify({error:String(error&&error.stack||error)});window.__benchmarkPending=false;}})())JS";
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(FAILED(initialized))return 1;
    struct ComScope {~ComScope(){CoUninitialize();}} comScope;
    std::wstring results=L"[";
    int status=0;
    if(engine==L"twebframe") {
        NativeHostWake wake;Document document;JavaScriptRuntime runtime(document);
        runtime.SetCompatibilityBridgeEnabled(false);
        runtime.SetLocation(L"https://benchmark.example/");
        if(argc>5&&std::wstring(argv[5])!=L"--async")runtime.SetJitCompilationThreshold(static_cast<size_t>(std::max(0,_wtoi(argv[5]))));
        if(asynchronous){
            runtime.SetTimerScheduler([&](unsigned delay){wake.Schedule(delay);});
            runtime.SetWorkerWakeHandler([&]{wake.Notify();});
        }
        std::wstring error;
        if(!runtime.Execute(source,nullptr,&error)){std::wcerr<<error<<L'\n';return 1;}
        for(int i=0;i<runs;++i) {
            std::wstring result;
            if(!asynchronous){if(!runtime.Execute(L"return "+probe+L";",&result,&error)){std::wcerr<<error<<L'\n';return 1;}}
            else{
                if(!runtime.Execute(asyncProbe,nullptr,&error)){std::wcerr<<error<<L'\n';return 1;}
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
                bool done=false;
                do{
                    runtime.RunTimers();std::wstring pending;
                    if(!runtime.Execute(L"return window.__benchmarkPending;",&pending,&error))return 1;
                    done=pending==L"false";
                    if(!done)wake.Wait(deadline);
                }while(!done&&std::chrono::steady_clock::now()<deadline);
                if(!done||!runtime.Execute(L"return window.__benchmarkResult;",&result,&error)){std::wcerr<<L"Async benchmark timed out: "<<error<<L'\n';return 1;}
            }
            if(result.find(L"\"error\":")!=std::wstring::npos){std::wcerr<<result<<L'\n';return 1;}
            if(i)results+=L',';results+=result;
            std::wcout<<i<<L' '<<result<<L'\n';
        }
        RegressionIO::WriteText(std::filesystem::path(argv[3]).wstring()+L".diagnostics.json",runtime.DiagnosticsJson());
    } else if(engine==L"webview2") {
        HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"Runtime benchmark",WS_POPUP,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        ComPtr<ICoreWebView2Environment> environment;
        ComPtr<ICoreWebView2Controller> controller;
        ComPtr<ICoreWebView2> webview;
        bool ready=false;HRESULT initialization=E_FAIL;
        const auto profile=std::filesystem::absolute(std::filesystem::path(argv[3]).parent_path()/L"webview2-profile").wstring();
        HRESULT started=CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT result,ICoreWebView2Environment* value)->HRESULT {
                initialization=result;
                if(FAILED(result)||!value){ready=true;return S_OK;}
                environment=value;
                HRESULT created=environment->CreateCoreWebView2Controller(host,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([&](HRESULT hr,ICoreWebView2Controller* value)->HRESULT {
                        initialization=hr;
                        if(SUCCEEDED(hr)&&value){controller=value;controller->get_CoreWebView2(&webview);controller->put_IsVisible(FALSE);RECT bounds{0,0,800,600};controller->put_Bounds(bounds);}
                        ready=true;return S_OK;
                    }).Get());
                if(FAILED(created)){initialization=created;ready=true;}
                return S_OK;
            }).Get());
        if(FAILED(started)||!PumpUntil(ready)||FAILED(initialization)||!webview){std::wcerr<<L"WebView2 initialization failed\n";return 1;}
        LPWSTR version=nullptr;
        if(SUCCEEDED(environment->get_BrowserVersionString(&version))&&version){
            RegressionIO::WriteText(std::filesystem::path(argv[3]).wstring()+L".engine.txt",version);CoTaskMemFree(version);
        }
        bool navigated=false;EventRegistrationToken token{};
        webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([&](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT {BOOL ok=FALSE;args->get_IsSuccess(&ok);if(!ok)status=1;navigated=true;return S_OK;}).Get(),&token);
        if(FAILED(webview->NavigateToString(L"<!doctype html><html><body></body></html>"))||!PumpUntil(navigated)||status)return 1;
        const auto execute=[&](const std::wstring& script,std::wstring& output)->bool {
            bool done=false;HRESULT completion=E_FAIL;
            const auto hr=webview->ExecuteScript(script.c_str(),Callback<ICoreWebView2ExecuteScriptCompletedHandler>([&](HRESULT result,LPCWSTR json)->HRESULT {completion=result;output=json?json:L"null";done=true;return S_OK;}).Get());
            return SUCCEEDED(hr)&&PumpUntil(done)&&SUCCEEDED(completion);
        };
        std::wstring ignored;
        if(!execute(source,ignored))return 1;
        const std::wstring webProbe=LR"JS((function(){var t=performance.now();var result=benchmarkRun();var ms=performance.now()-t;if(typeof benchmarkNormalize==='function')result=benchmarkNormalize(result);return {ms:ms,result:result};})())JS";
        for(int i=0;i<runs;++i){
            std::wstring result;
            if(!asynchronous){if(!execute(webProbe,result)||result==L"null")return 1;}
            else{
                if(!execute(asyncProbe,result))return 1;
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);bool done=false;
                do{std::wstring pending;if(!execute(L"window.__benchmarkPending",pending))return 1;done=pending==L"false";if(!done)std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(!done&&std::chrono::steady_clock::now()<deadline);
                if(!done||!execute(L"JSON.parse(window.__benchmarkResult)",result)||result==L"null")return 1;
            }
            if(result.find(L"\"error\":")!=std::wstring::npos){std::wcerr<<result<<L'\n';return 1;}
            if(i)results+=L',';results+=result;std::wcout<<i<<L' '<<result<<L'\n';
        }
        controller->Close();DestroyWindow(host);
    } else return 2;
    results+=L"]";
    RegressionIO::WriteText(argv[3],results);
    return status;
}

#include "../src/DOM.h"
#include "../src/JavaScript.h"
#include "TraceJson.h"
#include <ole2.h>
#include <chrono>
#include <iostream>

using namespace TWebFrame::Internal;
using namespace ParityTrace;
#include "StandaloneScriptRegressions.h"
int wmain(int argc,wchar_t** argv){
    if(argc==2&&std::wstring(argv[1])==L"--regressions")return RunStandaloneRegressions();
    if(argc<3){std::wcerr<<L"StandaloneScriptProbe source.js output.json [iterations] [jit-calls]\n";return 2;}
    const auto source=ReadText(argv[1]);if(source.empty())return 2;
    const unsigned repeats=argc>3?std::clamp(_wtoi(argv[3]),1,20):5;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    Document document;document.Parse(L"<!doctype html><html><head></head><body></body></html>");
    JavaScriptRuntime runtime(document);runtime.SetLocation(L"http://127.0.0.1/offline-script");
    runtime.SetCompatibilityBridgeEnabled(false);
    if(argc>4)runtime.SetJitCompilationThreshold(std::max(0,_wtoi(argv[4])));
    unsigned blocked=0;
    runtime.SetResourceLoader([&](const std::wstring&,std::wstring&){++blocked;return false;});
    runtime.SetRequestLoader([&](const ScriptRequest&){++blocked;ScriptResponse r;r.error=L"Offline benchmark: network disabled";return r;});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
    runtime.SetExecutionYieldHandler([&]{return std::chrono::steady_clock::now()<deadline;});
    std::wstring setupError;
    if(!runtime.Execute(L"window.__standaloneSource="+Quote(source)+L";",nullptr,&setupError)){std::wcerr<<setupError;return 1;}
    const auto expression=L"return eval(window.__standaloneSource);";
    std::wstring records=L"[";
    for(unsigned i=0;i<repeats;++i){
        const auto start=std::chrono::steady_clock::now();std::wstring result,error;
        const bool ok=runtime.Execute(expression,&result,&error);
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(i)records+=L',';
        records+=L"{\"ok\":"+std::wstring(ok?L"true":L"false")+L",\"ms\":"+std::to_wstring(ms)+L",\"result\":"+Quote(result)+L",\"error\":"+Quote(error)+L"}";
        std::wcout<<L"Run "<<i+1<<L" ms="<<ms<<L" ok="<<ok<<L" result="<<result.substr(0,180)<<L'\n';
        if(!ok)break;
    }
    const auto stats=runtime.GetJitStatistics();
    WriteText(argv[2],L"{\"networkBlocked\":"+std::to_wstring(blocked)+L",\"runs\":"+records+L"],\"compiledFunctions\":"+std::to_wstring(stats.compiledFunctions)+L",\"nativeCalls\":"+std::to_wstring(stats.nativeCalls)+L",\"unsupportedFunctions\":"+std::to_wstring(stats.unsupportedFunctions)+L",\"runtime\":"+runtime.DiagnosticsJson()+L"}");
    CoUninitialize();return 0;
}

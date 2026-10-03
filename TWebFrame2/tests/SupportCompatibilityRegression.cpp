#include "../src/DOM.h"
#include "../src/JavaScript.h"
#include "RegressionIO.h"
#include <ole2.h>
#include <chrono>
#include <iostream>
using namespace TWebFrame::Internal;
using namespace RegressionIO;
#include "SupportDpiRendering.h"
#include "SupportRuntimeChecks.h"
int wmain(){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);unsigned failures=0;
#ifdef SUPPORT_PCRE2
    const auto languageExpected=L"TWebFrame2/tests/support-language-expected.json";
    const auto semanticExpected=L"TWebFrame2/tests/semantic-expected.json";
    WriteText(L"TWebFrame2/tests/artifacts/support-regex-backend.txt",L"pcre2");
#else
    const auto languageExpected=L"TWebFrame2/tests/support-language-standard-expected.json";
    const auto semanticExpected=L"TWebFrame2/tests/semantic-standard-expected.json";
    WriteText(L"TWebFrame2/tests/artifacts/support-regex-backend.txt",L"standard");
#endif
    failures+=CheckSupportRuntimeLifetime();
    for(double scale:{1.0,1.5}){const bool ok=CheckSupportDpiRendering(scale);
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"CSS raster, DOM geometry and hit testing DPI="<<scale<<L'\n';if(!ok)++failures;}
    for(double scale:{1.0,1.5})for(size_t jit:{size_t(0),size_t(16)}){
        Document document;document.Parse(L"<!doctype html><html><body></body></html>");JavaScriptRuntime runtime(document);
        runtime.SetLocation(L"https://support.invalid/tests");runtime.SetViewportSize(800,600);runtime.SetDevicePixelRatio(scale);runtime.SetJitCompilationThreshold(jit);
        unsigned beacons=0;bool bodyCorrect=false;
        runtime.SetRequestLoader([&](const ScriptRequest& request){++beacons;bodyCorrect=request.method==L"POST"&&request.body==std::vector<unsigned char>{'p','a','y','l','o','a','d'}&&request.mode==ScriptRequest::Mode::NoCors;ScriptResponse response;response.status=204;return response;});
        std::wstring result,error;
#ifdef SUPPORT_PCRE2
        runtime.Execute(L"window.__supportPcre2=true;",nullptr,&error);
#else
        runtime.Execute(L"window.__supportPcre2=false;",nullptr,&error);
#endif
        const auto language=ReadText(L"TWebFrame2/tests/support-language-regression.js");
        bool ok=runtime.Execute(L"window.__languageResult=JSON.parse(eval("+Quote(language)+L"));",nullptr,&error)&&
            runtime.Execute(L"return JSON.stringify(window.__languageResult);",&result,&error);
        WriteText((L"TWebFrame2/tests/artifacts/support-language-"+std::to_wstring(scale)+L"-"+std::to_wstring(jit)+L".json").c_str(),result);
        std::wcout<<L"DPI="<<scale<<L" JIT="<<jit<<L" language="<<result<<L" error="<<error<<L'\n';
        if(!ok||result!=ReadText(languageExpected))++failures;
        ok=runtime.Execute(ReadText(L"TWebFrame2/tests/semantic-regression.js"),nullptr,&error);
        runtime.RunTimers();ok=runtime.Execute(L"return JSON.stringify(window.__semanticResults.cases);",&result,&error)&&ok;
        WriteText((L"TWebFrame2/tests/artifacts/semantic-"+std::to_wstring(scale)+L"-"+std::to_wstring(jit)+L".json").c_str(),result);
        if(!ok||result!=ReadText(semanticExpected))++failures;
        ok=runtime.Execute(ReadText(L"TWebFrame2/tests/support-api-regression.js"),nullptr,&error);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);bool done=false;
        do{runtime.RunTimers();std::wstring state;runtime.Execute(L"return window.__supportDone;",&state,&error);done=state==L"true";if(!done)Sleep(1);}while(!done&&std::chrono::steady_clock::now()<deadline);
        runtime.RunTimers();runtime.Execute(L"return JSON.stringify(window.__supportResults);",&result,&error);
        WriteText((L"TWebFrame2/tests/artifacts/support-api-"+std::to_wstring(scale)+L"-"+std::to_wstring(jit)+L".json").c_str(),result);
        std::wcout<<L"DPI="<<scale<<L" JIT="<<jit<<L" API="<<result<<L" error="<<error<<L'\n';
#ifdef SUPPORT_WEB_ASSEMBLY
        const unsigned expectedApiCases=35;
#else
        const unsigned expectedApiCases=30;
#endif
        std::wstring all;runtime.Execute(L"return Object.keys(window.__supportResults).length>="+std::to_wstring(expectedApiCases)+L"&&Object.keys(window.__supportResults).every(function(k){return window.__supportResults[k]===true;});",&all,&error);
        if(!ok||!done||all!=L"true"||beacons!=1||!bodyCorrect)++failures;
    }
    CoUninitialize();std::wcout<<L"Failures="<<failures<<L'\n';return failures?1:0;
}

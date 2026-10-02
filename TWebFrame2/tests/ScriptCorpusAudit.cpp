#include "../src/DOM.h"
#include "../src/JavaScript.h"
#include "TraceJson.h"
#include <ole2.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>

using namespace TWebFrame::Internal;
using namespace ParityTrace;

int wmain(int argc,wchar_t** argv){
    if(argc<3){std::wcerr<<L"ScriptCorpusAudit input.tsv output.jsonl [job-ms]\n";return 2;}
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const int budget=argc>3?std::clamp(_wtoi(argv[3]),50,10000):1000;
    const bool compileOnly=argc>4&&std::wstring(argv[4])==L"compile-only";
    SetEnvironmentVariableW(L"TWEBFRAME_TRACE_MISSING",L"1");
    SetEnvironmentVariableW(L"TWEBFRAME_TRACE_ERRORS",nullptr);
    SetEnvironmentVariableW(L"TWEBFRAME_TRACE_FUNCTIONS",nullptr);
    SetEnvironmentVariableW(L"TWEBFRAME_TRACE_INSTRUCTION_LIMIT",argc>4&&std::wstring(argv[4])==L"optimized"?nullptr:L"5000000");
    SetEnvironmentVariableW(L"TWEBFRAME_SCRIPT_ARCHIVE",nullptr);
    std::wistringstream input(ReadText(argv[1]));std::wstring line;unsigned count=0;
    std::ofstream output(std::filesystem::path(argv[2]),std::ios::binary);
    if(!output)return 2;
    while(std::getline(input,line)){
        if(line.empty())continue;if(line.back()==L'\r')line.pop_back();
        std::wistringstream fields(line);std::wstring id,mode,file,url;
        std::getline(fields,id,L'\t');std::getline(fields,mode,L'\t');std::getline(fields,file,L'\t');std::getline(fields,url);
        if(file.empty())return 2;
        const auto start=std::chrono::steady_clock::now();
        const auto source=ReadText(file);
        Document document;document.Parse(L"<!doctype html><html><head></head><body><pre id=results></pre></body></html>");
        std::atomic<unsigned> blocked{0};
        JavaScriptRuntime runtime(document);runtime.SetLocation(url.rfind(L"http://",0)==0||url.rfind(L"https://",0)==0?url:L"https://audit.invalid/offline");
        runtime.SetCompatibilityBridgeEnabled(false);
        runtime.SetResourceLoader([&](const std::wstring&,std::wstring&){++blocked;return false;});
        runtime.SetRequestLoader([&](const ScriptRequest&){++blocked;ScriptResponse response;response.error=L"Corpus audit: offline network";return response;});
        std::wstring syntaxError,error,result;
        const auto compileStart=std::chrono::steady_clock::now();
        const bool syntax=runtime.ValidateSyntax(source,&syntaxError);
        const auto compileMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compileStart).count();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(budget);
        runtime.SetExecutionYieldHandler([&]{return std::chrono::steady_clock::now()<deadline;});
        bool executed=false,ok=false,workerFinished=false;
        if(syntax&&!compileOnly&&mode!=L"wasm-placeholder"){
            executed=true;
            if(mode==L"worker"){
                const auto workerSource=source+L"\n;postMessage({__corpusAuditFinished:true});";
                const auto setup=L"window.__corpusWorkerDone=false;window.__corpusWorkerError='';window.__corpusWorker=new Worker(URL.createObjectURL(new Blob(["+Quote(workerSource)+L"],{type:'text/javascript'})));window.__corpusWorker.onmessage=function(e){if(e.data&&e.data.__corpusAuditFinished)window.__corpusWorkerDone=true;};window.__corpusWorker.onerror=function(e){window.__corpusWorkerError=String(e.message);};";
                ok=runtime.Execute(setup,nullptr,&error);
                while(ok&&std::chrono::steady_clock::now()<deadline){
                    runtime.RunTimers();std::wstring state;
                    if(!runtime.Execute(L"return JSON.stringify({done:window.__corpusWorkerDone,error:window.__corpusWorkerError});",&state,&error)){ok=false;break;}
                    if(RawField(state,L"done")==L"true"){workerFinished=true;break;}
                    const auto workerError=Field(state,L"error");if(!workerError.empty()){error=workerError;ok=false;break;}
                    Sleep(1);
                }
                std::wstring ignored;runtime.Execute(L"window.__corpusWorker.terminate();",nullptr,&ignored);
            }else{
                ok=runtime.Execute(source,&result,&error);
                if(ok&&std::chrono::steady_clock::now()<deadline){
                    runtime.DispatchDocumentEvent(L"DOMContentLoaded");runtime.DispatchWindowEvent(L"load");runtime.RunAnimationFrame();runtime.RunTimers();
                }
            }
        }
        if(argc>4&&std::wstring(argv[4])==L"read-compatibility"&&ok)
            ok=runtime.Execute(L"return JSON.stringify(window.__compatResults);",&result,&error);
        const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        const std::wstring record=L"{\"id\":"+Quote(id)+L",\"mode\":"+Quote(mode)+L",\"file\":"+Quote(file)+
            L",\"syntaxOk\":"+(syntax?L"true":L"false")+L",\"syntaxError\":"+Quote(syntaxError)+L",\"compileMs\":"+std::to_wstring(compileMs)+
            L",\"executed\":"+(executed?L"true":L"false")+L",\"executionOk\":"+(ok?L"true":L"false")+L",\"workerFinished\":"+(workerFinished?L"true":L"false")+
            L",\"error\":"+Quote(error)+L",\"lastError\":"+Quote(runtime.LastError())+L",\"result\":"+Quote(result.substr(0,65536))+L",\"jobBudgetMs\":"+std::to_wstring(budget)+L",\"ms\":"+std::to_wstring(elapsed)+
            L",\"networkBlocked\":"+std::to_wstring(blocked.load())+L",\"diagnostics\":"+runtime.DiagnosticsJson()+L"}";
        output<<Utf8(record)<<'\n';output.flush();
        ++count;if(count%25==0)std::wcout<<std::unitbuf<<L"Checked "<<count<<L" sources\n";
    }
    std::wcout<<L"Total="<<count<<L'\n';CoUninitialize();return 0;
}

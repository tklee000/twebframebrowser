#include "TraceJson.h"
#include <ole2.h>
#include <WebView2.h>
#include <wrl.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <vector>

using namespace ParityTrace;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

static bool WriteBase64Bytes(const std::filesystem::path& file,const std::wstring& encoded,bool wasm=false){
    DWORD size=0;if(encoded.empty()||!CryptStringToBinaryW(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,nullptr,&size,nullptr,nullptr))return false;
    std::vector<BYTE> bytes(size);
    if(!CryptStringToBinaryW(encoded.c_str(),static_cast<DWORD>(encoded.size()),CRYPT_STRING_BASE64,bytes.data(),&size,nullptr,nullptr))return false;
    if(wasm&&(size<8||bytes[0]!=0||bytes[1]!='a'||bytes[2]!='s'||bytes[3]!='m'))return false;
    std::ofstream output(file,std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),size);return output.good();
}

static bool Pump(const std::function<bool()>& done,int seconds=60){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now()<deadline){
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(done())return true;Sleep(5);
    }
    return false;
}
static const wchar_t* Snapshot=LR"JS((function(){
var checks={};var names=['Worker','WebAssembly','OffscreenCanvas','AudioContext','FontFace','TextEncoder','ReadableStream','MessageChannel','requestAnimationFrame','Intl','Promise','Proxy'];
for(var i=0;i<names.length;i++)checks[names[i]]=typeof window[names[i]];
var token=document.querySelector('input[name="cf-turnstile-response"]');
return {url:location.href,origin:location.origin,ready:document.readyState,visible:document.visibilityState,
userAgent:navigator.userAgent,cookieEnabled:navigator.cookieEnabled,secure:window.isSecureContext,
time:Date.now(),alignedTime:performance.timeOrigin+performance.now(),capabilities:checks,
cryptoSubtle:!!(window.crypto&&crypto.subtle),frames:window.length,scriptCount:document.scripts.length,
bodyText:document.body?document.body.innerText.slice(0,2500):'',tokenPresent:!!(token&&token.value),
phases:window.__stageMessages||[],compatibility:window.__compatResults||null,
worker:typeof document==='undefined'};
})())JS";
static const wchar_t* MessageObserver=LR"JS((function(){
window.__stageMessages=[];
addEventListener('message',function(e){var d=e.data;
if(!d||d.source!=='cloudflare-challenge')return;
var record={phase:d.event,code:d.code||d.errorCode||null,
origin:e.origin,trusted:e.isTrusted,mode:d.mode||null,tokenPresent:!!d.token,at:performance.now()};
window.__stageMessages.push(record);try{__twebParityTrace(JSON.stringify(record));}catch(ignore){}
});
})())JS";

struct Recorder:std::enable_shared_from_this<Recorder>{
    ComPtr<ICoreWebView2> web;
    ComPtr<ICoreWebView2_11> web11;
    std::filesystem::path output;
    std::ofstream events;
    std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    std::vector<ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>> receivers;
    std::set<std::wstring> sessions;
    std::map<std::pair<std::wstring,long>,std::wstring> contexts;
    std::map<std::pair<std::wstring,std::wstring>,std::wstring> resourceTypes;
    unsigned pending=0,scripts=0,wasmModules=0,responses=0,errors=0;
    bool closing=false;
    Recorder(ICoreWebView2* value,std::filesystem::path folder):web(value),output(std::move(folder)){
        web.As(&web11);std::filesystem::create_directories(output/L"scripts");
        std::filesystem::create_directories(output/L"responses");events.open(output/L"events.jsonl",std::ios::binary);
    }
    long long Elapsed()const{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();}
    void Log(const std::wstring& kind,const std::wstring& session,const std::wstring& data){
        const auto line=Utf8(L"{\"ms\":"+std::to_wstring(Elapsed())+L",\"kind\":"+Quote(kind)+L",\"session\":"+Quote(session)+L",\"data\":"+(data.empty()?L"null":data)+L"}\n");
        events.write(line.data(),line.size());events.flush();
    }
    void Call(const std::wstring& session,const std::wstring& method,const std::wstring& params,
              std::function<void(HRESULT,std::wstring)> complete={}){
        if(closing)return;++pending;const auto self=shared_from_this();
        auto handler=Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [self,session,method,complete](HRESULT status,LPCWSTR json)->HRESULT{
                --self->pending;std::wstring result=json?json:L"{}";
                if(FAILED(status)){++self->errors;self->Log(L"cdp-error",session,L"{\"method\":"+Quote(method)+L",\"status\":"+std::to_wstring(status)+L",\"result\":"+result+L"}");}
                if(complete&&!self->closing)complete(status,std::move(result));return S_OK;
            });
        const auto status=session.empty()?web->CallDevToolsProtocolMethod(method.c_str(),params.c_str(),handler.Get()):
            web11?web11->CallDevToolsProtocolMethodForSession(session.c_str(),method.c_str(),params.c_str(),handler.Get()):E_NOINTERFACE;
        if(FAILED(status)){--pending;++errors;Log(L"cdp-call-error",session,L"{\"method\":"+Quote(method)+L",\"status\":"+std::to_wstring(status)+L"}");}
    }
    void Configure(const std::wstring& session,bool worker=false){
        if(!sessions.insert(session).second)return;
        Call(session,L"Runtime.enable",L"{}");
        Call(session,L"Network.enable",L"{\"maxTotalBufferSize\":67108864,\"maxResourceBufferSize\":16777216}");
        Call(session,L"Debugger.enable",L"{\"maxScriptsCacheSize\":67108864}");
        Call(session,L"Runtime.addBinding",L"{\"name\":\"__twebParityTrace\"}");
        if(worker)Call(session,L"Debugger.setInstrumentationBreakpoint",L"{\"instrumentation\":\"beforeScriptExecution\"}");
        const auto self=shared_from_this();
        if(web11)Call(session,L"Target.setAutoAttach",L"{\"autoAttach\":true,\"waitForDebuggerOnStart\":true,\"flatten\":true}",
            [self,session](HRESULT,std::wstring){if(!session.empty())self->Call(session,L"Runtime.runIfWaitingForDebugger",L"{}");});
    }
    void Subscribe(const std::wstring& name){
        ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
        if(FAILED(web->GetDevToolsProtocolEventReceiver(name.c_str(),&receiver)))return;
        const auto self=shared_from_this();EventRegistrationToken token{};
        receiver->add_DevToolsProtocolEventReceived(Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [self,name](ICoreWebView2*,ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args)->HRESULT{
                if(self->closing)return S_OK;
                LPWSTR raw=nullptr,sessionRaw=nullptr;args->get_ParameterObjectAsJson(&raw);
                ComPtr<ICoreWebView2DevToolsProtocolEventReceivedEventArgs2> args2;
                if(SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))))args2->get_SessionId(&sessionRaw);
                const std::wstring data=raw?raw:L"{}",session=sessionRaw?sessionRaw:L"";
                CoTaskMemFree(raw);CoTaskMemFree(sessionRaw);self->Event(name,session,data);return S_OK;
            }).Get(),&token);receivers.push_back(std::move(receiver));
    }
    void Event(const std::wstring& name,const std::wstring& session,const std::wstring& data){
        if(name==L"Target.attachedToTarget"){
            Log(name,session,data);Configure(Field(data,L"sessionId"),Field(RawField(data,L"targetInfo"),L"type")==L"worker");
        }else if(name==L"Target.detachedFromTarget"){
            const auto detached=Field(data,L"sessionId");sessions.erase(detached);
            for(auto it=contexts.begin();it!=contexts.end();)if(it->first.first==detached)it=contexts.erase(it);else ++it;
            Log(name,session,data);
        }else if(name==L"Debugger.paused"){
            Log(name,session,data);Call(session,L"Debugger.resume",L"{}");
        }else if(name==L"Runtime.executionContextCreated"){
            const auto context=RawField(data,L"context");const auto id=Integer(context,L"id");
            const auto aux=RawField(context,L"auxData");
            if(RawField(aux,L"isDefault")==L"true")contexts[{session,id}]=Field(context,L"origin");
            Log(name,session,data);
        }else if(name==L"Runtime.executionContextDestroyed"){
            contexts.erase({session,Integer(data,L"executionContextId")});Log(name,session,data);
        }else if(name==L"Runtime.executionContextsCleared"){
            for(auto it=contexts.begin();it!=contexts.end();)if(it->first.first==session)it=contexts.erase(it);else ++it;
            Log(name,session,data);
        }else if(name==L"Debugger.scriptParsed"){
            Log(name,session,data);const auto id=Field(data,L"scriptId");
            const bool wasm=Field(data,L"scriptLanguage")==L"WebAssembly";if(wasm)++wasmModules;
            const auto number=++scripts;const auto file=L"scripts/"+std::to_wstring(number)+L"-"+SafeName(session)+L"-"+SafeName(id)+(wasm?L".wasm":L".js");
            WriteText(output/(file+L".meta.json"),data);
            const auto self=shared_from_this();
            Call(session,L"Debugger.getScriptSource",L"{\"scriptId\":"+Quote(id)+L"}",[self,session,file,wasm](HRESULT status,std::wstring result){
                if(SUCCEEDED(status)){
                    if(wasm){
                        if(WriteBase64Bytes(self->output/file,Field(result,L"bytecode"),true))self->Log(L"wasm-cached",session,L"{\"file\":"+Quote(file)+L"}");
                        else{++self->errors;self->Log(L"wasm-source-error",session,L"{\"file\":"+Quote(file)+L",\"error\":\"Missing or invalid WebAssembly bytecode\"}");}
                        return;
                    }
                    const auto source=Field(result,L"scriptSource");WriteText(self->output/file,source);
                    self->Log(L"script-cached",session,L"{\"file\":"+Quote(file)+L",\"characters\":"+std::to_wstring(source.size())+L"}");
                }
            });
        }else if(name==L"Network.requestWillBeSent"){
            const auto request=RawField(data,L"request"),headers=RawField(request,L"headers");
            Log(name,session,L"{\"requestId\":"+Quote(Field(data,L"requestId"))+L",\"url\":"+Quote(Field(request,L"url"))+
                L",\"method\":"+Quote(Field(request,L"method"))+L",\"type\":"+Quote(Field(data,L"type"))+
                L",\"userAgent\":"+Quote(Field(headers,L"User-Agent"))+L",\"origin\":"+Quote(Field(headers,L"Origin"))+
                L",\"hasPostData\":"+(RawField(request,L"hasPostData")==L"true"?L"true":L"false")+L"}");
        }else if(name==L"Network.responseReceived"){
            const auto response=RawField(data,L"response"),id=Field(data,L"requestId"),type=Field(data,L"type");
            resourceTypes[{session,id}]=type;
            Log(name,session,L"{\"requestId\":"+Quote(id)+L",\"url\":"+Quote(Field(response,L"url"))+L",\"type\":"+Quote(type)+
                L",\"status\":"+std::to_wstring(Integer(response,L"status"))+L",\"mimeType\":"+Quote(Field(response,L"mimeType"))+
                L",\"protocol\":"+Quote(Field(response,L"protocol"))+L",\"fromDiskCache\":"+(RawField(response,L"fromDiskCache")==L"true"?L"true":L"false")+L"}");
        }else if(name==L"Network.loadingFinished"){
            Log(name,session,data);const auto id=Field(data,L"requestId");const auto type=resourceTypes[{session,id}];
            if(type!=L"Script"&&type!=L"Document"&&type!=L"XHR"&&type!=L"Fetch"&&type!=L"Stylesheet")return;
            const auto file=L"responses/"+std::to_wstring(++responses)+L"-"+SafeName(session)+L"-"+SafeName(id)+L".body";
            const auto self=shared_from_this();
            Call(session,L"Network.getResponseBody",L"{\"requestId\":"+Quote(id)+L"}",[self,session,file,id](HRESULT status,std::wstring result){
                if(SUCCEEDED(status)){
                    const auto body=Field(result,L"body");
                    if(RawField(result,L"base64Encoded")==L"true"){
                        DWORD size=0;CryptStringToBinaryW(body.c_str(),static_cast<DWORD>(body.size()),CRYPT_STRING_BASE64,nullptr,&size,nullptr,nullptr);
                        std::vector<BYTE> bytes(size);
                        if(CryptStringToBinaryW(body.c_str(),static_cast<DWORD>(body.size()),CRYPT_STRING_BASE64,bytes.data(),&size,nullptr,nullptr)){
                            std::ofstream out(self->output/file,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),size);
                        }
                    }else WriteText(self->output/file,body);
                    self->Log(L"response-cached",session,L"{\"file\":"+Quote(file)+L",\"requestId\":"+Quote(id)+L"}");
                }
            });
        }else Log(name,session,data);
    }
    void Snapshots(){
        const auto active=contexts;const auto self=shared_from_this();
        for(const auto& entry:active){
            const auto session=entry.first.first;const auto id=entry.first.second;
            Call(session,L"Runtime.evaluate",L"{\"expression\":"+Quote(Snapshot)+L",\"contextId\":"+std::to_wstring(id)+L",\"returnByValue\":true}",
                [self,session,id](HRESULT status,std::wstring result){if(SUCCEEDED(status))self->Log(L"snapshot",session,L"{\"contextId\":"+std::to_wstring(id)+L",\"result\":"+result+L"}");});
        }
    }
};

int wmain(int argc,wchar_t** argv){
    if(argc==2&&std::wstring(argv[1])==L"--self-test"){
        const std::wstring sample=LR"({"other":{"key":"wrong"},"key":"a\n\uD83D\uDE00","empty":"","flag":true,"id":42})";
        const auto value=Field(sample,L"key");
        const bool ok=value.size()==4&&value[1]==L'\n'&&value[2]==0xd83d&&value[3]==0xde00&&Integer(sample,L"id")==42&&RawField(sample,L"flag")==L"true"&&Field(sample,L"other").empty();
        std::wcout<<(ok?L"Trace JSON decoding passed\n":L"Trace JSON decoding failed\n");return ok?0:1;
    }
    if(argc<4){std::wcerr<<L"WebView2TraceProbe URL seconds output-folder [--baseline]\n";return 2;}
    const int seconds=std::max(5,std::min(300,_wtoi(argv[2])));
    const auto output=std::filesystem::absolute(argv[3]);std::filesystem::create_directories(output);
    const bool baseline=argc>4&&std::wstring(argv[4])==L"--baseline";
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    RECT bounds{0,0,1000,760};
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"WebView2 stage comparison",WS_POPUP,
        30,30,bounds.right,bounds.bottom,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ComPtr<ICoreWebView2Controller> controller;ComPtr<ICoreWebView2> web;std::wstring version;
    bool created=false;
    CreateCoreWebView2EnvironmentWithOptions(nullptr,(output/L"profile").c_str(),nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [&](HRESULT status,ICoreWebView2Environment* environment)->HRESULT{
            if(FAILED(status)){created=true;return S_OK;}
            LPWSTR text=nullptr;environment->get_BrowserVersionString(&text);version=text?text:L"";CoTaskMemFree(text);
            return environment->CreateCoreWebView2Controller(host,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [&](HRESULT status,ICoreWebView2Controller* value)->HRESULT{
                    if(SUCCEEDED(status)&&value){controller=value;controller->get_CoreWebView2(&web);controller->put_Bounds(bounds);controller->put_IsVisible(TRUE);}
                    created=true;return S_OK;
                }).Get());
        }).Get());
    if(!Pump([&]{return created;})||!web){std::wcerr<<L"WebView2 initialization failed\n";return 3;}
    ShowWindow(host,SW_SHOWNOACTIVATE);
    auto recorder=std::make_shared<Recorder>(web.Get(),output);
    if(!baseline){
        for(const auto* event:{L"Target.attachedToTarget",L"Target.detachedFromTarget",L"Runtime.executionContextCreated",L"Runtime.executionContextDestroyed",
            L"Runtime.executionContextsCleared",L"Runtime.bindingCalled",L"Runtime.exceptionThrown",L"Debugger.scriptParsed",L"Debugger.scriptFailedToParse",L"Debugger.paused",
            L"Network.requestWillBeSent",L"Network.responseReceived",L"Network.loadingFinished",L"Network.loadingFailed",L"Page.frameNavigated"})recorder->Subscribe(event);
        recorder->Configure(L"");Pump([&]{return recorder->pending==0;},10);
        bool observerAdded=false;
        web->AddScriptToExecuteOnDocumentCreated(MessageObserver,Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [&](HRESULT,LPCWSTR)->HRESULT{observerAdded=true;return S_OK;}).Get());Pump([&]{return observerAdded;},10);
    }
    recorder->Log(L"run",L"",L"{\"version\":"+Quote(version)+L",\"baseline\":"+(baseline?L"true":L"false")+L",\"url\":"+Quote(argv[1])+L"}");
    std::wcout<<std::unitbuf<<L"WebView2 "<<version<<L" baseline="<<baseline<<L'\n';
    web->Navigate(argv[1]);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    auto next=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    Pump([&]{
        if(!baseline&&std::chrono::steady_clock::now()>=next){recorder->Snapshots();next+=std::chrono::seconds(5);}
        return std::chrono::steady_clock::now()>=deadline;
    },seconds+5);
    bool resultDone=false;
    web->ExecuteScript(Snapshot,Callback<ICoreWebView2ExecuteScriptCompletedHandler>([&](HRESULT status,LPCWSTR json)->HRESULT{
        if(SUCCEEDED(status)&&json)WriteText(output/L"result.json",json);resultDone=true;return S_OK;
    }).Get());Pump([&]{return resultDone;},10);
    if(!baseline)recorder->Snapshots();
    ComPtr<IStream> preview;SHCreateStreamOnFileEx((output/L"render.png").c_str(),STGM_CREATE|STGM_WRITE|STGM_SHARE_EXCLUSIVE,FILE_ATTRIBUTE_NORMAL,TRUE,nullptr,&preview);
    bool captured=false;
    web->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,preview.Get(),Callback<ICoreWebView2CapturePreviewCompletedHandler>([&](HRESULT)->HRESULT{captured=true;return S_OK;}).Get());
    Pump([&]{return captured&&recorder->pending==0;},10);
    WriteText(output/L"summary.json",L"{\"version\":"+Quote(version)+L",\"baseline\":"+(baseline?L"true":L"false")+L",\"scripts\":"+std::to_wstring(recorder->scripts)+L",\"javascriptScripts\":"+std::to_wstring(recorder->scripts-recorder->wasmModules)+L",\"wasmModules\":"+std::to_wstring(recorder->wasmModules)+L",\"responses\":"+std::to_wstring(recorder->responses)+L",\"cdpErrors\":"+std::to_wstring(recorder->errors)+L",\"pending\":"+std::to_wstring(recorder->pending)+L"}");
    std::wcout<<L"Scripts="<<recorder->scripts<<L" responses="<<recorder->responses<<L" cdpErrors="<<recorder->errors<<L" pending="<<recorder->pending<<L'\n';
    recorder->closing=true;recorder->receivers.clear();controller->Close();recorder->web.Reset();recorder->web11.Reset();
    web.Reset();controller.Reset();recorder.reset();preview.Reset();DestroyWindow(host);CoUninitialize();return 0;
}

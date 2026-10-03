#include <winsock2.h>
#include <ws2tcpip.h>
#include <TWebFrame/TWebFrame.h>
#include <ole2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include "../../Browser/ResourceScheduler.h"

#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"ole32.lib")

namespace {
int failures=0;
void Check(bool condition,const wchar_t* label){
    std::wcout<<(condition?L"PASS: ":L"FAIL: ")<<label<<L'\n';
    if(!condition)++failures;
}
void CheckInitialFrameContext(HWND host){
    RECT bounds{0,0,320,120};
    for(const bool parallel:{false,true})for(const bool secure:{false,true}){
        auto view=TWebFrame::View::Create(host,bounds);
        view->SetParallelResourceLoading(parallel);
        const std::wstring origin=secure?L"https://frame-context.test":L"http://frame-context.test";
        Check(view->NavigateToString(L"<body></body>",origin+L"/parent"),L"initial iframe security fixture loads");
        const std::wstring expected=secure?L"about:blank|true|function|object":L"about:blank|false|undefined|undefined";
        std::wstring result;
        Check(view->ExecuteScript(LR"JS(
            var f=document.createElement('iframe');document.body.appendChild(f);
            var w=f.contentWindow;
            return w.location.href+'|'+w.isSecureContext+'|'+typeof w.SubtleCrypto+'|'+typeof w.crypto.subtle;
        )JS",&result)&&result==expected,L"new blank iframe immediately inherits its parent's secure context");
        if(result!=expected)std::wcout<<L"  actual: "<<result<<L" expected: "<<expected<<L'\n';
    }
}
class Server {
    SOCKET listener=INVALID_SOCKET;
    std::thread worker;
    std::atomic<bool> stopping{false};
public:
    unsigned short port=0;
    std::atomic<unsigned> requests{0};
    Server(){
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(listener==INVALID_SOCKET||bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(listener,8))return;
        int size=sizeof(address);getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size);port=ntohs(address.sin_port);
        worker=std::thread([this]{while(!stopping){
            const auto client=accept(listener,nullptr,nullptr);if(client==INVALID_SOCKET)break;
            DWORD timeout=2000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
            std::string request;char buffer[4096];
            while(request.find("\r\n\r\n")==std::string::npos&&request.size()<16384){
                const int length=recv(client,buffer,sizeof(buffer),0);if(length<=0)break;request.append(buffer,length);
            }
            const auto first=request.find(' '),last=request.find(' ',first+1);
            const auto path=first==std::string::npos?std::string():request.substr(first+1,last-first-1);
            ++requests;
            std::string status="200 OK",body="body",headers="Content-Type: text/plain; charset=utf-8\r\nX-Result: retained\r\n";
            if(path=="/partial"){
                status="206 Partial Content";
                auto lower=request;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});
                body=lower.find("x-test: sent\r\n")!=std::string::npos?"sent":"missing";
            }else if(path=="/missing"){status="404 Not Found";body="missing-body";}
            else if(path=="/document-403"||path=="/document-404"||path=="/document-503"){
                const auto code=path.substr(10);
                status=code+(code=="403"?" Forbidden":code=="404"?" Not Found":" Service Unavailable");
                headers="Content-Type: text/html; charset=utf-8\r\n";
                if(code=="403")headers+="cf-mitigated: challenge\r\nSet-Cookie: document403=retained; Path=/\r\n";
                body="<style>#http-document{width:120px;height:36px}</style>"
                    "<link rel='stylesheet' href='/denied.css'><div id='http-document'>HTTP "+code+"</div>"
                    "<script src='/denied-script'></script><script>window.documentStatus="+code+";</script>";
            }else if(path=="/document-redirect"){status="302 Found";headers+="Location: /document-403\r\n";body="";}
            else if(path=="/denied-script"){status="403 Forbidden";headers="Content-Type: application/javascript\r\n";body="window.deniedScript=true;";}
            else if(path=="/denied.css"){status="403 Forbidden";headers="Content-Type: text/css\r\n";body="#http-document{width:999px}";}
            else if(path=="/unauthorized"){status="401 Unauthorized";body="unauthorized-body";}
            else if(path=="/cors")headers+="Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Result\r\n";
            else if(path=="/redirect"){status="302 Found";headers+="Location: /missing\r\n";body="";}
            else if(path=="/set-cookie"){headers+="Set-Cookie: server=1; Path=/; HttpOnly\r\nSet-Cookie: visible=2; Path=/\r\n";body="set";}
            else if(path=="/redirect-cookie"){status="302 Found";headers+="Location: /echo-cookie\r\nSet-Cookie: redirect=3; Path=/\r\n";body="";}
            else if(path=="/redirect-fragment"){status="302 Found";headers+="Location: /echo-cookie#replacement\r\n";body="";}
            else if(path=="/fragment-fixture"){headers="Content-Type: text/html; charset=utf-8\r\n";body="<script>parent.postMessage({fragment:location.hash},'*');</script>";}
            else if(path=="/folder/relative"){status="302 Found";headers+="Location: ../echo-cookie\r\n";body="";}
            else if(path=="/cookie-omit"){headers+="Set-Cookie: omitted=8; Path=/\r\n";}
            else if(path=="/cors-failure"){headers+="Set-Cookie: cors-side-effect=7; Path=/\r\n";}
            else if(path=="/frame-redirect"){status="302 Found";headers+="Location: http://localhost:"+std::to_string(port)+"/frame-fixture\r\n";body="";}
            else if(path=="/cors-credentials"){
                const auto start=request.find("\r\nOrigin: ");const auto end=start==std::string::npos?start:request.find("\r\n",start+2);
                const auto origin=start==std::string::npos?"null":request.substr(start+10,end-start-10);
                headers+="Access-Control-Allow-Origin: "+origin+"\r\nAccess-Control-Allow-Credentials: true\r\nAccess-Control-Expose-Headers: *\r\n";
            }
            else if(path=="/frame-fixture"){
                headers="Content-Type: text/html; charset=utf-8\r\n";
                body="<body><link rel='stylesheet' href='/frame.css'><script src='/frame-script'></script><img src='/frame.gif'></body>";
            }else if(path=="/frame-script"){
                headers="Content-Type: application/javascript\r\n";
                body="parent.postMessage({frameReady:document.cookie,enabled:navigator.cookieEnabled},'*');";
            }else if(path=="/frame.css"){headers="Content-Type: text/css\r\n";body="body {color:red}";}
            else if(path=="/frame.gif"){
                headers="Content-Type: image/gif\r\n";
                const unsigned char image[]={71,73,70,56,57,97,1,0,1,0,128,0,0,0,0,0,255,255,255,33,249,4,1,0,0,0,0,44,0,0,0,0,1,0,1,0,0,2,2,68,1,0,59};
                body.assign(reinterpret_cast<const char*>(image),sizeof(image));
            }
            else if(path.rfind("/cache",0)==0){
                headers+="Cache-Control: ";
                headers+=path=="/cache-no-store"?"max-age=60, no-store\r\n":path=="/cache-bad"?"max-age=-1\r\n":
                    path=="/cache-shared"?"s-maxage=60\r\n":"max-age=60\r\n";
                if(path=="/cache-vary")headers+="Vary: Cookie\r\n";
                if(path=="/cache-stale")headers+="Age: 61\r\n";
                if(path=="/cache-old-date")headers+="Date: Sun, 06 Nov 1994 08:49:37 GMT\r\n";
                const auto start=request.find("\r\nCookie: ");const auto end=start==std::string::npos?start:request.find("\r\n",start+2);
                body=start==std::string::npos?"":request.substr(start+10,end-start-10);
            }
            else if(path=="/echo-cookie"){
                const auto start=request.find("\r\nCookie: ");const auto end=start==std::string::npos?start:request.find("\r\n",start+2);
                body=start==std::string::npos?"":request.substr(start+10,end-start-10);
            }
            const auto response="HTTP/1.1 "+status+"\r\n"+headers+"Content-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body;
            size_t sent=0;while(sent<response.size()){
                const int count=send(client,response.data()+sent,static_cast<int>(response.size()-sent),0);if(count<=0)break;sent+=count;
            }
            shutdown(client,SD_BOTH);closesocket(client);
        }});
    }
    ~Server(){stopping=true;if(listener!=INVALID_SOCKET){shutdown(listener,SD_BOTH);closesocket(listener);}if(worker.joinable())worker.join();}
};
bool Execute(TWebFrame::View& view,const std::wstring& script,const std::wstring& expected,const wchar_t* label){
    std::wstring result,error;const bool ok=view.ExecuteScript(script,&result,&error);
    Check(ok&&result==expected,label);if(!ok||result!=expected)std::wcerr<<L"Expected "<<expected<<L", got "<<result<<L", error "<<error<<L'\n';
    return ok&&result==expected;
}
void Async(TWebFrame::View& view,const std::wstring& script,const std::wstring& expected,const wchar_t* label){
    std::wstring message,error;view.SetMessageHandler([&](const std::wstring& value){message=value;});
    const bool accepted=view.ExecuteScript(script,nullptr,&error);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(message.empty()&&std::chrono::steady_clock::now()<deadline){
        MSG event{};if(PeekMessageW(&event,nullptr,0,0,PM_REMOVE)){TranslateMessage(&event);DispatchMessageW(&event);}else Sleep(1);
    }
    Check(accepted&&message==expected,label);if(!accepted||message!=expected)std::wcerr<<L"Expected "<<expected<<L", got "<<message<<L", error "<<error<<L'\n';
    view.SetMessageHandler({});
}
bool WaitFor(const std::function<bool()>& ready){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!ready()&&std::chrono::steady_clock::now()<deadline){
        MSG event{};if(PeekMessageW(&event,nullptr,0,0,PM_REMOVE)){TranslateMessage(&event);DispatchMessageW(&event);}else Sleep(1);
    }
    return ready();
}
void CheckDocumentResponses(TWebFrame::View& view,const std::wstring& origin){
    // Exercise the common BrowserContext transport without the host adapter.
    view.SetResourceLoader({});view.SetNetworkResourceLoader({});view.SetPageScriptsEnabled(true);
    Async(view,LR"JS(fetch('/document-403').then(r=>chrome.webview.postMessage(r.status+'|'+r.ok+'|'+r.headers.get('cf-mitigated')));)JS",
        L"403|false|challenge",L"fetch retains the original 403 status independently of document rendering");
    auto client=std::make_shared<HttpClient>();
    for(const int status:{403,404,503}){
        const auto response=client->Get(origin+L"/document-"+std::to_wstring(status),origin,true);
        Check(response.status==static_cast<unsigned>(status)&&response.error.empty()&&HttpClient::DecodeText(response).find(L"HTTP "+std::to_wstring(status))!=std::wstring::npos,
            L"browser HTTP adapter retains complete 403/404/503 HTML without a transport error");
    }
    const auto failed=client->Get(L"not-a-network-url");
    Check(failed.status==0&&!failed.error.empty(),L"browser HTTP adapter still reports a failed request");
    for(const bool adapter:{false,true}){
        if(adapter)view.SetNetworkResourceLoader([client](const TWebFrame::NetworkRequest& request){
            const auto source=client->Request(request);TWebFrame::NetworkResponse response;
            response.status=source.status;response.url=source.url;response.headers=source.headers;response.contentType=source.contentType;
            response.body=HttpClient::DecodeText(source);response.bytes=source.body;response.error=source.error;return response;
        });
        std::wcout<<L"Document transport: "<<(adapter?L"browser HTTP adapter":L"common BrowserContext")<<L'\n';
        for(const bool parallel:{false,true})for(const int status:{403,404,503}){
            view.SetParallelResourceLoading(parallel);
            std::wstring event;view.SetMessageHandler([&](const std::wstring& value){event=value;});
            const auto code=std::to_wstring(status);
            Check(view.NavigateToString(L"<iframe id='http-frame' src='/document-"+code+
                L"' onload=\"chrome.webview.postMessage('document-load')\" onerror=\"chrome.webview.postMessage('document-error')\"></iframe>",origin+L"/parent"),
                L"HTTP error-document frame fixture starts");
            Check(WaitFor([&]{return !event.empty();})&&event==L"document-load",L"synchronous and asynchronous 403/404/503 iframe documents dispatch load");
            view.SetMessageHandler({});
            Execute(view,LR"JS(var f=document.getElementById('http-frame');return f.contentWindow.documentStatus+'|'+f.contentDocument.getElementById('http-document').textContent+'|'+f.contentDocument.getElementById('http-document').getBoundingClientRect().width+'|'+typeof f.contentWindow.deniedScript;)JS",
                code+L"|HTTP "+code+L"|120|undefined",L"error HTML renders and executes inline script while 403 script and CSS remain rejected");
        }
        for(const int status:{403,404,503}){
            Check(view.NavigateToString(L"<main id='previous-document'>Previous</main>",origin+L"/before"),L"standalone navigation fixture loads");
            bool completed=false,loaded=false;view.SetLoadHandler([&](bool ok,const std::wstring&){completed=true;loaded=ok;});
            const auto code=std::to_wstring(status);
            const auto path=status==403?L"/document-redirect#entry":L"/document-"+code;
            const bool accepted=view.ExecuteScript(L"location.href='"+path+L"';");
            Check(accepted&&WaitFor([&]{return completed;})&&loaded,L"standalone navigation commits 403/404/503 response documents");
            view.SetLoadHandler({});
            Execute(view,L"return location.pathname+location.hash+'|'+documentStatus+'|'+document.getElementById('http-document').getBoundingClientRect().width+'|'+typeof deniedScript;",
                L"/document-"+code+(status==403?L"#entry":L"")+L"|"+code+L"|120|undefined",L"document navigation retains the final redirect URL and ordinary resource error rules");
        }
        }
    for(const unsigned status:{0u,200u,403u}){
        view.SetNetworkResourceLoader([status](const TWebFrame::NetworkRequest& request){
            TWebFrame::NetworkResponse response;response.status=status;response.url=request.url;
            response.body=L"<script>window.partialDocumentExecuted=true;</script>";
            if(status)response.error=L"Incomplete response body";return response;
        });
        view.NavigateToString(L"<main id='previous-document'>Previous</main>",origin+L"/before");
        bool completed=false,loaded=true;view.SetLoadHandler([&](bool ok,const std::wstring&){completed=true;loaded=ok;});
        const bool accepted=view.ExecuteScript(L"location.href='/broken-document';");
        Check(accepted&&WaitFor([&]{return completed;})&&!loaded,L"transport failures and incomplete 200/403 responses remain load failures");
        view.SetLoadHandler({});
        Execute(view,L"return !!document.getElementById('previous-document')+'|'+typeof partialDocumentExecuted;",L"true|undefined",
            L"failed transport preserves the previous document and never executes a partial body");
    }
    view.SetNetworkResourceLoader({});view.SetParallelResourceLoading(false);
}
}
int wmain(){
    WSADATA sockets{};if(WSAStartup(MAKEWORD(2,2),&sockets))return 1;
    const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    {
        Server server;Check(server.port!=0,L"loopback HTTP server starts");if(!server.port)return 1;
        const auto suffix=L":"+std::to_wstring(server.port);
        const auto origin=L"http://127.0.0.1"+suffix;
        const auto cross=L"http://localhost"+suffix;
        const auto host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"HTTP regression",WS_POPUP,0,0,320,120,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        CheckInitialFrameContext(host);
        {
            RECT bounds{0,0,320,120};auto view=TWebFrame::View::Create(host,bounds);
            Check(static_cast<bool>(view),L"real View is created");if(!view)return 1;
            std::atomic<unsigned> callbackRequests{0};
            view->SetResourceLoader([&](const std::wstring&,std::wstring& text){++callbackRequests;text=L"incorrect text-only response";return true;});
            view->SetParallelResourceLoading(true);Check(view->NavigateToString(L"<main>HTTP fixture</main>",origin+L"/index"),L"HTTP document fixture loads");
            Async(*view,LR"JS(
                var workerUrl=URL.createObjectURL(new Blob(["var p=trustedTypes.createPolicy('identity',{createScript:function(s){return s;}});onmessage=function(e){if(e.isTrusted&&e.origin===''&&e.source===null)eval(p.createScript(e.data));};postMessage('ready');"],{type:'text/javascript'}));
                var worker=new Worker(workerUrl),workerReply='';
                worker.onmessage=function(e){
                    if(e.data==='ready')worker.postMessage("postMessage({same:eval('this')===self,number:eval('41')});setTimeout(function(){postMessage('timer');},20);");
                    else if(e.data==='timer'){worker.terminate();URL.revokeObjectURL(workerUrl);chrome.webview.postMessage(workerReply+'|timer');}
                    else workerReply=String(e.data.same&&e.isTrusted&&e.origin===''&&e.source===null)+'|'+e.data.number;
                };
                worker.onerror=function(e){chrome.webview.postMessage('error:'+e.message);};
            )JS",L"true|41|timer",L"View delivers Worker wakeups and Worker timers through its Windows event loop");
            Execute(*view,LR"JS(var x=new XMLHttpRequest(),events=[];x.open('GET','/partial',false);x.setRequestHeader('X-Test','sent');x.onload=()=>events.push('load');x.onerror=()=>events.push('error');x.send();return x.status+'|'+x.statusText+'|'+x.getResponseHeader('x-result')+'|'+x.responseText+'|'+events.join(',');)JS",
                L"206|Partial Content|retained|sent|load",L"GET XHR retains status, headers, body and request headers");
            Execute(*view,LR"JS(var denied=new XMLHttpRequest(),event='';denied.open('GET','/unauthorized',false);denied.onload=()=>event='load';denied.onerror=()=>event='error';denied.send();return denied.status+'|'+denied.responseText+'|'+event;)JS",
                L"401|unauthorized-body|load",L"GET HTTP 401 is a response rather than a transport error");
            Async(*view,LR"JS(fetch('/missing').then(r=>r.text().then(t=>window.chrome.webview.postMessage(r.status+'|'+r.ok+'|'+r.headers.get('x-result')+'|'+t))).catch(e=>window.chrome.webview.postMessage(e.name));)JS",
                L"404|false|retained|missing-body",L"asynchronous fetch resolves HTTP 404 with original metadata");
            Async(*view,L"fetch('"+cross+LR"JS(/cors').then(r=>window.chrome.webview.postMessage(r.status+'|'+r.headers.get('x-result'))).catch(e=>window.chrome.webview.postMessage(e.name));)JS",
                L"200|retained",L"cross-origin GET exposes only allowed response headers");
            Async(*view,L"fetch('"+cross+LR"JS(/blocked').then(r=>window.chrome.webview.postMessage('unexpected')).catch(e=>window.chrome.webview.postMessage(e.name));)JS",
                L"TypeError",L"cross-origin GET without CORS approval rejects");
            Async(*view,L"fetch('"+cross+LR"JS(/blocked',{mode:'no-cors'}).then(r=>r.text().then(t=>window.chrome.webview.postMessage(r.type+'|'+r.status+'|'+r.ok+'|'+r.headers.get('x-result')+'|'+t))).catch(e=>window.chrome.webview.postMessage(e.name));)JS",
                L"opaque|0|false|null|",L"no-cors GET resolves a filtered opaque response");
            Async(*view,LR"JS(fetch('/redirect').then(r=>window.chrome.webview.postMessage(r.status+'|'+r.url.endsWith('/missing'))).catch(e=>window.chrome.webview.postMessage(e.name));)JS",
                L"404|true",L"GET redirects preserve final status and response URL");
            Check(callbackRequests==0,L"HTTP script requests never pass through the text-only callback");
            auto context=std::make_shared<TWebFrame::BrowserContext>();view->SetBrowserContext(context);
            unsigned observed=0;
            context->SetNetworkObserver([&](const TWebFrame::NetworkRequest&,const TWebFrame::NetworkResponse& response){
                ++observed;Check(response.status==200,L"diagnostic observer receives the completed HTTP response");
                context->SetNetworkObserver({}); // Reentrant setter must not hold the profile mutex.
                throw std::runtime_error("diagnostic failure");
            });
            TWebFrame::NetworkRequest navigation;navigation.url=origin+L"/set-cookie";navigation.mode=TWebFrame::NetworkRequest::Mode::Navigation;
            navigation.credentials=TWebFrame::NetworkRequest::Credentials::Include;navigation.topLevelNavigation=true;
            Check(context->Request(navigation).status==200,L"navigation records multiple Set-Cookie fields in the shared jar");
            Check(observed==1,L"diagnostic failures and reentrant removal preserve the network response");
            Execute(*view,LR"JS(document.cookie='script=4; Path=/';var x=new XMLHttpRequest();x.open('GET','/echo-cookie',false);x.send();return document.cookie+'|'+x.responseText+'|'+x.getResponseHeader('set-cookie');)JS",
                L"visible=2; script=4|server=1; visible=2; script=4|null",L"navigation, document.cookie and XHR share cookies while HttpOnly and Set-Cookie remain hidden");
            Async(*view,LR"JS(fetch('/echo-cookie',{credentials:'omit'}).then(r=>r.text()).then(t=>window.chrome.webview.postMessage('cookies:'+t));)JS",
                L"cookies:",L"fetch credentials=omit suppresses same-origin cookies");
            Async(*view,LR"JS(fetch('/redirect-cookie').then(r=>r.text()).then(t=>window.chrome.webview.postMessage(t));)JS",
                L"server=1; visible=2; script=4; redirect=3",L"redirect responses update cookies before the next request");
            Async(*view,LR"JS(fetch('/folder/relative').then(r=>r.text()).then(t=>window.chrome.webview.postMessage(t));)JS",
                L"server=1; visible=2; script=4; redirect=3",L"relative redirects resolve dot segments against the request URL");
            Async(*view,LR"JS(fetch('/cookie-omit',{credentials:'omit'}).then(()=>window.chrome.webview.postMessage(String(document.cookie.includes('omitted='))));)JS",
                L"false",L"credentials=omit ignores Set-Cookie response fields");
            Async(*view,L"fetch('"+cross+LR"JS(/cors-credentials',{credentials:'include'}).then(r=>window.chrome.webview.postMessage(r.status+'|'+r.headers.get('x-result'))).catch(e=>window.chrome.webview.postMessage(e.name));)JS",
                L"200|null",L"credentialed CORS does not treat exposed-header wildcard as authorization");
            {
                Server other;Check(other.port!=0,L"second same-site cross-origin HTTP server starts");
                const auto otherOrigin=L"http://127.0.0.1:"+std::to_wstring(other.port);
                Async(*view,L"fetch('"+otherOrigin+LR"JS(/cors-failure',{credentials:'include'}).then(()=>window.chrome.webview.postMessage('unexpected')).catch(e=>window.chrome.webview.postMessage(e.name+'|'+document.cookie.includes('cors-side-effect=7')));)JS",
                    L"TypeError|true",L"same-site Set-Cookie is processed before a credentialed CORS response is rejected");
            }
            auto network=std::make_shared<ResourceScheduler>(2);view->SetBrowserContext(network->Context());
            struct RequestLog {std::mutex mutex;std::vector<TWebFrame::NetworkRequest> requests;};
            auto metadata=std::make_shared<RequestLog>();
            view->SetNetworkResourceLoader([network,metadata](const TWebFrame::NetworkRequest& request){
                {std::lock_guard<std::mutex> lock(metadata->mutex);metadata->requests.push_back(request);}
                const auto response=network->Get(request);TWebFrame::NetworkResponse result;
                if(response){result.status=response->status;result.url=response->url;result.contentType=response->contentType;
                    result.headers=response->headers;result.body=HttpClient::DecodeText(*response);result.bytes=response->body;}
                return result;
            });
            network->Context()->SetDocumentCookie(cross+L"/",L"frameLax=private; Path=/");
            view->SetPageScriptsEnabled(true);
            const auto frameFixture=L"<body><script>addEventListener('message',e=>{if(e.data.frameReady!==undefined)chrome.webview.postMessage('frame:'+e.data.enabled+'|'+e.data.frameReady);});</script><iframe src='"+origin+L"/frame-redirect'></iframe></body>";
            Check(view->NavigateToString(frameFixture,origin+L"/resources"),L"redirected cross-site frame fixture loads through the browser scheduler");
            Async(*view,L"return 'waiting';",L"frame:true|",L"iframe scripts share the profile and enforce cross-site document-cookie policy");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            bool scriptPolicy=false,cssPolicy=false,imagePolicy=false,framePolicy=false;
            do {
                {std::lock_guard<std::mutex> lock(metadata->mutex);for(const auto& request:metadata->requests){
                    if(request.url==origin+L"/frame-redirect")framePolicy=request.origin==origin&&request.siteForCookies==origin&&!request.topLevelNavigation;
                    if(request.origin!=cross||request.siteForCookies!=L"null")continue;
                    if(request.url==cross+L"/frame-script")scriptPolicy=true;
                    if(request.url==cross+L"/frame.css")cssPolicy=true;
                    if(request.url==cross+L"/frame.gif")imagePolicy=true;
                }}
                if(scriptPolicy&&cssPolicy&&imagePolicy&&framePolicy)break;
                MSG message{};if(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}else Sleep(1);
            }while(std::chrono::steady_clock::now()<deadline);
            Check(scriptPolicy&&cssPolicy&&imagePolicy&&framePolicy,L"iframe, script, CSS and image requests carry their initiating origin and ancestor site policy");
            Execute(*view,L"return document.querySelector('iframe').contentDocument===null;",L"true",L"asynchronous iframe redirects adopt the final origin and protect the redirected document");
            view->SetParallelResourceLoading(false);
            Check(view->NavigateToString(frameFixture,origin+L"/sync-resources"),L"synchronous redirected frame fixture loads");
            Async(*view,L"return 'waiting';",L"frame:true|",L"synchronous redirected iframe retains correct origin and cookie policy");
            Execute(*view,L"return document.querySelector('iframe').contentDocument===null;",L"true",L"synchronous iframe redirects enforce the final origin's same-origin policy");
            TWebFrame::NetworkRequest fragmentRequest;fragmentRequest.url=origin+L"/echo-cookie#host-origin=parent";
            fragmentRequest.mode=TWebFrame::NetworkRequest::Mode::Navigation;
            const auto fragmentResponse=network->Context()->Request(fragmentRequest);
            Check(fragmentResponse.status==200&&fragmentResponse.url==fragmentRequest.url,
                L"navigation response URLs preserve the document fragment while HTTP requests omit it");
            fragmentRequest.mode=TWebFrame::NetworkRequest::Mode::Cors;fragmentRequest.origin=origin;
            Check(network->Context()->Request(fragmentRequest).url==origin+L"/echo-cookie",
                L"fetch response URLs exclude the document fragment");
            fragmentRequest.mode=TWebFrame::NetworkRequest::Mode::Navigation;fragmentRequest.url=origin+L"/redirect#host-origin=parent";
            Check(network->Context()->Request(fragmentRequest).url==origin+L"/missing#host-origin=parent",
                L"redirects without an explicit fragment inherit the navigation fragment");
            fragmentRequest.url=origin+L"/redirect-fragment#host-origin=parent";
            Check(network->Context()->Request(fragmentRequest).url==origin+L"/echo-cookie#replacement",
                L"an explicit redirect fragment replaces the previous navigation fragment");
            for(const bool parallel:{false,true}){
                view->SetParallelResourceLoading(parallel);
                Check(view->NavigateToString(L"<script>addEventListener('message',e=>{if(e.data.fragment!==undefined)chrome.webview.postMessage('fragment:'+e.data.fragment);});</script><iframe src='"+origin+L"/fragment-fixture#host-origin=parent'></iframe>",origin+L"/fragment-parent"),
                    L"fragment-initialized iframe fixture loads");
                Async(*view,L"return 'waiting';",L"fragment:#host-origin=parent",
                    L"synchronous and asynchronous iframe bootstrap scripts receive the navigation fragment");
            }
            view->SetNetworkResourceLoader({});view->SetParallelResourceLoading(false);
            const auto cacheUrl=origin+L"/cache";
            network->Context()->SetDocumentCookie(origin+L"/",L"cached=first; Path=/");
            const auto before=server.requests.load();const auto cached=network->Get(cacheUrl,origin);
            Check(cached&&HttpClient::DecodeText(*cached)==L"cached=first"&&network->Get(cacheUrl,origin)==cached&&server.requests.load()==before+1,L"explicitly fresh resources reuse the profile cache");
            network->Context()->SetDocumentCookie(origin+L"/",L"cached=second; Path=/");
            const auto refreshed=network->Get(cacheUrl,origin);
            Check(refreshed&&HttpClient::DecodeText(*refreshed)==L"cached=second"&&server.requests.load()==before+2,L"cookie revision prevents reuse of stale authenticated cache responses");
            for(const auto* path:{L"/cache-no-store",L"/cache-vary",L"/cache-stale",L"/cache-bad",L"/cache-shared",L"/cache-old-date"}){
                const auto count=server.requests.load();network->Get(origin+path,origin);network->Get(origin+path,origin);
                Check(server.requests.load()==count+2,L"no-store, Vary, stale Age/Date and invalid or shared-only freshness are fetched again");
            }
            TWebFrame::NetworkRequest navigate; navigate.url=cacheUrl;navigate.referrer=origin;
            navigate.mode=TWebFrame::NetworkRequest::Mode::Navigation;navigate.credentials=TWebFrame::NetworkRequest::Credentials::Include;navigate.topLevelNavigation=true;
            const auto navigationCount=server.requests.load();network->Get(navigate);network->Get(navigate);
            Check(server.requests.load()==navigationCount+2,L"top-level navigations are not served from the resource cache");
            CheckDocumentResponses(*view,origin);
        }
        DestroyWindow(host);
    }
    if(SUCCEEDED(com))CoUninitialize();WSACleanup();return failures?1:0;
}

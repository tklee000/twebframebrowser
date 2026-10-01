#include <winsock2.h>
#include <ws2tcpip.h>
#include <TWebFrame/TWebFrame.h>
#include <ole2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"ole32.lib")

namespace {
int failures=0;
void Check(bool condition,const wchar_t* label){
    std::wcout<<(condition?L"PASS: ":L"FAIL: ")<<label<<L'\n';
    if(!condition)++failures;
}
class Server {
    SOCKET listener=INVALID_SOCKET;
    std::thread worker;
    std::atomic<bool> stopping{false};
public:
    unsigned short port=0;
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
            std::string status="200 OK",body="body",headers="Content-Type: text/plain; charset=utf-8\r\nX-Result: retained\r\n";
            if(path=="/partial"){
                status="206 Partial Content";
                auto lower=request;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(tolower(c));});
                body=lower.find("x-test: sent\r\n")!=std::string::npos?"sent":"missing";
            }else if(path=="/missing"){status="404 Not Found";body="missing-body";}
            else if(path=="/unauthorized"){status="401 Unauthorized";body="unauthorized-body";}
            else if(path=="/cors")headers+="Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Result\r\n";
            else if(path=="/redirect"){status="302 Found";headers+="Location: /missing\r\n";body="";}
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
        {
            RECT bounds{0,0,320,120};auto view=TWebFrame::View::Create(host,bounds);
            Check(static_cast<bool>(view),L"real View is created");if(!view)return 1;
            std::atomic<unsigned> callbackRequests{0};
            view->SetResourceLoader([&](const std::wstring&,std::wstring& text){++callbackRequests;text=L"incorrect text-only response";return true;});
            view->SetParallelResourceLoading(true);Check(view->NavigateToString(L"<main>HTTP fixture</main>",origin+L"/index"),L"HTTP document fixture loads");
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
        }
        DestroyWindow(host);
    }
    if(SUCCEEDED(com))CoUninitialize();WSACleanup();return failures?1:0;
}

#include "CookieJar.h"
#include "DOM.h"
#include "JavaScript.h"
#include <filesystem>
#include <iostream>

using namespace TWebFrame;
using namespace TWebFrame::Internal;
namespace {
int failures=0;
void Check(bool passed,const wchar_t* label){
    std::wcout<<(passed?L"PASS: ":L"FAIL: ")<<label<<L'\n';if(!passed)++failures;
}
void Script(JavaScriptRuntime& runtime,const std::wstring& script,const std::wstring& expected,const wchar_t* label){
    std::wstring result,error;const bool passed=runtime.Execute(script,&result,&error)&&result==expected;
    Check(passed,label);if(!passed)std::wcerr<<L"  actual="<<result<<L" expected="<<expected<<L" error="<<error<<L'\n';
}
}
int wmain(){
    Check(BrowserContext::Origin(L"HTTPS://b\u00fccher.example:443/page")==L"https://xn--bcher-kva.example"&&
        BrowserContext::Origin(L"http://[::1]:80/page")==L"http://[::1]",L"origin canonicalization supports IDN, IPv6 and default ports");
    CookieJar jar;
    Check(jar.Set(L"https://www.example.co.uk/account/index",L"session=server; Path=/; HttpOnly; Secure; SameSite=Lax"),L"server session cookie is accepted");
    Check(jar.Header(L"https://www.example.co.uk/")==L"session=server"&&jar.Header(L"https://www.example.co.uk/",L"",false,L"GET",true).empty(),L"HttpOnly is sent on HTTP but hidden from document.cookie");
    Check(!jar.Set(L"https://www.example.co.uk/",L"session=script; Path=/",true),L"script cannot replace an HttpOnly cookie");
    Check(jar.Set(L"https://www.example.co.uk/account/index",L"path=default",true)&&
        jar.Header(L"https://www.example.co.uk/account/x").find(L"path=default")!=std::wstring::npos&&
        jar.Header(L"https://www.example.co.uk/accounting").find(L"path=default")==std::wstring::npos,L"default path and path boundary matching");
    Check(jar.Set(L"https://www.example.co.uk/",L"domain=shared; Domain=example.co.uk; Path=/",true)&&
        jar.Header(L"https://other.example.co.uk/")==L"domain=shared",L"domain cookies span valid subdomains");
    Check(!jar.Set(L"https://www.example.co.uk/",L"bad=1; Domain=co.uk",true)&&
        !jar.Set(L"https://one.github.io/",L"bad=1; Domain=github.io",true)&&
        !jar.Set(L"https://www.example.co.uk/",L"bad=1; Domain=attacker.test",true),L"ICANN and private public suffixes and unrelated domains are rejected");
    Check(!jar.Set(L"http://www.example.co.uk/",L"bad=1; Secure",true)&&
        !jar.Set(L"https://www.example.co.uk/",L"bad=1; SameSite=None",true),L"Secure cookies require HTTPS and SameSite=None requires Secure");
    Check(!jar.Set(L"https://www.example.co.uk/",L"__Host-id=1; Secure",true)&&
        !jar.Set(L"https://www.example.co.uk/",L"__Secure-id=1",true)&&
        jar.Set(L"https://www.example.co.uk/",L"__Host-id=1; Secure; Path=/",true),L"cookie prefixes enforce their scope requirements");
    Check(jar.Set(L"https://www.example.co.uk/",L"expired=1; Path=/; Max-Age=0; Expires=Wed, 09 Jun 2099 10:18:14 GMT",true)&&
        jar.Header(L"https://www.example.co.uk/").find(L"expired=")==std::wstring::npos,L"Max-Age takes precedence over Expires");
    CookieJar dates;
    Check(dates.Set(L"https://dates.test/",L"rfc850=1; Expires=Sunday, 06-Nov-94 08:49:37 GMT")&&
        dates.Set(L"https://dates.test/",L"asctime=1; Expires=Sun Nov  6 08:49:37 1994")&&
        dates.Set(L"https://dates.test/",L"future=1; Expires=Wed, 09 Jun 2099 10:18:14 GMT")&&
        dates.Header(L"https://dates.test/")==L"future=1",L"cookie dates accept legacy formats and expire past dates");
    CookieJar overlay;overlay.Set(L"https://login.example.test/",L"login=secure; Secure; Path=/login");
    Check(!overlay.Set(L"http://example.test/login",L"login=insecure; Path=/login")&&
        !overlay.Set(L"http://login.example.test/login",L"login=insecure; Domain=example.test; Path=/login")&&
        !overlay.Set(L"https://login.example.test/",L"__hOsT-id=1; Secure"),L"insecure parent-domain overlays and mixed-case insecure prefixes are rejected");
    CookieJar sites;sites.Set(L"https://api.example.co.uk/",L"lax=1; Path=/; SameSite=Lax");
    sites.Set(L"https://api.example.co.uk/",L"strict=1; Path=/; SameSite=Strict");
    sites.Set(L"https://api.example.co.uk/",L"none=1; Path=/; SameSite=None; Secure");
    Check(sites.Header(L"https://api.example.co.uk/",L"https://www.example.co.uk/")==L"lax=1; strict=1; none=1",L"SameSite compares registrable domains");
    Check(sites.Header(L"https://api.example.co.uk/",L"https://attacker.test/")==L"none=1"&&
        sites.Header(L"https://api.example.co.uk/",L"https://attacker.test/",true)==L"lax=1; none=1"&&
        sites.Header(L"https://api.example.co.uk/",L"https://attacker.test/",true,L"POST")==L"none=1",L"cross-site subresources and safe top-level navigation have distinct SameSite rules");
    Check(sites.Header(L"https://api.example.co.uk/",L"http://www.example.co.uk/")==L"none=1",L"SameSite is schemeful");
    Check(sites.Header(L"https://api.example.co.uk/",L"https://attacker.test/",false,L"GET",true)==L"none=1"&&
        !sites.Set(L"https://api.example.co.uk/",L"scriptCross=1",true,L"https://attacker.test/"),L"cross-site document.cookie follows SameSite for both reads and writes");

    auto context=std::make_shared<BrowserContext>();const auto tab=context->NewBrowsingContext(),otherTab=context->NewBrowsingContext();
    Document firstDoc,secondDoc,foreignDoc;firstDoc.Parse(L"<body><iframe id='child'></iframe></body>");secondDoc.Parse(L"<body></body>");foreignDoc.Parse(L"<body></body>");
    JavaScriptRuntime first(firstDoc),second(secondDoc),foreign(foreignDoc);
    first.SetBrowserContext(context,tab);second.SetBrowserContext(context,otherTab);foreign.SetBrowserContext(context,tab);
    first.SetLocation(L"https://storage.example.test/page");second.SetLocation(L"https://storage.example.test/other");foreign.SetLocation(L"https://foreign.example.test/");
    Script(first,LR"JS(localStorage.clear();sessionStorage.clear();localStorage.setItem('a',1);localStorage.b=2;sessionStorage.setItem('s','tab');return localStorage.length+'|'+localStorage.key(0)+'|'+localStorage.getItem('a')+'|'+localStorage.b+'|'+Object.keys(localStorage).join(',');)JS",L"2|a|1|2|a,b",L"complete Storage API and named properties");
    Script(second,LR"JS(return localStorage.getItem('a')+'|'+sessionStorage.getItem('s');)JS",L"1|null",L"localStorage shares an origin; sessionStorage isolates tabs");
    Script(foreign,LR"JS(return localStorage.length+'|'+sessionStorage.length;)JS",L"0|0",L"both storage partitions isolate origins");
    second.RunTimers();
    Script(second,LR"JS(var storageEvents=[];addEventListener('storage',e=>storageEvents.push(e.key+'|'+e.oldValue+'|'+e.newValue+'|'+(e.storageArea===localStorage)+'|'+e.url+'|'+(e instanceof StorageEvent)));return 'ready';)JS",L"ready",L"storage event listener registers");
    Script(first,LR"JS(localStorage.setItem('a','3');localStorage.setItem('a','3');return 'changed';)JS",L"changed",L"equal-value storage writes are no-ops");
    second.RunTimers();
    Script(second,L"return storageEvents.join(',');",L"a|1|3|true|https://storage.example.test/page|true",L"other documents receive one asynchronous branded StorageEvent with correct values");
    Script(second,LR"JS(var e=new StorageEvent('storage',{key:12,oldValue:null,newValue:false,storageArea:localStorage});return (e instanceof Event)+'|'+(e instanceof StorageEvent)+'|'+e.key+'|'+e.oldValue+'|'+e.newValue+'|'+(e.storageArea===localStorage)+'|'+e.isTrusted;)JS",L"true|true|12|null|false|true|false",L"StorageEvent constructor uses nullable DOMString conversions and event inheritance");
    first.Clear();first.SetLocation(L"https://storage.example.test/reloaded");
    Script(first,LR"JS(delete localStorage.b;return localStorage.getItem('a')+'|'+sessionStorage.getItem('s')+'|'+localStorage.length;)JS",L"3|tab|1",L"storage survives runtime recreation and supports named deletion");
    Script(first,LR"JS(var marker={},caught=false,brand=false;try{localStorage.setItem({toString(){throw marker;}},'x');}catch(e){caught=e===marker;}try{localStorage.getItem.call({},'a');}catch(e){brand=e.name==='TypeError';}return caught+'|'+brand;)JS",L"true|true",L"Storage conversions preserve exceptions and brand checks");
    Script(first,LR"JS(var quota=false;try{localStorage.setItem('large','x'.repeat(3*1024*1024));}catch(e){quota=e.name==='QuotaExceededError';}return quota+'|'+(localStorage.getItem('large')===null);)JS",L"true|true",L"quota failure preserves existing storage atomically");
    context->SetDocumentCookie(L"https://storage.example.test/",L"fromHost=1; Path=/");
    Script(first,LR"JS(document.cookie='fromPage=2; Path=/';return navigator.cookieEnabled+'|'+document.cookie;)JS",L"true|fromHost=1; fromPage=2",L"document.cookie uses the actual profile cookie jar");
    Check(context->DocumentCookie(L"https://foreign.example.test/").empty(),L"document cookies isolate unrelated hosts");

    for(const size_t jit:{size_t{0},size_t{2}}){
        first.SetJitCompilationThreshold(jit);
        Script(first,LR"JS(var buffer=new ArrayBuffer(8),bytes=new Uint8Array(buffer);bytes[1]=77;var view=new DataView(buffer,1,4),graph={buffer,bytes,view};graph.self=graph;var copy=structuredClone(graph);copy.bytes[1]=11;return (copy.self===copy)+'|'+(copy.bytes.buffer===copy.buffer)+'|'+(copy.view.buffer===copy.buffer)+'|'+copy.view.getUint8(0)+'|'+bytes[1]+'|'+(copy.buffer instanceof ArrayBuffer);)JS",L"true|true|true|11|77|true",L"Structured Clone preserves typed-view buffer aliasing, offsets, cycles and realm brands");
    }
    Script(first,LR"JS(var transferable=new ArrayBuffer(4),oldView=new Uint8Array(transferable);oldView[0]=42;var moved=structuredClone({buffer:transferable,bytes:oldView},{transfer:[transferable]});return transferable.byteLength+'|'+oldView.length+'|'+oldView[0]+'|'+moved.bytes[0]+'|'+(moved.buffer===moved.bytes.buffer);)JS",L"0|0|undefined|42|true",L"ArrayBuffer transfer detaches sender views and retains receiver bytes");
    Script(first,LR"JS(var stable=new ArrayBuffer(4),duplicate=false,cloneFailure=false;try{structuredClone(1,{transfer:[stable,stable]});}catch(e){duplicate=e.name==='DataCloneError';}try{structuredClone(function(){},{transfer:[stable]});}catch(e){cloneFailure=e.name==='DataCloneError';}return duplicate+'|'+cloneFailure+'|'+stable.byteLength;)JS",L"true|true|4",L"failed transfer validation and cloning never detach buffers");
    auto child=std::make_shared<JavaScriptRuntime>(secondDoc);child->SetBrowserContext(context,tab);
    const auto node=firstDoc.GetElementById(L"child"); // first.Clear() preserves its DOM.
    child->SetEmbeddingFrame(&first,node);
    child->Clear();child->SetLocation(L"https://child.example.test/frame");
    first.SetFrameRuntimeProvider([child](const auto&){return child;});
    Script(*child,LR"JS(var messages=[];addEventListener('message',e=>{messages.push(e.data.value+'|'+(e.data.self===e.data)+'|'+(e.data.date instanceof Date)+'|'+e.origin+'|'+(e.source===parent)+'|'+(e instanceof MessageEvent)+'|'+e.ports.length);});return 'ready';)JS",L"ready",L"cross-origin receiver registers");
    Script(first,LR"JS(var frame=document.getElementById('child'),message={value:7,date:new Date(0)};message.self=message;frame.contentWindow.postMessage(message,'https://child.example.test');message.value=99;frame.contentWindow.postMessage({value:8},'https://wrong.test');return 'sent';)JS",L"sent",L"postMessage snapshots data synchronously and checks target origin");
    child->RunTimers();
    Script(*child,L"return messages.join(',');",L"7|true|true|https://storage.example.test|true|true|0",L"cross-origin Structured Clone delivery preserves types, event brand and exact source without host message sinks");
    Script(first,LR"JS(var documentBlocked=false,nameBlocked=false,cloneError=false,syntaxError=false;try{frame.contentWindow.document;}catch(e){documentBlocked=e.name==='SecurityError';}try{frame.contentWindow.name;}catch(e){nameBlocked=e.name==='SecurityError';}try{frame.contentWindow.postMessage(function(){},'*');}catch(e){cloneError=e.name==='DataCloneError';}try{frame.contentWindow.postMessage(1,'not-an-origin');}catch(e){syntaxError=e.name==='SyntaxError';}return documentBlocked+'|'+nameBlocked+'|'+(frame.contentDocument===null)+'|'+cloneError+'|'+syntaxError;)JS",L"true|true|true|true|true",L"cross-origin DOM and name are protected; clone and origin failures are synchronous");
    Script(first,LR"JS(var setBlocked=false,deleteBlocked=false;try{frame.contentWindow.name='changed';}catch(e){setBlocked=e.name==='SecurityError';}try{delete frame.contentWindow.name;}catch(e){deleteBlocked=e.name==='SecurityError';}return setBlocked+'|'+deleteBlocked;)JS",L"true|true",L"cross-origin WindowProxy rejects writes and deletion");
    context->SetDocumentCookie(L"https://thirdparty.example.org/",L"lax=1; Path=/");
    context->SetDocumentCookie(L"https://thirdparty.example.org/",L"none=2; Path=/; SameSite=None; Secure");
    child->SetLocation(L"https://thirdparty.example.org/frame");
    Script(*child,LR"JS(document.cookie='blocked=3; Path=/';return document.cookie;)JS",L"none=2",L"third-party frame uses opaque ancestor site policy for script cookies");
    Check(context->DocumentCookie(L"https://thirdparty.example.org/")==L"lax=1; none=2",L"third-party script cannot create a same-site cookie");
    child->SetLocation(L"https://storage.example.test/frame");child->Clear();
    Script(first,L"window.shared=10;return 'ready';",L"ready",L"parent exposes a same-origin global");
    Script(*child,LR"JS(parent.shared+=2;return parent.shared+'|'+(parent.document.getElementById('child')!==null)+'|'+(parent===top);)JS",L"12|true|true",L"same-origin parent WindowProxy forwards globals, document methods and writes");
    Script(first,L"return shared;",L"12",L"same-origin parent mutation reaches the actual parent realm");
    Script(*child,L"var retainedParentDocument=parent.document;return 'retained';",L"retained",L"same-origin document can be retained");
    first.SetLocation(L"https://navigated.example.org/page");
    Script(*child,LR"JS(var protected=true;try{retainedParentDocument.querySelector('body');protected=false;}catch(e){protected=e.name==='SecurityError';}return protected;)JS",L"true",L"retained realm values cannot access a document after it navigates across origins");
    first.SetLocation(L"https://storage.example.test/reloaded");
    first.SetCompatibilityBridgeEnabled(false);first.Clear();
    Script(first,L"return typeof chrome+'|'+typeof twebframe;",L"undefined|undefined",L"standalone browser does not advertise the embedded compatibility bridge after navigation");
    first.SetCompatibilityBridgeEnabled(true);
    Script(first,L"return typeof chrome.webview.postMessage+'|'+(chrome.webview===twebframe);",L"function|true",L"embedded hosts can explicitly restore the compatibility bridge");
    Check(context->Storage(L"HTTPS://STORAGE.EXAMPLE.TEST:443/path")==context->Storage(L"https://storage.example.test"),L"storage keys canonicalize scheme, host, port and path");
    const auto released=context->NewBrowsingContext();context->Storage(L"https://release.test",released)->Set(L"k",L"v",L"",1);
    context->ReleaseBrowsingContext(released);
    Check(context->Storage(L"https://release.test",released)->Length()==0,L"closing a tab discards its session-storage namespace");
    first.SetLocation(L"https://b\u00fccher.example/page");
    context->SetDocumentCookie(L"https://xn--bcher-kva.example/",L"idn=shared; Path=/");
    Script(first,L"localStorage.setItem('idn','shared');return document.cookie;",L"idn=shared",L"JavaScript document cookies use the canonical IDN origin");
    std::wstring idnStorage;Check(context->Storage(L"https://xn--bcher-kva.example")->Get(L"idn",idnStorage)&&idnStorage==L"shared",L"JavaScript and host storage agree on IDN origins");

    const auto profile=std::filesystem::temp_directory_path()/(L"TWebFrame-storage-test-"+std::to_wstring(GetCurrentProcessId()));
    {BrowserContext persistent(profile.wstring());persistent.Storage(L"https://persist.example")->Set(L"persist",L"value",L"https://persist.example/",1);}
    {BrowserContext reopened(profile.wstring());std::wstring value;Check(reopened.Storage(L"https://persist.example")->Get(L"persist",value)&&value==L"value"&&reopened.Storage(L"https://other.example")->Length()==0,L"profile localStorage persists across process-context recreation without origin leakage");}
    // Remove only the explicitly generated temporary fixture file and directories.
    for(const auto& entry:std::filesystem::directory_iterator(profile/L"LocalStorage"))if(entry.path().extension()==L".storage")std::filesystem::remove(entry.path());
    std::filesystem::remove(profile/L"LocalStorage");std::filesystem::remove(profile);
    std::wcout<<(failures?L"Browser context tests failed":L"All browser context tests passed")<<L'\n';
    return failures?1:0;
}

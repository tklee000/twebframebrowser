#define NOMINMAX
#include "../../Browser/HttpClient.h"
#include <TWebFrame/TWebFrame.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace {
std::wstring Resolve(const std::wstring& base, const std::wstring& reference) {
    if (reference.rfind(L"https://", 0) == 0 || reference.rfind(L"http://", 0) == 0)
        return reference;
    const auto scheme = base.find(L"://");
    if (reference.rfind(L"//", 0) == 0)
        return (scheme == std::wstring::npos ? L"https:" : base.substr(0, scheme + 1)) + reference;
    if (scheme == std::wstring::npos) return reference;
    const auto originEnd = base.find_first_of(L"/?#", scheme + 3);
    const auto origin = originEnd == std::wstring::npos ? base : base.substr(0, originEnd);
    if (!reference.empty() && reference.front() == L'/') return origin + reference;
    auto clean = base;
    const auto suffix = clean.find_first_of(L"?#");
    if (suffix != std::wstring::npos) clean.resize(suffix);
    const auto slash = clean.find_last_of(L'/');
    return (slash == std::wstring::npos ? origin + L"/" : clean.substr(0, slash + 1)) + reference;
}

std::wstring Compact(std::wstring value) {
    for (auto& character : value) if (character < L' ') character = L' ';
    if (value.size() > 6000) value.resize(6000);
    return value;
}
}

int wmain(int argumentCount,wchar_t** arguments) {
    std::wcout << std::unitbuf;
    const bool directCoupang=argumentCount>1&&std::wstring(arguments[1])==L"coupang";
    const std::wstring url=directCoupang?
        L"https://ads-partners.coupang.com/widgets.html?id=687906&trackingCode=AF9292700&template=carousel&width=728&height=90&rUrl=&tag=js&resolution=728x90&depth=1&serverBaseUrl=https%3A%2F%2Fads-partners.coupang.com%2F&logServerBaseUrl=https%3A%2F%2Flogs-partners.coupang.com%2Flog%2F#gid=687906-45159912109375":
        L"https://cdn4.ppomppu.co.kr/banner/kakao_ad_728x90.html?v=3";
    HttpClient client;
    const auto page=client.Get(url,L"https://www.ppomppu.co.kr/");
    std::wcout<<L"PAGE status="<<page.status<<L" bytes="<<page.body.size()<<L'\n';
    if(!page.Ok())return 1;
    auto html=HttpClient::DecodeText(page);
    // Diagnostic isolation: the response currently carries a Cloudflare
    // challenge bootstrap after the Kakao ad markup.  Remove only that
    // injected block here so the first Kakao SDK failure can be observed
    // without the unrelated challenge script monopolizing the UI thread.
    const auto cloudflare=html.find(L"<script>window.__CF$cv$params=");
    if(argumentCount<2&&cloudflare!=std::wstring::npos){
        const auto end=html.find(L"</script>",cloudflare);
        if(end!=std::wstring::npos)html.erase(cloudflare,end+9-cloudflare);
    }
    if(argumentCount<3)
        html=L"<script>window.addEventListener('unhandledrejection',function(event){"
             L"var reason=event.reason;window.chrome.webview.postMessage('UNHANDLED:'"
             L"+(reason&&reason.name?reason.name:'Error')+':'"
             L"+(reason&&reason.message?reason.message:String(reason))+'|'"
             L"+(reason&&reason.stack?reason.stack:''));});</script>"+html;

    SetProcessDPIAware();
    HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"direct kakao probe",
                              WS_POPUP|WS_VISIBLE,50,50,728,90,nullptr,nullptr,
                              GetModuleHandleW(nullptr),nullptr);
    RECT bounds{0,0,728,90};
    auto view=TWebFrame::View::Create(host,bounds);
    std::wstring diagnostic;
    view->SetMessageHandler([&](const std::wstring& message){
        diagnostic=message;std::wcerr<<L"WEB_MESSAGE "<<Compact(message)<<L'\n';
    });
    view->SetParallelResourceLoading(true);
    view->SetPageScriptsEnabled(true);
    view->SetResourceLoader([&](const std::wstring& resource,std::wstring& text){
        const auto target=Resolve(url,resource);
        const auto response=client.Get(target,url);
        if(response.Ok())text=HttpClient::DecodeText(response);
        if(argumentCount<3&&
           target.find(L"/kas/static/third-party/cookie/ct2.html")!=std::wstring::npos){
            const std::wstring cookieMessage=L"window.parent.postMessage(message, \"*\");";
            const auto cookieMessagePosition=text.find(cookieMessage);
            if(cookieMessagePosition!=std::wstring::npos)
                text.replace(cookieMessagePosition,cookieMessage.size(),
                    L"window.chrome.webview.postMessage('COOKIE_CHILD_BEFORE:'"
                    L"+message+'|'+typeof parent.postMessage+'|'+(parent===window));"
                    L"window.parent.postMessage(message, '*');"
                    L"window.chrome.webview.postMessage('COOKIE_CHILD_AFTER');");
        }
        if(argumentCount<3&&
           target.find(L"/cdn-cgi/challenge-platform/scripts/precursor/main.js")!=std::wstring::npos){
            text=L"window.chrome.webview.postMessage('CF_BEGIN');"+text;
            const std::wstring genericLoopStart=L";!![];)try{";
            const auto genericLoopStartPosition=text.find(genericLoopStart);
            if(genericLoopStartPosition!=std::wstring::npos){
                const auto sourceStart=genericLoopStartPosition>500?
                    genericLoopStartPosition-500:0;
                std::wcerr<<L"CF_SOURCE "<<Compact(text.substr(
                    sourceStart,std::min<size_t>(1600,text.size()-sourceStart)))<<L'\n';
                std::wstring tableName=L"qB",decoderName=L"Bb";
                const auto forPosition=text.rfind(L"for(",genericLoopStartPosition);
                if(forPosition!=std::wstring::npos){
                    const auto tableAssign=text.find(L'=',forPosition+4);
                    if(tableAssign!=std::wstring::npos&&tableAssign<genericLoopStartPosition){
                        tableName=text.substr(forPosition+4,tableAssign-(forPosition+4));
                        tableName.erase(std::remove_if(tableName.begin(),tableName.end(),
                            [](wchar_t character){return std::iswspace(character);}),tableName.end());
                        const auto objectEnd=text.find(L'}',tableAssign);
                        const auto decoderAssign=objectEnd==std::wstring::npos?std::wstring::npos:
                            text.find(L'=',objectEnd+1);
                        if(objectEnd!=std::wstring::npos&&decoderAssign!=std::wstring::npos&&
                           decoderAssign<genericLoopStartPosition){
                            auto decoderStart=objectEnd+1;
                            while(decoderStart<decoderAssign&&
                                  (text[decoderStart]==L','||std::iswspace(text[decoderStart])))++decoderStart;
                            decoderName=text.substr(decoderStart,decoderAssign-decoderStart);
                        }
                    }
                }
                const auto property=[&](const wchar_t* name){
                    return L"String("+decoderName+L"("+tableName+L"."+name+L"))";
                };
                const auto diagnostic=L",window.__cfProbeIterations=0;!![];)try{"
                    L"if(window.__cfProbeIterations===0)window.chrome.webview.postMessage("
                    L"'CF_LOOP_INPUT:arg1='+String(arguments[1])+'|decoded='"+
                    property(L"Q")+L"+','+"+property(L"q")+L"+','+"+property(L"F")+L"+','+"+
                    property(L"Oo")+L"+','+"+property(L"OG")+L"+','+"+property(L"Om")+L"+','+"+
                    property(L"OH")+L"+','+"+property(L"OY")+L"+','+"+property(L"Of")+L"+','+"+
                    property(L"OT")+L"+','+"+property(L"Oy")+L");";
                text.replace(genericLoopStartPosition,genericLoopStart.size(),
                    diagnostic);
            }
            const std::wstring genericLoopEnd=L")break;else ";
            const auto genericLoopEndPosition=text.find(genericLoopEnd,genericLoopStartPosition);
            if(genericLoopEndPosition!=std::wstring::npos){
                std::wstring valueName=L"arguments[5]",expectedName=L"arguments[1]";
                const auto equality=text.rfind(L"===",genericLoopEndPosition);
                const auto identifier=[](wchar_t character){
                    return std::iswalnum(character)||character==L'_'||character==L'$';
                };
                if(equality!=std::wstring::npos){
                    size_t begin=equality;while(begin&&identifier(text[begin-1]))--begin;
                    size_t end=equality+3;while(end<text.size()&&std::iswspace(text[end]))++end;
                    size_t expectedEnd=end;while(expectedEnd<text.size()&&identifier(text[expectedEnd]))++expectedEnd;
                    if(begin<equality)valueName=text.substr(begin,equality-begin);
                    if(end<expectedEnd)expectedName=text.substr(end,expectedEnd-end);
                }
                const auto replacement=L"){window.chrome.webview.postMessage('CF_LOOP_DONE:'"
                    L"+window.__cfProbeIterations+'|value='+String("+valueName+L")+'|expected='+String("+expectedName+L"));break;}"
                    L"else if(++window.__cfProbeIterations>=10000){"
                    L"window.chrome.webview.postMessage('CF_LOOP_LIMIT:value='+String("+valueName+
                    L")+'|expected='+String("+expectedName+L"));break;}else ";
                text.replace(genericLoopEndPosition,genericLoopEnd.size(),replacement);
            }
            const std::wstring loopStart=L"Bb=B,F=Q();!![];)try{";
            const auto loopStartPosition=text.find(loopStart);
            if(loopStartPosition!=std::wstring::npos)
                text.replace(loopStartPosition,loopStart.size(),
                    L"Bb=B,F=Q(),window.__cfProbeIterations=0;!![];)try{");
            const std::wstring loopEnd=L"if(I===q)break;else F.push(F.shift())";
            const auto loopEndPosition=text.find(loopEnd);
            if(loopEndPosition!=std::wstring::npos)
                text.replace(loopEndPosition,loopEnd.size(),
                    L"if(I===q){window.chrome.webview.postMessage('CF_LOOP_DONE:'"
                    L"+window.__cfProbeIterations+'|'+I);break;}else{"
                    L"if(++window.__cfProbeIterations>=10000){window.chrome.webview.postMessage("
                    L"'CF_LOOP_LIMIT:'+I+'|expected='+q);break;}F.push(F.shift())}");
        }
        if(argumentCount<3&&
           target.find(L"t1.kakaocdn.net/kas/static/ba.min.js")!=std::wstring::npos){
            const std::wstring eventBase=L"return c((function(){this.listeners={}}),";
            const auto eventBasePosition=text.find(eventBase);
            if(eventBasePosition!=std::wstring::npos)
                text.replace(eventBasePosition,eventBase.size(),
                    L"return c((function(){window.chrome.webview.postMessage('KAKAO_BASE_BEFORE:'"
                    L"+Object.keys(this).join(','));this.listeners={};"
                    L"window.chrome.webview.postMessage('KAKAO_BASE_AFTER:'"
                    L"+Object.keys(this).join(','))}),");
            const std::wstring errorConstructor=
                L"(i=Ye(this,t,[e])).name=\"AdFitError\";";
            const auto errorConstructorPosition=text.find(errorConstructor);
            if(errorConstructorPosition!=std::wstring::npos)
                text.replace(errorConstructorPosition,errorConstructor.size(),
                    errorConstructor+
                    L"window.chrome.webview.postMessage('KAKAO_ERROR_CREATED:'"
                    L"+String(e)+'|'+JSON.stringify(n||{}));");
            const std::wstring loggerConstructor=
                L"e&&(a.enabledNamespacePatterns=a.getEnabledNamespacePatterns(e))";
            const auto loggerConstructorPosition=text.find(loggerConstructor);
            if(loggerConstructorPosition!=std::wstring::npos)
                text.replace(loggerConstructorPosition,loggerConstructor.size(),
                    L"(window.chrome.webview.postMessage('KAKAO_LOGGER_PROTO:'"
                    L"+typeof a.getEnabledNamespacePatterns+'|'"
                    L"+(Object.getPrototypeOf(a)===t.prototype)+'|'"
                    L"+typeof t.prototype.getEnabledNamespacePatterns+'|'"
                    L"+typeof Object.getPrototypeOf(a).getEnabledNamespacePatterns+'|'"
                    L"+Object.keys(a).join(',')),"
                    +loggerConstructor+L")");
            const std::wstring namespaceIterator=L"r=be(this.enabledNamespacePatterns,!0)";
            const auto namespaceIteratorPosition=text.find(namespaceIterator);
            if(namespaceIteratorPosition!=std::wstring::npos)
                text.replace(namespaceIteratorPosition,namespaceIterator.size(),
                    L"r=(window.chrome.webview.postMessage('KAKAO_NAMESPACE:'"
                    L"+typeof this.enabledNamespacePatterns+'|'"
                    L"+Array.isArray(this.enabledNamespacePatterns)+'|'"
                    L"+String(this.enabledNamespacePatterns&&this.enabledNamespacePatterns.length)+'|'"
                    L"+Object.keys(this).join(',')),be(this.enabledNamespacePatterns,!0))");
            const std::wstring loggerReturn=L"a.addListener(\"log\",a.defaultLogHandler),a}";
            const auto loggerReturnPosition=text.find(loggerReturn);
            if(loggerReturnPosition!=std::wstring::npos)
                text.replace(loggerReturnPosition,loggerReturn.size(),
                    L"a.addListener(\"log\",a.defaultLogHandler),"
                    L"window.chrome.webview.postMessage('KAKAO_LOGGER_RETURN:'"
                    L"+Object.keys(a).join(',')+'|'"
                    L"+Array.isArray(a.enabledNamespacePatterns)),a}");
            const std::wstring loggerFactory=L"value:function(e){return this.log.bind(this,e)}";
            const auto loggerFactoryPosition=text.find(loggerFactory);
            if(loggerFactoryPosition!=std::wstring::npos)
                text.replace(loggerFactoryPosition,loggerFactory.size(),
                    L"value:function(e){window.chrome.webview.postMessage('KAKAO_LOGGER_FACTORY:'"
                    L"+Object.keys(this).join(',')+'|'"
                    L"+Array.isArray(this.enabledNamespacePatterns));return this.log.bind(this,e)}");
            const std::wstring createLogger=L"function Ce(e){return Se.createLogger(";
            const auto createLoggerPosition=text.find(createLogger);
            if(createLoggerPosition!=std::wstring::npos)
                text.replace(createLoggerPosition,createLogger.size(),
                    L"function Ce(e){window.chrome.webview.postMessage('KAKAO_SE:'"
                    L"+Object.keys(Se).join(',')+'|'"
                    L"+Array.isArray(Se.enabledNamespacePatterns));return Se.createLogger(");
            const std::wstring loggerSingleton=
                L"var Se=new Ae(new Ie((function(){return window.localStorage})));";
            const auto loggerSingletonPosition=text.find(loggerSingleton);
            if(loggerSingletonPosition!=std::wstring::npos)
                text.replace(loggerSingletonPosition,loggerSingleton.size(),
                    loggerSingleton+
                    L"window.chrome.webview.postMessage('KAKAO_SE_CREATED:'"
                    L"+Object.keys(Se).join(',')+'|'"
                    L"+typeof Se.enabledNamespacePatterns+'|'"
                    L"+(Object.getPrototypeOf(Se)===Ae.prototype)+'|'"
                    L"+(Se.constructor===Ae));");
            const std::wstring readinessStart=L"var Vu=function(){return Promise.all([";
            const auto readinessStartPosition=text.find(readinessStart);
            if(readinessStartPosition!=std::wstring::npos)
                text.replace(readinessStartPosition,readinessStart.size(),
                    L"var Vu=function(){window.chrome.webview.postMessage('KAKAO_READY_START:'"
                    L"+document.readyState);return Promise.all([");
            const std::wstring readinessTimer=
                L"window.setTimeout((function(){return e()}))";
            const auto readinessTimerPosition=text.find(readinessTimer);
            if(readinessTimerPosition!=std::wstring::npos)
                text.replace(readinessTimerPosition,readinessTimer.size(),
                    L"window.setTimeout((function(){window.chrome.webview.postMessage("
                    L"'KAKAO_READY_TIMER');return e()}))");
            const std::wstring readinessEnd=L")),me()]).then(Du)}";
            const auto readinessEndPosition=text.find(readinessEnd);
            if(readinessEndPosition!=std::wstring::npos)
                text.replace(readinessEndPosition,readinessEnd.size(),
                    L")),me().then(function(value){window.chrome.webview.postMessage("
                    L"'KAKAO_ME_DONE');return value},function(error){"
                    L"window.chrome.webview.postMessage('KAKAO_ME_FAIL:'"
                    L"+(error&&error.message?error.message:String(error))+'|'"
                    L"+(error&&error.stack?error.stack:''));throw error})])"
                    L".then(function(value){window.chrome.webview.postMessage("
                    L"'KAKAO_ALL_DONE');return Du(value)})"
                    L".then(function(value){window.chrome.webview.postMessage("
                    L"'KAKAO_DU_DONE:'+value);return value})}");
            const std::wstring asyncStep=
                L"function B(e,t,n,r,i,o,a){try{var s=e[o](a),u=s.value}";
            const auto asyncStepPosition=text.find(asyncStep);
            if(asyncStepPosition!=std::wstring::npos)
                text.replace(asyncStepPosition,asyncStep.size(),
                    L"function B(e,t,n,r,i,o,a){var p1=Object.getPrototypeOf(e),"
                    L"p2=p1&&Object.getPrototypeOf(p1);"
                    L"window.chrome.webview.postMessage('KAKAO_ASYNC_STEP:'"
                    L"+o+'|'+typeof e[o]+'|'+Object.keys(e).join(',')+'|'"
                    L"+(p1?Object.keys(p1).join(','):'null')+'|'"
                    L"+(p2?Object.keys(p2).join(','):'null')+'|'"
                    L"+(p1?typeof p1[o]:'none')+'|'"
                    L"+(p2?typeof p2[o]:'none'));"
                    L"try{var s=e[o](a),u=s.value}");
            const std::wstring generatorCreate=
                L"function u(e,n,o,a){var s=n&&n.prototype instanceof l?n:l,"
                L"u=Object.create(s.prototype);return";
            const auto generatorCreatePosition=text.find(generatorCreate);
            if(generatorCreatePosition!=std::wstring::npos)
                text.replace(generatorCreatePosition,generatorCreate.size(),
                    L"function u(e,n,o,a){var s=n&&n.prototype instanceof l?n:l,"
                    L"u=Object.create(s.prototype);"
                    L"window.chrome.webview.postMessage('KAKAO_GENERATOR_CREATE:'"
                    L"+(s===n)+'|'+typeof s.prototype+'|'"
                    L"+(Object.getPrototypeOf(u)!==null)+'|'"
                    L"+Object.keys(s.prototype).join(','));return");
            const std::wstring generatorReturn=L"}(e,o,a),!0),u}var c={};";
            const auto generatorReturnPosition=text.find(generatorReturn);
            if(generatorReturnPosition!=std::wstring::npos)
                text.replace(generatorReturnPosition,generatorReturn.size(),
                    L"}(e,o,a),!0),window.chrome.webview.postMessage("
                    L"'KAKAO_GENERATOR_RETURN:'+Object.keys(u).join(',')+'|'"
                    L"+(Object.getPrototypeOf(u)!==null)+'|'+typeof u.next),u}var c={};");
            const std::wstring runtimeWrap=
                L"wrap:function(e,t,n,r){return c.w(v(e),t,n,r&&r.reverse())}";
            const auto runtimeWrapPosition=text.find(runtimeWrap);
            if(runtimeWrapPosition!=std::wstring::npos)
                text.replace(runtimeWrapPosition,runtimeWrap.size(),
                    L"wrap:function(e,t,n,r){var value=c.w(v(e),t,n,r&&r.reverse());"
                    L"window.chrome.webview.postMessage('KAKAO_WRAP_RETURN:'"
                    L"+Object.keys(value).join(',')+'|'"
                    L"+(Object.getPrototypeOf(value)!==null)+'|'"
                    L"+typeof value.next);return value}");
            const std::wstring cookieParent=
                L"this.handleMessage=function(n){var r,i=n.data;"
                L"e.isThirdPartyCookieSupportStatusMessage(i)&&(";
            const auto cookieParentPosition=text.find(cookieParent);
            if(cookieParentPosition!=std::wstring::npos)
                text.replace(cookieParentPosition,cookieParent.size(),
                    L"this.handleMessage=function(n){var r,i=n.data;"
                    L"window.chrome.webview.postMessage('COOKIE_PARENT:'"
                    L"+typeof i+'|'+String(i)+'|'"
                    L"+e.isThirdPartyCookieSupportStatusMessage(i));"
                    L"e.isThirdPartyCookieSupportStatusMessage(i)&&(");
            const std::wstring tpcResult=
                L"tr.getInstance().getThirdPartyCookieSupportStatus(100);"
                L"case 1:return e.abrupt(\"return\",e.sent);";
            const auto tpcResultPosition=text.find(tpcResult);
            if(tpcResultPosition!=std::wstring::npos)
                text.replace(tpcResultPosition,tpcResult.size(),
                    L"tr.getInstance().getThirdPartyCookieSupportStatus(100);"
                    L"case 1:window.chrome.webview.postMessage('KAKAO_TPC_DONE:'"
                    L"+e.sent);return e.abrupt(\"return\",e.sent);");
            const std::wstring requestTpc=L"case 3:return H=t.sent,B=e.getSessionId()";
            const auto requestTpcPosition=text.find(requestTpc);
            if(requestTpcPosition!=std::wstring::npos)
                text.replace(requestTpcPosition,requestTpc.size(),
                    L"case 3:window.chrome.webview.postMessage('KAKAO_REQUEST_TPC:'"
                    L"+t.sent);return H=t.sent,B=e.getSessionId()");
            const std::wstring fetchStart=
                L"case 0:return t.next=1,fetch(n,";
            const auto fetchStartPosition=text.find(fetchStart,98000);
            if(fetchStartPosition!=std::wstring::npos)
                text.replace(fetchStartPosition,fetchStart.size(),
                    L"case 0:window.chrome.webview.postMessage('KAKAO_FETCH_START:'"
                    L"+n);return t.next=1,fetch(n,");
            const std::wstring visibilityCheck=
                L"function Cs(e,t,n,r){void 0===t&&(t=0),void 0===n&&(n=0),void 0===r&&(r=1);"
                L"var i=e.getBoundingClientRect(),o=window.getComputedStyle(e,null);return";
            const auto visibilityCheckPosition=text.find(visibilityCheck);
            if(visibilityCheckPosition!=std::wstring::npos)
                text.replace(visibilityCheckPosition,visibilityCheck.size(),
                    L"function Cs(e,t,n,r){void 0===t&&(t=0),void 0===n&&(n=0),void 0===r&&(r=1);"
                    L"var i=e.getBoundingClientRect(),o=window.getComputedStyle(e,null);"
                    L"window.chrome.webview.postMessage('KAKAO_VISIBLE_CHECK:'"
                    L"+e.nodeName+'|'+[i.left,i.top,i.right,i.bottom,i.width,i.height].join(',')+'|'"
                    L"+o.getPropertyValue('display')+'|'+o.getPropertyValue('visibility')+'|'"
                    L"+(e.getAttribute('style')||'')+'|parent='"
                    L"+(e.parentElement?e.parentElement.nodeName+'@'+"
                    L"[e.parentElement.getBoundingClientRect().left,e.parentElement.getBoundingClientRect().top,"
                    L"e.parentElement.getBoundingClientRect().width,e.parentElement.getBoundingClientRect().height].join(',')+'@'"
                    L"+(e.parentElement.getAttribute('style')||''):'none')+'|need='+t+'x'+n+'@'+r);return");
            const std::wstring consoleError=L"function M(){try{";
            const auto consoleErrorPosition=text.find(consoleError);
            if(consoleErrorPosition!=std::wstring::npos)
                text.replace(consoleErrorPosition,consoleError.size(),
                    L"function M(){window.chrome.webview.postMessage("
                    L"'KAKAO_CONSOLE_ERROR:'+Array.from(arguments).map("
                    L"function(value){return value&&value.stack?value.stack:String(value)})"
                    L".join('|'));try{");
            const std::wstring managerLoad=
                L"key:\"load\",value:function(){var e=this;"
                L"zt.findAdElements().forEach(";
            const auto managerLoadPosition=text.find(managerLoad,240000);
            if(managerLoadPosition!=std::wstring::npos)
                text.replace(managerLoadPosition,managerLoad.size(),
                    L"key:\"load\",value:function(){var e=this,found=zt.findAdElements();"
                    L"window.chrome.webview.postMessage('KAKAO_ELEMENTS:'+found.length);"
                    L"found.forEach(");
            const std::wstring managerAdd=
                L"this.adMediation.addContexts([{name:\"adfit\",adElement:e,params:{}}])";
            const auto managerAddPosition=text.find(managerAdd,240000);
            if(managerAddPosition!=std::wstring::npos)
                text.replace(managerAddPosition,managerAdd.size(),
                    L"window.chrome.webview.postMessage('KAKAO_ADD_CONTEXT:'+t),"
                    +managerAdd);
            const std::wstring mediationAdd=
                L"key:\"addContexts\",value:function(e){if(t.validateContexts(e)){";
            const auto mediationAddPosition=text.find(mediationAdd,235000);
            if(mediationAddPosition!=std::wstring::npos)
                text.replace(mediationAddPosition,mediationAdd.size(),
                    L"key:\"addContexts\",value:function(e){"
                    L"window.chrome.webview.postMessage('KAKAO_MEDIATION_ADD:'"
                    L"+e.length);if(t.validateContexts(e)){ ");
            const std::wstring processAdStart=
                L"case 0:if(t.isRemoved||t.isDone){e.next=3;break}"
                L"if(n=t.contexts.shift()){";
            const auto processAdStartPosition=text.find(processAdStart,235000);
            if(processAdStartPosition!=std::wstring::npos)
                text.replace(processAdStartPosition,processAdStart.size(),
                    L"case 0:window.chrome.webview.postMessage('KAKAO_PROCESS_AD:'"
                    L"+t.contexts.length+'|'+t.isRemoved+'|'+t.isDone);"
                    L"if(t.isRemoved||t.isDone){e.next=3;break}"
                    L"if(n=t.contexts.shift()){");
            const std::wstring loadAdFailure=
                L"this.log(\"Failed to load ad\",r),e.abrupt(\"return\",void 0)";
            const auto loadAdFailurePosition=text.find(loadAdFailure,235000);
            if(loadAdFailurePosition!=std::wstring::npos)
                text.replace(loadAdFailurePosition,loadAdFailure.size(),
                    L"window.chrome.webview.postMessage('KAKAO_LOAD_AD_FAILURE:'"
                    L"+(r&&r.name?r.name:'Error')+':'"
                    L"+(r&&r.message?r.message:String(r))+'|'"
                    L"+(r&&r.stack?r.stack:'')),"+loadAdFailure);
            const std::wstring staticCapture=
                L"value:function(e,n){return e instanceof t?e.capture(n):this.captureBy";
            auto staticPosition=text.find(staticCapture);
            if(staticPosition!=std::wstring::npos)
                text.replace(staticPosition,staticCapture.size(),
                    L"value:function(e,n){window.chrome.webview.postMessage('KAKAO_STATIC_CAPTURE:'"
                    L"+(e&&e.name?e.name:'Error')+':' +(e&&e.message?e.message:String(e))"
                    L"+'|'+JSON.stringify(e&&e.detail?e.detail:{}));return e instanceof t?e.capture(n):this.captureBy");
            const std::wstring instanceCapture=
                L"value:function(e){return t.captureBy(t.getSentryInstance(),this,e)}";
            auto instancePosition=text.find(instanceCapture);
            if(instancePosition!=std::wstring::npos)
                text.replace(instancePosition,instanceCapture.size(),
                    L"value:function(e){window.chrome.webview.postMessage('KAKAO_INSTANCE_CAPTURE:'"
                    L"+(this&&this.name?this.name:'Error')+':' +(this&&this.message?this.message:String(this))"
                    L"+'|'+JSON.stringify(this&&this.detail?this.detail:{}));return t.captureBy(t.getSentryInstance(),this,e)}");
            const std::wstring needle=L"n.captureException(r,h,(function(n)";
            const auto position=text.find(needle);
            if(position!=std::wstring::npos)
                text.replace(position,needle.size(),
                    L"window.chrome.webview.postMessage('KAKAO_CAPTURE:'+(r&&r.name?r.name:'Error')"
                    L"+':' +(r&&r.message?r.message:String(r))+'|'+JSON.stringify(r&&r.detail?r.detail:{}));"+needle);
            const std::wstring fetchFailure=L"throw new $e(\"Fetch request has failed\"";
            const auto fetchPosition=text.find(fetchFailure);
            if(fetchPosition!=std::wstring::npos)
                text.replace(fetchPosition,fetchFailure.size(),
                    L"window.chrome.webview.postMessage('KAKAO_FETCH_FAILURE:'+(e&&e.name?e.name:'Error')"
                    L"+':' +(e&&e.message?e.message:String(e)));throw new $e(\"Fetch request has failed\"");
            const std::wstring bootstrapCatch=L"}catch(e){console.error(e),u=e,c=\"3084\"";
            const auto bootstrapCatchPosition=text.rfind(bootstrapCatch);
            if(bootstrapCatchPosition!=std::wstring::npos)
                text.replace(bootstrapCatchPosition,bootstrapCatch.size(),
                    L"}catch(e){window.chrome.webview.postMessage('KAKAO_BOOTSTRAP:'"
                    L"+(e&&e.name?e.name:'Error')+':' +(e&&e.message?e.message:String(e))"
                    L"+'|'+(e&&e.stack?e.stack:''));console.error(e),u=e,c=\"3084\"");
        }
        if(argumentCount<3&&
           target.find(L"partners.coupangcdn.com/widget/carousel/default/main-")!=std::wstring::npos){
            text=L"window.chrome.webview.postMessage('COUPANG_MAIN_BEGIN');try{"+text+
                 L";window.chrome.webview.postMessage('COUPANG_MAIN_END');}catch(error){"
                 L"window.chrome.webview.postMessage('COUPANG_MAIN_ERROR:'"
                 L"+(error&&error.name?error.name:'Error')+':'"
                 L"+(error&&error.message?error.message:String(error))+'|'"
                 L"+(error&&error.stack?error.stack:''));}";
        }
        static std::mutex outputMutex;
        {std::lock_guard<std::mutex> lock(outputMutex);
            std::wcout<<L"TEXT "<<(response.Ok()?L"ok":L"fail")<<L" status="
                      <<response.status<<L" chars="<<text.size()<<L" "<<target<<L'\n';}
        return response.Ok();
    });
    view->SetBinaryResourceLoader([&](const std::wstring& resource,
                                      std::vector<unsigned char>& bytes){
        const auto target=Resolve(url,resource);
        const auto response=client.Get(target,url);if(response.Ok())bytes=response.body;
        static std::mutex outputMutex;
        if(target.find(L"logs-partners.coupang.com/")==std::wstring::npos){
            std::lock_guard<std::mutex> lock(outputMutex);
            std::wcout<<L"BINARY "<<(response.Ok()?L"ok":L"fail")<<L" status="
                      <<response.status<<L" bytes="<<bytes.size()<<L" "<<target<<L'\n';
        }
        return response.Ok();
    });
    bool loaded=false;std::wstring loadError;
    view->SetLoadHandler([&](bool success,const std::wstring& error){
        loaded=success;loadError=error;
        std::wcout<<L"LOAD success="<<success<<L" error="<<Compact(error)<<L'\n';
    });
    view->NavigateToStringAsync(html,url);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(std::chrono::steady_clock::now()<deadline){
        MSG message{};size_t processed=0;
        while(processed++<1000&&PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::wstring state,error;
    if(directCoupang)
        view->ExecuteScript(
            L"var container=document.getElementById('container'),images=document.querySelectorAll('img');"
            L"return 'ready='+document.readyState+'|children='+(container?container.children.length:-1)"
            L"+'|images='+images.length+'|view0='"
            L"+document.querySelectorAll('.view[index=\"0\"] img[data-src]').length"
            L"+'|first='+(images.length?images[0].src:'none')"
            L"+'|sample='+Array.from(images).slice(0,8).map(function(image){"
            L"return (image.getAttribute('src')||'')+'@'+(image.getAttribute('data-src')||'');}).join(',')"
            L"+'|last='+(images.length?images[images.length-1].src:'none');",
            &state,&error);
    else
        view->ExecuteScript(
            L"var ad=document.querySelector('.kakao_ad_area');"
            L"var frames=document.querySelectorAll('iframe');var images=document.querySelectorAll('img');"
            L"return 'ready='+document.readyState+'|body='+document.body.innerHTML.substring(0,3000)"
            L"+'|ad='+(ad?ad.outerHTML:'missing')+'|frames='+frames.length+'|images='+images.length"
            L"+'|scripts='+document.scripts.length+'|fetch='+typeof fetch+'|xhr='+typeof XMLHttpRequest;",
            &state,&error);
    std::wcout<<L"STATE "<<Compact(state)<<L" error="<<Compact(error)
              <<L" lastError="<<Compact(view->LastError())<<L'\n';
    HDC dc=GetDC(view->Window());std::unordered_set<COLORREF> colors;
    size_t valid=0,nonNeutral=0;
    for(int y=2;dc&&y<88;y+=3)for(int x=2;x<726;x+=3){
        const auto color=GetPixel(dc,x,y);if(color==CLR_INVALID)continue;
        ++valid;colors.insert(color);
        const auto r=GetRValue(color),g=GetGValue(color),b=GetBValue(color);
        if(std::max({r,g,b})-std::min({r,g,b})>8||r<235||g<235||b<235)++nonNeutral;
    }
    if(dc)ReleaseDC(view->Window(),dc);
    std::wcout<<L"PIXELS valid="<<valid<<L" unique="<<colors.size()
              <<L" nonNeutral="<<nonNeutral<<L" diagnostic="<<Compact(diagnostic)<<L'\n';
    RECT screenBounds{};GetWindowRect(view->Window(),&screenBounds);
    HDC screen=GetDC(nullptr);std::unordered_set<COLORREF> screenColors;
    size_t screenValid=0,screenNonNeutral=0;
    HDC capture=screen?CreateCompatibleDC(screen):nullptr;void* pixels=nullptr;
    BITMAPINFO bitmapInfo{};bitmapInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth=728;bitmapInfo.bmiHeader.biHeight=-90;
    bitmapInfo.bmiHeader.biPlanes=1;bitmapInfo.bmiHeader.biBitCount=32;
    bitmapInfo.bmiHeader.biCompression=BI_RGB;
    HBITMAP bitmap=capture?CreateDIBSection(capture,&bitmapInfo,DIB_RGB_COLORS,&pixels,nullptr,0):nullptr;
    HGDIOBJ previous=bitmap?SelectObject(capture,bitmap):nullptr;
    const bool copied=bitmap&&BitBlt(capture,0,0,728,90,screen,screenBounds.left,screenBounds.top,SRCCOPY);
    const auto* samples=static_cast<const std::uint32_t*>(pixels);
    for(int y=2;copied&&y<88;y+=3)for(int x=2;x<726;x+=3){
        const auto color=samples[y*728+x]&0x00ffffffu;
        ++screenValid;screenColors.insert(color);
        const auto b=color&0xffu,g=(color>>8)&0xffu,r=(color>>16)&0xffu;
        if(std::max({r,g,b})-std::min({r,g,b})>8||r<235||g<235||b<235)++screenNonNeutral;
    }
    if(previous)SelectObject(capture,previous);
    if(bitmap)DeleteObject(bitmap);if(capture)DeleteDC(capture);
    if(screen)ReleaseDC(nullptr,screen);
    std::vector<HWND> childWindows;
    EnumChildWindows(view->Window(),[](HWND child,LPARAM data){
        reinterpret_cast<std::vector<HWND>*>(data)->push_back(child);return TRUE;
    },reinterpret_cast<LPARAM>(&childWindows));
    std::wcout<<L"SCREEN_PIXELS valid="<<screenValid<<L" unique="<<screenColors.size()
              <<L" nonNeutral="<<screenNonNeutral<<L" childWindows="<<childWindows.size()<<L'\n';
    view.reset();DestroyWindow(host);return loaded?0:2;
}

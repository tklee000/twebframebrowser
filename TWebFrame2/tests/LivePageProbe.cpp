#define NOMINMAX
#include "../../Browser/HttpClient.h"
#include <TWebFrame/TWebFrame.h>
#include "../src/DOM.h"
#include "../src/JavaScript.h"

#include <algorithm>
#include <cwctype>
#include <iostream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <unordered_set>

using TWebFrame::Internal::Document;
using TWebFrame::Internal::JavaScriptRuntime;

namespace {
std::wstring Resolve(const std::wstring& base, const std::wstring& reference) {
    if (reference.rfind(L"https://", 0) == 0 || reference.rfind(L"http://", 0) == 0)
        return reference;
    const auto scheme = base.find(L"://");
    if (reference.rfind(L"//", 0) == 0)
        return (scheme == std::wstring::npos ? L"https:" : base.substr(0, scheme) + L":") + reference;
    if (scheme == std::wstring::npos) return reference;
    const auto originEnd = base.find_first_of(L"/?#", scheme + 3);
    const auto origin = originEnd == std::wstring::npos ? base : base.substr(0, originEnd);
    if (!reference.empty() && reference.front() == L'/') return origin + reference;
    const auto suffix = base.find_first_of(L"?#");
    const auto cleanBase = suffix == std::wstring::npos ? base : base.substr(0, suffix);
    const auto slash = cleanBase.find_last_of(L'/');
    return (slash == std::wstring::npos ? origin + L"/" : cleanBase.substr(0, slash + 1)) + reference;
}

std::wstring Compact(std::wstring value) {
    for (auto& character : value)
        if (character < L' ') character = L' ';
    if (value.size() > 5000) value.resize(5000);
    return value;
}

bool SkipNonAdvertisingScript(const std::wstring& target) {
    std::wstring lower = target;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    const bool siteScript =
        (lower.find(L"cdn3.ppomppu.co.kr/") != std::wstring::npos ||
         lower.find(L"cdn4.ppomppu.co.kr/") != std::wstring::npos ||
         lower.find(L"www.ppomppu.co.kr/") != std::wstring::npos) &&
        lower.find(L".js") != std::wstring::npos;
    if (siteScript && lower.find(L"slot_handler.js") == std::wstring::npos &&
        lower.find(L"wtg_ads.js") == std::wstring::npos) return true;
    return lower.find(L"googletagmanager.com/") != std::wstring::npos;
}

bool IsAdvertisingResource(const std::wstring& target) {
    std::wstring lower = target;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    for (const auto* marker : {L"google", L"doubleclick", L"wtg-ads", L"googlesyndication",
                               L"gampad", L"ads?", L"adurl", L"criteo", L"openx", L"kakao"})
        if (lower.find(marker) != std::wstring::npos) return true;
    return false;
}

void LogResource(const wchar_t* kind, const std::wstring& target, bool loaded,
                 std::size_t size) {
    if (!IsAdvertisingResource(target)) return;
    static std::mutex outputMutex;
    std::lock_guard<std::mutex> lock(outputMutex);
    std::wcout << L"RESOURCE_" << kind << L' ' << (loaded ? L"ok" : L"fail")
               << L" bytes=" << size << L' ' << target << L'\n';
}
}

int wmain() {
    std::wcout << std::unitbuf;
    const std::wstring url =
        L"https://www.ppomppu.co.kr/zboard/view.php?id=freeboard&page=1&divpage=1893&category=2&no=10133266";
    HttpClient client;
    const auto response = client.Get(url, L"https://www.ppomppu.co.kr/");
    std::wcout << L"status=" << response.status << L" bytes=" << response.body.size()
               << L" type=" << response.contentType << L" final=" << response.url << L'\n';
    if (!response.Ok()) {
        std::wcout << L"error=" << response.error << L'\n';
        return 1;
    }
    const auto html = HttpClient::DecodeText(response);
    std::wcout << L"characters=" << html.size() << L'\n';

    const std::wregex iframe(LR"(<iframe\b[^>]*>)", std::regex_constants::icase);
    for (auto it = std::wsregex_iterator(html.begin(), html.end(), iframe);
         it != std::wsregex_iterator(); ++it)
        std::wcout << L"IFRAME " << it->str() << L'\n';

    const std::wregex scriptTag(LR"(<script\b[^>]*>[\s\S]*?</script\s*>)",
                                std::regex_constants::icase);
    const std::wregex scriptSource(LR"(\bsrc\s*=\s*(['"])(.*?)\1)",
                                   std::regex_constants::icase);
    std::size_t inlineIndex = 0;
    for (auto it = std::wsregex_iterator(html.begin(), html.end(), scriptTag);
         it != std::wsregex_iterator(); ++it) {
        const auto script = it->str();
        std::wsmatch source;
        if (std::regex_search(script, source, scriptSource)) {
            std::wcout << L"SCRIPT_SRC " << source[2].str() << L'\n';
            continue;
        }
        auto lower = script;
        std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
        if (lower.find(L"ad") == std::wstring::npos &&
            lower.find(L"iframe") == std::wstring::npos &&
            lower.find(L"document.write") == std::wstring::npos &&
            lower.find(L"google") == std::wstring::npos) continue;
        ++inlineIndex;
        std::wstring oneLine = script;
        std::replace(oneLine.begin(), oneLine.end(), L'\r', L' ');
        std::replace(oneLine.begin(), oneLine.end(), L'\n', L' ');
        if (oneLine.size() > 2000) oneLine.resize(2000);
        std::wcout << L"INLINE_" << inlineIndex << L' ' << oneLine << L'\n';
    }

    // The View below already executes the page scripts.  Keep the standalone
    // interpreter pass available for focused debugging without doubling the
    // live advertising workload during renderer diagnostics.
    if (false) {
    Document document;
    std::wstring parseError;
    if (!document.Parse(html, &parseError)) {
        std::wcout << L"PARSE_ERROR " << parseError << L'\n';
        return 2;
    }
    JavaScriptRuntime runtime(document);
    runtime.SetLocation(response.url);
    runtime.SetViewportSize(1188, 716);
    runtime.SetDevicePixelRatio(1.5);
    runtime.SetResourceLoader([&](const std::wstring& resource, std::wstring& text) {
        const auto target = Resolve(response.url, resource);
        const auto item = client.Get(target, response.url);
        if (!item.Ok()) return false;
        text = HttpClient::DecodeText(item);
        return true;
    });
    std::size_t scriptIndex = 0;
    for (const auto& node : document.QuerySelectorAll(L"script")) {
        ++scriptIndex;
        auto type = node->Attribute(L"type");
        std::transform(type.begin(), type.end(), type.begin(), towlower);
        if (!type.empty() && type != L"text/javascript" && type != L"application/javascript" &&
            type != L"text/ecmascript" && type != L"application/ecmascript" && type != L"module")
            continue;
        const auto source = node->Attribute(L"src");
        std::wstring program;
        std::wstring label = source.empty() ? L"inline:" + std::to_wstring(scriptIndex)
                                            : Resolve(response.url, source);
        if (source.empty()) program = node->InnerText();
        else {
            const auto item = client.Get(label, response.url);
            if (!item.Ok()) {
                std::wcout << L"LOAD_ERROR " << label << L" status=" << item.status
                           << L" error=" << item.error << L'\n';
                continue;
            }
            program = HttpClient::DecodeText(item);
        }
        std::wcout << L"JS_BEGIN " << label << L" bytes=" << program.size() * sizeof(wchar_t)
                   << L" hash=" << std::hash<std::wstring>{}(program) << L'\n';
        std::wstring error;
        const bool ok = runtime.Execute(program, nullptr, &error);
        std::wcout << (ok ? L"JS_OK " : L"JS_ERROR ") << label;
        if (!ok) std::wcout << L" :: " << Compact(error);
        std::wcout << L'\n';
    }
    runtime.SetDocumentReadyState(L"interactive");
    runtime.DispatchDocumentEvent(L"DOMContentLoaded");
    runtime.SetDocumentReadyState(L"complete");
    runtime.DispatchWindowEvent(L"load");
    for (int index = 0; index < 5; ++index) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        runtime.RunTimers();
        runtime.RunAnimationFrame();
    }
    std::wcout << L"iframes_after_scripts=" << document.QuerySelectorAll(L"iframe").size() << L'\n';
    }

    SetProcessDPIAware();
    auto host = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"live page probe",
                                WS_POPUP | WS_VISIBLE, 50, 50, 1188, 716,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    RECT bounds{0, 0, 1188, 716};
    auto view = TWebFrame::View::Create(host, bounds);
    bool loaded = false;
    std::wstring loadError;
    std::wstring diagnosticMessage;
    view->SetMessageHandler([&](const std::wstring& message) {
        if(message.rfind(L"KAKAO_ERROR:",0)==0) diagnosticMessage=message;
    });
    view->SetParallelResourceLoading(true);
    view->SetPageScriptsEnabled(true);
    view->SetResourceLoader([&](const std::wstring& resource, std::wstring& text) {
        const auto target = Resolve(response.url, resource);
        if (SkipNonAdvertisingScript(target)) {
            text.clear();
            LogResource(L"TEXT", target, true, 0);
            return true;
        }
        const auto item = client.Get(target, response.url);
        if (!item.Ok()) {
            LogResource(L"TEXT", target, false, 0);
            return false;
        }
        text = HttpClient::DecodeText(item);
        if (target.find(L"/gampad/ads?") != std::wstring::npos && text.size() > 5000) {
            for (const auto* needle : {L"contentWindow.document", L"document.write",
                                       L"srcdoc", L"google_ads_iframe_"}) {
                const auto position = text.find(needle);
                std::wcout << L"GPT_BODY " << needle << L" at=" << position;
                if (position != std::wstring::npos) {
                    const auto start = position > 240 ? position - 240 : 0;
                    std::wcout << L" " << Compact(text.substr(start, 900));
                }
                std::wcout << L'\n';
            }
        }
        if (target.find(L"/pubads_impl.js") != std::wstring::npos) {
            const std::wstring needle = L"}catch(g){}f.state=1";
            const std::wstring replacement =
                L"}catch(g){window.__gptResponseError=(g&&g.name?g.name:'Error')+':'"
                L"+(g&&g.message?g.message:String(g))}f.state=1";
            const auto position = text.find(needle);
            if (position != std::wstring::npos) text.replace(position, needle.size(), replacement);
            const std::wstring streamNeedle =
                L"k2=function(a,b){a.state=4;try{a.G(b)}catch(c){}}";
            const std::wstring streamReplacement =
                L"k2=function(a,b){window.__gptStreamError=(b&&b.name?b.name:'Error')+':'"
                L"+(b&&b.message?b.message:String(b));a.state=4;try{a.G(b)}catch(c){}}";
            const auto streamPosition = text.find(streamNeedle);
            if (streamPosition != std::wstring::npos)
                text.replace(streamPosition, streamNeedle.size(), streamReplacement);
            const std::wstring fetchNeedle = L"var Oqa=function(a,b,c){";
            const std::wstring fetchReplacement =
                L"var Oqa=function(a,b,c){window.__gptFetchError=(b&&b.name?b.name:'Error')+':'"
                L"+(b&&b.message?b.message:String(b));";
            const auto fetchPosition = text.find(fetchNeedle);
            if (fetchPosition != std::wstring::npos)
                text.replace(fetchPosition, fetchNeedle.size(), fetchReplacement);
            const std::wstring deliveryStart = L"var wqa=function(a,b,c){function d(){";
            const auto deliveryPosition = text.find(deliveryStart);
            if (deliveryPosition != std::wstring::npos) {
                text.replace(deliveryPosition, deliveryStart.size(),
                             deliveryStart + L"try{");
                const std::wstring deliveryEnd = L"n.close();return!0}";
                const auto endPosition = text.find(deliveryEnd,
                                                   deliveryPosition + deliveryStart.size());
                if (endPosition != std::wstring::npos)
                    text.replace(endPosition, deliveryEnd.size(),
                        L"n.close();return!0}catch(error){window.__gptDeliveryError="
                        L"(error&&error.name?error.name:'Error')+':'"
                        L"+(error&&error.message?error.message:String(error));return!1}}");
            }
        }
        if (target.find(L"t1.kakaocdn.net/kas/static/ba.min.js") != std::wstring::npos) {
            const std::wstring needle = L"n.captureException(r,h,(function(n)";
            const auto position = text.find(needle);
            if(position != std::wstring::npos)
                text.replace(position,needle.size(),
                    L"window.chrome.webview.postMessage('KAKAO_ERROR:'+(r&&r.name?r.name:'Error')"
                    L"+':' +(r&&r.message?r.message:String(r)));" + needle);
        }
        LogResource(L"TEXT", target, true, text.size() * sizeof(wchar_t));
        return true;
    });
    view->SetBinaryResourceLoader([&](const std::wstring& resource,
                                     std::vector<unsigned char>& bytes) {
        const auto target = Resolve(response.url, resource);
        const auto item = client.Get(target, response.url);
        if (!item.Ok()) {
            LogResource(L"BINARY", target, false, 0);
            return false;
        }
        bytes = item.body;
        LogResource(L"BINARY", target, true, bytes.size());
        return true;
    });
    view->SetLoadHandler([&](bool success, const std::wstring& error) {
        loaded = success;
        loadError = error;
    });
    view->NavigateToStringAsync(html, response.url);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::wcerr << L"PAGE_LAST_ERROR " << Compact(view->LastError()) << L'\n';
    std::wcerr << L"PAGE_DIAGNOSTIC " << Compact(diagnosticMessage) << L'\n';
    std::wstring wtgResult, wtgError;
    view->ExecuteScript(
        L"var nodes=document.querySelectorAll('.w2g-slot'),out=[];"
        L"for(var i=0;i<nodes.length;i++){var node=nodes[i],r=node.getBoundingClientRect();"
        L"out.push((node.getAttribute('data-slot')||node.id)+':'+r.x+':'+r.y+':'"
        L"+r.width+':'+r.height+':children='+node.children.length+':html='"
        L"+node.innerHTML.substring(0,1000));}return out.join('||');",
        &wtgResult, &wtgError);
    std::wcout << L"WTG_SLOTS " << Compact(wtgResult) << L" error="
               << Compact(wtgError) << L'\n';
    std::wstring commandResult, commandError;
    view->ExecuteScript(
        L"try{window.probeCmdRan=0;googletag.cmd.push(function(){window.probeCmdRan++;});"
        L"return 'ok:'+window.probeCmdRan;}catch(error){return 'error:'+error.name+':'+error.message;}",
        &commandResult,&commandError);
    std::wcout<<L"CMD_PROBE "<<Compact(commandResult)<<L" error="<<Compact(commandError)<<L'\n';
    std::wstring adFrameResult,adFrameError;
    view->ExecuteScript(
        L"const allFrames=document.querySelectorAll('iframe');let frame=null;"
        L"for(let i=0;i<allFrames.length;i++)if(allFrames[i].id.indexOf('google_ads_iframe_')===0){frame=allFrames[i];break;}"
        L"return frame?'src='+frame.src+'|srcdoc='+frame.srcdoc.length+'|contentDocument='"
        L"+typeof frame.contentDocument+'|windowDocument='+typeof frame.contentWindow.document"
        L"+'|childNodes='+frame.childNodes.length:'missing';",
        &adFrameResult,&adFrameError);
    std::wcout<<L"AD_FRAME "<<Compact(adFrameResult)<<L" error="<<Compact(adFrameError)<<L'\n';
    const auto inspectFrames = [&](const wchar_t* label) {
    const auto pendingError=view->LastError();
    std::wstring viewResult, viewError;
    const bool inspected = view->ExecuteScript(
        L"var slot=document.querySelector('ins.adsbygoogle');"
        L"var frames=document.querySelectorAll('iframe'),visibleFrames=0,frameRects='';"
        L"for(var frameIndex=0;frameIndex<frames.length;frameIndex++){"
        L"var frameRect=frames[frameIndex].getBoundingClientRect();"
        L"if(frameRect.width>0&&frameRect.height>0)visibleFrames++;"
        L"var frameStyle=getComputedStyle(frames[frameIndex]);"
        L"frameRects+=(frameIndex?'||':'')+frameRect.x+':'+frameRect.y+':'"
        L"+frameRect.width+':'+frameRect.height+':'+frameStyle.display+':'"
        L"+frameStyle.visibility+':'+frames[frameIndex].src+':'"
        L"+frames[frameIndex].name;}"
        L"var slots=document.querySelectorAll('ins.adsbygoogle'),slotRects='';"
        L"for(var slotIndex=0;slotIndex<slots.length;slotIndex++){"
        L"var slotRect=slots[slotIndex].getBoundingClientRect(),slotStyle=getComputedStyle(slots[slotIndex]);"
        L"slotRects+=(slotIndex?'||':'')+slotRect.x+':'+slotRect.y+':'"
        L"+slotRect.width+':'+slotRect.height+':'+slotStyle.display+':'"
        L"+slotStyle.visibility;}"
        L"var gptNodes=document.querySelectorAll('[data-gpt-slot]'),gptState='';"
        L"for(var gptIndex=0;gptIndex<gptNodes.length;gptIndex++){"
        L"var gptRect=gptNodes[gptIndex].getBoundingClientRect(),gptStyle=getComputedStyle(gptNodes[gptIndex]);"
        L"gptState+=(gptIndex?'||':'')+gptNodes[gptIndex].id+':'+gptRect.x+':'"
        L"+gptRect.y+':'+gptRect.width+':'+gptRect.height+':'"
        L"+gptStyle.display+':'+gptNodes[gptIndex].children.length+':html='"
        L"+gptNodes[gptIndex].innerHTML.substring(0,1200);}"
        L"var registered='unavailable';"
        L"var slotInfos='unavailable';"
        L"if(window.googletag&&googletag.pubads&&googletag.pubads().getSlots){"
        L"var publicSlots=googletag.pubads().getSlots();"
        L"registered=publicSlots.map(function(item){return item.getSlotElementId();}).join(',');"
        L"slotInfos=publicSlots.map(function(item){var info=item.getResponseInformation?item.getResponseInformation():null;"
        L"return item.getAdUnitPath()+':'+item.getSlotElementId()+':'+(info?info.creativeId+'|'+info.lineItemId+'|'+info.isBackfill:'null');}).join('||');}"
        L"var cmdState=typeof window.googletag+':'+(window.googletag?typeof googletag.display:'missing')+':'"
        L"+(window.googletag?typeof googletag.cmd:'missing')+':'"
        L"+(window.googletag&&googletag.cmd?typeof googletag.cmd.push:'missing')+':'"
        L"+(window.googletag&&googletag.cmd?googletag.cmd.length:'missing');"
        L"var wtgState=typeof window.w2g+':'+typeof window.w2gLoaded;"
        L"if(window.w2g){wtgState+=':'+typeof w2g.single+':'+typeof w2g.publisherConfigData;"
        L"if(w2g.publisherConfigData)wtgState+=':keys='+Object.keys(w2g.publisherConfigData).join(',');"
        L"var publisherConfig=w2g.publisherConfigData&&w2g.publisherConfigData[location.hostname];"
        L"if(publisherConfig&&publisherConfig.slots)"
        L"wtgState+=':'+publisherConfig.slots.length+':'"
        L"+publisherConfig.slots.map(function(item){return item.SlotId;}).join(',');}"
        L"var wtgContainer=document.querySelector('[id$=\"-cnt\"]'),wtgChildren='';"
        L"if(wtgContainer)for(var childIndex=0;childIndex<wtgContainer.children.length;childIndex++){"
        L"var child=wtgContainer.children[childIndex];wtgChildren+=(childIndex?',':'')"
        L"+child.tagName+':'+child.id+':'+child.className;}"
        L"var wtgSlotState='missing';if(wtgContainer){var wtgSlot=document.getElementById(wtgContainer.id.slice(0,-4));"
        L"if(wtgSlot){var wtgRect=wtgSlot.getBoundingClientRect(),wtgStyle=getComputedStyle(wtgSlot);"
        L"wtgSlotState=wtgRect.x+':'+wtgRect.y+':'+wtgRect.width+':'+wtgRect.height+':'"
        L"+wtgStyle.display+':'+wtgSlot.children.length+':html='+wtgSlot.innerHTML.substring(0,1600);"
        L"for(var wtgChildIndex=0;wtgChildIndex<wtgSlot.children.length;wtgChildIndex++){"
        L"var wtgChild=wtgSlot.children[wtgChildIndex],wtgChildRect=wtgChild.getBoundingClientRect();"
        L"wtgSlotState+=':child='+wtgChild.tagName+','+wtgChild.id+','+wtgChild.className+','"
        L"+wtgChildRect.width+','+wtgChildRect.height+','+wtgChild.children.length;}}}"
        L"return 'gptError='+(window.__gptResponseError||'')+':'+(window.__gptStreamError||'')+':'"
        L"+(window.__gptFetchError||'')+':'+(window.__gptDeliveryError||'')"
        L"+'|viewport='+innerWidth+':'+innerHeight+':'+devicePixelRatio"
        L"+'|window='+String(window===top)+':'+String(window.top===window)+':'"
        L"+window.top.innerWidth+':'+String(document.defaultView===window)+':'"
        L"+document.documentElement.clientWidth+'|scroll='"
        L"+document.body.scrollWidth+':'+document.body.scrollHeight+'|frames='"
        L"+frames.length+'|slots='+slots.length+'|'"
        L"+(slot?slot.offsetWidth:'missing')+'|'+(slot?slot.clientWidth:'missing')+'|'"
        L"+(slot?slot.getBoundingClientRect().width:'missing')+'|'"
        L"+visibleFrames+'|'+frameRects+'|slotRects='+slotRects+'|gptNodes='"
        L"+gptNodes.length+'|registered='+registered+'|slotInfos='+slotInfos+'|cmd='+cmdState+'|wtg='+wtgState"
        L"+'|wtgChildren='+wtgChildren+'|wtgSlot='+wtgSlotState+'|gptState='+gptState;",
        &viewResult, &viewError);
    std::wcout << label << L" loaded=" << loaded << L" loadError=" << Compact(loadError)
               << L" inspected=" << inspected << L" result=" << Compact(viewResult)
               << L" inspectError=" << Compact(viewError) << L" lastError="
               << Compact(pendingError) << L'\n';
    };
    inspectFrames(L"VIEW_BEFORE");
    // A creative may contain an unmatched UTF-16 surrogate.  Do not let a
    // diagnostic stream conversion failure suppress the remaining evidence.
    std::wcout.clear();
    std::wstring scrollResult, scrollError;
    view->ExecuteScript(L"window.scrollTo(0,1000);return document.body.scrollTop;",
                        &scrollResult,&scrollError);
    const auto scrollDeadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    while(std::chrono::steady_clock::now()<scrollDeadline){
        MSG message{};
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::wcout<<L"SCROLL_RESULT "<<scrollResult<<L" error="<<Compact(scrollError)<<L'\n';
    adFrameResult.clear();adFrameError.clear();
    view->ExecuteScript(
        L"const allFrames=document.querySelectorAll('iframe');let frame=null;"
        L"for(let i=0;i<allFrames.length;i++)if(allFrames[i].id.indexOf('google_ads_iframe_')===0){frame=allFrames[i];break;}"
        L"return frame?'src='+frame.src+'|srcdoc='+frame.srcdoc.length+'|contentDocument='"
        L"+typeof frame.contentDocument+'|windowDocument='+typeof frame.contentWindow.document"
        L"+'|same='+(frame.contentDocument===frame.contentWindow.document)"
        L"+'|written='+frame.contentDocument['$text'].length"
        L"+'|nameLength='+frame.contentWindow.name.length"
        L"+'|nameHead='+frame.contentWindow.name.substring(0,160):'missing';",
        &adFrameResult,&adFrameError);
    std::wcerr<<L"AD_FRAME_AFTER "<<Compact(adFrameResult)<<L" error="<<Compact(adFrameError)<<L'\n';
    for(HWND child=FindWindowExW(view->Window(),nullptr,L"TWebFrame.View.1",nullptr);
        child;child=FindWindowExW(view->Window(),child,L"TWebFrame.View.1",nullptr)){
        RECT childBounds{};GetWindowRect(child,&childBounds);
        MapWindowPoints(HWND_DESKTOP,view->Window(),reinterpret_cast<POINT*>(&childBounds),2);
        std::wcerr<<L"CHILD_BEFORE_DETAIL "<<childBounds.left<<L":"<<childBounds.top<<L":"
                  <<childBounds.right-childBounds.left<<L":"
                  <<childBounds.bottom-childBounds.top<<L" visible="
                  <<IsWindowVisible(child)<<L'\n';
        const int width=childBounds.right-childBounds.left;
        const int height=childBounds.bottom-childBounds.top;
        if(IsWindowVisible(child)&&width>20&&height>20){
            RedrawWindow(child,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
            HDC dc=GetDC(child);std::unordered_set<COLORREF> colors;
            size_t valid=0,nonNeutral=0;
            if(dc){
                for(int y=4;y<height-4;y+=7)for(int x=4;x<width-4;x+=7){
                    const auto color=GetPixel(dc,x,y);if(color==CLR_INVALID)continue;
                    ++valid;colors.insert(color);
                    const auto r=GetRValue(color),g=GetGValue(color),b=GetBValue(color);
                    if(std::max({r,g,b})-std::min({r,g,b})>8||
                       (r<235||g<235||b<235))++nonNeutral;
                }
                ReleaseDC(child,dc);
            }
            std::wcerr<<L"PIXELS "<<width<<L"x"<<height<<L" valid="<<valid
                      <<L" unique="<<colors.size()<<L" nonNeutral="<<nonNeutral<<L'\n';
        }
    }
    inspectFrames(L"VIEW_AFTER");
    for(HWND child=FindWindowExW(view->Window(),nullptr,L"TWebFrame.View.1",nullptr);
        child;child=FindWindowExW(view->Window(),child,L"TWebFrame.View.1",nullptr)){
        RECT childBounds{};GetWindowRect(child,&childBounds);
        MapWindowPoints(HWND_DESKTOP,view->Window(),reinterpret_cast<POINT*>(&childBounds),2);
        std::wcout<<L"CHILD_RECT "<<childBounds.left<<L":"<<childBounds.top<<L":"
                  <<childBounds.right-childBounds.left<<L":"
                  <<childBounds.bottom-childBounds.top<<L" visible="
                  <<IsWindowVisible(child)<<L'\n';
    }
    view.reset();
    DestroyWindow(host);
    return 0;
}

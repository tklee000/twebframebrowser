#include "DOM.h"
#include "JavaScript.h"
#include <algorithm>
#include <iostream>
#include <vector>

using namespace TWebFrame::Internal;

int wmain() {
    Document document;
    std::wstring error, result;
    if (!document.Parse(L"<body><button id='attached'>keep</button></body>", &error)) return 1;
    JavaScriptRuntime runtime(document);
    runtime.SetJitCompilationThreshold(0);
    std::vector<JavaScriptRuntime::HeapStatistics> samples;
    runtime.SetMessageSink([&](const std::wstring&) { samples.push_back(runtime.GetHeapStatistics()); });
    if (!runtime.Execute(LR"JS(
        var fired=0;
        var attached=document.getElementById('attached');
        attached.addEventListener('click', function(){ fired+=1; });
        var retained=document.createElement('button');
        retained.addEventListener('click', function(){ fired+=10; });
        var queued=Promise.resolve(17).then(function(value){ fired+=value; });
        var retainedResolve,retainedAsyncValue=0;
        var retainedPromise=new Promise(function(resolve){retainedResolve=resolve;});
        async function retainedAwait(){var state={value:91};await retainedPromise;retainedAsyncValue=state.value;}
        retainedAwait();
        var liveController=new AbortController();
        liveController.signal.addEventListener('abort',function(){fired+=100;});
        var savedBound=(function(value){return this.base+value;}).bind({base:9},4);
        var standaloneCalls=0,liveTarget=new EventTarget();
        liveTarget.addEventListener('alive',function(event){if(event.target===liveTarget && event instanceof Event)standaloneCalls++;});
        async function suspended(){var pending=new Promise(function(){});await pending;return 1;}
        function discard() {
            var xml=new DOMParser().parseFromString('<Template><Item><![CDATA[editor]]></Item></Template>','text/xml');
            var entry=xml.getElementsByTagName('Item').item(0);
            entry.savedQuery=entry.getElementsByTagName;
            var sheetNode=document.createElement('style');
            sheetNode.sheet.insertRule('a { color:red }',0);
            sheetNode.savedSheet=sheetNode.sheet;
            var node=document.createElement('button');
            node.addEventListener('click', function(){ return node; });
            var cyclic={}; cyclic.self=cyclic;
            var values=[]; values.savedMap=values.map;
            var bound={}; bound.method=(function(){return this;}).bind(bound,bound);
            node.savedRect=node.getBoundingClientRect;
            var controller=new AbortController();
            controller.signal.addEventListener('abort',function(){return controller;});
            suspended();
            var target=matchMedia('(min-width:1px)');
            target.addEventListener('custom', function(){ return target; });
            var standalone=new EventTarget();
            standalone.addEventListener('cycle',function(){return standalone;});
            var discardedEvent=new MessageEvent('discarded',{data:cyclic});
            discardedEvent.savedDate=new Date(index);
            function closure(){ return closure; }
        }
        for(var index=0;index<40000;index+=1) {
            discard();
            if(index%5000===4999)window.chrome.webview.postMessage('sample');
        }
        attached.click(); retained.click();
        liveTarget.dispatchEvent(new Event('alive'));
    )JS", nullptr, &error)) {
        std::wcerr << error << L'\n'; return 1;
    }
    if (!runtime.Execute(L"return fired;", &result, &error) || result != L"28" || samples.size() != 8) {
        std::wcerr << L"reachable listeners or promise reactions were lost: " << result << L'\n'; return 1;
    }
    if(!runtime.Execute(L"return standaloneCalls+'|'+Date.prototype.getTime.call(new Date(42));",&result,&error)||result!=L"1|42"){
        std::wcerr<<L"EventTarget listeners or Date prototype were lost: "<<result<<L'\n';return 1;
    }
    if(!runtime.Execute(L"retainedResolve(1);liveController.abort();",nullptr,&error)||
       !runtime.Execute(L"return retainedAsyncValue+'|'+fired+'|'+savedBound();",&result,&error)||
       result!=L"91|128|13"){
        std::wcerr<<L"reachable native captures or suspended frames were lost: "<<result<<L'\n';return 1;
    }
    size_t minimum = static_cast<size_t>(-1), maximum = 0;
    for (const auto& sample : samples) {
        const auto live = sample.objects + sample.functions + sample.nativeFunctions + sample.environments;
        minimum = std::min(minimum, live); maximum = std::max(maximum, live);
        std::wcout << L"live=" << live << L" node-listeners=" << sample.nodeListeners
                   << L" collections=" << sample.collections << L'\n';
        if (!sample.collections || sample.nodeListeners > 1500 || live > 10000) return 1;
    }
    if (maximum - minimum > 5000) return 1;
    if(!runtime.Execute(L"var savedFactory=function(){return function(){return 73;};};",nullptr,&error))return 1;
    for(int index=0;index<1200;++index){
        if(!runtime.Execute(L"(function(){function temporary(){return 1;}return temporary();})();",nullptr,&error))return 1;
    }
    const auto afterScripts=runtime.GetHeapStatistics();
    if(afterScripts.prototypeSlots>160||
       !runtime.Execute(L"return savedFactory()();",&result,&error)||result!=L"73"){
        std::wcerr<<L"prototype slot reclamation failed: "<<afterScripts.prototypeSlots<<L'\n';return 1;
    }
    std::wcout<<L"prototype slots after 1200 scripts: "<<afterScripts.prototypeSlots<<L'\n';
    std::wcout << L"Long-job cycle collection preserves reachable DOM and async callbacks\n";
    return 0;
}

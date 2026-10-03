#pragma once
#include <atomic>
#include <chrono>

static unsigned RunEvalWorkerRegressions(){
    struct Case {const wchar_t* name;const wchar_t* script;const wchar_t* expected;};
    const Case cases[]={
        {L"eval cache keeps live caller bindings",L"function f(){var x=1,sum=0;for(var i=0;i<100;i++){x=i;sum+=eval('x');}return sum;}return f();",L"4950"},
        {L"eval cache distinguishes strict callers",L"function a(){eval('var hidden=1');return typeof hidden;}function b(){'use strict';eval('var hidden=1');return typeof hidden;}return a()+'|'+b()+'|'+a()+'|'+b();",L"number|undefined|number|undefined"},
        {L"eval keeps strict this",L"function f(){'use strict';for(var i=0;i<100;i++)if(eval('this')!==null)return false;return true;}return f.call(null);",L"true"},
        {L"prepared eval keeps shadowing and direct versus indirect scopes",L"var x=5;function direct(){var x=7;return eval('x');}function indirect(){var x=7;return (0,eval)('x');}function shadow(eval){return eval('ignored');}return direct()+'|'+indirect()+'|'+shadow(function(s){return s;});",L"7|5|ignored"},
        {L"eval callee is read before argument side effects",L"var saved=eval;function f(){var x=7;function argument(){eval=function(){return 0;};return 'x';}var result=eval(argument());eval=saved;return result;}return f();",L"7"},
        {L"cached eval creates distinct closures",L"function f(x){return eval('(function named(){return x;})');}var a=f(17),b=f(41);return a!==b&&a()===17&&b()===41&&a.name==='named'&&b.name==='named';",L"true"},
        {L"eval function declarations get fresh scopes",L"function f(x){eval('function inner(){return x;}');return inner;}var a=f(17),b=f(41);return a!==b&&a()===17&&b()===41;",L"true"},
        {L"cached eval does not share regex literals",L"var source='/x/g',a=eval(source),b=eval(source);a.lastIndex=8;return a!==b&&b.lastIndex===0;",L"true"},
        {L"nested cached functions do not share regex literals",L"var source='(function(){return /x/g;})',a=eval(source),b=eval(source);return a!==b&&a()!==b();",L"true"},
        {L"read-only eval still invokes getters",L"var hits=0;Object.defineProperty(window,'probe',{get:function(){return ++hits;}});var sum=0;for(var i=0;i<100;i++)sum+=eval('probe');return sum+'|'+hits;",L"5050|100"},
        {L"cached eval retains ReferenceError",L"var failures=0;for(var i=0;i<100;i++)try{eval('missingEvalBinding');}catch(e){if(e.name==='ReferenceError')failures++;}return failures;",L"100"},
        {L"cached eval keeps lexical declarations local",L"var x=9;for(var i=0;i<100;i++)if(eval('let x=2;x')!==2)return false;return x===9;",L"true"},
        {L"cache eviction preserves escaped functions",L"function f(x){return eval('(function named(){return x;})');}var keep=f(17);for(var i=0;i<100;i++)eval(String(i));return keep()===17&&f(41)()===41;",L"true"},
        {L"TrustedScript stringifiers share the prototype",L"var p=trustedTypes.createPolicy('shared',{createScript:function(s){return s;}}),a=p.createScript('this'),b=p.createScript('41');return !Object.prototype.hasOwnProperty.call(a,'toString')&&a.toString===b.toString&&a.toString()==='this'&&a.toJSON()==='this';",L"true"},
        {L"TrustedScript stringifier rejects forged receivers",L"var ok=false;try{TrustedScript.prototype.toString.call(Object.create(TrustedScript.prototype));}catch(e){ok=e.name==='TypeError';}return ok;",L"true"},
        {L"TrustedScript mutable properties do not change eval",L"var p=trustedTypes.createPolicy('immutable',{createScript:function(s){return s;}}),s=p.createScript('41');s.toString=function(){return '0';};s.$primitive='0';return eval(s)===41&&eval(s)===41;",L"true"},
        {L"parameter returns preserve identity and duplicate bindings",L"function id(x){return x;}function second(x,x){return x;}var o={},s='text';return id(o)===o&&id(s)===s&&id()===undefined&&id(null)===null&&second(1,2)===2&&second(1)===undefined;",L"true"},
        {L"parameter defaults and rest retain ordinary calls",L"var calls=0;function id(x=(++calls)){return x;}function rest(...x){return x;}return id()===1&&id()===2&&id(7)===7&&calls===2&&rest(3,4)[1]===4;",L"true"},
        {L"policy input coercion extra arguments and errors are observable",L"var hits=0,extra={},input={toString:function(){hits++;return '41';}},seen=false,p=trustedTypes.createPolicy('coercion',{createScript:function(s,x){seen=typeof s==='string'&&x===extra;return s;}});var s=p.createScript(input,extra),threw=false,q=trustedTypes.createPolicy('throw',{createScript:function(){throw new TypeError('rule');}});try{q.createScript('41');}catch(e){threw=e.name==='TypeError';}return hits===1&&seen&&eval(s)===41&&threw;",L"true"},
        {L"policy result coercion and Symbol rejection are observable",L"var hits=0,p=trustedTypes.createPolicy('result',{createScript:function(){return {toString:function(){hits++;return '41';}};}}),a=p.createScript('ignored'),threw=false;try{p.createScript(Symbol('s'));}catch(e){threw=e.name==='TypeError';}return hits===1&&eval(a)===41&&threw;",L"true"},
    };
    unsigned failures=0;
    for(const auto& test:cases)for(unsigned jit:{0u,16u}){
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(jit);std::wstring answer,error;
        const bool ok=runtime.Execute(test.script,&answer,&error)&&answer==test.expected;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" JIT="<<jit<<L" result="<<answer<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    for(unsigned jit:{0u,16u}){
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(jit);std::wstring answer,error;
        bool ok=runtime.Execute(L"var cacheSource='(function(){return 73;})';eval(cacheSource)();",nullptr,&error);
        // Cross host calls so the module's prototype collector can run after
        // the last Function created by cached eval has become unreachable.
        for(unsigned index=0;ok&&index<100;++index)
            ok=runtime.Execute(L"(function temporary(){return 1;})();",nullptr,&error);
        ok=ok&&runtime.Execute(L"return eval(cacheSource)();",&answer,&error)&&answer==L"73";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"cached eval survives prototype collection JIT="<<jit<<L" result="<<answer<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;JavaScriptRuntime runtime(document);std::wstring answer,error;
        bool ok=runtime.Execute(L"var calls=0;for(var i=0;i<100;i++)eval('setTimeout(function(){calls++;},0)');",nullptr,&error);
        // Each host event-loop turn runs one due timer. Drive every turn.
        for(unsigned turn=0;ok&&turn<100;++turn)runtime.RunTimers();
        ok=ok&&runtime.Execute(L"return calls;",&answer,&error)&&answer==L"100";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"cached eval schedules every callback result="<<answer<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    // Drive the host from Worker wakeups, with timer polling only at timer
    // deadlines. Exercise ordering, UTF-16/data decoding, teardown and reuse.
    {
        Document document;JavaScriptRuntime runtime(document);std::atomic<bool> wake{false};
        runtime.SetWorkerWakeHandler([&]{wake.store(true);});
        auto due=std::chrono::steady_clock::time_point::max();
        runtime.SetTimerScheduler([&](unsigned delay){due=delay?std::chrono::steady_clock::now()+std::chrono::milliseconds(delay):std::chrono::steady_clock::time_point::max();});
        std::wstring answer,error;
        bool ok=runtime.Execute(LR"JS(
            var done=false,valid=true,received=0,elapsed=0;
            var source="onmessage=function(e){postMessage(e.data);};";
            var url=URL.createObjectURL(new Blob([source],{type:'text/javascript'})),worker=new Worker(url);
            var value=Object.create(null);Object.defineProperty(value,'__proto__',{value:'own',enumerable:true});value.text='a\u0000\ud800!\udfff\\\"한글';value.nested=[true,false,null,1.25,1e30];
            worker.onmessage=function(e){
                valid=valid&&e.isTrusted&&e.origin===''&&e.source===null;
                if(received===0)valid=valid&&e.data.text===value.text&&Object.prototype.hasOwnProperty.call(e.data,'__proto__')&&e.data.__proto__==='own'&&e.data.nested[3]===1.25&&e.data.nested[4]===1e30;
                else valid=valid&&e.data===received;
                if(++received===101){var started=performance.now();worker.terminate();elapsed=performance.now()-started;URL.revokeObjectURL(url);done=true;}
            };
            worker.postMessage(value);for(var i=1;i<=100;i++)worker.postMessage(i);
        )JS",nullptr,&error);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        bool done=false;
        while(ok&&!done&&std::chrono::steady_clock::now()<deadline){
            if(wake.exchange(false)||std::chrono::steady_clock::now()>=due)runtime.RunTimers();
            ok=runtime.Execute(L"return done;",&answer,&error);done=answer==L"true";if(!done)Sleep(1);
        }
        ok=ok&&done&&runtime.Execute(L"return valid&&received===101&&elapsed<100;",&answer,&error)&&answer==L"true";
        std::wstring debug;runtime.Execute(L"return JSON.stringify({done:done,valid:valid,received:received,elapsed:elapsed});",&debug,nullptr);
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"Worker wakeup ordered data decoding and asynchronous termination state="<<debug<<L" error="<<error<<L'\n';if(!ok)++failures;
        runtime.Clear();runtime.SetWorkerWakeHandler({});runtime.SetTimerScheduler({});
        ok=runtime.Execute(L"return eval('40+2');",&answer,&error)&&answer==L"42";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"realm reset clears eval cache and stopped Workers\n";if(!ok)++failures;
    }
    return failures;
}

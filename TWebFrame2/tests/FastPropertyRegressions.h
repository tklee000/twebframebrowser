#pragma once

static unsigned RunFastPropertyRegressions(){
    struct Case {const wchar_t* name;const wchar_t* script;const wchar_t* expected;};
    const Case cases[]={
        {L"property writes retain a converted key before the RHS",LR"JS(var log='',o={};function f(o,k,v){o[k]=v();return o.x;}var k={toString:function(){log+='k';return 'x';}};function v(){log+='v';return 9;}for(var n=0;n<80;n++)f(o,k,v);log='';return f(o,k,v)+'|'+log;)JS",L"9|kv"},
        {L"write references do not invoke getters",LR"JS(var log='',o={get x(){log+='g';return 2;},set x(v){log+='s'+v;}};function f(o){o.x=7;return 1;}for(var n=0;n<80;n++)f(o);log='';return f(o)+'|'+log;)JS",L"1|s7"},
        {L"compound writes preserve one getter and setter",LR"JS(var log='',v=2,o={get x(){log+='g';return v;},set x(n){log+='s';v=n;}};function f(o){o.x+=3;return 1;}for(var n=0;n<80;n++)f(o);log='';v=2;return f(o)+'|'+v+'|'+log;)JS",L"1|5|gs"},
        {L"post and pre updates preserve property references",LR"JS(var o={x:3};function f(o){var a=o.x++,b=++o.x,c=o.x--,d=--o.x;return [a,b,c,d,o.x].join('|');}for(var n=0;n<80;n++)f(o);o.x=3;return f(o);)JS",L"3|5|5|3|3"},
        {L"array writes retain inherited setters",LR"JS(var hits=0,p=[];Object.defineProperty(p,'2',{set:function(v){hits+=v;}});var a=[0,1];Object.setPrototypeOf(a,p);function f(a){a[2]=7;return a.length;}for(var n=0;n<80;n++)f(a);hits=0;return f(a)+'|'+hits;)JS",L"2|7"},
        {L"typed writes retain conversion and shared storage",LR"JS(var buffer=new ArrayBuffer(4),a=new Uint8Array(buffer),b=new Uint8Array(buffer);function f(a){a[1]=257.8;var n=a[1]++;return n+'|'+a[1];}for(var n=0;n<80;n++)f(a);return f(a)+'|'+b[1];)JS",L"1|2|2"},
        {L"proxy assignment runs the set trap once",LR"JS(var hits=0,p=new Proxy({x:0},{set:function(o,k,v){hits++;o[k]=v;return true;}});function f(p){p.x=17;return p.x;}for(var n=0;n<80;n++)f(p);hits=0;return f(p)+'|'+hits;)JS",L"17|1"},
        {L"strict writes propagate a throwing setter",LR"JS(var o={set x(v){throw 'setter';}};function f(o){'use strict';o.x=2;return 1;}var e;for(var n=0;n<80;n++)try{f(o);}catch(x){e=x;}return e;)JS",L"setter"},
        {L"getters still run before member-call arguments",LR"JS(var log='',o={get f(){log+='g';return function(v){return this===o?v:-1;};}};function arg(){log+='a';return 8;}function f(o){o.x=1;return o.f(arg());}for(var n=0;n<80;n++)f(o);log='';return f(o)+'|'+log;)JS",L"8|ga"},
        {L"RHS mutations preserve the original receiver and key",LR"JS(var o={x:0},old=o,k='x';function rhs(){o={y:0};k='y';return 12;}function f(a,key){a[key]=rhs();return a.x;}for(var n=0;n<80;n++){o=old;k='x';f(o,k);}o=old;k='x';return f(o,k)+'|'+old.x+'|'+o.y;)JS",L"12|12|0"},
        {L"numeric fusion preserves local snapshots across calls",LR"JS(var x=2;function change(){x=100;return 3;}function f(){var n=4;return n+x+change();}for(var n=0;n<80;n++){x=2;f();}x=2;return f()+'|'+x;)JS",L"9|100"},
        {L"numeric fusion keeps signed zero and fractional bitwise semantics",LR"JS(function f(a,b){var x=-0,y=2.8;return [1/(x*a),b>>>y,(a+b)^y,a>b].join('|');}for(var n=0;n<80;n++)f(2,5);return f(2,5);)JS",L"-Infinity|1|5|false"},
    };
    unsigned failures=0;
    for(const auto& test:cases)for(const size_t threshold:{size_t(0),size_t(16)}){
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);
        runtime.SetJitCompilationThreshold(threshold);std::wstring answer,error;
        const bool ran=runtime.Execute(test.script,&answer,&error);
        const bool ok=ran&&answer==test.expected;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" jit="<<threshold<<L" result="<<answer<<L" error="<<error<<L'\n';
        if(!ok)++failures;
    }
    // Host reentry can delete the property and turn the next write into a setter.
    {
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        bool changed=false,reentered=false;
        runtime.SetExecutionYieldHandler([&]{
            if(!changed&&runtime.DiagnosticsJson().find(L"\"fastScopeCalls\":0,")==std::wstring::npos){
                changed=true;std::wstring answer,error;
                reentered=runtime.Execute(L"delete target.x;Object.defineProperty(target,'x',{set:function(v){hits++;},get:function(){return 9;}});",&answer,&error);
            }return true;
        });
        std::wstring answer,error;
        const bool ran=runtime.Execute(L"var target={x:0},hits=0;function set(o){o.x=2;return o.x;}for(var n=0;n<50000;n++)set(target);return hits>0;",&answer,&error);
        const bool ok=ran&&answer==L"true"&&changed&&reentered;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"fast property paths recheck after host reentry\n";if(!ok)++failures;
    }
    {
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        runtime.SetExecutionYieldHandler([&]{return runtime.DiagnosticsJson().find(L"\"fastScopeCalls\":0,")!=std::wstring::npos;});
        std::wstring answer,error;
        const bool ok=!runtime.Execute(L"var o={x:0};function set(o){o.x=2;return o.x;}for(var n=0;n<50000;n++)set(o);",&answer,&error)&&error.find(L"interrupted")!=std::wstring::npos;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"fast property paths retain host cancellation\n";if(!ok)++failures;
    }
    return failures;
}

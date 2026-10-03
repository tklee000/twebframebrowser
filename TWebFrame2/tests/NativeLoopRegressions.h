#pragma once

static unsigned RunNativeLoopRegressions(){
    struct Case {const wchar_t* name;const wchar_t* setup;const wchar_t* body;bool requireNative;};
    const wchar_t* common=LR"JS(
        var chars=[];for(var n=0;n<256;n++)chars[n]=String.fromCharCode(n);
        var bytes=new Uint8Array(2048);for(var n=0;n<bytes.length;n++)bytes[n]=(n*73+17)&255;
        function xor(a,b){return a^b;}function sub(a,b){return a-b;}
        function less(a,b){return a<b;}
    )JS";
    std::wstring wide=LR"JS(function run(){var a=123,b=456;for(var i=0;i<256;i++){)JS";
    for(unsigned round=0;round<48;++round)wide+=L"a=(a+bytes[(i+"+std::to_wstring(round)+L")&2047])^((b<<5)|(b>>>27));b=(b+a)|0;";
    wide+=L"}return a+'|'+b+'|'+i;}return run();";
    const Case cases[]={
        {L"native nested numeric reads cannot move ahead of array stores",LR"JS(var data=[3];function read(){return data[0];}function add(x){return x+read();})JS",LR"JS(function run(){var sum=0;for(var i=0;i<512;i++){data[0]=i;sum+=add(i);}return sum+'|'+data[0];}return run();)JS",false},
        {L"native cache revalidates the exact selected table cell",LR"JS(var names=['add','xor'],helpers={add:function(a,b){return a+b;},xor:xor};function lookup(k){return names[k];}function run(){var sum=0;for(var i=0;i<512;i++)sum=helpers[lookup(0)](sum,bytes[i])|0;return sum;})JS",LR"JS(var output=[];for(var j=0;j<8;j++)output.push(run());names[1]='unused';output.push(run());names[0]='xor';output.push(run());return output.join(',');)JS",true},
        {L"native comparison loops preserve descending and inclusive bounds",L"",LR"JS(function run(n){var sum=0;for(var i=n;i>=0;i--)sum+=bytes[i];var j=0;while(j<=n){sum+=bytes[j];j++;}var k=n;while(k>0)k--;return sum+'|'+i+'|'+j+'|'+k;}var output=[];for(var n=0;n<20;n++)output.push(run(255));return output.join(',');)JS",true},
        {L"native inclusive comparisons reject unordered numeric inputs",L"",LR"JS(function run(n){var sum=0;for(var i=0;i<=n;i++)sum+=bytes[i];return sum+'|'+i;}var output=[];for(var n=0;n<20;n++)output.push(run(128));output.push(run(NaN));return output.join(',');)JS",true},
        {L"native cache invalidates nested captured numeric lookups",LR"JS(var data=[3];function read(){return data[0];}function add(x){return x+read();}function run(){var sum=0;for(var i=0;i<512;i++)sum+=add(i);return sum;})JS",LR"JS(var output=[];for(var j=0;j<8;j++)output.push(run());data[0]=7;output.push(run());return output.join(',');)JS",true},
        {L"native header entries rebind fresh function parameters",L"",LR"JS(function run(seed,n){var sum=seed;for(var i=0;i<n;i++)sum=(sum+bytes[i])|0;return sum+'|'+i;}var output=[];for(var j=0;j<80;j++)output.push(run(j,j%4?128:0));return output.join(',');)JS",true},
        {L"native preparation cache observes replacement helpers and tables",LR"JS(var table=['xor'],helpers={xor:xor};function key(){return table[0];}function run(){var hash=3;for(var i=0;i<512;i++)hash=helpers[key()](hash,bytes[i])|0;return hash;})JS",LR"JS(var result=[];for(var j=0;j<8;j++)result.push(run());helpers.xor=function(a,b){return a+b;};result.push(run());table[0]='sub';helpers.sub=sub;result.push(run());return result.join(',');)JS",true},
        {L"native preparation cache resumes getters after descriptors change",LR"JS(var hits=0,o={key:3};function read(){return o.key;}function run(){var sum=0;for(var i=0;i<512;i++)sum+=read()+bytes[i];return sum;})JS",LR"JS(var output=[];for(var j=0;j<8;j++)output.push(run());Object.defineProperty(o,'key',{get:function(){hits++;return 7;},configurable:true});output.push(run());return output.join(',')+'|'+hits;)JS",true},
        {L"native preparation cache keeps numeric data live",L"",LR"JS(function run(){var sum=0;for(var i=0;i<512;i++)sum+=bytes[i];return sum;}var output=[];for(var j=0;j<8;j++)output.push(run());bytes[111]=1;output.push(run());return output.join(',');)JS",true},
        {L"native while header retains continue and final counter",L"",LR"JS(function run(n){var i=0,sum=0;while(i<n){sum=(sum+bytes[i])|0;i++;}return sum+'|'+i;}var output=[];for(var j=0;j<40;j++)output.push(run(128));return output.join(',');)JS",true},
        {L"native wide scalar graphs preserve every chained update",L"",wide.c_str(),true},
        {L"native scalar recurrence needs no array or string writes",L"",LR"JS(function run(){var hash=123,cursor=0;for(var i=0;less(i,2048);i++){hash=hash+(hash<<1)+(hash<<4)+(hash<<7)+(hash<<8)+(hash<<24)^bytes[cursor++]-3&255;}return hash+'|'+cursor;}return run();)JS",true},
        {L"native scalar guard resumes object coercion in the VM",LR"JS(var data=Array(2048).fill(3),hits=0;data[777]={valueOf:function(){hits++;return 19;}};)JS",LR"JS(function run(){var sum=0;for(var i=0;i<2048;i++)sum+=data[i]&255;return sum+'|'+hits;}return run();)JS",true},
        {L"native scalar fractional sums keep operation ordering",L"",LR"JS(function run(){var a=-0,b=0.125;for(var i=0;i<2048;i++){a=a+bytes[i]*b;b=b+0.003;}return a+'|'+b+'|'+i;}return run();)JS",true},
        {L"native graphs reuse changing keys lengths and receivers",LR"JS(function decode(key,length){var s='';for(var i=0;i<length;i++)s+=chars[bytes[i]^key];return s;}function invoke(receiver,length){var s='';for(var i=0;i<length;i++)s+=chars[xor(bytes[i],receiver.key)];return s;})JS",LR"JS(var result='';for(var j=0;j<30;j++){result+=decode(j*7&255,70+j);result+=invoke({key:j*5&255},70+j);}return result;)JS",true},
        {L"native carried locals exchange their previous values",L"",LR"JS(function run(){var s='',a=3,b=7,t=0;for(var i=0;i<2048;i++){t=a;a=b;b=t;s+=chars[a];}return s+'|'+a+'|'+b+'|'+t;}return run();)JS",true},
        {L"native helper reads a numeric cell updated by the loop",LR"JS(var output=[0];function read(){return output[0];})JS",LR"JS(function run(){var sum=0;for(var i=0;i<2048;i++){output[0]=i;sum+=read();}return sum+'|'+output[0];}return run();)JS",true},
        {L"native numeric array writes snapshot input counters",LR"JS(var output=[0];)JS",LR"JS(function run(){for(var i=0;i<2048;i++)output[0]=i;return output[0]+'|'+i;}return run();)JS",true},
        {L"native inlined calls forward prior numeric array writes",LR"JS(var output=[0,0];function update(a,key){a[key]=7;return a[0];})JS",LR"JS(function run(){var sum=0;for(var i=0;i<2048;i++){output[0]=0;sum+=update(output,i&1);}return sum+'|'+output.join('|');}return run();)JS",true},
        {L"native genuine Function.call keeps its receiver",LR"JS(var state={h:[0,0]};function update(n){this.h[0]=n;return this.h[0]+1;})JS",LR"JS(function run(){var sum=0;for(var i=0;i<2048;i++)sum+=update.call(state,i);return sum+'|'+state.h[0];}return run();)JS",true},
        {L"native object guards retain changing array selections",LR"JS(var state=[[3],[7]],output=[0];function read(a,i){return a[i][0];})JS",LR"JS(function run(){var sum=0;for(var i=0;i<2048;i++){output[0]=i;sum+=read(state,i&1);}return sum+'|'+output[0];}return run();)JS",true},
        {L"native inlining rejects observable getters",LR"JS(var hits=0,output=[0],o={get x(){hits++;return 3;}};function read(o){return o.x;})JS",LR"JS(function run(){var sum=0;for(var i=0;i<2048;i++){output[0]=i;sum+=read(o);}return sum+'|'+hits+'|'+output[0];}return run();)JS",false},
        {L"native inlining observes an overridden call method",LR"JS(var output=[0];function update(n){return 9;}update.call=function(receiver,n){return n+3;};)JS",LR"JS(function run(){var sum=0;for(var i=0;i<2048;i++){output[0]=i;sum+=update.call(null,i);}return sum+'|'+output[0];}return run();)JS",false},
        {L"native output snapshots the counter before its update",L"",LR"JS(function run(){var s='';for(var i=0;i<2048;i++)s+=String.fromCharCode(i);return s;}return run();)JS",true},
        {L"native recurrence and typed byte reads",L"",LR"JS(function run(){var s='',hash=123,cursor=0;for(var i=0;i<2048;i++){s+=chars[(xor(bytes[cursor++],47)^hash>>>16&255)-hash&255];hash=hash+2654435769|0;}return s+'|'+cursor+'|'+hash;}return run();)JS",true},
        {L"native pure helper and constant table keys",LR"JS(var table=['xor','sub'];function lookup(i){return table[i];}var helpers={xor:xor,sub:sub};)JS",LR"JS(function run(){var s='',cursor=0,hash=-987;for(var i=0;less(i,2048);i++){s+=chars[helpers[lookup(0)](helpers[lookup(1)](bytes[cursor++],233),hash)&255];hash=hash+2654435769|0;}return s+'|'+hash+'|'+cursor;}return run();)JS",true},
        {L"native fromCharCode keeps huge and fractional conversions",L"",LR"JS(function run(){var s='';for(var i=0;i<2048;i++)s+=String.fromCharCode((i-987.5)*1e18);return s;}return run();)JS",true},
        {L"native prefix exits before an out-of-range read",L"",LR"JS(function run(){var s='',cursor=0;for(var i=0;i<2100;i++)s+=chars[bytes[cursor++]&255];return s+'|'+cursor;}return run();)JS",true},
        {L"native array holes still observe inherited getters",LR"JS(var hits=0;delete chars[123];Object.defineProperty(Array.prototype,'123',{get:function(){hits++;return 'Q';},set:function(){},configurable:true});)JS",LR"JS(function run(){var s='';for(var i=0;i<2048;i++)s+=chars[bytes[i]];return s+'|'+hits;}return run();)JS",false},
        {L"native numeric-array guard retains nonnumeric coercion",LR"JS(var data=Array(2048).fill(17);data[777]={valueOf:function(){return 19;}};)JS",LR"JS(function run(){var s='';for(var i=0;i<2048;i++)s+=chars[data[i]&255];return s;}return run();)JS",true},
        {L"native writes leave shared string aliases immutable",L"",LR"JS(function run(){var s='seed',old=s;for(var i=0;i<2048;i++)s+=chars[bytes[i]];return old+'|'+s;}return run();)JS",true},
        {L"native strict const writes still throw",L"",LR"JS(function run(){'use strict';const s='';for(var i=0;i<2048;i++)s+=chars[bytes[i]];return s;}try{return run();}catch(e){return e.name;})JS",false},
        {L"native helper cannot hoist a changing captured binding",L"",LR"JS(function run(){var key=1;function adjust(v){return v+key;}var s='';for(var i=0;i<2048;i++){s+=chars[adjust(bytes[i])&255];key++;}return s+'|'+key;}return run();)JS",false},
        {L"native typed shared view keeps the actual byte offset",LR"JS(var buffer=new ArrayBuffer(2056),all=new Uint8Array(buffer);for(var n=0;n<all.length;n++)all[n]=(n*19+7)&255;bytes=new Uint8Array(buffer,8,2048);)JS",LR"JS(function run(){var s='';for(var i=0;i<2048;i++)s+=chars[bytes[i]];return s;}return run();)JS",true},
        {L"native excludes signed backing storage that needs byte masking",LR"JS(var signed=new Int8Array(2048);for(var n=0;n<signed.length;n++)signed[n]=n;bytes=new Uint8Array(signed.buffer);)JS",LR"JS(function run(){var s='';for(var i=0;i<2048;i++)s+=chars[bytes[i]];return s;}return run();)JS",false},
    };
    unsigned failures=0;
    for(const auto& test:cases){
        std::wstring baseline,optimized,error;bool ok=true,used=false;
        for(const size_t threshold:{size_t(0),size_t(16)}){
            Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(threshold);
            std::wstring answer;
            const bool ran=runtime.Execute(std::wstring(common)+test.setup+test.body,&answer,&error);
            ok=ok&&ran;
            if(threshold==0)baseline=std::move(answer);
            else {optimized=std::move(answer);const auto diagnostics=runtime.DiagnosticsJson();used=diagnostics.find(L"\"nativeLoopCalls\":0,")==std::wstring::npos;
                if(std::wstring_view(test.name)==L"native graphs reuse changing keys lengths and receivers")
                    ok=ok&&diagnostics.find(L"\"nativeLoopCacheLimitExits\":0,")!=std::wstring::npos;
                if(std::wstring_view(test.name)==L"native preparation cache keeps numeric data live")
                    ok=ok&&diagnostics.find(L"\"nativeLoopPrepareCacheHits\":0,")==std::wstring::npos;
                if(std::wstring_view(test.name)==L"native header entries rebind fresh function parameters")
                    ok=ok&&diagnostics.find(L"\"nativeLoopHeaderEntries\":0,")==std::wstring::npos;
            }
        }
        ok=ok&&baseline==optimized&&(!test.requireNative||used);
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" native="<<used<<L" error="<<error<<L'\n';
        if(!ok){++failures;std::wcout<<L"  expected="<<baseline.substr(0,100)<<L" actual="<<optimized.substr(0,100)<<L'\n';}
    }
    const struct {const wchar_t* name;const wchar_t* source;const wchar_t* expected;} standards[]={
        {L"deleted own array elements expose inherited getters",LR"JS(var a=[1,2,3],hits=0;delete a[1];Object.defineProperty(Array.prototype,'1',{get:function(){hits++;return 9;},set:function(){},configurable:true});return a[1]+'|'+hits+'|'+a.hasOwnProperty('1')+'|'+Object.hasOwn(a,'1')+'|'+a.propertyIsEnumerable('1')+'|'+Object.keys(a).join(',');)JS",L"9|1|false|false|false|0,2"},
        {L"array deletion preserves own undefined values when redefined",LR"JS(var a=[1,2,3];delete a[1];a[1]=undefined;return a.hasOwnProperty('1')+'|'+Object.keys(a).join(',')+'|'+a.length;)JS",L"true|0,1,2|3"},
        {L"own array cells ignore inherited setters",LR"JS(var a=[1,2,3],hits=0;Object.defineProperty(Array.prototype,'1',{set:function(){hits++;},configurable:true});a[1]=7;return a[1]+'|'+hits;)JS",L"7|0"},
        {L"numeric operators call object conversion with a number hint",LR"JS(var hints=[];var o={[Symbol.toPrimitive]:function(h){hints.push(h);return 19;}};return (o&255)+'|'+(o-1)+'|'+hints.join(',');)JS",L"19|18|number,number"},
        {L"numeric object conversion exceptions propagate",LR"JS(var o={valueOf:function(){throw new Error('conversion');}};try{return o&255;}catch(e){return e.message;})JS",L"conversion"},
    };
    for(const auto& test:standards)for(const size_t threshold:{size_t(0),size_t(16)}){
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(threshold);
        std::wstring answer,error;const bool ran=runtime.Execute(test.source,&answer,&error);
        const bool ok=ran&&answer==test.expected;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" jit="<<threshold<<L" result="<<answer<<L" error="<<error<<L'\n';
        if(!ok)++failures;
    }
    // Changing bytecode must continue warming/compiling beyond the bounded
    // plan cache. Independent expected sums also check evicted code ownership.
    {
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        bool ok=true;std::wstring answer,error;
        for(unsigned index=0;index<300&&ok;++index){
            const auto name=L"changingSource"+std::to_wstring(index);
            const auto script=L"function "+name+L"(){var sum="+std::to_wstring(index)+
                L";for(var i=0;i<128;i++)sum+=i;return sum;}return "+name+L"();";
            ok=runtime.Execute(script,&answer,&error)&&answer==std::to_wstring(index+8128);
        }
        ok=ok&&runtime.DiagnosticsJson().find(L"\"nativeLoopPlanEvictions\":0,")==std::wstring::npos;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"native plan eviction keeps compiling changing sources error="<<error<<L'\n';
        if(!ok)++failures;
    }
    // Large loops ensure a compiled batch reaches a later host-yield check.
    for(const bool cancel:{false,true}){
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        bool changed=false;
        runtime.SetExecutionYieldHandler([&]{
            if(!changed&&runtime.DiagnosticsJson().find(L"\"nativeLoopCalls\":0,")==std::wstring::npos){
                changed=true;if(cancel)return false;
                std::wstring answer,error;return runtime.Execute(L"chars[17]='Z';",&answer,&error);
            }return true;
        });
        std::wstring answer,error;
        const bool ran=runtime.Execute(std::wstring(common)+LR"JS(function run(){var s='';for(var i=0;i<2000000;i++)s+=chars[17];return s.length+'|'+s.lastIndexOf('Z');}return run();)JS",&answer,&error);
        const bool ok=changed&&(cancel?(!ran&&error.find(L"interrupted")!=std::wstring::npos):(ran&&answer.rfind(L"2000000|",0)==0&&answer!=L"2000000|-1"));
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<(cancel?L"native loop host cancellation":L"native loop reentry rebuilds its guarded inputs")<<L" error="<<error<<L'\n';
        if(!ok)++failures;
    }
    return failures;
}

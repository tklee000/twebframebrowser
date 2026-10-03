#pragma once

static unsigned RunByteCopyRegressions(){
    struct Case {const wchar_t* name;const wchar_t* setup;const wchar_t* expected;bool batch;};
    const Case cases[]={
        {L"UTF-16 bytes keep Uint8 conversion",LR"JS(var s='a\u0000\u0101\ud800Z'.repeat(100),a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return [i,a[0],a[1],a[2],a[3],a[4],a[499]].join('|');)JS",L"500|97|0|1|0|90|90",true},
        {L"comma increment copy and let counter",L"var s='abc'.repeat(100),a=new Uint8Array(s.length);for(let i=0;i<s.length;a[i]=s.charCodeAt(i),i++);return a[0]+'|'+a[299];",L"97|99",true},
        {L"pure constant helper keys retain byte batching",L"var keys=['length','charCodeAt'];function key(i){return keys[i];}var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=0;i<s[key(0)];a[i]=s[key(1)](i),i++);return i+'|'+a[299];",L"300|99",true},
        {L"pure helper comparison retains byte batching",L"var keys=['length','charCodeAt'],ops={less:function(a,b){return a<b;}};function key(i){return keys[i];}var s='abc'.repeat(100),a=new Uint8Array(s.length);for(let i=0;ops.less(i,s[key(0)]);a[i]=s[key(1)](i),i++);return a[0]+'|'+a[299];",L"97|99",true},
        {L"captured pure numeric limit retains byte batching",L"var limit=300;function less(i){return i<limit;}var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=0;less(i);i++)a[i]=s.charCodeAt(i);return i+'|'+a[299];",L"300|99",true},
        {L"short helper comparison retains original loop limit",L"var s='abc'.repeat(100),a=new Uint8Array(s.length);function less(a,b){return a<b;}for(var i=0;less(i,100);i++)a[i]=s.charCodeAt(i);return i+'|'+a[99]+'|'+a[100];",L"100|97|0",false},
        {L"impure helper comparison retains every call",L"var hits=0,s='abc'.repeat(100),a=new Uint8Array(s.length);function less(a,b){hits++;return a<b;}for(var i=0;less(i,s.length);i++)a[i]=s.charCodeAt(i);return hits+'|'+a[299];",L"301|99",false},
        {L"typed byte subview updates shared backing storage",L"var s='abc'.repeat(100),buffer=new ArrayBuffer(310),a=new Uint8Array(buffer,5,300),all=new Uint8Array(buffer);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return [all[4],all[5],all[304],all[305]].join('|');",L"0|97|99|0",true},
        {L"short destinations retain ignored out-of-range writes",L"var s='abc'.repeat(100),a=new Uint8Array(2);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return i+'|'+a[0]+'|'+a[1];",L"300|97|98",false},
        {L"signed arrays keep their normal conversion",L"var s='\u00ff'.repeat(300),a=new Int8Array(300);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return a[0]+'|'+a[299];",L"-1|-1",false},
        {L"replacement character reader remains observable",L"var hits=0;String.prototype.charCodeAt=function(i){hits++;return 90;};var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return hits+'|'+a[0]+'|'+a[299];",L"300|90|90",false},
        {L"character reader accessor remains observable",L"var hits=0,read=String.prototype.charCodeAt;Object.defineProperty(String.prototype,'charCodeAt',{get:function(){hits++;return read;}});var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return hits+'|'+a[299];",L"300|99",false},
        {L"inherited character reader receives the original string",L"var hits=0,read=String.prototype.charCodeAt;delete String.prototype.charCodeAt;Object.setPrototypeOf(String.prototype,{get charCodeAt(){'use strict';hits++;if(typeof this!=='string')throw 'receiver';return read;}});var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return hits+'|'+a[299];",L"300|99",false},
        {L"deleted character readers stay absent",L"delete String.prototype.charAt;delete String.prototype.charCodeAt;delete String.prototype.codePointAt;return [typeof 'x'.charAt,typeof 'x'.charCodeAt,typeof 'x'.codePointAt].join('|');",L"undefined|undefined|undefined",false},
        {L"proxy writes retain every trap",L"var hits=0,s='abc'.repeat(100),target=new Uint8Array(s.length),a=new Proxy(target,{set:function(t,k,v){hits++;t[k]=v;return true;}});for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return hits+'|'+target[299];",L"300|99",false},
        {L"loop break retains its exit and remaining zero bytes",L"var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=0;i<s.length;i++){if(i==100)break;a[i]=s.charCodeAt(i);}return i+'|'+a[99]+'|'+a[100];",L"100|97|0",false},
        {L"fractional indexes retain invalid typed writes",L"var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=.5;i<s.length;i++)a[i]=s.charCodeAt(i);return i+'|'+a[0]+'|'+a[299];",L"300.5|0|0",false},
        {L"impure key helper runs for every access",L"var hits=0;function key(){hits++;return 'charCodeAt';}var s='abc'.repeat(100),a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s[key()](i);return hits+'|'+a[299];",L"300|99",false},
    };
    unsigned failures=0;
    for(const auto& test:cases)for(const size_t threshold:{size_t(0),size_t(16)}){
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(threshold);
        std::wstring answer,error;
        const bool ran=runtime.Execute(L"return (function(){"+std::wstring(test.setup)+L"})();",&answer,&error);
        const bool batched=runtime.DiagnosticsJson().find(L"\"byteCopyCalls\":0,")==std::wstring::npos;
        const bool ok=ran&&answer==test.expected&&batched==(test.batch&&threshold!=0);
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" jit="<<threshold<<L" batched="<<batched<<L" result="<<answer<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        runtime.SetExecutionYieldHandler([&]{
            const bool before=runtime.DiagnosticsJson().find(L"\"byteCopyCalls\":0,")!=std::wstring::npos;
            if(before)std::this_thread::sleep_for(std::chrono::milliseconds(35));
            return before;
        });
        std::wstring answer,error;
        const bool ok=!runtime.Execute(L"function copy(s){var a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return a[0];}return copy('a'.repeat(1000000));",&answer,&error)&&error.find(L"interrupted")!=std::wstring::npos;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"byte copy batches retain host cancellation error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        bool changed=false,reentry=false;runtime.SetExecutionYieldHandler([&]{
            const bool before=runtime.DiagnosticsJson().find(L"\"byteCopyCalls\":0,")!=std::wstring::npos;
            if(!changed&&before)std::this_thread::sleep_for(std::chrono::milliseconds(35));
            if(!changed&&!before){changed=true;std::wstring value,error;reentry=runtime.Execute(L"String.prototype.charCodeAt=function(){return 90;};",&value,&error);}return true;
        });
        std::wstring answer,error;
        const bool ok=runtime.Execute(L"function copy(s){var a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return a[0]+'|'+a[a.length-1];}return copy('a'.repeat(300000));",&answer,&error)&&answer==L"97|90"&&changed&&reentry;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"host reentry rechecks byte copy intrinsics error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        std::wstring answer,error,expected;expected.reserve(2100000);
        const unsigned bytes[]={65,1,172,61,66,0,255,128};
        for(size_t at=0;at<524288;++at){if(at)expected+=L',';expected+=std::to_wstring(bytes[at&7]);}
        const bool ran=runtime.Execute(LR"JS(function copy(s){var a=new Uint8Array(s.length);for(var i=0;i<s.length;i++)a[i]=s.charCodeAt(i);return a;}var copied=copy('A\u0101\u20ac\ud83d\ude42\u0000\uffff\u0080'.repeat(65536));return copied.join(',');)JS",&answer,&error);
        const auto diagnostics=runtime.DiagnosticsJson();
        const bool simd=diagnostics.find(L"\"simdByteCopyIterations\":0,")==std::wstring::npos;
        const bool parallel=diagnostics.find(L"\"parallelByteCopyIterations\":0,")==std::wstring::npos;
        const bool ok=ran&&answer==expected&&simd&&(std::thread::hardware_concurrency()<2||parallel);
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"SIMD and parallel byte copy preserve every byte and lane boundary parallel="<<parallel<<L" error="<<error<<L'\n';
        if(!ok)++failures;
    }
    return failures;
}

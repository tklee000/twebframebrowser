#pragma once

// Differential tests use the ordinary interpreter as the reference and also
// check whether unsafe loop shapes actually stayed on the ordinary path.
static int RunStandaloneRegressions(){
    struct InstructionLimitScope {wchar_t previous[64]{};InstructionLimitScope(bool reference){GetEnvironmentVariableW(L"TWEBFRAME_TRACE_INSTRUCTION_LIMIT",previous,64);SetEnvironmentVariableW(L"TWEBFRAME_TRACE_INSTRUCTION_LIMIT",reference?L"10000000":nullptr);}~InstructionLimitScope(){SetEnvironmentVariableW(L"TWEBFRAME_TRACE_INSTRUCTION_LIMIT",*previous?previous:nullptr);}};
    struct Case {const wchar_t* name;const wchar_t* script;bool reduced;};
    const Case cases[]={
        {L"periodic read",LR"((function(){var a=['a','bb','ccc','dddd'];function table(){return a;}function pick(x){return table()[x-19];}var n=2000,s=0;for(var i=0;i<n;i++)s+=pick(19+(i&3)).length;return i+'|'+s;})())",true},
        {L"offset and partial period",LR"((function(){var a=['','a','bb','ccc','dddd'];function renamed(x){return a[x+1];}var n=2137,s=41;for(var i=137;i<n;i++)s+=renamed(i&3).length;return i+'|'+s;})())",true},
        {L"period one",LR"((function(){var a=['xyz'];function pick(x){return a[x];}var s=-100;for(var i=0;i<2049;i++)s+=pick(i&0).length;return i+'|'+s;})())",true},
        {L"fractional bound",LR"((function(){var a=['a','bb'];function pick(x){return a[x];}var s=0;for(var i=0;i<301.5;i++)s+=pick(i&1).length;return i+'|'+s;})())",false},
        {L"fractional sum",LR"((function(){var a=['a','bb'];function pick(x){return a[x];}var s=.5;for(var i=0;i<300;i++)s+=pick(i&1).length;return i+'|'+s;})())",false},
        {L"unsafe integer sum",LR"((function(){var a=['a','bb'];function pick(x){return a[x];}var s=9007199254740992;for(var i=0;i<300;i++)s+=pick(i&1).length;return i+'|'+s;})())",false},
        {L"accessor array",LR"((function(){var hits=0,a=['a','bb'];Object.defineProperty(a,'0',{get:function(){hits++;return 'ccc';}});function pick(x){return a[x];}var s=0;for(var i=0;i<300;i++)s+=pick(i&1).length;return i+'|'+s+'|'+hits;})())",false},
        {L"proxy array",LR"((function(){var hits=0,a=new Proxy(['a','bb'],{get:function(t,k){hits++;return t[k];}});function pick(x){return a[x];}var s=0;for(var i=0;i<300;i++)s+=pick(i&1).length;return i+'|'+s+'|'+hits;})())",false},
        {L"mutating getter",LR"((function(){var hits=0,a=['a','bb'];function table(){hits++;return a;}function pick(x){return table()[x];}var s=0;for(var i=0;i<300;i++)s+=pick(i&1).length;return i+'|'+s+'|'+hits;})())",false},
        {L"default parameter side effect",LR"((function(){var hits=0,a=['a','bb'];function pick(x,y=++hits){return a[x];}var s=0;for(var i=0;i<300;i++)s+=pick(i&1).length;return i+'|'+s+'|'+hits;})())",false},
        {L"conditional break",LR"((function(){var a=['a','bb'];function pick(x){return a[x];}var s=0;for(var i=0;i<300;i++){s+=pick(i&1).length;if(i==150)break;}return i+'|'+s;})())",false},
        {L"interleaved mutation",LR"((function(){var a=['a','bb'];function pick(x){return a[x];}var s=0;for(var i=0;i<300;i++){s+=pick(i&1).length;a[0]+='x';}return i+'|'+s;})())",false},
        {L"nonperiodic mask",LR"((function(){var a=['a','bb','ccc','dddd','eeeee','ffffff'];function pick(x){return a[x];}var s=0;for(var i=0;i<300;i++)s+=pick(i&5).length;return i+'|'+s;})())",false},
        {L"regrouping cancellation guard",LR"((function(){var a=['a','bb'];function pick(x){return a[x];}var s=-9007199254740991;for(var i=0;i<300;i++)s+=pick(i&1).length;return i+'|'+s;})())",true},
    };
    unsigned failures=0;
    for(const auto& test:cases){
        std::wstring answers[2],errors[2];bool success[2]{};bool reduced=false;
        for(unsigned mode=0;mode<2;++mode){
            InstructionLimitScope limit(!mode);
            Document document;document.Parse(L"<html><body></body></html>");JavaScriptRuntime runtime(document);
            runtime.SetCompatibilityBridgeEnabled(false);runtime.SetJitCompilationThreshold(mode?64:0);
            success[mode]=runtime.Execute(std::wstring(L"return ")+test.script+L";",answers+mode,errors+mode);
            if(mode)reduced=runtime.DiagnosticsJson().find(L"\"tableReductionIterations\":0,")==std::wstring::npos;
        }
        const bool ok=success[0]&&success[1]&&answers[0]==answers[1]&&reduced==test.reduced;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" result="<<answers[1]<<L" reduced="<<reduced<<L" error="<<errors[1]<<L'\n';if(!ok)++failures;
    }
    const wchar_t* numericCases[]={
        LR"((function(){function f(x,scratch){scratch=3;return x+scratch;}var s=0;for(var i=0;i<600;i++)s+=f(i);return s;})())",
        LR"((function(){function f(x,scratch){scratch=x*3;return scratch>>>1;}var s=0;for(var i=0;i<600;i++)s+=f(i,{valueOf:function(){throw Error('unused');}});return s;})())",
        LR"((function(){function f(x,y){if(x)y=3;return y+1;}var s=0;for(var i=0;i<600;i++)s+=f(i&1,2);return s+'|'+f(0);})())",
        LR"((function(){var i=2;return JSON.stringify([i++,++i,i--,--i,i,1/(-0),(-0)+0]);})())",
        LR"((function(){var s=0;for(var i=0;i<600;i++)s+=((i*17+31)&255)>>>0;return s;})())",
        LR"((function(){var s='5';return JSON.stringify([s+1,s-1,s<6,s===5,NaN===0,Infinity*0]);})())",
        LR"((function(){var i=4,hits=0,obj={valueOf:function(){hits++;i=7;return 3;}};return (i+obj)+'|'+i+'|'+hits;})())",
        LR"((function(){var v=1;function f(n){var a=2;if(n){let a=3;v+=a;}return a+v;}return f(1)+'|'+f(0)+'|'+f(1);})())",
    };
    for(size_t index=0;index<std::size(numericCases);++index){
        std::wstring answers[2],errors[2];bool success[2]{};
        for(unsigned mode=0;mode<2;++mode){InstructionLimitScope limit(!mode);Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(mode?64:0);
            success[mode]=runtime.Execute(std::wstring(L"return ")+numericCases[index]+L";",answers+mode,errors+mode);}
        const bool ok=success[0]&&success[1]&&answers[0]==answers[1];std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"numeric guards/fusion "<<index<<L" result="<<answers[1]<<L" error="<<errors[1]<<L'\n';if(!ok)++failures;
    }
    {
        FastMap<std::wstring,int> values;values[L"a"]=1;values[L"b"]=2;size_t slot=std::numeric_limits<size_t>::max();
        bool ok=values.find_cached(L"b",slot)&&*values.find_cached(L"b",slot)==2;
        values.erase(L"a");for(int i=0;i<100;++i)values[std::to_wstring(i)]=i;
        ok=ok&&values.find_cached(L"b",slot)&&*values.find_cached(L"b",slot)==2;values.clear();values[L"b"]=9;
        ok=ok&&values.find_cached(L"b",slot)&&*values.find_cached(L"b",slot)==9;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"binding cache survives compaction, allocation and clear\n";if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);std::wstring answer,error;
        const bool ok=runtime.Execute(L"return JSON.stringify('\\u0000\\t\\r\\n\\b\\f\"\\\\');",&answer,&error)&&answer==L"\"\\u0000\\t\\r\\n\\b\\f\\\"\\\\\"";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"JSON control escaping\n";if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);std::wstring answer,error;
        const bool ok=runtime.Execute(L"return JSON.stringify([NaN,Infinity,-Infinity,'\\ud800','\\udc00']);",&answer,&error)&&answer==L"[null,null,null,\"\\ud800\",\"\\udc00\"]";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"JSON nonfinite numbers and lone surrogates\n";if(!ok)++failures;
        const bool numbers=runtime.Execute(L"return [9007199254740991,1.23e20,1.23e21,1.23e-20,1e-6,1e-7,0.30000000000000004].map(String).join('|');",&answer,&error)&&answer==L"9007199254740991|123000000000000000000|1.23e+21|1.23e-20|0.000001|1e-7|0.30000000000000004";
        std::wcout<<(numbers?L"PASS ":L"FAIL ")<<L"shortest number strings and exponent digits result="<<answer<<L'\n';if(!numbers)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);std::wstring answer,error;
        const auto source=Quote(std::wstring(6000,L' ')+L"var cachedX=typeof cachedX==='undefined'?1:cachedX+1; /x/g;");
        bool ok=runtime.Execute(L"var code="+source+L";var first=eval(code);first.marker=9;var second=eval(code);return cachedX+'|'+(first===second)+'|'+typeof second.marker;",&answer,&error)&&answer==L"2|false|undefined";
        ok=ok&&runtime.DiagnosticsJson().find(L"\"evalCompileCacheHits\":1,")!=std::wstring::npos;
        ok=ok&&runtime.Execute(L"return eval('\"use strict\";'+code)===second;",&answer,&error)&&answer==L"false";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"cached eval keeps environments and regexp literals fresh error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(64);
        runtime.SetExecutionYieldHandler([&]{const bool reduced=runtime.DiagnosticsJson().find(L"\"tableReductionCalls\":0,")==std::wstring::npos;if(!reduced)Sleep(40);return !reduced;});std::wstring answer,error;
        const bool ok=!runtime.Execute(L"var a=['x'];function p(x){return a[x];}(function(){var s=0;for(var i=0;i<2147483647;i++)s+=p(i&0).length;return s;})();",&answer,&error)&&error.find(L"interrupted")!=std::wstring::npos;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"host cancellation between reduction batches\n";if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(1);std::wstring answer,error;
        runtime.Execute(L"function numericLoop(n){var x=0;while(x<n)x++;return x;}return numericLoop(1);",&answer,&error);
        unsigned yields=0;runtime.SetExecutionYieldHandler([&]{return ++yields<2;});
        const bool ok=!runtime.Execute(L"return numericLoop(1000000000000);",&answer,&error)&&error.find(L"interrupted")!=std::wstring::npos&&runtime.GetJitStatistics().nativeCalls>=2;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"host cancellation inside native JIT loop error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(1);std::wstring answer,error;
        runtime.Execute(L"function outerLoop(n){var x=0;while(x<n)x++;return x;}function nestedLoop(n){var s=0;for(var i=0;i<n;i++)s+=i;return s;}outerLoop(1);nestedLoop(64);",&answer,&error);
        unsigned yields=0;bool nestedOk=true;
        runtime.SetExecutionYieldHandler([&]{if(++yields==1)Sleep(40);std::wstring nestedAnswer,nestedError;nestedOk=runtime.Execute(L"return nestedLoop(64);",&nestedAnswer,&nestedError)&&nestedAnswer==L"2016"&&nestedOk;return true;});
        const bool ok=runtime.Execute(L"return outerLoop(20000000);",&answer,&error)&&answer==L"20000000"&&nestedOk&&yields>=2;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"JIT scratch frames survive host reentry yields="<<yields<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    std::wcout<<L"Standalone regressions: "<<failures<<L" failure(s)\n";return failures?1:0;
}

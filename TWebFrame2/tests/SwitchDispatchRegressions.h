#pragma once

static unsigned RunSwitchDispatchRegressions(){
    struct Case {const wchar_t* name;const wchar_t* script;const wchar_t* expected;bool dispatch;};
    const Case cases[]={
        {L"literal switch preserves strict types and avoids coercion",LR"JS(
            function f(x){switch(x){case 0:return 'zero';case false:return 'false';case '0':return 'string';case null:return 'null';case 1:return 'one';case true:return 'true';case 2:return 'two';case 3:return 'three';default:return 'default';}}
            return [f(-0),f(false),f('0'),f(null),f(1),f(true),f(2),f(3),f(NaN),f(undefined),f({valueOf:function(){throw 'coercion';}})].join('|');
        )JS",L"zero|false|string|null|one|true|two|three|default|default|default",true},
        {L"literal switch keeps first duplicate and source fallthrough",LR"JS(
            function f(x){var s='';switch(x){case -0:s+='a';case 0:s+='b';break;case 1:s+='c';default:s+='d';case 2:s+='e';break;case 3:s+='f';case 4:s+='g';case 5:s+='h';case 6:s+='i';}return s;}
            return [f(0),f(1),f(9),f(2),f(3)].join('|');
        )JS",L"ab|cde|de|e|fghi",true},
        {L"literal strings preserve UTF-16 and template literals",LR"JS(
            function f(x){switch(x){case 'a\u0000b':return 1;case '\ud800':return 2;case `word`:return 3;case 'x':return 4;case 'y':return 5;case 'z':return 6;case 'p':return 7;case 'q':return 8;default:return 9;}}
            return [f('a\u0000b'),f('\ud800'),f('word'),f('other')].join('|');
        )JS",L"1|2|3|9",true},
        {L"computed cases keep lazy evaluation across default",LR"JS(
            var seen=[];function mark(v){seen.push(v);return v;}
            function f(x){switch(x){case mark(1):return 'a';default:return 'd';case mark(2):return 'b';case 3:return 'c';case 4:return 'e';case 5:return 'f';case 6:return 'g';case 7:return 'h';}}
            var a=f(1),b=f(2),c=f(8);return a+b+c+'|'+seen.join(',');
        )JS",L"abd|1,1,2,1,2",false},
        {L"computed case exceptions retain order",LR"JS(
            function boom(){throw 'case-error';}function f(x){switch(x){case 1:return 'first';case boom():return 'never';case 2:return 'two';case 3:return 'three';case 4:return 'four';case 5:return 'five';case 6:return 'six';case 7:return 'seven';}}
            var s=f(1);try{f(2);}catch(e){s+='|'+e;}return s;
        )JS",L"first|case-error",false},
        {L"dispatch unwinds labeled continue, break and finally",LR"JS(
            var s='';outer:for(var i=0;i<3;i++){try{switch(i){case 0:s+='a';continue outer;case 1:s+='b';break;case 2:s+='c';break outer;case 3:break;case 4:break;case 5:break;case 6:break;case 7:break;}}finally{s+='f';}s+='x';}return s;
        )JS",L"afbfxcf",true},
        {L"dispatch keeps discriminant single evaluation and nested tables",LR"JS(
            var reads=0;function f(){switch(++reads){case 1:switch('z'){case 'a':return 0;case 'b':return 0;case 'c':return 0;case 'd':return 0;case 'e':return 0;case 'f':return 0;case 'g':return 0;case 'z':return 7;}case 2:return 2;case 3:return 3;case 4:return 4;case 5:return 5;case 6:return 6;case 7:return 7;case 8:return 8;}}return f()+'|'+reads;
        )JS",L"7|1",true},
        {L"no default falls past switch with unchanged state",LR"JS(
            var s=17;switch(99){case 1:s=1;break;case 2:s=2;break;case 3:s=3;break;case 4:s=4;break;case 5:s=5;break;case 6:s=6;break;case 7:s=7;break;case 8:s=8;break;}return s;
        )JS",L"17",true},
        {L"interpolated case template keeps observable evaluation",LR"JS(
            var n=0;function f(x){switch(x){case `k${++n}`:return 'hit';case 'a':return 'a';case 'b':return 'b';case 'c':return 'c';case 'd':return 'd';case 'e':return 'e';case 'f':return 'f';case 'g':return 'g';default:return 'miss';}}return f('k1')+'|'+f('k2')+'|'+n;
        )JS",L"hit|hit|2",false},
    };
    unsigned failures=0;
    for(const auto& test:cases)for(const size_t threshold:{size_t(0),size_t(16)}){
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(threshold);
        std::wstring answer,error;
        const bool ran=runtime.Execute(test.script,&answer,&error);
        const bool dispatched=runtime.DiagnosticsJson().find(L"\"switchDispatches\":0,")==std::wstring::npos;
        const bool ok=ran&&answer==test.expected&&dispatched==test.dispatch;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" jit="<<threshold<<L" result="<<answer<<L" error="<<error<<L'\n';
        if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);
        const auto code=Quote(std::wstring(6000,L' ')+L"switch(input){case 0:answer='a';break;case 1:answer='b';break;case 2:answer='c';break;case 3:answer='d';break;case 4:answer='e';break;case 5:answer='f';break;case 6:answer='g';break;case 7:answer='h';break;default:answer='z';}");
        std::wstring answer,error;
        const bool ok=runtime.Execute(L"var input=2,answer,code="+code+L";eval(code);var first=answer;input=7;eval(code);return first+'|'+answer;",&answer,&error)&&answer==L"c|h"&&
            runtime.DiagnosticsJson().find(L"\"evalCompileCacheHits\":1,")!=std::wstring::npos;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"cached eval retains switch targets and reads current bindings error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);
        unsigned yields=0;runtime.SetExecutionYieldHandler([&]{return ++yields<2;});
        std::wstring answer,error;
        const bool ok=!runtime.Execute(L"var i=0;while(true){switch(i&7){case 0:i++;break;case 1:i++;break;case 2:i++;break;case 3:i++;break;case 4:i++;break;case 5:i++;break;case 6:i++;break;case 7:i++;break;}}",&answer,&error)&&
            error.find(L"interrupted")!=std::wstring::npos&&yields==2;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"literal switch loop retains host cancellation error="<<error<<L'\n';if(!ok)++failures;
    }
    return failures;
}

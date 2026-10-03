#pragma once

static unsigned RunStringAppendRegressions(){
    struct Case {const wchar_t* name;const wchar_t* script;const wchar_t* expected;bool reuse;};
    const Case cases[]={
        {L"private string accumulation reuses backing storage",L"var s='abcd'.repeat(10);for(var i=0;i<100;i++)s+='x';return s.length+'|'+s.slice(0,4)+'|'+s.slice(-4);",L"140|abcd|xxxx",true},
        {L"saved primitive strings stay immutable",L"var s='abcd'.repeat(10),saved=s;for(var i=0;i<100;i++)s+='x';return saved.length+'|'+saved.slice(-4)+'|'+s.length;",L"40|abcd|140",true},
        {L"array and property aliases retain old strings",L"var s='abcd'.repeat(10),a=[s],o={value:s};s+='x';s+='y';return a[0].length+'|'+o.value.length+'|'+s.slice(-4);",L"40|40|cdxy",true},
        {L"concatenation assigned to a different binding preserves source",L"var a='abcd'.repeat(10),b;b=a+'y';return a.length+'|'+b.length;",L"40|41",false},
        {L"self concatenation preserves overlapping operands",L"var s='abcd'.repeat(10);s+=s;return s.length+'|'+s.slice(38,44);",L"80|cdabcd",false},
        {L"right operand conversion can retain the old string",L"var s='abcd'.repeat(10),saved;var rhs={valueOf:function(){saved=s;return 'x';}};s+=rhs;return saved.length+'|'+s.length;",L"40|41",false},
        {L"right operand conversion can replace the binding",L"var s='abcd'.repeat(10);var rhs={valueOf:function(){s='new';return 'x';}};s+=rhs;return s.length+'|'+s.slice(-4);",L"41|bcdx",false},
        {L"constant assignment throws without changing its string",L"const s='abcd'.repeat(10);var name;try{s+='x';}catch(e){name=e.name;}return name+'|'+s.length;",L"TypeError|40",false},
        {L"throwing primitive conversion leaves the binding untouched",L"var s='abcd'.repeat(10);try{s+={valueOf:function(){throw 'conversion';}};}catch(e){}return s.length;",L"40",false},
        {L"getter and setter keep separate observable evaluation",L"var old='abcd'.repeat(10),saved=old,calls='',o={get value(){calls+='g';return old;},set value(v){calls+='s';old=v;}};o.value+='x';return calls+'|'+saved.length+'|'+old.length;",L"gs|40|41",false},
        {L"UTF-16 accumulation retains NUL and lone surrogates",LR"JS(var s='abcd'.repeat(10);s+='\u0000\ud800\udfff';s+='Z';return s.length+'|'+s.charCodeAt(40)+'|'+s.charCodeAt(41)+'|'+s.charCodeAt(42)+'|'+s.charCodeAt(43);)JS",L"44|0|55296|57343|90",true},
        {L"numeric operands retain addition and string coercion",L"var s='abcd'.repeat(10);s+=7;var n=3;n+=4;return s.slice(-4)+'|'+n;",L"bcd7|7",false},
        {L"returned assignment values retain their original snapshot",L"var s='abcd'.repeat(10),saved=(s+='x');s+='y';return saved.length+'|'+saved.slice(-4)+'|'+s.length;",L"41|bcdx|42",true},
    };
    unsigned failures=0;
    for(const auto& test:cases)for(const size_t threshold:{size_t(0),size_t(16)}){
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(threshold);
        std::wstring answer,error;
        const bool ran=runtime.Execute(L"return (function(){"+std::wstring(test.script)+L"})();",&answer,&error);
        const bool reused=runtime.DiagnosticsJson().find(L"\"stringAppendReuses\":0,")==std::wstring::npos;
        const bool ok=ran&&answer==test.expected&&reused==test.reuse;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" jit="<<threshold<<L" reused="<<reused<<L" result="<<answer<<L" error="<<error<<L'\n';
        if(!ok)++failures;
    }
    return failures;
}

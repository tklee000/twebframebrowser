#pragma once

static unsigned RunStringTransformRegressions() {
    const std::wstring convert=LR"JS(
        function convert(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(((c&255)-32-i%65535+65535)%255)));return a.join('')+'|'+i+'|'+isNaN(c);}
    )JS";
    struct Case {const wchar_t* name;const wchar_t* setup;const wchar_t* invoke;bool optimized;bool packed=false;};
    const Case cases[]={
        {L"UTF-16 character transform",L"",LR"JS(return convert('a\u0000\ud800\udfffZ'.repeat(100));)JS",true},
        {L"replaced push is observable",L"var hits=0,push=Array.prototype.push;Array.prototype.push=function(v){hits++;return push.call(this,v);};",L"return convert('ab'.repeat(150))+'|'+hits;",false},
        {L"replaced character reader is observable",L"var hits=0,read=String.prototype.charCodeAt;String.prototype.charCodeAt=function(i){hits++;return read.call(this,i);};",L"return convert('ab'.repeat(150))+'|'+hits;",false},
        {L"replaced isNaN is observable",L"var hits=0,check=isNaN;isNaN=function(v){hits++;return check(v);};",L"return convert('ab'.repeat(150))+'|'+hits;",false},
        {L"fromCharCode accessor is observable",L"var hits=0,make=String.fromCharCode;Object.defineProperty(String,'fromCharCode',{get:function(){hits++;return make;}});",L"return convert('ab'.repeat(150))+'|'+hits;",false},
        {L"inherited indexed setter prevents batching",L"var hits=0;Object.defineProperty(Array.prototype,'200',{set:function(v){hits++;},configurable:true});",L"return convert('ab'.repeat(150))+'|'+hits;",false},
        {L"cached plan rechecks intrinsics",L"",L"var first=convert('ab'.repeat(150));var hits=0,make=String.fromCharCode;String.fromCharCode=function(v){hits++;return make(v+1);};return first+'|'+convert('ab'.repeat(150))+'|'+hits;",true},
        {L"proxy destination retains traps",L"convert=function(input){var hits=0,a=new Proxy([],{set:function(t,k,v){hits++;t[k]=v;return true;}}),i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('')+'|'+hits;};",L"return convert('ab'.repeat(150));",false},
        {L"fractional index uses ordinary ToInteger",L"convert=function(input){var a=[],i=.5,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('')+'|'+i;};",L"return convert('ab'.repeat(150));",false},
        {L"break remains observable",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c))){if(i==150)break;}return a.join('')+'|'+i;};",L"return convert('ab'.repeat(150));",false},
        {L"pure helpers and constant table lookup",L"var table=['charCodeAt','push','fromCharCode'];function key(i){return table[i];}var ops={add:function(a,b){return a+b;},mod:function(a,b){return a%b;},check:function(f,x){return f(x);}};convert=function(input){var a=[],i=-1,c;for(;!ops.check(isNaN,c=input[key(0)](++i));a[key(1)](String[key(2)](ops.mod(ops.add(c-i%65535,65535),255))));return a.join('')+'|'+i+'|'+isNaN(c);};",L"return convert('ab'.repeat(150));",true},
        {L"proxy helper does not lose reads",L"var hits=0,ops=new Proxy({plus:function(a,b){return a+b;}},{get:function(t,k){hits++;return t[k];}});convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(ops.plus(c,1))));return a.join('')+'|'+hits;};",L"return convert('ab'.repeat(150));",false},
        {L"input and destination aliases preserve order",L"convert=function(input){var a=[],i=-1,c,observed=a;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c+observed.length)));return a.join('');};",L"return convert('ab'.repeat(150));",false},
        {L"helper captured counter stays live",L"convert=function(input){var a=[],i=-1,c;function mix(v){return v+i;}for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(mix(c))));return a.join('');};",L"return convert('ab'.repeat(150));",false},
        {L"helper captured character stays live",L"convert=function(input){var a=[],i=-1,c;function mix(v){return v+c;}for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(mix(i))));return a.join('');};",L"return convert('ab'.repeat(150));",false},
        {L"helper this retains its call receiver",L"convert=function(input){var a=[],i=-1,c;var ops={mix:function(v){return v+this;},valueOf:function(){return 13;}};for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(ops.mix(c))));return a.join('');};",L"return convert.call(32,'ab'.repeat(150));",false},
        {L"private array returns packed UTF-16",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('');};",LR"JS(return convert('a\u0000\ud800\udfffZ'.repeat(100));)JS",true,true},
        {L"empty template separator returns packed string",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join(``);};",L"return convert('ab'.repeat(150));",true,true},
        {L"escaped array remains populated",L"var escaped;convert=function(input){var a=[],i=-1,c;escaped=a;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('');};",L"return convert('ab'.repeat(150))+'|'+escaped.length+'|'+escaped[299];",true},
        {L"closure retains complete array",L"var escaped;convert=function(input){var a=[],i=-1,c;escaped=function(){return a;};for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('');};",L"return convert('ab'.repeat(150))+'|'+escaped().length+'|'+escaped()[299];",true},
        {L"direct eval retains complete array",L"var escaped;convert=function(input){var a=[],i=-1,c;eval('escaped=a');for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('');};",L"return convert('ab'.repeat(150))+'|'+escaped.length;",true},
        {L"replaced join receives complete array",L"var hits=0,join=Array.prototype.join;Array.prototype.join=function(s){hits=this.length;return join.call(this,s);};convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('');};",L"return convert('ab'.repeat(150))+'|'+hits;",true},
        {L"inherited join accessor remains observable",L"var hits=0,join=Array.prototype.join;Object.defineProperty(Array.prototype,'join',{get:function(){hits=this.length;return join;}});convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('');};",L"return convert('ab'.repeat(150))+'|'+hits;",true},
        {L"nonempty join separator stays ordinary",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join(':');};",L"return convert('ab'.repeat(150));",true},
        {L"large and nonfinite character conversion",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c*1e24)));return a.join('');};",L"return convert('ab'.repeat(150));",true,true},
        {L"negative signed integer remainder",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode((c-100)*3%255)));return a.join('');};",L"return convert('ab'.repeat(150));",true,true},
        {L"integer overflow retains Number arithmetic",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c*1000000000)));return a.join('');};",L"return convert('ab'.repeat(150));",true,true},
        {L"zero divisor retains NaN",L"convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c%(c-97))));return a.join('');};",L"return convert('ab'.repeat(150));",true,true},
        {L"division retains negative zero",L"var negative=-0;convert=function(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(1/(negative*c))));return a.join('');};",L"return convert('ab'.repeat(150));",true,true},
    };
    unsigned failures=0;
    for(const auto& test:cases){
        std::wstring expected,actual,error;bool referenceOk=false,optimizedOk=false;std::wstring diagnostics;
        for(bool reference:{true,false}){
            Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);
            runtime.SetJitCompilationThreshold(reference?0:16);
            std::wstring value;
            const bool ok=runtime.Execute(convert+test.setup+test.invoke,&value,&error);
            if(reference){referenceOk=ok;expected=value;}else{optimizedOk=ok;actual=value;diagnostics=runtime.DiagnosticsJson();}
        }
        const bool batched=diagnostics.find(L"\"stringTransformCalls\":0,")==std::wstring::npos;
        const bool packed=diagnostics.find(L"\"packedStringTransformCalls\":0,")==std::wstring::npos;
        const bool ok=referenceOk&&optimizedOk&&expected==actual&&batched==test.optimized&&packed==test.packed;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<test.name<<L" batched="<<batched<<L" packed="<<packed<<L" equal="<<(expected==actual)<<L" error="<<error<<L'\n';
        if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        unsigned yields=0;runtime.SetExecutionYieldHandler([&]{Sleep(40);return ++yields<2;});
        std::wstring answer,error;
        const bool ok=!runtime.Execute(L"function convert(input){var a=[],i=-1,c;for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode(c)));return a.join('');}return convert('ab'.repeat(1000000));",&answer,&error)&&error.find(L"interrupted")!=std::wstring::npos&&yields>1;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"packed string retains host cancellation\n";if(!ok)++failures;
    }
    {
        Document document;JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        std::wstring answer,error;
        // Pure numeric work keeps the packed path active long enough for a
        // host callback even on a fast build; all masked terms equal zero.
        std::wstring transform=L"c";
        for(unsigned term=0;term<8;++term)transform+=L"+((i*"+std::to_wstring(31+term*2)+L"^i*"+std::to_wstring(17+term*2)+L")&0)";
        runtime.Execute(L"var state,generation=0;function convert(input){var a=[],i=-1,c;generation++;state=function(){return i;};for(;!isNaN(c=input.charCodeAt(++i));a.push(String.fromCharCode("+transform+L")));return a.join('');}convert('a'.repeat(300));",nullptr,&error);
        constexpr int reentryLength=256000;
        unsigned yields=0;int prefix=-1;bool nestedOk=true;
        runtime.SetExecutionYieldHandler([&]{
            ++yields;
            if(prefix<0){std::wstring snapshot,nestedError;nestedOk=runtime.Execute(L"return generation+'|'+state();",&snapshot,&nestedError);
                const auto separator=snapshot.find(L'|');const int cursor=separator==std::wstring::npos?-1:_wtoi(snapshot.substr(separator+1).c_str());
                if(snapshot.rfind(L"2|",0)==0&&cursor>=0&&cursor<reentryLength){nestedOk=nestedOk&&runtime.Execute(L"var make=String.fromCharCode;String.fromCharCode=function(v){return make(v+1);};",nullptr,&nestedError);prefix=cursor+1;}}
            // Make the next safepoint due even when optimized batches finish
            // faster than the host's ordinary 32 ms callback cadence.
            if(prefix<0)Sleep(40);
            return true;
        });
        const bool executed=runtime.Execute(L"return convert('a'.repeat("+std::to_wstring(reentryLength)+L"));",&answer,&error);
        const bool ok=executed&&nestedOk&&prefix>=0&&prefix<reentryLength&&answer==std::wstring(prefix,L'a')+std::wstring(reentryLength-prefix,L'b');
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"packed string deoptimizes after host reentry prefix="<<prefix<<L" yields="<<yields<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;JavaScriptRuntime runtime(document);std::wstring answer,error;
        const bool ok=runtime.Execute(LR"JS(
            var a=['a','b','c'];var s={toString:function(){a[1]='X';return ':';}};
            var result=[a.join(s)];
            a=['a',,'c'];Object.defineProperty(Array.prototype,'1',{get:function(){return 'P';},configurable:true});var sparse=a.join('');delete Array.prototype[1];result.push(sparse);
            a=['a','b'];Object.defineProperty(a,'1',{get:function(){return 'G';}});result.push(a.join(''));
            result.push(Array.prototype.join.call({0:'x',1:'y',length:2},'-'));
            result.push([null,undefined,3,true].join(':'));
            a=['a','b'];result.push(a.join({toString:function(){a.length=1;return ':';}}));
            try{['a'].join(Symbol('s'));result.push('missing error');}catch(e){result.push(e.name);}
            result.push(String.fromCharCode(1e24,-1.75,Infinity,NaN).split('').map(function(c){return c.charCodeAt(0);}).join(','));
            return result.join('|');
        )JS",&answer,&error)&&answer==L"a:X:c|aPc|aG|x-y|::3:true|a:|TypeError|0,65535,0,0";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"join coercion accessors sparse arrays and generic receivers result="<<answer<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;JavaScriptRuntime runtime(document);std::wstring answer,error;
        const bool ok=runtime.Execute(LR"JS(
            var result=[atob(' YW\tJj\n'),atob('YQ=='),atob('YQ'),btoa('\x00\xff'),atob({toString:function(){return 'YQ==';}}),btoa({toString:function(){return 'a';}})];
            for(var input of ['A','Y===','YWJj=','YQ==\u00a0','YQ\u0100']){try{atob(input);result.push('missing error');}catch(e){result.push(e.name);}}
            try{btoa('\u0100');result.push('missing error');}catch(e){result.push(e.name);}
            return result.join('|');
        )JS",&answer,&error)&&answer==L"abc|a|a|AP8=|a|YQ==|InvalidCharacterError|InvalidCharacterError|InvalidCharacterError|InvalidCharacterError|InvalidCharacterError|InvalidCharacterError";
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"base64 whitespace padding byte range and coercion result="<<answer<<L" error="<<error<<L'\n';if(!ok)++failures;
    }
    {
        Document document;document.Parse(L"<html></html>");JavaScriptRuntime runtime(document);runtime.SetJitCompilationThreshold(16);
        unsigned yields=0;runtime.SetExecutionYieldHandler([&]{if(++yields==1)Sleep(40);return runtime.DiagnosticsJson().find(L"\"stringTransformCalls\":0,")!=std::wstring::npos;});
        std::wstring answer,error;
        const bool ok=!runtime.Execute(convert+L"return convert('ab'.repeat(1000000));",&answer,&error)&&error.find(L"interrupted")!=std::wstring::npos&&yields>1;
        std::wcout<<(ok?L"PASS ":L"FAIL ")<<L"character batches retain host cancellation\n";if(!ok)++failures;
    }
    return failures;
}

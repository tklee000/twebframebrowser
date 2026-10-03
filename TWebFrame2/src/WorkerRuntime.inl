// Internal Worker transport is produced by Json(), so decode data directly
// instead of compiling another JavaScript program for each message. In
// particular __proto__ is an own data property, not object-literal syntax.
Value ParseWorkerMessage(const std::wstring& text){
    size_t cursor=0;
    const auto whitespace=[&]{while(cursor<text.size()&&(text[cursor]==L' '||text[cursor]==L'\t'||text[cursor]==L'\r'||text[cursor]==L'\n'))++cursor;};
    const auto fail=[&]()->void{throw JavaScriptException{ErrorValue(L"DataCloneError",L"Invalid Worker message")};};
    const auto string=[&]()->std::wstring{
        if(cursor>=text.size()||text[cursor++]!=L'"')fail();
        std::wstring result;size_t begin=cursor;
        while(cursor<text.size()){
            const auto character=text[cursor++];
            if(character==L'"'){result.append(text,begin,cursor-begin-1);return result;}
            if(character<32)fail();
            if(character!=L'\\')continue;
            result.append(text,begin,cursor-begin-1);if(cursor>=text.size())fail();
            switch(text[cursor++]){
            case L'"':result+=L'"';break;case L'\\':result+=L'\\';break;case L'/':result+=L'/';break;
            case L'b':result+=L'\b';break;case L'f':result+=L'\f';break;case L'n':result+=L'\n';break;case L'r':result+=L'\r';break;case L't':result+=L'\t';break;
            case L'u':{
                unsigned code=0;if(text.size()-cursor<4)fail();
                for(unsigned at=0;at<4;++at){const auto digit=text[cursor++];unsigned number=16;
                    if(digit>=L'0'&&digit<=L'9')number=digit-L'0';else if(digit>=L'a'&&digit<=L'f')number=digit-L'a'+10;else if(digit>=L'A'&&digit<=L'F')number=digit-L'A'+10;
                    if(number>=16)fail();code=code*16+number;
                }result+=static_cast<wchar_t>(code);break;
            }
            default:fail();
            }begin=cursor;
        }fail();return {};
    };
    const auto parse=[&](const auto& self,unsigned depth)->Value{
        whitespace();if(depth>256||cursor>=text.size())fail();
        const auto first=text[cursor];
        if(first==L'"')return Value::String(string());
        if(first==L'{'||first==L'['){
            const bool array=first==L'[';const auto close=array?L']':L'}';++cursor;
            auto result=array?ArrayValue({}):ObjectValue(ObjectKind::Plain);whitespace();
            if(cursor<text.size()&&text[cursor]==close){++cursor;return result;}
            for(;;){
                std::wstring key;if(!array){whitespace();key=string();whitespace();if(cursor>=text.size()||text[cursor++]!=L':')fail();}
                auto value=self(self,depth+1);if(array)result.object->items.push_back(std::move(value));else result.object->props[key]=std::move(value);
                whitespace();if(cursor>=text.size())fail();const auto separator=text[cursor++];if(separator==close)return result;if(separator!=L',')fail();
            }
        }
        for(const auto& literal:std::initializer_list<std::pair<const wchar_t*,Value>>{{L"true",Value::Bool(true)},{L"false",Value::Bool(false)},{L"null",Value::Null()},{L"undefined",Value::Undefined()}}){
            const auto length=std::wcslen(literal.first);if(text.compare(cursor,length,literal.first)==0){cursor+=length;return literal.second;}
        }
        const auto start=cursor;if(text[cursor]==L'-')++cursor;if(cursor>=text.size())fail();
        if(text[cursor]==L'0')++cursor;
        else{if(text[cursor]<L'1'||text[cursor]>L'9')fail();while(cursor<text.size()&&text[cursor]>=L'0'&&text[cursor]<=L'9')++cursor;}
        if(cursor<text.size()&&text[cursor]==L'.'){++cursor;const auto digits=cursor;while(cursor<text.size()&&text[cursor]>=L'0'&&text[cursor]<=L'9')++cursor;if(cursor==digits)fail();}
        if(cursor<text.size()&&(text[cursor]==L'e'||text[cursor]==L'E')){++cursor;if(cursor<text.size()&&(text[cursor]==L'+'||text[cursor]==L'-'))++cursor;const auto digits=cursor;while(cursor<text.size()&&text[cursor]>=L'0'&&text[cursor]<=L'9')++cursor;if(cursor==digits)fail();}
        wchar_t* end=nullptr;const auto number=std::wcstod(text.c_str()+start,&end);if(end!=text.c_str()+cursor)fail();return Value::Number(number);
    };
    auto result=parse(parse,0);whitespace();if(cursor!=text.size())fail();return result;
}

bool EvalCacheConstantsSafe(const Chunk& chunk,unsigned depth=0){
    if(depth>16)return false;
    for(const auto& value:chunk.constants){
        // RunFrame materializes RegExp literals from their immutable pattern
        // and flags on every execution, including nested cached functions.
        if(value.type==Value::Type::Object&&(!value.object||value.object->kind!=ObjectKind::RegExp))return false;
        if(value.type==Value::Type::Function||value.type==Value::Type::Native||value.type==Value::Type::Reference)return false;
    }
    for(const auto& expression:chunk.embeddedExpressions)if(expression&&!EvalCacheConstantsSafe(*expression,depth+1))return false;
    const auto prototypeSafe=[&](int index){
        if(index<0||static_cast<size_t>(index)>=module->prototypes.size()||!module->prototypes[index])return false;
        const auto& prototype=*module->prototypes[index];
        if(!EvalCacheConstantsSafe(prototype.chunk,depth+1))return false;
        for(const auto& parameter:prototype.parameterDefaults)if(parameter&&!EvalCacheConstantsSafe(*parameter,depth+1))return false;
        for(const auto& binding:prototype.bindings)if(binding.defaultValue&&!EvalCacheConstantsSafe(*binding.defaultValue,depth+1))return false;
        return true;
    };
    for(const auto& instruction:chunk.code)if(instruction.op==Op::MakeFunction&&!prototypeSafe(instruction.argument))return false;
    for(const auto& declaration:chunk.functionDeclarations)if(!prototypeSafe(declaration.prototype))return false;
    return true;
}

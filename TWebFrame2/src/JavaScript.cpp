#include "JavaScript.h"

// The interpreter currently uses native frames for nested JavaScript calls.
// Keep enough main-thread stack for common library call chains without the
// previous 16 MiB reservation, which multiplied across every host worker.
#if defined(_MSC_VER)
#pragma comment(linker, "/STACK:8388608")
#endif

#include "Canvas.h"
#include "CSS.h"
#include "NumericParser.h"
#include "RasterImage.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <deque>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TWebFrame::Internal {
namespace {

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(std::string_view value) {
    if(value.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(count<=0)return {};
    std::wstring result(static_cast<size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),count);
    return result;
}

int HexDigitValue(wchar_t character) {
    if(character>=L'0'&&character<=L'9')return character-L'0';
    if(character>=L'a'&&character<=L'f')return character-L'a'+10;
    if(character>=L'A'&&character<=L'F')return character-L'A'+10;
    return -1;
}

bool TryParseDecimalIndex(const std::wstring& text,size_t& result) {
    if(text.empty())return false;
    size_t value=0;
    constexpr auto maximum=(std::numeric_limits<size_t>::max)();
    for(const auto character:text){
        if(character<L'0'||character>L'9')return false;
        const auto digit=static_cast<size_t>(character-L'0');
        if(value>(maximum-digit)/10)return false;
        value=value*10+digit;
    }
    result=value;return true;
}

bool FillCryptographicRandom(void* buffer,size_t size) {
    using BCryptGenRandomFunction=LONG (WINAPI*)(void*,unsigned char*,unsigned long,unsigned long);
    static const auto function=[] {
        const auto library=LoadLibraryW(L"bcrypt.dll");
        return library?reinterpret_cast<BCryptGenRandomFunction>(
            GetProcAddress(library,"BCryptGenRandom")):nullptr;
    }();
    return function&&size<=(std::numeric_limits<unsigned long>::max)()&&
           function(nullptr,static_cast<unsigned char*>(buffer),static_cast<unsigned long>(size),
                    0x00000002L)>=0;
}

void AppendCodePoint(std::wstring& output,std::uint32_t codePoint) {
    if(codePoint>0x10ffff||(codePoint>=0xd800&&codePoint<=0xdfff))codePoint=0xfffd;
    if constexpr(sizeof(wchar_t)>=4)output+=static_cast<wchar_t>(codePoint);
    else if(codePoint<=0xffff)output+=static_cast<wchar_t>(codePoint);
    else{
        codePoint-=0x10000;
        output+=static_cast<wchar_t>(0xd800+(codePoint>>10));
        output+=static_cast<wchar_t>(0xdc00+(codePoint&0x3ff));
    }
}

bool ParseUnicodeEscape(const std::wstring& source,size_t& position,std::uint32_t& codePoint) {
    if(position>=source.size()||source[position]!=L'u')return false;
    size_t cursor=position+1;std::uint32_t value=0;
    if(cursor<source.size()&&source[cursor]==L'{'){
        ++cursor;const size_t digits=cursor;
        while(cursor<source.size()&&source[cursor]!=L'}'){
            const int digit=HexDigitValue(source[cursor]);
            if(digit<0||cursor-digits>=6)return false;
            value=value*16+static_cast<std::uint32_t>(digit);++cursor;
        }
        if(cursor==digits||cursor>=source.size()||source[cursor]!=L'}'||value>0x10ffff)return false;
        ++cursor;
    }else{
        if(cursor+4>source.size())return false;
        for(size_t index=0;index<4;++index){
            const int digit=HexDigitValue(source[cursor+index]);if(digit<0)return false;
            value=value*16+static_cast<std::uint32_t>(digit);
        }
        cursor+=4;
    }
    codePoint=value;position=cursor;return true;
}

std::wstring DecodeRegexUnicodeEscapes(const std::wstring& pattern) {
    std::wstring output;output.reserve(pattern.size());
    for(size_t position=0;position<pattern.size();){
        if(pattern[position]==L'\\'&&position+1<pattern.size()&&pattern[position+1]==L'u'){
            size_t end=position+1;std::uint32_t codePoint=0;
            if(ParseUnicodeEscape(pattern,end,codePoint)){
                std::wstring decoded;AppendCodePoint(decoded,codePoint);
                for(const auto character:decoded){
                    if(std::wstring_view(L"\\.^$|()[]{}*+?").find(character)!=std::wstring_view::npos)
                        output+=L'\\';
                    output+=character;
                }
                position=end;continue;
            }
        }
        output+=pattern[position++];
    }
    return output;
}

void AppendRegexClassCharacter(std::wstring& output,wchar_t character) {
    if(std::wstring_view(L"\\]-^").find(character)!=std::wstring_view::npos)
        output+=L'\\';
    output+=character;
}

const std::wstring& UnicodeLetterClassRanges() {
    static const std::wstring ranges=[](){
        std::wstring result;
        std::wstring characters(0x10000,L'\0');
        for(unsigned value=0;value<characters.size();++value)
            characters[value]=static_cast<wchar_t>(value);
        std::vector<WORD> types(characters.size());
        if(!GetStringTypeW(CT_CTYPE1,characters.data(),static_cast<int>(characters.size()),
                           types.data()))return result;
        const auto isLetter=[&](unsigned candidate){
            return !(candidate>=0xd800&&candidate<=0xdfff)&&
                   (types[candidate]&C1_ALPHA)!=0;
        };
        for(unsigned value=0;value<=0xffff;){
            if(!isLetter(value)){++value;continue;}
            const unsigned first=value;
            while(value<0xffff&&isLetter(value+1))++value;
            AppendRegexClassCharacter(result,static_cast<wchar_t>(first));
            if(value!=first){
                result+=L'-';
                AppendRegexClassCharacter(result,static_cast<wchar_t>(value));
            }
            ++value;
        }
        return result;
    }();
    return ranges;
}

bool TranslateUnicodePropertyEscapes(const std::wstring& pattern,const std::wstring& flags,
                                     std::wstring& output) {
    output.clear();output.reserve(pattern.size());
    const bool unicode=flags.find(L'u')!=std::wstring::npos||flags.find(L'v')!=std::wstring::npos;
    bool characterClass=false;
    for(size_t position=0;position<pattern.size();++position){
        const wchar_t character=pattern[position];
        if(character==L'\\'&&position+3<pattern.size()&&
           (pattern[position+1]==L'p'||pattern[position+1]==L'P')&&
           pattern[position+2]==L'{'){
            const auto end=pattern.find(L'}',position+3);
            if(!unicode||end==std::wstring::npos)return false;
            const auto property=pattern.substr(position+3,end-(position+3));
            if(property!=L"L"&&property!=L"Letter"&&
               property!=L"General_Category=L"&&property!=L"General_Category=Letter"&&
               property!=L"gc=L"&&property!=L"gc=Letter")return false;
            const bool negated=pattern[position+1]==L'P';
            if(characterClass&&negated)return false;
            if(!characterClass)output+=negated?L"[^":L"[";
            output+=UnicodeLetterClassRanges();
            if(!characterClass)output+=L']';
            position=end;continue;
        }
        output+=character;
        if(character==L'\\'&&position+1<pattern.size())output+=pattern[++position];
        else if(character==L'['&&!characterClass)characterClass=true;
        else if(character==L']'&&characterClass)characterClass=false;
    }
    return true;
}

std::wstring NormalizeLegacyRegexBraces(const std::wstring& pattern) {
    // Web JavaScript treats braces that do not form a numeric quantifier as
    // literals in non-Unicode legacy patterns. MSVC's ECMAScript wregex rejects
    // them, so escape only those literal braces before compiling the pattern.
    std::wstring output;output.reserve(pattern.size()+4);bool characterClass=false;
    for(size_t position=0;position<pattern.size();++position){
        const auto character=pattern[position];
        if(character==L'\\'){
            output+=character;if(position+1<pattern.size())output+=pattern[++position];continue;
        }
        if(character==L'['&&!characterClass){characterClass=true;output+=character;continue;}
        if(character==L']'&&characterClass){characterClass=false;output+=character;continue;}
        if(!characterClass&&character==L'{'){
            size_t cursor=position+1;
            while(cursor<pattern.size()&&std::iswdigit(pattern[cursor]))++cursor;
            const bool hasMinimum=cursor>position+1;
            if(hasMinimum&&cursor<pattern.size()&&pattern[cursor]==L','){
                ++cursor;while(cursor<pattern.size()&&std::iswdigit(pattern[cursor]))++cursor;
            }
            if(hasMinimum&&cursor<pattern.size()&&pattern[cursor]==L'}'){
                output.append(pattern,position,cursor-position+1);position=cursor;continue;
            }
            output+=L"\\{";continue;
        }
        if(!characterClass&&character==L'}'){output+=L"\\}";continue;}
        output+=character;
    }
    return output;
}

// A large share of split separators are finite regular expressions made from
// literal characters and greedy optional characters (for example /--?/ or
// /\r?\n/).  Running the general MSVC regex engine once per match is very
// expensive on multi-megabyte strings, so compile that whole regex class into
// ordered literal alternatives.  Unsupported expressions still use wregex.
struct FiniteRegexSeparator {
    std::vector<std::wstring> alternatives;
    bool ignoreCase=false;

    bool MatchAt(const std::wstring& input,size_t position,size_t& length) const {
        for(const auto& alternative:alternatives){
            if(alternative.size()>input.size()-position)continue;
            bool matches=true;
            for(size_t index=0;index<alternative.size();++index){
                auto left=input[position+index],right=alternative[index];
                if(ignoreCase){left=static_cast<wchar_t>(std::towlower(left));right=static_cast<wchar_t>(std::towlower(right));}
                if(left!=right){matches=false;break;}
            }
            if(matches){length=alternative.size();return true;}
        }
        return false;
    }
};

bool CompileFiniteRegexSeparator(const std::wstring& pattern,const std::wstring& flags,
                                 FiniteRegexSeparator& result){
    result.alternatives.assign(1,L"");
    result.ignoreCase=flags.find(L'i')!=std::wstring::npos;
    for(size_t position=0;position<pattern.size();++position){
        wchar_t literal=pattern[position];
        if(literal==L'\\'){
            if(++position>=pattern.size())return false;
            const auto escaped=pattern[position];
            switch(escaped){
            case L'r':literal=L'\r';break;
            case L'n':literal=L'\n';break;
            case L't':literal=L'\t';break;
            case L'f':literal=L'\f';break;
            case L'v':literal=L'\v';break;
            case L'0':literal=L'\0';break;
            default:
                if(std::iswalnum(escaped))return false;
                literal=escaped;break;
            }
        }else if(std::wstring_view(L".^$|?*+()[]{}").find(literal)!=std::wstring_view::npos){
            return false;
        }
        const bool optional=position+1<pattern.size()&&pattern[position+1]==L'?';
        if(optional)++position;
        if(optional){
            if(result.alternatives.size()>64)return false;
            std::vector<std::wstring> expanded;
            expanded.reserve(result.alternatives.size()*2);
            for(const auto& alternative:result.alternatives){
                expanded.push_back(alternative+literal);
                expanded.push_back(alternative);
            }
            result.alternatives=std::move(expanded);
        }else{
            for(auto& alternative:result.alternatives)alternative+=literal;
        }
    }
    return true;
}

struct AnchoredRegexPrefixes {
    std::vector<std::wstring> values;
    bool ignoreCase=false;

    bool MayMatch(const std::wstring& input) const {
        if(values.empty())return true;
        for(const auto& prefix:values){
            if(prefix.size()>input.size())continue;
            bool matches=true;
            for(size_t index=0;index<prefix.size();++index){
                auto left=input[index],right=prefix[index];
                if(ignoreCase){left=static_cast<wchar_t>(std::towlower(left));right=static_cast<wchar_t>(std::towlower(right));}
                if(left!=right){matches=false;break;}
            }
            if(matches)return true;
        }
        return false;
    }
};

AnchoredRegexPrefixes CompileAnchoredRegexPrefixes(const std::wstring& pattern,
                                                    const std::wstring& flags){
    AnchoredRegexPrefixes result;result.ignoreCase=flags.find(L'i')!=std::wstring::npos;
    if(pattern.empty()||pattern.front()!=L'^')return result;
    auto prefix=[&](size_t begin,size_t end){
        std::wstring value;
        for(size_t position=begin;position<end;++position){
            auto character=pattern[position];
            if(character==L'\\'){
                if(++position>=end)return std::wstring{};
                const auto escaped=pattern[position];
                if(std::iswalnum(escaped)){
                    if(escaped==L'r')character=L'\r';
                    else if(escaped==L'n')character=L'\n';
                    else if(escaped==L't')character=L'\t';
                    else return value;
                }else character=escaped;
            }else if(character==L'?'||character==L'*'){
                if(!value.empty())value.pop_back();
                return value;
            }else if(character==L'{')return std::wstring{};
            else if(std::wstring_view(L".^$|+()[]}").find(character)!=std::wstring_view::npos)return value;
            value+=character;
        }
        return value;
    };
    size_t start=1;
    if(start<pattern.size()&&pattern[start]==L'('){
        ++start;if(start+1<pattern.size()&&pattern[start]==L'?'&&pattern[start+1]==L':')start+=2;
        size_t alternative=start;int depth=0;
        for(size_t position=start;position<pattern.size();++position){
            if(pattern[position]==L'\\'){++position;continue;}
            if(pattern[position]==L'(')++depth;
            else if(pattern[position]==L')'){
                if(depth==0){
                    auto value=prefix(alternative,position);if(value.empty()){result.values.clear();return result;}
                    result.values.push_back(std::move(value));return result;
                }
                --depth;
            }else if(pattern[position]==L'|'&&depth==0){
                auto value=prefix(alternative,position);if(value.empty()){result.values.clear();return result;}
                result.values.push_back(std::move(value));alternative=position+1;
            }
        }
        result.values.clear();return result;
    }
    auto value=prefix(start,pattern.size());if(!value.empty())result.values.push_back(std::move(value));
    return result;
}

struct Object;
struct Function;
struct NativeFunction;
struct Reference;
struct Environment;
struct Chunk;
struct Prototype;

struct Value {
    enum class Type { Undefined, Null, Boolean, Number, String, Object, Function, Native, Reference };
    Type type = Type::Undefined;
    bool boolean = false;
    double number = 0.0;
    std::wstring string;
    std::shared_ptr<Object> object;
    std::shared_ptr<Function> function;
    std::shared_ptr<NativeFunction> native;
    std::shared_ptr<Reference> reference;

    static Value Undefined() { return {}; }
    static Value Null() { Value v; v.type = Type::Null; return v; }
    static Value Bool(bool b) { Value v; v.type=Type::Boolean; v.boolean=b; return v; }
    static Value Number(double n) { Value v; v.type=Type::Number; v.number=n; return v; }
    static Value String(std::wstring s) { Value v; v.type=Type::String; v.string=std::move(s); return v; }
    static Value FromObject(const std::shared_ptr<Object>& o) { Value v; v.type=Type::Object; v.object=o; return v; }
    static Value FromFunction(const std::shared_ptr<Function>& f) { Value v; v.type=Type::Function; v.function=f; return v; }
    static Value FromNative(const std::shared_ptr<NativeFunction>& f) { Value v; v.type=Type::Native; v.native=f; return v; }
    static Value FromReference(const std::shared_ptr<Reference>& r) { Value v; v.type=Type::Reference; v.reference=r; return v; }
};

struct JavaScriptException {
    Value value;
};

struct PromiseReaction {
    Value onFulfilled;
    Value onRejected;
    std::shared_ptr<Object> nextPromise;
};

enum class PromiseState { Pending, Fulfilled, Rejected };
enum class ObjectKind { Plain, Array, Map, Set, Proxy, RegExp, Window, FrameWindow, FrameDocument, Document, Node, NamedNodeMap, Attr, Range, Selection, TreeWalker, ClassList, Style, Dataset, Event, WebView, Performance, Math, Json, ObjectConstructor, ArrayConstructor, StringConstructor, NumberConstructor, DateConstructor, Date, PromiseConstructor, ErrorConstructor, Url, UrlSearchParams, Storage, MediaQuery, Headers, Response, ReadableStream, ReadableStreamReader, TextDecoder, XmlHttpRequest, MutationObserver, IntersectionObserver, ResizeObserver, Promise, Error, Location, File, FileReader, Clipboard, DataTransfer, DataTransferItem, CanvasContext2D, CanvasGradient };

struct Object {
    ObjectKind kind = ObjectKind::Plain;
    FastMap<std::wstring, Value> props;
    std::vector<Value> items;
    std::vector<std::pair<Value, Value>> entries;
    std::vector<std::weak_ptr<Node>> observedNodes;
    std::weak_ptr<Object> byteSource;
    std::shared_ptr<Node> node;
    std::shared_ptr<Node> rangeStart;
    std::shared_ptr<Node> rangeEnd;
    size_t rangeStartOffset = 0;
    size_t rangeEndOffset = 0;
    std::shared_ptr<CanvasGradient> canvasGradient;
    std::shared_ptr<Object> prototype;
    PromiseState promiseState = PromiseState::Pending;
    Value promiseResult;
    std::vector<PromiseReaction> promiseReactions;
    bool promiseHandled = false;
    bool promiseUnhandledNotified = false;
};

static bool IsInternalObjectProperty(const std::wstring& name){
    if(name.rfind(L"$get:",0)==0||name.rfind(L"$set:",0)==0||
       name.rfind(L"$method:",0)==0||name.rfind(L"$field:",0)==0||
       name.rfind(L"$enumerable:",0)==0||
       name.rfind(L"$request-header:",0)==0)return true;
    for(const auto* internal:{L"$abortListeners",L"$aborted",L"$async",L"$body",L"$callback",L"$class",
        L"$computed",L"$current",L"$customValidity",L"$file",L"$flags",L"$handler",
        L"$immediateStopped",L"$inPassiveListener",L"$matches",L"$method",L"$mimeType",
        L"$name",L"$options",L"$path",L"$pattern",L"$primitive",L"$propagationStopped",L"$prototypeValue",
        L"$query",L"$range",L"$read",L"$responseHeaders",L"$target",L"$text",L"$time",
        L"$dataView",L"$topProxy",L"$typedArrayBits",L"$typedArrayClamped",L"$typedArrayFloating",L"$typedArrayName",
        L"$typedArraySigned",L"$url",L"$whatToShow"})
        if(name==internal)return true;
    return false;
}

struct Environment {
    FastMap<std::wstring, Value> values;
    std::shared_ptr<Environment> parent;
    Value* FindValue(const std::wstring& name) {
        const auto found=values.find(name);
        if(found!=values.end())return &found->second;
        return parent ? parent->FindValue(name) : nullptr;
    }
    Environment* Find(const std::wstring& name) {
        if (values.find(name)!=values.end()) return this;
        return parent ? parent->Find(name) : nullptr;
    }
};

struct Reference {
    enum class Kind { Variable, Property, Super } kind = Kind::Variable;
    std::shared_ptr<Environment> env;
    std::wstring name;
    Value base;
};

enum class Op {
    Constant, Undefined, Null, TrueValue, FalseValue,
    BeginBlock, EndBlock, LoadReference, LoadSuperReference, Declare, GetProperty, GetIndex, Assign, EvaluateEmbeddedExpression, DefaultIfUndefined,
    NewArray, ArrayPush, ArraySpread, NewObject, ObjectSet, ObjectSetComputed, ObjectSetPrototype, ObjectSpread, EnumerableKeys, MakeFunction,
    Call, CallArray, Construct, ConstructArray, DeleteValue, ThrowValue, Await, Yield, YieldSpread, LeaveCatch, EndFinally, Pop, Duplicate, PostIncrement, PostDecrement, PreIncrement, PreDecrement,
    Add, Subtract, Multiply, Divide, Modulo, Power,
    BitwiseAnd, BitwiseOr, BitwiseXor, ShiftLeft, ShiftRight, UnsignedShiftRight, InValue, InstanceOf,
    Equal, NotEqual, StrictEqual, StrictNotEqual, Less, LessEqual, Greater, GreaterEqual,
    Not, BitwiseNot, Negate, Positive, VoidValue, TypeOf,
    Jump, JumpFalse, JumpFalseKeep, JumpTrueKeep, JumpNotNullishKeep,
    Template, TaggedTemplate, Return
};

struct Instruction {
    Op op = Op::Undefined;
    int argument = 0;
    std::wstring text;
    int line = 1;
    size_t offset = 0;
    int blockDepth = 0;
};

struct Chunk {
    struct FunctionDeclaration {
        std::wstring name;
        int prototype = -1;
    };
    struct ExceptionHandler {
        size_t tryStart=0,tryEnd=0;
        size_t catchStart=0,catchEnd=0;
        size_t finallyStart=0,finallyEnd=0;
        size_t end=0;
        std::wstring catchName;
        bool hasCatch=false,hasFinally=false;
    };
    std::vector<Instruction> code;
    std::vector<Value> constants;
    std::vector<std::shared_ptr<Chunk>> embeddedExpressions;
    std::vector<ExceptionHandler> handlers;
    // Function declarations are instantiated when their script or function
    // scope is entered. Keeping them outside the instruction stream implements
    // JavaScript declaration hoisting without shifting jumps/exception ranges.
    std::vector<FunctionDeclaration> functionDeclarations;
    // `var` declarations belong to the surrounding script/function scope even
    // when their statement is nested in a block.  Instantiate the bindings
    // before any initializer runs so an initializer never falls through to an
    // identically named value in an outer environment.
    std::vector<std::wstring> variableDeclarations;
};

constexpr size_t kBaselineJitMaxLocals=64;
constexpr size_t kBaselineJitMaxStack=64;

struct BaselineJitFrame {
    double locals[kBaselineJitMaxLocals];
    double stack[kBaselineJitMaxStack];
    double result;
    std::uint32_t resultKind;
};

struct BaselineJitCode {
    using Entry=void (*)(BaselineJitFrame*);
    void* memory=nullptr;
    size_t size=0;
    size_t allocationSize=0;
    Entry entry=nullptr;
    ~BaselineJitCode(){if(memory)VirtualFree(memory,0,MEM_RELEASE);}
};

struct Prototype {
    struct Binding {
        std::wstring parameter, property, name;
        bool indexed=false;
        std::shared_ptr<Chunk> defaultValue;
    };
    std::vector<std::wstring> parameters;
    std::vector<std::shared_ptr<Chunk>> parameterDefaults;
    std::vector<Binding> bindings;
    std::wstring restParameter;
    Chunk chunk;
    std::wstring name;
    // Only a syntactically named function expression/declaration creates a
    // lexical self binding. An inferred display name (for example when an
    // anonymous callback is assigned to `b`) must not shadow a captured `b`.
    std::wstring selfBindingName;
    bool lexicalThis = false;
    bool isAsync = false;
    bool isGenerator = false;
    bool isStrict = false;
    size_t jitCallCount = 0;
    bool jitCompilationAttempted = false;
    std::shared_ptr<BaselineJitCode> jitCode;
};

#if defined(_M_X64)
// The first tier is deliberately a leaf-code compiler: numeric arguments are
// guarded before entry, generated code never calls C++ or allocates, and any
// unsupported bytecode rejects the whole function before executable memory is
// created. This keeps exceptions and stack unwinding out of generated frames.
enum class JitScalarKind : std::uint8_t { Number, Boolean, Reference };
struct JitAbstractValue {
    JitScalarKind kind=JitScalarKind::Number;
    std::uint16_t slot=0;
    bool operator==(const JitAbstractValue& other)const{
        return kind==other.kind&&slot==other.slot;
    }
};
struct JitAbstractState {
    bool initialized=false;
    std::uint64_t assigned=0;
    std::vector<JitAbstractValue> stack;
};

class X64BaselineEmitter {
public:
    // Small x64/SSE2 encoder for the fixed instruction subset used below.
    struct Patch { size_t displacement=0,target=0; };
    std::vector<std::uint8_t> code;
    std::vector<Patch> patches;

    void Byte(std::uint8_t value){code.push_back(value);}
    void Dword(std::uint32_t value){
        for(int shift=0;shift<32;shift+=8)Byte(static_cast<std::uint8_t>(value>>shift));
    }
    void Qword(std::uint64_t value){
        for(int shift=0;shift<64;shift+=8)Byte(static_cast<std::uint8_t>(value>>shift));
    }
    void MovRax(std::uint64_t value){Byte(0x48);Byte(0xb8);Qword(value);}
    void MovFrameFromRax(std::uint32_t offset){Byte(0x48);Byte(0x89);Byte(0x81);Dword(offset);}
    void MovXmm0FromFrame(std::uint32_t offset){Byte(0xf2);Byte(0x0f);Byte(0x10);Byte(0x81);Dword(offset);}
    void MovXmm1FromFrame(std::uint32_t offset){Byte(0xf2);Byte(0x0f);Byte(0x10);Byte(0x89);Dword(offset);}
    void MovFrameFromXmm0(std::uint32_t offset){Byte(0xf2);Byte(0x0f);Byte(0x11);Byte(0x81);Dword(offset);}
    void MovXmm1FromRax(){Byte(0x66);Byte(0x48);Byte(0x0f);Byte(0x6e);Byte(0xc8);}
    void XorXmm1(){Byte(0x66);Byte(0x0f);Byte(0x57);Byte(0xc9);}
    void XorXmm0Xmm1(){Byte(0x66);Byte(0x0f);Byte(0x57);Byte(0xc1);}
    void Add(){Byte(0xf2);Byte(0x0f);Byte(0x58);Byte(0xc1);}
    void Multiply(){Byte(0xf2);Byte(0x0f);Byte(0x59);Byte(0xc1);}
    void Subtract(){Byte(0xf2);Byte(0x0f);Byte(0x5c);Byte(0xc1);}
    void Divide(){Byte(0xf2);Byte(0x0f);Byte(0x5e);Byte(0xc1);}
    void Compare(){Byte(0x66);Byte(0x0f);Byte(0x2e);Byte(0xc1);}
    void Set(std::uint8_t condition,std::uint8_t destination){
        Byte(0x0f);Byte(static_cast<std::uint8_t>(0x90+condition));Byte(destination);
    }
    void AndAlDl(){Byte(0x20);Byte(0xd0);}
    void OrAlDl(){Byte(0x08);Byte(0xd0);}
    void BooleanToXmm0(){
        Byte(0x0f);Byte(0xb6);Byte(0xc0);
        Byte(0xf2);Byte(0x0f);Byte(0x2a);Byte(0xc0);
    }
    void StoreDword(std::uint32_t offset,std::uint32_t value){
        Byte(0xc7);Byte(0x81);Dword(offset);Dword(value);
    }
    void Jump(size_t target){
        Byte(0xe9);patches.push_back({code.size(),target});Dword(0);
    }
    void JumpCondition(std::uint8_t condition,size_t target){
        Byte(0x0f);Byte(static_cast<std::uint8_t>(0x80+condition));
        patches.push_back({code.size(),target});Dword(0);
    }
    void Ret(){Byte(0xc3);}
};

std::shared_ptr<BaselineJitCode> CompileBaselineJit(const Prototype& prototype){
    const bool hasParameterDefaults=std::any_of(
        prototype.parameterDefaults.begin(),prototype.parameterDefaults.end(),
        [](const auto& value){return static_cast<bool>(value);});
    if(prototype.isAsync||hasParameterDefaults||
       !prototype.bindings.empty()||!prototype.restParameter.empty()||
       !prototype.chunk.handlers.empty())return {};
    const auto& chunk=prototype.chunk;
    if(chunk.code.empty())return {};

    std::unordered_map<std::wstring,size_t> locals;
    for(const auto& parameter:prototype.parameters){
        if(locals.size()>=kBaselineJitMaxLocals)return {};
        if(!locals.emplace(parameter,locals.size()).second)return {};
    }
    for(const auto& instruction:chunk.code)if(instruction.op==Op::Declare){
        if(locals.count(instruction.text)){if(instruction.blockDepth>0)return {};continue;}
        if(locals.size()>=kBaselineJitMaxLocals)return {};
        locals.emplace(instruction.text,locals.size());
    }
    for(const auto& instruction:chunk.code)if(instruction.op==Op::LoadReference&&!locals.count(instruction.text))return {};

    const auto reference=[&](const std::wstring& name){
        return JitAbstractValue{JitScalarKind::Reference,static_cast<std::uint16_t>(locals.at(name))};
    };
    const auto numeric=[](const JitAbstractValue& value,std::uint64_t assigned){
        return value.kind==JitScalarKind::Number||
            (value.kind==JitScalarKind::Reference&&(assigned&(std::uint64_t{1}<<value.slot))!=0);
    };
    const auto truthy=[&](const JitAbstractValue& value,std::uint64_t assigned){
        return numeric(value,assigned)||value.kind==JitScalarKind::Boolean;
    };
    std::vector<JitAbstractState> states(chunk.code.size()+1);
    std::deque<size_t> work;
    states[0].initialized=true;
    for(size_t index=0;index<prototype.parameters.size();++index)states[0].assigned|=std::uint64_t{1}<<index;
    work.push_back(0);
    bool valid=true,hasReturn=false;
    const auto merge=[&](size_t target,const std::vector<JitAbstractValue>& stack,std::uint64_t assigned){
        if(target>=chunk.code.size()){valid=false;return;}
        auto& state=states[target];
        if(!state.initialized){state.initialized=true;state.assigned=assigned;state.stack=stack;work.push_back(target);}
        else if(state.stack!=stack||state.assigned!=assigned)valid=false;
    };
    while(valid&&!work.empty()){
        const size_t ip=work.front();work.pop_front();
        auto stack=states[ip].stack;
        auto assigned=states[ip].assigned;
        const auto& instruction=chunk.code[ip];
        const auto pop=[&](){
            if(stack.empty()){valid=false;return JitAbstractValue{};}
            auto value=stack.back();stack.pop_back();return value;
        };
        bool fallthrough=true;
        switch(instruction.op){
        case Op::BeginBlock:case Op::EndBlock:break;
        case Op::Constant:
            if(instruction.argument<0||static_cast<size_t>(instruction.argument)>=chunk.constants.size()||
               chunk.constants[static_cast<size_t>(instruction.argument)].type!=Value::Type::Number){valid=false;break;}
            stack.push_back({JitScalarKind::Number,0});break;
        case Op::TrueValue:case Op::FalseValue:stack.push_back({JitScalarKind::Boolean,0});break;
        case Op::LoadReference:stack.push_back(reference(instruction.text));break;
        case Op::Declare:{
            auto value=pop();if(!numeric(value,assigned))valid=false;
            else assigned|=std::uint64_t{1}<<locals.at(instruction.text);break;
        }
        case Op::Duplicate:if(stack.empty())valid=false;else stack.push_back(stack.back());break;
        case Op::Pop:pop();break;
        case Op::Assign:{
            auto value=pop(),target=pop();if(target.kind!=JitScalarKind::Reference||!numeric(value,assigned))valid=false;
            else{assigned|=std::uint64_t{1}<<target.slot;stack.push_back({JitScalarKind::Number,0});}break;
        }
        case Op::PostIncrement:case Op::PostDecrement:case Op::PreIncrement:case Op::PreDecrement:{
            auto target=pop();if(target.kind!=JitScalarKind::Reference||!numeric(target,assigned))valid=false;
            else stack.push_back({JitScalarKind::Number,0});break;
        }
        case Op::Add:case Op::Subtract:case Op::Multiply:case Op::Divide:{
            auto right=pop(),left=pop();if(!numeric(left,assigned)||!numeric(right,assigned))valid=false;else stack.push_back({JitScalarKind::Number,0});break;
        }
        case Op::Equal:case Op::NotEqual:case Op::StrictEqual:case Op::StrictNotEqual:
        case Op::Less:case Op::LessEqual:case Op::Greater:case Op::GreaterEqual:{
            auto right=pop(),left=pop();if(!numeric(left,assigned)||!numeric(right,assigned))valid=false;else stack.push_back({JitScalarKind::Boolean,0});break;
        }
        case Op::Negate:case Op::Positive:{auto value=pop();if(!numeric(value,assigned))valid=false;else stack.push_back({JitScalarKind::Number,0});break;}
        case Op::Not:{auto value=pop();if(!truthy(value,assigned))valid=false;else stack.push_back({JitScalarKind::Boolean,0});break;}
        case Op::Jump:
            if(instruction.argument<0)valid=false;else merge(static_cast<size_t>(instruction.argument),stack,assigned);
            fallthrough=false;break;
        case Op::JumpFalse:{
            auto value=pop();if(!truthy(value,assigned)||instruction.argument<0)valid=false;
            else merge(static_cast<size_t>(instruction.argument),stack,assigned);break;
        }
        case Op::JumpFalseKeep:case Op::JumpTrueKeep:{
            if(stack.empty()||!truthy(stack.back(),assigned)||instruction.argument<0){valid=false;break;}
            merge(static_cast<size_t>(instruction.argument),stack,assigned);stack.pop_back();break;
        }
        case Op::Return:{auto value=pop();if(!truthy(value,assigned))valid=false;hasReturn=true;fallthrough=false;break;}
        default:valid=false;break;
        }
        if(stack.size()>kBaselineJitMaxStack)valid=false;
        if(valid&&fallthrough)merge(ip+1,stack,assigned);
    }
    if(!valid||!hasReturn)return {};

    X64BaselineEmitter out;
    // ENDBR64 keeps indirect JIT entries compatible with control-flow enforcement.
    out.Byte(0xf3);out.Byte(0x0f);out.Byte(0x1e);out.Byte(0xfa);
    std::vector<size_t> labels(chunk.code.size()+1,static_cast<size_t>(-1));
    const auto localOffset=[](size_t slot){return static_cast<std::uint32_t>(offsetof(BaselineJitFrame,locals)+slot*sizeof(double));};
    const auto stackOffset=[](size_t slot){return static_cast<std::uint32_t>(offsetof(BaselineJitFrame,stack)+slot*sizeof(double));};
    const auto doubleBits=[](double value){std::uint64_t bits=0;std::memcpy(&bits,&value,sizeof(bits));return bits;};
    const auto loadValue=[&](const std::vector<JitAbstractValue>& stack,size_t position,bool second){
        const auto& value=stack[position];const auto offset=value.kind==JitScalarKind::Reference?localOffset(value.slot):stackOffset(position);
        if(second)out.MovXmm1FromFrame(offset);else out.MovXmm0FromFrame(offset);
    };
    const auto storeStack=[&](size_t position){out.MovFrameFromXmm0(stackOffset(position));};
    const auto emitBoolean=[&](Op operation){
        // x86 condition codes: B=2, AE=3, E=4, NE=5, BE=6, A=7, P=A, NP=B.
        switch(operation){
        case Op::Less:out.Set(2,0xc0);out.Set(11,0xc2);out.AndAlDl();break;
        case Op::LessEqual:out.Set(6,0xc0);out.Set(11,0xc2);out.AndAlDl();break;
        case Op::Greater:out.Set(7,0xc0);break;
        case Op::GreaterEqual:out.Set(3,0xc0);break;
        case Op::Equal:case Op::StrictEqual:out.Set(4,0xc0);out.Set(11,0xc2);out.AndAlDl();break;
        case Op::NotEqual:case Op::StrictNotEqual:out.Set(5,0xc0);out.Set(10,0xc2);out.OrAlDl();break;
        default:break;
        }
        out.BooleanToXmm0();
    };
    const auto emitTruthBranch=[&](const std::vector<JitAbstractValue>& stack,bool whenTrue,size_t target){
        loadValue(stack,stack.size()-1,false);out.XorXmm1();out.Compare();out.JumpCondition(whenTrue?5:4,target);
    };
    for(size_t ip=0;ip<chunk.code.size();++ip){
        labels[ip]=out.code.size();if(!states[ip].initialized)continue;
        const auto& stack=states[ip].stack;const auto& instruction=chunk.code[ip];
        switch(instruction.op){
        case Op::BeginBlock:case Op::EndBlock:break;
        case Op::Constant:{
            const auto number=chunk.constants[static_cast<size_t>(instruction.argument)].number;
            out.MovRax(doubleBits(number));out.MovFrameFromRax(stackOffset(stack.size()));break;
        }
        case Op::TrueValue:case Op::FalseValue:
            out.MovRax(doubleBits(instruction.op==Op::TrueValue?1.0:0.0));out.MovFrameFromRax(stackOffset(stack.size()));break;
        case Op::LoadReference:break;
        case Op::Declare:{
            loadValue(stack,stack.size()-1,false);out.MovFrameFromXmm0(localOffset(locals.at(instruction.text)));break;
        }
        case Op::Duplicate:
            if(stack.back().kind!=JitScalarKind::Reference){loadValue(stack,stack.size()-1,false);storeStack(stack.size());}break;
        case Op::Pop:break;
        case Op::Assign:{
            const auto resultPosition=stack.size()-2;loadValue(stack,stack.size()-1,false);
            out.MovFrameFromXmm0(localOffset(stack[resultPosition].slot));storeStack(resultPosition);break;
        }
        case Op::PostIncrement:case Op::PostDecrement:case Op::PreIncrement:case Op::PreDecrement:{
            const auto position=stack.size()-1;const auto slot=stack.back().slot;loadValue(stack,position,false);
            if(instruction.op==Op::PostIncrement||instruction.op==Op::PostDecrement)storeStack(position);
            out.MovRax(doubleBits(1.0));out.MovXmm1FromRax();
            if(instruction.op==Op::PostIncrement||instruction.op==Op::PreIncrement)out.Add();else out.Subtract();
            out.MovFrameFromXmm0(localOffset(slot));
            if(instruction.op==Op::PreIncrement||instruction.op==Op::PreDecrement)storeStack(position);break;
        }
        case Op::Add:case Op::Subtract:case Op::Multiply:case Op::Divide:{
            const auto resultPosition=stack.size()-2;loadValue(stack,resultPosition,false);loadValue(stack,stack.size()-1,true);
            if(instruction.op==Op::Add)out.Add();else if(instruction.op==Op::Subtract)out.Subtract();
            else if(instruction.op==Op::Multiply)out.Multiply();else out.Divide();
            storeStack(resultPosition);break;
        }
        case Op::Equal:case Op::NotEqual:case Op::StrictEqual:case Op::StrictNotEqual:
        case Op::Less:case Op::LessEqual:case Op::Greater:case Op::GreaterEqual:{
            const auto resultPosition=stack.size()-2;loadValue(stack,resultPosition,false);loadValue(stack,stack.size()-1,true);
            out.Compare();emitBoolean(instruction.op);storeStack(resultPosition);break;
        }
        case Op::Negate:{
            const auto position=stack.size()-1;loadValue(stack,position,false);out.MovRax(0x8000000000000000ull);
            out.MovXmm1FromRax();out.XorXmm0Xmm1();storeStack(position);break;
        }
        case Op::Positive:{const auto position=stack.size()-1;loadValue(stack,position,false);storeStack(position);break;}
        case Op::Not:{
            const auto position=stack.size()-1;loadValue(stack,position,false);out.XorXmm1();out.Compare();out.Set(4,0xc0);
            out.BooleanToXmm0();storeStack(position);break;
        }
        case Op::Jump:out.Jump(static_cast<size_t>(instruction.argument));break;
        case Op::JumpFalse:emitTruthBranch(stack,false,static_cast<size_t>(instruction.argument));break;
        case Op::JumpFalseKeep:emitTruthBranch(stack,false,static_cast<size_t>(instruction.argument));break;
        case Op::JumpTrueKeep:emitTruthBranch(stack,true,static_cast<size_t>(instruction.argument));break;
        case Op::Return:{
            const auto& value=stack.back();loadValue(stack,stack.size()-1,false);
            out.MovFrameFromXmm0(static_cast<std::uint32_t>(offsetof(BaselineJitFrame,result)));
            out.StoreDword(static_cast<std::uint32_t>(offsetof(BaselineJitFrame,resultKind)),
                           value.kind==JitScalarKind::Boolean?1u:0u);
            out.Ret();break;
        }
        default:return {};
        }
    }
    labels[chunk.code.size()]=out.code.size();out.Ret();
    for(const auto& patch:out.patches){
        if(patch.target>=labels.size()||labels[patch.target]==static_cast<size_t>(-1))return {};
        const auto relative=static_cast<std::int64_t>(labels[patch.target])-static_cast<std::int64_t>(patch.displacement+4);
        if(relative<(std::numeric_limits<std::int32_t>::min)()||relative>(std::numeric_limits<std::int32_t>::max)())return {};
        const auto value=static_cast<std::uint32_t>(static_cast<std::int32_t>(relative));
        for(int shift=0;shift<32;shift+=8)out.code[patch.displacement+static_cast<size_t>(shift/8)]=static_cast<std::uint8_t>(value>>shift);
    }
    if(out.code.empty())return {};
    auto compiled=std::make_shared<BaselineJitCode>();compiled->size=out.code.size();
    static const size_t pageSize=[](){SYSTEM_INFO information{};GetSystemInfo(&information);return static_cast<size_t>(information.dwPageSize);}();
    compiled->allocationSize=(compiled->size+pageSize-1)/pageSize*pageSize;
    compiled->memory=VirtualAlloc(nullptr,compiled->allocationSize,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!compiled->memory)return {};
    std::memcpy(compiled->memory,out.code.data(),compiled->size);
    DWORD previous=0;
    if(!VirtualProtect(compiled->memory,compiled->size,PAGE_EXECUTE_READ,&previous))return {};
    if(!FlushInstructionCache(GetCurrentProcess(),compiled->memory,compiled->size))return {};
    compiled->entry=reinterpret_cast<BaselineJitCode::Entry>(compiled->memory);return compiled;
}
#else
std::shared_ptr<BaselineJitCode> CompileBaselineJit(const Prototype&){return {};}
#endif

struct Module {
    std::vector<std::shared_ptr<Prototype>> prototypes;
};

struct Function {
    std::shared_ptr<Prototype> prototype;
    std::shared_ptr<Environment> closure;
    FastMap<std::wstring, Value> props;
};

struct RuntimeCore;
using NativeCallback = std::function<Value(RuntimeCore&, const Value&, const std::vector<Value>&)>;
struct NativeFunction {
    NativeCallback callback;
    FastMap<std::wstring, Value> props;
};

enum class CompletionKind { Return, Jump, Throw };
struct PendingCompletion {
    int handler=-1;
    CompletionKind kind=CompletionKind::Return;
    Value value;
    size_t target=0;
};
struct ExecutionFrame {
    struct CatchScope { int handler=-1;std::shared_ptr<Environment> outer; };
    const Chunk* chunk=nullptr;
    std::shared_ptr<Prototype> prototype;
    std::shared_ptr<Environment> env;
    std::vector<Value> stack;
    std::vector<PendingCompletion> pending;
    std::vector<CatchScope> catchScopes;
    std::vector<std::shared_ptr<Environment>> blockOuters;
    size_t ip=0;
    std::shared_ptr<Object> asyncPromise;
    std::shared_ptr<std::vector<Value>> generatorValues;
    bool resumeThrow=false;
    Value resumeValue;
    size_t resumeOrigin=0;
    size_t callStackIndex=(std::numeric_limits<size_t>::max)();
};
struct FrameResult {
    bool suspended=false;
    Value value;
};

enum class TokenKind {
    End, Identifier, Number, String, Template, RegExp,
    LeftParen, RightParen, LeftBrace, RightBrace, LeftBracket, RightBracket,
    Dot, Ellipsis, Comma, Colon, Semicolon, Question, OptionalChain, Nullish, NullishAssign,
    Plus, Minus, PlusPlus, MinusMinus, PlusAssign, MinusAssign,
    Star, StarAssign, Exponent, ExponentAssign, Slash, SlashAssign, Percent, PercentAssign, Bang,
    Assign, Equal, NotEqual, StrictEqual, StrictNotEqual, Less, LessEqual, Greater, GreaterEqual,
    ShiftLeft, ShiftLeftAssign, ShiftRight, ShiftRightAssign, UnsignedShiftRight, UnsignedShiftRightAssign,
    BitAnd, BitAndAssign, BitOr, BitOrAssign, BitXor, BitXorAssign, BitNot,
    And, AndAssign, Or, OrAssign, Arrow
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::wstring text;
    double number = 0;
    int line = 1;
    size_t offset = 0;
    std::wstring extra;
};

class Lexer {
public:
    explicit Lexer(const std::wstring& source) : source_(source) {}
    std::vector<Token> Scan() {
        std::vector<Token> tokens;
        for (;;) {
            SkipTrivia();
            if (position_ >= source_.size()) { Token end;end.kind=TokenKind::End;end.line=line_;end.offset=source_.size();tokens.push_back(std::move(end));break; }
            const wchar_t c = source_[position_++];
            Token token; token.line = line_;token.offset=position_-1;
            if (std::iswalpha(c) || c == L'_' || c == L'$') {
                const size_t start = position_ - 1;
                while (position_ < source_.size() && (std::iswalnum(source_[position_]) || source_[position_] == L'_' || source_[position_] == L'$')) ++position_;
                token.kind=TokenKind::Identifier; token.text=source_.substr(start,position_-start); tokens.push_back(token); continue;
            }
            if (std::iswdigit(c) || (c == L'.' && position_ < source_.size() && std::iswdigit(source_[position_]))) {
                const size_t start=position_-1;
                const auto consumeDigits=[&](int radix){
                    while(position_<source_.size()){
                        const auto character=source_[position_];
                        const int digit=character>=L'0'&&character<=L'9'?character-L'0':
                            character>=L'a'&&character<=L'f'?character-L'a'+10:
                            character>=L'A'&&character<=L'F'?character-L'A'+10:-1;
                        if(character==L'_'||(digit>=0&&digit<radix)){++position_;continue;}
                        break;
                    }
                };
                bool integer=true;int radix=10;
                if(c==L'0'&&position_<source_.size()&&
                   (source_[position_]==L'x'||source_[position_]==L'X'||
                    source_[position_]==L'b'||source_[position_]==L'B'||
                    source_[position_]==L'o'||source_[position_]==L'O')){
                    const auto prefix=source_[position_++];radix=prefix==L'x'||prefix==L'X'?16:prefix==L'b'||prefix==L'B'?2:8;
                    consumeDigits(radix);
                }else{
                    if(c!=L'.')consumeDigits(10);
                    if(c==L'.'||(position_<source_.size()&&source_[position_]==L'.')){
                        integer=false;if(c!=L'.')++position_;consumeDigits(10);
                    }
                    if(position_<source_.size()&&(source_[position_]==L'e'||source_[position_]==L'E')){
                        integer=false;++position_;
                        if(position_<source_.size()&&(source_[position_]==L'+'||source_[position_]==L'-'))++position_;
                        consumeDigits(10);
                    }
                }
                if(integer&&position_<source_.size()&&source_[position_]==L'n')++position_;
                token.kind=TokenKind::Number; token.text=source_.substr(start,position_-start);
                auto numeric=token.text;numeric.erase(std::remove(numeric.begin(),numeric.end(),L'_'),numeric.end());
                if(!numeric.empty()&&numeric.back()==L'n')numeric.pop_back();size_t used=0;
                if(radix!=10){
                    unsigned long long parsed=0;
                    const auto digits=numeric.substr(2);
                    if(TryParseUnsignedInteger(digits,parsed,&used,radix)&&used==digits.size())
                        token.number=static_cast<double>(parsed);
                }else{
                    double parsed=0;if(TryParseDouble(numeric,parsed,&used)&&used==numeric.size())token.number=parsed;
                }
                tokens.push_back(token); continue;
            }
            if (c == L'\'' || c == L'"') { token.kind=TokenKind::String; token.text=ReadString(c); tokens.push_back(token); continue; }
            if (c == L'`') { token.kind=TokenKind::Template; token.text=ReadTemplate(); tokens.push_back(token); continue; }
            auto push=[&](TokenKind k){token.kind=k;token.text=std::wstring(1,c);tokens.push_back(token);};
            switch(c) {
            case L'(':push(TokenKind::LeftParen);break; case L')':push(TokenKind::RightParen);break;
            case L'{':push(TokenKind::LeftBrace);break; case L'}':push(TokenKind::RightBrace);break;
            case L'[':push(TokenKind::LeftBracket);break; case L']':push(TokenKind::RightBracket);break;
            case L'.':
                if(position_+1<source_.size()&&source_[position_]==L'.'&&source_[position_+1]==L'.'){
                    position_+=2;token.kind=TokenKind::Ellipsis;token.text=L"...";tokens.push_back(token);
                }else push(TokenKind::Dot);
                break;
            case L',':push(TokenKind::Comma);break;
            case L':':push(TokenKind::Colon);break; case L';':push(TokenKind::Semicolon);break;
            case L'?':
                if(Match(L'?')){const bool assign=Match(L'=');token.kind=assign?TokenKind::NullishAssign:TokenKind::Nullish;token.text=assign?L"??=":L"??";tokens.push_back(token);}
                else if(position_<source_.size()&&source_[position_]==L'.'&&
                        (position_+1>=source_.size()||!std::iswdigit(source_[position_+1]))){
                    ++position_;token.kind=TokenKind::OptionalChain;token.text=L"?.";tokens.push_back(token);
                }
                else push(TokenKind::Question);
                break;
            case L'+':if(Match(L'+'))push(TokenKind::PlusPlus);else if(Match(L'='))push(TokenKind::PlusAssign);else push(TokenKind::Plus);break;
            case L'-':if(Match(L'-'))push(TokenKind::MinusMinus);else if(Match(L'='))push(TokenKind::MinusAssign);else push(TokenKind::Minus);break;
            case L'*':
                if(Match(L'*')){if(Match(L'='))push(TokenKind::ExponentAssign);else push(TokenKind::Exponent);}
                else if(Match(L'='))push(TokenKind::StarAssign);else push(TokenKind::Star);break;
            case L'/':{
                const auto canEndExpression=[&](){
                    if(tokens.empty())return false;const auto& previous=tokens.back();
                    if(previous.kind==TokenKind::Identifier)return previous.text!=L"return"&&previous.text!=L"throw"&&previous.text!=L"case";
                    if(previous.kind==TokenKind::RightParen){
                        int depth=0;
                        for(size_t index=tokens.size();index>0;--index){
                            const auto& token=tokens[index-1];
                            if(token.kind==TokenKind::RightParen)++depth;
                            else if(token.kind==TokenKind::LeftParen&&--depth==0){
                                if(index>1&&tokens[index-2].kind==TokenKind::Identifier){
                                    const auto& word=tokens[index-2].text;
                                    if(word==L"if"||word==L"while"||word==L"for"||word==L"with"||word==L"switch"||word==L"catch")return false;
                                }
                                break;
                            }
                        }
                        return true;
                    }
                    return previous.kind==TokenKind::Number||previous.kind==TokenKind::String||
                           previous.kind==TokenKind::Template||previous.kind==TokenKind::RegExp||
                           previous.kind==TokenKind::RightBracket;
                };
                if(canEndExpression()){if(Match(L'='))push(TokenKind::SlashAssign);else push(TokenKind::Slash);break;}
                std::wstring pattern;bool escaped=false,inCharacterClass=false;
                while(position_<source_.size()){
                    const wchar_t item=source_[position_++];
                    if(!escaped&&item==L'[')inCharacterClass=true;
                    else if(!escaped&&item==L']')inCharacterClass=false;
                    else if(!escaped&&!inCharacterClass&&item==L'/')break;
                    pattern+=item;
                    if(escaped)escaped=false;else escaped=item==L'\\';
                }
                std::wstring flags;while(position_<source_.size()&&std::iswalpha(source_[position_]))flags+=source_[position_++];
                token.kind=TokenKind::RegExp;token.text=DecodeRegexUnicodeEscapes(pattern);token.extra=std::move(flags);tokens.push_back(std::move(token));break;
            }
            case L'%':if(Match(L'='))push(TokenKind::PercentAssign);else push(TokenKind::Percent);break;
            case L'!':
                if (Match(L'=')) { const bool strict=Match(L'='); token.kind=strict?TokenKind::StrictNotEqual:TokenKind::NotEqual; token.text=strict?L"!==":L"!="; tokens.push_back(token); }
                else push(TokenKind::Bang); break;
            case L'=':
                if (Match(L'>')) { token.kind=TokenKind::Arrow;token.text=L"=>";tokens.push_back(token); }
                else if (Match(L'=')) { const bool strict=Match(L'=');token.kind=strict?TokenKind::StrictEqual:TokenKind::Equal;token.text=strict?L"===":L"==";tokens.push_back(token); }
                else push(TokenKind::Assign); break;
            case L'<':
                if(Match(L'<')){if(Match(L'='))push(TokenKind::ShiftLeftAssign);else push(TokenKind::ShiftLeft);}
                else if(Match(L'=')){token.kind=TokenKind::LessEqual;tokens.push_back(token);}else push(TokenKind::Less);break;
            case L'>':
                if(Match(L'>')){
                    if(Match(L'>')){if(Match(L'='))push(TokenKind::UnsignedShiftRightAssign);else push(TokenKind::UnsignedShiftRight);}
                    else if(Match(L'='))push(TokenKind::ShiftRightAssign);else push(TokenKind::ShiftRight);
                }else if(Match(L'=')){token.kind=TokenKind::GreaterEqual;tokens.push_back(token);}else push(TokenKind::Greater);break;
            case L'&':
                if(Match(L'&')){if(Match(L'='))push(TokenKind::AndAssign);else push(TokenKind::And);}
                else if(Match(L'='))push(TokenKind::BitAndAssign);else push(TokenKind::BitAnd);break;
            case L'|':
                if(Match(L'|')){if(Match(L'='))push(TokenKind::OrAssign);else push(TokenKind::Or);}
                else if(Match(L'='))push(TokenKind::BitOrAssign);else push(TokenKind::BitOr);break;
            case L'^':if(Match(L'='))push(TokenKind::BitXorAssign);else push(TokenKind::BitXor);break;
            case L'~':push(TokenKind::BitNot);break;
            default: break;
            }
        }
        return tokens;
    }
private:
    bool Match(wchar_t c){if(position_<source_.size()&&source_[position_]==c){++position_;return true;}return false;}
    void SkipTrivia(){
        for(;;){
            while(position_<source_.size()&&std::iswspace(source_[position_])){if(source_[position_++]==L'\n')++line_;}
            if(position_+1<source_.size()&&source_[position_]==L'/'&&source_[position_+1]==L'/'){
                position_+=2;while(position_<source_.size()&&source_[position_]!=L'\n')++position_;continue;
            }
            if(position_+1<source_.size()&&source_[position_]==L'/'&&source_[position_+1]==L'*'){
                position_+=2;while(position_+1<source_.size()&&!(source_[position_]==L'*'&&source_[position_+1]==L'/')){if(source_[position_++]==L'\n')++line_;}if(position_+1<source_.size())position_+=2;continue;
            }
            break;
        }
    }
    void AppendEscape(std::wstring& out){
        if(position_>=source_.size())return;
        const wchar_t escaped=source_[position_++];
        switch(escaped){
        case L'n':out+=L'\n';return;case L'r':out+=L'\r';return;
        case L't':out+=L'\t';return;case L'b':out+=L'\b';return;
        case L'f':out+=L'\f';return;case L'v':out+=L'\v';return;
        case L'0':out+=L'\0';return;
        case L'\n':++line_;return;
        case L'\r':if(position_<source_.size()&&source_[position_]==L'\n')++position_;++line_;return;
        case L'x':{
            if(position_+2<=source_.size()){
                const int high=HexDigitValue(source_[position_]),low=HexDigitValue(source_[position_+1]);
                if(high>=0&&low>=0){out+=static_cast<wchar_t>((high<<4)|low);position_+=2;return;}
            }
            out+=L'x';return;
        }
        case L'u':{
            size_t unicodePosition=position_-1;std::uint32_t codePoint=0;
            if(ParseUnicodeEscape(source_,unicodePosition,codePoint)){
                AppendCodePoint(out,codePoint);position_=unicodePosition;return;
            }
            out+=L'u';return;
        }
        default:out+=escaped;return;
        }
    }
    std::wstring ReadString(wchar_t quote){
        std::wstring out;
        while(position_<source_.size()){
            wchar_t c=source_[position_++]; if(c==quote)break;
            if(c==L'\\')AppendEscape(out);else{if(c==L'\n')++line_;out+=c;}
        }return out;
    }
    std::wstring ReadTemplate(){
        std::wstring out; int interpolation=0;
        while(position_<source_.size()){
            wchar_t c=source_[position_++];
            if(c==L'\\'&&position_<source_.size()){
                if(interpolation==0)AppendEscape(out);
                else{out+=c;out+=source_[position_++];}
                continue;
            }
            if(c==L'`'&&interpolation==0)break;
            if(c==L'$'&&position_<source_.size()&&source_[position_]==L'{'){out+=c;out+=source_[position_++];++interpolation;continue;}
            if(c==L'{'&&interpolation>0)++interpolation;
            if(c==L'}'&&interpolation>0)--interpolation;
            out+=c;
        }return out;
    }
    const std::wstring& source_; size_t position_=0; int line_=1;
};

class Compiler {
public:
    Compiler(std::shared_ptr<Module> module, const std::wstring& source)
        : module_(std::move(module)), tokens_(Lexer(source).Scan()) {}
    Chunk CompileProgram(){ Chunk chunk; chunk_=&chunk;compilingStrict_=HasUseStrictDirective(position_); while(!AtEnd()) Statement(); Emit(Op::Undefined);Emit(Op::Return);return chunk; }
    Chunk CompileExpressionOnly(){Chunk chunk;chunk_=&chunk;Expression();Emit(Op::Return);return chunk;}
private:
    bool AtEnd()const{return Peek().kind==TokenKind::End;}
    const Token& Peek(int ahead=0)const{return tokens_[std::min(position_+static_cast<size_t>(ahead),tokens_.size()-1)];}
    const Token& Previous()const{return tokens_[position_-1];}
    bool Check(TokenKind kind)const{return Peek().kind==kind;}
    bool CheckWord(const wchar_t* word)const{return Check(TokenKind::Identifier)&&Peek().text==word;}
    Token Advance(){if(!AtEnd())++position_;return Previous();}
    bool Match(TokenKind kind){if(Check(kind)){Advance();return true;}return false;}
    bool MatchWord(const wchar_t* word){if(CheckWord(word)){Advance();return true;}return false;}
    Token Consume(TokenKind kind,const wchar_t* message){if(Check(kind))return Advance();throw Error(message);}
    Token ConsumeIdentifier(const wchar_t* message){return Consume(TokenKind::Identifier,message);}
    std::runtime_error Error(const wchar_t* message)const{
        std::wstringstream s;s<<L"JavaScript line "<<Peek().line<<L" offset "
                              <<Peek().offset<<L": "<<message;
        if(Peek().kind!=TokenKind::End&&!Peek().text.empty()){
            s<<L" near '"<<Peek().text.substr(0,48)<<L"'";
            if(position_>0&&!Previous().text.empty())s<<L" after '"<<Previous().text.substr(0,48)<<L"'";
            if(Peek(1).kind!=TokenKind::End&&!Peek(1).text.empty())s<<L" before '"<<Peek(1).text.substr(0,48)<<L"'";
        }
        return std::runtime_error(WideToUtf8(s.str()));
    }
    int Emit(Op op,int argument=0,std::wstring text=L""){
        const auto& token=position_?Previous():Peek();
        chunk_->code.push_back({op,argument,std::move(text),token.line,token.offset,
                                static_cast<int>(blockDepth_)});
        return static_cast<int>(chunk_->code.size()-1);
    }
    int Jump(Op op){return Emit(op,-1);}
    void Patch(int index){chunk_->code[index].argument=static_cast<int>(chunk_->code.size());}
    int Constant(Value v){chunk_->constants.push_back(std::move(v));return static_cast<int>(chunk_->constants.size()-1);}
    void OptionalSemicolon(){Match(TokenKind::Semicolon);}
    bool HasUseStrictDirective(size_t position)const{
        while(position<tokens_.size()&&tokens_[position].kind==TokenKind::String){
            const auto& directive=tokens_[position];
            size_t next=position+1;
            const bool terminated=next>=tokens_.size()||tokens_[next].kind==TokenKind::Semicolon||
                tokens_[next].kind==TokenKind::RightBrace||tokens_[next].kind==TokenKind::End||
                tokens_[next].line>directive.line;
            if(!terminated)break;
            if(directive.text==L"use strict")return true;
            if(next<tokens_.size()&&tokens_[next].kind==TokenKind::Semicolon)++next;
            position=next;
        }
        return false;
    }

    void Statement(){
        if(Match(TokenKind::Semicolon))return;
        if(Check(TokenKind::Identifier)&&Peek(1).kind==TokenKind::Colon){LabeledStatement();return;}
        if(Match(TokenKind::LeftBrace)){
            Emit(Op::BeginBlock);++blockDepth_;
            while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();
            Consume(TokenKind::RightBrace,L"Expected '}'");--blockDepth_;Emit(Op::EndBlock);return;
        }
        if(CheckWord(L"async")&&Peek(1).kind==TokenKind::Identifier&&Peek(1).text==L"function"){
            Advance();Advance();const bool generator=Match(TokenKind::Star);
            FunctionDeclaration(true,generator);return;
        }
        if(MatchWord(L"class")){ClassDeclaration();return;}
        if(MatchWord(L"function")){const bool generator=Match(TokenKind::Star);FunctionDeclaration(false,generator);return;}
        if(MatchWord(L"const")||MatchWord(L"let")){VariableDeclaration(false);return;}
        if(MatchWord(L"var")){VariableDeclaration(true);return;}
        if(MatchWord(L"if")){IfStatement();return;}
        if(MatchWord(L"try")){TryStatement();return;}
        if(MatchWord(L"throw")){Expression();Emit(Op::ThrowValue);OptionalSemicolon();return;}
        if(MatchWord(L"switch")){SwitchStatement();return;}
        if(MatchWord(L"for")){ForStatement();return;}
        if(MatchWord(L"do")){DoWhileStatement();return;}
        if(MatchWord(L"while")){WhileStatement();return;}
        if(MatchWord(L"break")){
            const int line=Previous().line;
            if(Check(TokenKind::Identifier)&&Peek().line==line){
                const auto label=Advance().text;
                auto context=std::find_if(labels_.rbegin(),labels_.rend(),[&](const LabelContext& value){return value.name==label;});
                if(context==labels_.rend())throw Error(L"Unknown break label");
                context->breaks.push_back(Jump(Op::Jump));OptionalSemicolon();return;
            }
            if(controls_.empty())throw Error(L"break outside loop or switch");
            controls_.back().breaks.push_back(Jump(Op::Jump));OptionalSemicolon();return;
        }
        if(MatchWord(L"continue")){
            const int line=Previous().line;
            if(Check(TokenKind::Identifier)&&Peek().line==line){
                const auto label=Advance().text;
                auto context=std::find_if(labels_.rbegin(),labels_.rend(),[&](const LabelContext& value){return value.name==label;});
                if(context==labels_.rend()||context->continueTarget==-1)
                    throw Error(L"Unknown or non-iteration continue label");
                if(context->continueTarget>=0)Emit(Op::Jump,context->continueTarget);
                else context->continues.push_back(Jump(Op::Jump));
                OptionalSemicolon();return;
            }
            auto context=std::find_if(controls_.rbegin(),controls_.rend(),[](const ControlContext& value){return value.continueTarget!=-1;});
            if(context==controls_.rend())throw Error(L"continue outside loop");
            if(context->continueTarget>=0)Emit(Op::Jump,context->continueTarget);
            else context->continues.push_back(Jump(Op::Jump));
            OptionalSemicolon();return;
        }
        if(MatchWord(L"return")){if(Check(TokenKind::Semicolon)||Check(TokenKind::RightBrace))Emit(Op::Undefined);else Expression();Emit(Op::Return);OptionalSemicolon();return;}
        Expression();Emit(Op::Pop);OptionalSemicolon();
    }
    struct LabelContext {
        std::wstring name;
        // -1 is a non-iteration label, -3 is an iteration label waiting for
        // its loop compiler, and -2 is a do-while whose condition is not yet emitted.
        int continueTarget=-1;
        std::vector<int> breaks;
        std::vector<int> continues;
    };
    bool NextLabeledStatementIsIteration()const{
        size_t cursor=position_;
        while(cursor+1<tokens_.size()&&tokens_[cursor].kind==TokenKind::Identifier&&
              tokens_[cursor+1].kind==TokenKind::Colon)cursor+=2;
        if(cursor>=tokens_.size()||tokens_[cursor].kind!=TokenKind::Identifier)return false;
        const auto& word=tokens_[cursor].text;
        return word==L"for"||word==L"while"||word==L"do";
    }
    void LabeledStatement(){
        const auto name=Advance().text;Consume(TokenKind::Colon,L"Expected ':' after label");
        if(std::any_of(labels_.begin(),labels_.end(),[&](const LabelContext& value){return value.name==name;}))
            throw Error(L"Duplicate statement label");
        LabelContext context;context.name=name;
        if(NextLabeledStatementIsIteration())context.continueTarget=-3;
        labels_.push_back(std::move(context));
        Statement();
        auto completed=std::move(labels_.back());labels_.pop_back();
        for(int index:completed.breaks)Patch(index);
        if(!completed.continues.empty()){
            if(completed.continueTarget<0)throw Error(L"Unresolved continue label");
            for(int index:completed.continues)chunk_->code[index].argument=completed.continueTarget;
        }
    }
    std::vector<size_t> BindAttachedIterationLabels(int target){
        std::vector<size_t> attached;
        for(size_t index=labels_.size();index>0;--index){
            auto& label=labels_[index-1];
            if(label.continueTarget!=-3)break;
            label.continueTarget=target;attached.push_back(index-1);
        }
        return attached;
    }
    void ResolveAttachedIterationLabels(const std::vector<size_t>& attached,int target){
        for(const auto index:attached){
            auto& label=labels_[index];label.continueTarget=target;
            for(int jump:label.continues)chunk_->code[jump].argument=target;
            label.continues.clear();
        }
    }
    void TryStatement(){
        const int handlerIndex=static_cast<int>(chunk_->handlers.size());
        chunk_->handlers.emplace_back();
        Chunk::ExceptionHandler handler;
        handler.tryStart=chunk_->code.size();
        Statement();
        handler.tryEnd=chunk_->code.size();
        const int afterTry=Jump(Op::Jump);
        int afterCatch=-1;
        if(MatchWord(L"catch")){
            handler.hasCatch=true;
            if(Match(TokenKind::LeftParen)){handler.catchName=ConsumeIdentifier(L"Expected catch variable").text;Consume(TokenKind::RightParen,L"Expected ')'");}
            handler.catchStart=chunk_->code.size();
            Statement();
            handler.catchEnd=chunk_->code.size();
            Emit(Op::LeaveCatch,handlerIndex);
            afterCatch=Jump(Op::Jump);
        }
        if(MatchWord(L"finally")){
            handler.hasFinally=true;
            handler.finallyStart=chunk_->code.size();
            Statement();
            Emit(Op::EndFinally,handlerIndex);
            handler.finallyEnd=chunk_->code.size();
        }
        if(!handler.hasCatch&&!handler.hasFinally)throw Error(L"Expected catch or finally");
        handler.end=chunk_->code.size();
        chunk_->code[afterTry].argument=static_cast<int>(handler.hasFinally?handler.finallyStart:handler.end);
        if(afterCatch>=0)chunk_->code[afterCatch].argument=static_cast<int>(handler.hasFinally?handler.finallyStart:handler.end);
        chunk_->handlers[handlerIndex]=std::move(handler);
    }
    void FunctionDeclaration(bool isAsync=false,bool isGenerator=false){
        const auto name=ConsumeIdentifier(L"Expected function name").text;
        // A declaration's name is the binding installed in its surrounding
        // scope.  Unlike a named function expression it must not receive a
        // second, inner self-name binding: assignments such as
        // `function table(){ table = memoized; }` replace the declaration.
        const int index=ParseFunction(false,L"",isAsync,name,false,isGenerator,false);
        if(blockDepth_==0)chunk_->functionDeclarations.push_back({name,index});
        else{Emit(Op::MakeFunction,index);Emit(Op::Declare,0,name);}
    }
    void ClassDeclaration(){
        const auto name=ConsumeIdentifier(L"Expected class name").text;
        ClassExpression();
        Emit(Op::Declare,0,name);
    }
    void ClassExpression(){
        // A class expression may be anonymous (`new class { ... }`) or carry
        // a local name.  Its method bodies are compiled into prototypes just
        // like a declaration; they must never be interpreted as an enclosing
        // statement block during construction.
        if(Check(TokenKind::Identifier)&&
           (Peek(1).kind==TokenKind::LeftBrace||Peek(1).text==L"extends"))Advance();
        const bool hasBase=MatchWord(L"extends");
        const auto baseName=hasBase?L"$class_base_"+std::to_wstring(hiddenVariable_++):L"";
        if(hasBase){Assignment();Emit(Op::Duplicate);Emit(Op::Declare,0,baseName);}
        Consume(TokenKind::LeftBrace,L"Expected '{' after class name");
        Emit(Op::NewObject);
        if(hasBase)Emit(Op::ObjectSetPrototype);
        Emit(Op::TrueValue);Emit(Op::ObjectSet,0,L"$class");classBases_.push_back(baseName);
        while(!Check(TokenKind::RightBrace)&&!AtEnd()){
            if(Match(TokenKind::Semicolon))continue;
            const bool isStatic=MatchWord(L"static");
            const bool isAsync=MatchWord(L"async");
            const bool isGenerator=Match(TokenKind::Star);
            bool isGetter=false,isSetter=false;
            if(!isAsync&&!isGenerator&&CheckWord(L"get")&&
               ((Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::LeftParen)||
                Peek(1).kind==TokenKind::LeftBracket)){Advance();isGetter=true;}
            else if(!isAsync&&!isGenerator&&CheckWord(L"set")&&
                    ((Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::LeftParen)||
                     Peek(1).kind==TokenKind::LeftBracket)){Advance();isSetter=true;}
            const bool computed=Match(TokenKind::LeftBracket);
            bool computedMethod=false;
            if(computed){
                size_t cursor=position_;int bracketDepth=1;
                while(cursor<tokens_.size()&&bracketDepth>0){
                    if(tokens_[cursor].kind==TokenKind::LeftBracket)++bracketDepth;
                    else if(tokens_[cursor].kind==TokenKind::RightBracket)--bracketDepth;
                    ++cursor;
                }
                computedMethod=cursor<tokens_.size()&&tokens_[cursor].kind==TokenKind::LeftParen;
            }
            std::wstring member;
            std::wstring prefix=isGetter?L"$get:":isSetter?L"$set:":isStatic?L"":
                computed&& !computedMethod?L"$field:":L"$method:";
            if(computed){
                if(!prefix.empty())Emit(Op::Constant,Constant(Value::String(prefix)));
                Expression();
                Consume(TokenKind::RightBracket,L"Expected ']' after computed class member name");
                if(!prefix.empty())Emit(Op::Add);
            }else member=ConsumeIdentifier(L"Expected class member name").text;
            if(Check(TokenKind::LeftParen)){
                const int index=ParseFunction(false,L"",isAsync,L"",true,isGenerator);
                Emit(Op::MakeFunction,index);
                if(computed)Emit(Op::ObjectSetComputed);
                else Emit(Op::ObjectSet,0,prefix+member);
            }else{
                if(isAsync||isGenerator)throw Error(L"Expected '(' after class method name");
                if(isStatic){
                    if(Match(TokenKind::Assign))Assignment();else Emit(Op::Undefined);
                    if(computed)Emit(Op::ObjectSetComputed);else Emit(Op::ObjectSet,0,member);
                    OptionalSemicolon();
                }else{
                    // Store instance field initializers as zero-argument
                    // functions closed over the class definition environment.
                    // Construction evaluates them with the new instance as
                    // `this`, matching public and private class-field timing.
                    auto initializer=std::make_shared<Prototype>();initializer->isStrict=true;
                    Chunk* outer=chunk_;const bool outerAsync=compilingAsync_,outerGenerator=compilingGenerator_,outerStrict=compilingStrict_;
                    const size_t outerBlockDepth=blockDepth_;
                    chunk_=&initializer->chunk;compilingAsync_=false;compilingGenerator_=false;compilingStrict_=true;blockDepth_=0;
                    if(Match(TokenKind::Assign))Assignment();else Emit(Op::Undefined);
                    Emit(Op::Return);OptionalSemicolon();
                    chunk_=outer;compilingAsync_=outerAsync;compilingGenerator_=outerGenerator;compilingStrict_=outerStrict;blockDepth_=outerBlockDepth;
                    module_->prototypes.push_back(initializer);
                    Emit(Op::MakeFunction,static_cast<int>(module_->prototypes.size()-1));
                    if(computed)Emit(Op::ObjectSetComputed);
                    else Emit(Op::ObjectSet,0,L"$field:"+member);
                }
            }
        }
        Consume(TokenKind::RightBrace,L"Expected '}' after class body");classBases_.pop_back();
    }
    void AddVariableDeclaration(const std::wstring& name){
        if(std::find(chunk_->variableDeclarations.begin(),chunk_->variableDeclarations.end(),name)==
           chunk_->variableDeclarations.end())chunk_->variableDeclarations.push_back(name);
    }
    void VariableDeclaration(bool functionScoped){
        do{
            if(Check(TokenKind::LeftBracket)||Check(TokenKind::LeftBrace)){
                const bool indexed=Match(TokenKind::LeftBracket);
                if(!indexed)Consume(TokenKind::LeftBrace,L"Expected '{'");
                struct BindingKey { std::wstring property;int expression=-1,defaultExpression=-1; };
                struct Binding { std::vector<BindingKey> path;std::wstring name;int defaultExpression=-1; };
                std::vector<Binding> bindings;
                std::function<void(bool,std::vector<BindingKey>)> parsePattern;
                parsePattern=[&](bool arrayPattern,std::vector<BindingKey> prefix){
                    const auto end=arrayPattern?TokenKind::RightBracket:TokenKind::RightBrace;
                    size_t arrayIndex=0;
                    while(!Check(end)&&!AtEnd()){
                        if(arrayPattern&&Match(TokenKind::Comma)){++arrayIndex;continue;}
                        BindingKey key;bool hasColon=false;
                        if(arrayPattern)key.property=std::to_wstring(arrayIndex++);
                        else if(Match(TokenKind::LeftBracket)){
                            auto expression=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=expression.get();
                            Expression();Emit(Op::Return);chunk_=outer;
                            Consume(TokenKind::RightBracket,L"Expected ']' after computed binding key");
                            key.expression=static_cast<int>(outer->embeddedExpressions.size());
                            outer->embeddedExpressions.push_back(std::move(expression));
                            Consume(TokenKind::Colon,L"Expected ':' after computed binding key");hasColon=true;
                        }else key.property=ConsumeIdentifier(L"Expected property name").text;
                        if(!arrayPattern&&!hasColon)hasColon=Match(TokenKind::Colon);
                        auto path=prefix;path.push_back(key);
                        const auto nestedStep=path.size()-1,bindingStart=bindings.size();
                        bool nested=false;
                        if(Match(TokenKind::LeftBracket)){nested=true;parsePattern(true,path);}
                        else if(Match(TokenKind::LeftBrace)){nested=true;parsePattern(false,path);}
                        if(nested){
                            if(Match(TokenKind::Assign)){
                                auto expression=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=expression.get();
                                Assignment();Emit(Op::Return);chunk_=outer;
                                const int defaultExpression=static_cast<int>(outer->embeddedExpressions.size());
                                outer->embeddedExpressions.push_back(std::move(expression));
                                for(size_t index=bindingStart;index<bindings.size();++index)
                                    bindings[index].path[nestedStep].defaultExpression=defaultExpression;
                            }
                        }else{
                            const auto name=arrayPattern||hasColon?
                                ConsumeIdentifier(L"Expected binding name").text:key.property;
                            int defaultExpression=-1;
                            if(Match(TokenKind::Assign)){
                                auto expression=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=expression.get();
                                Assignment();Emit(Op::Return);chunk_=outer;
                                defaultExpression=static_cast<int>(outer->embeddedExpressions.size());
                                outer->embeddedExpressions.push_back(std::move(expression));
                            }
                            bindings.push_back({std::move(path),name,defaultExpression});
                        }
                        if(!Match(TokenKind::Comma))break;
                    }
                    Consume(end,L"Expected closing destructuring bracket");
                };
                parsePattern(indexed,{});
                if(functionScoped)for(const auto& binding:bindings)AddVariableDeclaration(binding.name);
                if(Match(TokenKind::Assign))Assignment();else Emit(Op::Undefined);
                const auto source=L"$binding_"+std::to_wstring(hiddenVariable_++);
                Emit(Op::Declare,0,source);
                for(const auto& binding:bindings){
                    Emit(Op::LoadReference,0,source);
                    for(const auto& key:binding.path){
                        if(key.expression>=0){Emit(Op::EvaluateEmbeddedExpression,key.expression);Emit(Op::GetIndex);}
                        else Emit(Op::GetProperty,0,key.property);
                        if(key.defaultExpression>=0)Emit(Op::DefaultIfUndefined,key.defaultExpression);
                    }
                    if(binding.defaultExpression>=0)Emit(Op::DefaultIfUndefined,binding.defaultExpression);
                    Emit(Op::Declare,functionScoped?1:0,binding.name);
                }
                continue;
            }
            const auto name=ConsumeIdentifier(L"Expected variable name").text;
            if(functionScoped)AddVariableDeclaration(name);
            if(Match(TokenKind::Assign)){Assignment();Emit(Op::Declare,functionScoped?1:0,name);}
            else if(!functionScoped){Emit(Op::Undefined);Emit(Op::Declare,0,name);}
        }while(Match(TokenKind::Comma));OptionalSemicolon();
    }
    void IfStatement(){
        Consume(TokenKind::LeftParen,L"Expected '('");Expression();Consume(TokenKind::RightParen,L"Expected ')'");
        const int falseJump=Jump(Op::JumpFalse);Statement();
        if(MatchWord(L"else")){const int end=Jump(Op::Jump);Patch(falseJump);Statement();Patch(end);}else Patch(falseJump);
    }
    struct ControlContext { int continueTarget=-1;std::vector<int> breaks;std::vector<int> continues; };
    void FinishControl(){
        auto context=std::move(controls_.back());controls_.pop_back();
        for(int index:context.breaks)Patch(index);
        for(int index:context.continues)chunk_->code[index].argument=context.continueTarget;
    }
    void SwitchStatement(){
        struct Clause { bool isDefault=false;size_t expressionStart=0,expressionEnd=0,bodyStart=0,bodyEnd=0;int entryJump=-1,bodyEntry=-1; };
        Consume(TokenKind::LeftParen,L"Expected '(' after switch");Expression();Consume(TokenKind::RightParen,L"Expected ')' after switch value");
        const auto valueName=L"$switch_value_"+std::to_wstring(hiddenVariable_++);Emit(Op::Declare,0,valueName);
        Consume(TokenKind::LeftBrace,L"Expected '{' after switch value");
        std::vector<Clause> clauses;
        while(!Check(TokenKind::RightBrace)&&!AtEnd()){
            Clause clause;
            if(MatchWord(L"case")){
                clause.expressionStart=position_;int parens=0,brackets=0,braces=0,questions=0;
                while(!AtEnd()){
                    const auto kind=Peek().kind;
                    if(kind==TokenKind::LeftParen)++parens;else if(kind==TokenKind::RightParen)--parens;
                    else if(kind==TokenKind::LeftBracket)++brackets;else if(kind==TokenKind::RightBracket)--brackets;
                    else if(kind==TokenKind::LeftBrace)++braces;else if(kind==TokenKind::RightBrace)--braces;
                    else if(parens==0&&brackets==0&&braces==0&&kind==TokenKind::Question)++questions;
                    else if(parens==0&&brackets==0&&braces==0&&kind==TokenKind::Colon){if(questions>0)--questions;else break;}
                    Advance();
                }
                clause.expressionEnd=position_;Consume(TokenKind::Colon,L"Expected ':' after switch case");
            }else if(MatchWord(L"default")){
                clause.isDefault=true;Consume(TokenKind::Colon,L"Expected ':' after switch default");
            }else throw Error(L"Expected case or default in switch");
            clause.bodyStart=position_;int braces=0;
            while(!AtEnd()){
                if(braces==0&&(Check(TokenKind::RightBrace)||CheckWord(L"case")||CheckWord(L"default")))break;
                if(Check(TokenKind::LeftBrace))++braces;else if(Check(TokenKind::RightBrace))--braces;
                Advance();
            }
            clause.bodyEnd=position_;clauses.push_back(clause);
        }
        Consume(TokenKind::RightBrace,L"Expected '}' after switch");const auto afterSwitch=position_;

        int defaultIndex=-1;
        for(size_t index=0;index<clauses.size();++index){
            auto& clause=clauses[index];if(clause.isDefault){defaultIndex=static_cast<int>(index);continue;}
            Emit(Op::LoadReference,0,valueName);position_=clause.expressionStart;Expression();
            if(position_!=clause.expressionEnd)throw Error(L"Invalid switch case expression");
            Emit(Op::StrictEqual);const int noMatch=Jump(Op::JumpFalse);clause.entryJump=Jump(Op::Jump);Patch(noMatch);
        }
        const int noCaseMatched=Jump(Op::Jump);
        controls_.push_back({-1,{}});
        for(auto& clause:clauses){
            clause.bodyEntry=static_cast<int>(chunk_->code.size());
            if(clause.entryJump>=0)chunk_->code[clause.entryJump].argument=clause.bodyEntry;
            position_=clause.bodyStart;while(position_<clause.bodyEnd)Statement();
            if(position_!=clause.bodyEnd)throw Error(L"Invalid switch case body");
        }
        if(defaultIndex>=0)chunk_->code[noCaseMatched].argument=clauses[static_cast<size_t>(defaultIndex)].bodyEntry;
        else Patch(noCaseMatched);
        FinishControl();position_=afterSwitch;
    }
    void WhileStatement(){
        Consume(TokenKind::LeftParen,L"Expected '('");const int condition=static_cast<int>(chunk_->code.size());
        Expression();Consume(TokenKind::RightParen,L"Expected ')'");const int done=Jump(Op::JumpFalse);
        BindAttachedIterationLabels(condition);
        controls_.push_back({condition,{}});Statement();Emit(Op::Jump,condition);Patch(done);FinishControl();
    }
    void DoWhileStatement(){
        const int body=static_cast<int>(chunk_->code.size());
        const auto attachedLabels=BindAttachedIterationLabels(-2);
        controls_.push_back({-2,{}});Statement();
        if(!MatchWord(L"while"))throw Error(L"Expected 'while' after do-while body");
        const int condition=static_cast<int>(chunk_->code.size());
        controls_.back().continueTarget=condition;
        ResolveAttachedIterationLabels(attachedLabels,condition);
        Consume(TokenKind::LeftParen,L"Expected '(' after while");Expression();
        Consume(TokenKind::RightParen,L"Expected ')' after do-while condition");
        const int done=Jump(Op::JumpFalse);Emit(Op::Jump,body);Patch(done);
        OptionalSemicolon();FinishControl();
    }
    void ForStatement(){
        Consume(TokenKind::LeftParen,L"Expected '('");
        const auto destructuredForEachAhead=[&]{
            const bool declaration=CheckWord(L"const")||CheckWord(L"let")||CheckWord(L"var");
            const int patternOffset=declaration?1:0;
            if(Peek(patternOffset).kind!=TokenKind::LeftBracket&&
               Peek(patternOffset).kind!=TokenKind::LeftBrace)return false;
            const auto open=Peek(patternOffset).kind;
            const auto close=open==TokenKind::LeftBracket?TokenKind::RightBracket:TokenKind::RightBrace;
            int depth=0;
            for(int ahead=patternOffset;;++ahead){
                const auto& token=Peek(ahead);if(token.kind==TokenKind::End)return false;
                if(token.kind==open)++depth;
                else if(token.kind==close&&--depth==0){const auto& operation=Peek(ahead+1);return operation.kind==TokenKind::Identifier&&(operation.text==L"of"||operation.text==L"in");}
            }
        };
        if(destructuredForEachAhead()){
            const bool declaresBindings=CheckWord(L"const")||CheckWord(L"let")||CheckWord(L"var");
            const bool functionScoped=declaresBindings&&Peek().text==L"var";
            if(declaresBindings)Advance();
            std::vector<std::pair<std::wstring,std::wstring>> bindings;
            const bool indexed=Match(TokenKind::LeftBracket);if(!indexed)Consume(TokenKind::LeftBrace,L"Expected destructuring binding");
            const auto end=indexed?TokenKind::RightBracket:TokenKind::RightBrace;size_t index=0;
            while(!Check(end)&&!AtEnd()){
                if(indexed&&Match(TokenKind::Comma)){++index;continue;}
                const auto property=indexed?std::to_wstring(index++):ConsumeIdentifier(L"Expected property name").text;
                auto name=indexed?ConsumeIdentifier(L"Expected element name").text:property;
                if(!indexed&&Match(TokenKind::Colon))name=ConsumeIdentifier(L"Expected binding name").text;
                bindings.push_back({property,name});if(declaresBindings&&functionScoped)AddVariableDeclaration(name);
                if(!Match(TokenKind::Comma))break;
            }
            Consume(end,L"Expected closing destructuring bracket");
            const auto operation=ConsumeIdentifier(L"Expected 'of' or 'in'").text;
            ForEachStatement(L"",operation==L"in",declaresBindings,bindings,functionScoped);return;
        }
        if((CheckWord(L"const")||CheckWord(L"let")||CheckWord(L"var"))&&
           Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::Identifier&&
           (Peek(2).text==L"of"||Peek(2).text==L"in")){
            const bool functionScoped=Peek().text==L"var";
            Advance();const auto itemName=Advance().text;const bool enumerateKeys=Advance().text==L"in";
            if(functionScoped)AddVariableDeclaration(itemName);
            ForEachStatement(itemName,enumerateKeys,true,{},functionScoped);return;
        }
        if(Check(TokenKind::Identifier)&&Peek(1).kind==TokenKind::Identifier&&
           (Peek(1).text==L"of"||Peek(1).text==L"in")){
            const auto itemName=Advance().text;const bool enumerateKeys=Advance().text==L"in";
            ForEachStatement(itemName,enumerateKeys,false,{},false);return;
        }
        if(!Match(TokenKind::Semicolon)){
            if(MatchWord(L"const")||MatchWord(L"let"))VariableDeclaration(false);
            else if(MatchWord(L"var"))VariableDeclaration(true);
            else{Expression();Emit(Op::Pop);Consume(TokenKind::Semicolon,L"Expected ';'");}
        }
        const int condition=static_cast<int>(chunk_->code.size());
        if(Check(TokenKind::Semicolon))Emit(Op::TrueValue);else Expression();
        Consume(TokenKind::Semicolon,L"Expected ';'");const int done=Jump(Op::JumpFalse);
        const int enterBody=Jump(Op::Jump);
        const int increment=static_cast<int>(chunk_->code.size());
        if(!Check(TokenKind::RightParen)){Expression();Emit(Op::Pop);}
        Consume(TokenKind::RightParen,L"Expected ')'");Emit(Op::Jump,condition);
        Patch(enterBody);BindAttachedIterationLabels(increment);
        controls_.push_back({increment,{}});Statement();Emit(Op::Jump,increment);Patch(done);FinishControl();
    }
    void ForEachStatement(const std::wstring& itemName,bool enumerateKeys,bool declareItem,
                          const std::vector<std::pair<std::wstring,std::wstring>>& bindings,
                          bool functionScoped){
        if(enumerateKeys)Expression();else Assignment();
        Consume(TokenKind::RightParen,enumerateKeys?L"Expected ')' after for-in object":L"Expected ')' after for-of iterable");
        if(enumerateKeys)Emit(Op::EnumerableKeys);
        const auto suffix=std::to_wstring(hiddenVariable_++);
        const auto valuesName=L"$for_values_"+suffix,indexName=L"$for_index_"+suffix;
        Emit(Op::Declare,0,valuesName);
        Emit(Op::Constant,Constant(Value::Number(0)));Emit(Op::Declare,0,indexName);

        const int condition=static_cast<int>(chunk_->code.size());
        Emit(Op::LoadReference,0,indexName);Emit(Op::LoadReference,0,valuesName);Emit(Op::GetProperty,0,L"length");Emit(Op::Less);
        const int done=Jump(Op::JumpFalse);const int enterBody=Jump(Op::Jump);
        const int increment=static_cast<int>(chunk_->code.size());
        Emit(Op::LoadReference,0,indexName);Emit(Op::PostIncrement);Emit(Op::Pop);Emit(Op::Jump,condition);

        Patch(enterBody);
        if(bindings.empty()){
            if(!declareItem)Emit(Op::LoadReference,0,itemName);
            Emit(Op::LoadReference,0,valuesName);Emit(Op::LoadReference,0,indexName);Emit(Op::GetIndex);
            if(declareItem)Emit(Op::Declare,functionScoped?1:0,itemName);else{Emit(Op::Assign);Emit(Op::Pop);}
        }else{
            const auto item=L"$for_item_"+suffix;
            Emit(Op::LoadReference,0,valuesName);Emit(Op::LoadReference,0,indexName);Emit(Op::GetIndex);Emit(Op::Declare,0,item);
            for(const auto& binding:bindings){
                if(!declareItem)Emit(Op::LoadReference,0,binding.second);
                Emit(Op::LoadReference,0,item);Emit(Op::GetProperty,0,binding.first);
                if(declareItem)Emit(Op::Declare,functionScoped?1:0,binding.second);
                else{Emit(Op::Assign);Emit(Op::Pop);}
            }
        }
        BindAttachedIterationLabels(increment);
        controls_.push_back({increment,{}});Statement();Emit(Op::Jump,increment);Patch(done);FinishControl();
    }
    void ParseParameters(const std::shared_ptr<Prototype>& prototype){
        if(!Check(TokenKind::RightParen))do{
            if(Match(TokenKind::Ellipsis)){
                prototype->restParameter=ConsumeIdentifier(L"Expected rest parameter name").text;
                if(Check(TokenKind::Comma))throw Error(L"Rest parameter must be last");
                break;
            }
            if(Match(TokenKind::LeftBracket)||Check(TokenKind::LeftBrace)){
                const bool indexed=Previous().kind==TokenKind::LeftBracket;
                if(!indexed)Consume(TokenKind::LeftBrace,L"Expected '{'");
                const auto end=indexed?TokenKind::RightBracket:TokenKind::RightBrace;
                const auto parameter=L"$parameter_"+std::to_wstring(prototype->parameters.size());
                prototype->parameters.push_back(parameter);prototype->parameterDefaults.push_back({});
                size_t index=0;
                while(!Check(end)&&!AtEnd()){
                    auto property=indexed?std::to_wstring(index++):ConsumeIdentifier(L"Expected property name").text;
                    auto name=indexed?ConsumeIdentifier(L"Expected element name").text:property;
                    if(!indexed&&Match(TokenKind::Colon))name=ConsumeIdentifier(L"Expected binding name").text;
                    std::shared_ptr<Chunk> bindingDefault;
                    if(Match(TokenKind::Assign)){
                        bindingDefault=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=bindingDefault.get();
                        Assignment();Emit(Op::Return);chunk_=outer;
                    }
                    prototype->bindings.push_back({parameter,property,name,indexed,std::move(bindingDefault)});
                    if(!Match(TokenKind::Comma))break;
                }
                Consume(end,L"Expected closing parameter bracket");
                if(Match(TokenKind::Assign)){
                    auto parameterDefault=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=parameterDefault.get();
                    Assignment();Emit(Op::Return);chunk_=outer;
                    prototype->parameterDefaults.back()=std::move(parameterDefault);
                }
                continue;
            }
            prototype->parameters.push_back(ConsumeIdentifier(L"Expected parameter").text);
            std::shared_ptr<Chunk> defaultValue;
            if(Match(TokenKind::Assign)){
                defaultValue=std::make_shared<Chunk>();Chunk* outer=chunk_;chunk_=defaultValue.get();
                Assignment();Emit(Op::Return);chunk_=outer;
            }
            prototype->parameterDefaults.push_back(std::move(defaultValue));
        }while(Match(TokenKind::Comma)&&!Check(TokenKind::RightParen));
        Consume(TokenKind::RightParen,L"Expected ')'");
    }
    int ParseFunction(bool arrow,const std::wstring& singleParameter,bool isAsync=false,
                      const std::wstring& functionName=L"",bool forceStrict=false,
                      bool isGenerator=false,bool createSelfBinding=true){
        auto prototype=std::make_shared<Prototype>();
        prototype->name=functionName;
        prototype->selfBindingName=createSelfBinding?functionName:L"";
        prototype->lexicalThis=arrow;
        prototype->isAsync=isAsync;
        prototype->isGenerator=isGenerator;
        if(!singleParameter.empty()){prototype->parameters.push_back(singleParameter);prototype->parameterDefaults.push_back({});}
        else{
            if(!arrow)Consume(TokenKind::LeftParen,L"Expected '('");
            ParseParameters(prototype);
        }
        Chunk* outer=chunk_;const bool outerAsync=compilingAsync_,outerGenerator=compilingGenerator_,outerStrict=compilingStrict_;const size_t outerBlockDepth=blockDepth_;
        chunk_=&prototype->chunk;compilingAsync_=isAsync;compilingGenerator_=isGenerator;blockDepth_=0;
        if(arrow&&!Check(TokenKind::LeftBrace)){
            prototype->isStrict=forceStrict||outerStrict;compilingStrict_=prototype->isStrict;
            Assignment();Emit(Op::Return);
        }else{
            Consume(TokenKind::LeftBrace,L"Expected function body");
            prototype->isStrict=forceStrict||outerStrict||HasUseStrictDirective(position_);
            compilingStrict_=prototype->isStrict;
            while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();Consume(TokenKind::RightBrace,L"Expected '}'");Emit(Op::Undefined);Emit(Op::Return);
        }
        chunk_=outer;compilingAsync_=outerAsync;compilingGenerator_=outerGenerator;compilingStrict_=outerStrict;blockDepth_=outerBlockDepth;module_->prototypes.push_back(prototype);return static_cast<int>(module_->prototypes.size()-1);
    }
    bool IsParenArrow()const{
        size_t i=position_;if(i>=tokens_.size()||tokens_[i].kind!=TokenKind::LeftParen)return false;
        int depth=0;for(;i<tokens_.size();++i){
            if(tokens_[i].kind==TokenKind::LeftParen)++depth;
            else if(tokens_[i].kind==TokenKind::RightParen&&--depth==0)
                return i+1<tokens_.size()&&tokens_[i+1].kind==TokenKind::Arrow;
        }
        return false;
    }
    void Expression(){Assignment();while(Match(TokenKind::Comma)){Emit(Op::Pop);Assignment();}}
    void Assignment(){
        Conditional();
        if(Match(TokenKind::Assign)){Assignment();Emit(Op::Assign);}
        else if(Match(TokenKind::NullishAssign)){
            Emit(Op::Duplicate);const int existing=Jump(Op::JumpNotNullishKeep);
            Assignment();Emit(Op::Assign);const int end=Jump(Op::Jump);
            Patch(existing);Emit(Op::Pop);Patch(end);
        }
        else if(Match(TokenKind::PlusAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Add);Emit(Op::Assign);}
        else if(Match(TokenKind::MinusAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Subtract);Emit(Op::Assign);}
        else if(Match(TokenKind::StarAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Multiply);Emit(Op::Assign);}
        else if(Match(TokenKind::SlashAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Divide);Emit(Op::Assign);}
        else if(Match(TokenKind::PercentAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Modulo);Emit(Op::Assign);}
        else if(Match(TokenKind::ExponentAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::Power);Emit(Op::Assign);}
        else if(Match(TokenKind::BitAndAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::BitwiseAnd);Emit(Op::Assign);}
        else if(Match(TokenKind::BitOrAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::BitwiseOr);Emit(Op::Assign);}
        else if(Match(TokenKind::BitXorAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::BitwiseXor);Emit(Op::Assign);}
        else if(Match(TokenKind::ShiftLeftAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::ShiftLeft);Emit(Op::Assign);}
        else if(Match(TokenKind::ShiftRightAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::ShiftRight);Emit(Op::Assign);}
        else if(Match(TokenKind::UnsignedShiftRightAssign)){Emit(Op::Duplicate);Assignment();Emit(Op::UnsignedShiftRight);Emit(Op::Assign);}
        else if(Match(TokenKind::AndAssign)){
            Emit(Op::Duplicate);const int existing=Jump(Op::JumpFalseKeep);
            Assignment();Emit(Op::Assign);const int end=Jump(Op::Jump);
            Patch(existing);Emit(Op::Pop);Patch(end);
        }
        else if(Match(TokenKind::OrAssign)){
            Emit(Op::Duplicate);const int existing=Jump(Op::JumpTrueKeep);
            Assignment();Emit(Op::Assign);const int end=Jump(Op::Jump);
            Patch(existing);Emit(Op::Pop);Patch(end);
        }
    }
    void Conditional(){
        Nullish();
        if(Match(TokenKind::Question)){const int no=Jump(Op::JumpFalse);Assignment();Consume(TokenKind::Colon,L"Expected ':'");const int end=Jump(Op::Jump);Patch(no);Assignment();Patch(end);}
    }
    void Nullish(){LogicalOr();while(Match(TokenKind::Nullish)){const int jump=Jump(Op::JumpNotNullishKeep);LogicalOr();Patch(jump);}}
    void LogicalOr(){LogicalAnd();while(Match(TokenKind::Or)){const int jump=Jump(Op::JumpTrueKeep);LogicalAnd();Patch(jump);}}
    void LogicalAnd(){BitwiseOr();while(Match(TokenKind::And)){const int jump=Jump(Op::JumpFalseKeep);BitwiseOr();Patch(jump);}}
    void BitwiseOr(){BitwiseXor();while(Match(TokenKind::BitOr)){BitwiseXor();Emit(Op::BitwiseOr);}}
    void BitwiseXor(){BitwiseAnd();while(Match(TokenKind::BitXor)){BitwiseAnd();Emit(Op::BitwiseXor);}}
    void BitwiseAnd(){Equality();while(Match(TokenKind::BitAnd)){Equality();Emit(Op::BitwiseAnd);}}
    void Equality(){Comparison();while(Check(TokenKind::Equal)||Check(TokenKind::NotEqual)||Check(TokenKind::StrictEqual)||Check(TokenKind::StrictNotEqual)){const auto op=Advance().kind;Comparison();Emit(op==TokenKind::Equal?Op::Equal:op==TokenKind::NotEqual?Op::NotEqual:op==TokenKind::StrictEqual?Op::StrictEqual:Op::StrictNotEqual);}}
    void Comparison(){
        Shift();
        while(Check(TokenKind::Less)||Check(TokenKind::LessEqual)||Check(TokenKind::Greater)||Check(TokenKind::GreaterEqual)||CheckWord(L"in")||CheckWord(L"instanceof")){
            if(MatchWord(L"in")){Shift();Emit(Op::InValue);continue;}
            if(MatchWord(L"instanceof")){Shift();Emit(Op::InstanceOf);continue;}
            const auto op=Advance().kind;Shift();Emit(op==TokenKind::Less?Op::Less:op==TokenKind::LessEqual?Op::LessEqual:op==TokenKind::Greater?Op::Greater:Op::GreaterEqual);
        }
    }
    void Shift(){Term();while(Check(TokenKind::ShiftLeft)||Check(TokenKind::ShiftRight)||Check(TokenKind::UnsignedShiftRight)){const auto op=Advance().kind;Term();Emit(op==TokenKind::ShiftLeft?Op::ShiftLeft:op==TokenKind::ShiftRight?Op::ShiftRight:Op::UnsignedShiftRight);}}
    void Term(){Factor();while(Check(TokenKind::Plus)||Check(TokenKind::Minus)){const auto op=Advance().kind;Factor();Emit(op==TokenKind::Plus?Op::Add:Op::Subtract);}}
    void Factor(){PowerExpression();while(Check(TokenKind::Star)||Check(TokenKind::Slash)||Check(TokenKind::Percent)){const auto op=Advance().kind;PowerExpression();Emit(op==TokenKind::Star?Op::Multiply:op==TokenKind::Slash?Op::Divide:Op::Modulo);}}
    void PowerExpression(){Unary();if(Match(TokenKind::Exponent)){PowerExpression();Emit(Op::Power);}}
    void Unary(){
        if(Match(TokenKind::Bang)){Unary();Emit(Op::Not);return;}if(Match(TokenKind::Minus)){Unary();Emit(Op::Negate);return;}
        if(Match(TokenKind::Plus)){Unary();Emit(Op::Positive);return;}
        if(Match(TokenKind::BitNot)){Unary();Emit(Op::BitwiseNot);return;}
        if(Match(TokenKind::PlusPlus)){Unary();Emit(Op::PreIncrement);return;}
        if(Match(TokenKind::MinusMinus)){Unary();Emit(Op::PreDecrement);return;}
        if(MatchWord(L"yield")){
            if(!compilingGenerator_)throw Error(L"yield outside generator function");
            const bool spread=Match(TokenKind::Star);
            if(Check(TokenKind::Semicolon)||Check(TokenKind::RightBrace))Emit(Op::Undefined);
            else Assignment();
            Emit(spread?Op::YieldSpread:Op::Yield);return;
        }
        if(MatchWord(L"await")){if(!compilingAsync_)throw Error(L"await outside async function");Unary();Emit(Op::Await);return;}
        if(MatchWord(L"delete")){Unary();Emit(Op::DeleteValue);return;}
        if(MatchWord(L"void")){Unary();Emit(Op::VoidValue);return;}
        if(MatchWord(L"new")){NewExpression();return;}
        if(MatchWord(L"typeof")){Unary();Emit(Op::TypeOf);return;}Postfix();
    }
    void Arguments(){
        Emit(Op::NewArray);
        if(!Check(TokenKind::RightParen))do{
            const bool spread=Match(TokenKind::Ellipsis);Assignment();Emit(spread?Op::ArraySpread:Op::ArrayPush);
        }while(Match(TokenKind::Comma)&&!Check(TokenKind::RightParen));
        Consume(TokenKind::RightParen,L"Expected ')'");
    }
    void Postfix(){
        Primary();
        std::vector<int> optionalChainEnds;
        for(;;){
            if(Match(TokenKind::Dot)){Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);}
            else if(Match(TokenKind::OptionalChain)){
                const int value=Jump(Op::JumpNotNullishKeep);
                Emit(Op::Undefined);optionalChainEnds.push_back(Jump(Op::Jump));Patch(value);
                if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
                else if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::CallArray);}
                else Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);
            }
            else if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
            else if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::CallArray);}
            else if(Match(TokenKind::Template))Emit(Op::TaggedTemplate,Constant(Value::String(Previous().text)));
            else if(Match(TokenKind::PlusPlus))Emit(Op::PostIncrement);
            else if(Match(TokenKind::MinusMinus))Emit(Op::PostDecrement);
            else break;
        }
        for(const auto end:optionalChainEnds)Patch(end);
    }
    void NewExpression(){
        Primary();
        while(true){
            if(Match(TokenKind::Dot))Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);
            else if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
            else break;
        }
        if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::ConstructArray);}else Emit(Op::Construct,0);
        for(;;){
            if(Match(TokenKind::Dot))Emit(Op::GetProperty,0,ConsumeIdentifier(L"Expected property name").text);
            else if(Match(TokenKind::LeftBracket)){Expression();Consume(TokenKind::RightBracket,L"Expected ']'");Emit(Op::GetIndex);}
            else if(Match(TokenKind::LeftParen)){Arguments();Emit(Op::CallArray);}
            else break;
        }
    }
    void Primary(){
        if(Match(TokenKind::Number)){Emit(Op::Constant,Constant(Value::Number(Previous().number)));return;}
        if(Match(TokenKind::String)){Emit(Op::Constant,Constant(Value::String(Previous().text)));return;}
        if(Match(TokenKind::Template)){Emit(Op::Template,Constant(Value::String(Previous().text)));return;}
        if(Match(TokenKind::RegExp)){auto expression=std::make_shared<Object>();expression->kind=ObjectKind::RegExp;expression->props[L"$pattern"]=Value::String(Previous().text);expression->props[L"$flags"]=Value::String(Previous().extra);Emit(Op::Constant,Constant(Value::FromObject(expression)));return;}
        if(MatchWord(L"true")){Emit(Op::TrueValue);return;}if(MatchWord(L"false")){Emit(Op::FalseValue);return;}
        if(MatchWord(L"null")){Emit(Op::Null);return;}if(MatchWord(L"undefined")){Emit(Op::Undefined);return;}
        if(MatchWord(L"super")){if(!classBases_.empty()&&!classBases_.back().empty())Emit(Op::LoadSuperReference,0,classBases_.back());else Emit(Op::LoadReference,0,L"super");return;}
        if(MatchWord(L"class")){ClassExpression();return;}
        if(MatchWord(L"async")){
            if(MatchWord(L"function")){const bool generator=Match(TokenKind::Star);const auto name=Check(TokenKind::Identifier)?Advance().text:L"";Emit(Op::MakeFunction,ParseFunction(false,L"",true,name,false,generator));return;}
            if(Check(TokenKind::Identifier)&&Peek(1).kind==TokenKind::Arrow){const auto name=Advance().text;Advance();Emit(Op::MakeFunction,ParseArrowSingle(name,true));return;}
            if(IsParenArrow()){Emit(Op::MakeFunction,ParseParenArrow(true));return;}
            Emit(Op::LoadReference,0,L"async");return;
        }
        if(MatchWord(L"function")){const bool generator=Match(TokenKind::Star);const auto name=Check(TokenKind::Identifier)?Advance().text:L"";Emit(Op::MakeFunction,ParseFunction(false,L"",false,name,false,generator));return;}
        if(IsParenArrow()){Emit(Op::MakeFunction,ParseParenArrow(false));return;}
        if(Match(TokenKind::LeftParen)){Expression();Consume(TokenKind::RightParen,L"Expected ')'");return;}
        if(Match(TokenKind::LeftBracket)){
            Emit(Op::NewArray);
            while(!Check(TokenKind::RightBracket)&&!AtEnd()){
                if(Match(TokenKind::Comma)){Emit(Op::Undefined);Emit(Op::ArrayPush);continue;}
                const bool spread=Match(TokenKind::Ellipsis);Assignment();Emit(spread?Op::ArraySpread:Op::ArrayPush);
                if(!Match(TokenKind::Comma))break;
            }
            Consume(TokenKind::RightBracket,L"Expected ']'");return;
        }
        if(Match(TokenKind::LeftBrace)){
            Emit(Op::NewObject);
            if(!Check(TokenKind::RightBrace))do{
                if(Match(TokenKind::Ellipsis)){Assignment();Emit(Op::ObjectSpread);continue;}
                if(Match(TokenKind::Star)){
                    if(Match(TokenKind::LeftBracket)){
                        Expression();Consume(TokenKind::RightBracket,L"Expected ']' after computed generator method key");
                        Emit(Op::MakeFunction,ParseFunction(false,L"",false,L"",false,true));
                        Emit(Op::ObjectSetComputed);
                    }else{
                        const auto name=ConsumeIdentifier(L"Expected generator method name").text;
                        Emit(Op::MakeFunction,ParseFunction(false,L"",false,L"",false,true));
                        Emit(Op::ObjectSet,0,name);
                    }
                    continue;
                }
                if(Match(TokenKind::LeftBracket)){
                    Expression();Consume(TokenKind::RightBracket,L"Expected ']' after computed object key");
                    if(Check(TokenKind::LeftParen))Emit(Op::MakeFunction,ParseFunction(false,L""));
                    else{Consume(TokenKind::Colon,L"Expected ':' after computed object key");Assignment();}
                    Emit(Op::ObjectSetComputed);continue;
                }
                if(CheckWord(L"get")&&Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::LeftParen){
                    Advance();const auto name=Advance().text;Emit(Op::MakeFunction,ParseFunction(false,L""));Emit(Op::ObjectSet,0,L"$get:"+name);continue;
                }
                if(CheckWord(L"async")&&Peek(1).kind==TokenKind::Identifier&&Peek(2).kind==TokenKind::LeftParen){
                    Advance();const auto name=Advance().text;Emit(Op::MakeFunction,ParseFunction(false,L"",true));Emit(Op::ObjectSet,0,name);continue;
                }
                if(Check(TokenKind::Identifier)&&Peek(1).kind==TokenKind::LeftParen){
                    const auto name=Advance().text;Emit(Op::MakeFunction,ParseFunction(false,L""));Emit(Op::ObjectSet,0,name);continue;
                }
                Token key;if(Check(TokenKind::Identifier)||Check(TokenKind::String)||Check(TokenKind::Number))key=Advance();else throw Error(L"Expected object key");
                if(Check(TokenKind::LeftParen)){
                    Emit(Op::MakeFunction,ParseFunction(false,L""));
                    Emit(Op::ObjectSet,0,key.text);continue;
                }
                if(Match(TokenKind::Colon))Assignment();
                else if(key.kind==TokenKind::Identifier)Emit(Op::LoadReference,0,key.text);
                else throw Error(L"Expected ':'");
                Emit(Op::ObjectSet,0,key.text);
            }while(Match(TokenKind::Comma)&&!Check(TokenKind::RightBrace));
            Consume(TokenKind::RightBrace,L"Expected '}'");return;
        }
        if(Check(TokenKind::Identifier)){
            auto name=Advance().text;
            if(Match(TokenKind::Arrow)){Emit(Op::MakeFunction,ParseArrowSingle(name,false));return;}
            Emit(Op::LoadReference,0,name);return;
        }
        throw Error(L"Expected expression");
    }
    int ParseArrowSingle(const std::wstring& name,bool isAsync){
        auto prototype=std::make_shared<Prototype>();prototype->parameters.push_back(name);prototype->parameterDefaults.push_back({});prototype->lexicalThis=true;prototype->isAsync=isAsync;Chunk* outer=chunk_;const bool outerAsync=compilingAsync_,outerGenerator=compilingGenerator_,outerStrict=compilingStrict_;const size_t outerBlockDepth=blockDepth_;chunk_=&prototype->chunk;compilingAsync_=isAsync;compilingGenerator_=false;blockDepth_=0;
        if(Match(TokenKind::LeftBrace)){prototype->isStrict=outerStrict||HasUseStrictDirective(position_);compilingStrict_=prototype->isStrict;while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();Consume(TokenKind::RightBrace,L"Expected '}'");Emit(Op::Undefined);Emit(Op::Return);}else{prototype->isStrict=outerStrict;compilingStrict_=prototype->isStrict;Assignment();Emit(Op::Return);}
        chunk_=outer;compilingAsync_=outerAsync;compilingGenerator_=outerGenerator;compilingStrict_=outerStrict;blockDepth_=outerBlockDepth;module_->prototypes.push_back(prototype);return static_cast<int>(module_->prototypes.size()-1);
    }
    int ParseParenArrow(bool isAsync){
        Consume(TokenKind::LeftParen,L"");auto prototype=std::make_shared<Prototype>();ParseParameters(prototype);Consume(TokenKind::Arrow,L"Expected =>");
        prototype->lexicalThis=true;prototype->isAsync=isAsync;Chunk* outer=chunk_;const bool outerAsync=compilingAsync_,outerGenerator=compilingGenerator_,outerStrict=compilingStrict_;const size_t outerBlockDepth=blockDepth_;chunk_=&prototype->chunk;compilingAsync_=isAsync;compilingGenerator_=false;blockDepth_=0;
        if(Match(TokenKind::LeftBrace)){prototype->isStrict=outerStrict||HasUseStrictDirective(position_);compilingStrict_=prototype->isStrict;while(!Check(TokenKind::RightBrace)&&!AtEnd())Statement();Consume(TokenKind::RightBrace,L"Expected '}'");Emit(Op::Undefined);Emit(Op::Return);}else{prototype->isStrict=outerStrict;compilingStrict_=prototype->isStrict;Assignment();Emit(Op::Return);}
        chunk_=outer;compilingAsync_=outerAsync;compilingGenerator_=outerGenerator;compilingStrict_=outerStrict;blockDepth_=outerBlockDepth;module_->prototypes.push_back(prototype);return static_cast<int>(module_->prototypes.size()-1);
    }
    std::shared_ptr<Module> module_;std::vector<Token> tokens_;size_t position_=0;Chunk* chunk_=nullptr;std::vector<ControlContext> controls_;std::vector<LabelContext> labels_;std::vector<std::wstring> classBases_;size_t hiddenVariable_=0;size_t blockDepth_=0;bool compilingAsync_=false;bool compilingGenerator_=false;bool compilingStrict_=false;
};

std::wstring NumberString(double number) {
    if(std::isnan(number))return L"NaN";if(std::isinf(number))return number<0?L"-Infinity":L"Infinity";
    // Array indices, counters and template substitutions overwhelmingly use
    // small integers. Format these without constructing a locale-aware stream.
    // Stay below the existing general-format exponent threshold.
    if(std::abs(number)<1e15&&std::floor(number)==number){
        char buffer[32];
        const auto converted=std::to_chars(std::begin(buffer),std::end(buffer),
            static_cast<long long>(number));
        return std::wstring(buffer,converted.ptr);
    }
    std::wostringstream out;out<<std::setprecision(15)<<number;auto value=out.str();
    if(value.find(L'.')!=std::wstring::npos){while(!value.empty()&&value.back()==L'0')value.pop_back();if(!value.empty()&&value.back()==L'.')value.pop_back();}
    return value;
}

void AppendEscapedHtml(std::wstring& output,const std::wstring& value,bool attribute=false){
    for(const auto character:value){
        if(character==L'&')output+=L"&amp;";
        else if(character==L'<')output+=L"&lt;";
        else if(character==L'>')output+=L"&gt;";
        else if(attribute&&character==L'\"')output+=L"&quot;";
        else output+=character;
    }
}

bool IsVoidHtmlElement(const std::wstring& tag){
    static constexpr const wchar_t* names[]={L"area",L"base",L"br",L"col",L"embed",L"hr",L"img",L"input",L"link",L"meta",L"param",L"source",L"track",L"wbr"};
    return std::find(std::begin(names),std::end(names),tag)!=std::end(names);
}

void AppendOuterHtml(std::wstring& output,const std::shared_ptr<Node>& node){
    if(!node)return;
    if(node->type==NodeType::Text){AppendEscapedHtml(output,node->text);return;}
    if(node->type==NodeType::Comment){output+=L"<!--"+node->text+L"-->";return;}
    if(node->type==NodeType::Document){for(const auto& child:node->children)AppendOuterHtml(output,child);return;}
    output+=L'<'+node->tag;
    for(const auto& attribute:node->attributes){
        output+=L' '+attribute.first;
        output+=L"=\"";AppendEscapedHtml(output,attribute.second,true);output+=L'\"';
    }
    if(node->checked&&node->attributes.count(L"checked")==0)output+=L" checked=\"\"";
    if(node->disabled&&node->attributes.count(L"disabled")==0)output+=L" disabled=\"\"";
    output+=L'>';
    if(IsVoidHtmlElement(node->tag))return;
    for(const auto& child:node->children)AppendOuterHtml(output,child);
    output+=L"</"+node->tag+L'>';
}

std::wstring InnerHtml(const std::shared_ptr<Node>& node){
    std::wstring output;if(node)for(const auto& child:node->children)AppendOuterHtml(output,child);return output;
}

std::shared_ptr<Node> CloneDomNode(const std::shared_ptr<Node>& source,bool deep){
    if(!source)return {};
    auto clone=std::make_shared<Node>();clone->type=source->type;clone->tag=source->tag;clone->text=source->text;
    clone->attributes=source->attributes;clone->inlineStyle=source->inlineStyle;
    clone->inlineStylePriority=source->inlineStylePriority;clone->files=source->files;
    clone->checked=source->checked;clone->indeterminate=source->indeterminate;clone->disabled=source->disabled;
    clone->scrollLeft=source->scrollLeft;clone->scrollTop=source->scrollTop;
    clone->selectionStart=source->selectionStart;clone->selectionEnd=source->selectionEnd;
    clone->selectionDirection=source->selectionDirection;
    if(deep)for(const auto& child:source->children){auto copied=CloneDomNode(child,true);copied->parent=clone;clone->children.push_back(std::move(copied));}
    return clone;
}

bool NormalizeDomNode(const std::shared_ptr<Node>& node){
    if(!node||node->type==NodeType::Text)return false;
    bool changed=false;
    for(auto& child:node->children)changed=NormalizeDomNode(child)||changed;
    for(size_t index=0;index<node->children.size();){
        auto& child=node->children[index];
        if(child->type==NodeType::Text&&child->text.empty()&&node->children.size()>1){child->parent.reset();node->children.erase(node->children.begin()+static_cast<std::ptrdiff_t>(index));changed=true;continue;}
        if(index+1<node->children.size()&&child->type==NodeType::Text&&node->children[index+1]->type==NodeType::Text){
            child->text+=node->children[index+1]->text;node->children[index+1]->parent.reset();node->children.erase(node->children.begin()+static_cast<std::ptrdiff_t>(index+1));changed=true;continue;
        }
        ++index;
    }
    return changed;
}

std::wstring CamelToKebab(const std::wstring& value){std::wstring out;for(wchar_t c:value){if(std::iswupper(c)){out+=L'-';out+=std::towlower(c);}else out+=c;}return out;}
std::wstring SerializeInlineStyle(const std::shared_ptr<Node>& node){
    std::wstring out;if(!node)return out;
    for(const auto& declaration:node->inlineStyle){
        if(!out.empty())out+=L' ';
        out+=declaration.first+L": "+declaration.second;
        if(node->inlineStylePriority.count(declaration.first))out+=L" !important";
        out+=L';';
    }
    return out;
}
void SyncInlineStyleAttribute(const std::shared_ptr<Node>& node){
    if(!node)return;
    const auto text=SerializeInlineStyle(node);
    if(text.empty())node->attributes.erase(L"style");else node->attributes[L"style"]=text;
}
std::wstring DatasetAttributeName(const std::wstring& value){return L"data-"+CamelToKebab(value);}
bool AttributeRequiresLayout(const std::wstring& name){
    return name==L"value"||name==L"placeholder"||name==L"type"||name==L"rows"||
           name==L"cols"||name==L"size"||name==L"multiple"||name==L"colspan"||
           name==L"rowspan"||name==L"span"||name==L"width"||name==L"height"||
           name==L"lang"||name==L"src";
}
void ResetImageSourceState(const std::shared_ptr<Node>& node){
    if(!node||node->tag!=L"img")return;
    node->image.reset();node->imageSource=node->Attribute(L"src");
    node->imageComplete=node->imageSource.empty();
}

struct RuntimeCore {
    struct EventListener {
        std::uint64_t id=0;
        Value callback;
        bool capture=false;
        bool once=false;
        bool passive=false;
    };
    using EventListenerMap=FastMap<std::wstring,std::vector<EventListener>>;
    struct MutationBatch {
        RuntimeCore& runtime;
        explicit MutationBatch(RuntimeCore& value):runtime(value){runtime.BeginMutationBatch();}
        ~MutationBatch(){runtime.EndMutationBatch();}
    };
    Document& document;
    EditingCommandExecutor editingCommands;
    JavaScriptRuntime::MessageSink messageSink;
    JavaScriptRuntime::MutationSink mutationSink;
    JavaScriptRuntime::GeometryProvider geometryProvider;
    JavaScriptRuntime::StylePropertyProvider stylePropertyProvider;
    JavaScriptRuntime::FrameScheduler frameScheduler;
    JavaScriptRuntime::TimerScheduler timerScheduler;
    JavaScriptRuntime::ResourceLoader resourceLoader;
    JavaScriptRuntime::AsyncResourceLoader asyncResourceLoader;
    JavaScriptRuntime::NavigationSink navigationSink;
    JavaScriptRuntime::DialogSink dialogSink;
    JavaScriptRuntime::DocumentWriteSink documentWriteSink;
    JavaScriptRuntime::FrameMessageSink frameMessageSink;
    JavaScriptRuntime::FrameDocumentSink frameDocumentSink;
    JavaScriptRuntime::ParentMessageSink parentMessageSink;
    JavaScriptRuntime::TopMessageSink topMessageSink;
    JavaScriptRuntime::FocusSink focusSink;
    JavaScriptRuntime::DocumentFocusProvider documentFocusProvider;
    JavaScriptRuntime::ActivationSink activationSink;
    JavaScriptRuntime::PointerCaptureSink pointerCaptureSink;
    JavaScriptRuntime::SelectionProvider selectionProvider;
    JavaScriptRuntime::SelectionSetter selectionSetter;
    JavaScriptRuntime::DomSelectionProvider domSelectionProvider;
    JavaScriptRuntime::DomSelectionSetter domSelectionSetter;
    std::wstring location;
    std::wstring windowName;
    std::wstring documentReadyState=L"complete";
    std::wstring documentWriteBuffer;
    bool documentWriteOpen=false;
    std::shared_ptr<Environment> global;
    std::shared_ptr<Object> arrayPrototype;
    std::shared_ptr<Object> promisePrototype;
    size_t nextSymbolId=0;
    FastMap<std::wstring,Value> symbolRegistry;
    std::shared_ptr<Module> module=std::make_shared<Module>();
    struct CallStackEntry { std::wstring name;int line=1;size_t offset=0; };
    std::vector<CallStackEntry> callStack;
    std::unordered_map<const Object*,std::weak_ptr<Object>> managedObjects;
    std::unordered_map<const Function*,std::weak_ptr<Function>> managedFunctions;
    std::unordered_map<const NativeFunction*,std::weak_ptr<NativeFunction>> managedNativeFunctions;
    std::unordered_map<const Environment*,std::weak_ptr<Environment>> managedEnvironments;
    size_t managedAllocationsSinceSweep=0;
    size_t managedSweepInterval=256;
    std::uint64_t executedInstructions=0;
    std::uint64_t cycleCollectionNext=1000000;
    bool cycleCollectionPending=false;
    size_t prototypeSweepWatermark=0;
    size_t activeExecutionFrames=0;
    struct NodeEventListeners {
        std::weak_ptr<Node> node;
        EventListenerMap events;
    };
    FastMap<Node*,NodeEventListeners> listeners;
    EventListenerMap documentListeners;
    EventListenerMap windowListeners;
    EventListenerMap webViewListeners;
    FastMap<Object*,EventListenerMap> objectListeners;
    std::vector<std::weak_ptr<Object>> mediaQueries;
    struct MutationObserverRegistration {
        std::weak_ptr<Object> observer;
        std::weak_ptr<Node> target;
        bool subtree=false;
        bool attributes=false;
        bool childList=false;
        bool characterData=false;
        std::vector<std::wstring> attributeFilter;
    };
    std::vector<MutationObserverRegistration> mutationObservers;
    bool mutationObserverMicrotaskScheduled=false;
    FastMap<Node*,std::weak_ptr<Object>> nodeObjects;
    // A detached HTMLImageElement is kept alive by its in-flight fetch in a
    // browser. Keep its wrapper (and therefore onload/onerror properties)
    // rooted until View publishes the matching image completion.
    FastMap<Node*,std::shared_ptr<Object>> pendingImageObjects;
    FastMap<Node*,std::weak_ptr<Object>> canvasContexts;
    FastMap<Node*,std::weak_ptr<Object>> frameWindows;
    FastMap<Node*,std::weak_ptr<Object>> frameDocuments;
    FastMap<std::wstring,std::shared_ptr<const std::wregex>> regularExpressions;
    FastMap<std::wstring,std::shared_ptr<const FiniteRegexSeparator>> finiteRegularExpressions;
    FastMap<std::wstring,AnchoredRegexPrefixes> anchoredRegexPrefixes;
    struct TemplateProgram {
        std::vector<std::wstring> literals;
        std::vector<Chunk> expressions;
    };
    FastMap<std::wstring,std::shared_ptr<TemplateProgram>> templatePrograms;
    FastMap<std::wstring,Value> inlineHandlerCache;
    std::vector<std::pair<unsigned,Value>> frameCallbacks;
    struct TimerEntry { unsigned id=0;Value callback;std::chrono::steady_clock::time_point due;unsigned interval=0; };
    std::vector<TimerEntry> timers;
    std::deque<std::function<void()>> tasks;
    struct OrderedScriptLoad {
        std::shared_ptr<Node> node;
        std::shared_ptr<Object> scriptObject;
        std::wstring source;
        bool ready=false;
        bool loaded=false;
    };
    std::deque<std::shared_ptr<OrderedScriptLoad>> orderedScriptLoads;
    std::deque<std::function<void()>> microtasks;
    std::vector<std::weak_ptr<Object>> possiblyUnhandledRejections;
    unsigned nextFrameId=1;
    unsigned nextTimerId=1;
    std::uint64_t nextEventListenerId=1;
    bool frameScheduled=false;
    bool timerScheduled=false;
    bool drainingMicrotasks=false;
    bool inlineEventHandlersEnabled=true;
    std::chrono::steady_clock::time_point timerWake{};
    std::wstring lastError;
    std::wstring lastCreatedError;
    std::vector<std::wstring> createdErrors;
    int mutationBatchDepth=0;
    bool mutationPending=false;
    JavaScriptRuntime::MutationKind mutationKind=JavaScriptRuntime::MutationKind::Paint;
    std::vector<std::shared_ptr<Node>> mutationTargets;
    bool liveRegionMembershipChanged=false;
    bool indexDirty=false;
    double viewportWidth=0;
    double viewportHeight=0;
    double devicePixelRatio=1;
    size_t jitCompilationThreshold=64;
    size_t jitCodeBudget=8*1024*1024;
    size_t jitAllocatedCodeBytes=0;
    JavaScriptRuntime::JitStatistics jitStatistics;
    std::weak_ptr<Node> pointerCaptureNode;
    std::shared_ptr<Node> currentScript;

    explicit RuntimeCore(Document& d)
        :document(d),
         editingCommands(
             document,
             [this](EditingSelection& selection){
                 return domSelectionProvider&&domSelectionProvider(selection);
             },
             [this](const EditingSelection& selection){
                 if(domSelectionSetter)domSelectionSetter(selection);
             },
             [this](const std::shared_ptr<Node>& target,EditingMutationKind kind,
                    bool requiresIndex){
                 Mutated(target,
                         kind==EditingMutationKind::Tree?
                             JavaScriptRuntime::MutationKind::Tree:
                             JavaScriptRuntime::MutationKind::Style,
                         requiresIndex);
             },
             [this](const std::shared_ptr<Node>& node){ExecuteConnectedScripts(node);}){
        global=CreateEnvironment();InstallGlobals();
    }
    ~RuntimeCore(){ReleaseManagedGraph();}

    std::shared_ptr<Reference> CreateReference(){
        // References are short-lived evaluation results.  A persistent PMR
        // pool retained every high-water chunk reached by a large or repeating
        // script for the lifetime of the browsing context, so an otherwise
        // idle page could grow without returning that storage.  The process
        // heap already caches these uniform allocations and can trim them once
        // the last reference value is released.
        return std::make_shared<Reference>();
    }

    template<class Map>
    static void PruneExpiredManagedEntries(Map& entries){
        std::vector<typename Map::key_type> expired;
        for(const auto& entry:entries)if(entry.second.expired())expired.push_back(entry.first);
        for(const auto& key:expired)entries.erase(key);
    }
    template<class Key,class T,class Hash,class Equal>
    static void PruneExpiredManagedEntries(FastMap<Key,T,Hash,Equal>& entries){
        // Compact weak wrapper registries once. Erasing each expired wrapper
        // separately rebuilds the flat map's buckets once per removed entry.
        entries.erase_if([](const auto& entry){return entry.second.expired();});
    }
    void MaybePruneManagedEntries(){
        if(++managedAllocationsSinceSweep<managedSweepInterval)return;
        managedAllocationsSinceSweep=0;
        PruneExpiredManagedEntries(managedObjects);
        PruneExpiredManagedEntries(managedFunctions);
        PruneExpiredManagedEntries(managedNativeFunctions);
        PruneExpiredManagedEntries(managedEnvironments);
        PruneExpiredManagedEntries(nodeObjects);
        PruneExpiredManagedEntries(canvasContexts);
        PruneExpiredManagedEntries(frameWindows);
        PruneExpiredManagedEntries(frameDocuments);
        // Event listeners belong to their target node.  Keeping the callback
        // in a raw-pointer keyed map after a detached target has died turns the
        // listener into an unintended runtime root and leaves a dangling key.
        // Connected nodes and detached nodes still referenced by JavaScript
        // remain alive through this weak association and keep their listeners.
        listeners.erase_if([](const auto& entry){return entry.second.node.expired();});
        // Amortize a full registry scan over the live graph size. A fixed
        // interval repeatedly scans every retained record while a large array
        // is being mapped, making otherwise linear JavaScript work quadratic.
        managedSweepInterval=std::max<size_t>(256,managedObjects.size()+
            managedFunctions.size()+managedNativeFunctions.size()+managedEnvironments.size()+
            nodeObjects.size()+canvasContexts.size()+frameWindows.size()+frameDocuments.size()+
            listeners.size());
    }
    template<class T>
    void TrackManaged(std::unordered_map<const T*,std::weak_ptr<T>>& entries,
                      const std::shared_ptr<T>& value){
        if(!value)return;
        const auto found=entries.find(value.get());
        if(found==entries.end()){
            entries.emplace(value.get(),value);MaybePruneManagedEntries();
        }else if(found->second.expired()){
            found->second=value;MaybePruneManagedEntries();
        }
    }
    std::shared_ptr<Object> CreateObject(ObjectKind kind=ObjectKind::Plain){
        auto object=std::make_shared<Object>();object->kind=kind;
        TrackManaged(managedObjects,object);return object;
    }
    void TrackObject(const std::shared_ptr<Object>& object){
        TrackManaged(managedObjects,object);
    }
    void TrackValue(const Value& value){
        TrackManaged(managedObjects,value.object);
        TrackManaged(managedFunctions,value.function);
        TrackManaged(managedNativeFunctions,value.native);
        if(value.reference&&value.reference->env)
            TrackManaged(managedEnvironments,value.reference->env);
    }
    std::shared_ptr<Function> CreateFunction(){
        auto function=std::make_shared<Function>();TrackManaged(managedFunctions,function);return function;
    }
    std::shared_ptr<Environment> CreateEnvironment(){
        auto environment=std::make_shared<Environment>();TrackManaged(managedEnvironments,environment);return environment;
    }
    void ReleaseManagedGraph()noexcept{
        // JavaScript values form arbitrary graphs (window.parent points back to
        // window, closures point back to their environment, and user objects may
        // point to themselves). shared_ptr cannot collect those graphs, so sever
        // every engine-owned edge before dropping a page or the runtime itself.
        for(auto& entry:managedNativeFunctions)if(auto native=entry.second.lock()){
            native->callback={};native->props.clear();
        }
        for(auto& entry:managedFunctions)if(auto function=entry.second.lock()){
            function->closure.reset();function->prototype.reset();function->props.clear();
        }
        for(auto& entry:managedEnvironments)if(auto environment=entry.second.lock()){
            environment->values.clear();environment->parent.reset();
        }
        for(auto& entry:managedObjects)if(auto object=entry.second.lock()){
            object->props.clear();object->items.clear();object->entries.clear();
            object->prototype.reset();object->promiseResult=Value::Undefined();
            object->promiseReactions.clear();object->node.reset();object->canvasGradient.reset();
        }
        pendingImageObjects.clear();
        managedNativeFunctions.clear();managedFunctions.clear();
        managedEnvironments.clear();managedObjects.clear();
        arrayPrototype.reset();
        promisePrototype.reset();
        managedAllocationsSinceSweep=0;
        managedSweepInterval=256;
    }
    void ResetExecutionState(bool notifyTimerScheduler){
        if(notifyTimerScheduler&&timerScheduler)timerScheduler(0);
        ReleaseManagedGraph();
        listeners.clear();documentListeners.clear();windowListeners.clear();webViewListeners.clear();
        objectListeners.clear();mediaQueries.clear();pointerCaptureNode.reset();
        if(pointerCaptureSink)pointerCaptureSink(false);
        nodeObjects.clear();canvasContexts.clear();frameWindows.clear();frameDocuments.clear();
        inlineHandlerCache.clear();
        documentWriteBuffer.clear();documentWriteOpen=false;
        currentScript.reset();
        frameCallbacks.clear();frameScheduled=false;timers.clear();tasks.clear();
        orderedScriptLoads.clear();timerScheduled=false;
        microtasks.clear();possiblyUnhandledRejections.clear();templatePrograms.clear();mutationTargets.clear();
        jitStatistics={};jitAllocatedCodeBytes=0;
        executedInstructions=0;cycleCollectionNext=1000000;cycleCollectionPending=false;
        prototypeSweepWatermark=0;
        module=std::make_shared<Module>();global=CreateEnvironment();InstallGlobals();
        for(const auto& script:document.QuerySelectorAll(L"script"))script->scriptStarted=true;
    }

    std::shared_ptr<const std::wregex> RegularExpression(const std::wstring& pattern,
                                                        const std::wstring& flags){
        const auto key=flags+L'\x1f'+pattern;
        const auto cached=regularExpressions.find(key);
        if(cached!=regularExpressions.end())return cached->second;
        std::wstring compatiblePattern;
        if(!TranslateUnicodePropertyEscapes(pattern,flags,compatiblePattern))return {};
        std::wregex::flag_type options=std::regex_constants::ECMAScript;
        if(flags.find(L'i')!=std::wstring::npos)options|=std::regex_constants::icase;
        try{
            if(flags.find(L'u')==std::wstring::npos&&flags.find(L'v')==std::wstring::npos)
                compatiblePattern=NormalizeLegacyRegexBraces(compatiblePattern);
            auto expression=std::make_shared<const std::wregex>(compatiblePattern,options);
            if(regularExpressions.size()>=256)regularExpressions.clear();
            regularExpressions.emplace(key,expression);
            return expression;
        }catch(...){return {};}
    }

    std::shared_ptr<const FiniteRegexSeparator> FiniteRegularExpression(
        const std::wstring& pattern,const std::wstring& flags){
        const auto key=flags+L'\x1f'+pattern;
        const auto cached=finiteRegularExpressions.find(key);
        if(cached!=finiteRegularExpressions.end())return cached->second;
        auto expression=std::make_shared<FiniteRegexSeparator>();
        if(!CompileFiniteRegexSeparator(pattern,flags,*expression))expression.reset();
        if(finiteRegularExpressions.size()>=256)finiteRegularExpressions.clear();
        finiteRegularExpressions.emplace(key,expression);
        return expression;
    }

    bool RegularExpressionMayMatch(const std::wstring& pattern,const std::wstring& flags,
                                   const std::wstring& input){
        const auto key=flags+L'\x1f'+pattern;
        auto cached=anchoredRegexPrefixes.find(key);
        if(cached==anchoredRegexPrefixes.end()){
            if(anchoredRegexPrefixes.size()>=256)anchoredRegexPrefixes.clear();
            cached=anchoredRegexPrefixes.emplace(key,CompileAnchoredRegexPrefixes(pattern,flags)).first;
        }
        return cached->second.MayMatch(input);
    }

    Value Deref(const Value& input){
        if(input.type!=Value::Type::Reference)return input;
        const auto& ref=*input.reference;
        if(ref.kind==Reference::Kind::Variable||ref.kind==Reference::Kind::Super){
            auto* value=ref.env?ref.env->FindValue(ref.name):nullptr;
            if(value)return *value;
            // Browser scripts use the Window object as their global object. A
            // library export written through window.name must therefore be
            // visible to a later script as the unqualified identifier name.
            if(global){const auto window=global->values.find(L"window");
                if(window!=global->values.end())return GetProperty(window->second,ref.name);}
            return Value::Undefined();
        }
        return GetProperty(Deref(ref.base),ref.name);
    }
    Value Deref(Value&& input){
        if(input.type!=Value::Type::Reference)return std::move(input);
        return Deref(static_cast<const Value&>(input));
    }
    bool Truth(const Value& value){
        const auto v=Deref(value);switch(v.type){case Value::Type::Undefined:case Value::Type::Null:return false;case Value::Type::Boolean:return v.boolean;case Value::Type::Number:return v.number!=0&&!std::isnan(v.number);case Value::Type::String:return !v.string.empty();default:return true;}
    }
    double Number(const Value& value){
        const auto v=Deref(value);if(v.type==Value::Type::Number)return v.number;if(v.type==Value::Type::Boolean)return v.boolean?1:0;if(v.type==Value::Type::Null)return 0;if(v.type==Value::Type::String){const auto text=Trim(v.string);if(text.empty())return 0;wchar_t* end=nullptr;const double number=std::wcstod(text.c_str(),&end);if(end==text.c_str()||!end||*end!=L'\0')return std::numeric_limits<double>::quiet_NaN();return number;}if(v.type==Value::Type::Object&&v.object&&v.object->kind==ObjectKind::Date&&v.object->props.count(L"$time"))return Number(v.object->props[L"$time"]);return std::numeric_limits<double>::quiet_NaN();
    }
    std::uint32_t Uint32(const Value& value){
        const double number=Number(value);if(!std::isfinite(number)||number==0)return 0;
        double reduced=std::fmod(std::trunc(number),4294967296.0);if(reduced<0)reduced+=4294967296.0;
        return static_cast<std::uint32_t>(reduced);
    }
    std::int32_t Int32(const Value& value){return static_cast<std::int32_t>(Uint32(value));}
    bool OwnPropertyIsEnumerable(const Value& input,const std::wstring& key){
        const auto value=Deref(input);
        if(value.type==Value::Type::String){
            size_t index=0;return TryParseDecimalIndex(key,index)&&index<value.string.size();
        }
        const FastMap<std::wstring,Value>* properties=nullptr;
        if(value.type==Value::Type::Function&&value.function)properties=&value.function->props;
        else if(value.type==Value::Type::Native&&value.native)properties=&value.native->props;
        else if(value.type==Value::Type::Object&&value.object){
            if(value.object->kind==ObjectKind::Array){
                size_t index=0;
                if(TryParseDecimalIndex(key,index))return index<value.object->items.size();
                if(key==L"length")return false;
            }
            const auto primitive=value.object->props.find(L"$primitive");
            if(primitive!=value.object->props.end()){
                const auto unboxed=Deref(primitive->second);
                size_t index=0;
                if(unboxed.type==Value::Type::String&&TryParseDecimalIndex(key,index))
                    return index<unboxed.string.size();
            }
            properties=&value.object->props;
        }
        if(!properties)return false;
        if(properties->count(key)==0&&properties->count(L"$get:"+key)==0)return false;
        const auto attribute=properties->find(L"$enumerable:"+key);
        return attribute==properties->end()?!IsInternalObjectProperty(key):Truth(attribute->second);
    }
    FastMap<std::wstring,Value>* OwnPropertyStorage(Value& value){
        if(value.type==Value::Type::Function&&value.function)return &value.function->props;
        if(value.type==Value::Type::Native&&value.native)return &value.native->props;
        if(value.type==Value::Type::Object&&value.object)return &value.object->props;
        return nullptr;
    }
    bool HasOwnStoredProperty(const Value& input,const std::wstring& key){
        auto value=Deref(input);
        if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Array){
            size_t index=0;
            if(key==L"length")return true;
            if(TryParseDecimalIndex(key,index)&&index<value.object->items.size())return true;
        }
        const auto* properties=OwnPropertyStorage(value);
        return properties&&(properties->count(key)||properties->count(L"$get:"+key)||
                            properties->count(L"$set:"+key));
    }
    void DefineOwnDataProperty(const Value& input,const std::wstring& key,const Value& inputValue){
        auto target=Deref(input);const auto value=Deref(inputValue);
        auto* properties=OwnPropertyStorage(target);if(!properties)return;
        properties->erase(L"$get:"+key);properties->erase(L"$set:"+key);
        if(target.type==Value::Type::Object&&target.object&&
           target.object->kind==ObjectKind::Array){
            if(key==L"length"){
                const double requested=Number(value);
                if(!std::isfinite(requested)||requested<0||std::floor(requested)!=requested||
                   requested>static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
                    throw JavaScriptException{ErrorValue(L"RangeError",L"Invalid array length")};
                target.object->items.resize(static_cast<size_t>(requested));
                properties->erase(key);return;
            }
            size_t index=0;
            if(TryParseDecimalIndex(key,index)){
                if(index>=target.object->items.size())target.object->items.resize(index+1);
                target.object->items[index]=value;properties->erase(key);return;
            }
        }
        (*properties)[key]=value;
    }
    void ApplyOwnPropertyDescriptor(const Value& input,const std::wstring& key,
                                    const Value& descriptorInput){
        auto target=Deref(input);auto descriptor=Deref(descriptorInput);
        auto* properties=OwnPropertyStorage(target);
        if(!properties||descriptor.type!=Value::Type::Object||!descriptor.object)return;
        const bool existed=HasOwnStoredProperty(target,key);
        const bool hasValue=descriptor.object->props.count(L"value")!=0;
        const bool hasGetter=descriptor.object->props.count(L"get")!=0;
        const bool hasSetter=descriptor.object->props.count(L"set")!=0;
        if(hasValue){
            DefineOwnDataProperty(target,key,descriptor.object->props[L"value"]);
        }else if(hasGetter||hasSetter){
            properties->erase(key);
            if(hasGetter){
                const auto getter=Deref(descriptor.object->props[L"get"]);
                if(getter.type==Value::Type::Undefined||getter.type==Value::Type::Null)
                    properties->erase(L"$get:"+key);
                else (*properties)[L"$get:"+key]=getter;
            }else if(!existed)properties->erase(L"$get:"+key);
            if(hasSetter){
                const auto setter=Deref(descriptor.object->props[L"set"]);
                if(setter.type==Value::Type::Undefined||setter.type==Value::Type::Null)
                    properties->erase(L"$set:"+key);
                else (*properties)[L"$set:"+key]=setter;
            }else if(!existed)properties->erase(L"$set:"+key);
        }
        const auto enumerable=descriptor.object->props.find(L"enumerable");
        if(enumerable!=descriptor.object->props.end())
            (*properties)[L"$enumerable:"+key]=Value::Bool(Truth(enumerable->second));
        else if(!existed)
            (*properties)[L"$enumerable:"+key]=Value::Bool(false);
    }
    bool HasProperty(const Value& input,const std::wstring& key){
        const auto value=Deref(input);
        if(value.type==Value::Type::Function&&value.function)
            return key==L"prototype"||key==L"bind"||key==L"call"||key==L"apply"||
                   key==L"hasOwnProperty"||key==L"propertyIsEnumerable"||
                   value.function->props.count(key)!=0||value.function->props.count(L"$get:"+key)!=0;
        if(value.type==Value::Type::Native&&value.native)
            return key==L"bind"||key==L"call"||key==L"apply"||key==L"hasOwnProperty"||
                   key==L"propertyIsEnumerable"||
                   value.native->props.count(key)!=0||value.native->props.count(L"$get:"+key)!=0;
        if(value.type==Value::Type::Object&&value.object&&IsCallable(value)&&
           (key==L"bind"||key==L"call"||key==L"apply"||key==L"hasOwnProperty"||
            key==L"propertyIsEnumerable"))return true;
        if(value.type==Value::Type::String){
            if(key==L"length")return true;
            size_t index=0;return TryParseDecimalIndex(key,index)&&index<value.string.size();
        }
        if(value.type!=Value::Type::Object||!value.object)return false;
        const auto object=value.object;
        if(object->kind==ObjectKind::Proxy){
            const auto target=object->props.find(L"$target"),handler=object->props.find(L"$handler");
            if(target==object->props.end())return false;
            if(handler!=object->props.end()){
                const auto trap=GetProperty(handler->second,L"has");
                if(IsCallable(trap))return Truth(Call(trap,handler->second,{target->second,Value::String(key)}));
            }
            return HasProperty(target->second,key);
        }
        if(object->props.count(key)||object->props.count(L"$get:"+key))return true;
        if(object->kind==ObjectKind::Window){
            if(global&&global->values.count(key))return true;
            if(key==L"length")return true;
            if(WindowFrameNode(key))return true;
        }
        if(object->kind==ObjectKind::Array){
            if(key==L"length")return true;
            size_t index=0;if(TryParseDecimalIndex(key,index)&&index<object->items.size())return true;
        }
        if(object->kind==ObjectKind::NamedNodeMap&&object->node){
            if(key==L"length"||key==L"item"||key==L"getNamedItem"||
               key==L"setNamedItem"||key==L"removeNamedItem"||
               key==L"\uffffsymbol.wellKnown:iterator")return true;
            size_t index=0;
            if(TryParseDecimalIndex(key,index))return index<object->node->attributes.size();
            if(object->node->attributes.count(ToLower(key)))return true;
        }
        if(key==L"hasOwnProperty"||key==L"propertyIsEnumerable"||
           (object->kind==ObjectKind::Plain&&(key==L"toString"||key==L"isPrototypeOf")))
            return true;
        for(auto prototype=object->prototype;prototype;prototype=prototype->prototype)
            if(prototype->props.count(key)||prototype->props.count(L"$get:"+key)||prototype->props.count(L"$method:"+key))return true;
        return false;
    }
    std::wstring String(const Value& value){
        const auto v=Deref(value);switch(v.type){case Value::Type::Undefined:return L"undefined";case Value::Type::Null:return L"null";case Value::Type::Boolean:return v.boolean?L"true":L"false";case Value::Type::Number:return NumberString(v.number);case Value::Type::String:return v.string;case Value::Type::Function:case Value::Type::Native:return L"function";case Value::Type::Object:if(v.object&&v.object->kind==ObjectKind::Array){std::wstring result;for(size_t index=0;index<v.object->items.size();++index){if(index)result+=L',';const auto item=Deref(v.object->items[index]);if(item.type!=Value::Type::Undefined&&item.type!=Value::Type::Null)result+=String(item);}return result;}if(v.object&&v.object->kind==ObjectKind::Promise)return L"[object Promise]";if(v.object&&v.object->kind==ObjectKind::Url){const auto href=v.object->props.find(L"href");return href==v.object->props.end()?L"":String(href->second);}if(v.object&&v.object->kind==ObjectKind::Error){const auto name=v.object->props.find(L"name"),message=v.object->props.find(L"message");const auto n=name==v.object->props.end()?L"Error":String(name->second);const auto m=message==v.object->props.end()?L"":String(message->second);return m.empty()?n:n+L": "+m;}return L"[object Object]";default:return L"undefined";}
    }
    Value PrimitiveForAddition(const Value& input){
        const auto value=Deref(input);
        if(value.type!=Value::Type::Object&&value.type!=Value::Type::Function&&
           value.type!=Value::Type::Native)return value;
        const bool preferString=value.type==Value::Type::Object&&value.object&&
                                value.object->kind==ObjectKind::Date;
        const std::array<const wchar_t*,2> methods=preferString?
            std::array<const wchar_t*,2>{L"toString",L"valueOf"}:
            std::array<const wchar_t*,2>{L"valueOf",L"toString"};
        for(const auto* name:methods){
            const auto method=GetProperty(value,name);
            if(!IsCallable(method))continue;
            const auto result=Deref(Call(method,value,{}));
            if(result.type!=Value::Type::Object&&result.type!=Value::Type::Function&&
               result.type!=Value::Type::Native)return result;
        }
        // Built-in arrays stringify through join(). Keep this fallback for
        // sparse/minimal array objects that do not carry an explicit prototype.
        if(value.type==Value::Type::Object&&value.object&&
           value.object->kind==ObjectKind::Array)return Value::String(String(value));
        throw JavaScriptException{ErrorValue(L"TypeError",L"Cannot convert object to primitive value")};
    }
    std::wstring ObjectTag(const Value& input){
        const auto value=Deref(input);
        switch(value.type){
        case Value::Type::Undefined:return L"[object Undefined]";
        case Value::Type::Null:return L"[object Null]";
        case Value::Type::Boolean:return L"[object Boolean]";
        case Value::Type::Number:return L"[object Number]";
        case Value::Type::String:return L"[object String]";
        case Value::Type::Function:case Value::Type::Native:return L"[object Function]";
        case Value::Type::Object:
            if(!value.object)return L"[object Object]";
            switch(value.object->kind){
            case ObjectKind::Array:return L"[object Array]";
            case ObjectKind::Date:return L"[object Date]";
            case ObjectKind::RegExp:return L"[object RegExp]";
            case ObjectKind::Error:return L"[object Error]";
            case ObjectKind::Promise:return L"[object Promise]";
            case ObjectKind::NamedNodeMap:return L"[object NamedNodeMap]";
            case ObjectKind::Attr:return L"[object Attr]";
            case ObjectKind::ObjectConstructor:case ObjectKind::ArrayConstructor:
            case ObjectKind::StringConstructor:case ObjectKind::NumberConstructor:
            case ObjectKind::DateConstructor:case ObjectKind::PromiseConstructor:
            case ObjectKind::ErrorConstructor:return L"[object Function]";
            default:return L"[object Object]";
            }
        default:return L"[object Object]";
        }
    }
    static std::wstring DecodeUrlParameter(const std::wstring& input){
        std::wstring output;std::string bytes;
        const auto flush=[&]{if(bytes.empty())return;const auto decoded=Utf8ToWide(bytes);output+=decoded.empty()?std::wstring(bytes.begin(),bytes.end()):decoded;bytes.clear();};
        for(size_t index=0;index<input.size();++index){
            if(input[index]==L'+' ){flush();output+=L' ';continue;}
            if(input[index]==L'%'&&index+2<input.size()){
                const int high=HexDigitValue(input[index+1]),low=HexDigitValue(input[index+2]);
                if(high>=0&&low>=0){bytes.push_back(static_cast<char>((high<<4)|low));index+=2;continue;}
            }
            flush();output+=input[index];
        }
        flush();return output;
    }
    static std::wstring EncodeUrlParameter(const std::wstring& input){
        static constexpr wchar_t hexadecimal[]=L"0123456789ABCDEF";
        std::wstring output;const auto bytes=WideToUtf8(input);
        for(const unsigned char byte:bytes){
            if((byte>='a'&&byte<='z')||(byte>='A'&&byte<='Z')||(byte>='0'&&byte<='9')||
               byte=='*'||byte=='-'||byte=='.'||byte=='_')output+=static_cast<wchar_t>(byte);
            else if(byte==' ')output+=L'+';
            else{output+=L'%';output+=hexadecimal[(byte>>4)&15];output+=hexadecimal[byte&15];}
        }
        return output;
    }
    static bool HasUrlScheme(const std::wstring& value){
        const auto colon=value.find(L':');if(colon==std::wstring::npos||colon==0)return false;
        if(!std::iswalpha(value.front()))return false;
        for(size_t index=1;index<colon;++index)if(!std::iswalnum(value[index])&&value[index]!=L'+'&&value[index]!=L'-'&&value[index]!=L'.')return false;
        return true;
    }
    static std::wstring ResolveUrlReference(const std::wstring& input,const std::wstring& base){
        if(input.empty())return base;
        if(HasUrlScheme(input))return input;
        const auto scheme=base.find(L"://");
        if(input.rfind(L"//",0)==0)return scheme==std::wstring::npos?input:base.substr(0,scheme+1)+input;
        const auto fragment=base.find(L'#');const auto query=base.find(L'?');
        const auto suffix=std::min(fragment==std::wstring::npos?base.size():fragment,
                                   query==std::wstring::npos?base.size():query);
        const auto cleanBase=base.substr(0,suffix);
        if(input.front()==L'#')return base.substr(0,fragment==std::wstring::npos?base.size():fragment)+input;
        if(input.front()==L'?')return cleanBase+input;
        if(input.front()==L'/'&&scheme!=std::wstring::npos){const auto originEnd=base.find(L'/',scheme+3);return (originEnd==std::wstring::npos?cleanBase:base.substr(0,originEnd))+input;}
        const auto slash=cleanBase.find_last_of(L"/\\");return (slash==std::wstring::npos?L"":cleanBase.substr(0,slash+1))+input;
    }
    Value UrlSearchParamsValue(std::wstring query,const std::shared_ptr<Object>& owner={}){
        auto parameters=CreateObject(ObjectKind::UrlSearchParams);
        if(!query.empty()&&query.front()==L'?')query.erase(query.begin());
        size_t start=0;while(start<=query.size()){
            const auto amp=query.find(L'&',start);const auto part=query.substr(start,amp==std::wstring::npos?std::wstring::npos:amp-start);
            const auto equal=part.find(L'=');if(!part.empty())parameters->entries.push_back({Value::String(DecodeUrlParameter(part.substr(0,equal))),Value::String(DecodeUrlParameter(equal==std::wstring::npos?L"":part.substr(equal+1)))});
            if(amp==std::wstring::npos)break;start=amp+1;
        }
        if(owner)parameters->props[L"$url"]=Value::FromObject(owner);
        return Value::FromObject(parameters);
    }
    std::wstring SerializeUrlSearchParams(const std::shared_ptr<Object>& parameters){
        std::wstring result;
        for(const auto& entry:parameters->entries){if(!result.empty())result+=L'&';result+=EncodeUrlParameter(String(entry.first));result+=L'=';result+=EncodeUrlParameter(String(entry.second));}
        return result;
    }
    void SyncUrlSearchParams(const std::shared_ptr<Object>& parameters){
        const auto owner=parameters->props.find(L"$url");if(owner==parameters->props.end())return;
        const auto value=Deref(owner->second);if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Url)return;
        auto url=value.object;const auto href=String(url->props[L"href"]);const auto query=href.find(L'?');const auto fragment=href.find(L'#');
        const auto pathEnd=std::min(query==std::wstring::npos?href.size():query,fragment==std::wstring::npos?href.size():fragment);
        const auto encoded=SerializeUrlSearchParams(parameters);const auto hash=fragment==std::wstring::npos?L"":href.substr(fragment);
        url->props[L"search"]=Value::String(encoded.empty()?L"":L"?"+encoded);
        url->props[L"href"]=Value::String(href.substr(0,pathEnd)+(encoded.empty()?L"":L"?"+encoded)+hash);
    }
    Value UrlValue(const std::wstring& input,const std::wstring& base){
        const auto href=ResolveUrlReference(input,base);auto url=CreateObject(ObjectKind::Url);
        const auto fragment=href.find(L'#');const auto query=href.find(L'?');
        const bool hasQuery=query!=std::wstring::npos&&(fragment==std::wstring::npos||query<fragment);
        const auto pathEnd=std::min(hasQuery?query:href.size(),fragment==std::wstring::npos?href.size():fragment);
        const auto schemeEnd=href.find(L':');const auto authorityStart=schemeEnd!=std::wstring::npos&&href.compare(schemeEnd,3,L"://")==0?schemeEnd+3:std::wstring::npos;
        const auto authorityEnd=authorityStart==std::wstring::npos?std::wstring::npos:href.find_first_of(L"/?#",authorityStart);
        auto authority=authorityStart==std::wstring::npos?L"":href.substr(authorityStart,(authorityEnd==std::wstring::npos?href.size():authorityEnd)-authorityStart);
        const auto at=authority.rfind(L'@');if(at!=std::wstring::npos)authority.erase(0,at+1);
        std::wstring host=authority,hostname=authority,port;
        if(!authority.empty()&&authority.front()==L'['){
            const auto close=authority.find(L']');
            if(close!=std::wstring::npos){hostname=authority.substr(0,close+1);if(close+1<authority.size()&&authority[close+1]==L':')port=authority.substr(close+2);}
        }else{
            const auto colon=authority.rfind(L':');
            if(colon!=std::wstring::npos){hostname=authority.substr(0,colon);port=authority.substr(colon+1);}
        }
        std::wstring pathname;
        if(authorityStart!=std::wstring::npos){
            pathname=authorityEnd==std::wstring::npos?L"/":href.substr(authorityEnd,pathEnd-authorityEnd);
            if(pathname.empty())pathname=L"/";
        }else pathname=href.substr(0,pathEnd);
        url->props[L"href"]=Value::String(href);url->props[L"protocol"]=Value::String(schemeEnd==std::wstring::npos?L"":href.substr(0,schemeEnd+1));
        url->props[L"host"]=Value::String(host);url->props[L"hostname"]=Value::String(hostname);url->props[L"port"]=Value::String(port);
        url->props[L"origin"]=Value::String(authorityStart==std::wstring::npos?L"null":href.substr(0,authorityStart)+host);
        url->props[L"pathname"]=Value::String(pathname);
        url->props[L"search"]=Value::String(hasQuery?href.substr(query,(fragment==std::wstring::npos?href.size():fragment)-query):L"");
        url->props[L"hash"]=Value::String(fragment==std::wstring::npos?L"":href.substr(fragment));
        url->props[L"searchParams"]=UrlSearchParamsValue(hasQuery?href.substr(query,(fragment==std::wstring::npos?href.size():fragment)-query):L"",url);
        return Value::FromObject(url);
    }
    std::wstring CurrentOrigin(){
        const auto parsed=UrlValue(location,location);
        if(parsed.type!=Value::Type::Object||!parsed.object)return L"null";
        const auto found=parsed.object->props.find(L"origin");
        return found==parsed.object->props.end()?L"null":String(found->second);
    }
    void RequestNavigation(const std::wstring& target){
        const auto resolved=ResolveUrlReference(target,location);if(resolved.empty())return;
        if(navigationSink)navigationSink(resolved);
    }
    Value NodeValue(const std::shared_ptr<Node>& node){
        if(!node)return Value::Null();auto found=nodeObjects.find(node.get());if(found!=nodeObjects.end())if(auto cached=found->second.lock())return Value::FromObject(cached);
        auto object=CreateObject(ObjectKind::Node);object->node=node;nodeObjects[node.get()]=object;return Value::FromObject(object);
    }
    void UpdatePendingImageObject(const std::shared_ptr<Node>& node){
        if(!node||node->tag!=L"img")return;
        if(node->Attribute(L"src").empty()){
            pendingImageObjects.erase(node.get());return;
        }
        const auto found=nodeObjects.find(node.get());
        if(found!=nodeObjects.end())if(auto object=found->second.lock())
            pendingImageObjects[node.get()]=std::move(object);
    }
    void CompleteImageRequest(const std::shared_ptr<Node>& node){
        if(node&&node->imageComplete)pendingImageObjects.erase(node.get());
    }
    Value AttrValue(const std::shared_ptr<Node>& owner,const std::wstring& inputName){
        const auto name=ToLower(inputName);
        if(!owner)return Value::Null();
        const auto found=owner->attributes.find(name);
        if(found==owner->attributes.end())return Value::Null();
        auto attribute=CreateObject(ObjectKind::Attr);attribute->node=owner;
        attribute->props[L"$name"]=Value::String(found->first);
        // Preserve the last value if this Attr is subsequently detached while
        // keeping reads live for as long as the attribute remains installed.
        attribute->props[L"$value"]=Value::String(found->second);
        return Value::FromObject(attribute);
    }
    Value AttrAt(const std::shared_ptr<Node>& owner,size_t index){
        if(!owner||index>=owner->attributes.size())return Value::Null();
        auto attribute=owner->attributes.begin();
        for(size_t current=0;current<index;++current)++attribute;
        return AttrValue(owner,attribute->first);
    }
    void SetDomAttribute(const std::shared_ptr<Node>& node,const std::wstring& inputName,
                         const std::wstring& value){
        if(!node)return;
        const auto name=ToLower(inputName);
        const auto oldId=name==L"id"?node->Attribute(L"id"):L"";
        node->SetAttribute(name,value);
        if(node->tag==L"dialog"&&name==L"open")node->modal=false;
        if(name==L"src"){ResetImageSourceState(node);UpdatePendingImageObject(node);}
        if(node->tag==L"canvas"&&(name==L"width"||name==L"height"))ResetCanvas(node);
        const bool indexOk=name!=L"id"||document.UpdateElementId(node,oldId,node->Attribute(L"id"));
        const auto kind=AttributeRequiresLayout(name)?JavaScriptRuntime::MutationKind::Layout:
            JavaScriptRuntime::MutationKind::Style;
        Mutated(node,kind,!indexOk,name==L"aria-live",name);
    }
    void RemoveDomAttribute(const std::shared_ptr<Node>& node,const std::wstring& inputName){
        if(!node)return;
        const auto name=ToLower(inputName);
        const auto oldId=name==L"id"?node->Attribute(L"id"):L"";
        node->RemoveAttribute(name);
        if(node->tag==L"dialog"&&name==L"open")node->modal=false;
        if(name==L"src"){ResetImageSourceState(node);UpdatePendingImageObject(node);}
        if(node->tag==L"canvas"&&(name==L"width"||name==L"height"))ResetCanvas(node);
        const bool indexOk=name!=L"id"||document.UpdateElementId(node,oldId,L"");
        const auto kind=AttributeRequiresLayout(name)?JavaScriptRuntime::MutationKind::Layout:
            JavaScriptRuntime::MutationKind::Style;
        Mutated(node,kind,!indexOk,name==L"aria-live",name);
    }
    Value RangeValue(const std::shared_ptr<Node>& start,size_t startOffset,
                     const std::shared_ptr<Node>& end,size_t endOffset){
        auto range=CreateObject(ObjectKind::Range);range->rangeStart=start;range->rangeStartOffset=startOffset;
        range->rangeEnd=end;range->rangeEndOffset=endOffset;return Value::FromObject(range);
    }
    static std::shared_ptr<Node> CommonAncestor(std::shared_ptr<Node> first,
                                                std::shared_ptr<Node> second){
        std::vector<std::shared_ptr<Node>> ancestors;
        for(auto current=first;current;current=current->parent.lock())ancestors.push_back(current);
        for(auto current=second;current;current=current->parent.lock())
            if(std::find(ancestors.begin(),ancestors.end(),current)!=ancestors.end())return current;
        return {};
    }
    static size_t DomTextLength(const std::shared_ptr<Node>& node){
        if(!node)return 0;if(node->type==NodeType::Text)return node->text.size();
        size_t length=0;for(const auto& child:node->children)length+=DomTextLength(child);return length;
    }
    static bool BoundaryTextOffset(const std::shared_ptr<Node>& root,
                                   const std::shared_ptr<Node>& container,size_t offset,
                                   size_t& cursor,size_t& result){
        if(!root)return false;
        if(root==container){
            if(root->type==NodeType::Text)result=cursor+std::min(offset,root->text.size());
            else{
                const size_t count=std::min(offset,root->children.size());
                size_t local=0;for(size_t index=0;index<count;++index)local+=DomTextLength(root->children[index]);
                result=cursor+local;
            }
            return true;
        }
        if(root->type==NodeType::Text){cursor+=root->text.size();return false;}
        for(const auto& child:root->children)if(BoundaryTextOffset(child,container,offset,cursor,result))return true;
        return false;
    }
    static std::wstring RangeText(const std::shared_ptr<Object>& range){
        if(!range||!range->rangeStart||!range->rangeEnd)return {};
        const auto root=CommonAncestor(range->rangeStart,range->rangeEnd);if(!root)return {};
        size_t cursor=0,start=0,end=0;
        if(!BoundaryTextOffset(root,range->rangeStart,range->rangeStartOffset,cursor,start))return {};
        cursor=0;if(!BoundaryTextOffset(root,range->rangeEnd,range->rangeEndOffset,cursor,end))return {};
        if(end<start)std::swap(start,end);const auto text=root->InnerText();
        return text.substr(std::min(start,text.size()),std::min(end,text.size())-std::min(start,text.size()));
    }
    static bool IsDocumentFragment(const std::shared_ptr<Node>& node){
        return node&&node->type==NodeType::Element&&node->tag==L"#document-fragment";
    }
    static std::shared_ptr<Node> NextNodeWithin(const std::shared_ptr<Node>& current,
                                                const std::shared_ptr<Node>& root){
        if(!current||!root)return {};
        if(!current->children.empty())return current->children.front();
        for(auto node=current;node&&node!=root;node=node->parent.lock()){
            const auto parent=node->parent.lock();if(!parent)return {};
            const auto found=std::find(parent->children.begin(),parent->children.end(),node);
            if(found!=parent->children.end()&&std::next(found)!=parent->children.end())return *std::next(found);
        }
        return {};
    }
    void DeleteRangeContents(const std::shared_ptr<Object>& range){
        if(!range||!range->rangeStart||!range->rangeEnd)return;
        auto start=range->rangeStart,end=range->rangeEnd;
        size_t startOffset=range->rangeStartOffset,endOffset=range->rangeEndOffset;
        if(start==end){
            if(start->type==NodeType::Text){
                startOffset=std::min(startOffset,start->text.size());endOffset=std::min(endOffset,start->text.size());
                if(endOffset<startOffset)std::swap(startOffset,endOffset);
                start->text.erase(startOffset,endOffset-startOffset);
            }else{
                startOffset=std::min(startOffset,start->children.size());endOffset=std::min(endOffset,start->children.size());
                if(endOffset<startOffset)std::swap(startOffset,endOffset);
                for(size_t index=startOffset;index<endOffset;++index){document.UnindexSubtree(start->children[index]);start->children[index]->parent.reset();}
                start->children.erase(start->children.begin()+static_cast<std::ptrdiff_t>(startOffset),
                                      start->children.begin()+static_cast<std::ptrdiff_t>(endOffset));
            }
            range->rangeEnd=start;range->rangeEndOffset=startOffset;
            Mutated(start,JavaScriptRuntime::MutationKind::Tree);return;
        }
        const auto root=CommonAncestor(start,end);if(!root)return;
        std::vector<std::shared_ptr<Node>> textNodes;
        std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& node){
            if(node->type==NodeType::Text){textNodes.push_back(node);return;}
            for(const auto& child:node->children)collect(child);
        };collect(root);
        const auto first=std::find(textNodes.begin(),textNodes.end(),start);
        const auto last=std::find(textNodes.begin(),textNodes.end(),end);
        if(first!=textNodes.end()&&last!=textNodes.end()){
            auto begin=first,finish=last;size_t beginOffset=startOffset,finishOffset=endOffset;
            if(begin>finish){std::swap(begin,finish);std::swap(beginOffset,finishOffset);}
            beginOffset=std::min(beginOffset,(*begin)->text.size());
            finishOffset=std::min(finishOffset,(*finish)->text.size());
            (*begin)->text.erase(beginOffset);
            for(auto iterator=std::next(begin);iterator!=finish;++iterator)(*iterator)->text.clear();
            (*finish)->text.erase(0,finishOffset);
            range->rangeStart=*begin;range->rangeStartOffset=beginOffset;
            range->rangeEnd=*begin;range->rangeEndOffset=beginOffset;
            Mutated(root,JavaScriptRuntime::MutationKind::Tree);
        }
    }
    void InsertRangeNode(const std::shared_ptr<Object>& range,const std::shared_ptr<Node>& inserted){
        if(!range||!range->rangeStart||!inserted)return;
        auto container=range->rangeStart;size_t offset=range->rangeStartOffset;
        std::shared_ptr<Node> parent;size_t insertion=0;
        if(container->type==NodeType::Text){
            parent=container->parent.lock();if(!parent)return;
            const auto found=std::find(parent->children.begin(),parent->children.end(),container);if(found==parent->children.end())return;
            insertion=static_cast<size_t>(found-parent->children.begin());offset=std::min(offset,container->text.size());
            if(offset>0){
                auto left=std::make_shared<Node>();left->type=NodeType::Text;left->tag=L"#text";left->text=container->text.substr(0,offset);left->parent=parent;left->ownerDocument=container->ownerDocument;
                *found=left;++insertion;
            }else parent->children.erase(found);
            container->text.erase(0,offset);
            if(!container->text.empty()){container->parent=parent;parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(insertion),container);}
        }else{
            parent=container;insertion=std::min(offset,parent->children.size());
        }
        std::vector<std::shared_ptr<Node>> nodes;
        if(IsDocumentFragment(inserted)){nodes=inserted->children;inserted->children.clear();}
        else nodes.push_back(inserted);
        bool indexOk=true;
        for(auto& node:nodes){
            if(auto old=node->parent.lock()){
                indexOk=document.UnindexSubtree(node)&&indexOk;
                old->children.erase(std::remove(old->children.begin(),old->children.end(),node),old->children.end());
            }
            node->parent=parent;parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(insertion++),node);
            indexOk=document.IndexSubtree(node)&&indexOk;ExecuteConnectedScripts(node);
        }
        Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);
    }
    Value SelectionValue(){
        auto selection=CreateObject(ObjectKind::Selection);JavaScriptRuntime::DomSelection current;
        if(domSelectionProvider&&domSelectionProvider(current)&&current.anchorNode&&current.focusNode)
            selection->props[L"$range"]=RangeValue(current.anchorNode,current.anchorOffset,
                                                    current.focusNode,current.focusOffset);
        return Value::FromObject(selection);
    }
    static unsigned CanvasDimension(const std::shared_ptr<Node>& node,const wchar_t* name,
                                    unsigned fallback){
        if(!node)return fallback;
        const auto raw=Trim(node->Attribute(name));if(raw.empty())return fallback;
        size_t used=0;double number=0;
        if(!TryParseDouble(raw,number,&used)||used!=raw.size()||!std::isfinite(number)||number<0)
            return fallback;
        return static_cast<unsigned>(std::min(65535.0,std::floor(number)));
    }
    std::shared_ptr<CanvasSurface> EnsureCanvas(const std::shared_ptr<Node>& node){
        if(!node)return {};
        const unsigned width=CanvasDimension(node,L"width",300);
        const unsigned height=CanvasDimension(node,L"height",150);
        if(!node->canvas){node->canvas=std::make_shared<CanvasSurface>();node->canvas->Reset(width,height);}
        else if(node->canvas->width!=width||node->canvas->height!=height)
            node->canvas->Reset(width,height);
        return node->canvas;
    }
    std::shared_ptr<CanvasSurface> ResetCanvas(const std::shared_ptr<Node>& node){
        if(!node)return {};
        if(!node->canvas)node->canvas=std::make_shared<CanvasSurface>();
        node->canvas->Reset(CanvasDimension(node,L"width",300),
                            CanvasDimension(node,L"height",150));
        return node->canvas;
    }
    Value CanvasContextValue(const std::shared_ptr<Node>& node){
        if(!node||node->tag!=L"canvas")return Value::Null();
        auto found=canvasContexts.find(node.get());
        if(found!=canvasContexts.end())if(auto cached=found->second.lock()){
            EnsureCanvas(node);return Value::FromObject(cached);
        }
        EnsureCanvas(node);auto object=CreateObject(ObjectKind::CanvasContext2D);object->node=node;
        canvasContexts[node.get()]=object;return Value::FromObject(object);
    }
    Value FrameWindowValue(const std::shared_ptr<Node>& node){
        if(!node)return Value::Null();
        auto found=frameWindows.find(node.get());
        if(found!=frameWindows.end())if(auto cached=found->second.lock())return Value::FromObject(cached);
        auto object=CreateObject(ObjectKind::FrameWindow);object->node=node;
        // The browsing-context name starts from the iframe's name content
        // attribute, but is thereafter independent.  SafeFrame and other
        // generic frame bootstraps assign a serialized startup payload through
        // iframe.contentWindow.name before navigating the child.
        object->props[L"name"]=Value::String(node->frameWindowNameInitialized?
            node->frameWindowName:node->Attribute(L"name"));
        frameWindows[node.get()]=object;return Value::FromObject(object);
    }
    Value FrameDocumentValue(const std::shared_ptr<Node>& node){
        if(!node)return Value::Null();
        auto found=frameDocuments.find(node.get());
        if(found!=frameDocuments.end())if(auto cached=found->second.lock())
            return Value::FromObject(cached);
        auto object=CreateObject(ObjectKind::FrameDocument);object->node=node;
        object->props[L"$text"]=Value::String(L"");
        object->props[L"$open"]=Value::Bool(false);
        frameDocuments[node.get()]=object;return Value::FromObject(object);
    }
    std::wstring FrameWindowName(const std::shared_ptr<Node>& node){
        if(!node)return {};
        return node->frameWindowNameInitialized?node->frameWindowName:node->Attribute(L"name");
    }
    std::vector<std::shared_ptr<Node>> WindowFrameNodes(){
        auto frames=document.QuerySelectorAll(L"iframe");
        const auto legacyFrames=document.QuerySelectorAll(L"frame");
        frames.insert(frames.end(),legacyFrames.begin(),legacyFrames.end());
        return frames;
    }
    std::shared_ptr<Node> WindowFrameNode(const std::wstring& key){
        const auto frames=WindowFrameNodes();
        size_t index=0;
        if(TryParseDecimalIndex(key,index))
            return index<frames.size()?frames[index]:std::shared_ptr<Node>{};
        // Window named access resolves a child browsing-context name before an
        // element id. Advertising and consent SDKs use this for their hidden
        // locator and SafeFrame contexts (window.frames.contextName).
        for(const auto& frame:frames)if(frame->Attribute(L"name")==key)return frame;
        for(const auto& frame:frames)if(frame->Attribute(L"id")==key)return frame;
        return {};
    }
    Value ArrayValue(const std::vector<Value>& items){
        auto o=CreateObject(ObjectKind::Array);o->items=items;
        if(arrayPrototype&&o!=arrayPrototype)o->prototype=arrayPrototype;
        return Value::FromObject(o);
    }
    Value IteratorValue(std::vector<Value> items){
        auto iterator=CreateObject(ObjectKind::Plain);
        auto values=std::make_shared<std::vector<Value>>(std::move(items));
        auto cursor=std::make_shared<size_t>(0);
        iterator->props[L"next"]=Native([values,cursor](RuntimeCore& r,const Value&,
                                                        const std::vector<Value>&){
            auto result=r.ObjectValue(ObjectKind::Plain);
            const bool done=*cursor>=values->size();
            result.object->props[L"done"]=Value::Bool(done);
            result.object->props[L"value"]=done?Value::Undefined():(*values)[(*cursor)++];
            return result;
        });
        iterator->props[L"\uffffsymbol.wellKnown:iterator"]=Native(
            [weak=std::weak_ptr<Object>(iterator)](RuntimeCore&,const Value&,
                                                   const std::vector<Value>&){
                const auto value=weak.lock();
                return value?Value::FromObject(value):Value::Undefined();
            });
        return Value::FromObject(iterator);
    }
    Value CloneValue(const Value& input){
        const auto value=Deref(input);if(value.type!=Value::Type::Object||!value.object)return value;
        if(value.object->kind==ObjectKind::Array){std::vector<Value> items;items.reserve(value.object->items.size());for(const auto& item:value.object->items)items.push_back(CloneValue(item));return ArrayValue(items);}
        if(value.object->kind==ObjectKind::Date)return DateValue({value.object->props[L"$time"]});
        if(value.object->kind!=ObjectKind::Plain)return value;
        auto clone=ObjectValue(ObjectKind::Plain);for(const auto& property:value.object->props)clone.object->props[property.first]=CloneValue(property.second);return clone;
    }
    static double CurrentTimeMilliseconds(){
        using namespace std::chrono;return duration<double,std::milli>(system_clock::now().time_since_epoch()).count();
    }
    static double ParseDateMilliseconds(const std::wstring& value){
        SYSTEMTIME time{};wchar_t separator=0;int consumed=0;
        const int matched=swscanf_s(value.c_str(),L"%hu-%hu-%hu%c%hu:%hu:%hu%n",
            &time.wYear,&time.wMonth,&time.wDay,&separator,1,&time.wHour,&time.wMinute,&time.wSecond,&consumed);
        if(matched<7||(separator!=L'T'&&separator!=L't'&&separator!=L' '))
            return std::numeric_limits<double>::quiet_NaN();
        size_t index=consumed>0?static_cast<size_t>(consumed):value.size();
        time.wMilliseconds=0;
        if(index<value.size()&&value[index]==L'.'){
            ++index;unsigned milliseconds=0,digits=0;
            while(index<value.size()&&std::iswdigit(value[index])){
                if(digits<3)milliseconds=milliseconds*10+static_cast<unsigned>(value[index]-L'0');
                ++digits;++index;
            }
            while(digits<3){milliseconds*=10;++digits;}
            time.wMilliseconds=static_cast<WORD>(milliseconds);
        }
        bool explicitZone=false;int offsetMinutes=0;
        if(index<value.size()&&(value[index]==L'Z'||value[index]==L'z')){
            explicitZone=true;++index;
        }else if(index<value.size()&&(value[index]==L'+'||value[index]==L'-')){
            explicitZone=true;const int direction=value[index++]==L'+'?1:-1;
            auto twoDigits=[&](int& result){
                if(index+1>=value.size()||!std::iswdigit(value[index])||!std::iswdigit(value[index+1]))return false;
                result=(value[index]-L'0')*10+(value[index+1]-L'0');index+=2;return true;
            };
            int hours=0,minutes=0;if(!twoDigits(hours))return std::numeric_limits<double>::quiet_NaN();
            if(index<value.size()&&value[index]==L':')++index;
            if(!twoDigits(minutes)||hours>23||minutes>59)return std::numeric_limits<double>::quiet_NaN();
            offsetMinutes=direction*(hours*60+minutes);
        }
        while(index<value.size()&&std::iswspace(value[index]))++index;
        if(index!=value.size())return std::numeric_limits<double>::quiet_NaN();
        SYSTEMTIME utc=time;
        if(!explicitZone&&!TzSpecificLocalTimeToSystemTime(nullptr,&time,&utc))
            return std::numeric_limits<double>::quiet_NaN();
        FILETIME file{};if(!SystemTimeToFileTime(&utc,&file))return std::numeric_limits<double>::quiet_NaN();
        ULARGE_INTEGER ticks{};ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;
        long long adjusted=static_cast<long long>(ticks.QuadPart)-
            static_cast<long long>(offsetMinutes)*60ll*10000000ll;
        constexpr unsigned long long epoch=116444736000000000ull;
        return adjusted<static_cast<long long>(epoch)?std::numeric_limits<double>::quiet_NaN():
            static_cast<double>(adjusted-static_cast<long long>(epoch))/10000.0;
    }
    static bool DateSystemTime(double milliseconds,SYSTEMTIME& time,bool local){
        if(!std::isfinite(milliseconds))return false;constexpr unsigned long long epoch=116444736000000000ull;
        const auto ticks=static_cast<long long>(std::trunc(milliseconds*10000.0))+static_cast<long long>(epoch);if(ticks<0)return false;
        ULARGE_INTEGER value{};value.QuadPart=static_cast<unsigned long long>(ticks);FILETIME utc{value.LowPart,value.HighPart},converted=utc;
        if(local&&!FileTimeToLocalFileTime(&utc,&converted))return false;return FileTimeToSystemTime(&converted,&time)!=FALSE;
    }
    Value DateValue(const std::vector<Value>& args){
        double milliseconds=CurrentTimeMilliseconds();if(!args.empty()){const auto value=Deref(args[0]);milliseconds=value.type==Value::Type::String?ParseDateMilliseconds(value.string):Number(value);}
        auto date=ObjectValue(ObjectKind::Date);date.object->props[L"$time"]=Value::Number(milliseconds);return date;
    }
    Value FileValue(const Node::FileInfo& info){
        auto file=ObjectValue(ObjectKind::File);
        file.object->props[L"name"]=Value::String(info.name);
        file.object->props[L"type"]=Value::String(info.type);
        file.object->props[L"path"]=Value::String(info.path);
        file.object->props[L"$path"]=Value::String(info.path);
        file.object->props[L"size"]=Value::Number(static_cast<double>(info.size));
        return file;
    }
    Value FileListValue(const std::vector<Node::FileInfo>& infos){
        std::vector<Value> files;files.reserve(infos.size());
        for(const auto& info:infos)files.push_back(FileValue(info));
        return ArrayValue(files);
    }
    Value DataTransferValue(const std::wstring& text,const std::vector<Node::FileInfo>& infos){
        auto transfer=ObjectValue(ObjectKind::DataTransfer);
        transfer.object->props[L"$text"]=Value::String(text);
        transfer.object->props[L"files"]=FileListValue(infos);
        std::vector<Value> types;if(!text.empty())types.push_back(Value::String(L"text/plain"));
        if(!infos.empty())types.push_back(Value::String(L"Files"));
        transfer.object->props[L"types"]=ArrayValue(types);
        std::vector<Value> items;items.reserve(infos.size());
        for(const auto& info:infos){auto item=ObjectValue(ObjectKind::DataTransferItem);
            item.object->props[L"kind"]=Value::String(L"file");
            item.object->props[L"type"]=Value::String(info.type);
            item.object->props[L"$file"]=FileValue(info);items.push_back(item);
        }
        transfer.object->props[L"items"]=ArrayValue(items);
        return transfer;
    }
    Value Native(NativeCallback callback){auto n=std::make_shared<NativeFunction>();n->callback=std::move(callback);TrackManaged(managedNativeFunctions,n);return Value::FromNative(n);}
    Value ObjectValue(ObjectKind kind){return Value::FromObject(CreateObject(kind));}
    Value FunctionPrototype(const std::shared_ptr<Function>& function){
        const auto found=function->props.find(L"prototype");
        if(found!=function->props.end())return Deref(found->second);
        auto prototype=ObjectValue(ObjectKind::Plain);
        prototype.object->props[L"constructor"]=Value::FromFunction(function);
        function->props[L"prototype"]=prototype;
        return prototype;
    }
    double MediaLength(std::wstring value,double widthReference,double heightReference)const{
        value=ToLower(Trim(value));size_t used=0;float number=0;
        if(!TryParseFloat(value,number,&used))return std::numeric_limits<double>::quiet_NaN();
        const auto unit=Trim(value.substr(used));
        if(unit.empty()||unit==L"px")return number;
        if(unit==L"em"||unit==L"rem")return number*16.0;
        if(unit==L"vw")return number*widthReference/100.0;
        if(unit==L"vh")return number*heightReference/100.0;
        return std::numeric_limits<double>::quiet_NaN();
    }
    bool EvaluateMediaFeature(std::wstring expression)const{
        expression=ToLower(Trim(expression));
        const auto colon=expression.find(L':');
        auto name=Trim(expression.substr(0,colon));
        const auto value=colon==std::wstring::npos?L"":Trim(expression.substr(colon+1));
        auto compareLength=[&](const std::wstring& base,double actual){
            const double expected=MediaLength(value,viewportWidth,viewportHeight);
            if(!std::isfinite(expected))return false;
            if(name==L"min-"+base)return actual+0.001>=expected;
            if(name==L"max-"+base)return actual<=expected+0.001;
            return name==base&&std::abs(actual-expected)<0.001;
        };
        if(name==L"width"||name==L"min-width"||name==L"max-width"||
           name==L"device-width"||name==L"min-device-width"||name==L"max-device-width"){
            const std::wstring base=name.find(L"device-")!=std::wstring::npos?L"device-width":L"width";
            return compareLength(base,viewportWidth);
        }
        if(name==L"height"||name==L"min-height"||name==L"max-height"||
           name==L"device-height"||name==L"min-device-height"||name==L"max-device-height"){
            const std::wstring base=name.find(L"device-")!=std::wstring::npos?L"device-height":L"height";
            return compareLength(base,viewportHeight);
        }
        if(name==L"orientation")return value==(viewportWidth>=viewportHeight?L"landscape":L"portrait");
        if(name==L"prefers-reduced-motion")return value.empty()||value==L"no-preference";
        if(name==L"prefers-color-scheme")return value.empty()||value==L"light";
        if(name==L"hover")return value.empty()||value==L"hover";
        if(name==L"any-hover")return value.empty()||value==L"hover";
        if(name==L"pointer"||name==L"any-pointer")return value.empty()||value==L"fine";
        if(name==L"color")return value.empty()||value==L"8";
        if(name==L"resolution"||name==L"min-resolution"||name==L"max-resolution"){
            size_t used=0;float number=0;if(!TryParseFloat(value,number,&used))return false;
            const auto unit=Trim(ToLower(value.substr(used)));
            const double expected=unit==L"dpi"?number/96.0:unit==L"dpcm"?number*2.54/96.0:
                (unit==L"dppx"||unit==L"x"||unit.empty()?number:std::numeric_limits<double>::quiet_NaN());
            if(!std::isfinite(expected))return false;
            if(name==L"min-resolution")return devicePixelRatio+0.0001>=expected;
            if(name==L"max-resolution")return devicePixelRatio<=expected+0.0001;
            return std::abs(devicePixelRatio-expected)<0.0001;
        }
        if(name==L"-webkit-min-device-pixel-ratio"||name==L"-webkit-max-device-pixel-ratio"){
            size_t used=0;float expected=0;if(!TryParseFloat(value,expected,&used))return false;
            return name.find(L"min-")!=std::wstring::npos?
                devicePixelRatio+0.0001>=expected:devicePixelRatio<=expected+0.0001;
        }
        return false;
    }
    bool EvaluateMediaQuery(const std::wstring& source)const{
        size_t clauseStart=0;int nesting=0;
        for(size_t index=0;index<=source.size();++index){
            const wchar_t character=index<source.size()?source[index]:L',';
            if(character==L'(')++nesting;else if(character==L')')--nesting;
            if(character!=L','||nesting!=0)continue;
            auto clause=ToLower(Trim(source.substr(clauseStart,index-clauseStart)));clauseStart=index+1;
            bool negate=false;
            if(clause.rfind(L"not ",0)==0){negate=true;clause=Trim(clause.substr(4));}
            if(clause.rfind(L"only ",0)==0)clause=Trim(clause.substr(5));
            const auto firstFeature=clause.find(L'(');
            auto mediaType=Trim(clause.substr(0,firstFeature));
            while(mediaType.size()>=3&&mediaType.rfind(L"and")==mediaType.size()-3)
                mediaType=Trim(mediaType.substr(0,mediaType.size()-3));
            bool matches=mediaType.empty()||mediaType==L"all"||mediaType==L"screen";
            size_t position=firstFeature;
            while(matches&&position!=std::wstring::npos&&position<clause.size()){
                int depth=1;size_t end=position+1;
                for(;end<clause.size()&&depth;++end){if(clause[end]==L'(')++depth;else if(clause[end]==L')')--depth;}
                if(depth!=0){matches=false;break;}
                matches=EvaluateMediaFeature(clause.substr(position+1,end-position-2));
                position=clause.find(L'(',end);
            }
            if(negate)matches=!matches;
            if(matches)return true;
        }
        return false;
    }
    Value MediaQueryValue(const std::wstring& query){
        auto media=CreateObject(ObjectKind::MediaQuery);
        media->props[L"media"]=Value::String(query);
        media->props[L"$query"]=Value::String(query);
        media->props[L"$matches"]=Value::Bool(EvaluateMediaQuery(query));
        mediaQueries.push_back(media);
        return Value::FromObject(media);
    }
    void UpdateMediaQueries(){
        mediaQueries.erase(std::remove_if(mediaQueries.begin(),mediaQueries.end(),
            [](const auto& item){return item.expired();}),mediaQueries.end());
        for(const auto& weak:mediaQueries)if(const auto media=weak.lock()){
            const auto query=String(media->props[L"$query"]);
            const bool previous=Truth(media->props[L"$matches"]);
            const bool matches=EvaluateMediaQuery(query);media->props[L"$matches"]=Value::Bool(matches);
            if(previous==matches)continue;
            auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(L"change");
            event->props[L"matches"]=Value::Bool(matches);event->props[L"media"]=Value::String(query);
            event->props[L"target"]=Value::FromObject(media);event->props[L"currentTarget"]=Value::FromObject(media);
            event->props[L"defaultPrevented"]=Value::Bool(false);event->props[L"isTrusted"]=Value::Bool(true);
            const auto found=objectListeners.find(media.get());
            if(found!=objectListeners.end())InvokeEventListeners(found->second,L"change",false,
                Value::FromObject(media),Value::FromObject(event),event);
            const auto onchange=media->props.find(L"onchange");
            if(onchange!=media->props.end()&&IsCallable(onchange->second))
                CallEventCallback(onchange->second,Value::FromObject(media),Value::FromObject(event));
        }
    }
    bool IsCallable(const Value& input){
        const auto value=Deref(input);
        if(value.type==Value::Type::Function||value.type==Value::Type::Native)return true;
        if(value.type!=Value::Type::Object||!value.object)return false;
        if(value.object->kind==ObjectKind::Proxy){
            const auto target=value.object->props.find(L"$target");
            return target!=value.object->props.end()&&IsCallable(target->second);
        }
        switch(value.object->kind){
        case ObjectKind::ObjectConstructor:case ObjectKind::ArrayConstructor:
        case ObjectKind::StringConstructor:case ObjectKind::NumberConstructor:
        case ObjectKind::DateConstructor:case ObjectKind::PromiseConstructor:
        case ObjectKind::ErrorConstructor:return true;
        default:return value.object->props.count(L"$class")!=0;
        }
    }
    bool IsInstanceOf(const Value& input,const Value& constructorInput){
        const auto value=Deref(input),constructor=Deref(constructorInput);
        if(!IsCallable(constructor))
            throw JavaScriptException{ErrorValue(L"TypeError",L"Right-hand side of 'instanceof' is not callable")};

        if(constructor.type==Value::Type::Native&&constructor.native){
            const auto interfaceName=constructor.native->props.find(L"$domInterface");
            if(interfaceName!=constructor.native->props.end()){
                const auto name=String(interfaceName->second);
                if(name==L"Document")
                    return value.type==Value::Type::Object&&value.object&&
                           value.object->kind==ObjectKind::Document;
                if(value.type!=Value::Type::Object||!value.object||
                   value.object->kind!=ObjectKind::Node||!value.object->node)return false;
                const auto& node=value.object->node;
                if(name==L"Node")return true;
                const bool element=node->type==NodeType::Element&&!IsDocumentFragment(node);
                if(name==L"Element"||name==L"HTMLElement")return element;
                if(name==L"HTMLScriptElement")return element&&node->tag==L"script";
                if(name==L"HTMLIFrameElement")return element&&node->tag==L"iframe";
                if(name==L"HTMLImageElement")return element&&node->tag==L"img";
                return false;
            }
        }

        // The VM represents the standard constructors as lightweight host
        // objects instead of ordinary interpreted Function instances. Preserve
        // the ECMAScript relationships that web libraries use for feature and
        // callback detection even though those constructors have a different
        // internal representation.
        if(global){
            const auto function=global->values.find(L"Function");
            if(function!=global->values.end()&&EqualValues(constructor,function->second))
                return IsCallable(value);
        }
        if(constructor.type==Value::Type::Object&&constructor.object){
            switch(constructor.object->kind){
            case ObjectKind::ArrayConstructor:
                return value.type==Value::Type::Object&&value.object&&
                       value.object->kind==ObjectKind::Array;
            case ObjectKind::ObjectConstructor:
                return value.type==Value::Type::Object||value.type==Value::Type::Function||
                       value.type==Value::Type::Native;
            case ObjectKind::DateConstructor:
                return value.type==Value::Type::Object&&value.object&&
                       value.object->kind==ObjectKind::Date;
            case ObjectKind::PromiseConstructor:
                return value.type==Value::Type::Object&&value.object&&
                       value.object->kind==ObjectKind::Promise;
            case ObjectKind::ErrorConstructor:
                return value.type==Value::Type::Object&&value.object&&
                       value.object->kind==ObjectKind::Error;
            case ObjectKind::NumberConstructor:
            case ObjectKind::StringConstructor:
                return value.type==Value::Type::Object&&value.object&&
                       value.object->props.count(L"$primitive")!=0;
            default:break;
            }
        }

        if(value.type!=Value::Type::Object||!value.object)return false;
        std::shared_ptr<Object> expectedPrototype;
        if(constructor.type==Value::Type::Function&&constructor.function){
            const auto prototype=FunctionPrototype(constructor.function);
            if(prototype.type==Value::Type::Object)expectedPrototype=prototype.object;
        }else if(constructor.type==Value::Type::Native&&constructor.native){
            const auto found=constructor.native->props.find(L"prototype");
            if(found!=constructor.native->props.end()){
                const auto prototype=Deref(found->second);
                if(prototype.type==Value::Type::Object)expectedPrototype=prototype.object;
            }
        }else if(constructor.type==Value::Type::Object&&constructor.object->props.count(L"$class")){
            expectedPrototype=constructor.object;
        }
        if(!expectedPrototype)return false;
        for(auto prototype=value.object->prototype;prototype;prototype=prototype->prototype)
            if(prototype==expectedPrototype)return true;
        return false;
    }
    Value ErrorValue(const std::wstring& name,const std::wstring& message){
        auto error=ObjectValue(ObjectKind::Error);
        error.object->props[L"name"]=Value::String(name);
        error.object->props[L"message"]=Value::String(message);
        std::wstring stack=message.empty()?name:name+L": "+message;
        for(auto frame=callStack.rbegin();frame!=callStack.rend();++frame)
            stack+=L"\n    at "+frame->name+L" (line "+std::to_wstring(frame->line)+
                   L", offset "+std::to_wstring(frame->offset)+L")";
        lastCreatedError=stack;
        if(createdErrors.size()<64)createdErrors.push_back(stack);
        error.object->props[L"stack"]=Value::String(std::move(stack));
        return error;
    }
    Value PromiseValue(){
        auto promise=ObjectValue(ObjectKind::Promise);
        if(promisePrototype)promise.object->prototype=promisePrototype;
        return promise;
    }
    void EnqueueMicrotask(std::function<void()> job){microtasks.push_back(std::move(job));}
    void SchedulePromiseReaction(const std::shared_ptr<Object>& promise,PromiseReaction reaction){
        EnqueueMicrotask([this,promise,reaction=std::move(reaction)]() mutable {
            const bool fulfilled=promise->promiseState==PromiseState::Fulfilled;
            const auto handler=fulfilled?reaction.onFulfilled:reaction.onRejected;
            if(!IsCallable(handler)){
                if(fulfilled)ResolvePromise(reaction.nextPromise,promise->promiseResult);
                else RejectPromise(reaction.nextPromise,promise->promiseResult);
                return;
            }
            try{ResolvePromise(reaction.nextPromise,Call(handler,Value::Undefined(),{promise->promiseResult}));}
            catch(const JavaScriptException& exception){RejectPromise(reaction.nextPromise,exception.value);}
            catch(const std::exception& exception){RejectPromise(reaction.nextPromise,ErrorValue(L"Error",Utf8ToWide(exception.what())));}
        });
    }
    Value PerformThen(const std::shared_ptr<Object>& promise,const Value& onFulfilled,const Value& onRejected){
        auto next=PromiseValue().object;
        if(promise->promiseUnhandledNotified){promise->promiseUnhandledNotified=false;auto* runtime=this;EnqueueMicrotask([runtime,promise](){runtime->DispatchPromiseRejectionEvent(L"rejectionhandled",promise);});}
        promise->promiseHandled=true;
        PromiseReaction reaction{Deref(onFulfilled),Deref(onRejected),next};
        if(promise->promiseState==PromiseState::Pending)promise->promiseReactions.push_back(std::move(reaction));
        else SchedulePromiseReaction(promise,std::move(reaction));
        return Value::FromObject(next);
    }
    void SettlePromise(const std::shared_ptr<Object>& promise,PromiseState state,const Value& result){
        if(!promise||promise->kind!=ObjectKind::Promise||promise->promiseState!=PromiseState::Pending)return;
        promise->promiseState=state;promise->promiseResult=Deref(result);
        auto reactions=std::move(promise->promiseReactions);promise->promiseReactions.clear();
        if(state==PromiseState::Rejected&&!promise->promiseHandled&&reactions.empty())possiblyUnhandledRejections.push_back(promise);
        for(auto& reaction:reactions)SchedulePromiseReaction(promise,std::move(reaction));
    }
    void RejectPromise(const std::shared_ptr<Object>& promise,const Value& reason){SettlePromise(promise,PromiseState::Rejected,reason);}
    void ResolvePromise(const std::shared_ptr<Object>& promise,const Value& input){
        if(!promise||promise->promiseState!=PromiseState::Pending)return;
        const auto resolution=Deref(input);
        if(resolution.type==Value::Type::Object&&resolution.object==promise){RejectPromise(promise,ErrorValue(L"TypeError",L"Chaining cycle detected for promise"));return;}
        if(resolution.type==Value::Type::Object&&resolution.object&&resolution.object->kind==ObjectKind::Promise){
            const auto source=resolution.object;source->promiseHandled=true;
            PromiseReaction adoption{Value::Undefined(),Value::Undefined(),promise};
            if(source->promiseState==PromiseState::Pending)source->promiseReactions.push_back(std::move(adoption));
            else SchedulePromiseReaction(source,std::move(adoption));
            return;
        }
        if(resolution.type==Value::Type::Object&&resolution.object){
            Value then;
            try{then=GetProperty(resolution,L"then");}catch(const JavaScriptException& exception){RejectPromise(promise,exception.value);return;}
            if(IsCallable(then)){
                auto called=std::make_shared<bool>(false);
                auto resolve=Native([promise,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.ResolvePromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
                auto reject=Native([promise,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.RejectPromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
                try{Call(then,resolution,{resolve,reject});}
                catch(const JavaScriptException& exception){if(!*called){*called=true;RejectPromise(promise,exception.value);}}
                catch(const std::exception& exception){if(!*called){*called=true;RejectPromise(promise,ErrorValue(L"Error",Utf8ToWide(exception.what())));}}
                return;
            }
        }
        SettlePromise(promise,PromiseState::Fulfilled,resolution);
    }
    Value PromiseResolveValue(const Value& value){const auto resolved=Deref(value);if(resolved.type==Value::Type::Object&&resolved.object&&resolved.object->kind==ObjectKind::Promise)return resolved;auto promise=PromiseValue();ResolvePromise(promise.object,resolved);return promise;}
    Value ConstructPromise(const Value& executor){
        if(!IsCallable(executor))throw JavaScriptException{ErrorValue(L"TypeError",L"Promise resolver is not a function")};
        auto promise=PromiseValue();auto called=std::make_shared<bool>(false);
        auto resolve=Native([promise=promise.object,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.ResolvePromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
        auto reject=Native([promise=promise.object,called](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){if(!*called){*called=true;runtime.RejectPromise(promise,args.empty()?Value::Undefined():args[0]);}return Value::Undefined();});
        try{Call(executor,Value::Undefined(),{resolve,reject});}
        catch(const JavaScriptException& exception){if(!*called){*called=true;RejectPromise(promise.object,exception.value);}}
        catch(const std::exception& exception){if(!*called){*called=true;RejectPromise(promise.object,ErrorValue(L"Error",Utf8ToWide(exception.what())));}}
        return promise;
    }
    void DispatchPromiseRejectionEvent(const std::wstring& type,const std::shared_ptr<Object>& promise){
        auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(type);event->props[L"promise"]=Value::FromObject(promise);event->props[L"reason"]=promise->promiseResult;
        const auto eventValue=Value::FromObject(event);const auto target=global->values[L"window"];
        event->props[L"target"]=target;event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
        InvokeEventListeners(windowListeners,type,true,target,eventValue,event);
        InvokeEventListeners(windowListeners,type,false,target,eventValue,event);
    }
    void CollectManagedCycles(){
        std::vector<std::shared_ptr<Object>> objects;
        std::vector<std::shared_ptr<Function>> functions;
        std::vector<std::shared_ptr<NativeFunction>> natives;
        std::vector<std::shared_ptr<Environment>> environments;
        objects.reserve(managedObjects.size());functions.reserve(managedFunctions.size());
        natives.reserve(managedNativeFunctions.size());environments.reserve(managedEnvironments.size());
        for(const auto& entry:managedObjects)if(auto value=entry.second.lock())objects.push_back(std::move(value));
        for(const auto& entry:managedFunctions)if(auto value=entry.second.lock())functions.push_back(std::move(value));
        for(const auto& entry:managedNativeFunctions)if(auto value=entry.second.lock())natives.push_back(std::move(value));
        for(const auto& entry:managedEnvironments)if(auto value=entry.second.lock())environments.push_back(std::move(value));

        std::unordered_map<const Object*,size_t> objectIncoming;
        std::unordered_map<const Function*,size_t> functionIncoming;
        std::unordered_map<const NativeFunction*,size_t> nativeIncoming;
        std::unordered_map<const Environment*,size_t> environmentIncoming;
        std::function<void(const Value&)> countValue=[&](const Value& value){
            if(value.object)++objectIncoming[value.object.get()];
            if(value.function)++functionIncoming[value.function.get()];
            if(value.native)++nativeIncoming[value.native.get()];
            if(value.reference){
                if(value.reference->env)++environmentIncoming[value.reference->env.get()];
                countValue(value.reference->base);
            }
        };
        for(const auto& object:objects){
            for(const auto& property:object->props)countValue(property.second);
            for(const auto& value:object->items)countValue(value);
            for(const auto& entry:object->entries){countValue(entry.first);countValue(entry.second);}
            if(object->prototype)++objectIncoming[object->prototype.get()];
            countValue(object->promiseResult);
            for(const auto& reaction:object->promiseReactions){
                countValue(reaction.onFulfilled);countValue(reaction.onRejected);
                if(reaction.nextPromise)++objectIncoming[reaction.nextPromise.get()];
            }
        }
        for(const auto& function:functions){
            if(function->closure)++environmentIncoming[function->closure.get()];
            for(const auto& property:function->props)countValue(property.second);
        }
        for(const auto& native:natives)
            for(const auto& property:native->props)countValue(property.second);
        for(const auto& environment:environments){
            if(environment->parent)++environmentIncoming[environment->parent.get()];
            for(const auto& binding:environment->values)countValue(binding.second);
        }

        std::unordered_set<const Object*> markedObjects;
        std::unordered_set<const Function*> markedFunctions;
        std::unordered_set<const NativeFunction*> markedNatives;
        std::unordered_set<const Environment*> markedEnvironments;
        std::deque<std::shared_ptr<Object>> objectQueue;
        std::deque<std::shared_ptr<Function>> functionQueue;
        std::deque<std::shared_ptr<NativeFunction>> nativeQueue;
        std::deque<std::shared_ptr<Environment>> environmentQueue;
        const auto markObject=[&](const std::shared_ptr<Object>& value){
            if(value&&markedObjects.insert(value.get()).second)objectQueue.push_back(value);
        };
        const auto markFunction=[&](const std::shared_ptr<Function>& value){
            if(value&&markedFunctions.insert(value.get()).second)functionQueue.push_back(value);
        };
        const auto markNative=[&](const std::shared_ptr<NativeFunction>& value){
            if(value&&markedNatives.insert(value.get()).second)nativeQueue.push_back(value);
        };
        const auto markEnvironment=[&](const std::shared_ptr<Environment>& value){
            if(value&&markedEnvironments.insert(value.get()).second)environmentQueue.push_back(value);
        };
        std::function<void(const Value&)> markValue=[&](const Value& value){
            markObject(value.object);markFunction(value.function);markNative(value.native);
            if(value.reference){markEnvironment(value.reference->env);markValue(value.reference->base);}
        };
        // Each vector above contributes one temporary strong reference. Any
        // count beyond graph edges plus that temporary owner comes from an
        // actual engine root (global state, timers/listeners, a pending async
        // operation, bytecode constants, or host code).
        for(const auto& object:objects)
            if(static_cast<size_t>(object.use_count())>objectIncoming[object.get()]+1)markObject(object);
        for(const auto& function:functions)
            if(static_cast<size_t>(function.use_count())>functionIncoming[function.get()]+1)markFunction(function);
        for(const auto& native:natives)
            if(static_cast<size_t>(native.use_count())>nativeIncoming[native.get()]+1)markNative(native);
        for(const auto& environment:environments)
            if(static_cast<size_t>(environment.use_count())>environmentIncoming[environment.get()]+1)
                markEnvironment(environment);

        while(!objectQueue.empty()||!functionQueue.empty()||!nativeQueue.empty()||
              !environmentQueue.empty()){
            while(!objectQueue.empty()){
                auto object=std::move(objectQueue.front());objectQueue.pop_front();
                for(const auto& property:object->props)markValue(property.second);
                for(const auto& value:object->items)markValue(value);
                for(const auto& entry:object->entries){markValue(entry.first);markValue(entry.second);}
                markObject(object->prototype);markValue(object->promiseResult);
                for(const auto& reaction:object->promiseReactions){
                    markValue(reaction.onFulfilled);markValue(reaction.onRejected);
                    markObject(reaction.nextPromise);
                }
            }
            while(!functionQueue.empty()){
                auto function=std::move(functionQueue.front());functionQueue.pop_front();
                markEnvironment(function->closure);
                for(const auto& property:function->props)markValue(property.second);
            }
            while(!nativeQueue.empty()){
                auto native=std::move(nativeQueue.front());nativeQueue.pop_front();
                for(const auto& property:native->props)markValue(property.second);
            }
            while(!environmentQueue.empty()){
                auto environment=std::move(environmentQueue.front());environmentQueue.pop_front();
                markEnvironment(environment->parent);
                for(const auto& binding:environment->values)markValue(binding.second);
            }
        }

        for(const auto& native:natives)if(markedNatives.count(native.get())==0){
            native->callback={};native->props.clear();
        }
        for(const auto& function:functions)if(markedFunctions.count(function.get())==0){
            function->closure.reset();function->prototype.reset();function->props.clear();
        }
        for(const auto& environment:environments)if(markedEnvironments.count(environment.get())==0){
            environment->values.clear();environment->parent.reset();
        }
        for(const auto& object:objects)if(markedObjects.count(object.get())==0){
            object->props.clear();object->items.clear();object->entries.clear();
            object->prototype.reset();object->promiseResult=Value::Undefined();
            object->promiseReactions.clear();object->node.reset();object->canvasGradient.reset();
        }
        objects.clear();functions.clear();natives.clear();environments.clear();
        PruneExpiredManagedEntries(managedObjects);PruneExpiredManagedEntries(managedFunctions);
        PruneExpiredManagedEntries(managedNativeFunctions);PruneExpiredManagedEntries(managedEnvironments);
        managedAllocationsSinceSweep=0;
        managedSweepInterval=std::max<size_t>(256,managedObjects.size()+managedFunctions.size()+
            managedNativeFunctions.size()+managedEnvironments.size()+nodeObjects.size()+
            canvasContexts.size()+frameWindows.size()+frameDocuments.size()+listeners.size());
    }
    void CollectUnusedPrototypes(){
        if(!module||module->prototypes.empty()||activeExecutionFrames||!callStack.empty())return;
        std::vector<unsigned char> marked(module->prototypes.size(),0);
        std::function<void(size_t)> markPrototype;
        std::function<void(const Chunk&)> markChunk;
        markPrototype=[&](size_t index){
            if(index>=module->prototypes.size()||marked[index])return;
            const auto& prototype=module->prototypes[index];
            if(!prototype)return;
            marked[index]=1;
            markChunk(prototype->chunk);
            for(const auto& value:prototype->parameterDefaults)if(value)markChunk(*value);
            for(const auto& binding:prototype->bindings)if(binding.defaultValue)
                markChunk(*binding.defaultValue);
        };
        markChunk=[&](const Chunk& chunk){
            for(const auto& instruction:chunk.code)
                if(instruction.op==Op::MakeFunction&&instruction.argument>=0)
                    markPrototype(static_cast<size_t>(instruction.argument));
            for(const auto& declaration:chunk.functionDeclarations)
                if(declaration.prototype>=0)
                    markPrototype(static_cast<size_t>(declaration.prototype));
            for(const auto& embedded:chunk.embeddedExpressions)if(embedded)markChunk(*embedded);
        };
        // The module vector owns one reference. Any additional owner is a live
        // Function or suspended execution frame. Preserve its complete nested
        // function tree, then release bytecode that belonged only to a finished
        // top-level script.
        for(size_t index=0;index<module->prototypes.size();++index){
            const auto& prototype=module->prototypes[index];
            if(prototype&&prototype.use_count()>1)markPrototype(index);
        }
        for(const auto& entry:templatePrograms)if(entry.second)
            for(const auto& expression:entry.second->expressions)markChunk(expression);
        for(size_t index=0;index<module->prototypes.size();++index)
            if(!marked[index])module->prototypes[index].reset();
    }
    void DrainMicrotasks(){
        if(drainingMicrotasks)return;drainingMicrotasks=true;
        for(;;){
            while(!microtasks.empty()){
                auto job=std::move(microtasks.front());microtasks.pop_front();
                try{MutationBatch batch(*this);job();}
                catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}
                catch(const std::exception& exception){lastError=Utf8ToWide(exception.what());}
            }
            auto pending=std::move(possiblyUnhandledRejections);possiblyUnhandledRejections.clear();
            for(const auto& weak:pending)if(auto promise=weak.lock())if(promise->promiseState==PromiseState::Rejected&&!promise->promiseHandled&&!promise->promiseUnhandledNotified){promise->promiseUnhandledNotified=true;DispatchPromiseRejectionEvent(L"unhandledrejection",promise);}
            if(microtasks.empty())break;
        }
        drainingMicrotasks=false;
        // A full shared-pointer cycle scan walks the complete live JavaScript
        // graph. Running it after every Promise reaction made large advertising
        // bundles repeatedly rescan the same objects. Request collection from
        // the interpreter and perform it only at this safe job boundary, with
        // the next interval scaled to the retained graph.
        if(cycleCollectionPending){
            CollectManagedCycles();cycleCollectionPending=false;
            const auto live=managedObjects.size()+managedFunctions.size()+
                managedNativeFunctions.size()+managedEnvironments.size();
            const auto interval=std::max<std::uint64_t>(1000000,
                static_cast<std::uint64_t>(live)*16);
            cycleCollectionNext=executedInstructions+interval;
        }
        // Prototype reachability changes when a script adds a sizeable batch
        // of functions, not on every microtask. Amortize this independent scan
        // over compiler growth as well.
        if(module&&module->prototypes.size()>=prototypeSweepWatermark+32){
            CollectUnusedPrototypes();prototypeSweepWatermark=module->prototypes.size();
        }
    }
    void BeginMutationBatch(){++mutationBatchDepth;}
    void FlushMutations(){
        if(indexDirty){document.Reindex();indexDirty=false;}
        if(mutationPending&&mutationSink){
            JavaScriptRuntime::Mutation mutation;
            mutation.kind=mutationKind;
            mutation.targets=std::move(mutationTargets);
            mutation.liveRegionMembershipChanged=liveRegionMembershipChanged;
            mutationPending=false;mutationKind=JavaScriptRuntime::MutationKind::Paint;
            liveRegionMembershipChanged=false;
            mutationSink(mutation);
        }else{
            mutationPending=false;mutationTargets.clear();
            mutationKind=JavaScriptRuntime::MutationKind::Paint;
            liveRegionMembershipChanged=false;
        }
    }
    void EndMutationBatch(){if(mutationBatchDepth>0&&--mutationBatchDepth==0)FlushMutations();}
    void EnsureIndex(){if(indexDirty){document.Reindex();indexDirty=false;}}
    void DeliverMutationObservers(){
        mutationObserverMicrotaskScheduled=false;
        std::vector<std::shared_ptr<Object>> ready;
        mutationObservers.erase(std::remove_if(mutationObservers.begin(),mutationObservers.end(),
            [&](const MutationObserverRegistration& registration){
                const auto observer=registration.observer.lock();
                const auto target=registration.target.lock();
                if(!observer||!target)return true;
                if(!observer->items.empty()&&std::none_of(ready.begin(),ready.end(),
                    [&](const auto& candidate){return candidate==observer;}))ready.push_back(observer);
                return false;
            }),mutationObservers.end());
        for(const auto& observer:ready){
            auto records=std::move(observer->items);observer->items.clear();
            const auto callback=observer->props.find(L"$callback");
            if(callback==observer->props.end()||!IsCallable(callback->second))continue;
            Call(callback->second,Value::Undefined(),
                 {ArrayValue(records),Value::FromObject(observer)});
        }
    }
    void QueueMutationObservers(const std::shared_ptr<Node>& target,
                                JavaScriptRuntime::MutationKind kind,
                                const std::wstring& attributeName){
        if(!target||mutationObservers.empty())return;
        const bool attributeMutation=!attributeName.empty();
        const bool characterMutation=!attributeMutation&&target->type==NodeType::Text;
        bool queued=false;
        for(auto iterator=mutationObservers.begin();iterator!=mutationObservers.end();){
            const auto observer=iterator->observer.lock();const auto observed=iterator->target.lock();
            if(!observer||!observed){iterator=mutationObservers.erase(iterator);continue;}
            bool inScope=target==observed;
            if(!inScope&&iterator->subtree)for(auto current=target->parent.lock();current;
                current=current->parent.lock())if(current==observed){inScope=true;break;}
            bool requested=inScope&&(attributeMutation?iterator->attributes:
                characterMutation?iterator->characterData:iterator->childList&&
                    kind==JavaScriptRuntime::MutationKind::Tree);
            if(requested&&attributeMutation&&!iterator->attributeFilter.empty()&&
               std::find(iterator->attributeFilter.begin(),iterator->attributeFilter.end(),
                         ToLower(attributeName))==iterator->attributeFilter.end())requested=false;
            if(requested){
                auto record=ObjectValue(ObjectKind::Plain);
                record.object->props[L"type"]=Value::String(attributeMutation?L"attributes":
                    characterMutation?L"characterData":L"childList");
                record.object->props[L"target"]=NodeValue(target);
                record.object->props[L"attributeName"]=attributeMutation?
                    Value::String(ToLower(attributeName)):Value::Null();
                record.object->props[L"attributeNamespace"]=Value::Null();
                record.object->props[L"oldValue"]=Value::Null();
                record.object->props[L"addedNodes"]=ArrayValue({});
                record.object->props[L"removedNodes"]=ArrayValue({});
                record.object->props[L"previousSibling"]=Value::Null();
                record.object->props[L"nextSibling"]=Value::Null();
                observer->items.push_back(record);queued=true;
            }
            ++iterator;
        }
        if(queued&&!mutationObserverMicrotaskScheduled){
            mutationObserverMicrotaskScheduled=true;
            EnqueueMicrotask([this]{DeliverMutationObservers();});
        }
    }
    void Mutated(const std::shared_ptr<Node>& target,JavaScriptRuntime::MutationKind kind,
                 bool requiresIndex=false,bool liveRegionMembership=false,
                 const std::wstring& attributeName=L""){
        QueueMutationObservers(target,kind,attributeName);
        mutationPending=true;
        if(static_cast<int>(kind)>static_cast<int>(mutationKind))mutationKind=kind;
        if(target&&std::none_of(mutationTargets.begin(),mutationTargets.end(),
            [&](const auto& candidate){return candidate==target;}))mutationTargets.push_back(target);
        indexDirty=indexDirty||requiresIndex;
        liveRegionMembershipChanged=liveRegionMembershipChanged||liveRegionMembership;
        if(mutationBatchDepth==0)FlushMutations();
    }
    unsigned ScheduleAnimationFrame(const Value& callback){
        const unsigned id=nextFrameId++;
        frameCallbacks.emplace_back(id,Deref(callback));
        if(frameScheduler&&!frameScheduled){frameScheduled=true;frameScheduler();}
        return id;
    }
    void RunAnimationFrame(){
        frameScheduled=false;
        std::vector<std::pair<unsigned,Value>> callbacks;
        callbacks.swap(frameCallbacks);
        if(callbacks.empty())return;
        MutationBatch batch(*this);
        using namespace std::chrono;
        const auto timestamp=Value::Number(duration<double,std::milli>(steady_clock::now().time_since_epoch()).count());
        for(const auto& callback:callbacks){try{Call(callback.second,Value::Undefined(),{timestamp});}catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}DrainMicrotasks();}
    }
    void ScheduleNextTimer(){
        if(!tasks.empty()){
            const auto now=std::chrono::steady_clock::now();
            if(timerScheduled&&timerWake<=now)return;
            timerScheduled=true;timerWake=now;
            if(timerScheduler)timerScheduler(1);
            return;
        }
        if(timers.empty()){timerScheduled=false;if(timerScheduler)timerScheduler(0);return;}
        const auto earliest=std::min_element(timers.begin(),timers.end(),[](const auto& a,const auto& b){return a.due<b.due;})->due;
        if(timerScheduled&&earliest>=timerWake)return;
        timerScheduled=true;timerWake=earliest;
        const auto now=std::chrono::steady_clock::now();
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(earliest-now).count();
        if(timerScheduler)timerScheduler(static_cast<unsigned>(std::max<long long>(1,remaining)));
    }
    void EnqueueTask(std::function<void()> job){
        tasks.push_back(std::move(job));ScheduleNextTimer();
    }
    unsigned ScheduleTimer(const Value& callback,double delay,bool repeat=false){
        const unsigned id=nextTimerId++;
        const auto milliseconds=static_cast<long long>(std::max(0.0,std::min(delay,2147483647.0)));
        timers.push_back({id,Deref(callback),std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds),repeat?static_cast<unsigned>(std::max<long long>(1,milliseconds)):0});
        ScheduleNextTimer();return id;
    }
    void ClearTimer(unsigned id){
        timers.erase(std::remove_if(timers.begin(),timers.end(),[&](const auto& timer){return timer.id==id;}),timers.end());
        timerScheduled=false;ScheduleNextTimer();
    }
    void RunTimers(){
        timerScheduled=false;
        if(!tasks.empty()){
            // Starting one queued task per Windows timer tick adds avoidable
            // latency when a page inserts several independent async scripts.
            // Drain a small browser-style time slice, keeping a microtask
            // checkpoint after every task and yielding before UI input starves.
            constexpr size_t kMaximumTasksPerSlice=16;
            const auto deadline=std::chrono::steady_clock::now()+
                std::chrono::milliseconds(4);
            size_t completed=0;
            do{
                auto task=std::move(tasks.front());tasks.pop_front();
                try{MutationBatch batch(*this);task();DrainMicrotasks();}
                catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}
                catch(const std::exception& exception){lastError=Utf8ToWide(exception.what());}
                ++completed;
            }while(!tasks.empty()&&completed<kMaximumTasksPerSlice&&
                    std::chrono::steady_clock::now()<deadline);
            ScheduleNextTimer();return;
        }
        const auto now=std::chrono::steady_clock::now();
        const auto due=std::min_element(timers.begin(),timers.end(),
            [](const auto& left,const auto& right){return left.due<right.due;});
        if(due!=timers.end()&&due->due<=now+std::chrono::milliseconds(1)){
            const auto callback=due->callback;
            if(due->interval)due->due=now+std::chrono::milliseconds(due->interval);
            else timers.erase(due);
            try{MutationBatch batch(*this);Call(callback,Value::Undefined(),{});DrainMicrotasks();}
            catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}
            catch(const std::exception& exception){lastError=Utf8ToWide(exception.what());}
        }
        ScheduleNextTimer();
    }
    bool IsConnected(const std::shared_ptr<Node>& node)const{
        auto current=node;
        while(current&&current->parent.lock())current=current->parent.lock();
        return current&&current==document.Root();
    }
    void CompleteExternalScript(const std::shared_ptr<Node>& node,
                                const std::shared_ptr<Object>& scriptObject,
                                bool loaded,std::wstring source){
        (void)scriptObject;
        MutationBatch batch(*this);
        const auto previousScript=currentScript;
        currentScript=node;
        try{
            if(!loaded){
                currentScript=previousScript;Dispatch(node,L"error");
                DrainMicrotasks();return;
            }
            if(!source.empty()){
                Compiler compiler(module,source);
                auto program=compiler.CompileProgram();
                Run(program,global);
            }
            currentScript=previousScript;
            Dispatch(node,L"load");
            DrainMicrotasks();
        }catch(const JavaScriptException& exception){
            currentScript=previousScript;
            lastError=L"Uncaught "+String(exception.value);
            Dispatch(node,L"error");DrainMicrotasks();
        }catch(const std::exception& exception){
            currentScript=previousScript;
            lastError=Utf8ToWide(exception.what());
            Dispatch(node,L"error");DrainMicrotasks();
        }
    }
    void DrainOrderedScriptLoads(){
        while(!orderedScriptLoads.empty()&&orderedScriptLoads.front()->ready){
            auto load=std::move(orderedScriptLoads.front());
            orderedScriptLoads.pop_front();
            CompleteExternalScript(load->node,load->scriptObject,load->loaded,
                                   std::move(load->source));
        }
    }
    void ExecuteConnectedScripts(const std::shared_ptr<Node>& root){
        if(!root||!IsConnected(root))return;
        std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& node){
            if(!node)return;
            if(node->type==NodeType::Element&&node->tag==L"script"&&!node->scriptStarted){
                node->scriptStarted=true;
                const auto resource=node->Attribute(L"src");
                if(!resource.empty()){
                    // A DOM-inserted external classic script is async by default.  In
                    // particular it must not execute inside appendChild()/insertBefore():
                    // callers commonly install the script's callback immediately after
                    // insertion.  Queue the load as engine work so it runs after the
                    // current JavaScript job has returned.  Inline inserted scripts keep
                    // their required synchronous behavior below.
                    // A connected DOM node owns its event-handler properties in the
                    // browser even after the last JavaScript reference to the wrapper
                    // goes out of scope. Keep the wrapper alive until this resource
                    // task has dispatched load/error; otherwise cycle collection can
                    // discard script.onload between appendChild() and the async task.
                    const auto scriptObject=NodeValue(node).object;
                    const auto asyncProperty=scriptObject->props.find(L"async");
                    const bool ordered=asyncProperty!=scriptObject->props.end()&&
                                       !Truth(asyncProperty->second);
                    std::shared_ptr<OrderedScriptLoad> orderedLoad;
                    if(ordered){
                        orderedLoad=std::make_shared<OrderedScriptLoad>();
                        orderedLoad->node=node;orderedLoad->scriptObject=scriptObject;
                        orderedScriptLoads.push_back(orderedLoad);
                    }
                    EnqueueTask([this,node,resource,scriptObject,orderedLoad]{
                        auto complete=[this,node,scriptObject,orderedLoad]
                            (bool loaded,std::wstring source){
                            if(orderedLoad){
                                orderedLoad->loaded=loaded;
                                orderedLoad->source=std::move(source);
                                orderedLoad->ready=true;
                                DrainOrderedScriptLoads();return;
                            }
                            CompleteExternalScript(node,scriptObject,loaded,std::move(source));
                        };
                        // Parallel loading must include DOM-inserted scripts.
                        // The old path called the synchronous loader from the
                        // UI task, so a slow network response froze painting
                        // and delayed every subsequently inserted ad script.
                        if(asyncResourceLoader){
                            asyncResourceLoader(resource,std::move(complete));
                            return;
                        }
                        std::wstring source;
                        const bool loaded=resourceLoader&&resourceLoader(resource,source);
                        complete(loaded,std::move(source));
                    });
                }else{
                    const auto source=node->InnerText();
                    if(!source.empty()){
                        const auto previousScript=currentScript;
                        currentScript=node;
                        try{
                            Compiler compiler(module,source);
                            auto program=compiler.CompileProgram();
                            Run(program,global);
                            DrainMicrotasks();
                        }catch(...){currentScript=previousScript;throw;}
                        currentScript=previousScript;
                    }
                    Dispatch(node,L"load");
                }
            }
            for(const auto& child:node->children)visit(child);
        };
        visit(root);
    }
    Value GetProperty(const Value& input,const std::wstring& key){
        const auto base=Deref(input);
        if(base.type==Value::Type::Function&&base.function){
            const auto found=base.function->props.find(key);
            if(found!=base.function->props.end())return Deref(found->second);
            const auto getter=base.function->props.find(L"$get:"+key);
            if(getter!=base.function->props.end())return Call(getter->second,base,{});
            if(key==L"prototype")return FunctionPrototype(base.function);
            const auto inherited=base.function->props.find(L"$prototypeValue");
            if(inherited!=base.function->props.end()){
                const auto prototype=Deref(inherited->second);
                if(!EqualValues(prototype,base)&&prototype.type!=Value::Type::Null)
                    return GetProperty(prototype,key);
            }
        }
        if(base.type==Value::Type::Native&&base.native){
            const auto found=base.native->props.find(key);
            if(found!=base.native->props.end())return Deref(found->second);
            const auto getter=base.native->props.find(L"$get:"+key);
            if(getter!=base.native->props.end())return Call(getter->second,base,{});
            const auto inherited=base.native->props.find(L"$prototypeValue");
            if(inherited!=base.native->props.end()){
                const auto prototype=Deref(inherited->second);
                if(!EqualValues(prototype,base)&&prototype.type!=Value::Type::Null)
                    return GetProperty(prototype,key);
            }
        }
        if((base.type==Value::Type::Function||base.type==Value::Type::Native)&&
           (key==L"hasOwnProperty"||key==L"propertyIsEnumerable"))
            return Native([key](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& arguments){
                if(arguments.empty())return Value::Bool(false);
                const auto value=r.Deref(thisValue);const auto name=r.String(arguments[0]);
                if(key==L"propertyIsEnumerable")
                    return Value::Bool(r.OwnPropertyIsEnumerable(value,name));
                if(value.type==Value::Type::Function&&value.function)
                    return Value::Bool(value.function->props.count(name)!=0);
                if(value.type==Value::Type::Native&&value.native)
                    return Value::Bool(value.native->props.count(name)!=0);
                return Value::Bool(false);
            });
        if((base.type==Value::Type::Function||base.type==Value::Type::Native)&&
           key==L"toString")
            return Native([base](RuntimeCore&,const Value&,const std::vector<Value>&){
                std::wstring name;
                if(base.type==Value::Type::Function&&base.function&&base.function->prototype)
                    name=base.function->prototype->name;
                else if(base.type==Value::Type::Native&&base.native){
                    const auto found=base.native->props.find(L"name");
                    if(found!=base.native->props.end()&&found->second.type==Value::Type::String)
                        name=found->second.string;
                }
                return Value::String(L"function "+name+L"() { [native code] }");
            });
        if(base.type==Value::Type::Number&&key==L"toFixed")return Native([value=base.number](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const int digits=a.empty()?0:std::max(0,std::min(100,static_cast<int>(r.Number(a[0]))));
            if(!std::isfinite(value))return Value::String(NumberString(value));
            std::wostringstream out;out<<std::fixed<<std::setprecision(digits)<<value;
            return Value::String(out.str());
        });
        if(base.type==Value::Type::Number&&(key==L"toString"||key==L"valueOf"))
            return Native([value=base.number,key](RuntimeCore&,const Value&,const std::vector<Value>&){
                return key==L"valueOf"?Value::Number(value):Value::String(NumberString(value));
            });
        if(base.type==Value::Type::Boolean&&(key==L"toString"||key==L"valueOf"))
            return Native([value=base.boolean,key](RuntimeCore&,const Value&,const std::vector<Value>&){
                return key==L"valueOf"?Value::Bool(value):Value::String(value?L"true":L"false");
            });
        if(IsCallable(base)&&(key==L"bind"||key==L"call"||key==L"apply")){
            // call/apply/bind live on Function.prototype and use the call-site
            // receiver as their callable.  Capturing `base` here is incorrect
            // when a function has a custom [[Prototype]]: resolving the method
            // through that chain would bind it to the prototype function rather
            // than to the original receiver (a pattern used by regenerator).
            return Native([key](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){
                const auto callable=r.Deref(thisValue);
                if(!r.IsCallable(callable))
                    throw JavaScriptException{r.ErrorValue(L"TypeError",key+L" called on incompatible receiver")};
                const auto boundThis=a.empty()?Value::Undefined():r.Deref(a[0]);
                if(key==L"call"){std::vector<Value> forwarded;if(a.size()>1)forwarded.assign(a.begin()+1,a.end());return r.Call(callable,boundThis,forwarded);}
                if(key==L"apply"){
                    std::vector<Value> forwarded;
                    if(a.size()>1){
                        const auto values=r.Deref(a[1]);
                        if(values.type==Value::Type::Object&&values.object){
                            if(values.object->kind==ObjectKind::Array)forwarded=values.object->items;
                            else{
                                const auto length=static_cast<size_t>(std::max(0.0,r.Number(r.GetProperty(values,L"length"))));
                                forwarded.reserve(length);
                                for(size_t index=0;index<length;++index)
                                    forwarded.push_back(r.GetProperty(values,std::to_wstring(index)));
                            }
                        }
                    }
                    return r.Call(callable,boundThis,forwarded);
                }
                std::vector<Value> leading;
                if(a.size()>1)leading.assign(a.begin()+1,a.end());
                return r.Native([callable,boundThis,leading=std::move(leading)](RuntimeCore& inner,const Value&,const std::vector<Value>& args){
                    auto forwarded=leading;forwarded.insert(forwarded.end(),args.begin(),args.end());
                    return inner.Call(callable,boundThis,forwarded);
                });
            });
        }
        if(base.type==Value::Type::String){
            if(key==L"length")return Value::Number(static_cast<double>(base.string.size()));
            if(key==L"toString"||key==L"valueOf")return Native([s=base.string](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(s);});
            size_t index=0;if(TryParseDecimalIndex(key,index)&&index<base.string.size())return Value::String(std::wstring(1,base.string[index]));
            if(key==L"indexOf")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto found=s.find(a.empty()?L"":r.String(a[0]),a.size()>1?static_cast<size_t>(std::max(0.0,r.Number(a[1]))):0);return Value::Number(found==std::wstring::npos?-1.0:static_cast<double>(found));});
            if(key==L"lastIndexOf")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto needle=a.empty()?L"":r.String(a[0]);size_t position=std::wstring::npos;if(a.size()>1){const auto number=r.Number(a[1]);if(!std::isnan(number)){const auto integer=std::trunc(number);position=static_cast<size_t>(std::max(0.0,std::min(static_cast<double>(s.size()),integer)));}}const auto found=s.rfind(needle,position);return Value::Number(found==std::wstring::npos?-1.0:static_cast<double>(found));});
            if(key==L"toLowerCase"||key==L"toLocaleLowerCase"||key==L"toUpperCase")return Native([s=base.string,key](RuntimeCore&,const Value&,const std::vector<Value>&){auto out=s;std::transform(out.begin(),out.end(),out.begin(),[&](wchar_t c){return key==L"toUpperCase"?std::towupper(c):std::towlower(c);});return Value::String(out);});
            if(key==L"trim")return Native([s=base.string](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(Trim(s));});
            if(key==L"trimStart")return Native([s=base.string](RuntimeCore&,const Value&,const std::vector<Value>&){const auto begin=std::find_if_not(s.begin(),s.end(),[](wchar_t character){return std::iswspace(character)!=0;});return Value::String(std::wstring(begin,s.end()));});
            if(key==L"charAt"||key==L"charCodeAt"||key==L"codePointAt")
                return Native([s=base.string,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                    const double raw=a.empty()?0:r.Number(a[0]);
                    const auto index=std::isnan(raw)?0ll:static_cast<long long>(std::trunc(raw));
                    if(index<0||static_cast<size_t>(index)>=s.size())
                        return key==L"charAt"?Value::String(L""):
                               key==L"charCodeAt"?Value::Number(std::numeric_limits<double>::quiet_NaN()):
                                                    Value::Undefined();
                    const auto first=static_cast<std::uint32_t>(s[static_cast<size_t>(index)]);
                    if(key==L"charAt")return Value::String(std::wstring(1,static_cast<wchar_t>(first)));
                    if(key==L"codePointAt"&&first>=0xd800&&first<=0xdbff&&
                       static_cast<size_t>(index+1)<s.size()){
                        const auto second=static_cast<std::uint32_t>(s[static_cast<size_t>(index+1)]);
                        if(second>=0xdc00&&second<=0xdfff)
                            return Value::Number(static_cast<double>(0x10000+((first-0xd800)<<10)+(second-0xdc00)));
                    }
                    return Value::Number(static_cast<double>(first));
                });
            if(key==L"substring")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto offset=[&](size_t index,size_t fallback){
                    if(index>=a.size())return fallback;const double raw=r.Number(a[index]);
                    if(std::isnan(raw)||raw<=0)return size_t{0};
                    return static_cast<size_t>(std::min(static_cast<double>(s.size()),std::trunc(raw)));
                };
                auto begin=offset(0,0),end=offset(1,s.size());if(begin>end)std::swap(begin,end);
                return Value::String(s.substr(begin,end-begin));
            });
            if(key==L"substr")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto size=static_cast<long long>(s.size());
                auto begin=a.empty()?0ll:static_cast<long long>(std::trunc(r.Number(a[0])));
                if(begin<0)begin=std::max(0ll,size+begin);else begin=std::min(size,begin);
                auto count=a.size()>1?static_cast<long long>(std::trunc(r.Number(a[1]))):size-begin;
                count=std::max(0ll,std::min(size-begin,count));
                return Value::String(s.substr(static_cast<size_t>(begin),static_cast<size_t>(count)));
            });
            if(key==L"includes")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto needle=a.empty()?L"undefined":r.String(a[0]);
                const auto start=a.size()>1?static_cast<size_t>(std::max(0.0,r.Number(a[1]))):0;
                return Value::Bool(start<=s.size()&&s.find(needle,start)!=std::wstring::npos);
            });
            if(key==L"startsWith")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto prefix=a.empty()?L"":r.String(a[0]);return Value::Bool(s.rfind(prefix,0)==0);});
            if(key==L"endsWith")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto suffix=a.empty()?L"":r.String(a[0]);size_t end=s.size();if(a.size()>1)end=static_cast<size_t>(std::max(0.0,std::min(static_cast<double>(s.size()),r.Number(a[1]))));return Value::Bool(suffix.size()<=end&&s.compare(end-suffix.size(),suffix.size(),suffix)==0);});
            if(key==L"concat")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto result=s;for(const auto& value:a)result+=r.String(value);return Value::String(std::move(result));});
            if(key==L"repeat")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto count=a.empty()?0ll:static_cast<long long>(r.Number(a[0]));if(count<0||count>1000000)throw JavaScriptException{r.ErrorValue(L"RangeError",L"Invalid count value")};std::wstring result;result.reserve(s.size()*static_cast<size_t>(count));for(long long index=0;index<count;++index)result+=s;return Value::String(std::move(result));});
            if(key==L"slice")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto size=static_cast<long long>(s.size());auto offset=[&](size_t i,long long fallback){if(i>=a.size())return fallback;auto n=static_cast<long long>(r.Number(a[i]));return n<0?std::max(0ll,size+n):std::min(size,n);};const auto begin=offset(0,0),end=offset(1,size);return Value::String(s.substr(static_cast<size_t>(begin),static_cast<size_t>(std::max(0ll,end-begin))));});
            if(key==L"split")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::vector<Value> out;
                const size_t limit=a.size()>1?static_cast<size_t>(r.Uint32(a[1])):
                    static_cast<size_t>((std::numeric_limits<std::uint32_t>::max)());
                if(limit==0)return r.ArrayValue(out);
                const auto separatorValue=a.empty()?Value::Undefined():r.Deref(a[0]);
                if(separatorValue.type==Value::Type::Undefined){out.push_back(Value::String(s));return r.ArrayValue(out);}
                auto append=[&](const std::wstring& value){if(out.size()<limit)out.push_back(Value::String(value));};
                if(separatorValue.type==Value::Type::Object&&separatorValue.object&&
                   separatorValue.object->kind==ObjectKind::RegExp){
                    const auto pattern=r.String(separatorValue.object->props[L"$pattern"]);
                    const auto flags=r.String(separatorValue.object->props[L"$flags"]);
                    if(const auto finiteSeparator=r.FiniteRegularExpression(pattern,flags)){
                        size_t lastEnd=0,searchStart=0;
                        bool matchedEmptyInput=false;
                        while(searchStart<=s.size()&&out.size()<limit){
                            size_t matchStart=searchStart,matchLength=0;
                            for(;matchStart<=s.size();++matchStart)
                                if(finiteSeparator->MatchAt(s,matchStart,matchLength))break;
                            if(matchStart>s.size())break;
                            if(matchLength==0&&matchStart==lastEnd){
                                matchedEmptyInput=matchedEmptyInput||s.empty();
                                if(matchStart>=s.size())break;
                                searchStart=matchStart+1;
                                continue;
                            }
                            if(matchLength==0&&matchStart==s.size())break;
                            append(s.substr(lastEnd,matchStart-lastEnd));
                            lastEnd=matchStart+matchLength;
                            searchStart=matchLength==0?matchStart+1:lastEnd;
                        }
                        if(out.size()<limit&&!(s.empty()&&matchedEmptyInput))append(s.substr(lastEnd));
                        return r.ArrayValue(out);
                    }
                    if(const auto expression=r.RegularExpression(pattern,flags)){
                        size_t lastEnd=0;
                        bool matchedEmptyInput=false;
                        for(std::wsregex_iterator iterator(s.begin(),s.end(),*expression),end;
                            iterator!=end&&out.size()<limit;++iterator){
                            const auto& match=*iterator;
                            const size_t matchStart=static_cast<size_t>(match.position());
                            const size_t matchEnd=matchStart+static_cast<size_t>(match.length());
                            if(match.length()==0&&matchStart==lastEnd){
                                matchedEmptyInput=matchedEmptyInput||s.empty();
                                continue;
                            }
                            if(match.length()==0&&matchStart==s.size())break;
                            append(s.substr(lastEnd,matchStart-lastEnd));
                            for(size_t capture=1;capture<match.size()&&out.size()<limit;++capture)
                                append(match[capture].matched?match[capture].str():L"");
                            lastEnd=matchEnd;
                        }
                        if(out.size()<limit&&!(s.empty()&&matchedEmptyInput))append(s.substr(lastEnd));
                        return r.ArrayValue(out);
                    }else{out.push_back(Value::String(s));return r.ArrayValue(out);}
                }
                const auto separator=r.String(separatorValue);
                if(separator.empty()){
                    for(wchar_t character:s){append(std::wstring(1,character));if(out.size()>=limit)break;}
                }else{
                    size_t start=0;
                    for(;;){
                        const auto end=s.find(separator,start);
                        append(s.substr(start,end==std::wstring::npos?std::wstring::npos:end-start));
                        if(end==std::wstring::npos||out.size()>=limit)break;
                        start=end+separator.size();
                    }
                }
                return r.ArrayValue(out);
            });
            if(key==L"match")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto patternValue=a.empty()?Value::Undefined():r.Deref(a[0]);
                std::wstring pattern,flags;
                if(patternValue.type==Value::Type::Object&&patternValue.object&&
                   patternValue.object->kind==ObjectKind::RegExp){
                    pattern=r.String(patternValue.object->props[L"$pattern"]);
                    flags=r.String(patternValue.object->props[L"$flags"]);
                }else pattern=patternValue.type==Value::Type::Undefined?L"":r.String(patternValue);
                if(!r.RegularExpressionMayMatch(pattern,flags,s))return Value::Null();
                if(const auto expression=r.RegularExpression(pattern,flags)){
                    std::vector<Value> matches;
                    if(flags.find(L'g')!=std::wstring::npos){
                        for(std::wsregex_iterator it(s.begin(),s.end(),*expression),end;it!=end;++it)
                            matches.push_back(Value::String((*it)[0].str()));
                        return matches.empty()?Value::Null():r.ArrayValue(matches);
                    }
                    std::wsmatch match;
                    if(!std::regex_search(s,match,*expression))return Value::Null();
                    for(const auto& capture:match)
                        matches.push_back(capture.matched?Value::String(capture.str()):Value::Undefined());
                    auto result=r.ArrayValue(matches);
                    result.object->props[L"index"]=Value::Number(static_cast<double>(match.position()));
                    result.object->props[L"input"]=Value::String(s);
                    return result;
                }else return Value::Null();
            });
            if(key==L"search")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto patternValue=a.empty()?Value::Undefined():r.Deref(a[0]);
                std::wstring pattern,flags;
                if(patternValue.type==Value::Type::Object&&patternValue.object&&
                   patternValue.object->kind==ObjectKind::RegExp){
                    pattern=r.String(patternValue.object->props[L"$pattern"]);
                    flags=r.String(patternValue.object->props[L"$flags"]);
                }else pattern=patternValue.type==Value::Type::Undefined?L"":r.String(patternValue);
                if(!r.RegularExpressionMayMatch(pattern,flags,s))return Value::Number(-1);
                if(const auto expression=r.RegularExpression(pattern,flags)){
                    std::wsmatch match;
                    if(std::regex_search(s,match,*expression))
                        return Value::Number(static_cast<double>(match.position()));
                }
                return Value::Number(-1);
            });
            if(key==L"matchAll")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto patternValue=a.empty()?Value::Undefined():r.Deref(a[0]);std::wstring pattern,flags;
                if(patternValue.type==Value::Type::Object&&patternValue.object&&patternValue.object->kind==ObjectKind::RegExp){pattern=r.String(patternValue.object->props[L"$pattern"]);flags=r.String(patternValue.object->props[L"$flags"]);}else pattern=patternValue.type==Value::Type::Undefined?L"":r.String(patternValue);
                std::vector<Value> results;if(!r.RegularExpressionMayMatch(pattern,flags,s))return r.ArrayValue(results);
                if(const auto expression=r.RegularExpression(pattern,flags))for(std::wsregex_iterator iterator(s.begin(),s.end(),*expression),end;iterator!=end;++iterator){const auto& match=*iterator;std::vector<Value> captures;for(const auto& capture:match)captures.push_back(capture.matched?Value::String(capture.str()):Value::Undefined());auto item=r.ArrayValue(captures);item.object->props[L"index"]=Value::Number(static_cast<double>(match.position()));item.object->props[L"input"]=Value::String(s);results.push_back(item);}
                return r.ArrayValue(results);
            });
            if(key==L"padStart")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){size_t n=a.empty()?0:static_cast<size_t>(r.Number(a[0]));std::wstring fill=a.size()>1?r.String(a[1]):L" ";if(fill.empty())fill=L" ";std::wstring out=s;while(out.size()<n)out=fill+out;if(out.size()>n)out=out.substr(out.size()-n);return Value::String(out);});
            if(key==L"replaceAll")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto search=a.empty()?L"":r.String(a[0]);const auto replacement=a.size()>1?r.String(a[1]):L"undefined";
                if(search.empty()){std::wstring out=replacement;for(wchar_t c:s){out+=c;out+=replacement;}return Value::String(out);}
                auto match=s.find(search);if(match==std::wstring::npos)return Value::String(s);
                std::wstring out;out.reserve(s.size()+replacement.size());size_t position=0;
                do{
                    out.append(s,position,match-position);out+=replacement;
                    position=match+search.size();match=s.find(search,position);
                }while(match!=std::wstring::npos);
                out.append(s,position,std::wstring::npos);return Value::String(std::move(out));
            });
            if(key==L"replace")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::String(s);const auto pattern=r.Deref(a[0]);const auto replacement=a.size()>1?r.Deref(a[1]):Value::Undefined();
                if(pattern.type==Value::Type::Object&&pattern.object&&pattern.object->kind==ObjectKind::RegExp){
                    const auto patternText=r.String(pattern.object->props[L"$pattern"]),flags=r.String(pattern.object->props[L"$flags"]);
                    if(!r.RegularExpressionMayMatch(patternText,flags,s))return Value::String(s);
                    if(const auto expression=r.RegularExpression(patternText,flags)){std::wstring out;size_t cursor=0;for(std::wsregex_iterator it(s.begin(),s.end(),*expression),end;it!=end;++it){const auto& match=*it;const auto matchPosition=static_cast<size_t>(match.position()),matchLength=static_cast<size_t>(match.length());out+=s.substr(cursor,matchPosition-cursor);if(replacement.type==Value::Type::Function||replacement.type==Value::Type::Native){std::vector<Value> arguments;for(size_t i=0;i<match.size();++i)arguments.push_back(Value::String(match[i].str()));arguments.push_back(Value::Number(static_cast<double>(matchPosition)));arguments.push_back(Value::String(s));out+=r.String(r.Call(replacement,Value::Undefined(),arguments));}else{const auto replacementText=r.String(replacement);for(size_t index=0;index<replacementText.size();++index){const wchar_t character=replacementText[index];if(character!=L'$'||index+1>=replacementText.size()){out+=character;continue;}const wchar_t token=replacementText[index+1];if(token==L'$'){out+=L'$';++index;}else if(token==L'&'){out+=match[0].str();++index;}else if(token==L'`'){out+=s.substr(0,matchPosition);++index;}else if(token==L'\''){out+=s.substr(matchPosition+matchLength);++index;}else if(token>=L'1'&&token<=L'9'){size_t capture=static_cast<size_t>(token-L'0'),used=1;if(index+2<replacementText.size()&&replacementText[index+2]>=L'0'&&replacementText[index+2]<=L'9'){const size_t two=capture*10+static_cast<size_t>(replacementText[index+2]-L'0');if(two<match.size()){capture=two;used=2;}}if(capture<match.size()){if(match[capture].matched)out+=match[capture].str();index+=used;}else out+=L'$';}else out+=L'$';} }cursor=matchPosition+matchLength;if(flags.find(L'g')==std::wstring::npos)break;}out+=s.substr(cursor);return Value::String(out);}return Value::String(s);
                }
                const auto needle=r.String(pattern);const auto position=s.find(needle);if(position==std::wstring::npos)return Value::String(s);auto out=s;out.replace(position,needle.size(),r.String(replacement));return Value::String(out);
            });
            if(key==L"at")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto size=static_cast<long long>(s.size());auto index=a.empty()?0ll:static_cast<long long>(r.Number(a[0]));if(index<0)index+=size;return index>=0&&index<size?Value::String(std::wstring(1,s[static_cast<size_t>(index)])):Value::Undefined();});
            if(key==L"localeCompare")return Native([s=base.string](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto other=a.empty()?L"undefined":r.String(a[0]);const int result=CompareStringEx(LOCALE_NAME_USER_DEFAULT,NORM_LINGUISTIC_CASING,s.c_str(),static_cast<int>(s.size()),other.c_str(),static_cast<int>(other.size()),nullptr,nullptr,0);return Value::Number(result==CSTR_LESS_THAN?-1:result==CSTR_GREATER_THAN?1:0);});
        }
        if(base.type==Value::Type::Number){
            if(key==L"toFixed")return Native([n=base.number](RuntimeCore& r,const Value&,const std::vector<Value>& a){int d=a.empty()?0:static_cast<int>(r.Number(a[0]));std::wostringstream out;out<<std::fixed<<std::setprecision(std::max(0,std::min(20,d)))<<n;return Value::String(out.str());});
            if(key==L"toLocaleString")return Native([n=base.number](RuntimeCore&,const Value&,const std::vector<Value>&){
                auto value=NumberString(n);const auto exponent=value.find_first_of(L"eE");if(exponent!=std::wstring::npos)return Value::String(value);
                const auto decimal=value.find(L'.');const size_t end=decimal==std::wstring::npos?value.size():decimal;const size_t begin=!value.empty()&&(value[0]==L'-'||value[0]==L'+')?1:0;
                for(size_t position=end;position>begin+3;position-=3)value.insert(position-3,1,L',');return Value::String(value);
            });
        }
        if(base.type!=Value::Type::Object||!base.object)return Value::Undefined();
        auto object=base.object;
        if(object->kind==ObjectKind::Proxy){
            const auto target=object->props.find(L"$target"),handler=object->props.find(L"$handler");
            if(target==object->props.end())return Value::Undefined();
            if(handler!=object->props.end()){
                const auto trap=GetProperty(handler->second,L"get");
                if(IsCallable(trap))return Call(trap,handler->second,{target->second,Value::String(key),base});
            }
            return GetProperty(target->second,key);
        }
        // Array length is a non-configurable data property backed by the
        // indexed storage.  It must not be shadowed by an ordinary property,
        // especially after code clears a history stack with array.length = 0.
        if(object->kind==ObjectKind::Array&&key==L"length")
            return Value::Number(static_cast<double>(object->items.size()));
        const auto own=object->props.find(key);if(own!=object->props.end())return own->second;
        const auto getter=object->props.find(L"$get:"+key);if(getter!=object->props.end())return Call(getter->second,base,{});
        if(key==L"__proto__"){
            const auto explicitPrototype=object->props.find(L"$prototypeValue");
            if(explicitPrototype!=object->props.end())return explicitPrototype->second;
            return object->prototype?Value::FromObject(object->prototype):Value::Null();
        }
        if(const auto explicitPrototype=object->props.find(L"$prototypeValue");
           explicitPrototype!=object->props.end()){
            const auto prototype=Deref(explicitPrototype->second);
            if(prototype.type==Value::Type::Function&&prototype.function){
                const auto inherited=prototype.function->props.find(key);
                if(inherited!=prototype.function->props.end())return Deref(inherited->second);
                const auto inheritedGetter=prototype.function->props.find(L"$get:"+key);
                if(inheritedGetter!=prototype.function->props.end())return Call(inheritedGetter->second,base,{});
            }else if(prototype.type==Value::Type::Native&&prototype.native){
                const auto inherited=prototype.native->props.find(key);
                if(inherited!=prototype.native->props.end())return Deref(inherited->second);
                const auto inheritedGetter=prototype.native->props.find(L"$get:"+key);
                if(inheritedGetter!=prototype.native->props.end())return Call(inheritedGetter->second,base,{});
            }
        }
        for(auto prototype=object->prototype;prototype;prototype=prototype->prototype){
            const auto inherited=prototype->props.find(key);
            if(inherited!=prototype->props.end())return inherited->second;
            const auto inheritedGetter=prototype->props.find(L"$get:"+key);
            if(inheritedGetter!=prototype->props.end())return Call(inheritedGetter->second,base,{});
            const auto method=prototype->props.find(L"$method:"+key);
            if(method!=prototype->props.end())return method->second;
        }
        if(object->kind==ObjectKind::DateConstructor&&key==L"now")return Native([](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Number(CurrentTimeMilliseconds());});
        if(object->kind==ObjectKind::DateConstructor&&key==L"parse")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            return Value::Number(a.empty()?std::numeric_limits<double>::quiet_NaN():
                ParseDateMilliseconds(r.String(a[0])));
        });
        if(object->kind==ObjectKind::Date){
            const auto milliseconds=Number(object->props[L"$time"]);
            if(key==L"getTime"||key==L"valueOf")return Native([milliseconds](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Number(milliseconds);});
            if(key==L"setTime")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
                const double value=r.Number(arguments.empty()?Value::Undefined():arguments[0]);
                object->props[L"$time"]=Value::Number(value);return Value::Number(value);
            });
            if(key==L"getTimezoneOffset")return Native([milliseconds](RuntimeCore&,const Value&,const std::vector<Value>&){
                SYSTEMTIME utc{},local{};FILETIME utcFile{},localFile{};
                if(!DateSystemTime(milliseconds,utc,false)||!DateSystemTime(milliseconds,local,true)||
                   !SystemTimeToFileTime(&utc,&utcFile)||!SystemTimeToFileTime(&local,&localFile))
                    return Value::Number(std::numeric_limits<double>::quiet_NaN());
                ULARGE_INTEGER utcTicks{},localTicks{};
                utcTicks.LowPart=utcFile.dwLowDateTime;utcTicks.HighPart=utcFile.dwHighDateTime;
                localTicks.LowPart=localFile.dwLowDateTime;localTicks.HighPart=localFile.dwHighDateTime;
                return Value::Number(static_cast<double>(static_cast<long long>(utcTicks.QuadPart)-
                    static_cast<long long>(localTicks.QuadPart))/(60.0*10000000.0));
            });
            if(key==L"getFullYear"||key==L"getMonth"||key==L"getDate"||key==L"getDay"||
               key==L"getHours"||key==L"getMinutes"||key==L"getSeconds"||key==L"getMilliseconds"||
               key==L"getUTCFullYear"||key==L"getUTCMonth"||key==L"getUTCDate"||key==L"getUTCDay"||
               key==L"getUTCHours"||key==L"getUTCMinutes"||key==L"getUTCSeconds"||key==L"getUTCMilliseconds")
                return Native([milliseconds,key](RuntimeCore&,const Value&,const std::vector<Value>&){
                    SYSTEMTIME time{};const bool utc=key.rfind(L"getUTC",0)==0;
                    if(!DateSystemTime(milliseconds,time,!utc))return Value::Number(std::numeric_limits<double>::quiet_NaN());
                    const auto suffix=utc?key.substr(6):key.substr(3);
                    if(suffix==L"FullYear")return Value::Number(time.wYear);
                    if(suffix==L"Month")return Value::Number(time.wMonth?time.wMonth-1:0);
                    if(suffix==L"Date")return Value::Number(time.wDay);
                    if(suffix==L"Day")return Value::Number(time.wDayOfWeek);
                    if(suffix==L"Hours")return Value::Number(time.wHour);
                    if(suffix==L"Minutes")return Value::Number(time.wMinute);
                    if(suffix==L"Seconds")return Value::Number(time.wSecond);
                    return Value::Number(time.wMilliseconds);
                });
            if(key==L"toString"||key==L"toUTCString"||key==L"toGMTString")return Native([milliseconds](RuntimeCore&,const Value&,const std::vector<Value>&){
                SYSTEMTIME time{};if(!DateSystemTime(milliseconds,time,false))return Value::String(L"Invalid Date");
                static constexpr const wchar_t* weekdays[]={L"Sun",L"Mon",L"Tue",L"Wed",L"Thu",L"Fri",L"Sat"};
                static constexpr const wchar_t* months[]={L"Jan",L"Feb",L"Mar",L"Apr",L"May",L"Jun",L"Jul",L"Aug",L"Sep",L"Oct",L"Nov",L"Dec"};
                wchar_t buffer[48]{};swprintf_s(buffer,L"%ls, %02u %ls %04u %02u:%02u:%02u GMT",
                    weekdays[std::min<unsigned>(time.wDayOfWeek,6)],time.wDay,
                    months[std::min<unsigned>(time.wMonth?time.wMonth-1:0,11)],time.wYear,
                    time.wHour,time.wMinute,time.wSecond);
                return Value::String(buffer);
            });
            if(key==L"toISOString"||key==L"toJSON")return Native([milliseconds](RuntimeCore&,const Value&,const std::vector<Value>&){SYSTEMTIME time{};if(!DateSystemTime(milliseconds,time,false))return Value::String(L"Invalid Date");wchar_t buffer[40]{};swprintf_s(buffer,L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute,time.wSecond,time.wMilliseconds);return Value::String(buffer);});
            if(key==L"toLocaleTimeString")return Native([milliseconds](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                SYSTEMTIME time{};
                if(!DateSystemTime(milliseconds,time,true))return Value::String(L"Invalid Date");
                bool force24Hour=false;
                if(a.size()>1){
                    const auto options=r.Deref(a[1]);
                    if(options.type==Value::Type::Object&&options.object){
                        const auto hour12=options.object->props.find(L"hour12");
                        force24Hour=hour12!=options.object->props.end()&&
                            r.Deref(hour12->second).type==Value::Type::Boolean&&
                            !r.Deref(hour12->second).boolean;
                    }
                }
                wchar_t buffer[128]{};
                if(force24Hour){
                    swprintf_s(buffer,L"%02u:%02u:%02u",time.wHour,time.wMinute,time.wSecond);
                    return Value::String(buffer);
                }
                const auto locale=a.empty()?L"":r.String(a[0]);
                if(GetTimeFormatEx(locale.empty()?LOCALE_NAME_USER_DEFAULT:locale.c_str(),0,&time,nullptr,
                                   buffer,static_cast<int>(std::size(buffer)))>0)
                    return Value::String(buffer);
                swprintf_s(buffer,L"%02u:%02u:%02u",time.wHour,time.wMinute,time.wSecond);
                return Value::String(buffer);
            });
        }
        if(object->kind==ObjectKind::Promise){
            if(key==L"then")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.PerformThen(object,a.empty()?Value::Undefined():a[0],a.size()>1?a[1]:Value::Undefined());});
            if(key==L"catch")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.PerformThen(object,Value::Undefined(),a.empty()?Value::Undefined():a[0]);});
            if(key==L"finally")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto callback=a.empty()?Value::Undefined():r.Deref(a[0]);
                if(!r.IsCallable(callback))return r.PerformThen(object,Value::Undefined(),Value::Undefined());
                auto fulfilled=r.Native([callback](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){
                    const auto original=values.empty()?Value::Undefined():values[0];
                    const auto waited=runtime.PromiseResolveValue(runtime.Call(callback,Value::Undefined(),{}));
                    auto restore=runtime.Native([original](RuntimeCore&,const Value&,const std::vector<Value>&){return original;});
                    return runtime.PerformThen(waited.object,restore,Value::Undefined());
                });
                auto rejected=r.Native([callback](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){
                    const auto reason=values.empty()?Value::Undefined():values[0];
                    const auto waited=runtime.PromiseResolveValue(runtime.Call(callback,Value::Undefined(),{}));
                    auto rethrow=runtime.Native([reason](RuntimeCore&,const Value&,const std::vector<Value>&)->Value{throw JavaScriptException{reason};});
                    return runtime.PerformThen(waited.object,rethrow,Value::Undefined());
                });
                return r.PerformThen(object,fulfilled,rejected);
            });
        }
        if(object->kind==ObjectKind::Error&&key==L"toString")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto name=object->props.count(L"name")?r.String(object->props[L"name"]):L"Error";const auto message=object->props.count(L"message")?r.String(object->props[L"message"]):L"";return Value::String(message.empty()?name:name+L": "+message);});
        if(object->kind==ObjectKind::Array){
            if(key==L"toString")return Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>&){return Value::String(r.String(thisValue));});
            if(key==L"push")return Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& args){
                const auto target=r.Deref(thisValue);
                if(target.type!=Value::Type::Object||!target.object)return Value::Number(0);
                size_t length=target.object->kind==ObjectKind::Array?target.object->items.size():
                    static_cast<size_t>(std::max(0.0,r.Number(r.GetProperty(target,L"length"))));
                for(const auto& value:args)r.SetProperty(target,std::to_wstring(length++),value);
                r.SetProperty(target,L"length",Value::Number(static_cast<double>(length)));
                return Value::Number(static_cast<double>(length));
            });
            if(key==L"unshift")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>& args){object->items.insert(object->items.begin(),args.begin(),args.end());return Value::Number(static_cast<double>(object->items.size()));});
            if(key==L"pop")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){if(object->items.empty())return Value::Undefined();auto last=object->items.back();object->items.pop_back();return last;});
            if(key==L"shift")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){if(object->items.empty())return Value::Undefined();auto first=object->items.front();object->items.erase(object->items.begin());return first;});
            if(key==L"indexOf")return Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& args){
                if(args.empty())return Value::Number(-1);
                const auto target=r.Deref(thisValue);
                const auto length=target.type==Value::Type::Object&&target.object&&target.object->kind==ObjectKind::Array?
                    target.object->items.size():static_cast<size_t>(std::max(0.0,r.Number(r.GetProperty(target,L"length"))));
                size_t start=0;if(args.size()>1){const auto offset=static_cast<long long>(r.Number(args[1]));start=offset<0?static_cast<size_t>(std::max(0ll,static_cast<long long>(length)+offset)):std::min(length,static_cast<size_t>(offset));}
                for(size_t index=start;index<length;++index)
                    if(r.EqualValues(r.GetProperty(target,std::to_wstring(index)),args[0]))return Value::Number(static_cast<double>(index));
                return Value::Number(-1);
            });
            if(key==L"concat")return Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& args){
                std::vector<Value> output;
                const auto append=[&](const Value& input){
                    const auto value=r.Deref(input);
                    if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Array)
                        output.insert(output.end(),value.object->items.begin(),value.object->items.end());
                    else output.push_back(value);
                };
                append(thisValue);for(const auto& argument:args)append(argument);
                return r.ArrayValue(output);
            });
            if(key==L"includes")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Bool(false);for(const auto& item:object->items)if(r.EqualValues(item,args[0]))return Value::Bool(true);return Value::Bool(false);});
            if(key==L"find")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Undefined();for(size_t i=0;i<object->items.size();++i)if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[i],Value::Number(static_cast<double>(i)),Value::FromObject(object)})))return object->items[i];return Value::Undefined();});
            if(key==L"findIndex")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Number(-1);for(size_t i=0;i<object->items.size();++i)if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[i],Value::Number(static_cast<double>(i)),Value::FromObject(object)})))return Value::Number(static_cast<double>(i));return Value::Number(-1);});
            if(key==L"findLastIndex")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Number(-1);for(size_t i=object->items.size();i>0;--i){const size_t index=i-1;if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[index],Value::Number(static_cast<double>(index)),Value::FromObject(object)})))return Value::Number(static_cast<double>(index));}return Value::Number(-1);});
            if(key==L"some")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){if(args.empty())return Value::Bool(false);for(size_t i=0;i<object->items.size();++i)if(r.Truth(r.Call(args[0],Value::Undefined(),{object->items[i],Value::Number(static_cast<double>(i)),Value::FromObject(object)})))return Value::Bool(true);return Value::Bool(false);});
            if(key==L"at")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){const auto size=static_cast<long long>(object->items.size());auto index=args.empty()?0ll:static_cast<long long>(r.Number(args[0]));if(index<0)index+=size;return index>=0&&index<size?object->items[static_cast<size_t>(index)]:Value::Undefined();});
            if(key==L"fill")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){const auto size=static_cast<long long>(object->items.size());auto index=[&](size_t argument,long long fallback){if(argument>=args.size())return fallback;auto value=static_cast<long long>(r.Number(args[argument]));return value<0?std::max(0ll,size+value):std::min(size,value);};const auto start=index(1,0),end=index(2,size);const auto value=args.empty()?Value::Undefined():r.Deref(args[0]);for(auto position=start;position<end;++position)object->items[static_cast<size_t>(position)]=value;return Value::FromObject(object);});
            if(key==L"reverse")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){std::reverse(object->items.begin(),object->items.end());return Value::FromObject(object);});
            if(key==L"sort")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){const auto compare=args.empty()?Value::Undefined():r.Deref(args[0]);std::stable_sort(object->items.begin(),object->items.end(),[&](const Value& left,const Value& right){if(r.IsCallable(compare)){const auto result=r.Number(r.Call(compare,Value::Undefined(),{left,right}));return std::isnan(result)?false:result<0;}return r.String(left)<r.String(right);});return Value::FromObject(object);});
            if(key==L"slice")return Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& args){
                const auto target=r.Deref(thisValue);
                const auto size=target.type==Value::Type::Object&&target.object&&target.object->kind==ObjectKind::Array?
                    static_cast<long long>(target.object->items.size()):
                    static_cast<long long>(std::max(0.0,r.Number(r.GetProperty(target,L"length"))));
                auto offset=[&](size_t index,long long fallback){
                    if(index>=args.size())return fallback;
                    const double number=r.Number(args[index]);
                    if(std::isnan(number))return 0ll;
                    const auto integer=static_cast<long long>(number);
                    return integer<0?std::max(0ll,size+integer):std::min(size,integer);
                };
                const auto begin=offset(0,0),end=offset(1,size);
                if(end<=begin)return r.ArrayValue({});
                std::vector<Value> output;output.reserve(static_cast<size_t>(end-begin));
                for(auto index=begin;index<end;++index)
                    output.push_back(r.GetProperty(target,std::to_wstring(index)));
                return r.ArrayValue(output);
            });
            if(key==L"splice")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto size=static_cast<std::ptrdiff_t>(object->items.size());
                auto boundedIndex=[&](const Value& value){const double number=r.Number(value);
                    if(std::isnan(number))return std::ptrdiff_t{0};
                    return static_cast<std::ptrdiff_t>(std::max(-static_cast<double>(size),std::min(static_cast<double>(size),number)));
                };
                std::ptrdiff_t start=args.empty()?0:boundedIndex(args[0]);
                if(start<0)start=std::max(std::ptrdiff_t{0},size+start);else start=std::min(start,size);
                std::ptrdiff_t count=args.size()>1?boundedIndex(args[1]):size-start;
                count=std::max(std::ptrdiff_t{0},std::min(count,size-start));auto first=object->items.begin()+start;
                std::vector<Value> removed(first,first+count);object->items.erase(first,first+count);
                if(args.size()>2)object->items.insert(object->items.begin()+start,args.begin()+2,args.end());
                return r.ArrayValue(removed);
            });
            if(key==L"forEach"||key==L"filter"||key==L"every"||key==L"map")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                if(args.empty())return key==L"every"?Value::Bool(true):(key==L"filter"||key==L"map"?r.ArrayValue({}):Value::Undefined());std::vector<Value> output;if(key==L"filter"||key==L"map")output.reserve(object->items.size());
                std::vector<Value> callbackArgs(3);callbackArgs[2]=Value::FromObject(object);
                for(size_t i=0;i<object->items.size();++i){callbackArgs[0]=object->items[i];callbackArgs[1]=Value::Number(static_cast<double>(i));auto result=r.Call(args[0],Value::Undefined(),callbackArgs);if(key==L"filter"&&r.Truth(result))output.push_back(object->items[i]);if(key==L"map")output.push_back(r.Deref(std::move(result)));if(key==L"every"&&!r.Truth(result))return Value::Bool(false);}
                if(key==L"filter"||key==L"map")return r.ArrayValue(output);if(key==L"every")return Value::Bool(true);return Value::Undefined();});
            if(key==L"flatMap")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                if(args.empty()||!r.IsCallable(args[0]))
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"flatMap callback is not callable")};
                std::vector<Value> output;output.reserve(object->items.size());
                const auto array=Value::FromObject(object);
                for(size_t index=0;index<object->items.size();++index){
                    const auto mapped=r.Deref(r.Call(args[0],Value::Undefined(),
                        {object->items[index],Value::Number(static_cast<double>(index)),array}));
                    if(mapped.type==Value::Type::Object&&mapped.object&&
                       mapped.object->kind==ObjectKind::Array)
                        output.insert(output.end(),mapped.object->items.begin(),mapped.object->items.end());
                    else output.push_back(mapped);
                }
                return r.ArrayValue(output);
            });
            if(key==L"reduce"||key==L"reduceRight")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                if(args.empty()||!r.IsCallable(args[0]))
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"reduce callback is not callable")};
                const bool reverse=key==L"reduceRight";const auto count=object->items.size();
                if(count==0&&args.size()<2)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"Reduce of empty array with no initial value")};
                size_t consumed=0;Value accumulator;
                if(args.size()>1)accumulator=r.Deref(args[1]);
                else{accumulator=object->items[reverse?count-1:0];consumed=1;}
                for(size_t step=consumed;step<count;++step){const size_t index=reverse?count-1-step:step;
                    accumulator=r.Deref(r.Call(args[0],Value::Undefined(),{accumulator,object->items[index],
                        Value::Number(static_cast<double>(index)),Value::FromObject(object)}));}
                return accumulator;
            });
            if(key==L"join")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto separator=args.empty()?L",":r.String(args[0]);
                size_t capacity=separator.size()*(object->items.empty()?0:object->items.size()-1);
                bool stringsOnly=true;
                for(const auto& item:object->items){
                    const auto value=r.Deref(item);
                    if(value.type==Value::Type::String)capacity+=value.string.size();
                    else if(value.type!=Value::Type::Undefined&&value.type!=Value::Type::Null){stringsOnly=false;break;}
                }
                std::wstring out;
                if(stringsOnly)out.reserve(capacity);
                for(size_t i=0;i<object->items.size();++i){
                    if(i)out+=separator;
                    const auto value=r.Deref(object->items[i]);
                    if(value.type==Value::Type::String)out+=value.string;
                    else if(value.type!=Value::Type::Undefined&&value.type!=Value::Type::Null)out+=r.String(value);
                }
                return Value::String(std::move(out));
            });
            size_t index=0;if(TryParseDecimalIndex(key,index)&&index<object->items.size())return object->items[index];
        }
        if(object->kind==ObjectKind::RegExp&&key==L"source")return object->props[L"$pattern"];
        if(object->kind==ObjectKind::RegExp&&
           (key==L"global"||key==L"ignoreCase"||key==L"multiline"||key==L"unicode"||key==L"sticky")){
            const auto flags=String(object->props[L"$flags"]);
            const wchar_t flag=key==L"global"?L'g':key==L"ignoreCase"?L'i':
                               key==L"multiline"?L'm':key==L"unicode"?L'u':L'y';
            return Value::Bool(flags.find(flag)!=std::wstring::npos);
        }
        if(object->kind==ObjectKind::RegExp&&(key==L"test"||key==L"exec"))return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto input=a.empty()?L"":r.String(a[0]),pattern=r.String(object->props[L"$pattern"]),flags=r.String(object->props[L"$flags"]);
            if(!r.RegularExpressionMayMatch(pattern,flags,input))return key==L"test"?Value::Bool(false):Value::Null();
            if(const auto expression=r.RegularExpression(pattern,flags)){std::wsmatch match;const bool found=std::regex_search(input,match,*expression);if(key==L"test")return Value::Bool(found);if(!found)return Value::Null();std::vector<Value> captures;for(const auto& capture:match)captures.push_back(Value::String(capture.str()));return r.ArrayValue(captures);}return key==L"test"?Value::Bool(false):Value::Null();
        });
        if(object->kind==ObjectKind::ObjectConstructor){
            if(key==L"is")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto left=r.Deref(a.empty()?Value::Undefined():a[0]);
                const auto right=r.Deref(a.size()>1?a[1]:Value::Undefined());
                if(left.type==Value::Type::Number&&right.type==Value::Type::Number){
                    if(std::isnan(left.number)&&std::isnan(right.number))return Value::Bool(true);
                    if(left.number==0&&right.number==0)return Value::Bool(std::signbit(left.number)==std::signbit(right.number));
                }
                return Value::Bool(r.EqualValues(left,right));
            });
            if(key==L"hasOwn")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<2)return Value::Bool(false);const auto source=r.Deref(a[0]);const auto name=r.String(a[1]);
                if(source.type==Value::Type::Function&&source.function)return Value::Bool(source.function->props.count(name)!=0||source.function->props.count(L"$get:"+name)!=0);
                if(source.type==Value::Type::Native&&source.native)return Value::Bool(source.native->props.count(name)!=0||source.native->props.count(L"$get:"+name)!=0);
                if(source.type!=Value::Type::Object||!source.object)return Value::Bool(false);
                if(source.object->kind==ObjectKind::Array){size_t index=0;if(name==L"length")return Value::Bool(true);if(TryParseDecimalIndex(name,index))return Value::Bool(index<source.object->items.size());}
                return Value::Bool(source.object->props.count(name)!=0);
            });
            if(key==L"create")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto value=r.ObjectValue(ObjectKind::Plain);
                if(!a.empty()){
                    const auto prototype=r.Deref(a[0]);
                    if(prototype.type==Value::Type::Object)value.object->prototype=prototype.object;
                    else if(prototype.type==Value::Type::Function||prototype.type==Value::Type::Native)
                        value.object->props[L"$prototypeValue"]=prototype;
                    else if(prototype.type!=Value::Type::Null)
                        throw JavaScriptException{r.ErrorValue(L"TypeError",L"Object prototype may only be an Object or null")};
                }
                if(a.size()>1){
                    const auto descriptors=r.Deref(a[1]);
                    if(descriptors.type==Value::Type::Object&&descriptors.object)
                        for(const auto& property:descriptors.object->props){
                            const auto descriptor=r.Deref(property.second);
                            if(descriptor.type!=Value::Type::Object||!descriptor.object)continue;
                            r.ApplyOwnPropertyDescriptor(value,property.first,descriptor);
                        }
                }
                return value;
            });
            if(key==L"assign")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();
                auto target=r.Deref(a[0]);
                for(size_t index=1;index<a.size();++index){
                    const auto source=r.Deref(a[index]);
                    const FastMap<std::wstring,Value>* properties=nullptr;
                    if(source.type==Value::Type::Object&&source.object)properties=&source.object->props;
                    else if(source.type==Value::Type::Function&&source.function)properties=&source.function->props;
                    else if(source.type==Value::Type::Native&&source.native)properties=&source.native->props;
                    if(properties)for(const auto& property:*properties){
                        if(IsInternalObjectProperty(property.first)){
                            constexpr std::wstring_view getterPrefix=L"$get:";
                            if(property.first.compare(0,getterPrefix.size(),getterPrefix)==0)
                                {const auto publicName=property.first.substr(getterPrefix.size());
                                 if(r.OwnPropertyIsEnumerable(source,publicName))
                                     r.SetProperty(target,publicName,r.GetProperty(source,publicName));}
                            continue;
                        }
                        if(!r.OwnPropertyIsEnumerable(source,property.first))continue;
                        r.SetProperty(target,property.first,property.second);
                    }
                }
                return target;
            });
            if(key==L"defineProperty")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<3)return a.empty()?Value::Undefined():r.Deref(a[0]);
                const auto target=r.Deref(a[0]);const auto name=r.String(a[1]);
                const auto descriptor=r.Deref(a[2]);
                r.ApplyOwnPropertyDescriptor(target,name,descriptor);
                return target;
            });
            if(key==L"defineProperties")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<2)return a.empty()?Value::Undefined():r.Deref(a[0]);
                const auto target=r.Deref(a[0]),descriptors=r.Deref(a[1]);
                if(descriptors.type==Value::Type::Object&&descriptors.object)
                    for(const auto& property:descriptors.object->props){
                        const auto descriptor=r.Deref(property.second);
                        if(descriptor.type!=Value::Type::Object||!descriptor.object)continue;
                        r.ApplyOwnPropertyDescriptor(target,property.first,descriptor);
                    }
                return target;
            });
            if(key==L"getOwnPropertyDescriptor")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<2)return Value::Undefined();
                const auto source=r.Deref(a[0]);const auto name=r.String(a[1]);
                const FastMap<std::wstring,Value>* properties=nullptr;
                if(source.type==Value::Type::Object&&source.object)properties=&source.object->props;
                else if(source.type==Value::Type::Function&&source.function)properties=&source.function->props;
                else if(source.type==Value::Type::Native&&source.native)properties=&source.native->props;
                if(!properties)return Value::Undefined();
                auto descriptor=r.ObjectValue(ObjectKind::Plain);
                if(const auto value=properties->find(name);value!=properties->end())
                    descriptor.object->props[L"value"]=r.Deref(value->second);
                else if(const auto getter=properties->find(L"$get:"+name);getter!=properties->end())
                    descriptor.object->props[L"get"]=r.Deref(getter->second);
                else return Value::Undefined();
                descriptor.object->props[L"writable"]=Value::Bool(true);
                const auto enumerable=properties->find(L"$enumerable:"+name);
                descriptor.object->props[L"enumerable"]=Value::Bool(
                    enumerable==properties->end()?true:r.Truth(enumerable->second));
                descriptor.object->props[L"configurable"]=Value::Bool(true);
                return descriptor;
            });
            if(key==L"getPrototypeOf")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                const FastMap<std::wstring,Value>* properties=nullptr;
                if(value.type==Value::Type::Function&&value.function)properties=&value.function->props;
                else if(value.type==Value::Type::Native&&value.native)properties=&value.native->props;
                else if(value.type==Value::Type::Object&&value.object)properties=&value.object->props;
                if(properties){
                    const auto explicitPrototype=properties->find(L"$prototypeValue");
                    if(explicitPrototype!=properties->end())return r.Deref(explicitPrototype->second);
                }
                if(value.type==Value::Type::Object&&value.object&&value.object->prototype)
                    return Value::FromObject(value.object->prototype);
                if(value.type==Value::Type::Function||value.type==Value::Type::Native){
                    if(r.global){
                        const auto function=r.global->values.find(L"Function");
                        if(function!=r.global->values.end())
                            return r.GetProperty(function->second,L"prototype");
                    }
                }
                return Value::Null();
            });
            if(key==L"setPrototypeOf")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();auto value=r.Deref(a[0]);
                if(a.size()>1){
                    const auto prototype=r.Deref(a[1]);
                    const bool valid=prototype.type==Value::Type::Null||prototype.type==Value::Type::Object||
                                     prototype.type==Value::Type::Function||prototype.type==Value::Type::Native;
                    if(!valid)throw JavaScriptException{r.ErrorValue(L"TypeError",L"Object prototype may only be an Object or null")};
                    if(value.type==Value::Type::Function&&value.function)
                        value.function->props[L"$prototypeValue"]=prototype;
                    else if(value.type==Value::Type::Native&&value.native)
                        value.native->props[L"$prototypeValue"]=prototype;
                    else if(value.type==Value::Type::Object&&value.object){
                        if(prototype.type==Value::Type::Object){
                            value.object->prototype=prototype.object;
                            value.object->props.erase(L"$prototypeValue");
                        }else{
                            value.object->prototype.reset();
                            value.object->props[L"$prototypeValue"]=prototype;
                        }
                    }
                }
                return value;
            });
            if(key==L"freeze"||key==L"seal"||key==L"preventExtensions")
                return Native([](RuntimeCore&,const Value&,const std::vector<Value>& a){
                    return a.empty()?Value::Undefined():a[0];
                });
            if(key==L"isFrozen"||key==L"isSealed"||key==L"isExtensible")
                return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                    if(a.empty())return Value::Bool(key!=L"isExtensible");
                    const auto value=r.Deref(a[0]);
                    const bool objectLike=value.type==Value::Type::Object||
                        value.type==Value::Type::Function||value.type==Value::Type::Native;
                    return Value::Bool(key==L"isExtensible"?objectLike:!objectLike);
                });
            if(key==L"fromEntries")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto result=r.ObjectValue(ObjectKind::Plain);
                if(!a.empty()){auto entries=r.Deref(a[0]);
                    if(entries.type==Value::Type::Object&&entries.object&&entries.object->kind==ObjectKind::Array)
                        for(const auto& item:entries.object->items){auto pair=r.Deref(item);
                            if(pair.type==Value::Type::Object&&pair.object&&pair.object->kind==ObjectKind::Array&&pair.object->items.size()>1)
                                result.object->props[r.String(pair.object->items[0])]=r.Deref(pair.object->items[1]);
                        }
                }
                return result;
            });
            if(key==L"entries"||key==L"values"||key==L"keys")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::vector<Value> result;if(a.empty())return r.ArrayValue(result);
                auto source=r.Deref(a[0]);const FastMap<std::wstring,Value>* properties=nullptr;
                if(source.type==Value::Type::Object&&source.object){
                    if(source.object->kind==ObjectKind::Array)
                        for(size_t index=0;index<source.object->items.size();++index){
                            auto name=Value::String(std::to_wstring(index)),value=r.Deref(source.object->items[index]);
                            result.push_back(key==L"keys"?name:key==L"values"?value:r.ArrayValue({name,value}));
                        }
                    properties=&source.object->props;
                }else if(source.type==Value::Type::Function&&source.function)properties=&source.function->props;
                else if(source.type==Value::Type::Native&&source.native)properties=&source.native->props;
                if(!properties)return r.ArrayValue(result);
                for(const auto& property:*properties){
                    if(IsInternalObjectProperty(property.first)){
                        constexpr std::wstring_view getterPrefix=L"$get:";
                        if(property.first.compare(0,getterPrefix.size(),getterPrefix)!=0)continue;
                        const auto publicName=property.first.substr(getterPrefix.size());
                        if(!r.OwnPropertyIsEnumerable(source,publicName))continue;
                        auto name=Value::String(publicName),value=r.Deref(r.GetProperty(source,publicName));
                        result.push_back(key==L"keys"?name:key==L"values"?value:r.ArrayValue({name,value}));
                        continue;
                    }
                    if(!r.OwnPropertyIsEnumerable(source,property.first))continue;
                    auto name=Value::String(property.first),value=r.Deref(property.second);
                    result.push_back(key==L"keys"?name:key==L"values"?value:r.ArrayValue({name,value}));
                }
                return r.ArrayValue(result);
            });
        }
        if(object->kind==ObjectKind::Headers){
            if(key==L"set"||key==L"append")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
                if(arguments.empty())return Value::Undefined();
                const auto name=ToLower(Trim(r.String(arguments[0])));
                const auto value=arguments.size()>1?r.String(arguments[1]):L"undefined";
                auto found=std::find_if(object->entries.begin(),object->entries.end(),[&](const auto& entry){return r.String(entry.first)==name;});
                if(found==object->entries.end())object->entries.push_back({Value::String(name),Value::String(value)});
                else if(key==L"append")found->second=Value::String(r.String(found->second)+L", "+value);
                else found->second=Value::String(value);
                return Value::Undefined();
            });
            if(key==L"get"||key==L"has"||key==L"delete")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
                const auto name=arguments.empty()?L"":ToLower(Trim(r.String(arguments[0])));
                const auto found=std::find_if(object->entries.begin(),object->entries.end(),[&](const auto& entry){return r.String(entry.first)==name;});
                if(key==L"has")return Value::Bool(found!=object->entries.end());
                if(key==L"delete"){if(found!=object->entries.end())object->entries.erase(found);return Value::Undefined();}
                return found==object->entries.end()?Value::Null():Value::String(r.String(found->second));
            });
            if(key==L"keys"||key==L"values"||key==L"entries")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>&){
                std::vector<Value> result;result.reserve(object->entries.size());
                for(const auto& entry:object->entries)
                    result.push_back(key==L"keys"?entry.first:key==L"values"?entry.second:r.ArrayValue({entry.first,entry.second}));
                return r.ArrayValue(result);
            });
            if(key==L"forEach")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
                if(arguments.empty()||!r.IsCallable(arguments[0]))
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"Headers forEach callback is not callable")};
                const auto thisValue=arguments.size()>1?arguments[1]:Value::Undefined();
                const auto headers=Value::FromObject(object);
                const auto snapshot=object->entries;
                for(const auto& entry:snapshot)r.Call(arguments[0],thisValue,{entry.second,entry.first,headers});
                return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::Map||object->kind==ObjectKind::Set){
            const bool map=object->kind==ObjectKind::Map;
            if(key==L"size")return Value::Number(static_cast<double>(object->entries.size()));
            if(key==L"clear")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->entries.clear();return Value::Undefined();});
            if(key==L"values")return Native([object,map](RuntimeCore& r,const Value&,const std::vector<Value>&){std::vector<Value> values;values.reserve(object->entries.size());for(const auto& entry:object->entries)values.push_back(map?entry.second:entry.first);return r.ArrayValue(values);});
            if(key==L"keys")return Native([object,map](RuntimeCore& r,const Value&,const std::vector<Value>&){std::vector<Value> values;values.reserve(object->entries.size());for(const auto& entry:object->entries)values.push_back(entry.first);return r.ArrayValue(values);});
            if(key==L"entries")return Native([object,map](RuntimeCore& r,const Value&,const std::vector<Value>&){std::vector<Value> values;values.reserve(object->entries.size());for(const auto& entry:object->entries)values.push_back(r.ArrayValue({entry.first,map?entry.second:entry.first}));return r.ArrayValue(values);});
            if(key==L"forEach")return Native([object,map](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
                if(arguments.empty()||!r.IsCallable(arguments[0]))
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"collection forEach callback is not callable")};
                const auto thisValue=arguments.size()>1?arguments[1]:Value::Undefined();
                const auto collection=Value::FromObject(object);
                const auto snapshot=object->entries;
                for(const auto& entry:snapshot){
                    const auto value=map?entry.second:entry.first;
                    r.Call(arguments[0],thisValue,{value,entry.first,collection});
                }
                return Value::Undefined();
            });
            if(key==L"has"||key==L"get"||key==L"set"||key==L"add"||key==L"delete")return Native([object,key,map](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto needle=args.empty()?Value::Undefined():r.Deref(args[0]);
                auto found=std::find_if(object->entries.begin(),object->entries.end(),[&](const auto& entry){return r.EqualValues(entry.first,needle);});
                if(key==L"has")return Value::Bool(found!=object->entries.end());
                if(key==L"get")return found==object->entries.end()?Value::Undefined():found->second;
                if(key==L"delete"){if(found==object->entries.end())return Value::Bool(false);object->entries.erase(found);return Value::Bool(true);}
                const auto value=map?(args.size()>1?r.Deref(args[1]):Value::Undefined()):needle;
                if(found==object->entries.end())object->entries.push_back({needle,value});else found->second=value;
                return Value::FromObject(object);
            });
        }
        if(object->kind==ObjectKind::CanvasGradient){
            if(key==L"addColorStop")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!object->canvasGradient||a.size()<2)return Value::Undefined();
                const double rawOffset=r.Number(a[0]);
                if(!std::isfinite(rawOffset)||rawOffset<0||rawOffset>1)return Value::Undefined();
                object->canvasGradient->stops.push_back(
                    {static_cast<float>(rawOffset),r.String(a[1])});
                std::stable_sort(object->canvasGradient->stops.begin(),
                    object->canvasGradient->stops.end(),
                    [](const CanvasGradientStop& left,const CanvasGradientStop& right){
                        return left.offset<right.offset;
                    });
                return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::CanvasContext2D){
            const auto surface=EnsureCanvas(object->node);if(!surface)return Value::Undefined();
            if(key==L"canvas")return NodeValue(object->node);
            if(key==L"fillStyle"||key==L"strokeStyle"){
                const auto& paint=key==L"fillStyle"?surface->state.fillStyle:surface->state.strokeStyle;
                if(!paint.gradient)return Value::String(paint.color);
                auto gradient=CreateObject(ObjectKind::CanvasGradient);
                gradient->canvasGradient=paint.gradient;return Value::FromObject(gradient);
            }
            if(key==L"lineWidth")return Value::Number(surface->state.lineWidth);
            if(key==L"font")return Value::String(surface->state.font);
            if(key==L"textAlign")return Value::String(surface->state.textAlign);
            if(key==L"textBaseline")return Value::String(surface->state.textBaseline);
            if(key==L"save")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->stateStack.push_back(value->state);
                return Value::Undefined();
            });
            if(key==L"restore")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node);value&&!value->stateStack.empty()){
                    value->state=value->stateStack.back();value->stateStack.pop_back();
                }
                return Value::Undefined();
            });
            if(key==L"setTransform"||key==L"transform")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<6)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasTransform transform{static_cast<float>(r.Number(a[0])),static_cast<float>(r.Number(a[1])),
                    static_cast<float>(r.Number(a[2])),static_cast<float>(r.Number(a[3])),
                    static_cast<float>(r.Number(a[4])),static_cast<float>(r.Number(a[5]))};
                if(!std::isfinite(transform.a)||!std::isfinite(transform.b)||!std::isfinite(transform.c)||
                   !std::isfinite(transform.d)||!std::isfinite(transform.e)||!std::isfinite(transform.f))return Value::Undefined();
                if(key==L"setTransform")value->state.transform=transform;
                else value->state.transform=MultiplyCanvasTransforms(transform,value->state.transform);
                return Value::Undefined();
            });
            if(key==L"resetTransform")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->state.transform={};return Value::Undefined();
            });
            if(key==L"translate"||key==L"scale"||key==L"rotate")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();CanvasTransform transform;
                if(key==L"translate"){
                    transform.e=static_cast<float>(a.empty()?0:r.Number(a[0]));
                    transform.f=static_cast<float>(a.size()>1?r.Number(a[1]):0);
                }else if(key==L"scale"){
                    transform.a=static_cast<float>(a.empty()?1:r.Number(a[0]));
                    transform.d=static_cast<float>(a.size()>1?r.Number(a[1]):transform.a);
                }else{
                    const float angle=static_cast<float>(a.empty()?0:r.Number(a[0]));
                    transform.a=std::cos(angle);transform.b=std::sin(angle);
                    transform.c=-std::sin(angle);transform.d=std::cos(angle);
                }
                value->state.transform=MultiplyCanvasTransforms(transform,value->state.transform);
                return Value::Undefined();
            });
            if(key==L"beginPath")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->currentPath.clear();return Value::Undefined();
            });
            if(key==L"closePath")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(const auto value=r.EnsureCanvas(object->node))value->currentPath.push_back({CanvasPathVerb::Close});
                return Value::Undefined();
            });
            if(key==L"moveTo"||key==L"lineTo")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<2)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                const float x=static_cast<float>(r.Number(a[0])),y=static_cast<float>(r.Number(a[1]));
                if(!std::isfinite(x)||!std::isfinite(y))return Value::Undefined();CanvasPathSegment segment;
                segment.verb=key==L"moveTo"?CanvasPathVerb::MoveTo:CanvasPathVerb::LineTo;
                segment.point=TransformCanvasPoint(value->state.transform,x,y);value->currentPath.push_back(segment);
                return Value::Undefined();
            });
            if(key==L"arc")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<5)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasPathSegment segment;segment.verb=CanvasPathVerb::Arc;
                segment.centerX=static_cast<float>(r.Number(a[0]));segment.centerY=static_cast<float>(r.Number(a[1]));
                segment.radius=static_cast<float>(r.Number(a[2]));segment.startAngle=static_cast<float>(r.Number(a[3]));
                segment.endAngle=static_cast<float>(r.Number(a[4]));segment.counterClockwise=a.size()>5&&r.Truth(a[5]);
                segment.transform=value->state.transform;
                if(segment.radius<0||!std::isfinite(segment.centerX)||!std::isfinite(segment.centerY)||
                   !std::isfinite(segment.radius)||!std::isfinite(segment.startAngle)||!std::isfinite(segment.endAngle))
                    return Value::Undefined();
                value->currentPath.push_back(segment);return Value::Undefined();
            });
            if(key==L"fill"||key==L"stroke")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto value=r.EnsureCanvas(object->node);if(!value||value->currentPath.empty())return Value::Undefined();
                CanvasDrawCommand command;command.kind=key==L"fill"?CanvasCommandKind::FillPath:CanvasCommandKind::StrokePath;
                command.state=SnapshotCanvasState(value->state);command.path=value->currentPath;
                value->commands.push_back(std::move(command));r.Mutated(object->node,JavaScriptRuntime::MutationKind::Paint);
                return Value::Undefined();
            });
            if(key==L"fillRect"||key==L"clearRect")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<4)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasDrawCommand command;command.kind=key==L"fillRect"?CanvasCommandKind::FillRect:CanvasCommandKind::ClearRect;
                command.state=SnapshotCanvasState(value->state);command.x=static_cast<float>(r.Number(a[0]));
                command.y=static_cast<float>(r.Number(a[1]));command.width=static_cast<float>(r.Number(a[2]));
                command.height=static_cast<float>(r.Number(a[3]));
                const auto& t=value->state.transform;
                const bool identity=t.a==1&&t.b==0&&t.c==0&&t.d==1&&t.e==0&&t.f==0;
                if(command.kind==CanvasCommandKind::ClearRect&&identity&&command.x<=0&&command.y<=0&&
                   command.x+command.width>=value->width&&command.y+command.height>=value->height)
                    value->commands.clear();
                else value->commands.push_back(std::move(command));
                r.Mutated(object->node,JavaScriptRuntime::MutationKind::Paint);return Value::Undefined();
            });
            if(key==L"createLinearGradient")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<4)return Value::Null();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Null();
                const auto first=TransformCanvasPoint(value->state.transform,static_cast<float>(r.Number(a[0])),static_cast<float>(r.Number(a[1])));
                const auto second=TransformCanvasPoint(value->state.transform,static_cast<float>(r.Number(a[2])),static_cast<float>(r.Number(a[3])));
                auto gradient=r.CreateObject(ObjectKind::CanvasGradient);
                gradient->canvasGradient=std::make_shared<CanvasGradient>();gradient->canvasGradient->x0=first.x;
                gradient->canvasGradient->y0=first.y;gradient->canvasGradient->x1=second.x;gradient->canvasGradient->y1=second.y;
                return Value::FromObject(gradient);
            });
            if(key==L"fillText")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<3)return Value::Undefined();const auto value=r.EnsureCanvas(object->node);if(!value)return Value::Undefined();
                CanvasDrawCommand command;command.kind=CanvasCommandKind::FillText;command.state=SnapshotCanvasState(value->state);
                command.text=r.String(a[0]);command.x=static_cast<float>(r.Number(a[1]));command.y=static_cast<float>(r.Number(a[2]));
                value->commands.push_back(std::move(command));r.Mutated(object->node,JavaScriptRuntime::MutationKind::Paint);
                return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::Document){
            if(key==L"nodeType")return Value::Number(9);
            if(key==L"compatMode")return Value::String(L"CSS1Compat");
            if(key==L"readyState")return Value::String(documentReadyState);
            if(key==L"URL"||key==L"documentURI"||key==L"baseURI")return Value::String(location);
            if(key==L"referrer")return Value::String(L"");
            if(key==L"defaultView"||key==L"parentWindow")return global->values[L"window"];
            if(key==L"location")return global->values[L"location"];
            if(key==L"documentElement")return NodeValue(document.QuerySelector(L"html"));
            if(key==L"head")return NodeValue(document.QuerySelector(L"head"));
            if(key==L"body")return NodeValue(document.Body());
            if(key==L"hidden")return Value::Bool(false);
            if(key==L"visibilityState")return Value::String(L"visible");
            if(key==L"hasFocus")return Native([](RuntimeCore& r,const Value&,
                                                  const std::vector<Value>&){
                return Value::Bool(r.documentFocusProvider?r.documentFocusProvider():true);
            });
            if(key==L"currentScript")return NodeValue(currentScript);
            if(key==L"scripts"){
                std::vector<Value> out;
                for(auto& node:document.QuerySelectorAll(L"script"))out.push_back(NodeValue(node));
                return ArrayValue(out);
            }
            if(key==L"activeElement"){
                std::shared_ptr<Node> active;
                std::function<void(const std::shared_ptr<Node>&)> find=[&](const std::shared_ptr<Node>& node){
                    if(!node||active)return;if(node->focused){active=node;return;}
                    for(const auto& child:node->children)find(child);
                };
                find(document.Root());return NodeValue(active);
            }
            if(key==L"open")return Native([object](RuntimeCore& r,const Value&,
                                                   const std::vector<Value>&){
                r.documentWriteBuffer.clear();r.documentWriteOpen=true;
                return Value::FromObject(object);
            });
            if(key==L"close")return Native([](RuntimeCore& r,const Value&,
                                                const std::vector<Value>&){
                if(!r.documentWriteOpen)return Value::Undefined();
                r.documentWriteOpen=false;
                auto markup=std::move(r.documentWriteBuffer);
                r.documentWriteBuffer.clear();
                // document.open()/write()/close() replaces the active
                // document while retaining its Window/global object.  The
                // owning View reparses markup and installs its styles and
                // subresources without resetting this runtime.
                if(r.documentWriteSink)r.documentWriteSink(markup);
                return Value::Undefined();
            });
            if(key==L"write"||key==L"writeln")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::wstring markup;
                for(const auto& value:a)markup+=r.String(value);
                if(key==L"writeln")markup+=L'\n';
                if(r.documentWriteOpen){r.documentWriteBuffer+=markup;return Value::Undefined();}
                auto destination=r.document.Body();
                if(!destination)destination=r.document.QuerySelector(L"html");
                if(!destination)return Value::Undefined();
                auto fragment=r.document.CreateElement(L"div");
                r.document.SetInnerHtml(fragment,markup,false);
                auto children=std::move(fragment->children);fragment->children.clear();
                bool indexOk=true;
                for(auto& child:children){
                    child->parent=destination;destination->children.push_back(child);
                    indexOk=r.document.IndexSubtree(child)&&indexOk;
                    r.ExecuteConnectedScripts(child);
                }
                r.Mutated(destination,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                return Value::Undefined();
            });
            if(key==L"elementFromPoint"||key==L"elementsFromPoint")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const double x=a.empty()?0:r.Number(a[0]),y=a.size()>1?r.Number(a[1]):0;
                std::vector<std::shared_ptr<Node>> matches;
                const bool inViewport=std::isfinite(x)&&std::isfinite(y)&&x>=0&&y>=0&&
                    x<r.viewportWidth&&y<r.viewportHeight;
                if(inViewport&&r.geometryProvider){
                    std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& node){
                        if(!node)return;
                        bool hidden=node->attributes.count(L"hidden")!=0;
                        if(r.stylePropertyProvider){
                            hidden=hidden||ToLower(r.stylePropertyProvider(node,L"display"))==L"none";
                            const auto visibility=ToLower(r.stylePropertyProvider(node,L"visibility"));
                            hidden=hidden||visibility==L"hidden"||visibility==L"collapse";
                        }
                        if(hidden)return;
                        if(node->type==NodeType::Element){
                            const auto geometry=r.geometryProvider(node);
                            if(geometry.width>0&&geometry.height>0&&x>=geometry.x&&y>=geometry.y&&
                               x<geometry.x+geometry.width&&y<geometry.y+geometry.height)
                                matches.push_back(node);
                        }
                        for(const auto& child:node->children)collect(child);
                    };
                    collect(r.document.Root());
                }else if(inViewport){if(const auto body=r.document.Body())matches.push_back(body);}
                if(key==L"elementFromPoint")return matches.empty()?Value::Null():r.NodeValue(matches.back());
                std::vector<Value> values;values.reserve(matches.size());
                for(auto iterator=matches.rbegin();iterator!=matches.rend();++iterator)values.push_back(r.NodeValue(*iterator));
                return r.ArrayValue(values);
            });
            if(key==L"getElementById")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.EnsureIndex();return r.NodeValue(r.document.GetElementById(a.empty()?L"":r.String(a[0])));});
            if(key==L"getElementsByName")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){std::vector<Value> out;for(auto& n:r.document.GetElementsByName(a.empty()?L"":r.String(a[0])))out.push_back(r.NodeValue(n));return r.ArrayValue(out);});
            if(key==L"getElementsByTagName")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto tag=ToLower(a.empty()?L"*":r.String(a[0]));std::vector<Value> out;
                std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& current){
                    if(!current)return;if(current->type==NodeType::Element&&(tag==L"*"||current->tag==tag))out.push_back(r.NodeValue(current));
                    for(const auto& child:current->children)collect(child);
                };collect(r.document.Root());return r.ArrayValue(out);
            });
            if(key==L"getElementsByClassName")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::vector<std::wstring> classes;std::wistringstream input(a.empty()?L"":r.String(a[0]));std::wstring token;
                while(input>>token)classes.push_back(token);std::vector<Value> out;
                std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& current){
                    if(!current)return;if(current->type==NodeType::Element&&!classes.empty()&&
                        std::all_of(classes.begin(),classes.end(),[&](const auto& name){return current->HasClass(name);}))out.push_back(r.NodeValue(current));
                    for(const auto& child:current->children)collect(child);
                };collect(r.document.Root());return r.ArrayValue(out);
            });
            if(key==L"querySelector"||key==L"querySelectorAll")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.EnsureIndex();auto selector=a.empty()?L"":r.String(a[0]);if(key==L"querySelector")return r.NodeValue(r.document.QuerySelector(selector));std::vector<Value> out;for(auto& n:r.document.QuerySelectorAll(selector))out.push_back(r.NodeValue(n));return r.ArrayValue(out);});
            if(key==L"createElement")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.NodeValue(r.document.CreateElement(a.empty()?L"div":r.String(a[0])));});
            if(key==L"createAttribute")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto attribute=r.CreateObject(ObjectKind::Attr);
                attribute->props[L"$name"]=Value::String(ToLower(a.empty()?L"":r.String(a[0])));
                attribute->props[L"$value"]=Value::String(L"");
                return Value::FromObject(attribute);
            });
            if(key==L"createTextNode")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto node=std::make_shared<Node>();node->type=NodeType::Text;node->tag=L"#text";node->text=a.empty()?L"":r.String(a[0]);node->ownerDocument=&r.document;return r.NodeValue(node);});
            if(key==L"createComment")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto node=std::make_shared<Node>();node->type=NodeType::Comment;node->tag=L"#comment";node->text=a.empty()?L"":r.String(a[0]);node->ownerDocument=&r.document;return r.NodeValue(node);});
            if(key==L"createDocumentFragment")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){auto node=std::make_shared<Node>();node->type=NodeType::Element;node->tag=L"#document-fragment";node->ownerDocument=&r.document;return r.NodeValue(node);});
            if(key==L"createEvent")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
                auto event=r.CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(L"");
                event->props[L"bubbles"]=Value::Bool(false);event->props[L"cancelable"]=Value::Bool(false);
                event->props[L"defaultPrevented"]=Value::Bool(false);event->props[L"isTrusted"]=Value::Bool(false);
                return Value::FromObject(event);
            });
            if(key==L"createTreeWalker")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();const auto root=r.Deref(a[0]);
                if(root.type!=Value::Type::Object||!root.object||root.object->kind!=ObjectKind::Node)return Value::Null();
                auto walker=r.CreateObject(ObjectKind::TreeWalker);walker->node=root.object->node;
                walker->props[L"$current"]=r.NodeValue(root.object->node);
                walker->props[L"$whatToShow"]=Value::Number(a.size()>1?r.Number(a[1]):0xffffffffu);
                return Value::FromObject(walker);
            });
            if(key==L"createRange")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto root=r.document.Root();return r.RangeValue(root,0,root,0);
            });
            if(key==L"execCommand")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto command=a.empty()?L"":r.String(a[0]);
                const auto value=a.size()>2?r.String(a[2]):L"";
                return Value::Bool(r.editingCommands.Execute(command,value));
            });
            if(key==L"queryCommandState")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                return Value::Bool(!a.empty()&&r.editingCommands.QueryState(r.String(a[0])));
            });
            if(key==L"queryCommandSupported")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                return Value::Bool(!a.empty()&&
                    EditingCommandExecutor::IsSupported(r.String(a[0])));
            });
            if(key==L"addEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.documentListeners,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.RemoveEventListener(r.documentListeners,a);return Value::Undefined();});
            if(key==L"dispatchEvent")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Event)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"dispatchEvent requires an Event")};
                JavaScriptRuntime::EventInit init{};
                init.bubbles=r.Truth(r.GetProperty(value,L"bubbles"));
                init.cancelable=r.Truth(r.GetProperty(value,L"cancelable"));
                init.data=r.String(r.GetProperty(value,L"data"));
                init.inputType=r.String(r.GetProperty(value,L"inputType"));
                return Value::Bool(!r.Dispatch({},r.String(r.GetProperty(value,L"type")),nullptr,init));
            });
            EnsureIndex();
            if(const auto named=document.GetElementById(key))return NodeValue(named);
            const auto named=document.GetElementsByName(key);if(!named.empty())return NodeValue(named.front());
        }
        if(object->kind==ObjectKind::Range){
            if(key==L"startContainer")return NodeValue(object->rangeStart);
            if(key==L"endContainer")return NodeValue(object->rangeEnd);
            if(key==L"startOffset")return Value::Number(static_cast<double>(object->rangeStartOffset));
            if(key==L"endOffset")return Value::Number(static_cast<double>(object->rangeEndOffset));
            if(key==L"collapsed")return Value::Bool(object->rangeStart==object->rangeEnd&&object->rangeStartOffset==object->rangeEndOffset);
            if(key==L"commonAncestorContainer")return NodeValue(CommonAncestor(object->rangeStart,object->rangeEnd));
            if(key==L"cloneRange")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                return r.RangeValue(object->rangeStart,object->rangeStartOffset,object->rangeEnd,object->rangeEndOffset);
            });
            if(key==L"selectNodeContents")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node)return Value::Undefined();
                object->rangeStart=object->rangeEnd=value.object->node;object->rangeStartOffset=0;
                object->rangeEndOffset=value.object->node->type==NodeType::Text?value.object->node->text.size():value.object->node->children.size();
                return Value::Undefined();
            });
            if(key==L"selectNode")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())throw JavaScriptException{r.ErrorValue(L"TypeError",L"Range.selectNode requires a Node")};
                const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"Range.selectNode requires a Node")};
                const auto parent=value.object->node->parent.lock();
                if(!parent)throw JavaScriptException{r.ErrorValue(L"InvalidNodeTypeError",L"Node has no parent")};
                const auto found=std::find(parent->children.begin(),parent->children.end(),value.object->node);
                if(found==parent->children.end())throw JavaScriptException{r.ErrorValue(L"InvalidStateError",L"Node is not a child of its parent")};
                const auto offset=static_cast<size_t>(found-parent->children.begin());
                object->rangeStart=object->rangeEnd=parent;
                object->rangeStartOffset=offset;object->rangeEndOffset=offset+1;
                return Value::Undefined();
            });
            if(key==L"createContextualFragment")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto fragment=std::make_shared<Node>();fragment->type=NodeType::Element;
                fragment->tag=L"#document-fragment";fragment->ownerDocument=&r.document;
                const auto markup=a.empty()?L"":r.String(a[0]);
                fragment->children=r.document.ParseFragment(markup);
                for(auto& child:fragment->children)child->parent=fragment;
                return r.NodeValue(fragment);
            });
            if(key==L"deleteContents")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){r.DeleteRangeContents(object);return Value::Undefined();});
            if(key==L"insertNode")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)r.InsertRangeNode(object,value.object->node);
                return Value::Undefined();
            });
            if(key==L"setStart"||key==L"setEnd")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()<2)return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node)return Value::Undefined();
                const auto offset=static_cast<size_t>(std::max(0.0,r.Number(a[1])));
                if(key==L"setStart"){object->rangeStart=value.object->node;object->rangeStartOffset=offset;}
                else{object->rangeEnd=value.object->node;object->rangeEndOffset=offset;}
                return Value::Undefined();
            });
            if(key==L"setStartAfter"||key==L"setEndAfter")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Undefined();
                const auto parent=value.object->node->parent.lock();if(!parent)return Value::Undefined();
                const auto found=std::find(parent->children.begin(),parent->children.end(),value.object->node);
                const size_t offset=found==parent->children.end()?parent->children.size():static_cast<size_t>(found-parent->children.begin())+1;
                if(key==L"setStartAfter"){object->rangeStart=parent;object->rangeStartOffset=offset;}
                else{object->rangeEnd=parent;object->rangeEndOffset=offset;}
                return Value::Undefined();
            });
            if(key==L"collapse")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty()||r.Truth(a[0])){object->rangeEnd=object->rangeStart;object->rangeEndOffset=object->rangeStartOffset;}
                else{object->rangeStart=object->rangeEnd;object->rangeStartOffset=object->rangeEndOffset;}
                return Value::Undefined();
            });
            if(key==L"toString")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(RangeText(object));});
        }
        if(object->kind==ObjectKind::TreeWalker){
            if(key==L"root")return NodeValue(object->node);
            if(key==L"currentNode")return object->props[L"$current"];
            if(key==L"nextNode")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                auto currentValue=r.Deref(object->props[L"$current"]);auto current=currentValue.type==Value::Type::Object&&currentValue.object?currentValue.object->node:object->node;
                const auto mask=static_cast<unsigned>(r.Number(object->props[L"$whatToShow"]));
                for(auto next=NextNodeWithin(current,object->node);next;next=NextNodeWithin(next,object->node)){
                    const unsigned flag=next->type==NodeType::Element?1u:next->type==NodeType::Text?4u:0x100u;
                    if(mask==0xffffffffu||(mask&flag)){object->props[L"$current"]=r.NodeValue(next);return r.NodeValue(next);}
                }
                return Value::Null();
            });
        }
        if(object->kind==ObjectKind::Selection){
            const auto stored=object->props.find(L"$range");
            const auto range=stored!=object->props.end()&&stored->second.type==Value::Type::Object?stored->second.object:std::shared_ptr<Object>{};
            if(key==L"rangeCount")return Value::Number(range?1:0);
            if(key==L"anchorNode")return NodeValue(range?range->rangeStart:std::shared_ptr<Node>{});
            if(key==L"focusNode")return NodeValue(range?range->rangeEnd:std::shared_ptr<Node>{});
            if(key==L"anchorOffset")return Value::Number(static_cast<double>(range?range->rangeStartOffset:0));
            if(key==L"focusOffset")return Value::Number(static_cast<double>(range?range->rangeEndOffset:0));
            if(key==L"isCollapsed")return Value::Bool(!range||(range->rangeStart==range->rangeEnd&&range->rangeStartOffset==range->rangeEndOffset));
            if(key==L"getRangeAt")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto found=object->props.find(L"$range");
                if(a.empty()||r.Number(a[0])!=0||found==object->props.end())return Value::Undefined();return found->second;
            });
            if(key==L"removeAllRanges")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->props.erase(L"$range");return Value::Undefined();});
            if(key==L"addRange")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Range)return Value::Undefined();
                object->props[L"$range"]=value;
                if(r.domSelectionSetter){JavaScriptRuntime::DomSelection selection;
                    selection.anchorNode=value.object->rangeStart;selection.anchorOffset=value.object->rangeStartOffset;
                    selection.focusNode=value.object->rangeEnd;selection.focusOffset=value.object->rangeEndOffset;
                    r.domSelectionSetter(selection);
                }
                return Value::Undefined();
            });
            if(key==L"toString")return Native([range](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(RangeText(range));});
        }
        if(object->kind==ObjectKind::Node){
            auto node=object->node;if(!node)return Value::Undefined();
            if(key==L"contentWindow"&&node->tag==L"iframe")return FrameWindowValue(node);
            if(key==L"contentDocument"&&node->tag==L"iframe")return FrameDocumentValue(node);
            if(node->tag==L"img"&&(key==L"naturalWidth"||key==L"naturalHeight"))
                return Value::Number(node->image?
                    (key==L"naturalWidth"?node->image->width:node->image->height):0);
            if(node->tag==L"img"&&key==L"complete")return Value::Bool(node->imageComplete);
            if(node->tag==L"img"&&(key==L"width"||key==L"height")){
                const auto authored=Trim(node->Attribute(key));
                double parsed=0;
                if(TryParseDouble(authored,parsed)&&std::isfinite(parsed))
                    return Value::Number(std::max(0.0,parsed));
                if(node->image)return Value::Number(
                    key==L"width"?node->image->width:node->image->height);
                return Value::Number(0);
            }
            if((node->tag==L"iframe"||node->tag==L"embed"||node->tag==L"object"||
                node->tag==L"video")&&(key==L"width"||key==L"height")){
                const auto authored=Trim(node->Attribute(key));
                double parsed=0;
                if(TryParseDouble(authored,parsed)&&std::isfinite(parsed))
                    return Value::Number(std::max(0.0,parsed));
                return Value::Number(key==L"width"?300:150);
            }
            if(node->tag==L"canvas"&&(key==L"width"||key==L"height"))
                return Value::Number(CanvasDimension(node,key.c_str(),key==L"width"?300:150));
            if(node->tag==L"canvas"&&key==L"getContext")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty()||ToLower(Trim(r.String(a[0])))!=L"2d")return Value::Null();
                return r.CanvasContextValue(node);
            });
            if(key==L"id")return Value::String(node->Attribute(L"id"));
            if(key==L"className")return Value::String(node->Attribute(L"class"));
            if(key==L"value")return Value::String(node->Attribute(L"value"));
            if(key==L"nodeType")return Value::Number(IsDocumentFragment(node)?11:node->type==NodeType::Element?1:node->type==NodeType::Text?3:node->type==NodeType::Comment?8:9);
            if(key==L"ownerDocument")return global->values[L"document"];
            if(key==L"length"&&(node->type==NodeType::Text||node->type==NodeType::Comment))return Value::Number(static_cast<double>(node->text.size()));
            if(key==L"nodeValue")return node->type==NodeType::Text||node->type==NodeType::Comment?Value::String(node->text):Value::Null();
            if(key==L"tagName"||key==L"nodeName"){
                std::wstring name;
                if(node->type==NodeType::Element)name=IsDocumentFragment(node)?L"#document-fragment":node->tag;
                else if(node->type==NodeType::Text)name=L"#text";
                else if(node->type==NodeType::Comment)name=L"#comment";
                else name=L"#document";
                if(node->type==NodeType::Element&&!IsDocumentFragment(node))
                    std::transform(name.begin(),name.end(),name.begin(),[](wchar_t character){
                        return static_cast<wchar_t>(std::towupper(character));
                    });
                return Value::String(std::move(name));
            }
            if(key==L"selectionStart"||key==L"selectionEnd"){
                size_t start=node->selectionStart,end=node->selectionEnd;
                if(selectionProvider)selectionProvider(node,start,end);
                return Value::Number(static_cast<double>(key==L"selectionStart"?start:end));
            }
            if(key==L"selectionDirection")return Value::String(node->selectionDirection);
            if(key==L"files"&&node->tag==L"input"&&ToLower(node->Attribute(L"type"))==L"file"){
                return FileListValue(node->files);
            }
            if(key==L"checked")return Value::Bool(node->checked);
            if(key==L"indeterminate")return Value::Bool(node->indeterminate);
            if(key==L"disabled")return Value::Bool(node->disabled);
            if(key==L"isConnected"){
                for(auto current=node;current;current=current->parent.lock())
                    if(current->type==NodeType::Document)return Value::Bool(true);
                return Value::Bool(false);
            }
            if(key==L"required")return Value::Bool(node->attributes.count(L"required")!=0);
            if(key==L"hidden")return Value::Bool(node->attributes.count(L"hidden")!=0);
            if(key==L"open")return Value::Bool(node->attributes.count(L"open")!=0);
            if(key==L"defer")return Value::Bool(node->attributes.count(L"defer")!=0);
            if(key==L"contentEditable")return Value::String(node->Attribute(L"contenteditable"));
            if((node->tag==L"a"||node->tag==L"area")&&
               (key==L"href"||key==L"protocol"||key==L"host"||key==L"hostname"||
                key==L"port"||key==L"pathname"||key==L"search"||key==L"hash"||key==L"origin")){
                if(node->attributes.count(L"href")==0)return Value::String(L"");
                const auto parsed=UrlValue(node->Attribute(L"href"),location);
                const auto found=parsed.object->props.find(key);
                return found==parsed.object->props.end()?Value::String(L""):found->second;
            }
            if(key==L"title"||key==L"type"||key==L"lang"||key==L"src"||key==L"srcdoc"||key==L"href"||
               key==L"name"||key==L"placeholder"||key==L"rel"||key==L"alt"||
               key==L"draggable"||key==L"colSpan"||key==L"rowSpan"||key==L"returnValue")
                return key==L"draggable"?Value::Bool(node->Attribute(L"draggable")==L"true"):
                    Value::String(node->Attribute(key==L"colSpan"?L"colspan":
                        (key==L"rowSpan"?L"rowspan":ToLower(key))));
            if(key==L"scrollTop")return Value::Number(node->scrollTop);
            if(key==L"scrollLeft")return Value::Number(node->scrollLeft);
            if(key==L"offsetWidth"||key==L"offsetHeight"||
               key==L"offsetLeft"||key==L"offsetTop"){
                const auto geometry=geometryProvider?geometryProvider(node):
                    JavaScriptRuntime::NodeGeometry{};
                if(key==L"offsetWidth")return Value::Number(std::round(geometry.width));
                if(key==L"offsetHeight")return Value::Number(std::round(geometry.height));
                auto parent=node->parent.lock();
                while(parent&&parent->type!=NodeType::Element)parent=parent->parent.lock();
                const auto parentGeometry=parent&&geometryProvider?geometryProvider(parent):
                    JavaScriptRuntime::NodeGeometry{};
                return Value::Number(std::round((key==L"offsetLeft"?geometry.x:geometry.y)-
                    (parent?(key==L"offsetLeft"?parentGeometry.x:parentGeometry.y):0.0)));
            }
            if(key==L"offsetParent"){
                auto parent=node->parent.lock();
                while(parent&&parent->type!=NodeType::Element)parent=parent->parent.lock();
                return NodeValue(parent);
            }
            if(key==L"clientHeight"||key==L"clientWidth"||key==L"scrollHeight"||key==L"scrollWidth"){
                const auto g=geometryProvider?geometryProvider(node):JavaScriptRuntime::NodeGeometry{};
                return Value::Number(key==L"clientHeight"?g.clientHeight:key==L"clientWidth"?g.clientWidth:key==L"scrollWidth"?g.scrollWidth:g.scrollHeight);
            }
            if(key==L"getBoundingClientRect")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto g=r.geometryProvider?r.geometryProvider(node):JavaScriptRuntime::NodeGeometry{};
                auto rect=r.ObjectValue(ObjectKind::Plain);
                rect.object->props[L"x"]=rect.object->props[L"left"]=Value::Number(g.x);
                rect.object->props[L"y"]=rect.object->props[L"top"]=Value::Number(g.y);
                rect.object->props[L"width"]=Value::Number(g.width);
                rect.object->props[L"height"]=Value::Number(g.height);
                rect.object->props[L"right"]=Value::Number(g.x+g.width);
                rect.object->props[L"bottom"]=Value::Number(g.y+g.height);
                return rect;
            });
            if(key==L"getClientRects")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto g=r.geometryProvider?r.geometryProvider(node):JavaScriptRuntime::NodeGeometry{};
                if(g.width<=0&&g.height<=0)return r.ArrayValue({});
                auto rect=r.ObjectValue(ObjectKind::Plain);
                rect.object->props[L"x"]=rect.object->props[L"left"]=Value::Number(g.x);
                rect.object->props[L"y"]=rect.object->props[L"top"]=Value::Number(g.y);
                rect.object->props[L"width"]=Value::Number(g.width);
                rect.object->props[L"height"]=Value::Number(g.height);
                rect.object->props[L"right"]=Value::Number(g.x+g.width);
                rect.object->props[L"bottom"]=Value::Number(g.y+g.height);
                return r.ArrayValue({rect});
            });
            if(key==L"innerText"||key==L"textContent")return Value::String(node->InnerText());
            if(key==L"innerHTML")return Value::String(InnerHtml(node));
            if(key==L"content"&&node->tag==L"template")return NodeValue(node);
            if(key==L"attributes"){
                const auto cached=object->props.find(L"$attributes");
                if(cached!=object->props.end())return cached->second;
                auto attributes=CreateObject(ObjectKind::NamedNodeMap);attributes->node=node;
                const auto value=Value::FromObject(attributes);object->props[L"$attributes"]=value;
                return value;
            }
            if(key==L"children"){
                std::vector<Value> out;for(const auto& child:node->children)if(child->type==NodeType::Element)out.push_back(NodeValue(child));return ArrayValue(out);
            }
            if(key==L"elements"&&node->tag==L"form"){
                std::vector<Value> controls;
                std::function<void(const std::shared_ptr<Node>&)> collect=
                    [&](const std::shared_ptr<Node>& current){
                        for(const auto& child:current->children){
                            if(child->type!=NodeType::Element)continue;
                            if(child->tag==L"button"||child->tag==L"fieldset"||
                               child->tag==L"input"||child->tag==L"object"||
                               child->tag==L"output"||child->tag==L"select"||
                               child->tag==L"textarea")
                                controls.push_back(NodeValue(child));
                            collect(child);
                        }
                    };
                collect(node);return ArrayValue(controls);
            }
            if(key==L"childNodes"){
                std::vector<Value> out;for(const auto& child:node->children)out.push_back(NodeValue(child));return ArrayValue(out);
            }
            if(key==L"childElementCount"){
                size_t count=0;for(const auto& child:node->children)if(child->type==NodeType::Element)++count;return Value::Number(static_cast<double>(count));
            }
            if(key==L"firstElementChild"||key==L"lastElementChild"){
                if(key==L"firstElementChild"){for(const auto& child:node->children)if(child->type==NodeType::Element)return NodeValue(child);}
                else{for(auto child=node->children.rbegin();child!=node->children.rend();++child)if((*child)->type==NodeType::Element)return NodeValue(*child);}
                return Value::Null();
            }
            if(key==L"classList"){auto o=CreateObject(ObjectKind::ClassList);o->node=node;return Value::FromObject(o);}
            if(key==L"style"){auto o=CreateObject(ObjectKind::Style);o->node=node;return Value::FromObject(o);}
            if(key==L"dataset"){auto o=CreateObject(ObjectKind::Dataset);o->node=node;return Value::FromObject(o);}
            if(key==L"cells"){std::vector<Value> out;for(auto& c:node->children)if(c->tag==L"td"||c->tag==L"th")out.push_back(NodeValue(c));return ArrayValue(out);}
            if(key==L"tBodies"){std::vector<Value> out;for(auto& child:node->children)if(child->tag==L"tbody")out.push_back(NodeValue(child));return ArrayValue(out);}
            if(key==L"rows"){
                std::vector<Value> out;std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& current){
                    for(const auto& child:current->children){if(child->tag==L"tr")out.push_back(NodeValue(child));else if(child->tag==L"thead"||child->tag==L"tbody"||child->tag==L"tfoot")visit(child);}
                };visit(node);return ArrayValue(out);
            }
            if(key==L"parentNode")return NodeValue(node->parent.lock());
            if(key==L"parentElement"){const auto parent=node->parent.lock();return NodeValue(parent&&parent->type==NodeType::Element?parent:std::shared_ptr<Node>{});}
            if(key==L"firstChild"||key==L"lastChild")return NodeValue(node->children.empty()?std::shared_ptr<Node>{}:(key==L"firstChild"?node->children.front():node->children.back()));
            if(key==L"nextSibling"||key==L"previousSibling"){
                const auto parent=node->parent.lock();if(!parent)return Value::Null();
                const auto found=std::find(parent->children.begin(),parent->children.end(),node);if(found==parent->children.end())return Value::Null();
                if(key==L"nextSibling")return NodeValue(found+1==parent->children.end()?std::shared_ptr<Node>{}:*(found+1));
                return NodeValue(found==parent->children.begin()?std::shared_ptr<Node>{}:*(found-1));
            }
            if(key==L"nextElementSibling"||key==L"previousElementSibling"){
                auto parent=node->parent.lock();if(!parent)return Value::Null();
                const auto found=std::find(parent->children.begin(),parent->children.end(),node);
                if(found==parent->children.end())return Value::Null();
                if(key==L"nextElementSibling")for(auto current=std::next(found);current!=parent->children.end();++current)
                    if((*current)->type==NodeType::Element)return NodeValue(*current);
                if(key==L"previousElementSibling")for(auto current=found;current!=parent->children.begin();){--current;
                    if((*current)->type==NodeType::Element)return NodeValue(*current);}
                return Value::Null();
            }
            if(key==L"appendChild")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Null();
                auto child=value.object->node;auto old=child->parent.lock();
                bool indexOk=r.document.UnindexSubtree(child);
                if(old){
                    old->children.erase(std::remove(old->children.begin(),old->children.end(),child),old->children.end());
                }
                child->parent=node;node->children.push_back(child);
                indexOk=r.document.IndexSubtree(child)&&indexOk;
                if(old)r.Mutated(old,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.ExecuteConnectedScripts(child);return value;
            });
            if(key==L"insertBefore")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();auto value=r.Deref(a[0]);if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Null();
                auto child=value.object->node;auto old=child->parent.lock();bool indexOk=r.document.UnindexSubtree(child);
                if(old)old->children.erase(std::remove(old->children.begin(),old->children.end(),child),old->children.end());
                auto position=node->children.end();if(a.size()>1){auto before=r.Deref(a[1]);if(before.type==Value::Type::Object&&before.object&&before.object->kind==ObjectKind::Node)position=std::find(node->children.begin(),node->children.end(),before.object->node);}
                child->parent=node;node->children.insert(position,child);indexOk=r.document.IndexSubtree(child)&&indexOk;
                if(old)r.Mutated(old,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                r.ExecuteConnectedScripts(child);return value;
            });
            if(key==L"append"||key==L"prepend"||key==L"replaceChildren")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                bool indexOk=true;
                if(key==L"replaceChildren"){
                    for(auto& child:node->children)indexOk=r.document.UnindexSubtree(child)&&indexOk;
                    for(auto& child:node->children)child->parent.reset();node->children.clear();
                }
                size_t insertion=key==L"prepend"?0:node->children.size();
                for(const auto& input:a){
                    auto value=r.Deref(input);std::shared_ptr<Node> child;
                    if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)child=value.object->node;
                    else{child=std::make_shared<Node>();child->type=NodeType::Text;child->tag=L"#text";child->text=r.String(value);}
                    if(!child)continue;std::vector<std::shared_ptr<Node>> inserted;
                    if(IsDocumentFragment(child)){inserted=child->children;child->children.clear();}
                    else inserted.push_back(child);
                    for(auto& item:inserted){auto old=item->parent.lock();indexOk=r.document.UnindexSubtree(item)&&indexOk;
                        if(old){old->children.erase(std::remove(old->children.begin(),old->children.end(),item),old->children.end());r.Mutated(old,JavaScriptRuntime::MutationKind::Tree);}
                        item->parent=node;node->children.insert(node->children.begin()+static_cast<std::ptrdiff_t>(insertion++),item);indexOk=r.document.IndexSubtree(item)&&indexOk;
                        r.ExecuteConnectedScripts(item);
                    }
                }
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();
            });
            if(key==L"replaceChild")return Native([node](RuntimeCore& r,const Value&,
                                                          const std::vector<Value>& a){
                if(a.size()<2)return Value::Null();
                const auto replacementValue=r.Deref(a[0]),removedValue=r.Deref(a[1]);
                if(replacementValue.type!=Value::Type::Object||!replacementValue.object||
                   replacementValue.object->kind!=ObjectKind::Node||
                   removedValue.type!=Value::Type::Object||!removedValue.object||
                   removedValue.object->kind!=ObjectKind::Node)return Value::Null();
                auto replacement=replacementValue.object->node;
                const auto removed=removedValue.object->node;
                if(!replacement||!removed)return Value::Null();
                auto removedPosition=std::find(node->children.begin(),node->children.end(),removed);
                if(removedPosition==node->children.end())return Value::Null();
                if(replacement==removed)return removedValue;
                std::vector<std::shared_ptr<Node>> inserted;
                if(IsDocumentFragment(replacement)){
                    inserted=replacement->children;replacement->children.clear();
                }else inserted.push_back(replacement);
                bool indexOk=true;
                for(auto& item:inserted){
                    const auto old=item->parent.lock();
                    indexOk=r.document.UnindexSubtree(item)&&indexOk;
                    if(old){
                        old->children.erase(std::remove(old->children.begin(),
                            old->children.end(),item),old->children.end());
                        if(old!=node)r.Mutated(old,JavaScriptRuntime::MutationKind::Tree);
                    }
                    item->parent.reset();
                }
                removedPosition=std::find(node->children.begin(),node->children.end(),removed);
                if(removedPosition==node->children.end())return Value::Null();
                const auto insertion=static_cast<size_t>(removedPosition-node->children.begin());
                indexOk=r.document.UnindexSubtree(removed)&&indexOk;
                node->children.erase(removedPosition);removed->parent.reset();
                size_t offset=0;
                for(auto& item:inserted){
                    item->parent=node;
                    node->children.insert(node->children.begin()+
                        static_cast<std::ptrdiff_t>(insertion+offset++),item);
                    indexOk=r.document.IndexSubtree(item)&&indexOk;
                    r.ExecuteConnectedScripts(item);
                }
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);
                return removedValue;
            });
            if(key==L"removeChild")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)return Value::Null();
                const auto child=value.object->node;const auto position=std::find(node->children.begin(),node->children.end(),child);
                if(position==node->children.end())return Value::Null();
                const bool indexOk=r.document.UnindexSubtree(child);node->children.erase(position);child->parent.reset();
                r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return value;
            });
            if(key==L"setAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()>1)r.SetDomAttribute(node,r.String(a[0]),r.String(a[1]));
                return Value::Undefined();
            });
            if(key==L"getAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();const auto name=ToLower(r.String(a[0]));
                const auto found=node->attributes.find(name);return found==node->attributes.end()?Value::Null():Value::String(found->second);
            });
            if(key==L"getAttributeNode")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                return a.empty()?Value::Null():r.AttrValue(node,r.String(a[0]));
            });
            if(key==L"hasAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool(!a.empty()&&node->attributes.count(ToLower(r.String(a[0])))!=0);});
            if(key==L"removeAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!a.empty())r.RemoveDomAttribute(node,r.String(a[0]));
                return Value::Undefined();
            });
            if(key==L"toggleAttribute")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto name=ToLower(r.String(a[0]));
                const bool present=node->attributes.count(name)!=0;const bool enabled=a.size()>1?r.Truth(a[1]):!present;
                if(enabled&&!present)node->SetAttribute(name,L"");else if(!enabled&&present)node->RemoveAttribute(name);
                if(node->tag==L"dialog"&&name==L"open")node->modal=false;
                r.Mutated(node,JavaScriptRuntime::MutationKind::Style,false,false,name);return Value::Bool(enabled);
            });
            if(key==L"remove")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){if(auto parent=node->parent.lock()){const bool indexOk=r.document.UnindexSubtree(node);parent->children.erase(std::remove(parent->children.begin(),parent->children.end(),node),parent->children.end());node->parent.reset();r.Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);}return Value::Undefined();});
            if(key==L"replaceWith")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto parent=node->parent.lock();if(!parent)return Value::Undefined();auto position=std::find(parent->children.begin(),parent->children.end(),node);if(position==parent->children.end())return Value::Undefined();
                bool indexOk=r.document.UnindexSubtree(node);const auto offset=static_cast<size_t>(position-parent->children.begin());parent->children.erase(position);node->parent.reset();size_t insert=offset;
                for(const auto& input:a){auto value=r.Deref(input);std::shared_ptr<Node> replacement;if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)replacement=value.object->node;else{replacement=std::make_shared<Node>();replacement->type=NodeType::Text;replacement->tag=L"#text";replacement->text=r.String(value);}std::vector<std::shared_ptr<Node>> replacements;if(IsDocumentFragment(replacement)){replacements=replacement->children;replacement->children.clear();}else replacements.push_back(replacement);for(auto& item:replacements){auto old=item->parent.lock();indexOk=r.document.UnindexSubtree(item)&&indexOk;if(old){old->children.erase(std::remove(old->children.begin(),old->children.end(),item),old->children.end());r.Mutated(old,JavaScriptRuntime::MutationKind::Tree);}item->parent=parent;parent->children.insert(parent->children.begin()+static_cast<std::ptrdiff_t>(insert++),item);indexOk=r.document.IndexSubtree(item)&&indexOk;r.ExecuteConnectedScripts(item);}}
                r.Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();
            });
            if(key==L"cloneNode")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.NodeValue(CloneDomNode(node,!a.empty()&&r.Truth(a[0])));});
            if(key==L"normalize")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                // normalize() is commonly called after every contenteditable
                // input.  A single existing text node is already normalized;
                // avoid turning that no-op into a full DOM index and layout
                // rebuild. Text-node merges do not affect element indexes.
                if(NormalizeDomNode(node))r.Mutated(node,JavaScriptRuntime::MutationKind::Tree);
                return Value::Undefined();
            });
            if(key==L"focus")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){if(r.focusSink)r.focusSink(node);else{node->focused=true;r.Mutated(node,JavaScriptRuntime::MutationKind::Style);}return Value::Undefined();});
            if(key==L"blur")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){if(r.focusSink)r.focusSink({});return Value::Undefined();});
            if(key==L"click")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){if(r.activationSink)r.activationSink(node);else r.Dispatch(node,L"click");return Value::Undefined();});
            if(key==L"dispatchEvent")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Event)throw JavaScriptException{r.ErrorValue(L"TypeError",L"dispatchEvent requires an Event")};
                JavaScriptRuntime::EventInit init{};init.bubbles=r.Truth(r.GetProperty(value,L"bubbles"));init.cancelable=r.Truth(r.GetProperty(value,L"cancelable"));
                init.data=r.String(r.GetProperty(value,L"data"));init.inputType=r.String(r.GetProperty(value,L"inputType"));
                return Value::Bool(!r.Dispatch(node,r.String(r.GetProperty(value,L"type")),nullptr,init));
            });
            if(key==L"setSelectionRange")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto length=node->Attribute(L"value").size();
                const auto start=std::min(length,static_cast<size_t>(std::max(0.0,a.empty()?0:r.Number(a[0]))));
                const auto end=std::min(length,static_cast<size_t>(std::max(0.0,a.size()>1?r.Number(a[1]):static_cast<double>(start))));
                const auto direction=a.size()>2?ToLower(r.String(a[2])):L"none";
                node->selectionStart=std::min(start,end);node->selectionEnd=std::max(start,end);
                node->selectionDirection=direction==L"forward"||direction==L"backward"?direction:L"none";
                if(r.selectionSetter)r.selectionSetter(node,node->selectionStart,node->selectionEnd);
                return Value::Undefined();
            });
            if(key==L"select")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto length=node->Attribute(L"value").size();node->selectionStart=0;node->selectionEnd=length;node->selectionDirection=L"none";if(r.focusSink)r.focusSink(node);if(r.selectionSetter)r.selectionSetter(node,0,length);return Value::Undefined();});
            if(key==L"setRangeText")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();auto value=node->Attribute(L"value");
                size_t start=a.size()>1?static_cast<size_t>(std::max(0.0,r.Number(a[1]))):node->selectionStart;
                size_t end=a.size()>2?static_cast<size_t>(std::max(0.0,r.Number(a[2]))):node->selectionEnd;
                start=std::min(start,value.size());end=std::min(end,value.size());if(end<start)std::swap(start,end);
                const auto replacement=r.String(a[0]);value.replace(start,end-start,replacement);node->SetAttribute(L"value",value);
                const auto mode=a.size()>3?ToLower(r.String(a[3])):L"preserve";size_t selectionStart=start,selectionEnd=start+replacement.size();
                if(mode==L"start")selectionEnd=selectionStart;else if(mode==L"end")selectionStart=selectionEnd;else if(mode==L"preserve"){
                    const auto delta=static_cast<long long>(replacement.size())-static_cast<long long>(end-start);
                    auto adjust=[&](size_t position){if(position<=start)return position;if(position>=end)return static_cast<size_t>(std::max<long long>(0,static_cast<long long>(position)+delta));return start+replacement.size();};
                    selectionStart=adjust(node->selectionStart);selectionEnd=adjust(node->selectionEnd);
                }
                node->selectionStart=std::min(selectionStart,value.size());node->selectionEnd=std::min(selectionEnd,value.size());
                node->selectionDirection=L"none";
                if(r.selectionSetter)r.selectionSetter(node,node->selectionStart,node->selectionEnd);r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);return Value::Undefined();
            });
            if(key==L"setCustomValidity")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){object->props[L"$customValidity"]=Value::String(a.empty()?L"":r.String(a[0]));return Value::Undefined();});
            if(key==L"reportValidity")return Native([object,node](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto custom=object->props.find(L"$customValidity");const bool customError=custom!=object->props.end()&&!r.String(custom->second).empty();const bool missing=node->attributes.count(L"required")&&!node->disabled&&node->Attribute(L"value").empty();return Value::Bool(!customError&&!missing);});
            if(key==L"reset"&&node->tag==L"form")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                std::function<void(const std::shared_ptr<Node>&)> reset=[&](const std::shared_ptr<Node>& current){for(const auto& child:current->children){if(child->tag==L"input"||child->tag==L"textarea"){const auto type=ToLower(child->Attribute(L"type"));if(type==L"checkbox"||type==L"radio")child->checked=child->attributes.count(L"checked")!=0;else{child->SetAttribute(L"value",L"");child->files.clear();child->selectionStart=child->selectionEnd=0;child->selectionDirection=L"none";}}reset(child);}};reset(node);r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);return Value::Undefined();
            });
            if((key==L"showModal"||key==L"show")&&node->tag==L"dialog")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>&){node->SetAttribute(L"open",L"");node->modal=key==L"showModal";r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);return Value::Undefined();});
            if(key==L"close"&&node->tag==L"dialog")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){node->RemoveAttribute(L"open");node->modal=false;node->SetAttribute(L"returnvalue",a.empty()?L"":r.String(a[0]));r.Mutated(node,JavaScriptRuntime::MutationKind::Layout);r.Dispatch(node,L"close");return Value::Undefined();});
            if(key==L"requestSubmit"&&node->tag==L"form")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){r.Dispatch(node,L"submit");return Value::Undefined();});
            if(key==L"scrollTo"||key==L"scroll"||key==L"scrollBy")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const bool relative=key==L"scrollBy";double left=relative?0:node->scrollLeft,top=relative?0:node->scrollTop;
                if(!a.empty()){
                    auto value=r.Deref(a[0]);
                    if(value.type==Value::Type::Object&&value.object){
                        if(value.object->props.count(L"left"))left=r.Number(value.object->props[L"left"]);
                        if(value.object->props.count(L"top"))top=r.Number(value.object->props[L"top"]);
                    }else{
                        left=r.Number(value);top=a.size()>1?r.Number(a[1]):0;
                    }
                }
                node->scrollLeft=std::max(0.0f,static_cast<float>((relative?node->scrollLeft:0)+left));node->scrollTop=std::max(0.0f,static_cast<float>((relative?node->scrollTop:0)+top));r.Mutated(node,JavaScriptRuntime::MutationKind::Paint);return Value::Undefined();
            });
            if(key==L"scrollIntoView")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){
                if(!r.geometryProvider)return Value::Undefined();const auto target=r.geometryProvider(node);
                for(auto ancestor=node->parent.lock();ancestor;ancestor=ancestor->parent.lock()){
                    const auto geometry=r.geometryProvider(ancestor);if(geometry.scrollHeight>geometry.clientHeight+0.01){
                        if(target.y<geometry.y)ancestor->scrollTop=std::max(0.0f,ancestor->scrollTop+static_cast<float>(target.y-geometry.y));
                        else if(target.y+target.height>geometry.y+geometry.clientHeight)ancestor->scrollTop=std::max(0.0f,ancestor->scrollTop+static_cast<float>(target.y+target.height-geometry.y-geometry.clientHeight));
                        r.Mutated(ancestor,JavaScriptRuntime::MutationKind::Paint);
                    }
                }return Value::Undefined();
            });
            if(key==L"setPointerCapture")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty()||static_cast<int>(r.Number(a[0]))==1){r.pointerCaptureNode=node;if(r.pointerCaptureSink)r.pointerCaptureSink(true);}return Value::Undefined();});
            if(key==L"hasPointerCapture")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool((a.empty()||static_cast<int>(r.Number(a[0]))==1)&&r.pointerCaptureNode.lock()==node);});
            if(key==L"releasePointerCapture")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){if((a.empty()||static_cast<int>(r.Number(a[0]))==1)&&r.pointerCaptureNode.lock()==node){r.pointerCaptureNode.reset();if(r.pointerCaptureSink)r.pointerCaptureSink(false);}return Value::Undefined();});
            if(key==L"createTBody"&&node->tag==L"table")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>&){auto body=r.document.CreateElement(L"tbody");body->parent=node;node->children.push_back(body);const bool indexOk=r.document.IndexSubtree(body);r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return r.NodeValue(body);});
            if(key==L"insertRow"&&(node->tag==L"table"||node->tag==L"thead"||node->tag==L"tbody"||node->tag==L"tfoot"))return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto container=node;if(node->tag==L"table"){for(const auto& child:node->children)if(child->tag==L"tbody"){container=child;break;}if(container==node){container=r.document.CreateElement(L"tbody");container->parent=node;node->children.push_back(container);}}
                std::vector<std::shared_ptr<Node>> rows;for(const auto& child:container->children)if(child->tag==L"tr")rows.push_back(child);
                long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?rows.size():std::min(rows.size(),static_cast<size_t>(requested));
                auto row=r.document.CreateElement(L"tr");row->parent=container;auto position=container->children.end();if(index<rows.size())position=std::find(container->children.begin(),container->children.end(),rows[index]);container->children.insert(position,row);
                const bool indexOk=r.document.IndexSubtree(row);r.Mutated(container,JavaScriptRuntime::MutationKind::Tree,!indexOk);return r.NodeValue(row);
            });
            if(key==L"deleteRow"&&(node->tag==L"table"||node->tag==L"thead"||node->tag==L"tbody"||node->tag==L"tfoot"))return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                std::vector<std::shared_ptr<Node>> rows;std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& current){for(const auto& child:current->children){if(child->tag==L"tr")rows.push_back(child);else if(current->tag==L"table"&&(child->tag==L"thead"||child->tag==L"tbody"||child->tag==L"tfoot"))collect(child);}};collect(node);if(rows.empty())return Value::Undefined();
                long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?rows.size()-1:static_cast<size_t>(requested);if(index>=rows.size())return Value::Undefined();auto row=rows[index];auto parent=row->parent.lock();const bool indexOk=r.document.UnindexSubtree(row);parent->children.erase(std::remove(parent->children.begin(),parent->children.end(),row),parent->children.end());row->parent.reset();r.Mutated(parent,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();
            });
            if(key==L"insertCell"&&node->tag==L"tr")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){std::vector<std::shared_ptr<Node>> cells;for(const auto& child:node->children)if(child->tag==L"td"||child->tag==L"th")cells.push_back(child);long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?cells.size():std::min(cells.size(),static_cast<size_t>(requested));auto cell=r.document.CreateElement(L"td");cell->parent=node;auto position=node->children.end();if(index<cells.size())position=std::find(node->children.begin(),node->children.end(),cells[index]);node->children.insert(position,cell);const bool indexOk=r.document.IndexSubtree(cell);r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return r.NodeValue(cell);});
            if(key==L"deleteCell"&&node->tag==L"tr")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){std::vector<std::shared_ptr<Node>> cells;for(const auto& child:node->children)if(child->tag==L"td"||child->tag==L"th")cells.push_back(child);if(cells.empty())return Value::Undefined();long long requested=a.empty()?-1:static_cast<long long>(r.Number(a[0]));size_t index=requested<0?cells.size()-1:static_cast<size_t>(requested);if(index>=cells.size())return Value::Undefined();auto cell=cells[index];const bool indexOk=r.document.UnindexSubtree(cell);node->children.erase(std::remove(node->children.begin(),node->children.end(),cell),node->children.end());cell->parent.reset();r.Mutated(node,JavaScriptRuntime::MutationKind::Tree,!indexOk);return Value::Undefined();});
            if(key==L"hasChildNodes")return Native([node](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Bool(!node->children.empty());});
            if(key==L"contains")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Bool(false);auto value=r.Deref(a[0]);if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node)return Value::Bool(false);for(auto current=value.object->node;current;current=current->parent.lock())if(current==node)return Value::Bool(true);return Value::Bool(false);});
            if(key==L"compareDocumentPosition")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())throw JavaScriptException{r.ErrorValue(L"TypeError",L"compareDocumentPosition requires a Node")};
                const auto value=r.Deref(a[0]);
                if(value.type!=Value::Type::Object||!value.object||value.object->kind!=ObjectKind::Node||!value.object->node)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"compareDocumentPosition requires a Node")};
                const auto other=value.object->node;if(other==node)return Value::Number(0);
                std::vector<std::shared_ptr<Node>> left,right;
                for(auto current=node;current;current=current->parent.lock())left.push_back(current);
                for(auto current=other;current;current=current->parent.lock())right.push_back(current);
                if(left.back()!=right.back())return Value::Number(1|32);
                if(std::find(left.begin(),left.end(),other)!=left.end())return Value::Number(2|8);
                if(std::find(right.begin(),right.end(),node)!=right.end())return Value::Number(4|16);
                size_t li=left.size(),ri=right.size();
                while(li>0&&ri>0&&left[li-1]==right[ri-1]){--li;--ri;}
                const auto parent=left[li-1]->parent.lock();
                if(parent)for(const auto& child:parent->children){
                    if(child==left[li-1])return Value::Number(4);
                    if(child==right[ri-1])return Value::Number(2);
                }
                return Value::Number(1|32);
            });
            if(key==L"addEventListener")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto& listeners=r.listeners[node.get()];if(listeners.node.lock()!=node)listeners.events.clear();listeners.node=node;r.AddEventListener(listeners.events,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto found=r.listeners.find(node.get());if(found!=r.listeners.end()){if(found->second.node.lock()!=node){r.listeners.erase(node.get());return Value::Undefined();}r.RemoveEventListener(found->second.events,a);if(found->second.events.empty())r.listeners.erase(node.get());}return Value::Undefined();});
            if(key==L"querySelector"||key==L"querySelectorAll")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.EnsureIndex();auto selector=a.empty()?L"":r.String(a[0]);if(key==L"querySelector")return r.NodeValue(r.document.QuerySelector(selector,node));std::vector<Value> out;for(auto& n:r.document.QuerySelectorAll(selector,node))out.push_back(r.NodeValue(n));return r.ArrayValue(out);});
            if(key==L"getElementsByTagName"||key==L"getElementsByClassName")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto argument=a.empty()?L"":r.String(a[0]);const auto tag=ToLower(argument);
                std::vector<std::wstring> classes;if(key==L"getElementsByClassName"){
                    std::wistringstream input(argument);std::wstring token;while(input>>token)classes.push_back(token);
                }
                std::vector<Value> out;std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& current){
                    for(const auto& child:current->children){
                        const bool matches=child->type==NodeType::Element&&
                            (key==L"getElementsByTagName"?(tag==L"*"||child->tag==tag):
                             (!classes.empty()&&std::all_of(
                                 classes.begin(),classes.end(),
                                 [&](const auto& name){return child->HasClass(name);}))
                            );
                        if(matches)out.push_back(r.NodeValue(child));collect(child);
                    }
                };collect(node);return r.ArrayValue(out);
            });
            if(key==L"closest")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.NodeValue(node->Closest(a.empty()?L"":r.String(a[0])));});
            if(key==L"matches")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool(!a.empty()&&Document::MatchesSelector(node,r.String(a[0])));});
            if(node->tag==L"form"){
                std::shared_ptr<Node> named;std::function<void(const std::shared_ptr<Node>&)> find=[&](const std::shared_ptr<Node>& current){
                    if(named)return;for(const auto& child:current->children){
                        if(child->Attribute(L"name")==key||child->Attribute(L"id")==key){named=child;return;}find(child);
                    }
                };find(node);if(named)return NodeValue(named);
            }
        }
        if(object->kind==ObjectKind::NamedNodeMap){
            const auto node=object->node;
            if(key==L"length")return Value::Number(node?static_cast<double>(node->attributes.size()):0);
            if(key==L"item")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const double raw=a.empty()?0:r.Number(a[0]);
                if(!std::isfinite(raw)||raw<0)return Value::Null();
                return r.AttrAt(object->node,static_cast<size_t>(std::floor(raw)));
            });
            if(key==L"getNamedItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                return a.empty()?Value::Null():r.AttrValue(object->node,r.String(a[0]));
            });
            if(key==L"setNamedItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())throw JavaScriptException{r.ErrorValue(L"TypeError",L"Attr argument is required")};
                const auto attribute=r.Deref(a[0]);
                if(attribute.type!=Value::Type::Object||!attribute.object||
                   attribute.object->kind!=ObjectKind::Attr)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"Argument is not an Attr")};
                const auto name=r.String(attribute.object->props[L"$name"]);
                const auto previous=r.AttrValue(object->node,name);
                r.SetDomAttribute(object->node,name,r.String(r.GetProperty(attribute,L"value")));
                attribute.object->node=object->node;
                return previous;
            });
            if(key==L"removeNamedItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto name=ToLower(a.empty()?L"":r.String(a[0]));
                const auto removed=r.AttrValue(object->node,name);
                if(removed.type==Value::Type::Null)
                    throw JavaScriptException{r.ErrorValue(L"NotFoundError",L"Attribute was not found")};
                r.RemoveDomAttribute(object->node,name);return removed;
            });
            if(key==L"\uffffsymbol.wellKnown:iterator")return Native([object](RuntimeCore& r,const Value&,
                                                                              const std::vector<Value>&){
                std::vector<Value> values;
                if(object->node){values.reserve(object->node->attributes.size());
                    for(const auto& attribute:object->node->attributes)
                        values.push_back(r.AttrValue(object->node,attribute.first));}
                return r.IteratorValue(std::move(values));
            });
            size_t index=0;
            if(TryParseDecimalIndex(key,index))return AttrAt(node,index);
            return AttrValue(node,key);
        }
        if(object->kind==ObjectKind::Attr){
            const auto storedName=object->props.find(L"$name");
            const auto name=storedName==object->props.end()?L"":String(storedName->second);
            if(key==L"name"||key==L"localName"||key==L"nodeName")return Value::String(name);
            if(key==L"nodeType")return Value::Number(2);
            if(key==L"specified")return Value::Bool(true);
            if(key==L"namespaceURI"||key==L"prefix")return Value::Null();
            if(key==L"ownerElement")return object->node&&object->node->attributes.count(name)?
                NodeValue(object->node):Value::Null();
            if(key==L"value"||key==L"nodeValue"||key==L"textContent"){
                if(object->node){const auto found=object->node->attributes.find(name);
                    if(found!=object->node->attributes.end())return Value::String(found->second);}
                const auto stored=object->props.find(L"$value");
                return Value::String(stored==object->props.end()?L"":String(stored->second));
            }
        }
        if(object->kind==ObjectKind::ClassList){
            auto node=object->node;
            if(key==L"length"){size_t count=0;std::wistringstream classes(node?node->Attribute(L"class"):L"");std::wstring token;while(classes>>token)++count;return Value::Number(static_cast<double>(count));}
            if(key==L"add"||key==L"remove")return Native([node,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!node)return Value::Undefined();
                const auto before=node->Attribute(L"class");
                for(auto& v:a)if(key==L"add")node->AddClass(r.String(v));else node->RemoveClass(r.String(v));
                if(node->Attribute(L"class")!=before)
                    r.Mutated(node,JavaScriptRuntime::MutationKind::Style);
                return Value::Undefined();
            });
            if(key==L"toggle")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!node||a.empty())return Value::Bool(false);const auto name=r.String(a[0]);
                const auto before=node->Attribute(L"class");
                bool has=a.size()>1;
                bool force=has?r.Truth(a[1]):false;node->ToggleClass(name,force,has);
                if(node->Attribute(L"class")!=before)
                    r.Mutated(node,JavaScriptRuntime::MutationKind::Style);
                return Value::Bool(node->HasClass(name));
            });
            if(key==L"contains")return Native([node](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Bool(!a.empty()&&node->HasClass(r.String(a[0])));});
        }
        if(object->kind==ObjectKind::Style){
            const auto computedFallback=[&](const std::wstring& name){
                static const StyleSheet defaults;
                return defaults.Compute(object->node).Get(name);
            };
            if(key==L"length")return Value::Number(object->node?static_cast<double>(object->node->inlineStyle.size()):0);
            if(key==L"cssText")return Value::String(SerializeInlineStyle(object->node));
            if(key==L"item")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!object->node||a.empty())return Value::String(L"");
                const auto index=static_cast<size_t>(std::max(0.0,r.Number(a[0])));
                size_t current=0;for(const auto& declaration:object->node->inlineStyle)
                    if(current++==index)return Value::String(declaration.first);
                return Value::String(L"");
            });
            if(key==L"setProperty")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(object->node&&a.size()>1){
                    auto name=Trim(r.String(a[0]));if(name.rfind(L"--",0)!=0)name=ToLower(name);
                    const auto value=r.String(a[1]);
                    const auto priority=a.size()>2?ToLower(Trim(r.String(a[2]))):L"";
                    if(!name.empty()&&(priority.empty()||priority==L"important")){
                        if(value.empty()){object->node->inlineStyle.erase(name);object->node->inlineStylePriority.erase(name);}
                        else{object->node->inlineStyle[name]=value;if(priority==L"important")object->node->inlineStylePriority[name]=priority;else object->node->inlineStylePriority.erase(name);}
                        SyncInlineStyleAttribute(object->node);r.Mutated(object->node,JavaScriptRuntime::MutationKind::Style);
                    }
                }
                return Value::Undefined();
            });
            if(key==L"getPropertyValue")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!object->node||a.empty())return Value::String(L"");
                const auto name=ToLower(r.String(a[0]));if(object->props.count(L"$computed")){static const StyleSheet defaults;return Value::String(r.stylePropertyProvider?r.stylePropertyProvider(object->node,name):defaults.Compute(object->node).Get(name));}
                const auto found=object->node->inlineStyle.find(name);return Value::String(found==object->node->inlineStyle.end()?L"":found->second);
            });
            if(key==L"getPropertyPriority")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!object->node||a.empty())return Value::String(L"");
                auto name=Trim(r.String(a[0]));if(name.rfind(L"--",0)!=0)name=ToLower(name);
                const auto found=object->node->inlineStylePriority.find(name);
                return Value::String(found==object->node->inlineStylePriority.end()?L"":found->second);
            });
            if(key==L"removeProperty")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!object->node||a.empty())return Value::String(L"");
                auto name=Trim(r.String(a[0]));if(name.rfind(L"--",0)!=0)name=ToLower(name);
                const auto found=object->node->inlineStyle.find(name);
                if(found==object->node->inlineStyle.end())return Value::String(L"");
                const auto previous=found->second;object->node->inlineStyle.erase(name);
                object->node->inlineStylePriority.erase(name);SyncInlineStyleAttribute(object->node);
                r.Mutated(object->node,JavaScriptRuntime::MutationKind::Style);
                return Value::String(previous);
            });
            if(!object->node)return Value::String(L"");const auto name=CamelToKebab(key);if(object->props.count(L"$computed"))return Value::String(stylePropertyProvider?stylePropertyProvider(object->node,name):computedFallback(name));const auto found=object->node->inlineStyle.find(name);return Value::String(found==object->node->inlineStyle.end()?L"":found->second);
        }
        if(object->kind==ObjectKind::Dataset)
            return Value::String(object->node?object->node->Attribute(DatasetAttributeName(key)):L"");
        if(object->kind==ObjectKind::Window){
            if(key==L"addEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.windowListeners,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.RemoveEventListener(r.windowListeners,a);return Value::Undefined();});
            if(key==L"focus"||key==L"close")return Native([](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Undefined();});
            if(key==L"open")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!a.empty())r.RequestNavigation(r.String(a[0]));return Value::Null();
            });
            if(key==L"scrollTo"||key==L"scroll"||key==L"scrollBy")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const auto body=r.document.Body();if(!body)return Value::Undefined();const bool relative=key==L"scrollBy";
                double left=relative?0:body->scrollLeft,top=relative?0:body->scrollTop;
                if(!a.empty()){
                    const auto first=r.Deref(a[0]);
                    if(first.type==Value::Type::Object&&first.object){
                        if(first.object->props.count(L"left"))left=r.Number(first.object->props[L"left"]);
                        if(first.object->props.count(L"top"))top=r.Number(first.object->props[L"top"]);
                    }else{left=r.Number(first);top=a.size()>1?r.Number(a[1]):0;}
                }
                body->scrollLeft=std::max(0.0f,static_cast<float>((relative?body->scrollLeft:0)+left));
                body->scrollTop=std::max(0.0f,static_cast<float>((relative?body->scrollTop:0)+top));
                const auto x=Value::Number(body->scrollLeft),y=Value::Number(body->scrollTop);
                r.global->values[L"scrollX"]=r.global->values[L"pageXOffset"]=x;
                r.global->values[L"scrollY"]=r.global->values[L"pageYOffset"]=y;
                const auto window=r.global->values[L"window"].object;
                if(window){window->props[L"scrollX"]=window->props[L"pageXOffset"]=x;window->props[L"scrollY"]=window->props[L"pageYOffset"]=y;}
                r.Mutated(body,JavaScriptRuntime::MutationKind::Paint);return Value::Undefined();
            });
            if(key==L"dispatchEvent")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);const auto eventValue=r.Deref(a[0]);
                if(eventValue.type!=Value::Type::Object||!eventValue.object||eventValue.object->kind!=ObjectKind::Event)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"dispatchEvent requires an Event")};
                return Value::Bool(r.DispatchWindowEventObject(eventValue.object));
            });
            const auto globalProperty=global->values.find(key);
            if(globalProperty!=global->values.end())return globalProperty->second;
            if(key==L"length")return Value::Number(static_cast<double>(WindowFrameNodes().size()));
            if(const auto frame=WindowFrameNode(key))return FrameWindowValue(frame);
        }
        if(object->kind==ObjectKind::FrameWindow&&key==L"postMessage")return Native([object,node=object->node](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(!a.empty()){
                const auto data=r.Json(a[0]);
                const auto origin=r.CurrentOrigin();
                if(node){if(r.frameMessageSink)r.frameMessageSink(node,data,origin);}
                else if(object->props.count(L"$topProxy")){if(r.topMessageSink)r.topMessageSink(data,origin);}
                else if(r.parentMessageSink)r.parentMessageSink(data,origin);
            }
            return Value::Undefined();
        });
        if(object->kind==ObjectKind::FrameWindow&&key==L"document")
            return FrameDocumentValue(object->node);
        if(object->kind==ObjectKind::FrameDocument){
            if(key==L"defaultView"||key==L"parentWindow")return FrameWindowValue(object->node);
            if(key==L"readyState")return Value::String(L"complete");
            if(key==L"URL"||key==L"documentURI"||key==L"baseURI")
                return Value::String(L"about:blank");
            if(key==L"open")return Native([object](RuntimeCore&,const Value&,
                                                    const std::vector<Value>&){
                object->props[L"$text"]=Value::String(L"");
                object->props[L"$open"]=Value::Bool(true);
                return Value::FromObject(object);
            });
            if(key==L"write"||key==L"writeln")return Native([object,key](RuntimeCore& r,
                const Value&,const std::vector<Value>& values){
                auto& stored=object->props[L"$text"];
                std::wstring text=stored.type==Value::Type::String?stored.string:L"";
                for(const auto& value:values)text+=r.String(value);
                if(key==L"writeln")text+=L'\n';
                stored=Value::String(std::move(text));
                object->props[L"$open"]=Value::Bool(true);
                return Value::Undefined();
            });
            if(key==L"close")return Native([object](RuntimeCore& r,const Value&,
                                                     const std::vector<Value>&){
                const auto found=object->props.find(L"$text");
                const auto markup=found==object->props.end()?L"":r.String(found->second);
                object->props[L"$open"]=Value::Bool(false);
                if(r.frameDocumentSink&&object->node)
                    r.frameDocumentSink(object->node,markup);
                return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::Event){
            if(key==L"initEvent"||key==L"initCustomEvent")return Native([object,key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                object->props[L"type"]=Value::String(a.empty()?L"":r.String(a[0]));
                object->props[L"bubbles"]=Value::Bool(a.size()>1&&r.Truth(a[1]));
                object->props[L"cancelable"]=Value::Bool(a.size()>2&&r.Truth(a[2]));
                object->props[L"defaultPrevented"]=Value::Bool(false);
                if(key==L"initCustomEvent")object->props[L"detail"]=a.size()>3?r.Deref(a[3]):Value::Null();
                return Value::Undefined();
            });
            if(key==L"stopPropagation")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->props[L"$propagationStopped"]=Value::Bool(true);return Value::Undefined();});
            if(key==L"stopImmediatePropagation")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->props[L"$propagationStopped"]=Value::Bool(true);object->props[L"$immediateStopped"]=Value::Bool(true);return Value::Undefined();});
            if(key==L"preventDefault")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto passive=object->props.find(L"$inPassiveListener");if(passive==object->props.end()||!r.Truth(passive->second))object->props[L"defaultPrevented"]=Value::Bool(true);return Value::Undefined();});
            if(key==L"composedPath")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){std::vector<Value> path;auto target=object->props.find(L"target");if(target!=object->props.end()){auto value=r.Deref(target->second);if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node)for(auto node=value.object->node;node;node=node->parent.lock())path.push_back(r.NodeValue(node));}return r.ArrayValue(path);});
        }
        if(object->kind==ObjectKind::WebView){
            if(key==L"postMessage")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(r.messageSink&&!a.empty()){
                    const auto value=r.Deref(a[0]);
                    r.messageSink(value.type==Value::Type::Object?r.Json(value):r.String(value));
                }
                return Value::Undefined();
            });
            if(key==L"addEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.webViewListeners,a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.RemoveEventListener(r.webViewListeners,a);return Value::Undefined();});
        }
        if(object->kind==ObjectKind::Storage){
            if(key==L"getItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Null();const auto found=object->props.find(r.String(a[0]));return found==object->props.end()?Value::Null():found->second;});
            if(key==L"setItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.size()>1)object->props[r.String(a[0])]=Value::String(r.String(a[1]));return Value::Undefined();});
            if(key==L"removeItem")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(!a.empty())object->props.erase(r.String(a[0]));return Value::Undefined();});
        }
        if(object->kind==ObjectKind::MutationObserver){
            if(key==L"observe")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())throw JavaScriptException{r.ErrorValue(L"TypeError",L"MutationObserver.observe requires a Node")};
                const auto target=r.Deref(a[0]);
                if(target.type!=Value::Type::Object||!target.object||target.object->kind!=ObjectKind::Node||!target.object->node)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"MutationObserver.observe requires a Node")};
                MutationObserverRegistration registration;registration.observer=object;
                registration.target=target.object->node;
                if(a.size()>1){const auto options=r.Deref(a[1]);
                    registration.subtree=r.Truth(r.GetProperty(options,L"subtree"));
                    registration.attributes=r.Truth(r.GetProperty(options,L"attributes"));
                    registration.childList=r.Truth(r.GetProperty(options,L"childList"));
                    registration.characterData=r.Truth(r.GetProperty(options,L"characterData"));
                    const auto filter=r.Deref(r.GetProperty(options,L"attributeFilter"));
                    if(filter.type==Value::Type::Object&&filter.object&&filter.object->kind==ObjectKind::Array)
                        for(const auto& item:filter.object->items)
                            registration.attributeFilter.push_back(ToLower(r.String(item)));
                    if(!registration.attributeFilter.empty())registration.attributes=true;
                }
                if(!registration.attributes&&!registration.childList&&!registration.characterData)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"MutationObserver options do not select any mutation type")};
                r.mutationObservers.erase(std::remove_if(r.mutationObservers.begin(),r.mutationObservers.end(),
                    [&](const MutationObserverRegistration& existing){return existing.observer.lock()==object&&existing.target.lock()==target.object->node;}),r.mutationObservers.end());
                r.mutationObservers.push_back(std::move(registration));return Value::Undefined();
            });
            if(key==L"disconnect")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                r.mutationObservers.erase(std::remove_if(r.mutationObservers.begin(),r.mutationObservers.end(),
                    [&](const MutationObserverRegistration& registration){return registration.observer.lock()==object;}),r.mutationObservers.end());
                object->items.clear();return Value::Undefined();
            });
            if(key==L"takeRecords")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){auto records=std::move(object->items);object->items.clear();return r.ArrayValue(records);});
        }
        if(object->kind==ObjectKind::IntersectionObserver||object->kind==ObjectKind::ResizeObserver){
            if(key==L"observe")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())throw JavaScriptException{r.ErrorValue(L"TypeError",L"observer.observe requires a Node")};
                const auto target=r.Deref(a[0]);
                if(target.type!=Value::Type::Object||!target.object||target.object->kind!=ObjectKind::Node||!target.object->node)
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"observer.observe requires a Node")};
                const auto node=target.object->node;
                if(std::none_of(object->observedNodes.begin(),object->observedNodes.end(),
                    [&](const auto& weak){return weak.lock()==node;}))object->observedNodes.push_back(node);
                r.EnqueueMicrotask([object,node,&r]{
                    if(std::none_of(object->observedNodes.begin(),object->observedNodes.end(),
                        [&](const auto& weak){return weak.lock()==node;}))return;
                    const auto callback=object->props.find(L"$callback");
                    if(callback==object->props.end()||!r.IsCallable(callback->second))return;
                    const auto geometry=r.geometryProvider?r.geometryProvider(node):JavaScriptRuntime::NodeGeometry{};
                    auto rectangle=r.ObjectValue(ObjectKind::Plain);
                    rectangle.object->props[L"x"]=rectangle.object->props[L"left"]=Value::Number(geometry.x);
                    rectangle.object->props[L"y"]=rectangle.object->props[L"top"]=Value::Number(geometry.y);
                    rectangle.object->props[L"width"]=Value::Number(geometry.width);
                    rectangle.object->props[L"height"]=Value::Number(geometry.height);
                    rectangle.object->props[L"right"]=Value::Number(geometry.x+geometry.width);
                    rectangle.object->props[L"bottom"]=Value::Number(geometry.y+geometry.height);
                    auto entry=r.ObjectValue(ObjectKind::Plain);entry.object->props[L"target"]=r.NodeValue(node);
                    entry.object->props[L"boundingClientRect"]=rectangle;
                    entry.object->props[L"contentRect"]=rectangle;
                    if(object->kind==ObjectKind::IntersectionObserver){
                        const bool hasGeometry=geometry.width>0||geometry.height>0;
                        const bool intersects=!hasGeometry||
                            (geometry.x+geometry.width>=-200&&geometry.y+geometry.height>=-200&&
                             geometry.x<=r.viewportWidth+200&&geometry.y<=r.viewportHeight+200);
                        entry.object->props[L"isIntersecting"]=Value::Bool(intersects);
                        entry.object->props[L"intersectionRatio"]=Value::Number(intersects?1:0);
                        entry.object->props[L"intersectionRect"]=intersects?rectangle:r.ObjectValue(ObjectKind::Plain);
                        entry.object->props[L"rootBounds"]=Value::Null();
                    }
                    r.Call(callback->second,Value::Undefined(),
                           {r.ArrayValue({entry}),Value::FromObject(object)});
                });
                return Value::Undefined();
            });
            if(key==L"unobserve")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Undefined();const auto target=r.Deref(a[0]);
                if(target.type==Value::Type::Object&&target.object&&target.object->kind==ObjectKind::Node)
                    object->observedNodes.erase(std::remove_if(object->observedNodes.begin(),object->observedNodes.end(),
                        [&](const auto& weak){return weak.expired()||weak.lock()==target.object->node;}),object->observedNodes.end());
                return Value::Undefined();
            });
            if(key==L"disconnect")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){object->observedNodes.clear();return Value::Undefined();});
            if(key==L"takeRecords")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.ArrayValue({});});
        }
        if(object->kind==ObjectKind::MediaQuery){
            if(key==L"matches")return object->props[L"$matches"];
            if(key==L"addEventListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.objectListeners[object.get()],a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto found=r.objectListeners.find(object.get());if(found!=r.objectListeners.end())r.RemoveEventListener(found->second,a);return Value::Undefined();});
            if(key==L"addListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(!a.empty())r.AddEventListener(r.objectListeners[object.get()],{Value::String(L"change"),a[0]});return Value::Undefined();});
            if(key==L"removeListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto found=r.objectListeners.find(object.get());if(found!=r.objectListeners.end()&&!a.empty())r.RemoveEventListener(found->second,{Value::String(L"change"),a[0]});return Value::Undefined();});
        }
        if(object->kind==ObjectKind::FileReader){
            if(key==L"addEventListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){r.AddEventListener(r.objectListeners[object.get()],a);return Value::Undefined();});
            if(key==L"removeEventListener")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto found=r.objectListeners.find(object.get());if(found!=r.objectListeners.end())r.RemoveEventListener(found->second,a);return Value::Undefined();});
            if(key==L"readAsDataURL")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto dispatch=[&](const std::wstring& type){auto event=r.CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(type);event->props[L"target"]=Value::FromObject(object);event->props[L"currentTarget"]=Value::FromObject(object);event->props[L"defaultPrevented"]=Value::Bool(false);const auto found=r.objectListeners.find(object.get());if(found!=r.objectListeners.end())r.InvokeEventListeners(found->second,type,false,Value::FromObject(object),Value::FromObject(event),event);};
                if(a.empty()){object->props[L"error"]=r.ErrorValue(L"TypeError",L"FileReader requires a File");dispatch(L"error");return Value::Undefined();}
                const auto file=r.Deref(a[0]);if(file.type!=Value::Type::Object||!file.object||file.object->kind!=ObjectKind::File){object->props[L"error"]=r.ErrorValue(L"TypeError",L"FileReader requires a File");dispatch(L"error");return Value::Undefined();}
                const auto path=r.String(file.object->props[L"$path"]);std::ifstream input(path,std::ios::binary);
                if(!input){object->props[L"error"]=r.ErrorValue(L"NotFoundError",L"File could not be read");dispatch(L"error");return Value::Undefined();}
                std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string encoded;encoded.reserve((bytes.size()+2)/3*4);
                for(size_t index=0;index<bytes.size();index+=3){const unsigned first=static_cast<unsigned char>(bytes[index]);const unsigned second=index+1<bytes.size()?static_cast<unsigned char>(bytes[index+1]):0;const unsigned third=index+2<bytes.size()?static_cast<unsigned char>(bytes[index+2]):0;const unsigned value=(first<<16)|(second<<8)|third;encoded.push_back(alphabet[(value>>18)&63]);encoded.push_back(alphabet[(value>>12)&63]);encoded.push_back(index+1<bytes.size()?alphabet[(value>>6)&63]:'=');encoded.push_back(index+2<bytes.size()?alphabet[value&63]:'=');}
                const auto type=r.String(file.object->props[L"type"]);object->props[L"result"]=Value::String(L"data:"+(type.empty()?std::wstring(L"application/octet-stream"):type)+L";base64,"+Utf8ToWide(encoded));object->props[L"error"]=Value::Null();dispatch(L"load");return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::Clipboard){
            if(key==L"writeText")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto promise=r.PromiseValue();const auto text=a.empty()?L"":r.String(a[0]);bool success=false;if(OpenClipboard(nullptr)){if(EmptyClipboard()){const SIZE_T bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);if(memory){if(void* target=GlobalLock(memory)){std::memcpy(target,text.c_str(),bytes);GlobalUnlock(memory);if(SetClipboardData(CF_UNICODETEXT,memory))success=true;else GlobalFree(memory);}else GlobalFree(memory);}}CloseClipboard();}if(success)r.ResolvePromise(promise.object,Value::Undefined());else r.RejectPromise(promise.object,r.ErrorValue(L"NotAllowedError",L"Clipboard is unavailable"));return promise;});
            if(key==L"readText")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){auto promise=r.PromiseValue();std::wstring text;bool success=false;if(OpenClipboard(nullptr)){if(HANDLE data=GetClipboardData(CF_UNICODETEXT)){if(const auto* value=static_cast<const wchar_t*>(GlobalLock(data))){text=value;GlobalUnlock(data);success=true;}}CloseClipboard();}if(success)r.ResolvePromise(promise.object,Value::String(text));else r.RejectPromise(promise.object,r.ErrorValue(L"NotAllowedError",L"Clipboard is unavailable"));return promise;});
        }
        if(object->kind==ObjectKind::DataTransfer&&key==L"getData")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){const auto type=a.empty()?L"":ToLower(r.String(a[0]));if(type==L"text"||type==L"text/plain"){const auto found=object->props.find(L"$text");return Value::String(found==object->props.end()?L"":r.String(found->second));}return Value::String(L"");});
        if(object->kind==ObjectKind::DataTransferItem&&key==L"getAsFile")return Native([object](RuntimeCore&,const Value&,const std::vector<Value>&){const auto found=object->props.find(L"$file");return found==object->props.end()?Value::Null():found->second;});
        if(object->kind==ObjectKind::UrlSearchParams){
            if(key==L"get")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Null();const auto name=r.String(a[0]);for(const auto& entry:object->entries)if(r.String(entry.first)==name)return entry.second;return Value::Null();});
            if(key==L"has")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Bool(false);const auto name=r.String(a[0]);return Value::Bool(std::any_of(object->entries.begin(),object->entries.end(),[&](const auto& entry){return r.String(entry.first)==name;}));});
            if(key==L"set")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Undefined();const auto name=r.String(a[0]),value=a.size()>1?r.String(a[1]):L"undefined";auto first=object->entries.end();for(auto iterator=object->entries.begin();iterator!=object->entries.end();){if(r.String(iterator->first)!=name){++iterator;continue;}if(first==object->entries.end()){iterator->second=Value::String(value);first=iterator;++iterator;}else iterator=object->entries.erase(iterator);}if(first==object->entries.end())object->entries.push_back({Value::String(name),Value::String(value)});r.SyncUrlSearchParams(object);return Value::Undefined();});
            if(key==L"append")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Undefined();object->entries.push_back({Value::String(r.String(a[0])),Value::String(a.size()>1?r.String(a[1]):L"undefined")});r.SyncUrlSearchParams(object);return Value::Undefined();});
            if(key==L"delete")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Undefined();const auto name=r.String(a[0]);object->entries.erase(std::remove_if(object->entries.begin(),object->entries.end(),[&](const auto& entry){return r.String(entry.first)==name;}),object->entries.end());r.SyncUrlSearchParams(object);return Value::Undefined();});
            if(key==L"toString")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){return Value::String(r.SerializeUrlSearchParams(object));});
        }
        if(object->kind==ObjectKind::Url&&key==L"toString")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){return object->props.count(L"href")?Value::String(r.String(object->props[L"href"])):Value::String(L"");});
        if(object->kind==ObjectKind::Location&&(key==L"replace"||key==L"assign"))return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(!a.empty())r.RequestNavigation(r.String(a[0]));return Value::Undefined();});
        if(object->kind==ObjectKind::Location&&key==L"reload")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){r.RequestNavigation(r.location);return Value::Undefined();});
        if(object->kind==ObjectKind::ReadableStream&&key==L"getReader")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
            auto reader=r.ObjectValue(ObjectKind::ReadableStreamReader);
            const auto body=object->props.find(L"$body");
            reader.object->props[L"$body"]=body==object->props.end()?Value::String(L""):body->second;
            reader.object->props[L"$read"]=Value::Bool(false);
            return reader;
        });
        if(object->kind==ObjectKind::ReadableStreamReader&&key==L"read")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
            auto result=r.ObjectValue(ObjectKind::Plain);
            const auto read=object->props.find(L"$read");
            if(read!=object->props.end()&&r.Truth(read->second)){
                result.object->props[L"done"]=Value::Bool(true);
                result.object->props[L"value"]=Value::Undefined();
                return r.PromiseResolveValue(result);
            }
            object->props[L"$read"]=Value::Bool(true);
            const auto body=object->props.find(L"$body");
            const auto bytes=WideToUtf8(body==object->props.end()?L"":r.String(body->second));
            std::vector<Value> values;values.reserve(bytes.size());
            for(const unsigned char byte:bytes)values.push_back(Value::Number(static_cast<double>(byte)));
            auto chunk=r.ArrayValue(values);
            chunk.object->props[L"$typedArrayName"]=Value::String(L"Uint8Array");
            chunk.object->props[L"$typedArrayBits"]=Value::Number(8);
            chunk.object->props[L"$typedArraySigned"]=Value::Bool(false);
            chunk.object->props[L"$typedArrayClamped"]=Value::Bool(false);
            chunk.object->props[L"$typedArrayFloating"]=Value::Bool(false);
            chunk.object->props[L"BYTES_PER_ELEMENT"]=Value::Number(1);
            chunk.object->props[L"byteLength"]=Value::Number(static_cast<double>(values.size()));
            chunk.object->props[L"byteOffset"]=Value::Number(0);
            auto buffer=r.ObjectValue(ObjectKind::Plain);buffer.object->byteSource=chunk.object;
            buffer.object->props[L"byteLength"]=chunk.object->props[L"byteLength"];
            chunk.object->props[L"buffer"]=buffer;
            result.object->props[L"done"]=Value::Bool(false);
            result.object->props[L"value"]=chunk;
            return r.PromiseResolveValue(result);
        });
        if(object->kind==ObjectKind::ReadableStreamReader&&key==L"cancel")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){object->props[L"$read"]=Value::Bool(true);return r.PromiseResolveValue(Value::Undefined());});
        if(object->kind==ObjectKind::TextDecoder&&key==L"decode")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(a.empty()||r.Deref(a[0]).type==Value::Type::Undefined)return Value::String(L"");
            const auto input=r.Deref(a[0]);
            if(input.type==Value::Type::String)return input;
            if(input.type!=Value::Type::Object||!input.object)
                throw JavaScriptException{r.ErrorValue(L"TypeError",L"TextDecoder input must be an ArrayBuffer or ArrayBufferView")};
            auto source=input.object;
            if(!source->byteSource.expired())source=source->byteSource.lock();
            std::string bytes;bytes.reserve(source?source->items.size():0);
            if(source)for(const auto& item:source->items)bytes.push_back(static_cast<char>(r.Uint32(item)&0xffu));
            return Value::String(Utf8ToWide(bytes));
        });
        if(object->kind==ObjectKind::Response&&key==L"text")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto found=object->props.find(L"$body");return r.PromiseResolveValue(found==object->props.end()?Value::String(L""):Value::String(r.String(found->second)));});
        if(object->kind==ObjectKind::Response&&key==L"json")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){const auto found=object->props.find(L"$body");if(found==object->props.end())return r.PromiseResolveValue(r.ObjectValue(ObjectKind::Plain));try{Compiler parser(r.module,r.String(found->second));return r.PromiseResolveValue(r.Run(parser.CompileExpressionOnly(),r.global));}catch(const JavaScriptException& exception){auto promise=r.PromiseValue();r.RejectPromise(promise.object,exception.value);return promise;}catch(const std::exception& exception){auto promise=r.PromiseValue();r.RejectPromise(promise.object,r.ErrorValue(L"SyntaxError",Utf8ToWide(exception.what())));return promise;}});
        if(object->kind==ObjectKind::XmlHttpRequest){
            if(key==L"open")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                object->props[L"$method"]=Value::String(a.empty()?L"GET":ToLower(r.String(a[0])));
                object->props[L"$url"]=Value::String(a.size()>1?r.String(a[1]):L"");
                object->props[L"$async"]=Value::Bool(a.size()<3||r.Truth(a[2]));
                object->props[L"readyState"]=Value::Number(1);
                const auto callback=object->props.find(L"onreadystatechange");
                if(callback!=object->props.end()&&r.IsCallable(callback->second))
                    r.Call(callback->second,Value::FromObject(object),{});
                return Value::Undefined();
            });
            if(key==L"setRequestHeader")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.size()>1)object->props[L"$request-header:"+ToLower(r.String(a[0]))]=Value::String(r.String(a[1]));
                return Value::Undefined();
            });
            if(key==L"overrideMimeType")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(!a.empty())object->props[L"$mimeType"]=Value::String(r.String(a[0]));
                return Value::Undefined();
            });
            if(key==L"getAllResponseHeaders")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                const auto found=object->props.find(L"$responseHeaders");
                return Value::String(found==object->props.end()?L"":r.String(found->second));
            });
            if(key==L"getResponseHeader")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                if(a.empty())return Value::Null();
                return ToLower(r.String(a[0]))==L"content-type"?
                    Value::String(L"application/json; charset=utf-8"):Value::Null();
            });
            if(key==L"abort")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                object->props[L"$aborted"]=Value::Bool(true);object->props[L"readyState"]=Value::Number(0);
                const auto callback=object->props.find(L"onabort");
                if(callback!=object->props.end()&&r.IsCallable(callback->second))
                    r.Call(callback->second,Value::FromObject(object),{});
                return Value::Undefined();
            });
            if(key==L"send")return Native([object](RuntimeCore& r,const Value&,const std::vector<Value>&){
                object->props[L"$aborted"]=Value::Bool(false);
                const auto resource=r.String(object->props[L"$url"]);
                auto complete=[object](RuntimeCore& runtime,bool loaded,std::wstring body){
                    const auto aborted=object->props.find(L"$aborted");
                    if(aborted!=object->props.end()&&runtime.Truth(aborted->second))return;
                    object->props[L"status"]=Value::Number(loaded?200:404);
                    object->props[L"statusText"]=Value::String(loaded?L"OK":L"Not Found");
                    object->props[L"responseText"]=Value::String(body);
                    const auto responseType=ToLower(runtime.String(object->props[L"responseType"]));
                    if(responseType==L"json"){
                        try{
                            Compiler parser(runtime.module,body);
                            object->props[L"response"]=runtime.Run(
                                parser.CompileExpressionOnly(),runtime.global);
                        }catch(...){object->props[L"response"]=Value::Null();}
                    }else object->props[L"response"]=Value::String(body);
                    object->props[L"responseURL"]=object->props[L"$url"];
                    object->props[L"$responseHeaders"]=Value::String(L"content-type: application/json; charset=utf-8\r\n");
                    object->props[L"readyState"]=Value::Number(4);
                    const auto self=Value::FromObject(object);
                    for(const auto* name:{L"onreadystatechange",loaded?L"onload":L"onerror"}){
                        const auto callback=object->props.find(name);
                        if(callback!=object->props.end()&&runtime.IsCallable(callback->second))
                            runtime.Call(callback->second,self,{});
                    }
                };
                if(r.asyncResourceLoader&&r.Truth(object->props[L"$async"])){
                    auto* runtime=&r;
                    r.asyncResourceLoader(resource,
                        [runtime,complete=std::move(complete)](bool loaded,std::wstring body) mutable {
                            complete(*runtime,loaded,std::move(body));runtime->DrainMicrotasks();
                        });
                }else{
                    std::wstring body;const bool loaded=r.resourceLoader&&r.resourceLoader(resource,body);
                    complete(r,loaded,std::move(body));
                }
                return Value::Undefined();
            });
        }
        if(object->kind==ObjectKind::Performance){
            if(key==L"now")return Native([](RuntimeCore&,const Value&,const std::vector<Value>&){using namespace std::chrono;return Value::Number(duration<double,std::milli>(steady_clock::now().time_since_epoch()).count());});
            if(key==L"getEntries"||key==L"getEntriesByType"||key==L"getEntriesByName")
                return Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.ArrayValue({});});
            if(key==L"clearMarks"||key==L"clearMeasures"||key==L"clearResourceTimings"||key==L"setResourceTimingBufferSize")
                return Native([](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Undefined();});
            if(key==L"mark"||key==L"measure")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                auto entry=r.ObjectValue(ObjectKind::Plain);
                entry.object->props[L"name"]=Value::String(a.empty()?L"":r.String(a[0]));
                entry.object->props[L"entryType"]=Value::String(key==L"mark"?L"mark":L"measure");
                entry.object->props[L"startTime"]=Value::Number(0);entry.object->props[L"duration"]=Value::Number(0);
                return entry;
            });
        }
        if(object->kind==ObjectKind::Math){
            if(key==L"PI")return Value::Number(3.14159265358979323846);
            if(key==L"E")return Value::Number(2.71828182845904523536);
            if(key==L"random")return Native([](RuntimeCore&,const Value&,const std::vector<Value>&){
                return Value::Number(static_cast<double>(std::rand())/
                    (static_cast<double>(RAND_MAX)+1.0));
            });
            if(key==L"trunc")return Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                return Value::Number(std::trunc(a.empty()?0:r.Number(a[0])));
            });
            if(key==L"floor"||key==L"ceil"||key==L"round"||key==L"log"||key==L"pow"||
               key==L"min"||key==L"max"||key==L"abs"||key==L"sqrt"||key==L"sin"||
               key==L"cos"||key==L"tan"||key==L"atan2")return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){
                const double first=a.empty()?0:r.Number(a[0]);
                if(key==L"floor")return Value::Number(std::floor(first));if(key==L"ceil")return Value::Number(std::ceil(first));
                if(key==L"round")return Value::Number(std::round(first));if(key==L"log")return Value::Number(std::log(first));
                if(key==L"abs")return Value::Number(std::abs(first));if(key==L"sqrt")return Value::Number(std::sqrt(first));
                if(key==L"sin")return Value::Number(std::sin(first));if(key==L"cos")return Value::Number(std::cos(first));
                if(key==L"tan")return Value::Number(std::tan(first));
                if(key==L"atan2")return Value::Number(std::atan2(first,a.size()>1?r.Number(a[1]):0));
                if(key==L"pow")return Value::Number(std::pow(first,a.size()>1?r.Number(a[1]):0));
                double number=first;for(size_t i=1;i<a.size();++i)number=key==L"min"?std::min(number,r.Number(a[i])):std::max(number,r.Number(a[i]));return Value::Number(number);
            });
        }
        if(object->kind==ObjectKind::Json&&(key==L"stringify"||key==L"parse"))return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(key==L"stringify")return Value::String(a.empty()?L"undefined":r.Json(a[0]));if(a.empty())return Value::Undefined();try{Compiler parser(r.module,r.String(a[0]));return r.Run(parser.CompileExpressionOnly(),r.global);}catch(...){return Value::Undefined();}});
        if(object->kind==ObjectKind::ArrayConstructor&&(key==L"from"||key==L"isArray"))return Native([key](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(key==L"isArray")return Value::Bool(!a.empty()&&r.Deref(a[0]).type==Value::Type::Object&&r.Deref(a[0]).object->kind==ObjectKind::Array);if(a.empty())return r.ArrayValue({});auto v=r.Deref(a[0]);std::vector<Value> values;if(v.type==Value::Type::Object&&v.object&&v.object->kind==ObjectKind::Array)values=v.object->items;else if(v.type==Value::Type::Object&&v.object){const auto length=v.object->props.find(L"length");const auto count=length==v.object->props.end()?0:static_cast<size_t>(std::max(0.0,r.Number(length->second)));values.resize(count,Value::Undefined());}if(a.size()>1)for(size_t i=0;i<values.size();++i)values[i]=r.Deref(r.Call(a[1],Value::Undefined(),{values[i],Value::Number(static_cast<double>(i))}));return r.ArrayValue(values);});
        if(object->kind==ObjectKind::NumberConstructor&&key==L"MAX_SAFE_INTEGER")
            return Value::Number(9007199254740991.0);
        if(object->kind==ObjectKind::NumberConstructor&&key==L"MIN_SAFE_INTEGER")
            return Value::Number(-9007199254740991.0);
        if(object->kind==ObjectKind::NumberConstructor&&
           (key==L"isFinite"||key==L"isNaN"||key==L"isInteger"||key==L"isSafeInteger"))
            return Native([key](RuntimeCore& r,const Value&,
                                const std::vector<Value>& arguments){
                const auto value=arguments.empty()?Value::Undefined():r.Deref(arguments[0]);
                if(value.type!=Value::Type::Number)return Value::Bool(false);
                if(key==L"isFinite")return Value::Bool(std::isfinite(value.number));
                if(key==L"isNaN")return Value::Bool(std::isnan(value.number));
                const bool integer=std::isfinite(value.number)&&
                    std::trunc(value.number)==value.number;
                return Value::Bool(key==L"isSafeInteger"?
                    integer&&std::abs(value.number)<=9007199254740991.0:integer);
            });
        // Object literals inherit these methods from Object.prototype.  Keep
        // the fallback limited to ordinary objects so Array#toString and other
        // specialized built-ins retain their own behavior.  Libraries such as
        // jQuery use a detached Object#toString with call() to classify values.
        if(object->kind==ObjectKind::Plain&&key==L"toString")
            return Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>&){
                return Value::String(r.ObjectTag(thisValue));
            });
        if(key==L"hasOwnProperty"||key==L"propertyIsEnumerable")
            return Native([key](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& arguments){
                if(arguments.empty())return Value::Bool(false);
                const auto value=r.Deref(thisValue);const auto name=r.String(arguments[0]);
                if(key==L"propertyIsEnumerable")
                    return Value::Bool(r.OwnPropertyIsEnumerable(value,name));
                if(value.type==Value::Type::Function&&value.function)
                    return Value::Bool(value.function->props.count(name)!=0);
                if(value.type==Value::Type::Native&&value.native)
                    return Value::Bool(value.native->props.count(name)!=0);
                if(value.type!=Value::Type::Object||!value.object)return Value::Bool(false);
                if(value.object->kind==ObjectKind::Array){
                    size_t index=0;if(name==L"length")return Value::Bool(true);
                    if(TryParseDecimalIndex(name,index))return Value::Bool(index<value.object->items.size());
                }
                return Value::Bool(value.object->props.count(name)!=0);
            });
        if(object->kind==ObjectKind::Plain&&key==L"isPrototypeOf")
            return Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& arguments){
                const auto prototype=r.Deref(thisValue);
                if(prototype.type!=Value::Type::Object||!prototype.object||arguments.empty())
                    return Value::Bool(false);
                const auto value=r.Deref(arguments[0]);
                if(value.type!=Value::Type::Object||!value.object)return Value::Bool(false);
                for(auto current=value.object->prototype;current;current=current->prototype)
                    if(current==prototype.object)return Value::Bool(true);
                return Value::Bool(false);
            });
        return Value::Undefined();
    }
    struct ParsedEventListenerOptions { bool capture=false,once=false,passive=false; };
    ParsedEventListenerOptions EventListenerOptions(const Value& input){
        ParsedEventListenerOptions options;const auto value=Deref(input);
        if(value.type==Value::Type::Boolean){options.capture=value.boolean;return options;}
        if(value.type!=Value::Type::Object||!value.object)return options;
        options.capture=Truth(GetProperty(value,L"capture"));
        options.once=Truth(GetProperty(value,L"once"));
        options.passive=Truth(GetProperty(value,L"passive"));
        return options;
    }
    bool IsEventCallback(const Value& input){
        const auto callback=Deref(input);if(IsCallable(callback))return true;
        return callback.type==Value::Type::Object&&callback.object&&
            IsCallable(GetProperty(callback,L"handleEvent"));
    }
    void AddEventListener(EventListenerMap& map,const std::vector<Value>& arguments){
        if(arguments.size()<2||!IsEventCallback(arguments[1]))return;
        const auto type=String(arguments[0]);const auto callback=Deref(arguments[1]);
        const auto options=arguments.size()>2?EventListenerOptions(arguments[2]):ParsedEventListenerOptions{};
        auto& entries=map[type];
        for(const auto& entry:entries)
            if(entry.capture==options.capture&&EqualValues(entry.callback,callback))return;
        entries.push_back({nextEventListenerId++,callback,options.capture,options.once,options.passive});
    }
    void RemoveEventListener(EventListenerMap& map,const std::vector<Value>& arguments){
        if(arguments.size()<2)return;const auto found=map.find(String(arguments[0]));if(found==map.end())return;
        const auto callback=Deref(arguments[1]);const bool capture=arguments.size()>2?EventListenerOptions(arguments[2]).capture:false;
        auto& entries=found->second;
        entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const EventListener& entry){return entry.capture==capture&&EqualValues(entry.callback,callback);}),entries.end());
        if(entries.empty())map.erase(String(arguments[0]));
    }
    void CallEventCallback(const Value& callback,const Value& currentTarget,const Value& eventValue){
        const auto value=Deref(callback);
        if(IsCallable(value)){Call(value,currentTarget,{eventValue});return;}
        if(value.type==Value::Type::Object&&value.object){const auto handler=GetProperty(value,L"handleEvent");if(IsCallable(handler))Call(handler,value,{eventValue});}
    }
    void InvokeEventListeners(EventListenerMap& map,const std::wstring& type,bool capture,
                              const Value& currentTarget,const Value& eventValue,
                              const std::shared_ptr<Object>& event){
        const auto immediate=event->props.find(L"$immediateStopped");
        if(immediate!=event->props.end()&&Truth(immediate->second))return;
        const auto found=map.find(type);if(found==map.end())return;
        const auto snapshot=found->second;
        for(const auto& entry:snapshot){
            auto live=map.find(type);if(live==map.end())break;
            const auto position=std::find_if(live->second.begin(),live->second.end(),[&](const EventListener& candidate){return candidate.id==entry.id;});
            if(position==live->second.end()||position->capture!=capture)continue;
            const auto callback=position->callback;const bool passive=position->passive;
            if(position->once){live->second.erase(position);if(live->second.empty())map.erase(type);}
            event->props[L"$inPassiveListener"]=Value::Bool(passive);
            try{CallEventCallback(callback,currentTarget,eventValue);}
            catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);}
            catch(const std::exception& exception){lastError=Utf8ToWide(exception.what());}
            event->props[L"$inPassiveListener"]=Value::Bool(false);
            const auto stopped=event->props.find(L"$immediateStopped");if(stopped!=event->props.end()&&Truth(stopped->second))break;
        }
    }
    std::shared_ptr<Object> GlobalWindowObject()const{
        if(!global)return {};
        const auto found=global->values.find(L"window");
        if(found==global->values.end()||found->second.type!=Value::Type::Object)return {};
        return found->second.object;
    }
    void SetEnvironmentValue(const std::shared_ptr<Environment>& environment,
                             const std::wstring& name,const Value& input,
                             bool ensureWindowProperty=false){
        if(!environment)return;
        const auto value=Deref(input);environment->values[name]=value;
        if(environment!=global)return;
        const auto window=GlobalWindowObject();
        if(window&&(ensureWindowProperty||window->props.count(name)))window->props[name]=value;
    }
    void SetProperty(const Value& input,const std::wstring& key,const Value& value){
        auto base=Deref(input);auto v=Deref(value);
        if(base.type==Value::Type::Function&&base.function){const auto setter=base.function->props.find(L"$set:"+key);if(setter!=base.function->props.end())Call(setter->second,base,{v});else base.function->props[key]=v;return;}
        if(base.type==Value::Type::Native&&base.native){const auto setter=base.native->props.find(L"$set:"+key);if(setter!=base.native->props.end())Call(setter->second,base,{v});else base.native->props[key]=v;return;}
        if(base.type!=Value::Type::Object||!base.object)return;auto object=base.object;
        if(object->kind==ObjectKind::Proxy){
            const auto target=object->props.find(L"$target"),handler=object->props.find(L"$handler");
            if(target==object->props.end())return;
            if(handler!=object->props.end()){
                const auto trap=GetProperty(handler->second,L"set");
                if(IsCallable(trap)){Call(trap,handler->second,{target->second,Value::String(key),v,base});return;}
            }
            SetProperty(target->second,key,v);return;
        }
        const auto ownSetter=object->props.find(L"$set:"+key);
        if(ownSetter!=object->props.end()){Call(ownSetter->second,base,{v});return;}
        for(auto prototype=object->prototype;prototype;prototype=prototype->prototype){
            const auto setter=prototype->props.find(L"$set:"+key);
            if(setter!=prototype->props.end()){Call(setter->second,base,{v});return;}
        }
        if(key==L"__proto__"){
            if(v.type==Value::Type::Object){object->prototype=v.object;object->props.erase(L"$prototypeValue");}
            else if(v.type==Value::Type::Function||v.type==Value::Type::Native){
                object->prototype.reset();object->props[L"$prototypeValue"]=v;
            }else{object->prototype.reset();object->props.erase(L"$prototypeValue");}
            return;
        }
        if(object->kind==ObjectKind::Array){
            if(key==L"length"){
                const double requested=Number(v);
                if(!std::isfinite(requested)||requested<0||std::floor(requested)!=requested||
                   requested>static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
                    throw JavaScriptException{ErrorValue(L"RangeError",L"Invalid array length")};
                object->items.resize(static_cast<size_t>(requested));
                object->props.erase(L"length");return;
            }
            size_t i=0;if(TryParseDecimalIndex(key,i)){if(i>=object->items.size())object->items.resize(i+1);object->items[i]=v;return;}
        }
        if(object->kind==ObjectKind::Location&&key==L"hash"){
            auto hash=String(v);if(!hash.empty()&&hash.front()!=L'#')hash.insert(hash.begin(),L'#');
            const auto found=object->props.find(L"hash");const auto previous=found==object->props.end()?L"":String(found->second);
            if(hash==previous)return;
            auto baseLocation=location;const auto fragment=baseLocation.find(L'#');if(fragment!=std::wstring::npos)baseLocation.resize(fragment);
            location=baseLocation+hash;object->props[L"hash"]=Value::String(hash);object->props[L"href"]=Value::String(location);
            DispatchWindow(L"hashchange");return;
        }
        if(object->kind==ObjectKind::Location&&key==L"href"){
            RequestNavigation(String(v));return;
        }
        if(object->kind==ObjectKind::Window&&key==L"location"){
            RequestNavigation(String(v));return;
        }
        if(object->kind==ObjectKind::Window&&key==L"name")windowName=String(v);
        // Window properties and global `var` bindings are the same storage in
        // classic scripts. Keep an existing binding current when a library
        // publishes or replaces an export through window.name.
        if(object->kind==ObjectKind::Window&&global&&global->values.count(key))
            global->values[key]=v;
        if(object->kind==ObjectKind::FrameWindow&&key==L"name"){
            const auto name=String(v);object->props[key]=Value::String(name);
            if(object->node){
                object->node->frameWindowName=name;
                object->node->frameWindowNameInitialized=true;
                Mutated(object->node,JavaScriptRuntime::MutationKind::Style);
            }
            return;
        }
        if(object->kind==ObjectKind::Node&&object->node){auto n=object->node;bool indexOk=true;
            if(n->tag==L"canvas"&&(key==L"width"||key==L"height")){
                const double number=Number(v);n->SetAttribute(key,std::isfinite(number)&&number>=0?NumberString(std::floor(number)):L"0");
                ResetCanvas(n);Mutated(n,JavaScriptRuntime::MutationKind::Layout);return;
            }
            if(n->tag==L"img"&&(key==L"width"||key==L"height")){
                const double number=Number(v);
                n->SetAttribute(key,std::isfinite(number)&&number>=0?
                    NumberString(std::floor(number)):L"0");
                Mutated(n,JavaScriptRuntime::MutationKind::Layout);return;
            }
            if((n->tag==L"iframe"||n->tag==L"embed"||n->tag==L"object"||
                n->tag==L"video")&&(key==L"width"||key==L"height")){
                const double number=Number(v);
                n->SetAttribute(key,std::isfinite(number)&&number>=0?
                    NumberString(std::floor(number)):L"0");
                Mutated(n,JavaScriptRuntime::MutationKind::Layout);return;
            }
            if(key==L"id"){const auto oldId=n->Attribute(L"id");n->SetAttribute(L"id",String(v));indexOk=document.UpdateElementId(n,oldId,n->Attribute(L"id"));}else if(key==L"className")n->SetAttribute(L"class",String(v));else if(key==L"value"){
                const auto text=String(v);if(n->tag==L"input"&&ToLower(n->Attribute(L"type"))==L"file"&&text.empty())n->files.clear();
                n->SetAttribute(L"value",text);n->selectionStart=std::min(n->selectionStart,text.size());n->selectionEnd=std::min(n->selectionEnd,text.size());
                if(n->selectionStart==n->selectionEnd)n->selectionDirection=L"none";
            }
            else if(key==L"selectionStart"||key==L"selectionEnd"){
                const auto length=n->Attribute(L"value").size();const auto position=std::min(length,static_cast<size_t>(std::max(0.0,Number(v))));
                if(key==L"selectionStart")n->selectionStart=position;else n->selectionEnd=position;
                if(selectionSetter)selectionSetter(n,n->selectionStart,n->selectionEnd);return;
            }
            else if(key==L"selectionDirection"){const auto direction=ToLower(String(v));n->selectionDirection=direction==L"forward"||direction==L"backward"?direction:L"none";return;}
            else if(key==L"title"||key==L"type"||key==L"lang"||key==L"src"||key==L"srcdoc"||key==L"href"||
                    key==L"name"||key==L"placeholder"||key==L"rel"||key==L"alt"||
                    key==L"colSpan"||key==L"rowSpan"||key==L"returnValue")
                {const auto reflected=String(v);
                 n->SetAttribute(key==L"colSpan"?L"colspan":(key==L"rowSpan"?L"rowspan":ToLower(key)),reflected);
                 if(key==L"src"){ResetImageSourceState(n);UpdatePendingImageObject(n);}}
            else if(key==L"draggable")n->SetAttribute(L"draggable",Truth(v)?L"true":L"false");
            else if(key==L"contentEditable")n->SetAttribute(L"contenteditable",String(v));
            else if(key==L"hidden"||key==L"open"||key==L"defer"||key==L"required"){if(Truth(v))n->SetAttribute(ToLower(key),L"");else n->RemoveAttribute(ToLower(key));if(n->tag==L"dialog"&&key==L"open")n->modal=false;Mutated(n,JavaScriptRuntime::MutationKind::Style);return;}
            else if(key==L"scrollTop"){n->scrollTop=std::max(0.0f,static_cast<float>(Number(v)));Mutated(n,JavaScriptRuntime::MutationKind::Paint);return;}
            else if(key==L"scrollLeft"){n->scrollLeft=std::max(0.0f,static_cast<float>(Number(v)));Mutated(n,JavaScriptRuntime::MutationKind::Paint);return;}
            else if(key==L"checked")n->checked=Truth(v);
            else if(key==L"indeterminate")n->indeterminate=Truth(v);
            else if(key==L"disabled")n->disabled=Truth(v);
            else if(key==L"innerText"||key==L"textContent"){for(const auto& child:n->children)indexOk=document.UnindexSubtree(child)&&indexOk;n->SetInnerText(String(v));Mutated(n,JavaScriptRuntime::MutationKind::Tree,!indexOk);return;}else if(key==L"innerHTML"){for(const auto& child:n->children)indexOk=document.UnindexSubtree(child)&&indexOk;document.SetInnerHtml(n,String(v),false);for(const auto& child:n->children)indexOk=document.IndexSubtree(child)&&indexOk;Mutated(n,JavaScriptRuntime::MutationKind::Tree,!indexOk);return;}else object->props[key]=v;
            const bool requiresLayout=key==L"value"||key==L"type"||key==L"lang"||key==L"placeholder"||key==L"src"||key==L"srcdoc"||
                key==L"colSpan"||key==L"rowSpan";
            Mutated(n,requiresLayout?JavaScriptRuntime::MutationKind::Layout:
                JavaScriptRuntime::MutationKind::Style,!indexOk);return;}
        if(object->kind==ObjectKind::Attr&&
           (key==L"value"||key==L"nodeValue"||key==L"textContent")){
            const auto found=object->props.find(L"$name");
            const auto name=found==object->props.end()?L"":String(found->second);
            const auto text=String(v);object->props[L"$value"]=Value::String(text);
            if(object->node&&object->node->attributes.count(name))SetDomAttribute(object->node,name,text);
            return;
        }
        if(object->kind==ObjectKind::Style&&object->node){
            if(key==L"cssText")object->node->SetAttribute(L"style",String(v));
            else{const auto name=CamelToKebab(key),propertyValue=String(v);if(propertyValue.empty()){object->node->inlineStyle.erase(name);object->node->inlineStylePriority.erase(name);}else{object->node->inlineStyle[name]=propertyValue;object->node->inlineStylePriority.erase(name);}SyncInlineStyleAttribute(object->node);}
            Mutated(object->node,JavaScriptRuntime::MutationKind::Style);return;
        }
        if(object->kind==ObjectKind::Dataset&&object->node){object->node->SetAttribute(DatasetAttributeName(key),String(v));Mutated(object->node,JavaScriptRuntime::MutationKind::Style);return;}
        if(object->kind==ObjectKind::CanvasContext2D&&object->node){
            const auto surface=EnsureCanvas(object->node);if(!surface)return;auto& state=surface->state;
            if(key==L"fillStyle"||key==L"strokeStyle"){
                auto& paint=key==L"fillStyle"?state.fillStyle:state.strokeStyle;
                if(v.type==Value::Type::Object&&v.object&&v.object->kind==ObjectKind::CanvasGradient&&v.object->canvasGradient){
                    paint.gradient=v.object->canvasGradient;paint.color.clear();
                }else{paint.color=String(v);paint.gradient.reset();}
            }else if(key==L"lineWidth"){
                const double width=Number(v);if(std::isfinite(width)&&width>0)state.lineWidth=static_cast<float>(width);
            }else if(key==L"font")state.font=String(v);
            else if(key==L"textAlign")state.textAlign=ToLower(Trim(String(v)));
            else if(key==L"textBaseline")state.textBaseline=ToLower(Trim(String(v)));
            else object->props[key]=v;
            return;
        }
        object->props[key]=v;
    }
    void Assign(const Value& reference,const Value& value){
        if(reference.type!=Value::Type::Reference)return;auto ref=reference.reference;
        if(ref->kind==Reference::Kind::Variable){
            const auto owner=ref->env?ref->env->Find(ref->name):nullptr;
            if(owner){
                const auto resolved=Deref(value);owner->values[ref->name]=resolved;
                if(global&&owner==global.get()){
                    const auto window=GlobalWindowObject();
                    if(window&&window->props.count(ref->name))window->props[ref->name]=resolved;
                }
            }else if(global)SetEnvironmentValue(global,ref->name,value,true);
        }else SetProperty(ref->base,ref->name,value);
    }
    bool Delete(const Value& reference){
        if(reference.type!=Value::Type::Reference)return true;const auto ref=reference.reference;
        if(ref->kind==Reference::Kind::Variable){if(ref->env){auto* env=ref->env->Find(ref->name);if(env)env->values.erase(ref->name);}return true;}
        auto base=Deref(ref->base);
        if(base.type==Value::Type::Function&&base.function){base.function->props.erase(ref->name);return true;}
        if(base.type==Value::Type::Native&&base.native){base.native->props.erase(ref->name);return true;}
        if(base.type!=Value::Type::Object||!base.object)return true;
        if(base.object->kind==ObjectKind::Dataset&&base.object->node){base.object->node->RemoveAttribute(DatasetAttributeName(ref->name));Mutated(base.object->node,JavaScriptRuntime::MutationKind::Style);return true;}
        base.object->props.erase(ref->name);return true;
    }
    Value Call(const Value& callable,const Value& explicitThis,const std::vector<Value>& args){
        Value callee=Deref(callable),thisValue=explicitThis;
        if(callable.type==Value::Type::Reference&&callable.reference->kind==Reference::Kind::Property)thisValue=Deref(callable.reference->base);
        else if(callable.type==Value::Type::Reference&&callable.reference->kind==Reference::Kind::Super&&callable.reference->env){
            if(const auto value=callable.reference->env->FindValue(L"this"))thisValue=Deref(*value);
        }
        if(callee.type==Value::Type::Native)return callee.native->callback(*this,thisValue,args);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::Proxy){
            const auto target=callee.object->props.find(L"$target"),handler=callee.object->props.find(L"$handler");
            if(target==callee.object->props.end())throw JavaScriptException{ErrorValue(L"TypeError",L"Proxy target is not callable")};
            if(handler!=callee.object->props.end()){
                const auto trap=GetProperty(handler->second,L"apply");
                if(IsCallable(trap))return Call(trap,handler->second,{target->second,thisValue,ArrayValue(args)});
            }
            return Call(target->second,thisValue,args);
        }
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::ObjectConstructor){
            if(args.empty())return ObjectValue(ObjectKind::Plain);
            const auto value=Deref(args[0]);
            if(value.type==Value::Type::Undefined||value.type==Value::Type::Null)return ObjectValue(ObjectKind::Plain);
            if(value.type==Value::Type::Object||value.type==Value::Type::Function||value.type==Value::Type::Native)return value;
            auto wrapper=ObjectValue(ObjectKind::Plain);wrapper.object->props[L"$primitive"]=value;return wrapper;
        }
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::NumberConstructor)
            return Value::Number(args.empty()?0:Number(args[0]));
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::ArrayConstructor){
            if(args.size()==1){const auto value=Deref(args[0]);if(value.type==Value::Type::Number&&std::isfinite(value.number)&&value.number>=0&&std::floor(value.number)==value.number){std::vector<Value> items(static_cast<size_t>(value.number),Value::Undefined());return ArrayValue(items);}}
            return ArrayValue(args);
        }
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::DateConstructor)return DateValue(args);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::ErrorConstructor){const auto name=callee.object->props.count(L"$name")?String(callee.object->props[L"$name"]):L"Error";return ErrorValue(name,args.empty()?L"":String(args[0]));}
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::PromiseConstructor)throw JavaScriptException{ErrorValue(L"TypeError",L"Promise constructor must be called with new")};
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->props.count(L"$class")){
            const auto constructor=callee.object->props.find(L"$method:constructor");
            if(constructor!=callee.object->props.end())Call(constructor->second,thisValue,args);
            else{
                // Calling a derived class from `super()` still executes its
                // implicit constructor. Walk through constructor-less class
                // layers until the first inherited constructor is found.
                for(auto base=callee.object->prototype;base;base=base->prototype){
                    const auto inherited=base->props.find(L"$method:constructor");
                    if(inherited==base->props.end())continue;
                    Call(inherited->second,thisValue,args);break;
                }
            }
            return Deref(thisValue);
        }
        if(callee.type==Value::Type::Function){
            const auto prototype=callee.function->prototype;
            // Recursive JavaScript calls currently share the native Windows
            // stack. Turn excessive recursion into the ECMAScript RangeError
            // before the process can hit an unrecoverable native stack fault.
            // Real-world minified libraries can legitimately build callback and
            // serializer chains well beyond a few dozen frames. Keep a
            // deterministic guard while allowing ordinary browser workloads to
            // use a browser-like amount of nesting; do not raise the PE stack
            // reservation for every thread in a host process.
            constexpr size_t kMaximumInterpreterCallDepth=104;
            if(callStack.size()>=kMaximumInterpreterCallDepth)
                throw JavaScriptException{ErrorValue(L"RangeError",L"Maximum call stack size exceeded")};
            struct CallStackScope {
                std::vector<CallStackEntry>& stack;
                CallStackScope(std::vector<CallStackEntry>& value,const std::wstring& name):stack(value){
                    stack.push_back({name.empty()?L"<anonymous>":name});
                }
                ~CallStackScope(){stack.pop_back();}
            } callStackScope(callStack,prototype->name);
            // Sloppy functions substitute Window for a nullish receiver;
            // strict functions preserve the caller's exact this value. Arrow
            // functions keep lexical this through the environment chain below.
            const auto dereferencedThis=Deref(thisValue);
            if(!prototype->lexicalThis&&!prototype->isStrict&&
               (dereferencedThis.type==Value::Type::Undefined||
                dereferencedThis.type==Value::Type::Null)){
                if(global){
                    const auto window=global->values.find(L"window");
                    if(window!=global->values.end())thisValue=window->second;
                }
            }
            if(!prototype->isGenerator&&jitCompilationThreshold&&prototype&&!prototype->jitCompilationAttempted&&
               ++prototype->jitCallCount>=jitCompilationThreshold){
                prototype->jitCompilationAttempted=true;auto compiled=CompileBaselineJit(*prototype);
                if(compiled&&compiled->allocationSize<=jitCodeBudget-jitAllocatedCodeBytes){
                    prototype->jitCode=std::move(compiled);++jitStatistics.compiledFunctions;
                    jitStatistics.generatedCodeBytes+=prototype->jitCode->size;
                    jitAllocatedCodeBytes+=prototype->jitCode->allocationSize;
                }else ++jitStatistics.unsupportedFunctions;
            }
            if(!prototype->isGenerator&&jitCompilationThreshold&&prototype&&prototype->jitCode){
                // Generated numeric code is a leaf and cannot re-enter the VM,
                // so one scratch frame per runtime thread avoids per-call heap
                // allocation and does not inflate every interpreter call frame.
                static thread_local BaselineJitFrame frame;
                bool argumentsAreNumeric=args.size()>=prototype->parameters.size();
                for(size_t index=0;argumentsAreNumeric&&index<prototype->parameters.size();++index){
                    const auto value=Deref(args[index]);
                    if(value.type!=Value::Type::Number)argumentsAreNumeric=false;
                    else frame.locals[index]=value.number;
                }
                if(argumentsAreNumeric){
                    prototype->jitCode->entry(&frame);++jitStatistics.nativeCalls;
                    return frame.resultKind?Value::Bool(frame.result!=0):Value::Number(frame.result);
                }
                ++jitStatistics.guardFallbacks;
            }
            auto env=CreateEnvironment();env->parent=callee.function->closure;
            env->values.reserve(prototype->parameters.size()+3);
            try{
                if(!prototype->lexicalThis)env->values[L"this"]=thisValue;
                if(!prototype->selfBindingName.empty())
                    env->values[prototype->selfBindingName]=callee;
                for(size_t i=0;i<prototype->parameters.size();++i){auto value=i<args.size()?Deref(args[i]):Value::Undefined();env->values[prototype->parameters[i]]=value;if(value.type==Value::Type::Undefined&&i<prototype->parameterDefaults.size()&&prototype->parameterDefaults[i])env->values[prototype->parameters[i]]=Run(*prototype->parameterDefaults[i],env,true);}
                if(!prototype->lexicalThis)env->values[L"arguments"]=ArrayValue(args);
                if(!prototype->restParameter.empty()){
                    std::vector<Value> rest;
                    if(args.size()>prototype->parameters.size())rest.assign(args.begin()+prototype->parameters.size(),args.end());
                    env->values[prototype->restParameter]=ArrayValue(rest);
                }
                for(const auto& binding:prototype->bindings){
                    auto value=GetProperty(env->values[binding.parameter],binding.property);
                    if(Deref(value).type==Value::Type::Undefined&&binding.defaultValue)
                        value=Run(*binding.defaultValue,env,true);
                    env->values[binding.name]=Deref(value);
                }
                if(prototype->isAsync)return StartAsyncFunction(prototype,env);
                if(prototype->isGenerator)return StartGeneratorFunction(prototype,env);
                return Run(prototype->chunk,env,true);
            }catch(const JavaScriptException& exception){if(!prototype->isAsync)throw;auto promise=PromiseValue();RejectPromise(promise.object,exception.value);return promise;}
            catch(const std::exception& exception){if(!prototype->isAsync)throw;auto promise=PromiseValue();RejectPromise(promise.object,ErrorValue(L"Error",Utf8ToWide(exception.what())));return promise;}
        }
        const auto label=callable.type==Value::Type::Reference&&callable.reference?
            callable.reference->name:String(callee);
        throw JavaScriptException{ErrorValue(L"TypeError",label+L" is not a function")};
    }
    Value Construct(const Value& constructor,const std::vector<Value>& args,
                    const Value* newTarget=nullptr){
        const auto callee=Deref(constructor);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::Proxy){
            const auto target=callee.object->props.find(L"$target"),handler=callee.object->props.find(L"$handler");
            if(target==callee.object->props.end())throw JavaScriptException{ErrorValue(L"TypeError",L"Proxy target is not a constructor")};
            if(handler!=callee.object->props.end()){
                const auto trap=GetProperty(handler->second,L"construct");
                if(IsCallable(trap))return Deref(Call(trap,handler->second,
                    {target->second,ArrayValue(args),newTarget?Deref(*newTarget):callee}));
            }
            return Construct(target->second,args,newTarget);
        }
        if(callee.type==Value::Type::Native){
            if(!newTarget){const auto result=Call(callee,Value::Undefined(),args);return Deref(result);}
            auto instance=ObjectValue(ObjectKind::Plain);
            const auto prototype=GetProperty(*newTarget,L"prototype");
            if(prototype.type==Value::Type::Object)instance.object->prototype=prototype.object;
            else if(prototype.type==Value::Type::Function||prototype.type==Value::Type::Native)
                instance.object->props[L"$prototypeValue"]=prototype;
            const auto result=Deref(Call(callee,instance,args));
            if(result.type==Value::Type::Object||result.type==Value::Type::Function||
               result.type==Value::Type::Native)return result;
            instance.object->props[L"$primitive"]=result;
            return instance;
        }
        if(callee.type==Value::Type::Object&&callee.object&&
           (callee.object->kind==ObjectKind::ObjectConstructor||
            callee.object->kind==ObjectKind::ArrayConstructor||
            callee.object->kind==ObjectKind::NumberConstructor))
            return Call(callee,Value::Undefined(),args);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::DateConstructor)return DateValue(args);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::PromiseConstructor)return ConstructPromise(args.empty()?Value::Undefined():args[0]);
        if(callee.type==Value::Type::Object&&callee.object&&callee.object->kind==ObjectKind::ErrorConstructor){
            const auto name=callee.object->props.count(L"$name")?String(callee.object->props[L"$name"]):L"Error";
            auto error=ErrorValue(name,args.empty()?L"":String(args[0]));
            if(newTarget){
                const auto prototype=GetProperty(*newTarget,L"prototype");
                if(prototype.type==Value::Type::Object)error.object->prototype=prototype.object;
                else if(prototype.type==Value::Type::Function||prototype.type==Value::Type::Native)
                    error.object->props[L"$prototypeValue"]=prototype;
            }
            return error;
        }
        auto instance=ObjectValue(ObjectKind::Plain);
        if(callee.type==Value::Type::Object&&callee.object){
            instance.object->prototype=callee.object;
            if(callee.object->props.count(L"$class")){
                std::vector<std::shared_ptr<Object>> hierarchy;
                for(auto current=callee.object;current&&current->props.count(L"$class");current=current->prototype)
                    hierarchy.push_back(current);
                for(auto current=hierarchy.rbegin();current!=hierarchy.rend();++current)
                    for(const auto& property:(*current)->props){
                        constexpr std::wstring_view prefix=L"$field:";
                        if(property.first.compare(0,prefix.size(),prefix)!=0)continue;
                        const auto name=property.first.substr(prefix.size());
                        instance.object->props[name]=IsCallable(property.second)?
                            Deref(Call(property.second,instance,{})):Deref(property.second);
                    }
            }
            const auto found=callee.object->props.find(L"$method:constructor");
            if(found!=callee.object->props.end())Call(found->second,instance,args);
            else{
                // A derived class without an explicit constructor has the
                // implicit `constructor(...args) { super(...args); }`.
                for(auto base=callee.object->prototype;base;base=base->prototype){
                    const auto inherited=base->props.find(L"$method:constructor");
                    if(inherited==base->props.end())continue;
                    Call(inherited->second,instance,args);break;
                }
            }
            return instance;
        }
        if(callee.type==Value::Type::Function){
            const auto instancePrototype=newTarget?GetProperty(*newTarget,L"prototype"):
                                                   FunctionPrototype(callee.function);
            if(instancePrototype.type==Value::Type::Object)instance.object->prototype=instancePrototype.object;
            else if(instancePrototype.type==Value::Type::Function||instancePrototype.type==Value::Type::Native)
                instance.object->props[L"$prototypeValue"]=instancePrototype;
            const auto result=Deref(Call(callee,instance,args));
            return result.type==Value::Type::Object||result.type==Value::Type::Function||
                   result.type==Value::Type::Native?result:instance;
        }
        return instance;
    }
    bool EqualValues(const Value& a,const Value& b){auto x=Deref(a),y=Deref(b);if(x.type!=y.type)return false;switch(x.type){case Value::Type::Undefined:case Value::Type::Null:return true;case Value::Type::Boolean:return x.boolean==y.boolean;case Value::Type::Number:return x.number==y.number;case Value::Type::String:return x.string==y.string;case Value::Type::Object:return x.object==y.object;case Value::Type::Function:return x.function==y.function;case Value::Type::Native:return x.native==y.native;default:return false;}}
    bool LooseEqualValues(const Value& a,const Value& b){
        auto x=Deref(a),y=Deref(b);
        if(x.type==y.type)return EqualValues(x,y);
        const auto nullish=[](const Value& value){return value.type==Value::Type::Undefined||value.type==Value::Type::Null;};
        if(nullish(x)||nullish(y))return nullish(x)&&nullish(y);
        if(x.type==Value::Type::Boolean)return LooseEqualValues(Value::Number(x.boolean?1:0),y);
        if(y.type==Value::Type::Boolean)return LooseEqualValues(x,Value::Number(y.boolean?1:0));
        if((x.type==Value::Type::Number&&y.type==Value::Type::String)||
           (x.type==Value::Type::String&&y.type==Value::Type::Number))
            return Number(x)==Number(y);
        const auto objectLike=[](const Value& value){return value.type==Value::Type::Object||
            value.type==Value::Type::Function||value.type==Value::Type::Native;};
        const auto primitive=[](const Value& value){return value.type==Value::Type::String||
            value.type==Value::Type::Number||value.type==Value::Type::Boolean;};
        if(objectLike(x)&&primitive(y))return LooseEqualValues(PrimitiveForAddition(x),y);
        if(primitive(x)&&objectLike(y))return LooseEqualValues(x,PrimitiveForAddition(y));
        return false;
    }
    std::wstring Json(const Value& input){
        auto v=Deref(input);if(v.type==Value::Type::Undefined)return L"undefined";if(v.type==Value::Type::Null)return L"null";if(v.type==Value::Type::Boolean)return v.boolean?L"true":L"false";if(v.type==Value::Type::Number)return NumberString(v.number);
        if(v.type==Value::Type::String){std::wstring o=L"\"";for(wchar_t c:v.string){if(c==L'\\'||c==L'"')o+=L'\\';if(c==L'\n')o+=L"\\n";else o+=c;}return o+L"\"";}
        if(v.type==Value::Type::Object&&v.object){if(v.object->kind==ObjectKind::Array){std::wstring o=L"[";for(size_t i=0;i<v.object->items.size();++i){if(i)o+=L",";auto item=Json(v.object->items[i]);o+=item==L"undefined"?L"null":item;}return o+L"]";}std::wstring o=L"{";bool first=true;for(auto& p:v.object->props){if(IsInternalObjectProperty(p.first)||!OwnPropertyIsEnumerable(v,p.first))continue;const auto encoded=Json(p.second);if(encoded==L"undefined")continue;if(!first)o+=L",";first=false;o+=Json(Value::String(p.first))+L":"+encoded;}return o+L"}";}return L"undefined";
    }
    std::wstring TypeName(const Value& input){auto v=Deref(input);switch(v.type){case Value::Type::Undefined:return L"undefined";case Value::Type::Boolean:return L"boolean";case Value::Type::Number:return L"number";case Value::Type::String:return L"string";case Value::Type::Function:case Value::Type::Native:return L"function";case Value::Type::Object:if(v.object&&(v.object->kind==ObjectKind::ObjectConstructor||v.object->kind==ObjectKind::ArrayConstructor||v.object->kind==ObjectKind::StringConstructor||v.object->kind==ObjectKind::PromiseConstructor||v.object->kind==ObjectKind::ErrorConstructor||v.object->kind==ObjectKind::NumberConstructor||v.object->kind==ObjectKind::DateConstructor||v.object->kind==ObjectKind::Proxy&&IsCallable(v)))return L"function";return L"object";default:return L"object";}}
    std::shared_ptr<TemplateProgram> GetTemplateProgram(const std::wstring& raw){
        auto found=templatePrograms.find(raw);
        if(found==templatePrograms.end()){
            auto program=std::make_shared<TemplateProgram>();size_t position=0;
            while(position<raw.size()){
                const auto begin=raw.find(L"${",position);
                if(begin==std::wstring::npos){program->literals.push_back(raw.substr(position));position=raw.size();break;}
                program->literals.push_back(raw.substr(position,begin-position));
                int depth=1;size_t end=begin+2;wchar_t quote=0;
                for(;end<raw.size()&&depth;++end){const auto c=raw[end];if(quote){if(c==quote&&raw[end-1]!=L'\\')quote=0;}else if(c==L'\''||c==L'"'||c==L'`')quote=c;else if(c==L'{')++depth;else if(c==L'}')--depth;}
                if(depth){program->literals.back()+=raw.substr(begin);position=raw.size();break;}
                Compiler compiler(module,raw.substr(begin+2,end-begin-3));
                program->expressions.push_back(compiler.CompileExpressionOnly());position=end;
            }
            if(program->literals.size()==program->expressions.size())program->literals.emplace_back();
            if(templatePrograms.size()>=256)templatePrograms.clear();
            found=templatePrograms.emplace(raw,program).first;
        }
        return found->second;
    }
    Value EvaluateTemplate(const std::wstring& raw,const std::shared_ptr<Environment>& env){
        const auto programValue=GetTemplateProgram(raw);const auto& program=*programValue;std::wstring out;size_t reserve=0;
        for(const auto& literal:program.literals)reserve+=literal.size();out.reserve(reserve+32*program.expressions.size());
        for(size_t index=0;index<program.expressions.size();++index){
            out+=program.literals[index];out+=String(Run(program.expressions[index],env));
        }
        if(!program.literals.empty())out+=program.literals.back();
        return Value::String(out);
    }
    Value EvaluateTaggedTemplate(const Value& tag,const std::wstring& raw,const std::shared_ptr<Environment>& env){
        const auto program=GetTemplateProgram(raw);std::vector<Value> literalValues;literalValues.reserve(program->literals.size());
        for(const auto& literal:program->literals)literalValues.push_back(Value::String(literal));
        auto strings=ArrayValue(literalValues),rawStrings=ArrayValue(literalValues);strings.object->props[L"raw"]=rawStrings;
        std::vector<Value> arguments;arguments.reserve(program->expressions.size()+1);arguments.push_back(strings);
        for(const auto& expression:program->expressions)arguments.push_back(Deref(Run(expression,env)));
        return Call(tag,Value::Undefined(),arguments);
    }
    static bool InRange(size_t position,size_t begin,size_t end){return position>=begin&&position<end;}
    size_t BlockDepthAt(const std::shared_ptr<ExecutionFrame>& frame,size_t target)const{
        return target<frame->chunk->code.size()?
            static_cast<size_t>(std::max(0,frame->chunk->code[target].blockDepth)):0;
    }
    void UnwindBlocks(const std::shared_ptr<ExecutionFrame>& frame,size_t depth){
        while(frame->blockOuters.size()>depth){
            frame->env=frame->blockOuters.back();frame->blockOuters.pop_back();
        }
    }
    void JumpFrame(const std::shared_ptr<ExecutionFrame>& frame,size_t target){
        UnwindBlocks(frame,BlockDepthAt(frame,target));frame->ip=target;
    }
    void CancelPendingFinally(const std::shared_ptr<ExecutionFrame>& frame,size_t origin,
                              size_t localJumpTarget=(std::numeric_limits<size_t>::max)()){
        frame->pending.erase(std::remove_if(frame->pending.begin(),frame->pending.end(),[&](const PendingCompletion& completion){
            const auto& handler=frame->chunk->handlers[completion.handler];
            if(!handler.hasFinally||!InRange(origin,handler.finallyStart,handler.finallyEnd))return false;
            return localJumpTarget==(std::numeric_limits<size_t>::max)()||
                   !InRange(localJumpTarget,handler.finallyStart,handler.finallyEnd);
        }),frame->pending.end());
    }
    void LeaveCatchScope(const std::shared_ptr<ExecutionFrame>& frame,int handlerIndex){
        const auto found=std::find_if(frame->catchScopes.rbegin(),frame->catchScopes.rend(),[&](const ExecutionFrame::CatchScope& scope){return scope.handler==handlerIndex;});
        if(found==frame->catchScopes.rend())return;frame->env=found->outer;frame->catchScopes.erase(std::next(found).base());
    }
    bool DispatchException(const std::shared_ptr<ExecutionFrame>& frame,const Value& thrown,size_t origin){
        CancelPendingFinally(frame,origin);
        for(int index=static_cast<int>(frame->chunk->handlers.size())-1;index>=0;--index){const auto& handler=frame->chunk->handlers[static_cast<size_t>(index)];
            if(InRange(origin,handler.tryStart,handler.tryEnd)){
                frame->stack.clear();
                if(handler.hasCatch){JumpFrame(frame,handler.catchStart);auto catchEnvironment=CreateEnvironment();catchEnvironment->parent=frame->env;if(!handler.catchName.empty())catchEnvironment->values[handler.catchName]=Deref(thrown);frame->catchScopes.push_back({index,frame->env});frame->env=catchEnvironment;return true;}
                if(handler.hasFinally){frame->pending.push_back({index,CompletionKind::Throw,Deref(thrown),0});JumpFrame(frame,handler.finallyStart);return true;}
            }
            if(handler.hasCatch&&InRange(origin,handler.catchStart,handler.catchEnd)){frame->stack.clear();UnwindBlocks(frame,BlockDepthAt(frame,handler.catchStart));LeaveCatchScope(frame,index);if(handler.hasFinally){frame->pending.push_back({index,CompletionKind::Throw,Deref(thrown),0});JumpFrame(frame,handler.finallyStart);return true;}}
        }
        return false;
    }
    bool BeginAbrupt(const std::shared_ptr<ExecutionFrame>& frame,CompletionKind kind,const Value& value,size_t target,size_t origin){
        CancelPendingFinally(frame,origin,kind==CompletionKind::Jump?target:
            (std::numeric_limits<size_t>::max)());
        for(int index=static_cast<int>(frame->chunk->handlers.size())-1;index>=0;--index){const auto& handler=frame->chunk->handlers[static_cast<size_t>(index)];
            const bool inTry=InRange(origin,handler.tryStart,handler.tryEnd),inCatch=handler.hasCatch&&InRange(origin,handler.catchStart,handler.catchEnd);if(!inTry&&!inCatch)continue;
            if(kind==CompletionKind::Jump){const bool stays=inTry?InRange(target,handler.tryStart,handler.tryEnd):InRange(target,handler.catchStart,handler.catchEnd);if(stays)continue;if(target==handler.finallyStart)return false;}
            if(inCatch){UnwindBlocks(frame,BlockDepthAt(frame,handler.catchStart));LeaveCatchScope(frame,index);}if(!handler.hasFinally)continue;
            frame->pending.push_back({index,kind,Deref(value),target});JumpFrame(frame,handler.finallyStart);return true;
        }
        return false;
    }
    void ResumeAsync(const std::shared_ptr<ExecutionFrame>& frame,bool rejected,const Value& value){
        if(!frame||!frame->asyncPromise||frame->asyncPromise->promiseState!=PromiseState::Pending)return;
        if(rejected){frame->resumeThrow=true;frame->resumeValue=Deref(value);}else frame->stack.push_back(Deref(value));
        try{const auto result=RunFrame(frame);if(!result.suspended)ResolvePromise(frame->asyncPromise,result.value);}
        catch(const JavaScriptException& exception){RejectPromise(frame->asyncPromise,exception.value);}
        catch(const std::exception& exception){RejectPromise(frame->asyncPromise,ErrorValue(L"Error",Utf8ToWide(exception.what())));}
    }
    FrameResult RunFrame(const std::shared_ptr<ExecutionFrame>& frame){
        struct ExecutionFrameScope {
            size_t& count;
            explicit ExecutionFrameScope(size_t& value):count(value){++count;}
            ~ExecutionFrameScope(){--count;}
        } executionFrameScope(activeExecutionFrames);
        for(const auto& constant:frame->chunk->constants)TrackValue(constant);
        auto& stack=frame->stack;auto pop=[&](){if(stack.empty())return Value::Undefined();auto value=std::move(stack.back());stack.pop_back();return value;};
        if(frame->resumeThrow){frame->resumeThrow=false;const auto thrown=frame->resumeValue;if(!DispatchException(frame,thrown,frame->resumeOrigin))throw JavaScriptException{thrown};}
        while(frame->ip<frame->chunk->code.size()){
            const size_t current=frame->ip++;const auto& ins=frame->chunk->code[current];
            ++executedInstructions;
            if(executedInstructions>=cycleCollectionNext){
                cycleCollectionPending=true;
                // Avoid testing the expensive safe-point condition on every
                // instruction until the current JavaScript job returns.
                cycleCollectionNext=executedInstructions+1000000;
            }
            if(frame->callStackIndex<callStack.size()){
                callStack[frame->callStackIndex].line=ins.line;
                callStack[frame->callStackIndex].offset=ins.offset;
            }
            try{switch(ins.op){
            case Op::Constant:stack.push_back(frame->chunk->constants[ins.argument]);break;case Op::Undefined:stack.push_back(Value::Undefined());break;case Op::Null:stack.push_back(Value::Null());break;case Op::TrueValue:stack.push_back(Value::Bool(true));break;case Op::FalseValue:stack.push_back(Value::Bool(false));break;
            case Op::BeginBlock:{auto environment=CreateEnvironment();environment->parent=frame->env;frame->blockOuters.push_back(frame->env);frame->env=environment;break;}
            case Op::EndBlock:if(!frame->blockOuters.empty()){frame->env=frame->blockOuters.back();frame->blockOuters.pop_back();}break;
            case Op::LoadReference:{auto reference=CreateReference();reference->env=frame->env;reference->name=ins.text;stack.push_back(Value::FromReference(reference));break;}
            case Op::LoadSuperReference:{auto reference=CreateReference();reference->kind=Reference::Kind::Super;reference->env=frame->env;reference->name=ins.text;stack.push_back(Value::FromReference(reference));break;}
            case Op::Declare:{
                auto value=Deref(pop());
                if(value.type==Value::Type::Function&&value.function&&value.function->prototype&&
                   value.function->prototype->name.empty())value.function->prototype->name=ins.text;
                if(ins.argument!=0){
                    if(auto* owner=frame->env->Find(ins.text)){
                        owner->values[ins.text]=value;
                        if(global&&owner==global.get()){
                            const auto window=GlobalWindowObject();
                            if(window)window->props[ins.text]=value;
                        }
                    }
                    else frame->env->values[ins.text]=value;
                }else frame->env->values[ins.text]=value;
                break;
            }
            case Op::GetProperty:{auto base=pop();auto reference=CreateReference();reference->kind=Reference::Kind::Property;reference->base=base;reference->name=ins.text;stack.push_back(Value::FromReference(reference));break;}
            case Op::GetIndex:{auto key=String(pop());auto base=pop();auto reference=CreateReference();reference->kind=Reference::Kind::Property;reference->base=base;reference->name=key;stack.push_back(Value::FromReference(reference));break;}
            case Op::Assign:{auto value=Deref(pop());auto reference=pop();Assign(reference,value);stack.push_back(value);break;}
            case Op::EvaluateEmbeddedExpression:{
                if(ins.argument>=0&&static_cast<size_t>(ins.argument)<frame->chunk->embeddedExpressions.size())
                    stack.push_back(Deref(Run(*frame->chunk->embeddedExpressions[static_cast<size_t>(ins.argument)],frame->env,true)));
                else stack.push_back(Value::Undefined());
                break;
            }
            case Op::DefaultIfUndefined:{
                if(Deref(stack.back()).type==Value::Type::Undefined){
                    pop();
                    if(ins.argument>=0&&static_cast<size_t>(ins.argument)<frame->chunk->embeddedExpressions.size())
                        stack.push_back(Deref(Run(*frame->chunk->embeddedExpressions[static_cast<size_t>(ins.argument)],frame->env,true)));
                    else stack.push_back(Value::Undefined());
                }
                break;
            }
            case Op::PostIncrement:case Op::PostDecrement:case Op::PreIncrement:case Op::PreDecrement:{auto reference=pop();const auto old=Deref(reference);const double delta=(ins.op==Op::PostIncrement||ins.op==Op::PreIncrement)?1.0:-1.0;const auto next=Value::Number(Number(old)+delta);Assign(reference,next);stack.push_back(ins.op==Op::PostIncrement||ins.op==Op::PostDecrement?old:next);break;}
            case Op::NewArray:stack.push_back(ArrayValue({}));break;case Op::ArrayPush:{auto value=Deref(pop());auto array=Deref(stack.back());array.object->items.push_back(value);break;}
            case Op::ArraySpread:{auto source=Deref(pop());auto array=Deref(stack.back());if(source.type==Value::Type::Object&&source.object&&array.type==Value::Type::Object&&array.object){if(source.object->kind==ObjectKind::Array)array.object->items.insert(array.object->items.end(),source.object->items.begin(),source.object->items.end());else if(source.object->kind==ObjectKind::Map||source.object->kind==ObjectKind::Set)for(const auto& entry:source.object->entries)array.object->items.push_back(source.object->kind==ObjectKind::Map?entry.second:entry.first);}break;}
            case Op::NewObject:stack.push_back(ObjectValue(ObjectKind::Plain));break;case Op::ObjectSet:{auto value=Deref(pop());auto object=Deref(stack.back());object.object->props[ins.text]=value;break;}
            case Op::ObjectSetComputed:{auto value=Deref(pop());const auto key=String(pop());auto object=Deref(stack.back());object.object->props[key]=value;break;}
            case Op::ObjectSetPrototype:{
                auto object=Deref(pop()),base=Deref(pop());
                if(object.type==Value::Type::Object&&object.object){
                    if(base.type==Value::Type::Object&&base.object)object.object->prototype=base.object;
                    else if(base.type==Value::Type::Function&&base.function){
                        const auto prototype=FunctionPrototype(base.function);
                        if(prototype.type==Value::Type::Object)object.object->prototype=prototype.object;
                    }
                }
                stack.push_back(object);break;
            }
            case Op::ObjectSpread:{auto source=Deref(pop());auto target=Deref(stack.back());if(source.type==Value::Type::Object&&source.object&&target.type==Value::Type::Object&&target.object)for(const auto& property:source.object->props)if(!IsInternalObjectProperty(property.first)&&OwnPropertyIsEnumerable(source,property.first))target.object->props[property.first]=Deref(property.second);break;}
            case Op::EnumerableKeys:{
                const auto source=Deref(pop());std::vector<Value> keys;
                if(source.type==Value::Type::String){for(size_t index=0;index<source.string.size();++index)keys.push_back(Value::String(std::to_wstring(index)));}
                else if(source.type==Value::Type::Function&&source.function){
                    for(const auto& property:source.function->props)
                        if(OwnPropertyIsEnumerable(source,property.first))keys.push_back(Value::String(property.first));
                }else if(source.type==Value::Type::Native&&source.native){
                    for(const auto& property:source.native->props)
                        if(OwnPropertyIsEnumerable(source,property.first))keys.push_back(Value::String(property.first));
                }else if(source.type==Value::Type::Object&&source.object){
                    if(source.object->kind==ObjectKind::Array)for(size_t index=0;index<source.object->items.size();++index)keys.push_back(Value::String(std::to_wstring(index)));
                    for(const auto& property:source.object->props){
                        if(!IsInternalObjectProperty(property.first)&&OwnPropertyIsEnumerable(source,property.first))
                            keys.push_back(Value::String(property.first));
                        else if(property.first.rfind(L"$get:",0)==0)
                            {const auto publicName=property.first.substr(5);
                             if(OwnPropertyIsEnumerable(source,publicName))keys.push_back(Value::String(publicName));}
                    }
                }
                stack.push_back(ArrayValue(keys));break;
            }
            case Op::MakeFunction:{
                if(ins.argument<0||static_cast<size_t>(ins.argument)>=module->prototypes.size()||
                   !module->prototypes[static_cast<size_t>(ins.argument)])
                    throw JavaScriptException{ErrorValue(L"TypeError",L"Function bytecode is no longer available")};
                auto function=CreateFunction();
                function->prototype=module->prototypes[static_cast<size_t>(ins.argument)];
                function->closure=frame->env;stack.push_back(Value::FromFunction(function));break;
            }
            case Op::Call:{std::vector<Value> args(ins.argument);for(int i=ins.argument-1;i>=0;--i)args[i]=Deref(pop());auto callee=pop();stack.push_back(Call(callee,Value::Undefined(),args));break;}
            case Op::CallArray:{auto arguments=Deref(pop());auto callee=pop();stack.push_back(Call(callee,Value::Undefined(),arguments.type==Value::Type::Object&&arguments.object?arguments.object->items:std::vector<Value>{}));break;}
            case Op::Construct:{std::vector<Value> args(ins.argument);for(int i=ins.argument-1;i>=0;--i)args[i]=Deref(pop());auto constructor=pop();stack.push_back(Construct(constructor,args));break;}
            case Op::ConstructArray:{auto arguments=Deref(pop());auto constructor=pop();stack.push_back(Construct(constructor,arguments.type==Value::Type::Object&&arguments.object?arguments.object->items:std::vector<Value>{}));break;}
            case Op::DeleteValue:stack.push_back(Value::Bool(Delete(pop())));break;
            case Op::ThrowValue:throw JavaScriptException{Deref(pop())};
            case Op::Await:{const auto awaited=PromiseResolveValue(pop());const auto origin=current;frame->resumeOrigin=origin;auto fulfilled=Native([frame](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){runtime.ResumeAsync(frame,false,values.empty()?Value::Undefined():values[0]);return Value::Undefined();});auto rejected=Native([frame](RuntimeCore& runtime,const Value&,const std::vector<Value>& values){runtime.ResumeAsync(frame,true,values.empty()?Value::Undefined():values[0]);return Value::Undefined();});PerformThen(awaited.object,fulfilled,rejected);return {true,Value::Undefined()};}
            case Op::Yield:{
                const auto value=Deref(pop());
                if(frame->generatorValues)frame->generatorValues->push_back(value);
                stack.push_back(Value::Undefined());break;
            }
            case Op::YieldSpread:{
                const auto source=Deref(pop());
                if(frame->generatorValues){
                    if(source.type==Value::Type::String){
                        for(const auto character:source.string)
                            frame->generatorValues->push_back(Value::String(std::wstring(1,character)));
                    }else if(source.type==Value::Type::Object&&source.object){
                        if(source.object->kind==ObjectKind::Array)
                            frame->generatorValues->insert(frame->generatorValues->end(),
                                source.object->items.begin(),source.object->items.end());
                        else if(source.object->kind==ObjectKind::Map)
                            for(const auto& entry:source.object->entries)
                                frame->generatorValues->push_back(ArrayValue({entry.first,entry.second}));
                        else if(source.object->kind==ObjectKind::Set)
                            for(const auto& entry:source.object->entries)
                                frame->generatorValues->push_back(entry.first);
                        else{
                            const auto length=Number(GetProperty(source,L"length"));
                            if(std::isfinite(length)&&length>0)
                                for(size_t index=0;index<static_cast<size_t>(std::min(length,1000000.0));++index)
                                    frame->generatorValues->push_back(
                                        Deref(GetProperty(source,std::to_wstring(index))));
                        }
                    }
                }
                stack.push_back(Value::Undefined());break;
            }
            case Op::LeaveCatch:LeaveCatchScope(frame,ins.argument);break;
            case Op::EndFinally:{auto found=std::find_if(frame->pending.rbegin(),frame->pending.rend(),[&](const PendingCompletion& pending){return pending.handler==ins.argument;});if(found==frame->pending.rend())break;auto completion=*found;frame->pending.erase(std::next(found).base());if(completion.kind==CompletionKind::Throw){if(!DispatchException(frame,completion.value,current))throw JavaScriptException{completion.value};break;}if(BeginAbrupt(frame,completion.kind,completion.value,completion.target,current))break;if(completion.kind==CompletionKind::Return)return {false,completion.value};JumpFrame(frame,completion.target);break;}
            case Op::Pop:pop();break;case Op::Duplicate:if(!stack.empty())stack.push_back(stack.back());break;
            case Op::Add:{auto b=PrimitiveForAddition(pop()),a=PrimitiveForAddition(pop());if(a.type==Value::Type::String||b.type==Value::Type::String){std::wstring text;if(a.type==Value::Type::String)text=std::move(a.string);else text=String(a);if(b.type==Value::Type::String)text+=b.string;else text+=String(b);stack.push_back(Value::String(std::move(text)));}else stack.push_back(Value::Number(Number(a)+Number(b)));break;}
            case Op::Subtract:{auto b=pop(),a=pop();stack.push_back(Value::Number(Number(a)-Number(b)));break;}case Op::Multiply:{auto b=pop(),a=pop();stack.push_back(Value::Number(Number(a)*Number(b)));break;}case Op::Divide:{auto b=pop(),a=pop();stack.push_back(Value::Number(Number(a)/Number(b)));break;}case Op::Modulo:{auto b=pop(),a=pop();stack.push_back(Value::Number(std::fmod(Number(a),Number(b))));break;}case Op::Power:{auto b=pop(),a=pop();stack.push_back(Value::Number(std::pow(Number(a),Number(b))));break;}
            case Op::BitwiseAnd:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)&Uint32(b))));break;}
            case Op::BitwiseOr:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)|Uint32(b))));break;}
            case Op::BitwiseXor:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)^Uint32(b))));break;}
            case Op::ShiftLeft:{auto b=pop(),a=pop();stack.push_back(Value::Number(static_cast<std::int32_t>(Uint32(a)<<(Uint32(b)&31u))));break;}
            case Op::ShiftRight:{auto b=pop(),a=pop();stack.push_back(Value::Number(Int32(a)>>(Uint32(b)&31u)));break;}
            case Op::UnsignedShiftRight:{auto b=pop(),a=pop();stack.push_back(Value::Number(Uint32(a)>>(Uint32(b)&31u)));break;}
            case Op::InValue:{auto object=pop(),key=pop();stack.push_back(Value::Bool(HasProperty(object,String(key))));break;}
            case Op::InstanceOf:{
                const auto constructor=Deref(pop()),value=Deref(pop());
                stack.push_back(Value::Bool(IsInstanceOf(value,constructor)));break;
            }
            case Op::Equal:{auto b=pop(),a=pop();stack.push_back(Value::Bool(LooseEqualValues(a,b)));break;}case Op::NotEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(!LooseEqualValues(a,b)));break;}case Op::StrictEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(EqualValues(a,b)));break;}case Op::StrictNotEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(!EqualValues(a,b)));break;}
            case Op::Less:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)<Number(b)));break;}case Op::LessEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)<=Number(b)));break;}case Op::Greater:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)>Number(b)));break;}case Op::GreaterEqual:{auto b=pop(),a=pop();stack.push_back(Value::Bool(Number(a)>=Number(b)));break;}
            case Op::Not:stack.push_back(Value::Bool(!Truth(pop())));break;case Op::BitwiseNot:stack.push_back(Value::Number(static_cast<std::int32_t>(~Uint32(pop()))));break;case Op::Negate:stack.push_back(Value::Number(-Number(pop())));break;case Op::Positive:stack.push_back(Value::Number(Number(pop())));break;case Op::VoidValue:pop();stack.push_back(Value::Undefined());break;case Op::TypeOf:stack.push_back(Value::String(TypeName(pop())));break;
            case Op::Jump:{const auto target=static_cast<size_t>(ins.argument);if(!BeginAbrupt(frame,CompletionKind::Jump,Value::Undefined(),target,current))JumpFrame(frame,target);break;}
            case Op::JumpFalse:if(!Truth(pop()))JumpFrame(frame,static_cast<size_t>(ins.argument));break;
            case Op::JumpFalseKeep:if(!Truth(stack.back()))JumpFrame(frame,static_cast<size_t>(ins.argument));else pop();break;case Op::JumpTrueKeep:if(Truth(stack.back()))JumpFrame(frame,static_cast<size_t>(ins.argument));else pop();break;
            case Op::JumpNotNullishKeep:{const auto value=Deref(stack.back());if(value.type!=Value::Type::Undefined&&value.type!=Value::Type::Null)JumpFrame(frame,static_cast<size_t>(ins.argument));else pop();break;}
            case Op::Template:stack.push_back(EvaluateTemplate(frame->chunk->constants[ins.argument].string,frame->env));break;
            case Op::TaggedTemplate:{auto tag=pop();stack.push_back(EvaluateTaggedTemplate(tag,frame->chunk->constants[ins.argument].string,frame->env));break;}
            case Op::Return:{const auto value=stack.empty()?Value::Undefined():Deref(pop());if(BeginAbrupt(frame,CompletionKind::Return,value,0,current))break;return {false,value};}
            }}catch(const JavaScriptException& exception){if(!DispatchException(frame,exception.value,current))throw;}
            catch(const std::exception& exception){const auto error=ErrorValue(L"Error",Utf8ToWide(exception.what()));if(!DispatchException(frame,error,current))throw JavaScriptException{error};}
        }
        return {false,Value::Undefined()};
    }
    void InstantiateFunctionDeclarations(const Chunk& chunk,const std::shared_ptr<Environment>& env){
        for(const auto& declaration:chunk.functionDeclarations){
            if(declaration.prototype<0||static_cast<size_t>(declaration.prototype)>=module->prototypes.size()||
               !module->prototypes[static_cast<size_t>(declaration.prototype)])continue;
            auto function=CreateFunction();function->prototype=module->prototypes[static_cast<size_t>(declaration.prototype)];
            function->closure=env;SetEnvironmentValue(env,declaration.name,Value::FromFunction(function),true);
        }
    }
    void InstantiateVariableDeclarations(const Chunk& chunk,const std::shared_ptr<Environment>& env){
        for(const auto& name:chunk.variableDeclarations){
            if(env->values.find(name)!=env->values.end())continue;
            Value initial=Value::Undefined();
            if(env==global){
                const auto window=GlobalWindowObject();
                if(window){
                    const auto existing=window->props.find(name);
                    if(existing!=window->props.end())initial=Deref(existing->second);
                }
            }
            SetEnvironmentValue(env,name,initial,true);
        }
    }
    Value StartAsyncFunction(const std::shared_ptr<Prototype>& prototype,const std::shared_ptr<Environment>& env){
        auto promise=PromiseValue();auto frame=std::make_shared<ExecutionFrame>();frame->chunk=&prototype->chunk;frame->prototype=prototype;frame->env=env;frame->asyncPromise=promise.object;
        if(!callStack.empty())frame->callStackIndex=callStack.size()-1;
        InstantiateVariableDeclarations(*frame->chunk,frame->env);
        InstantiateFunctionDeclarations(*frame->chunk,frame->env);
        try{const auto result=RunFrame(frame);if(!result.suspended)ResolvePromise(promise.object,result.value);}
        catch(const JavaScriptException& exception){RejectPromise(promise.object,exception.value);}
        catch(const std::exception& exception){RejectPromise(promise.object,ErrorValue(L"Error",Utf8ToWide(exception.what())));}
        return promise;
    }
    Value StartGeneratorFunction(const std::shared_ptr<Prototype>& prototype,
                                 const std::shared_ptr<Environment>& env){
        auto frame=std::make_shared<ExecutionFrame>();frame->chunk=&prototype->chunk;
        frame->prototype=prototype;frame->env=env;
        frame->generatorValues=std::make_shared<std::vector<Value>>();
        if(!callStack.empty())frame->callStackIndex=callStack.size()-1;
        InstantiateVariableDeclarations(*frame->chunk,frame->env);
        InstantiateFunctionDeclarations(*frame->chunk,frame->env);
        RunFrame(frame);
        auto generator=ArrayValue(*frame->generatorValues);
        const auto values=generator.object;auto cursor=std::make_shared<size_t>(0);
        generator.object->props[L"next"]=Native([values,cursor](RuntimeCore& r,const Value&,
            const std::vector<Value>&){
            auto result=r.ObjectValue(ObjectKind::Plain);
            const bool done=*cursor>=values->items.size();
            result.object->props[L"done"]=Value::Bool(done);
            result.object->props[L"value"]=done?Value::Undefined():values->items[(*cursor)++];
            return result;
        });
        generator.object->props[L"\uffffsymbol.wellKnown:iterator"]=Native(
            [values](RuntimeCore&,const Value&,const std::vector<Value>&){
                return Value::FromObject(values);
            });
        return generator;
    }
    Value Run(const Chunk& chunk,const std::shared_ptr<Environment>& env,bool trackCurrentCall=false){
        auto frame=std::make_shared<ExecutionFrame>();frame->chunk=&chunk;frame->env=env;
        if(trackCurrentCall&&!callStack.empty())frame->callStackIndex=callStack.size()-1;
        InstantiateVariableDeclarations(chunk,env);
        InstantiateFunctionDeclarations(chunk,env);
        frame->stack.reserve(std::min<size_t>(chunk.code.size(),64));
        const auto result=RunFrame(frame);if(result.suspended)throw std::runtime_error("await outside async execution frame");return result.value;
    }
    void InstallGlobals(){
        auto documentValue=ObjectValue(ObjectKind::Document);
        documentValue.object->props[L"cookie"]=Value::String(L"");
        global->values[L"document"]=documentValue;
        global->values[L"Infinity"]=Value::Number(std::numeric_limits<double>::infinity());
        global->values[L"NaN"]=Value::Number(std::numeric_limits<double>::quiet_NaN());
        auto window=CreateObject(ObjectKind::Window);auto chrome=CreateObject(ObjectKind::Plain);const auto hostBridge=ObjectValue(ObjectKind::WebView);chrome->props[L"webview"]=hostBridge;window->props[L"chrome"]=Value::FromObject(chrome);window->props[L"twebframe"]=hostBridge;global->values[L"window"]=Value::FromObject(window);
        // Classic Script code evaluates top-level `this` as the global Window,
        // including when the script has a "use strict" directive.  Libraries
        // commonly pass it into an IIFE to obtain the global object.
        global->values[L"this"]=Value::FromObject(window);
        window->props[L"window"]=Value::FromObject(window);window->props[L"self"]=Value::FromObject(window);
        // Per HTML, Window.frames is the WindowProxy itself; numeric and named
        // properties are supplied dynamically from the document's live child
        // browsing-context list in GetProperty/HasProperty.
        window->props[L"frames"]=Value::FromObject(window);
        window->props[L"document"]=documentValue;
        window->props[L"name"]=Value::String(windowName);
        global->values[L"self"]=Value::FromObject(window);
        const auto getSelection=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.SelectionValue();});
        global->values[L"getSelection"]=getSelection;window->props[L"getSelection"]=getSelection;
        const auto postMessage=Native([](RuntimeCore& runtime,const Value&,
                                         const std::vector<Value>& arguments){
            if(arguments.empty())return Value::Undefined();
            std::wstring targetOrigin=L"/";
            if(arguments.size()>1){
                const auto options=runtime.Deref(arguments[1]);
                targetOrigin=options.type==Value::Type::Object?
                    runtime.String(runtime.GetProperty(options,L"targetOrigin")):
                    runtime.String(options);
            }
            const auto origin=runtime.CurrentOrigin();
            if(!targetOrigin.empty()&&targetOrigin!=L"*"&&targetOrigin!=L"/"&&
               targetOrigin!=origin)return Value::Undefined();
            const auto data=runtime.Deref(arguments[0]);
            runtime.EnqueueTask([&runtime,data,origin]{
                runtime.DispatchSelfWindowMessage(data,origin);
            });
            return Value::Undefined();
        });
        global->values[L"postMessage"]=postMessage;window->props[L"postMessage"]=postMessage;
        const auto domConstructor=[this,&window](const std::wstring& name){
            auto constructor=Native([name](RuntimeCore& runtime,const Value&,
                                           const std::vector<Value>&)->Value{
                throw JavaScriptException{runtime.ErrorValue(
                    L"TypeError",L"Illegal constructor: "+name)};
            });
            constructor.native->props[L"$domInterface"]=Value::String(name);
            constructor.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);
            global->values[name]=constructor;window->props[name]=constructor;
            return constructor;
        };
        auto nodeConstructor=domConstructor(L"Node");
        nodeConstructor.native->props[L"ELEMENT_NODE"]=Value::Number(1);
        nodeConstructor.native->props[L"TEXT_NODE"]=Value::Number(3);
        nodeConstructor.native->props[L"DOCUMENT_NODE"]=Value::Number(9);
        nodeConstructor.native->props[L"DOCUMENT_FRAGMENT_NODE"]=Value::Number(11);
        domConstructor(L"Element");domConstructor(L"HTMLElement");
        domConstructor(L"HTMLScriptElement");domConstructor(L"HTMLIFrameElement");
        auto htmlImageElementConstructor=domConstructor(L"HTMLImageElement");
        domConstructor(L"Document");
        const auto imageConstructor=Native([](RuntimeCore& runtime,const Value&,
                                               const std::vector<Value>& arguments){
            auto image=runtime.document.CreateElement(L"img");
            const auto setDimension=[&](size_t index,const wchar_t* name){
                if(index>=arguments.size())return;
                const double number=runtime.Number(arguments[index]);
                image->SetAttribute(name,std::isfinite(number)&&number>=0?
                    NumberString(std::floor(number)):L"0");
            };
            setDimension(0,L"width");setDimension(1,L"height");
            return runtime.NodeValue(image);
        });
        imageConstructor.native->props[L"$domInterface"]=Value::String(L"HTMLImageElement");
        imageConstructor.native->props[L"prototype"]=
            htmlImageElementConstructor.native->props[L"prototype"];
        global->values[L"Image"]=imageConstructor;window->props[L"Image"]=imageConstructor;
        auto nodeFilter=ObjectValue(ObjectKind::Plain);nodeFilter.object->props[L"SHOW_ALL"]=Value::Number(0xffffffffu);nodeFilter.object->props[L"SHOW_ELEMENT"]=Value::Number(1);nodeFilter.object->props[L"SHOW_TEXT"]=Value::Number(4);global->values[L"NodeFilter"]=nodeFilter;window->props[L"NodeFilter"]=nodeFilter;
        const auto eventConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto event=r.CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(a.empty()?L"":r.String(a[0]));
            event->props[L"detail"]=Value::Null();event->props[L"bubbles"]=Value::Bool(false);
            event->props[L"cancelable"]=Value::Bool(false);event->props[L"defaultPrevented"]=Value::Bool(false);
            event->props[L"data"]=Value::String(L"");event->props[L"inputType"]=Value::String(L"");
            event->props[L"isTrusted"]=Value::Bool(false);
            if(a.size()>1){const auto options=r.Deref(a[1]);if(options.type==Value::Type::Object&&options.object){
                const auto detail=options.object->props.find(L"detail");if(detail!=options.object->props.end())event->props[L"detail"]=r.Deref(detail->second);
                event->props[L"bubbles"]=Value::Bool(r.Truth(r.GetProperty(options,L"bubbles")));
                event->props[L"cancelable"]=Value::Bool(r.Truth(r.GetProperty(options,L"cancelable")));
                const auto data=options.object->props.find(L"data");if(data!=options.object->props.end())event->props[L"data"]=r.Deref(data->second);
                const auto inputType=options.object->props.find(L"inputType");if(inputType!=options.object->props.end())event->props[L"inputType"]=r.Deref(inputType->second);
            }}
            return Value::FromObject(event);
        });
        global->values[L"Event"]=eventConstructor;global->values[L"CustomEvent"]=eventConstructor;global->values[L"InputEvent"]=eventConstructor;
        window->props[L"Event"]=eventConstructor;window->props[L"CustomEvent"]=eventConstructor;window->props[L"InputEvent"]=eventConstructor;
        const auto alert=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(r.dialogSink)r.dialogSink(a.empty()?L"undefined":r.String(a[0]));
            return Value::Undefined();
        });
        global->values[L"alert"]=alert;window->props[L"alert"]=alert;
        const auto innerWidth=Value::Number(viewportWidth),innerHeight=Value::Number(viewportHeight);
        global->values[L"innerWidth"]=innerWidth;global->values[L"innerHeight"]=innerHeight;
        window->props[L"innerWidth"]=innerWidth;window->props[L"innerHeight"]=innerHeight;
        global->values[L"scrollX"]=global->values[L"scrollY"]=Value::Number(0);
        global->values[L"pageXOffset"]=global->values[L"pageYOffset"]=Value::Number(0);
        window->props[L"scrollX"]=window->props[L"scrollY"]=Value::Number(0);
        window->props[L"pageXOffset"]=window->props[L"pageYOffset"]=Value::Number(0);
        auto screen=ObjectValue(ObjectKind::Plain);screen.object->props[L"width"]=innerWidth;screen.object->props[L"availWidth"]=innerWidth;
        screen.object->props[L"height"]=innerHeight;screen.object->props[L"availHeight"]=innerHeight;
        screen.object->props[L"colorDepth"]=Value::Number(24);screen.object->props[L"pixelDepth"]=Value::Number(24);
        global->values[L"screen"]=screen;window->props[L"screen"]=screen;
        const auto pixelRatio=Value::Number(devicePixelRatio);
        global->values[L"devicePixelRatio"]=pixelRatio;window->props[L"devicePixelRatio"]=pixelRatio;
        auto parent=parentMessageSink?ObjectValue(ObjectKind::FrameWindow):Value::FromObject(window);
        auto top=Value::FromObject(window);
        if(parentMessageSink){
            if(topMessageSink){top=ObjectValue(ObjectKind::FrameWindow);top.object->props[L"$topProxy"]=Value::Bool(true);}
            else top=parent;
            parent.object->props[L"window"]=parent;parent.object->props[L"self"]=parent;
            parent.object->props[L"parent"]=top;parent.object->props[L"top"]=top;
            top.object->props[L"window"]=top;top.object->props[L"self"]=top;
            top.object->props[L"parent"]=top;top.object->props[L"top"]=top;
        }
        global->values[L"parent"]=parent;window->props[L"parent"]=parent;
        // A direct child browsing context sees the containing context through
        // both `parent` and `top`. Top-level documents continue to reference
        // their own Window. This identity is used by frame messaging SDKs.
        global->values[L"top"]=top;window->props[L"top"]=top;
        global->values[L"performance"]=ObjectValue(ObjectKind::Performance);global->values[L"Math"]=ObjectValue(ObjectKind::Math);global->values[L"JSON"]=ObjectValue(ObjectKind::Json);
        auto arrayConstructor=ObjectValue(ObjectKind::ArrayConstructor);
        auto arrayPrototypeValue=ArrayValue({});arrayPrototype=arrayPrototypeValue.object;
        arrayConstructor.object->props[L"prototype"]=arrayPrototypeValue;
        global->values[L"Array"]=arrayConstructor;
        window->props[L"performance"]=global->values[L"performance"];window->props[L"Math"]=global->values[L"Math"];
        window->props[L"JSON"]=global->values[L"JSON"];window->props[L"Array"]=global->values[L"Array"];
        auto objectConstructor=CreateObject(ObjectKind::ObjectConstructor);auto objectPrototype=CreateObject(ObjectKind::Plain);objectPrototype->props[L"hasOwnProperty"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){
            if(a.empty())return Value::Bool(false);const auto value=r.Deref(thisValue);const auto name=r.String(a[0]);
            if(value.type==Value::Type::Function&&value.function)return Value::Bool(value.function->props.count(name)!=0);
            if(value.type==Value::Type::Native&&value.native)return Value::Bool(value.native->props.count(name)!=0);
            if(value.type!=Value::Type::Object||!value.object)return Value::Bool(false);
            if(value.object->kind==ObjectKind::Array){size_t index=0;if(name==L"length")return Value::Bool(true);if(TryParseDecimalIndex(name,index))return Value::Bool(index<value.object->items.size());}
            return Value::Bool(value.object->props.count(name)!=0);
        });
        objectPrototype->props[L"propertyIsEnumerable"]=Native(
            [](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){
                if(a.empty())return Value::Bool(false);
                return Value::Bool(r.OwnPropertyIsEnumerable(thisValue,r.String(a[0])));
            });
        objectConstructor->props[L"prototype"]=Value::FromObject(objectPrototype);global->values[L"Object"]=Value::FromObject(objectConstructor);
        auto functionConstructor=Native([](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Undefined();});
        auto functionPrototype=ObjectValue(ObjectKind::Plain);
        functionPrototype.object->props[L"apply"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){
            const auto receiver=a.empty()?Value::Undefined():r.Deref(a[0]);std::vector<Value> forwarded;
            if(a.size()>1){const auto values=r.Deref(a[1]);if(values.type==Value::Type::Object&&values.object){
                if(values.object->kind==ObjectKind::Array)forwarded=values.object->items;
                else{const auto length=static_cast<size_t>(std::max(0.0,r.Number(r.GetProperty(values,L"length"))));
                    for(size_t index=0;index<length;++index)forwarded.push_back(r.GetProperty(values,std::to_wstring(index)));}
            }}
            return r.Call(thisValue,receiver,forwarded);
        });
        functionPrototype.object->props[L"call"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){
            const auto receiver=a.empty()?Value::Undefined():r.Deref(a[0]);std::vector<Value> forwarded;
            if(a.size()>1)forwarded.assign(a.begin()+1,a.end());return r.Call(thisValue,receiver,forwarded);
        });
        functionConstructor.native->props[L"prototype"]=functionPrototype;
        global->values[L"Function"]=functionConstructor;window->props[L"Function"]=functionConstructor;
        global->values[L"Number"]=ObjectValue(ObjectKind::NumberConstructor);
        const auto typedArrayConstructor=[this](std::wstring name,unsigned bits,bool isSigned,bool clamped,bool floating){
            auto constructor=Native([name,bits,isSigned,clamped,floating](RuntimeCore& r,const Value&,
                                                                        const std::vector<Value>& arguments){
                std::vector<Value> source;
                if(!arguments.empty()){
                    const auto input=r.Deref(arguments[0]);
                    if(input.type==Value::Type::Number){
                        const double requested=input.number;
                        if(!std::isfinite(requested)||requested<0||std::floor(requested)!=requested||
                           requested>static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
                            throw JavaScriptException{r.ErrorValue(L"RangeError",L"Invalid typed array length")};
                        source.resize(static_cast<size_t>(requested),Value::Number(0));
                    }else if(input.type==Value::Type::Object&&input.object){
                        if(input.object->kind==ObjectKind::Array)source=input.object->items;
                        else{
                            const auto length=static_cast<size_t>(std::max(0.0,r.Number(r.GetProperty(input,L"length"))));
                            source.reserve(length);
                            for(size_t index=0;index<length;++index)
                                source.push_back(r.GetProperty(input,std::to_wstring(index)));
                        }
                    }
                }
                const auto convert=[&](const Value& value){
                    double number=r.Number(value);
                    if(floating)return Value::Number(number);
                    if(clamped){
                        if(std::isnan(number)||number<=0)return Value::Number(0);
                        if(number>=255)return Value::Number(255);
                        return Value::Number(std::nearbyint(number));
                    }
                    const std::uint32_t raw=r.Uint32(value);
                    const std::uint32_t mask=bits==32?0xffffffffu:(std::uint32_t{1}<<bits)-1;
                    const std::uint32_t narrowed=raw&mask;
                    if(!isSigned)return Value::Number(static_cast<double>(narrowed));
                    const std::uint32_t sign=std::uint32_t{1}<<(bits-1);
                    const std::int64_t signedValue=(narrowed&sign)?
                        static_cast<std::int64_t>(narrowed)-static_cast<std::int64_t>(std::uint64_t{1}<<bits):
                        static_cast<std::int64_t>(narrowed);
                    return Value::Number(static_cast<double>(signedValue));
                };
                for(auto& value:source)value=convert(value);
                auto result=r.ArrayValue(source);
                result.object->props[L"$typedArrayName"]=Value::String(name);
                result.object->props[L"$typedArrayBits"]=Value::Number(static_cast<double>(bits));
                result.object->props[L"$typedArraySigned"]=Value::Bool(isSigned);
                result.object->props[L"$typedArrayClamped"]=Value::Bool(clamped);
                result.object->props[L"$typedArrayFloating"]=Value::Bool(floating);
                result.object->props[L"BYTES_PER_ELEMENT"]=Value::Number(static_cast<double>(bits/8));
                result.object->props[L"byteLength"]=Value::Number(static_cast<double>(source.size()*(bits/8)));
                result.object->props[L"byteOffset"]=Value::Number(0);
                auto buffer=r.ObjectValue(ObjectKind::Plain);
                buffer.object->byteSource=result.object;
                buffer.object->props[L"byteLength"]=result.object->props[L"byteLength"];
                result.object->props[L"buffer"]=buffer;
                return result;
            });
            constructor.native->props[L"BYTES_PER_ELEMENT"]=Value::Number(static_cast<double>(bits/8));
            constructor.native->props[L"name"]=Value::String(name);
            return constructor;
        };
        for(const auto& definition:std::vector<std::tuple<const wchar_t*,unsigned,bool,bool,bool>>{
                {L"Int8Array",8,true,false,false},{L"Uint8Array",8,false,false,false},
                {L"Uint8ClampedArray",8,false,true,false},{L"Int16Array",16,true,false,false},
                {L"Uint16Array",16,false,false,false},{L"Int32Array",32,true,false,false},
                {L"Uint32Array",32,false,false,false},{L"Float32Array",32,true,false,true},
                {L"Float64Array",64,true,false,true}}){
            const auto name=std::get<0>(definition);
            auto constructor=typedArrayConstructor(name,std::get<1>(definition),std::get<2>(definition),
                                                   std::get<3>(definition),std::get<4>(definition));
            global->values[name]=constructor;window->props[name]=constructor;
        }
        auto arrayBufferConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
            const double requested=arguments.empty()?0:r.Number(arguments[0]);
            if(!std::isfinite(requested)||requested<0||std::floor(requested)!=requested)
                throw JavaScriptException{r.ErrorValue(L"RangeError",L"Invalid array buffer length")};
            auto buffer=r.ObjectValue(ObjectKind::Plain);
            buffer.object->items.resize(static_cast<size_t>(requested),Value::Number(0));
            buffer.object->props[L"byteLength"]=Value::Number(requested);
            return buffer;
        });
        arrayBufferConstructor.native->props[L"isView"]=Native([](RuntimeCore& r,const Value&,
                                                                   const std::vector<Value>& arguments){
            if(arguments.empty())return Value::Bool(false);
            const auto value=r.Deref(arguments[0]);
            return Value::Bool(value.type==Value::Type::Object&&value.object&&
                (value.object->props.count(L"$typedArrayBits")||value.object->props.count(L"$dataView")));
        });
        global->values[L"ArrayBuffer"]=arrayBufferConstructor;window->props[L"ArrayBuffer"]=arrayBufferConstructor;
        auto dataViewConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
            if(arguments.empty())throw JavaScriptException{r.ErrorValue(L"TypeError",L"DataView requires an ArrayBuffer")};
            const auto input=r.Deref(arguments[0]);
            if(input.type!=Value::Type::Object||!input.object)
                throw JavaScriptException{r.ErrorValue(L"TypeError",L"DataView requires an ArrayBuffer")};
            std::weak_ptr<Object> source=input.object->byteSource;
            if(source.expired())source=input.object;
            auto view=r.ObjectValue(ObjectKind::Plain);
            view.object->props[L"$dataView"]=Value::Bool(true);
            view.object->props[L"buffer"]=input;
            view.object->props[L"byteOffset"]=Value::Number(0);
            const auto live=source.lock();
            size_t byteLength=0;
            if(live){
                const auto bits=live->props.count(L"$typedArrayBits")?
                    static_cast<size_t>(r.Number(live->props[L"$typedArrayBits"])):8;
                byteLength=live->items.size()*std::max<size_t>(1,bits/8);
            }
            view.object->props[L"byteLength"]=Value::Number(static_cast<double>(byteLength));
            const auto byteAt=[source](RuntimeCore& inner,size_t byteIndex)->std::uint8_t{
                const auto object=source.lock();
                if(!object)return 0;
                const size_t bits=object->props.count(L"$typedArrayBits")?
                    static_cast<size_t>(inner.Number(object->props[L"$typedArrayBits"])):8;
                const size_t bytesPerElement=std::max<size_t>(1,bits/8);
                const size_t element=byteIndex/bytesPerElement;
                if(element>=object->items.size())
                    throw JavaScriptException{inner.ErrorValue(L"RangeError",L"Offset is outside the bounds of the DataView")};
                const size_t within=byteIndex%bytesPerElement;
                const auto raw=static_cast<std::uint64_t>(inner.Number(object->items[element]));
                return static_cast<std::uint8_t>((raw>>(within*8))&0xffu);
            };
            const auto addGetter=[&](const wchar_t* name,size_t width,bool signedValue){
                view.object->props[name]=r.Native([source,byteAt,width,signedValue](RuntimeCore& inner,const Value&,
                                                                                  const std::vector<Value>& values){
                    const double rawOffset=values.empty()?0:inner.Number(values[0]);
                    if(!std::isfinite(rawOffset)||rawOffset<0||std::floor(rawOffset)!=rawOffset)
                        throw JavaScriptException{inner.ErrorValue(L"RangeError",L"Offset is outside the bounds of the DataView")};
                    const size_t offset=static_cast<size_t>(rawOffset);
                    const bool littleEndian=values.size()>1&&inner.Truth(values[1]);
                    std::uint32_t result=0;
                    for(size_t index=0;index<width;++index){
                        const auto byte=byteAt(inner,offset+index);
                        if(littleEndian)result|=static_cast<std::uint32_t>(byte)<<(index*8);
                        else result=(result<<8)|byte;
                    }
                    if(signedValue){
                        const unsigned bitCount=static_cast<unsigned>(width*8);
                        const std::uint32_t sign=std::uint32_t{1}<<(bitCount-1);
                        if(result&sign){
                            const auto signedResult=static_cast<std::int64_t>(result)-
                                static_cast<std::int64_t>(std::uint64_t{1}<<bitCount);
                            return Value::Number(static_cast<double>(signedResult));
                        }
                    }
                    return Value::Number(static_cast<double>(result));
                });
            };
            addGetter(L"getUint8",1,false);addGetter(L"getInt8",1,true);
            addGetter(L"getUint16",2,false);addGetter(L"getInt16",2,true);
            addGetter(L"getUint32",4,false);addGetter(L"getInt32",4,true);
            return view;
        });
        global->values[L"DataView"]=dataViewConstructor;window->props[L"DataView"]=dataViewConstructor;
        const auto textDecoderConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
            return r.ObjectValue(ObjectKind::TextDecoder);
        });
        global->values[L"TextDecoder"]=textDecoderConstructor;window->props[L"TextDecoder"]=textDecoderConstructor;
        auto crypto=ObjectValue(ObjectKind::Plain);
        crypto.object->props[L"getRandomValues"]=Native([](RuntimeCore& r,const Value&,
                                                            const std::vector<Value>& arguments){
            if(arguments.empty())
                throw JavaScriptException{r.ErrorValue(L"TypeError",L"getRandomValues requires an integer typed array")};
            const auto target=r.Deref(arguments[0]);
            if(target.type!=Value::Type::Object||!target.object||
               target.object->props.find(L"$typedArrayBits")==target.object->props.end()||
               r.Truth(target.object->props[L"$typedArrayFloating"]))
                throw JavaScriptException{r.ErrorValue(L"TypeError",L"getRandomValues requires an integer typed array")};
            const auto bits=static_cast<unsigned>(r.Number(target.object->props[L"$typedArrayBits"]));
            if(target.object->items.size()*(bits/8)>65536)
                throw JavaScriptException{r.ErrorValue(L"QuotaExceededError",L"The requested length exceeds 65,536 bytes")};
            const std::uint32_t mask=bits==32?0xffffffffu:(std::uint32_t{1}<<bits)-1;
            for(auto& item:target.object->items){
                unsigned int random=0;
                if(!FillCryptographicRandom(&random,sizeof(random)))
                    random=static_cast<unsigned int>(
                        std::chrono::high_resolution_clock::now().time_since_epoch().count());
                item=Value::Number(static_cast<double>(static_cast<std::uint32_t>(random)&mask));
            }
            return target;
        });
        crypto.object->props[L"randomUUID"]=Native([](RuntimeCore&,const Value&,
                                                       const std::vector<Value>&){
            std::array<unsigned char,16> bytes{};
            if(!FillCryptographicRandom(bytes.data(),bytes.size())){
                auto seed=static_cast<std::uint64_t>(
                    std::chrono::high_resolution_clock::now().time_since_epoch().count());
                for(size_t index=0;index<bytes.size();++index){
                    seed=seed*6364136223846793005ULL+1442695040888963407ULL;
                    bytes[index]=static_cast<unsigned char>(seed>>56);
                }
            }
            bytes[6]=static_cast<unsigned char>((bytes[6]&0x0f)|0x40);
            bytes[8]=static_cast<unsigned char>((bytes[8]&0x3f)|0x80);
            constexpr wchar_t hex[]=L"0123456789abcdef";
            std::wstring uuid;uuid.reserve(36);
            for(size_t index=0;index<bytes.size();++index){
                if(index==4||index==6||index==8||index==10)uuid.push_back(L'-');
                uuid.push_back(hex[bytes[index]>>4]);
                uuid.push_back(hex[bytes[index]&0x0f]);
            }
            return Value::String(uuid);
        });
        global->values[L"crypto"]=crypto;window->props[L"crypto"]=crypto;
        auto abortControllerConstructor=Native([](RuntimeCore& r,const Value&,
                                                   const std::vector<Value>&){
            auto signal=r.ObjectValue(ObjectKind::Plain);
            signal.object->props[L"aborted"]=Value::Bool(false);
            signal.object->props[L"reason"]=Value::Undefined();
            signal.object->props[L"$abortListeners"]=r.ArrayValue({});
            signal.object->props[L"addEventListener"]=r.Native([signalObject=signal.object](RuntimeCore& inner,const Value&,
                                                                                           const std::vector<Value>& values){
                if(values.size()>1&&ToLower(inner.String(values[0]))==L"abort"&&inner.IsCallable(values[1])){
                    const auto listeners=inner.Deref(signalObject->props[L"$abortListeners"]);
                    if(listeners.type==Value::Type::Object&&listeners.object)listeners.object->items.push_back(inner.Deref(values[1]));
                }
                return Value::Undefined();
            });
            signal.object->props[L"removeEventListener"]=r.Native([signalObject=signal.object](RuntimeCore& inner,const Value&,
                                                                                              const std::vector<Value>& values){
                if(values.size()>1&&ToLower(inner.String(values[0]))==L"abort"){
                    const auto listeners=inner.Deref(signalObject->props[L"$abortListeners"]);
                    if(listeners.type==Value::Type::Object&&listeners.object)
                        listeners.object->items.erase(std::remove_if(listeners.object->items.begin(),listeners.object->items.end(),
                            [&](const Value& listener){return inner.EqualValues(listener,values[1]);}),listeners.object->items.end());
                }
                return Value::Undefined();
            });
            signal.object->props[L"throwIfAborted"]=r.Native([signalObject=signal.object](RuntimeCore& inner,const Value&,
                                                                                         const std::vector<Value>&)->Value{
                if(inner.Truth(signalObject->props[L"aborted"]))
                    throw JavaScriptException{signalObject->props[L"reason"]};
                return Value::Undefined();
            });
            auto controller=r.ObjectValue(ObjectKind::Plain);
            controller.object->props[L"signal"]=signal;
            controller.object->props[L"abort"]=r.Native([signalObject=signal.object](RuntimeCore& inner,const Value&,
                                                                                    const std::vector<Value>& values){
                if(inner.Truth(signalObject->props[L"aborted"]))return Value::Undefined();
                signalObject->props[L"aborted"]=Value::Bool(true);
                signalObject->props[L"reason"]=values.empty()?inner.ErrorValue(L"AbortError",L"The operation was aborted"):inner.Deref(values[0]);
                auto event=inner.ObjectValue(ObjectKind::Event);event.object->props[L"type"]=Value::String(L"abort");
                const auto listeners=inner.Deref(signalObject->props[L"$abortListeners"]);
                if(listeners.type==Value::Type::Object&&listeners.object){
                    const auto snapshot=listeners.object->items;
                    for(const auto& listener:snapshot)inner.Call(listener,Value::FromObject(signalObject),{event});
                }
                const auto handler=signalObject->props.find(L"onabort");
                if(handler!=signalObject->props.end()&&inner.IsCallable(handler->second))
                    inner.Call(handler->second,Value::FromObject(signalObject),{event});
                return Value::Undefined();
            });
            return controller;
        });
        global->values[L"AbortController"]=abortControllerConstructor;
        window->props[L"AbortController"]=abortControllerConstructor;
        auto stringConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::String(a.empty()?L"":r.String(a[0]));});
        stringConstructor.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);
        stringConstructor.native->props[L"fromCharCode"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            std::wstring result;result.reserve(a.size());
            for(const auto& value:a){
                const auto number=r.Number(value);
                const auto code=std::isfinite(number)?static_cast<std::uint32_t>(static_cast<std::int64_t>(number))&0xffffu:0u;
                result.push_back(static_cast<wchar_t>(code));
            }
            return Value::String(result);
        });
        stringConstructor.native->props[L"fromCodePoint"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            std::wstring result;
            for(const auto& value:a){
                const auto number=r.Number(value);
                if(!std::isfinite(number)||number<0||number>0x10ffff||std::floor(number)!=number)
                    throw JavaScriptException{r.ErrorValue(L"RangeError",L"Invalid code point")};
                const auto code=static_cast<std::uint32_t>(number);
                if(code<=0xffff)result.push_back(static_cast<wchar_t>(code));
                else{
                    const auto adjusted=code-0x10000;
                    result.push_back(static_cast<wchar_t>(0xd800+(adjusted>>10)));
                    result.push_back(static_cast<wchar_t>(0xdc00+(adjusted&0x3ff)));
                }
            }
            return Value::String(result);
        });
        global->values[L"String"]=stringConstructor;
        auto symbolConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto description=a.empty()?L"":r.String(a[0]);
            return Value::String(L"\uffffsymbol:"+std::to_wstring(++r.nextSymbolId)+L":"+description);
        });
        for(const auto* name:{L"iterator",L"asyncIterator",L"hasInstance",L"isConcatSpreadable",L"match",L"matchAll",L"replace",L"search",L"species",L"split",L"toPrimitive",L"toStringTag",L"unscopables"})
            symbolConstructor.native->props[name]=Value::String(L"\uffffsymbol.wellKnown:"+std::wstring(name));
        symbolConstructor.native->props[L"for"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto key=a.empty()?L"undefined":r.String(a[0]);const auto found=r.symbolRegistry.find(key);
            if(found!=r.symbolRegistry.end())return found->second;
            auto symbol=Value::String(L"\uffffsymbol.global:"+key);r.symbolRegistry[key]=symbol;return symbol;
        });
        symbolConstructor.native->props[L"keyFor"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(a.empty())return Value::Undefined();const auto symbol=r.String(a[0]);
            for(const auto& entry:r.symbolRegistry)if(r.String(entry.second)==symbol)return Value::String(entry.first);
            return Value::Undefined();
        });
        global->values[L"Symbol"]=symbolConstructor;window->props[L"Symbol"]=symbolConstructor;
        auto regExpConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto expression=r.ObjectValue(ObjectKind::RegExp);
            const auto pattern=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(pattern.type==Value::Type::Object&&pattern.object&&pattern.object->kind==ObjectKind::RegExp){
                expression.object->props[L"$pattern"]=pattern.object->props[L"$pattern"];
                expression.object->props[L"$flags"]=a.size()>1?Value::String(r.String(a[1])):pattern.object->props[L"$flags"];
            }else{
                expression.object->props[L"$pattern"]=Value::String(a.empty()?L"":r.String(a[0]));
                expression.object->props[L"$flags"]=Value::String(a.size()>1?r.String(a[1]):L"");
            }
            return expression;
        });
        regExpConstructor.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);
        global->values[L"RegExp"]=regExpConstructor;
        global->values[L"encodeURIComponent"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto bytes=WideToUtf8(a.empty()?L"undefined":r.String(a[0]));
            constexpr wchar_t hex[]=L"0123456789ABCDEF";
            std::wstring encoded;
            for(unsigned char byte:bytes){
                if((byte>=L'A'&&byte<=L'Z')||(byte>=L'a'&&byte<=L'z')||
                   (byte>=L'0'&&byte<=L'9')||byte=='-'||byte=='_'||byte=='.'||
                   byte=='!'||byte=='~'||byte=='*'||byte=='\''||byte=='('||byte==')')
                    encoded+=static_cast<wchar_t>(byte);
                else{encoded+=L'%';encoded+=hex[byte>>4];encoded+=hex[byte&15];}
            }
            return Value::String(encoded);
        });
        global->values[L"decodeURIComponent"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto encoded=a.empty()?L"undefined":r.String(a[0]);std::string bytes;
            for(size_t index=0;index<encoded.size();){
                if(encoded[index]==L'%'){
                    if(index+2>=encoded.size())throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
                    const int high=HexDigitValue(encoded[index+1]),low=HexDigitValue(encoded[index+2]);
                    if(high<0||low<0)throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
                    bytes.push_back(static_cast<char>((high<<4)|low));index+=3;continue;
                }
                const auto next=encoded.find(L'%',index);const auto chunk=encoded.substr(index,next==std::wstring::npos?std::wstring::npos:next-index);
                bytes+=WideToUtf8(chunk);if(next==std::wstring::npos)break;index=next;
            }
            if(bytes.empty())return Value::String(L"");
            const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
            if(count<=0)throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
            std::wstring decoded(static_cast<size_t>(count),L'\0');
            MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),decoded.data(),count);
            return Value::String(std::move(decoded));
        });
        global->values[L"encodeURI"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto bytes=WideToUtf8(a.empty()?L"undefined":r.String(a[0]));
            constexpr wchar_t hex[]=L"0123456789ABCDEF";std::wstring encoded;
            const auto allowed=[](unsigned char byte){
                return (byte>='A'&&byte<='Z')||(byte>='a'&&byte<='z')||(byte>='0'&&byte<='9')||
                    std::string("-_.!~*'();,/?:@&=+$#").find(static_cast<char>(byte))!=std::string::npos;
            };
            for(unsigned char byte:bytes)if(allowed(byte))encoded+=static_cast<wchar_t>(byte);
                else{encoded+=L'%';encoded+=hex[byte>>4];encoded+=hex[byte&15];}
            return Value::String(std::move(encoded));
        });
        global->values[L"decodeURI"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto encoded=a.empty()?L"undefined":r.String(a[0]);std::wstring decoded;std::string bytes;
            const auto flush=[&]{if(bytes.empty())return;const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);if(count<=0)throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};const auto offset=decoded.size();decoded.resize(offset+static_cast<size_t>(count));MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes.data(),static_cast<int>(bytes.size()),decoded.data()+offset,count);bytes.clear();};
            const std::string reserved=";/?:@&=+$,#";
            for(size_t index=0;index<encoded.size();){
                if(encoded[index]!=L'%'){flush();decoded+=encoded[index++];continue;}
                if(index+2>=encoded.size())throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
                const int high=HexDigitValue(encoded[index+1]),low=HexDigitValue(encoded[index+2]);
                if(high<0||low<0)throw JavaScriptException{r.ErrorValue(L"URIError",L"URI malformed")};
                const auto byte=static_cast<unsigned char>((high<<4)|low);
                if(byte<128&&reserved.find(static_cast<char>(byte))!=std::string::npos){flush();decoded.append(encoded,index,3);}
                else bytes.push_back(static_cast<char>(byte));index+=3;
            }
            flush();return Value::String(std::move(decoded));
        });
        global->values[L"escape"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto input=a.empty()?L"undefined":r.String(a[0]);constexpr wchar_t hex[]=L"0123456789ABCDEF";
            std::wstring output;
            for(const auto character:input){
                if(std::iswalnum(character)||std::wstring(L"@*_+-./").find(character)!=std::wstring::npos){output+=character;continue;}
                const auto code=static_cast<unsigned>(character);
                if(code<=0xff){output+=L'%';output+=hex[(code>>4)&15];output+=hex[code&15];}
                else{output+=L"%u";output+=hex[(code>>12)&15];output+=hex[(code>>8)&15];output+=hex[(code>>4)&15];output+=hex[code&15];}
            }
            return Value::String(std::move(output));
        });
        global->values[L"unescape"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto input=a.empty()?L"undefined":r.String(a[0]);std::wstring output;
            for(size_t index=0;index<input.size();){
                if(input[index]==L'%'&&index+5<input.size()&&(input[index+1]==L'u'||input[index+1]==L'U')){
                    unsigned code=0;bool valid=true;
                    for(size_t digit=0;digit<4;++digit){const int value=HexDigitValue(input[index+2+digit]);if(value<0){valid=false;break;}code=(code<<4)|static_cast<unsigned>(value);}
                    if(valid){output+=static_cast<wchar_t>(code);index+=6;continue;}
                }
                if(input[index]==L'%'&&index+2<input.size()){
                    const int high=HexDigitValue(input[index+1]),low=HexDigitValue(input[index+2]);
                    if(high>=0&&low>=0){output+=static_cast<wchar_t>((high<<4)|low);index+=3;continue;}
                }
                output+=input[index++];
            }
            return Value::String(std::move(output));
        });
        auto booleanConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            return Value::Bool(!a.empty()&&r.Truth(a[0]));
        });
        auto booleanPrototype=ObjectValue(ObjectKind::Plain);
        booleanPrototype.object->props[L"valueOf"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>&){
            const auto value=r.Deref(thisValue);
            if(value.type==Value::Type::Boolean)return value;
            if(value.type==Value::Type::Object&&value.object){
                const auto primitive=value.object->props.find(L"$primitive");
                if(primitive!=value.object->props.end()&&r.Deref(primitive->second).type==Value::Type::Boolean)
                    return r.Deref(primitive->second);
            }
            throw JavaScriptException{r.ErrorValue(L"TypeError",L"Boolean.prototype.valueOf called on incompatible receiver")};
        });
        booleanPrototype.object->props[L"constructor"]=booleanConstructor;
        booleanConstructor.native->props[L"prototype"]=booleanPrototype;
        global->values[L"Boolean"]=booleanConstructor;
        global->values[L"BigInt"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Number(a.empty()?0:r.Number(a[0]));});
        for(const auto* name:{L"Error",L"TypeError",L"RangeError",L"ReferenceError",L"SyntaxError",L"AggregateError",L"URIError"}){
            auto constructor=ObjectValue(ObjectKind::ErrorConstructor);
            constructor.object->props[L"$name"]=Value::String(name);
            auto prototype=ObjectValue(ObjectKind::Plain);
            prototype.object->props[L"name"]=Value::String(name);
            prototype.object->props[L"message"]=Value::String(L"");
            prototype.object->props[L"constructor"]=constructor;
            prototype.object->props[L"toString"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>&){
                const auto error=r.Deref(thisValue);
                const auto name=r.String(r.GetProperty(error,L"name"));
                const auto message=r.String(r.GetProperty(error,L"message"));
                return Value::String(message.empty()?name:name+L": "+message);
            });
            constructor.object->props[L"prototype"]=prototype;
            global->values[name]=constructor;
        }
        global->values[L"parseFloat"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Number(std::numeric_limits<double>::quiet_NaN());const auto text=Trim(r.String(a[0]));if(text.empty())return Value::Number(std::numeric_limits<double>::quiet_NaN());wchar_t* end=nullptr;const double number=std::wcstod(text.c_str(),&end);return Value::Number(end==text.c_str()?std::numeric_limits<double>::quiet_NaN():number);});
        global->values[L"isNaN"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            return Value::Bool(std::isnan(r.Number(a.empty()?Value::Undefined():a[0])));
        });
        global->values[L"isFinite"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            return Value::Bool(std::isfinite(r.Number(a.empty()?Value::Undefined():a[0])));
        });
        global->values[L"parseInt"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto text=Trim(a.empty()?L"undefined":r.String(a[0]));
            int radix=a.size()>1?static_cast<int>(r.Number(a[1])):0;
            if(radix!=0&&(radix<2||radix>36))
                return Value::Number(std::numeric_limits<double>::quiet_NaN());
            size_t index=0;bool negative=false;
            if(index<text.size()&&(text[index]==L'+'||text[index]==L'-')){
                negative=text[index]==L'-';++index;
            }
            if((radix==0||radix==16)&&index+1<text.size()&&text[index]==L'0'&&
               (text[index+1]==L'x'||text[index+1]==L'X')){
                radix=16;index+=2;
            }else if(radix==0)radix=10;
            double value=0;bool consumed=false;
            for(;index<text.size();++index){
                const wchar_t character=text[index];int digit=-1;
                if(character>=L'0'&&character<=L'9')digit=character-L'0';
                else if(character>=L'a'&&character<=L'z')digit=character-L'a'+10;
                else if(character>=L'A'&&character<=L'Z')digit=character-L'A'+10;
                if(digit<0||digit>=radix)break;
                value=value*radix+digit;consumed=true;
            }
            if(!consumed)return Value::Number(std::numeric_limits<double>::quiet_NaN());
            return Value::Number(negative?-value:value);
        });
        window->props[L"parseFloat"]=global->values[L"parseFloat"];
        window->props[L"parseInt"]=global->values[L"parseInt"];
        window->props[L"isNaN"]=global->values[L"isNaN"];
        window->props[L"isFinite"]=global->values[L"isFinite"];
        window->props[L"encodeURIComponent"]=global->values[L"encodeURIComponent"];
        window->props[L"decodeURIComponent"]=global->values[L"decodeURIComponent"];
        window->props[L"encodeURI"]=global->values[L"encodeURI"];
        window->props[L"decodeURI"]=global->values[L"decodeURI"];
        window->props[L"escape"]=global->values[L"escape"];
        window->props[L"unescape"]=global->values[L"unescape"];
        auto reflect=ObjectValue(ObjectKind::Plain);
        reflect.object->props[L"get"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            return a.size()<2?Value::Undefined():r.GetProperty(a[0],r.String(a[1]));
        });
        reflect.object->props[L"set"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(a.size()<3)return Value::Bool(false);r.SetProperty(a[0],r.String(a[1]),a[2]);return Value::Bool(true);
        });
        reflect.object->props[L"apply"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(a.empty()||!r.IsCallable(a[0]))throw JavaScriptException{r.ErrorValue(L"TypeError",L"Reflect.apply target is not callable")};
            std::vector<Value> forwarded;
            if(a.size()>2){const auto list=r.Deref(a[2]);if(list.type==Value::Type::Object&&list.object&&list.object->kind==ObjectKind::Array)forwarded=list.object->items;}
            return r.Call(a[0],a.size()>1?a[1]:Value::Undefined(),forwarded);
        });
        reflect.object->props[L"construct"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            std::vector<Value> forwarded;
            if(a.size()>1){const auto list=r.Deref(a[1]);if(list.type==Value::Type::Object&&list.object&&list.object->kind==ObjectKind::Array)forwarded=list.object->items;}
            return a.empty()?Value::Undefined():r.Construct(a[0],forwarded,a.size()>2?&a[2]:nullptr);
        });
        reflect.object->props[L"has"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            return Value::Bool(a.size()>1&&r.HasProperty(a[0],r.String(a[1])));
        });
        global->values[L"Reflect"]=reflect;window->props[L"Reflect"]=reflect;
        const auto proxyConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(a.size()<2){throw JavaScriptException{r.ErrorValue(L"TypeError",L"Proxy requires a target and handler")};}
            const auto target=r.Deref(a[0]),handler=r.Deref(a[1]);
            const auto targetObject=target.type==Value::Type::Object||target.type==Value::Type::Function||target.type==Value::Type::Native;
            const auto handlerObject=handler.type==Value::Type::Object||handler.type==Value::Type::Function||handler.type==Value::Type::Native;
            if(!targetObject||!handlerObject)throw JavaScriptException{r.ErrorValue(L"TypeError",L"Proxy target and handler must be objects")};
            auto proxy=r.ObjectValue(ObjectKind::Proxy);proxy.object->props[L"$target"]=target;proxy.object->props[L"$handler"]=handler;return proxy;
        });
        global->values[L"Proxy"]=proxyConstructor;window->props[L"Proxy"]=proxyConstructor;
        const auto mapConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.ObjectValue(ObjectKind::Map);
            if(!a.empty()){
                const auto iterable=r.Deref(a[0]);
                if(iterable.type==Value::Type::Object&&iterable.object&&iterable.object->kind==ObjectKind::Array)
                    for(const auto& item:iterable.object->items){
                        const auto pair=r.Deref(item);
                        if(pair.type==Value::Type::Object&&pair.object&&pair.object->kind==ObjectKind::Array&&!pair.object->items.empty())
                            result.object->entries.push_back({r.Deref(pair.object->items[0]),pair.object->items.size()>1?r.Deref(pair.object->items[1]):Value::Undefined()});
                    }
            }
            return result;
        });
        const auto setConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.ObjectValue(ObjectKind::Set);
            if(!a.empty()){
                const auto iterable=r.Deref(a[0]);
                if(iterable.type==Value::Type::Object&&iterable.object&&iterable.object->kind==ObjectKind::Array)
                    for(const auto& item:iterable.object->items){const auto value=r.Deref(item);result.object->entries.push_back({value,value});}
            }
            return result;
        });
        global->values[L"Map"]=mapConstructor;window->props[L"Map"]=mapConstructor;
        global->values[L"Set"]=setConstructor;window->props[L"Set"]=setConstructor;
        // Weak collection reachability is not observable to script. Reuse the same
        // entry storage while providing the standard WeakMap/WeakSet interface.
        global->values[L"WeakMap"]=mapConstructor;window->props[L"WeakMap"]=mapConstructor;
        global->values[L"WeakSet"]=setConstructor;window->props[L"WeakSet"]=setConstructor;
        const auto headersConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
            auto result=r.ObjectValue(ObjectKind::Headers);
            if(arguments.empty())return result;
            const auto source=r.Deref(arguments[0]);
            const auto set=[&](const Value& rawName,const Value& rawValue){
                const auto name=ToLower(Trim(r.String(rawName))),value=r.String(rawValue);
                auto found=std::find_if(result.object->entries.begin(),result.object->entries.end(),[&](const auto& entry){return r.String(entry.first)==name;});
                if(found==result.object->entries.end())result.object->entries.push_back({Value::String(name),Value::String(value)});
                else found->second=Value::String(value);
            };
            if(source.type==Value::Type::Object&&source.object){
                if(source.object->kind==ObjectKind::Headers)result.object->entries=source.object->entries;
                else if(source.object->kind==ObjectKind::Array){
                    for(const auto& item:source.object->items){
                        const auto pair=r.Deref(item);
                        if(pair.type==Value::Type::Object&&pair.object&&pair.object->kind==ObjectKind::Array&&pair.object->items.size()>1)
                            set(pair.object->items[0],pair.object->items[1]);
                    }
                }else for(const auto& property:source.object->props)
                    if(!IsInternalObjectProperty(property.first))set(Value::String(property.first),property.second);
            }
            return result;
        });
        global->values[L"Headers"]=headersConstructor;window->props[L"Headers"]=headersConstructor;
        const auto fileReader=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.ObjectValue(ObjectKind::FileReader);});
        global->values[L"FileReader"]=fileReader;window->props[L"FileReader"]=fileReader;
        auto navigator=ObjectValue(ObjectKind::Plain);auto clipboard=ObjectValue(ObjectKind::Clipboard);navigator.object->props[L"clipboard"]=clipboard;
        wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
        const std::wstring language=GetUserDefaultLocaleName(localeName,static_cast<int>(std::size(localeName)))>0?localeName:L"en-US";
        navigator.object->props[L"appName"]=Value::String(L"Netscape");navigator.object->props[L"appVersion"]=Value::String(L"5.0");
        navigator.object->props[L"userAgent"]=Value::String(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) TWebFrame/1.0");
        navigator.object->props[L"platform"]=Value::String(L"Win32");navigator.object->props[L"language"]=Value::String(language);
        navigator.object->props[L"languages"]=ArrayValue({Value::String(language)});navigator.object->props[L"cookieEnabled"]=Value::Bool(false);
        navigator.object->props[L"onLine"]=Value::Bool(true);
        global->values[L"navigator"]=navigator;window->props[L"navigator"]=navigator;
        global->values[L"Date"]=ObjectValue(ObjectKind::DateConstructor);
        auto intl=ObjectValue(ObjectKind::Plain);
        intl.object->props[L"RelativeTimeFormat"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto locale=a.empty()?L"":ToLower(r.String(a[0]));auto formatter=r.ObjectValue(ObjectKind::Plain);
            formatter.object->props[L"format"]=r.Native([locale](RuntimeCore& inner,const Value&,const std::vector<Value>& values){
                const double raw=values.empty()?0:inner.Number(values[0]);const auto unit=values.size()>1?ToLower(inner.String(values[1])):L"second";const auto amount=NumberString(std::abs(raw));
                if(locale.rfind(L"ko",0)==0){std::wstring translated=unit;if(unit==L"second")translated=L"\uCD08";else if(unit==L"minute")translated=L"\uBD84";else if(unit==L"hour")translated=L"\uC2DC\uAC04";else if(unit==L"day")translated=L"\uC77C";else if(unit==L"week")translated=L"\uC8FC";else if(unit==L"month")translated=L"\uAC1C\uC6D4";else if(unit==L"year")translated=L"\uB144";return Value::String(amount+translated+(raw<0?L" \uC804":L" \uD6C4"));}
                auto label=unit;if(std::abs(raw)!=1)label+=L"s";return Value::String(raw<0?amount+L" "+label+L" ago":L"in "+amount+L" "+label);
            });return formatter;
        });
        intl.object->props[L"NumberFormat"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            bool grouping=true;int minimumFractionDigits=0,maximumFractionDigits=3;
            if(a.size()>1){
                const auto options=r.Deref(a[1]);
                if(options.type==Value::Type::Object&&options.object){
                    if(const auto found=options.object->props.find(L"useGrouping");found!=options.object->props.end())
                        grouping=r.Truth(found->second);
                    if(const auto found=options.object->props.find(L"minimumFractionDigits");found!=options.object->props.end())
                        minimumFractionDigits=std::clamp(static_cast<int>(r.Number(found->second)),0,20);
                    if(const auto found=options.object->props.find(L"maximumFractionDigits");found!=options.object->props.end())
                        maximumFractionDigits=std::clamp(static_cast<int>(r.Number(found->second)),0,20);
                }
            }
            maximumFractionDigits=std::max(maximumFractionDigits,minimumFractionDigits);
            auto formatter=r.ObjectValue(ObjectKind::Plain);
            formatter.object->props[L"format"]=r.Native([grouping,minimumFractionDigits,maximumFractionDigits](RuntimeCore& inner,const Value&,const std::vector<Value>& values){
                const double number=inner.Number(values.empty()?Value::Undefined():values[0]);
                if(!std::isfinite(number))return Value::String(NumberString(number));
                std::wostringstream stream;stream<<std::fixed<<std::setprecision(maximumFractionDigits)<<number;
                auto text=stream.str();const auto decimal=text.find(L'.');
                if(decimal!=std::wstring::npos){
                    while(text.size()>decimal+1+static_cast<size_t>(minimumFractionDigits)&&text.back()==L'0')text.pop_back();
                    if(text.back()==L'.')text.pop_back();
                }
                if(grouping){
                    const auto point=text.find(L'.');const size_t end=point==std::wstring::npos?text.size():point;
                    const size_t begin=!text.empty()&&(text.front()==L'-'||text.front()==L'+')?1:0;
                    for(size_t position=end;position>begin+3;position-=3)text.insert(position-3,1,L',');
                }
                return Value::String(text);
            });
            return formatter;
        });
        intl.object->props[L"DateTimeFormat"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto locale=a.empty()?L"":r.String(a[0]);
            std::wstring year,month,day,hour,minute,second;int hour12=-1;
            if(a.size()>1){
                const auto options=r.Deref(a[1]);
                if(options.type==Value::Type::Object&&options.object){
                    auto stringOption=[&](const wchar_t* name){
                        const auto found=options.object->props.find(name);
                        return found==options.object->props.end()?std::wstring{}:r.String(found->second);
                    };
                    year=stringOption(L"year");month=stringOption(L"month");day=stringOption(L"day");
                    hour=stringOption(L"hour");minute=stringOption(L"minute");second=stringOption(L"second");
                    if(const auto found=options.object->props.find(L"hour12");found!=options.object->props.end()){
                        const auto value=r.Deref(found->second);
                        if(value.type==Value::Type::Boolean)hour12=value.boolean?1:0;
                    }
                }
            }
            auto formatter=r.ObjectValue(ObjectKind::Plain);
            formatter.object->props[L"format"]=r.Native([locale,year,month,day,hour,minute,second,hour12](RuntimeCore& inner,const Value&,const std::vector<Value>& values){
                const auto value=values.empty()?inner.DateValue({}):inner.Deref(values[0]);const double milliseconds=value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Date?inner.Number(value.object->props[L"$time"]):inner.Number(value);SYSTEMTIME time{};if(!DateSystemTime(milliseconds,time,true))return Value::String(L"Invalid Date");
                wchar_t date[128]{},clock[128]{};const auto name=locale.empty()?LOCALE_NAME_USER_DEFAULT:locale.c_str();
                const bool wantsDate=!year.empty()||!month.empty()||!day.empty();
                const bool wantsTime=!hour.empty()||!minute.empty()||!second.empty();
                if(wantsDate){
                    if(year.empty()&&!month.empty()&&!day.empty()){
                        wchar_t pattern[128]{};
                        if(GetLocaleInfoEx(name,LOCALE_SMONTHDAY,pattern,
                                           static_cast<int>(std::size(pattern)))>0)
                            GetDateFormatEx(name,0,&time,pattern,date,
                                            static_cast<int>(std::size(date)),nullptr);
                    }else GetDateFormatEx(name,DATE_SHORTDATE,&time,nullptr,date,
                                          static_cast<int>(std::size(date)),nullptr);
                }
                if(wantsTime){
                    DWORD flags=second.empty()?TIME_NOSECONDS:0;
                    if(hour12==0)flags|=TIME_FORCE24HOURFORMAT|TIME_NOTIMEMARKER;
                    GetTimeFormatEx(name,flags,&time,nullptr,clock,static_cast<int>(std::size(clock)));
                }
                if(date[0]&&clock[0])return Value::String(std::wstring(date)+L" "+clock);
                if(date[0])return Value::String(date);if(clock[0])return Value::String(clock);
                wchar_t fallback[40]{};swprintf_s(fallback,L"%04u-%02u-%02u %02u:%02u",time.wYear,time.wMonth,time.wDay,time.wHour,time.wMinute);return Value::String(fallback);
            });return formatter;
        });global->values[L"Intl"]=intl;
        auto css=ObjectValue(ObjectKind::Plain);css.object->props[L"escape"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto value=a.empty()?L"undefined":r.String(a[0]);std::wstring escaped;
            for(size_t index=0;index<value.size();++index){const wchar_t c=value[index];
                const bool control=(c>=1&&c<=31)||c==127;
                const bool leadingDigit=index==0&&std::iswdigit(c);
                const bool secondDigit=index==1&&value[0]==L'-'&&std::iswdigit(c);
                if(c==0){escaped+=static_cast<wchar_t>(0xfffd);continue;}
                if(control||leadingDigit||secondDigit){wchar_t buffer[16]{};swprintf_s(buffer,L"\\%x ",static_cast<unsigned>(c));escaped+=buffer;continue;}
                if(index==0&&c==L'-'&&value.size()==1){escaped+=L"\\-";continue;}
                if(c>=128||c==L'-'||c==L'_'||std::iswalnum(c)){escaped+=c;continue;}
                escaped+=L'\\';escaped+=c;
            }
            return Value::String(escaped);
        });global->values[L"CSS"]=css;
        global->values[L"requestAnimationFrame"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return Value::Number(a.empty()?0:r.ScheduleAnimationFrame(a[0]));});
        global->values[L"cancelAnimationFrame"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            if(!a.empty()){const unsigned id=static_cast<unsigned>(r.Number(a[0]));
                r.frameCallbacks.erase(std::remove_if(r.frameCallbacks.begin(),r.frameCallbacks.end(),
                    [id](const auto& frame){return frame.first==id;}),r.frameCallbacks.end());}
            return Value::Undefined();
        });
        window->props[L"requestAnimationFrame"]=global->values[L"requestAnimationFrame"];
        window->props[L"cancelAnimationFrame"]=global->values[L"cancelAnimationFrame"];
        auto timeout=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Number(0);return Value::Number(r.ScheduleTimer(a[0],a.size()>1?r.Number(a[1]):0));});
        auto interval=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return Value::Number(0);return Value::Number(r.ScheduleTimer(a[0],a.size()>1?r.Number(a[1]):0,true));});
        auto clearTimeout=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(!a.empty())r.ClearTimer(static_cast<unsigned>(r.Number(a[0])));return Value::Undefined();});
        global->values[L"setTimeout"]=timeout;global->values[L"clearTimeout"]=clearTimeout;global->values[L"setInterval"]=interval;global->values[L"clearInterval"]=clearTimeout;window->props[L"setTimeout"]=timeout;window->props[L"clearTimeout"]=clearTimeout;window->props[L"setInterval"]=interval;window->props[L"clearInterval"]=clearTimeout;
        global->values[L"structuredClone"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return a.empty()?Value::Undefined():r.CloneValue(a[0]);});
        global->values[L"getComputedStyle"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())return r.ObjectValue(ObjectKind::Style);const auto value=r.Deref(a[0]);if(value.type==Value::Type::Object&&value.object&&value.object->kind==ObjectKind::Node){auto style=r.ObjectValue(ObjectKind::Style);style.object->node=value.object->node;style.object->props[L"$computed"]=Value::Bool(true);return style;}return r.ObjectValue(ObjectKind::Style);});
        window->props[L"getComputedStyle"]=global->values[L"getComputedStyle"];
        auto promise=ObjectValue(ObjectKind::PromiseConstructor);
        auto promisePrototypeValue=ObjectValue(ObjectKind::Plain);
        promisePrototype=promisePrototypeValue.object;
        promisePrototype->props[L"then"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){
            const auto promise=r.Deref(thisValue);
            if(promise.type!=Value::Type::Object||!promise.object||promise.object->kind!=ObjectKind::Promise)
                throw JavaScriptException{r.ErrorValue(L"TypeError",L"Promise.prototype.then called on incompatible receiver")};
            return r.PerformThen(promise.object,a.empty()?Value::Undefined():a[0],
                                 a.size()>1?a[1]:Value::Undefined());
        });
        promisePrototype->props[L"catch"]=Native([](RuntimeCore& r,const Value& thisValue,const std::vector<Value>& a){
            const auto promise=r.Deref(thisValue);
            if(promise.type!=Value::Type::Object||!promise.object||promise.object->kind!=ObjectKind::Promise)
                throw JavaScriptException{r.ErrorValue(L"TypeError",L"Promise.prototype.catch called on incompatible receiver")};
            return r.PerformThen(promise.object,Value::Undefined(),a.empty()?Value::Undefined():a[0]);
        });
        promisePrototype->props[L"constructor"]=promise;
        promise.object->props[L"prototype"]=promisePrototypeValue;
        promise.object->props[L"resolve"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.PromiseResolveValue(a.empty()?Value::Undefined():a[0]);});
        promise.object->props[L"reject"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){auto result=r.PromiseValue();r.RejectPromise(result.object,a.empty()?Value::Undefined():a[0]);return result;});
        promise.object->props[L"withResolvers"]=Native([](RuntimeCore& r,const Value&,
                                                           const std::vector<Value>&){
            auto deferred=r.PromiseValue();auto called=std::make_shared<bool>(false);
            auto resolve=r.Native([promise=deferred.object,called](RuntimeCore& runtime,
                const Value&,const std::vector<Value>& values){
                if(!*called){*called=true;runtime.ResolvePromise(promise,
                    values.empty()?Value::Undefined():values[0]);}
                return Value::Undefined();
            });
            auto reject=r.Native([promise=deferred.object,called](RuntimeCore& runtime,
                const Value&,const std::vector<Value>& values){
                if(!*called){*called=true;runtime.RejectPromise(promise,
                    values.empty()?Value::Undefined():values[0]);}
                return Value::Undefined();
            });
            auto result=r.ObjectValue(ObjectKind::Plain);
            result.object->props[L"promise"]=deferred;
            result.object->props[L"resolve"]=resolve;
            result.object->props[L"reject"]=reject;
            return result;
        });
        promise.object->props[L"all"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.all requires an array"));return result;}
            auto values=std::make_shared<std::vector<Value>>(input.object->items.size(),Value::Undefined());auto remaining=std::make_shared<size_t>(values->size());
            if(values->empty()){r.ResolvePromise(result.object,r.ArrayValue({}));return result;}
            for(size_t index=0;index<input.object->items.size();++index){const auto item=r.PromiseResolveValue(input.object->items[index]);auto fulfilled=r.Native([result=result.object,values,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){(*values)[index]=args.empty()?Value::Undefined():args[0];if(--*remaining==0)runtime.ResolvePromise(result,runtime.ArrayValue(*values));return Value::Undefined();});auto rejected=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.RejectPromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}
            return result;
        });
        promise.object->props[L"race"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.race requires an array"));return result;}
            for(const auto& value:input.object->items){const auto item=r.PromiseResolveValue(value);auto fulfilled=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.ResolvePromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});auto rejected=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.RejectPromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}return result;
        });
        promise.object->props[L"allSettled"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.allSettled requires an array"));return result;}
            auto values=std::make_shared<std::vector<Value>>(input.object->items.size(),Value::Undefined());auto remaining=std::make_shared<size_t>(values->size());
            if(values->empty()){r.ResolvePromise(result.object,r.ArrayValue({}));return result;}
            for(size_t index=0;index<input.object->items.size();++index){const auto item=r.PromiseResolveValue(input.object->items[index]);auto fulfilled=r.Native([result=result.object,values,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){auto record=runtime.ObjectValue(ObjectKind::Plain);record.object->props[L"status"]=Value::String(L"fulfilled");record.object->props[L"value"]=args.empty()?Value::Undefined():args[0];(*values)[index]=record;if(--*remaining==0)runtime.ResolvePromise(result,runtime.ArrayValue(*values));return Value::Undefined();});auto rejected=r.Native([result=result.object,values,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){auto record=runtime.ObjectValue(ObjectKind::Plain);record.object->props[L"status"]=Value::String(L"rejected");record.object->props[L"reason"]=args.empty()?Value::Undefined():args[0];(*values)[index]=record;if(--*remaining==0)runtime.ResolvePromise(result,runtime.ArrayValue(*values));return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}
            return result;
        });
        promise.object->props[L"any"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            auto result=r.PromiseValue();const auto input=a.empty()?Value::Undefined():r.Deref(a[0]);
            if(input.type!=Value::Type::Object||!input.object||input.object->kind!=ObjectKind::Array){r.RejectPromise(result.object,r.ErrorValue(L"TypeError",L"Promise.any requires an array"));return result;}
            auto errors=std::make_shared<std::vector<Value>>(input.object->items.size(),Value::Undefined());auto remaining=std::make_shared<size_t>(errors->size());
            auto rejectAll=[result=result.object,errors](RuntimeCore& runtime){auto error=runtime.ErrorValue(L"AggregateError",L"All promises were rejected");error.object->props[L"errors"]=runtime.ArrayValue(*errors);runtime.RejectPromise(result,error);};
            if(errors->empty()){rejectAll(r);return result;}
            for(size_t index=0;index<input.object->items.size();++index){const auto item=r.PromiseResolveValue(input.object->items[index]);auto fulfilled=r.Native([result=result.object](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){runtime.ResolvePromise(result,args.empty()?Value::Undefined():args[0]);return Value::Undefined();});auto rejected=r.Native([result=result.object,errors,remaining,index](RuntimeCore& runtime,const Value&,const std::vector<Value>& args){(*errors)[index]=args.empty()?Value::Undefined():args[0];if(--*remaining==0){auto error=runtime.ErrorValue(L"AggregateError",L"All promises were rejected");error.object->props[L"errors"]=runtime.ArrayValue(*errors);runtime.RejectPromise(result,error);}return Value::Undefined();});r.PerformThen(item.object,fulfilled,rejected);}
            return result;
        });
        global->values[L"Promise"]=promise;
        global->values[L"queueMicrotask"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty()||!r.IsCallable(a[0]))throw JavaScriptException{r.ErrorValue(L"TypeError",L"queueMicrotask callback is not callable")};const auto callback=r.Deref(a[0]);auto* runtime=&r;r.EnqueueMicrotask([runtime,callback](){runtime->Call(callback,Value::Undefined(),{});});return Value::Undefined();});
        auto locationObject=CreateObject(ObjectKind::Location);
        locationObject->props=UrlValue(location,location).object->props;
        global->values[L"location"]=Value::FromObject(locationObject);window->props[L"location"]=Value::FromObject(locationObject);
        auto storage=ObjectValue(ObjectKind::Storage);global->values[L"localStorage"]=storage;window->props[L"localStorage"]=storage;
        auto sessionStorage=ObjectValue(ObjectKind::Storage);global->values[L"sessionStorage"]=sessionStorage;window->props[L"sessionStorage"]=sessionStorage;
        const auto urlConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){if(a.empty())throw JavaScriptException{r.ErrorValue(L"TypeError",L"URL requires an input")};return r.UrlValue(r.String(a[0]),a.size()>1?r.String(a[1]):r.location);});
        const auto urlSearchParamsConstructor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.UrlSearchParamsValue(a.empty()?L"":r.String(a[0]));});
        global->values[L"URL"]=urlConstructor;window->props[L"URL"]=urlConstructor;
        global->values[L"URLSearchParams"]=urlSearchParamsConstructor;window->props[L"URLSearchParams"]=urlSearchParamsConstructor;
        global->values[L"matchMedia"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){return r.MediaQueryValue(a.empty()?L"":r.String(a[0]));});
        window->props[L"matchMedia"]=global->values[L"matchMedia"];
        auto console=ObjectValue(ObjectKind::Plain);
        for(const auto* level:{L"log",L"info",L"warn",L"error",L"debug"}){
            const std::wstring name=level;
            console.object->props[name]=Native([name](RuntimeCore& runtime,const Value&,const std::vector<Value>& arguments){
                std::wstring line=L"[TWebFrame console."+name+L"]";
                for(const auto& argument:arguments)line+=L" "+runtime.String(argument);
                line+=L"\r\n";OutputDebugStringW(line.c_str());
                return Value::Undefined();
            });
        }
        global->values[L"console"]=console;window->props[L"console"]=console;
        const auto observerConstructor=[this](ObjectKind kind){
            return Native([kind](RuntimeCore& r,const Value&,const std::vector<Value>& arguments){
                if(arguments.empty()||!r.IsCallable(arguments[0]))
                    throw JavaScriptException{r.ErrorValue(L"TypeError",L"observer callback is not callable")};
                auto observer=r.ObjectValue(kind);observer.object->props[L"$callback"]=r.Deref(arguments[0]);
                if(arguments.size()>1)observer.object->props[L"$options"]=r.Deref(arguments[1]);
                return observer;
            });
        };
        const auto mutationObserver=observerConstructor(ObjectKind::MutationObserver);
        const auto intersectionObserver=observerConstructor(ObjectKind::IntersectionObserver);
        const auto resizeObserver=observerConstructor(ObjectKind::ResizeObserver);
        global->values[L"MutationObserver"]=mutationObserver;window->props[L"MutationObserver"]=mutationObserver;
        global->values[L"IntersectionObserver"]=intersectionObserver;window->props[L"IntersectionObserver"]=intersectionObserver;
        global->values[L"ResizeObserver"]=resizeObserver;window->props[L"ResizeObserver"]=resizeObserver;
        const auto xmlHttpRequest=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
            auto request=r.ObjectValue(ObjectKind::XmlHttpRequest);
            request.object->props[L"readyState"]=Value::Number(0);
            request.object->props[L"status"]=Value::Number(0);
            request.object->props[L"statusText"]=Value::String(L"");
            request.object->props[L"responseText"]=Value::String(L"");
            request.object->props[L"response"]=Value::String(L"");
            request.object->props[L"responseType"]=Value::String(L"");
            request.object->props[L"withCredentials"]=Value::Bool(false);
            request.object->props[L"timeout"]=Value::Number(0);
            return request;
        });
        global->values[L"XMLHttpRequest"]=xmlHttpRequest;window->props[L"XMLHttpRequest"]=xmlHttpRequest;
        global->values[L"fetch"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& a){
            const auto resource=a.empty()?L"":r.String(a[0]);
            auto makeResponse=[](RuntimeCore& runtime,bool loaded,std::wstring body){
                auto response=runtime.ObjectValue(ObjectKind::Response);
                response.object->props[L"ok"]=Value::Bool(loaded);
                response.object->props[L"status"]=Value::Number(loaded?200:404);
                response.object->props[L"$body"]=Value::String(body);
                auto stream=runtime.ObjectValue(ObjectKind::ReadableStream);
                stream.object->props[L"$body"]=Value::String(std::move(body));
                response.object->props[L"body"]=stream;
                return response;
            };
            if(r.asyncResourceLoader){
                auto promise=r.PromiseValue();auto* runtime=&r;
                r.asyncResourceLoader(resource,
                    [runtime,promise=promise.object,makeResponse](bool loaded,std::wstring body){
                        runtime->ResolvePromise(promise,makeResponse(*runtime,loaded,std::move(body)));
                        runtime->DrainMicrotasks();
                    });
                return promise;
            }
            std::wstring body;const bool loaded=r.resourceLoader&&r.resourceLoader(resource,body);
            return r.PromiseResolveValue(makeResponse(r,loaded,std::move(body)));
        });
    }
    bool CompileRun(const std::wstring& source,std::wstring* result,std::wstring* error){
        MutationBatch batch(*this);
        try{Compiler compiler(module,source);auto program=compiler.CompileProgram();
            auto value=Run(program,global);if(result)*result=String(value);DrainMicrotasks();
            if(error)error->clear();lastError.clear();return true;}
        catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);if(error)*error=lastError;return false;}
        catch(const std::exception& e){const auto* text=e.what();const int bytes=static_cast<int>(std::strlen(text));int count=MultiByteToWideChar(CP_UTF8,0,text,bytes,nullptr,0);lastError.assign(std::max(0,count),L'\0');if(count>0)MultiByteToWideChar(CP_UTF8,0,text,bytes,lastError.data(),count);if(error)*error=lastError;return false;}
    }
    void DispatchWebMessageValue(const Value& data){
        MutationBatch batch(*this);
        auto event=CreateObject(ObjectKind::Event);event->props[L"data"]=Deref(data);event->props[L"type"]=Value::String(L"message");
        auto eventValue=Value::FromObject(event);global->values[L"event"]=eventValue;
        const auto target=GetProperty(global->values[L"window"],L"twebframe");
        event->props[L"target"]=target;event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
        InvokeEventListeners(webViewListeners,L"message",true,target,eventValue,event);
        InvokeEventListeners(webViewListeners,L"message",false,target,eventValue,event);
        DrainMicrotasks();
    }
    bool DispatchWebMessageJson(const std::wstring& json,std::wstring* error){
        try{Compiler compiler(module,json);DispatchWebMessageValue(Run(compiler.CompileExpressionOnly(),global));if(error)error->clear();lastError.clear();return true;}
        catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);if(error)*error=lastError;return false;}
        catch(const std::exception& e){const auto* text=e.what();const int bytes=static_cast<int>(std::strlen(text));int count=MultiByteToWideChar(CP_UTF8,0,text,bytes,nullptr,0);lastError.assign(std::max(0,count),L'\0');if(count>0)MultiByteToWideChar(CP_UTF8,0,text,bytes,lastError.data(),count);if(error)*error=lastError;return false;}
    }
    void DispatchSelfWindowMessage(const Value& data,const std::wstring& sourceOrigin){
        MutationBatch batch(*this);
        auto event=CreateObject(ObjectKind::Event);
        event->props[L"data"]=Deref(data);event->props[L"type"]=Value::String(L"message");
        const auto target=global->values[L"window"],eventValue=Value::FromObject(event);
        event->props[L"source"]=target;event->props[L"origin"]=Value::String(sourceOrigin);
        event->props[L"lastEventId"]=Value::String(L"");event->props[L"target"]=target;
        event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
        event->props[L"defaultPrevented"]=Value::Bool(false);
        event->props[L"isTrusted"]=Value::Bool(true);
        InvokeEventListeners(windowListeners,L"message",true,target,eventValue,event);
        const auto immediate=event->props.find(L"$immediateStopped");
        if(immediate==event->props.end()||!Truth(immediate->second))
            InvokeEventListeners(windowListeners,L"message",false,target,eventValue,event);
        event->props[L"currentTarget"]=Value::Null();event->props[L"eventPhase"]=Value::Number(0);
        DrainMicrotasks();
    }
    bool DispatchWindowMessageJson(const std::wstring& json,const std::shared_ptr<Node>& sourceFrame,
                                   const std::wstring& sourceOrigin,std::wstring* error){
        try{
            Compiler compiler(module,json);
            const auto data=Run(compiler.CompileExpressionOnly(),global);
            MutationBatch batch(*this);
            auto event=CreateObject(ObjectKind::Event);
            event->props[L"data"]=Deref(data);event->props[L"type"]=Value::String(L"message");
            event->props[L"source"]=sourceFrame?FrameWindowValue(sourceFrame):global->values[L"parent"];
            event->props[L"origin"]=Value::String(sourceOrigin);
            event->props[L"lastEventId"]=Value::String(L"");
            const auto eventValue=Value::FromObject(event),target=global->values[L"window"];
            event->props[L"target"]=target;event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
            InvokeEventListeners(windowListeners,L"message",true,target,eventValue,event);
            InvokeEventListeners(windowListeners,L"message",false,target,eventValue,event);
            DrainMicrotasks();
            if(error)error->clear();lastError.clear();return true;
        }catch(const JavaScriptException& exception){lastError=L"Uncaught "+String(exception.value);if(error)*error=lastError;return false;
        }catch(const std::exception& exception){
            const auto* message=exception.what();const int bytes=static_cast<int>(std::strlen(message));
            const int count=MultiByteToWideChar(CP_UTF8,0,message,bytes,nullptr,0);
            lastError.assign(std::max(0,count),L'\0');
            if(count>0)MultiByteToWideChar(CP_UTF8,0,message,bytes,lastError.data(),count);
            if(error)*error=lastError;return false;
        }
    }
    void DispatchInlineEventHandler(const std::shared_ptr<Node>& current,
                                    const std::wstring& eventName,
                                    const Value& eventValue,
                                    const std::shared_ptr<Object>& event){
        if(!inlineEventHandlersEnabled||!current)return;
        const auto source=current->Attribute(L"on"+ToLower(eventName));
        if(source.empty())return;
        try{
            // HTML event attributes execute as functions whose `this` value and
            // event argument are the element currently handling the event. Keep
            // this in the shared dispatcher so inline handlers and listeners see
            // the same target/currentTarget state during bubbling.
            const auto cacheKey=ToLower(eventName)+L'\x1f'+source;
            auto cached=inlineHandlerCache.find(cacheKey);
            Value callback;
            if(cached!=inlineHandlerCache.end())callback=cached->second;
            else{
                Compiler compiler(module,L"(function(event){\n"+source+L"\n})");
                callback=Run(compiler.CompileExpressionOnly(),global);
                if(inlineHandlerCache.size()>=256)inlineHandlerCache.clear();
                inlineHandlerCache.emplace(cacheKey,callback);
            }
            const auto result=Deref(Call(callback,NodeValue(current),{eventValue}));
            if(result.type==Value::Type::Boolean&&!result.boolean)
                event->props[L"defaultPrevented"]=Value::Bool(true);
        }catch(const JavaScriptException& exception){
            lastError=L"Uncaught "+String(exception.value);
        }catch(const std::exception& exception){
            lastError=Utf8ToWide(exception.what());
        }
    }
    void DispatchEventHandlerProperty(const Value& currentTarget,
                                      const std::wstring& eventName,
                                      const Value& eventValue,
                                      const std::shared_ptr<Object>& event){
        const auto target=Deref(currentTarget);
        if(target.type!=Value::Type::Object||!target.object)return;
        const auto handler=target.object->props.find(L"on"+ToLower(eventName));
        if(handler==target.object->props.end()||!IsCallable(handler->second))return;
        try{
            const auto result=Deref(Call(handler->second,currentTarget,{eventValue}));
            if(result.type==Value::Type::Boolean&&!result.boolean)
                event->props[L"defaultPrevented"]=Value::Bool(true);
        }catch(const JavaScriptException& exception){
            lastError=L"Uncaught "+String(exception.value);
        }catch(const std::exception& exception){
            lastError=Utf8ToWide(exception.what());
        }
    }
    bool Dispatch(const std::shared_ptr<Node>& node,const std::wstring& eventName,
                  const std::vector<Node::FileInfo>* transferredFiles=nullptr,
                  const JavaScriptRuntime::EventInit& init={},
                  const std::wstring* transferredText=nullptr){
        MutationBatch batch(*this);
        auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(eventName);
        event->props[L"target"]=NodeValue(node);event->props[L"currentTarget"]=NodeValue(node);
        event->props[L"key"]=Value::String(init.key);event->props[L"button"]=Value::Number(init.button);
        event->props[L"buttons"]=Value::Number(init.buttons);
        event->props[L"clientX"]=Value::Number(init.clientX);event->props[L"clientY"]=Value::Number(init.clientY);
        event->props[L"x"]=Value::Number(init.clientX);event->props[L"y"]=Value::Number(init.clientY);
        event->props[L"pageX"]=Value::Number(init.pageX);event->props[L"pageY"]=Value::Number(init.pageY);
        event->props[L"screenX"]=Value::Number(init.screenX);event->props[L"screenY"]=Value::Number(init.screenY);
        event->props[L"movementX"]=Value::Number(init.movementX);event->props[L"movementY"]=Value::Number(init.movementY);
        event->props[L"relatedTarget"]=NodeValue(init.relatedTarget);
        event->props[L"pointerId"]=Value::Number(1);event->props[L"pointerType"]=Value::String(L"mouse");
        event->props[L"isPrimary"]=Value::Bool(true);
        event->props[L"data"]=Value::String(init.data);event->props[L"inputType"]=Value::String(init.inputType);
        event->props[L"detail"]=Value::Number(init.detail);event->props[L"ctrlKey"]=Value::Bool(init.ctrlKey);
        event->props[L"shiftKey"]=Value::Bool(init.shiftKey);event->props[L"altKey"]=Value::Bool(init.altKey);
        event->props[L"metaKey"]=Value::Bool(init.metaKey);event->props[L"isComposing"]=Value::Bool(init.isComposing);
        event->props[L"defaultPrevented"]=Value::Bool(false);event->props[L"isTrusted"]=Value::Bool(true);
        event->props[L"bubbles"]=Value::Bool(init.bubbles);event->props[L"cancelable"]=Value::Bool(init.cancelable);event->props[L"eventPhase"]=Value::Number(0);
        if(transferredFiles||transferredText){const std::vector<Node::FileInfo> emptyFiles;
            auto transfer=DataTransferValue(transferredText?*transferredText:L"",transferredFiles?*transferredFiles:emptyFiles);
            event->props[eventName==L"paste"||eventName==L"copy"||eventName==L"cut"?L"clipboardData":L"dataTransfer"]=transfer;}
        auto eventValue=Value::FromObject(event);global->values[L"event"]=eventValue;
        auto stopped=[&](const wchar_t* property){const auto found=event->props.find(property);return found!=event->props.end()&&Truth(found->second);};
        const auto windowTarget=global->values[L"window"],documentTarget=global->values[L"document"];
        auto invoke=[&](EventListenerMap& map,const Value& target,bool capture,int phase){event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(phase);InvokeEventListeners(map,eventName,capture,target,eventValue,event);};
        if(node){
            std::vector<std::shared_ptr<Node>> ancestors;
            bool connected=false;
            for(auto current=node->parent.lock();current;current=current->parent.lock()){
                if(current->type==NodeType::Document){connected=true;break;}
                ancestors.push_back(current);
            }
            if(connected)invoke(windowListeners,windowTarget,true,1);
            if(connected&&!stopped(L"$propagationStopped"))invoke(documentListeners,documentTarget,true,1);
            if(!stopped(L"$propagationStopped"))for(auto iterator=ancestors.rbegin();iterator!=ancestors.rend();++iterator){
                auto found=listeners.find(iterator->get());if(found!=listeners.end()&&found->second.node.lock()==*iterator)invoke(found->second.events,NodeValue(*iterator),true,1);
                if(stopped(L"$propagationStopped"))break;
            }
            if(!stopped(L"$propagationStopped")){
                const auto target=NodeValue(node);auto found=listeners.find(node.get());
                if(found!=listeners.end()&&found->second.node.lock()==node)invoke(found->second.events,target,true,2);
                if(!stopped(L"$immediateStopped")){event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);DispatchInlineEventHandler(node,eventName,eventValue,event);}
                if(!stopped(L"$immediateStopped"))DispatchEventHandlerProperty(target,eventName,eventValue,event);
                if(!stopped(L"$immediateStopped")){auto bubbleListeners=listeners.find(node.get());if(bubbleListeners!=listeners.end()&&bubbleListeners->second.node.lock()==node)invoke(bubbleListeners->second.events,target,false,2);}
            }
            if(init.bubbles&&!stopped(L"$propagationStopped"))for(const auto& current:ancestors){
                const auto target=NodeValue(current);event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(3);
                DispatchInlineEventHandler(current,eventName,eventValue,event);
                if(!stopped(L"$immediateStopped"))DispatchEventHandlerProperty(target,eventName,eventValue,event);
                if(!stopped(L"$immediateStopped")){auto found=listeners.find(current.get());if(found!=listeners.end()&&found->second.node.lock()==current)invoke(found->second.events,target,false,3);}
                if(stopped(L"$propagationStopped"))break;
            }
            if(connected&&init.bubbles&&!stopped(L"$propagationStopped"))invoke(documentListeners,documentTarget,false,3);
            if(connected&&init.bubbles&&!stopped(L"$propagationStopped"))invoke(windowListeners,windowTarget,false,3);
        }else{
            event->props[L"target"]=documentTarget;
            invoke(windowListeners,windowTarget,true,1);
            if(!stopped(L"$propagationStopped")){
                invoke(documentListeners,documentTarget,true,2);
                if(!stopped(L"$immediateStopped"))invoke(documentListeners,documentTarget,false,2);
            }
            if(!stopped(L"$propagationStopped"))invoke(windowListeners,windowTarget,false,3);
        }
        event->props[L"currentTarget"]=Value::Null();event->props[L"eventPhase"]=Value::Number(0);
        DrainMicrotasks();
        return stopped(L"defaultPrevented");
    }
    bool DispatchWindowEventObject(const std::shared_ptr<Object>& event){
        MutationBatch batch(*this);
        const auto target=global->values[L"window"],eventValue=Value::FromObject(event);
        const auto eventName=String(event->props[L"type"]);event->props[L"target"]=target;
        event->props[L"currentTarget"]=target;event->props[L"eventPhase"]=Value::Number(2);
        if(!event->props.count(L"defaultPrevented"))event->props[L"defaultPrevented"]=Value::Bool(false);
        if(!event->props.count(L"isTrusted"))event->props[L"isTrusted"]=Value::Bool(false);
        InvokeEventListeners(windowListeners,eventName,true,target,eventValue,event);
        const auto immediate=event->props.find(L"$immediateStopped");
        if(immediate==event->props.end()||!Truth(immediate->second))InvokeEventListeners(windowListeners,eventName,false,target,eventValue,event);
        event->props[L"currentTarget"]=Value::Null();event->props[L"eventPhase"]=Value::Number(0);
        DrainMicrotasks();
        return !Truth(event->props[L"defaultPrevented"]);
    }
    void DispatchWindow(const std::wstring& eventName){
        auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(eventName);
        event->props[L"defaultPrevented"]=Value::Bool(false);event->props[L"isTrusted"]=Value::Bool(true);
        DispatchWindowEventObject(event);
    }
};

} // namespace

struct JavaScriptRuntime::Impl {
    RuntimeCore core;
    explicit Impl(Document& document):core(document){}
};

JavaScriptRuntime::JavaScriptRuntime(Document& document):impl_(std::make_unique<Impl>(document)){}
JavaScriptRuntime::~JavaScriptRuntime()=default;
void JavaScriptRuntime::SetMessageSink(MessageSink sink){impl_->core.messageSink=std::move(sink);}
void JavaScriptRuntime::SetMutationSink(MutationSink sink){impl_->core.mutationSink=std::move(sink);}
void JavaScriptRuntime::SetFrameScheduler(FrameScheduler scheduler){impl_->core.frameScheduler=std::move(scheduler);}
void JavaScriptRuntime::SetTimerScheduler(TimerScheduler scheduler){impl_->core.timerScheduler=std::move(scheduler);}
void JavaScriptRuntime::SetGeometryProvider(GeometryProvider provider){impl_->core.geometryProvider=std::move(provider);}
void JavaScriptRuntime::SetStylePropertyProvider(StylePropertyProvider provider){impl_->core.stylePropertyProvider=std::move(provider);}
void JavaScriptRuntime::SetResourceLoader(ResourceLoader loader){impl_->core.resourceLoader=std::move(loader);}
void JavaScriptRuntime::SetAsyncResourceLoader(AsyncResourceLoader loader){impl_->core.asyncResourceLoader=std::move(loader);}
void JavaScriptRuntime::SetNavigationSink(NavigationSink sink){impl_->core.navigationSink=std::move(sink);}
void JavaScriptRuntime::SetDialogSink(DialogSink sink){impl_->core.dialogSink=std::move(sink);}
void JavaScriptRuntime::SetDocumentWriteSink(DocumentWriteSink sink){impl_->core.documentWriteSink=std::move(sink);}
void JavaScriptRuntime::SetFrameMessageSink(FrameMessageSink sink){impl_->core.frameMessageSink=std::move(sink);}
void JavaScriptRuntime::SetFrameDocumentSink(FrameDocumentSink sink){impl_->core.frameDocumentSink=std::move(sink);}
void JavaScriptRuntime::SetParentMessageSink(ParentMessageSink sink){impl_->core.parentMessageSink=std::move(sink);}
void JavaScriptRuntime::SetTopMessageSink(TopMessageSink sink){impl_->core.topMessageSink=std::move(sink);}
void JavaScriptRuntime::SetFocusSink(FocusSink sink){impl_->core.focusSink=std::move(sink);}
void JavaScriptRuntime::SetDocumentFocusProvider(DocumentFocusProvider provider){impl_->core.documentFocusProvider=std::move(provider);}
void JavaScriptRuntime::SetActivationSink(ActivationSink sink){impl_->core.activationSink=std::move(sink);}
void JavaScriptRuntime::SetPointerCaptureSink(PointerCaptureSink sink){impl_->core.pointerCaptureSink=std::move(sink);}
void JavaScriptRuntime::SetSelectionProvider(SelectionProvider provider){impl_->core.selectionProvider=std::move(provider);}
void JavaScriptRuntime::SetSelectionSetter(SelectionSetter setter){impl_->core.selectionSetter=std::move(setter);}
void JavaScriptRuntime::SetDomSelectionProvider(DomSelectionProvider provider){impl_->core.domSelectionProvider=std::move(provider);}
void JavaScriptRuntime::SetDomSelectionSetter(DomSelectionSetter setter){impl_->core.domSelectionSetter=std::move(setter);}
void JavaScriptRuntime::SetInlineEventHandlersEnabled(bool enabled){
    impl_->core.inlineEventHandlersEnabled=enabled;
    if(!enabled)impl_->core.inlineHandlerCache.clear();
}
void JavaScriptRuntime::SetViewportSize(double width,double height){
    impl_->core.viewportWidth=std::max(0.0,width);impl_->core.viewportHeight=std::max(0.0,height);
    const auto widthValue=Value::Number(impl_->core.viewportWidth),heightValue=Value::Number(impl_->core.viewportHeight);
    impl_->core.global->values[L"innerWidth"]=widthValue;impl_->core.global->values[L"innerHeight"]=heightValue;
    const auto found=impl_->core.global->values.find(L"window");
    if(found!=impl_->core.global->values.end()&&found->second.type==Value::Type::Object&&found->second.object){
        found->second.object->props[L"innerWidth"]=widthValue;
        found->second.object->props[L"innerHeight"]=heightValue;
    }
    const auto screen=impl_->core.global->values.find(L"screen");
    if(screen!=impl_->core.global->values.end()&&screen->second.object){
        screen->second.object->props[L"width"]=widthValue;screen->second.object->props[L"availWidth"]=widthValue;
        screen->second.object->props[L"height"]=heightValue;screen->second.object->props[L"availHeight"]=heightValue;
    }
    impl_->core.UpdateMediaQueries();
}
void JavaScriptRuntime::SetDevicePixelRatio(double ratio){
    impl_->core.devicePixelRatio=std::isfinite(ratio)&&ratio>0?ratio:1;
    const auto value=Value::Number(impl_->core.devicePixelRatio);
    impl_->core.global->values[L"devicePixelRatio"]=value;
    const auto found=impl_->core.global->values.find(L"window");
    if(found!=impl_->core.global->values.end()&&found->second.object)
        found->second.object->props[L"devicePixelRatio"]=value;
    impl_->core.UpdateMediaQueries();
}
void JavaScriptRuntime::SetLocation(const std::wstring& location){
    auto& core=impl_->core;core.location=location;
    const auto found=core.global->values.find(L"location");
    if(found==core.global->values.end())return;
    const auto value=core.Deref(found->second);
    if(value.type!=Value::Type::Object||!value.object||
       value.object->kind!=ObjectKind::Location)return;
    value.object->props=core.UrlValue(location,location).object->props;
}
void JavaScriptRuntime::SetWindowName(const std::wstring& name){
    auto& core=impl_->core;core.windowName=name;
    const auto found=core.global->values.find(L"window");
    if(found!=core.global->values.end()&&found->second.object)
        found->second.object->props[L"name"]=Value::String(name);
}
void JavaScriptRuntime::SetCurrentScript(const std::shared_ptr<Node>& script){
    impl_->core.currentScript=script;
}
std::wstring JavaScriptRuntime::FrameWindowName(const std::shared_ptr<Node>& node){
    return impl_->core.FrameWindowName(node);
}
void JavaScriptRuntime::SetDocumentReadyState(const std::wstring& state){
    impl_->core.documentReadyState=state==L"loading"||state==L"interactive"?state:L"complete";
}
void JavaScriptRuntime::NavigateToFragment(const std::wstring& fragment){
    const auto found=impl_->core.global->values.find(L"location");
    if(found!=impl_->core.global->values.end())
        impl_->core.SetProperty(found->second,L"hash",Value::String(fragment));
}
bool JavaScriptRuntime::Load(const std::wstring& source,std::wstring* error){
    impl_->core.ResetExecutionState(true);
    return impl_->core.CompileRun(source,nullptr,error);
}
bool JavaScriptRuntime::Execute(const std::wstring& source,std::wstring* result,std::wstring* error){return impl_->core.CompileRun(source,result,error);}
void JavaScriptRuntime::SetJitCompilationThreshold(size_t calls){impl_->core.jitCompilationThreshold=calls;}
JavaScriptRuntime::JitStatistics JavaScriptRuntime::GetJitStatistics()const{return impl_->core.jitStatistics;}
std::wstring JavaScriptRuntime::LastError()const{return impl_->core.lastError;}
std::wstring JavaScriptRuntime::LastCreatedError()const{return impl_->core.lastCreatedError;}
std::wstring JavaScriptRuntime::CreatedErrorTrace()const{
    std::wstring result;
    for(const auto& error:impl_->core.createdErrors){
        if(!result.empty())result+=L"\n---\n";
        result+=error;
    }
    return result;
}
void JavaScriptRuntime::DispatchDocumentEvent(const std::wstring& eventName){impl_->core.Dispatch({},eventName);}
void JavaScriptRuntime::DispatchWindowEvent(const std::wstring& eventName){impl_->core.DispatchWindow(eventName);}
void JavaScriptRuntime::RunAnimationFrame(){impl_->core.RunAnimationFrame();}
void JavaScriptRuntime::RunTimers(){impl_->core.RunTimers();}
bool JavaScriptRuntime::DispatchNodeEvent(const std::shared_ptr<Node>& node,const std::wstring& eventName,const EventInit& init){
    const bool defaultPrevented=impl_->core.Dispatch(node,eventName,nullptr,init);
    // Primary pointer button transitions produce their compatibility mouse
    // events unless the pointer event was canceled. Keep the rule in the
    // shared DOM path so content-editable and ordinary elements behave alike.
    if(!defaultPrevented&&(eventName==L"pointerdown"||eventName==L"pointerup"))
        return impl_->core.Dispatch(node,eventName==L"pointerdown"?L"mousedown":L"mouseup",nullptr,init);
    return defaultPrevented;
}
void JavaScriptRuntime::CompleteImageRequest(const std::shared_ptr<Node>& node){
    impl_->core.CompleteImageRequest(node);
}
void JavaScriptRuntime::CancelPendingImageRequests(){
    impl_->core.pendingImageObjects.clear();
}
bool JavaScriptRuntime::DispatchClipboardEvent(const std::shared_ptr<Node>& node,const std::wstring& eventName,const std::wstring& text,const std::vector<Node::FileInfo>& files){return impl_->core.Dispatch(node,eventName,&files,{},&text);}
std::shared_ptr<Node> JavaScriptRuntime::CapturedPointerTarget()const{return impl_->core.pointerCaptureNode.lock();}
void JavaScriptRuntime::ClearPointerCapture(){impl_->core.pointerCaptureNode.reset();if(impl_->core.pointerCaptureSink)impl_->core.pointerCaptureSink(false);}
void JavaScriptRuntime::DispatchFileDrop(const std::shared_ptr<Node>& node,const std::vector<Node::FileInfo>& files){impl_->core.Dispatch(node,L"drop",&files);}
bool JavaScriptRuntime::DispatchWebMessageAsJson(const std::wstring& json,std::wstring* error){return impl_->core.DispatchWebMessageJson(json,error);}
void JavaScriptRuntime::DispatchWebMessageAsString(const std::wstring& message){impl_->core.DispatchWebMessageValue(Value::String(message));}
bool JavaScriptRuntime::DispatchWindowMessageAsJson(const std::wstring& json,const std::shared_ptr<Node>& sourceFrame,const std::wstring& sourceOrigin,std::wstring* error){return impl_->core.DispatchWindowMessageJson(json,sourceFrame,sourceOrigin,error);}
void JavaScriptRuntime::Clear(){impl_->core.ResetExecutionState(true);}

} // namespace TWebFrame::Internal

#pragma once
#include "../vendor/wasm3/m3_env.h"
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

inline thread_local std::function<bool()> runtimeWasmPoll;
extern "C" const char* TWebFrameWasmYield(){
    try{return runtimeWasmPoll&&!runtimeWasmPoll()?"WebAssembly execution interrupted":nullptr;}catch(...){return "WebAssembly execution interrupted";}
}
namespace TWebFrame::Internal {
struct RuntimeWasmPollScope {
    std::function<bool()> previous;
    explicit RuntimeWasmPollScope(std::function<bool()> callback):previous(std::move(runtimeWasmPoll)){runtimeWasmPoll=std::move(callback);}
    ~RuntimeWasmPollScope(){runtimeWasmPoll=std::move(previous);}
};
struct RuntimeWasmState {
    struct Export {std::string name;unsigned kind=0,index=0;};
    std::vector<unsigned char> bytes;
    std::vector<Export> exports;
    IM3Environment environment=nullptr;IM3Runtime runtime=nullptr;IM3Module module=nullptr;
    std::weak_ptr<void> wrapper;
    struct Import {void* core=nullptr;RuntimeWasmState* state=nullptr;unsigned index=0;};
    std::vector<std::unique_ptr<Import>> imports;
    explicit RuntimeWasmState(std::vector<unsigned char> binary):bytes(std::move(binary)){
        environment=m3_NewEnvironment();if(!environment)throw std::bad_alloc();
        const auto result=m3_ParseModule(environment,&module,bytes.data(),static_cast<uint32_t>(bytes.size()));
        if(result){m3_FreeEnvironment(environment);environment=nullptr;throw std::runtime_error(result);}
        try{
            size_t position=8;
            const auto number=[&](){uint32_t result=0;unsigned shift=0;for(;;){
                if(position>=bytes.size()||shift>=35)throw std::runtime_error("Invalid LEB128 length");
                const auto byte=bytes[position++];result|=(byte&127)<<shift;if(!(byte&128))return result;shift+=7;}};
            while(position<bytes.size()){
                const auto section=bytes[position++];const auto length=number();const size_t end=position+length;
                if(end>bytes.size())throw std::runtime_error("Invalid section length");
                if(section==7){const auto count=number();for(unsigned i=0;i<count;++i){const auto size=number();
                    if(position+size>=end)throw std::runtime_error("Invalid export name");
                    Export item;item.name.assign(reinterpret_cast<const char*>(bytes.data()+position),size);position+=size;
                    item.kind=bytes[position++];item.index=number();exports.push_back(std::move(item));}}
                position=end;
            }
        }catch(...){m3_FreeModule(module);module=nullptr;m3_FreeEnvironment(environment);environment=nullptr;throw;}
    }
    ~RuntimeWasmState(){if(runtime)m3_FreeRuntime(runtime);else if(module)m3_FreeModule(module);if(environment)m3_FreeEnvironment(environment);}
    void Load(){runtime=m3_NewRuntime(environment,1024*1024,nullptr);if(!runtime)throw std::bad_alloc();
        const auto result=m3_LoadModule(runtime,module);if(result){m3_FreeModule(module);module=nullptr;throw std::runtime_error(result);}}
    static char Type(M3ValueType type){return type==c_m3Type_i32?'i':type==c_m3Type_i64?'I':type==c_m3Type_f32?'f':type==c_m3Type_f64?'F':'v';}
    static std::string Signature(IM3Function function){std::string result;
        if(m3_GetRetCount(function)>1)throw std::runtime_error("Multi-value imports are not supported");
        result+=m3_GetRetCount(function)?Type(m3_GetRetType(function,0)):'v';result+='(';
        for(unsigned i=0;i<m3_GetArgCount(function);++i)result+=Type(m3_GetArgType(function,i));result+=')';return result;
    }
};
}

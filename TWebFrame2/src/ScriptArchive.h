#pragma once
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace TWebFrame::Internal {
// Opt-in local diagnostics, including eval, Function and Worker sources.
// Never replays a captured script or changes its execution environment.
class ScriptArchive {
    struct State {
        std::filesystem::path directory;
        std::mutex mutex;
        unsigned long long sequence=0;
        std::chrono::steady_clock::time_point origin=std::chrono::steady_clock::now();
        State(){wchar_t path[32768]{};const auto n=GetEnvironmentVariableW(L"TWEBFRAME_SCRIPT_ARCHIVE",path,32768);
            if(n&&n<32768)try{directory=path;std::filesystem::create_directories(directory);}catch(...){directory.clear();}}
    };
    static State& Shared(){static State state;return state;}
    static std::string Utf8(const std::wstring& text){
        const auto n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
        std::string result(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr);return result;
    }
    static std::string Quote(const std::wstring& text){
        std::string result="\"";const char hex[]="0123456789abcdef";
        for(unsigned char c:Utf8(text)){if(c=='"'||c=='\\'){result+='\\';result+=c;}
            else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}else result+=c;}
        return result+'"';
    }
    static unsigned long long Cpu(){FILETIME a{},b{},k{},u{};GetThreadTimes(GetCurrentThread(),&a,&b,&k,&u);
        ULARGE_INTEGER x{},y{};x.LowPart=k.dwLowDateTime;x.HighPart=k.dwHighDateTime;y.LowPart=u.dwLowDateTime;y.HighPart=u.dwHighDateTime;return x.QuadPart+y.QuadPart;}
    unsigned long long id_=0,cpu_=0;
    std::chrono::steady_clock::time_point start_;
public:
    static void Job(const std::wstring& url,unsigned long long wallMs,unsigned long long cpuTicks,unsigned long long instructions){
        auto& state=Shared();if(state.directory.empty())return;
        try{std::lock_guard<std::mutex> lock(state.mutex);
            const auto end=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-state.origin).count();
            std::ofstream events(state.directory/"jobs.jsonl",std::ios::binary|std::ios::app);
            events<<"{\"url\":"<<Quote(url)<<",\"startMs\":"<<end-static_cast<long long>(wallMs)<<",\"endMs\":"<<end
                <<",\"wallMs\":"<<wallMs<<",\"cpuMs\":"<<cpuTicks/10000.0<<",\"instructions\":"<<instructions
                <<",\"thread\":"<<GetCurrentThreadId()<<"}\n";
        }catch(...){}
    }
    ScriptArchive(const std::wstring& source,const std::wstring& url,const wchar_t* kind){
        auto& state=Shared();if(state.directory.empty())return;
        try{std::lock_guard<std::mutex> lock(state.mutex);id_=++state.sequence;
            const auto file=std::to_string(id_)+".js";std::ofstream output(state.directory/file,std::ios::binary);
            output<<Utf8(source);output.close();
            std::ofstream events(state.directory/"sources.jsonl",std::ios::binary|std::ios::app);
            events<<"{\"id\":"<<id_<<",\"file\":\""<<file<<"\",\"url\":"<<Quote(url)<<",\"kind\":"<<Quote(kind)
                <<",\"characters\":"<<source.size()<<",\"thread\":"<<GetCurrentThreadId()<<",\"ms\":"
                <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-state.origin).count()<<"}\n";
            start_=std::chrono::steady_clock::now();cpu_=Cpu();
        }catch(...){id_=0;}
    }
    ~ScriptArchive(){if(!id_)return;
        const auto end=std::chrono::steady_clock::now();const auto cpu=Cpu();auto& state=Shared();
        try{std::lock_guard<std::mutex> lock(state.mutex);std::ofstream events(state.directory/"execution.jsonl",std::ios::binary|std::ios::app);
            events<<"{\"id\":"<<id_<<",\"wallMs\":"<<std::chrono::duration<double,std::milli>(end-start_).count()
                <<",\"cpuMs\":"<<(cpu-cpu_)/10000.0<<"}\n";}catch(...){}
    }
};
}

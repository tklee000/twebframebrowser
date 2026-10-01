#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// Diagnostic only: sample the CPU thread of one of this directory's hosts.
// Resume immediately after copying its native stack; resolve names afterwards.
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct ThreadPause {
    HANDLE thread;bool suspended=false;
    explicit ThreadPause(HANDLE value):thread(value){suspended=SuspendThread(thread)!=DWORD(-1);}
    ~ThreadPause(){if(suspended)ResumeThread(thread);}
};
unsigned long long Ticks(FILETIME value){
    ULARGE_INTEGER ticks{};ticks.LowPart=value.dwLowDateTime;ticks.HighPart=value.dwHighDateTime;
    return ticks.QuadPart;
}
DWORD CpuThread(DWORD processId){
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0)};
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);DWORD selected=0;unsigned long long maximum=0;
    if(snapshot.value==INVALID_HANDLE_VALUE||!Thread32First(snapshot.value,&entry))return 0;
    do{if(entry.th32OwnerProcessID!=processId)continue;
        Handle thread{OpenThread(THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID)};
        FILETIME creation{},exit{},kernel{},user{};
        if(!thread.value||!GetThreadTimes(thread.value,&creation,&exit,&kernel,&user))continue;
        const auto elapsed=Ticks(kernel)+Ticks(user);
        if(!selected||elapsed>maximum){selected=entry.th32ThreadID;maximum=elapsed;}
    }while(Thread32Next(snapshot.value,&entry));return selected;
}
int wmain(int argc,wchar_t** argv){
    if(argc<2||argc>3){std::cerr<<"Usage: RuntimeSamplingProbe PID [seconds]\n";return 2;}
    const DWORD processId=wcstoul(argv[1],nullptr,10);const int seconds=argc==3?std::clamp(_wtoi(argv[2]),1,45):20;
    Handle process{OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,processId)};
    if(!process.value){std::cerr<<"OpenProcess failed: "<<GetLastError()<<'\n';return 1;}
    wchar_t path[32768]{};DWORD length=32768;
    if(!QueryFullProcessImageNameW(process.value,0,path,&length))return 1;
    const std::wstring imagePath(path,length);const auto slash=imagePath.find_last_of(L"\\/");
    const auto basename=imagePath.substr(slash==std::wstring::npos?0:slash+1);
    if(basename!=L"PatchedBrowser.exe"&&basename!=L"PageScriptProbe.exe"){
        std::cerr<<"Only the engine diagnostic hosts may be sampled\n";return 2;
    }
    const auto symbolDirectory=imagePath.substr(0,slash);
    SymSetOptions(SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS|SYMOPT_LOAD_LINES|SYMOPT_NO_PROMPTS|SYMOPT_FAIL_CRITICAL_ERRORS);
    if(!SymInitializeW(process.value,symbolDirectory.c_str(),TRUE))return 1;
    const DWORD threadId=CpuThread(processId);
    Handle thread{OpenThread(THREAD_GET_CONTEXT|THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,threadId)};
    if(!thread.value){SymCleanup(process.value);return 1;}
    std::map<DWORD64,size_t> leafCounts,inclusiveCounts;
    size_t samples=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    while(std::chrono::steady_clock::now()<deadline&&WaitForSingleObject(process.value,0)==WAIT_TIMEOUT){
        std::vector<DWORD64> addresses;
        {
            ThreadPause pause(thread.value);
            CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
            if(pause.suspended&&GetThreadContext(thread.value,&context)){
                STACKFRAME64 frame{};frame.AddrPC.Offset=context.Rip;frame.AddrStack.Offset=context.Rsp;
                frame.AddrFrame.Offset=context.Rbp;frame.AddrPC.Mode=frame.AddrStack.Mode=frame.AddrFrame.Mode=AddrModeFlat;
                addresses.push_back(context.Rip);
                for(unsigned depth=0;depth<11;++depth){
                    if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process.value,thread.value,&frame,&context,
                        nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr)||!frame.AddrPC.Offset)break;
                    if(addresses.back()!=frame.AddrPC.Offset)addresses.push_back(frame.AddrPC.Offset);
                }
            }
        }
        if(!addresses.empty()){
            ++samples;++leafCounts[addresses.front()];
            for(const auto address:addresses)++inclusiveCounts[address];
        }
        Sleep(10);
    }
    const auto nameFor=[&](DWORD64 address){
        alignas(SYMBOL_INFO) unsigned char buffer[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
        auto* symbol=reinterpret_cast<SYMBOL_INFO*>(buffer);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=MAX_SYM_NAME;
        DWORD64 displacement=0;
        if(!SymFromAddr(process.value,address,&displacement,symbol))return std::string("unresolved");
        std::string name(symbol->Name,symbol->NameLen);IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD lineDisplacement=0;
        if(SymGetLineFromAddr64(process.value,address,&lineDisplacement,&line)){
            const std::string file=line.FileName;const auto separator=file.find_last_of("\\/");
            name+=" ["+file.substr(separator==std::string::npos?0:separator+1)+":"+std::to_string(line.LineNumber)+"]";
        }
        return name;
    };
    std::cout<<"PID "<<processId<<" thread "<<threadId<<" samples "<<samples<<'\n';
    for(const auto& definition:{std::make_pair("LEAF",&leafCounts),std::make_pair("INCLUSIVE",&inclusiveCounts)}){
        std::map<std::string,size_t> grouped;
        for(const auto& entry:*definition.second)grouped[nameFor(entry.first)]+=entry.second;
        std::vector<std::pair<std::string,size_t>> sorted(grouped.begin(),grouped.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second>b.second;});
        std::cout<<definition.first<<'\n';
        for(size_t index=0;index<std::min<size_t>(sorted.size(),35);++index)
            std::cout<<sorted[index].second<<" "<<sorted[index].first<<'\n';
    }
    SymCleanup(process.value);return samples?0:1;
}

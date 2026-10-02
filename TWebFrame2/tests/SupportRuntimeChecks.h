#include "../include/TWebFrame/BrowserContext.h"
#include "../src/OriginFileSystem.h"
#include <filesystem>

unsigned CheckSupportRuntimeLifetime(){
    unsigned failures=0;
    const auto check=[&](bool ok,const wchar_t* label){std::wcout<<(ok?L"PASS ":L"FAIL ")<<label<<L'\n';if(!ok)++failures;};
    Document document;document.Parse(L"<html><body></body></html>");JavaScriptRuntime runtime(document);std::wstring result,error;
    runtime.SetLocation(L"https://support.invalid");
    bool ok=runtime.Execute(L"window.cleaned=[];window.registry=new FinalizationRegistry(function(value){cleaned.push(value);});"
        L"(function(){var target={};target.self=target;registry.register(target,'released');})();",nullptr,&error);
    for(unsigned i=0;i<6000;++i)ok=runtime.Execute(L"(function(){var object={};object.self=object;})();",nullptr,&error)&&ok;
    runtime.RunTimers();ok=runtime.Execute(L"return cleaned.join(',');",&result,&error)&&ok;
    check(ok&&result==L"released",L"FinalizationRegistry schedules a cleanup for a collected cycle");
    unsigned polls=0;runtime.SetExecutionYieldHandler([&]{return ++polls<3;});
    ok=runtime.Execute(L"new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array([0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,8,1,0,10,9,1,7,0,3,64,12,0,11,11])));",nullptr,&error);
    runtime.SetExecutionYieldHandler({});check(!ok&&polls>=3&&error.find(L"interrupted")!=std::wstring::npos,L"Host can interrupt an infinite WebAssembly start function");
    const auto profile=std::filesystem::absolute(L"TWebFrame2/tests/artifacts/opfs-profile-"+std::to_wstring(GetCurrentProcessId()));
    {TWebFrame::BrowserContext context(profile.wstring());const auto files=context.OriginFiles(L"HTTPS://PERSIST.EXAMPLE:443/path");
        auto child=std::make_shared<OriginFileEntry>();child->directory=false;child->name=L"record";child->bytes={1,2,3};child->parent=files->root;files->root->children[child->name]=child;files->Save();}
    {TWebFrame::BrowserContext context(profile.wstring());const auto files=context.OriginFiles(L"https://persist.example"),other=context.OriginFiles(L"https://other.example");
        const auto found=files->root->children.find(L"record");check(found!=files->root->children.end()&&found->second->bytes==std::vector<unsigned char>({1,2,3})&&other->root->children.empty(),L"OPFS profile persists across contexts and isolates origins");}
    return failures;
}

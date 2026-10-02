#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <windows.h>

namespace TWebFrame::Internal {
struct OriginFileEntry {
    bool directory=true,removed=false,locked=false;
    std::wstring name;
    std::vector<unsigned char> bytes;
    std::map<std::wstring,std::shared_ptr<OriginFileEntry>> children;
    std::weak_ptr<OriginFileEntry> parent;
    double modified=0;
};
struct OriginFileSystem {
    std::recursive_mutex mutex;
    std::shared_ptr<OriginFileEntry> root=std::make_shared<OriginFileEntry>();
    std::filesystem::path file;
    size_t Usage(const std::shared_ptr<OriginFileEntry>& entry)const{
        size_t result=entry->bytes.size();for(const auto& child:entry->children)result+=Usage(child.second);return result;
    }
    static constexpr size_t quota=64*1024*1024;
    void Save()const{
        if(file.empty())return;
        std::error_code error;std::filesystem::create_directories(file.parent_path(),error);if(error)return;
        auto temporary=file;temporary+=L".tmp";std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        output.write("TWOPFS1",8);
        const auto number=[&](uint64_t value){output.write(reinterpret_cast<const char*>(&value),8);};
        std::function<void(const std::shared_ptr<OriginFileEntry>&)> write=[&](const auto& entry){
            output.put(entry->directory?1:0);number(entry->name.size());output.write(reinterpret_cast<const char*>(entry->name.data()),entry->name.size()*sizeof(wchar_t));
            output.write(reinterpret_cast<const char*>(&entry->modified),sizeof(entry->modified));number(entry->bytes.size());
            if(!entry->bytes.empty())output.write(reinterpret_cast<const char*>(entry->bytes.data()),entry->bytes.size());number(entry->children.size());
            for(const auto& child:entry->children)write(child.second);
        };
        write(root);output.flush();const bool success=output.good();output.close();
        if(success)MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    }
    void Load(){
        if(file.empty())return;std::ifstream input(file,std::ios::binary);char magic[8]{};input.read(magic,8);
        if(!input||std::string(magic,7)!="TWOPFS1")return;
        const auto number=[&](){uint64_t value=0;input.read(reinterpret_cast<char*>(&value),8);if(!input)throw std::runtime_error("Invalid OPFS snapshot");return value;};
        size_t usage=0,nodes=0;
        std::function<std::shared_ptr<OriginFileEntry>(unsigned)> read=[&](unsigned depth){
            if(depth>128||++nodes>100000)throw std::runtime_error("Invalid OPFS tree");auto entry=std::make_shared<OriginFileEntry>();
            const int kind=input.get();if(kind!=0&&kind!=1)throw std::runtime_error("Invalid OPFS entry");entry->directory=kind==1;
            const auto size=number();if(size>32768)throw std::runtime_error("Invalid OPFS name");entry->name.resize(static_cast<size_t>(size));
            input.read(reinterpret_cast<char*>(entry->name.data()),size*sizeof(wchar_t));input.read(reinterpret_cast<char*>(&entry->modified),sizeof(entry->modified));
            const auto bytes=number();if(bytes>quota-usage)throw std::runtime_error("OPFS snapshot exceeds quota");usage+=static_cast<size_t>(bytes);entry->bytes.resize(static_cast<size_t>(bytes));
            if(bytes)input.read(reinterpret_cast<char*>(entry->bytes.data()),bytes);const auto children=number();if(children>100000)throw std::runtime_error("Invalid OPFS children");
            for(uint64_t i=0;i<children;++i){auto child=read(depth+1);child->parent=entry;entry->children[child->name]=child;}if(!input)throw std::runtime_error("Invalid OPFS snapshot");return entry;
        };
        try{root=read(0);}catch(...){root=std::make_shared<OriginFileEntry>();}
    }
};
}

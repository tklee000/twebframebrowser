#include "DOM.h"
#include "CSS.h"
#include "Layout.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace TWebFrame::Internal;
static std::wstring Read(const wchar_t* path){
    std::ifstream file(std::filesystem::path(path),std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)),{});
    std::wstring text(MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0),L'\0');
    MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),text.data(),static_cast<int>(text.size()));return text;
}
static void Print(const LayoutBox* box,int depth=0){
    if(!box)return;
    if(box->node->type==NodeType::Element&&box->rect.y<500){
        std::wcout<<box->node->tag<<L" #"<<box->node->Attribute(L"id")<<L" ."<<box->node->Attribute(L"class")
            <<L" rect="<<box->rect.x<<L","<<box->rect.y<<L","<<box->rect.width<<L","<<box->rect.height;
        for(const auto* property:{L"display",L"margin-left",L"margin-right",L"padding-left",L"left",L"width",L"height",L"min-height",L"overflow-x",L"overflow-y",L"font-size",L"line-height",L"vertical-align",L"position",L"transform"})
            std::wcout<<L" "<<property<<L"="<<box->style.Get(property);
        std::wcout<<L'\n';
    }
    for(const auto& child:box->children)Print(child.get(),depth+1);
}
int wmain(int argc,wchar_t** argv){
    if(argc<3)return 2;
    Document document;StyleSheet css;std::wstring error;
    if(!document.Parse(Read(argv[1]),&error)||!css.Parse(Read(argv[2])+document.StyleText(),&error)){std::wcerr<<error;return 1;}
    for(const auto& node:document.QuerySelectorAll(L"a")){
        const auto before=css.Compute(node,nullptr,L"before");
        std::wcout<<L"pseudo ."<<node->Attribute(L"class")<<L" match="<<css.HasPseudoRulesFor(node,L"before")
            <<L" content="<<before.Get(L"content")<<L" width="<<before.Get(L"width")<<L'\n';
    }
    LayoutEngine layout(document,css);layout.Layout(960,660);Print(layout.Root());return 0;
}

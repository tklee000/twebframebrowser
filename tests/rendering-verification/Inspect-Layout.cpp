#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <objbase.h>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
using namespace TWebFrame::Internal;
namespace fs=std::filesystem;

// Read the unchanged input with the common DOM/CSS/layout engine. The selector
// only controls diagnostic output; it never alters style, geometry or painting.
int wmain(int argc,wchar_t** argv){
    if(argc<3){std::cerr<<"Inspect-Layout <document.html> <output.json> [dpi] [selector]\n";return 2;}
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try{
        const fs::path file=fs::absolute(argv[1]);
        const float scale=argc>3?std::stof(argv[3])/96.0f:1.0f;
        Document document;StyleSheet styles;std::wstring error;
        if(!document.Parse(RegressionIO::ReadText(file),&error))throw std::runtime_error("HTML parse failed");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,scale);
        for(const auto& link:document.QuerySelectorAll(L"link[rel=stylesheet]"))
            if(!styles.Parse(RegressionIO::ReadText(file.parent_path()/link->Attribute(L"href")),&error))throw std::runtime_error("CSS parse failed");
        for(const auto& node:document.QuerySelectorAll(L"style"))
            if(!styles.Parse(node->InnerText(),&error))throw std::runtime_error("inline CSS parse failed");
        LayoutEngine layout(document,styles);layout.Layout(800,600,scale);
        std::wostringstream out;out<<std::setprecision(9)<<L"{\"dpi\":"<<scale*96<<L",\"boxes\":[";
        bool first=true;
        for(const auto& node:document.QuerySelectorAll(argc>4?argv[4]:L"[data-probe]")){
            if(!first)out<<L',';first=false;LayoutRect rect{};layout.ReadElementRect(node,rect);
            const auto* box=layout.BoxFor(node);const auto style=layout.StyleForRendering(node);
            out<<L"{\"id\":"<<RegressionIO::Quote(node->Attribute(L"id"))<<L",\"tag\":"<<RegressionIO::Quote(node->tag)
               <<L",\"class\":"<<RegressionIO::Quote(node->Attribute(L"class"))<<L",\"rect\":["<<rect.x<<L','<<rect.y<<L','<<rect.width<<L','<<rect.height<<L']';
            if(box)out<<L",\"content\":["<<box->content.x<<L','<<box->content.y<<L','<<box->content.width<<L','<<box->content.height<<L"],\"internalScroll\":["<<box->scrollWidth<<L','<<box->scrollHeight<<L"],\"naturalHeight\":["<<box->naturalHeightReference<<L','<<box->naturalHeight<<L','<<box->naturalFloatBottom<<L']';
            out<<L",\"sizing\":{";bool firstProperty=true;
            for(const auto* property:{L"width",L"min-width",L"max-width",L"height",L"min-height",L"max-height",
                L"writing-mode",L"text-orientation",L"overflow-wrap",L"word-break",L"padding-left",L"padding-right",
                L"margin-left",L"margin-right",L"box-sizing",L"transform",L"column-count",L"column-width",L"column-fill",L"column-gap"}){
                if(!firstProperty)out<<L',';firstProperty=false;
                out<<RegressionIO::Quote(property)<<L':'<<RegressionIO::Quote(style.Get(property));
            }
            out<<L"},\"styles\":"<<layout.DumpRenderingStyleJson(style)<<L'}';
        }
        out<<L"]}";RegressionIO::WriteText(fs::absolute(argv[2]),out.str());
        std::cout<<"Layout diagnostics saved\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';CoUninitialize();return 1;}
    CoUninitialize();return 0;
}

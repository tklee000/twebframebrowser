#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
using namespace TWebFrame::Internal;

// Independent WebView2 coordinates and computed spacing are kept as inputs.
int wmain(int argc,wchar_t** argv){
    if(argc<2)return 2;
    const std::filesystem::path fixture=argv[1];unsigned checks=0,failed=0;
    const auto check=[&](bool ok,const std::wstring& name){++checks;if(!ok){++failed;std::wcerr<<L"FAIL "<<name<<L'\n';}};
    for(int dpi:{96,144}){
        Document document;StyleSheet styles;std::wstring error;
        check(document.Parse(RegressionIO::ReadText(fixture/L"index.html"),&error),L"parse HTML");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,dpi/96.0f);
        check(styles.Parse(RegressionIO::ReadText(fixture/L"style.css"),&error),L"parse CSS");
        LayoutEngine layout(document,styles);layout.Layout(800,600,dpi/96.0f);
        for(int pass=0;pass<2;++pass){
            std::wistringstream expected(RegressionIO::ReadText(fixture/(L"expected-"+std::to_wstring(dpi)+L".tsv")));
            std::wstring id;float x,y,width,height,word,letter;
            while(expected>>id>>x>>y>>width>>height>>word>>letter){
                LayoutRect rect{};const auto node=document.QuerySelector(L"#"+id);
                check(layout.ReadElementRect(node,rect),id+L" exists");
                check(std::abs(rect.x-x)<.02f&&std::abs(rect.y-y)<.02f&&
                    std::abs(rect.width-width)<.02f&&std::abs(rect.height-height)<.02f,id+L" reference rectangle");
                const auto style=layout.StyleForRendering(node);
                const auto length=[&](const wchar_t* name){return StyleSheet::Length(style.Get(name),1,800,0);};
                check(std::abs(length(L"word-spacing")-word)<.001f&&
                    std::abs(length(L"letter-spacing")-letter)<.001f,id+L" computed spacing");
            }
            layout.Relayout(800,600,dpi/96.0f);
        }
    }
    std::cout<<"Text spacing regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

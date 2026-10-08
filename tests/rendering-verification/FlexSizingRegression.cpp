#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
using namespace TWebFrame::Internal;

// Independent WebView2 coordinates for flexible lengths and min/max freezing.
int wmain(int argc,wchar_t** argv){
    if(argc<2)return 2;
    const std::filesystem::path fixture=argv[1];unsigned checks=0,failed=0;
    const auto check=[&](bool ok,const std::wstring& name){++checks;if(!ok){++failed;std::wcerr<<L"FAIL "<<name<<L'\n';}};
    for(bool renamed:{false,true})for(int dpi:{96,144}){
        Document document;StyleSheet styles;std::wstring error;
        auto source=RegressionIO::ReadText(fixture/L"index.html");
        if(renamed){
            // The same geometry must survive changing every input identifier.
            // Expected data is read only here, never by the production engine.
            size_t position=0;
            while((position=source.find(L"id=\"",position))!=std::wstring::npos){
                source.insert(position+4,L"renamed-");position+=12;
            }
        }
        check(document.Parse(source,&error),L"parse HTML");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,dpi/96.0f);
        check(styles.Parse(RegressionIO::ReadText(fixture/L"style.css"),&error),L"parse CSS");
        LayoutEngine layout(document,styles);layout.Layout(800,600,dpi/96.0f);
        for(int pass=0;pass<2;++pass){
            std::wistringstream expected(RegressionIO::ReadText(fixture/(L"expected-"+std::to_wstring(dpi)+L".tsv")));
            std::wstring id;float x,y,width,height;unsigned probes=0;
            while(expected>>id>>x>>y>>width>>height){
                ++probes;
                LayoutRect rect{};const auto node=document.QuerySelector(L"#"+
                    (renamed?std::wstring(L"renamed-"):std::wstring{})+id);
                check(layout.ReadElementRect(node,rect),id+L" exists");
                check(std::abs(rect.x-x)<.02f&&std::abs(rect.y-y)<.02f&&
                    std::abs(rect.width-width)<.02f&&std::abs(rect.height-height)<.02f,id+L" reference rectangle");

            }
            check(probes==64,L"complete independent probe matrix");
            layout.Relayout(800,600,dpi/96.0f);
        }
    }
    std::cout<<"Flex sizing regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

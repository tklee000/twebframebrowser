#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
using namespace TWebFrame::Internal;

// Expected geometry is captured independently from WebView2, at each DPI.
int wmain(int argc,wchar_t** argv){
    if(argc<2)return 2;
    unsigned checks=0,failed=0;
    const auto check=[&](bool ok,const std::wstring& name){++checks;if(!ok){++failed;std::wcerr<<L"FAIL "<<name<<L'\n';}};
    const std::filesystem::path base=argv[1];
    for(const auto& fixtureCase:std::vector<std::pair<std::filesystem::path,unsigned>>{{base,97},{base.parent_path()/L"intrinsic-contributions",32}}){
    const auto& fixture=fixtureCase.first;
    for(int dpi:{96,144}){
        Document document;StyleSheet styles;std::wstring error;
        check(document.Parse(RegressionIO::ReadText(fixture/L"index.html"),&error),L"parse HTML");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,dpi/96.0f);
        check(styles.Parse(RegressionIO::ReadText(fixture/L"style.css"),&error),L"parse CSS");
        LayoutEngine layout(document,styles);layout.Layout(800,600,dpi/96.0f);
        for(int pass=0;pass<2;++pass){
            std::wistringstream expected(RegressionIO::ReadText(fixture/(L"expected-"+std::to_wstring(dpi)+L".tsv")));
            std::wstring id;float x,y,width,height;unsigned probes=0;
            while(expected>>id>>x>>y>>width>>height){
                ++probes;LayoutRect rect{};
                check(layout.ReadElementRect(document.QuerySelector(L"#"+id),rect),id+L" exists");
                const bool equal=std::abs(rect.x-x)<.02f&&std::abs(rect.y-y)<.02f&&
                    std::abs(rect.width-width)<.02f&&std::abs(rect.height-height)<.02f;
                check(equal,id+L" reference rectangle");
                if(!equal)std::wcerr<<L"  actual "<<rect.x<<L","<<rect.y<<L","<<rect.width<<L","<<rect.height
                    <<L" expected "<<x<<L","<<y<<L","<<width<<L","<<height<<L'\n';
            }
            check(probes==fixtureCase.second,L"complete independent probe matrix");
            layout.Relayout(800,600,dpi/96.0f);
        }
    }
    }
    std::cout<<"Intrinsic width regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

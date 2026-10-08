#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
using namespace TWebFrame::Internal;

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
            std::wstring id;float x,y,width,height,scrollWidth,scrollHeight,clientWidth,clientHeight;
            while(expected>>id>>x>>y>>width>>height>>scrollWidth>>scrollHeight>>clientWidth>>clientHeight){
                const auto node=document.QuerySelector(L"#"+id);LayoutRect rect{};
                check(layout.ReadElementRect(node,rect),id+L" exists");
                check(std::abs(rect.x-x)<.02f&&std::abs(rect.y-y)<.02f&&std::abs(rect.width-width)<.02f&&std::abs(rect.height-height)<.02f,id+L" reference rectangle");
                const auto sizes=layout.ReadElementSizes(node);
                check(std::abs(sizes.scrollWidth-scrollWidth)<.02f&&std::abs(sizes.scrollHeight-scrollHeight)<.02f&&
                    std::abs(sizes.clientWidth-clientWidth)<.02f&&std::abs(sizes.clientHeight-clientHeight)<.02f,id+L" reference scroll sizes");
            }
            const auto root=layout.ReadElementSizes(document.QuerySelector(L"html"));
            check(root.clientWidth==800&&root.clientHeight==600&&root.scrollWidth==800&&root.scrollHeight==600,L"visible overflow has no false page scrollbar");
            layout.Relayout(800,600,dpi/96.0f);
        }
        const auto scroller=document.QuerySelector(L"#scroller"),wide=document.QuerySelector(L"#wide");
        scroller->scrollLeft=20;scroller->scrollTop=5;
        for(int pass=0;pass<2;++pass){
            layout.Relayout(800,600,dpi/96.0f);LayoutRect rect{};layout.ReadElementRect(wide,rect);
            check(std::abs(rect.x+5)<.02f&&std::abs(rect.y-110)<.02f,L"both scroll offsets survive gutter resolution");
        }
    }
    std::cout<<"Overflow padding regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

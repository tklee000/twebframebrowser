#include "DOM.h"
#include "CSS.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
using namespace TWebFrame::Internal;

int wmain(int argc,wchar_t** argv){
    if(argc<2)return 2;
    const std::filesystem::path fixtures=argv[1];unsigned checks=0,failed=0;
    const auto check=[&](bool ok,const std::wstring& name){++checks;if(!ok){++failed;std::wcerr<<L"FAIL "<<name<<L'\n';}};
    struct Matrix {const wchar_t* name;unsigned rectangles,hits;};
    for(const auto& matrix:{Matrix{L"clip-path",48,1008},Matrix{L"clip-path-rects",48,1008},
        Matrix{L"hit-test-boundaries",8,336}})for(bool renamed:{false,true})for(int dpi:{96,144}){
        const auto* name=matrix.name;
        const auto fixture=fixtures/name;
        Document document;StyleSheet styles;std::wstring error;
        auto source=RegressionIO::ReadText(fixture/L"index.html");
        if(renamed){size_t p=0;while((p=source.find(L"id=\"",p))!=std::wstring::npos){source.insert(p+4,L"renamed-");p+=12;}}
        check(document.Parse(source,&error),L"parse HTML");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,dpi/96.0f);
        check(styles.Parse(RegressionIO::ReadText(fixture/L"style.css"),&error),L"parse CSS");
        LayoutEngine layout(document,styles);layout.Layout(800,600,dpi/96.0f);
        for(int pass=0;pass<2;++pass){
            std::wistringstream expected(RegressionIO::ReadText(fixture/(L"expected-"+std::to_wstring(dpi)+L".tsv")));
            std::wstring id;float x,y,width,height;unsigned probes=0;
            while(expected>>id>>x>>y>>width>>height){
                ++probes;LayoutRect rect{};
                const auto node=document.QuerySelector(L"#"+(renamed?std::wstring(L"renamed-"):std::wstring{})+id);
                check(layout.ReadElementRect(node,rect),id+L" exists");
                check(std::abs(rect.x-x)<.02f&&std::abs(rect.y-y)<.02f&&std::abs(rect.width-width)<.02f&&std::abs(rect.height-height)<.02f,id+L" unclipped reference rectangle");
            }
            check(probes==matrix.rectangles,L"complete independent geometry matrix");
            std::wistringstream hits(RegressionIO::ReadText(fixture/(L"hits-"+std::to_wstring(dpi)+L".tsv")));
            probes=0;
            while(hits>>x>>y>>id){
                ++probes;const auto hit=layout.HitTest(x,y);
                const auto wanted=id==L"__none__"||!renamed?id:L"renamed-"+id;
                check((hit?hit->Attribute(L"id"):L"__none__")==wanted,
                    std::wstring(name)+L" hit "+std::to_wstring(x)+L","+std::to_wstring(y)+L" -> "+wanted+L" actual "+(hit?hit->Attribute(L"id"):L"__none__"));
            }
            check(probes==matrix.hits,L"complete independent hit matrix");
            layout.Relayout(800,600,dpi/96.0f);
        }
    }
    std::cout<<"CSS clip-path regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

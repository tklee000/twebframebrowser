#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <filesystem>
#include <iostream>
using namespace TWebFrame::Internal;

// Expected rectangles were independently read from WebView2 at 96/144 DPI.
// Cases cover fitting collapsed margins and real block/grid/flex overflow.
int wmain(int argc,wchar_t** argv){
    if(argc<2)return 2;
    const std::filesystem::path fixture=argv[1];
    unsigned checks=0,failed=0;
    const auto check=[&](bool ok,const std::wstring& name){++checks;if(!ok){++failed;std::wcerr<<L"FAIL "<<name<<L'\n';}};
    struct Expected {const wchar_t* id;float x,y,width,height;bool gutter=false,rtl=false;};
    const Expected expected[]={
        {L"fit-block",0,0,160,85},{L"fit-first",10,20,140,15},{L"fit-second",10,45,140,15},
        {L"overflow-block",0,85,160,60},{L"overflow-child",10,95,125,100,true},
        {L"overflow-rtl",0,145,160,60},{L"rtl-child",25,155,125,100,true,true},
        {L"fit-grid",0,205,160,60},{L"grid-first",10,215,65,40},{L"grid-second",85,215,65,40},
        {L"overflow-grid",0,265,160,40},{L"grid-overflow-first",10,275,57.5f,40,true},
        {L"overflow-flex",0,305,160,40},{L"flex-first",10,315,57.5f,40,true}
    };
    for(float scale:{1.0f,1.5f}){
        Document document;StyleSheet styles;std::wstring error;
        check(document.Parse(RegressionIO::ReadText(fixture/L"index.html"),&error),L"parse HTML");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,scale);
        check(styles.Parse(RegressionIO::ReadText(fixture/L"style.css"),&error),L"parse CSS");
        LayoutEngine layout(document,styles);layout.Layout(800,600,scale);
        for(const auto& e:expected){
            const auto node=document.QuerySelector(L"#"+std::wstring(e.id));LayoutRect rect;
            check(layout.ReadElementRect(node,rect),std::wstring(e.id)+L" exists");
            const float delta=scale==1.5f&&e.gutter?1.0f/3:0;
            const bool split=e.width<100;
            check(std::abs(rect.x-(e.x+(e.rtl?delta:0)))<.01f,std::wstring(e.id)+L" x");
            check(std::abs(rect.y-e.y)<.01f,std::wstring(e.id)+L" y");
            check(std::abs(rect.width-(e.width-(split?delta/2:delta)))<.01f,std::wstring(e.id)+L" width");
            check(std::abs(rect.height-e.height)<.01f,std::wstring(e.id)+L" height");
        }
        for(const auto* selector:{L"#fit-block",L"#fit-grid"}){
            const auto* box=layout.BoxFor(document.QuerySelector(selector));
            check(box&&!box->verticalGutterReserved,std::wstring(selector)+L" no spurious scrollbar");
        }
        const auto parent=document.QuerySelector(L"#overflow-block"),child=document.QuerySelector(L"#overflow-child");
        parent->scrollTop=20;layout.Relayout(800,600,scale);LayoutRect first,second;
        layout.ReadElementRect(child,first);layout.Relayout(800,600,scale);layout.ReadElementRect(child,second);
        check(std::abs(first.y-75)<.01f&&std::abs(second.y-first.y)<.01f,L"scroll positions remain stable across gutter reflow");
    }
    std::cout<<"Automatic overflow regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

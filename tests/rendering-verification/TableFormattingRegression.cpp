#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
using namespace TWebFrame::Internal;

// The expected TSVs are independent WebView2 measurements of this small
// fixture at both DPIs, retained as regression inputs rather than capture logs.
int wmain(int argc,wchar_t** argv){
    if(argc<2)return 2;
    const std::filesystem::path fixture=argv[1];unsigned checks=0,failed=0;
    const auto check=[&](bool ok,const std::wstring& name){++checks;if(!ok){++failed;std::wcerr<<L"FAIL "<<name<<L'\n';}};
    for(int dpi:{96,144}){
        Document document;StyleSheet styles;std::wstring error;
        check(document.Parse(RegressionIO::ReadText(fixture/L"index.html"),&error),L"parse HTML");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,dpi/96.0f);
        check(styles.Parse(RegressionIO::ReadText(fixture/L"style.css"),&error),L"parse CSS");
        const auto elementCount=document.QuerySelectorAll(L"*").size();
        LayoutEngine layout(document,styles);layout.Layout(800,600,dpi/96.0f);
        for(int pass=0;pass<2;++pass){
            std::wistringstream expected(RegressionIO::ReadText(fixture/(L"expected-"+std::to_wstring(dpi)+L".tsv")));
            std::wstring id;float x,y,width,height;
            while(expected>>id>>x>>y>>width>>height){
                LayoutRect rect{};const auto node=document.QuerySelector(L"#"+id);
                check(layout.ReadElementRect(node,rect),id+L" exists");
                check(std::abs(rect.x-x)<.02f&&std::abs(rect.y-y)<.02f&&
                    std::abs(rect.width-width)<.02f&&std::abs(rect.height-height)<.02f,id+L" reference rectangle");
            }
            check(document.QuerySelectorAll(L"*").size()==elementCount,L"anonymous boxes do not change DOM membership");
            layout.Relayout(800,600,dpi/96.0f);
        }
        const auto* cell=layout.BoxFor(document.QuerySelector(L"#direct-a"));
        check(cell&&cell->parent&&cell->parent->pseudo==L"anonymous-table"&&
            cell->parent->style.Is(L"display",L"table-row"),L"missing row is generated in layout tree");
        const auto* block=layout.BoxFor(document.QuerySelector(L"#block-a"));
        check(block&&block->parent&&block->parent->style.Is(L"display",L"table-cell")&&
            block->parent->style.Get(L"border-spacing")==L"4px 6px",L"anonymous cell inherits table spacing");
        // Selector matching and non-inherited decorations must never leak
        // from an authored element into its anonymous formatting wrappers.
        auto parent=styles.Compute(document.QuerySelector(L"#direct"));
        (*parent.values)[L"background"]=L"red";(*parent.values)[L"padding"]=L"12px";
        (*parent.values)[L"transform"]=L"scale(2)";(*parent.values)[L"--example"]=L"9px";
        const auto anonymous=StyleSheet::AnonymousStyle(parent,L"table-row");
        check(anonymous.Get(L"background").empty()&&anonymous.Get(L"padding").empty()&&
            anonymous.Get(L"transform").empty(),L"anonymous boxes use initial decorations");
        check(anonymous.Get(L"--example")==L"9px"&&anonymous.Get(L"font-size")==parent.Get(L"font-size"),L"anonymous boxes inherit computed text and variables");
    }
    std::cout<<"Table formatting regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

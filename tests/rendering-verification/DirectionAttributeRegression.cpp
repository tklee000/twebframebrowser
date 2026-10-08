#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "CaptureIO.h"
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
using namespace TWebFrame::Internal;

// Independent WebView2 coordinates for HTML direction attributes and author CSS precedence.
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
        auto css=RegressionIO::ReadText(fixture/L"style.css");
        if(renamed){
            std::vector<std::wstring> names;
            for(const auto& node:document.QuerySelectorAll(L"[id]"))names.push_back(node->Attribute(L"id").substr(8));
            std::sort(names.begin(),names.end(),[](const auto& a,const auto& b){return a.size()>b.size();});
            for(const auto& name:names){
                const auto selector=L"#"+name;size_t position=0;
                while((position=css.find(selector,position))!=std::wstring::npos){
                    css.insert(position+1,L"renamed-");position+=selector.size()+8;
                }
            }
        }
        check(styles.Parse(css,&error),L"parse CSS");
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
            check(probes==76,L"complete independent probe matrix");
            std::wistringstream expectedStyles(RegressionIO::ReadText(fixture/(L"styles-"+std::to_wstring(dpi)+L".tsv")));
            std::wstring direction,bidi,paddingLeft,paddingRight;
            unsigned styleProbes=0;
            while(expectedStyles>>id>>direction>>bidi>>paddingLeft>>paddingRight){
                ++styleProbes;
                const auto node=document.QuerySelector(L"#"+(renamed?std::wstring(L"renamed-"):std::wstring{})+id);
                const auto computed=layout.StyleForRendering(node);
                check(computed.Get(L"direction")==direction,id+L" direction");
                check(computed.Get(L"unicode-bidi")==bidi,id+L" bidi UA/cascade");
                check(std::abs(StyleSheet::Length(computed.Get(L"padding-left"),0,0,0)-
                    StyleSheet::Length(paddingLeft,0,0,0))<.001f,id+L" logical left padding");
                check(std::abs(StyleSheet::Length(computed.Get(L"padding-right"),0,0,0)-
                    StyleSheet::Length(paddingRight,0,0,0))<.001f,id+L" logical right padding");
            }
            check(styleProbes==76,L"complete independent computed-style matrix");
            layout.Relayout(800,600,dpi/96.0f);
        }
    }
    std::cout<<"HTML direction regression: "<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

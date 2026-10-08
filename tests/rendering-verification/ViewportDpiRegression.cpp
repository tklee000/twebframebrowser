#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include <cmath>
#include <iostream>
using namespace TWebFrame::Internal;

int wmain(){
    unsigned checks=0,failures=0;
    const auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<message<<"\n";}};
    struct Scenario{const wchar_t* root;const wchar_t* body;const wchar_t* size;float clientWidth,clientHeight,childWidth,childX;};
    const Scenario scenarios[]={
        {L"",L"",L"height:1000px",785,600,785,0},
        {L"",L"",L"height:100px",800,600,800,0},
        {L"",L"",L"height:600px",800,600,800,0},
        {L"",L"",L"height:1000px;width:790px",785,585,790,0},
        {L"",L"overflow-y:scroll",L"height:100px",785,600,785,0},
        {L"",L"overflow:hidden",L"height:1000px",800,600,800,0},
        {L"scrollbar-width:none",L"",L"height:1000px",800,600,800,0},
        {L"scrollbar-gutter:stable",L"",L"height:100px",785,600,785,0},
        {L"scrollbar-gutter:stable both-edges",L"",L"height:100px",770,600,770,15},
        {L"",L"",L"height:1000px;width:100vw",785,585,800,0}
    };
    for(float scale:{1.0f,1.5f})for(const auto& s:scenarios){
        Document document;StyleSheet styles;std::wstring error;
        check(document.Parse(L"<!doctype html><html><head></head><body><main></main></body></html>",&error),"parse document");
        styles.SetViewport(800,600);styles.SetDisplay(1920,1080,scale);
        check(styles.Parse(L"html{"+std::wstring(s.root)+L"}body{margin:0;"+s.body+L"}main{width:100%;"+s.size+L"}",&error),"parse styles");
        LayoutEngine layout(document,styles);layout.Layout(800,600,scale);
        const auto sizes=layout.ReadElementSizes(document.QuerySelector(L"html"));
        LayoutRect rect;check(layout.ReadElementRect(document.QuerySelector(L"main"),rect),"read main rect");
        const float expectedClientWidth=scale==1.5f&&s.clientWidth==770?769:s.clientWidth;
        const float expectedChildWidth=scale==1.5f?(s.childWidth==785?784.666667f:s.childWidth==770?769.333333f:s.childWidth):s.childWidth;
        const float expectedChildX=scale==1.5f&&s.childX==15?15.333333f:s.childX;
        check(std::abs(sizes.clientWidth-expectedClientWidth)<.01f,"viewport client width excludes reserved gutters");
        check(std::abs(sizes.clientHeight-s.clientHeight)<.01f,"viewport client height excludes horizontal bar");
        check(std::abs(rect.width-expectedChildWidth)<.01f,"percentage uses client width while vw uses viewport width");
        check(std::abs(rect.x-expectedChildX)<.01f,"both edges reserve the start gutter");
        if(&s==&scenarios[0]){
            layout.Relayout(500,600,scale);
            check(layout.ReadElementRect(document.QuerySelector(L"main"),rect)&&std::abs(rect.width-(scale==1.5f?484.666667f:485))<.01f,"resize recomputes percentage containing block");
        }
    }
    std::cout<<"Viewport/DPI regression: "<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}

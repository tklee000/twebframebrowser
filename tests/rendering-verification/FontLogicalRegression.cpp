#include "CSS.h"
#include "DOM.h"
#include <cmath>
#include <iostream>
#include <functional>
using namespace TWebFrame::Internal;

int wmain(){
    unsigned tested=0,failed=0;
    const auto check=[&](bool ok,const char* name){++tested;if(!ok){++failed;std::cerr<<"FAIL "<<name<<"\n";}};
    for(float dpi:{96.0f,144.0f}){
        Document document;std::wstring error;
        check(document.Parse(L"<!doctype html><html><head></head><body><div class='parent'><span class='em'>em</span><span class='percent'>percent</span><span class='calc'>calc</span><span class='inherit'>inherit</span><h1>heading</h1></div><div class='ltr'>ltr</div><div class='rtl'>rtl</div><div class='order'>order</div><div class='important'>important</div><div class='vertical'>vertical</div><div class='rem'>rem</div><div class='literal'>literal</div></body></html>",&error),"parse HTML");
        StyleSheet sheet;sheet.SetViewport(800,600);sheet.SetDisplay(1920,1080,dpi/96);
        check(sheet.Parse(L":root{font-size:20px}body{font-size:14px}.parent{font-size:30px}.em{font-size:.7em;line-height:150%}.percent{font-size:75%}.calc{font-size:calc(1em + 2px)}.inherit{font-size:inherit}.ltr{direction:ltr;padding-inline:7px 11px;margin-block:3px 5px}.rtl{direction:rtl;padding-inline-start:13px;padding-inline-end:17px}.order{padding-left:19px;padding-inline-start:23px;padding-left:29px}.important{padding-left:31px!important;padding-inline-start:37px}.vertical{writing-mode:vertical-rl;padding-inline:2px 4px;padding-block:6px 8px}.rem{width:calc(2rem + 3px);font-size:1.25rem}.literal{content:'2rem';background-image:url(thing2rem.png)}@media(min-resolution:1.5dppx){.ltr{margin-block-start:9px}}",&error),"parse CSS");
        const std::function<ComputedStyle(const std::shared_ptr<Node>&)> compute=[&](const std::shared_ptr<Node>& node){
            const auto parent=node->parent.lock();
            if(!parent||parent->type==NodeType::Document)return sheet.Compute(node);
            const auto parentStyle=compute(parent);return sheet.Compute(node,&parentStyle);
        };
        const auto numeric=[&](const wchar_t* selector,const wchar_t* property,float expected){
            const auto style=compute(document.QuerySelector(selector));
            const auto value=style.Get(property);const float actual=StyleSheet::Length(value,16,800,-1000,16);
            return std::abs(actual-expected)<.001f;
        };
        check(numeric(L".em",L"font-size",21),"em uses parent computed font");
        check(numeric(L".em",L"line-height",31.5f),"percentage line-height uses element computed font");
        check(numeric(L".percent",L"font-size",22.5f),"percent uses parent font");
        check(numeric(L".calc",L"font-size",32),"calc em uses parent font");
        check(numeric(L".inherit",L"font-size",30),"inherit is computed length");
        check(numeric(L"h1",L"font-size",60),"UA heading uses inherited font basis");
        check(numeric(L".rem",L"font-size",25),"rem font uses root computed font");
        check(numeric(L".rem",L"width",43),"rem in calc uses root font");
        check(numeric(L".ltr",L"padding-left",7),"LTR inline start");
        check(numeric(L".ltr",L"padding-right",11),"LTR inline end");
        check(numeric(L".ltr",L"margin-top",dpi==144?9:3),"block start and resolution query");
        check(numeric(L".ltr",L"margin-bottom",5),"block end");
        check(numeric(L".rtl",L"padding-right",13),"RTL inline start");
        check(numeric(L".rtl",L"padding-left",17),"RTL inline end");
        check(numeric(L".order",L"padding-left",29),"physical/logical declaration order");
        check(numeric(L".important",L"padding-left",31),"physical important outranks logical");
        check(numeric(L".vertical",L"padding-top",2),"vertical inline start");
        check(numeric(L".vertical",L"padding-bottom",4),"vertical inline end");
        check(numeric(L".vertical",L"padding-right",6),"vertical block start");
        check(numeric(L".vertical",L"padding-left",8),"vertical block end");
        const auto literal=compute(document.QuerySelector(L".literal"));
        check(literal.Get(L"content")==L"'2rem'","quoted literal unchanged");
        check(literal.Get(L"background-image")==L"url(thing2rem.png)","URL unchanged");
        Document cases;
        check(cases.Parse(L"<!doctype html><html><head></head><body><div class='rootrem'></div><div class='logical-order'></div><div class='axes-last'></div><div class='axes-layer'></div><div class='inline' style='padding-left:1px;padding-inline-start:2px;padding-left:3px'></div><div class='unitless'><span class='child'></span></div><div class='length-line'><span class='child'></span></div><div class='logical-inherit'><span class='child'></span></div><div class='invalid-logical'></div><div class='all-parent'><span class='all-child'></span></div></body></html>",&error),"parse additional cascade cases");
        StyleSheet additional;additional.SetViewport(800,600);additional.SetDisplay(1920,1080,dpi/96);
        check(additional.Parse(L":root{font-size:1.25rem}.rootrem{width:calc(1e1rem + 2px);--value:3rem;content:'4rem';background-image:url(5rem.png)}.logical-order{padding-inline-start:7px;padding-left:9px;padding-inline-start:11px}.axes-last{padding-inline-start:13px;direction:rtl}.unitless{font-size:20px;line-height:1.5}.unitless>.child{font-size:10px}.length-line{font-size:20px;line-height:calc(1em + 2px)}.length-line>.child{font-size:10px}.logical-inherit{padding-left:17px}.logical-inherit>.child{direction:rtl;padding-inline-start:inherit}.invalid-logical{padding-left:23px;padding-inline:3 4;margin-left:29px;margin-inline:5 6}@layer low,high;@layer low{.axes-layer{direction:rtl}}@layer high{.axes-layer{direction:revert-layer;padding-inline-start:19px}}",&error),"parse additional cascade styles");
        const std::function<ComputedStyle(const std::shared_ptr<Node>&)> extra=[&](const std::shared_ptr<Node>& node){
            const auto ancestor=node->parent.lock();if(!ancestor||ancestor->type==NodeType::Document)return additional.Compute(node);
            const auto style=extra(ancestor);return additional.Compute(node,&style);
        };
        const auto value=[&](const wchar_t* selector,const wchar_t* property){return extra(cases.QuerySelector(selector)).Get(property);};
        const auto length=[&](const wchar_t* selector,const wchar_t* property,float expected){return std::abs(StyleSheet::Length(value(selector,property),16,800,-1000)-expected)<.001f;};
        check(length(L"html",L"font-size",20),"root rem uses initial font basis");
        check(length(L".rootrem",L"width",202),"exponent dimension token rem");
        check(value(L".rootrem",L"--value")==L"3rem","custom-property token stream remains unchanged");
        check(length(L".logical-order",L"padding-left",11),"logical declaration following physical wins");
        check(length(L".inline",L"padding-left",3),"inline repeated physical declaration retains order");
        check(length(L".axes-last",L"padding-right",13),"direction declared after logical property defines axes");
        check(length(L".axes-layer",L"padding-right",19),"revert-layer direction resolves before logical mapping");
        check(value(L".unitless>.child",L"line-height")==L"1.5","unitless line-height inherits as a number");
        check(length(L".length-line>.child",L"line-height",22),"computed line-height length inherits without rescaling");
        check(length(L".logical-inherit>.child",L"padding-right",17),"logical inherit uses the parent's own axes");
        check(length(L".invalid-logical",L"padding-left",23),"invalid unitless logical padding preserves earlier declaration");
        check(length(L".invalid-logical",L"margin-left",29),"invalid unitless logical margin preserves earlier declaration");
        check(additional.Parse(L".all-parent{padding-left:31px;padding-right:37px}.all-child{padding-inline-start:5px;direction:rtl;all:inherit}",&error),"parse all inheritance case");
        check(length(L".all-child",L"padding-right",37),"all inherit resets physical counterparts after logical counterparts");
        check(cases.Parse(L"<!doctype html><html><head></head><body><div class='border-default'></div><div class='border-side'></div><div class='border-logical'></div><div class='border-reset'></div><div class='border-var'></div><div class='border-explicit'></div></body></html>",&error),"parse border shorthand cases");
        check(additional.Parse(L"body{color:#162e40}.border-default{border:1px solid}.border-side{border-left:2px solid}.border-logical{direction:rtl;border-inline-start:3px solid}.border-reset{border:1px solid red;border:2px solid}.border-var{--edge:1px solid;border:var(--edge)}.border-explicit{border:1px solid #123456}",&error),"parse border shorthand styles");
        check(value(L".border-default",L"border-top-color")==L"currentcolor","omitted border color defaults to currentcolor");
        check(value(L".border-side",L"border-left-color")==L"currentcolor","side shorthand defaults to currentcolor");
        check(value(L".border-logical",L"border-right-color")==L"currentcolor","logical shorthand defaults to currentcolor");
        check(value(L".border-reset",L"border-top-color")==L"currentcolor","later shorthand resets an earlier color");
        check(value(L".border-var",L"border-top-color")==L"currentcolor","variable shorthand defaults to currentcolor");
        check(value(L".border-explicit",L"border-top-color")==L"#123456","explicit border color is retained");
    }
    std::cout<<"Font/logical regression: "<<tested<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

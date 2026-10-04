#include "../src/CSS.h"
#include "../src/DOM.h"
#include "../src/Layout.h"
#include "../src/JavaScript.h"
#include "RegressionIO.h"
#include <WebView2.h>
#include <wrl.h>
#include <ole2.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

using namespace TWebFrame::Internal;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace {
int failures=0,checks=0;
void Check(bool ok,const std::wstring& name){++checks;if(!ok){++failures;std::wcerr<<L"FAIL: "<<name<<L'\n';}}
struct Fixture {std::wstring name,css,html;double expected;bool color=false;};
const Fixture fixtures[]={
    {L"string delimiters",L"#target{--text:'/*literal*/; }';width:31px}",L"<div id='target'></div>",31},
    {L"escaped declaration",LR"CSS(#target{w\69 dth:32px})CSS",L"<div id='target'></div>",32},
    {L"escaped leading digit",LR"CSS(.\31 target{width:33px})CSS",L"<div id='target' class='1target'></div>",33},
    {L"escaped class",LR"CSS(.a\:b{width:34px})CSS",L"<div id='target' class='a:b'></div>",34},
    {L"escaped selector comma",LR"CSS(.a\,b{width:35px})CSS",L"<div id='target' class='a,b'></div>",35},
    {L"inline quoted semicolon",L"",L"<div id='target' style='--data:&quot;x;y&quot;;width:36px'></div>",36},
    {L"inline important duplication",L"",L"<div id='target' style='width:37px ! IMPORTANT; width:99px'></div>",37},
    {L"stylesheet important trivia",L"#target{width:38px ! /**/ IMPORTANT; width:99px}",L"<div id='target'></div>",38},
    {L"is complex selector",L":is(main > .item, .missing){width:39px}",L"<main><div id='target' class='item'></div></main>",39},
    {L"where zero specificity",L".item{width:40px}:where(#target){width:99px}",L"<div id='target' class='item'></div>",40},
    {L"is maximum specificity",L":is(#missing,.item){width:41px}.item.item{width:99px}",L"<div id='target' class='item'></div>",41},
    {L"not selector list",L"div:not(.missing,main > .excluded){width:42px}",L"<div id='target'></div>",42},
    {L"nested functional selector",L"div:not(:is(.missing,.other)){width:43px}",L"<div id='target'></div>",43},
    {L"has child anchoring",L"main:has(> .item){width:44px}",L"<main id='target'><span class='item'></span></main>",44},
    {L"has adjacent sibling",L"main:has(+ .item){width:45px}",L"<main id='target'></main><span class='item'></span>",45},
    {L"nth-last-child",L"div:nth-last-child(2){width:46px}",L"<main><div></div><div id='target'></div><div></div></main>",46},
    {L"nth filtered siblings",L"div:nth-child(2 of .item){width:47px}",L"<main><div class='item'></div><p></p><div id='target' class='item'></div></main>",47},
    {L"language inheritance",L"div:lang(ko){width:48px}",L"<main lang='ko-KR'><div id='target'></div></main>",48},
    {L"attribute insensitive flag",L"[data-value='a,b > c' i]{width:49px}",L"<div id='target' data-value='A,B > C'></div>",49},
    {L"nesting parent ampersand",L"main{& > .item{width:50px}}",L"<main><div id='target' class='item'></div></main>",50},
    {L"implicit nesting",L"main{.item{width:51px}}",L"<main><div id='target' class='item'></div></main>",51},
    {L"nesting parent specificity",L"#absent,main{& .item{width:52px}}main .item{width:99px}",L"<main><div id='target' class='item'></div></main>",52},
    {L"nesting declaration order",L"#target{width:99px;&{width:98px}width:53px}",L"<div id='target'></div>",53},
    {L"nested media",L"#target{width:99px;@media (width >= 700px){width:54px}}",L"<div id='target'></div>",54},
    {L"layer order outranks specificity",L"@layer base,theme;@layer theme{div{width:55px}}@layer base{#target{width:99px}}",L"<div id='target'></div>",55},
    {L"unlayered wins normal",L"div{width:56px}@layer base{#target{width:99px}}",L"<div id='target'></div>",56},
    {L"layer important reversed",L"@layer base,theme;@layer base{div{width:57px!important}}@layer theme{#target{width:99px!important}}",L"<div id='target'></div>",57},
    {L"nested layer order",L"@layer base{div{width:58px}@layer child{#target{width:99px}}}",L"<div id='target'></div>",58},
    {L"revert layer",L"@layer base,theme;@layer base{div{width:59px}}@layer theme{div{width:99px}#target{width:revert-layer}}",L"<div id='target'></div>",59},
    {L"inline specificity",L"#target#target{width:99px}",L"<div id='target' style='width:60px'></div>",60},
    {L"inline important layer",L"@layer base{#target{width:99px!important}}",L"<div id='target' style='width:61px!important'></div>",61},
    {L"nested variable fallback",L"#target{width:var(--missing,var(--also-missing,62px))}",L"<div id='target'></div>",62},
    {L"cyclic variable fallback",L"#target{--a:var(--b);--b:var(--a);width:var(--a,63px)}",L"<div id='target'></div>",63},
    {L"unused recursive fallback",L"#target{--ok:99px;--a:var(--ok,var(--a));width:var(--a,64px)}",L"<div id='target'></div>",99},
    {L"cycle in selected fallback",L"#target{--a:var(--missing,var(--a));width:var(--a,64px)}",L"<div id='target'></div>",64},
    {L"variable inheritance freeze",L"main{--b:65px;--a:var(--b)}#target{--b:99px;width:var(--a)}",L"<main><div id='target'></div></main>",65},
    {L"variable shorthand expansion",L"#target{--p:10px 11px 12px 13px;width:42px;padding:var(--p)}",L"<div id='target'></div>",66},
    {L"arithmetic precedence",L"#target{width:calc(5px + 31px * 2)}",L"<div id='target'></div>",67},
    {L"nested arithmetic grouping",L"#target{width:calc((30px + 4px) * 2)}",L"<div id='target'></div>",68},
    {L"comparison arithmetic",L"#target{width:clamp(30px,calc(70px - 1px),100px)}",L"<div id='target'></div>",69},
    {L"stepped arithmetic",L"#target{width:round(nearest,68px,10px)}",L"<div id='target'></div>",70},
    {L"supports condition",L"@supports (display:grid) and (width:calc(30px + 41px)){#target{width:71px}}",L"<div id='target'></div>",71},
    {L"supports false branch",L"#target{width:72px}@supports (display:invented){#target{width:99px}}",L"<div id='target'></div>",72},
    {L"media range",L"@media (700px < width <= 900px){#target{width:73px}}",L"<div id='target'></div>",73},
    {L"media boolean grouping",L"@media ((width > 700px) and (height >= 600px)) or (width < 100px){#target{width:74px}}",L"<div id='target'></div>",74},
    {L"unknown at-rule recovery",L"@unknown ';{}' {#target{width:99px}}#target{width:75px}",L"<div id='target'></div>",75},
    {L"EOF closes rule",L"#target{width:76px",L"<div id='target'></div>",76},
    {L"hex alpha",L"#target{color:#1234}",L"<div id='target'></div>",0x44112233u,true},
    {L"modern rgb",L"#target{color:rgb(100% 0 0 / 50%)}",L"<div id='target'></div>",0x80ff0000u,true},
    {L"HSL hue unit",L"#target{color:hsl(.5turn 100% 50%)}",L"<div id='target'></div>",0xff00ffffu,true},
    {L"HWB",L"#target{color:hwb(0 20% 30%)}",L"<div id='target'></div>",0xffb33333u,true},
    {L"OKLab",L"#target{color:oklab(62.795536% .224863 .125846)}",L"<div id='target'></div>",0xffff0000u,true},
    {L"Lab white",L"#target{color:lab(100% 0 0)}",L"<div id='target'></div>",0xffffffffu,true},
    {L"sRGB color space",L"#target{color:color(srgb .2 .4 .6)}",L"<div id='target'></div>",0xff336699u,true},
    {L"named color completeness",L"#target{color:papayawhip}",L"<div id='target'></div>",0xffffefd5u,true},
    {L"initial inherited property",L"main{color:red}#target{color:initial}",L"<main><div id='target'></div></main>",0xff000000u,true},
    {L"unset inherited property",L"main{color:lime}#target{color:unset}",L"<main><div id='target'></div></main>",0xff00ff00u,true},
    {L"escaped variable name",LR"CSS(#target{--a\:b:77px;width:var(--a\:b)})CSS",L"<div id='target'></div>",77},
    {L"invalid initial variable fallback",L"#target{--a:initial;--b:var(--a,78px);width:var(--b)}",L"<div id='target'></div>",78},
    {L"empty selected variable",L"#target{--a:;width:var(--a,79px)}",L"<div id='target'></div>",800},
    {L"invalid variable unsets winner",L"#target{width:80px;width:var(--missing)}",L"<div id='target'></div>",800},
    {L"custom property revert layer",L"@layer base,theme;@layer base{#target{--size:81px}}@layer theme{#target{--size:99px}#target{--size:revert-layer}}#target{width:var(--size)}",L"<div id='target'></div>",81},
    {L"multiple nesting references",L"main{& :is(& .item){width:82px}}",L"<main><div id='target' class='item'></div></main>",82},
    {L"escaped ID",LR"CSS(#\74 arget{width:83px})CSS",L"<div id='target'></div>",83},
    {L"empty substring never matches",L"#target{width:84px}[data-value^='']{width:99px}",L"<div id='target' data-value='anything'></div>",84},
    {L"ancestor combinator backtracking",L"#root > .a .b{width:85px}",L"<main id='root'><div class='a'><div><div class='a'><div id='target' class='b'></div></div></div></div></main>",85},
    {L"supports selector function",L"@supports selector(:is(main > .item)){#target{width:86px}}",L"<main><div id='target' class='item'></div></main>",86},
    {L"shorthand initial",L"#target{width:87px;padding:10px;padding:initial}",L"<div id='target'></div>",87},
    {L"layer important beats unlayered",L"#target{width:99px!important}@layer base{div{width:88px!important}}",L"<div id='target'></div>",88},
    {L"invalid color preserves lower declaration",L"#target{color:lime;color:rgb(1 2 3 4)}",L"<div id='target'></div>",0xff00ff00u,true},
};
bool ColorNear(unsigned a,unsigned b,bool canvas=false){
    const int alpha=static_cast<int>((a>>24)&255);
    for(int shift:{0,8,16,24}){
        const int tolerance=canvas&&shift!=24?static_cast<int>(std::ceil(255.0/std::max(1,alpha))):1;
        if(std::abs(int((a>>shift)&255)-int((b>>shift)&255))>tolerance)return false;
    }
    return true;
}
std::wstring FixtureHtml(const Fixture& fixture){return L"<!doctype html><html><head><style>body{margin:0}"+fixture.css+L"</style></head><body>"+fixture.html+L"</body></html>";}
double NativeValue(const Fixture& fixture,float scale){
    Document document;StyleSheet sheet;std::wstring error;
    Check(document.Parse(FixtureHtml(fixture),&error)&&sheet.Parse(document.StyleText(),&error),fixture.name+L" parses");
    LayoutEngine layout(document,sheet);layout.Layout(800,600,scale);
    const auto node=document.GetElementById(L"target");const auto* box=layout.BoxFor(node);
    if(!box){Check(false,fixture.name+L" has layout");return -1;}
    if(fixture.color)return static_cast<double>(StyleSheet::Color(box->style.Get(L"color")));
    return box->rect.width;
}
bool Pump(const bool& done){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(!done&&std::chrono::steady_clock::now()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    return done;
}
bool CompareWebView2(const std::vector<double>& native){
    const HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"CSS compatibility",WS_POPUP,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ComPtr<ICoreWebView2Environment> environment;ComPtr<ICoreWebView2Controller> controller;ComPtr<ICoreWebView2> webview;
    bool ready=false;HRESULT initialized=E_FAIL;
    const auto profile=std::filesystem::absolute(L"TWebFrame2/tests/artifacts/css-webview2-profile").wstring();
    const auto start=CreateCoreWebView2EnvironmentWithOptions(nullptr,profile.c_str(),nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([&](HRESULT hr,ICoreWebView2Environment* value)->HRESULT{
            initialized=hr;if(FAILED(hr)||!value){ready=true;return S_OK;}environment=value;
            initialized=environment->CreateCoreWebView2Controller(host,Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([&](HRESULT result,ICoreWebView2Controller* value)->HRESULT{
                initialized=result;if(SUCCEEDED(result)&&value){controller=value;controller->get_CoreWebView2(&webview);controller->put_IsVisible(FALSE);RECT bounds{0,0,800,600};controller->put_Bounds(bounds);}ready=true;return S_OK;
            }).Get());if(FAILED(initialized))ready=true;return S_OK;
        }).Get());
    if(FAILED(start)||!Pump(ready)||FAILED(initialized)||!webview){DestroyWindow(host);return false;}
    LPWSTR version=nullptr;if(SUCCEEDED(environment->get_BrowserVersionString(&version))&&version){std::wcout<<L"WebView2 "<<version<<L'\n';RegressionIO::WriteText(L"TWebFrame2/tests/artifacts/css-webview2-version.txt",version);CoTaskMemFree(version);}
    bool navigated=false;bool navigationOk=false;EventRegistrationToken token{};
    webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([&](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT{BOOL success=FALSE;args->get_IsSuccess(&success);navigationOk=success!=FALSE;navigated=true;return S_OK;}).Get(),&token);
    if(FAILED(webview->NavigateToString(L"<!doctype html><html><body></body></html>"))||!Pump(navigated)||!navigationOk){controller->Close();DestroyWindow(host);return false;}
    std::wstring report=L"[";size_t index=0;
    for(const auto& fixture:fixtures){
        const auto script=L"(function(){document.documentElement.innerHTML="+RegressionIO::Quote(L"<head><style>body{margin:0}"+fixture.css+L"</style></head><body>"+fixture.html+L"</body>")+L";var target=document.getElementById('target');"+
            (fixture.color?L"var canvas=document.createElement('canvas');canvas.width=canvas.height=1;var c=canvas.getContext('2d');c.fillStyle=getComputedStyle(target).color;c.fillRect(0,0,1,1);var p=c.getImageData(0,0,1,1).data;return ((p[3]*16777216)+(p[0]*65536)+(p[1]*256)+p[2]);":L"return target.getBoundingClientRect().width;")+L"})()";
        bool done=false;std::wstring json;HRESULT completion=E_FAIL;
        const auto executed=webview->ExecuteScript(script.c_str(),Callback<ICoreWebView2ExecuteScriptCompletedHandler>([&](HRESULT hr,LPCWSTR result)->HRESULT{completion=hr;json=result?result:L"null";done=true;return S_OK;}).Get());
        const bool ran=SUCCEEDED(executed)&&Pump(done)&&SUCCEEDED(completion);
        double reference=-1;if(ran&&json!=L"null")reference=wcstod(json.c_str(),nullptr);
        const bool equal=fixture.color?ColorNear(static_cast<unsigned>(native[index]),static_cast<unsigned>(reference),true):std::abs(native[index]-reference)<0.05;
        Check(ran&&equal,fixture.name+L" matches WebView2 (native="+std::to_wstring(native[index])+L", reference="+json+L")");
        if(index)report+=L',';report+=L"{\"name\":"+RegressionIO::Quote(fixture.name)+L",\"native\":"+std::to_wstring(native[index])+L",\"webview2\":"+json+L",\"pass\":"+(ran&&equal?L"true":L"false")+L"}";++index;
    }
    RegressionIO::WriteText(L"TWebFrame2/tests/artifacts/css-webview2-results.json",report+L"]");
    controller->Close();DestroyWindow(host);return true;
}
}
int wmain(int argc,wchar_t** argv){
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 1;
    std::vector<double> native;
    for(float scale:{1.0f,1.5f}){
        for(const auto& fixture:fixtures){
            const double value=NativeValue(fixture,scale);
            const bool equal=fixture.color?ColorNear(static_cast<unsigned>(value),static_cast<unsigned>(fixture.expected)):std::abs(value-fixture.expected)<0.05;
            Check(equal,fixture.name+L" at DPI "+std::to_wstring(scale)+L" (actual="+std::to_wstring(value)+L")");
            if(scale==1)native.push_back(value);
        }
    }
    Check(StyleSheet::Length(L"calc(10px / 0)",100,100,777)==777,L"division by zero uses fallback");
    Check(StyleSheet::Length(L"calc(10px * 2px)",100,100,777)==777,L"dimensionally invalid multiplication uses fallback");
    Check(StyleSheet::Length(L"calc(10px+2px)",100,100,777)==777,L"binary addition requires CSS whitespace");
    Check(StyleSheet::Color(L"rgb(1,2,3,4,5)",0x01020304u)==0x01020304u,L"extra RGB components are rejected");
    Check(StyleSheet::Color(L"rgb(1,,2,3)",0x01020304u)==0x01020304u,L"empty RGB components are rejected");
    Check(!StyleSheet::Supports(L"display:grid")&&!StyleSheet::Supports(L"not display:grid"),L"supports rejects unparenthesized declarations");
    Check(!StyleSheet::Supports(L"padding",L"auto")&&!StyleSheet::Supports(L"width",L"3")&&!StyleSheet::Supports(L"width",L"-1px"),L"supports validates dimensional property grammars");
    Document apiDocument;apiDocument.Parse(L"<div></div>");JavaScriptRuntime runtime(apiDocument);std::wstring result,error;
    Check(runtime.Execute(L"return CSS.supports('display','grid')+'|'+CSS.supports('(display:grid) and (width:calc(20px * 2))')+'|'+CSS.supports('display','invented');",&result,&error)&&result==L"true|true|false",L"CSS.supports uses the stylesheet engine for both signatures");
    StyleSheet dependencySheet;dependencySheet.Parse(LR"CSS([data-\76 alue='x']{width:2px}div:lang(ko){color:red})CSS");
    Check(dependencySheet.AttributeAffectsStyle(L"data-value")&&dependencySheet.AttributeAffectsStyle(L"lang"),L"escaped attribute and language selectors participate in style invalidation");
    Document flagDocument;flagDocument.Parse(L"<div id='target' data-value='exact'></div>");
    Check(Document::MatchesSelector(flagDocument.GetElementById(L"target"),L"[data-value='exact' s]"),L"explicit sensitive attribute flag is a native extension (WebView2 154 rejects it)");
    if(argc>1&&std::wstring(argv[1])==L"--webview2")Check(CompareWebView2(native),L"WebView2 comparison ran");
    std::wcout<<checks<<L" CSS checks, "<<failures<<L" failures\n";CoUninitialize();return failures?1:0;
}

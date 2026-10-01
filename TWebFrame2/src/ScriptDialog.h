#pragma once

#include "CSS.h"
#include "DOM.h"
#include "Layout.h"

#include <algorithm>
#include <memory>
#include <string>

namespace TWebFrame::Internal {

// Browser-owned HTML surface. It uses the ordinary CSS layout and painter,
// but its nodes and styles are isolated from the page's document and scripts.
struct ScriptDialog {
    Document document;
    StyleSheet styles;
    LayoutEngine layout{document,styles};
    std::shared_ptr<Node> card;
    std::shared_ptr<Node> okButton;
    std::shared_ptr<Node> cancelButton;
    bool confirm=false;
    bool active=true;
    bool accepted=false;
    int focusedButton=0;
    int pressedButton=-1;
    int hoveredButton=-1;

    static std::wstring SiteName(const std::wstring& location){
        const auto scheme=location.find(L"://");
        if(scheme==std::wstring::npos)return {};
        const auto begin=scheme+3,end=location.find_first_of(L"/?#",begin);
        auto authority=location.substr(begin,end==std::wstring::npos?end:end-begin);
        const auto userInfo=authority.rfind(L'@');
        if(userInfo!=std::wstring::npos)authority.erase(0,userInfo+1);
        return authority;
    }
    bool Initialize(const std::wstring& site,const std::wstring& message,
                    bool isConfirm,bool korean){
        confirm=isConfirm;
        if(!document.Parse(L"<!doctype html><html><body><section id='script-dialog' role='alertdialog'>"
            L"<div id='script-dialog-title'></div><div id='script-dialog-message'></div>"
            L"<div id='script-dialog-actions'><button id='script-dialog-ok'></button>"
            L"<button id='script-dialog-cancel'></button></div></section></body></html>"))return false;
        if(!styles.Parse(LR"CSS(
            html,body{margin:0;padding:0;background:transparent;color:#202124;
                font-family:Arial,"Malgun Gothic",sans-serif}
            #script-dialog{position:fixed;top:12px;box-sizing:border-box;min-height:168px;
                padding:16px;background:#fff;border-radius:8px;
                box-shadow:0 7px 18px rgba(0,0,0,0.2),0 2px 5px rgba(0,0,0,0.13)}
            #script-dialog-title{font-size:16px;font-weight:500;line-height:24px;
                margin-bottom:12px}
            #script-dialog-message{font-size:13px;line-height:20px;white-space:pre-wrap;
                overflow-wrap:anywhere;overflow:auto;min-height:36px}
            #script-dialog-actions{display:flex;justify-content:flex-end;gap:8px;
                margin-top:28px}
            #script-dialog-actions button{appearance:none;box-sizing:border-box;height:32px;
                padding:0 10px;border-radius:4px;font:13px Arial,"Malgun Gothic",sans-serif;
                cursor:pointer}
            #script-dialog-ok{color:#fff;background:#1967d2;border:1px solid #1967d2}
            #script-dialog-cancel{color:#202124;background:#fff;border:1px solid #dadce0}
            #script-dialog-ok:hover{background:#185abc}
            #script-dialog-cancel:hover{background:#f8f9fa}
            #script-dialog-actions button:focus-visible{outline:2px solid #1a73e8;
                outline-offset:2px}
        )CSS"))return false;
        card=document.GetElementById(L"script-dialog");
        okButton=document.GetElementById(L"script-dialog-ok");
        cancelButton=document.GetElementById(L"script-dialog-cancel");
        const auto title=site.empty()?(korean?L"\uc774 \ud398\uc774\uc9c0\uc758 \uba54\uc2dc\uc9c0":L"This page says"):
            site+(korean?L"\uc758 \uba54\uc2dc\uc9c0":L" says");
        document.GetElementById(L"script-dialog-title")->SetInnerText(title);
        document.GetElementById(L"script-dialog-message")->SetInnerText(message);
        okButton->SetInnerText(korean?L"\ud655\uc778":L"OK");
        cancelButton->SetInnerText(korean?L"\ucde8\uc18c":L"Cancel");
        return true;
    }
    void Layout(float width,float height,float scale){
        const float cardWidth=std::min(448.0f,std::max(1.0f,width-24.0f));
        const float left=std::max(0.0f,(width-cardWidth)/2.0f);
        const bool compact=height<200.0f;
        card->SetAttribute(L"style",L"left:"+std::to_wstring(left)+L"px;width:"+
            std::to_wstring(cardWidth)+L"px;"+(compact?L"top:4px;padding:8px;min-height:0":L""));
        const auto title=document.GetElementById(L"script-dialog-title");
        const auto message=document.GetElementById(L"script-dialog-message");
        const auto actions=document.GetElementById(L"script-dialog-actions");
        title->SetAttribute(L"style",compact?L"margin-bottom:4px;font-size:14px;line-height:18px":L"");
        message->SetAttribute(L"style",L"max-height:"+
            std::to_wstring(std::max(20.0f,height-(compact?96.0f:150.0f)))+L"px;"+
            (compact?L"min-height:20px;font-size:12px;line-height:16px":L""));
        actions->SetAttribute(L"style",compact?L"margin-top:8px":L"");
        okButton->SetAttribute(L"style",compact?L"height:28px":L"");
        cancelButton->SetAttribute(L"style",confirm?(compact?L"height:28px":L""):L"display:none");
        layout.Layout(width,height,scale);
    }
    int ButtonAt(float x,float y)const{
        const auto hit=layout.HitTest(x,y);
        if(hit==okButton)return 0;
        if(confirm&&hit==cancelButton)return 1;
        return -1;
    }
    void Hover(int button){
        if(hoveredButton==button)return;
        hoveredButton=button;okButton->hovered=button==0;cancelButton->hovered=button==1;
        layout.Restyle(okButton);layout.Restyle(cancelButton);
    }
    void Focus(int button,bool keyboard){
        focusedButton=button;
        okButton->focused=button==0;cancelButton->focused=button==1;
        okButton->focusVisible=keyboard&&button==0;
        cancelButton->focusVisible=keyboard&&button==1;
        layout.Restyle(okButton);layout.Restyle(cancelButton);
    }
};

} // namespace TWebFrame::Internal

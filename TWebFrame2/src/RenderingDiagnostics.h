#pragma once
#include "Layout.h"
#include <iomanip>
#include <locale>
#include <map>
#include <sstream>

namespace TWebFrame::Internal {
inline std::wstring RenderingQuote(const std::wstring& value){
    std::wstring out=L"\"";const wchar_t hex[]=L"0123456789abcdef";
    for(wchar_t c:value){if(c==L'"'||c==L'\\'){out+=L'\\';out+=c;}
        else if(c<32){out+=L"\\u00";out+=hex[(c>>4)&15];out+=hex[c&15];}else out+=c;}
    return out+L'"';
}
inline std::wstring RenderingDiagnostics(Document& document,LayoutEngine& layout,
    unsigned width,unsigned height,float dpi,unsigned windowDpi){
    std::wostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9);
    out<<L"{\"captureRoute\":\"explicit-dpi-view-paint\",\"width\":"<<width
       <<L",\"height\":"<<height<<L",\"dpi\":"<<dpi<<L",\"windowDpi\":"<<windowDpi<<L",\"styleContractVersion\":1,\"dom\":[";
    auto root=document.QuerySelector(L"html");if(!root)root=document.Body();
    bool first=true;
    const std::function<void(const std::shared_ptr<Node>&,const std::wstring&)> dom=
        [&](const std::shared_ptr<Node>& node,const std::wstring& path){
        if(!node)return;if(!first)out<<L',';first=false;
        const int type=node->type==NodeType::Element?1:node->type==NodeType::Text?3:
            node->type==NodeType::Comment?8:node->type==NodeType::Document?9:4;
        out<<L"{\"path\":"<<RenderingQuote(path)<<L",\"type\":"<<type
           <<L",\"tag\":"<<RenderingQuote(node->tag)<<L",\"text\":"
           <<RenderingQuote(type==1?L"":node->text)<<L",\"attrs\":{";
        std::map<std::wstring,std::wstring> attrs;for(const auto& pair:node->attributes)attrs.emplace(pair.first,pair.second);
        bool firstAttribute=true;for(const auto& pair:attrs){if(!firstAttribute)out<<L',';firstAttribute=false;
            out<<RenderingQuote(pair.first)<<L':'<<RenderingQuote(pair.second);}
        out<<L"}}";for(size_t i=0;i<node->children.size();++i)dom(node->children[i],path+L"/"+std::to_wstring(i));
    };dom(root,L"0");out<<L"],\"boxes\":[";first=true;
    const std::function<void(const std::shared_ptr<Node>&)> boxes=[&](const std::shared_ptr<Node>& node){
        if(!node)return;const auto id=node->Attribute(L"data-case-node");
        if(!id.empty()){
            const auto* box=layout.BoxFor(node);const auto style=layout.StyleForRendering(node);
            bool present=box&&box->visible;
            for(auto ancestor=node->parent.lock();present&&ancestor;ancestor=ancestor->parent.lock())
                if(const auto* parentBox=layout.BoxFor(ancestor))present=parentBox->visible;
            if(!first)out<<L',';first=false;
            out<<L"{\"id\":"<<RenderingQuote(id)<<L",\"present\":"<<(present?L"true":L"false");
            if(box){const LayoutRect rect=box->rect;
                out<<L",\"rect\":["<<rect.x<<L','<<rect.y<<L','<<rect.width<<L','<<rect.height
                   <<L"],\"content\":["<<box->content.x<<L','<<box->content.y<<L','<<box->content.width<<L','<<box->content.height
                   <<L"],\"scroll\":["<<box->scrollWidth<<L','<<box->scrollHeight<<L']';
            }
            out<<L",\"styles\":{";bool firstStyle=true;std::map<std::wstring,std::wstring> values;
            if(style.values)for(const auto& pair:*style.values)values.emplace(pair.first,pair.second);
            for(const auto& pair:values){if(!firstStyle)out<<L',';firstStyle=false;
                out<<RenderingQuote(pair.first)<<L':'<<RenderingQuote(pair.second);}
            out<<L'}';
            out<<L",\"computedStyles\":"<<layout.DumpRenderingStyleJson(style)<<L'}';
        }
        for(const auto& child:node->children)boxes(child);
    };boxes(root);out<<L"],\"text\":[";first=true;
    const std::function<void(const LayoutBox*)> text=[&](const LayoutBox* box){
        if(!box)return;if(box->node&&box->node->type==NodeType::Text&&box->visible){
            if(!first)out<<L',';first=false;
            out<<L"{\"text\":"<<RenderingQuote(box->node->text)<<L",\"offset\":"<<box->textSourceOffset
               <<L",\"rect\":["<<box->rect.x<<L','<<box->rect.y<<L','<<box->rect.width<<L','<<box->rect.height
               <<L"],\"font\":"<<RenderingQuote(box->style.Get(L"font-family"))
               <<L",\"color\":"<<RenderingQuote(box->style.Get(L"color"))<<L'}';
        }for(const auto& child:box->children)text(child.get());
    };text(layout.Root());out<<L"],\"textGeometry\":"<<layout.DumpRenderingTextJson()<<L"}";return out.str();
}
}

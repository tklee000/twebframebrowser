#include "Layout.h"
#include "Canvas.h"
#include "NumericParser.h"
#include "RasterImage.h"
#include "RenderingGlyphs.h"
#include "RasterSurface.h"
#include "RasterGradient.h"

#include <algorithm>
#include <numeric>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <cwctype>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <list>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <wrl/client.h>
#include <wincodec.h>

namespace TWebFrame::Internal {
namespace {

// Evict the least recently measured text instead of dropping every width when
// a document crosses an entry-count threshold. Keys include all shaping inputs;
// neither text length nor character set changes the actual measurement path.
class TextWidthCache {
    using Entry=std::pair<std::wstring,float>;
    using Entries=std::list<Entry>;
    Entries entries_;
    std::unordered_map<std::wstring_view,Entries::iterator> index_;
    size_t bytes_=0;
    static constexpr size_t budget_=4*1024*1024;
    static size_t Cost(const std::wstring& key){
        return sizeof(Entry)+8*sizeof(void*)+(key.capacity()+1)*sizeof(wchar_t);
    }
public:
    bool Find(const std::wstring& key,float& width){
        const auto found=index_.find(key);
        if(found==index_.end())return false;
        width=found->second->second;
        entries_.splice(entries_.begin(),entries_,found->second);
        return true;
    }
    void Remember(std::wstring key,float width){
        const auto cost=Cost(key);
        if(cost>budget_)return;
        while(bytes_+cost>budget_&&!entries_.empty()){
            const auto& oldest=entries_.back().first;
            bytes_-=Cost(oldest);index_.erase(oldest);entries_.pop_back();
        }
        entries_.emplace_front(std::move(key),width);
        index_.emplace(std::wstring_view(entries_.front().first),entries_.begin());
        bytes_+=cost;
    }
    void Clear(){index_.clear();entries_.clear();bytes_=0;}
};
TextWidthCache& ThreadTextWidthCache(){
    static thread_local TextWidthCache cache;return cache;
}

struct Edges { float top=0,right=0,bottom=0,left=0; };
struct CachedStyleMetrics {
    std::weak_ptr<ComputedStyle::ValueMap> owner;
    Edges borders;
    float borderScale=0;
    float fontSize=0;
    float lineHeight=0;
    float lineHeightScale=0;
    bool bordersValid=false;
    bool fontSizeValid=false;
    bool lineHeightValid=false;
    bool edgeDependenciesValid[2]{};
    bool edgesUseReference[2]{};
    bool edgesUseViewport[2]{};
    bool monospaceValid=false;
    bool monospace=false;
    bool inlineFontValid=false;
    bool scrollbarGutterValid=false;
    unsigned int scrollbarGutterFlags=0;
    bool scrollbarStyleValid=false, scrollbarThin=false, scrollbarNone=false;
    bool scrollbarStandardStyling=false;
    bool relativeOffsetsValid=false, hasRelativeOffsets=false;
    float inlineFontScale=0;
    float inlineFontHeight=0;
    float inlineFontXHeight=0;
    float inlineFontBaseline=0;
    float inlineFontLineGap=0;
};
using StyleMetricsCache=FastMap<const void*,CachedStyleMetrics>;
StyleMetricsCache& ThreadStyleMetricsCache(){
    static thread_local StyleMetricsCache cache;return cache;
}
CachedStyleMetrics& StyleMetrics(const ComputedStyle& style){
    auto& cache=ThreadStyleMetricsCache();
    const auto key=static_cast<const void*>(style.values.get());
    auto found=cache.find(key);
    if(found!=cache.end()){
        const auto owner=found->second.owner.lock();
        if(owner&&owner.get()==style.values.get())return found->second;
        cache.clear();
    }else if(cache.size()>=4096)cache.clear();
    auto& result=cache[key];result.owner=style.values;return result;
}

struct EdgeCacheKey {
    const void* values=nullptr;
    float reference=0;
    float viewport=0;
    bool padding=false;
    bool operator==(const EdgeCacheKey& other) const {
        return values==other.values&&reference==other.reference&&
            viewport==other.viewport&&padding==other.padding;
    }
};
struct EdgeCacheHash {
    size_t operator()(const EdgeCacheKey& key) const {
        size_t hash=std::hash<const void*>{}(key.values);
        hash^=std::hash<float>{}(key.reference)+0x9e3779b9u+(hash<<6)+(hash>>2);
        hash^=std::hash<float>{}(key.viewport)+0x9e3779b9u+(hash<<6)+(hash>>2);
        return hash^(key.padding?0x85ebca6bu:0u);
    }
};
struct CachedEdges {
    std::weak_ptr<ComputedStyle::ValueMap> owner;
    Edges value;
};
using EdgeValuesCache=FastMap<EdgeCacheKey,CachedEdges,EdgeCacheHash>;
EdgeValuesCache& ThreadEdgeValuesCache(){
    static thread_local EdgeValuesCache cache;return cache;
}
void ClearOwnerBoundThreadCaches(){
    ThreadTextWidthCache().Clear();
    // These caches use weak ownership to validate raw style pointers. Releasing
    // them with the last layout engine avoids retaining weak control blocks for
    // documents whose view has already been destroyed.
    ThreadStyleMetricsCache().clear();ThreadEdgeValuesCache().clear();
}
struct CornerRadii {
    // Clockwise from top left. x/y retain the first corner for scrollbar sizing.
    float x=0,y=0;
    std::array<D2D1_POINT_2F,4> corners{};
    bool Any() const {
        for(const auto& corner:corners)if(corner.x>0&&corner.y>0)return true;
        return false;
    }
    bool Uniform() const {
        for(const auto& corner:corners)if(corner.x!=x||corner.y!=y)return false;
        return true;
    }
};
struct BoxShadow {
    float offsetX=0,offsetY=0,blur=0,spread=0;
    unsigned int color=0;
    bool inset=false;
};
struct VerticalScrollbarGeometry {
    LayoutRect track;
    LayoutRect thumb;
    float trackStart = 0;
    float travel = 0;
    float maximum = 0;
    float arrowHeight = 0;
    bool compactArrows = false;
    bool standardStyling = false;
};

struct VerticalScrollbarMetrics {
    float width = 15.0f;
    float arrowHeight = 0;
    float minimumThumbHeight = 0;
    float thumbInset = 0;
    bool compactArrows = false;
    bool standardStyling = false;
};

struct HorizontalScrollbarGeometry {
    LayoutRect track;
    LayoutRect thumb;
    float trackStart = 0;
    float travel = 0;
    float maximum = 0;
    float arrowWidth = 0;
    bool compactArrows = false;
    bool standardStyling = false;
};

struct HorizontalScrollbarMetrics {
    float height = 15.0f;
    float arrowWidth = 0;
    float minimumThumbWidth = 0;
    float thumbInset = 0;
    bool compactArrows = false;
    bool standardStyling = false;
};

Edges BorderValues(const ComputedStyle& style);
Edges UsedBorderValues(const LayoutBox& box);

LayoutRect PaddingBox(const LayoutBox& box) {
    if(box.viewportScrollContainer)return box.viewportScrollport;
    const auto border=BorderValues(box.style);
    return {box.rect.x+border.left,box.rect.y+border.top,
        std::max(0.0f,box.rect.width-border.left-border.right),
        std::max(0.0f,box.rect.height-border.top-border.bottom)};
}

std::wstring OverflowX(const LayoutBox& box) {
    if(box.viewportScrollContainer)return box.viewportOverflowX;
    return box.style.Get(L"overflow-x",box.style.Get(L"overflow",L"visible"));
}

std::wstring OverflowY(const LayoutBox& box) {
    if(box.viewportScrollContainer)return box.viewportOverflowY;
    return box.style.Get(L"overflow-y",box.style.Get(L"overflow",L"visible"));
}

float ScrollClientWidth(const LayoutBox& box) {
    return box.viewportScrollContainer?std::max(0.0f,box.viewportScrollport.width-box.viewportGutterLeft-box.viewportGutterRight):box.content.width;
}

float ScrollClientHeight(const LayoutBox& box) {
    return box.viewportScrollContainer?std::max(0.0f,box.viewportScrollport.height-box.viewportGutterBottom):box.content.height;
}

bool AutoScrollbarHeight(const LayoutBox& box,bool definiteHeight){
    const auto height=box.style.Get(L"height"),maximum=box.style.Get(L"max-height");
    if((!height.empty()&&height!=L"auto")||(!maximum.empty()&&maximum!=L"none"&&maximum!=L"auto"))return false;
    if(!definiteHeight)return true;
    return (box.style.Is(L"position",L"absolute")||box.style.Is(L"position",L"fixed"))&&
        (box.style.Get(L"top",L"auto")==L"auto"||box.style.Get(L"bottom",L"auto")==L"auto");
}

struct StandardScrollbarStyle {bool thin=false,none=false,standard=false;};
StandardScrollbarStyle ScrollbarStyleFor(const ComputedStyle& style) {
    auto& cached=StyleMetrics(style);
    if(!cached.scrollbarStyleValid){
        const auto width=ToLower(Trim(style.Get(L"scrollbar-width",L"auto")));
        const auto colors=ToLower(Trim(style.Get(L"scrollbar-color")));
        cached.scrollbarThin=width==L"thin";cached.scrollbarNone=width==L"none";
        cached.scrollbarStandardStyling=width!=L"auto"||(!colors.empty()&&colors!=L"auto");
        cached.scrollbarStyleValid=true;
    }
    return {cached.scrollbarThin,cached.scrollbarNone,cached.scrollbarStandardStyling};
}

VerticalScrollbarMetrics VerticalScrollbarMetricsFor(const LayoutBox& box,const StyleSheet& styleSheet) {
    VerticalScrollbarMetrics metrics;
    const auto standard=ScrollbarStyleFor(box.style);
    if(standard.none){metrics.width=0;metrics.arrowHeight=0;return metrics;}
    metrics.standardStyling=standard.standard;
    // Ordinary scrollbars need no temporary pseudo-style maps. The style
    // cache follows computed-value ownership; dimensions and custom rules
    // remain live inputs below so resizing and selector changes stay visible.
    if(!styleSheet.HasPseudoRules(L"-webkit-scrollbar")&&
       !styleSheet.HasPseudoRules(L"-webkit-scrollbar-thumb")){
        if(standard.thin)metrics.width=10.0f;
        else {const float scale=std::max(0.01f,box.style.deviceScale);metrics.width=std::round(metrics.width*scale)/scale;}
        metrics.compactArrows=standard.thin;
        metrics.arrowHeight=metrics.width*1.2f;
        metrics.minimumThumbHeight=metrics.width*(standard.thin?3.6f:1.15f);
        metrics.thumbInset=metrics.width*0.2f;
        return metrics;
    }
    const auto scrollbarStyle=styleSheet.HasPseudoRules(L"-webkit-scrollbar")?
        styleSheet.Compute(box.node,&box.style,L"-webkit-scrollbar"):ComputedStyle{};
    const auto customWidth=Trim(scrollbarStyle.Get(L"width"));
    const bool custom=!customWidth.empty()&&customWidth!=L"auto";
    if(custom)metrics.width=std::max(0.0f,StyleSheet::Length(customWidth,box.content.width,box.content.width,metrics.width));
    else if(standard.thin)metrics.width=10.0f;
    metrics.compactArrows=standard.thin||custom;
    // Keep scrollbar metrics in CSS DIPs. Direct2D applies the window DPI, so
    // the authored 10px width becomes 15 physical pixels at 150% without any
    // monitor-specific constants here. Button, thumb and arrow proportions are
    // derived from that authored width for the same reason.
    metrics.arrowHeight=custom&&!metrics.standardStyling?0:metrics.width*1.2f;
    metrics.minimumThumbHeight=metrics.width*(metrics.compactArrows?3.6f:1.15f);
    metrics.thumbInset=metrics.width*0.2f;
    if(custom&&!metrics.standardStyling){
        const float scale=std::max(0.01f,box.style.deviceScale);
        metrics.minimumThumbHeight=std::floor(17.0f*scale)/scale;
        metrics.thumbInset=0;
    }
    const auto thumbStyle=styleSheet.HasPseudoRules(L"-webkit-scrollbar-thumb")?
        styleSheet.Compute(box.node,&box.style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
    const auto minimum=Trim(thumbStyle.Get(L"min-height"));
    if(!metrics.standardStyling&&!minimum.empty())
        metrics.minimumThumbHeight=std::max(0.0f,StyleSheet::Length(minimum,box.content.height,box.content.height,metrics.minimumThumbHeight));
    const auto border=Trim(thumbStyle.Get(L"border-width",thumbStyle.Get(L"border")));
    if(!metrics.standardStyling&&!border.empty())
        metrics.thumbInset=std::max(0.0f,StyleSheet::Length(border,metrics.width,metrics.width,metrics.thumbInset));
    return metrics;
}

HorizontalScrollbarMetrics HorizontalScrollbarMetricsFor(const LayoutBox& box,const StyleSheet& styleSheet) {
    HorizontalScrollbarMetrics metrics;
    const auto standard=ScrollbarStyleFor(box.style);
    if(standard.none){metrics.height=0;metrics.arrowWidth=0;return metrics;}
    metrics.standardStyling=standard.standard;
    if(!styleSheet.HasPseudoRules(L"-webkit-scrollbar")&&
       !styleSheet.HasPseudoRules(L"-webkit-scrollbar-thumb")){
        if(standard.thin)metrics.height=10.0f;
        else {const float scale=std::max(0.01f,box.style.deviceScale);metrics.height=std::round(metrics.height*scale)/scale;}
        metrics.compactArrows=standard.thin;
        metrics.arrowWidth=metrics.height*1.2f;
        metrics.minimumThumbWidth=metrics.height*(standard.thin?3.6f:1.15f);
        metrics.thumbInset=metrics.height*0.2f;
        return metrics;
    }
    const auto scrollbarStyle=styleSheet.HasPseudoRules(L"-webkit-scrollbar")?
        styleSheet.Compute(box.node,&box.style,L"-webkit-scrollbar"):ComputedStyle{};
    const auto customHeight=Trim(scrollbarStyle.Get(L"height"));
    const bool custom=!customHeight.empty()&&customHeight!=L"auto";
    if(custom)metrics.height=std::max(0.0f,StyleSheet::Length(customHeight,box.content.height,box.content.height,metrics.height));
    else if(standard.thin)metrics.height=10.0f;
    metrics.compactArrows=standard.thin||custom;
    // Metrics remain CSS DIPs; the render target and pointer conversion apply
    // the monitor DPI exactly once at both 100% and 150% scaling.
    metrics.arrowWidth=custom&&!metrics.standardStyling?0:metrics.height*1.2f;
    metrics.minimumThumbWidth=metrics.height*(metrics.compactArrows?3.6f:1.15f);
    metrics.thumbInset=metrics.height*0.2f;
    if(custom&&!metrics.standardStyling){
        const float scale=std::max(0.01f,box.style.deviceScale);
        metrics.minimumThumbWidth=std::floor(17.0f*scale)/scale;
        metrics.thumbInset=0;
    }
    const auto thumbStyle=styleSheet.HasPseudoRules(L"-webkit-scrollbar-thumb")?
        styleSheet.Compute(box.node,&box.style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
    const auto minimum=Trim(thumbStyle.Get(L"min-width"));
    if(!metrics.standardStyling&&!minimum.empty())
        metrics.minimumThumbWidth=std::max(0.0f,StyleSheet::Length(minimum,box.content.width,box.content.width,metrics.minimumThumbWidth));
    const auto border=Trim(thumbStyle.Get(L"border-width",thumbStyle.Get(L"border")));
    if(!metrics.standardStyling&&!border.empty())
        metrics.thumbInset=std::max(0.0f,StyleSheet::Length(border,metrics.height,metrics.height,metrics.thumbInset));
    return metrics;
}

bool VerticalScrollbarFor(const LayoutBox& box,const StyleSheet& styleSheet,VerticalScrollbarGeometry& geometry) {
    const auto overflowY=OverflowY(box);
    if(overflowY!=L"auto"&&overflowY!=L"scroll")return false;
    if(overflowY==L"auto"&&(box.node->tag==L"textarea"?!box.textareaVerticalScrollbar:
       (box.children.empty()||box.scrollHeight<=ScrollClientHeight(box)+1)))return false;
    const auto metrics=VerticalScrollbarMetricsFor(box,styleSheet);
    const float trackWidth=metrics.width;
    if(trackWidth<=0)return false;
    const auto paddingBox=PaddingBox(box);
    const auto overflowX=OverflowX(box);
    const bool horizontal=box.node->tag==L"textarea"?box.textareaHorizontalScrollbar:
        overflowX==L"scroll"||(overflowX==L"auto"&&box.scrollWidth>ScrollClientWidth(box)+1);
    const float horizontalHeight=horizontal?HorizontalScrollbarMetricsFor(box,styleSheet).height:0;
    const float arrowHeight=metrics.arrowHeight;
    const float minimumThumbHeight=metrics.minimumThumbHeight;
    const float trackHeight=std::max(0.0f,paddingBox.height-horizontalHeight);
    const float available=std::max(0.0f,trackHeight-2*arrowHeight);
    if(available<=0)return false;
    const float paddingHeight=box.viewportScrollContainer?0.0f:
        std::max(0.0f,paddingBox.height-box.content.height-
            (box.node->tag==L"textarea"?horizontalHeight:box.scrollbarGutterBottom));
    const float scrollExtent=box.scrollHeight+paddingHeight;
    const bool custom=!metrics.standardStyling&&metrics.arrowHeight==0;
    const float visibleHeight=custom?trackHeight:paddingBox.height;
    float proportional=available*visibleHeight/std::max(visibleHeight,scrollExtent);
    if(custom){const float scale=std::max(0.01f,box.style.deviceScale);proportional=std::round(proportional*scale)/scale;}
    const float thumbHeight=std::min(available,std::max(minimumThumbHeight,proportional));
    const float maximum=std::max(0.0f,box.scrollHeight-ScrollClientHeight(box));
    const float travel=std::max(0.0f,available-thumbHeight);
    const float trackStart=paddingBox.y+arrowHeight;
    const float thumbY=trackStart+(maximum>0?travel*(box.node->scrollTop/maximum):0);
    geometry.track={box.style.Is(L"direction",L"rtl")?paddingBox.x:
        paddingBox.x+paddingBox.width-trackWidth,paddingBox.y,trackWidth,trackHeight};
    const float thumbInset=std::min(trackWidth/2.0f,metrics.thumbInset);
    geometry.thumb={geometry.track.x+thumbInset,thumbY,std::max(1.0f,trackWidth-2*thumbInset),thumbHeight};
    geometry.trackStart=trackStart;geometry.travel=travel;geometry.maximum=maximum;geometry.arrowHeight=arrowHeight;geometry.compactArrows=metrics.compactArrows;geometry.standardStyling=metrics.standardStyling;
    return true;
}

bool HorizontalScrollbarFor(const LayoutBox& box,const StyleSheet& styleSheet,HorizontalScrollbarGeometry& geometry) {
    const auto overflowX=OverflowX(box);
    if(overflowX!=L"auto"&&overflowX!=L"scroll")return false;
    if(overflowX==L"auto"&&(box.node->tag==L"textarea"?!box.textareaHorizontalScrollbar:
       (box.children.empty()||box.scrollWidth<=ScrollClientWidth(box)+1)))return false;
    const auto metrics=HorizontalScrollbarMetricsFor(box,styleSheet);
    const bool nativeTheme=!metrics.compactArrows;
    const float scale=std::max(0.01f,box.style.deviceScale);
    const float trackHeight=nativeTheme?std::round(metrics.height*scale)/scale:metrics.height;if(trackHeight<=0)return false;
    const auto paddingBox=PaddingBox(box);
    const auto overflowY=OverflowY(box);
    const bool vertical=box.node->tag==L"textarea"?box.textareaVerticalScrollbar:
        overflowY==L"scroll"||(overflowY==L"auto"&&box.scrollHeight>ScrollClientHeight(box)+1);
    const float verticalWidth=vertical?VerticalScrollbarMetricsFor(box,styleSheet).width:0;
    const float trackWidth=std::max(0.0f,paddingBox.width-verticalWidth);
    const float arrowWidth=nativeTheme?std::round(metrics.arrowWidth*scale)/scale:metrics.arrowWidth;
    const float available=std::max(0.0f,trackWidth-2*arrowWidth);if(available<=0)return false;
    const float paddingWidth=box.viewportScrollContainer?0.0f:
        std::max(0.0f,paddingBox.width-box.content.width-
            (box.node->tag==L"textarea"?verticalWidth:box.scrollbarGutterLeft+box.scrollbarGutterRight));
    const float scrollExtent=box.scrollWidth+paddingWidth;
    const bool custom=!metrics.standardStyling&&metrics.arrowWidth==0;
    const float visibleWidth=custom?trackWidth:paddingBox.width;
    const float paintedExtent=nativeTheme?std::ceil(scrollExtent*scale)/scale:scrollExtent;
    float proportional=available*visibleWidth/std::max(visibleWidth,paintedExtent);
    if(custom||nativeTheme)proportional=std::round(proportional*scale)/scale;
    const float minimum=nativeTheme?std::floor(17.0f*scale)/scale:metrics.minimumThumbWidth;
    const float thumbWidth=std::min(available,std::max(minimum,proportional));
    const float maximum=std::max(0.0f,box.scrollWidth-ScrollClientWidth(box));
    const float travel=std::max(0.0f,available-thumbWidth);
    const float trackStart=paddingBox.x+arrowWidth;
    float position=maximum>0?travel*(box.node->scrollLeft/maximum):0;
    if(nativeTheme){const float device=std::max(0.0f,position*scale);position=(device>0&&device<1?1:std::floor(device))/scale;}
    const float thumbX=trackStart+position;
    const float bottom=paddingBox.y+paddingBox.height;
    geometry.track={paddingBox.x,(nativeTheme?std::round(bottom*scale)/scale:bottom)-trackHeight,trackWidth,trackHeight};
    // Fluent thumbs have an integer thickness and equal offsets on both sides.
    // Keep layout/client dimensions in CSS units; this quantization is paint
    // and hit-test geometry only.
    const int deviceTrack=static_cast<int>(std::round(trackHeight*scale));
    const int deviceThumb=static_cast<int>(std::round(9.0f*scale));
    const float nativeInset=(deviceTrack-deviceThumb+(deviceTrack-deviceThumb)%2)/(2.0f*scale);
    const float thumbInset=std::min(trackHeight/2.0f,nativeTheme?nativeInset:metrics.thumbInset);
    geometry.thumb={thumbX,geometry.track.y+thumbInset,thumbWidth,std::max(1.0f,trackHeight-2*thumbInset)};
    geometry.trackStart=trackStart;geometry.travel=travel;geometry.maximum=maximum;geometry.arrowWidth=arrowWidth;geometry.compactArrows=metrics.compactArrows;geometry.standardStyling=metrics.standardStyling;
    return true;
}

bool IsInlineLevel(const std::wstring& display) {
    return display==L"inline"||display==L"inline-block"||
           display==L"inline-flex"||display==L"inline-grid"||display==L"inline-table";
}

bool IsTable(const LayoutBox& box){
    return box.style.Is(L"display",L"table")||box.style.Is(L"display",L"inline-table");
}

bool IsColumnFlexDirection(const ComputedStyle& style) {
    const auto direction=ToLower(Trim(style.Get(L"flex-direction",L"row")));
    return direction==L"column"||direction==L"column-reverse";
}

unsigned int ScrollbarGutterFlags(const ComputedStyle& style) {
    auto& cached=StyleMetrics(style);
    if(!cached.scrollbarGutterValid){
        const auto value=ToLower(style.Get(L"scrollbar-gutter"));
        size_t start=0;
        while(start<value.size()){
            while(start<value.size()&&std::iswspace(value[start]))++start;
            size_t end=start;while(end<value.size()&&!std::iswspace(value[end]))++end;
            const std::wstring_view token(value.data()+start,end-start);
            if(token==L"stable")cached.scrollbarGutterFlags|=1;
            else if(token==L"both-edges")cached.scrollbarGutterFlags|=2;
            start=end;
        }
        cached.scrollbarGutterValid=true;
    }
    return cached.scrollbarGutterFlags;
}
bool HasStableScrollbarGutter(const ComputedStyle& style) {
    return (ScrollbarGutterFlags(style)&1)!=0;
}

bool IsBlockifiedItem(const LayoutBox& box) {
    const auto floating=box.style.Get(L"float",L"none");
    if(floating==L"left"||floating==L"right"||floating==L"inline-start"||floating==L"inline-end")return true;
    if(!box.parent||box.style.Is(L"position",L"absolute")||box.style.Is(L"position",L"fixed"))return false;
    const auto parentDisplay=box.parent->style.Get(L"display");
    return parentDisplay==L"grid"||parentDisplay==L"inline-grid"||
           parentDisplay==L"flex"||parentDisplay==L"inline-flex";
}

bool HasInFlowBlockChildren(const LayoutBox& box) {
    if(!box.inFlowBlockChildrenValid){
        box.inFlowBlockChildren=std::any_of(box.children.begin(),box.children.end(),[](const auto& child){
            if(!child->visible||child->style.Is(L"position",L"absolute")||
               child->style.Is(L"position",L"fixed")||child->style.Get(L"float",L"none")!=L"none")return false;
            const auto display=child->style.Get(L"display");
            return !IsInlineLevel(display)||(display==L"inline"&&HasInFlowBlockChildren(*child));
        });
        box.inFlowBlockChildrenValid=true;
    }
    return box.inFlowBlockChildren;
}

int ZIndex(const LayoutBox& box) {
    const auto value=Trim(ToLower(box.style.Get(L"z-index",L"auto")));
    if(value.empty()||value==L"auto")return 0;
    size_t used=0;int parsed=0;
    return TryParseInteger(value,parsed,&used)&&used==value.size()?parsed:0;
}

bool HasExplicitZIndex(const LayoutBox& box) {
    const auto value=Trim(ToLower(box.style.Get(L"z-index",L"auto")));
    return !value.empty()&&value!=L"auto";
}

bool IsStackingContext(const LayoutBox& box) {
    if(!box.parent)return true;
    const auto position=box.style.Get(L"position",L"static");
    if(position==L"fixed"||position==L"sticky")return true;
    const auto parentDisplay=box.parent->style.Get(L"display");
    const bool positioned=position!=L"static";
    const bool flexOrGridItem=parentDisplay==L"flex"||parentDisplay==L"inline-flex"||
        parentDisplay==L"grid"||parentDisplay==L"inline-grid";
    if(HasExplicitZIndex(box)&&(positioned||flexOrGridItem))return true;
    if(box.style.Get(L"transform",L"none")!=L"none")return true;
    if(box.style.Get(L"clip-path",L"none")!=L"none")return true;
    float opacity=1;return TryParseFloat(box.style.Get(L"opacity",L"1"),opacity)&&opacity<0.999f;
}

bool ClipsOverflow(const LayoutBox& box) {
    if(box.overflowFlagsValid)return box.clipsOverflow;
    const auto overflowX=OverflowX(box);
    const auto overflowY=OverflowY(box);
    const auto clips=[](const std::wstring& value){
        return value==L"hidden"||value==L"clip"||value==L"auto"||value==L"scroll";
    };
    return clips(overflowX)||clips(overflowY);
}

LayoutRect OverflowClipRect(const LayoutBox& box) {
    if(!box.overflowClipValid)return PaddingBox(box);
    const auto& offsets=box.overflowClipOffsets;
    return {box.rect.x+offsets.x,box.rect.y+offsets.y,offsets.width,offsets.height};
}

void RefreshOverflowClip(LayoutBox& box,const StyleSheet& styleSheet) {
    box.overflowClipValid=false;
    if(!box.overflowFlagsValid){
        box.clipsOverflow=box.node->type!=NodeType::Text&&ClipsOverflow(box);
        box.overflowFlagsValid=true;
    }
    if(box.clipsOverflow){
        auto clip=PaddingBox(box);
        if(!box.viewportScrollContainer){
            VerticalScrollbarGeometry vertical;HorizontalScrollbarGeometry horizontal;
            if(VerticalScrollbarFor(box,styleSheet,vertical)){
                if(box.style.Is(L"direction",L"rtl"))clip.x+=vertical.track.width;
                clip.width=std::max(0.0f,clip.width-vertical.track.width);
            }
            if(HorizontalScrollbarFor(box,styleSheet,horizontal))
                clip.height=std::max(0.0f,clip.height-horizontal.track.height);
        }
        box.overflowClipOffsets={clip.x-box.rect.x,clip.y-box.rect.y,clip.width,clip.height};
    }
    box.overflowClipValid=true;
}

enum class FloatSide { None, Left, Right };

FloatSide UsedFloatSide(const ComputedStyle& style) {
    const auto value=ToLower(Trim(style.Get(L"float",L"none")));
    if(value==L"left"||value==L"inline-start")return FloatSide::Left;
    if(value==L"right"||value==L"inline-end")return FloatSide::Right;
    return FloatSide::None;
}

bool HasColumnSizing(const ComputedStyle& style){
    const auto count=style.Get(L"column-count",L"auto"),width=style.Get(L"column-width",L"auto");
    return (!count.empty()&&count!=L"auto")||(!width.empty()&&width!=L"auto");
}

bool EstablishesBlockFormattingContext(const LayoutBox& box) {
    // body is stored at the tree root, but remains an ordinary block in html.
    if((!box.parent&&(!box.node||box.node->tag!=L"body"))||UsedFloatSide(box.style)!=FloatSide::None)return true;
    const auto position=ToLower(Trim(box.style.Get(L"position",L"static")));
    if(position==L"absolute"||position==L"fixed")return true;
    const auto display=ToLower(Trim(box.style.Get(L"display",L"block")));
    if((display==L"block"||display==L"flow-root")&&HasColumnSizing(box.style))return true;
    if(display==L"flow-root"||display==L"inline-block"||display==L"table-cell"||
       display==L"table-caption"||display==L"table"||display==L"inline-table"||display==L"flex"||display==L"inline-flex"||
       display==L"grid"||display==L"inline-grid")return true;
    const auto overflow=ToLower(Trim(box.style.Get(L"overflow",L"visible")));
    const auto overflowX=ToLower(Trim(box.style.Get(L"overflow-x",overflow)));
    const auto overflowY=ToLower(Trim(box.style.Get(L"overflow-y",overflow)));
    const auto creates=[](const std::wstring& value){return value!=L"visible"&&value!=L"clip";};
    return creates(overflow)||creates(overflowX)||creates(overflowY);
}

struct FloatArea {
    LayoutRect rect;
    FloatSide side=FloatSide::None;
};

bool ClearIncludes(FloatSide side,const std::wstring& clear) {
    const auto value=ToLower(Trim(clear));
    if(value==L"both")return true;
    if(side==FloatSide::Left)return value==L"left"||value==L"inline-start";
    if(side==FloatSide::Right)return value==L"right"||value==L"inline-end";
    return false;
}

float ClearedFloatY(const std::vector<FloatArea>& floats,float y,const std::wstring& clear) {
    float result=y;
    for(const auto& area:floats)if(ClearIncludes(area.side,clear))
        result=std::max(result,area.rect.y+area.rect.height);
    return result;
}

void AvailableFloatBand(const std::vector<FloatArea>& floats,float containerLeft,
                        float containerRight,float y,float& left,float& right,
                        float& nextBottom) {
    left=containerLeft;right=containerRight;
    nextBottom=std::numeric_limits<float>::infinity();
    for(const auto& area:floats){
        const float bottom=area.rect.y+area.rect.height;
        if(area.rect.y<=y+0.01f&&bottom>y+0.01f){
            if(area.side==FloatSide::Left)left=std::max(left,area.rect.x+area.rect.width);
            else if(area.side==FloatSide::Right)right=std::min(right,area.rect.x);
            nextBottom=std::min(nextBottom,bottom);
        }
    }
}

LayoutRect PlaceFloat(const std::vector<FloatArea>& floats,float containerLeft,
                      float containerRight,float startY,float width,float height,
                      FloatSide side,const std::wstring& clear=L"") {
    float y=ClearedFloatY(floats,startY,clear);
    const float containerWidth=std::max(0.0f,containerRight-containerLeft);
    width=std::min(std::max(0.0f,width),containerWidth);
    for(size_t attempt=0;attempt<=floats.size();++attempt){
        float left=containerLeft,right=containerRight,nextBottom=0;
        AvailableFloatBand(floats,containerLeft,containerRight,y,left,right,nextBottom);
        if(width<=right-left+0.01f)
            return {side==FloatSide::Right?right-width:left,y,width,height};
        if(!std::isfinite(nextBottom)||nextBottom<=y+0.01f)break;
        y=nextBottom;
    }
    return {side==FloatSide::Right?containerRight-width:containerLeft,y,width,height};
}

LayoutRect IntersectRects(const LayoutRect& left,const LayoutRect& right) {
    const float x=std::max(left.x,right.x),y=std::max(left.y,right.y);
    const float r=std::min(left.x+left.width,right.x+right.width);
    const float b=std::min(left.y+left.height,right.y+right.height);
    return {x,y,std::max(0.0f,r-x),std::max(0.0f,b-y)};
}

LayoutRect StackingContextClip(const LayoutBox& context,const LayoutBox& scope,
                               LayoutRect clip) {
    for(auto* ancestor=context.parent;ancestor&&ancestor!=&scope;ancestor=ancestor->parent)
        if(ClipsOverflow(*ancestor))clip=IntersectRects(clip,OverflowClipRect(*ancestor));
    return clip;
}

bool StackingContextAllowsPoint(const LayoutBox& context,const LayoutBox& scope,
                                float x,float y) {
    for(auto* ancestor=context.parent;ancestor&&ancestor!=&scope;ancestor=ancestor->parent)
        if(ClipsOverflow(*ancestor)&&!OverflowClipRect(*ancestor).Contains(x,y))return false;
    return true;
}

bool IsDeferredContext(const LayoutBox& box,const LayoutBox* scope) {
    return scope&&box.deferredStackingScope==scope;
}

bool Intersects(const LayoutRect& left,const LayoutRect& right) {
    return left.x<right.x+right.width&&left.x+left.width>right.x&&
           left.y<right.y+right.height&&left.y+left.height>right.y;
}

void UpdateSubtreeBounds(LayoutBox& box) {
    box.subtreeBounds=box.viewportScrollContainer?box.viewportScrollport:box.rect;
    for(const auto& child:box.children){
        if(!child->visible)continue;
        const auto& bounds=child->subtreeBounds;
        const float left=std::min(box.subtreeBounds.x,bounds.x);
        const float top=std::min(box.subtreeBounds.y,bounds.y);
        const float right=std::max(box.subtreeBounds.x+box.subtreeBounds.width,bounds.x+bounds.width);
        const float bottom=std::max(box.subtreeBounds.y+box.subtreeBounds.height,bounds.y+bounds.height);
        box.subtreeBounds={left,top,right-left,bottom-top};
    }
}

template<class BoxPointer>
void StableStackingOrder(std::vector<BoxPointer>& boxes) {
    std::stable_sort(boxes.begin(),boxes.end(),[](const BoxPointer left,const BoxPointer right){
        return ZIndex(*left)<ZIndex(*right);
    });
}

LayoutBox* SingleLineClippedTextChild(LayoutBox& box) {
    if(!box.style.Is(L"white-space",L"nowrap"))return nullptr;
    const auto overflow=box.style.Get(L"overflow",L"visible");
    const auto overflowX=box.style.Get(L"overflow-x",overflow);
    if(overflow!=L"hidden"&&overflowX!=L"hidden")return nullptr;

    LayoutBox* textChild=nullptr;
    for(auto& child:box.children){
        if(!child->visible)continue;
        if(textChild||child->node->type!=NodeType::Text||
           child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))
            return nullptr;
        textChild=child.get();
    }
    return textChild;
}

std::vector<std::wstring> Words(const std::wstring& text);

CornerRadii ResolveCornerRadii(const ComputedStyle& style,float width,float height,float viewportWidth) {
    CornerRadii radii;
    if(width<=0||height<=0)return radii;
    const auto raw=Trim(style.Get(L"border-radius",L"0"));
    const auto slash=raw.find(L'/');
    auto horizontal=Words(raw.substr(0,slash));
    auto vertical=slash==std::wstring::npos?horizontal:Words(raw.substr(slash+1));
    const auto expand=[](std::vector<std::wstring> values){
        if(values.empty()||values.size()>4)return std::array<std::wstring,4>{L"0",L"0",L"0",L"0"};
        return std::array<std::wstring,4>{values[0],values.size()>1?values[1]:values[0],
            values.size()>2?values[2]:values[0],values.size()>3?values[3]:(values.size()>1?values[1]:values[0])};
    };
    const auto xs=expand(horizontal),ys=expand(vertical);
    static const wchar_t* names[]={L"border-top-left-radius",L"border-top-right-radius",
        L"border-bottom-right-radius",L"border-bottom-left-radius"};
    for(size_t i=0;i<4;++i){
        const auto longhand=Words(style.Get(names[i]));
        const auto& horizontalValue=longhand.empty()?xs[i]:longhand[0];
        const auto& verticalValue=longhand.empty()?ys[i]:(longhand.size()>1?longhand[1]:longhand[0]);
        auto& corner=radii.corners[i];
        corner.x=std::max(0.0f,StyleSheet::Length(horizontalValue,width,viewportWidth,0));
        corner.y=std::max(0.0f,StyleSheet::Length(verticalValue,height,viewportWidth,0));
        if(corner.x==0||corner.y==0)corner={0,0};
    }
    // One common factor preserves the relationships between all eight radii.
    float scale=1;
    const auto limit=[&](float length,float sum){if(sum>0)scale=std::min(scale,length/sum);};
    const auto& c=radii.corners;
    limit(width,c[0].x+c[1].x);limit(width,c[3].x+c[2].x);
    limit(height,c[0].y+c[3].y);limit(height,c[1].y+c[2].y);
    for(auto& corner:radii.corners){corner.x*=scale;corner.y*=scale;}
    radii.x=c[0].x;radii.y=c[0].y;
    return radii;
}

CornerRadii InsetCornerRadii(CornerRadii radii,const Edges& inset) {
    const float xs[]={inset.left,inset.right,inset.right,inset.left};
    const float ys[]={inset.top,inset.top,inset.bottom,inset.bottom};
    for(size_t i=0;i<4;++i){
        auto& corner=radii.corners[i];
        if(corner.x<=0||corner.y<=0){corner={0,0};continue;}
        corner.x=std::max(0.0f,corner.x-xs[i]);corner.y=std::max(0.0f,corner.y-ys[i]);
        if(corner.x==0||corner.y==0)corner={0,0};
    }
    radii.x=radii.corners[0].x;radii.y=radii.corners[0].y;
    return radii;
}

Microsoft::WRL::ComPtr<ID2D1Geometry> RoundedBoxGeometry(
        ID2D1Factory* factory,const D2D1_RECT_F& rect,const CornerRadii& radii) {
    Microsoft::WRL::ComPtr<ID2D1Geometry> result;
    if(!factory)return result;
    if(radii.Uniform()){
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geometry;
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect,radii.x,radii.y),&geometry);
        return geometry;
    }
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(factory->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink)))return result;
    const auto& c=radii.corners;
    sink->BeginFigure(D2D1::Point2F(rect.left+c[0].x,rect.top),D2D1_FIGURE_BEGIN_FILLED);
    const auto arc=[&](D2D1_POINT_2F end,const D2D1_POINT_2F& radius){
        if(radius.x>0&&radius.y>0)sink->AddArc(D2D1::ArcSegment(end,D2D1::SizeF(radius.x,radius.y),
            0,D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
        else sink->AddLine(end);
    };
    sink->AddLine(D2D1::Point2F(rect.right-c[1].x,rect.top));
    arc(D2D1::Point2F(rect.right,rect.top+c[1].y),c[1]);
    sink->AddLine(D2D1::Point2F(rect.right,rect.bottom-c[2].y));
    arc(D2D1::Point2F(rect.right-c[2].x,rect.bottom),c[2]);
    sink->AddLine(D2D1::Point2F(rect.left+c[3].x,rect.bottom));
    arc(D2D1::Point2F(rect.left,rect.bottom-c[3].y),c[3]);
    sink->AddLine(D2D1::Point2F(rect.left,rect.top+c[0].y));
    arc(D2D1::Point2F(rect.left+c[0].x,rect.top),c[0]);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if(SUCCEEDED(sink->Close()))result=geometry;
    return result;
}

Microsoft::WRL::ComPtr<ID2D1Geometry> RoundedBoxGeometry(
        ID2D1RenderTarget* target,const D2D1_RECT_F& rect,const CornerRadii& radii) {
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;target->GetFactory(&factory);
    return RoundedBoxGeometry(factory.Get(),rect,radii);
}

void FillRoundedBox(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,
                    const CornerRadii& radius,ID2D1Brush* brush) {
    if(!radius.Any()){target->FillRectangle(rect,brush);return;}
    if(!radius.Uniform()&&activeRasterSurface&&activeRasterSurface->target==target){
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solid;
        if(SUCCEEDED(brush->QueryInterface(IID_PPV_ARGS(&solid)))){
            auto color=solid->GetColor();color.a*=solid->GetOpacity();
            const auto byte=[](float v){return static_cast<unsigned int>(std::lround(std::clamp(v,0.0f,1.0f)*255));};
            const auto argb=(byte(color.a)<<24)|(byte(color.r)<<16)|(byte(color.g)<<8)|byte(color.b);
            if(activeRasterSurface->PaintSkia(rect,radius.corners,argb))return;
        }
    }
    if(radius.Uniform()&&std::abs(rect.right-rect.left-(rect.bottom-rect.top))<0.0001f&&
       radius.x*2+0.0001f>=rect.right-rect.left&&radius.y*2+0.0001f>=rect.bottom-rect.top&&
       activeRasterSurface&&activeRasterSurface->target==target&&activeRasterSurface->FillCircle(rect,brush))return;
    if(radius.Uniform()&&radius.x==radius.y&&activeRasterSurface&&activeRasterSurface->target==target&&
       activeRasterSurface->FillRoundedRect(rect,radius.x,brush))return;
    if(radius.Uniform()){target->FillRoundedRectangle(D2D1::RoundedRect(rect,radius.x,radius.y),brush);return;}
    if(auto geometry=RoundedBoxGeometry(target,rect,radius))target->FillGeometry(geometry.Get(),brush);
}

bool PaintSoftwareRoundedRect(ID2D1RenderTarget* target,const D2D1_ROUNDED_RECT& rect,
                         ID2D1Brush* brush,float strokeWidth=0){
    return target->GetAntialiasMode()!=D2D1_ANTIALIAS_MODE_ALIASED&&rect.radiusX==rect.radiusY&&
        activeRasterSurface&&activeRasterSurface->target==target&&
        activeRasterSurface->FillRoundedRect(rect.rect,rect.radiusX,brush,strokeWidth);
}

std::vector<std::wstring> Words(const std::wstring& text) {
    std::vector<std::wstring> result; std::wstring current; int nesting=0;
    for (wchar_t c : text) {
        if (c==L'(') ++nesting; if(c==L')') --nesting;
        if (std::iswspace(c) && nesting==0) { if(!current.empty()){result.push_back(current);current.clear();} }
        else current+=c;
    }
    if(!current.empty())result.push_back(current);return result;
}

float FontSize(const ComputedStyle& style);
std::vector<float> SvgNumbers(const std::wstring& source);

std::vector<std::wstring> CommaSeparated(const std::wstring& text) {
    std::vector<std::wstring> result;size_t start=0;int nesting=0;
    for(size_t i=0;i<=text.size();++i){
        const wchar_t c=i<text.size()?text[i]:L',';
        if(c==L'(')++nesting;else if(c==L')')--nesting;
        else if(c==L','&&nesting==0){auto item=Trim(text.substr(start,i-start));if(!item.empty())result.push_back(item);start=i+1;}
    }
    return result;
}

struct TransitionDefinition {
    std::wstring property = L"all";
    float durationMs = 0;
    float delayMs = 0;
    float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1.0f;
};

bool TransitionTime(const std::wstring& token,float& milliseconds) {
    const auto value=ToLower(Trim(token));
    size_t consumed=0;float number=0;
    if(!TryParseFloat(value,number,&consumed))return false;
    const auto unit=value.substr(consumed);
    if(unit==L"ms"){milliseconds=number;return true;}
    if(unit==L"s"){milliseconds=number*1000.0f;return true;}
    return false;
}

bool TransitionTiming(const std::wstring& token,TransitionDefinition& definition) {
    const auto value=ToLower(Trim(token));
    if(value==L"linear"){definition.x1=0;definition.y1=0;definition.x2=1;definition.y2=1;return true;}
    if(value==L"ease"){definition.x1=.25f;definition.y1=.1f;definition.x2=.25f;definition.y2=1;return true;}
    if(value==L"ease-in"){definition.x1=.42f;definition.y1=0;definition.x2=1;definition.y2=1;return true;}
    if(value==L"ease-out"){definition.x1=0;definition.y1=0;definition.x2=.58f;definition.y2=1;return true;}
    if(value==L"ease-in-out"){definition.x1=.42f;definition.y1=0;definition.x2=.58f;definition.y2=1;return true;}
    if(value.rfind(L"cubic-bezier(",0)!=0||value.back()!=L')')return false;
    const auto values=CommaSeparated(value.substr(13,value.size()-14));
    if(values.size()!=4)return false;
    size_t used=0;
    if(!TryParseFloat(values[0],definition.x1,&used)||used!=values[0].size()||
       !TryParseFloat(values[1],definition.y1,&used)||used!=values[1].size()||
       !TryParseFloat(values[2],definition.x2,&used)||used!=values[2].size()||
       !TryParseFloat(values[3],definition.y2,&used)||used!=values[3].size())return false;
    definition.x1=std::max(0.0f,std::min(1.0f,definition.x1));
    definition.x2=std::max(0.0f,std::min(1.0f,definition.x2));
    return true;
}

std::vector<TransitionDefinition> TransitionDefinitions(const ComputedStyle& style) {
    std::vector<TransitionDefinition> result;
    const auto shorthand=Trim(style.Get(L"transition"));
    if(shorthand.empty()||ToLower(shorthand)==L"none")return result;
    for(const auto& item:CommaSeparated(shorthand)){
        TransitionDefinition definition;bool haveDuration=false;
        for(const auto& word:Words(item)){
            float time=0;
            if(TransitionTime(word,time)){
                if(!haveDuration){definition.durationMs=std::max(0.0f,time);haveDuration=true;}
                else definition.delayMs=time;
            }else if(!TransitionTiming(word,definition))definition.property=ToLower(word);
        }
        result.push_back(std::move(definition));
    }
    return result;
}

const TransitionDefinition* TransitionFor(const std::vector<TransitionDefinition>& definitions,
                                          const std::wstring& property) {
    const TransitionDefinition* all=nullptr;const TransitionDefinition* exact=nullptr;
    for(const auto& definition:definitions){
        if(definition.property==L"all")all=&definition;
        else if(definition.property==property)exact=&definition;
    }
    return exact?exact:all;
}

struct NumericToken { size_t offset=0,length=0;double value=0; };

std::vector<NumericToken> NumericTokens(const std::wstring& value) {
    std::vector<NumericToken> result;
    for(size_t index=0;index<value.size();){
        const bool sign=(value[index]==L'+'||value[index]==L'-')&&index+1<value.size()&&
            (std::iswdigit(value[index+1])||(value[index+1]==L'.'&&index+2<value.size()&&std::iswdigit(value[index+2])));
        const bool start=std::iswdigit(value[index])||
            (value[index]==L'.'&&index+1<value.size()&&std::iswdigit(value[index+1]))||sign;
        if(!start){++index;continue;}
        wchar_t* end=nullptr;const double number=std::wcstod(value.c_str()+index,&end);
        if(end==value.c_str()+index){++index;continue;}
        const size_t length=static_cast<size_t>(end-(value.c_str()+index));
        result.push_back({index,length,number});index+=length;
    }
    return result;
}

std::wstring NumberText(double value) {
    if(std::abs(value)<0.0000005)value=0;
    // CSS numbers and cache keys are ASCII, independent of the process locale.
    // Avoid constructing a locale-aware stream for every text/grid measurement.
    char buffer[384];
    const auto converted=std::to_chars(std::begin(buffer),std::end(buffer),value,
        std::chars_format::fixed,6);
    std::wstring text(buffer,converted.ptr);
    while(text.size()>1&&text.back()==L'0')text.pop_back();
    if(!text.empty()&&text.back()==L'.')text.pop_back();return text;
}

std::wstring IdentityTransform(const std::wstring& model) {
    const auto numbers=NumericTokens(model);if(numbers.empty())return L"none";
    std::wstring result;size_t cursor=0;
    for(const auto& number:numbers){
        result.append(model,cursor,number.offset-cursor);
        const auto open=model.rfind(L'(',number.offset);
        size_t begin=open;
        while(begin!=std::wstring::npos&&begin>0&&(std::iswalpha(model[begin-1])||model[begin-1]==L'-'))--begin;
        const auto function=open==std::wstring::npos?L"":ToLower(model.substr(begin,open-begin));
        result+=function.rfind(L"scale",0)==0?L"1":L"0";
        cursor=number.offset+number.length;
    }
    result.append(model,cursor,std::wstring::npos);return result;
}

std::wstring TransitionBaseValue(const ComputedStyle& style,const std::wstring& property,
                                 const std::wstring& other=L"") {
    auto value=Trim(style.Get(property));
    if(property==L"opacity"&&value.empty())return L"1";
    if(property==L"visibility"&&value.empty())return L"visible";
    if(property==L"transform"&&(value.empty()||ToLower(value)==L"none"))
        return other.empty()?L"none":IdentityTransform(other);
    return value;
}

bool CanInterpolateNumbers(const std::wstring& from,const std::wstring& to) {
    const auto first=NumericTokens(from),second=NumericTokens(to);
    return !first.empty()&&first.size()==second.size();
}

bool IsNumericTransitionProperty(const std::wstring& property) {
    if(property==L"opacity"||property==L"transform"||property==L"grid-template-columns"||
       property==L"grid-template-rows"||property==L"gap"||property==L"row-gap"||
       property==L"column-gap"||property==L"font-size"||property==L"font-weight"||
       property==L"line-height"||property==L"letter-spacing"||property==L"word-spacing"||
       property==L"text-indent"||property==L"flex-grow"||property==L"flex-shrink"||
       property==L"flex-basis"||property==L"stroke-width"||property==L"outline-width"||
       property==L"background-position"||property==L"background-size"||
       property==L"object-position"||property==L"perspective"||property==L"z-index"||
       property==L"order")return true;
    for(const auto* prefix:{L"width",L"height",L"min-width",L"max-width",L"min-height",L"max-height",
                            L"top",L"right",L"bottom",L"left",L"margin",L"padding",L"inset",
                            L"border-radius",L"border-top-left-radius",L"border-top-right-radius",
                            L"border-bottom-left-radius",L"border-bottom-right-radius",
                            L"border-top-width",L"border-right-width",L"border-bottom-width",L"border-left-width"})
        if(property==prefix)return true;
    return false;
}

float CubicCoordinate(float t,float first,float second) {
    const float inverse=1-t;
    return 3*inverse*inverse*t*first+3*inverse*t*t*second+t*t*t;
}

float TransitionProgress(const StyleTransition& transition) {
    if(transition.elapsedMs<=transition.delayMs)return 0;
    if(transition.durationMs<=0)return 1;
    const float linear=std::max(0.0f,std::min(1.0f,
        (transition.elapsedMs-transition.delayMs)/transition.durationMs));
    float low=0,high=1;
    for(int iteration=0;iteration<18;++iteration){
        const float middle=(low+high)/2;
        if(CubicCoordinate(middle,transition.x1,transition.x2)<linear)low=middle;else high=middle;
    }
    return CubicCoordinate((low+high)/2,transition.y1,transition.y2);
}

std::wstring TransitionValue(const StyleTransition& transition) {
    const float progress=TransitionProgress(transition);
    if(transition.discrete)return progress>=1?transition.to:transition.from;
    const auto from=NumericTokens(transition.from),to=NumericTokens(transition.to);
    if(from.empty()||from.size()!=to.size())return progress>=1?transition.to:transition.from;
    std::wstring result;size_t cursor=0;
    for(size_t index=0;index<to.size();++index){
        result.append(transition.to,cursor,to[index].offset-cursor);
        result+=NumberText(from[index].value+(to[index].value-from[index].value)*progress);
        cursor=to[index].offset+to[index].length;
    }
    result.append(transition.to,cursor,std::wstring::npos);return result;
}

bool TransitionComplete(const StyleTransition& transition) {
    return transition.elapsedMs>=transition.delayMs+transition.durationMs;
}

std::vector<StyleAnimation> AnimationDefinitions(const ComputedStyle& style){
    std::vector<StyleAnimation> result;
    for(const auto& entry:CommaSeparated(style.Get(L"animation"))){
        StyleAnimation animation;bool duration=false;
        TransitionDefinition timing;
        for(const auto& word:Words(entry)){
            const auto lower=ToLower(word);float number=0;size_t used=0;
            if(TransitionTime(word,number)){if(!duration){animation.durationMs=std::max(0.0f,number);duration=true;}else animation.delayMs=number;}
            else if(TransitionTiming(word,timing)){}
            else if(lower==L"infinite")animation.iterations=std::numeric_limits<float>::infinity();
            else if(TryParseFloat(word,number,&used)&&used==word.size()&&number>=0)animation.iterations=number;
            else if(lower==L"normal"||lower==L"reverse"||lower==L"alternate"||lower==L"alternate-reverse")animation.direction=lower;
            else if(lower==L"forwards"||lower==L"backwards"||lower==L"both"||lower==L"none")animation.fill=lower;
            else if(lower==L"paused"||lower==L"running")animation.paused=lower==L"paused";
            else animation.name=word;
        }
        animation.x1=timing.x1;animation.y1=timing.y1;animation.x2=timing.x2;animation.y2=timing.y2;
        animation.signature=entry;result.push_back(std::move(animation));
    }
    const auto names=CommaSeparated(style.Get(L"animation-name"));
    if(!names.empty()){
        if(result.empty())result.resize(names.size());else result.resize(names.size(),result.back());
        for(size_t index=0;index<names.size();++index)result[index].name=names[index];
    }
    for(size_t index=0;index<result.size();++index){
        auto& animation=result[index];
        const auto value=[&](const wchar_t* property){const auto entries=CommaSeparated(style.Get(property));return entries.empty()?std::wstring{}:entries[index%entries.size()];};
        float number=0;size_t used=0;TransitionDefinition timing;
        auto token=value(L"animation-duration");if(!token.empty()&&TransitionTime(token,number))animation.durationMs=std::max(0.0f,number);
        token=value(L"animation-delay");if(!token.empty()&&TransitionTime(token,number))animation.delayMs=number;
        token=value(L"animation-iteration-count");if(token==L"infinite")animation.iterations=std::numeric_limits<float>::infinity();
        else if(!token.empty()&&TryParseFloat(token,number,&used)&&used==token.size()&&number>=0)animation.iterations=number;
        token=value(L"animation-direction");if(!token.empty())animation.direction=token;
        token=value(L"animation-fill-mode");if(!token.empty())animation.fill=token;
        token=value(L"animation-play-state");if(!token.empty())animation.paused=token==L"paused";
        token=value(L"animation-timing-function");if(!token.empty()&&TransitionTiming(token,timing)){
            animation.x1=timing.x1;animation.y1=timing.y1;animation.x2=timing.x2;animation.y2=timing.y2;
        }
        animation.signature+=L"|"+animation.name+L"|"+NumberText(animation.durationMs)+L"|"+NumberText(animation.delayMs)+L"|"+
            NumberText(animation.iterations)+L"|"+animation.direction+L"|"+animation.fill;
    }
    return result;
}

bool AnimationProgress(const StyleAnimation& animation,float& progress){
    const float time=animation.elapsedMs-animation.delayMs;
    const float activeDuration=animation.durationMs*animation.iterations;
    const bool before=time<0,after=animation.durationMs<=0||time>=activeDuration;
    if(before&&animation.fill!=L"backwards"&&animation.fill!=L"both")return false;
    if(after&&animation.fill!=L"forwards"&&animation.fill!=L"both")return false;
    float iteration=0;
    if(before)progress=0;
    else if(after){iteration=std::max(0.0f,std::ceil(animation.iterations)-1);progress=animation.iterations-iteration;}
    else{iteration=std::floor(time/animation.durationMs);progress=(time-iteration*animation.durationMs)/animation.durationMs;}
    const bool odd=std::fmod(iteration,2.0f)>=1;
    if(animation.direction==L"reverse"||(animation.direction==L"alternate"&&odd)||(animation.direction==L"alternate-reverse"&&!odd))progress=1-progress;
    return true;
}

void ApplyAnimationValues(ComputedStyle& style,const StyleAnimation& animation){
    float progress=0;if(!AnimationProgress(animation,progress))return;
    std::vector<std::wstring> properties;
    for(const auto& frame:animation.frames)for(const auto& declaration:frame.declarations)
        if(IsNumericTransitionProperty(declaration.name)&&std::find(properties.begin(),properties.end(),declaration.name)==properties.end())properties.push_back(declaration.name);
    if(properties.empty())return;
    style.values=std::make_shared<ComputedStyle::ValueMap>(*style.values);
    for(const auto& property:properties){
        std::vector<std::pair<float,std::wstring>> points;
        for(const auto& frame:animation.frames)for(const auto& declaration:frame.declarations)
            if(declaration.name==property){if(!points.empty()&&points.back().first==frame.offset)points.back().second=declaration.value;else points.emplace_back(frame.offset,declaration.value);}
        if(points.empty())continue;
        const auto underlying=TransitionBaseValue(style,property,points.front().second);
        if(points.front().first>0)points.insert(points.begin(),{0,underlying});
        if(points.back().first<1)points.emplace_back(1,underlying);
        size_t right=1;while(right<points.size()&&points[right].first<progress)++right;
        if(right>=points.size()){(*style.values)[property]=points.back().second;continue;}
        StyleTransition segment;segment.from=points[right-1].second;segment.to=points[right].second;
        if(property==L"transform"){
            if(segment.from.empty()||segment.from==L"none")segment.from=IdentityTransform(segment.to);
            if(segment.to.empty()||segment.to==L"none")segment.to=IdentityTransform(segment.from);
        }
        segment.durationMs=points[right].first-points[right-1].first;segment.elapsedMs=progress-points[right-1].first;
        segment.x1=animation.x1;segment.y1=animation.y1;segment.x2=animation.x2;segment.y2=animation.y2;
        (*style.values)[property]=TransitionValue(segment);
    }
}

bool IsColorToken(const std::wstring& token) {
    const auto value=ToLower(Trim(token));
    return !value.empty()&&(value[0]==L'#'||value.rfind(L"rgb",0)==0||value.rfind(L"color-mix(",0)==0||value==L"transparent"||
        value==L"white"||value==L"black"||value==L"red"||value==L"blue"||
        value==L"green"||value==L"gray"||value==L"grey"||value==L"orange");
}

std::vector<BoxShadow> BoxShadows(const ComputedStyle& style,float viewport) {
    std::vector<BoxShadow> result;const auto raw=Trim(ToLower(style.Get(L"box-shadow")));
    if(raw.empty()||raw==L"none")return result;
    for(const auto& item:CommaSeparated(raw)){
        BoxShadow shadow;std::vector<std::wstring> lengths;bool valid=true;
        shadow.color=StyleSheet::Color(style.Get(L"color",L"#000000"),0xff000000);
        for(const auto& token:Words(item)){
            if(token==L"inset")shadow.inset=true;
            else if(IsColorToken(token))shadow.color=StyleSheet::Color(token,shadow.color);
            else lengths.push_back(token);
        }
        if(lengths.size()<2||lengths.size()>4)valid=false;
        if(valid){
            shadow.offsetX=StyleSheet::Length(lengths[0],0,viewport,0);
            shadow.offsetY=StyleSheet::Length(lengths[1],0,viewport,0);
            if(lengths.size()>2)shadow.blur=std::max(0.0f,StyleSheet::Length(lengths[2],0,viewport,0));
            if(lengths.size()>3)shadow.spread=StyleSheet::Length(lengths[3],0,viewport,0);
            result.push_back(shadow);
        }
    }
    return result;
}

Edges EdgeValues(const ComputedStyle& style,const std::wstring& base,float reference,float viewport) {
    const size_t kind=base==L"padding"?1:0;
    auto& metrics=StyleMetrics(style);
    if(!metrics.edgeDependenciesValid[kind]){
        for(const auto* suffix:{L"",L"-top",L"-right",L"-bottom",L"-left"}){
            const auto value=style.Get(base+suffix);
            metrics.edgesUseReference[kind]|=value.find(L'%')!=std::wstring::npos;
            metrics.edgesUseViewport[kind]|=value.find_first_of(L"vV")!=std::wstring::npos;
        }
        metrics.edgeDependenciesValid[kind]=true;
    }
    auto& cache=ThreadEdgeValuesCache();
    // Fixed/em edges are identical across intrinsic and final grid widths.
    // Keep percentage and viewport dependencies in the key only when present.
    const EdgeCacheKey key{style.values.get(),metrics.edgesUseReference[kind]?reference:0,
        metrics.edgesUseViewport[kind]?viewport:0,kind==1};
    auto found=cache.find(key);
    if(found!=cache.end()){
        const auto owner=found->second.owner.lock();
        if(owner&&owner.get()==style.values.get())return found->second.value;
        cache.clear();
    }else if(cache.size()>=8192)cache.clear();
    Edges e; auto values=Words(style.Get(base,L"0"));
    auto len=[&](const std::wstring& v){return StyleSheet::Length(v,reference,viewport,0,FontSize(style));};
    if(values.size()==1)e.top=e.right=e.bottom=e.left=len(values[0]);
    else if(values.size()==2){e.top=e.bottom=len(values[0]);e.left=e.right=len(values[1]);}
    else if(values.size()==3){e.top=len(values[0]);e.left=e.right=len(values[1]);e.bottom=len(values[2]);}
    else if(values.size()>=4){e.top=len(values[0]);e.right=len(values[1]);e.bottom=len(values[2]);e.left=len(values[3]);}
    for(auto [name,side]:std::vector<std::pair<std::wstring,float*>>{{base+L"-top",&e.top},{base+L"-right",&e.right},{base+L"-bottom",&e.bottom},{base+L"-left",&e.left}}){auto v=style.Get(name);if(!v.empty())*side=len(v);}
    cache.emplace(key,CachedEdges{style.values,e});
    return e;
}

float BorderWidth(const ComputedStyle& style,const std::wstring& side=L"") {
    auto borderStyle=side.empty()?style.Get(L"border-style"):
        style.Get(L"border-"+side+L"-style",style.Get(L"border-style"));
    if(borderStyle.empty())borderStyle=side.empty()?style.Get(L"border"):
        style.Get(L"border-"+side,style.Get(L"border"));
    std::wistringstream styleTokens(ToLower(borderStyle));std::wstring styleToken;
    while(styleTokens>>styleToken)if(styleToken==L"none"||styleToken==L"hidden")return 0;
    auto base=style.Get(L"border-width",style.Get(L"border"));
    auto value=side.empty()?base:style.Get(L"border-"+side+L"-width",style.Get(L"border-"+side,base));
    const auto snap=[&](float width){
        if(width<=0)return 0.0f;
        const auto scale=std::max(0.01f,style.deviceScale);
        return std::max(1.0f,std::floor(width*scale+0.0001f))/scale;
    };
    std::wistringstream widthTokens(ToLower(value));std::wstring widthToken;
    while(widthTokens>>widthToken){
        if(widthToken==L"thin")return snap(1.0f);if(widthToken==L"medium")return snap(3.0f);if(widthToken==L"thick")return snap(5.0f);
        size_t used=0;float parsed=0;
        if(TryParseFloat(widthToken,parsed,&used)&&used>0)return snap(StyleSheet::Length(widthToken,0,0,0));
    }
    return borderStyle.empty()?0.0f:snap(3.0f);
}

Edges BorderValues(const ComputedStyle& style){
    auto& cached=StyleMetrics(style);if(cached.bordersValid&&cached.borderScale==style.deviceScale)return cached.borders;
    cached.borders.top=BorderWidth(style,L"top");cached.borders.right=BorderWidth(style,L"right");
    cached.borders.bottom=BorderWidth(style,L"bottom");cached.borders.left=BorderWidth(style,L"left");
    cached.borderScale=style.deviceScale;cached.bordersValid=true;return cached.borders;
}

float FontSize(const ComputedStyle& style){
    auto& cached=StyleMetrics(style);if(!cached.fontSizeValid){
        cached.fontSize=StyleSheet::Length(style.Get(L"font-size",L"16px"),16,16,16);
        cached.fontSizeValid=true;
    }return cached.fontSize;
}

// CSS clipping changes paint and pointer regions, never the layout boxes.
// Use the same resolved geometry for masks and hit testing. Percentages are
// relative to the selected reference box, including the normalized diagonal
// for circle radii; they are independent of the output device scale.
struct BasicClipShape {
    enum Kind { None, RoundedRect, Ellipse, Polygon } kind=None;
    LayoutRect rect;
    D2D1_RECT_F ellipseHitRect{};
    CornerRadii radii;
    std::vector<D2D1_POINT_2F> points;
    D2D1_FILL_MODE fill=D2D1_FILL_MODE_WINDING;
    bool referenceOnly=false;
};

bool ClipPosition(const std::vector<std::wstring>& words,float width,float height,
                  float viewport,float font,float& x,float& y,float scale=1){
    x=width*scale/2;y=height*scale/2;
    const auto length=[&](const std::wstring& token,float reference){
        return StyleSheet::Length(token,reference,viewport,
            std::numeric_limits<float>::quiet_NaN(),font,scale);
    };
    const auto horizontal=[](const std::wstring& token){return token==L"left"||token==L"right";};
    const auto vertical=[](const std::wstring& token){return token==L"top"||token==L"bottom";};
    const auto axis=[&](const std::wstring& token,float extent,bool second){
        if(token==L"center")return extent*scale/2;
        if(token==(second?L"top":L"left"))return 0.0f;
        if(token==(second?L"bottom":L"right"))return extent*scale;
        if(horizontal(token)||vertical(token))return std::numeric_limits<float>::quiet_NaN();
        return length(token,extent);
    };
    if(words.empty())return true;
    if(words.size()==1){
        if(vertical(words[0]))y=axis(words[0],height,true);
        else x=axis(words[0],width,false);
    }else if(words.size()==2){
        if(vertical(words[0])||horizontal(words[1])){
            x=axis(words[1],width,false);y=axis(words[0],height,true);
        }else{x=axis(words[0],width,false);y=axis(words[1],height,true);}
    }else if(words.size()<=4){
        bool hasX=false,hasY=false;unsigned centers=0;
        for(size_t i=0;i<words.size();++i){
            const auto& edge=words[i];const bool isX=horizontal(edge),isY=vertical(edge);
            if(!isX&&!isY&&edge!=L"center")return false;
            float offset=0;
            if((isX||isY)&&i+1<words.size()&&!horizontal(words[i+1])&&!vertical(words[i+1])&&words[i+1]!=L"center"){
                offset=length(words[++i],isX?width:height);if(!std::isfinite(offset))return false;
            }
            if(isX){if(hasX)return false;hasX=true;x=edge==L"right"?width*scale-offset:offset;}
            else if(isY){if(hasY)return false;hasY=true;y=edge==L"bottom"?height*scale-offset:offset;}
            else ++centers;
        }
        if(centers&&!hasX){hasX=true;x=width*scale/2;--centers;}
        if(centers&&!hasY){hasY=true;y=height*scale/2;--centers;}
        if(centers)return false;
        if(!hasX||!hasY)return false;
    }else return false;
    return std::isfinite(x)&&std::isfinite(y);
}

BasicClipShape ResolveBasicClip(const LayoutBox& box,float viewport,bool hitTest=false){
    BasicClipShape result;
    const auto raw=ToLower(Trim(box.style.Get(L"clip-path",L"none")));
    if(raw.empty()||raw==L"none")return result;
    std::wstring function,reference=L"border-box";bool hasReference=false;
    for(const auto& word:Words(raw)){
        if(word.find(L'(')!=std::wstring::npos){if(!function.empty())return {};function=word;}
        else{
            if(hasReference)return {};
            if(word!=L"border-box"&&word!=L"padding-box"&&word!=L"content-box"&&word!=L"margin-box"&&
               word!=L"fill-box"&&word!=L"stroke-box"&&word!=L"view-box")return {};
            reference=word;hasReference=true;
        }
    }
    LayoutRect area=box.rect;
    auto radius=ResolveCornerRadii(box.style,area.width,area.height,viewport);
    if(reference==L"padding-box"||reference==L"content-box"||reference==L"fill-box"){
        const auto border=UsedBorderValues(box);
        area={area.x+border.left,area.y+border.top,
            std::max(0.0f,area.width-border.left-border.right),std::max(0.0f,area.height-border.top-border.bottom)};
        radius=InsetCornerRadii(radius,border);
        if(reference!=L"padding-box"){
            const auto padding=EdgeValues(box.style,L"padding",box.parent?box.parent->content.width:viewport,viewport);
            area={area.x+padding.left,area.y+padding.top,
                std::max(0.0f,area.width-padding.left-padding.right),std::max(0.0f,area.height-padding.top-padding.bottom)};
            radius=InsetCornerRadii(radius,padding);
        }
    }else if(reference==L"margin-box"){
        const auto margin=EdgeValues(box.style,L"margin",box.parent?box.parent->content.width:viewport,viewport);
        area={area.x-margin.left,area.y-margin.top,
            std::max(0.0f,area.width+margin.left+margin.right),std::max(0.0f,area.height+margin.top+margin.bottom)};
        // CSS Shapes extends each corner with the cubic margin-box adjustment.
        const float xs[]={margin.left,margin.right,margin.right,margin.left};
        const float ys[]={margin.top,margin.top,margin.bottom,margin.bottom};
        const auto expand=[](float r,float m){
            if(m>0&&r<m){const float delta=r/m-1;return std::max(0.0f,r+m*(1+delta*delta*delta));}
            return std::max(0.0f,r+m);
        };
        for(size_t i=0;i<4;++i)radius.corners[i]={expand(radius.corners[i].x,xs[i]),expand(radius.corners[i].y,ys[i])};
        radius.x=radius.corners[0].x;radius.y=radius.corners[0].y;
    }
    result.rect=area;
    if(function.empty()){result.kind=BasicClipShape::RoundedRect;result.radii=radius;result.referenceOnly=true;return result;}
    const auto open=function.find(L'(');
    if(function.back()!=L')')return {};
    const auto name=function.substr(0,open),arguments=function.substr(open+1,function.size()-open-2);
    const float font=FontSize(box.style);
    const auto length=[&](const std::wstring& token,float extent){
        return StyleSheet::Length(token,extent,viewport,std::numeric_limits<float>::quiet_NaN(),font);
    };
    if(name==L"inset"){
        auto words=Words(arguments);const auto round=std::find(words.begin(),words.end(),L"round");
        const size_t count=static_cast<size_t>(round-words.begin());if(count<1||count>4)return {};
        const std::array<std::wstring,4> values={words[0],count>1?words[1]:words[0],
            count>2?words[2]:words[0],count>3?words[3]:(count>1?words[1]:words[0])};
        float top=length(values[0],area.height),right=length(values[1],area.width),
            bottom=length(values[2],area.height),left=length(values[3],area.width);
        if(!std::isfinite(top)||!std::isfinite(right)||!std::isfinite(bottom)||!std::isfinite(left))return {};
        const bool collapsedY=top+bottom>=area.height,collapsedX=left+right>=area.width;
        if(top+bottom>area.height){const float factor=area.height/(top+bottom);top*=factor;bottom*=factor;}
        if(left+right>area.width){const float factor=area.width/(left+right);left*=factor;right*=factor;}
        result.kind=BasicClipShape::RoundedRect;
        result.rect={area.x+left,area.y+top,collapsedX?0:area.width-left-right,collapsedY?0:area.height-top-bottom};
        if(round!=words.end()){
            if(round+1==words.end())return {};
            std::wstring corners;for(auto it=round+1;it!=words.end();++it){if(!corners.empty())corners+=L' ';corners+=*it;}
            ComputedStyle cornerStyle;(*cornerStyle.values)[L"border-radius"]=corners;
            result.radii=ResolveCornerRadii(cornerStyle,result.rect.width,result.rect.height,viewport);
        }
    }else if(name==L"circle"||name==L"ellipse"){
        const auto words=Words(arguments);const auto at=std::find(words.begin(),words.end(),L"at");
        const size_t count=static_cast<size_t>(at-words.begin());const bool circle=name==L"circle";
        if(count>(circle?1u:2u)||(!circle&&count==1))return {};
        if(at!=words.end()&&at+1==words.end())return {};
        const auto position=at==words.end()?std::vector<std::wstring>{}:std::vector<std::wstring>(at+1,words.end());
        const auto first=count?words[0]:L"closest-side";
        const auto ellipseRect=[&](float scale,float originX,float originY,LayoutRect& rect){
            float x=0,y=0;
            if(!ClipPosition(position,area.width,area.height,viewport,font,x,y,scale))return false;
            const auto radial=[&](const std::wstring& token,float extent,float center){
                const float firstDistance=std::abs(center),secondDistance=std::abs(extent*scale-center);
                if(token==L"closest-side")return std::min(firstDistance,secondDistance);
                if(token==L"farthest-side")return std::max(firstDistance,secondDistance);
                return StyleSheet::Length(token,extent,viewport,std::numeric_limits<float>::quiet_NaN(),font,scale);
            };
            float rx=0,ry=0;
            if(circle){
                if(first==L"closest-side"||first==L"farthest-side"){
                    const float horizontal=radial(first,area.width,x),vertical=radial(first,area.height,y);
                    rx=first==L"closest-side"?std::min(horizontal,vertical):std::max(horizontal,vertical);
                }else rx=radial(first,std::hypot(area.width,area.height)/std::sqrt(2.0f),0);
                ry=rx;
            }else{rx=radial(first,area.width,x);ry=radial(count?words[1]:L"closest-side",area.height,y);}
            if(!std::isfinite(rx)||!std::isfinite(ry)||rx<0||ry<0)return false;
            rect={originX+x-rx,originY+y-ry,2*rx,2*ry};return true;
        };
        if(!ellipseRect(1,area.x,area.y,result.rect))return {};
        result.kind=BasicClipShape::Ellipse;
        // Browser paths are constructed in local device coordinates. Resolve
        // lengths at that scale before rounding to float, rather than scale
        // a global CSS rectangle whose translated edges have already rounded.
        if(hitTest){
            const float scale=std::max(.01f,box.style.deviceScale);LayoutRect hitRect;
            if(ellipseRect(scale,(area.x-box.rect.x)*scale,(area.y-box.rect.y)*scale,hitRect))
                result.ellipseHitRect=D2D1::RectF(hitRect.x,hitRect.y,hitRect.x+hitRect.width,hitRect.y+hitRect.height);
        }
    }else if(name==L"polygon"){
        auto vertices=CommaSeparated(arguments);size_t first=0;
        if(!vertices.empty()&&(vertices[0]==L"evenodd"||vertices[0]==L"nonzero")){
            result.fill=vertices[0]==L"evenodd"?D2D1_FILL_MODE_ALTERNATE:D2D1_FILL_MODE_WINDING;first=1;
        }
        if(vertices.size()-first<3)return {};
        for(size_t i=first;i<vertices.size();++i){
            const auto pair=Words(vertices[i]);if(pair.size()!=2)return {};
            const float x=length(pair[0],area.width),y=length(pair[1],area.height);
            if(!std::isfinite(x)||!std::isfinite(y))return {};
            result.points.push_back({area.x+x,area.y+y});
        }
        result.kind=BasicClipShape::Polygon;
    }
    return result;
}

Microsoft::WRL::ComPtr<ID2D1Geometry> BasicClipGeometry(ID2D1Factory* factory,const BasicClipShape& shape){
    if(!factory)return {};
    if(shape.kind==BasicClipShape::RoundedRect){
        const auto& r=shape.rect;
        return RoundedBoxGeometry(factory,D2D1::RectF(r.x,r.y,r.x+r.width,r.y+r.height),shape.radii);
    }
    if(shape.kind==BasicClipShape::Ellipse){
        Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> geometry;const auto& r=shape.rect;
        factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(r.x+r.width/2,r.y+r.height/2),r.width/2,r.height/2),&geometry);
        return geometry;
    }
    if(shape.kind==BasicClipShape::Polygon){
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if(FAILED(factory->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink)))return {};
        sink->SetFillMode(shape.fill);sink->BeginFigure(shape.points[0],D2D1_FIGURE_BEGIN_FILLED);
        for(size_t i=1;i<shape.points.size();++i)sink->AddLine(shape.points[i]);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);if(FAILED(sink->Close()))return {};
        return geometry;
    }
    return {};
}

bool BasicClipAllowsPoint(const LayoutBox& box,float x,float y,float viewport){
    const auto shape=ResolveBasicClip(box,viewport,true);if(shape.kind==BasicClipShape::None)return true;
    if(shape.kind!=BasicClipShape::Polygon&&(shape.rect.width<=0||shape.rect.height<=0))return false;
    if(!shape.referenceOnly&&shape.kind!=BasicClipShape::Polygon&&!shape.rect.Contains(x,y))return false;
    if(!shape.referenceOnly&&shape.kind==BasicClipShape::Ellipse){
        const float scale=std::max(.01f,box.style.deviceScale);bool contains=false;
        if(rasterSkia.EllipseContains(shape.ellipseHitRect,
            D2D1::Point2F((x-box.rect.x)*scale,(y-box.rect.y)*scale),contains))return contains;
        const auto& r=shape.rect;const float dx=(x-r.x-r.width/2)/(r.width/2),dy=(y-r.y-r.height/2)/(r.height/2);
        return dx*dx+dy*dy<=1;
    }
    if(!shape.referenceOnly&&shape.kind==BasicClipShape::RoundedRect){
        const auto& r=shape.rect;
        const float xs[]={r.x,r.x+r.width,r.x+r.width,r.x};
        const float ys[]={r.y,r.y,r.y+r.height,r.y+r.height};
        for(size_t i=0;i<4;++i){
            const auto radius=shape.radii.corners[i];if(radius.x<=0||radius.y<=0)continue;
            const float dx=(i==0||i==3)?x-xs[i]:xs[i]-x,dy=i<2?y-ys[i]:ys[i]-y;
            if(dx<radius.x&&dy<radius.y){
                const float nx=(dx-radius.x)/radius.x,ny=(dy-radius.y)/radius.y;
                if(nx*nx+ny*ny>1)return false;
            }
        }
        return true;
    }
    static thread_local Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    if(!factory)D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf());
    if(const auto geometry=BasicClipGeometry(factory.Get(),shape)){
        if(shape.referenceOnly){
            Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> pixel;
            const float size=1/std::max(.01f,box.style.deviceScale);
            factory->CreateRectangleGeometry(D2D1::RectF(x,y,x+size,y+size),&pixel);
            D2D1_GEOMETRY_RELATION relation=D2D1_GEOMETRY_RELATION_UNKNOWN;
            if(pixel&&SUCCEEDED(geometry->CompareWithGeometry(pixel.Get(),nullptr,.01f,&relation)))
                return relation!=D2D1_GEOMETRY_RELATION_DISJOINT;
        }
        BOOL contains=FALSE;geometry->FillContainsPoint(D2D1::Point2F(x,y),nullptr,.01f,&contains);return contains!=FALSE;
    }
    return true;
}

bool HitRectContains(const LayoutRect& rect,float x,float y,float scale){
    // Browser point hit tests intersect a one-device-pixel rectangle with box
    // bounds. The path itself still tests the original continuous point.
    const float left=std::floor(x*64)/64,top=std::floor(y*64)/64;
    const float size=1/std::max(.01f,scale);
    return rect.width>0&&rect.height>0&&left+size>rect.x&&left<rect.x+rect.width&&
        top+size>rect.y&&top<rect.y+rect.height;
}

bool RoundedBorderAllowsPoint(const LayoutBox& box,float x,float y,float viewport,float scale){
    const auto radii=ResolveCornerRadii(box.style,box.rect.width,box.rect.height,viewport);
    if(!radii.Any())return true;
    // Border radius limits this box's own hit region. Visible overflow from
    // descendants must still be tested before rejecting the parent's corner.
    const auto& r=box.rect;const float size=1/std::max(.01f,scale);
    const float left=std::max(r.x,std::floor(x*64)/64),top=std::max(r.y,std::floor(y*64)/64);
    const float right=std::min(r.x+r.width,std::floor(x*64)/64+size);
    const float bottom=std::min(r.y+r.height,std::floor(y*64)/64+size);
    if(left>=right||top>=bottom)return false;
    for(size_t i=0;i<4;++i){
        const auto radius=radii.corners[i];if(radius.x<=0||radius.y<=0)continue;
        const bool startX=i==0||i==3,startY=i<2;
        const float cx=startX?r.x+radius.x:r.x+r.width-radius.x;
        const float cy=startY?r.y+radius.y:r.y+r.height-radius.y;
        if((startX?right<cx:left>cx)&&(startY?bottom<cy:top>cy)){
            const float dx=((startX?right:left)-cx)/radius.x;
            const float dy=((startY?bottom:top)-cy)/radius.y;
            if(dx*dx+dy*dy>1)return false;
        }
    }
    return true;
}

IDWriteFactory* SharedWriteFactory();
Microsoft::WRL::ComPtr<IDWriteTextFormat> TextFormat(IDWriteFactory* factory,
                                                      const ComputedStyle& style);
struct FontBoxMetrics { float height=0,baseline=0,xHeight=0,lineGap=0; };
FontBoxMetrics NaturalFontBoxMetrics(const ComputedStyle& style);
FontBoxMetrics InlineFontBoxMetrics(const ComputedStyle& style);
float NaturalFontLineHeight(const ComputedStyle& style);

float LineHeight(const ComputedStyle& style){
    auto& cached=StyleMetrics(style);if(cached.lineHeightValid&&cached.lineHeightScale==style.deviceScale)return cached.lineHeight;
    const auto raw=style.Get(L"line-height");const float font=FontSize(style);
    // CSS normal line-height follows the selected face's ascent, descent and
    // line gap. Keep that metric in DIPs; monitor scaling belongs exclusively
    // to the render target and must not change CSS layout geometry.
    constexpr float fallbackNormalLineHeight=1.2f;
    if(raw.empty()||raw==L"normal")cached.lineHeight=NaturalFontLineHeight(style);
    else{size_t used=0;float multiple=0;
        cached.lineHeight=TryParseFloat(raw,multiple,&used)&&used==raw.size()?
            font*multiple:StyleSheet::Length(raw,font,font,font*fallbackNormalLineHeight);
    }
    cached.lineHeightValid=true;cached.lineHeightScale=style.deviceScale;return cached.lineHeight;
}

bool ParticipatesInEditableContent(const std::shared_ptr<Node>& node){
    for(auto current=node;current;current=current->parent.lock()){
        if(!current->attributes.count(L"contenteditable"))continue;
        const auto value=ToLower(Trim(current->Attribute(L"contenteditable")));
        if(value==L"false")return false;
        if(value.empty()||value==L"true"||value==L"plaintext-only")return true;
    }
    return false;
}

float InlineFormattingDescent(const ComputedStyle& style){
    // An atomic inline-level box uses its bottom margin edge as its baseline.
    // The containing line therefore keeps the parent font's descent below the
    // box. Measure that descent in CSS DIPs; the render target applies the
    // monitor DPI later, so this remains correct at 100%, 150%, and per-monitor
    // DPI changes.
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const DWRITE_MATRIX identity{1,0,0,1,0,0};
        constexpr wchar_t sample[]=L" ";
        if(format&&SUCCEEDED(factory->CreateGdiCompatibleTextLayout(sample,1,
            format.Get(),100000.0f,100000.0f,1.0f,&identity,FALSE,&layout))){
            DWRITE_LINE_METRICS metrics{};UINT32 count=0;
            if(SUCCEEDED(layout->GetLineMetrics(&metrics,1,&count))&&count)
                return std::max(0.0f,metrics.height-metrics.baseline);
        }
    }
    return FontSize(style)*0.25f;
}

bool IsAtomicInlineLevel(const LayoutBox& box){
    const auto display=box.style.Get(L"display");
    return display==L"inline-block"||display==L"inline-flex"||display==L"inline-grid"||display==L"inline-table"||
        (display==L"inline"&&box.node->tag==L"svg");
}

float GapValue(const ComputedStyle& style,bool horizontal,float reference,float viewport) {
    const auto property=horizontal?L"column-gap":L"row-gap";
    return StyleSheet::Length(style.Get(property,style.Get(L"gap",L"0")),reference,viewport,0,FontSize(style));
}

bool IsCollapsibleTextSpace(wchar_t c) {
    return c==L' '||c==L'\t'||c==L'\n'||c==L'\r'||c==L'\f';
}

std::wstring NormalizeText(const std::wstring& text,bool preserve,bool preserveLeading=false,bool preserveTrailing=false) {
    if(preserve)return text;std::wstring out;bool space=false;
    for(wchar_t c:text){if(IsCollapsibleTextSpace(c)){if(!space&&(!out.empty()||preserveLeading))out+=L' ';space=true;}else{out+=c;space=false;}}
    if(!preserveTrailing&&!out.empty()&&out.back()==L' ')out.pop_back();
    return out;
}

bool PreservesSpaces(const std::wstring& whiteSpace){
    return whiteSpace==L"pre"||whiteSpace==L"pre-wrap"||whiteSpace==L"break-spaces";
}

bool PreservesLineBreaks(const std::wstring& whiteSpace){
    return PreservesSpaces(whiteSpace)||whiteSpace==L"pre-line";
}

bool PreventsTextWrapping(const std::wstring& whiteSpace){
    return whiteSpace==L"nowrap"||whiteSpace==L"pre";
}

DWRITE_WORD_WRAPPING TextWrappingMode(const ComputedStyle& style){
    if(PreventsTextWrapping(style.Get(L"white-space")))return DWRITE_WORD_WRAPPING_NO_WRAP;
    const auto wordBreak=style.Get(L"word-break",L"normal");
    if(wordBreak==L"break-all")return DWRITE_WORD_WRAPPING_CHARACTER;
    const auto overflowWrap=style.Get(L"overflow-wrap",L"normal");
    if(overflowWrap==L"anywhere"||overflowWrap==L"break-word"||wordBreak==L"break-word")
        return DWRITE_WORD_WRAPPING_EMERGENCY_BREAK;
    // CSS normal wrapping lets an unbreakable word overflow. DirectWrite's
    // default WRAP instead splits that word and adds spurious line boxes.
    return DWRITE_WORD_WRAPPING_WHOLE_WORD;
}

std::wstring NormalizeText(const std::wstring& text,const std::wstring& whiteSpace,
                           bool preserveLeading=false,bool preserveTrailing=false) {
    if(PreservesSpaces(whiteSpace)){
        std::wstring out;out.reserve(text.size());
        for(size_t index=0;index<text.size();++index){
            // HTML physical CR was normalized by the parser. Chromium's
            // preserved DOM CR is zero-width, not another segment break.
            // A zero-width control retains DOM offsets in layout fragments.
            out+=text[index]==L'\r'?L'\x200b':text[index];
        }
        return out;
    }
    if(whiteSpace!=L"pre-line")return NormalizeText(text,false,preserveLeading,preserveTrailing);
    std::wstring out;bool pendingSpace=false,lineHasText=false;
    for(size_t index=0;index<text.size();++index){
        wchar_t character=text[index];
        if(character==L'\r'||character==L'\n'){
            if(character==L'\r'&&index+1<text.size()&&text[index+1]==L'\n')++index;
            if(!out.empty()&&out.back()==L' ')out.pop_back();
            out+=L'\n';pendingSpace=false;lineHasText=false;continue;
        }
        if(IsCollapsibleTextSpace(character)){pendingSpace=lineHasText;continue;}
        if(pendingSpace)out+=L' ';
        out+=character;pendingSpace=false;lineHasText=true;
    }
    return out;
}

std::uint64_t HashText(const std::wstring& value){std::uint64_t hash=1469598103934665603ull;for(wchar_t c:value){hash^=static_cast<std::uint64_t>(c);hash*=1099511628211ull;}return hash;}
void MixHash(std::uint64_t& seed,std::uint64_t value){seed^=value+0x9e3779b97f4a7c15ull+(seed<<6)+(seed>>2);}

std::uint64_t StyleContextHash(const std::shared_ptr<Node>& node,std::uint64_t parentHash,
                               const StyleSheet& styleSheet,size_t knownIndex=0,size_t knownCount=0,
                               const std::shared_ptr<Node>& knownPrevious={}){
    std::uint64_t hash=parentHash;MixHash(hash,HashText(node->tag));MixHash(hash,static_cast<std::uint64_t>(node->type));if(node->type==NodeType::Text)return hash;
    std::uint64_t attributes=0;for(const auto& item:node->attributes)if(styleSheet.AttributeAffectsStyle(item.first)){std::uint64_t pair=HashText(item.first);MixHash(pair,HashText(item.second));attributes^=pair;}MixHash(hash,attributes);
    std::uint64_t inlineStyle=0;for(const auto& item:node->inlineStyle){std::uint64_t pair=HashText(item.first);MixHash(pair,HashText(item.second));if(node->inlineStylePriority.count(item.first))MixHash(pair,1);inlineStyle^=pair;}MixHash(hash,inlineStyle);
    MixHash(hash,(node->checked?1ull:0ull)|(node->disabled?2ull:0ull)|(node->hovered?4ull:0ull)|(node->focused?8ull:0ull)|(node->focusVisible?16ull:0ull)|(node->focusWithin?128ull:0ull)|(node->indeterminate?256ull:0ull));auto parent=node->parent.lock();if(parent){bool first=false,last=false;std::uint64_t childIndex=0;std::shared_ptr<Node> previous;if(knownIndex){childIndex=knownIndex;first=knownIndex==1;last=knownIndex==knownCount;previous=knownPrevious;}else{std::uint64_t currentIndex=0;for(const auto& sibling:parent->children)if(sibling->type==NodeType::Element){++currentIndex;if(sibling==node){first=currentIndex==1;childIndex=currentIndex;break;}previous=sibling;}for(auto it=parent->children.rbegin();it!=parent->children.rend();++it)if((*it)->type==NodeType::Element){last=*it==node;break;}}MixHash(hash,(first?32ull:0ull)|(last?64ull:0ull));if(styleSheet.UsesNthChildFor(node))MixHash(hash,childIndex);if(previous){MixHash(hash,HashText(previous->tag));MixHash(hash,(previous->checked?1ull:0ull)|(previous->disabled?2ull:0ull)|(previous->focused?4ull:0ull)|(previous->focusVisible?8ull:0ull)|(previous->indeterminate?16ull:0ull));std::uint64_t siblingAttributes=0;for(const auto& item:previous->attributes)if(styleSheet.AttributeAffectsStyle(item.first)){std::uint64_t pair=HashText(item.first);MixHash(pair,HashText(item.second));siblingAttributes^=pair;}MixHash(hash,siblingAttributes);}}return hash;
}

std::vector<std::wstring> CssFontFamilies(const std::wstring& source){
    std::vector<std::wstring> result;wchar_t quote=0;size_t start=0;
    for(size_t index=0;index<=source.size();++index){
        const wchar_t character=index<source.size()?source[index]:L',';
        if(quote){
            if(character==quote&&(index==0||source[index-1]!=L'\\'))quote=0;
            continue;
        }
        if(character==L'\''||character==L'"'){quote=character;continue;}
        if(character!=L',')continue;
        auto family=Trim(source.substr(start,index-start));start=index+1;
        if(family.size()>1&&((family.front()==L'\''&&family.back()==L'\'')||
                            (family.front()==L'"'&&family.back()==L'"')))
            family=family.substr(1,family.size()-2);
        if(!family.empty())result.push_back(std::move(family));
    }
    return result;
}

std::wstring WindowsGenericFontFamily(const std::wstring& family){
    const auto generic=ToLower(Trim(family));
    if(generic==L"serif"||generic==L"ui-serif")return L"Times New Roman";
    if(generic==L"sans-serif"||generic==L"ui-sans-serif")return L"Arial";
    if(generic==L"system-ui")return L"Segoe UI";
    if(generic==L"monospace")return PRIMARYLANGID(GetUserDefaultUILanguage())==LANG_KOREAN?L"GulimChe":L"Consolas";
    if(generic==L"ui-monospace")return L"Consolas";
    if(generic==L"cursive")return L"Comic Sans MS";
    if(generic==L"fantasy")return L"Impact";
    if(generic==L"math")return L"Cambria Math";
    if(generic==L"emoji")return L"Segoe UI Emoji";
    return family;
}

bool SystemFontFamilyExists(const std::wstring& family){
    static thread_local FastMap<std::wstring,bool> cache;
    const auto key=ToLower(family);
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    bool result=false;
    if(auto* factory=SharedWriteFactory()){
        Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
        UINT32 index=0;BOOL exists=FALSE;
        result=SUCCEEDED(factory->GetSystemFontCollection(&collection))&&collection&&
            SUCCEEDED(collection->FindFamilyName(family.c_str(),&index,&exists))&&exists;
    }
    if(cache.size()>256)cache.clear();cache[key]=result;return result;
}

std::wstring ResolveFontFamilyList(const std::wstring& source,
                                   const std::wstring& fallback){
    static thread_local FastMap<std::wstring,std::wstring> cache;
    const auto key=source+L'\x1f'+fallback;
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    for(auto family:CssFontFamilies(source)){
        family=WindowsGenericFontFamily(family);
        if(SystemFontFamilyExists(family)){
            if(cache.size()>256)cache.clear();cache[key]=family;return family;
        }
    }
    auto resolved=WindowsGenericFontFamily(fallback);
    if(!SystemFontFamilyExists(resolved))resolved=L"Segoe UI";
    if(cache.size()>256)cache.clear();cache[key]=resolved;return resolved;
}

std::wstring FontFamily(const ComputedStyle& style) {
    return ResolveFontFamilyList(style.Get(L"font-family",L"Malgun Gothic"),
                                 L"Malgun Gothic");
}

bool UsesGenericMonospaceMetrics(const ComputedStyle& style) {
    auto& cached=StyleMetrics(style);
    if(cached.monospaceValid)return cached.monospace;
    cached.monospaceValid=true;
    for(auto family:CssFontFamilies(style.Get(L"font-family"))){
        const auto generic=ToLower(Trim(family));
        if(generic==L"monospace"||generic==L"ui-monospace")return cached.monospace=true;
        family=WindowsGenericFontFamily(family);
        if(SystemFontFamilyExists(family))return false;
    }
    return false;
}

std::wstring ExplicitKoreanFontFamily(const std::wstring& source){
    size_t start=0;
    while(start<source.size()){
        const auto comma=source.find(L',',start);auto family=Trim(source.substr(start,
            comma==std::wstring::npos?std::wstring::npos:comma-start));
        if(family.size()>1&&((family.front()==L'\''&&family.back()==L'\'')||
                            (family.front()==L'"'&&family.back()==L'"')))
            family=family.substr(1,family.size()-2);
        const auto lower=ToLower(family);
        if(lower.find(L"noto sans kr")!=std::wstring::npos||
           lower.find(L"malgun gothic")!=std::wstring::npos||
           lower.find(L"yu gothic")!=std::wstring::npos||family==L"\ub9d1\uc740 \uace0\ub515")return family;
        if(comma==std::wstring::npos)break;start=comma+1;
    }
    return {};
}

bool IsHangul(wchar_t c) {
    return (c>=0x1100&&c<=0x11ff)||(c>=0x3130&&c<=0x318f)||
           (c>=0xac00&&c<=0xd7af);
}

bool ContainsHangul(const std::wstring& text) {
    return std::any_of(text.begin(),text.end(),IsHangul);
}

bool UsesAutomaticHangulFallback(const ComputedStyle& style) {
    const auto family=ToLower(FontFamily(style));
    return family!=L"arial"&&family.find(L"noto sans kr")==std::wstring::npos&&
           family.find(L"malgun gothic")==std::wstring::npos&&
           family.find(L"yu gothic")==std::wstring::npos;
}

bool IsFormControlText(const LayoutBox& box) {
    if(!box.node)return false;
    if(box.node->tag==L"input"||box.node->tag==L"select"||box.node->tag==L"textarea")return true;
    // An anonymous text run directly owned by a button participates in the
    // native control's metrics. Descendant elements create ordinary CSS boxes
    // and retain standard metrics even when their font is inherited.
    return box.parent&&box.parent->node->tag==L"button";
}

bool IsButtonControlText(const std::shared_ptr<Node>& node) {
    for(auto current=node;current;current=current->parent.lock())
        if(current->tag==L"button")return true;
    return false;
}

bool IsDecorativeControlText(const std::shared_ptr<Node>& node) {
    for(auto current=node;current;current=current->parent.lock()){
        if(ToLower(Trim(current->Attribute(L"aria-hidden")))==L"true")return true;
        if(current->tag==L"button")break;
    }
    return false;
}

int FontWeight(const ComputedStyle& style) {
    const auto raw=style.Get(L"font-weight",L"400");size_t used=0;int parsed=0;
    if(TryParseInteger(raw,parsed,&used)&&used==raw.size())return std::max(1,std::min(999,parsed));
    return style.Is(L"font-weight",L"bold")?700:400;
}

float BrowserSymbolAdvanceAdjustment(wchar_t value,float size) {
    // DirectWrite and Chromium choose different fallback advances for a few
    // common Windows UI symbols. Express the difference in em units so the
    // inline geometry remains stable across font sizes and monitor DPI.
    switch(value){
        case 0x2191:return size*0.065f; // upwards arrow
        case 0x2193:return size*0.05f; // downwards arrow
        case 0x21bb:return size*0.04f; // clockwise open circle arrow
        case 0x2442:
        case 0x2443:return size*-0.025f; // OCR branch/merge marks
        case 0x25c7:return size*0.20f; // white diamond
        case 0x25cf:return size*-0.37f; // black circle
        default:return 0;
    }
}

DWRITE_FONT_STYLE FontStyle(const ComputedStyle& style) {
    const auto value=ToLower(Trim(style.Get(L"font-style",L"normal")));
    return value==L"italic"?DWRITE_FONT_STYLE_ITALIC:
        (value==L"oblique"?DWRITE_FONT_STYLE_OBLIQUE:DWRITE_FONT_STYLE_NORMAL);
}

IDWriteFactory* SharedWriteFactory() {
    static Microsoft::WRL::ComPtr<IDWriteFactory> factory=[] {
        Microsoft::WRL::ComPtr<IDWriteFactory> value;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(value.ReleaseAndGetAddressOf()));
        return value;
    }();
    return factory.Get();
}

Microsoft::WRL::ComPtr<IDWriteRenderingParams> WebTextRenderingParams(
        IDWriteFactory* factory,bool subpixel,DWRITE_RENDERING_MODE mode=DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC) {
    static thread_local std::map<std::tuple<IDWriteFactory*,bool,DWRITE_RENDERING_MODE>,
        Microsoft::WRL::ComPtr<IDWriteRenderingParams>> cache;
    if(!factory)return {};
    const auto key=std::make_tuple(factory,subpixel,mode);
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> params;
    Microsoft::WRL::ComPtr<IDWriteFactory1> factory1;
    if(SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory1)))){
        Microsoft::WRL::ComPtr<IDWriteRenderingParams1> params1;
        if(SUCCEEDED(factory1->CreateCustomRenderingParams(2.2f,0.0f,0.0f,subpixel?1.0f:0.0f,
           subpixel?DWRITE_PIXEL_GEOMETRY_RGB:DWRITE_PIXEL_GEOMETRY_FLAT,
           mode,&params1)))
            params=params1;
    }
    if(!params)factory->CreateCustomRenderingParams(2.2f,0.0f,subpixel?1.0f:0.0f,
        subpixel?DWRITE_PIXEL_GEOMETRY_RGB:DWRITE_PIXEL_GEOMETRY_FLAT,
        mode,&params);
    if(params){if(cache.size()>=8)cache.clear();cache.emplace(key,params);}
    return params;
}

void ConfigureWebTextRendering(ID2D1RenderTarget* target,IDWriteFactory* factory,
                               bool transparentLayer=false) {
    if(!target)return;
    const bool subpixel=!transparentLayer&&
        target->GetTextAntialiasMode()!=D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE&&
        target->GetPixelFormat().alphaMode==D2D1_ALPHA_MODE_IGNORE;
    target->SetTextAntialiasMode(subpixel?D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE:
                                        D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if(auto params=WebTextRenderingParams(factory,subpixel))target->SetTextRenderingParams(params.Get());
}

class ScopedWebTextRendering {
    ID2D1RenderTarget* target_;
    D2D1_TEXT_ANTIALIAS_MODE mode_;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams> params_;
public:
    ScopedWebTextRendering(ID2D1RenderTarget* target,IDWriteFactory* factory,
                          bool transparentLayer=false):target_(target),mode_(target->GetTextAntialiasMode()) {
        target_->GetTextRenderingParams(&params_);
        ConfigureWebTextRendering(target_,factory,transparentLayer);
    }
    ~ScopedWebTextRendering() {
        target_->SetTextAntialiasMode(mode_);
        target_->SetTextRenderingParams(params_.Get());
    }
};

class WebGlyphTextRenderer final : public TextRunObserver {
    ID2D1RenderTarget* target_;
    IDWriteFactory* factory_;
    ID2D1Brush* brush_;
public:
    WebGlyphTextRenderer(ID2D1RenderTarget* target,IDWriteFactory* factory,ID2D1Brush* brush):
        target_(target),factory_(factory),brush_(brush){}
    void FlushRaster(){
        if(activeRasterSurface&&activeRasterSurface->target==target_)activeRasterSurface->FlushGlyphBatch();
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled) override {
        if(!disabled)return E_POINTER;*disabled=FALSE;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* matrix) override {
        if(!matrix)return E_POINTER;D2D1_MATRIX_3X2_F transform{};target_->GetTransform(&transform);
        *matrix={transform._11,transform._12,transform._21,transform._22,transform._31,transform._32};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* scale) override {
        if(!scale)return E_POINTER;FLOAT x=96,y=96;target_->GetDpi(&x,&y);*scale=y/96;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE measuring,
        const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*) override {
        if(!run)return E_INVALIDARG;
        if(activeRasterSurface&&activeRasterSurface->target==target_&&
           activeRasterSurface->DrawGlyphRun(factory_,x,y,*run,brush_))return S_OK;
        FlushRaster();
        FLOAT dpiX=96,dpiY=96;target_->GetDpi(&dpiX,&dpiY);
        D2D1_MATRIX_3X2_F transform{};target_->GetTransform(&transform);
        const float physicalSize=run->fontEmSize*dpiY/96*std::hypot(transform._21,transform._22);
        const auto mode=FontRasterMode(run->fontFace,physicalSize);
        Microsoft::WRL::ComPtr<IDWriteRenderingParams> previous;target_->GetTextRenderingParams(&previous);
        const bool subpixel=target_->GetTextAntialiasMode()==D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE;
        if(auto params=WebTextRenderingParams(factory_,subpixel,mode))target_->SetTextRenderingParams(params.Get());
        // Browser glyph atlases have four horizontal coverage phases. Quantize
        // the raster origin of each shaped glyph, rather than rounding the run
        // origin and then allowing arbitrary fractional advances to select a
        // different DirectWrite coverage phase. Advances and layout stay exact.
        const bool axisAligned=std::abs(transform._12)<0.00001f&&
            std::abs(transform._21)<0.00001f&&transform._11>0&&transform._22>0;
        if(axisAligned&&!run->isSideways&&run->glyphAdvances&&run->glyphIndices){
            const float scaleX=dpiX/96*transform._11,scaleY=dpiY/96*transform._22;
            const float translateX=dpiX/96*transform._31,translateY=dpiY/96*transform._32;
            const bool rtl=(run->bidiLevel&1)!=0;
            float pen=x;
            for(UINT32 index=0;index<run->glyphCount;++index){
                if(rtl)pen-=run->glyphAdvances[index];
                const DWRITE_GLYPH_OFFSET offset=run->glyphOffsets?run->glyphOffsets[index]:DWRITE_GLYPH_OFFSET{};
                const float rawX=pen+(rtl?-offset.advanceOffset:offset.advanceOffset);
                const float rawY=y-offset.ascenderOffset;
                const float snappedX=(std::floor((rawX*scaleX+translateX)*4+0.5f)/4-translateX)/scaleX;
                const float snappedY=(std::floor(rawY*scaleY+translateY+0.5f)-translateY)/scaleY;
                const float advance=0;
                auto glyph=*run;glyph.glyphCount=1;glyph.glyphIndices=run->glyphIndices+index;
                glyph.glyphAdvances=&advance;glyph.glyphOffsets=nullptr;glyph.bidiLevel=0;
                target_->DrawGlyphRun(D2D1::Point2F(snappedX,snappedY),&glyph,brush_,measuring);
                if(!rtl)pen+=run->glyphAdvances[index];
            }
        }else target_->DrawGlyphRun(D2D1::Point2F(x,y),run,brush_,measuring);
        target_->SetTextRenderingParams(previous.Get());return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT x,FLOAT y,const DWRITE_UNDERLINE* line,IUnknown*) override {
        if(!line)return E_INVALIDARG;
        FlushRaster();
        target_->FillRectangle(D2D1::RectF(x,y+line->offset,x+line->width,y+line->offset+line->thickness),brush_);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT x,FLOAT y,const DWRITE_STRIKETHROUGH* line,IUnknown*) override {
        if(!line)return E_INVALIDARG;
        FlushRaster();
        target_->FillRectangle(D2D1::RectF(x,y+line->offset,x+line->width,y+line->offset+line->thickness),brush_);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context,FLOAT x,FLOAT y,IDWriteInlineObject* object,BOOL sideways,BOOL rtl,IUnknown* effect) override {
        FlushRaster();
        return object?object->Draw(context,this,x,y,sideways,rtl,effect):E_INVALIDARG;
    }
};

bool DrawHorizontalScrollbarIcons(ID2D1RenderTarget* target,IDWriteFactory* factory,
                                  const HorizontalScrollbarGeometry& scrollbar,ID2D1Brush* brush){
    if(!factory||scrollbar.compactArrows)return false;
    struct Icons {
        bool attempted=false;
        Microsoft::WRL::ComPtr<IDWriteFontFace> face;
        std::array<UINT16,2> glyphs{};
    };
    static thread_local Icons icons;
    if(!icons.attempted){
        icons.attempted=true;
        Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
        Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
        Microsoft::WRL::ComPtr<IDWriteFont> font;
        UINT32 index=0;BOOL exists=FALSE;
        if(SUCCEEDED(factory->GetSystemFontCollection(&collection))&&
           SUCCEEDED(collection->FindFamilyName(L"Segoe Fluent Icons",&index,&exists))&&exists&&
           SUCCEEDED(collection->GetFontFamily(index,&family))&&
           SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STRETCH_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,&font))&&SUCCEEDED(font->CreateFontFace(&icons.face))){
            const UINT32 codes[]{0xedd9,0xedda};
            if(FAILED(icons.face->GetGlyphIndices(codes,2,icons.glyphs.data()))||
               !icons.glyphs[0]||!icons.glyphs[1])icons.face.Reset();
        }
    }
    if(!icons.face)return false;
    FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
    const float scale=dpiX/96;
    const float button=std::round(scrollbar.arrowWidth*scale);
    const float side=std::ceil(9.0f*button/18.0f);
    const float offset=std::round(button/18.0f);
    const float y=scrollbar.track.y+(scrollbar.track.height+side/scale)/2.0f;
    const std::array<float,2> x{
        scrollbar.track.x+((button-side)/2-offset)/scale,
        scrollbar.track.x+scrollbar.track.width-scrollbar.arrowWidth+((button-side)/2+offset)/scale};
    const auto aa=target->GetTextAntialiasMode();target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    const float advance=0;const DWRITE_GLYPH_OFFSET placement{};
    if(activeRasterSurface&&activeRasterSurface->target==target)activeRasterSurface->BeginGlyphBatch();
    for(size_t n=0;n<2;++n){
        const DWRITE_GLYPH_RUN glyph{icons.face.Get(),side/scale,1,&icons.glyphs[n],&advance,&placement,FALSE,0};
        if(!activeRasterSurface||activeRasterSurface->target!=target||
           !activeRasterSurface->DrawGlyphRun(factory,x[n],y,glyph,brush,true)){
            if(activeRasterSurface&&activeRasterSurface->target==target)activeRasterSurface->FlushGlyphBatch();
            target->DrawGlyphRun(D2D1::Point2F(x[n],y),&glyph,brush,DWRITE_MEASURING_MODE_NATURAL);
        }
    }
    if(activeRasterSurface&&activeRasterSurface->target==target)activeRasterSurface->EndGlyphBatch();
    target->SetTextAntialiasMode(aa);
    return true;
}

void DrawWebTextLayout(ID2D1RenderTarget* target,IDWriteFactory* factory,D2D1_POINT_2F origin,
                       IDWriteTextLayout* layout,ID2D1Brush* brush){
    WebGlyphTextRenderer renderer(target,factory,brush);
    struct GlyphBatch {
        RasterSurface* surface;
        explicit GlyphBatch(ID2D1RenderTarget* target):surface(activeRasterSurface&&activeRasterSurface->target==target?activeRasterSurface:nullptr){if(surface)surface->BeginGlyphBatch();}
        ~GlyphBatch(){if(surface)surface->EndGlyphBatch();}
    } batch(target);
    layout->Draw(nullptr,&renderer,origin.x,origin.y);
}

Microsoft::WRL::ComPtr<IDWriteFontFace> MatchedFontFace(const ComputedStyle& style){
    static thread_local FastMap<std::wstring,Microsoft::WRL::ComPtr<IDWriteFontFace>> cache;
    const auto key=FontFamily(style)+L'\x1f'+std::to_wstring(FontWeight(style))+L'\x1f'+style.Get(L"font-style");
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    Microsoft::WRL::ComPtr<IDWriteFontFace> face;
    if(auto* factory=SharedWriteFactory()){
        Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
        Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
        Microsoft::WRL::ComPtr<IDWriteFont> font;
        UINT32 index=0;BOOL exists=FALSE;
        if(SUCCEEDED(factory->GetSystemFontCollection(&collection))&&
           SUCCEEDED(collection->FindFamilyName(FontFamily(style).c_str(),&index,&exists))&&exists&&
           SUCCEEDED(collection->GetFontFamily(index,&family))&&
           SUCCEEDED(family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(FontWeight(style)),
                DWRITE_FONT_STRETCH_NORMAL,FontStyle(style),&font)))font->CreateFontFace(&face);
    }
    if(cache.size()>=256)cache.clear();cache.emplace(key,face);return face;
}

Microsoft::WRL::ComPtr<IDWriteTextFormat> TextFormat(IDWriteFactory* factory,
                                                      const ComputedStyle& style) {
    static thread_local FastMap<std::wstring,Microsoft::WRL::ComPtr<IDWriteTextFormat>> cache;
    std::wstring key=std::to_wstring(reinterpret_cast<std::uintptr_t>(factory));key+=L'\x1f';
    key+=FontFamily(style);key+=L'\x1f';key+=NumberText(FontSize(style));key+=L'\x1f';
    key+=std::to_wstring(FontWeight(style));key+=L'\x1f';key+=style.Get(L"font-style");
    key+=L'\x1f';key+=style.Get(L"direction",L"ltr");
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    if(cache.size()>=256)cache.clear();
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
    if(factory)factory->CreateTextFormat(FontFamily(style).c_str(),nullptr,
        static_cast<DWRITE_FONT_WEIGHT>(FontWeight(style)),FontStyle(style),
        DWRITE_FONT_STRETCH_NORMAL,FontSize(style),L"ko-kr",&format);
    if(format){
        format->SetReadingDirection(style.Is(L"direction",L"rtl")?
            DWRITE_READING_DIRECTION_RIGHT_TO_LEFT:DWRITE_READING_DIRECTION_LEFT_TO_RIGHT);
        cache.emplace(std::move(key),format);
    }
    return format;
}

FontBoxMetrics NaturalFontBoxMetrics(const ComputedStyle& style) {
    static thread_local FastMap<std::wstring,FontBoxMetrics> cache;
    std::wstring key=style.Get(L"font-family");key+=L'\x1f';
    key+=NumberText(FontSize(style));key+=L'\x1f';
    key+=std::to_wstring(FontWeight(style));key+=L'\x1f';key+=style.Get(L"font-style");
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    if(cache.size()>=256)cache.clear();
    const float fontSize=FontSize(style);
    FontBoxMetrics result{fontSize*1.2f,fontSize*0.8f};
    if(UsesGenericMonospaceMetrics(style)){
        result={fontSize,std::round(fontSize*0.85f)};
    }else if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        constexpr wchar_t sample[]=L"Hg";
        if(format&&SUCCEEDED(factory->CreateTextLayout(sample,2,format.Get(),
            100000.0f,100000.0f,&layout))){
            DWRITE_LINE_METRICS metrics{};UINT32 count=0;
            if(SUCCEEDED(layout->GetLineMetrics(&metrics,1,&count))&&count&&
               metrics.height>0)
                result={metrics.height,metrics.baseline};
        }
    }
    cache.emplace(std::move(key),result);return result;
}

float NaturalFontLineHeight(const ComputedStyle& style) {
    const auto metrics=InlineFontBoxMetrics(style);
    return metrics.height+metrics.lineGap;
}

FontBoxMetrics InlineFontBoxMetrics(const ComputedStyle& style) {
    const auto& cachedMetrics=StyleMetrics(style);
    if(cachedMetrics.inlineFontValid&&cachedMetrics.inlineFontScale==style.deviceScale)
        return {cachedMetrics.inlineFontHeight,cachedMetrics.inlineFontBaseline,
                cachedMetrics.inlineFontXHeight,cachedMetrics.inlineFontLineGap};
    const auto remember=[&](FontBoxMetrics result){
        auto& cached=StyleMetrics(style);
        cached.inlineFontValid=true;cached.inlineFontScale=style.deviceScale;
        cached.inlineFontHeight=result.height;cached.inlineFontBaseline=result.baseline;
        cached.inlineFontXHeight=result.xHeight;
        cached.inlineFontLineGap=result.lineGap;
        return result;
    };
    static thread_local FastMap<std::wstring,FontBoxMetrics> cache;
    const bool genericMonospace=UsesGenericMonospaceMetrics(style);
    std::wstring key=style.Get(L"font-family");key+=L'\x1f';
    key+=FontFamily(style);key+=L'\x1f';key+=NumberText(FontSize(style));key+=L'\x1f';
    key+=std::to_wstring(FontWeight(style));key+=L'\x1f';key+=style.Get(L"font-style");
    key+=genericMonospace?L"\x1fmono":L"\x1fresolved";
    key+=L'\x1f';key+=NumberText(style.deviceScale);
    if(const auto found=cache.find(key);found!=cache.end())return remember(found->second);
    if(cache.size()>=256)cache.clear();
    FontBoxMetrics result=NaturalFontBoxMetrics(style);
    result.xHeight=FontSize(style)*0.5f;
    {
        if(auto* factory=SharedWriteFactory()){
            Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
            Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
            Microsoft::WRL::ComPtr<IDWriteFont> font;
            Microsoft::WRL::ComPtr<IDWriteFontFace> face;
            UINT32 familyIndex=0;BOOL exists=FALSE;
            if(SUCCEEDED(factory->GetSystemFontCollection(&collection))&&collection&&
               SUCCEEDED(collection->FindFamilyName(FontFamily(style).c_str(),
                                                    &familyIndex,&exists))&&exists&&
               SUCCEEDED(collection->GetFontFamily(familyIndex,&family))&&
               SUCCEEDED(family->GetFirstMatchingFont(
                    static_cast<DWRITE_FONT_WEIGHT>(FontWeight(style)),
                    DWRITE_FONT_STRETCH_NORMAL,FontStyle(style),&font))&&
               SUCCEEDED(font->CreateFontFace(&face))){
                DWRITE_FONT_METRICS metrics{};face->GetMetrics(&metrics);
                const bool bitmapFont=!FontBitmapSizes(face.Get()).empty();
                if(bitmapFont)face->GetGdiCompatibleMetrics(FontSize(style),
                    std::max(0.01f,style.deviceScale),nullptr,&metrics);
                if(metrics.designUnitsPerEm){
                    const float deviceScale=std::max(0.01f,style.deviceScale);
                    const float scale=FontSize(style)/metrics.designUnitsPerEm;
                    const float baseline=std::round(metrics.ascent*scale*deviceScale)/deviceScale;
                    const float descent=std::round(metrics.descent*scale*deviceScale)/deviceScale;
                    if(baseline+descent>0)result={baseline+descent,baseline,
                        metrics.xHeight>0?std::round(metrics.xHeight*scale*64.0f)/64.0f:
                            FontSize(style)*0.5f,bitmapFont?0.0f:std::round(metrics.lineGap*scale*deviceScale)/deviceScale};
                }
            }
        }
    }
    cache.emplace(std::move(key),result);return remember(result);
}

float InlineContentBoxHeight(const ComputedStyle& style) {
    return InlineFontBoxMetrics(style).height;
}

float InlineHalfLeading(const ComputedStyle& style) {
    // Round ascent/descent and odd leading in device space, then convert
    // once to CSS coordinates. Inline backgrounds and glyph baselines must
    // share these metrics at both 100% and 150% DPI.
    const float leading=(LineHeight(style)-InlineContentBoxHeight(style))/2.0f;
    const float scale=std::max(0.01f,style.deviceScale);
    return std::floor(leading*scale+0.0001f)/scale;
}

float InlineBaselineOffset(const ComputedStyle& parentStyle,
                           const LayoutBox& child,float reference,float viewport) {
    const auto parentMetrics=InlineFontBoxMetrics(parentStyle);
    const auto childMetrics=InlineFontBoxMetrics(child.style);
    float offset=parentMetrics.baseline-childMetrics.baseline;
    if(child.node&&child.node->type==NodeType::Element){
        const auto margin=EdgeValues(child.style,L"margin",reference,viewport);
        const auto padding=EdgeValues(child.style,L"padding",reference,viewport);
        const auto border=BorderValues(child.style);
        offset-=margin.top+border.top+padding.top;
    }
    return offset;
}

float TextBaselineOffset(const ComputedStyle& style) {
    static thread_local FastMap<std::wstring,float> cache;
    std::wstring key=FontFamily(style);key+=L'\x1f';key+=NumberText(FontSize(style));
    key+=L'\x1f';key+=std::to_wstring(FontWeight(style));key+=L'\x1f';
    key+=style.Get(L"font-style");key+=L'\x1f';key+=style.Get(L"line-height");
    key+=L'\x1f';key+=style.Get(L"font-family");
    key+=L'\x1f';key+=NumberText(style.deviceScale);
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    if(cache.size()>=256)cache.clear();
    const auto metrics=InlineFontBoxMetrics(style);
    const float result=std::max(0.0f,metrics.baseline+
        InlineHalfLeading(style));
    cache.emplace(std::move(key),result);return result;
}

std::wstring AlignmentKeyword(std::wstring value){
    value=ToLower(Trim(value));
    const auto separator=value.find_last_of(L" \t\r\n");
    return separator==std::wstring::npos?value:value.substr(separator+1);
}

struct InlineLineMetrics {
    float baseline=0;
    float descent=0;
    float edgeAlignedHeight=0;

    float Height() const {
        return std::max(baseline+descent,edgeAlignedHeight);
    }
};

const ComputedStyle* FirstAvailableBaselineStyle(const LayoutBox& box) {
    if(box.node&&box.node->type==NodeType::Text)return &box.style;
    for(const auto& child:box.children){
        if(!child->visible||child->style.Is(L"position",L"absolute")||
           child->style.Is(L"position",L"fixed"))continue;
        if(const auto* style=FirstAvailableBaselineStyle(*child))return style;
    }
    return nullptr;
}

bool HasCenteredControlLabel(const LayoutBox& box) {
    if(!box.node||box.node->tag!=L"input")return false;
    const auto type=ToLower(box.node->Attribute(L"type"));
    return type==L"button"||type==L"submit"||type==L"reset";
}

float InlineOuterBaseline(const LayoutBox& child,float outerHeight,
                          float reference,float viewport) {
    if(IsAtomicInlineLevel(child)){
        if(child.node->tag==L"select"&&child.style.Is(L"appearance",L"none")){
            const auto margin=EdgeValues(child.style,L"margin",reference,viewport);
            const auto padding=EdgeValues(child.style,L"padding",reference,viewport);
            const auto border=BorderValues(child.style);
            return margin.top+border.top+padding.top+TextBaselineOffset(child.style);
        }
        if(HasCenteredControlLabel(child)){
            const auto margin=EdgeValues(child.style,L"margin",reference,viewport);
            const auto metrics=InlineFontBoxMetrics(child.style);
            return margin.top+(outerHeight-margin.top-margin.bottom)/2+metrics.baseline-metrics.height/2;
        }
        const auto overflow=ToLower(Trim(child.style.Get(L"overflow",L"visible")));
        if(overflow==L"visible"){
            if(const auto* baselineStyle=FirstAvailableBaselineStyle(child)){
                const auto margin=EdgeValues(child.style,L"margin",reference,viewport);
                const auto padding=EdgeValues(child.style,L"padding",reference,viewport);
                const auto border=BorderValues(child.style);
                return margin.top+border.top+padding.top+
                    TextBaselineOffset(*baselineStyle);
            }
        }
        return outerHeight;
    }
    if(!child.node||child.node->type==NodeType::Text)
        return TextBaselineOffset(child.style);
    const auto margin=EdgeValues(child.style,L"margin",reference,viewport);
    const auto padding=EdgeValues(child.style,L"padding",reference,viewport);
    const auto border=BorderValues(child.style);
    return margin.top+border.top+padding.top+TextBaselineOffset(child.style);
}

float InlineBaselineRelativeTopFromBaseline(const ComputedStyle& parentStyle,
                                            const LayoutBox& child,
                                            float outerHeight,float boxBaseline,
                                            float viewport) {
    const auto raw=ToLower(Trim(child.style.Get(L"vertical-align",L"baseline")));
    const auto alignment=AlignmentKeyword(raw);
    if(alignment==L"middle")
        // CSS middle aligns the box midpoint with the parent's baseline plus
        // half the x-height, where "plus" points toward the text top in CSS's
        // baseline coordinate system. Layout Y grows downward, so both the
        // half x-height and half box height subtract from the line baseline.
        return -InlineFontBoxMetrics(parentStyle).xHeight/2.0f-outerHeight/2.0f;
    if(alignment==L"text-top")
        return -InlineFontBoxMetrics(parentStyle).baseline;
    if(alignment==L"text-bottom")
        return InlineFontBoxMetrics(parentStyle).height-
            InlineFontBoxMetrics(parentStyle).baseline-outerHeight;

    float shift=0;
    if(alignment==L"sub")shift=FontSize(parentStyle)*0.2f;
    else if(alignment==L"super")shift=-FontSize(parentStyle)*0.33f;
    else if(alignment!=L"baseline"&&alignment!=L"top"&&alignment!=L"bottom")
        // Positive <length> and <percentage> values raise an inline box.
        shift=-StyleSheet::Length(raw,LineHeight(parentStyle),viewport,0,
                                  FontSize(child.style));
    return -boxBaseline+shift;
}

float InlineBaselineRelativeTop(const ComputedStyle& parentStyle,
                                const LayoutBox& child,float outerHeight,
                                float reference,float viewport) {
    return InlineBaselineRelativeTopFromBaseline(
        parentStyle,child,outerHeight,
        InlineOuterBaseline(child,outerHeight,reference,viewport),viewport);
}

InlineLineMetrics InitialInlineLineMetrics(const LayoutBox& parent) {
    InlineLineMetrics result;
    // Legacy HTML block containers omit the font strut from their inline lines.
    // Actual text and replaced boxes still contribute through IncludeInlineLineBox.
    const auto* document=parent.node?parent.node->ownerDocument:nullptr;
    const bool inlineContainer=parent.style.Is(L"display",L"inline")&&
        !IsAtomicInlineLevel(parent)&&!IsBlockifiedItem(parent);
    const bool explicitBreak=std::any_of(parent.children.begin(),parent.children.end(),
        [](const auto& child){return child->visible&&child->node->tag==L"br";});
    if(document&&(document->QuirksMode()||document->LimitedQuirksMode())&&
       !inlineContainer&&!explicitBreak)return result;
    const auto& parentStyle=parent.style;
    result.baseline=TextBaselineOffset(parentStyle);
    result.descent=std::max(0.0f,LineHeight(parentStyle)-result.baseline);
    return result;
}

void IncludeInlineLineBox(InlineLineMetrics& line,const ComputedStyle& parentStyle,
                          const LayoutBox& child,float outerHeight,
                          float reference,float viewport) {
    const auto alignment=AlignmentKeyword(
        child.style.Get(L"vertical-align",L"baseline"));
    const bool atomic=IsAtomicInlineLevel(child);
    float contributionHeight=outerHeight;
    if(!atomic){
        const auto margin=EdgeValues(child.style,L"margin",reference,viewport);
        const auto padding=EdgeValues(child.style,L"padding",reference,viewport);
        const auto border=BorderValues(child.style);
        const float contentHeight=outerHeight-margin.top-margin.bottom-
            padding.top-padding.bottom-border.top-border.bottom;
        contributionHeight=LineHeight(child.style);
        if(contentHeight>std::max(contributionHeight,InlineContentBoxHeight(child.style))+0.01f)
            contributionHeight=contentHeight;
    }
    if(alignment==L"top"||alignment==L"bottom"){
        line.edgeAlignedHeight=std::max(line.edgeAlignedHeight,contributionHeight);
        return;
    }
    const float contributionBaseline=atomic?
        InlineOuterBaseline(child,outerHeight,reference,viewport):
        TextBaselineOffset(child.style);
    const float topFromBaseline=InlineBaselineRelativeTopFromBaseline(
        parentStyle,child,contributionHeight,contributionBaseline,viewport);
    line.baseline=std::max(line.baseline,-topFromBaseline);
    line.descent=std::max(line.descent,contributionHeight+topFromBaseline);
}

float InlineLineBoxOffset(const InlineLineMetrics& line,
                          const ComputedStyle& parentStyle,
                          const LayoutBox& child,float outerHeight,
                          float reference,float viewport) {
    // A block inside an inline splits the inline formatting context. Its
    // anonymous block fragment starts at the containing block's flow edge.
    if(child.style.Is(L"display",L"inline")&&HasInFlowBlockChildren(child))return 0;
    const auto alignment=AlignmentKeyword(
        child.style.Get(L"vertical-align",L"baseline"));
    const bool inlineElement=child.node&&child.node->type==NodeType::Element&&
        !IsAtomicInlineLevel(child)&&!UsesGenericMonospaceMetrics(child.style);
    if(alignment==L"top"||alignment==L"bottom"){
        if(!inlineElement)return alignment==L"top"?0:
            std::max(0.0f,line.Height()-outerHeight);
        const auto margin=EdgeValues(child.style,L"margin",reference,viewport);
        const auto padding=EdgeValues(child.style,L"padding",reference,viewport);
        const auto border=BorderValues(child.style);
        const float edge=InlineHalfLeading(child.style)-margin.top-padding.top-border.top;
        return edge+(alignment==L"bottom"?line.Height()-LineHeight(child.style):0);
    }
    const bool baselineAlignment=alignment==L"baseline"||alignment==L"initial"||
        alignment==L"unset"||alignment==L"revert"||alignment==L"revert-layer";
    if(baselineAlignment&&IsAtomicInlineLevel(child)&&
       !FirstAvailableBaselineStyle(child)&&!HasCenteredControlLabel(child)){
        // Replaced and otherwise empty atomic boxes synthesize a baseline at
        // their bottom margin edge. DirectWrite text-run rectangles already
        // start on the visual line track used by that synthesized baseline;
        // applying the ascent again pushes a short badge one full badge-height
        // below adjacent glyphs. Keep the synthetic box on the shared track,
        // while content-bearing inline-block/flex boxes continue to use their
        // real descendant baseline above.
        return 0;
    }
    float offset=line.baseline+InlineBaselineRelativeTop(
        parentStyle,child,IsAtomicInlineLevel(child)?outerHeight:LineHeight(child.style),
        reference,viewport);
    if(child.node&&child.node->type==NodeType::Element&&!IsAtomicInlineLevel(child)&&
       !UsesGenericMonospaceMetrics(child.style))
        offset+=InlineHalfLeading(child.style);
    return offset;
}

struct TextCaretLineMetrics {
    float topInset=0;
    float height=0;
};

TextCaretLineMetrics CaretLineMetrics(const ComputedStyle& style) {
    static thread_local FastMap<std::wstring,TextCaretLineMetrics> cache;
    std::wstring key=FontFamily(style);key+=L'\x1f';key+=NumberText(FontSize(style));
    key+=L'\x1f';key+=std::to_wstring(FontWeight(style));key+=L'\x1f';
    key+=style.Get(L"font-style");key+=L'\x1f';key+=style.Get(L"line-height");
    key+=L'\x1f';key+=NumberText(style.deviceScale);
    if(const auto found=cache.find(key);found!=cache.end())return found->second;
    if(cache.size()>=256)cache.clear();
    const auto metrics=NaturalFontBoxMetrics(style);
    TextCaretLineMetrics result{std::max(0.0f,TextBaselineOffset(style)-metrics.baseline),
                                std::max(1.0f,metrics.height)};
    cache.emplace(std::move(key),result);return result;
}

float FlexItemBaselineOffset(const LayoutBox& box,float referenceWidth,
                             float outerHeight) {
    const auto margin=EdgeValues(box.style,L"margin",referenceWidth,referenceWidth);
    const auto padding=EdgeValues(box.style,L"padding",referenceWidth,referenceWidth);
    const auto border=BorderValues(box.style);
    if(const auto* textStyle=FirstAvailableBaselineStyle(box))
        return margin.top+border.top+padding.top+TextBaselineOffset(*textStyle);
    // CSS synthesizes a baseline at the item's bottom margin edge when the
    // item has no usable first baseline.
    return std::max(margin.top,outerHeight-margin.bottom);
}

class UnbreakableFallbackGlyph final:public IDWriteInlineObject {
public:
    UnbreakableFallbackGlyph(IDWriteFontFace* face,UINT16 glyph,float size,
            DWRITE_BREAK_CONDITION after=DWRITE_BREAK_CONDITION_MAY_NOT_BREAK):face_(face),glyph_(glyph),size_(size),after_(after){
        DWRITE_FONT_METRICS font{};face_->GetMetrics(&font);
        DWRITE_GLYPH_METRICS metrics{};face_->GetDesignGlyphMetrics(&glyph_,1,&metrics,FALSE);
        const float scale=size_/std::max<UINT16>(1,font.designUnitsPerEm);
        advance_=metrics.advanceWidth*scale;
        metrics_={advance_,(font.ascent+font.descent)*scale,font.ascent*scale,FALSE};
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDWriteInlineObject)){
            *out=static_cast<IDWriteInlineObject*>(this);AddRef();return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&references_);}
    ULONG STDMETHODCALLTYPE Release()override{const auto value=InterlockedDecrement(&references_);if(!value)delete this;return value;}
    HRESULT STDMETHODCALLTYPE Draw(void* context,IDWriteTextRenderer* renderer,FLOAT x,FLOAT y,
            BOOL sideways,BOOL rtl,IUnknown* effect)override{
        if(!renderer)return E_POINTER;
        if(size_<=0)return S_OK;
        const DWRITE_GLYPH_RUN run{face_.Get(),size_,1,&glyph_,&advance_,nullptr,sideways,rtl?1u:0u};
        return renderer->DrawGlyphRun(context,rtl?x+advance_:x,y+metrics_.baseline,
            DWRITE_MEASURING_MODE_NATURAL,&run,nullptr,effect);
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS* out)override{
        if(!out)return E_POINTER;*out=metrics_;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS* out)override{
        if(!out)return E_POINTER;*out={};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION* before,DWRITE_BREAK_CONDITION* after)override{
        if(!before||!after)return E_POINTER;
        *before=DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;*after=after_;return S_OK;
    }
private:
    LONG references_=1;
    Microsoft::WRL::ComPtr<IDWriteFontFace> face_;
    UINT16 glyph_;float size_,advance_=0;
    DWRITE_BREAK_CONDITION after_;
    DWRITE_INLINE_OBJECT_METRICS metrics_{};
};

void ApplyFontFallback(IDWriteFactory* factory,IDWriteTextLayout* layout,
                       const std::wstring& text,const ComputedStyle& style,
                       bool controlMetrics=false) {
    if(!factory||!layout)return;
    // HarfBuzz uses the primary face's U+2010 glyph when U+2011 is absent.
    // Preserve the original character's no-break semantics and DOM offsets
    // while sharing the same glyph and advance in measurement and painting.
    if(text.find_first_of(L"\x2010\x2011")!=std::wstring::npos)if(const auto face=MatchedFontFace(style)){
        const UINT32 codes[]{0x2011,0x2010};UINT16 glyphs[2]{};
        if(SUCCEEDED(face->GetGlyphIndices(codes,2,glyphs))&&!glyphs[0]&&glyphs[1]){
            Microsoft::WRL::ComPtr<IDWriteInlineObject> glyph;
            glyph.Attach(new UnbreakableFallbackGlyph(face.Get(),glyphs[1],FontSize(style)));
            for(size_t index=0;index<text.size();++index)if(text[index]==L'\x2011')
                layout->SetInlineObject(glyph.Get(),{static_cast<UINT32>(index),1});
        }
        // The Korean UI fallback also supplies Common-script hyphens when
        // neither representation exists in the primary face. Validate font
        // coverage instead of borrowing an unrelated symbol font's advance.
        if(!glyphs[1]){
            const auto authored=ExplicitKoreanFontFamily(style.Get(L"font-family"));
            const auto family=authored.empty()?std::wstring(L"Noto Sans KR"):authored;
            Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
            Microsoft::WRL::ComPtr<IDWriteFontFamily> fontFamily;
            Microsoft::WRL::ComPtr<IDWriteFont> font;
            Microsoft::WRL::ComPtr<IDWriteFontFace> fallback;
            UINT32 index=0;BOOL exists=FALSE;UINT16 available[2]{};
            if(SUCCEEDED(factory->GetSystemFontCollection(&collection))&&
               SUCCEEDED(collection->FindFamilyName(family.c_str(),&index,&exists))&&exists&&
               SUCCEEDED(collection->GetFontFamily(index,&fontFamily))&&
               SUCCEEDED(fontFamily->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(FontWeight(style)),
                    DWRITE_FONT_STRETCH_NORMAL,FontStyle(style),&font))&&
               SUCCEEDED(font->CreateFontFace(&fallback))&&
               SUCCEEDED(fallback->GetGlyphIndices(codes,2,available)))
                for(size_t position=0;position<text.size();++position){
                    const int code=text[position]==L'\x2011'?0:text[position]==L'\x2010'?1:-1;
                    if(code>=0&&!glyphs[code]&&available[code])
                        layout->SetFontFamilyName(family.c_str(),{static_cast<UINT32>(position),1});
                }
        }
    }
    if(style.Is(L"hyphens",L"none")&&text.find(L'\x00ad')!=std::wstring::npos)
        if(const auto face=MatchedFontFace(style)){
            Microsoft::WRL::ComPtr<IDWriteInlineObject> invisible;
            invisible.Attach(new UnbreakableFallbackGlyph(face.Get(),0,0));
            for(size_t index=0;index<text.size();++index)if(text[index]==L'\x00ad')
                layout->SetInlineObject(invisible.Get(),{static_cast<UINT32>(index),1});
        }
    const auto primaryFamily=ToLower(FontFamily(style));
    const bool primaryProvidesKorean=primaryFamily.find(L"noto sans kr")!=std::wstring::npos||
        primaryFamily.find(L"malgun gothic")!=std::wstring::npos||
        primaryFamily.find(L"yu gothic")!=std::wstring::npos||FontFamily(style)==L"\ub9d1\uc740 \uace0\ub515";
    bool missingHangul=!primaryProvidesKorean;
    if(missingHangul&&ContainsHangul(text))if(const auto face=MatchedFontFace(style)){
        std::vector<UINT32> characters;
        for(const auto character:text)if(IsHangul(character))characters.push_back(character);
        std::vector<UINT16> indices(characters.size());
        if(SUCCEEDED(face->GetGlyphIndices(characters.data(),static_cast<UINT32>(characters.size()),indices.data())))
            missingHangul=std::find(indices.begin(),indices.end(),0)!=indices.end();
    }
    if(!controlMetrics&&missingHangul&&ContainsHangul(text)){
        // A CSS family such as Segoe UI or Arial does not contain Hangul.
        // Chromium resolves the Korean runs through the installed UI fallback
        // face while DirectWrite's implicit fallback can choose a wider legacy
        // face. Pin only those missing-glyph runs to the available Korean UI
        // font so layout measurement and painting use the browser fallback.
        const auto authoredFallback=ExplicitKoreanFontFamily(style.Get(L"font-family"));
        static const bool hasKoreanUiFont=[factory] {
            Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;
            UINT32 index=0;BOOL exists=FALSE;
            return SUCCEEDED(factory->GetSystemFontCollection(&collection))&&
                   SUCCEEDED(collection->FindFamilyName(L"Noto Sans KR",&index,&exists))&&exists;
        }();
        const auto fallbackFamily=!authoredFallback.empty()?authoredFallback:
            (hasKoreanUiFont?L"Noto Sans KR":L"");
        if(!fallbackFamily.empty())for(size_t start=0;start<text.size();){
            if(!IsHangul(text[start])){++start;continue;}
            size_t end=start+1;
            while(end<text.size()&&IsHangul(text[end]))++end;
            layout->SetFontFamilyName(fallbackFamily.c_str(),
                DWRITE_TEXT_RANGE{static_cast<UINT32>(start),static_cast<UINT32>(end-start)});
            start=end;
        }
    }
    if(controlMetrics){
        // Browser form controls resolve arrows, geometric shapes and technical
        // symbols through the Windows symbol face instead of borrowing the
        // surrounding UI face's wider notdef/fallback advance.
        for(size_t start=0;start<text.size();){
            const auto code=static_cast<unsigned int>(text[start]);
            const bool symbol=(code>=0x2190&&code<=0x27ff)&&code!=0x25cf;
            if(!symbol){++start;continue;}
            size_t end=start+1;
            while(end<text.size()){
                const auto next=static_cast<unsigned int>(text[end]);
                if(next<0x2190||next>0x27ff)break;
                ++end;
            }
            layout->SetFontFamilyName(L"Segoe UI Symbol",
                DWRITE_TEXT_RANGE{static_cast<UINT32>(start),static_cast<UINT32>(end-start)});
            start=end;
        }
    }
}

void ApplyPairKerning(IDWriteTextLayout* layout,const std::wstring& text,
                      const ComputedStyle& style) {
    if(!layout||text.empty())return;
    Microsoft::WRL::ComPtr<IDWriteTextLayout1> extended;
    if(SUCCEEDED(layout->QueryInterface(IID_PPV_ARGS(&extended))))
        extended->SetPairKerning(FALSE,
            DWRITE_TEXT_RANGE{0,static_cast<UINT32>(text.size())});
    // Prefer GPOS kerning. PairKerning can add legacy pairs even when GPOS
    // already provides positioning, so enable it only on legacy-only faces.
    static thread_local Microsoft::WRL::ComPtr<IDWriteTypography> typography[2];
    const auto enabled=style.Is(L"font-kerning",L"none")?0:1;
    if(!typography[enabled])if(auto* factory=SharedWriteFactory()){
        if(SUCCEEDED(factory->CreateTypography(&typography[enabled])))
            typography[enabled]->AddFontFeature({DWRITE_FONT_FEATURE_TAG_KERNING,static_cast<UINT32>(enabled)});
    }
    if(typography[enabled])layout->SetTypography(typography[enabled].Get(),
        DWRITE_TEXT_RANGE{0,static_cast<UINT32>(text.size())});
    if(enabled&&extended){
        LegacyKerningRanges collector;
        if(SUCCEEDED(layout->Draw(nullptr,&collector,0,0)))
            for(const auto range:collector.ranges)extended->SetPairKerning(TRUE,range);
    }
}

void ApplyWordSpacing(IDWriteTextLayout1* layout,const std::wstring& text,
                      const ComputedStyle& style,UINT32 offset=0) {
    if(!layout)return;
    const float spacing=StyleSheet::Length(style.Get(L"word-spacing"),FontSize(style),
        FontSize(style),0,FontSize(style));
    if(std::abs(spacing)<.00001f)return;
    for(size_t index=0;index<text.size();++index){
        unsigned code=text[index];UINT32 length=1;
        if(code>=0xd800&&code<=0xdbff&&index+1<text.size()&&
           text[index+1]>=0xdc00&&text[index+1]<=0xdfff){
            code=0x10000+((code-0xd800)<<10)+(text[index+1]-0xdc00);length=2;
        }
        // Fixed-width Unicode spaces and zero-width breaks are not word
        // separators. Apply after white-space normalization and retain any
        // letter spacing or font advance adjustment on the same range.
        if(code==0x20||code==0xa0||code==0x1361||code==0x10100||
           code==0x10101||code==0x1039f||code==0x1091f){
            float leading=0,trailing=0,minimum=0;
            if(SUCCEEDED(layout->GetCharacterSpacing(offset+static_cast<UINT32>(index),
                    &leading,&trailing,&minimum,nullptr)))
                layout->SetCharacterSpacing(leading+spacing/2,trailing+spacing/2,minimum,
                    {offset+static_cast<UINT32>(index),length});
        }
        index+=length-1;
    }
}

void ApplyCharacterSpacing(IDWriteTextLayout* layout,const std::wstring& text,
                           const ComputedStyle& style,bool controlMetrics=false) {
    if(!layout||text.empty())return;
    ApplyPairKerning(layout,text,style);
    Microsoft::WRL::ComPtr<IDWriteTextLayout1> extended;
    if(FAILED(layout->QueryInterface(IID_PPV_ARGS(&extended))))return;

    float authorSpacing=0;
    const auto rawSpacing=style.Get(L"letter-spacing");
    if(!rawSpacing.empty()&&rawSpacing!=L"normal")
        authorSpacing=StyleSheet::Length(rawSpacing,FontSize(style),FontSize(style),0,FontSize(style));
    BitmapAdvanceRanges bitmapAdvances(std::max(0.01f,style.deviceScale));
    layout->Draw(nullptr,&bitmapAdvances,0,0);
    if(std::abs(authorSpacing)>0.001f)
        extended->SetCharacterSpacing(0,authorSpacing,0,
            DWRITE_TEXT_RANGE{0,static_cast<UINT32>(text.size())});
    for(const auto& adjustment:bitmapAdvances.adjustments)
        extended->SetCharacterSpacing(0,authorSpacing+adjustment.trailing,0,adjustment.range);

    // Chromium uses GDI-compatible metrics for native form-control text on
    // Windows. Keep those small fallback/space adjustments out of ordinary
    // document text so large headings retain their authored letter spacing.
    const float size=FontSize(style);
    if(controlMetrics&&UsesAutomaticHangulFallback(style)){
        const float hangulSpacing=authorSpacing-size*0.08f;
        for(size_t start=0;start<text.size();){
            if(!IsHangul(text[start])){++start;continue;}
            size_t end=start+1;
            while(end<text.size()&&IsHangul(text[end]))++end;
            extended->SetCharacterSpacing(0,hangulSpacing,0,
                DWRITE_TEXT_RANGE{static_cast<UINT32>(start),static_cast<UINT32>(end-start)});
            start=end;
        }
    }
    if(controlMetrics&&size>11.0f)for(size_t index=0;index<text.size();++index)
            if(text[index]==L' ')
                extended->SetCharacterSpacing(0,authorSpacing-(size-11.0f)*0.216f,0,
                    DWRITE_TEXT_RANGE{static_cast<UINT32>(index),1});

    for(size_t index=0;index<text.size();++index){
        const float adjustment=BrowserSymbolAdvanceAdjustment(text[index],size);
        if(std::abs(adjustment)>0.001f)
            extended->SetCharacterSpacing(0,authorSpacing+adjustment,0,
                DWRITE_TEXT_RANGE{static_cast<UINT32>(index),1});
    }

    for(size_t index=0;index<text.size();++index)
        if(text[index]==0x21f2)
            extended->SetCharacterSpacing(0,authorSpacing-1.0f,0,
                DWRITE_TEXT_RANGE{static_cast<UINT32>(index),1});
    ApplyWordSpacing(extended.Get(),text,style);
}

float SpaceAdvance(IDWriteFactory* factory,IDWriteTextFormat* format,
                   const ComputedStyle& style,bool controlMetrics) {
    if(!factory||!format)return FontSize(style)*0.5f;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    constexpr wchar_t space[]=L" ";HRESULT created=E_FAIL;
    if(controlMetrics){
        const DWRITE_MATRIX identity{1,0,0,1,0,0};
        created=factory->CreateGdiCompatibleTextLayout(space,1,format,100.0f,100.0f,
            1.0f,&identity,FALSE,&layout);
    }else created=factory->CreateTextLayout(space,1,format,100.0f,100.0f,&layout);
    if(FAILED(created))return FontSize(style)*0.5f;
    ApplyFontFallback(factory,layout.Get(),space,style,controlMetrics);
    ApplyCharacterSpacing(layout.Get(),space,style,controlMetrics);
    DWRITE_TEXT_METRICS metrics{};
    return SUCCEEDED(layout->GetMetrics(&metrics))?
        std::max(0.01f,metrics.widthIncludingTrailingWhitespace):FontSize(style)*0.5f;
}

void ApplyTabSize(IDWriteFactory* factory,IDWriteTextLayout* layout,
                  IDWriteTextFormat* format,const std::wstring& text,
                  const ComputedStyle& style,bool controlMetrics=false) {
    if(!layout||text.find(L'\t')==std::wstring::npos)return;
    const auto raw=ToLower(Trim(style.Get(L"tab-size",L"8")));
    float stop=0;bool number=false;
    size_t used=0;float value=0;
    if(TryParseFloat(raw,value,&used)&&used==raw.size()){
        stop=SpaceAdvance(factory,format,style,controlMetrics)*value;
        number=true;
    }
    if(!number)stop=StyleSheet::Length(raw,FontSize(style),FontSize(style),
        SpaceAdvance(factory,format,style,controlMetrics)*8.0f,FontSize(style));
    layout->SetIncrementalTabStop(std::max(0.01f,stop));
}

void ApplyTextDecorations(IDWriteTextLayout* layout,const std::wstring& text,
                          const ComputedStyle& style) {
    if(!layout||text.empty())return;
    const auto decoration=ToLower(style.Get(L"text-decoration-line",
        style.Get(L"text-decoration",L"none")));
    const DWRITE_TEXT_RANGE range{0,static_cast<UINT32>(text.size())};
    if(decoration.find(L"underline")!=std::wstring::npos)layout->SetUnderline(TRUE,range);
    if(decoration.find(L"line-through")!=std::wstring::npos)layout->SetStrikethrough(TRUE,range);
}

float TextWidth(const std::wstring& source,const ComputedStyle& style,bool preserveLeading=false,bool preserveTrailing=false,bool gdiCompatible=false){
    const auto text=NormalizeText(source,style.Get(L"white-space"),preserveLeading,preserveTrailing);
    auto& cache=ThreadTextWidthCache();
    std::wstring cacheKey=text;cacheKey+=L'\x1f';cacheKey+=FontFamily(style);cacheKey+=L'\x1f';
    cacheKey+=NumberText(FontSize(style));cacheKey+=L'\x1f';cacheKey+=std::to_wstring(FontWeight(style));
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"font-style");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"letter-spacing");cacheKey+=L'\x1f';cacheKey+=style.Get(L"word-spacing");
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"tab-size");
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"font-family");
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"font-kerning",L"auto");
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"hyphens",L"manual");
    cacheKey+=L'\x1f';cacheKey+=NumberText(style.deviceScale);
    cacheKey+=gdiCompatible?L"\x1fG":L"\x1fD";
    float cachedWidth=0;
    if(cache.Find(cacheKey,cachedWidth))return cachedWidth;
    auto remember=[&](float value){cache.Remember(std::move(cacheKey),value);return value;};
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        HRESULT created=E_FAIL;
        if(format){
            if(gdiCompatible){
                const DWRITE_MATRIX identity{1,0,0,1,0,0};
                created=factory->CreateGdiCompatibleTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
                    format.Get(),100000.0f,100000.0f,1.0f,&identity,FALSE,&layout);
            }else created=factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
                format.Get(),100000.0f,100000.0f,&layout);
        }
        if(SUCCEEDED(created)){
            ApplyFontFallback(factory,layout.Get(),text,style,gdiCompatible);
            ApplyCharacterSpacing(layout.Get(),text,style,gdiCompatible);
            ApplyTabSize(factory,layout.Get(),format.Get(),text,style,gdiCompatible);
            DWRITE_TEXT_METRICS metrics{};
            if(SUCCEEDED(layout->GetMetrics(&metrics))){
                float width=metrics.widthIncludingTrailingWhitespace;
                // Chromium stores inline geometry in 1/64 layout units at the
                // target raster scale. Round upward to that boundary so
                // DirectWrite cannot wrap the final glyph because of a smaller
                // floating-point width at paint time.
                const float units=64.0f*std::max(0.01f,style.deviceScale);
                return remember(std::max(1.0f,std::ceil(width*units)/units));
            }
        }
    }
    const float fontSize=FontSize(style);float em=0;
    for(wchar_t c:text){
        if(std::iswspace(c))em+=0.34f;
        else if((c>=0x2e80&&c<=0xd7af)||(c>=0xf900&&c<=0xfaff))em+=1.0f;
        else if(std::iswupper(c))em+=0.66f;
        else if(std::iswdigit(c))em+=0.58f;
        else if(std::iswpunct(c))em+=0.42f;
        else em+=0.57f;
    }
    return remember(std::max(1.0f,em*fontSize+1.0f));
}

void ApplyDiscretionaryHyphens(IDWriteTextLayout* layout,const std::wstring& text,const ComputedStyle& style){
    if(!layout||style.Is(L"hyphens",L"none")||text.find(L'\x00ad')==std::wstring::npos)return;
    const auto face=MatchedFontFace(style);if(!face)return;
    const UINT32 codes[]{0x2010,0x2d};UINT16 glyphs[2]{};
    if(FAILED(face->GetGlyphIndices(codes,2,glyphs))||(!glyphs[0]&&!glyphs[1]))return;
    Microsoft::WRL::ComPtr<IDWriteInlineObject> visible,invisible;
    visible.Attach(new UnbreakableFallbackGlyph(face.Get(),glyphs[0]?glyphs[0]:glyphs[1],
        FontSize(style),DWRITE_BREAK_CONDITION_CAN_BREAK));
    invisible.Attach(new UnbreakableFallbackGlyph(face.Get(),0,0));
    // DirectWrite reports discretionary breaks but omits the hyphen glyph.
    // Reserve and paint that glyph only at a taken break. If reserving it
    // moves the entire word to a line where it fits, disable that opportunity
    // and hide the glyph. Each opportunity changes state at most twice.
    std::vector<unsigned char> state(text.size(),0);
    const auto opportunities=std::count(text.begin(),text.end(),L'\x00ad');
    for(size_t pass=0;pass<=2*static_cast<size_t>(opportunities);++pass){
        UINT32 count=0;layout->GetLineMetrics(nullptr,0,&count);
        std::vector<DWRITE_LINE_METRICS> lines(count);
        if(!count||FAILED(layout->GetLineMetrics(lines.data(),count,&count)))return;
        std::vector<bool> taken(text.size(),false);size_t end=0;
        for(size_t line=0;line<lines.size();++line){
            end+=lines[line].length;
            if(line+1<lines.size()&&!lines[line].newlineLength&&end>0&&end<text.size()&&text[end-1]==L'\x00ad')
                taken[end-1]=true;
        }
        bool changed=false;
        for(size_t position=0;position<text.size();++position)if(text[position]==L'\x00ad'){
            if(!state[position]&&taken[position]){
                layout->SetInlineObject(visible.Get(),{static_cast<UINT32>(position),1});
                state[position]=1;changed=true;
            }else if(state[position]==1&&!taken[position]){
                layout->SetInlineObject(invisible.Get(),{static_cast<UINT32>(position),1});
                state[position]=2;changed=true;
            }
        }
        if(!changed)return;
    }
}

float WrappableTextMinimum(const std::wstring& text,const ComputedStyle& style);

float TextFlowFragmentAdvance(const LayoutBox& box){
    struct Advances {std::vector<float> positions;};
    static thread_local FastMap<std::wstring,Advances> cache;
    static thread_local size_t cachedCharacters=0;
    const auto& source=box.generatedFrom->text;
    std::wstring key=source+L'\x1f'+FontFamily(box.style)+L'\x1f'+box.style.Get(L"font-family")+
        L'\x1f'+NumberText(FontSize(box.style))+L'\x1f'+std::to_wstring(FontWeight(box.style));
    for(const auto* property:{L"font-style",L"font-kerning",L"letter-spacing",L"word-spacing",L"tab-size"})
        key+=L'\x1f'+box.style.Get(property);
    key+=L'\x1f'+NumberText(box.style.deviceScale);
    auto found=cache.find(key);
    if(found==cache.end()){
        if(cache.size()>=64||cachedCharacters+source.size()>262144){cache.clear();cachedCharacters=0;}
        const auto text=NormalizeText(source,false,true,true);
        auto* factory=SharedWriteFactory();if(!factory)return TextWidth(box.node->text,box.style,true,box.preserveTrailingWhitespace);
        auto format=TextFormat(factory,box.style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if(!format||FAILED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
            format.Get(),100000,100000,&layout)))return TextWidth(box.node->text,box.style,true,box.preserveTrailingWhitespace);
        ApplyFontFallback(factory,layout.Get(),text,box.style);
        ApplyCharacterSpacing(layout.Get(),text,box.style);
        ApplyTabSize(factory,layout.Get(),format.Get(),text,box.style);
        UINT32 count=0;layout->GetClusterMetrics(nullptr,0,&count);
        std::vector<DWRITE_CLUSTER_METRICS> clusters(count);
        if(FAILED(layout->GetClusterMetrics(clusters.data(),count,&count)))return TextWidth(box.node->text,box.style,true,box.preserveTrailingWhitespace);
        std::vector<float> normalized(text.size()+1);size_t position=0;float advance=0;
        const float units=64*std::max(.01f,box.style.deviceScale);
        for(const auto& cluster:clusters){
            advance+=cluster.width;
            for(size_t length=0;length<cluster.length&&position<text.size();++length)
                normalized[++position]=std::ceil(advance*units)/units;
        }
        Advances measured;measured.positions.resize(source.size()+1);position=0;bool space=false;
        for(size_t index=0;index<source.size();++index){
            if(!IsCollapsibleTextSpace(source[index])){++position;space=false;}
            else if(!space){++position;space=true;}
            measured.positions[index+1]=normalized[std::min(position,text.size())];
        }
        cachedCharacters+=source.size();found=cache.emplace(std::move(key),std::move(measured)).first;
    }
    const auto& advances=found->second.positions;
    const size_t start=std::min(box.textSourceOffset,source.size());
    const size_t end=std::min(start+box.node->text.size(),source.size());
    return std::max(0.0f,advances[end]-advances[start]);
}

float TextHeight(const std::wstring& source,const ComputedStyle& style,float availableWidth,
                 bool preserveLeading=false,bool preserveTrailing=false){
    const auto whiteSpace=style.Get(L"white-space");
    // Collapsed no-wrap content always occupies one line. Preformatted modes
    // still need measurement because authored segment breaks create lines.
    if(whiteSpace==L"nowrap")return LineHeight(style);
    const auto text=NormalizeText(source,whiteSpace,preserveLeading,preserveTrailing);
    // Preserved lines cannot wrap in pre. Their height depends only on authored
    // segment breaks, including the final empty line, never on the grid width.
    // In particular, provisional intrinsic widths must not trigger shaping.
    // Leave other Unicode line separators to DirectWrite's line breaker.
    if(whiteSpace==L"pre"&&text.find_first_of(L"\v\f\x85\u2028\u2029")==std::wstring::npos)
        return LineHeight(style)*(1+std::count(text.begin(),text.end(),L'\n'));
    static thread_local FastMap<std::wstring,float> cache;
    std::wstring cacheKey=text;cacheKey+=L'\x1f';cacheKey+=FontFamily(style);cacheKey+=L'\x1f';
    cacheKey+=NumberText(FontSize(style));cacheKey+=L'\x1f';cacheKey+=std::to_wstring(FontWeight(style));
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"font-style");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"letter-spacing");cacheKey+=L'\x1f';cacheKey+=style.Get(L"word-spacing");
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"tab-size");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"line-height");cacheKey+=L'\x1f';cacheKey+=whiteSpace;cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"word-break");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"overflow-wrap");cacheKey+=L'\x1f';
    cacheKey+=style.Get(L"hyphens",L"manual");cacheKey+=L'\x1f';
    cacheKey+=NumberText(std::round(availableWidth*64.0f)/64.0f);
    cacheKey+=L'\x1f';cacheKey+=style.Get(L"font-kerning",L"auto");
    cacheKey+=L'\x1f';cacheKey+=NumberText(style.deviceScale);
    if(const auto found=cache.find(cacheKey);found!=cache.end())return found->second;
    if(cache.size()>8192)cache.clear();
    auto remember=[&](float value){cache[std::move(cacheKey)]=value;return value;};
    if((!PreservesLineBreaks(whiteSpace)||text.find(L'\n')==std::wstring::npos)&&
       TextWidth(text,style,preserveLeading,preserveTrailing)<=availableWidth+0.5f)
        return remember(LineHeight(style));
    if(TextWrappingMode(style)==DWRITE_WORD_WRAPPING_WHOLE_WORD&&
       (!PreservesLineBreaks(whiteSpace)||text.find(L'\n')==std::wstring::npos)&&
       WrappableTextMinimum(text,style)>=TextWidth(text,style,preserveLeading,preserveTrailing))
        return remember(LineHeight(style));
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if(format&&SUCCEEDED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
            format.Get(),std::max(1.0f,availableWidth),100000.0f,&layout))){
            layout->SetWordWrapping(TextWrappingMode(style));
            ApplyFontFallback(factory,layout.Get(),text,style);
            ApplyCharacterSpacing(layout.Get(),text,style);
            ApplyTabSize(factory,layout.Get(),format.Get(),text,style);
            ApplyDiscretionaryHyphens(layout.Get(),text,style);
            UINT32 lineCount=0;layout->GetLineMetrics(nullptr,0,&lineCount);
            if(lineCount)return remember(LineHeight(style)*lineCount);
        }
    }
    return remember(LineHeight(style)*std::max(1.0f,std::ceil(TextWidth(text,style)/std::max(1.0f,availableWidth))));
}

float ControlLineHeight(const std::wstring& source,const ComputedStyle& style) {
    const auto raw=style.Get(L"line-height");
    if(!raw.empty()&&raw!=L"normal")return LineHeight(style);
    if(auto* factory=SharedWriteFactory()){
        const auto text=source.empty()?L" ":source;
        auto format=TextFormat(factory,style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        const DWRITE_MATRIX identity{1,0,0,1,0,0};
        if(format&&SUCCEEDED(factory->CreateGdiCompatibleTextLayout(text.c_str(),
            static_cast<UINT32>(text.size()),format.Get(),100000.0f,100000.0f,
            1.0f,&identity,FALSE,&layout))){
            ApplyFontFallback(factory,layout.Get(),text,style,true);
            DWRITE_LINE_METRICS metrics{};UINT32 count=0;
            if(SUCCEEDED(layout->GetLineMetrics(&metrics,1,&count))&&count)
                return std::max(LineHeight(style),std::ceil(metrics.height));
        }
    }
    return LineHeight(style);
}

std::wstring SelectedOptionText(const std::shared_ptr<Node>& select) {
    if(!select||select->tag!=L"select")return L"";
    const auto value=select->Attribute(L"value");
    std::vector<std::shared_ptr<Node>> options;
    std::function<void(const std::shared_ptr<Node>&)> collect=[&](const std::shared_ptr<Node>& parent){
        for(const auto& child:parent->children){
            if(child->tag==L"option")options.push_back(child);
            else if(child->tag==L"optgroup")collect(child);
        }
    };
    collect(select);
    std::shared_ptr<Node> fallback;
    for(const auto& option:options){
        if(!fallback||option->attributes.count(L"selected"))fallback=option;
        const auto optionValue=option->attributes.count(L"value")?option->Attribute(L"value"):option->InnerText();
        if(select->attributes.count(L"value")&&optionValue==value)return option->InnerText();
    }
    return fallback?fallback->InnerText():L"";
}

std::wstring BoxText(const LayoutBox& box,bool* placeholder=nullptr) {
    if(placeholder)*placeholder=false;
    if(box.node->type==NodeType::Text)
        return NormalizeText(box.node->text,box.style.Get(L"white-space"),
                             box.preserveLeadingWhitespace&&!box.trimLeadingLineWhitespace,box.preserveTrailingWhitespace);
    if((box.node->tag==L"input"&&box.node->Attribute(L"type")!=L"checkbox"&&
        box.node->Attribute(L"type")!=L"radio")||box.node->tag==L"textarea"){
        auto text=box.node->Attribute(L"value");
        if(box.node->tag==L"textarea"&&!box.node->attributes.count(L"value"))text=box.node->InnerText();
        if(box.node->tag==L"input"&&ToLower(box.node->Attribute(L"type"))==L"password"&&
           !text.empty())text.assign(text.size(),L'\x2022');
        if(text.empty()){
            text=box.node->Attribute(L"placeholder");
            if(placeholder)*placeholder=!text.empty();
        }
        return text;
    }
    return box.node->tag==L"select"?SelectedOptionText(box.node):std::wstring{};
}

size_t TextSourceToLayoutOffset(const LayoutBox& box,size_t rawOffset){
    if(box.node->type!=NodeType::Text||PreservesSpaces(box.style.Get(L"white-space")))
        return std::min(rawOffset,BoxText(box).size());
    const auto prefix=box.node->text.substr(0,std::min(rawOffset,box.node->text.size()));
    return std::min(BoxText(box).size(),NormalizeText(prefix,box.style.Get(L"white-space"),
        box.preserveLeadingWhitespace&&!box.trimLeadingLineWhitespace,true).size());
}

size_t TextLayoutToSourceOffset(const LayoutBox& box,size_t layoutOffset){
    if(box.node->type!=NodeType::Text||PreservesSpaces(box.style.Get(L"white-space")))return layoutOffset;
    size_t first=0,last=box.node->text.size();
    while(first<last){
        const size_t middle=first+(last-first+1)/2;
        if(TextSourceToLayoutOffset(box,middle)<=layoutOffset)first=middle;
        else last=middle-1;
    }
    return first;
}

D2D1_POINT_2F TextOrigin(const LayoutBox& box) {
    const auto appearance=box.style.Get(L"appearance",box.style.Get(L"-webkit-appearance",L"auto"));
    const float selectInset=box.node->tag==L"select"&&appearance!=L"none"?4.0f:0.0f;
    float top=box.content.y;
    if(box.node->tag==L"textarea")top-=box.node->scrollTop;
    else if(box.node->tag==L"input"){
        // The single-line editor occupies an integer device-pixel line box.
        // Center the spare height before glyph baseline snapping so an odd
        // number of spare pixels cannot shift the entire line down one pixel.
        const float scale=std::max(0.01f,box.style.deviceScale);
        top+=std::floor((box.content.height-LineHeight(box.style))*scale/2+0.0001f)/scale;
    }
    return D2D1::Point2F(box.content.x+selectInset-
        (box.node->tag==L"textarea"?box.node->scrollLeft:0.0f),top);
}

bool ParseListInteger(const std::wstring& source,int& value) {
    const auto text=Trim(source);if(text.empty())return false;
    size_t index=0;bool negative=false;
    if(text[index]==L'+'||text[index]==L'-'){negative=text[index]==L'-';++index;}
    if(index==text.size())return false;
    long long parsed=0;
    for(;index<text.size();++index){
        if(text[index]<L'0'||text[index]>L'9')return false;
        parsed=std::min(1000000LL,parsed*10+static_cast<long long>(text[index]-L'0'));
    }
    value=static_cast<int>(negative?-parsed:parsed);return true;
}

std::wstring AlphabeticListMarker(int value,bool uppercase) {
    if(value<=0)return std::to_wstring(value);
    std::wstring marker;
    while(value>0){
        --value;const wchar_t base=uppercase?L'A':L'a';
        marker.insert(marker.begin(),static_cast<wchar_t>(base+value%26));value/=26;
    }
    return marker;
}

std::wstring RomanListMarker(int value,bool uppercase) {
    if(value<=0||value>3999)return std::to_wstring(value);
    struct Entry { int value; const wchar_t* digits; };
    static constexpr Entry entries[]={
        {1000,L"m"},{900,L"cm"},{500,L"d"},{400,L"cd"},{100,L"c"},
        {90,L"xc"},{50,L"l"},{40,L"xl"},{10,L"x"},{9,L"ix"},
        {5,L"v"},{4,L"iv"},{1,L"i"}};
    std::wstring marker;
    for(const auto& entry:entries)while(value>=entry.value){
        marker+=entry.digits;value-=entry.value;
    }
    if(uppercase)std::transform(marker.begin(),marker.end(),marker.begin(),
        [](wchar_t character){return static_cast<wchar_t>(std::towupper(character));});
    return marker;
}

int OrderedListMarkerValue(const LayoutBox& box) {
    const auto* list=box.parent;if(!list||!list->node)return 1;
    const bool reversed=list->node->tag==L"ol"&&
        list->node->attributes.count(L"reversed")!=0;
    int current=1;
    if(list->node->tag==L"ol"&&!ParseListInteger(list->node->Attribute(L"start"),current)&&reversed)
        current=static_cast<int>(std::count_if(list->children.begin(),list->children.end(),
            [](const std::unique_ptr<LayoutBox>& child){
                return child&&child->node&&child->node->tag==L"li";
            }));
    for(const auto& child:list->children){
        if(!child->node||child->node->tag!=L"li")continue;
        int explicitValue=0;
        if(ParseListInteger(child->node->Attribute(L"value"),explicitValue))current=explicitValue;
        if(child.get()==&box)return current;
        current+=reversed?-1:1;
    }
    return current;
}

std::wstring ListMarkerText(const LayoutBox& box) {
    if(!box.node||box.node->tag!=L"li"||
       !box.style.Is(L"display",L"list-item"))return L"";
    const auto type=ToLower(Trim(box.style.Get(L"list-style-type",L"disc")));
    if(type.empty()||type==L"none")return L"";
    if(type==L"disc")return L"\x2022";
    if(type==L"circle")return L"\x25e6";
    if(type==L"square")return L"\x25aa";
    const int value=OrderedListMarkerValue(box);
    if(type==L"lower-alpha"||type==L"lower-latin")return AlphabeticListMarker(value,false)+L".";
    if(type==L"upper-alpha"||type==L"upper-latin")return AlphabeticListMarker(value,true)+L".";
    if(type==L"lower-roman")return RomanListMarker(value,false)+L".";
    if(type==L"upper-roman")return RomanListMarker(value,true)+L".";
    if(type==L"decimal-leading-zero"&&value>=0&&value<10)
        return L"0"+std::to_wstring(value)+L".";
    return std::to_wstring(value)+L".";
}

std::wstring PhysicalTextAlignment(const ComputedStyle& style){
    const auto align=style.Get(L"text-align",L"start");const bool rtl=style.Is(L"direction",L"rtl");
    if(align==L"start")return rtl?L"right":L"left";
    if(align==L"end")return rtl?L"left":L"right";
    return align;
}

void ApplySvgReplacementFallback(IDWriteTextLayout* layout,const std::wstring& text,const ComputedStyle& style,UINT32 offset=0) {
    if (!layout || text.find(L'\xfffd') == std::wstring::npos || !SystemFontFamilyExists(L"Tahoma")) return;
    const auto face = MatchedFontFace(style);UINT32 code = 0xfffd;UINT16 glyph = 0;
    if (!face || FAILED(face->GetGlyphIndices(&code,1,&glyph)) || glyph) return;
    // The fixed Windows browser's U+FFFD fallback matches installed Tahoma.
    // DirectWrite's implicit Segoe UI fallback has different ink/advances.
    // Keep the authored face whenever it already supplies this character.
    for (size_t i=0;i<text.size();++i) if (text[i]==L'\xfffd')
        layout->SetFontFamilyName(L"Tahoma",{offset+static_cast<UINT32>(i),1});
}

void EnsureTextLayout(LayoutBox& box,IDWriteFactory* factory,const std::wstring& text) {
    if(text.empty()||!factory){box.textLayout.Reset();box.textLayoutKey.clear();return;}
    const bool inlineTextRun=box.inlineTextPositioned||
        (box.node->type==NodeType::Text&&box.parent&&box.parent->style.Is(L"display",L"inline"));
    const auto whiteSpace=box.style.Get(L"white-space");
    const float nativeSelectInset=box.node->tag==L"select"?4.0f:0.0f;
    std::wstring layoutKey=text;layoutKey+=L'\x1f';layoutKey+=FontFamily(box.style);layoutKey+=L'\x1f';
    layoutKey+=NumberText(FontSize(box.style));layoutKey+=L'\x1f';layoutKey+=std::to_wstring(FontWeight(box.style));
    layoutKey+=L'\x1f';layoutKey+=box.style.Get(L"font-style");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"letter-spacing");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"word-spacing");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"tab-size");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"text-decoration-line",box.style.Get(L"text-decoration"));layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"text-align");layoutKey+=L'\x1f';layoutKey+=box.style.Get(L"direction");
    layoutKey+=L'\x1f';layoutKey+=whiteSpace;layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"word-break");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"overflow-wrap");layoutKey+=L'\x1f';
    layoutKey+=box.style.Get(L"hyphens",L"manual");layoutKey+=L'\x1f';
    layoutKey+=inlineTextRun?L"inline-run":L"paragraph";
    const float textLayoutHeight=box.node->tag==L"textarea"?
        std::max(box.content.height,box.scrollHeight):box.content.height;
    layoutKey+=NumberText(box.content.width);layoutKey+=L',';layoutKey+=NumberText(textLayoutHeight);
    layoutKey+=L'\x1f';layoutKey+=box.style.Get(L"font-kerning",L"auto");
    layoutKey+=L'\x1f';layoutKey+=NumberText(box.style.deviceScale);
    if(box.textLayout&&box.textLayoutKey==layoutKey)return;
    box.textLayout.Reset();auto format=TextFormat(factory,box.style);if(!format)return;
    const auto created=factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format.Get(),
        std::max(1.0f,box.content.width-nativeSelectInset),std::max(1.0f,textLayoutHeight),&box.textLayout);
    if(FAILED(created)){box.textLayout.Reset();return;}
    // Inline runs have already been positioned by the containing line. A
    // second paragraph alignment would shift text by its trailing spaces.
    const auto alignment=PhysicalTextAlignment(box.style);
    const bool rtl=box.style.Is(L"direction",L"rtl");
    if(!inlineTextRun&&(alignment==L"center"||alignment==L"-webkit-center"))box.textLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    else{
        const bool physicalRight=!inlineTextRun&&(alignment==L"right"||alignment==L"-webkit-right");
        box.textLayout->SetTextAlignment(physicalRight!=rtl?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    box.textLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
        LineHeight(box.style),TextBaselineOffset(box.style));
    box.textLayout->SetParagraphAlignment(box.node->tag==L"textarea"||box.node->tag==L"input"?
        DWRITE_PARAGRAPH_ALIGNMENT_NEAR:DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // The containing line has already measured a one-line DOM fragment.
    // Device-rounded bitmap advances can exceed its stored width by a float
    // ULP; wrapping it again creates a phantom line and moves caret/selection.
    const bool singleLineFragment=box.node->type==NodeType::Text&&
        box.content.height<=LineHeight(box.style)+0.01f&&text.find(L'\n')==std::wstring::npos;
    box.textLayout->SetWordWrapping(box.node->tag==L"input"||box.node->tag==L"select"||singleLineFragment?
        DWRITE_WORD_WRAPPING_NO_WRAP:TextWrappingMode(box.style));
    const auto textParent = box.node->parent.lock();
    // SVG uses vector advances, including fallback glyphs, rather than the
    // device-rounded fallback advances used by HTML inline text.
    const bool svgText = textParent && textParent->namespaceUri == L"http://www.w3.org/2000/svg" && textParent->tag == L"text";
    if (!svgText) {
        ApplyFontFallback(factory,box.textLayout.Get(),text,box.style);
        ApplyCharacterSpacing(box.textLayout.Get(),text,box.style);
    } else {
        ApplySvgReplacementFallback(box.textLayout.Get(),text,box.style);
        ApplyPairKerning(box.textLayout.Get(),text,box.style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout1> extended;
        if (SUCCEEDED(box.textLayout.As(&extended))) {
            const float spacing = StyleSheet::Length(box.style.Get(L"letter-spacing",L"0"),FontSize(box.style),
                FontSize(box.style),0,FontSize(box.style));
            extended->SetCharacterSpacing(0,spacing,0,{0,static_cast<UINT32>(text.size())});
            ApplyWordSpacing(extended.Get(),text,box.style);
        }
    }
    ApplyTabSize(factory,box.textLayout.Get(),format.Get(),text,box.style);
    ApplyDiscretionaryHyphens(box.textLayout.Get(),text,box.style);
    ApplyTextDecorations(box.textLayout.Get(),text,box.style);
    box.textLayoutKey=std::move(layoutKey);
}

float Constrain(const ComputedStyle& style,const wchar_t* minimum,const wchar_t* maximum,float value,float reference,float viewport){
    const auto minValue=style.Get(minimum);if(!minValue.empty()&&minValue!=L"auto")value=std::max(value,StyleSheet::Length(minValue,reference,viewport,value));
    const auto maxValue=style.Get(maximum);if(!maxValue.empty()&&maxValue!=L"none"&&maxValue!=L"auto")value=std::min(value,StyleSheet::Length(maxValue,reference,viewport,value));
    return value;
}

float ConstrainIntrinsicHeight(const ComputedStyle& style,float value,float viewport){
    const auto display=style.Get(L"display");
    if(display==L"table-cell"||display==L"table-row"||display==L"table-row-group"||
       display==L"table-header-group"||display==L"table-footer-group")return value;
    const auto minimum=style.Get(L"min-height");
    if(!minimum.empty()&&minimum!=L"auto"&&minimum.find(L'%')==std::wstring::npos)
        value=std::max(value,StyleSheet::Length(minimum,500,viewport,value));
    const auto maximum=style.Get(L"max-height");
    if(display!=L"table"&&display!=L"inline-table"&&
       !maximum.empty()&&maximum!=L"none"&&maximum!=L"auto"&&
       maximum.find(L'%')==std::wstring::npos)
        value=std::min(value,StyleSheet::Length(maximum,500,viewport,value));
    return value;
}

float ConstrainIntrinsicWidth(const ComputedStyle& style,float value,float viewport){
    // Percentage inline-size constraints depend on the containing block. They
    // are indefinite while collecting max-content contributions and must be
    // resolved later by the formatting context that knows the real width.
    const auto minimum=style.Get(L"min-width");
    if(!minimum.empty()&&minimum!=L"auto"&&minimum.find(L'%')==std::wstring::npos)
        value=std::max(value,StyleSheet::Length(minimum,500,viewport,value));
    const auto maximum=style.Get(L"max-width");
    if(!maximum.empty()&&maximum!=L"none"&&maximum!=L"auto"&&
       maximum.find(L'%')==std::wstring::npos)
        value=std::min(value,StyleSheet::Length(maximum,500,viewport,value));
    return value;
}

float NaturalWidth(const LayoutBox& box,bool ignoreOwnSizing=false);
float MinContentWidth(const LayoutBox& box,bool intrinsic=false,bool ignoreOwnSizing=false);
float TableIntrinsicWidth(const LayoutBox& box,bool minimum);
float FixedTableMinimumWidth(const LayoutBox& box,float width,float viewportWidth);
float NaturalGridWidth(const LayoutBox& box,bool minimum=false);

bool IsIntrinsicWidth(const std::wstring& raw){
    const auto value=ToLower(Trim(raw));
    return value==L"min-content"||value==L"max-content"||value==L"fit-content";
}

bool HasIntrinsicWidth(const LayoutBox& box){
    if(box.style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(box)&&!IsBlockifiedItem(box)&&
       !box.style.Is(L"position",L"absolute")&&!box.style.Is(L"position",L"fixed")){
        const auto& tag=box.node->tag;
        const bool replaced=tag==L"img"||tag==L"canvas"||tag==L"input"||tag==L"select"||
            tag==L"textarea"||tag==L"iframe"||tag==L"embed"||tag==L"object"||tag==L"video";
        if(!replaced)return false;
    }
    return IsIntrinsicWidth(box.style.Get(L"width"))||
        IsIntrinsicWidth(box.style.Get(L"min-width"))||
        IsIntrinsicWidth(box.style.Get(L"max-width"));
}

float IntrinsicContentWidth(const LayoutBox& box,bool minimum){
    const auto margin=EdgeValues(box.style,L"margin",500,500);
    const auto padding=EdgeValues(box.style,L"padding",500,500);
    const auto border=UsedBorderValues(box);
    const float outer=minimum?MinContentWidth(box,true,true):NaturalWidth(box,true);
    return std::max(0.0f,outer-margin.left-margin.right-padding.left-padding.right-
        border.left-border.right);
}

float UsedIntrinsicWidth(const LayoutBox& box,const std::wstring& raw,float availableWidth,
                         float viewportWidth,bool indefinite=false){
    const auto margin=EdgeValues(box.style,L"margin",availableWidth,viewportWidth);
    const auto padding=EdgeValues(box.style,L"padding",availableWidth,viewportWidth);
    const auto border=UsedBorderValues(box);
    const float edges=padding.left+padding.right+border.left+border.right;
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");
    const float availableContent=std::max(0.0f,availableWidth-margin.left-margin.right-edges);
    const auto keywordWidth=[&](const std::wstring& value){
        const auto keyword=ToLower(Trim(value));
        float content=IntrinsicContentWidth(box,keyword==L"min-content");
        if(keyword==L"fit-content")content=std::max(IntrinsicContentWidth(box,true),
            std::min(content,availableContent));
        // Intrinsic keywords describe the content box regardless of box-sizing.
        return content+(borderBox?edges:0);
    };
    const auto normalized=ToLower(Trim(raw));
    float width=indefinite&&(normalized.empty()||normalized==L"auto"||raw.find(L'%')!=std::wstring::npos)?
        keywordWidth(L"max-content"):IsIntrinsicWidth(raw)?keywordWidth(raw):
        StyleSheet::Length(raw,availableWidth,viewportWidth,
            availableContent+(borderBox?edges:0),FontSize(box.style));
    const auto apply=[&](const wchar_t* property,bool minimum){
        const auto limit=ToLower(Trim(box.style.Get(property)));
        if(limit.empty()||limit==L"auto"||limit==L"none"||
           (indefinite&&limit.find(L'%')!=std::wstring::npos))return;
        const float value=IsIntrinsicWidth(limit)?keywordWidth(limit):
            StyleSheet::Length(limit,availableWidth,viewportWidth,width,FontSize(box.style));
        width=minimum?std::max(width,value):std::min(width,value);
    };
    apply(L"max-width",false);apply(L"min-width",true);
    return std::max(edges,borderBox?width:width+edges);
}

float BlockOuterWidth(const LayoutBox& box,float availableWidth,float viewportWidth){
    const auto margin=EdgeValues(box.style,L"margin",availableWidth,viewportWidth);
    const auto raw=box.style.Get(L"width");
    const bool intrinsicWidth=HasIntrinsicWidth(box);
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");const auto padding=EdgeValues(box.style,L"padding",availableWidth,viewportWidth);const auto border=UsedBorderValues(box);
    const float decoration=borderBox?0.0f:padding.left+padding.right+border.left+border.right;const bool automatic=raw.empty()||raw==L"auto";
    float width=intrinsicWidth?UsedIntrinsicWidth(box,raw,availableWidth,viewportWidth)-decoration:
        automatic?std::max(0.0f,availableWidth-margin.left-margin.right-decoration):StyleSheet::Length(raw,availableWidth,viewportWidth,std::max(0.0f,availableWidth-margin.left-margin.right-decoration));
    // A block button keeps its shrink-to-fit automatic width. Its authored
    // display changes outer participation without making it fill the line.
    if(!intrinsicWidth&&automatic&&box.node->tag==L"button"&&box.style.Is(L"display",L"block"))
        width=std::min(width,std::max(0.0f,NaturalWidth(box)-margin.left-margin.right-decoration));
    if(!intrinsicWidth&&automatic&&IsTable(box))
        width=std::max(MinContentWidth(box)-margin.left-margin.right-decoration,
            std::min(width,std::max(0.0f,NaturalWidth(box)-margin.left-margin.right-decoration)));
    if(!intrinsicWidth)width=Constrain(box.style,L"min-width",L"max-width",width,availableWidth,viewportWidth);
    width+=decoration;
    if(IsTable(box)){
        const float edges=padding.left+padding.right+border.left+border.right;
        const float minimum=!automatic&&box.style.Is(L"table-layout",L"fixed")?
            FixedTableMinimumWidth(box,std::max(0.0f,width-edges),viewportWidth):TableIntrinsicWidth(box,true);
        width=std::max(width,minimum+edges);
    }
    return std::max(0.0f,width)+margin.left+margin.right;
}

struct MarginStrut {
    float positive=0,negative=0;
    MarginStrut()=default;
    explicit MarginStrut(float value):positive(std::max(0.0f,value)),negative(std::min(0.0f,value)){}
    float Value() const {return positive+negative;}
    void Merge(const MarginStrut& other){
        positive=std::max(positive,other.positive);
        negative=std::min(negative,other.negative);
    }
};

struct BlockMarginMetrics {
    MarginStrut top,bottom;
    bool first=false,last=false,self=false;
};

BlockMarginMetrics CollapsedBlockMargins(const LayoutBox& box,float availableWidth){
    if(box.blockMarginsValid&&std::abs(box.blockMarginsReference-availableWidth)<0.01f){
        BlockMarginMetrics result;
        result.top.positive=box.marginTopPositive;result.top.negative=box.marginTopNegative;
        result.bottom.positive=box.marginBottomPositive;result.bottom.negative=box.marginBottomNegative;
        result.first=box.collapseFirstChildMargin;result.last=box.collapseLastChildMargin;
        result.self=box.selfCollapsingMargins;return result;
    }
    const auto margin=EdgeValues(box.style,L"margin",availableWidth,availableWidth);
    BlockMarginMetrics result;result.top=MarginStrut(margin.top);result.bottom=MarginStrut(margin.bottom);
    const auto display=box.style.Get(L"display");
    if((display==L"block"||display==L"list-item")&&
       !EstablishesBlockFormattingContext(box)&&!IsBlockifiedItem(box)){
        const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
        const auto border=BorderValues(box.style);
        const float outerWidth=BlockOuterWidth(box,availableWidth,availableWidth);
        const float innerWidth=std::max(1.0f,outerWidth-margin.left-margin.right-
            padding.left-padding.right-border.left-border.right);
        std::vector<const LayoutBox*> flow;
        for(const auto& child:box.children){
            if(!child->visible||child->style.Is(L"position",L"absolute")||
               child->style.Is(L"position",L"fixed")||UsedFloatSide(child->style)!=FloatSide::None)continue;
            flow.push_back(child.get());
        }
        const auto adjoining=[](const LayoutBox& child){
            return !IsInlineLevel(child.style.Get(L"display"))&&
                child.style.Get(L"clear",L"none")==L"none";
        };
        result.first=padding.top==0&&border.top==0&&!flow.empty()&&adjoining(*flow.front());
        const auto height=ToLower(Trim(box.style.Get(L"height",L"auto")));
        result.last=padding.bottom==0&&border.bottom==0&&
            (height.empty()||height==L"auto")&&!flow.empty()&&adjoining(*flow.back());
        if(result.first){
            for(const auto* child:flow){
                if(!adjoining(*child))break;
                const auto childMargins=CollapsedBlockMargins(*child,innerWidth);
                result.top.Merge(childMargins.top);
                if(!childMargins.self)break;
                result.top.Merge(childMargins.bottom);
            }
        }
        if(result.last){
            for(auto it=flow.rbegin();it!=flow.rend();++it){
                if(!adjoining(**it))break;
                const auto childMargins=CollapsedBlockMargins(**it,innerWidth);
                result.bottom.Merge(childMargins.bottom);
                if(!childMargins.self)break;
                result.bottom.Merge(childMargins.top);
            }
        }
        const bool zeroHeight=height.empty()||height==L"auto"||
            StyleSheet::Length(height,500,500,-1)==0;
        result.self=zeroHeight&&StyleSheet::Length(box.style.Get(L"min-height"),500,500,0)<=0&&
            padding.top==0&&padding.bottom==0&&border.top==0&&border.bottom==0&&
            !ParticipatesInEditableContent(box.node);
        for(const auto* child:flow)
            if(!adjoining(*child)||!CollapsedBlockMargins(*child,innerWidth).self)result.self=false;
        if(result.self){result.top.Merge(result.bottom);result.bottom=result.top;}
    }
    box.blockMarginsReference=availableWidth;box.blockMarginsValid=true;
    box.marginTopPositive=result.top.positive;box.marginTopNegative=result.top.negative;
    box.marginBottomPositive=result.bottom.positive;box.marginBottomNegative=result.bottom.negative;
    box.collapseFirstChildMargin=result.first;box.collapseLastChildMargin=result.last;
    box.selfCollapsingMargins=result.self;return result;
}

Edges UsedLayoutMargins(const LayoutBox& box,float reference,float viewport){
    auto margin=EdgeValues(box.style,L"margin",reference,viewport);
    if(box.node&&box.node->type==NodeType::Element){
        const auto collapsed=CollapsedBlockMargins(box,reference);
        margin.top=collapsed.top.Value();margin.bottom=collapsed.bottom.Value();
    }
    return margin;
}

struct BlockMarginFlow {
    BlockMarginMetrics parent;
    MarginStrut previousBottom,joinedTop;
    bool atTop=true,previousBlock=false;
    float Before(const BlockMarginMetrics& child,bool clearance=false){
        joinedTop=child.top;
        if(atTop&&parent.first&&!clearance)return -child.top.Value();
        if(previousBlock&&!clearance){
            joinedTop.Merge(previousBottom);
            return joinedTop.Value()-previousBottom.Value()-child.top.Value();
        }
        return 0;
    }
    float After(const BlockMarginMetrics& child){
        if(child.self){
            if(atTop&&parent.first)return -child.bottom.Value();
            auto joined=joinedTop;joined.Merge(child.bottom);
            previousBottom=joined;previousBlock=true;
            return joined.Value()-joinedTop.Value()-child.bottom.Value();
        }
        atTop=false;previousBlock=true;previousBottom=child.bottom;return 0;
    }
    void Separate(){atTop=false;previousBlock=false;previousBottom={};}
    float Finish() const {return parent.last&&previousBlock?-previousBottom.Value():0;}
};

float NaturalHeight(const LayoutBox& box,float availableWidth);

float BlockOuterHeight(const LayoutBox& box,float availableHeight,float availableWidth,
                       float viewportHeight,float viewportWidth){
    const auto margin=UsedLayoutMargins(box,availableWidth,viewportWidth);
    const auto raw=box.style.Get(L"height");
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");
    const auto padding=EdgeValues(box.style,L"padding",availableWidth,viewportWidth);
    const auto border=UsedBorderValues(box);
    const float decoration=borderBox?0.0f:padding.top+padding.bottom+border.top+border.bottom;
    float height=StyleSheet::Length(raw,availableHeight,viewportHeight,
        std::max(0.0f,availableHeight-margin.top-margin.bottom-decoration));
    height=Constrain(box.style,L"min-height",L"max-height",height,availableHeight,viewportHeight)+decoration;
    if(IsTable(box)){
        float captions=0;
        for(const auto& child:box.children)if(child->visible&&child->style.Is(L"display",L"table-caption"))
            captions+=NaturalHeight(*child,availableWidth);
        height=std::max(height+captions,NaturalHeight(box,availableWidth)-margin.top-margin.bottom);
    }
    return std::max(0.0f,height)+margin.top+margin.bottom;
}

size_t GridColumnCount(const LayoutBox& box,float availableWidth){
    const auto definition=box.style.Get(L"grid-template-columns",L"none");
    if(definition.find(L"repeat(auto-fit")!=std::wstring::npos||definition.find(L"repeat(auto-fill")!=std::wstring::npos){
        float minimum=320;const auto minmax=definition.find(L"minmax(");
        if(minmax!=std::wstring::npos){const auto comma=definition.find(L',',minmax);if(comma!=std::wstring::npos)minimum=StyleSheet::Length(definition.substr(minmax+7,comma-minmax-7),availableWidth,availableWidth,minimum);}
        const float gap=GapValue(box.style,true,availableWidth,availableWidth);
        const size_t fits=static_cast<size_t>(std::max(1.0f,std::floor((availableWidth+gap)/std::max(1.0f,minimum+gap))));
        return std::max<size_t>(1,std::min(fits,std::max<size_t>(1,box.children.size())));
    }
    const auto repeat=definition.find(L"repeat(");
    if(repeat!=std::wstring::npos){unsigned long long parsed=0;
        if(TryParseUnsignedInteger(definition.substr(repeat+7),parsed)&&
           parsed<=(std::numeric_limits<size_t>::max)())
            return std::max<size_t>(1,static_cast<size_t>(parsed));
    }
    return std::max<size_t>(1,Words(definition).size());
}

bool HasTableDisplay(const LayoutBox& box,const wchar_t* display){
    return box.visible&&box.style.Is(L"display",display);
}

bool IsTableRowGroup(const LayoutBox& box){
    const auto display=box.style.Get(L"display");
    return box.visible&&(display==L"table-header-group"||display==L"table-row-group"||
        display==L"table-footer-group");
}

// CSS 2.1 section 17.2.1: repair the formatting tree, never the DOM. In
// particular, consecutive ordinary children of a table share one anonymous
// row/cell; treating each child as a row changes both width and height.
void NormalizeTableChildren(LayoutBox& box){
    if(box.children.empty())return;
    const auto display=box.style.Get(L"display");
    const bool table=IsTable(box),group=IsTableRowGroup(box),row=display==L"table-row";
    const auto properChild=[](const LayoutBox& child){
        const auto value=child.style.Get(L"display");
        return value==L"table-row"||IsTableRowGroup(child)||value==L"table-column"||
            value==L"table-column-group"||value==L"table-caption";
    };
    if(display==L"table-column"||display==L"table-column-group"){
        for(auto& child:box.children)
            if(display==L"table-column"||!child->style.Is(L"display",L"table-column"))child->visible=false;
        return;
    }
    if(table||group||row){
        for(auto& child:box.children)
            if(child->node->type==NodeType::Text&&
                std::all_of(child->node->text.begin(),child->node->text.end(),
                    [](wchar_t c){return IsCollapsibleTextSpace(c);}))child->visible=false;
    }
    const auto wrapRuns=[&](const std::wstring& wrapperDisplay,const auto& needsWrapper){
        if(std::none_of(box.children.begin(),box.children.end(),[&](const auto& child){
            return child->visible&&needsWrapper(*child);
        }))return;
        std::vector<std::unique_ptr<LayoutBox>> result;
        for(size_t index=0;index<box.children.size();){
            if(!box.children[index]->visible||!needsWrapper(*box.children[index])){
                result.push_back(std::move(box.children[index++]));continue;
            }
            auto wrapper=std::make_unique<LayoutBox>();wrapper->parent=&box;
            wrapper->node=std::make_shared<Node>();wrapper->node->tag=L"#anonymous-table";
            wrapper->node->parent=box.node;wrapper->generatedFrom=box.node;
            wrapper->pseudo=L"anonymous-table";
            wrapper->style=StyleSheet::AnonymousStyle(box.style,wrapperDisplay);
            do{
                auto child=std::move(box.children[index++]);child->parent=wrapper.get();
                wrapper->containsSticky=wrapper->containsSticky||child->containsSticky;
                wrapper->children.push_back(std::move(child));
            }while(index<box.children.size()&&box.children[index]->visible&&needsWrapper(*box.children[index]));
            NormalizeTableChildren(*wrapper);result.push_back(std::move(wrapper));
        }
        box.children=std::move(result);
    };
    if(table)wrapRuns(L"table-row",[&](const LayoutBox& child){return !properChild(child);});
    else if(group)wrapRuns(L"table-row",[](const LayoutBox& child){return !child.style.Is(L"display",L"table-row");});
    else if(row)wrapRuns(L"table-cell",[](const LayoutBox& child){return !child.style.Is(L"display",L"table-cell");});
    const bool layoutItems=display==L"flex"||display==L"inline-flex"||display==L"grid"||display==L"inline-grid";
    const auto inFlow=[](const LayoutBox& child){return !child.style.Is(L"position",L"absolute")&&
        !child.style.Is(L"position",L"fixed")&&UsedFloatSide(child.style)==FloatSide::None;};
    if(!row&&!layoutItems)wrapRuns(L"table-row",[&](const LayoutBox& child){return inFlow(child)&&child.style.Is(L"display",L"table-cell");});
    if(!table&&!layoutItems){
        wrapRuns(display==L"inline"?L"inline-table":L"table",[&](const LayoutBox& child){
            if(!inFlow(child))return false;
            const auto value=child.style.Get(L"display");
            if(value==L"table-row")return !group;
            if(value==L"table-column")return display!=L"table-column-group";
            return IsTableRowGroup(child)||value==L"table-column-group"||value==L"table-caption";
        });
    }
}

size_t TableSpan(const std::shared_ptr<Node>& node,const wchar_t* attribute,
                 size_t maximum,bool zeroIsSpecial=false){
    const auto raw=Trim(node?node->Attribute(attribute):L"");
    if(raw.empty())return 1;
    size_t value=0;
    for(const wchar_t c:raw){
        if(c<L'0'||c>L'9')return 1;
        const size_t digit=static_cast<size_t>(c-L'0');
        if(value>(maximum-digit)/10)return maximum;
        value=value*10+digit;
    }
    if(value==0)return zeroIsSpecial?0:1;
    return std::min(value,maximum);
}

} // namespace

struct TableRowEntry {
    LayoutBox* box = nullptr;
    LayoutBox* group = nullptr;
};

struct TableCellEntry {
    LayoutBox* box = nullptr;
    size_t row = 0;
    size_t column = 0;
    size_t rowSpan = 1;
    size_t columnSpan = 1;
};

struct TableGridModel {
    std::vector<TableRowEntry> rows;
    std::vector<TableCellEntry> cells;
    struct Column { LayoutBox* box; LayoutBox* group; };
    std::vector<Column> columns;
    std::vector<LayoutBox*> captions;
    size_t columnCount = 0;
    mutable bool columnsValid = false;
    mutable bool columnsUseViewport = false;
    mutable float availableWidth = 0, viewportWidth = 0;
    mutable std::vector<float> usedColumns;
    mutable bool rowsValid = false;
    mutable std::vector<float> measuredRows;
    mutable bool intrinsicColumnsValid = false;
    mutable std::vector<float> minimumColumns,preferredColumns,percentageColumns;
    mutable std::vector<unsigned char> constrainedColumns,originatingColumns;
};

namespace {

const TableGridModel& BuildTableGrid(LayoutBox& table){
    if(table.tableGrid)return *table.tableGrid;
    table.tableGrid=std::make_shared<TableGridModel>();
    auto& model=*table.tableGrid;
    // Columns belong to this table alone. Walking the DOM recursively would
    // let an inner table's colgroup reserve tracks in its outer table.
    const auto addColumn=[&](LayoutBox& column,LayoutBox* group){
        const size_t span=TableSpan(column.node,L"span",1000);
        for(size_t i=0;i<span;++i)model.columns.push_back({&column,group});
    };
    for(auto& child:table.children){
        if(HasTableDisplay(*child,L"table-column"))addColumn(*child,nullptr);
        else if(HasTableDisplay(*child,L"table-column-group")){
            const auto begin=model.columns.size();
            for(auto& column:child->children)
                if(HasTableDisplay(*column,L"table-column"))addColumn(*column,child.get());
            if(begin==model.columns.size())addColumn(*child,child.get());
        }else if(HasTableDisplay(*child,L"table-caption"))model.captions.push_back(child.get());
    }
    model.columnCount=model.columns.size();
    std::function<void(LayoutBox&,LayoutBox*)> collectRows=
        [&](LayoutBox& current,LayoutBox* group){
            if(!current.visible)return;
            if(&current!=&table&&IsTable(current))return;
            if(IsTableRowGroup(current))group=&current;
            if(HasTableDisplay(current,L"table-row")){
                model.rows.push_back({&current,group?group:&table});
                return;
            }
            for(auto& child:current.children)collectRows(*child,group);
        };
    for(auto& child:table.children)collectRows(*child,nullptr);
    if(model.rows.empty())return model;

    LayoutBox* header=nullptr;LayoutBox* footer=nullptr;
    for(const auto& child:table.children)if(child->visible){
        if(!header&&child->style.Is(L"display",L"table-header-group"))header=child.get();
        if(!footer&&child->style.Is(L"display",L"table-footer-group"))footer=child.get();
    }
    // Only the first header/footer group is special. Other groups retain
    // their source order, including additional headers and footers.
    std::stable_sort(model.rows.begin(),model.rows.end(),[&](const auto& a,const auto& b){
        const auto rank=[&](const auto& entry){return entry.group==header?0:entry.group==footer?2:1;};
        return rank(a)<rank(b);
    });

    std::vector<size_t> groupEnds(model.rows.size());
    for(size_t begin=0;begin<model.rows.size();){
        size_t end=begin+1;
        while(end<model.rows.size()&&model.rows[end].group==model.rows[begin].group)++end;
        for(size_t row=begin;row<end;++row)groupEnds[row]=end;
        begin=end;
    }

    std::vector<std::vector<unsigned char>> occupied(model.rows.size());
    for(size_t row=0;row<model.rows.size();++row){
        size_t searchColumn=0;
        for(auto& child:model.rows[row].box->children){
            if(!HasTableDisplay(*child,L"table-cell")||
               child->style.Is(L"position",L"absolute")||
               child->style.Is(L"position",L"fixed"))continue;
            const size_t columnSpan=TableSpan(child->node,L"colspan",1000);
            const size_t authoredRowSpan=TableSpan(child->node,L"rowspan",65534,true);
            const size_t rowSpan=authoredRowSpan==0?groupEnds[row]-row:
                std::min(authoredRowSpan,groupEnds[row]-row);
            for(;;++searchColumn){
                if(occupied[row].size()<searchColumn+columnSpan)
                    occupied[row].resize(searchColumn+columnSpan);
                bool available=true;
                for(size_t column=searchColumn;column<searchColumn+columnSpan;++column)
                    if(occupied[row][column]){available=false;break;}
                if(available)break;
            }
            for(size_t targetRow=row;targetRow<row+rowSpan;++targetRow){
                if(occupied[targetRow].size()<searchColumn+columnSpan)
                    occupied[targetRow].resize(searchColumn+columnSpan);
                for(size_t column=searchColumn;column<searchColumn+columnSpan;++column)
                    occupied[targetRow][column]=1;
            }
            model.cells.push_back({child.get(),row,searchColumn,rowSpan,columnSpan});
            model.columnCount=std::max(model.columnCount,searchColumn+columnSpan);
            searchColumn+=columnSpan;
        }
        model.columnCount=std::max(model.columnCount,occupied[row].size());
    }
    return model;
}

struct CollapsedEdge {
    const ComputedStyle* style=nullptr;
    const wchar_t* side=L"top";
    float width=0;
    int rank=0,ownerRank=0;
    size_t row=0,column=0;
};

int CollapsedStyleRank(const ComputedStyle& style,const wchar_t* side){
    const auto value=Words(style.Get(std::wstring(L"border-")+side+L"-style",
        style.Get(L"border-style",style.Get(std::wstring(L"border-")+side,style.Get(L"border")))));
    for(const auto& token:value){
        if(token==L"hidden")return 100;
        if(token==L"double")return 8;if(token==L"solid")return 7;
        if(token==L"dashed")return 6;if(token==L"dotted")return 5;
        if(token==L"ridge")return 4;if(token==L"outset")return 3;
        if(token==L"groove")return 2;if(token==L"inset")return 1;
    }
    return 0;
}

struct CollapsedTableEdges {
    std::vector<std::vector<CollapsedEdge>> horizontal,vertical;
};

CollapsedTableEdges ResolveCollapsedTableEdges(const LayoutBox& table,const TableGridModel& model){
    const auto rowCount=model.rows.size(),columnCount=model.columnCount;
    CollapsedTableEdges edges;
    edges.horizontal.resize(rowCount+1,std::vector<CollapsedEdge>(columnCount));
    edges.vertical.resize(rowCount,std::vector<CollapsedEdge>(columnCount+1));
    const bool rtl=table.style.Is(L"direction",L"rtl");
    auto choose=[&](CollapsedEdge& previous,const ComputedStyle& style,const wchar_t* side,
                    int ownerRank,size_t row,size_t column){
        CollapsedEdge next{&style,side,BorderWidth(style,side),CollapsedStyleRank(style,side),ownerRank,row,column};
        if(!next.rank)return;
        bool wins=!previous.style||next.rank==100;
        if(previous.rank==100)wins=false;
        else if(!wins){
            if(next.width!=previous.width)wins=next.width>previous.width;
            else if(next.rank!=previous.rank)wins=next.rank>previous.rank;
            else if(next.ownerRank!=previous.ownerRank)wins=next.ownerRank>previous.ownerRank;
            else if(next.row!=previous.row)wins=next.row<previous.row;
            // Columns are indexed from inline-start: the earlier column is
            // the physical left one in LTR and the physical right one in RTL.
            else wins=next.column<previous.column;
        }
        if(wins)previous=next;
    };
    auto add=[&](const ComputedStyle& style,size_t row,size_t column,size_t rows,size_t columns,int rank){
        for(size_t c=column;c<column+columns;++c){
            choose(edges.horizontal[row][c],style,L"top",rank,row,column);
            choose(edges.horizontal[row+rows][c],style,L"bottom",rank,row,column);
        }
        for(size_t r=row;r<row+rows;++r){
            choose(edges.vertical[r][column],style,rtl?L"right":L"left",rank,row,column);
            choose(edges.vertical[r][column+columns],style,rtl?L"left":L"right",rank,row,column);
        }
    };
    if(rowCount&&columnCount)add(table.style,0,0,rowCount,columnCount,1);
    for(size_t row=0;row<rowCount;++row){
        add(model.rows[row].box->style,row,0,1,columnCount,5);
        auto* group=model.rows[row].group;
        if(group&&(row==0||model.rows[row-1].group!=group)){
            size_t end=row+1;while(end<rowCount&&model.rows[end].group==group)++end;
            add(group->style,row,0,end-row,columnCount,4);
        }
    }
    for(const auto& cell:model.cells)add(cell.box->style,cell.row,cell.column,cell.rowSpan,cell.columnSpan,6);
    return edges;
}

Edges UsedBorderValues(const LayoutBox& box){
    if(box.collapsedBordersResolved)
        return {box.collapsedTop,box.collapsedRight,box.collapsedBottom,box.collapsedLeft};
    const LayoutBox* table=&box;
    if(HasTableDisplay(box,L"table-cell")){
        table=box.parent;
        while(table&&!IsTable(*table))table=table->parent;
    }else if(!IsTable(box))return BorderValues(box.style);
    if(!table||!table->style.Is(L"border-collapse",L"collapse"))return BorderValues(box.style);
    const auto& model=BuildTableGrid(*const_cast<LayoutBox*>(table));
    const auto resolved=ResolveCollapsedTableEdges(*table,model);
    const bool rtl=table->style.Is(L"direction",L"rtl");
    Edges tableEdges{};
    for(const auto& cell:model.cells){
        Edges edges{};
        for(size_t c=cell.column;c<cell.column+cell.columnSpan;++c){
            edges.top=std::max(edges.top,resolved.horizontal[cell.row][c].width);
            edges.bottom=std::max(edges.bottom,resolved.horizontal[cell.row+cell.rowSpan][c].width);
        }
        for(size_t r=cell.row;r<cell.row+cell.rowSpan;++r){
            edges.left=std::max(edges.left,resolved.vertical[r][cell.column+(rtl?cell.columnSpan:0)].width);
            edges.right=std::max(edges.right,resolved.vertical[r][cell.column+(rtl?0:cell.columnSpan)].width);
        }
        auto& used=*cell.box;used.collapsedBordersResolved=true;
        used.collapsedTop=edges.top/2;used.collapsedRight=edges.right/2;
        used.collapsedBottom=edges.bottom/2;used.collapsedLeft=edges.left/2;
        if(cell.row==0){
            tableEdges.top=std::max(tableEdges.top,used.collapsedTop);
            if(cell.column==(rtl?model.columnCount-cell.columnSpan:0))tableEdges.left=used.collapsedLeft;
            if(cell.column==(rtl?0:model.columnCount-cell.columnSpan))tableEdges.right=used.collapsedRight;
        }
        if(cell.row+cell.rowSpan==model.rows.size())tableEdges.bottom=std::max(tableEdges.bottom,used.collapsedBottom);
    }
    table->collapsedBordersResolved=true;
    table->collapsedTop=tableEdges.top;table->collapsedRight=tableEdges.right;
    table->collapsedBottom=tableEdges.bottom;table->collapsedLeft=tableEdges.left;
    return {box.collapsedTop,box.collapsedRight,box.collapsedBottom,box.collapsedLeft};
}

// A table cell's declared width is a preferred contribution, not a substitute
// for measuring its contents. Measure the contents before applying that hint.
D2D1_POINT_2F TableBorderSpacing(const LayoutBox& table);
float TableCellContentWidth(const LayoutBox& cell,bool minimum){
    float width=0,line=0;
    for(const auto& child:cell.children){
        if(!child->visible||child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))continue;
        const float contribution=minimum?MinContentWidth(*child,true):NaturalWidth(*child);
        if(minimum)width=std::max(width,contribution);
        else if(child->node->tag==L"br"){width=std::max(width,line);line=0;}
        else if(IsInlineLevel(child->style.Get(L"display")))line+=contribution;
        else{width=std::max(width,std::max(line,contribution));line=0;}
    }
    width=std::max(width,line);
    const auto padding=EdgeValues(cell.style,L"padding",0,0),border=UsedBorderValues(cell);
    const float decoration=padding.left+padding.right+border.left+border.right;
    const auto minWidth=Trim(cell.style.Get(L"min-width")),maxWidth=Trim(cell.style.Get(L"max-width"));
    if(!minWidth.empty()&&minWidth!=L"auto"&&minWidth.find(L'%')==std::wstring::npos){
        const float limit=StyleSheet::Length(minWidth,0,0,0,FontSize(cell.style));
        width=std::max(width,cell.style.Is(L"box-sizing",L"border-box")?limit-decoration:limit);
    }
    if(!minimum&&!maxWidth.empty()&&maxWidth!=L"none"&&maxWidth!=L"auto"&&maxWidth.find(L'%')==std::wstring::npos){
        const float limit=StyleSheet::Length(maxWidth,0,0,width,FontSize(cell.style));
        width=std::min(width,cell.style.Is(L"box-sizing",L"border-box")?limit-decoration:limit);
    }
    return std::max(0.0f,width)+decoration;
}

void MeasureAutoTableColumns(const LayoutBox& table,const TableGridModel& model){
    if(model.intrinsicColumnsValid)return;
    model.intrinsicColumnsValid=true;
    const size_t count=model.columnCount;
    auto& minimum=model.minimumColumns;auto& preferred=model.preferredColumns;
    auto& percentage=model.percentageColumns;auto& constrained=model.constrainedColumns;
    minimum.assign(count,0);preferred.assign(count,0);percentage.assign(count,0);
    constrained.assign(count,0);model.originatingColumns.assign(count,0);
    const auto spacing=TableBorderSpacing(table);
    const auto hint=[&](const LayoutBox& box,std::wstring raw,float& percent,bool& specified){
        raw=Trim(raw);if(raw.empty()||raw==L"auto")return 0.0f;
        if(raw.find(L'%')!=std::wstring::npos){
            percent=std::max(percent,std::max(0.0f,StyleSheet::Length(raw,100,0,0,FontSize(box.style))/100));
            return 0.0f;
        }
        specified=true;
        const float width=std::max(0.0f,StyleSheet::Length(raw,0,0,0,FontSize(box.style)));
        if(!box.style.Is(L"display",L"table-cell"))return width;
        const auto padding=EdgeValues(box.style,L"padding",0,0),border=UsedBorderValues(box);
        const float decoration=padding.left+padding.right+border.left+border.right;
        return box.style.Is(L"box-sizing",L"border-box")?std::max(width,decoration):width+decoration;
    };
    for(size_t i=0;i<model.columns.size();++i){
        const auto& column=model.columns[i];auto raw=column.box->style.Get(L"width");
        if((raw.empty()||raw==L"auto")&&column.group)raw=column.group->style.Get(L"width");
        bool specified=false;preferred[i]=hint(*column.box,raw,percentage[i],specified);constrained[i]=specified;
    }
    struct Contribution {const TableCellEntry* cell;float minimum,preferred,percentage;};
    std::vector<Contribution> spans;
    for(const auto& cell:model.cells){
        model.originatingColumns[cell.column]=1;
        float percent=0;bool specified=false;
        auto raw=cell.box->style.Get(L"width");
        if((raw.empty()||raw==L"auto")&&cell.box->node)raw=cell.box->node->Attribute(L"width");
        const float desired=hint(*cell.box,raw,percent,specified);
        const float min=TableCellContentWidth(*cell.box,true);
        const float max=std::max(min,std::max(desired,TableCellContentWidth(*cell.box,false)));
        if(cell.columnSpan==1){
            minimum[cell.column]=std::max(minimum[cell.column],min);
            preferred[cell.column]=std::max(preferred[cell.column],max);
            percentage[cell.column]=std::max(percentage[cell.column],percent);
            constrained[cell.column]=constrained[cell.column]||specified;
        }else spans.push_back({&cell,min,max,percent});
    }
    for(size_t i=0;i<count;++i)preferred[i]=std::max(preferred[i],minimum[i]);
    std::stable_sort(spans.begin(),spans.end(),[](const auto& a,const auto& b){return a.cell->columnSpan<b.cell->columnSpan;});
    for(const auto& span:spans){
        const size_t begin=span.cell->column,end=std::min(count,begin+span.cell->columnSpan);
        if(end<=begin)continue;
        const float gaps=spacing.x*(end-begin-1);
        float minSum=0,maxSum=0,percentSum=0;
        for(size_t i=begin;i<end;++i){minSum+=minimum[i];maxSum+=preferred[i];percentSum+=percentage[i];}
        const float within=std::min(std::max(0.0f,span.minimum-gaps-minSum),maxSum-minSum);
        const float beyond=std::max(0.0f,span.minimum-gaps-maxSum);
        const float maxDeficit=std::max(0.0f,span.preferred-gaps-maxSum);
        float percentWeight=0;size_t percentTracks=0;
        for(size_t i=begin;i<end;++i)if(percentage[i]==0){percentWeight+=preferred[i];++percentTracks;}
        for(size_t i=begin;i<end;++i){
            const float weight=maxSum>0?preferred[i]/maxSum:1.0f/static_cast<float>(end-begin);
            const float flexibility=maxSum>minSum?(preferred[i]-minimum[i])/(maxSum-minSum):0;
            minimum[i]+=within*flexibility+beyond*weight;
            preferred[i]=std::max(minimum[i],preferred[i]+maxDeficit*weight);
            if(percentage[i]==0&&percentTracks)
                percentage[i]=std::max(0.0f,span.percentage-percentSum)*
                    (percentWeight>0?(maxSum*weight)/percentWeight:1.0f/percentTracks);
        }
    }
    // A percentage column cannot reserve more than the remaining percentage.
    float reserved=0;
    for(auto& percent:percentage){percent=std::min(percent,std::max(0.0f,1-reserved));reserved+=percent;}
}

const std::vector<float>& ResolveTableColumns(const LayoutBox& table,const TableGridModel& model,
                                       float availableWidth,float viewportWidth);
D2D1_POINT_2F TableBorderSpacing(const LayoutBox& table){
    if(table.style.Is(L"border-collapse",L"collapse"))return {0,0};
    // CSS's initial spacing is zero; the HTML table UA rule supplies 2px.
    std::wistringstream tokens(table.style.Get(L"border-spacing",L"0px"));std::wstring x,y;
    tokens>>x;if(!(tokens>>y))y=x;
    const float scale=std::max(0.01f,table.style.deviceScale);
    const auto used=[&](const std::wstring& length){
        return std::floor(std::max(0.0f,StyleSheet::Length(length,0,0,0))*scale+0.0001f)/scale;
    };
    return {used(x),used(y)};
}
const std::vector<float>& ResolveTableRows(const TableGridModel& model,
                                    const std::vector<float>& columns,D2D1_POINT_2F spacing);
void MeasureAutoTableColumns(const LayoutBox& table,const TableGridModel& model);

float TableIntrinsicWidth(const LayoutBox& box,bool minimum){
    const auto& model=BuildTableGrid(*const_cast<LayoutBox*>(&box));
    const auto spacing=TableBorderSpacing(box);
    const auto tableWidth=Trim(box.style.Get(L"width"));
    if(!box.style.Is(L"table-layout",L"fixed")||tableWidth.empty()||tableWidth==L"auto"){
        MeasureAutoTableColumns(box,model);
        const auto& columns=minimum?model.minimumColumns:model.preferredColumns;
        float width=std::accumulate(columns.begin(),columns.end(),0.0f)+spacing.x*(columns.size()+1);
        if(!minimum){
            float percentSum=0,nonPercent=0;
            for(size_t i=0;i<columns.size();++i){
                const float percent=model.percentageColumns[i];percentSum+=percent;
                if(percent>0)width=std::max(width,columns[i]/percent+spacing.x*(columns.size()+1));
                else nonPercent+=columns[i];
            }
            if(percentSum<1)width=std::max(width,nonPercent/(1-percentSum)+spacing.x*(columns.size()+1));
        }
        for(const auto* caption:model.captions)
            width=std::max(width,minimum?MinContentWidth(*caption,true):NaturalWidth(*caption));
        return width;
    }
    std::vector<float> widths(model.columnCount);
    for(size_t i=0;i<model.columns.size();++i){
        const auto& column=model.columns[i];
        auto raw=column.box->style.Get(L"width");
        if((raw.empty()||raw==L"auto")&&column.group)raw=column.group->style.Get(L"width");
        // Percentage constraints are indefinite during intrinsic measurement.
        if(!raw.empty()&&raw!=L"auto"&&raw.find(L'%')==std::wstring::npos)
            widths[i]=std::max(0.0f,StyleSheet::Length(raw,0,0,0,FontSize(column.box->style)));
    }
    for(const auto& cell:model.cells){
        const size_t end=std::min(widths.size(),cell.column+cell.columnSpan);
        float current=spacing.x*(end-cell.column-1);
        for(size_t column=cell.column;column<end;++column)current+=widths[column];
        float required=minimum?MinContentWidth(*cell.box):NaturalWidth(*cell.box);
        // Most cells have no legacy width hint. Avoid looking up/copying the
        // computed CSS width unless a hint can contribute to intrinsic size.
        if(cell.box->node&&cell.box->node->attributes.count(L"width")){
          const auto cssWidth=Trim(cell.box->style.Get(L"width"));
          if(cssWidth.empty()||cssWidth==L"auto"){
            const auto hint=Trim(cell.box->node->Attribute(L"width"));
            if(!hint.empty()&&hint.find(L'%')==std::wstring::npos){
                const auto padding=EdgeValues(cell.box->style,L"padding",500,500);
                const auto border=UsedBorderValues(*cell.box);
                const float decoration=padding.left+padding.right+border.left+border.right;
                const float width=StyleSheet::Length(hint,500,500,0,FontSize(cell.box->style));
                required=std::max(required,cell.box->style.Is(L"box-sizing",L"border-box")?
                    std::max(width,decoration):std::max(0.0f,width)+decoration);
            }
          }
        }
        const float share=std::max(0.0f,required-current)/static_cast<float>(end-cell.column);
        for(size_t column=cell.column;column<end;++column)widths[column]+=share;
    }
    float width=std::accumulate(widths.begin(),widths.end(),0.0f)+spacing.x*(widths.size()+1);
    for(const auto& child:box.children)if(child->visible&&child->style.Is(L"display",L"table-caption"))
        width=std::max(width,minimum?MinContentWidth(*child):NaturalWidth(*child));
    return width;
}

float NaturalWidth(const LayoutBox& box,bool ignoreOwnSizing){
    if(ignoreOwnSizing?box.maxContentWidthValid:box.naturalWidthValid)
        return ignoreOwnSizing?box.maxContentWidth:box.naturalWidth;
    auto remember=[&](float value){
        if(ignoreOwnSizing){box.maxContentWidth=value;box.maxContentWidthValid=true;}
        else{box.naturalWidth=value;box.naturalWidthValid=true;}
        return value;
    };
    if(box.node->type==NodeType::Text){
        float advance=TextWidth(box.node->text,box.style,box.preserveLeadingWhitespace&&!box.trimLeadingLineWhitespace,
                                      box.preserveTrailingWhitespace);
        if(box.textFlowFragment&&box.generatedFrom&&box.textSourceOffset>0&&
           !box.trimLeadingLineWhitespace){
            // Keep the original run's cumulative advance instead of rounding
            // each word independently to a layout unit.
            advance=TextFlowFragmentAdvance(box);
        }
        return remember(advance);
    }
    if(box.node->tag==L"br")return remember(0.0f);
    auto width=box.style.Get(L"width");float value=0;
    // Percentages depend on the containing block and are indefinite during
    // intrinsic sizing.  Use the element's intrinsic contribution here; the
    // percentage is resolved later when the containing block is known.
    if(!ignoreOwnSizing&&HasIntrinsicWidth(box)){
        const auto margin=EdgeValues(box.style,L"margin",500,500);
        return remember(UsedIntrinsicWidth(box,width,500,500,true)+margin.left+margin.right);
    }
    const bool definiteWidth=!ignoreOwnSizing&&!width.empty()&&width!=L"auto"&&!IsIntrinsicWidth(width)&&
        width.find(L'%')==std::wstring::npos;
    if(definiteWidth){
        value=ConstrainIntrinsicWidth(box.style,
            StyleSheet::Length(width,500,500,0,FontSize(box.style)),500);
        const auto padding=EdgeValues(box.style,L"padding",500,500);
        const auto border=UsedBorderValues(box);
        const float decoration=padding.left+padding.right+border.left+border.right;
        value=box.style.Is(L"box-sizing",L"border-box")?std::max(decoration,value):
            decoration+std::max(0.0f,value);
    }
    else if(box.node->tag==L"input"&&(box.node->Attribute(L"type")==L"checkbox"||box.node->Attribute(L"type")==L"radio"))value=13;
    else if(box.node->tag==L"input"){
        const auto type=ToLower(box.node->Attribute(L"type"));
        if(type==L"button"||type==L"submit"||type==L"reset"){
            const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=UsedBorderValues(box);
            const auto label=box.node->Attribute(L"value");
            value=TextWidth(label,box.style)+padding.left+padding.right+border.left+border.right;
        }else value=160;
    }
    else if(box.node->tag==L"select"){
        float optionWidth=TextWidth(SelectedOptionText(box.node),box.style);
        std::function<void(const std::shared_ptr<Node>&)> measureOptions=
            [&](const std::shared_ptr<Node>& node){
                for(const auto& child:node->children){
                    if(child->tag==L"option")
                        optionWidth=std::max(optionWidth,TextWidth(child->InnerText(),box.style));
                    else measureOptions(child);
                }
            };
        measureOptions(box.node);
        // Native select appearance reserves its leading text inset and the
        // disclosure-button area in addition to the widest option.
        value=optionWidth+20.0f;
    }
    else if(box.node->tag==L"canvas")
        value=StyleSheet::Length(box.node->Attribute(L"width"),500,500,300);
    else if(box.node->tag==L"iframe"||box.node->tag==L"embed"||
            (box.node->tag==L"object"&&(box.node->attributes.count(L"data")||box.node->attributes.count(L"type")))||box.node->tag==L"video")
        value=StyleSheet::Length(box.node->Attribute(L"width"),500,500,300);
    else if(box.node->tag==L"img"){
        const auto authoredWidth=Trim(box.node->Attribute(L"width"));
        const auto authoredHeight=Trim(box.node->Attribute(L"height"));
        if(!authoredWidth.empty())value=StyleSheet::Length(authoredWidth,500,500,0);
        else if(box.node->image){
            const auto cssHeight=Trim(box.style.Get(L"height"));
            const auto ratioHeight=!cssHeight.empty()&&cssHeight!=L"auto"?
                cssHeight:authoredHeight;
            if(!ratioHeight.empty()&&box.node->image->height)
                value=StyleSheet::Length(ratioHeight,500,500,0)*
                    box.node->image->width/box.node->image->height;
            else value=static_cast<float>(box.node->image->width);
        }
    }
    else if(box.node->tag==L"svg"){
        const auto viewBox=SvgNumbers(box.node->Attribute(L"viewbox"));
        const auto height=box.style.Get(L"height",box.node->Attribute(L"height"));
        if(!height.empty()&&height!=L"auto"&&viewBox.size()==4&&viewBox[3]>0)
            value=StyleSheet::Length(height,500,500,150)*viewBox[2]/viewBox[3];
        else value=StyleSheet::Length(box.node->Attribute(L"width"),500,500,300);
    }
    else if(box.style.Is(L"display",L"grid")||box.style.Is(L"display",L"inline-grid")){
        const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=UsedBorderValues(box);
        value=NaturalGridWidth(box)+padding.left+padding.right+border.left+border.right;
    }
    else if(IsTable(box)){
        const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=UsedBorderValues(box);
        value=TableIntrinsicWidth(box,false)+padding.left+padding.right+border.left+border.right;
    }
    else if(box.node->tag==L"button"&&!HasInFlowBlockChildren(box)&&box.style.Get(L"display")!=L"grid"&&
            box.style.Get(L"display")!=L"inline-grid"){
        auto padding=EdgeValues(box.style,L"padding",500,500);auto border=UsedBorderValues(box);
        const auto display=box.style.Get(L"display");
        const bool flex=display==L"flex"||display==L"inline-flex";
        const bool row=!flex||!IsColumnFlexDirection(box.style);
        float content=0;int visible=0;
        for(const auto& child:box.children)
            if(child->visible&&!child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")){
                const float childWidth=NaturalWidth(*child);
                content=row?content+childWidth:std::max(content,childWidth);
                ++visible;
            }
        if(box.children.empty())content=TextWidth(box.node->InnerText(),box.style);
        if(flex&&row)content+=GapValue(box.style,true,500,500)*std::max(0,visible-1);
        value=content+padding.left+padding.right+border.left+border.right;
    }
    else{
        const auto display=box.style.Get(L"display");const bool flex=display==L"flex"||display==L"inline-flex";
        // The outer inline participation of inline-flex does not change its
        // children's main axis. A column's intrinsic width is the widest item.
        const bool horizontal=flex?!IsColumnFlexDirection(box.style):
            (IsInlineLevel(display)&&!HasInFlowBlockChildren(box))||display==L"table-row";int visible=0;
        if(horizontal){
            float lineWidth=0;
            for(auto& c:box.children)if(c->visible&&!c->style.Is(L"position",L"absolute")&&!c->style.Is(L"position",L"fixed")){
                if(c->node->tag==L"br"){value=std::max(value,lineWidth);lineWidth=0;continue;}
                lineWidth+=NaturalWidth(*c);++visible;
            }
            value=std::max(value,lineWidth);
        }else if(flex){
            // Column flex items form separate tracks even when their authored
            // display is inline. Their intrinsic widths contribute by maximum.
            for(const auto& child:box.children)if(child->visible&&
                !child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")){
                value=std::max(value,NaturalWidth(*child));++visible;
            }
        }else{
            // The max-content width of normal block flow is the widest line,
            // not the widest individual inline child. Adjacent inline boxes
            // therefore contribute together until a block or <br> ends the
            // line. This also gives shrink-to-fit flex/grid items enough room
            // to keep an authored inline run on one line.
            float lineWidth=0;
            auto finishLine=[&]{value=std::max(value,lineWidth);lineWidth=0;};
            for(auto& c:box.children){
                if(!c->visible||c->style.Is(L"position",L"absolute")||
                   c->style.Is(L"position",L"fixed"))continue;
                ++visible;
                if(c->node->tag==L"br"){finishLine();continue;}
                if(IsInlineLevel(c->style.Get(L"display")))lineWidth+=NaturalWidth(*c);
                else{finishLine();value=std::max(value,NaturalWidth(*c));}
            }
            finishLine();
        }
        if(flex&&horizontal)value+=GapValue(box.style,true,500,500)*std::max(0,visible-1);
        auto padding=EdgeValues(box.style,L"padding",500,500);auto border=UsedBorderValues(box);value+=padding.left+padding.right+border.left+border.right;
    }
    if(ignoreOwnSizing){
        // These replaced controls/elements measure their intrinsic content directly.
        const auto tag=box.node->tag;
        const auto type=ToLower(box.node->Attribute(L"type"));
        if((tag==L"input"&&type!=L"button"&&type!=L"submit"&&type!=L"reset")||
           tag==L"select"||tag==L"canvas"||tag==L"iframe"||tag==L"embed"||
           (tag==L"object"&&(box.node->attributes.count(L"data")||box.node->attributes.count(L"type")))||
           tag==L"video"||tag==L"img"||tag==L"svg"){
            const auto padding=EdgeValues(box.style,L"padding",500,500),border=UsedBorderValues(box);
            value+=padding.left+padding.right+border.left+border.right;
        }
    }else if(!definiteWidth)value=ConstrainIntrinsicWidth(box.style,value,500);
    auto margin=EdgeValues(box.style,L"margin",500,500);return remember(value+margin.left+margin.right);
}

const std::vector<float>& ResolveTableColumns(const LayoutBox& table,const TableGridModel& model,
                                       float availableWidth,float viewportWidth){
    if(model.columnsValid&&model.availableWidth==availableWidth&&
       (!model.columnsUseViewport||model.viewportWidth==viewportWidth))
        return model.usedColumns;
    model.availableWidth=availableWidth;model.viewportWidth=viewportWidth;model.columnsValid=true;
    model.columnsUseViewport=false;
    // Intrinsic and final layout can use different viewport references but
    // still resolve identical tracks. Row heights depend on those tracks,
    // not on the resolver's key. Preserve them until actual widths change.
    const auto rememberColumns=[&](std::vector<float>&& widths)->const std::vector<float>&{
        if(model.usedColumns!=widths)model.rowsValid=false;
        model.usedColumns=std::move(widths);
        return model.usedColumns;
    };
    if(!model.columnCount)return rememberColumns({});
    const auto spacing=TableBorderSpacing(table);
    availableWidth=std::max(0.0f,availableWidth-spacing.x*(model.columnCount+1));
    const auto useViewport=[&](const std::wstring& raw){
        // Computed CSS lengths normally have viewport units resolved already.
        // Retain the reference for any remaining raw unit, including HTML
        // width hints. Styles and this grid are invalidated on relayout.
        model.columnsUseViewport|=raw.find_first_of(L"vV")!=std::wstring::npos;
    };
    const auto cellWidth=[&](const LayoutBox& cell,const std::wstring& raw){
        useViewport(raw);
        const float width=StyleSheet::Length(raw,availableWidth,viewportWidth,0,FontSize(cell.style));
        const auto padding=EdgeValues(cell.style,L"padding",availableWidth,viewportWidth);
        model.columnsUseViewport|=StyleMetrics(cell.style).edgesUseViewport[1];
        const auto border=UsedBorderValues(cell);
        const float decoration=padding.left+padding.right+border.left+border.right;
        return cell.style.Is(L"box-sizing",L"border-box")?
            std::max(width,decoration):std::max(0.0f,width)+decoration;
    };
    const auto tableWidth=Trim(table.style.Get(L"width"));
    const bool fixed=table.style.Is(L"table-layout",L"fixed")&&!tableWidth.empty()&&tableWidth!=L"auto";
    if(fixed){
        std::vector<float> widths(model.columnCount),percentages(model.columnCount);
        std::vector<unsigned char> specified(model.columnCount);
        for(size_t i=0;i<model.columns.size();++i){
            // A populated colgroup does not supply a fixed width to its auto
            // columns. Empty colgroups are represented as repeated columns.
            const auto* column=model.columns[i].box;
            const auto raw=Trim(column->style.Get(L"width"));
            if(raw.empty()||raw==L"auto")continue;
            useViewport(raw);
            specified[i]=1;
            widths[i]=std::max(0.0f,StyleSheet::Length(raw,availableWidth,viewportWidth,0,FontSize(column->style)));
            if(raw.find(L'%')!=std::wstring::npos)percentages[i]=widths[i];
        }
        // Only first-row cells can reserve unspecified tracks. In particular,
        // an explicit zero column is not automatic, and a cell cannot enlarge
        // an already specified col. No full-vector copy is needed per cell.
        for(const auto& cell:model.cells){
            if(cell.row!=0)break;
            auto raw=Trim(cell.box->style.Get(L"width"));
            if((raw.empty()||raw==L"auto")&&cell.box->node)
                raw=Trim(cell.box->node->Attribute(L"width"));
            if(raw.empty()||raw==L"auto")continue;
            const size_t end=std::min(model.columnCount,cell.column+cell.columnSpan);
            if(end<=cell.column)continue;
            const float required=cellWidth(*cell.box,raw);
            const float share=std::max(0.0f,required-spacing.x*(end-cell.column-1))/static_cast<float>(end-cell.column);
            for(size_t i=cell.column;i<end;++i)if(!specified[i]){
                specified[i]=1;widths[i]=share;
                if(raw.find(L'%')!=std::wstring::npos)percentages[i]=share;
            }
        }
        float assigned=std::accumulate(widths.begin(),widths.end(),0.0f);
        const auto automatic=std::count(specified.begin(),specified.end(),0);
        if(assigned>availableWidth){
            const float percentage=std::accumulate(percentages.begin(),percentages.end(),0.0f);
            const float absolute=assigned-percentage;
            if(percentage>0&&absolute<availableWidth){
                const float ratio=(availableWidth-absolute)/percentage;
                for(size_t i=0;i<widths.size();++i)
                    widths[i]=widths[i]-percentages[i]+percentages[i]*ratio;
                assigned=availableWidth;
            }
        }
        if(automatic){
            const float share=std::max(0.0f,availableWidth-assigned)/static_cast<float>(automatic);
            for(size_t i=0;i<widths.size();++i)if(!specified[i])widths[i]=share;
        }else if(assigned<availableWidth){
            const float percentage=std::accumulate(percentages.begin(),percentages.end(),0.0f);
            const float absolute=assigned-percentage;
            if(absolute>0){
                // Keep authored percentage tracks at their resolved widths;
                // specified absolute tracks share the surplus proportionally.
                const float ratio=(availableWidth-percentage)/absolute;
                for(size_t i=0;i<widths.size();++i)if(percentages[i]==0)widths[i]*=ratio;
            }else if(assigned>0){
                for(auto& width:widths)width*=availableWidth/assigned;
            }else{
                const float share=availableWidth/static_cast<float>(widths.size());
                std::fill(widths.begin(),widths.end(),share);
            }
        }
        if(!widths.empty()){
            const float units=64.0f*std::max(0.01f,table.style.deviceScale);
            const float total=std::max(availableWidth,std::accumulate(widths.begin(),widths.end(),0.0f));
            float consumed=0;
            for(size_t i=0;i+1<widths.size();++i){
                widths[i]=std::floor(widths[i]*units+0.0001f)/units;consumed+=widths[i];
            }
            widths.back()=std::max(0.0f,total-consumed);
        }
        return rememberColumns(std::move(widths));
    }
    MeasureAutoTableColumns(table,model);
    const auto& minimum=model.minimumColumns;const auto& preferred=model.preferredColumns;
    const auto& percentage=model.percentageColumns;const auto& constrained=model.constrainedColumns;
    std::vector<float> result=minimum;
    availableWidth=std::max(availableWidth,std::accumulate(minimum.begin(),minimum.end(),0.0f));
    float previousSum=std::accumulate(result.begin(),result.end(),0.0f);
    // Move between the four increasing guesses: content minimum, percentage
    // minimum, specified minimum and maximum content. Equal sizing types move
    // together instead of letting the last track absorb an earlier overflow.
    for(int guess=1;guess<=3;++guess){
        std::vector<float> next=minimum;
        for(size_t i=0;i<next.size();++i){
            if(percentage[i]>0)next[i]=std::max(minimum[i],percentage[i]*availableWidth);
            else if(guess==3||(guess==2&&constrained[i]))next[i]=preferred[i];
        }
        const float nextSum=std::accumulate(next.begin(),next.end(),0.0f);
        if(nextSum>=availableWidth&&nextSum>previousSum){
            const float fraction=std::clamp((availableWidth-previousSum)/(nextSum-previousSum),0.0f,1.0f);
            for(size_t i=0;i<result.size();++i)result[i]+=(next[i]-result[i])*fraction;
            previousSum=availableWidth;break;
        }
        result=std::move(next);previousSum=nextSum;
    }
    if(previousSum<availableWidth){
        // Prefer unconstrained non-percentage columns, then specified lengths,
        // then percentages. Empty columns share space only if no other track
        // can grow. This also applies to an all-empty CSS table grid.
        std::vector<size_t> eligible;float weight=0;int rule=0;
        for(;rule<6&&eligible.empty();++rule){
            for(size_t i=0;i<result.size();++i){
                const bool origin=model.originatingColumns[i]!=0;
                const bool accept=rule==0?(!constrained[i]&&percentage[i]==0&&origin&&preferred[i]>0):
                    rule==1?(!constrained[i]&&percentage[i]==0&&origin):
                    rule==2?(constrained[i]&&percentage[i]==0&&preferred[i]>0):
                    rule==3?(percentage[i]>0):rule==4?origin:true;
                if(accept)eligible.push_back(i);
            }
        }
        --rule;
        for(const auto i:eligible)weight+=rule==0||rule==2?preferred[i]:rule==3?percentage[i]:1;
        const float excess=availableWidth-previousSum;
        for(const auto i:eligible)result[i]+=excess*(rule==0||rule==2?preferred[i]:rule==3?percentage[i]:1)/weight;
    }
    if(!result.empty()){
        const float units=64.0f*std::max(0.01f,table.style.deviceScale);
        float consumed=0;
        for(size_t i=0;i+1<result.size();++i){
            result[i]=std::floor(result[i]*units+0.0001f)/units;consumed+=result[i];
        }
        result.back()=std::max(0.0f,availableWidth-consumed);
    }
    return rememberColumns(std::move(result));
}

float FixedTableMinimumWidth(const LayoutBox& box,float width,float viewportWidth){
    const auto& model=BuildTableGrid(*const_cast<LayoutBox*>(&box));
    const auto& columns=ResolveTableColumns(box,model,width,viewportWidth);
    return std::accumulate(columns.begin(),columns.end(),0.0f)+
        TableBorderSpacing(box).x*std::max<size_t>(2,columns.size()+1);
}

float BreakableTextMinimum(const std::wstring& text,const ComputedStyle& style){
    // Shape before measuring so combining marks and surrogate pairs are never
    // split into individual UTF-16 units. Spacing and fallback are the same as
    // those used for the painted text.
    std::wstring key=L"\x1eMIN"+text+L'\x1f'+FontFamily(style)+L'\x1f'+NumberText(FontSize(style));
    key+=L'\x1f'+std::to_wstring(FontWeight(style))+L'\x1f'+style.Get(L"font-style");
    key+=L'\x1f'+style.Get(L"letter-spacing")+L'\x1f'+style.Get(L"word-spacing");
    key+=L'\x1f'+style.Get(L"font-kerning",L"auto")+L'\x1f'+NumberText(style.deviceScale);
    auto& cache=ThreadTextWidthCache();float minimum=0;
    if(cache.Find(key,minimum))return minimum;
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if(format&&SUCCEEDED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
                format.Get(),100000.0f,100000.0f,&layout))){
            ApplyFontFallback(factory,layout.Get(),text,style);
            ApplyCharacterSpacing(layout.Get(),text,style);
            UINT32 count=0;layout->GetClusterMetrics(nullptr,0,&count);
            std::vector<DWRITE_CLUSTER_METRICS> clusters(count);
            if(count&&SUCCEEDED(layout->GetClusterMetrics(clusters.data(),count,&count)))
                for(const auto& cluster:clusters)if(!cluster.isNewline&&!cluster.isWhitespace)
                    minimum=std::max(minimum,cluster.width);
        }
    }
    if(minimum<=0&&!text.empty())minimum=TextWidth(text,style);
    const float units=64.0f*std::max(0.01f,style.deviceScale);
    minimum=std::ceil(minimum*units)/units;
    cache.Remember(std::move(key),minimum);return minimum;
}

float WrappableTextMinimum(const std::wstring& text,const ComputedStyle& style){
    // A word is not necessarily delimited by ASCII spaces: Unicode shaping
    // supplies breaks at ideographs, punctuation and explicit break controls.
    // Keep those opportunities on cluster boundaries, preserving NBSP and
    // combining characters, and measure the glyphs that remain on each line.
    std::wstring key=L"\x1eWRAPMIN"+text+L'\x1f'+FontFamily(style)+L'\x1f'+NumberText(FontSize(style));
    for(const auto* property:{L"font-family",L"font-weight",L"font-style",L"font-kerning",L"letter-spacing",
            L"word-spacing",L"tab-size",L"direction",L"white-space",L"word-break",L"hyphens"})
        key+=L'\x1f'+style.Get(property);
    key+=L'\x1f'+NumberText(style.deviceScale);
    auto& cache=ThreadTextWidthCache();float cached=0;
    if(cache.Find(key,cached))return cached;
    const auto remember=[&](float value){cache.Remember(std::move(key),value);return value;};
    const auto cjkLetter=[](wchar_t c){
        return IsHangul(c)||(c>=0x3041&&c<=0x30fa)||(c>=0x3105&&c<=0x312f)||
            (c>=0x31a0&&c<=0x31bf)||(c>=0x3400&&c<=0x9fff)||(c>=0xf900&&c<=0xfaff);
    };
    if(auto* factory=SharedWriteFactory()){
        auto format=TextFormat(factory,style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if(format&&SUCCEEDED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
                format.Get(),100000.0f,100000.0f,&layout))){
            ApplyFontFallback(factory,layout.Get(),text,style);
            ApplyCharacterSpacing(layout.Get(),text,style);
            ApplyTabSize(factory,layout.Get(),format.Get(),text,style);
            UINT32 count=0;layout->GetClusterMetrics(nullptr,0,&count);
            std::vector<DWRITE_CLUSTER_METRICS> clusters(count);
            if(count&&SUCCEEDED(layout->GetClusterMetrics(clusters.data(),count,&count))){
                float minimum=0;size_t start=0,offset=0;
                const bool keepAll=style.Is(L"word-break",L"keep-all");
                const bool hyphensNone=style.Is(L"hyphens",L"none");
                const auto measure=[&](size_t end,bool softHyphen){
                    auto part=text.substr(start,end-start);
                    while(!part.empty()&&IsCollapsibleTextSpace(part.back()))part.pop_back();
                    if(softHyphen&&!part.empty()&&part.back()==L'\x00ad')part.back()=L'-';
                    if(!part.empty())minimum=std::max(minimum,TextWidth(part,style));
                };
                for(const auto& cluster:clusters){
                    offset+=cluster.length;
                    bool breakAfter=cluster.canWrapLineAfter||cluster.isNewline;
                    if(cluster.isSoftHyphen&&hyphensNone)breakAfter=false;
                    // CSS normal keeps a solidus inside a Latin word together;
                    // DirectWrite's default Unicode tailoring permits a break.
                    if(offset<text.size()&&text[offset-1]==L'/'&&offset>1&&
                       ((text[offset-2]>=L'A'&&text[offset-2]<=L'Z')||(text[offset-2]>=L'a'&&text[offset-2]<=L'z'))&&
                       ((text[offset]>=L'A'&&text[offset]<=L'Z')||(text[offset]>=L'a'&&text[offset]<=L'z')))
                        breakAfter=false;
                    if(keepAll&&offset<text.size()&&
                       (cjkLetter(text[offset-cluster.length])||cjkLetter(text[offset]))){
                        WORD left=0,right=0;
                        GetStringTypeW(CT_CTYPE1,&text[offset-cluster.length],1,&left);
                        GetStringTypeW(CT_CTYPE1,&text[offset],1,&right);
                        if((left&C1_ALPHA)&&(right&C1_ALPHA))breakAfter=false;
                    }
                    if(breakAfter){measure(offset,cluster.isSoftHyphen);start=offset;}
                }
                if(start<text.size())measure(text.size(),false);
                return remember(minimum);
            }
        }
    }
    float longest=1;
    for(const auto& word:Words(text))longest=std::max(longest,TextWidth(word,style));
    return remember(longest);
}

bool CanFlattenInlineFlow(const LayoutBox& box,float availableWidth){
    // Emergency and character wrapping need different widths on the first
    // and following lines. Keep their existing inline formatting context
    // until the flattened flow can fragment a shaped run at cluster breaks.
    const auto wrapping=TextWrappingMode(box.style);
    if(wrapping==DWRITE_WORD_WRAPPING_CHARACTER||wrapping==DWRITE_WORD_WRAPPING_EMERGENCY_BREAK){
        if(availableWidth<=0)return false;
        for(const auto& child:box.children)if(child->node->type==NodeType::Text){
            const auto& text=child->node->text;size_t start=0;
            while(start<text.size()){
                while(start<text.size()&&IsCollapsibleTextSpace(text[start]))++start;
                size_t end=start;while(end<text.size()&&!IsCollapsibleTextSpace(text[end]))++end;
                if(TextWidth(text.substr(start,end-start),child->style)>availableWidth)return false;
                start=end;
            }
        }
    }
    if(!box.visible||box.node->type!=NodeType::Element||!box.style.Is(L"display",L"inline")||
       IsAtomicInlineLevel(box)||IsBlockifiedItem(box)||HasInFlowBlockChildren(box)||box.children.empty()||
       box.style.Is(L"direction",L"rtl")||box.style.Is(L"position",L"relative")||
       box.style.Is(L"position",L"absolute")||box.style.Is(L"position",L"fixed")||
       UsedFloatSide(box.style)!=FloatSide::None||
       box.style.Get(L"transform",L"none")!=L"none"||
       box.style.Get(L"vertical-align",L"baseline")!=L"baseline"||
       box.style.Get(L"opacity",L"1")!=L"1"||
       OverflowX(box)!=L"visible"||OverflowY(box)!=L"visible"||
       PreservesLineBreaks(box.style.Get(L"white-space"))||
       PreventsTextWrapping(box.style.Get(L"white-space")))return false;
    const auto padding=EdgeValues(box.style,L"padding",500,500);
    const auto border=UsedBorderValues(box),margin=EdgeValues(box.style,L"margin",500,500);
    return padding.left==0&&padding.right==0&&padding.top==0&&padding.bottom==0&&
        border.left==0&&border.right==0&&border.top==0&&border.bottom==0&&
        margin.left==0&&margin.right==0&&margin.top==0&&margin.bottom==0&&
        std::none_of(box.children.begin(),box.children.end(),[](const auto& child){
            return child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed")||
                UsedFloatSide(child->style)!=FloatSide::None;
        });
}

std::vector<LayoutBox*> InlineFlowChildren(const LayoutBox& box,
                                         std::vector<LayoutBox*>* flattened=nullptr,float availableWidth=0){
    std::vector<LayoutBox*> result;
    const auto append=[&](auto&& self,LayoutBox& child)->void{
        if(!box.style.Is(L"direction",L"rtl")&&CanFlattenInlineFlow(child,availableWidth)){
            if(flattened)flattened->push_back(&child);
            for(const auto& descendant:child.children)self(self,*descendant);
        }else result.push_back(&child);
    };
    // Keep an inline group together when one of its non-atomic elements
    // retains its own formatting context. Flattening only its neighbours
    // loses attached word boundaries and can create spurious overflow.
    for(size_t begin=0;begin<box.children.size();){
        size_t end=begin;
        while(end<box.children.size()&&IsInlineLevel(box.children[end]->style.Get(L"display")))++end;
        if(end==begin){result.push_back(box.children[begin].get());++begin;continue;}
        bool safe=true,clusterWrapping=false;
        for(size_t index=begin;index<end;++index){
            const auto& child=*box.children[index];
            const auto wrapping=TextWrappingMode(child.style);
            clusterWrapping=clusterWrapping||wrapping==DWRITE_WORD_WRAPPING_CHARACTER||
                wrapping==DWRITE_WORD_WRAPPING_EMERGENCY_BREAK;
            if(child.visible&&child.node->type==NodeType::Element&&!IsAtomicInlineLevel(child)&&
               child.node->tag!=L"br"&&!CanFlattenInlineFlow(child,availableWidth))safe=false;
        }
        for(size_t index=begin;index<end;++index){
            if(safe||!clusterWrapping)append(append,*box.children[index]);else result.push_back(box.children[index].get());
        }
        begin=end;
    }
    return result;
}

bool InlineBreakBefore(const std::vector<LayoutBox*>& children,size_t index){
    const auto& child=*children[index];
    if(child.node->type!=NodeType::Text)return true;
    if(!child.node->text.empty()&&IsCollapsibleTextSpace(child.node->text.front()))return true;
    if(!index)return true;
    const auto& previous=*children[index-1];
    return IsAtomicInlineLevel(previous)||previous.node->tag==L"br"||
        (previous.node->type==NodeType::Text&&!previous.node->text.empty()&&
         IsCollapsibleTextSpace(previous.node->text.back()));
}

float InlineAttachedTailWidth(const std::vector<LayoutBox*>& children,size_t index){
    float width=0;
    for(size_t next=index+1;next<children.size();++next){
        const auto& previous=*children[next-1];const auto& child=*children[next];
        if(previous.node->type!=NodeType::Text||previous.node->text.empty()||
           IsCollapsibleTextSpace(previous.node->text.back())||
           child.node->type!=NodeType::Text||child.node->text.empty()||
           IsCollapsibleTextSpace(child.node->text.front())||!child.visible)break;
        width+=NaturalWidth(child);
    }
    return width;
}

float TextLastLineAdvance(const LayoutBox& box,float width,bool trimLeading){
    const auto text=NormalizeText(box.node->text,box.style.Get(L"white-space"),
        box.preserveLeadingWhitespace&&!trimLeading,box.preserveTrailingWhitespace);
    auto* factory=SharedWriteFactory();if(!factory||text.empty())return 0;
    auto format=TextFormat(factory,box.style);Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if(!format||FAILED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
        format.Get(),std::max(1.0f,width),100000,&layout)))return width;
    ApplyFontFallback(factory,layout.Get(),text,box.style);
    ApplyCharacterSpacing(layout.Get(),text,box.style);
    ApplyTabSize(factory,layout.Get(),format.Get(),text,box.style);
    layout->SetWordWrapping(TextWrappingMode(box.style));
    float x=0,y=0;DWRITE_HIT_TEST_METRICS hit{};
    if(FAILED(layout->HitTestTextPosition(static_cast<UINT32>(text.size()-1),TRUE,&x,&y,&hit)))return width;
    const float units=64*std::max(.01f,box.style.deviceScale);
    return std::ceil(x*units)/units;
}

float MinContentWidth(const LayoutBox& box,bool intrinsic,bool ignoreOwnSizing){
    if(ignoreOwnSizing&&box.minContentWidthValid)return box.minContentWidth;
    if(!ignoreOwnSizing&&(intrinsic?box.intrinsicMinimumWidthValid:box.minimumWidthValid))
        return intrinsic?box.intrinsicMinimumWidth:box.minimumWidth;
    auto remember=[&](float value){
        if(ignoreOwnSizing){box.minContentWidth=value;box.minContentWidthValid=true;}
        else if(intrinsic){box.intrinsicMinimumWidth=value;box.intrinsicMinimumWidthValid=true;}
        else{box.minimumWidth=value;box.minimumWidthValid=true;}
        return value;
    };
    const auto margin=EdgeValues(box.style,L"margin",500,500);
    const auto overflow=box.style.Get(L"overflow-x",box.style.Get(L"overflow",L"visible"));
    const auto explicitMinimum=Trim(box.style.Get(L"min-width"));
    if(!intrinsic&&!explicitMinimum.empty()&&explicitMinimum!=L"auto"&&
       (!IsIntrinsicWidth(explicitMinimum)||HasIntrinsicWidth(box))){
        if(IsIntrinsicWidth(explicitMinimum))return remember(
            UsedIntrinsicWidth(box,L"0",500,500,true)+margin.left+margin.right);
        const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=UsedBorderValues(box);
        const float decoration=box.style.Is(L"box-sizing",L"border-box")?0.0f:padding.left+padding.right+border.left+border.right;
        return remember(std::max(0.0f,StyleSheet::Length(explicitMinimum,500,500,0)+decoration+margin.left+margin.right));
    }
    if(!intrinsic&&overflow!=L"visible"&&overflow!=L"clip")return remember(margin.left+margin.right);
    if(box.node->type==NodeType::Text){
        const auto whiteSpace=box.style.Get(L"white-space");
        if(whiteSpace==L"pre")return remember(NaturalWidth(box));
        const auto text=NormalizeText(box.node->text,whiteSpace,box.preserveLeadingWhitespace&&!box.trimLeadingLineWhitespace,box.preserveTrailingWhitespace);
        if(PreventsTextWrapping(whiteSpace))return remember(TextWidth(text,box.style));
        if(box.style.Is(L"overflow-wrap",L"anywhere")||box.style.Is(L"word-break",L"break-all")||
           box.style.Is(L"word-break",L"break-word"))return remember(BreakableTextMinimum(text,box.style));
        return remember(WrappableTextMinimum(text,box.style));
    }
    if(box.node->tag==L"br")return remember(0.0f);
    if(intrinsic&&!ignoreOwnSizing&&HasIntrinsicWidth(box)){
        const auto width=ToLower(Trim(box.style.Get(L"width")));
        const bool preferred=width==L"max-content"||(!width.empty()&&width!=L"auto"&&
            !IsIntrinsicWidth(width)&&width.find(L'%')==std::wstring::npos);
        return remember(preferred?NaturalWidth(box):
            UsedIntrinsicWidth(box,L"min-content",500,500,true)+margin.left+margin.right);
    }
    if(intrinsic&&!ignoreOwnSizing){
        // A descendant with a definite preferred inline size contributes that
        // size to the table cell, even when its contents are empty. The cell's
        // own width hint is handled separately by TableCellContentWidth.
        const auto width=ToLower(Trim(box.style.Get(L"width")));
        if(!width.empty()&&width!=L"auto"&&width!=L"min-content"&&width!=L"max-content"&&
           width!=L"fit-content"&&width!=L"stretch"&&width.find(L'%')==std::wstring::npos)
        {
            float value=NaturalWidth(box);
            if(IsTable(box)){
                const auto padding=EdgeValues(box.style,L"padding",0,0),border=UsedBorderValues(box);
                const float edges=padding.left+padding.right+border.left+border.right;
                const float gridMinimum=box.style.Is(L"table-layout",L"fixed")?
                    FixedTableMinimumWidth(box,std::max(0.0f,value-edges-margin.left-margin.right),500):TableIntrinsicWidth(box,true);
                value=std::max(value,gridMinimum+edges+margin.left+margin.right);
            }
            return remember(value);
        }
    }
    if(box.node->tag==L"input"||box.node->tag==L"select"||box.node->tag==L"button"||
       box.node->tag==L"img"||box.node->tag==L"iframe"||box.node->tag==L"embed"||
       (box.node->tag==L"object"&&(box.node->attributes.count(L"data")||box.node->attributes.count(L"type")))||box.node->tag==L"video"||box.node->tag==L"canvas"||box.node->tag==L"svg")return remember(NaturalWidth(box,ignoreOwnSizing));
    const auto display=box.style.Get(L"display");const bool flex=display==L"flex"||display==L"inline-flex";
    if(intrinsic&&(display==L"grid"||display==L"inline-grid")){
        const auto padding=EdgeValues(box.style,L"padding",0,0),border=UsedBorderValues(box);
        const float edges=padding.left+padding.right+border.left+border.right;
        if(ignoreOwnSizing){
            const auto rawPadding=EdgeValues(box.style,L"padding",500,500);
            return remember(NaturalGridWidth(box,true)+rawPadding.left+rawPadding.right+
                border.left+border.right+margin.left+margin.right);
        }
        return remember(ConstrainIntrinsicWidth(box.style,NaturalGridWidth(box,true)+
            (box.style.Is(L"box-sizing",L"border-box")?edges:0),500)+
            (box.style.Is(L"box-sizing",L"border-box")?0:edges)+margin.left+margin.right);
    }
    if(IsTable(box)){
        const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=UsedBorderValues(box);
        return remember(TableIntrinsicWidth(box,true)+padding.left+padding.right+border.left+border.right+margin.left+margin.right);
    }
    const bool horizontal=flex?!IsColumnFlexDirection(box.style):IsInlineLevel(display)||display==L"table-row";
    const auto flexWrap=ToLower(Trim(box.style.Get(L"flex-wrap",L"nowrap")));
    const bool wraps=flex&&horizontal&&(flexWrap==L"wrap"||flexWrap==L"wrap-reverse");
    float value=0;int visible=0;
    float unbrokenWidth=0;
    const auto flowChildren=InlineFlowChildren(box);
    for(size_t index=0;index<flowChildren.size();++index){
        const auto* child=flowChildren[index];
        if(!child->visible||child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))continue;
        const float childWidth=MinContentWidth(*child,intrinsic);
        if(flex||display==L"table-row")value=horizontal&&!wraps?value+childWidth:std::max(value,childWidth);
        else if(IsInlineLevel(child->style.Get(L"display"))){
            if(InlineBreakBefore(flowChildren,index)){value=std::max(value,unbrokenWidth);unbrokenWidth=0;}
            unbrokenWidth+=childWidth;
            if(IsAtomicInlineLevel(*child)){value=std::max(value,unbrokenWidth);unbrokenWidth=0;}
        }else{value=std::max(value,std::max(unbrokenWidth,childWidth));unbrokenWidth=0;}
        ++visible;
    }
    value=std::max(value,unbrokenWidth);
    if(flex&&horizontal&&!wraps)value+=GapValue(box.style,true,500,500)*std::max(0,visible-1);
    const auto padding=EdgeValues(box.style,L"padding",500,500);const auto border=UsedBorderValues(box);
    value+=padding.left+padding.right+border.left+border.right+margin.left+margin.right;
    if(!ignoreOwnSizing&&intrinsic&&!explicitMinimum.empty()&&explicitMinimum!=L"auto"&&explicitMinimum.find(L'%')==std::wstring::npos){
        const float decoration=box.style.Is(L"box-sizing",L"border-box")?0:
            padding.left+padding.right+border.left+border.right;
        value=std::max(value,StyleSheet::Length(explicitMinimum,0,0,0)+decoration+margin.left+margin.right);
    }
    const auto maximum=Trim(box.style.Get(L"max-width"));
    if(!ignoreOwnSizing&&!maximum.empty()&&maximum!=L"none"&&maximum!=L"auto"&&(!intrinsic||maximum.find(L'%')==std::wstring::npos))
        value=std::min(value,StyleSheet::Length(maximum,500,500,value));
    return remember(std::max(0.0f,value));
}

float FloatOuterWidth(const LayoutBox& box,float availableWidth,float viewportWidth){
    const auto width=ToLower(Trim(box.style.Get(L"width")));
    if(!width.empty()&&width!=L"auto")
        return BlockOuterWidth(box,availableWidth,viewportWidth);

    // CSS 2.1 floats with an automatic inline size are shrink-to-fit.  In
    // particular, an inline <span> that becomes a float must not inherit the
    // fill-available width used by an ordinary block.  All three operands are
    // outer widths here (including decorations and margins), which keeps the
    // result suitable for PlaceFloat and stable in CSS DIPs at every DPI.
    const float preferredMinimum=MinContentWidth(box);
    const float preferred=std::max(preferredMinimum,NaturalWidth(box));
    return std::min(std::max(preferredMinimum,availableWidth),preferred);
}

float NaturalGridHeight(const LayoutBox& box,float availableWidth);
bool CanLayoutColumns(const LayoutBox& box);
float NaturalColumnsHeight(const LayoutBox& box,float contentWidth,float viewportWidth);

float NaturalHeight(const LayoutBox& box,float availableWidth=500){
    if(box.naturalHeightValid&&std::abs(box.naturalHeightReference-availableWidth)<0.01f)return box.naturalHeight;
    box.naturalFloatBottom=0;
    auto remember=[&](float result){box.naturalHeightReference=availableWidth;box.naturalHeight=result;box.naturalHeightValid=true;return result;};
    // Ordinary text inherits font/line formatting, not element box edges or
    // size constraints. Match LayoutBoxTree's text path during intrinsic sizing.
    if(box.node->type==NodeType::Text&&
       (!box.generatedFrom||box.generatedFrom->type==NodeType::Text))
        return remember(TextHeight(box.node->text,box.style,availableWidth,
            box.preserveLeadingWhitespace&&!box.trimLeadingLineWhitespace,box.preserveTrailingWhitespace));
    auto height=box.style.Get(L"height");float value=0;
    if(box.style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(box)&&!IsBlockifiedItem(box))height=L"auto";
    float cellMinimum=0,tableCaptionExtent=0;
    const bool table=IsTable(box);
    if((box.style.Is(L"display",L"table-cell")||table)&&!height.empty()&&height!=L"auto"&&
       height.find(L'%')==std::wstring::npos){
        const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
        const auto border=UsedBorderValues(box);
        const float decoration=padding.top+padding.bottom+border.top+border.bottom;
        cellMinimum=StyleSheet::Length(height,500,500,0);
        cellMinimum=box.style.Is(L"box-sizing",L"border-box")?
            std::max(cellMinimum,decoration):cellMinimum+decoration;
        if(table){
            // A specified table height is a grid minimum, subject to max-height.
            // Content can still make the grid taller than that constrained hint.
            const auto maximum=box.style.Get(L"max-height");
            if(!maximum.empty()&&maximum!=L"none"&&maximum!=L"auto"&&maximum.find(L'%')==std::wstring::npos){
                float limit=StyleSheet::Length(maximum,500,500,cellMinimum);
                if(!box.style.Is(L"box-sizing",L"border-box"))limit+=decoration;
                cellMinimum=std::min(cellMinimum,limit);
            }
        }
        // Cell height is a minimum. Measure wrapped content at the resolved
        // column width through the existing intrinsic cache.
        height=L"auto";
    }
    if(!height.empty()&&height!=L"auto"&&height.find(L'%')==std::wstring::npos){
        value=StyleSheet::Length(height,500,500,20);
        if(!box.style.Is(L"box-sizing",L"border-box")){
            const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
            const auto border=UsedBorderValues(box);
            value+=padding.top+padding.bottom+border.top+border.bottom;
        }
    }
    else if(box.node->tag==L"br")value=LineHeight(box.style);
    else if(box.node->tag==L"input"&&(box.node->Attribute(L"type")==L"checkbox"||box.node->Attribute(L"type")==L"radio"))value=13;
    else if(box.node->tag==L"textarea"){
        auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
        auto border=UsedBorderValues(box);size_t rows=2;
        const auto rawRows=Trim(box.node->Attribute(L"rows"));
        unsigned long long parsedRows=0;size_t usedRows=0;
        if(TryParseUnsignedInteger(rawRows,parsedRows,&usedRows)&&usedRows==rawRows.size()&&
           parsedRows<=(std::numeric_limits<size_t>::max)())
            rows=std::max<size_t>(1,static_cast<size_t>(parsedRows));
        value=LineHeight(box.style)*rows+padding.top+padding.bottom+border.top+border.bottom;
    }
    else if(box.node->tag==L"input"||box.node->tag==L"select"||
            (box.node->tag==L"button"&&!HasInFlowBlockChildren(box)&&box.style.Get(L"display")!=L"grid"&&
             box.style.Get(L"display")!=L"inline-grid"&&
             box.style.Get(L"display")!=L"flex"&&box.style.Get(L"display")!=L"inline-flex")){
        auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);auto border=UsedBorderValues(box);
        const auto text=box.node->tag==L"button"?box.node->InnerText():(box.node->tag==L"select"?SelectedOptionText(box.node):box.node->Attribute(L"value"));
        float textHeight=ControlLineHeight(text,box.style);
        if(box.node->tag==L"select"){
            const auto appearance=box.style.Get(L"appearance",box.style.Get(L"-webkit-appearance",L"auto"));
            textHeight=InlineContentBoxHeight(box.style)+(appearance==L"none"?0.0f:2.0f/std::max(0.01f,box.style.deviceScale));
        }else if(box.node->tag==L"button"){
            const auto width=Trim(box.style.Get(L"width"));
            float borderWidth=availableWidth;
            const float horizontal=padding.left+padding.right+border.left+border.right;
            if(!width.empty()&&width!=L"auto"){
                borderWidth=StyleSheet::Length(width,availableWidth,availableWidth,availableWidth);
                if(!box.style.Is(L"box-sizing",L"border-box"))borderWidth+=horizontal;
            }
            textHeight=TextHeight(text,box.style,std::max(1.0f,borderWidth-horizontal));
        }
        value=textHeight+padding.top+padding.bottom+border.top+border.bottom;
    }
    else if(box.node->tag==L"canvas")
        value=StyleSheet::Length(box.node->Attribute(L"height"),availableWidth,availableWidth,150);
    else if(box.node->tag==L"iframe"||box.node->tag==L"embed"||
            (box.node->tag==L"object"&&(box.node->attributes.count(L"data")||box.node->attributes.count(L"type")))||box.node->tag==L"video")
        value=StyleSheet::Length(box.node->Attribute(L"height"),availableWidth,availableWidth,150);
    else if(box.node->tag==L"img"){
        const auto authoredHeight=Trim(box.node->Attribute(L"height"));
        if(!authoredHeight.empty())
            value=StyleSheet::Length(authoredHeight,availableWidth,availableWidth,0);
        else if(box.node->image&&box.node->image->width){
            const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
            const auto border=UsedBorderValues(box);
            const auto margin=EdgeValues(box.style,L"margin",availableWidth,availableWidth);
            const float contentWidth=std::max(0.0f,availableWidth-padding.left-padding.right-
                border.left-border.right-margin.left-margin.right);
            value=contentWidth*box.node->image->height/box.node->image->width;
            value+=padding.top+padding.bottom+border.top+border.bottom;
        }
    }
    else if(box.node->tag==L"svg"){
        const auto viewBox=SvgNumbers(box.node->Attribute(L"viewbox"));
        if(viewBox.size()==4&&viewBox[2]>0)value=availableWidth*viewBox[3]/viewBox[2];
        else value=StyleSheet::Length(box.node->Attribute(L"height"),availableWidth,availableWidth,150);
    }
    else if(box.node->type==NodeType::Text)value=TextHeight(box.node->text,box.style,availableWidth,
        box.preserveLeadingWhitespace&&!box.trimLeadingLineWhitespace,box.preserveTrailingWhitespace);
    else if(!box.children.empty()){
        const auto display=box.style.Get(L"display");const float rowGap=GapValue(box.style,false,availableWidth,availableWidth);
        auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);auto border=UsedBorderValues(box);const float innerWidth=std::max(1.0f,availableWidth-padding.left-padding.right-border.left-border.right);
        if(CanLayoutColumns(box)){
            value=NaturalColumnsHeight(box,innerWidth,availableWidth);
        }else if(IsTable(box)){
            auto& mutableBox=const_cast<LayoutBox&>(box);
            const auto& model=BuildTableGrid(mutableBox);
            const auto& columns=ResolveTableColumns(box,model,innerWidth,availableWidth);
            const auto spacing=TableBorderSpacing(box);
            const auto& rows=ResolveTableRows(model,columns,spacing);
            for(const auto rowHeight:rows)value+=rowHeight;
            value+=spacing.y*(rows.size()+1);
            for(const auto& child:box.children)if(child->visible&&child->style.Is(L"display",L"table-caption"))
                tableCaptionExtent+=NaturalHeight(*child,innerWidth);
            value+=tableCaptionExtent;
        }else if(display==L"grid"){
            value=NaturalGridHeight(box,innerWidth);
        }else{
            const bool flex=display==L"flex"||display==L"inline-flex";
            const bool row=(flex&&!IsColumnFlexDirection(box.style))||display==L"table-row";
            int visible=0;
            const auto flexWrap=ToLower(Trim(box.style.Get(L"flex-wrap",L"nowrap")));
            if(flex&&row&&(flexWrap==L"wrap"||flexWrap==L"wrap-reverse")){
                const float columnGap=GapValue(box.style,true,innerWidth,availableWidth);
                float lineWidth=0,lineHeight=0;size_t lineItems=0,lineCount=0;
                auto finishFlexLine=[&]{
                    if(!lineItems)return;
                    if(lineCount++)value+=rowGap;
                    value+=lineHeight;lineWidth=0;lineHeight=0;lineItems=0;
                };
                for(auto& child:box.children)if(child->visible&&
                    !child->style.Is(L"position",L"absolute")&&
                    !child->style.Is(L"position",L"fixed")){
                    const float childWidth=NaturalWidth(*child);
                    const float candidate=lineWidth+(lineItems?columnGap:0)+childWidth;
                    if(lineItems&&candidate>innerWidth+0.5f)finishFlexLine();
                    if(lineItems)lineWidth+=columnGap;
                    lineWidth+=childWidth;
                    lineHeight=std::max(lineHeight,NaturalHeight(*child,
                        std::max(1.0f,std::min(childWidth,innerWidth))));
                    ++lineItems;++visible;
                }
                finishFlexLine();
            }else if(row||flex){
                float lineHeight=0;
                for(auto& child:box.children)if(child->visible&&!child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")){
                    if(row&&child->node->tag==L"br"){
                        value+=lineHeight>0?lineHeight:LineHeight(child->style);lineHeight=0;continue;
                    }
                    if(row){
                        // An inline replaced element is measured from its own used
                        // width. Passing the entire line width can expand an auto-
                        // height icon to the width of its link or table cell.
                        const float childWidth=std::max(1.0f,flex&&HasIntrinsicWidth(*child)?
                            BlockOuterWidth(*child,innerWidth,availableWidth):
                            HasInFlowBlockChildren(*child)?innerWidth:std::min(NaturalWidth(*child),innerWidth));
                        lineHeight=std::max(lineHeight,NaturalHeight(*child,childWidth));
                    }
                    else value+=NaturalHeight(*child,HasIntrinsicWidth(*child)?
                        BlockOuterWidth(*child,innerWidth,availableWidth):innerWidth);
                    ++visible;
                }
                if(row)value+=lineHeight;
                if(flex&&!row)value+=rowGap*std::max(0,visible-1);
            }else{
                const bool hasFloats=std::any_of(box.children.begin(),box.children.end(),
                    [](const auto& child){return child->visible&&
                        UsedFloatSide(child->style)!=FloatSide::None;});
                if(hasFloats){
                    // Floats are removed from normal flow and share each
                    // available horizontal band. A block formatting context
                    // with auto height encloses their margin boxes.
                    std::vector<FloatArea> floats;
                    float cursor=0,lineWidth=0,lineHeight=0,lineStart=0,maxFloatBottom=0;
                    BlockMarginFlow marginFlow; marginFlow.parent=CollapsedBlockMargins(box,availableWidth);
                    auto flushLine=[&]{
                        cursor+=lineHeight;lineWidth=0;lineHeight=0;lineStart=0;
                    };
                    for(auto& child:box.children){
                        if(!child->visible||child->style.Is(L"position",L"absolute")||
                           child->style.Is(L"position",L"fixed"))continue;
                        const auto side=UsedFloatSide(child->style);
                        if(side!=FloatSide::None){
                            const float childWidth=FloatOuterWidth(*child,innerWidth,availableWidth);
                            const float childHeight=NaturalHeight(*child,std::max(1.0f,childWidth));
                            float left=0,right=innerWidth,nextBottom=0;
                            AvailableFloatBand(floats,0,innerWidth,cursor,left,right,nextBottom);
                            if(lineHeight>0&&(childWidth+lineWidth>right-left+0.01f||
                               ClearedFloatY(floats,cursor,child->style.Get(L"clear"))>cursor+0.01f))flushLine();
                            const auto placed=PlaceFloat(floats,0,innerWidth,cursor,childWidth,
                                childHeight,side,child->style.Get(L"clear"));
                            if(lineHeight>0&&side==FloatSide::Left)lineStart+=childWidth;
                            floats.push_back({placed,side});
                            maxFloatBottom=std::max(maxFloatBottom,placed.y+placed.height);
                            continue;
                        }
                        cursor=ClearedFloatY(floats,cursor,child->style.Get(L"clear"));
                        if(child->node->tag==L"br"){
                            marginFlow.Separate();
                            if(lineWidth>0||lineHeight>0)flushLine();
                            else cursor+=LineHeight(child->style);
                            continue;
                        }
                        if(IsInlineLevel(child->style.Get(L"display"))){
                            marginFlow.Separate();
                            float left=0,right=innerWidth,nextBottom=0;
                            AvailableFloatBand(floats,0,innerWidth,cursor,left,right,nextBottom);
                            const float childWidth=HasInFlowBlockChildren(*child)?innerWidth:NaturalWidth(*child);
                            if(lineWidth==0)lineStart=left;
                            if(lineStart+lineWidth+childWidth>right+0.5f){
                                if(lineWidth>0||lineHeight>0)flushLine();
                                else if(std::isfinite(nextBottom))cursor=nextBottom;
                                AvailableFloatBand(floats,0,innerWidth,cursor,left,right,nextBottom);
                                lineStart=left;
                            }
                            lineWidth+=std::min(childWidth,std::max(0.0f,right-lineStart));
                            const bool atomic=IsAtomicInlineLevel(*child);
                            lineHeight=std::max(LineHeight(box.style),std::max(lineHeight,
                                NaturalHeight(*child,std::max(1.0f,std::min(childWidth,
                                    std::max(1.0f,right-lineStart))))+
                                (atomic?InlineFormattingDescent(box.style):0.0f)));
                        }else{
                            if(lineWidth>0||lineHeight>0)flushLine();
                            const auto childMargins=CollapsedBlockMargins(*child,innerWidth);
                            cursor+=marginFlow.Before(childMargins,child->style.Get(L"clear",L"none")!=L"none");
                            float left=0,right=innerWidth,nextBottom=0;
                            AvailableFloatBand(floats,0,innerWidth,cursor,left,right,nextBottom);
                            if(right<=left+0.01f&&std::isfinite(nextBottom)){
                                cursor=nextBottom;
                                AvailableFloatBand(floats,0,innerWidth,cursor,left,right,nextBottom);
                            }
                            cursor+=NaturalHeight(*child,std::max(1.0f,
                                BlockOuterWidth(*child,right-left,availableWidth)))+marginFlow.After(childMargins);
                        }
                    }
                    if(lineWidth>0||lineHeight>0)flushLine();
                    cursor+=marginFlow.Finish();
                    value=cursor;
                    if(EstablishesBlockFormattingContext(box))value=std::max(value,maxFloatBottom);
                }else{
                    // Normal block flow groups adjacent inline boxes into lines.
                    // A <br> flushes the current line, and consecutive breaks add
                    // an empty line instead of behaving like a tall empty block.
                    float lineWidth=0,lineHeight=0;
                    auto lineMetrics=InitialInlineLineMetrics(box);
                    BlockMarginFlow marginFlow; marginFlow.parent=CollapsedBlockMargins(box,availableWidth);
                    auto flushLine=[&]{
                        value+=lineHeight;lineWidth=0;lineHeight=0;
                        lineMetrics=InitialInlineLineMetrics(box);
                    };
                    const auto flowChildren=InlineFlowChildren(box,nullptr,innerWidth);
                    for(size_t index=0;index<flowChildren.size();++index){
                        auto* child=flowChildren[index];
                        if(!child->visible||child->style.Is(L"position",L"absolute")||
                           child->style.Is(L"position",L"fixed"))continue;
                        if(child->node->tag==L"br"){
                            marginFlow.Separate();
                            if(lineWidth>0||lineHeight>0)flushLine();
                            else value+=LineHeight(child->style);
                            continue;
                        }
                        if(IsInlineLevel(child->style.Get(L"display"))){
                            marginFlow.Separate();
                            float childWidth=HasInFlowBlockChildren(*child)?innerWidth:NaturalWidth(*child);
                            if(!PreventsTextWrapping(box.style.Get(L"white-space"))&&
                               lineWidth>0&&InlineBreakBefore(flowChildren,index)&&
                               lineWidth+childWidth+InlineAttachedTailWidth(flowChildren,index)>
                                   innerWidth+(child->textFlowFragment?.001f:.5f))flushLine();
                            const bool trimLeading=lineWidth==0&&child->node->type==NodeType::Text&&
                                !PreservesSpaces(child->style.Get(L"white-space"));
                            if(trimLeading)childWidth=TextWidth(child->node->text,child->style,false,child->preserveTrailingWhitespace);
                            lineWidth+=childWidth;
                            const bool atomic=IsAtomicInlineLevel(*child);
                            const float measureWidth=std::max(1.0f,std::min(childWidth,innerWidth));
                            float childHeight=trimLeading?TextHeight(child->node->text,child->style,
                                measureWidth,false,child->preserveTrailingWhitespace):NaturalHeight(*child,measureWidth);
                            if(!atomic&&child->node->type==NodeType::Element&&
                               !IsBlockifiedItem(*child)){
                                const auto childPadding=EdgeValues(child->style,L"padding",innerWidth,innerWidth);
                                const auto childBorder=BorderValues(child->style);
                                const auto childMargin=EdgeValues(child->style,L"margin",innerWidth,innerWidth);
                                childHeight=std::max(LineHeight(child->style),childHeight-
                                    childPadding.top-childPadding.bottom-childBorder.top-childBorder.bottom-
                                    childMargin.top-childMargin.bottom);
                            }
                            // Every CSS inline formatting context carries a strut
                            // with the containing block's font and line-height. A
                            // line made only from smaller inline descendants must
                            // therefore not collapse below the parent's line box.
                            IncludeInlineLineBox(lineMetrics,box.style,*child,childHeight,
                                                 innerWidth,availableWidth);
                            // A text child may already contain several shaped
                            // lines. Its full block extent must survive the
                            // first-line baseline/strut calculation.
                            lineHeight=std::max(lineHeight,lineMetrics.Height());
                            if(child->node->type==NodeType::Text&&childHeight>LineHeight(child->style)+.01f){
                                value+=childHeight-LineHeight(child->style);
                                lineWidth=TextLastLineAdvance(*child,measureWidth,trimLeading);
                                lineMetrics=InitialInlineLineMetrics(box);
                                lineHeight=std::max(LineHeight(box.style),LineHeight(child->style));
                            }
                        }else{
                            if(lineWidth>0||lineHeight>0)flushLine();
                            const auto childMargins=CollapsedBlockMargins(*child,innerWidth);
                            value+=marginFlow.Before(childMargins,child->style.Get(L"clear",L"none")!=L"none");
                            value+=NaturalHeight(*child,BlockOuterWidth(*child,innerWidth,availableWidth))+marginFlow.After(childMargins);
                        }
                    }
                    if(lineWidth>0||lineHeight>0)flushLine();
                    value+=marginFlow.Finish();
                }
            }
        }
        value+=padding.top+padding.bottom+border.top+border.bottom;
    }else{
        // An empty non-replaced box still owns its padding and borders. Inside
        // editable content it also reserves the line box that contains the
        // caret, so formatting an empty block does not change height when the
        // first character is typed.
        const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
        const auto border=UsedBorderValues(box);
        value=padding.top+padding.bottom+border.top+border.bottom+
            (ParticipatesInEditableContent(box.node)?LineHeight(box.style):0.0f);
    }
    const auto display=box.style.Get(L"display");
    const bool automaticHeight=height.empty()||height==L"auto";
    if(automaticHeight&&box.node->type==NodeType::Element&&display==L"inline"&&
       !IsAtomicInlineLevel(box)&&!IsBlockifiedItem(box)&&
       box.node->InnerText().find(L'\n')==std::wstring::npos&&
       NaturalWidth(box)<=availableWidth+0.5f){
        const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
        const auto border=UsedBorderValues(box);
        const float decoration=padding.top+padding.bottom+border.top+border.bottom;
        if(value<=LineHeight(box.style)+decoration+0.01f)
            value=InlineContentBoxHeight(box.style)+decoration;
    }
    // Percentage block-size constraints are indefinite during intrinsic
    // measurement. Resolve them later from a definite containing block rather
    // than from this routine's measurement fallback.
    const auto margin=UsedLayoutMargins(box,availableWidth,availableWidth);
    const bool normalFlow=display!=L"flex"&&display!=L"inline-flex"&&display!=L"grid"&&
        display!=L"inline-grid"&&!IsTable(box)&&display!=L"table-row"&&!IsTableRowGroup(box)&&!CanLayoutColumns(box);
    if(normalFlow&&!box.children.empty()){
        const auto padding=EdgeValues(box.style,L"padding",availableWidth,availableWidth);
        const auto border=UsedBorderValues(box);
        const float innerWidth=std::max(1.0f,availableWidth-padding.left-padding.right-border.left-border.right);
        float cursor=0,lineWidth=0,lineHeight=0,floatBottom=0;
        std::vector<FloatArea> floats;
        BlockMarginFlow marginFlow;marginFlow.parent=CollapsedBlockMargins(box,availableWidth);
        for(const auto& child:box.children){
            if(!child->visible||child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))continue;
            const auto side=UsedFloatSide(child->style);
            if(side!=FloatSide::None){
                const auto width=FloatOuterWidth(*child,innerWidth,availableWidth);
                const auto floatHeight=NaturalHeight(*child,std::max(1.0f,width));
                const auto placed=PlaceFloat(floats,0,innerWidth,cursor,width,floatHeight,side,child->style.Get(L"clear"));
                floats.push_back({placed,side});floatBottom=std::max(floatBottom,placed.y+placed.height);continue;
            }
            const bool inlineChild=IsInlineLevel(child->style.Get(L"display"))&&!HasInFlowBlockChildren(*child);
            const auto width=inlineChild?std::min(innerWidth,NaturalWidth(*child)):
                BlockOuterWidth(*child,innerWidth,availableWidth);
            const auto childHeight=NaturalHeight(*child,std::max(1.0f,width));
            if(inlineChild){
                marginFlow.Separate();
                if(lineWidth>0&&lineWidth+width>innerWidth+0.5f){cursor+=lineHeight;lineWidth=lineHeight=0;}
                if(!EstablishesBlockFormattingContext(*child))floatBottom=std::max(floatBottom,cursor+child->naturalFloatBottom);
                lineWidth+=width;lineHeight=std::max(lineHeight,childHeight);
            }else{
                cursor+=lineHeight;lineWidth=lineHeight=0;
                const auto childMargins=CollapsedBlockMargins(*child,innerWidth);
                cursor+=marginFlow.Before(childMargins,child->style.Get(L"clear",L"none")!=L"none");
                if(!EstablishesBlockFormattingContext(*child))floatBottom=std::max(floatBottom,cursor+child->naturalFloatBottom);
                cursor+=childHeight+marginFlow.After(childMargins);
            }
        }
        if(automaticHeight&&EstablishesBlockFormattingContext(box))
            value=std::max(value,padding.top+border.top+floatBottom+padding.bottom+border.bottom);
        box.naturalFloatBottom=margin.top+padding.top+border.top+floatBottom;
    }
    value=std::max(value-tableCaptionExtent,cellMinimum);
    value=ConstrainIntrinsicHeight(box.style,value,500)+tableCaptionExtent;
    return remember(value+margin.top+margin.bottom);
}

bool CanLayoutColumns(const LayoutBox& box){
    const auto display=box.style.Get(L"display");
    if((display!=L"block"&&display!=L"flow-root")||!HasColumnSizing(box.style)||
       box.style.Get(L"writing-mode",L"horizontal-tb")!=L"horizontal-tb")return false;
    for(const auto& child:box.children){
        if(!child->visible||child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))continue;
        if(child->node->type==NodeType::Text){
            if(NormalizeText(child->node->text,false).empty())continue;
            return false;
        }
        if(UsedFloatSide(child->style)!=FloatSide::None)return false;
        if(child->style.Is(L"column-span",L"all"))continue;
        const auto breaking=child->style.Get(L"break-inside",child->style.Get(L"page-break-inside"));
        if(breaking!=L"avoid"&&breaking!=L"avoid-column"&&!IsAtomicInlineLevel(*child))return false;
    }
    return true;
}

struct ColumnPlacement {LayoutBox* box=nullptr;LayoutRect area;};
struct ColumnFlowModel {float height=0;std::vector<ColumnPlacement> placements;};

ColumnFlowModel BuildColumnFlow(const LayoutBox& box,float contentWidth,float viewportWidth){
    ColumnFlowModel model;
    const auto countText=box.style.Get(L"column-count",L"auto"),widthText=box.style.Get(L"column-width",L"auto");
    unsigned long long parsedCount=0;size_t parsedLength=0;
    const bool definiteCount=TryParseUnsignedInteger(countText,parsedCount,&parsedLength)&&
        parsedLength==countText.size()&&parsedCount>0;
    const float requestedWidth=StyleSheet::Length(widthText,contentWidth,viewportWidth,-1,FontSize(box.style));
    const auto gapText=box.style.Get(L"column-gap",L"normal");
    const float gap=gapText==L"normal"||gapText.empty()?FontSize(box.style):
        std::max(0.0f,StyleSheet::Length(gapText,contentWidth,viewportWidth,0,FontSize(box.style)));
    size_t count=definiteCount?static_cast<size_t>(std::min<unsigned long long>(parsedCount,4096)):1;
    if(requestedWidth>0){
        const size_t fitting=static_cast<size_t>(std::max(1.0f,std::floor((contentWidth+gap)/(requestedWidth+gap))));
        count=definiteCount?std::min(count,fitting):std::min<size_t>(4096,fitting);
    }
    const float units=64*std::max(.01f,box.style.deviceScale);
    const float stride=(contentWidth+gap)/count;
    const float columnWidth=std::max(0.0f,std::floor((stride-gap)*units)/units);
    float heightLimit=std::numeric_limits<float>::infinity();
    const auto padding=EdgeValues(box.style,L"padding",contentWidth,viewportWidth),border=UsedBorderValues(box);
    const float decoration=padding.top+padding.bottom+border.top+border.bottom;
    for(const auto* name:{L"height",L"max-height"}){
        const auto value=box.style.Get(name);
        if(value.empty()||value==L"auto"||value==L"none"||value.find(L'%')!=std::wstring::npos)continue;
        float height=StyleSheet::Length(value,0,0,std::numeric_limits<float>::infinity(),FontSize(box.style));
        if(box.style.Is(L"box-sizing",L"border-box"))height-=decoration;
        heightLimit=std::min(heightLimit,std::max(0.0f,height));
    }
    struct Item {LayoutBox* box;float width,height,top,bottom;bool breakBefore,breakAfter;};
    std::vector<Item> group;
    float rowY=0;
    const bool rtl=box.style.Is(L"direction",L"rtl");
    const auto flush=[&](bool beforeSpanner){
        if(group.empty())return;
        size_t forcedColumns=1;float upper=0,lower=0;
        for(size_t index=0;index<group.size();++index){
            const auto& item=group[index];
            upper+=item.height+std::max(0.0f,item.top)+std::max(0.0f,item.bottom);
            lower=std::max(lower,item.height);
            if(index&&(item.breakBefore||group[index-1].breakAfter))++forcedColumns;
        }
        const size_t budget=std::max(count,forcedColumns);
        const auto place=[&](float limit,bool save){
            size_t column=0;float y=0,pending=0,maximum=0;bool empty=true;
            for(size_t index=0;index<group.size();++index){
                const auto& item=group[index];
                float before=std::max(std::max(0.0f,pending),std::max(0.0f,item.top))+
                    std::min(std::min(0.0f,pending),std::min(0.0f,item.top));
                if(!empty&&(item.breakBefore||group[index-1].breakAfter||y+before+item.height>limit+.0001f)){
                    maximum=std::max(maximum,y);++column;y=0;pending=0;before=0;empty=true;
                }
                if(save){
                    const float offset=std::round(column*stride*units)/units;
                    const float left=rtl?contentWidth-columnWidth-offset:offset;
                    model.placements.push_back({item.box,{left,rowY+y+before-item.top,
                        item.width,item.height+item.top+item.bottom}});
                }
                y+=before+item.height;pending=item.bottom;empty=false;
            }
            maximum=std::max(maximum,y+pending);
            return std::pair<size_t,float>{column+1,maximum};
        };
        float usedHeight=heightLimit;
        const bool balance=beforeSpanner||box.style.Get(L"column-fill",L"balance")!=L"auto"||!std::isfinite(heightLimit);
        if(balance){
            // Find the least height that fits all indivisible blocks while
            // respecting forced breaks. The search operates in layout units.
            long long low=static_cast<long long>(std::floor(lower*units));
            long long high=static_cast<long long>(std::ceil(upper*units));
            while(low<high){
                const auto middle=low+(high-low)/2;const auto trial=place(middle/units,false);
                if(trial.first<=budget&&trial.second<=middle/units+.0001f)high=middle;
                else low=middle+1;
            }
            usedHeight=std::min(heightLimit,low/units);
        }
        const auto placed=place(usedHeight,true);
        rowY+=std::isfinite(heightLimit)?usedHeight:placed.second;
        group.clear();
    };
    for(const auto& child:box.children){
        if(!child->visible||child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed")||
           child->node->type==NodeType::Text)continue;
        const bool spanning=child->style.Is(L"column-span",L"all");
        if(spanning)flush(true);
        const float reference=spanning?contentWidth:columnWidth;
        const float width=BlockOuterWidth(*child,reference,viewportWidth);
        const auto margin=EdgeValues(child->style,L"margin",reference,viewportWidth);
        const float height=NaturalHeight(*child,std::max(1.0f,width));
        if(spanning){model.placements.push_back({child.get(),{0,rowY,width,height}});rowY+=height;continue;}
        group.push_back({child.get(),width,std::max(0.0f,height-margin.top-margin.bottom),margin.top,margin.bottom,
            child->style.Is(L"break-before",L"column"),child->style.Is(L"break-after",L"column")});
    }
    flush(false);model.height=rowY;return model;
}

float NaturalColumnsHeight(const LayoutBox& box,float contentWidth,float viewportWidth){
    return BuildColumnFlow(box,contentWidth,viewportWidth).height;
}

const std::vector<float>& ResolveTableRows(const TableGridModel& model,
                                    const std::vector<float>& columns,D2D1_POINT_2F spacing){
    // ResolveTableColumns invalidates this measurement whenever its input
    // width changes; relayout/restyle rebuilds the owning grid altogether.
    if(model.rowsValid)return model.measuredRows;
    auto& rows=model.measuredRows;
    rows.assign(model.rows.size(),0.0f);
    for(size_t row=0;row<model.rows.size();++row){
        const auto raw=Trim(model.rows[row].box->style.Get(L"height"));
        if(!raw.empty()&&raw!=L"auto"&&raw.find(L'%')==std::wstring::npos)
            rows[row]=std::max(rows[row],StyleSheet::Length(raw,500,500,0));
    }
    auto cellWidth=[&](const TableCellEntry& cell){
        float width=0;
        const size_t end=std::min(columns.size(),cell.column+cell.columnSpan);
        for(size_t column=cell.column;column<end;++column)width+=columns[column];
        if(end>cell.column)width+=spacing.x*(end-cell.column-1);
        return width;
    };
    auto cellHeight=[&](const TableCellEntry& cell){
        float height=NaturalHeight(*cell.box,cellWidth(cell));
        return height;
    };
    for(const auto& cell:model.cells)if(cell.rowSpan==1&&cell.row<rows.size())
        rows[cell.row]=std::max(rows[cell.row],cellHeight(cell));
    for(const auto& cell:model.cells)if(cell.rowSpan>1&&cell.row<rows.size()){
        const size_t end=std::min(rows.size(),cell.row+cell.rowSpan);
        float current=0;
        for(size_t row=cell.row;row<end;++row)current+=rows[row];
        if(end>cell.row)current+=spacing.y*(end-cell.row-1);
        const float deficit=cellHeight(cell)-current;
        if(deficit<=0||end<=cell.row)continue;
        const float share=deficit/static_cast<float>(end-cell.row);
        for(size_t row=cell.row;row<end;++row)rows[row]+=share;
    }
    model.rowsValid=true;
    return rows;
}

bool IsAutomaticInset(const std::wstring& value){
    const auto normalized=ToLower(Trim(value));
    return normalized.empty()||normalized==L"auto";
}

LayoutRect PositionedRect(const LayoutBox& box,const LayoutRect& area,float viewportWidth,float viewportHeight){
    const auto leftRaw=box.style.Get(L"left"),rightRaw=box.style.Get(L"right");
    const auto topRaw=box.style.Get(L"top"),bottomRaw=box.style.Get(L"bottom");
    const bool autoLeft=IsAutomaticInset(leftRaw),autoRight=IsAutomaticInset(rightRaw);
    const bool autoTop=IsAutomaticInset(topRaw),autoBottom=IsAutomaticInset(bottomRaw);
    const float left=StyleSheet::Length(leftRaw,area.width,viewportWidth,0);
    const float right=StyleSheet::Length(rightRaw,area.width,viewportWidth,0);
    const float top=StyleSheet::Length(topRaw,area.height,viewportHeight,0);
    const float bottom=StyleSheet::Length(bottomRaw,area.height,viewportHeight,0);
    const auto widthRaw=box.style.Get(L"width"),heightRaw=box.style.Get(L"height");
    const auto padding=EdgeValues(box.style,L"padding",area.width,viewportWidth);
    const auto border=UsedBorderValues(box);
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");
    const float decorationWidth=padding.left+padding.right+border.left+border.right;
    const float decorationHeight=padding.top+padding.bottom+border.top+border.bottom;
    const auto margin=UsedLayoutMargins(box,area.width,viewportWidth);
    float width=NaturalWidth(box);
    if(HasIntrinsicWidth(box))width=UsedIntrinsicWidth(box,widthRaw,area.width,viewportWidth);
    else if(!widthRaw.empty()&&widthRaw!=L"auto"){
        width=StyleSheet::Length(widthRaw,area.width,viewportWidth,width);
        width=(borderBox?std::max(width,decorationWidth):width+decorationWidth)+margin.left+margin.right;
    }
    else if(!autoLeft&&!autoRight)width=std::max(0.0f,area.width-left-right);
    float height=NaturalHeight(box,width);
    if(!heightRaw.empty()&&heightRaw!=L"auto"){
        height=StyleSheet::Length(heightRaw,area.height,viewportHeight,height);
        height=(borderBox?std::max(height,decorationHeight):height+decorationHeight)+margin.top+margin.bottom;
    }
    else if(!autoTop&&!autoBottom)height=std::max(0.0f,area.height-top-bottom);
    const float x=!autoLeft?area.x+left:(!autoRight?area.x+area.width-right-width:area.x);
    const float y=!autoTop?area.y+top:(!autoBottom?area.y+area.height-bottom-height:area.y);
    return {x,y,std::max(0.0f,width),std::max(0.0f,height)};
}

float SingleFlexItemOffset(const std::wstring& rawAlignment,float freeSpace,
                           bool flexAxisReversed,bool horizontal){
    const auto alignment=AlignmentKeyword(rawAlignment);
    if(alignment==L"center"||alignment==L"space-around"||
       alignment==L"space-evenly")return freeSpace/2.0f;
    if(alignment==L"right"&&horizontal)return freeSpace;
    if(alignment==L"left"&&horizontal)return 0;
    if(alignment==L"flex-end")return flexAxisReversed?0:freeSpace;
    if(alignment==L"flex-start"||alignment==L"normal"||
       alignment==L"stretch"||alignment==L"space-between")
        return flexAxisReversed?freeSpace:0;
    if(alignment==L"end"||alignment==L"self-end")return freeSpace;
    return 0;
}

LayoutRect FlexStaticPositionedRect(const LayoutBox& container,const LayoutBox& child,
                                    LayoutRect positioned){
    const auto direction=ToLower(Trim(container.style.Get(L"flex-direction",L"row")));
    const bool column=direction==L"column"||direction==L"column-reverse";
    const bool mainReversed=direction==L"row-reverse"||direction==L"column-reverse";
    const bool crossReversed=ToLower(Trim(container.style.Get(L"flex-wrap",L"nowrap")))==
        L"wrap-reverse";
    const bool autoLeft=IsAutomaticInset(child.style.Get(L"left"));
    const bool autoRight=IsAutomaticInset(child.style.Get(L"right"));
    const bool autoTop=IsAutomaticInset(child.style.Get(L"top"));
    const bool autoBottom=IsAutomaticInset(child.style.Get(L"bottom"));
    auto alignSelf=child.style.Get(L"align-self",L"auto");
    if(AlignmentKeyword(alignSelf)==L"auto")
        alignSelf=container.style.Get(L"align-items",L"stretch");

    // An out-of-flow flex child with automatic insets keeps the static
    // position it would have as the container's sole flex item. Distributed
    // main-axis alignment therefore centers a single item for space-around
    // and space-evenly, while space-between falls back to flex-start. Use the
    // content box for this hypothetical placement; explicit insets continue
    // to resolve against the positioned ancestor's padding box.
    if(!column){
        if(autoLeft&&autoRight)
            positioned.x=container.content.x+SingleFlexItemOffset(
                container.style.Get(L"justify-content",L"normal"),
                container.content.width-positioned.width,mainReversed,true);
        if(autoTop&&autoBottom)
            positioned.y=container.content.y+SingleFlexItemOffset(
                alignSelf,container.content.height-positioned.height,crossReversed,false);
    }else{
        if(autoTop&&autoBottom)
            positioned.y=container.content.y+SingleFlexItemOffset(
                container.style.Get(L"justify-content",L"normal"),
                container.content.height-positioned.height,mainReversed,false);
        if(autoLeft&&autoRight)
            positioned.x=container.content.x+SingleFlexItemOffset(
                alignSelf,container.content.width-positioned.width,crossReversed,true);
    }
    return positioned;
}

LayoutRect PositionedPaddingBox(const LayoutBox& box){
    // CSS establishes an absolutely positioned descendant's containing block
    // from the ancestor's padding box, not its content box.  Derive it from
    // the final border box so percentages and opposing insets include authored
    // padding while still excluding the border at every monitor DPI.
    const auto border=BorderValues(box.style);
    return {box.rect.x+border.left,box.rect.y+border.top,
        std::max(0.0f,box.rect.width-border.left-border.right),
        std::max(0.0f,box.rect.height-border.top-border.bottom)};
}

const LayoutBox* AbsoluteContainingBlockAncestor(const LayoutBox& box){
    // Even an identity transform establishes a padding-box containing block.
    // Fixed descendants skip ordinary positioned ancestors, but share the
    // transformed ancestor's coordinate system with absolute descendants.
    const bool fixed=box.style.Is(L"position",L"fixed");
    for(auto ancestor=box.parent;ancestor;ancestor=ancestor->parent){
        const auto transform=ToLower(Trim(ancestor->style.Get(L"transform",L"none")));
        const bool transformable=!ancestor->style.Is(L"display",L"inline")||
            IsAtomicInlineLevel(*ancestor)||IsBlockifiedItem(*ancestor)||
            ancestor->style.Is(L"position",L"absolute")||ancestor->style.Is(L"position",L"fixed");
        if((transformable&&!transform.empty()&&transform!=L"none")||
           (!fixed&&ToLower(Trim(ancestor->style.Get(L"position",L"static")))!=L"static"))
            return ancestor;
    }
    return nullptr;
}

LayoutRect AbsoluteContainingBlock(const LayoutBox& box,float viewportWidth,
                                   float viewportHeight){
    if(const auto* ancestor=AbsoluteContainingBlockAncestor(box))
        return PositionedPaddingBox(*ancestor);
    return {0,0,viewportWidth,viewportHeight};
}

struct GridAreaDefinition {
    std::wstring name;
    size_t row = 0, column = 0, rowSpan = 1, columnSpan = 1;
};

struct GridItemPlacement {
    LayoutBox* box = nullptr;
    size_t row = 0, column = 0, rowSpan = 1, columnSpan = 1;
};

enum class GridLineKind { Automatic, Line, Span };
struct GridLineValue {
    GridLineKind kind = GridLineKind::Automatic;
    int line = 0;
    size_t span = 1;
};
struct GridAxisPlacement {
    bool definite = false;
    size_t start = 0, span = 1;
};

std::vector<std::wstring> SplitGridPlacement(const std::wstring& source) {
    std::vector<std::wstring> result;size_t start=0;int nesting=0;
    for(size_t index=0;index<source.size();++index){
        if(source[index]==L'(')++nesting;
        else if(source[index]==L')')--nesting;
        else if(source[index]==L'/'&&nesting==0){
            result.push_back(Trim(source.substr(start,index-start)));start=index+1;
        }
    }
    result.push_back(Trim(source.substr(start)));return result;
}

bool GridInteger(const std::wstring& source,int& value) {
    const auto trimmed=Trim(source);
    size_t used=0;int parsed=0;
    if(!TryParseInteger(trimmed,parsed,&used)||used!=trimmed.size()||parsed==0)return false;
    value=parsed;return true;
}

GridLineValue ParseGridLine(const std::wstring& source) {
    const auto value=ToLower(Trim(source));
    if(value.empty()||value==L"auto")return {};
    const auto words=Words(value);
    if(!words.empty()&&words.front()==L"span"){
        GridLineValue result;result.kind=GridLineKind::Span;
        for(size_t index=1;index<words.size();++index){
            int span=0;if(GridInteger(words[index],span)&&span>0){result.span=static_cast<size_t>(span);break;}
        }
        return result;
    }
    int line=0;if(GridInteger(value,line))return {GridLineKind::Line,line,1};
    return {};
}

size_t ResolveGridLine(int line,size_t explicitTracks) {
    if(line>0)return static_cast<size_t>(line-1);
    const auto resolved=static_cast<long long>(explicitTracks)+1+line;
    return static_cast<size_t>(std::max<long long>(0,resolved));
}

GridAxisPlacement ResolveGridAxis(const std::wstring& startSource,
                                  const std::wstring& endSource,size_t explicitTracks) {
    const auto start=ParseGridLine(startSource),end=ParseGridLine(endSource);
    GridAxisPlacement result;
    if(start.kind==GridLineKind::Line){
        const size_t first=ResolveGridLine(start.line,explicitTracks);
        result.definite=true;result.start=first;
        if(end.kind==GridLineKind::Line){
            const size_t last=ResolveGridLine(end.line,explicitTracks);
            result.start=std::min(first,last);
            result.span=std::max<size_t>(1,first>last?first-last:last-first);
        }else if(end.kind==GridLineKind::Span)result.span=end.span;
        return result;
    }
    if(end.kind==GridLineKind::Line){
        const size_t last=ResolveGridLine(end.line,explicitTracks);
        result.definite=true;result.span=start.kind==GridLineKind::Span?start.span:1;
        result.start=last>result.span?last-result.span:0;return result;
    }
    if(start.kind==GridLineKind::Span)result.span=start.span;
    else if(end.kind==GridLineKind::Span)result.span=end.span;
    return result;
}

GridAxisPlacement ItemGridAxis(const LayoutBox& box,bool rows,size_t explicitTracks,
                               const std::vector<std::wstring>& areaParts) {
    std::wstring start,end;
    const size_t startIndex=rows?0:1,endIndex=rows?2:3;
    if(startIndex<areaParts.size())start=areaParts[startIndex];
    if(endIndex<areaParts.size())end=areaParts[endIndex];
    const auto shorthand=Trim(box.style.Get(rows?L"grid-row":L"grid-column"));
    if(!shorthand.empty()){
        const auto parts=SplitGridPlacement(shorthand);
        start=parts.empty()?L"auto":parts[0];end=parts.size()>1?parts[1]:L"auto";
    }
    const auto startLonghand=Trim(box.style.Get(rows?L"grid-row-start":L"grid-column-start"));
    const auto endLonghand=Trim(box.style.Get(rows?L"grid-row-end":L"grid-column-end"));
    if(!startLonghand.empty())start=startLonghand;
    if(!endLonghand.empty())end=endLonghand;
    return ResolveGridAxis(start,end,explicitTracks);
}

struct GridTrackSizing {
    float base = 0;
    float limit = 0;
    float fraction = 0;
    bool intrinsic = false;
    bool stretch = false;
    bool intrinsicMinimum = false;
    bool maxContentMinimum = false;
};

using GridTrackDefinitions=std::vector<std::wstring>;
std::shared_ptr<const GridTrackDefinitions> ExpandGridTracks(
    const std::wstring& definition,const LayoutBox& box,float reference) {
    const bool contextDependent=definition.find(L"repeat(auto-fit")!=std::wstring::npos||
        definition.find(L"repeat(auto-fill")!=std::wstring::npos;
    static thread_local FastMap<std::wstring,std::shared_ptr<const GridTrackDefinitions>> cache;
    if(!contextDependent)
        if(const auto found=cache.find(definition);found!=cache.end())return found->second;
    auto result=std::make_shared<GridTrackDefinitions>();
    for(const auto& token:Words(definition)){
        if(token.rfind(L"repeat(",0)!=0||token.size()<9||token.back()!=L')'){
            if(!token.empty()&&token!=L"none")result->push_back(token);
            continue;
        }
        const auto inside=token.substr(7,token.size()-8);
        size_t comma=std::wstring::npos;int nesting=0;
        for(size_t i=0;i<inside.size();++i){
            if(inside[i]==L'(')++nesting;
            else if(inside[i]==L')')--nesting;
            else if(inside[i]==L','&&nesting==0){comma=i;break;}
        }
        if(comma==std::wstring::npos)continue;
        const auto countToken=Trim(inside.substr(0,comma));
        const auto repeated=Words(Trim(inside.substr(comma+1)));
        size_t count=0;
        if(countToken==L"auto-fit"||countToken==L"auto-fill")count=GridColumnCount(box,reference);
        else{unsigned long long parsed=0;size_t used=0;
            count=TryParseUnsignedInteger(countToken,parsed,&used)&&used==countToken.size()&&
                parsed<=(std::numeric_limits<size_t>::max)()?
                std::max<size_t>(1,static_cast<size_t>(parsed)):1;
        }
        for(size_t repeat=0;repeat<count;++repeat)
            result->insert(result->end(),repeated.begin(),repeated.end());
    }
    if(!contextDependent){if(cache.size()>=256)cache.clear();cache.emplace(definition,result);}
    return result;
}

std::vector<GridAreaDefinition> ParseGridAreas(const std::wstring& definition,
                                               size_t& rowCount,size_t& columnCount) {
    std::vector<std::vector<std::wstring>> rows;
    wchar_t quote=0;std::wstring current;
    for(const wchar_t c:definition){
        if(!quote&&(c==L'\''||c==L'"')){quote=c;current.clear();}
        else if(quote&&c==quote){rows.push_back(Words(current));quote=0;}
        else if(quote)current+=c;
    }
    rowCount=rows.size();columnCount=0;
    for(const auto& row:rows)columnCount=std::max(columnCount,row.size());
    std::vector<GridAreaDefinition> areas;
    for(size_t row=0;row<rows.size();++row)for(size_t column=0;column<rows[row].size();++column){
        const auto& name=rows[row][column];if(name.empty()||name==L".")continue;
        auto found=std::find_if(areas.begin(),areas.end(),[&](const auto& area){return area.name==name;});
        if(found==areas.end())areas.push_back({name,row,column,1,1});
        else{
            const auto lastRow=std::max(found->row+found->rowSpan,row+1);
            const auto lastColumn=std::max(found->column+found->columnSpan,column+1);
            found->row=std::min(found->row,row);found->column=std::min(found->column,column);
            found->rowSpan=lastRow-found->row;found->columnSpan=lastColumn-found->column;
        }
    }
    return areas;
}

std::vector<GridItemPlacement> PlaceGridItems(const LayoutBox& box,
    const std::vector<GridAreaDefinition>& areas,size_t explicitRows,size_t& columnCount,
    size_t& usedRows) {
    struct Request { LayoutBox* box=nullptr;GridAxisPlacement row,column; };
    const size_t explicitColumns=columnCount;
    const auto flow=Words(ToLower(Trim(box.style.Get(L"grid-auto-flow",L"row"))));
    const bool columnFlow=std::find(flow.begin(),flow.end(),L"column")!=flow.end();
    const bool dense=std::find(flow.begin(),flow.end(),L"dense")!=flow.end();
    // Normalize column flow by transposing axes. In these coordinates rows
    // grow implicitly, while columns are the bounded scanning dimension.
    size_t minorCount=std::max<size_t>(1,columnFlow?explicitRows:explicitColumns);
    const size_t explicitMajor=columnFlow?explicitColumns:explicitRows;
    std::vector<Request> requests;requests.reserve(box.children.size());
    for(const auto& child:box.children){
        if(!child->visible||child->style.Is(L"position",L"absolute")||
           child->style.Is(L"position",L"fixed"))continue;
        const auto areaValue=Trim(child->style.Get(L"grid-area"));
        const auto named=std::find_if(areas.begin(),areas.end(),
            [&](const auto& value){return value.name==areaValue;});
        Request request;request.box=child.get();
        if(named!=areas.end()){
            request.row={true,named->row,named->rowSpan};
            request.column={true,named->column,named->columnSpan};
        }else{
            const auto areaParts=SplitGridPlacement(areaValue);
            request.row=ItemGridAxis(*child,true,explicitRows,areaParts);
            request.column=ItemGridAxis(*child,false,explicitColumns,areaParts);
        }
        if(columnFlow)std::swap(request.row,request.column);
        if(request.column.definite)
            minorCount=std::max(minorCount,request.column.start+request.column.span);
        else minorCount=std::max(minorCount,request.column.span);
        requests.push_back(request);
    }
    std::vector<GridItemPlacement> items;items.reserve(requests.size());
    std::vector<std::vector<bool>> occupied(explicitMajor,std::vector<bool>(minorCount,false));
    auto ensureRows=[&](size_t count){
        while(occupied.size()<count)occupied.push_back(std::vector<bool>(minorCount,false));
    };
    auto canPlace=[&](size_t row,size_t column,size_t rowSpan,size_t columnSpan){
        // Locked-row placement can create columns before the implicit grid's
        // width is finalized; cells beyond existing tracks are unoccupied.
        for(size_t y=row;y<row+rowSpan;++y)for(size_t x=column;x<column+columnSpan;++x)
            if(y<occupied.size()&&x<occupied[y].size()&&occupied[y][x])return false;
        return true;
    };
    auto place=[&](const Request& request,size_t row,size_t column){
        if(column+request.column.span>minorCount){
            minorCount=column+request.column.span;
            for(auto& cells:occupied)cells.resize(minorCount,false);
        }
        ensureRows(row+request.row.span);
        if(columnFlow)items.push_back({request.box,column,row,request.column.span,request.row.span});
        else items.push_back({request.box,row,column,request.row.span,request.column.span});
        for(size_t y=row;y<row+request.row.span;++y)
            for(size_t x=column;x<column+request.column.span;++x)occupied[y][x]=true;
    };

    // Explicitly positioned items may overlap each other, but their occupied
    // cells still steer the later auto-placement phases.
    for(const auto& request:requests)if(request.row.definite&&request.column.definite)
        place(request,request.row.start,request.column.start);
    std::vector<size_t> lockedRowEnd;
    for(const auto& request:requests)if(request.row.definite&&!request.column.definite){
        if(lockedRowEnd.size()<=request.row.start)lockedRowEnd.resize(request.row.start+1,0);
        size_t column=dense?0:lockedRowEnd[request.row.start];
        while(!canPlace(request.row.start,column,request.row.span,request.column.span))++column;
        place(request,request.row.start,column);
        lockedRowEnd[request.row.start]=column+request.column.span;
    }
    // Remaining definite-column and fully automatic items share one cursor
    // in order-modified document order (CSS Grid 8.5). Dense packing restarts
    // the scan for every item; sparse packing never fills an earlier hole.
    size_t cursorRow=0,cursorColumn=0;
    for(const auto& request:requests)if(!request.row.definite){
        if(dense){cursorRow=0;cursorColumn=0;}
        if(request.column.definite){
            if(!dense&&request.column.start<cursorColumn)++cursorRow;
            cursorColumn=request.column.start;
            while(!canPlace(cursorRow,cursorColumn,request.row.span,request.column.span))++cursorRow;
        }else{
            while(true){
                if(cursorColumn+request.column.span>minorCount){++cursorRow;cursorColumn=0;}
                if(canPlace(cursorRow,cursorColumn,request.row.span,request.column.span))break;
                ++cursorColumn;
            }
        }
        place(request,cursorRow,cursorColumn);
    }
    columnCount=columnFlow?std::max<size_t>(1,occupied.size()):minorCount;
    usedRows=columnFlow?minorCount:occupied.size();
    return items;
}

GridTrackSizing ParseGridTrack(const std::wstring& source,float reference,float viewport) {
    const bool contextIndependent=source.find(L'%')==std::wstring::npos&&
        source.find(L"vh")==std::wstring::npos&&source.find(L"vw")==std::wstring::npos&&
        source.find(L"em")==std::wstring::npos&&source.find(L"calc(")==std::wstring::npos&&
        source.find(L"clamp(")==std::wstring::npos;
    static thread_local FastMap<std::wstring,GridTrackSizing> cache;
    if(contextIndependent)
        if(const auto found=cache.find(source);found!=cache.end())return found->second;
    const auto token=ToLower(Trim(source));
    GridTrackSizing track;track.limit=std::numeric_limits<float>::infinity();
    auto remember=[&](const GridTrackSizing& value){
        if(contextIndependent){if(cache.size()>=256)cache.clear();cache.emplace(source,value);}
        return value;
    };
    auto fraction=[](const std::wstring& value){float parsed=0;return TryParseFloat(value,parsed)?parsed:1.0f;};
    auto intrinsic=[](const std::wstring& value){return value==L"auto"||value==L"min-content"||value==L"max-content";};
    if(token.rfind(L"minmax(",0)==0&&token.size()>8&&token.back()==L')'){
        const auto values=CommaSeparated(token.substr(7,token.size()-8));
        if(values.size()==2){
            const auto minimum=Trim(values[0]),maximum=Trim(values[1]);
            if(intrinsic(minimum)){track.intrinsic=true;track.intrinsicMinimum=true;
                track.maxContentMinimum=minimum==L"max-content";}
            else track.base=std::max(0.0f,StyleSheet::Length(minimum,reference,viewport,0));
            if(maximum.find(L"fr")!=std::wstring::npos){track.fraction=std::max(0.0f,fraction(maximum));}
            else if(intrinsic(maximum)){track.intrinsic=true;track.stretch=maximum==L"auto";}
            else track.limit=std::max(track.base,StyleSheet::Length(maximum,reference,viewport,track.base));
            return remember(track);
        }
    }
    if(token.find(L"fr")!=std::wstring::npos){track.fraction=std::max(0.0f,fraction(token));track.intrinsic=true;track.intrinsicMinimum=true;return remember(track);}
    if(intrinsic(token)){track.intrinsic=true;track.intrinsicMinimum=true;track.maxContentMinimum=token==L"max-content";track.stretch=token==L"auto";return remember(track);}
    track.base=std::max(0.0f,StyleSheet::Length(token,reference,viewport,0));track.limit=track.base;
    return remember(track);
}

std::vector<float> ResolveGridTracks(const std::vector<std::wstring>& definitions,
                                     size_t requiredCount,float available,float gap,float viewport,
                                     const std::vector<GridItemPlacement>& items,bool columns,
                                     const std::vector<float>& oppositeSizes,bool definiteAvailable,
                                     bool stretchAutoTracks=true,bool minimumMode=false) {
    const size_t count=std::max<size_t>(1,std::max(requiredCount,definitions.size()));
    std::vector<GridTrackSizing> tracks;tracks.reserve(count);
    for(size_t i=0;i<count;++i)
        tracks.push_back(ParseGridTrack(i<definitions.size()?definitions[i]:L"auto",available,viewport));
    std::vector<bool> shrinkableAutoTracks(count,false);

    auto spanSize=[&](const GridItemPlacement& item){
        const auto start=columns?item.row:item.column;
        const auto span=columns?item.rowSpan:item.columnSpan;
        float result=gap*std::max(0,static_cast<int>(span)-1);
        for(size_t index=0;index<span&&start+index<oppositeSizes.size();++index)result+=oppositeSizes[start+index];
        return std::max(1.0f,result);
    };
    for(const auto& item:items){
        const auto start=columns?item.column:item.row;
        const auto span=columns?item.columnSpan:item.rowSpan;
        if(start>=tracks.size())continue;
        bool spansFlexibleTrack=false,spansStretchTrack=false,spansIntrinsicTrack=false,maxContentMinimum=false;
        for(size_t index=0;index<span&&start+index<tracks.size();++index){
            spansFlexibleTrack=spansFlexibleTrack||tracks[start+index].fraction>0;
            spansStretchTrack=spansStretchTrack||tracks[start+index].stretch;
            spansIntrinsicTrack=spansIntrinsicTrack||(minimumMode?tracks[start+index].intrinsicMinimum:tracks[start+index].intrinsic);
            maxContentMinimum=maxContentMinimum||tracks[start+index].maxContentMinimum;
        }
        // Content cannot grow fixed tracks. It still receives normal layout
        // and contributes overflow after track sizing, without this extra pass.
        if(!spansIntrinsicTrack)continue;
        const auto itemSize=Trim(item.box->style.Get(columns?L"width":L"height"));
        const bool percentageSize=itemSize.find(L'%')!=std::wstring::npos;
        const bool automaticSize=itemSize.empty()||itemSize==L"auto"||percentageSize;
        const auto minimum=Trim(item.box->style.Get(columns?L"min-width":L"min-height"));
        const auto overflow=item.box->style.Get(columns?L"overflow-x":L"overflow-y",
            item.box->style.Get(L"overflow",L"visible"));
        const bool zeroAutomaticMinimum=automaticSize&&
            ((!minimum.empty()&&StyleSheet::Length(minimum,available,viewport,1)==0)||
             (overflow!=L"visible"&&overflow!=L"clip"));
        if(zeroAutomaticMinimum&&!spansFlexibleTrack&&definiteAvailable&&stretchAutoTracks)
            for(size_t index=0;index<span&&start+index<tracks.size();++index)
                shrinkableAutoTracks[start+index]=tracks[start+index].stretch;
        // Determine zero contributions before descending into content. Scroll
        // containers in flexible tracks can contain arbitrarily large trees;
        // measuring those trees only to discard the result stalls first layout.
        if(percentageSize||(zeroAutomaticMinimum&&spansFlexibleTrack))continue;
        // Bare fr and stretched auto tracks use min-content contributions in a
        // definite grid; other intrinsic tracks retain their natural size.
        const float contribution=columns&&minimumMode&&!maxContentMinimum?MinContentWidth(*item.box,true):columns&&definiteAvailable&&
            (spansFlexibleTrack||spansStretchTrack)?MinContentWidth(*item.box):
            (columns?NaturalWidth(*item.box):NaturalHeight(*item.box,
                BlockOuterWidth(*item.box,spanSize(item),viewport)));
        float occupied=gap*std::max(0,static_cast<int>(span)-1);
        for(size_t index=0;index<span&&start+index<tracks.size();++index)occupied+=tracks[start+index].base;
        float deficit=std::max(0.0f,contribution-occupied);
        if(deficit<=0)continue;
        std::vector<size_t> eligible;
        for(size_t index=0;index<span&&start+index<tracks.size();++index){
            const size_t trackIndex=start+index;
            if(minimumMode?tracks[trackIndex].intrinsicMinimum:tracks[trackIndex].intrinsic)eligible.push_back(trackIndex);
        }
        if(eligible.empty())continue;
        for(size_t remaining=eligible.size();remaining&&!eligible.empty()&&deficit>0.01f;){
            const float share=deficit/static_cast<float>(remaining);bool removed=false;
            for(auto it=eligible.begin();it!=eligible.end();){
                auto& track=tracks[*it];const float room=track.limit-track.base;
                const float growth=std::min(share,std::max(0.0f,room));track.base+=growth;deficit-=growth;
                if(room<=share+0.01f){it=eligible.erase(it);--remaining;removed=true;}else ++it;
            }
            if(!removed)break;
        }
        if(!eligible.empty()&&deficit>0){const float share=deficit/eligible.size();for(const auto index:eligible)tracks[index].base+=share;}
    }

    // An auto track has an automatic minimum and a max-content maximum. Keep
    // those two sizes separate: the minimum lets a definite grid contract,
    // while the preferred size lets a short metadata value stay unwrapped
    // before an adjacent fr track receives the remaining space.
    std::vector<float> preferredAutoSizes(tracks.size());
    for(size_t index=0;index<tracks.size();++index)preferredAutoSizes[index]=tracks[index].base;
    if(columns&&definiteAvailable)for(const auto& item:items){
        const auto start=item.column,span=item.columnSpan;
        if(start>=tracks.size())continue;
        const auto itemWidth=Trim(item.box->style.Get(L"width"));
        if(itemWidth.find(L'%')!=std::wstring::npos)continue;
        std::vector<size_t> eligible;
        float occupied=gap*std::max(0,static_cast<int>(span)-1);
        for(size_t offset=0;offset<span&&start+offset<tracks.size();++offset){
            const size_t index=start+offset;
            occupied+=preferredAutoSizes[index];
            if(tracks[index].stretch&&tracks[index].fraction<=0)eligible.push_back(index);
        }
        if(eligible.empty())continue;
        float deficit=std::max(0.0f,NaturalWidth(*item.box)-occupied);
        if(deficit<=0.01f)continue;
        const float share=deficit/static_cast<float>(eligible.size());
        for(const auto index:eligible)preferredAutoSizes[index]+=share;
    }

    const float gaps=gap*std::max(0,static_cast<int>(tracks.size())-1);
    auto used=[&](){float total=gaps;for(const auto& track:tracks)total+=track.base;return total;};
    float overflow=std::max(0.0f,used()-available);
    std::vector<size_t> shrinkable;
    for(size_t index=0;index<tracks.size();++index)
        if(shrinkableAutoTracks[index]&&tracks[index].base>0.01f)shrinkable.push_back(index);
    while(overflow>0.01f&&!shrinkable.empty()){
        const float share=overflow/static_cast<float>(shrinkable.size());
        bool removed=false;
        for(auto it=shrinkable.begin();it!=shrinkable.end();){
            auto& track=tracks[*it];
            const float reduction=std::min(share,track.base);
            track.base-=reduction;overflow-=reduction;
            if(track.base<=0.01f){track.base=0;it=shrinkable.erase(it);removed=true;}
            else ++it;
        }
        if(!removed)break;
    }
    float free=std::max(0.0f,available-used());
    std::vector<size_t> capped;
    for(size_t i=0;i<tracks.size();++i)if(std::isfinite(tracks[i].limit)&&tracks[i].limit>tracks[i].base+0.01f)capped.push_back(i);
    while(free>0.01f&&!capped.empty()){
        const float share=free/capped.size();bool removed=false;
        for(auto it=capped.begin();it!=capped.end();){auto& track=tracks[*it];const float growth=std::min(share,track.limit-track.base);track.base+=growth;free-=growth;if(track.limit-track.base<=0.01f){it=capped.erase(it);removed=true;}else ++it;}
        if(!removed)break;
    }
    std::vector<size_t> preferredAutoTracks;
    for(size_t index=0;index<tracks.size();++index)
        if(preferredAutoSizes[index]>tracks[index].base+0.01f)
            preferredAutoTracks.push_back(index);
    while(free>0.01f&&!preferredAutoTracks.empty()){
        const float share=free/static_cast<float>(preferredAutoTracks.size());
        bool reachedPreferredSize=false;
        for(auto iterator=preferredAutoTracks.begin();iterator!=preferredAutoTracks.end();){
            auto& track=tracks[*iterator];
            const float growth=std::min(share,preferredAutoSizes[*iterator]-track.base);
            track.base+=growth;free-=growth;
            if(preferredAutoSizes[*iterator]-track.base<=0.01f){
                iterator=preferredAutoTracks.erase(iterator);reachedPreferredSize=true;
            }else ++iterator;
        }
        if(!reachedPreferredSize)break;
    }
    float totalFraction=0;for(const auto& track:tracks)totalFraction+=track.fraction;
    if(free>0.01f&&totalFraction>0){
        // An fr track's base is its automatic minimum, not a head start that
        // is added to an equal share of the remaining space.  Resolve one flex
        // fraction from the whole flexible area and freeze only tracks whose
        // minimum is larger than that share.  This keeps equal 1fr columns
        // equal whenever all of their min-content sizes fit, as CSS Grid does.
        std::vector<size_t> flexible;
        float flexibleSpace=available-gaps;
        for(size_t index=0;index<tracks.size();++index){
            if(tracks[index].fraction>0)flexible.push_back(index);
            else flexibleSpace-=tracks[index].base;
        }
        flexibleSpace=std::max(0.0f,flexibleSpace);
        std::vector<bool> frozen(tracks.size(),false);
        while(!flexible.empty()){
            float frozenSize=0,totalActiveFraction=0;
            for(size_t index=0;index<tracks.size();++index)
                if(frozen[index])frozenSize+=tracks[index].base;
            for(const auto index:flexible)totalActiveFraction+=tracks[index].fraction;
            if(totalActiveFraction<=0)break;
            const float fractionSize=std::max(0.0f,flexibleSpace-frozenSize)/
                std::max(1.0f,totalActiveFraction);
            bool frozeTrack=false;
            for(auto it=flexible.begin();it!=flexible.end();){
                auto& track=tracks[*it];
                if(track.base>fractionSize*track.fraction+0.01f){
                    frozen[*it]=true;it=flexible.erase(it);frozeTrack=true;
                }else ++it;
            }
            if(frozeTrack)continue;
            for(const auto index:flexible)
                tracks[index].base=fractionSize*tracks[index].fraction;
            break;
        }
    }else if(free>0.01f&&stretchAutoTracks){
        size_t stretchCount=0;for(const auto& candidate:tracks)if(candidate.stretch)++stretchCount;
        if(stretchCount)for(auto& stretchTrack:tracks)if(stretchTrack.stretch)stretchTrack.base+=free/stretchCount;
    }

    std::vector<float> result;result.reserve(tracks.size());
    const double units=64.0*std::max(0.01f,items.empty()?1.0f:items.front().box->style.deviceScale);
    double accumulated=0;float assigned=0;
    for(const auto& track:tracks){
        accumulated+=std::max(0.0f,track.base);
        // Fractional tracks use floats upstream. Recover exact layout-unit
        // boundaries before flooring (e.g. 81 2/3 CSS px at 144 DPI), so
        // representation noise cannot remove an entire 1/64 device pixel.
        double layoutUnits=accumulated*units;
        const double nearestUnit=std::round(layoutUnits);
        const double representationError=std::max(0.001,
            std::abs(layoutUnits)*std::numeric_limits<float>::epsilon());
        if(std::abs(layoutUnits-nearestUnit)<representationError)layoutUnits=nearestUnit;
        const float edge=static_cast<float>(std::floor(layoutUnits)/units);
        result.push_back(edge-assigned);assigned=edge;
    }
    return result;
}

struct GridContentDistribution {
    float offset = 0;
    float extraGap = 0;
};

std::wstring GridContentAlignment(const std::wstring& source) {
    const auto parts=Words(ToLower(Trim(source)));
    for(auto it=parts.rbegin();it!=parts.rend();++it)
        if(*it!=L"safe"&&*it!=L"unsafe")return *it;
    return L"normal";
}

bool StretchesGridAutoTracks(const std::wstring& source) {
    const auto alignment=GridContentAlignment(source);
    return alignment.empty()||alignment==L"normal"||alignment==L"stretch";
}

GridContentDistribution DistributeGridContent(const std::wstring& source,float available,
                                               const std::vector<float>& tracks,float gap) {
    GridContentDistribution result;
    if(tracks.empty())return result;
    float occupied=gap*std::max(0,static_cast<int>(tracks.size())-1);
    for(const float track:tracks)occupied+=track;
    const float free=std::max(0.0f,available-occupied);
    if(free<=0.01f)return result;

    const auto alignment=GridContentAlignment(source);
    if(alignment==L"center")result.offset=free/2;
    else if(alignment==L"end"||alignment==L"flex-end")result.offset=free;
    else if(alignment==L"space-between"){
        if(tracks.size()>1)result.extraGap=free/static_cast<float>(tracks.size()-1);
    }else if(alignment==L"space-around"){
        result.extraGap=free/static_cast<float>(tracks.size());
        result.offset=result.extraGap/2;
    }else if(alignment==L"space-evenly"){
        result.extraGap=free/static_cast<float>(tracks.size()+1);
        result.offset=result.extraGap;
    }
    return result;
}

GridTrackDefinitions CompleteGridTracks(const GridTrackDefinitions& explicitTracks,
    const LayoutBox& box,bool columns,size_t required,float reference){
    auto result=explicitTracks;
    const auto automatic=ExpandGridTracks(box.style.Get(columns?L"grid-auto-columns":L"grid-auto-rows",L"auto"),box,reference);
    const size_t start=result.size();
    while(result.size()<required)result.push_back(automatic->empty()?L"auto":(*automatic)[(result.size()-start)%automatic->size()]);
    return result;
}

float NaturalGridWidth(const LayoutBox& box,bool minimum){
    const auto definitions=ExpandGridTracks(box.style.Get(L"grid-template-columns",L"none"),box,0);
    const auto rowDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-rows"),box,0);
    size_t areaRows=0,areaColumns=0;const auto areas=ParseGridAreas(box.style.Get(L"grid-template-areas"),areaRows,areaColumns);
    size_t columns=std::max<size_t>(1,std::max(definitions->size(),areaColumns)),usedRows=0;
    const auto items=PlaceGridItems(box,areas,std::max(areaRows,rowDefinitions->size()),columns,usedRows);
    const auto gap=GapValue(box.style,true,0,500);
    const auto complete=CompleteGridTracks(*definitions,box,true,columns,0);
    auto tracks=ResolveGridTracks(complete,columns,0,gap,500,items,true,{},false,false,minimum);
    float fraction=0;
    for(size_t index=0;index<tracks.size();++index){
        const auto sizing=ParseGridTrack(complete[index],0,500);
        if(sizing.fraction>0)fraction=std::max(fraction,tracks[index]/sizing.fraction);
    }
    float width=gap*std::max(0,static_cast<int>(tracks.size())-1);
    for(size_t index=0;index<tracks.size();++index){
        const auto sizing=ParseGridTrack(complete[index],0,500);
        width+=sizing.fraction>0?std::max(tracks[index],fraction*sizing.fraction):tracks[index];
    }
    return width;
}

float NaturalGridHeight(const LayoutBox& box,float availableWidth) {
    const auto columnDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-columns",L"none"),box,availableWidth);
    const auto rowDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-rows"),box,0);
    size_t areaRows=0,areaColumns=0;const auto areas=ParseGridAreas(box.style.Get(L"grid-template-areas"),areaRows,areaColumns);
    size_t columnCount=std::max<size_t>(1,std::max(columnDefinitions->size(),areaColumns));
    const size_t explicitRows=std::max(areaRows,rowDefinitions->size());
    size_t usedRows=0;const auto items=PlaceGridItems(box,areas,explicitRows,columnCount,usedRows);
    const size_t rowCount=std::max<size_t>(1,std::max(rowDefinitions->size(),usedRows));
    const float columnGap=GapValue(box.style,true,availableWidth,availableWidth);
    const float rowGap=GapValue(box.style,false,availableWidth,availableWidth);
    const std::vector<float> provisionalRows(rowCount,20.0f);
    const auto completeColumns=CompleteGridTracks(*columnDefinitions,box,true,columnCount,availableWidth);
    const auto completeRows=CompleteGridTracks(*rowDefinitions,box,false,rowCount,0);
    std::function<bool(const LayoutBox&)> widthIndependentHeight=[&](const LayoutBox& item){
        const auto explicitHeight=Trim(item.style.Get(L"height"));
        if(!explicitHeight.empty()&&explicitHeight!=L"auto"&&explicitHeight.find(L'%')==std::wstring::npos)return true;
        if(item.node->type==NodeType::Text){
            return PreventsTextWrapping(item.style.Get(L"white-space"));
        }
        if(item.node->tag==L"input"||item.node->tag==L"select"||item.node->tag==L"button"||item.node->tag==L"br")return true;
        if(item.node->tag==L"svg"||item.node->tag==L"img")return false;
        if((item.style.Is(L"display",L"flex")||item.style.Is(L"display",L"inline-flex"))&&
           !IsColumnFlexDirection(item.style)&&item.style.Get(L"flex-wrap",L"nowrap")!=L"nowrap")return false;
        for(const auto& child:item.children)
            if(child->visible&&!child->style.Is(L"position",L"absolute")&&
               !child->style.Is(L"position",L"fixed")&&!widthIndependentHeight(*child))return false;
        return true;
    };
    const bool heightDoesNotDependOnColumns=std::all_of(items.begin(),items.end(),
        [&](const GridItemPlacement& item){return widthIndependentHeight(*item.box);});
    const auto columns=heightDoesNotDependOnColumns?
        std::vector<float>(columnCount,std::max(1.0f,(availableWidth-columnGap*std::max(0,static_cast<int>(columnCount)-1))/columnCount)):
        ResolveGridTracks(completeColumns,columnCount,availableWidth,columnGap,availableWidth,items,true,provisionalRows,true);
    const auto rows=ResolveGridTracks(completeRows,rowCount,0,rowGap,availableWidth,items,false,columns,false);
    float height=rowGap*std::max(0,static_cast<int>(rows.size())-1);for(const float row:rows)height+=row;
    return height;
}

unsigned int BackgroundColor(const ComputedStyle& style) {
    auto value=style.Get(L"background-color");if(value.empty())value=style.Get(L"background");
    if(ToLower(Trim(value))==L"currentcolor")value=style.Get(L"color",L"#000000");
    if(value.find(L"gradient")!=std::wstring::npos){
        // CSS backgrounds are painted back-to-front. If gradients are not yet
        // available, preserve the last authored solid layer as the fallback
        // instead of replacing the whole background with the first color stop.
        const auto layers=CommaSeparated(value);
        constexpr unsigned int invalid=0x01020304u;
        for(auto it=layers.rbegin();it!=layers.rend();++it){
            if(it->find(L"gradient")!=std::wstring::npos)continue;
            const auto direct=StyleSheet::Color(*it,invalid);
            if(direct!=invalid)return direct;
            const auto hash=it->find(L'#');
            if(hash!=std::wstring::npos){size_t end=hash+1;while(end<it->size()&&std::iswxdigit((*it)[end]))++end;return StyleSheet::Color(it->substr(hash,end-hash),0);}
            const auto rgb=it->find(L"rgb");
            if(rgb!=std::wstring::npos){const auto end=it->find(L')',rgb);if(end!=std::wstring::npos)return StyleSheet::Color(it->substr(rgb,end-rgb+1),0);}
        }
        // Gradient layers are painted separately and must stay transparent
        // here. Flattening their first stop changes semi-transparent gradients
        // into an opaque wash before the real layers are composited.
        return 0;
    }
    constexpr unsigned int invalid=0x01020304u;
    const auto direct=StyleSheet::Color(value,invalid);if(direct!=invalid)return direct;
    for(auto token:Words(value)){
        while(!token.empty()&&(token.back()==L','||token.back()==L';'))token.pop_back();
        const auto color=StyleSheet::Color(token,invalid);if(color!=invalid)return color;
    }
    return 0;
}

bool HasCanvasBackground(const ComputedStyle& style){
    if((BackgroundColor(style)>>24)!=0)return true;
    const auto image=ToLower(Trim(style.Get(L"background-image")));
    if(!image.empty()&&image!=L"none")return true;
    const auto shorthand=ToLower(style.Get(L"background"));
    return shorthand.find(L"gradient(")!=std::wstring::npos||
           shorthand.find(L"url(")!=std::wstring::npos;
}

unsigned int DeclarationColor(const std::wstring& value,unsigned int fallback){
    constexpr unsigned int invalid=0x01020304u;const auto direct=StyleSheet::Color(value,invalid);if(direct!=invalid)return direct;
    const auto lowered=ToLower(value);if(lowered==L"none")return 0;
    auto tokens=Words(value);for(auto it=tokens.rbegin();it!=tokens.rend();++it){
        const auto color=StyleSheet::Color(*it,invalid);if(color!=invalid)return color;
    }
    return fallback;
}

unsigned int BorderColor(const ComputedStyle& style,const std::wstring& side){
    auto value=style.Get(L"border-"+side+L"-color");if(value.empty())value=style.Get(L"border-color");if(value.empty())value=style.Get(L"border-"+side);if(value.empty())value=style.Get(L"border");const auto lowered=ToLower(value);const auto current=lowered.find(L"currentcolor");if(current!=std::wstring::npos)value.replace(current,12,style.Get(L"color",L"#000000"));return DeclarationColor(value,0xffd1d5db);
}

bool IsPaintOnlyProperty(const std::wstring& name){
    return name==L"background"||name==L"background-color"||name==L"color"||
           name==L"background-image"||name==L"background-position"||
           name==L"background-size"||name==L"background-repeat"||
           name==L"border-color"||name==L"border-top-color"||name==L"border-right-color"||
           name==L"border-bottom-color"||name==L"border-left-color"||name==L"box-shadow"||
           name==L"opacity"||name==L"cursor"||name==L"outline"||name==L"outline-color"||
           name==L"text-decoration"||name==L"text-decoration-line"||
           name==L"text-decoration-color"||name==L"accent-color"||
           name==L"appearance"||name==L"-webkit-appearance"||
           name==L"caret-color"||name==L"fill"||name==L"stroke";
}

bool HasLayoutStyleChange(const ComputedStyle& before,const ComputedStyle& after){
    for(const auto& item:*before.values){const auto found=after.values->find(item.first);if((found==after.values->end()||found->second!=item.second)&&!IsPaintOnlyProperty(item.first))return true;}
    for(const auto& item:*after.values){const auto found=before.values->find(item.first);if((found==before.values->end()||found->second!=item.second)&&!IsPaintOnlyProperty(item.first))return true;}
    return false;
}

bool HasOnlyFixedOffsetLayoutStyleChange(const ComputedStyle& before,
                                         const ComputedStyle& after){
    bool changed=false;
    const auto inspect=[&](const auto& values,const auto& other){
        for(const auto& item:*values){
            const auto found=other->find(item.first);
            if(found!=other->end()&&found->second==item.second)continue;
            if(IsPaintOnlyProperty(item.first))continue;
            if(item.first!=L"top"&&item.first!=L"right"&&
               item.first!=L"bottom"&&item.first!=L"left")return false;
            changed=true;
        }
        return true;
    };
    return inspect(before.values,after.values)&&inspect(after.values,before.values)&&changed;
}

D2D1_COLOR_F D2DColor(unsigned int color) {
    return D2D1::ColorF(((color>>16)&255)/255.0f,((color>>8)&255)/255.0f,(color&255)/255.0f,((color>>24)&255)/255.0f);
}

D2D1_RECT_F PixelAlignedRect(const LayoutRect& rect,float deviceScale);

struct RadialGradientStop {
    unsigned int color = 0;
    float position = -1;
};

bool ParseRadialGradientStop(const std::wstring& source,RadialGradientStop& stop) {
    const auto value=Trim(source);if(value.empty())return false;
    std::wstring colorText,positionText;
    const auto lowered=ToLower(value);
    if(lowered.rfind(L"rgb(",0)==0||lowered.rfind(L"rgba(",0)==0){
        const auto close=value.find(L')');if(close==std::wstring::npos)return false;
        colorText=value.substr(0,close+1);positionText=Trim(value.substr(close+1));
    }else{
        const auto tokens=Words(value);if(tokens.empty())return false;
        colorText=tokens.front();if(tokens.size()>1)positionText=tokens.back();
    }
    constexpr unsigned int invalid=0x01020304u;
    stop.color=StyleSheet::Color(colorText,invalid);if(stop.color==invalid)return false;
    if(!positionText.empty()){
        size_t used=0;float parsed=0;
        if(TryParseFloat(positionText,parsed,&used))
            stop.position=positionText.find(L'%',used)!=std::wstring::npos?parsed/100.0f:parsed;
        else stop.position=-1;
    }
    return true;
}

void NormalizeGradientStops(std::vector<RadialGradientStop>& stops) {
    if(stops.empty())return;
    if(stops.front().position<0)stops.front().position=0;
    if(stops.back().position<0)stops.back().position=1;
    size_t begin=0;
    while(begin+1<stops.size()){
        size_t end=begin+1;while(end<stops.size()&&stops[end].position<0)++end;
        if(end>=stops.size())break;
        const float from=stops[begin].position,to=std::max(from,stops[end].position);
        for(size_t index=begin+1;index<end;++index)
            stops[index].position=from+(to-from)*static_cast<float>(index-begin)/static_cast<float>(end-begin);
        begin=end;
    }
    float previous=0;
    for(auto& stop:stops){stop.position=std::max(previous,std::min(1.0f,stop.position));previous=stop.position;}
    // Premultiplied CSS interpolation keeps the hue while fading to the
    // `transparent` keyword. Give fully transparent stops their neighbour's
    // RGB channels so Direct2D produces the same soft edge.
    for(size_t index=0;index<stops.size();++index)if((stops[index].color>>24)==0){
        const auto neighbour=index?stops[index-1].color:(index+1<stops.size()?stops[index+1].color:0u);
        stops[index].color=neighbour&0x00ffffffu;
    }
}

Edges BackgroundBleedInset(const ComputedStyle& style,const CornerRadii& radius){
    Edges inset{};if(!radius.Any())return inset;
    const auto borders=BorderValues(style);if(borders.top<=0||borders.right<=0||borders.bottom<=0||borders.left<=0)return inset;
    float fraction=.5f;
    for(const auto* side:{L"top",L"right",L"bottom",L"left"}){
        const auto borderStyle=style.Get(L"border-"+std::wstring(side)+L"-style",
            style.Get(L"border-style",style.Get(L"border-"+std::wstring(side),style.Get(L"border"))));
        if((BorderColor(style,side)>>24)!=255)return inset;
        if(borderStyle.find(L"double")!=std::wstring::npos)fraction=1.0f/6;
        else if(borderStyle.find(L"solid")==std::wstring::npos)return inset;
    }
    return {borders.top*fraction,borders.right*fraction,borders.bottom*fraction,borders.left*fraction};
}

void PaintGradientBackgrounds(ID2D1RenderTarget* target,const ComputedStyle& style,
                              const LayoutRect& box,const CornerRadii& radius,float viewport) {
    auto background=style.Get(L"background-image",style.Get(L"background"));
    const auto layers=CommaSeparated(background);const auto rect=PixelAlignedRect(box,style.deviceScale);
    const auto bleed=BackgroundBleedInset(style,radius);
    const auto fillRect=D2D1::RectF(rect.left+bleed.left,rect.top+bleed.top,rect.right-bleed.right,rect.bottom-bleed.bottom);
    const auto fillRadius=InsetCornerRadii(radius,bleed);
    for(auto layer=layers.rbegin();layer!=layers.rend();++layer){
        const auto lowered=ToLower(Trim(*layer));
        const bool radial=lowered.find(L"radial-gradient(")!=std::wstring::npos;
        const bool linear=lowered.find(L"linear-gradient(")!=std::wstring::npos;
        if((!radial&&!linear)||lowered.empty()||lowered.back()!=L')')continue;
        const auto open=lowered.find(radial?L"radial-gradient(":L"linear-gradient(");
        const size_t prefixLength=radial?16:16;
        const auto arguments=CommaSeparated(layer->substr(open+prefixLength,layer->size()-open-prefixLength-1));
        if(arguments.size()<2)continue;
        size_t firstStop=0;
        // CSS gradients are images: their default positioning area is the
        // padding box, while background-clip defaults to the border box.
        auto originBox=box;
        const auto origins=CommaSeparated(style.Get(L"background-origin",L"padding-box"));
        const size_t layerIndex=static_cast<size_t>(std::distance(layers.begin(),layer.base())-1);
        const auto origin=origins.empty()?L"padding-box":origins[layerIndex%origins.size()];
        if(origin!=L"border-box"){
            const auto borders=BorderValues(style);
            originBox.x+=borders.left;originBox.y+=borders.top;
            originBox.width=std::max(0.0f,originBox.width-borders.left-borders.right);
            originBox.height=std::max(0.0f,originBox.height-borders.top-borders.bottom);
            if(origin==L"content-box"){
                const auto padding=EdgeValues(style,L"padding",box.width,viewport);
                originBox.x+=padding.left;originBox.y+=padding.top;
                originBox.width=std::max(0.0f,originBox.width-padding.left-padding.right);
                originBox.height=std::max(0.0f,originBox.height-padding.top-padding.bottom);
            }
        }
        // The generated background image occupies its snapped destination
        // tile. Use that tile for the gradient line without changing layout.
        const auto paintOrigin=PixelAlignedRect(originBox,style.deviceScale);
        originBox={paintOrigin.left,paintOrigin.top,paintOrigin.right-paintOrigin.left,
            paintOrigin.bottom-paintOrigin.top};
        float centerX=originBox.x+originBox.width/2.0f,centerY=originBox.y+originBox.height/2.0f;
        float angleDegrees=180.0f;
        const auto prelude=ToLower(Trim(arguments.front()));
        if(radial&&(prelude.find(L"circle")!=std::wstring::npos||prelude.find(L"ellipse")!=std::wstring::npos||prelude.find(L" at ")!=std::wstring::npos)){
            firstStop=1;const auto at=prelude.find(L" at ");
            if(at!=std::wstring::npos){const auto positions=Words(prelude.substr(at+4));
                if(!positions.empty())centerX=originBox.x+StyleSheet::Length(positions[0],originBox.width,viewport,originBox.width/2.0f);
                if(positions.size()>1)centerY=originBox.y+StyleSheet::Length(positions[1],originBox.height,viewport,originBox.height/2.0f);
            }
        }else if(linear){
            if(prelude.find(L"deg")!=std::wstring::npos){
                float parsed=0;if(TryParseFloat(prelude,parsed)){angleDegrees=parsed;firstStop=1;}
            }else if(prelude.rfind(L"to ",0)==0){
                firstStop=1;
                const bool left=prelude.find(L"left")!=std::wstring::npos;
                const bool right=prelude.find(L"right")!=std::wstring::npos;
                const bool top=prelude.find(L"top")!=std::wstring::npos;
                const bool bottom=prelude.find(L"bottom")!=std::wstring::npos;
                if((left||right)&&(top||bottom))angleDegrees=(right?(bottom?135.0f:45.0f):(bottom?225.0f:315.0f));
                else if(right)angleDegrees=90;else if(left)angleDegrees=270;
                else if(top)angleDegrees=0;else angleDegrees=180;
            }
        }
        std::vector<RadialGradientStop> parsed;
        for(size_t index=firstStop;index<arguments.size();++index){RadialGradientStop stop;if(ParseRadialGradientStop(arguments[index],stop))parsed.push_back(stop);}
        if(parsed.size()<2)continue;NormalizeGradientStops(parsed);
        std::vector<D2D1_GRADIENT_STOP> gradientStops;gradientStops.reserve(parsed.size());
        for(const auto& stop:parsed)gradientStops.push_back({stop.position,D2DColor(stop.color)});
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
        if(FAILED(target->CreateGradientStopCollection(gradientStops.data(),static_cast<UINT32>(gradientStops.size()),
            D2D1_GAMMA_2_2,D2D1_EXTEND_MODE_CLAMP,&collection)))continue;
        auto paint=[&](ID2D1Brush* brush){
            FillRoundedBox(target,fillRect,fillRadius,brush);
        };
        if(radial){
            const float farX=std::max(centerX-originBox.x,originBox.x+originBox.width-centerX);
            const float farY=std::max(centerY-originBox.y,originBox.y+originBox.height-centerY);
            const float circleRadius=std::max(1.0f,std::sqrt(farX*farX+farY*farY));
            Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> brush;
            const auto properties=D2D1::RadialGradientBrushProperties(D2D1::Point2F(centerX,centerY),D2D1::Point2F(0,0),circleRadius,circleRadius);
            if(SUCCEEDED(target->CreateRadialGradientBrush(properties,collection.Get(),&brush)))paint(brush.Get());
        }else{
            constexpr float pi=3.14159265358979323846f;
            const float radians=angleDegrees*pi/180.0f;
            const float directionX=std::sin(radians),directionY=-std::cos(radians);
            const float halfLength=std::max(1.0f,std::abs(directionX)*originBox.width/2.0f+
                std::abs(directionY)*originBox.height/2.0f);
            const auto start=D2D1::Point2F(centerX-directionX*halfLength,centerY-directionY*halfLength);
            const auto end=D2D1::Point2F(centerX+directionX*halfLength,centerY+directionY*halfLength);
            if(parsed.size()==2&&parsed.front().position==0&&parsed.back().position==1&&
               (parsed.front().color>>24)==255&&(parsed.back().color>>24)==255){
                FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
                const auto width=static_cast<UINT>(std::max(0.0f,std::round((rect.right-rect.left)*dpiX/96)));
                const auto height=static_cast<UINT>(std::max(0.0f,std::round((rect.bottom-rect.top)*dpiY/96)));
                std::vector<BYTE> pixels;
                // An unscaled generated image can shade the destination
                // directly. Scaled picture tiles retain their local grid.
                const bool direct=dpiX==96&&dpiY==96;
                const auto ditherOrigin=direct?D2D1::Point2F(0,0):
                    D2D1::Point2F(paintOrigin.left*dpiX/96,paintOrigin.top*dpiY/96);
                if(RasterLinearGradient(width,height,rect.left*dpiX/96,rect.top*dpiY/96,
                    D2D1::Point2F(start.x*dpiX/96,start.y*dpiY/96),D2D1::Point2F(end.x*dpiX/96,end.y*dpiY/96),
                    ditherOrigin,
                    gradientStops.front().color,gradientStops.back().color,pixels)){
                    const auto brushOrigin=D2D1::Point2F(rect.left,rect.top);
                    if(!radius.Uniform()&&activeRasterSurface&&activeRasterSurface->target==target&&
                        activeRasterSurface->PaintSkia(fillRect,fillRadius.corners,0xffffffff,0,nullptr,nullptr,&pixels,width,height,&brushOrigin))continue;
                    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                    const auto bitmapProperties=D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),dpiX,dpiY);
                    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> bitmapBrush;
                    if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(width,height),pixels.data(),width*4,bitmapProperties,&bitmap))&&
                       SUCCEEDED(target->CreateBitmapBrush(bitmap.Get(),D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP,
                        D2D1_EXTEND_MODE_CLAMP,D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),&bitmapBrush))){
                        bitmapBrush->SetTransform(D2D1::Matrix3x2F::Translation(rect.left,rect.top));
                        paint(bitmapBrush.Get());continue;
                    }
                }
            }
            Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
            if(SUCCEEDED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(start,end),
                collection.Get(),&brush)))paint(brush.Get());
        }
    }
}

D2D1_RECT_F PixelAlignedRect(const LayoutRect& rect,float deviceScale) {
    const float scale=std::max(0.01f,deviceScale);
    const auto snap=[scale](float value){
        double device=static_cast<double>(value)*scale;
        const double half=std::round(device*2)/2;
        // Float division by 150% DPI can put an exact half-pixel just below
        // the rounding boundary. Correct representation noise before snapping.
        if(std::abs(device-half)<0.0001)device=half;
        return static_cast<float>(std::round(device)/scale);
    };
    return D2D1::RectF(snap(rect.x),snap(rect.y),
        snap(rect.x+rect.width),snap(rect.y+rect.height));
}

void FillShadowShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,
                     const D2D1_RECT_F& base,const CornerRadii& radius,
                     const BoxShadow& shadow,float expansion) {
    const auto rect=D2D1::RectF(base.left+shadow.offsetX-expansion,
        base.top+shadow.offsetY-expansion,base.right+shadow.offsetX+expansion,
        base.bottom+shadow.offsetY+expansion);
    if(rect.right<=rect.left||rect.bottom<=rect.top)return;
    const Edges inset{-expansion,-expansion,-expansion,-expansion};
    FillRoundedBox(target,rect,InsetCornerRadii(radius,inset),brush);
}

std::array<int,3> GaussianBoxWidths(float sigma){
    if(sigma<=0.01f)return {1,1,1};
    constexpr int count=3;
    const float ideal=std::sqrt(12.0f*sigma*sigma/count+1.0f);
    int lower=static_cast<int>(std::floor(ideal));if((lower&1)==0)--lower;
    lower=std::max(1,lower);const int upper=lower+2;
    const int lowerCount=std::max(0,std::min(count,static_cast<int>(std::lround(
        (12.0f*sigma*sigma-count*lower*lower-4.0f*count*lower-3.0f*count)/
        (-4.0f*lower-4.0f)))));
    return {lowerCount>0?lower:upper,lowerCount>1?lower:upper,
            lowerCount>2?lower:upper};
}

void BoxBlurHorizontal(const std::vector<float>& source,std::vector<float>& target,
                       UINT width,UINT height,int radius){
    if(radius<=0){target=source;return;}
    const float inverse=1.0f/(radius*2+1);
    for(UINT y=0;y<height;++y){
        const size_t row=static_cast<size_t>(y)*width;float sum=0;
        for(int x=0;x<=radius&&x<static_cast<int>(width);++x)sum+=source[row+x];
        for(UINT x=0;x<width;++x){
            target[row+x]=sum*inverse;
            const int remove=static_cast<int>(x)-radius;
            const int append=static_cast<int>(x)+radius+1;
            if(remove>=0)sum-=source[row+static_cast<size_t>(remove)];
            if(append<static_cast<int>(width))sum+=source[row+static_cast<size_t>(append)];
        }
    }
}

void BoxBlurVertical(const std::vector<float>& source,std::vector<float>& target,
                     UINT width,UINT height,int radius){
    if(radius<=0){target=source;return;}
    const float inverse=1.0f/(radius*2+1);
    for(UINT x=0;x<width;++x){
        float sum=0;for(int y=0;y<=radius&&y<static_cast<int>(height);++y)
            sum+=source[static_cast<size_t>(y)*width+x];
        for(UINT y=0;y<height;++y){
            target[static_cast<size_t>(y)*width+x]=sum*inverse;
            const int remove=static_cast<int>(y)-radius;
            const int append=static_cast<int>(y)+radius+1;
            if(remove>=0)sum-=source[static_cast<size_t>(remove)*width+x];
            if(append<static_cast<int>(height))sum+=source[static_cast<size_t>(append)*width+x];
        }
    }
}

bool RoundedRectContains(float x,float y,float left,float top,float right,float bottom,
                         float radiusX,float radiusY){
    if(x<left||x>=right||y<top||y>=bottom)return false;
    radiusX=std::max(0.0f,std::min(radiusX,(right-left)/2.0f));
    radiusY=std::max(0.0f,std::min(radiusY,(bottom-top)/2.0f));
    if(radiusX<=0||radiusY<=0)return true;
    const float centerX=std::max(left+radiusX,std::min(right-radiusX,x));
    const float centerY=std::max(top+radiusY,std::min(bottom-radiusY,y));
    const float dx=(x-centerX)/radiusX,dy=(y-centerY)/radiusY;
    return dx*dx+dy*dy<=1.0f;
}

bool PaintBlurredShadow(ID2D1RenderTarget* target,const LayoutRect& rect,
                        const CornerRadii& radius,const BoxShadow& shadow,float deviceScale,
                        ID2D1RenderTarget*& cacheTarget,
                        FastMap<std::wstring,Microsoft::WRL::ComPtr<ID2D1Bitmap>>& cache,bool opaqueBackground){
    if(!target||shadow.blur<=0.01f||(shadow.color>>24)==0)return false;
    if(activeRasterSurface&&activeRasterSurface->target==target){
        const auto base=PixelAlignedRect(rect,deviceScale);
        const auto shape=D2D1::RectF(base.left-shadow.spread,base.top-shadow.spread,
            base.right+shadow.spread,base.bottom+shadow.spread);
        const auto offset=D2D1::Point2F(shadow.offsetX,shadow.offsetY);
        const auto shadowRadii=InsetCornerRadii(radius,{-shadow.spread,-shadow.spread,-shadow.spread,-shadow.spread});
        const float inset=opaqueBackground?1.0f:0.0f;
        const auto exclusion=D2D1::RectF(base.left+inset,base.top+inset,base.right-inset,base.bottom-inset);
        const auto exclusionRadius=InsetCornerRadii(radius,{inset,inset,inset,inset});
        if(shape.right<=shape.left||shape.bottom<=shape.top)return true;
        if(activeRasterSurface->PaintSkia(shape,shadowRadii.corners,shadow.color,shadow.blur*.5f,
            nullptr,nullptr,nullptr,0,0,nullptr,&exclusion,&exclusionRadius.corners,&offset))return true;
    }
    FLOAT dpiX=USER_DEFAULT_SCREEN_DPI,dpiY=USER_DEFAULT_SCREEN_DPI;
    target->GetDpi(&dpiX,&dpiY);const float scale=dpiX>0?dpiX/USER_DEFAULT_SCREEN_DPI:deviceScale;
    if(scale<=0.01f)return false;
    const float sigma=shadow.blur*scale*0.5f;
    const auto widths=GaussianBoxWidths(sigma);
    int padding=0;for(const auto width:widths)padding+=(width-1)/2;
    const float sourceLeft=(rect.x+shadow.offsetX-shadow.spread)*scale;
    const float sourceTop=(rect.y+shadow.offsetY-shadow.spread)*scale;
    const float sourceRight=(rect.x+rect.width+shadow.offsetX+shadow.spread)*scale;
    const float sourceBottom=(rect.y+rect.height+shadow.offsetY+shadow.spread)*scale;
    if(sourceRight<=sourceLeft||sourceBottom<=sourceTop)return true;
    const int left=static_cast<int>(std::floor(sourceLeft-padding));
    const int top=static_cast<int>(std::floor(sourceTop-padding));
    const int right=static_cast<int>(std::ceil(sourceRight+padding));
    const int bottom=static_cast<int>(std::ceil(sourceBottom+padding));
    if(right<=left||bottom<=top||right-left>8192||bottom-top>8192)return false;
    const UINT bitmapWidth=static_cast<UINT>(right-left),bitmapHeight=static_cast<UINT>(bottom-top);
    std::wostringstream key;key<<std::fixed<<std::setprecision(4)<<bitmapWidth<<L'x'<<bitmapHeight
        <<L':'<<sourceLeft-left<<L','<<sourceTop-top<<L','<<sourceRight-left<<L','<<sourceBottom-top
        <<L':'<<shadow.spread<<L':'<<shadow.color<<L':'
        <<widths[0]<<L','<<widths[1]<<L','<<widths[2]<<L':'<<scale;
    for(const auto& corner:radius.corners)key<<L':'<<corner.x*scale<<L','<<corner.y*scale;
    if(cacheTarget!=target){cache.clear();cacheTarget=target;}
    auto found=cache.find(key.str());
    if(found==cache.end()){
        const size_t count=static_cast<size_t>(bitmapWidth)*bitmapHeight;
        std::vector<float> mask(count),temporary(count);
        auto shadowRadius=InsetCornerRadii(radius,{-shadow.spread,-shadow.spread,-shadow.spread,-shadow.spread});
        for(auto& corner:shadowRadius.corners){corner.x*=scale;corner.y*=scale;}
        constexpr std::array<float,2> samples{0.25f,0.75f};
        for(UINT y=0;y<bitmapHeight;++y)for(UINT x=0;x<bitmapWidth;++x){
            float coverage=0;for(const float sy:samples)for(const float sx:samples)
                {
                    const float px=left+x+sx,py=top+y+sy;
                    const size_t corner=(py<(sourceTop+sourceBottom)/2)?
                        (px<(sourceLeft+sourceRight)/2?0:1):(px<(sourceLeft+sourceRight)/2?3:2);
                    const auto& r=shadowRadius.corners[corner];
                    if(RoundedRectContains(px,py,sourceLeft,sourceTop,sourceRight,sourceBottom,r.x,r.y))coverage+=0.25f;
                }
            mask[static_cast<size_t>(y)*bitmapWidth+x]=coverage;
        }
        for(const auto width:widths){const int blurRadius=(width-1)/2;
            BoxBlurHorizontal(mask,temporary,bitmapWidth,bitmapHeight,blurRadius);
            BoxBlurVertical(temporary,mask,bitmapWidth,bitmapHeight,blurRadius);
        }
        const unsigned char colorAlpha=static_cast<unsigned char>(shadow.color>>24);
        const unsigned char colorRed=static_cast<unsigned char>(shadow.color>>16);
        const unsigned char colorGreen=static_cast<unsigned char>(shadow.color>>8);
        const unsigned char colorBlue=static_cast<unsigned char>(shadow.color);
        std::vector<unsigned char> pixels(count*4);
        for(size_t index=0;index<count;++index){
            const auto alpha=static_cast<unsigned char>(std::lround(
                std::max(0.0f,std::min(1.0f,mask[index]))*colorAlpha));
            pixels[index*4]=static_cast<unsigned char>((colorBlue*alpha+127)/255);
            pixels[index*4+1]=static_cast<unsigned char>((colorGreen*alpha+127)/255);
            pixels[index*4+2]=static_cast<unsigned char>((colorRed*alpha+127)/255);
            pixels[index*4+3]=alpha;
        }
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        const auto properties=D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),
            USER_DEFAULT_SCREEN_DPI*scale,USER_DEFAULT_SCREEN_DPI*scale);
        if(FAILED(target->CreateBitmap(D2D1::SizeU(bitmapWidth,bitmapHeight),pixels.data(),
                bitmapWidth*4,properties,&bitmap)))return false;
        if(cache.size()>=64)cache.clear();found=cache.emplace(key.str(),std::move(bitmap)).first;
    }
    target->DrawBitmap(found->second.Get(),
        D2D1::RectF(left/scale,top/scale,right/scale,bottom/scale),1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
        D2D1::RectF(0,0,found->second->GetSize().width,found->second->GetSize().height));
    return true;
}

void PaintOuterBoxShadows(ID2D1RenderTarget* target,const ComputedStyle& style,
                          const LayoutRect& rect,const CornerRadii& radius,float viewport,
                          float deviceScale,ID2D1RenderTarget*& cacheTarget,
                          FastMap<std::wstring,Microsoft::WRL::ComPtr<ID2D1Bitmap>>& cache) {
    auto shadows=BoxShadows(style,viewport);const auto base=PixelAlignedRect(rect,deviceScale);
    // CSS paints the first listed shadow closest to the element.
    for(auto it=shadows.rbegin();it!=shadows.rend();++it){
        const auto& shadow=*it;if(shadow.inset||(shadow.color>>24)==0)continue;
        if(PaintBlurredShadow(target,rect,radius,shadow,deviceScale,cacheTarget,cache,(BackgroundColor(style)>>24)==255))continue;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if(shadow.blur<=0.01f){
            target->CreateSolidColorBrush(D2DColor(shadow.color),&brush);
            FillShadowShape(target,brush.Get(),base,radius,shadow,shadow.spread);
            continue;
        }
        const int steps=std::max(2,static_cast<int>(std::ceil(shadow.blur)));
        auto color=D2DColor(shadow.color);color.a/=steps;
        target->CreateSolidColorBrush(color,&brush);
        for(int step=steps;step>=1;--step){
            const float expansion=shadow.spread+shadow.blur*step/steps;
            FillShadowShape(target,brush.Get(),base,radius,shadow,expansion);
        }
    }
}

void PaintInsetBoxShadows(ID2D1RenderTarget* target,const ComputedStyle& style,
                          const LayoutRect& rect,float viewport) {
    const auto shadows=BoxShadows(style,viewport);
    const auto outer=PixelAlignedRect(rect,style.deviceScale);
    if(outer.right<=outer.left||outer.bottom<=outer.top)return;
    PushPaintClip(target,outer,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for(auto it=shadows.rbegin();it!=shadows.rend();++it){
        const auto& shadow=*it;
        if(!shadow.inset||(shadow.color>>24)==0)continue;
        const int steps=shadow.blur>0.01f?std::max(2,static_cast<int>(std::ceil(shadow.blur))):1;
        auto color=D2DColor(shadow.color);
        color.a/=static_cast<float>(steps);
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if(FAILED(target->CreateSolidColorBrush(color,&brush)))continue;
        auto fill=[&](float left,float top,float right,float bottom){
            if(right>left&&bottom>top)target->FillRectangle(D2D1::RectF(left,top,right,bottom),brush.Get());
        };
        for(int step=0;step<steps;++step){
            const float expansion=shadow.spread+(steps==1?0.0f:shadow.blur*step/steps);
            const float left=outer.left+shadow.offsetX+expansion;
            const float top=outer.top+shadow.offsetY+expansion;
            const float right=outer.right+shadow.offsetX-expansion;
            const float bottom=outer.bottom+shadow.offsetY-expansion;
            if(left>outer.left)fill(outer.left,outer.top,std::min(left,outer.right),outer.bottom);
            if(right<outer.right)fill(std::max(right,outer.left),outer.top,outer.right,outer.bottom);
            if(top>outer.top)fill(std::max(outer.left,left),outer.top,std::min(outer.right,right),std::min(top,outer.bottom));
            if(bottom<outer.bottom)fill(std::max(outer.left,left),std::max(bottom,outer.top),std::min(outer.right,right),outer.bottom);
        }
    }
    PopPaintClip(target);
}

void PaintOutline(ID2D1RenderTarget* target,const ComputedStyle& style,
                  const LayoutRect& rect,const CornerRadii& radius,float viewport){
    const auto outlineStyle=ToLower(Trim(style.Get(L"outline-style")));
    if(outlineStyle.empty()||outlineStyle==L"none"||outlineStyle==L"hidden")return;
    const auto rawWidth=ToLower(Trim(style.Get(L"outline-width",L"medium")));
    float width=0;
    if(rawWidth==L"thin")width=1;
    else if(rawWidth==L"medium")width=3;
    else if(rawWidth==L"thick")width=5;
    else width=StyleSheet::Length(rawWidth,rect.width,viewport,0);
    if(width<=0)return;
    const float offset=StyleSheet::Length(style.Get(L"outline-offset",L"0"),
                                          rect.width,viewport,0);
    const auto currentColor=StyleSheet::Color(style.Get(L"color",L"#000"),0xff000000);
    const auto rawColor=ToLower(Trim(style.Get(L"outline-color",L"currentcolor")));
    const auto color=rawColor==L"currentcolor"||rawColor==L"invert"?
        currentColor:StyleSheet::Color(rawColor,currentColor);
    if((color>>24)==0)return;

    const float expansion=offset+width/2.0f;
    auto outline=PixelAlignedRect(rect,style.deviceScale);
    outline.left-=expansion;outline.top-=expansion;
    outline.right+=expansion;outline.bottom+=expansion;
    if(outline.right<=outline.left||outline.bottom<=outline.top)return;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if(FAILED(target->CreateSolidColorBrush(D2DColor(color),&brush)))return;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke;
    if(outlineStyle==L"dashed"||outlineStyle==L"dotted"){
        Microsoft::WRL::ComPtr<ID2D1Factory> strokeFactory;target->GetFactory(&strokeFactory);
        if(strokeFactory){
            auto properties=D2D1::StrokeStyleProperties();
            properties.dashStyle=outlineStyle==L"dotted"?
                D2D1_DASH_STYLE_DOT:D2D1_DASH_STYLE_DASH;
            if(outlineStyle==L"dotted")properties.dashCap=D2D1_CAP_STYLE_ROUND;
            strokeFactory->CreateStrokeStyle(properties,nullptr,0,&stroke);
        }
    }
    const auto outlineRadius=InsetCornerRadii(radius,{-expansion,-expansion,-expansion,-expansion});
    if(outlineRadius.Any()){
        if(auto geometry=RoundedBoxGeometry(target,outline,outlineRadius))
            target->DrawGeometry(geometry.Get(),brush.Get(),width,stroke.Get());
    }else target->DrawRectangle(outline,brush.Get(),width,stroke.Get());
}

std::wstring EscapeJson(const std::wstring& value){std::wstring o;for(wchar_t c:value){if(c==L'\\'||c==L'"')o+=L'\\';if(c==L'\n')o+=L"\\n";else o+=c;}return o;}

void TranslateBoxGeometry(LayoutBox& box,float dx,float dy){
    box.rect.x+=dx;box.rect.y+=dy;box.content.x+=dx;box.content.y+=dy;
    box.subtreeBounds.x+=dx;box.subtreeBounds.y+=dy;
}
void TranslateBox(LayoutBox& box,float dx,float dy){TranslateBoxGeometry(box,dx,dy);for(auto& child:box.children)TranslateBox(*child,dx,dy);}
void TranslateRelativeBox(LayoutBox& box,float dx,float dy){
    TranslateBoxGeometry(box,dx,dy);
    for(auto& child:box.children)if(!child->style.Is(L"position",L"fixed"))
        TranslateRelativeBox(*child,dx,dy);
}
void ApplyRelativePosition(LayoutBox& box,float viewportWidth,float viewportHeight){
    auto* containing=box.parent;
    while(containing&&containing->style.Is(L"display",L"inline")&&!IsBlockifiedItem(*containing))
        containing=containing->parent;
    const float width=containing?containing->content.width:viewportWidth;
    const float height=containing?containing->content.height:viewportHeight;
    const bool definiteHeight=!containing||containing->definiteContentHeight;
    const auto inset=[&](const wchar_t* property,bool vertical){
        auto value=box.style.Get(property);
        if(vertical&&!definiteHeight&&value.find(L'%')!=std::wstring::npos)value=L"auto";
        return value;
    };
    const auto left=inset(L"left",false),right=inset(L"right",false);
    const auto top=inset(L"top",true),bottom=inset(L"bottom",true);
    const auto specified=[](const std::wstring& value){return !value.empty()&&value!=L"auto";};
    const bool rtl=containing&&containing->style.Is(L"direction",L"rtl");
    const float dx=specified(right)&&(!specified(left)||rtl)?
        -StyleSheet::Length(right,width,viewportWidth,0):StyleSheet::Length(left,width,viewportWidth,0);
    const float dy=specified(top)?StyleSheet::Length(top,height,viewportHeight,0):
        -StyleSheet::Length(bottom,height,viewportHeight,0);
    box.relativeOffsetX=dx;box.relativeOffsetY=dy;
    if(dx!=0||dy!=0)TranslateRelativeBox(box,dx,dy);
}
bool HasRelativeOffsets(const LayoutBox& box){
    if(!box.visible||box.node->type!=NodeType::Element)return false;
    auto& cached=StyleMetrics(box.style);
    if(!cached.relativeOffsetsValid){
        if(box.style.Is(L"position",L"relative"))
            for(const auto* property:{L"left",L"right",L"top",L"bottom"}){
                const auto value=box.style.Get(property);
                if(!value.empty()&&value!=L"auto"&&value!=L"0"&&value!=L"0px"){
                    cached.hasRelativeOffsets=true;break;
                }
            }
        cached.relativeOffsetsValid=true;
    }
    return cached.hasRelativeOffsets;
}
void ApplyTransform(LayoutBox& box,float viewportWidth,float viewportHeight){
    const auto transform=ToLower(Trim(box.style.Get(L"transform")));if(transform.empty()||transform==L"none")return;
    auto argument=[&](const std::wstring& function){const auto start=transform.find(function+L"(");if(start==std::wstring::npos)return std::wstring{};const auto first=start+function.size()+1,close=transform.find(L')',first);return close==std::wstring::npos?std::wstring{}:Trim(transform.substr(first,close-first));};
    float dx=0,dy=0;
    if(const auto raw=argument(L"translatex");!raw.empty())dx+=StyleSheet::Length(raw,box.rect.width,viewportWidth,0);
    if(const auto raw=argument(L"translatey");!raw.empty())dy+=StyleSheet::Length(raw,box.rect.height,viewportHeight,0);
    if(const auto raw=argument(L"translate");!raw.empty()){
        const auto values=CommaSeparated(raw);if(!values.empty())dx+=StyleSheet::Length(values[0],box.rect.width,viewportWidth,0);
        if(values.size()>1)dy+=StyleSheet::Length(values[1],box.rect.height,viewportHeight,0);
    }
    if(std::abs(dx)>0.001f||std::abs(dy)>0.001f)TranslateBox(box,dx,dy);
}

D2D1::Matrix3x2F PaintTransform(const LayoutBox& box){
    const auto transform=ToLower(Trim(box.style.Get(L"transform")));
    if(transform.empty()||transform==L"none")return D2D1::Matrix3x2F::Identity();
    auto argument=[&](const std::wstring& function){const auto start=transform.find(function+L"(");if(start==std::wstring::npos)return std::wstring{};const auto first=start+function.size()+1,close=transform.find(L')',first);return close==std::wstring::npos?std::wstring{}:Trim(transform.substr(first,close-first));};
    float angle=0,scaleX=1,scaleY=1;
    if(const auto raw=argument(L"rotate");!raw.empty())TryParseFloat(raw,angle);
    if(const auto raw=argument(L"scale");!raw.empty()){
        const auto values=CommaSeparated(raw);float parsedX=1,parsedY=1;
        if(!values.empty()&&TryParseFloat(values[0],parsedX)&&
           (values.size()<=1||TryParseFloat(values[1],parsedY))){
            scaleX=parsedX;scaleY=values.size()>1?parsedY:parsedX;
        }
    }
    if(const auto raw=argument(L"scalex");!raw.empty())TryParseFloat(raw,scaleX);
    if(const auto raw=argument(L"scaley");!raw.empty())TryParseFloat(raw,scaleY);
    const auto center=D2D1::Point2F(box.rect.x+box.rect.width/2,box.rect.y+box.rect.height/2);
    return D2D1::Matrix3x2F::Scale(scaleX,scaleY,center)*D2D1::Matrix3x2F::Rotation(angle,center);
}
bool ApplyPaintTransform(ID2D1RenderTarget* target,const LayoutBox& box,D2D1_MATRIX_3X2_F& previous){
    const auto matrix=PaintTransform(box);if(matrix.IsIdentity())return false;
    target->GetTransform(&previous);target->SetTransform(matrix*previous);
    return true;
}
void ShiftStickyFlow(LayoutBox& box,float dy){if(!box.containsSticky)return;if(box.stickyFlowYValid)box.stickyFlowY+=dy;for(auto& child:box.children)if(child->containsSticky)ShiftStickyFlow(*child,dy);}
void ApplySticky(LayoutBox& box,float clipTop,float viewportHeight){
    if(box.style.Is(L"position",L"sticky")){if(!box.stickyFlowYValid){box.stickyFlowY=box.rect.y;box.stickyFlowYValid=true;}const float top=StyleSheet::Length(box.style.Get(L"top",L"0"),viewportHeight,viewportHeight,0),desired=std::max(box.stickyFlowY,clipTop+top);if(std::abs(box.rect.y-desired)>0.001f)TranslateBox(box,0,desired-box.rect.y);}
    for(auto& child:box.children)if(child->containsSticky)ApplySticky(*child,clipTop,viewportHeight);
    if(HasTableDisplay(box,L"table-row")||IsTableRowGroup(box)){bool found=false;float left=0,top=0,right=0,bottom=0;for(const auto& child:box.children)if(child->visible&&child->rect.width>0&&child->rect.height>0){if(!found){left=child->rect.x;top=child->rect.y;right=child->rect.x+child->rect.width;bottom=child->rect.y+child->rect.height;found=true;}else{left=std::min(left,child->rect.x);top=std::min(top,child->rect.y);right=std::max(right,child->rect.x+child->rect.width);bottom=std::max(bottom,child->rect.y+child->rect.height);}}if(found){box.rect={left,top,right-left,bottom-top};box.content=box.rect;}}
    UpdateSubtreeBounds(box);
}

void ApplyScrollOffset(LayoutBox& box,float oldScrollLeft,float oldScrollTop,float viewportHeight){
    const float dx=oldScrollLeft-box.node->scrollLeft,dy=oldScrollTop-box.node->scrollTop;
    if(std::abs(dx)>=0.001f||std::abs(dy)>=0.001f){
        if(box.scrollTraversalCached){
            for(auto* child:box.scrollTranslationBoxes)TranslateBoxGeometry(*child,dx,dy);
            for(auto* child:box.scrollStickyChildren){
                ShiftStickyFlow(*child,dy);ApplySticky(*child,PaddingBox(box).y,viewportHeight);
            }
        }else{
            for(auto& child:box.children)if(!child->style.Is(L"position",L"fixed")){ShiftStickyFlow(*child,dy);TranslateBox(*child,dx,dy);if(child->containsSticky)ApplySticky(*child,PaddingBox(box).y,viewportHeight);}
        }
        for(auto* ancestor=&box;ancestor;ancestor=ancestor->parent)UpdateSubtreeBounds(*ancestor);
    }
    box.appliedScrollLeft=box.node->scrollLeft;
    box.appliedScrollTop=box.node->scrollTop;
}

std::vector<float> SvgNumbers(const std::wstring& source){
    std::vector<float> values;const wchar_t* cursor=source.c_str();
    while(*cursor){
        if(std::iswspace(*cursor)||*cursor==L','){++cursor;continue;}
        wchar_t* end=nullptr;const float value=std::wcstof(cursor,&end);
        if(end==cursor){++cursor;continue;}values.push_back(value);cursor=end;
    }
    return values;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> SvgPath(ID2D1Factory* factory,const std::wstring& data,
                                             D2D1_FILL_MODE fillMode=D2D1_FILL_MODE_WINDING){
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if(!factory||FAILED(factory->CreatePathGeometry(&path)))return {};
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(path->Open(&sink)))return {};
    sink->SetFillMode(fillMode);
    size_t position=0;wchar_t command=0,previous=0;bool open=false;
    D2D1_POINT_2F point{0,0},begin{0,0},cubic{0,0},quadratic{0,0};
    auto read=[&](float& value){
        while(position<data.size()&&(std::iswspace(data[position])||data[position]==L','))++position;
        if(position>=data.size())return false;
        wchar_t* end=nullptr;value=std::wcstof(data.c_str()+position,&end);
        if(end==data.c_str()+position)return false;
        position=static_cast<size_t>(end-data.c_str());return true;
    };
    auto nextPoint=[&](bool relative,D2D1_POINT_2F& result){
        float x=0,y=0;if(!read(x)||!read(y))return false;
        result={x+(relative?point.x:0),y+(relative?point.y:0)};return true;
    };
    while(position<data.size()){
        while(position<data.size()&&(std::iswspace(data[position])||data[position]==L','))++position;
        if(position>=data.size())break;
        if(std::iswalpha(data[position]))command=data[position++];
        if(!command){++position;continue;}
        const auto before=position;
        const bool relative=std::iswlower(command)!=0;
        const wchar_t action=static_cast<wchar_t>(std::towupper(command));
        D2D1_POINT_2F end{};
        if(action==L'Z'){
            if(open){sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false;point=begin;}
            command=0;previous=L'Z';continue;
        }
        if(action==L'M'){
            if(!nextPoint(relative,end))break;
            if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(end,D2D1_FIGURE_BEGIN_FILLED);point=begin=end;open=true;
            command=relative?L'l':L'L';previous=L'M';continue;
        }
        if(!open){sink->BeginFigure(point,D2D1_FIGURE_BEGIN_FILLED);begin=point;open=true;}
        if(action==L'L'){
            if(!nextPoint(relative,end))break;sink->AddLine(end);point=end;
        }else if(action==L'H'||action==L'V'){
            float coordinate=0;if(!read(coordinate))break;
            end=point;
            if(action==L'H')end.x=coordinate+(relative?point.x:0);
            else end.y=coordinate+(relative?point.y:0);
            sink->AddLine(end);point=end;
        }else if(action==L'C'||action==L'S'){
            D2D1_POINT_2F first=point,second{};
            if(action==L'C'&&!nextPoint(relative,first))break;
            if(action==L'S'&&(previous==L'C'||previous==L'S'))first={2*point.x-cubic.x,2*point.y-cubic.y};
            if(!nextPoint(relative,second)||!nextPoint(relative,end))break;
            sink->AddBezier(D2D1::BezierSegment(first,second,end));point=end;cubic=second;
        }else if(action==L'Q'||action==L'T'){
            D2D1_POINT_2F control=point;
            if(action==L'Q'&&!nextPoint(relative,control))break;
            if(action==L'T'&&(previous==L'Q'||previous==L'T'))control={2*point.x-quadratic.x,2*point.y-quadratic.y};
            if(!nextPoint(relative,end))break;
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(control,end));point=end;quadratic=control;
        }else if(action==L'A'){
            float rx=0,ry=0,rotation=0,large=0,sweep=0;
            if(!read(rx)||!read(ry)||!read(rotation)||!read(large)||!read(sweep)||!nextPoint(relative,end))break;
            if(rx>0&&ry>0)sink->AddArc(D2D1::ArcSegment(end,D2D1::SizeF(rx,ry),rotation,
                sweep?D2D1_SWEEP_DIRECTION_CLOCKWISE:D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                large?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL));
            else sink->AddLine(end);
            point=end;
        }else {command=0;continue;}
        previous=action;
        if(position==before)++position;
    }
    if(open)sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if(FAILED(sink->Close()))return {};
    return path;
}

std::shared_ptr<Node> FindSvgReference(const std::shared_ptr<Node>& source,const std::wstring& id){
    if(!source||id.empty())return {};
    auto root=source;while(auto parent=root->parent.lock())root=parent;
    std::shared_ptr<Node> found;
    std::function<void(const std::shared_ptr<Node>&)> visit=[&](const std::shared_ptr<Node>& node){
        if(!node||found)return;if(node->Attribute(L"id")==id){found=node;return;}
        for(const auto& child:node->children)visit(child);
    };
    visit(root);return found;
}

std::shared_ptr<Node> SvgUseTarget(const std::shared_ptr<Node>& use){
    if(!use||use->tag!=L"use")return {};
    auto reference=use->Attribute(L"href");if(reference.empty())reference=use->Attribute(L"xlink:href");
    return !reference.empty()&&reference.front()==L'#'?FindSvgReference(use,reference.substr(1)):std::shared_ptr<Node>{};
}

D2D1::Matrix3x2F SvgLocalTransform(const std::shared_ptr<Node>& node){
    auto result=D2D1::Matrix3x2F::Identity();const auto source=node->Attribute(L"transform");
    size_t start=0;
    while(start<source.size()){
        const auto open=source.find(L'(',start),close=source.find(L')',open);
        if(open==std::wstring::npos||close==std::wstring::npos)break;
        const auto name=Trim(source.substr(start,open-start));const auto v=SvgNumbers(source.substr(open+1,close-open-1));
        auto local=D2D1::Matrix3x2F::Identity();
        if(name==L"matrix"&&v.size()==6)local={v[0],v[1],v[2],v[3],v[4],v[5]};
        else if(name==L"translate"&&!v.empty())local=D2D1::Matrix3x2F::Translation(v[0],v.size()>1?v[1]:0);
        else if(name==L"scale"&&!v.empty())local=D2D1::Matrix3x2F::Scale(v[0],v.size()>1?v[1]:v[0]);
        else if(name==L"rotate"&&!v.empty())local=D2D1::Matrix3x2F::Rotation(v[0],v.size()>2?D2D1::Point2F(v[1],v[2]):D2D1::Point2F());
        else if(name==L"skewX"&&!v.empty())local=D2D1::Matrix3x2F::Skew(v[0],0);
        else if(name==L"skewY"&&!v.empty())local=D2D1::Matrix3x2F::Skew(0,v[0]);
        result=local*result;start=close+1;
        while(start<source.size()&&(std::iswspace(source[start])||source[start]==L','))++start;
    }
    return result;
}

Microsoft::WRL::ComPtr<IDWriteTextLayout> SvgTextLayout(
        const std::shared_ptr<Node>& node,StyleSheet& styles,
        const ComputedStyle& style,IDWriteFactory* factory){
    struct Run { UINT32 start=0,length=0;ComputedStyle style; };
    std::wstring text;std::vector<Run> runs;bool pendingSpace=false;
    const auto append=[&](wchar_t character,const ComputedStyle& runStyle){
        const auto offset=static_cast<UINT32>(text.size());text+=character;
        if(!runs.empty()&&runs.back().style.values==runStyle.values)++runs.back().length;
        else runs.push_back({offset,1,runStyle});
    };
    std::function<void(const std::shared_ptr<Node>&,const ComputedStyle&)> collect=
        [&](const std::shared_ptr<Node>& current,const ComputedStyle& inherited){
        if(current->type==NodeType::Text){
            const auto whitespace=inherited.Get(L"white-space",L"normal");
            const bool preserve=whitespace==L"pre"||whitespace==L"pre-wrap"||whitespace==L"break-spaces";
            for(const auto character:current->text){
                const bool collapsible=character==L' '||character==L'\t'||character==L'\r'||character==L'\n'||character==L'\f';
                if(!preserve&&collapsible){pendingSpace=!text.empty();continue;}
                if(pendingSpace){append(L' ',inherited);pendingSpace=false;}
                append(character,inherited);
            }
        }else if(current->type==NodeType::Element){
            const auto computed=current==node?style:styles.Compute(current,&inherited);
            if(computed.Is(L"display",L"none"))return;
            for(const auto& child:current->children)
                if(child->type==NodeType::Text||child->tag==L"tspan"||ToLower(child->tag)==L"textpath"||child->tag==L"a")collect(child,computed);
        }
    };
    collect(node,style);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    const auto format=TextFormat(factory,style);
    if(!format||FAILED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),
            format.Get(),1000000,1000000,&layout)))return {};
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    Microsoft::WRL::ComPtr<IDWriteTextLayout1> extended;layout.As(&extended);
    for(const auto& run:runs){
        const DWRITE_TEXT_RANGE range{run.start,run.length};
        layout->SetFontFamilyName(FontFamily(run.style).c_str(),range);
        layout->SetFontSize(FontSize(run.style),range);
        layout->SetFontWeight(static_cast<DWRITE_FONT_WEIGHT>(FontWeight(run.style)),range);
        layout->SetFontStyle(run.style.Is(L"font-style",L"italic")?DWRITE_FONT_STYLE_ITALIC:
            run.style.Is(L"font-style",L"oblique")?DWRITE_FONT_STYLE_OBLIQUE:DWRITE_FONT_STYLE_NORMAL,range);
        if(extended){
            const float spacing=StyleSheet::Length(run.style.Get(L"letter-spacing",L"0"),
                FontSize(run.style),FontSize(run.style),0,FontSize(run.style));
            extended->SetCharacterSpacing(0,spacing,0,range);
            ApplyWordSpacing(extended.Get(),text.substr(run.start,run.length),run.style,run.start);
        }
    }
    for (const auto& run:runs)
        ApplySvgReplacementFallback(layout.Get(),text.substr(run.start,run.length),run.style,run.start);
    return layout;
}

bool MeasureSvgObject(const std::shared_ptr<Node>& node,StyleSheet& styles,
                      const ComputedStyle* parentStyle,ID2D1Factory* factory,IDWriteFactory* writeFactory,
                      const D2D1::Matrix3x2F& transform,D2D1_RECT_F& total,bool& hasBounds,
                      bool includeTransform=false,unsigned depth=0){
    if(!node||node->type!=NodeType::Element||depth>32)return false;
    const auto style=styles.Compute(node,parentStyle);
    const auto matrix=includeTransform?SvgLocalTransform(node)*transform:transform;
    auto number=[&](const wchar_t* name,float fallback=0){return StyleSheet::Length(node->Attribute(name),24,FontSize(style),fallback);};
    const auto merge=[&](const D2D1_RECT_F& b){
        if(!std::isfinite(b.left)||!std::isfinite(b.top)||!std::isfinite(b.right)||!std::isfinite(b.bottom)||b.right<b.left||b.bottom<b.top)return;
        if(!hasBounds){total=b;hasBounds=true;}
        else{total.left=std::min(total.left,b.left);total.top=std::min(total.top,b.top);total.right=std::max(total.right,b.right);total.bottom=std::max(total.bottom,b.bottom);}
    };
    Microsoft::WRL::ComPtr<ID2D1Geometry> geometry;
    if(node->tag==L"path")geometry=SvgPath(factory,node->Attribute(L"d"));
    else if(node->tag==L"rect"||node->tag==L"image"){
        const float x=number(L"x"),y=number(L"y"),w=number(L"width"),h=number(L"height");
        if(w>0&&h>0){Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> rect;factory->CreateRectangleGeometry(D2D1::RectF(x,y,x+w,y+h),&rect);geometry=rect;}
    }else if(node->tag==L"circle"||node->tag==L"ellipse"){
        const float rx=number(node->tag==L"circle"?L"r":L"rx"),ry=node->tag==L"circle"?rx:number(L"ry");
        if(rx>0&&ry>0){Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> ellipse;factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(number(L"cx"),number(L"cy")),rx,ry),&ellipse);geometry=ellipse;}
    }else if(node->tag==L"line"||node->tag==L"polygon"||node->tag==L"polyline"){
        const auto v=node->tag==L"line"?std::vector<float>{number(L"x1"),number(L"y1"),number(L"x2"),number(L"y2")}:SvgNumbers(node->Attribute(L"points"));
        if(v.size()>=4){Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if(SUCCEEDED(factory->CreatePathGeometry(&path))&&SUCCEEDED(path->Open(&sink))){
                sink->BeginFigure(D2D1::Point2F(v[0],v[1]),D2D1_FIGURE_BEGIN_FILLED);
                for(size_t i=2;i+1<v.size();i+=2)sink->AddLine(D2D1::Point2F(v[i],v[i+1]));
                sink->EndFigure(node->tag==L"polygon"?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);
                if(SUCCEEDED(sink->Close()))geometry=path;
            }
        }
    }else if(node->tag==L"text"||node->tag==L"tspan"||ToLower(node->tag)==L"textpath"){
        const auto layout=SvgTextLayout(node,styles,style,writeFactory);
        if(layout){
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);DWRITE_TEXT_METRICS metrics{};DWRITE_LINE_METRICS line{};UINT32 lines=0;
            if(SUCCEEDED(layout->GetMetrics(&metrics))&&SUCCEEDED(layout->GetLineMetrics(&line,1,&lines))){
                float x=number(L"x")+number(L"dx"),y=number(L"y")+number(L"dy")-line.baseline;
                const auto anchor=style.Get(L"text-anchor",L"start");
                if(anchor==L"middle")x-=metrics.widthIncludingTrailingWhitespace/2;else if(anchor==L"end")x-=metrics.widthIncludingTrailingWhitespace;
                Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> rect;
                factory->CreateRectangleGeometry(D2D1::RectF(x,y,x+metrics.widthIncludingTrailingWhitespace,y+metrics.height),&rect);geometry=rect;
            }
        }
    }
    if(geometry){D2D1_RECT_F b{};if(SUCCEEDED(geometry->GetBounds(matrix,&b)))merge(b);}
    if(node->tag==L"use"){
        if(const auto target=SvgUseTarget(node))MeasureSvgObject(target,styles,&style,factory,writeFactory,
            D2D1::Matrix3x2F::Translation(number(L"x"),number(L"y"))*matrix,total,hasBounds,true,depth+1);
    }else if(node->tag==L"svg"||node->tag==L"g"||node->tag==L"symbol"||node->tag==L"a"||node->tag==L"switch"){
        for(const auto& child:node->children)if(child->tag!=L"defs")MeasureSvgObject(child,styles,&style,factory,writeFactory,matrix,total,hasBounds,true,depth+1);
    }
    return hasBounds;
}

void PaintSvgShape(ID2D1RenderTarget* target,ID2D1Factory* factory,StyleSheet& styleSheet,
                   const std::shared_ptr<Node>& node,const ComputedStyle* parentStyle,
                   float parentOpacity,unsigned int inheritedColor,
                   FastMap<std::wstring,Microsoft::WRL::ComPtr<ID2D1PathGeometry>>& geometryCache,
                   D2D1_SIZE_F viewport,
                   int referenceDepth=0){
    if(!node||referenceDepth>16)return;
    const auto style=styleSheet.Compute(node,parentStyle);
    if(style.Is(L"display",L"none")||style.Is(L"visibility",L"hidden")||node->tag==L"defs")return;
    // SVG children do not create HTML layout boxes. Their authored transforms
    // must still be composed in SVG user coordinates and scoped to this subtree.
    struct RestoreTransform {
        ID2D1RenderTarget* target;D2D1_MATRIX_3X2_F previous{};
        explicit RestoreTransform(ID2D1RenderTarget* value):target(value){target->GetTransform(&previous);}
        ~RestoreTransform(){target->SetTransform(previous);}
    } restore(target);
    target->SetTransform(SvgLocalTransform(node)*restore.previous);
    const auto currentColor=StyleSheet::Color(style.Get(L"color"),inheritedColor);
    const auto fill=style.Get(L"fill",L"black");
    const auto stroke=style.Get(L"stroke",L"none");
    const auto strokeWidthText=style.Get(L"stroke-width",L"1");
    const float diagonal=std::hypot(viewport.width,viewport.height)/std::sqrt(2.0f);
    const auto strokeWidth=StyleSheet::Length(strokeWidthText,diagonal,viewport.width,1,FontSize(style));
    float opacity=parentOpacity;
    float parsedOpacity=1;if(TryParseFloat(style.Get(L"opacity",L"1"),parsedOpacity))opacity*=parsedOpacity;
    opacity=std::max(0.0f,std::min(1.0f,opacity));
    if(node->tag==L"g"||node->tag==L"symbol"){
        for(const auto& child:node->children)
            PaintSvgShape(target,factory,styleSheet,child,&style,opacity,currentColor,geometryCache,viewport,referenceDepth);
        return;
    }
    if(node->tag==L"use"){
        const float x=StyleSheet::Length(node->Attribute(L"x"),viewport.width,viewport.width,0,FontSize(style));
        const float y=StyleSheet::Length(node->Attribute(L"y"),viewport.height,viewport.height,0,FontSize(style));
        D2D1_MATRIX_3X2_F transformed{};target->GetTransform(&transformed);
        target->SetTransform(D2D1::Matrix3x2F::Translation(x,y)*transformed);
        if(const auto referenced=SvgUseTarget(node)){
            PaintSvgShape(target,factory,styleSheet,referenced,&style,opacity,currentColor,geometryCache,viewport,referenceDepth+1);
        }
        return;
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fillBrush,strokeBrush;
    auto color=[&](const std::wstring& raw){return raw==L"currentColor"?currentColor:StyleSheet::Color(raw,currentColor);};
    auto brushColor=[&](const std::wstring& raw,const wchar_t* opacityProperty){
        auto result=D2DColor(color(raw));float localOpacity=1;
        TryParseFloat(style.Get(opacityProperty,L"1"),localOpacity);
        result.a*=opacity*std::max(0.0f,std::min(1.0f,localOpacity));return result;
    };
    if(!fill.empty()&&fill!=L"none")target->CreateSolidColorBrush(brushColor(fill,L"fill-opacity"),&fillBrush);
    if(!stroke.empty()&&stroke!=L"none"&&strokeWidth>0)target->CreateSolidColorBrush(brushColor(stroke,L"stroke-opacity"),&strokeBrush);
    D2D1_STROKE_STYLE_PROPERTIES strokeProperties=D2D1::StrokeStyleProperties();
    strokeProperties.lineJoin=D2D1_LINE_JOIN_MITER_OR_BEVEL;
    strokeProperties.miterLimit=std::max(1.0f,StyleSheet::Length(style.Get(L"stroke-miterlimit",L"4"),1,1,4));
    const auto lineCap=ToLower(style.Get(L"stroke-linecap",L"butt"));
    if(lineCap==L"round")strokeProperties.startCap=strokeProperties.endCap=strokeProperties.dashCap=D2D1_CAP_STYLE_ROUND;
    else if(lineCap==L"square")strokeProperties.startCap=strokeProperties.endCap=strokeProperties.dashCap=D2D1_CAP_STYLE_SQUARE;
    const auto lineJoin=ToLower(style.Get(L"stroke-linejoin",L"miter"));
    if(lineJoin==L"round")strokeProperties.lineJoin=D2D1_LINE_JOIN_ROUND;
    else if(lineJoin==L"bevel")strokeProperties.lineJoin=D2D1_LINE_JOIN_BEVEL;
    std::vector<float> dashes;
    auto dashText=Trim(style.Get(L"stroke-dasharray",L"none"));
    if(ToLower(dashText)!=L"none"&&strokeWidth>0){
        std::replace(dashText.begin(),dashText.end(),L',',L' ');
        float sum=0;std::wistringstream tokens(dashText);std::wstring token;
        while(tokens>>token){
            const float length=StyleSheet::Length(token,diagonal,viewport.width,
                std::numeric_limits<float>::quiet_NaN(),FontSize(style));
            if(!std::isfinite(length)||length<0){dashes.clear();sum=0;break;}
            dashes.push_back(length);sum+=length;
        }
        if(sum>0&&std::isfinite(sum)){
            if(dashes.size()%2){const auto repeated=dashes;dashes.insert(dashes.end(),repeated.begin(),repeated.end());}
        }else dashes.clear();
    }
    const auto fillMode=style.Is(L"fill-rule",L"evenodd")?D2D1_FILL_MODE_ALTERNATE:D2D1_FILL_MODE_WINDING;
    auto paintGeometry=[&](ID2D1Geometry* geometry){
        if(!geometry)return;
        if(fillBrush&&(!activeRasterSurface||activeRasterSurface->target!=target||
           !activeRasterSurface->DrawGeometry(geometry,fillBrush.Get())))
            target->FillGeometry(geometry,fillBrush.Get());
        if(strokeBrush){
            auto properties=strokeProperties;auto lengths=dashes;
            if(!lengths.empty()){
                float pathScale=1,authoredLength=0,actualLength=0;
                if(TryParseFloat(node->Attribute(L"pathlength"),authoredLength)&&authoredLength>0&&
                   SUCCEEDED(geometry->ComputeLength(nullptr,&actualLength)))pathScale=actualLength/authoredLength;
                for(auto& length:lengths)length*=pathScale/strokeWidth;
                properties.dashStyle=D2D1_DASH_STYLE_CUSTOM;
                properties.dashOffset=StyleSheet::Length(style.Get(L"stroke-dashoffset",L"0"),diagonal,
                    viewport.width,0,FontSize(style))*pathScale/strokeWidth;
            }
            Microsoft::WRL::ComPtr<ID2D1StrokeStyle> strokeStyle;
            factory->CreateStrokeStyle(properties,lengths.empty()?nullptr:lengths.data(),
                static_cast<UINT32>(lengths.size()),&strokeStyle);
            if(!activeRasterSurface||activeRasterSurface->target!=target||
               !activeRasterSurface->DrawGeometry(geometry,strokeBrush.Get(),strokeWidth,strokeStyle.Get()))
                target->DrawGeometry(geometry,strokeBrush.Get(),strokeWidth,strokeStyle.Get());
        }
    };
    if(node->tag==L"text"){
        const auto layout=SvgTextLayout(node,styleSheet,style,SharedWriteFactory());
        DWRITE_TEXT_METRICS metrics{};DWRITE_LINE_METRICS line{};UINT32 lines=0;
        if(fillBrush&&layout&&SUCCEEDED(layout->GetMetrics(&metrics))&&
           SUCCEEDED(layout->GetLineMetrics(&line,1,&lines))){
            const auto number=[&](const wchar_t* name,float reference){
                return StyleSheet::Length(node->Attribute(name),reference,reference,0,FontSize(style));
            };
            float x=number(L"x",viewport.width)+number(L"dx",viewport.width);
            const float y=number(L"y",viewport.height)+number(L"dy",viewport.height)-line.baseline;
            const auto anchor=style.Get(L"text-anchor",L"start");
            if(anchor==L"middle")x-=metrics.widthIncludingTrailingWhitespace/2;
            else if(anchor==L"end")x-=metrics.widthIncludingTrailingWhitespace;
            DrawWebTextLayout(target,SharedWriteFactory(),D2D1::Point2F(x,y),layout.Get(),fillBrush.Get());
        }
        return;
    }else if(node->tag==L"path"){
        const auto data=node->Attribute(L"d"),key=L"path\x1f"+std::to_wstring(fillMode)+L"\x1f"+data;
        auto found=geometryCache.find(key);
        if(found==geometryCache.end()){
            if(geometryCache.size()>=256)geometryCache.clear();
            found=geometryCache.emplace(key,SvgPath(factory,data,fillMode)).first;
        }
        paintGeometry(found->second.Get());
    }else if(node->tag==L"polygon"||node->tag==L"polyline"){
        const auto source=node->Attribute(L"points"),key=node->tag+
            (fillBrush?L"\x001f" L"f" L"\x001f":L"\x001f" L"h" L"\x001f")+std::to_wstring(fillMode)+L"\x1f"+source;
        auto found=geometryCache.find(key);
        if(found==geometryCache.end()){
            const auto points=SvgNumbers(source);
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if(points.size()>=4&&SUCCEEDED(factory->CreatePathGeometry(&geometry))&&
               SUCCEEDED(geometry->Open(&sink))){
                sink->SetFillMode(fillMode);
                sink->BeginFigure(D2D1::Point2F(points[0],points[1]),
                    fillBrush?D2D1_FIGURE_BEGIN_FILLED:D2D1_FIGURE_BEGIN_HOLLOW);
                for(size_t index=2;index+1<points.size();index+=2)
                    sink->AddLine(D2D1::Point2F(points[index],points[index+1]));
                sink->EndFigure(node->tag==L"polygon"?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);
                if(FAILED(sink->Close()))geometry.Reset();
            }
            if(geometryCache.size()>=256)geometryCache.clear();
            found=geometryCache.emplace(key,std::move(geometry)).first;
        }
        paintGeometry(found->second.Get());
    }else if(node->tag==L"rect"){
        const float x=StyleSheet::Length(node->Attribute(L"x"),24,24,0),y=StyleSheet::Length(node->Attribute(L"y"),24,24,0);
        const float width=StyleSheet::Length(node->Attribute(L"width"),24,24,0),height=StyleSheet::Length(node->Attribute(L"height"),24,24,0);
        const float radius=StyleSheet::Length(node->Attribute(L"rx"),24,24,0);
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> rect;
        if(width>0&&height>0)factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(D2D1::RectF(x,y,x+width,y+height),radius,radius),&rect);
        paintGeometry(rect.Get());
    }else if(node->tag==L"circle"||node->tag==L"ellipse"){
        const float cx=StyleSheet::Length(node->Attribute(L"cx"),24,24,0),cy=StyleSheet::Length(node->Attribute(L"cy"),24,24,0);
        const float rx=StyleSheet::Length(node->Attribute(node->tag==L"circle"?L"r":L"rx"),24,24,0);
        const float ry=node->tag==L"circle"?rx:StyleSheet::Length(node->Attribute(L"ry"),24,24,0);
        Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> ellipse;
        if(rx>0&&ry>0)factory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx,cy),rx,ry),&ellipse);
        if(rx>0&&rx==ry&&dashes.empty()&&activeRasterSurface&&activeRasterSurface->target==target){
            const auto bounds=D2D1::RectF(cx-rx,cy-ry,cx+rx,cy+ry);
            if(fillBrush&&activeRasterSurface->FillCircle(bounds,fillBrush.Get()))fillBrush.Reset();
            if(strokeBrush&&activeRasterSurface->FillCircle(bounds,strokeBrush.Get(),strokeWidth))strokeBrush.Reset();
        }
        paintGeometry(ellipse.Get());
    }else if(node->tag==L"line"){
        const float x1=StyleSheet::Length(node->Attribute(L"x1"),24,24,0),y1=StyleSheet::Length(node->Attribute(L"y1"),24,24,0);
        const float x2=StyleSheet::Length(node->Attribute(L"x2"),24,24,0),y2=StyleSheet::Length(node->Attribute(L"y2"),24,24,0);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> line;Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if(SUCCEEDED(factory->CreatePathGeometry(&line))&&SUCCEEDED(line->Open(&sink))){
            sink->BeginFigure(D2D1::Point2F(x1,y1),D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(x2,y2));sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if(SUCCEEDED(sink->Close()))paintGeometry(line.Get());
        }
    }
    for(const auto& child:node->children)
        PaintSvgShape(target,factory,styleSheet,child,&style,opacity,currentColor,geometryCache,viewport,referenceDepth);
}

void PaintSvg(ID2D1RenderTarget* target,const LayoutBox& box,StyleSheet& styleSheet,
              FastMap<std::wstring,Microsoft::WRL::ComPtr<ID2D1PathGeometry>>& geometryCache){
    auto viewport=SvgNumbers(box.node->Attribute(L"viewbox"));
    if(viewport.empty())for(const auto& child:box.node->children)if(child->tag==L"use"){
        if(const auto referenced=SvgUseTarget(child))viewport=SvgNumbers(referenced->Attribute(L"viewbox"));
        if(!viewport.empty())break;
    }
    const float left=viewport.size()==4?viewport[0]:0,top=viewport.size()==4?viewport[1]:0;
    const float width=viewport.size()==4?viewport[2]:box.content.width;
    const float height=viewport.size()==4?viewport[3]:box.content.height;
    if(width<=0||height<=0||box.content.width<=0||box.content.height<=0)return;
    const float scale=std::min(box.content.width/width,box.content.height/height);
    const float x=box.content.x+(box.content.width-width*scale)/2;
    const float y=box.content.y+(box.content.height-height*scale)/2;
    const bool clip = OverflowX(box) != L"visible" || OverflowY(box) != L"visible";
    if (clip) PushPaintClip(target,D2D1::RectF(box.content.x,box.content.y,
        box.content.x+box.content.width,box.content.y+box.content.height),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    D2D1_MATRIX_3X2_F old{};target->GetTransform(&old);
    target->SetTransform(D2D1::Matrix3x2F::Translation(-left,-top)*
        D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(x,y)*old);
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;target->GetFactory(&factory);
    const auto currentColor=StyleSheet::Color(box.style.Get(L"color",L"#000"),0xff000000);
    for(const auto& child:box.node->children)if(child->tag!=L"defs"&&child->tag!=L"symbol")
        PaintSvgShape(target,factory.Get(),styleSheet,child,&box.style,1,currentColor,geometryCache,D2D1::SizeF(width,height));
    target->SetTransform(old);
    if (clip) PopPaintClip(target);
}

int HexDigit(wchar_t value){
    if(value>=L'0'&&value<=L'9')return value-L'0';
    value=static_cast<wchar_t>(std::towlower(value));
    return value>=L'a'&&value<=L'f'?value-L'a'+10:-1;
}

std::wstring DecodeSvgDataUrl(const std::wstring& cssUrl){
    auto value=Trim(cssUrl);const auto lowered=ToLower(value);
    if(lowered.rfind(L"url(",0)!=0||value.size()<5||value.back()!=L')')return {};
    value=Trim(value.substr(4,value.size()-5));
    if(value.size()>=2&&((value.front()==L'"'&&value.back()==L'"')||
                         (value.front()==L'\''&&value.back()==L'\'')))
        value=value.substr(1,value.size()-2);
    const auto comma=value.find(L',');if(comma==std::wstring::npos)return {};
    const auto header=ToLower(value.substr(0,comma));
    if(header.rfind(L"data:image/svg+xml",0)!=0||header.find(L";base64")!=std::wstring::npos)
        return {};
    std::string bytes;
    for(size_t index=comma+1;index<value.size();++index){
        if(value[index]==L'%'&&index+2<value.size()){
            const int high=HexDigit(value[index+1]),low=HexDigit(value[index+2]);
            if(high>=0&&low>=0){bytes.push_back(static_cast<char>((high<<4)|low));index+=2;continue;}
        }
        if(value[index]<=0x7f)bytes.push_back(static_cast<char>(value[index]));
        else{
            char encoded[4]{};const wchar_t character=value[index];
            const int count=WideCharToMultiByte(CP_UTF8,0,&character,1,encoded,
                                                 static_cast<int>(std::size(encoded)),nullptr,nullptr);
            if(count>0)bytes.append(encoded,encoded+count);
        }
    }
    if(bytes.empty())return {};
    const int length=MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),nullptr,0);
    if(length<=0)return {};
    std::wstring decoded(static_cast<size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,bytes.data(),static_cast<int>(bytes.size()),decoded.data(),length);
    return decoded;
}

std::wstring CssUrlReference(const std::wstring& cssUrl){
    auto value=Trim(cssUrl);const auto lowered=ToLower(value);
    if(lowered.rfind(L"url(",0)!=0||value.size()<5||value.back()!=L')')return {};
    value=Trim(value.substr(4,value.size()-5));
    if(value.size()>=2&&((value.front()==L'"'&&value.back()==L'"')||
                         (value.front()==L'\''&&value.back()==L'\'')))
        value=value.substr(1,value.size()-2);
    std::wstring result;result.reserve(value.size());
    for(size_t index=0;index<value.size();++index){
        if(value[index]==L'\\'&&index+1<value.size())++index;
        result.push_back(value[index]);
    }
    return result;
}

float BackgroundPosition(const std::wstring& token,float freeSpace,float viewport){
    const auto value=ToLower(Trim(token));
    if(value==L"right"||value==L"bottom")return freeSpace;
    if(value==L"center")return freeSpace/2.0f;
    if(value==L"left"||value==L"top"||value.empty())return 0;
    return StyleSheet::Length(value,freeSpace,viewport,0);
}

ID2D1Bitmap* RasterBitmap(ID2D1RenderTarget* target,const std::shared_ptr<RasterImage>& image,
                          ID2D1RenderTarget*& cacheTarget,
                          FastMap<const RasterImageFrame*,Microsoft::WRL::ComPtr<ID2D1Bitmap>>& cache){
    if(!image||image->frames.empty()||image->frameIndex>=image->frames.size()||
       !image->width||!image->height)return nullptr;
    const auto& frame=image->frames[image->frameIndex];
    const auto required=static_cast<std::size_t>(image->width)*image->height*4;
    if(frame.pixels.size()<required)return nullptr;
    if(cacheTarget!=target){cache.clear();cacheTarget=target;}
    auto cached=cache.find(&frame);
    if(cached==cache.end()){
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
        // Decoded image pixels are CSS-pixel resources. A 96-DPI bitmap is 1:1
        // at 100% and Direct2D applies the target DPI exactly once above that.
        const auto properties=D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),
            USER_DEFAULT_SCREEN_DPI,USER_DEFAULT_SCREEN_DPI);
        if(FAILED(target->CreateBitmap(D2D1::SizeU(image->width,image->height),
                frame.pixels.data(),image->width*4,properties,&bitmap)))return nullptr;
        cached=cache.emplace(&frame,std::move(bitmap)).first;
    }
    return cached->second.Get();
}

void PaintImageBackgrounds(ID2D1RenderTarget* target,const ComputedStyle& style,
                           const LayoutRect& box,const CornerRadii& radius,float viewport,
                           Document& document,StyleSheet& styleSheet,
                           FastMap<std::wstring,std::shared_ptr<Node>>& svgCache,
                           FastMap<std::wstring,Microsoft::WRL::ComPtr<ID2D1PathGeometry>>& geometryCache,
                           const LayoutEngine::RasterImageResolver& rasterResolver,
                           ID2D1RenderTarget*& bitmapCacheTarget,
                           FastMap<const RasterImageFrame*,Microsoft::WRL::ComPtr<ID2D1Bitmap>>& bitmapCache){
    const auto images=CommaSeparated(style.Get(L"background-image"));
    if(images.empty())return;
    const auto positions=CommaSeparated(style.Get(L"background-position",L"0% 0%"));
    const auto sizes=CommaSeparated(style.Get(L"background-size",L"auto"));
    const auto repeats=CommaSeparated(style.Get(L"background-repeat",L"repeat"));
    auto layerValue=[](const std::vector<std::wstring>& values,size_t index,const wchar_t* fallback){
        if(values.empty())return std::wstring(fallback);
        return values[std::min(index,values.size()-1)];
    };

    Microsoft::WRL::ComPtr<ID2D1Layer> clipLayer;
    Microsoft::WRL::ComPtr<ID2D1Geometry> clipGeometry;
    bool roundedClip=false;
    if(radius.Any()){
        clipGeometry=RoundedBoxGeometry(target,PixelAlignedRect(box,style.deviceScale),radius);
        if(clipGeometry&&SUCCEEDED(target->CreateLayer(nullptr,&clipLayer))){
            PushPaintLayer(target,D2D1::LayerParameters(D2D1::InfiniteRect(),clipGeometry.Get()),clipLayer.Get());
            roundedClip=true;
        }
    }
    if(!roundedClip)PushPaintClip(target,PixelAlignedRect(box,style.deviceScale),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for(size_t reversed=images.size();reversed>0;--reversed){
        const size_t index=reversed-1;std::shared_ptr<Node> svg;
        std::shared_ptr<RasterImage> raster;
        const auto svgSource=DecodeSvgDataUrl(images[index]);
        if(!svgSource.empty()){
            auto found=svgCache.find(svgSource);
            if(found==svgCache.end()){
                for(const auto& node:document.ParseFragment(svgSource))if(node&&node->tag==L"svg"){svg=node;break;}
                if(svgCache.size()>=128)svgCache.clear();
                found=svgCache.emplace(svgSource,std::move(svg)).first;
            }
            svg=found->second;
        }else if(rasterResolver){
            const auto reference=CssUrlReference(images[index]);
            if(!reference.empty())raster=rasterResolver(reference);
        }
        float intrinsicWidth=0,intrinsicHeight=0;
        if(svg){
            const auto viewBox=SvgNumbers(svg->Attribute(L"viewbox"));
            intrinsicWidth=viewBox.size()==4?viewBox[2]:
                StyleSheet::Length(svg->Attribute(L"width"),box.width,viewport,box.width);
            intrinsicHeight=viewBox.size()==4?viewBox[3]:
                StyleSheet::Length(svg->Attribute(L"height"),box.height,viewport,box.height);
        }else if(raster){
            intrinsicWidth=static_cast<float>(raster->width);
            intrinsicHeight=static_cast<float>(raster->height);
        }else continue;
        if(intrinsicWidth<=0||intrinsicHeight<=0)continue;

        const auto sizeValue=ToLower(Trim(layerValue(sizes,index,L"auto")));
        float width=intrinsicWidth,height=intrinsicHeight;
        if(sizeValue==L"contain"||sizeValue==L"cover"){
            const float fit=sizeValue==L"cover"?
                std::max(box.width/intrinsicWidth,box.height/intrinsicHeight):
                std::min(box.width/intrinsicWidth,box.height/intrinsicHeight);
            width=intrinsicWidth*fit;height=intrinsicHeight*fit;
        }else{
            const auto words=Words(sizeValue);
            const bool autoWidth=words.empty()||words[0]==L"auto";
            const bool autoHeight=words.size()<2||words[1]==L"auto";
            if(!autoWidth)width=StyleSheet::Length(words[0],box.width,viewport,width);
            if(!autoHeight)height=StyleSheet::Length(words[1],box.height,viewport,height);
            if(autoWidth&&!autoHeight)width=height*intrinsicWidth/intrinsicHeight;
            else if(!autoWidth&&autoHeight)height=width*intrinsicHeight/intrinsicWidth;
        }
        if(width<=0||height<=0)continue;

        const auto positionWords=Words(ToLower(layerValue(positions,index,L"0% 0%")));
        std::wstring horizontal=L"0%",vertical=L"0%";
        if(positionWords.size()==1){
            if(positionWords[0]==L"top"||positionWords[0]==L"bottom"){
                horizontal=L"center";vertical=positionWords[0];
            }else{horizontal=positionWords[0];vertical=L"center";}
        }else if(positionWords.size()>=2){horizontal=positionWords[0];vertical=positionWords[1];}
        const float left=box.x+BackgroundPosition(horizontal,box.width-width,viewport);
        const float top=box.y+BackgroundPosition(vertical,box.height-height,viewport);
        const auto repeat=ToLower(Trim(layerValue(repeats,index,L"repeat")));
        const auto repeatWords=Words(repeat);
        bool repeatX=true,repeatY=true;
        if(repeat==L"repeat-x")repeatY=false;
        else if(repeat==L"repeat-y")repeatX=false;
        else if(repeat==L"no-repeat")repeatX=repeatY=false;
        else if(!repeatWords.empty()){
            repeatX=repeatWords[0]!=L"no-repeat";
            repeatY=(repeatWords.size()>1?repeatWords[1]:repeatWords[0])!=L"no-repeat";
        }
        float firstX=left,firstY=top;
        if(repeatX)while(firstX>box.x)firstX-=width;
        if(repeatY)while(firstY>box.y)firstY-=height;
        size_t painted=0;
        for(float y=firstY;y<box.y+box.height&&painted<4096;y+=repeatY?height:box.height+height){
            for(float x=firstX;x<box.x+box.width&&painted<4096;x+=repeatX?width:box.width+width){
                if(svg){
                    LayoutBox image;image.node=svg;image.style=style;
                    image.rect=image.content={x,y,width,height};
                    PaintSvg(target,image,styleSheet,geometryCache);
                }else if(auto* bitmap=RasterBitmap(target,raster,bitmapCacheTarget,bitmapCache)){
                    const auto interpolation=style.Is(L"image-rendering",L"pixelated")||
                        style.Is(L"image-rendering",L"crisp-edges")?
                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR:
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
                    target->DrawBitmap(bitmap,D2D1::RectF(x,y,x+width,y+height),1.0f,interpolation,
                        D2D1::RectF(0,0,intrinsicWidth,intrinsicHeight));
                }
                ++painted;
                if(!repeatX)break;
            }
            if(!repeatY)break;
        }
    }
    if(roundedClip)PopPaintLayer(target);else PopPaintClip(target);
}

D2D1_MATRIX_3X2_F CanvasMatrix(const CanvasTransform& transform){
    return D2D1::Matrix3x2F(transform.a,transform.b,transform.c,transform.d,
                           transform.e,transform.f);
}

Microsoft::WRL::ComPtr<ID2D1Brush> CanvasRadialBrush(ID2D1RenderTarget* target,const CanvasGradient& gradient){
    Microsoft::WRL::ComPtr<ID2D1Brush> result;
    D2D1::Matrix3x2F inverse(gradient.transform.a,gradient.transform.b,gradient.transform.c,
        gradient.transform.d,gradient.transform.e,gradient.transform.f);
    const auto size=target->GetSize();const auto width=static_cast<UINT>(std::ceil(size.width)),height=static_cast<UINT>(std::ceil(size.height));
    if(!width||!height||static_cast<std::uint64_t>(width)*height>16*1024*1024||!inverse.Invert())return result;
    std::vector<D2D1_COLOR_F> colors;colors.reserve(gradient.stops.size());
    for(const auto& stop:gradient.stops)colors.push_back(D2DColor(StyleSheet::Color(stop.color,0xff000000)));
    std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4,0);
    const double dx=gradient.x1-gradient.x0,dy=gradient.y1-gradient.y0,dr=gradient.radius1-gradient.radius0;
    const double quadratic=dx*dx+dy*dy-dr*dr;
    if(dx!=0||dy!=0||dr!=0)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
        const double px=(x+0.5)*inverse._11+(y+0.5)*inverse._21+inverse._31-gradient.x0;
        const double py=(x+0.5)*inverse._12+(y+0.5)*inverse._22+inverse._32-gradient.y0;
        const double linear=-2*(px*dx+py*dy+gradient.radius0*dr),constant=px*px+py*py-gradient.radius0*gradient.radius0;
        double position=-std::numeric_limits<double>::infinity();
        const auto accept=[&](double value){if(std::isfinite(value)&&gradient.radius0+value*dr>=0)position=std::max(position,value);};
        if(std::abs(quadratic)<1e-12){if(std::abs(linear)>1e-12)accept(-constant/linear);}
        else{
            const double discriminant=linear*linear-4*quadratic*constant;
            if(discriminant>=0){const double root=std::sqrt(discriminant);accept((-linear-root)/(2*quadratic));accept((-linear+root)/(2*quadratic));}
        }
        if(!std::isfinite(position))continue;
        const auto stop=std::upper_bound(gradient.stops.begin(),gradient.stops.end(),position,
            [](double offset,const CanvasGradientStop& value){return offset<value.offset;});
        D2D1_COLOR_F color;
        if(stop==gradient.stops.begin())color=colors.front();
        else if(stop==gradient.stops.end())color=colors.back();
        else{
            const auto index=static_cast<size_t>(stop-gradient.stops.begin());
            const float factor=static_cast<float>((position-gradient.stops[index-1].offset)/(stop->offset-gradient.stops[index-1].offset));
            const auto& first=colors[index-1];const auto& second=colors[index];
            color={first.r+(second.r-first.r)*factor,first.g+(second.g-first.g)*factor,
                first.b+(second.b-first.b)*factor,first.a+(second.a-first.a)*factor};
        }
        auto* pixel=pixels.data()+(static_cast<size_t>(y)*width+x)*4;
        const auto byte=[](float value){return static_cast<unsigned char>(std::lround(std::clamp(value,0.0f,1.0f)*255));};
        pixel[0]=byte(color.b*color.a);pixel[1]=byte(color.g*color.a);pixel[2]=byte(color.r*color.a);pixel[3]=byte(color.a);
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;Microsoft::WRL::ComPtr<ID2D1BitmapBrush> brush;
    if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(width,height),pixels.data(),width*4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96),&bitmap))&&
       SUCCEEDED(target->CreateBitmapBrush(bitmap.Get(),&brush))){
        D2D1::Matrix3x2F drawing;target->GetTransform(&drawing);
        if(drawing.Invert()){brush->SetTransform(drawing);brush.As(&result);}
    }
    return result;
}

Microsoft::WRL::ComPtr<ID2D1Brush> CanvasBrush(ID2D1RenderTarget* target,
                                                const CanvasPaint& paint,double opacity){
    Microsoft::WRL::ComPtr<ID2D1Brush> result;
    if(paint.gradient&&(paint.gradient->radial||paint.gradient->stops.empty())){
        if(!paint.gradient->stops.empty())result=CanvasRadialBrush(target,*paint.gradient);
        if(!result){Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
            if(SUCCEEDED(target->CreateSolidColorBrush(D2D1::ColorF(0,0.0f),&brush)))brush.As(&result);}
        if(result)result->SetOpacity(static_cast<float>(opacity));
        return result;
    }
    if(paint.gradient&&!paint.gradient->stops.empty()){
        std::vector<D2D1_GRADIENT_STOP> stops;stops.reserve(paint.gradient->stops.size()+2);
        for(const auto& stop:paint.gradient->stops)
            stops.push_back({std::max(0.0f,std::min(1.0f,stop.offset)),
                D2DColor(StyleSheet::Color(stop.color,0xff000000))});
        if(stops.size()==1)stops.push_back({1.0f,stops.front().color});
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
        if(SUCCEEDED(target->CreateGradientStopCollection(stops.data(),
                static_cast<UINT32>(stops.size()),&collection))&&
           SUCCEEDED(target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(paint.gradient->x0,paint.gradient->y0),
                    D2D1::Point2F(paint.gradient->x1,paint.gradient->y1)),
                collection.Get(),&brush)))
            brush.As(&result);
    }
    if(!result){
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if(SUCCEEDED(target->CreateSolidColorBrush(
                D2DColor(StyleSheet::Color(paint.color,0xff000000)),&brush)))
            brush.As(&result);
    }
    if(result)result->SetOpacity(static_cast<float>(opacity));
    return result;
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry> CanvasGeometry(
        ID2D1RenderTarget* target,const std::vector<CanvasPathSegment>& path){
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;target->GetFactory(&factory);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if(!factory||FAILED(factory->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink)))return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);bool open=false,hasCurrent=false;CanvasPoint current{},start{};
    auto endOpen=[&]{if(open){sink->EndFigure(D2D1_FIGURE_END_OPEN);open=false;}};
    auto begin=[&](CanvasPoint point){endOpen();sink->BeginFigure(D2D1::Point2F(point.x,point.y),D2D1_FIGURE_BEGIN_FILLED);open=hasCurrent=true;current=start=point;};
    constexpr float pi=3.14159265358979323846f;
    for(const auto& segment:path){
        if(segment.verb==CanvasPathVerb::MoveTo){begin(segment.point);continue;}
        if(segment.verb==CanvasPathVerb::LineTo){
            if(!open)begin(hasCurrent?current:segment.point);
            sink->AddLine(D2D1::Point2F(segment.point.x,segment.point.y));current=segment.point;
            continue;
        }
        if(segment.verb==CanvasPathVerb::QuadraticCurveTo||segment.verb==CanvasPathVerb::BezierCurveTo){
            if(!open)begin(hasCurrent?current:segment.control1);
            if(segment.verb==CanvasPathVerb::QuadraticCurveTo)
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(
                    D2D1::Point2F(segment.control1.x,segment.control1.y),
                    D2D1::Point2F(segment.point.x,segment.point.y)));
            else sink->AddBezier(D2D1::BezierSegment(
                D2D1::Point2F(segment.control1.x,segment.control1.y),
                D2D1::Point2F(segment.control2.x,segment.control2.y),
                D2D1::Point2F(segment.point.x,segment.point.y)));
            current=segment.point;continue;
        }
        if(segment.verb==CanvasPathVerb::Close){
            if(open){sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false;current=start;}continue;
        }
        if(segment.radius<=0)continue;
        const auto pointAt=[&](float angle){return TransformCanvasPoint(segment.transform,
            segment.centerX+std::cos(angle)*segment.radius,
            segment.centerY+std::sin(angle)*segment.radius);};
        const auto arcStart=pointAt(segment.startAngle);
        if(!open)begin(hasCurrent?current:arcStart);
        if(std::abs(current.x-arcStart.x)>0.001f||std::abs(current.y-arcStart.y)>0.001f){
            sink->AddLine(D2D1::Point2F(arcStart.x,arcStart.y));current=arcStart;
        }
        float delta=segment.endAngle-segment.startAngle;
        if(!segment.counterClockwise){
            if(std::abs(delta)>=2*pi)delta=2*pi;else while(delta<0)delta+=2*pi;
        }else{
            if(std::abs(delta)>=2*pi)delta=-2*pi;else while(delta>0)delta-=2*pi;
        }
        if(std::abs(delta)<0.000001f)continue;
        const float radiusX=segment.radius*std::hypot(segment.transform.a,segment.transform.b);
        const float radiusY=segment.radius*std::hypot(segment.transform.c,segment.transform.d);
        const float rotation=std::atan2(segment.transform.b,segment.transform.a)*180.0f/pi;
        const float determinant=segment.transform.a*segment.transform.d-segment.transform.b*segment.transform.c;
        const bool clockwise=delta*(determinant<0?-1.0f:1.0f)>0;
        const int pieces=std::max(1,static_cast<int>(std::ceil(std::abs(delta)/pi)));
        for(int piece=1;piece<=pieces;++piece){
            const float angle=segment.startAngle+delta*piece/pieces;const auto end=pointAt(angle);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(end.x,end.y),
                D2D1::SizeF(radiusX,radiusY),rotation,
                D2D1_SWEEP_DIRECTION(clockwise?D2D1_SWEEP_DIRECTION_CLOCKWISE:D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE),
                D2D1_ARC_SIZE_SMALL));current=end;
        }
    }
    endOpen();if(FAILED(sink->Close()))return {};return geometry;
}

struct CanvasFont {
    std::wstring family=L"Arial";
    std::wstring koreanFamily;
    float size=10.0f;
    DWRITE_FONT_WEIGHT weight=DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STYLE style=DWRITE_FONT_STYLE_NORMAL;
};

float ImagePositionOffset(const std::wstring& token,float freeSpace,bool horizontal){
    const auto value=ToLower(Trim(token));
    if(value.empty()||value==L"center")return freeSpace*0.5f;
    if(value==(horizontal?L"left":L"top"))return 0;
    if(value==(horizontal?L"right":L"bottom"))return freeSpace;
    if(!value.empty()&&value.back()==L'%'){
        float percent=0;if(TryParseFloat(value.substr(0,value.size()-1),percent))
            return freeSpace*percent/100.0f;
    }
    return StyleSheet::Length(value,std::abs(freeSpace),std::abs(freeSpace),0);
}

void PaintRasterImage(ID2D1RenderTarget* target,const LayoutBox& box,
                      ID2D1RenderTarget*& cacheTarget,
                      FastMap<const RasterImageFrame*,Microsoft::WRL::ComPtr<ID2D1Bitmap>>& cache){
    const auto image=box.node?box.node->image:nullptr;
    if(!image||image->frames.empty()||image->frameIndex>=image->frames.size()||
       !image->width||!image->height||box.content.width<=0||box.content.height<=0)return;
    auto* bitmap=RasterBitmap(target,image,cacheTarget,cache);if(!bitmap)return;

    const float naturalWidth=static_cast<float>(image->width);
    const float naturalHeight=static_cast<float>(image->height);
    float width=box.content.width,height=box.content.height;
    const auto fit=ToLower(Trim(box.style.Get(L"object-fit",L"fill")));
    if(fit==L"contain"||fit==L"cover"||fit==L"none"||fit==L"scale-down"){
        const float containScale=std::min(box.content.width/naturalWidth,
                                          box.content.height/naturalHeight);
        if(fit==L"none") { width=naturalWidth;height=naturalHeight; }
        else if(fit==L"scale-down"){
            const float scale=std::min(1.0f,containScale);
            width=naturalWidth*scale;height=naturalHeight*scale;
        }else{
            const float scale=fit==L"cover"?
                std::max(box.content.width/naturalWidth,box.content.height/naturalHeight):
                containScale;
            width=naturalWidth*scale;height=naturalHeight*scale;
        }
    }

    std::wstring xToken=L"50%",yToken=L"50%";
    std::wistringstream position(box.style.Get(L"object-position",L"50% 50%"));
    std::wstring first,second;position>>first>>second;
    const auto normalizedFirst=ToLower(first);
    if(normalizedFirst==L"top"||normalizedFirst==L"bottom"){
        yToken=first;if(!second.empty())xToken=second;
    }else{
        if(!first.empty())xToken=first;if(!second.empty())yToken=second;
    }
    const float left=box.content.x+ImagePositionOffset(xToken,box.content.width-width,true);
    const float top=box.content.y+ImagePositionOffset(yToken,box.content.height-height,false);
    const auto destination=D2D1::RectF(left,top,left+width,top+height);
    const auto interpolation=box.style.Is(L"image-rendering",L"pixelated")||
        box.style.Is(L"image-rendering",L"crisp-edges")?
        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR:
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
    PushPaintClip(target,D2D1::RectF(box.content.x,box.content.y,
        box.content.x+box.content.width,box.content.y+box.content.height),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target->DrawBitmap(bitmap,destination,1.0f,interpolation,
        D2D1::RectF(0,0,naturalWidth,naturalHeight));
    PopPaintClip(target);
}

CanvasFont ParseCanvasFont(const std::wstring& source){
    CanvasFont result;const auto lower=ToLower(source);const auto px=lower.find(L"px");
    if(px==std::wstring::npos)return result;size_t begin=px;
    while(begin>0&&(std::iswdigit(source[begin-1])||source[begin-1]==L'.'))--begin;
    float parsed=0;if(TryParseFloat(source.substr(begin,px-begin),parsed))result.size=std::max(0.1f,parsed);
    auto familyList=Trim(source.substr(px+2));
    if(!familyList.empty())result.family=ResolveFontFamilyList(familyList,L"Arial");
    result.koreanFamily=ExplicitKoreanFontFamily(familyList);
    if(lower.find(L"bold")!=std::wstring::npos)result.weight=DWRITE_FONT_WEIGHT_BOLD;
    if(lower.find(L"italic")!=std::wstring::npos)result.style=DWRITE_FONT_STYLE_ITALIC;
    else if(lower.find(L"oblique")!=std::wstring::npos)result.style=DWRITE_FONT_STYLE_OBLIQUE;
    return result;
}

Microsoft::WRL::ComPtr<IDWriteTextLayout> CanvasTextLayout(IDWriteFactory* factory,const CanvasFont& parsed,std::wstring text){
    for(auto& character:text)if(character>=9&&character<=13)character=L' ';
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format;Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if(!factory||FAILED(factory->CreateTextFormat(parsed.family.c_str(),nullptr,parsed.weight,
            parsed.style,DWRITE_FONT_STRETCH_NORMAL,parsed.size,L"",&format)))return {};
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    if(FAILED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format.Get(),100000,100000,&layout)))return {};
    if(!parsed.koreanFamily.empty()&&ContainsHangul(text))for(size_t start=0;start<text.size();){
        if(!IsHangul(text[start])){++start;continue;}
        size_t end=start+1;while(end<text.size()&&IsHangul(text[end]))++end;
        layout->SetFontFamilyName(parsed.koreanFamily.c_str(),DWRITE_TEXT_RANGE{static_cast<UINT32>(start),static_cast<UINT32>(end-start)});start=end;
    }
    DWRITE_TEXT_METRICS metrics{};
    if(SUCCEEDED(layout->GetMetrics(&metrics))){layout->SetMaxWidth(std::max(1.0f,metrics.widthIncludingTrailingWhitespace));layout->SetMaxHeight(std::max(1.0f,metrics.height));}
    return layout;
}

float CanvasTextTop(const CanvasFont& font,const std::wstring& text,const std::wstring& baseline,IDWriteTextLayout* layout,const DWRITE_TEXT_METRICS& metrics){
    const auto value=ToLower(baseline);
    if(value==L"top"||value==L"hanging")return -font.size*(ContainsHangul(text)?0.30f:0.10f);
    if(value==L"middle")return -metrics.height/2;
    if(value==L"bottom"||value==L"ideographic")return -metrics.height;
    DWRITE_LINE_METRICS line{};UINT32 count=0;layout->GetLineMetrics(&line,1,&count);
    return -line.baseline;
}

class CanvasStrokeTextRenderer final:public IDWriteTextRenderer {
    LONG references_=1;
    ID2D1RenderTarget* target_;
    Microsoft::WRL::ComPtr<ID2D1Brush> brush_;
    float width_;
public:
    CanvasStrokeTextRenderer(ID2D1RenderTarget* target,ID2D1Brush* brush,float width):target_(target),brush_(brush),width_(width){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result)override{
        if(!result)return E_POINTER;*result=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDWritePixelSnapping)||iid==__uuidof(IDWriteTextRenderer)){
            *result=static_cast<IDWriteTextRenderer*>(this);AddRef();return S_OK;
        }return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return static_cast<ULONG>(InterlockedIncrement(&references_));}
    ULONG STDMETHODCALLTYPE Release()override{const auto count=InterlockedDecrement(&references_);if(!count)delete this;return static_cast<ULONG>(count);}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled)override{if(!disabled)return E_POINTER;*disabled=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* transform)override{
        if(!transform)return E_POINTER;D2D1_MATRIX_3X2_F matrix{};target_->GetTransform(&matrix);
        *transform={matrix._11,matrix._12,matrix._21,matrix._22,matrix._31,matrix._32};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* pixels)override{if(!pixels)return E_POINTER;*pixels=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*)override{
        if(!run||!run->fontFace)return E_INVALIDARG;
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;target_->GetFactory(&factory);
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        HRESULT result=factory->CreatePathGeometry(&geometry);if(FAILED(result))return result;
        result=geometry->Open(&sink);if(FAILED(result))return result;
        result=run->fontFace->GetGlyphRunOutline(run->fontEmSize,run->glyphIndices,run->glyphAdvances,
            run->glyphOffsets,run->glyphCount,run->isSideways,(run->bidiLevel&1)!=0,sink.Get());
        const auto closed=sink->Close();if(FAILED(result))return result;if(FAILED(closed))return closed;
        D2D1::Matrix3x2F previous;target_->GetTransform(&previous);
        target_->SetTransform(D2D1::Matrix3x2F::Translation(x,y)*previous);
        target_->DrawGeometry(geometry.Get(),brush_.Get(),width_);target_->SetTransform(previous);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT x,FLOAT y,const DWRITE_UNDERLINE* line,IUnknown*)override{
        if(!line)return E_INVALIDARG;target_->DrawLine(D2D1::Point2F(x,y+line->offset),D2D1::Point2F(x+line->width,y+line->offset),brush_.Get(),line->thickness);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT x,FLOAT y,const DWRITE_STRIKETHROUGH* line,IUnknown*)override{
        if(!line)return E_INVALIDARG;target_->DrawLine(D2D1::Point2F(x,y+line->offset),D2D1::Point2F(x+line->width,y+line->offset),brush_.Get(),line->thickness);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context,FLOAT x,FLOAT y,IDWriteInlineObject* object,BOOL sideways,BOOL rtl,IUnknown* effect)override{
        return object?object->Draw(context,this,x,y,sideways,rtl,effect):E_INVALIDARG;
    }
};

HRESULT ReplayCanvas(ID2D1RenderTarget* drawingTarget,IDWriteFactory* factory,const CanvasSurface& surface,bool clearBackground=true){
    // Canvas text is also web content. Its intrinsic 96-DPI backing store must
    // use the same grayscale coverage as DOM text before the bitmap is scaled
    // to the CSS box.
    ConfigureWebTextRendering(drawingTarget,factory);
    drawingTarget->BeginDraw();drawingTarget->SetTransform(D2D1::IdentityMatrix());
    if(clearBackground)drawingTarget->Clear(D2D1::ColorF(0,surface.alpha?0.0f:1.0f));
    for(const auto& command:surface.commands){
        const bool pathCommand=command.kind==CanvasCommandKind::FillPath||
                               command.kind==CanvasCommandKind::StrokePath;
        drawingTarget->SetTransform(pathCommand?D2D1::IdentityMatrix():CanvasMatrix(command.state.transform));
        if(command.kind==CanvasCommandKind::ClearRect){
            const float left=std::min(command.x,command.x+command.width);
            const float top=std::min(command.y,command.y+command.height);
            const float right=std::max(command.x,command.x+command.width);
            const float bottom=std::max(command.y,command.y+command.height);
            PushPaintClip(drawingTarget,D2D1::RectF(left,top,right,bottom),
                D2D1_ANTIALIAS_MODE_ALIASED);
            drawingTarget->Clear(D2D1::ColorF(0,surface.alpha?0.0f:1.0f));
            PopPaintClip(drawingTarget);
        }else if(command.kind==CanvasCommandKind::FillRect){
            const float left=std::min(command.x,command.x+command.width);
            const float top=std::min(command.y,command.y+command.height);
            const float right=std::max(command.x,command.x+command.width);
            const float bottom=std::max(command.y,command.y+command.height);
            const auto brush=CanvasBrush(drawingTarget,command.state.fillStyle,command.state.globalAlpha);
            if(brush)drawingTarget->FillRectangle(D2D1::RectF(left,top,right,bottom),brush.Get());
        }else if(pathCommand){
            const auto geometry=CanvasGeometry(drawingTarget,command.path);
            const auto brush=CanvasBrush(drawingTarget,command.kind==CanvasCommandKind::FillPath?
                command.state.fillStyle:command.state.strokeStyle,command.state.globalAlpha);
            if(geometry&&brush){
                if(command.kind==CanvasCommandKind::FillPath)drawingTarget->FillGeometry(geometry.Get(),brush.Get());
                else drawingTarget->DrawGeometry(geometry.Get(),brush.Get(),command.state.lineWidth);
            }
        }else if((command.kind==CanvasCommandKind::FillText||command.kind==CanvasCommandKind::StrokeText)&&!command.text.empty()){
            const auto parsed=ParseCanvasFont(command.state.font);
            const auto layout=CanvasTextLayout(factory,parsed,command.text);
            if(layout){
                DWRITE_TEXT_METRICS metrics{};layout->GetMetrics(&metrics);
                const float horizontalScale=command.width>0&&metrics.widthIncludingTrailingWhitespace>command.width?
                    command.width/metrics.widthIncludingTrailingWhitespace:1;
                const float textWidth=metrics.widthIncludingTrailingWhitespace*horizontalScale;
                float left=command.x,top=command.y;const auto align=ToLower(command.state.textAlign);
                if(align==L"center")left-=textWidth/2;
                else if(align==L"right"||align==L"end")left-=textWidth;
                top+=CanvasTextTop(parsed,command.text,command.state.textBaseline,layout.Get(),metrics);
                const auto brush=CanvasBrush(drawingTarget,command.kind==CanvasCommandKind::StrokeText?command.state.strokeStyle:command.state.fillStyle,command.state.globalAlpha);
                drawingTarget->SetTransform(D2D1::Matrix3x2F::Scale(horizontalScale,1)*
                    D2D1::Matrix3x2F::Translation(left,top)*CanvasMatrix(command.state.transform));
                if(brush){
                    if(command.kind==CanvasCommandKind::FillText)drawingTarget->DrawTextLayout(D2D1::Point2F(0,0),layout.Get(),brush.Get());
                    else{
                        Microsoft::WRL::ComPtr<IDWriteTextRenderer> renderer;
                        renderer.Attach(new CanvasStrokeTextRenderer(drawingTarget,brush.Get(),command.state.lineWidth));
                        layout->Draw(nullptr,renderer.Get(),0,0);
                    }
                }
            }
        }
    }
    drawingTarget->SetTransform(D2D1::IdentityMatrix());
    return drawingTarget->EndDraw();
}

#include "CanvasCompositing.inl"

void PaintCanvas(ID2D1RenderTarget* target,IDWriteFactory* factory,const LayoutBox& box){
    const auto surface=box.node?box.node->canvas:nullptr;
    if(!surface||surface->width==0||surface->height==0||
       box.content.width<=0||box.content.height<=0)return;
    // A browser rasterizes into the canvas's intrinsic backing store first and
    // then composites that bitmap into the CSS box. Replaying vector commands
    // directly into the page would make a 960x630 canvas scaled to 640x420
    // noticeably sharper and would change stroke/point pixels. Keeping this
    // intermediate bitmap also makes the result independent of monitor DPI.
    const auto intrinsicSize=D2D1::SizeF(static_cast<float>(surface->width),
                                         static_cast<float>(surface->height));
    const auto pixelSize=D2D1::SizeU(surface->width,surface->height);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if(NeedsCanvasCompositing(*surface)){
        Microsoft::WRL::ComPtr<IWICBitmap> pixels;
        if(!RasterizeCanvasBitmap(*surface,pixels)||FAILED(target->CreateBitmapFromWicBitmap(pixels.Get(),nullptr,&bitmap)))return;
    }else{
        Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> bitmapTarget;
        if(FAILED(target->CreateCompatibleRenderTarget(&intrinsicSize,&pixelSize,nullptr,
                D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,&bitmapTarget)))return;
        bitmapTarget->SetDpi(USER_DEFAULT_SCREEN_DPI,USER_DEFAULT_SCREEN_DPI);
        auto* drawingTarget=static_cast<ID2D1RenderTarget*>(bitmapTarget.Get());
        if(FAILED(ReplayCanvas(drawingTarget,factory,*surface))||FAILED(bitmapTarget->GetBitmap(&bitmap)))return;
    }
    PushPaintClip(target,D2D1::RectF(box.content.x,box.content.y,
        box.content.x+box.content.width,box.content.y+box.content.height),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target->DrawBitmap(bitmap.Get(),D2D1::RectF(box.content.x,box.content.y,
        box.content.x+box.content.width,box.content.y+box.content.height),1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        D2D1::RectF(0,0,static_cast<float>(surface->width),static_cast<float>(surface->height)));
    PopPaintClip(target);
}

std::wstring GeneratedContentText(const std::wstring& source,
                                  const std::shared_ptr<Node>& origin){
    std::wstring result;size_t position=0;
    while(position<source.size()){
        while(position<source.size()&&std::iswspace(source[position]))++position;
        if(position>=source.size())break;
        const auto quote=source[position];
        if(quote==L'\''||quote==L'"'){
            ++position;
            while(position<source.size()&&source[position]!=quote){
                if(source[position]==L'\\'&&position+1<source.size())++position;
                result+=source[position++];
            }
            if(position<source.size())++position;
            continue;
        }
        const auto remaining=ToLower(source.substr(position));
        if(remaining.rfind(L"attr(",0)==0){
            const auto close=source.find(L')',position+5);
            if(close==std::wstring::npos)break;
            auto name=Trim(source.substr(position+5,close-position-5));
            const auto separator=name.find_first_of(L" ,\t\r\n");
            if(separator!=std::wstring::npos)name=name.substr(0,separator);
            if(origin&&!name.empty())result+=origin->Attribute(ToLower(name));
            position=close+1;continue;
        }
        auto end=position;
        while(end<source.size()&&!std::iswspace(source[end]))++end;
        result+=source.substr(position,end-position);position=end;
    }
    // CSS strings and attr() are already literal DOM/CSS values. HTML
    // character-reference decoding here would decode attribute text twice.
    return result;
}

bool FindFirstLetterRun(const std::shared_ptr<Node>& node,
                        std::shared_ptr<Node>& textNode,
                        size_t& offset,size_t& length){
    if(!node)return false;
    if(node->type==NodeType::Text){
        for(size_t index=0;index<node->text.size();++index){
            const wchar_t character=node->text[index];
            if(std::iswspace(character))continue;
            textNode=node;offset=index;length=1;
            if(character>=0xd800&&character<=0xdbff&&index+1<node->text.size()&&
               node->text[index+1]>=0xdc00&&node->text[index+1]<=0xdfff)length=2;
            return true;
        }
        return false;
    }
    if(node->type!=NodeType::Element)return false;
    const auto tag=ToLower(node->tag);
    if(tag==L"script"||tag==L"style"||tag==L"template"||tag==L"head")return false;
    for(const auto& child:node->children)
        if(FindFirstLetterRun(child,textNode,offset,length))return true;
    return false;
}

} // namespace

bool SvgObjectBoundingBox(const std::shared_ptr<Node>& node,StyleSheet& styles,
                          LayoutRect& bounds,ID2D1Factory* factory,IDWriteFactory* writeFactory){
    bounds={};if(!node)return false;
    Microsoft::WRL::ComPtr<ID2D1Factory> ownedFactory;
    if(!factory){if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,ownedFactory.GetAddressOf())))return false;factory=ownedFactory.Get();}
    if(!writeFactory)writeFactory=SharedWriteFactory();
    std::vector<std::shared_ptr<Node>> ancestors;for(auto p=node->parent.lock();p;p=p->parent.lock())ancestors.push_back(p);
    ComputedStyle inherited;for(auto it=ancestors.rbegin();it!=ancestors.rend();++it)inherited=styles.Compute(*it,&inherited);
    D2D1_RECT_F total{};bool found=false;
    MeasureSvgObject(node,styles,ancestors.empty()?nullptr:&inherited,factory,writeFactory,D2D1::Matrix3x2F::Identity(),total,found);
    if(found)bounds={total.left,total.top,total.right-total.left,total.bottom-total.top};return true;
}

bool SvgTextAdvanceLength(const std::shared_ptr<Node>& node,StyleSheet& styles,
                          float& length,IDWriteFactory* writeFactory,std::vector<LayoutRect>* characters){
    length=0;if(characters)characters->clear();if(!node)return false;
    if(!writeFactory)writeFactory=SharedWriteFactory();if(!writeFactory)return false;
    std::vector<std::shared_ptr<Node>> ancestors;
    for(auto parent=node->parent.lock();parent;parent=parent->parent.lock())ancestors.push_back(parent);
    ComputedStyle inherited;
    for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
        inherited=styles.Compute(*it,&inherited);if(inherited.Is(L"display",L"none"))return true;
    }
    const auto style=styles.Compute(node,ancestors.empty()?nullptr:&inherited);
    const auto layout=SvgTextLayout(node,styles,style,writeFactory);if(!layout)return false;
    UINT32 count=0;layout->GetClusterMetrics(nullptr,0,&count);
    if(!count)return true;
    std::vector<DWRITE_CLUSTER_METRICS> clusters(count);
    if(FAILED(layout->GetClusterMetrics(clusters.data(),count,&count)))return false;
    for(const auto& cluster:clusters)length+=cluster.width;
    if(characters){
        DWRITE_TEXT_METRICS metrics{};DWRITE_LINE_METRICS line{};UINT32 lines=0;
        if(FAILED(layout->GetMetrics(&metrics))||FAILED(layout->GetLineMetrics(&line,1,&lines)))return false;
        auto number=[&](const wchar_t* name){return StyleSheet::Length(node->Attribute(name),24,FontSize(style),0);};
        float x=number(L"x")+number(L"dx"),y=number(L"y")+number(L"dy")-line.baseline;
        const auto anchor=style.Get(L"text-anchor",L"start");
        if(anchor==L"middle")x-=metrics.widthIncludingTrailingWhitespace/2;
        else if(anchor==L"end")x-=metrics.widthIncludingTrailingWhitespace;
        UINT32 position=0;
        for(const auto& cluster:clusters){
            FLOAT hitX=0,hitY=0;DWRITE_HIT_TEST_METRICS hit{};
            if(FAILED(layout->HitTestTextPosition(position,FALSE,&hitX,&hitY,&hit)))return false;
            // UTF-16 units sharing a shaped cluster share its glyph cell.
            // DirectWrite supplies visual positions for ligatures and bidi text.
            for(UINT16 unit=0;unit<cluster.length;++unit)
                characters->push_back({x+hit.left,y+hit.top,hit.width,hit.height});
            position+=cluster.length;
        }
    }
    return true;
}

bool MeasureCanvasText(const CanvasDrawingState& state,const std::wstring& text,CanvasTextMetrics& output){
    output={};const auto font=ParseCanvasFont(state.font);auto* factory=SharedWriteFactory();
    const auto layout=CanvasTextLayout(factory,font,text);if(!layout)return false;
    DWRITE_TEXT_METRICS metrics{};DWRITE_OVERHANG_METRICS overhang{};DWRITE_LINE_METRICS line{};UINT32 count=0;
    if(FAILED(layout->GetMetrics(&metrics))||FAILED(layout->GetOverhangMetrics(&overhang))||FAILED(layout->GetLineMetrics(&line,1,&count)))return false;
    output.width=metrics.widthIncludingTrailingWhitespace;
    const auto align=ToLower(state.textAlign);const double horizontal=align==L"center"?-output.width/2:
        align==L"right"||align==L"end"?-output.width:0;
    const double top=CanvasTextTop(font,text,state.textBaseline,layout.Get(),metrics),alphabetic=top+line.baseline;
    if(std::any_of(text.begin(),text.end(),[](wchar_t character){return character!=L' '&&!(character>=9&&character<=13);})){
        output.actualBoundingBoxLeft=overhang.left-horizontal;
        output.actualBoundingBoxRight=metrics.layoutWidth+overhang.right+horizontal;
        output.actualBoundingBoxAscent=overhang.top-top;
        output.actualBoundingBoxDescent=metrics.layoutHeight+overhang.bottom+top;
    }
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection;Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
    Microsoft::WRL::ComPtr<IDWriteFont> matched;UINT32 index=0;BOOL exists=FALSE;
    if(FAILED(factory->GetSystemFontCollection(&collection))||FAILED(collection->FindFamilyName(font.family.c_str(),&index,&exists))||!exists||
       FAILED(collection->GetFontFamily(index,&family))||FAILED(family->GetFirstMatchingFont(font.weight,DWRITE_FONT_STRETCH_NORMAL,font.style,&matched)))return false;
    DWRITE_FONT_METRICS face{};matched->GetMetrics(&face);if(!face.designUnitsPerEm)return false;
    const double ascent=font.size*face.ascent/face.designUnitsPerEm,descent=font.size*face.descent/face.designUnitsPerEm;
    output.fontBoundingBoxAscent=ascent-alphabetic;output.fontBoundingBoxDescent=descent+alphabetic;
    output.emHeightAscent=ascent-alphabetic;output.emHeightDescent=descent+alphabetic;
    output.hangingBaseline=ascent*0.8-alphabetic;output.alphabeticBaseline=-alphabetic;output.ideographicBaseline=-descent-alphabetic;
    return true;
}

bool ReadCanvasPixels(const CanvasSurface& surface,std::vector<unsigned char>& rgba){
    rgba.clear();
    if(surface.width==0||surface.height==0)return true;
    const auto bytes=static_cast<std::uint64_t>(surface.width)*surface.height*4;
    if(bytes>64*1024*1024)return false;
    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if(!RasterizeCanvasBitmap(surface,bitmap))return false;
    Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
    const WICRect bounds{0,0,static_cast<INT>(surface.width),static_cast<INT>(surface.height)};
    if(FAILED(bitmap->Lock(&bounds,WICBitmapLockRead,&lock)))return false;
    UINT stride=0,length=0;BYTE* pixels=nullptr;
    if(FAILED(lock->GetStride(&stride))||FAILED(lock->GetDataPointer(&length,&pixels))||
       static_cast<std::uint64_t>(stride)*(surface.height-1)+surface.width*4>length)return false;
    rgba.resize(static_cast<size_t>(bytes));
    for(unsigned y=0;y<surface.height;++y)for(unsigned x=0;x<surface.width;++x){
        const auto* source=pixels+static_cast<size_t>(y)*stride+x*4;
        auto* destination=rgba.data()+(static_cast<size_t>(y)*surface.width+x)*4;
        const unsigned alpha=source[3];destination[3]=static_cast<unsigned char>(alpha);
        for(unsigned channel=0;channel<3;++channel)
            destination[channel]=alpha?static_cast<unsigned char>(std::min(255u,(source[2-channel]*255u+alpha/2)/alpha)):0;
    }
    return true;
}

bool EncodeCanvasPng(const CanvasSurface& surface,std::vector<unsigned char>& png){
    png.clear();std::vector<unsigned char> rgba;
    if(!surface.width||!surface.height||!ReadCanvasPixels(surface,rgba))return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
    Microsoft::WRL::ComPtr<IStream> stream;
    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))||
       FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream))||
       FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))||
       FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))||
       FAILED(encoder->CreateNewFrame(&frame,nullptr))||FAILED(frame->Initialize(nullptr))||
       FAILED(frame->SetSize(surface.width,surface.height))||FAILED(frame->SetResolution(96,96)))return false;
    // WIC's PNG encoder accepts straight-alpha BGRA. The Canvas reader exposes
    // RGBA, so convert channel order without changing alpha or pixel geometry.
    for(size_t index=0;index<rgba.size();index+=4)std::swap(rgba[index],rgba[index+2]);
    auto format=GUID_WICPixelFormat32bppBGRA;
    if(FAILED(frame->SetPixelFormat(&format))||format!=GUID_WICPixelFormat32bppBGRA||
       FAILED(frame->WritePixels(surface.height,surface.width*4,static_cast<UINT>(rgba.size()),rgba.data()))||
       FAILED(frame->Commit())||FAILED(encoder->Commit()))return false;
    STATSTG information{};HGLOBAL memory=nullptr;
    if(FAILED(stream->Stat(&information,STATFLAG_NONAME))||information.cbSize.HighPart||
       FAILED(GetHGlobalFromStream(stream.Get(),&memory)))return false;
    const auto* data=static_cast<const unsigned char*>(GlobalLock(memory));if(!data)return false;
    png.assign(data,data+information.cbSize.LowPart);GlobalUnlock(memory);return true;
}

LayoutEngine::LayoutEngine(Document& document,StyleSheet& styleSheet):document_(document),styleSheet_(styleSheet){}
LayoutEngine::~LayoutEngine(){ClearOwnerBoundThreadCaches();}
void LayoutEngine::SetRasterImageResolver(RasterImageResolver resolver){rasterImageResolver_=std::move(resolver);}

static void PreserveInlineEdgeSpace(LayoutBox& box,bool leading){
    if(box.node->type==NodeType::Text){
        if(leading)box.preserveLeadingWhitespace=true;else box.preserveTrailingWhitespace=true;
        return;
    }
    if(!box.style.Is(L"display",L"inline")||IsAtomicInlineLevel(box))return;
    for(size_t offset=0;offset<box.children.size();++offset){
        auto& child=*box.children[leading?offset:box.children.size()-1-offset];
        if(!child.visible||child.style.Is(L"position",L"absolute")||child.style.Is(L"position",L"fixed"))continue;
        PreserveInlineEdgeSpace(child,leading);break;
    }
}

std::unique_ptr<LayoutBox> LayoutEngine::Build(const std::shared_ptr<Node>& node,const ComputedStyle* parentStyle,std::uint64_t parentContext,size_t siblingIndex,size_t siblingCount,const std::shared_ptr<Node>& previousElement){
    if(!node||node->type==NodeType::Comment)return {};
    auto box=std::make_unique<LayoutBox>();box->node=node;boxIndex_[node.get()]=box.get();
    auto treeRoot=node;while(const auto ancestor=treeRoot->parent.lock())treeRoot=ancestor;
    const bool shadowTree=!treeRoot->shadowHost.expired();
    const auto cacheKey=StyleContextHash(node,parentContext,styleSheet_,siblingIndex,siblingCount,previousElement);
    const auto cached=styleCache_.find(cacheKey);
    if(!shadowTree&&cached!=styleCache_.end())box->style=cached->second;
    else{
        box->style=styleSheet_.Compute(node,parentStyle);
        if(!shadowTree)styleCache_.emplace(cacheKey,box->style);
    }
    box->style.deviceScale=deviceScale_;box->visible=!box->style.Is(L"display",L"none");
    if(!box->visible)return box;
    if(node->type==NodeType::Element&&styleSheet_.HasPseudoRulesFor(node,L"first-letter")){
        std::shared_ptr<Node> textNode;size_t offset=0,length=0;
        if(FindFirstLetterRun(node,textNode,offset,length)){
            auto pseudoStyle=styleSheet_.Compute(node,&box->style,L"first-letter");
            pseudoStyle.deviceScale=deviceScale_;
            firstLetterRuns_[textNode.get()]={offset,length,std::move(pseudoStyle)};
        }
    }
    auto appendPseudo=[&](const wchar_t* name){
        if(!styleSheet_.HasPseudoRulesFor(node,name))return;
        auto pseudoStyle=styleSheet_.Compute(node,&box->style,name);pseudoStyle.deviceScale=deviceScale_;const auto content=Trim(pseudoStyle.Get(L"content"));
        if(content.empty()||content==L"none"||content==L"normal")return;
        auto generated=std::make_shared<Node>();generated->tag=L"span";generated->parent=node;
        auto pseudoBox=std::make_unique<LayoutBox>();pseudoBox->node=generated;pseudoBox->generatedFrom=node;
        pseudoBox->pseudo=name;pseudoBox->style=std::move(pseudoStyle);pseudoBox->visible=!pseudoBox->style.Is(L"display",L"none");
        const auto value=GeneratedContentText(content,node);
        if(pseudoBox->visible&&!value.empty()){
            auto text=std::make_shared<Node>();text->type=NodeType::Text;text->tag=L"#text";text->text=value;text->parent=generated;
            generated->children.push_back(text);auto textBox=Build(text,&pseudoBox->style,parentContext^HashText(name));
            if(textBox){textBox->parent=pseudoBox.get();pseudoBox->children.push_back(std::move(textBox));}
        }
        pseudoBox->parent=box.get();box->children.push_back(std::move(pseudoBox));
    };
    appendPseudo(L"before");
    if(node->namespaceUri.empty()&&node->tag==L"option"){
        // Option text is rendered through an internal label rather than its
        // light-DOM text node. Its pixels remain visible, while DOM Ranges do
        // not expose the internal label's glyph rectangles.
        const auto label=node->Attribute(L"label");
        const auto value=NormalizeText(label.empty()?node->InnerText():label,L"normal");
        if(!value.empty()){
            auto text=std::make_shared<Node>();text->type=NodeType::Text;text->tag=L"#text";
            text->text=value;text->parent=node;
            auto labelBox=Build(text,&box->style,cacheKey^HashText(L"internal-option-label"));
            if(labelBox){labelBox->pseudo=L"internal-option-label";labelBox->parent=box.get();box->children.push_back(std::move(labelBox));}
        }
    }
    if (node->namespaceUri.empty() && node->tag == L"optgroup") {
        // A visible optgroup has an internal label row even when label is
        // absent. Keep that generated row out of DOM text/range diagnostics.
        auto generated=std::make_shared<Node>();generated->tag=L"span";generated->parent=node;
        auto label=std::make_unique<LayoutBox>();label->node=generated;label->generatedFrom=node;
        label->pseudo=L"internal-optgroup-label";label->visible=true;label->parent=box.get();
        label->style=box->style;label->style.values=std::make_shared<ComputedStyle::ValueMap>(*box->style.values);
        auto& values=*label->style.values;
        values[L"display"]=L"block";values[L"position"]=L"static";values[L"width"]=L"auto";
        values[L"height"]=L"auto";
        // Store the internal row's minimum as a border-box extent, including
        // its 1px bottom padding; its external optgroup styles stay untouched.
        values[L"box-sizing"]=L"border-box";
        values[L"min-height"]=std::to_wstring(std::floor(FontSize(box->style)*1.2f*64)/64+1)+L"px";
        values[L"margin"]=L"0";values[L"padding"]=L"0 2px 1px";values[L"border"]=L"0";
        values[L"padding-top"]=L"0";values[L"padding-right"]=L"2px";
        values[L"padding-bottom"]=L"1px";values[L"padding-left"]=L"2px";
        values[L"background"]=L"transparent";values[L"background-color"]=L"transparent";
        const auto text=node->Attribute(L"label");
        if (!text.empty()) {
            auto textNode=std::make_shared<Node>();textNode->type=NodeType::Text;textNode->tag=L"#text";
            textNode->text=text;textNode->parent=generated;generated->children.push_back(textNode);
            auto textBox=Build(textNode,&label->style,cacheKey^HashText(L"internal-optgroup-label"));
            if(textBox){textBox->parent=label.get();label->children.push_back(std::move(textBox));}
        }
        box->children.push_back(std::move(label));
    }
    const auto parentDisplay=box->style.Get(L"display");
    const bool anonymousLayoutItem=parentDisplay==L"flex"||parentDisplay==L"inline-flex"||
        parentDisplay==L"grid"||parentDisplay==L"inline-grid";
    bool hasInlineContent=false;
    size_t elementCount=0;for(const auto& child:node->RenderChildren())if(child->type==NodeType::Element)++elementCount;
    const auto parentWhiteSpace=box->style.Get(L"white-space");
    // Keep ordinary paragraphs in one shaped run. Mixed inline content needs
    // break opportunities inside text siblings, rather than moving the whole
    // DOM text node to the next line after an atomic inline or styled span.
    const bool mixedInlineText=!anonymousLayoutItem&&!PreservesLineBreaks(parentWhiteSpace)&&
        !PreventsTextWrapping(parentWhiteSpace)&&!box->style.Is(L"direction",L"rtl")&&
        ((box->style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(*box)&&!IsBlockifiedItem(*box))||
        std::any_of(node->RenderChildren().begin(),node->RenderChildren().end(),
            [&](const auto& sibling){
                if(sibling->type!=NodeType::Element||sibling->tag==L"br")return false;
                const auto style=styleSheet_.Compute(sibling,&box->style);
                return IsInlineLevel(style.Get(L"display"))&&!style.Is(L"display",L"none")&&
                    !style.Is(L"position",L"absolute")&&!style.Is(L"position",L"fixed")&&
                    UsedFloatSide(style)==FloatSide::None;
            }));
    const auto hasRenderableText=[&](const std::wstring& text){
        // Flex and grid turn direct text runs into anonymous layout items, but
        // a run containing only document-formatting whitespace does not
        // generate an item.  This remains true when white-space: pre is used
        // on the container (for example, to preserve shortcut-label tabs).
        if(anonymousLayoutItem&&std::all_of(text.begin(),text.end(),
            [](wchar_t character){return std::iswspace(character)!=0;}))return false;
        const auto normalized=NormalizeText(text,parentWhiteSpace);
        return PreservesLineBreaks(parentWhiteSpace)?!normalized.empty():
            std::any_of(normalized.begin(),normalized.end(),[](wchar_t c){return !IsCollapsibleTextSpace(c);});
    };
    std::vector<bool> hasFollowingContent(node->RenderChildren().size());bool following=false;
    for(size_t index=node->RenderChildren().size();index>0;--index){const auto& child=node->RenderChildren()[index-1];hasFollowingContent[index-1]=following;if(child->type==NodeType::Text){if(hasRenderableText(child->text))following=true;}else if(child->type==NodeType::Element)following=true;}
    const auto appendBuilt=[&](std::unique_ptr<LayoutBox> built,
                               const std::shared_ptr<Node>& sourceNode,
                               bool preserveLeading,bool preserveTrailing){
        if(!built)return;
        built->parent=box.get();
        if(node->tag==L"select"&&!node->attributes.count(L"multiple")){
            float size=0;TryParseFloat(node->Attribute(L"size"),size);
            // The closed menu renders its selected label, while option/group
            // DOM styles remain queryable without painting popup descendants.
            if(size<=1)built->visible=false;
        }
        if(sourceNode&&sourceNode->type==NodeType::Text&&built->node!=sourceNode){
            built->generatedFrom=sourceNode;
            if(!boxIndex_.count(sourceNode.get()))boxIndex_[sourceNode.get()]=built.get();
        }
        const bool inlineLevel=built->node->type==NodeType::Text||IsInlineLevel(built->style.Get(L"display"));
        const bool outOfFlow=built->style.Is(L"position",L"absolute")||
            built->style.Is(L"position",L"fixed")||
            (!anonymousLayoutItem&&UsedFloatSide(built->style)!=FloatSide::None);
        if(built->node->type==NodeType::Text&&!anonymousLayoutItem){
            built->preserveLeadingWhitespace=preserveLeading;
            built->preserveTrailingWhitespace=preserveTrailing;
        }else if(!outOfFlow&&!anonymousLayoutItem&&built->style.Is(L"display",L"inline")){
            // Collapsed edge spaces inside nested inlines belong to the
            // surrounding line, including a space before a following control.
            if(preserveLeading)PreserveInlineEdgeSpace(*built,true);
            if(preserveTrailing)PreserveInlineEdgeSpace(*built,false);
        }
        // Out-of-flow elements do not make a whitespace-only sibling into an
        // inline run, nor interrupt real inline content on either side.
        if(!outOfFlow)hasInlineContent=inlineLevel;
        box->containsSticky=box->containsSticky||built->containsSticky;
        box->children.push_back(std::move(built));
    };
    size_t elementIndex=0;std::shared_ptr<Node> previousChildElement;
    for(size_t childIndex=0;childIndex<node->RenderChildren().size();++childIndex){auto& child=node->RenderChildren()[childIndex];
        if(node->tag==L"svg"||node->tag==L"textarea")break; // Replaced contents use their own paint/measurement path.
        if(node->namespaceUri.empty()&&node->tag==L"option")break;
        if(child->type==NodeType::Comment)continue;
        if(child->type==NodeType::Text&&!hasRenderableText(child->text)){
            // A collapsed space between inline siblings is still a text run.
            // Dropping whitespace-only DOM nodes joins an image or inline
            // box directly to the following label. Formatting whitespace
            // around block children and anonymous flex/grid items stays empty.
            if(anonymousLayoutItem||!hasInlineContent||!hasFollowingContent[childIndex])continue;
            bool nextInline=false;
            for(size_t next=childIndex+1;next<node->RenderChildren().size();++next){
                const auto& followingNode=node->RenderChildren()[next];
                if(followingNode->type==NodeType::Comment)continue;
                if(followingNode->type==NodeType::Text){
                    if(!hasRenderableText(followingNode->text))continue;
                    nextInline=true;break;
                }
                const auto followingStyle=styleSheet_.Compute(followingNode,&box->style);
                if(followingStyle.Is(L"display",L"none")||
                   followingStyle.Is(L"position",L"absolute")||
                   followingStyle.Is(L"position",L"fixed")||
                   UsedFloatSide(followingStyle)!=FloatSide::None)continue;
                nextInline=IsInlineLevel(followingStyle.Get(L"display"));break;
            }
            if(!nextInline)continue;
        }
        if(child->type==NodeType::Text){
            const auto firstLetter=firstLetterRuns_.find(child.get());
            if(firstLetter!=firstLetterRuns_.end()&&
               firstLetter->second.offset+firstLetter->second.length<=child->text.size()){
                const auto appendFragment=[&](size_t start,size_t length,
                                              const ComputedStyle* style){
                    if(!length)return;
                    auto fragment=std::make_shared<Node>();fragment->type=NodeType::Text;
                    fragment->tag=L"#text";fragment->text=child->text.substr(start,length);
                    fragment->parent=node;auto fragmentBox=Build(fragment,&box->style,cacheKey);
                    if(fragmentBox){
                        fragmentBox->textSourceOffset=start;
                        if(style){fragmentBox->style=*style;fragmentBox->style.deviceScale=deviceScale_;}
                    }
                    appendBuilt(std::move(fragmentBox),child,hasInlineContent,
                        start+length<child->text.size()||hasFollowingContent[childIndex]);
                };
                const auto& run=firstLetter->second;
                appendFragment(0,run.offset,nullptr);
                appendFragment(run.offset,run.length,&run.style);
                appendFragment(run.offset+run.length,
                    child->text.size()-run.offset-run.length,nullptr);
                continue;
            }
        }
        if(child->type==NodeType::Text&&PreservesLineBreaks(parentWhiteSpace)){
            const auto normalized=NormalizeText(child->text,parentWhiteSpace);
            if(normalized.find(L'\n')!=std::wstring::npos){
                size_t start=0;
                while(start<=normalized.size()){
                    const auto end=normalized.find(L'\n',start);
                    const auto length=(end==std::wstring::npos?normalized.size():end)-start;
                    if(length){
                        auto fragment=std::make_shared<Node>();fragment->type=NodeType::Text;
                        fragment->tag=L"#text";fragment->text=normalized.substr(start,length);fragment->parent=node;
                        auto fragmentBox=Build(fragment,&box->style,cacheKey);
                        if(fragmentBox)fragmentBox->textSourceOffset=start;
                        appendBuilt(std::move(fragmentBox),child,hasInlineContent,
                            end==std::wstring::npos&&hasFollowingContent[childIndex]);
                    }else if(end==std::wstring::npos&&start==normalized.size()&&start>0&&
                             normalized.back()==L'\n'&&ParticipatesInEditableContent(child)){
                        // A preserved trailing newline owns an empty editable
                        // line even though it has no DOM character after the
                        // break.  Keep a zero-width layout probe for that line
                        // so its height and DOM caret geometry exist before the
                        // user types the next character.
                        auto fragment=std::make_shared<Node>();fragment->type=NodeType::Text;
                        fragment->tag=L"#text";fragment->text=L"\x200b";fragment->parent=node;
                        auto fragmentBox=Build(fragment,&box->style,cacheKey);
                        if(fragmentBox)fragmentBox->textSourceOffset=start;
                        appendBuilt(std::move(fragmentBox),child,false,
                            hasFollowingContent[childIndex]);
                    }
                    if(end==std::wstring::npos)break;
                    auto lineBreak=std::make_shared<Node>();lineBreak->tag=L"br";lineBreak->parent=node;
                    appendBuilt(Build(lineBreak,&box->style,cacheKey),{},false,false);
                    hasInlineContent=false;start=end+1;
                }
                continue;
            }
        }
        if(child->type==NodeType::Text&&mixedInlineText&&
           child->text.find_first_of(L" \t\r\n\f")!=std::wstring::npos){
            size_t start=0;
            while(start<child->text.size()){
                size_t end=start;
                while(end<child->text.size()&&IsCollapsibleTextSpace(child->text[end]))++end;
                while(end<child->text.size()&&!IsCollapsibleTextSpace(child->text[end]))++end;
                auto fragment=std::make_shared<Node>();fragment->type=NodeType::Text;
                fragment->tag=L"#text";fragment->text=child->text.substr(start,end-start);
                fragment->parent=node;auto built=Build(fragment,&box->style,cacheKey);
                if(built){built->textSourceOffset=start;built->textFlowFragment=true;}
                appendBuilt(std::move(built),child,start>0||hasInlineContent,
                            end<child->text.size()||hasFollowingContent[childIndex]);
                start=end;
            }
            continue;
        }
        if(child->type==NodeType::Element)++elementIndex;
        auto built=Build(child,&box->style,cacheKey,child->type==NodeType::Element?elementIndex:0,elementCount,previousChildElement);if(child->type==NodeType::Element)previousChildElement=child;
        appendBuilt(std::move(built),child,hasInlineContent,hasFollowingContent[childIndex]);
    }
    appendPseudo(L"after");
    NormalizeTableChildren(*box);
    box->containsSticky=box->containsSticky||box->style.Is(L"position",L"sticky");
    return box;
}

void LayoutEngine::ApplyTransitions(LayoutBox& box){
    if(!box.generatedFrom&&box.node){
        const auto key=box.node.get();
        const auto definitions=TransitionDefinitions(box.style);
        const auto previous=transitionTargets_.find(key);
        auto active=transitions_.find(key);
        if(previous!=transitionTargets_.end()){
            std::vector<std::wstring> properties;
            bool includesAll=false;
            for(const auto& definition:definitions){
                if(definition.property==L"all")includesAll=true;
                else if(std::find(properties.begin(),properties.end(),definition.property)==properties.end())
                    properties.push_back(definition.property);
            }
            if(includesAll){
                for(const auto& item:*previous->second.values)
                    if(std::find(properties.begin(),properties.end(),item.first)==properties.end())properties.push_back(item.first);
                for(const auto& item:*box.style.values)
                    if(std::find(properties.begin(),properties.end(),item.first)==properties.end())properties.push_back(item.first);
            }
            for(const auto& property:properties){
                if(property.empty()||property.rfind(L"transition",0)==0||property.rfind(L"--",0)==0)continue;
                auto oldValue=TransitionBaseValue(previous->second,property,box.style.Get(property));
                auto newValue=TransitionBaseValue(box.style,property,oldValue);
                if(property==L"transform"){
                    if((oldValue.empty()||ToLower(oldValue)==L"none")&&newValue!=L"none")oldValue=IdentityTransform(newValue);
                    if((newValue.empty()||ToLower(newValue)==L"none")&&oldValue!=L"none")newValue=IdentityTransform(oldValue);
                }
                if(oldValue==newValue)continue;
                std::wstring currentValue=oldValue;
                if(active!=transitions_.end()){
                    for(const auto& transition:active->second)
                        if(transition.property==property){currentValue=TransitionValue(transition);break;}
                    active->second.erase(std::remove_if(active->second.begin(),active->second.end(),
                        [&](const StyleTransition& transition){return transition.property==property;}),active->second.end());
                }
                if(currentValue==newValue)continue;
                const auto* definition=TransitionFor(definitions,property);
                const bool discrete=property==L"visibility";
                if(!definition||(definition->durationMs<=0&&definition->delayMs<=0)||
                   (!discrete&&(!IsNumericTransitionProperty(property)||!CanInterpolateNumbers(currentValue,newValue))))continue;
                StyleTransition transition;
                transition.property=property;transition.from=std::move(currentValue);transition.to=std::move(newValue);
                transition.durationMs=definition->durationMs;transition.delayMs=definition->delayMs;
                transition.x1=definition->x1;transition.y1=definition->y1;
                transition.x2=definition->x2;transition.y2=definition->y2;transition.discrete=discrete;
                if(active==transitions_.end())active=transitions_.emplace(key,std::vector<StyleTransition>{}).first;
                active->second.push_back(std::move(transition));
            }
        }
        if(!definitions.empty()||active!=transitions_.end())transitionTargets_[key]=box.style;
        if(active!=transitions_.end()){
            active->second.erase(std::remove_if(active->second.begin(),active->second.end(),TransitionComplete),active->second.end());
            if(!active->second.empty()){
                box.style.values=std::make_shared<ComputedStyle::ValueMap>(*box.style.values);
                for(const auto& transition:active->second)(*box.style.values)[transition.property]=TransitionValue(transition);
            }else transitions_.erase(key);
        }
    }
    ApplyAnimations(box,true);
    box.relativeChildren.clear();
    for(auto& child:box.children){
        ApplyTransitions(*child);
        if(HasRelativeOffsets(*child))box.relativeChildren.push_back(child.get());
    }
    const auto display=box.style.Get(L"display");
    if(display==L"flex"||display==L"inline-flex"||display==L"grid"||display==L"inline-grid"){
        // Layout boxes are rebuilt in DOM order before styles/animations are
        // resolved. Stable CSS order therefore preserves source order for ties
        // while sharing the visual order across sizing, placement and painting.
        // The document's children and selector indices remain untouched.
        std::stable_sort(box.children.begin(),box.children.end(),[](const auto& left,const auto& right){
            const auto order=[](const ComputedStyle& style){
                return std::floor(StyleSheet::Length(style.Get(L"order",L"0"),0,0,0)+0.5);
            };
            return order(left->style)<order(right->style);
        });
    }
}

void LayoutEngine::ApplyAnimations(LayoutBox& box,bool updateDefinitions){
    if(box.generatedFrom||!box.node)return;
    const auto key=box.node.get();
    if(updateDefinitions){
        auto definitions=AnimationDefinitions(box.style);std::vector<StyleAnimation> active;
        const auto previous=animations_.find(key);
        for(size_t index=0;index<definitions.size();++index){
            auto animation=std::move(definitions[index]);
            if(animation.name.empty()||animation.name==L"none")continue;
            const auto frames=styleSheet_.FindKeyframes(box.node,animation.name);if(!frames)continue;
            animation.frames=frames->frames;
            if(previous!=animations_.end()&&index<previous->second.size()&&previous->second[index].signature==animation.signature)
                animation.elapsedMs=previous->second[index].elapsedMs;
            active.push_back(std::move(animation));
        }
        if(active.empty()){animations_.erase(key);animationTargets_.erase(key);return;}
        animationTargets_[key]=box.style;animations_[key]=std::move(active);
    }
    const auto active=animations_.find(key);if(active==animations_.end())return;
    for(const auto& animation:active->second)ApplyAnimationValues(box.style,animation);
}

bool LayoutEngine::HasActiveTransitions()const{
    for(const auto& item:transitions_)if(!item.second.empty())return true;
    for(const auto& item:animations_)for(const auto& animation:item.second)
        if(!animation.paused&&animation.durationMs>0&&animation.elapsedMs<animation.delayMs+animation.durationMs*animation.iterations)return true;
    return false;
}

void LayoutEngine::RefreshTransitionFrame(LayoutBox& box){
    if(!box.generatedFrom&&box.node){
        const auto key=box.node.get();auto active=transitions_.find(key);
        const auto animationTarget=animationTargets_.find(key);
        if(animationTarget!=animationTargets_.end())box.style=animationTarget->second;
        if(active!=transitions_.end()){
            const auto target=transitionTargets_.find(key);
            if(target!=transitionTargets_.end())box.style=target->second;
            const bool intrinsicChanged=std::any_of(active->second.begin(),active->second.end(),[](const StyleTransition& transition){
                return transition.property!=L"opacity"&&transition.property!=L"visibility"&&
                       transition.property!=L"transform"&&transition.property!=L"z-index";
            });
            active->second.erase(std::remove_if(active->second.begin(),active->second.end(),TransitionComplete),active->second.end());
            if(active->second.empty())transitions_.erase(key);
            else{
                box.style.values=std::make_shared<ComputedStyle::ValueMap>(*box.style.values);
                for(const auto& transition:active->second)(*box.style.values)[transition.property]=TransitionValue(transition);
            }
            if(intrinsicChanged)for(auto* current=&box;current;current=current->parent){
                current->naturalWidthValid=false;current->minimumWidthValid=false;current->intrinsicMinimumWidthValid=false;current->maxContentWidthValid=false;current->minContentWidthValid=false;current->naturalHeightValid=false;
                if(current->tableGrid){current->tableGrid->intrinsicColumnsValid=false;
                    current->tableGrid->columnsValid=false;current->tableGrid->rowsValid=false;}
                current->blockMarginsValid=false;
                current->inFlowBlockChildrenValid=false;
            }
        }
        ApplyAnimations(box,false);
    }
    box.relativeChildren.clear();
    for(auto& child:box.children){
        RefreshTransitionFrame(*child);
        if(HasRelativeOffsets(*child))box.relativeChildren.push_back(child.get());
    }
}

bool LayoutEngine::AdvanceTransitions(float milliseconds){
    milliseconds=std::max(0.0f,milliseconds);
    for(auto& item:transitions_)for(auto& transition:item.second){
        transition.elapsedMs+=milliseconds;
    }
    for(auto& item:animations_)for(auto& animation:item.second)
        if(!animation.paused)animation.elapsedMs+=milliseconds;
    if(root_){
        RefreshTransitionFrame(*root_);
        LayoutRoot();
        UpdateTraversalMetadata(*root_);
        UpdateStackingContexts(*root_);
    }
    return HasActiveTransitions();
}

void LayoutEngine::ClearTransitions(){
    transitionTargets_.clear();transitions_.clear();animationTargets_.clear();animations_.clear();
}

void LayoutEngine::DiscardDeviceResources(){
    imageBitmapCache_.clear();imageBitmapCacheTarget_=nullptr;
    shadowBitmapCache_.clear();shadowBitmapCacheTarget_=nullptr;
    brushCache_.clear();brushCacheTarget_=nullptr;
}

void LayoutEngine::Layout(float width,float height,float deviceScale){
    imageBitmapCache_.clear();imageBitmapCacheTarget_=nullptr;
    deviceScale_=std::max(0.01f,deviceScale);
    viewportWidth_=std::max(1.0f,width);viewportHeight_=std::max(1.0f,height);
    styleSheet_.SetViewport(viewportWidth_,viewportHeight_);
    if(styleCacheVersion_!=styleSheet_.Version()||styleCache_.size()>8192){
        styleCache_.clear();styleCacheVersion_=styleSheet_.Version();
    }
    canvasBackgroundBox_=nullptr;canvasHtmlBox_=nullptr;canvasBodyBox_=nullptr;
    boxIndex_.clear();firstLetterRuns_.clear();auto rootNode=document_.Body();if(!rootNode)rootNode=document_.Root();
    std::uint64_t context=1469598103934665603ull;
    std::vector<std::shared_ptr<Node>> ancestors;
    for(auto ancestor=rootNode->parent.lock();ancestor;ancestor=ancestor->parent.lock())ancestors.push_back(ancestor);
    std::vector<ComputedStyle> ancestorStyles;ancestorStyles.reserve(ancestors.size());
    const ComputedStyle* inheritedStyle=nullptr;
    for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
        context=StyleContextHash(*it,context,styleSheet_);
        ancestorStyles.push_back(styleSheet_.Compute(*it,inheritedStyle));
        inheritedStyle=&ancestorStyles.back();
    }
    root_=Build(rootNode,inheritedStyle,context);if(!root_)return;
    const auto rememberCanvasBox=[&](const std::shared_ptr<Node>& node,const LayoutBox*& box){
        if(!node)return;const auto found=boxIndex_.find(node.get());
        if(found!=boxIndex_.end())box=found->second;
    };
    rememberCanvasBox(document_.QuerySelector(L"html"),canvasHtmlBox_);
    rememberCanvasBox(document_.Body(),canvasBodyBox_);
    ApplyTransitions(*root_);
    std::vector<const Node*> removed;
    for(const auto& item:transitionTargets_)if(!boxIndex_.count(item.first))removed.push_back(item.first);
    for(const auto key:removed){transitionTargets_.erase(key);transitions_.erase(key);}
    removed.clear();
    for(const auto& item:animationTargets_)if(!boxIndex_.count(item.first))removed.push_back(item.first);
    for(const auto key:removed){animationTargets_.erase(key);animations_.erase(key);}
    // CSS propagates overflow from the root element, or from body while the
    // root remains visible, to the viewport. A visible propagated value is
    // used as auto by the viewport so an ordinary tall document scrolls even
    // without an authored overflow declaration.
    const ComputedStyle* viewportOverflowStyle=&root_->style;
    if(root_->node&&root_->node->tag==L"body"&&inheritedStyle){
        const auto htmlOverflow=inheritedStyle->Get(L"overflow",L"visible");
        const auto htmlX=ToLower(Trim(inheritedStyle->Get(L"overflow-x",htmlOverflow)));
        const auto htmlY=ToLower(Trim(inheritedStyle->Get(L"overflow-y",htmlOverflow)));
        if(htmlX!=L"visible"||htmlY!=L"visible")viewportOverflowStyle=inheritedStyle;
    }
    const auto propagatedOverflow=[&](const wchar_t* axis){
        const auto shorthand=viewportOverflowStyle->Get(L"overflow",L"visible");
        auto value=ToLower(Trim(viewportOverflowStyle->Get(axis,shorthand)));
        if(value==L"clip")return std::wstring(L"hidden");
        return value.empty()||value==L"visible"?std::wstring(L"auto"):value;
    };
    root_->viewportScrollContainer=true;
    root_->viewportScrollport={0,0,viewportWidth_,viewportHeight_};
    root_->viewportOverflowX=propagatedOverflow(L"overflow-x");
    root_->viewportOverflowY=propagatedOverflow(L"overflow-y");
    // Root scrollbar styling/gutters apply to the viewport. Body's overflow
    // can propagate, but its scrollbar-gutter does not propagate with it.
    if(root_->node&&root_->node->tag==L"body"&&inheritedStyle){
        for(const auto* property:{L"scrollbar-width",L"scrollbar-color",L"scrollbar-gutter"})
            (*root_->style.values)[property]=inheritedStyle->Get(property,L"auto");
    }
    LayoutRoot();
    UpdateTraversalMetadata(*root_);
    UpdateStackingContexts(*root_);
    UpdateTopLayer();
}

void LayoutEngine::InvalidateMeasurements(LayoutBox& box){
    box.naturalWidthValid=false;box.minimumWidthValid=false;box.intrinsicMinimumWidthValid=false;box.maxContentWidthValid=false;box.minContentWidthValid=false;box.naturalHeightValid=false;
    box.collapsedBordersResolved=false;box.trimLeadingLineWhitespace=false;
    box.tableGrid.reset();
    box.blockMarginsValid=false;
    box.inFlowBlockChildrenValid=false;
    box.stickyFlowYValid=false;
    for(auto& child:box.children)InvalidateMeasurements(*child);
}

void LayoutEngine::Relayout(float width,float height,float deviceScale){
    width=std::max(1.0f,width);height=std::max(1.0f,height);
    deviceScale=std::max(0.01f,deviceScale);
    const auto previousStyleVersion=styleSheet_.Version();
    styleSheet_.SetViewport(width,height);
    if(!root_||styleSheet_.Version()!=previousStyleVersion||std::abs(deviceScale-deviceScale_)>0.0001f){
        Layout(width,height,deviceScale);
        return;
    }
    viewportWidth_=width;viewportHeight_=height;
    InvalidateMeasurements(*root_);
    root_->viewportScrollport={0,0,viewportWidth_,viewportHeight_};
    LayoutRoot();
    UpdateTraversalMetadata(*root_);
    UpdateStackingContexts(*root_);
    UpdateTopLayer();
}

void LayoutEngine::LayoutRoot(){
    if(!root_)return;
    const auto x=OverflowX(*root_),y=OverflowY(*root_);
    const bool stable=HasStableScrollbarGutter(root_->style)&&(y==L"auto"||y==L"scroll"||y==L"hidden");
    const bool both=stable&&(ScrollbarGutterFlags(root_->style)&2)!=0;
    bool reserveVertical=y==L"scroll"||stable,reserveHorizontal=x==L"scroll";
    const bool rtl=root_->style.Is(L"direction",L"rtl");
    const float vertical=VerticalScrollbarMetricsFor(*root_,styleSheet_).width;
    const float horizontal=HorizontalScrollbarMetricsFor(*root_,styleSheet_).height;
    // Settle the two axes after normal layout. A vertical scrollbar can
    // introduce horizontal overflow, and that bar can introduce vertical
    // overflow. Keep media queries/vw on the original viewport CSS dimensions.
    for(unsigned pass=0;pass<3;++pass){
    root_->viewportGutterLeft=reserveVertical&&(both||rtl)?vertical:0;
    root_->viewportGutterRight=reserveVertical&&(both||!rtl)?vertical:0;
    root_->viewportGutterBottom=reserveHorizontal?horizontal:0;
    const float clientWidth=ScrollClientWidth(*root_),clientHeight=ScrollClientHeight(*root_);
    if(pass)InvalidateMeasurements(*root_);
    if(root_->node&&root_->node->tag==L"body"){
        const auto height=ToLower(Trim(root_->style.Get(L"height",L"auto")));
        const bool authoredHeight=!height.empty()&&height!=L"auto";
        const bool definiteHeight=authoredHeight||document_.QuirksMode();
        const float outerWidth=BlockOuterWidth(*root_,clientWidth,viewportWidth_);
        float outerHeight=authoredHeight?
            BlockOuterHeight(*root_,clientHeight,clientWidth,clientHeight,viewportWidth_):
            NaturalHeight(*root_,std::max(1.0f,clientWidth));
        if(!authoredHeight&&document_.QuirksMode())outerHeight=std::max(outerHeight,clientHeight);
        LayoutBoxTree(*root_,{root_->viewportGutterLeft,0,outerWidth,outerHeight},true,true,definiteHeight);
    }else LayoutBoxTree(*root_,{root_->viewportGutterLeft,0,clientWidth,clientHeight},true);
    const bool nextVertical=reserveVertical||(y==L"auto"&&root_->scrollHeight>clientHeight+1);
    const bool nextHorizontal=reserveHorizontal||(x==L"auto"&&root_->scrollWidth>clientWidth+1);
    if(nextVertical==reserveVertical&&nextHorizontal==reserveHorizontal)break;
    reserveVertical=nextVertical;reserveHorizontal=nextHorizontal;
    }
}

void LayoutEngine::LayoutBoxTree(LayoutBox& box,const LayoutRect& available,bool forcedSize,
                                 bool definiteWidth,bool definiteHeight){
    if(!box.visible)return;
    box.inlineTextPositioned=false;
    box.inlineFlowFragments=false;
    box.inlineContinuationValid=false;
    box.overflowClipValid=false;
    box.definiteContentHeight=definiteHeight;
    box.relativeOffsetX=box.relativeOffsetY=0;
    if(box.node->type==NodeType::Text){
        // Text nodes do not generate an independently stylable CSS box. Their
        // inline/block parent has already resolved the content rectangle, so
        // running element margin, border, overflow and transform resolution is
        // both redundant and observably expensive in large code/list views.
        box.rect=available;
        box.content=available;
        box.clipsOverflow=false;box.overflowFlagsValid=true;
        return;
    }
    const bool flexItem=box.parent&&(box.parent->style.Is(L"display",L"flex")||
        box.parent->style.Is(L"display",L"inline-flex"))&&
        !box.style.Is(L"position",L"absolute")&&!box.style.Is(L"position",L"fixed");
    // A shrink-sized box still resolves percentage edges against its containing
    // block, rather than against its already resolved margin-box width.
    const bool intrinsicFlowBox=forcedSize&&HasIntrinsicWidth(box)&&box.parent&&
        !box.style.Is(L"position",L"absolute")&&!box.style.Is(L"position",L"fixed")&&
        !box.parent->style.Is(L"display",L"grid")&&!box.parent->style.Is(L"display",L"inline-grid");
    const float edgeReference=flexItem||intrinsicFlowBox?box.parent->content.width:available.width;
    auto margin=UsedLayoutMargins(box,edgeReference,viewportWidth_);
    auto padding=EdgeValues(box.style,L"padding",edgeReference,viewportWidth_);
    const auto border=UsedBorderValues(box);float x=available.x+margin.left,y=available.y+margin.top;
    float width=std::max(0.0f,available.width-margin.left-margin.right),height=std::max(0.0f,available.height-margin.top-margin.bottom);
    auto cssWidth=box.style.Get(L"width"),cssHeight=box.style.Get(L"height");
    const bool intrinsicWidth=!forcedSize&&HasIntrinsicWidth(box);
    if(intrinsicWidth)width=UsedIntrinsicWidth(box,cssWidth,available.width,viewportWidth_);
    else if(!forcedSize&& !cssWidth.empty()&&cssWidth!=L"auto")width=StyleSheet::Length(cssWidth,available.width,viewportWidth_,width);
    if(!forcedSize&& !cssHeight.empty()&&cssHeight!=L"auto")height=StyleSheet::Length(cssHeight,available.height,viewportHeight_,height);
    const bool borderBox=box.style.Is(L"box-sizing",L"border-box");if(!borderBox&&!forcedSize){if(!intrinsicWidth&&!cssWidth.empty()&&cssWidth!=L"auto")width+=padding.left+padding.right+border.left+border.right;if(!cssHeight.empty()&&cssHeight!=L"auto")height+=padding.top+padding.bottom+border.top+border.bottom;}
    if(IsTable(box)){
        const float edges=padding.left+padding.right+border.left+border.right;
        const bool fixed=box.style.Is(L"table-layout",L"fixed")&&!cssWidth.empty()&&cssWidth!=L"auto";
        width=std::max(width,(fixed?FixedTableMinimumWidth(box,std::max(0.0f,width-edges),viewportWidth_):
            TableIntrinsicWidth(box,true))+edges);
        height=std::max(height,NaturalHeight(box,width)-margin.top-margin.bottom);
    }
    // An inline split by explicit/preserved breaks owns font boxes at its first and last
    // lines, while its parent reserves the full line-height for normal flow.
    // Keeping both outer half-leading portions in the inline rectangle adds
    // false scroll overflow below a preserved newline inside a span.
    if(forcedSize&&box.style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(box)&&
       !IsBlockifiedItem(box)&&!HasInFlowBlockChildren(box)&&
       !UsesGenericMonospaceMetrics(box.style)&&
       height>LineHeight(box.style)+padding.top+padding.bottom+border.top+border.bottom+0.01f&&
       std::any_of(box.children.begin(),box.children.end(),
           [](const auto& child){return child->visible&&child->node->tag==L"br";}))
        height=std::max(0.0f,height-2*InlineHalfLeading(box.style));
    box.rect={x,y,std::max(0.0f,width),std::max(0.0f,height)};box.content={x+border.left+padding.left,y+border.top+padding.top,std::max(0.0f,width-border.left-border.right-padding.left-padding.right),std::max(0.0f,height-border.top-border.bottom-padding.top-padding.bottom)};
    box.verticalGutterReserved=false;box.scrollbarGutterLeft=box.scrollbarGutterRight=0;
    box.horizontalGutterReserved=false;box.scrollbarGutterBottom=0;
    box.autoScrollbarHeightExpansion=0;
    if(box.viewportScrollContainer)box.verticalGutterReserved=true;
    const auto overflowY=OverflowY(box),overflowX=OverflowX(box);
    const auto clips=[](const std::wstring& value){
        return value==L"hidden"||value==L"clip"||value==L"auto"||value==L"scroll";
    };
    // This axis is already resolved for layout. Reuse it instead of looking
    // up both axes again for every element in the metadata pass.
    box.clipsOverflow=clips(overflowY)||clips(OverflowX(box));
    box.overflowFlagsValid=true;
    if(!box.viewportScrollContainer&&
       (overflowY==L"scroll"||((overflowY==L"auto"||overflowY==L"hidden")&&
       HasStableScrollbarGutter(box.style)))){
        box.verticalGutterReserved=true;
        const float gutter=VerticalScrollbarMetricsFor(box,styleSheet_).width;
        const bool both=HasStableScrollbarGutter(box.style)&&(ScrollbarGutterFlags(box.style)&2)!=0;
        const bool rtl=box.style.Is(L"direction",L"rtl");
        box.scrollbarGutterLeft=both||rtl?gutter:0;
        box.scrollbarGutterRight=both||!rtl?gutter:0;
        box.content.x+=box.scrollbarGutterLeft;
        box.content.width=std::max(0.0f,box.content.width-
            box.scrollbarGutterLeft-box.scrollbarGutterRight);
    }
    if(!box.viewportScrollContainer&&box.node->tag!=L"textarea"&&overflowX==L"scroll"){
        box.scrollbarGutterBottom=HorizontalScrollbarMetricsFor(box,styleSheet_).height;
        box.horizontalGutterReserved=box.scrollbarGutterBottom>0;
        if(AutoScrollbarHeight(box,definiteHeight)){
            box.rect.height+=box.scrollbarGutterBottom;box.autoScrollbarHeightExpansion=box.scrollbarGutterBottom;
        }else box.content.height=std::max(0.0f,box.content.height-box.scrollbarGutterBottom);
    }
    float arrangeHeight=box.rect.height,arrangeContentHeight=box.content.height;
    float ownAutoExpansion=box.autoScrollbarHeightExpansion;
    const float requestedScrollLeft=box.node->scrollLeft,requestedScrollTop=box.node->scrollTop;
    const auto display=box.style.Get(L"display");
    const auto arrange=[&]{
        box.rect.height=arrangeHeight;box.content.height=arrangeContentHeight;
        box.autoScrollbarHeightExpansion=ownAutoExpansion;
        // Intermediate passes must not erase a scroll offset before the
        // final pair of gutters exposes its full scroll range.
        box.node->scrollLeft=requestedScrollLeft;box.node->scrollTop=requestedScrollTop;
        if(display==L"flex"||display==L"inline-flex")LayoutFlex(box);
        else if(display==L"grid"||display==L"inline-grid")LayoutGrid(box,definiteWidth,definiteHeight);
        else if(IsTable(box))LayoutTable(box);
        else if(CanLayoutColumns(box))LayoutColumns(box);
        else LayoutBlock(box,definiteHeight);
        for(auto* child:box.relativeChildren)ApplyRelativePosition(*child,viewportWidth_,viewportHeight_);
        FinalizeScroll(box);
    };
    arrange();
    // Intrinsic child heights include margins and are not an overflow test:
    // adjacent margins can collapse, and inline content shares line boxes.
    // Either bar can cause overflow on the other axis. Reserve each at most
    // once, rerunning from normal child coordinates after each decision.
    if(!box.viewportScrollContainer&&box.node->tag!=L"textarea")for(int pass=0;pass<2;++pass){
        bool changed=false;
        if(!box.verticalGutterReserved&&overflowY==L"auto"&&box.scrollHeight>ScrollClientHeight(box)+1){
            const float gutter=VerticalScrollbarMetricsFor(box,styleSheet_).width;
            if(gutter>0){
            box.verticalGutterReserved=true;
            const bool rtl=box.style.Is(L"direction",L"rtl");
            box.scrollbarGutterLeft=rtl?gutter:0;
            box.scrollbarGutterRight=rtl?0:gutter;
            box.content.x+=box.scrollbarGutterLeft;
            box.content.width=std::max(0.0f,box.content.width-gutter);
            changed=true;
            }
        }
        if(!box.horizontalGutterReserved&&overflowX==L"auto"&&box.scrollWidth>ScrollClientWidth(box)+1){
            const float gutter=HorizontalScrollbarMetricsFor(box,styleSheet_).height;
            if(gutter>0){box.horizontalGutterReserved=true;box.scrollbarGutterBottom=gutter;
                if(AutoScrollbarHeight(box,definiteHeight)){arrangeHeight+=gutter;ownAutoExpansion+=gutter;}
                else arrangeContentHeight=std::max(0.0f,arrangeContentHeight-gutter);
                changed=true;}
        }
        if(!changed)break;
        arrange();
    }
    ApplyTransform(box,viewportWidth_,viewportHeight_);
}

void LayoutEngine::UpdateTraversalMetadata(LayoutBox& box){
    for(auto& child:box.children)UpdateTraversalMetadata(*child);
    box.splitInlineRectValid=false;
    box.splitInlinePaintOffsets.clear();
    if(box.node&&box.node->type==NodeType::Element&&box.style.Is(L"display",L"inline")&&
       !IsAtomicInlineLevel(box)&&!IsBlockifiedItem(box)&&
       (HasInFlowBlockChildren(box)||box.inlineFlowFragments)){
        LayoutRect fragments{};
        for(const auto& child:box.children){
            if(!child->visible||child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))continue;
            auto fragment=child->rect;
            if(child->node->type==NodeType::Text){
                fragment.y+=InlineHalfLeading(child->style);
                fragment.height=std::max(0.0f,fragment.height-LineHeight(child->style)+InlineContentBoxHeight(child->style));
            }else if(child->splitInlineRectValid){
                fragment=child->splitInlineRectOffsets;fragment.x+=child->rect.x;fragment.y+=child->rect.y;
            }
            if(child->splitInlineRectValid){
                for(auto paint:child->splitInlinePaintOffsets){
                    paint.x+=child->rect.x-box.rect.x;paint.y+=child->rect.y-box.rect.y;
                    box.splitInlinePaintOffsets.push_back(paint);
                }
            }else if(IsInlineLevel(child->style.Get(L"display"))){
                auto paint=fragment;paint.x-=box.rect.x;paint.y-=box.rect.y;
                box.splitInlinePaintOffsets.push_back(paint);
            }
            if(!box.splitInlineRectValid){fragments=fragment;box.splitInlineRectValid=true;}
            else{
                const float right=std::max(fragments.x+fragments.width,fragment.x+fragment.width);
                const float bottom=std::max(fragments.y+fragments.height,fragment.y+fragment.height);
                fragments.x=std::min(fragments.x,fragment.x);fragments.y=std::min(fragments.y,fragment.y);
                fragments.width=right-fragments.x;fragments.height=bottom-fragments.y;
            }
        }
        fragments.x-=box.rect.x;fragments.y-=box.rect.y;
        box.splitInlineRectOffsets=fragments;
    }
    if(box.node&&box.node->type==NodeType::Text){
        box.cssOverflowRight=box.cssScrollWidth=box.rect.width;
        box.cssOverflowBottom=box.cssScrollHeight=box.rect.height;
    }else{
    const auto borders=UsedBorderValues(box);
    const auto padding=EdgeValues(box.style,L"padding",box.parent?box.parent->content.width:viewportWidth_,viewportWidth_);
    const float clientWidth=std::max(0.0f,box.rect.width-borders.left-borders.right-
        box.scrollbarGutterLeft-box.scrollbarGutterRight);
    const float clientHeight=std::max(0.0f,box.rect.height-borders.top-borders.bottom-box.scrollbarGutterBottom);
    const bool rtl=box.style.Is(L"direction",L"rtl");
    const auto overflowY=OverflowY(box),overflowX=OverflowX(box);
    const bool clipX=overflowX==L"clip";
    const float unusedGutters=box.verticalGutterReserved?
        box.scrollbarGutterLeft+box.scrollbarGutterRight-
        (overflowY==L"scroll"?std::max(box.scrollbarGutterLeft,box.scrollbarGutterRight):0):0;
    const float rtlOrigin=box.content.x+box.content.width+padding.right+unusedGutters;
    float left=rtlOrigin-clientWidth,overflowLeft=box.rect.x;
    float right=box.rect.x+borders.left+box.scrollbarGutterLeft+clientWidth;
    float bottom=box.rect.y+borders.top+clientHeight;
    const LayoutBox* lastFlowChild=nullptr;
    for(const auto& child:box.children)if(child->visible&&
        !child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")&&
        child->node&&child->node->type==NodeType::Element)lastFlowChild=child.get();
    for(const auto& child:box.children){
        if(!child->visible||child->style.Is(L"position",L"fixed"))continue;
        // An absolute child attached to an outer containing block does not
        // enlarge this element's CSSOM scrolling area (for example a static
        // body containing only viewport-positioned color calibration boxes).
        if(child->style.Is(L"position",L"absolute")&&AbsoluteContainingBlockAncestor(*child)!=&box)continue;
        const auto margin=child->node&&child->node->type==NodeType::Text?Edges{}:
            UsedLayoutMargins(*child,box.content.width,viewportWidth_);
        const auto overflow=child->style.Get(L"overflow",L"visible");
        const bool visibleX=child->style.Get(L"overflow-x",overflow)==L"visible";
        const bool visibleY=child->style.Get(L"overflow-y",overflow)==L"visible";
        // Relative positioning extends overflow; moving an item towards the
        // start edge must not discard the region occupied by its normal flow.
        const float childRight=child->rect.x+(visibleX?child->cssOverflowRight:child->rect.width)-(clipX?0:std::min(0.0f,child->relativeOffsetX));
        const float childBottom=child->rect.y+(visibleY?child->cssOverflowBottom:child->rect.height)-(overflowY==L"clip"?0:std::min(0.0f,child->relativeOffsetY));
        const float childLeft=child->rect.x+(visibleX?child->cssOverflowLeft:0)-std::max(0.0f,child->relativeOffsetX);
        left=std::min(left,childLeft+box.appliedScrollLeft);
        overflowLeft=std::min(overflowLeft,childLeft+box.appliedScrollLeft);
        const bool positioned=child->style.Is(L"position",L"absolute");
        const bool collapsedEnd=box.collapseLastChildMargin&&child.get()==lastFlowChild;
        right=std::max(right,childRight+box.appliedScrollLeft+std::max(0.0f,margin.right));
        bottom=std::max(bottom,childBottom+box.appliedScrollTop+(collapsedEnd?0:std::max(0.0f,margin.bottom)));
        // End padding follows the direct in-flow content of a scroll
        // container. Visible descendant overflow does not acquire another
        // copy of every ancestor's padding as it propagates outwards.
        const auto scrollable=[](const std::wstring& value){return value==L"auto"||value==L"scroll"||value==L"hidden";};
        if(!positioned&&scrollable(overflowX))right=std::max(right,
            child->rect.x+child->rect.width+box.appliedScrollLeft+std::max(0.0f,margin.right)+padding.right);
        if(!positioned&&scrollable(overflowY))bottom=std::max(bottom,
            child->rect.y+child->rect.height+box.appliedScrollTop+(collapsedEnd?0:std::max(0.0f,margin.bottom))+padding.bottom);
    }
    box.cssOverflowRight=right-box.rect.x;
    box.cssOverflowLeft=overflowLeft-box.rect.x;
    box.cssOverflowBottom=bottom-box.rect.y;
    const auto domBorders=IsTable(box)?BorderValues(box.style):borders;
    box.cssScrollWidth=std::max(clientWidth,rtl?rtlOrigin-left:
        right-box.rect.x-domBorders.left-box.scrollbarGutterLeft);
    box.cssScrollHeight=std::max(clientHeight,bottom-box.rect.y-domBorders.top);
    if(box.node->tag==L"textarea"){
        // FinalizeScroll has already measured the editor and settled both
        // scrollbar axes. Reuse those extents for CSSOM reads and painting.
        box.cssScrollWidth=box.scrollWidth+padding.left+padding.right;
        box.cssScrollHeight=box.scrollHeight+padding.top+padding.bottom;
    }else if(box.node->tag==L"input"){
        const auto text=BoxText(box);
        if(!text.empty())box.cssScrollWidth=std::max(box.cssScrollWidth,
            TextWidth(text,box.style)+padding.left+padding.right);
    }
    }
    // Empty reserved gutters remain inside the padding-edge overflow clip.
    // Only actual scrollbar tracks reduce it, independently of layout gutters.
    // Ordinary and deferred contexts must share this resolved rectangle.
    RefreshOverflowClip(box,styleSheet_);
    UpdateSubtreeBounds(box);
    box.deferredStackingScope=nullptr;
    box.scrollTranslationBoxes.clear();
    box.scrollStickyChildren.clear();
    box.scrollTraversalCached=box.scrollWidth>ScrollClientWidth(box)||
        box.scrollHeight>ScrollClientHeight(box);
    if(box.scrollTraversalCached){
        const auto collect=[&](const auto& self,LayoutBox& child)->void{
            box.scrollTranslationBoxes.push_back(&child);
            for(auto& descendant:child.children)self(self,*descendant);
        };
        for(auto& child:box.children)if(!child->style.Is(L"position",L"fixed")){
            collect(collect,*child);
            if(child->containsSticky)box.scrollStickyChildren.push_back(child.get());
        }
    }

    box.paintChildren.clear();
    box.paintChildren.reserve(box.children.size());
    for(auto& child:box.children)box.paintChildren.push_back(child.get());
    StableStackingOrder(box.paintChildren);

    // Any sufficiently large, non-overlapping vertical flow can skip directly
    // to the visible children. This is based only on final layout geometry and
    // CSS positioning, so it applies equally to lists, code views, tables, and
    // application-defined components.
    box.verticallyOrderedChildren.clear();
    box.overlayChildren.clear();
    box.verticallyOrderedChildren.reserve(box.paintChildren.size());
    float previousBottom=-std::numeric_limits<float>::infinity();
    for(auto* child:box.paintChildren){
        const auto position=child->style.Get(L"position",L"static");
        if(!child->visible||child->rect.width<=0||child->rect.height<=0)continue;
        if(child->containsSticky||position==L"absolute"||position==L"fixed"||
           position==L"sticky"){
            box.overlayChildren.push_back(child);
            continue;
        }
        // Indexed traversal must include visible overflow. If descendant bounds
        // overlap the next sibling, retain the full paint order instead of
        // building an index that can skip those descendants.
        const float top=child->subtreeBounds.y;
        const float bottom=top+child->subtreeBounds.height;
        if(top<previousBottom-0.01f){
            box.verticallyOrderedChildren.clear();
            box.overlayChildren.clear();
            break;
        }
        box.verticallyOrderedChildren.push_back(child);
        previousBottom=bottom;
    }
    if(box.verticallyOrderedChildren.size()<8){
        box.verticallyOrderedChildren.clear();
        box.overlayChildren.clear();
    }
}

void LayoutEngine::UpdateStackingContexts(LayoutBox& scope){
    scope.establishesStackingContext=true;
    scope.nonNegativeStackingContexts.clear();
    std::function<void(LayoutBox&)> collect=[&](LayoutBox& current){
        for(auto& child:current.children){
            child->establishesStackingContext=IsStackingContext(*child);
            if(child->establishesStackingContext){
                // CSS paints zero/auto positioned stacking contexts after
                // ordinary in-flow descendants. Sticky boxes always establish
                // a stacking context, even without an explicit z-index.
                if(ZIndex(*child)>=0){
                    scope.nonNegativeStackingContexts.push_back(child.get());
                    child->deferredStackingScope=&scope;
                }
                UpdateStackingContexts(*child);
            }else{
                // Positioned auto boxes paint after ordinary flow but do not
                // trap their descendants in a new stacking context.
                const auto position=child->style.Get(L"position",L"static");
                if(child->node->type==NodeType::Element&&position!=L"static"){
                    scope.nonNegativeStackingContexts.push_back(child.get());
                    child->deferredStackingScope=&scope;
                }
                collect(*child);
            }
        }
    };
    collect(scope);
    StableStackingOrder(scope.nonNegativeStackingContexts);
}

void LayoutEngine::UpdateTopLayer(){
    modalBoxes_.clear();if(!root_)return;
    std::function<void(LayoutBox&)> collect=[&](LayoutBox& box){
        if(box.visible&&box.node&&box.node->tag==L"dialog"&&box.node->modal&&
           box.node->attributes.count(L"open"))modalBoxes_.push_back(&box);
        for(auto& child:box.children)collect(*child);
    };
    collect(*root_);
}

void LayoutEngine::FinalizeScroll(LayoutBox& box){
    const auto overflowX=OverflowX(box),overflowY=OverflowY(box);
    const auto scrollport=PaddingBox(box);
    const float clientWidth=ScrollClientWidth(box),clientHeight=ScrollClientHeight(box);
    box.scrollWidth=clientWidth;box.scrollHeight=clientHeight;
    const auto scrollable=[](const std::wstring& overflow){return overflow==L"auto"||overflow==L"scroll"||overflow==L"hidden";};
    const bool scrollX=scrollable(overflowX),scrollY=scrollable(overflowY);
    if(box.node->tag==L"textarea"){
        const auto text=BoxText(box);
        const auto padding=EdgeValues(box.style,L"padding",box.parent?box.parent->content.width:viewportWidth_,viewportWidth_);
        const float paddingWidth=padding.left+padding.right;
        const float baseWidth=box.content.width,baseHeight=box.content.height;
        const float verticalWidth=box.verticalGutterReserved?0:VerticalScrollbarMetricsFor(box,styleSheet_).width;
        const float horizontalHeight=HorizontalScrollbarMetricsFor(box,styleSheet_).height;
        const bool nowrap=PreventsTextWrapping(box.style.Get(L"white-space"));
        const float textWidth=nowrap?TextWidth(text,box.style):0;
        bool vertical=overflowY==L"scroll",horizontal=overflowX==L"scroll";
        float textHeight=0;
        // Reserving one scrollbar can require the other. At most three
        // monotonic passes settle the two axes; TextHeight shares its cache
        // with intrinsic measurement and does no work during scroll paints.
        for(int pass=0;pass<3;++pass){
            const float width=std::max(0.0f,baseWidth-(vertical?verticalWidth:0));
            const float height=std::max(0.0f,baseHeight-(horizontal?horizontalHeight:0));
            textHeight=TextHeight(text,box.style,std::max(1.0f,width));
            const bool nextVertical=vertical||(overflowY==L"auto"&&textHeight>height+0.01f);
            // Chromium's horizontal editor overflow includes start padding,
            // while the overflowing line does not add trailing padding.
            const bool nextHorizontal=horizontal||(overflowX==L"auto"&&nowrap&&
                textWidth+padding.left>width+paddingWidth+0.01f);
            if(nextVertical==vertical&&nextHorizontal==horizontal)break;
            vertical=nextVertical;horizontal=nextHorizontal;
        }
        box.textareaVerticalScrollbar=vertical;box.textareaHorizontalScrollbar=horizontal;
        box.content.width=std::max(0.0f,baseWidth-(vertical?verticalWidth:0));
        box.content.height=std::max(0.0f,baseHeight-(horizontal?horizontalHeight:0));
        box.scrollWidth=std::max(box.content.width,nowrap?textWidth-padding.right:0.0f);
        box.scrollHeight=std::max(box.content.height,textHeight);
        box.node->scrollLeft=scrollX?std::max(0.0f,std::min(std::max(0.0f,box.scrollWidth-box.content.width),box.node->scrollLeft)):0;
        box.node->scrollTop=scrollY?std::max(0.0f,std::min(std::max(0.0f,box.scrollHeight-box.content.height),box.node->scrollTop)):0;
        box.appliedScrollLeft=box.node->scrollLeft;box.appliedScrollTop=box.node->scrollTop;
        return;
    }
    if(!scrollX&&!scrollY){box.node->scrollLeft=0;box.node->scrollTop=0;box.appliedScrollLeft=0;box.appliedScrollTop=0;return;}
    struct ScrollExtent { float right=0,bottom=0; };
    std::function<ScrollExtent(const LayoutBox&)> measure=[&](const LayoutBox& current){
        if(!current.visible||current.style.Is(L"position",L"fixed"))
            return ScrollExtent{-std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity()};
        const auto overflow=current.style.Get(L"overflow",L"visible");
        const auto clips=[](const std::wstring& value){return value==L"hidden"||value==L"clip"||value==L"auto"||value==L"scroll";};
        const bool clipsX=clips(current.style.Get(L"overflow-x",overflow));
        const bool clipsY=clips(current.style.Get(L"overflow-y",overflow));
        // A nested overflow box contributes its own border box, not the
        // translated contents behind its clip. Otherwise scrolling the inner
        // box changes the outer scroll range on the next layout rebuild.
        const float flowRight=current.rect.x+current.rect.width-std::min(0.0f,current.relativeOffsetX);
        const float flowBottom=current.rect.y+current.rect.height-std::min(0.0f,current.relativeOffsetY);
        if(clipsX&&clipsY)return ScrollExtent{flowRight,flowBottom};
        ScrollExtent descendants{current.content.x,current.content.y};
        for(const auto& child:current.children){const auto extent=measure(*child);descendants.right=std::max(descendants.right,extent.right);descendants.bottom=std::max(descendants.bottom,extent.bottom);}
        ScrollExtent extent{clipsX?flowRight:std::max(flowRight,descendants.right),
                            clipsY?flowBottom:std::max(flowBottom,descendants.bottom)};
        return extent;
    };
    ScrollExtent extent{box.content.x,box.content.y};
    const auto padding=EdgeValues(box.style,L"padding",box.parent?box.parent->content.width:viewportWidth_,viewportWidth_);
    for(const auto& child:box.children){
        const auto childExtent=measure(*child);extent.right=std::max(extent.right,childExtent.right);extent.bottom=std::max(extent.bottom,childExtent.bottom);
        if(!box.viewportScrollContainer&&child->visible&&!child->style.Is(L"position",L"absolute")&&!child->style.Is(L"position",L"fixed")){
            const auto margin=child->node->type==NodeType::Text?Edges{}:UsedLayoutMargins(*child,box.content.width,viewportWidth_);
            extent.right=std::max(extent.right,child->rect.x+child->rect.width+std::max(0.0f,margin.right)+padding.right);
            extent.bottom=std::max(extent.bottom,child->rect.y+child->rect.height+std::max(0.0f,margin.bottom)+padding.bottom);
        }
    }
    if(box.viewportScrollContainer){
        // Body margins live outside its border box but remain part of the
        // document's scrollable overflow at the trailing viewport edges.
        const float endX=std::max(0.0f,scrollport.x+box.viewportGutterLeft+ScrollClientWidth(box)-
            (box.rect.x+box.rect.width));
        const float endY=std::max(0.0f,scrollport.y+ScrollClientHeight(box)-
            (box.rect.y+box.rect.height));
        if(extent.right>scrollport.x+scrollport.width+0.01f)extent.right+=endX;
        if(extent.bottom>scrollport.y+scrollport.height+0.01f)extent.bottom+=endY;
    }
    // Non-viewport scroll metrics use the content box for both the client
    // size and overflow origin. Counting start padding as overflow creates
    // a scrollbar even when the contents fit, and inflates long scroll ranges.
    const float originX=box.viewportScrollContainer?scrollport.x+box.viewportGutterLeft:box.content.x+padding.right;
    const float originY=box.viewportScrollContainer?scrollport.y:box.content.y+padding.bottom;
    if(scrollX)box.scrollWidth=std::max(clientWidth,extent.right-originX);
    if(scrollY)box.scrollHeight=std::max(clientHeight,extent.bottom-originY);
    box.node->scrollLeft=std::max(0.0f,std::min(std::max(0.0f,box.scrollWidth-clientWidth),box.node->scrollLeft));
    box.node->scrollTop=std::max(0.0f,std::min(std::max(0.0f,box.scrollHeight-clientHeight),box.node->scrollTop));
    if(box.node->scrollLeft>0||box.node->scrollTop>0){
        for(auto& child:box.children)if(!child->style.Is(L"position",L"fixed"))TranslateBox(*child,-box.node->scrollLeft,-box.node->scrollTop);
        for(auto& child:box.children)ApplySticky(*child,scrollport.y,viewportHeight_);
    }
    box.appliedScrollLeft=box.node->scrollLeft;box.appliedScrollTop=box.node->scrollTop;
}

void LayoutEngine::LayoutBlock(LayoutBox& box,bool definiteHeight){
    float flowWidth=box.content.width;const auto overflowY=OverflowY(box);
    // A single no-wrap text run clipped by its block does not participate in
    // sibling flow. Give it the block's content rectangle directly so text
    // alignment, clipping, and ellipsis use the CSS containing block without
    // an otherwise redundant DirectWrite natural-width measurement.
    if(auto* textChild=SingleLineClippedTextChild(box)){
        LayoutBoxTree(*textChild,{box.content.x,box.content.y,flowWidth,box.content.height},true);
        return;
    }
    auto inlineOuterWidth=[&](const LayoutBox& child){
        if(child.style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(child)&&
           HasInFlowBlockChildren(child))return flowWidth;
        const auto width=child.style.Get(L"width");
        if(IsIntrinsicWidth(width)&&!HasIntrinsicWidth(child))return NaturalWidth(child);
        if(!width.empty()&&width!=L"auto")return BlockOuterWidth(child,flowWidth,viewportWidth_);
        float natural=NaturalWidth(child);
        if(child.node->tag==L"img"){
            // Replaced elements keep their intrinsic width, but percentage
            // min/max constraints are relative to the actual containing block.
            // NaturalWidth deliberately cannot resolve those percentages.
            const auto intrinsicMargin=EdgeValues(child.style,L"margin",500,500);
            const auto margin=EdgeValues(child.style,L"margin",flowWidth,viewportWidth_);
            float content=std::max(0.0f,natural-intrinsicMargin.left-intrinsicMargin.right);
            content=Constrain(child.style,L"min-width",L"max-width",content,
                              flowWidth,viewportWidth_);
            natural=content+margin.left+margin.right;
        }
        return natural;
    };
    auto inlineOuterHeight=[&](const LayoutBox& child,float width){
        if(child.style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(child)&&!IsBlockifiedItem(child))return NaturalHeight(child,width);
        const auto height=child.style.Get(L"height");
        return !height.empty()&&height!=L"auto"?
            BlockOuterHeight(child,box.content.height,width,viewportHeight_,viewportWidth_):
            NaturalHeight(child,width);
    };
    const bool hasFloats=std::any_of(box.children.begin(),box.children.end(),
        [](const auto& child){return child->visible&&
            UsedFloatSide(child->style)!=FloatSide::None;});
    bool inlineOnly=!hasFloats&&!box.children.empty();
    std::vector<std::pair<float,float>> inlineSizes;
    float inlineWidth=0;
    auto inlineLine=InitialInlineLineMetrics(box);
    for(auto& child:box.children){
        if(!child->visible)continue;
        if(child->style.Is(L"position",L"absolute")||
           child->style.Is(L"position",L"fixed"))continue;
        if(child->node->tag==L"br"){
            inlineOnly=false;
            break;
        }
        const auto display=child->style.Get(L"display");
        if(!IsInlineLevel(display)||HasInFlowBlockChildren(*child)){
            inlineOnly=false;
            break;
        }
        const float width=inlineOuterWidth(*child);
        const float height=inlineOuterHeight(*child,width);
        inlineSizes.push_back({width,height});
        inlineWidth+=width;
        IncludeInlineLineBox(inlineLine,box.style,*child,height,
                             flowWidth,viewportWidth_);
    }
    const float inlineHeight=inlineLine.Height();
    const auto whiteSpace=box.style.Get(L"white-space");
    const bool noWrap=PreventsTextWrapping(whiteSpace);
    if(inlineOnly&&(!inlineSizes.empty())&&(inlineWidth<=flowWidth+0.5f||noWrap)){
        float x=box.content.x;
        // Atomic inline boxes are neutral bidi objects. In a line containing
        // only those objects, the paragraph direction determines their order.
        // Mixed text runs still use their shaping/inline formatting path.
        const bool reverseAtoms=box.style.Is(L"direction",L"rtl")&&
            std::all_of(box.children.begin(),box.children.end(),[](const auto& child){
                return !child->visible||child->style.Is(L"position",L"absolute")||
                    child->style.Is(L"position",L"fixed")||IsAtomicInlineLevel(*child);
            });
        const auto alignment=PhysicalTextAlignment(box.style);
        if(alignment==L"center"||alignment==L"-webkit-center")x+=(flowWidth-inlineWidth)/2;
        else if(alignment==L"right"||alignment==L"-webkit-right")x+=flowWidth-inlineWidth;
        if(reverseAtoms)x+=inlineWidth;
        float y=box.content.y;
        if(box.style.Is(L"display",L"inline")&&!IsBlockifiedItem(box)&&
           !UsesGenericMonospaceMetrics(box.style))
            y-=InlineHalfLeading(box.style);
        // Ordinary inline formatting starts at the block's content edge.
        // A table cell, however, distributes the row's extra block size around
        // its inline line.  Keep a mixed line (for example icon + text) on the
        // same vertical center as the single text-run fast path used by sibling
        // cells.  Work in CSS DIPs so the result is stable at every monitor DPI.
        const bool tableCell=box.style.Is(L"display",L"table-cell");
        const auto verticalAlign=ToLower(Trim(box.style.Get(L"vertical-align",L"baseline")));
        const float verticalFree=std::max(0.0f,box.content.height-inlineHeight);
        if(box.node&&box.node->tag==L"button")y+=verticalFree/2;
        else if(tableCell){
            if(verticalAlign==L"bottom"||verticalAlign==L"text-bottom")y+=verticalFree;
            else if(verticalAlign==L"middle")y+=verticalFree/2;
        }
        size_t index=0;
        for(auto& child:box.children)if(child->visible){
            if(child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed")){
                const LayoutRect area=AbsoluteContainingBlock(*child,viewportWidth_,viewportHeight_);
                LayoutBoxTree(*child,PositionedRect(*child,area,viewportWidth_,viewportHeight_),true);continue;
            }
            const auto size=inlineSizes[index++];
            const float verticalOffset=InlineLineBoxOffset(
                inlineLine,box.style,*child,size.second,flowWidth,viewportWidth_);
            LayoutBoxTree(*child,{reverseAtoms?x-size.first:x,y+verticalOffset,size.first,size.second},true);
            // The parent has aligned this single inline line. Reapplying
            // paragraph alignment to its rounded width shifts glyph phases.
            child->inlineTextPositioned=child->node->type==NodeType::Text;
            x+=reverseAtoms?-size.first:size.first;
        }
        return;
    }
    float cursorY=box.content.y;float lineX=box.content.x;float lineHeight=0;
    float scrollbarChildExpansion=0;
    if(box.style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(box)&&
       !IsBlockifiedItem(box)&&!HasInFlowBlockChildren(box)&&
       !UsesGenericMonospaceMetrics(box.style)&&
       std::any_of(box.children.begin(),box.children.end(),
           [](const auto& child){return child->visible&&child->node->tag==L"br";}))
        cursorY-=InlineHalfLeading(box.style);
    auto mixedInlineLine=InitialInlineLineMetrics(box);
    struct InlinePlacedBox {LayoutBox* box;float height,offset;};
    std::vector<InlinePlacedBox> inlinePlacedBoxes;
    BlockMarginFlow marginFlow; marginFlow.parent=CollapsedBlockMargins(box,box.rect.width);
    std::vector<FloatArea> floats;
    size_t inlineLineBegin=0;
    std::vector<LayoutBox*> flattened;
    const auto flowChildren=InlineFlowChildren(box,&flattened,flowWidth);
    for(auto* child:flattened){child->inlineFlowFragments=true;child->clipsOverflow=false;child->overflowFlagsValid=true;}
    for(size_t childIndex=0;childIndex<flowChildren.size();++childIndex){
        auto* child=flowChildren[childIndex];if(!child->visible)continue;
        const bool absolute=child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed");
        if(absolute){
            const LayoutRect area=AbsoluteContainingBlock(*child,viewportWidth_,viewportHeight_);
            LayoutBoxTree(*child,PositionedRect(*child,area,viewportWidth_,viewportHeight_),true);continue;
        }
        const auto floatSide=UsedFloatSide(child->style);
        if(floatSide!=FloatSide::None){
            marginFlow.Separate();
            const float w=FloatOuterWidth(*child,flowWidth,viewportWidth_);
            float bandLeft=box.content.x,bandRight=box.content.x+flowWidth,nextBottom=0;
            AvailableFloatBand(floats,box.content.x,box.content.x+flowWidth,cursorY,
                               bandLeft,bandRight,nextBottom);
            const float pendingInlineWidth=std::max(0.0f,lineX-bandLeft);
            // A float can share the current line with preceding inline content.
            // Only finish that line when its content and the float cannot fit.
            if(lineHeight>0&&(w+pendingInlineWidth>bandRight-bandLeft+0.01f||
               ClearedFloatY(floats,cursorY,child->style.Get(L"clear"))>cursorY+0.01f)){
                cursorY+=lineHeight;lineX=box.content.x;lineHeight=0;
            }
            const auto cssH=ToLower(Trim(child->style.Get(L"height")));
            const bool explicitHeight=!cssH.empty()&&cssH!=L"auto";
            const bool resolvedHeight=explicitHeight&&
                (cssH.find(L'%')==std::wstring::npos||definiteHeight);
            const float h=resolvedHeight?
                BlockOuterHeight(*child,box.content.height,flowWidth,viewportHeight_,viewportWidth_):
                NaturalHeight(*child,std::max(1.0f,w));
            const auto placed=PlaceFloat(floats,box.content.x,box.content.x+flowWidth,
                cursorY,w,h,floatSide,child->style.Get(L"clear"));
            if(lineHeight>0&&floatSide==FloatSide::Left){
                for(size_t prior=inlineLineBegin;prior<childIndex;++prior){
                    auto& inlineChild=*flowChildren[prior];
                    if(inlineChild.visible&&UsedFloatSide(inlineChild.style)==FloatSide::None&&
                       !inlineChild.style.Is(L"position",L"absolute")&&!inlineChild.style.Is(L"position",L"fixed"))
                        TranslateBox(inlineChild,w,0);
                }
                lineX+=w;
            }
            LayoutBoxTree(*child,placed,true,true,resolvedHeight);
            floats.push_back({placed,floatSide});
            continue;
        }
        const float clearedY=ClearedFloatY(floats,cursorY,child->style.Get(L"clear"));
        if(clearedY>cursorY+0.01f){
            if(lineX>box.content.x)cursorY+=lineHeight;
            cursorY=std::max(cursorY,clearedY);lineX=box.content.x;lineHeight=0;
        }
        if(child->node->tag==L"br"){
            marginFlow.Separate();
            const float breakHeight=LineHeight(child->style);
            LayoutBoxTree(*child,{lineX,cursorY,0,breakHeight},true,false,false);
            cursorY+=lineX>box.content.x?std::max(lineHeight,breakHeight):breakHeight;
            lineX=box.content.x;lineHeight=0;continue;
        }
        const auto d=child->style.Get(L"display");const bool inlineBox=IsInlineLevel(d);
        if(inlineBox){
            marginFlow.Separate();
            float bandLeft=box.content.x,bandRight=box.content.x+flowWidth,nextBottom=0;
            AvailableFloatBand(floats,box.content.x,box.content.x+flowWidth,cursorY,
                               bandLeft,bandRight,nextBottom);
            if(lineX==box.content.x)lineX=bandLeft;
            float w=inlineOuterWidth(*child);
            const bool wrapText=child->node->type==NodeType::Text&&!noWrap;
            const bool atomic=IsAtomicInlineLevel(*child);
            const bool wrappingInlineContainer=!noWrap&&
                child->node->type==NodeType::Element&&!atomic&&
                !child->children.empty();
            if((wrapText||wrappingInlineContainer)&&InlineBreakBefore(flowChildren,childIndex)&&
               lineX+w+InlineAttachedTailWidth(flowChildren,childIndex)>
                   bandRight+(child->textFlowFragment?.001f:.5f)){
                if(lineX>bandLeft){cursorY+=lineHeight;lineHeight=0;}
                else if(std::isfinite(nextBottom))cursorY=nextBottom;
                AvailableFloatBand(floats,box.content.x,box.content.x+flowWidth,
                                   cursorY,bandLeft,bandRight,nextBottom);
                lineX=bandLeft;
                if(wrapText&&!PreservesSpaces(child->style.Get(L"white-space"))){
                    child->trimLeadingLineWhitespace=true;
                    child->naturalWidthValid=false;child->minimumWidthValid=false;child->intrinsicMinimumWidthValid=false;child->maxContentWidthValid=false;child->minContentWidthValid=false;child->naturalHeightValid=false;
                    if(child->tableGrid){child->tableGrid->intrinsicColumnsValid=false;
                        child->tableGrid->columnsValid=false;child->tableGrid->rowsValid=false;}
                    w=inlineOuterWidth(*child);
                }
                w=std::min(w,std::max(0.0f,bandRight-bandLeft));
            }
            const float h=(wrapText||wrappingInlineContainer)?
                NaturalHeight(*child,w):inlineOuterHeight(*child,w);
            if(!wrapText&&!wrappingInlineContainer&&lineX+w>bandRight+0.5f&&
               lineX>bandLeft){
                cursorY+=lineHeight;
                lineHeight=0;
                AvailableFloatBand(floats,box.content.x,box.content.x+flowWidth,
                                   cursorY,bandLeft,bandRight,nextBottom);
                lineX=bandLeft;
            }
            if(lineHeight<=0){mixedInlineLine=InitialInlineLineMetrics(box);inlinePlacedBoxes.clear();}
            const float previousBaseline=mixedInlineLine.baseline,previousHeight=mixedInlineLine.Height();
            IncludeInlineLineBox(mixedInlineLine,box.style,*child,h,
                                 flowWidth,viewportWidth_);
            if(mixedInlineLine.baseline!=previousBaseline||mixedInlineLine.Height()!=previousHeight)
                for(auto& placed:inlinePlacedBoxes){
                    const float offset=InlineLineBoxOffset(mixedInlineLine,box.style,*placed.box,placed.height,flowWidth,viewportWidth_);
                    if(offset!=placed.offset)TranslateBox(*placed.box,0,offset-placed.offset);
                    placed.offset=offset;
                }
            const float verticalOffset=InlineLineBoxOffset(
                mixedInlineLine,box.style,*child,h,flowWidth,viewportWidth_);
            if(lineHeight<=0)inlineLineBegin=childIndex;
            LayoutBoxTree(*child,{lineX,cursorY+verticalOffset,w,h},
                          true,false,false);
            if(child->inlineContinuationValid){
                lineX=child->rect.x+child->inlineContinuationX;
                cursorY=child->rect.y+child->inlineContinuationY;
                lineHeight=child->inlineContinuationHeight;
                mixedInlineLine=InitialInlineLineMetrics(box);inlinePlacedBoxes.clear();
                continue;
            }
            inlinePlacedBoxes.push_back({child,h,verticalOffset});
            lineX+=w;
            lineHeight=std::max(lineHeight,mixedInlineLine.Height());
            if(child->node->type==NodeType::Text&&h>LineHeight(child->style)+.01f){
                cursorY+=h-LineHeight(child->style);
                lineX=bandLeft+TextLastLineAdvance(*child,w,child->trimLeadingLineWhitespace);
                lineHeight=std::max(LineHeight(box.style),LineHeight(child->style));
                mixedInlineLine=InitialInlineLineMetrics(box);inlinePlacedBoxes.clear();
            }
        }
        else{
            if(lineX>box.content.x){cursorY+=lineHeight;lineX=box.content.x;lineHeight=0;}
            const auto childMargins=CollapsedBlockMargins(*child,flowWidth);
            cursorY+=marginFlow.Before(childMargins,child->style.Get(L"clear",L"none")!=L"none");
            float bandLeft=box.content.x,bandRight=box.content.x+flowWidth,nextBottom=0;
            AvailableFloatBand(floats,box.content.x,box.content.x+flowWidth,cursorY,
                               bandLeft,bandRight,nextBottom);
            if(bandRight<=bandLeft+0.01f&&std::isfinite(nextBottom)){
                cursorY=nextBottom;
                AvailableFloatBand(floats,box.content.x,box.content.x+flowWidth,cursorY,
                                   bandLeft,bandRight,nextBottom);
            }
            const float availableWidth=std::max(0.0f,bandRight-bandLeft);
            const float w=BlockOuterWidth(*child,availableWidth,viewportWidth_);
            float h=NaturalHeight(*child,std::max(1.0f,w));const auto cssH=child->style.Get(L"height");
            const bool explicitHeight=!cssH.empty()&&cssH!=L"auto";
            if(explicitHeight)
                h=BlockOuterHeight(*child,box.content.height,flowWidth,viewportHeight_,viewportWidth_);
            else if(definiteHeight){
                const auto minimum=Trim(child->style.Get(L"min-height"));
                const auto maximum=Trim(child->style.Get(L"max-height"));
                const bool percentageConstraint=minimum.find(L'%')!=std::wstring::npos||
                    maximum.find(L'%')!=std::wstring::npos;
                if(percentageConstraint){
                    const auto childMargin=EdgeValues(child->style,L"margin",flowWidth,viewportWidth_);
                    const auto childPadding=EdgeValues(child->style,L"padding",flowWidth,viewportWidth_);
                    const auto childBorder=BorderValues(child->style);
                    const float decoration=child->style.Is(L"box-sizing",L"border-box")?0.0f:
                        childPadding.top+childPadding.bottom+childBorder.top+childBorder.bottom;
                    float constrained=std::max(0.0f,h-childMargin.top-childMargin.bottom-decoration);
                    constrained=Constrain(child->style,L"min-height",L"max-height",constrained,
                                          box.content.height,viewportHeight_);
                    h=constrained+decoration+childMargin.top+childMargin.bottom;
                }
            }
            const bool autoLeft=ToLower(Trim(child->style.Get(L"margin-left")))==L"auto";
            const bool autoRight=ToLower(Trim(child->style.Get(L"margin-right")))==L"auto";
            const float freeWidth=std::max(0.0f,availableWidth-w);float childX=bandLeft;
            if(autoLeft&&autoRight)childX+=freeWidth/2;else if(autoLeft)childX+=freeWidth;
            else if(!autoRight){
                if(box.style.Is(L"text-align",L"-webkit-center"))childX+=freeWidth/2;
                else if(box.style.Is(L"text-align",L"-webkit-right"))childX+=freeWidth;
                else if(box.style.Is(L"direction",L"rtl")){
                    childX+=availableWidth-w;
                    // In over-constrained RTL flow Chromium resolves the
                    // end margin against the padding area excluding an actual
                    // scrollbar, rather than the empty stable reservations.
                    if(w>availableWidth&&box.verticalGutterReserved)
                        childX+=box.scrollbarGutterLeft+box.scrollbarGutterRight-
                            (overflowY==L"scroll"?std::max(box.scrollbarGutterLeft,box.scrollbarGutterRight):0);
                }
            }
            LayoutBoxTree(*child,{childX,cursorY,w,h},true,true,explicitHeight);
            if(child->autoScrollbarHeightExpansion>0){
                const float extra=std::max(0.0f,child->rect.height+childMargins.top.Value()+childMargins.bottom.Value()-h);
                h+=extra;scrollbarChildExpansion+=extra;
            }
            cursorY+=h+marginFlow.After(childMargins);
        }
    }
    for(auto it=flattened.rbegin();it!=flattened.rend();++it){
        auto& inlineBox=**it;bool first=true;LayoutRect bounds{};
        for(const auto& descendant:inlineBox.children){
            if(!descendant->visible)continue;
            auto rect=descendant->rect;
            if(descendant->node->type==NodeType::Text){
                rect.y+=InlineHalfLeading(descendant->style);
                rect.height=std::max(0.0f,rect.height-LineHeight(descendant->style)+InlineContentBoxHeight(descendant->style));
            }
            if(first){bounds=rect;first=false;}
            else{const float right=std::max(bounds.x+bounds.width,rect.x+rect.width),bottom=std::max(bounds.y+bounds.height,rect.y+rect.height);
                bounds.x=std::min(bounds.x,rect.x);bounds.y=std::min(bounds.y,rect.y);
                bounds.width=right-bounds.x;bounds.height=bottom-bounds.y;}
        }
        inlineBox.rect=inlineBox.content=bounds;
    }
    if(scrollbarChildExpansion>0&&AutoScrollbarHeight(box,definiteHeight)){
        box.rect.height+=scrollbarChildExpansion;box.content.height+=scrollbarChildExpansion;
        box.autoScrollbarHeightExpansion+=scrollbarChildExpansion;
    }
    if(box.style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(box)&&HasInFlowBlockChildren(box)){
        box.inlineContinuationValid=true;
        box.inlineContinuationX=lineX-box.rect.x;
        box.inlineContinuationY=cursorY-box.rect.y;
        box.inlineContinuationHeight=lineHeight;
    }
    if(box.style.Is(L"display",L"table-cell")){
        // Vertical alignment applies to all in-flow cell contents, including
        // block children and floats, not just the inline-only fast path above.
        float flowBottom=cursorY+lineHeight;
        for(const auto& area:floats)flowBottom=std::max(flowBottom,area.rect.y+area.rect.height);
        const float extra=std::max(0.0f,box.content.height-(flowBottom-box.content.y));
        const auto alignment=box.style.Get(L"vertical-align",L"baseline");
        const float offset=alignment==L"bottom"?extra:alignment==L"middle"?extra/2:0;
        if(offset>0)for(auto& child:box.children){
            if(child->visible&&!child->style.Is(L"position",L"absolute")&&
               !child->style.Is(L"position",L"fixed"))TranslateBox(*child,0,offset);
        }
    }
}

void LayoutEngine::LayoutColumns(LayoutBox& box){
    const auto model=BuildColumnFlow(box,box.content.width,viewportWidth_);
    for(const auto& placement:model.placements){
        auto area=placement.area;area.x+=box.content.x;area.y+=box.content.y;
        const auto height=placement.box->style.Get(L"height");
        LayoutBoxTree(*placement.box,area,true,true,!height.empty()&&height!=L"auto");
    }
    for(const auto& child:box.children)if(child->visible&&
        (child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))){
        const auto containing=AbsoluteContainingBlock(*child,viewportWidth_,viewportHeight_);
        LayoutBoxTree(*child,PositionedRect(*child,containing,viewportWidth_,viewportHeight_),true);
    }
}

void LayoutEngine::LayoutFlex(LayoutBox& box){
    std::vector<LayoutBox*> children;for(auto& c:box.children)if(c->visible&& !c->style.Is(L"position",L"absolute")&&!c->style.Is(L"position",L"fixed"))children.push_back(c.get());
    const auto direction=ToLower(Trim(box.style.Get(L"flex-direction",L"row")));
    const bool column=direction==L"column"||direction==L"column-reverse";
    const bool reverse=(direction==L"row-reverse"||direction==L"column-reverse")!=
        (!column&&box.style.Is(L"direction",L"rtl"));
    const float mainSize=column?box.content.height:box.content.width;const float crossSize=column?box.content.width:box.content.height;const float gap=GapValue(box.style,!column,mainSize,viewportWidth_);
    const auto decorationExtent=[&](const LayoutBox& child,bool horizontal){
        // Flex allocations passed to LayoutBoxTree include padding and borders.
        // Percentage padding on either axis resolves against the containing width.
        const auto padding=EdgeValues(child.style,L"padding",box.content.width,viewportWidth_);
        const auto border=BorderValues(child.style);
        return horizontal?padding.left+padding.right+border.left+border.right:
            padding.top+padding.bottom+border.top+border.bottom;
    };
    const auto definiteExtent=[&](const LayoutBox& child,const std::wstring& raw,
                                  bool horizontal,float reference){
        const float viewport=horizontal?viewportWidth_:viewportHeight_;
        if(horizontal&&(IsIntrinsicWidth(raw)||HasIntrinsicWidth(child)))
            return UsedIntrinsicWidth(child,raw,reference,viewport);
        float value=StyleSheet::Length(raw,reference,viewport,0,FontSize(child.style));
        value=Constrain(child.style,horizontal?L"min-width":L"min-height",
                        horizontal?L"max-width":L"max-height",value,reference,viewport);
        const float decoration=decorationExtent(child,horizontal);
        return child.style.Is(L"box-sizing",L"border-box")?std::max(decoration,value):
            decoration+std::max(0.0f,value);
    };
    const auto wrapMode=ToLower(Trim(box.style.Get(L"flex-wrap",L"nowrap")));
    struct MainMetrics {
        float base=0,minimum=0,maximum=0,hypothetical=0;
        float decoration=0,margin=0,grow=0,shrink=0;
    };
    std::vector<MainMetrics> mainMetrics;mainMetrics.reserve(children.size());
    for(const auto* child:children){
        MainMetrics item;
        const auto margin=EdgeValues(child->style,L"margin",box.content.width,viewportWidth_);
        item.margin=column?margin.top+margin.bottom:margin.left+margin.right;
        item.decoration=decorationExtent(*child,!column);
        item.grow=std::max(0.0f,StyleSheet::Length(child->style.Get(L"flex-grow",L"0"),0,0,0));
        item.shrink=std::max(0.0f,StyleSheet::Length(child->style.Get(L"flex-shrink",L"1"),0,0,1));
        const auto outerLength=[&](const std::wstring& raw){
            if(!column&&IsIntrinsicWidth(raw)){
                float content=IntrinsicContentWidth(*child,raw==L"min-content");
                if(raw==L"fit-content")content=std::max(IntrinsicContentWidth(*child,true),
                    std::min(content,std::max(0.0f,mainSize-item.margin-item.decoration)));
                return content+item.decoration+item.margin;
            }
            const float value=StyleSheet::Length(raw,mainSize,column?viewportHeight_:viewportWidth_,
                0,FontSize(child->style));
            return std::max(0.0f,value)+item.margin+
                (child->style.Is(L"box-sizing",L"border-box")?0:item.decoration);
        };
        const auto preferred=ToLower(Trim(child->style.Get(column?L"height":L"width")));
        auto basis=ToLower(Trim(child->style.Get(L"flex-basis",L"auto")));
        if(basis.empty()||basis==L"auto")basis=preferred;
        const bool percentageIndefinite=column&&!box.definiteContentHeight&&basis.find(L'%')!=std::wstring::npos;
        if(basis.empty()||basis==L"auto"||basis==L"content"||percentageIndefinite){
            // The flex base is measured before applying this item's min/max
            // constraints. A definite preferred size remains an auto basis.
            item.base=column?NaturalHeight(*child,HasIntrinsicWidth(*child)?
                BlockOuterWidth(*child,crossSize,viewportWidth_):crossSize):
                IntrinsicContentWidth(*child,false)+item.decoration+item.margin;
        }else item.base=outerLength(basis);
        const auto minimum=ToLower(Trim(child->style.Get(column?L"min-height":L"min-width")));
        const auto maximum=ToLower(Trim(child->style.Get(column?L"max-height":L"max-width")));
        item.minimum=item.decoration+item.margin;
        if(!minimum.empty()&&minimum!=L"auto")item.minimum=std::max(item.minimum,outerLength(minimum));
        else if(!column){
            const auto overflow=child->style.Get(L"overflow-x",child->style.Get(L"overflow",L"visible"));
            if(overflow==L"visible"||overflow==L"clip"){
                float contentMinimum=IntrinsicContentWidth(*child,true)+item.decoration+item.margin;
                if(!preferred.empty()&&preferred!=L"auto")contentMinimum=std::min(contentMinimum,outerLength(preferred));
                item.minimum=std::max(item.minimum,contentMinimum);
            }
        }
        item.maximum=maximum.empty()||maximum==L"none"||maximum==L"auto"?
            (std::numeric_limits<float>::max)():std::max(item.decoration+item.margin,outerLength(maximum));
        if(minimum.empty()||minimum==L"auto")item.minimum=std::min(item.minimum,item.maximum);
        item.maximum=std::max(item.maximum,item.minimum);
        item.hypothetical=std::max(item.minimum,std::min(item.maximum,item.base));
        mainMetrics.push_back(item);
    }
    const auto resolveMain=[&](const std::vector<size_t>& items,std::vector<float>& sizes){
        // CSS Flexbox 9.7: use hypothetical sizes to choose the flex factor,
        // but distribute from the unconstrained bases. Freeze only violations
        // of the selected sign, then recompute free space for the other items.
        const double gaps=gap*std::max(0,static_cast<int>(items.size())-1);
        double hypothetical=gaps;
        for(const auto index:items)hypothetical+=mainMetrics[index].hypothetical;
        const bool growing=hypothetical<mainSize;
        std::vector<unsigned char> frozen(items.size(),0);
        std::vector<double> targets(items.size()),violations(items.size());
        for(size_t i=0;i<items.size();++i){
            const auto& item=mainMetrics[items[i]];
            targets[i]=item.base;
            if((growing?item.grow:item.shrink)==0||
               (growing?item.base>item.hypothetical:item.base<item.hypothetical)){
                frozen[i]=1;targets[i]=item.hypothetical;
            }
        }
        const auto freeSpace=[&]{
            double occupied=gaps;
            for(size_t i=0;i<items.size();++i)occupied+=frozen[i]?targets[i]:mainMetrics[items[i]].base;
            return mainSize-occupied;
        };
        const double initialFree=freeSpace();
        for(size_t iteration=0;iteration<=items.size();++iteration){
            double factorTotal=0,weightTotal=0;size_t active=0;
            for(size_t i=0;i<items.size();++i)if(!frozen[i]){
                ++active;const auto& item=mainMetrics[items[i]];
                factorTotal+=growing?item.grow:item.shrink;
                weightTotal+=growing?item.grow:item.shrink*std::max(0.0f,item.base-item.margin-item.decoration);
            }
            if(!active)break;
            double remaining=freeSpace();
            if(factorTotal<1&&std::abs(initialFree*factorTotal)<std::abs(remaining))remaining=initialFree*factorTotal;
            double totalViolation=0;
            for(size_t i=0;i<items.size();++i)if(!frozen[i]){
                const auto& item=mainMetrics[items[i]];
                const double weight=growing?item.grow:item.shrink*std::max(0.0f,item.base-item.margin-item.decoration);
                const double target=item.base+(weightTotal>0?(growing?remaining:-std::abs(remaining))*weight/weightTotal:0);
                targets[i]=std::max<double>(item.minimum,std::min<double>(item.maximum,target));
                violations[i]=targets[i]-target;totalViolation+=violations[i];
            }
            for(size_t i=0;i<items.size();++i)if(!frozen[i]&&
                (std::abs(totalViolation)<0.000001||(totalViolation>0?violations[i]>0:violations[i]<0)))frozen[i]=1;
        }
        const double units=64.0*deviceScale_;
        double accumulated=0;float assigned=0;
        for(size_t i=0;i<items.size();++i){
            accumulated+=targets[i];
            const float edge=static_cast<float>(std::round(accumulated*units)/units);
            sizes[items[i]]=edge-assigned;assigned=edge;
        }
    };
    if((wrapMode==L"wrap"||wrapMode==L"wrap-reverse")&&!children.empty()){
        const size_t count=children.size();
        std::vector<float> sizes(count),crossExtents(count);
        std::vector<bool> mainAutoBefore(count),mainAutoAfter(count),
            crossAutoBefore(count),crossAutoAfter(count),crossDefinite(count);
        std::vector<std::wstring> alignments(count);
        const auto parentAlign=box.style.Get(L"align-items",L"stretch");
        for(size_t index=0;index<count;++index){
            auto* child=children[index];
            const auto margin=EdgeValues(child->style,L"margin",box.content.width,viewportWidth_);
            const auto isAutoMargin=[&](const wchar_t* property){
                return ToLower(Trim(child->style.Get(property)))==L"auto";
            };
            mainAutoBefore[index]=isAutoMargin(column?L"margin-top":L"margin-left");
            mainAutoAfter[index]=isAutoMargin(column?L"margin-bottom":L"margin-right");
            crossAutoBefore[index]=isAutoMargin(column?L"margin-left":L"margin-top");
            crossAutoAfter[index]=isAutoMargin(column?L"margin-right":L"margin-bottom");
            sizes[index]=mainMetrics[index].hypothetical;

            auto align=child->style.Get(L"align-self",L"auto");
            if(align.empty()||align==L"auto")align=parentAlign;
            alignments[index]=align;
            const auto crossProperty=child->style.Get(column?L"width":L"height");
            crossDefinite[index]=!crossProperty.empty()&&crossProperty!=L"auto";
            if(crossDefinite[index]){
                crossExtents[index]=definiteExtent(*child,crossProperty,column,crossSize)+
                    (column?margin.left+margin.right:margin.top+margin.bottom);
            }else{
                crossExtents[index]=column?NaturalWidth(*child):NaturalHeight(*child,std::max(1.0f,sizes[index]));
            }
        }

        struct FlexLine { std::vector<size_t> items; float cross=0; };
        std::vector<FlexLine> lines(1);float lineMain=0;
        for(size_t index=0;index<count;++index){
            auto& line=lines.back();
            const float candidate=lineMain+(line.items.empty()?0.0f:gap)+sizes[index];
            if(!line.items.empty()&&candidate>mainSize+0.5f){
                lines.push_back({});lineMain=0;
            }
            auto& destination=lines.back();
            if(!destination.items.empty())lineMain+=gap;
            destination.items.push_back(index);lineMain+=sizes[index];
        }

        for(auto& line:lines){
            resolveMain(line.items,sizes);
            for(const auto index:line.items){
                if(!column&&!crossDefinite[index])
                    crossExtents[index]=NaturalHeight(*children[index],std::max(1.0f,sizes[index]));
                line.cross=std::max(line.cross,crossExtents[index]);
            }
        }

        const float crossGap=GapValue(box.style,column,crossSize,viewportWidth_);
        float occupiedCross=crossGap*std::max(0,static_cast<int>(lines.size())-1);
        for(const auto& line:lines)occupiedCross+=line.cross;
        float crossRemain=std::max(0.0f,crossSize-occupiedCross);
        auto alignContent=ToLower(Trim(box.style.Get(L"align-content",L"stretch")));
        if(alignContent.empty()||alignContent==L"normal")alignContent=L"stretch";
        float crossOffset=0,dynamicCrossGap=crossGap;
        if(alignContent==L"stretch"&&!lines.empty()){
            const float extra=crossRemain/static_cast<float>(lines.size());
            for(auto& line:lines)line.cross+=extra;crossRemain=0;
        }else if(alignContent==L"center")crossOffset=crossRemain/2;
        else if(alignContent==L"flex-end"||alignContent==L"end")crossOffset=crossRemain;
        else if(alignContent==L"space-between"&&lines.size()>1)
            dynamicCrossGap+=crossRemain/static_cast<float>(lines.size()-1);
        else if(alignContent==L"space-around"&&!lines.empty()){
            const float extra=crossRemain/static_cast<float>(lines.size());
            crossOffset=extra/2;dynamicCrossGap+=extra;
        }else if(alignContent==L"space-evenly"&&!lines.empty()){
            const float extra=crossRemain/static_cast<float>(lines.size()+1);
            crossOffset=extra;dynamicCrossGap+=extra;
        }

        float crossCursor=(column?box.content.x:box.content.y)+crossOffset;
        std::vector<size_t> lineOrder;lineOrder.reserve(lines.size());
        if(wrapMode==L"wrap-reverse")
            for(size_t index=lines.size();index>0;--index)lineOrder.push_back(index-1);
        else for(size_t index=0;index<lines.size();++index)lineOrder.push_back(index);
        const auto justify=box.style.Get(L"justify-content");
        for(const auto lineIndex:lineOrder){
            auto& line=lines[lineIndex];
            float occupied=gap*std::max(0,static_cast<int>(line.items.size())-1);
            size_t autoMarginCount=0;
            for(const auto index:line.items){
                occupied+=sizes[index];
                autoMarginCount+=static_cast<size_t>(mainAutoBefore[index])+static_cast<size_t>(mainAutoAfter[index]);
            }
            float remain=std::max(0.0f,mainSize-occupied);
            const float autoMargin=autoMarginCount?remain/static_cast<float>(autoMarginCount):0;
            if(autoMarginCount)remain=0;
            float mainOffset=0,dynamicGap=gap;
            if(justify==L"center")mainOffset=remain/2;
            else if(justify==L"flex-end"||justify==L"end")mainOffset=remain;
            else if(justify==L"space-between"&&line.items.size()>1)
                dynamicGap+=remain/static_cast<float>(line.items.size()-1);
            else if(justify==L"space-around"&&!line.items.empty()){
                const float extra=remain/static_cast<float>(line.items.size());
                mainOffset=extra/2;dynamicGap+=extra;
            }else if(justify==L"space-evenly"&&!line.items.empty()){
                const float extra=remain/static_cast<float>(line.items.size()+1);
                mainOffset=extra;dynamicGap+=extra;
            }
            float mainCursor=reverse?(column?box.content.y+box.content.height:box.content.x+box.content.width)-mainOffset:
                (column?box.content.y:box.content.x)+mainOffset;

            float sharedBaseline=0;
            for(const auto index:line.items)
                if(alignments[index]==L"baseline"&&!crossAutoBefore[index]&&!crossAutoAfter[index])
                    sharedBaseline=std::max(sharedBaseline,FlexItemBaselineOffset(
                        *children[index],sizes[index],crossExtents[index]));
            for(const auto index:line.items){
                auto* child=children[index];const auto& align=alignments[index];
                if(mainAutoBefore[index])mainCursor+=reverse?-autoMargin:autoMargin;
                const bool autoCross=crossAutoBefore[index]||crossAutoAfter[index];
                const bool stretch=!autoCross&&!crossDefinite[index]&&align==L"stretch";
                const float childCross=stretch?line.cross:std::min(line.cross,crossExtents[index]);
                const float crossFree=std::max(0.0f,line.cross-childCross);
                float childCrossPosition=crossCursor;
                if(crossAutoBefore[index]&&crossAutoAfter[index])childCrossPosition+=crossFree/2;
                else if(crossAutoBefore[index])childCrossPosition+=crossFree;
                else if(!crossAutoAfter[index]&&align==L"center")childCrossPosition+=crossFree/2;
                else if(!crossAutoAfter[index]&&(align==L"flex-end"||align==L"end"))childCrossPosition+=crossFree;
                else if(!crossAutoAfter[index]&&align==L"baseline")
                    childCrossPosition+=sharedBaseline-FlexItemBaselineOffset(
                        *child,sizes[index],childCross);
                const float childMainPosition=reverse?mainCursor-sizes[index]:mainCursor;
                const LayoutRect area=column?LayoutRect{childCrossPosition,childMainPosition,childCross,sizes[index]}:
                    LayoutRect{childMainPosition,childCrossPosition,sizes[index],childCross};
                LayoutBoxTree(*child,area,true,column?stretch||crossDefinite[index]:true,
                              column?true:stretch||crossDefinite[index]);
                const float advance=sizes[index]+(mainAutoAfter[index]?autoMargin:0)+dynamicGap;
                mainCursor+=reverse?-advance:advance;
            }
            crossCursor+=line.cross+dynamicCrossGap;
        }
        for(auto& child:box.children)if(child->visible&&
            (child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))){
            const LayoutRect area=AbsoluteContainingBlock(*child,viewportWidth_,viewportHeight_);
            auto positioned=FlexStaticPositionedRect(box,*child,
                PositionedRect(*child,area,viewportWidth_,viewportHeight_));
            LayoutBoxTree(*child,positioned,true);
        }
        return;
    }
    std::vector<float> sizes(children.size());
    std::vector<size_t> allItems(children.size());
    std::iota(allItems.begin(),allItems.end(),0);
    resolveMain(allItems,sizes);
    std::vector<bool> mainAutoBefore(children.size()),mainAutoAfter(children.size()),
        crossAutoBefore(children.size()),crossAutoAfter(children.size());
    size_t mainAutoMarginCount=0;
    for(size_t i=0;i<children.size();++i){
        auto* c=children[i];
        const auto isAutoMargin=[&](const wchar_t* property){return ToLower(Trim(c->style.Get(property)))==L"auto";};
        mainAutoBefore[i]=isAutoMargin(column?L"margin-top":L"margin-left");
        mainAutoAfter[i]=isAutoMargin(column?L"margin-bottom":L"margin-right");
        crossAutoBefore[i]=isAutoMargin(column?L"margin-left":L"margin-top");
        crossAutoAfter[i]=isAutoMargin(column?L"margin-right":L"margin-bottom");
        mainAutoMarginCount+=static_cast<size_t>(mainAutoBefore[i])+static_cast<size_t>(mainAutoAfter[i]);
    }
    float occupied=gap*std::max(0,static_cast<int>(children.size())-1);for(float size:sizes)occupied+=size;float remain=std::max(0.0f,mainSize-occupied);
    const float autoMainMargin=mainAutoMarginCount&&remain>0?
        remain/static_cast<float>(mainAutoMarginCount):0;
    if(autoMainMargin>0)remain=0;
    const auto justify=box.style.Get(L"justify-content");float dynamicGap=gap,mainOffset=0;
    if(justify==L"center")mainOffset=remain/2;else if(justify==L"flex-end"||justify==L"end")mainOffset=remain;
    else if(justify==L"space-between"&&children.size()>1)dynamicGap+=remain/(children.size()-1);
    else if(justify==L"space-around"&&!children.empty()){const float extra=remain/children.size();mainOffset=extra/2;dynamicGap+=extra;}
    else if(justify==L"space-evenly"&&!children.empty()){const float extra=remain/(children.size()+1);mainOffset=extra;dynamicGap+=extra;}
    float cursor=reverse?(column?box.content.y+box.content.height:box.content.x+box.content.width)-mainOffset:
        (column?box.content.y:box.content.x)+mainOffset;
    const auto parentAlign=box.style.Get(L"align-items",L"stretch");
    std::vector<std::wstring> alignments(children.size());
    std::vector<float> crossExtents(children.size(),crossSize);
    std::vector<bool> crossDefinite(children.size(),true);
    float sharedBaseline=0;
    for(size_t i=0;i<children.size();++i){
        auto* child=children[i];auto align=child->style.Get(L"align-self",L"auto");
        if(align.empty()||align==L"auto")align=parentAlign;
        alignments[i]=align;
        const auto crossRaw=child->style.Get(column?L"width":L"height");
        if(!crossRaw.empty()&&crossRaw!=L"auto"){
            const auto margin=EdgeValues(child->style,L"margin",box.content.width,viewportWidth_);
            crossExtents[i]=definiteExtent(*child,crossRaw,column,crossSize)+
                (column?margin.left+margin.right:margin.top+margin.bottom);
        }else if(align!=L"stretch"){
            crossExtents[i]=std::min(crossSize,column?NaturalWidth(*child):
                NaturalHeight(*child,sizes[i]));
            crossDefinite[i]=false;
        }
        if(!column&&align==L"baseline"&&!crossAutoBefore[i]&&!crossAutoAfter[i])
            sharedBaseline=std::max(sharedBaseline,FlexItemBaselineOffset(
                *child,sizes[i],crossExtents[i]));
    }
    for(size_t i=0;i<children.size();++i){
        auto* child=children[i];const auto& align=alignments[i];
        const float cross=crossExtents[i];float crossPos=column?box.content.x:box.content.y;
        const float crossFree=std::max(0.0f,crossSize-cross);
        if(crossAutoBefore[i]&&crossAutoAfter[i])crossPos+=crossFree/2;
        else if(crossAutoBefore[i])crossPos+=crossFree;
        else if(!crossAutoAfter[i]&&align==L"center")crossPos+=crossFree/2;
        else if(!crossAutoAfter[i]&&(align==L"flex-end"||align==L"end"))crossPos+=crossFree;
        else if(!crossAutoAfter[i]&&!column&&align==L"baseline")crossPos+=sharedBaseline-
            FlexItemBaselineOffset(*child,sizes[i],cross);
        if(mainAutoBefore[i])cursor+=reverse?-autoMainMargin:autoMainMargin;
        const float mainPosition=reverse?cursor-sizes[i]:cursor;
        const LayoutRect area=column?LayoutRect{crossPos,mainPosition,cross,sizes[i]}:
            LayoutRect{mainPosition,crossPos,sizes[i],cross};
        LayoutBoxTree(*child,area,true,column?crossDefinite[i]:true,
                      column?true:crossDefinite[i]);
        const float advance=sizes[i]+(mainAutoAfter[i]?autoMainMargin:0)+dynamicGap;
        cursor+=reverse?-advance:advance;
    }
    for(auto& c:box.children)if(c->visible&&(c->style.Is(L"position",L"absolute")||c->style.Is(L"position",L"fixed"))){
        const LayoutRect area=AbsoluteContainingBlock(*c,viewportWidth_,viewportHeight_);
        auto positioned=FlexStaticPositionedRect(box,*c,
            PositionedRect(*c,area,viewportWidth_,viewportHeight_));
        LayoutBoxTree(*c,positioned,true);
    }
}

void LayoutEngine::LayoutGrid(LayoutBox& box,bool definiteWidth,bool definiteHeight){
    float gridWidth=box.content.width;
    const auto columnDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-columns",L"none"),box,gridWidth);
    const auto rowDefinitions=ExpandGridTracks(box.style.Get(L"grid-template-rows"),box,box.content.height);
    size_t areaRows=0,areaColumns=0;const auto areas=ParseGridAreas(box.style.Get(L"grid-template-areas"),areaRows,areaColumns);
    size_t columnCount=std::max<size_t>(1,std::max(columnDefinitions->size(),areaColumns));
    const size_t explicitRows=std::max(areaRows,rowDefinitions->size());
    size_t usedRows=0;const auto items=PlaceGridItems(box,areas,explicitRows,columnCount,usedRows);
    const size_t rowCount=std::max<size_t>(1,std::max(rowDefinitions->size(),usedRows));
    const float columnGap=GapValue(box.style,true,gridWidth,viewportWidth_);
    const float rowGap=GapValue(box.style,false,box.content.height,viewportHeight_);
    const std::vector<float> provisionalRows(rowCount,std::max(1.0f,(box.content.height-rowGap*std::max(0,static_cast<int>(rowCount)-1))/rowCount));
    const auto justifyContent=box.style.Get(L"justify-content",L"normal");
    const auto alignContent=box.style.Get(L"align-content",L"normal");
    const auto completeColumns=CompleteGridTracks(*columnDefinitions,box,true,columnCount,gridWidth);
    const auto completeRows=CompleteGridTracks(*rowDefinitions,box,false,rowCount,box.content.height);
    const auto columns=ResolveGridTracks(completeColumns,columnCount,gridWidth,columnGap,viewportWidth_,items,true,provisionalRows,definiteWidth,StretchesGridAutoTracks(justifyContent));
    const auto rows=ResolveGridTracks(completeRows,rowCount,box.content.height,rowGap,viewportHeight_,items,false,columns,definiteHeight,StretchesGridAutoTracks(alignContent));
    const auto horizontalDistribution=DistributeGridContent(justifyContent,gridWidth,columns,columnGap);
    const auto verticalDistribution=DistributeGridContent(alignContent,box.content.height,rows,rowGap);
    const float distributedColumnGap=columnGap+horizontalDistribution.extraGap;
    const float distributedRowGap=rowGap+verticalDistribution.extraGap;
    std::vector<float> x(columns.size()),y(rows.size());float position=box.content.x+horizontalDistribution.offset;
    for(size_t i=0;i<columns.size();++i){x[i]=position;position+=columns[i]+distributedColumnGap;}
    position=box.content.y+verticalDistribution.offset;for(size_t i=0;i<rows.size();++i){y[i]=position;position+=rows[i]+distributedRowGap;}
    const auto parentAlign=box.style.Get(L"align-items",L"stretch"),parentJustify=box.style.Get(L"justify-items",L"stretch");
    const bool rtl=box.style.Is(L"direction",L"rtl");
    for(const auto& item:items){
        if(item.row>=rows.size()||item.column>=columns.size())continue;
        float cellWidth=distributedColumnGap*std::max(0,static_cast<int>(item.columnSpan)-1),cellHeight=distributedRowGap*std::max(0,static_cast<int>(item.rowSpan)-1);
        for(size_t index=0;index<item.columnSpan&&item.column+index<columns.size();++index)cellWidth+=columns[item.column+index];
        for(size_t index=0;index<item.rowSpan&&item.row+index<rows.size();++index)cellHeight+=rows[item.row+index];
        auto align=item.box->style.Get(L"align-self",L"auto");if(align.empty()||align==L"auto")align=parentAlign;
        auto justify=item.box->style.Get(L"justify-self",L"auto");if(justify.empty()||justify==L"auto")justify=parentJustify;
        const auto cssHeight=item.box->style.Get(L"height"),cssWidth=item.box->style.Get(L"width");
        float width=cellWidth,height=cellHeight,left=rtl?
            box.content.x+gridWidth-(x[item.column]-box.content.x)-cellWidth:
            x[item.column],top=y[item.row];
        if(HasIntrinsicWidth(*item.box)||(!cssWidth.empty()&&cssWidth!=L"auto"))
            width=BlockOuterWidth(*item.box,cellWidth,viewportWidth_);
        else if(justify!=L"stretch")width=std::min(cellWidth,NaturalWidth(*item.box));
        if(!cssHeight.empty()&&cssHeight!=L"auto")
            height=BlockOuterHeight(*item.box,cellHeight,cellWidth,viewportHeight_,viewportWidth_);
        else if(align!=L"stretch")height=std::min(cellHeight,NaturalHeight(*item.box,width));
        const bool autoLeft=ToLower(Trim(item.box->style.Get(L"margin-left")))==L"auto";
        const bool autoRight=ToLower(Trim(item.box->style.Get(L"margin-right")))==L"auto";
        const bool autoTop=ToLower(Trim(item.box->style.Get(L"margin-top")))==L"auto";
        const bool autoBottom=ToLower(Trim(item.box->style.Get(L"margin-bottom")))==L"auto";
        if(autoLeft&&autoRight)left+=(cellWidth-width)/2;
        else if(autoLeft)left+=cellWidth-width;
        else if(!autoRight){
            if(justify==L"center")left+=(cellWidth-width)/2;
            else{
                const bool itemRtl=item.box->style.Is(L"direction",L"rtl");
                const bool alignRight=justify==L"right"?true:justify==L"left"?false:
                    justify==L"self-start"?itemRtl:justify==L"self-end"?!itemRtl:
                    (justify==L"end"||justify==L"flex-end")?!rtl:rtl;
                if(alignRight)left+=cellWidth-width;
            }
        }
        if(autoTop&&autoBottom)top+=(cellHeight-height)/2;
        else if(autoTop)top+=cellHeight-height;
        else if(!autoBottom){
            if(align==L"center")top+=(cellHeight-height)/2;
            else if(align==L"end"||align==L"flex-end")top+=cellHeight-height;
        }
        LayoutBoxTree(*item.box,{left,top,width,height},true);
    }
    for(auto& child:box.children)if(child->visible&&(child->style.Is(L"position",L"absolute")||child->style.Is(L"position",L"fixed"))){
        const auto* containingBlock=AbsoluteContainingBlockAncestor(*child);
        auto area=AbsoluteContainingBlock(*child,viewportWidth_,viewportHeight_);
        if(!child->style.Is(L"position",L"fixed")&&containingBlock==&box)
            area.width=std::max(0.0f,area.width-(box.content.width-gridWidth));
        LayoutBoxTree(*child,PositionedRect(*child,area,viewportWidth_,viewportHeight_),true);
    }
}

void LayoutEngine::LayoutTable(LayoutBox& box){
    const auto& model=BuildTableGrid(box);
    if(!model.columnCount||model.rows.empty()){LayoutBlock(box);return;}
    const auto& columns=ResolveTableColumns(box,model,box.content.width,viewportWidth_);
    const auto spacing=TableBorderSpacing(box);
    auto rows=ResolveTableRows(model,columns,spacing);
    float captionHeight=0,topCaptionHeight=0;
    for(auto* caption:model.captions){
        const float height=NaturalHeight(*caption,box.content.width);
        if(!caption->style.Is(L"caption-side",L"bottom")){
            LayoutBoxTree(*caption,{box.content.x,box.content.y+topCaptionHeight,box.content.width,height},true);
            topCaptionHeight+=height;
        }
        captionHeight+=height;
    }
    float rowsHeight=spacing.y*(rows.size()+1);for(const auto height:rows)rowsHeight+=height;
    if(rowsHeight>0&&box.content.height-captionHeight>rowsHeight+0.01f){
        const float share=(box.content.height-captionHeight-rowsHeight)/static_cast<float>(rows.size());
        for(auto& height:rows)height+=share;
    }

    std::vector<float> x(columns.size()),y(rows.size());
    const bool rtl=box.style.Is(L"direction",L"rtl");
    float position=box.content.x+(rtl?box.content.width-spacing.x:spacing.x);
    for(size_t column=0;column<columns.size();++column){
        if(rtl){position-=columns[column];x[column]=position;position-=spacing.x;}
        else{x[column]=position;position+=columns[column]+spacing.x;}
    }
    position=box.content.y+topCaptionHeight+spacing.y;
    for(size_t row=0;row<rows.size();++row){
        y[row]=position;
        auto* rowBox=model.rows[row].box;
        rowBox->rect={box.content.x+spacing.x,position,std::max(0.0f,box.content.width-2*spacing.x),rows[row]};
        rowBox->content=rowBox->rect;
        position+=rows[row]+spacing.y;
    }
    for(auto* caption:model.captions)if(caption->style.Is(L"caption-side",L"bottom")){
        const float height=NaturalHeight(*caption,box.content.width);
        LayoutBoxTree(*caption,{box.content.x,position,box.content.width,height},true);
        position+=height;
    }
    for(size_t column=0;column<model.columns.size();){
        auto* columnBox=model.columns[column].box;
        size_t end=column+1;float width=columns[column];
        while(end<model.columns.size()&&model.columns[end].box==columnBox){width+=spacing.x+columns[end];++end;}
        columnBox->rect={x[rtl?end-1:column],y.front(),width,y.back()+rows.back()-y.front()};
        columnBox->content=columnBox->rect;
        column=end;
    }
    for(size_t column=0;column<model.columns.size();){
        auto* group=model.columns[column].group;
        if(!group){++column;continue;}
        size_t end=column+1;float width=columns[column];
        while(end<model.columns.size()&&model.columns[end].group==group){width+=spacing.x+columns[end];++end;}
        group->rect={x[rtl?end-1:column],y.front(),width,y.back()+rows.back()-y.front()};group->content=group->rect;
        column=end;
    }
    for(const auto& cell:model.cells){
        if(cell.row>=rows.size()||cell.column>=columns.size())continue;
        float width=0,height=0;
        for(size_t column=cell.column;
            column<std::min(columns.size(),cell.column+cell.columnSpan);++column)
            width+=columns[column];
        for(size_t row=cell.row;row<std::min(rows.size(),cell.row+cell.rowSpan);++row)
            height+=rows[row];
        width+=spacing.x*(std::min(cell.columnSpan,columns.size()-cell.column)-1);
        height+=spacing.y*(std::min(cell.rowSpan,rows.size()-cell.row)-1);
        const size_t leftColumn=rtl?std::min(columns.size(),cell.column+cell.columnSpan)-1:cell.column;
        LayoutBoxTree(*cell.box,{x[leftColumn],y[cell.row],width,height},true);
    }
    std::function<bool(LayoutBox&,LayoutRect&)> fitGroups=
        [&](LayoutBox& current,LayoutRect& bounds){
            if(&current!=&box&&IsTable(current))return false;
            if(HasTableDisplay(current,L"table-row")){bounds=current.rect;return true;}
            bool found=false;float left=0,top=0,right=0,bottom=0;
            for(auto& child:current.children){
                LayoutRect childBounds;if(!fitGroups(*child,childBounds))continue;
                if(!found){left=childBounds.x;top=childBounds.y;
                    right=childBounds.x+childBounds.width;
                    bottom=childBounds.y+childBounds.height;found=true;}
                else{left=std::min(left,childBounds.x);top=std::min(top,childBounds.y);
                    right=std::max(right,childBounds.x+childBounds.width);
                    bottom=std::max(bottom,childBounds.y+childBounds.height);}
            }
            if(found&&IsTableRowGroup(current)){
                current.rect={left,top,right-left,bottom-top};current.content=current.rect;
            }
            if(found)bounds={left,top,right-left,bottom-top};
            return found;
        };
    LayoutRect ignored;for(auto& child:box.children)fitGroups(*child,ignored);
}

ID2D1SolidColorBrush* LayoutEngine::SolidBrush(ID2D1RenderTarget* target,unsigned int color){
    if(!target)return nullptr;
    // Keep the resource owner alive so a fresh target at a reused address
    // cannot accidentally inherit brushes belonging to a destroyed target.
    if(brushCacheTarget_.Get()!=target){brushCache_.clear();brushCacheTarget_=target;}
    const auto found=brushCache_.find(color);
    if(found!=brushCache_.end())return found->second.Get();
    if(brushCache_.size()>=512)brushCache_.clear();
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    if(FAILED(target->CreateSolidColorBrush(D2DColor(color),&brush)))return nullptr;
    auto inserted=brushCache_.emplace(color,std::move(brush));
    return inserted.first->second.Get();
}

ID2D1SolidColorBrush* LayoutEngine::SolidBrush(ID2D1RenderTarget* target,const D2D1_COLOR_F& color){
    const auto channel=[](float value){return static_cast<unsigned int>(std::lround(std::max(0.0f,std::min(1.0f,value))*255.0f));};
    return SolidBrush(target,(channel(color.a)<<24)|(channel(color.r)<<16)|(channel(color.g)<<8)|channel(color.b));
}

void LayoutEngine::EnsureGeometryResources(ID2D1RenderTarget* target){
    if(!target)return;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;target->GetFactory(&factory);
    if(!factory)return;
    if(geometryFactory_!=factory.Get()){
        selectArrowGeometry_.Reset();verticalArrowGeometry_.Reset();horizontalArrowGeometry_.Reset();
        svgGeometryCache_.clear();geometryFactory_=factory.Get();
    }
    auto create=[&](Microsoft::WRL::ComPtr<ID2D1PathGeometry>& geometry,
                    std::initializer_list<D2D1_POINT_2F> points,bool closed=true){
        if(geometry||points.size()<3)return;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if(FAILED(factory->CreatePathGeometry(&geometry))||FAILED(geometry->Open(&sink))){geometry.Reset();return;}
        auto point=points.begin();sink->BeginFigure(*point++,closed?D2D1_FIGURE_BEGIN_FILLED:D2D1_FIGURE_BEGIN_HOLLOW);
        for(;point!=points.end();++point)sink->AddLine(*point);
        sink->EndFigure(closed?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);
        if(FAILED(sink->Close()))geometry.Reset();
    };
    create(selectArrowGeometry_,{D2D1::Point2F(-4,-2),D2D1::Point2F(0,2),D2D1::Point2F(4,-2)},false);
    create(verticalArrowGeometry_,{D2D1::Point2F(0,0),D2D1::Point2F(-1,1),D2D1::Point2F(1,1)});
    create(horizontalArrowGeometry_,{D2D1::Point2F(0,0),D2D1::Point2F(1,-1),D2D1::Point2F(1,1)});
}

void LayoutEngine::Paint(ID2D1RenderTarget* target,IDWriteFactory* factory,const LayoutRect* dirtyBounds){
    if(!root_||!target||!factory)return;
    // Opaque page surfaces support subpixel coverage; transparent compositing
    // surfaces require grayscale coverage so text survives alpha blending.
    // This choice applies to every text run without changing CSS font metrics.
    ConfigureWebTextRendering(target,factory);
    EnsureGeometryResources(target);
    LayoutRect clip{0,0,viewportWidth_,viewportHeight_};
    if(dirtyBounds){
        const float right=std::min(viewportWidth_,dirtyBounds->x+dirtyBounds->width);
        const float bottom=std::min(viewportHeight_,dirtyBounds->y+dirtyBounds->height);
        clip.x=std::max(0.0f,dirtyBounds->x);clip.y=std::max(0.0f,dirtyBounds->y);
        clip.width=std::max(0.0f,right-clip.x);clip.height=std::max(0.0f,bottom-clip.y);
        if(clip.width<=0||clip.height<=0)return;
    }
    // The HTML/body boxes only change when the layout tree is rebuilt. Keep
    // direct candidates instead of walking every layout box before each dirty
    // paint; large off-screen code or diff views must not tax an unrelated
    // scrolling pane.
    canvasBackgroundBox_=canvasHtmlBox_&&HasCanvasBackground(canvasHtmlBox_->style)?canvasHtmlBox_:
        (canvasBodyBox_&&HasCanvasBackground(canvasBodyBox_->style)?canvasBodyBox_:nullptr);
    if(canvasBackgroundBox_){
        PushPaintClip(target,PixelAlignedRect(clip,deviceScale_),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const LayoutRect canvas{0,0,viewportWidth_,viewportHeight_};const CornerRadii radius{};
        const auto color=BackgroundColor(canvasBackgroundBox_->style);
        if((color>>24)!=0)target->FillRectangle(PixelAlignedRect(canvas,deviceScale_),SolidBrush(target,color));
        PaintGradientBackgrounds(target,canvasBackgroundBox_->style,canvas,radius,viewportWidth_);
        PaintImageBackgrounds(target,canvasBackgroundBox_->style,canvas,radius,viewportWidth_,
            document_,styleSheet_,svgBackgroundCache_,svgGeometryCache_,rasterImageResolver_,
            imageBitmapCacheTarget_,imageBitmapCache_);
        PopPaintClip(target);
    }
    paintingTopLayer_=nullptr;
    PaintStackingContext(target,factory,*root_,clip);
    // Modal dialogs and their backdrops are HTML top-layer boxes: they paint
    // after every document stacking context and are not clipped by ancestors.
    for(auto* dialog:modalBoxes_)if(dialog&&dialog->visible){
        PaintDialogBackdrop(target,*dialog,clip);
        paintingTopLayer_=dialog;
        PaintStackingContext(target,factory,*dialog,clip);
    }
    paintingTopLayer_=nullptr;
    // The viewport's scrolling UI belongs to the browsing context. Paint it
    // after document stacking contexts and top-layer content, independently
    // of the authored body's transform, opacity and border radius.
    if(root_->visible&&root_->viewportScrollContainer){
        PushPaintClip(target,PixelAlignedRect(clip,deviceScale_),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        PaintScrollbars(target,*root_);
        PopPaintClip(target);
    }
    canvasBackgroundBox_=nullptr;
}

void LayoutEngine::PaintDialogBackdrop(ID2D1RenderTarget* target,const LayoutBox& dialog,
                                       const LayoutRect& clipBounds){
    if(!target||!dialog.node||!dialog.node->modal)return;
    auto style=styleSheet_.Compute(dialog.node,nullptr,L"backdrop");
    style.deviceScale=deviceScale_;
    if(style.Is(L"visibility",L"hidden"))return;
    float opacity=1.0f;TryParseFloat(style.Get(L"opacity",L"1"),opacity);
    opacity=std::max(0.0f,std::min(1.0f,opacity));if(opacity<=0.001f)return;
    PushPaintClip(target,PixelAlignedRect(clipBounds,deviceScale_),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    Microsoft::WRL::ComPtr<ID2D1Layer> opacityLayer;
    if(opacity<0.999f&&SUCCEEDED(target->CreateLayer(nullptr,&opacityLayer)))
        PushPaintLayer(target,D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),opacity),opacityLayer.Get());
    const LayoutRect viewport{0,0,viewportWidth_,viewportHeight_};
    const CornerRadii radius{};const auto color=BackgroundColor(style);
    if((color>>24)!=0)target->FillRectangle(PixelAlignedRect(viewport,deviceScale_),SolidBrush(target,color));
    PaintGradientBackgrounds(target,style,viewport,radius,viewportWidth_);
    PaintImageBackgrounds(target,style,viewport,radius,viewportWidth_,document_,styleSheet_,
        svgBackgroundCache_,svgGeometryCache_,rasterImageResolver_,imageBitmapCacheTarget_,
        imageBitmapCache_);
    if(opacityLayer)PopPaintLayer(target);
    PopPaintClip(target);
}

void LayoutEngine::PaintStackingContext(ID2D1RenderTarget* target,IDWriteFactory* factory,
                                        LayoutBox& box,const LayoutRect& clipBounds){
    if(clipBounds.width<=0||clipBounds.height<=0)return;
    if(!Intersects(box.subtreeBounds,clipBounds))return;
    if(box.node&&box.node->modal&& &box!=paintingTopLayer_)return;
    float opacity=1;TryParseFloat(box.style.Get(L"opacity",L"1"),opacity);
    // The compositing surface stores eight-bit coverage. Round the group
    // alpha before Direct2D's conversion, preserving computed CSS opacity.
    opacity=std::round(std::clamp(opacity,0.0f,1.0f)*255)/255;
    if(opacity<=0.001f)return;
    PushPaintClip(target,PixelAlignedRect(clipBounds,deviceScale_),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    // Keep the mask and transform active through deferred z-index descendants.
    // Clipping only PaintBox would let those descendants escape the shape.
    D2D1_MATRIX_3X2_F previousTransform{};
    const bool transformed=ApplyPaintTransform(target,box,previousTransform);
    const auto shape=ResolveBasicClip(box,viewportWidth_);
    if(shape.kind!=BasicClipShape::None&&shape.kind!=BasicClipShape::Polygon&&
       (shape.rect.width<=0||shape.rect.height<=0)){
        if(transformed)target->SetTransform(previousTransform);
        PopPaintClip(target);return;
    }
    Microsoft::WRL::ComPtr<ID2D1Layer> shapeLayer;
    Microsoft::WRL::ComPtr<ID2D1Geometry> shapeGeometry;
    std::unique_ptr<ScopedWebTextRendering> shapeTextRendering;
    if(shape.kind!=BasicClipShape::None){
        Microsoft::WRL::ComPtr<ID2D1Factory> geometryFactory;target->GetFactory(&geometryFactory);
        shapeGeometry=BasicClipGeometry(geometryFactory.Get(),shape);
        if(shapeGeometry&&SUCCEEDED(target->CreateLayer(nullptr,&shapeLayer))){
            PushPaintLayer(target,D2D1::LayerParameters(D2D1::InfiniteRect(),shapeGeometry.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),shapeLayer.Get());
            shapeTextRendering=std::make_unique<ScopedWebTextRendering>(target,factory,true);
        }
    }
    Microsoft::WRL::ComPtr<ID2D1Layer> opacityLayer;
    std::unique_ptr<ScopedWebTextRendering> opacityTextRendering;
    if(opacity<0.999f&&SUCCEEDED(target->CreateLayer(nullptr,&opacityLayer))){
        PushPaintLayer(target,D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),opacity),opacityLayer.Get());
        opacityTextRendering=std::make_unique<ScopedWebTextRendering>(target,factory,true);
    }
    const auto* paintScope=box.establishesStackingContext?&box:box.deferredStackingScope;
    PaintBox(target,factory,box,clipBounds,paintScope,true);
    const auto contextClip=ClipsOverflow(box)?IntersectRects(clipBounds,OverflowClipRect(box)):clipBounds;
    for(auto* context:box.nonNegativeStackingContexts)
        if(Intersects(context->subtreeBounds,contextClip))
            PaintStackingContext(target,factory,*context,StackingContextClip(*context,box,contextClip));
    if(opacityLayer)PopPaintLayer(target);
    opacityTextRendering.reset();
    if(shapeLayer)PopPaintLayer(target);
    shapeTextRendering.reset();
    if(transformed)target->SetTransform(previousTransform);
    PopPaintClip(target);
}

void LayoutEngine::PaintBox(ID2D1RenderTarget* target,IDWriteFactory* factory,LayoutBox& box,
                            const LayoutRect& clipBounds,
                            const LayoutBox* deferredScope,bool paintDeferredRoot){
    if(box.node&&box.node->modal&& &box!=paintingTopLayer_)return;
    if(!paintDeferredRoot&&IsDeferredContext(box,deferredScope))return;
    // An auto-sized ancestor may be outside the clip while an overflow-visible
    // descendant remains inside it. Cull the painted subtree as a unit.
    if(!box.visible||!Intersects(box.subtreeBounds,clipBounds))return;
    float opacity=1.0f;TryParseFloat(box.style.Get(L"opacity",L"1"),opacity);
    opacity=std::max(0.0f,std::min(1.0f,opacity));
    if(box.style.Is(L"visibility",L"hidden")||opacity<=0.001f)return;D2D1_MATRIX_3X2_F previousTransform{};const bool transformed=!paintDeferredRoot&&ApplyPaintTransform(target,box,previousTransform);Microsoft::WRL::ComPtr<ID2D1Layer> opacityLayer;
    std::unique_ptr<ScopedWebTextRendering> opacityTextRendering;
    if(!paintDeferredRoot&&opacity<0.999f&&SUCCEEDED(target->CreateLayer(nullptr,&opacityLayer))){
        PushPaintLayer(target,D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),opacity),opacityLayer.Get());
        opacityTextRendering=std::make_unique<ScopedWebTextRendering>(target,factory,true);
    }
    const auto background=BackgroundColor(box.style);ID2D1SolidColorBrush* brush=nullptr;
    const auto radius=ResolveCornerRadii(box.style,box.rect.width,box.rect.height,viewportWidth_);
    const auto appearance=ToLower(Trim(box.style.Get(L"appearance",
        box.style.Get(L"-webkit-appearance",L"auto"))));
    const auto borders=BorderValues(box.style);
    const auto controlType=ToLower(box.node->Attribute(L"type"));
    const auto controlImage=Trim(box.style.Get(L"background-image"));
    const bool plainControlBackground=controlImage.empty()||controlImage==L"none";
    const auto defaultBorder=[&](const wchar_t* expectedStyle,unsigned int expectedColor){
        for(const auto* side:{L"top",L"right",L"bottom",L"left"}){
            const auto value=box.style.Get(L"border-"+std::wstring(side)+L"-style",
                box.style.Get(L"border-style",box.style.Get(L"border-"+std::wstring(side),box.style.Get(L"border"))));
            if(value.find(expectedStyle)==std::wstring::npos||BorderColor(box.style,side)!=expectedColor)return false;
        }
        return true;
    };
    const bool nativeButton=plainControlBackground&&appearance!=L"none"&&(box.node->tag==L"button"||
        (box.node->tag==L"input"&&(controlType==L"button"||controlType==L"submit"||controlType==L"reset")))&&
        !radius.Any()&&borders.top==2&&borders.right==2&&borders.bottom==2&&borders.left==2&&
        defaultBorder(L"outset",0xff000000)&&(background==0xffefefef||background==0xfff0f0f0);
    const bool nativeTextField=plainControlBackground&&appearance!=L"none"&&box.node->tag==L"input"&&
        controlType!=L"button"&&controlType!=L"submit"&&controlType!=L"reset"&&
        controlType!=L"checkbox"&&controlType!=L"radio"&&!radius.Any()&&
        borders.top==2&&borders.right==2&&borders.bottom==2&&borders.left==2&&
        defaultBorder(L"inset",0xff767676)&&background==0xffffffff;
    const float themeBorder=std::max(1.0f,std::floor(box.style.deviceScale))/std::max(0.01f,box.style.deviceScale);
    const bool nativeSelect=plainControlBackground&&appearance!=L"none"&&box.node->tag==L"select"&&
        !radius.Any()&&borders.top==themeBorder&&borders.right==themeBorder&&borders.bottom==themeBorder&&borders.left==themeBorder&&
        defaultBorder(L"solid",0xff767676)&&background==0xffffffff;
    const bool nativeDecoration=nativeButton||nativeTextField||nativeSelect;
    PaintOuterBoxShadows(target,box.style,box.rect,radius,viewportWidth_,deviceScale_,
                         shadowBitmapCacheTarget_,shadowBitmapCache_);
    if(&box!=canvasBackgroundBox_&&!nativeDecoration){
        if((background>>24)!=0){
            brush=SolidBrush(target,background);const auto rect=PixelAlignedRect(box.rect,deviceScale_);
            const auto bleed=BackgroundBleedInset(box.style,radius);
            if(box.splitInlineRectValid){
                // An inline interrupted by a block paints its own line
                // fragments; the union DOM bounds include the block but are
                // not a background rectangle. Offsets follow scrolling.
                for(auto fragment:box.splitInlinePaintOffsets){
                    fragment.x+=box.rect.x;fragment.y+=box.rect.y;
                    target->FillRectangle(PixelAlignedRect(fragment,deviceScale_),brush);
                }
            }else FillRoundedBox(target,D2D1::RectF(rect.left+bleed.left,rect.top+bleed.top,rect.right-bleed.right,rect.bottom-bleed.bottom),
                InsetCornerRadii(radius,bleed),brush);
        }
        PaintGradientBackgrounds(target,box.style,box.rect,radius,viewportWidth_);
        PaintImageBackgrounds(target,box.style,box.rect,radius,viewportWidth_,document_,styleSheet_,
                              svgBackgroundCache_,svgGeometryCache_,rasterImageResolver_,
                              imageBitmapCacheTarget_,imageBitmapCache_);
    }
    PaintInsetBoxShadows(target,box.style,box.rect,viewportWidth_);
    const bool uniform=borders.top==borders.right&&borders.top==borders.bottom&&borders.top==borders.left;
    const LayoutBox* collapsedTable=nullptr;
    if(IsTable(box)&&box.style.Is(L"border-collapse",L"collapse"))collapsedTable=&box;
    else if(HasTableDisplay(box,L"table-cell")||HasTableDisplay(box,L"table-row")||IsTableRowGroup(box))
        for(auto* ancestor=box.parent;ancestor;ancestor=ancestor->parent)
            if(IsTable(*ancestor)){
                if(ancestor->style.Is(L"border-collapse",L"collapse"))collapsedTable=ancestor;
                break;
            }
    const bool collapsedTableCell=collapsedTable!=nullptr;
    const auto outerRect=PixelAlignedRect(box.rect,box.style.deviceScale);
    if(nativeDecoration){
        // Theme borders quantize their thickness in device pixels. Keep the
        // UA layout border separate from this decorative frame.
        const float scale=std::max(0.01f,box.style.deviceScale);
        const float width=std::max(1.0f,std::floor(scale))/scale;
        const float corner=std::max(1.0f,std::floor(2*scale))/scale;
        const auto frame=D2D1::RectF(outerRect.left+width/2,outerRect.top+width/2,
            outerRect.right-width/2,outerRect.bottom-width/2);
        const auto antialias=target->GetAntialiasMode();
        if(nativeTextField||nativeSelect)target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        brush=SolidBrush(target,nativeButton?0xffefefef:0xffffffff);
        if(!PaintSoftwareRoundedRect(target,D2D1::RoundedRect(frame,corner,corner),brush))
            target->FillRoundedRectangle(D2D1::RoundedRect(frame,corner,corner),brush);
        brush=SolidBrush(target,box.node->disabled?0xffd1d1d1:0xff767676);
        if(!PaintSoftwareRoundedRect(target,D2D1::RoundedRect(frame,corner,corner),brush,width))
            target->DrawRoundedRectangle(D2D1::RoundedRect(frame,corner,corner),brush,width);
        target->SetAntialiasMode(antialias);
    }else if(!collapsedTable){
    if(uniform&&borders.top>0&&!collapsedTableCell){
        brush=SolidBrush(target,BorderColor(box.style,L"top"));const float inset=borders.top/2;
        const auto rect=D2D1::RectF(outerRect.left+inset,outerRect.top+inset,outerRect.right-inset,outerRect.bottom-inset);
        const auto borderRadius=InsetCornerRadii(radius,{inset,inset,inset,inset});
        if(borderRadius.Any()){
            bool painted=false;
            if(!radius.Uniform()&&activeRasterSurface&&activeRasterSurface->target==target){
                const auto inner=D2D1::RectF(outerRect.left+borders.left,outerRect.top+borders.top,
                    outerRect.right-borders.right,outerRect.bottom-borders.bottom);
                const auto innerRadius=InsetCornerRadii(radius,borders);
                if(inner.right>inner.left&&inner.bottom>inner.top)
                    painted=activeRasterSurface->PaintSkia(outerRect,radius.corners,BorderColor(box.style,L"top"),0,&inner,&innerRadius.corners);
            }
            if(!painted)if(auto geometry=RoundedBoxGeometry(target,rect,borderRadius))target->DrawGeometry(geometry.Get(),brush,borders.top);
        }else target->DrawRectangle(rect,brush,borders.top);
    }
    else{
        FLOAT dpiX=USER_DEFAULT_SCREEN_DPI,dpiY=USER_DEFAULT_SCREEN_DPI;target->GetDpi(&dpiX,&dpiY);
        const auto pixelCenter=[](float value,float dpi){const float scale=dpi/USER_DEFAULT_SCREEN_DPI;return scale>0?(std::round(value*scale-0.5f)+0.5f)/scale:value;};
        // Collapsed table edges are shared, so paint each join once. Cells on
        // the right own vertical joins and cells above own horizontal joins;
        // only the outer top/right edges need an additional stroke.
        const float epsilon=0.51f/std::max(0.01f,box.style.deviceScale);
        const bool tableTop=!collapsedTableCell||
            std::abs(box.rect.y-collapsedTable->content.y)<=epsilon;
        const bool tableRight=!collapsedTableCell||
            std::abs(box.rect.x+box.rect.width-
                     (collapsedTable->content.x+collapsedTable->content.width))<=epsilon;
        if(borders.top>0&&tableTop){brush=SolidBrush(target,BorderColor(box.style,L"top"));const float y=pixelCenter(outerRect.top+borders.top/2,dpiY);target->DrawLine(D2D1::Point2F(outerRect.left,y),D2D1::Point2F(outerRect.right,y),brush,borders.top);}
        if(borders.right>0&&tableRight){brush=SolidBrush(target,BorderColor(box.style,L"right"));const float x=pixelCenter(outerRect.right+(collapsedTableCell?borders.right/2:-borders.right/2),dpiX);target->DrawLine(D2D1::Point2F(x,outerRect.top),D2D1::Point2F(x,outerRect.bottom),brush,borders.right);}
        if(borders.bottom>0){brush=SolidBrush(target,BorderColor(box.style,L"bottom"));const float y=pixelCenter(outerRect.bottom+(collapsedTableCell?borders.bottom/2:-borders.bottom/2),dpiY);target->DrawLine(D2D1::Point2F(outerRect.left,y),D2D1::Point2F(outerRect.right,y),brush,borders.bottom);}
        if(borders.left>0){brush=SolidBrush(target,BorderColor(box.style,L"left"));const float x=pixelCenter(outerRect.left+borders.left/2,dpiX);target->DrawLine(D2D1::Point2F(x,outerRect.top),D2D1::Point2F(x,outerRect.bottom),brush,borders.left);}
    }
    }
    // A browsing context is replaced content in its owner's CSS stacking
    // context. Paint it here, inside the owner's transform/opacity and clips,
    // before later siblings and higher z-index contexts.
    if(box.node->tag==L"iframe"&&framePainter_)framePainter_(target,box);
    const auto listMarker=ListMarkerText(box);
    if(!listMarker.empty()&&factory){
        auto format=TextFormat(factory,box.style);
        Microsoft::WRL::ComPtr<IDWriteTextLayout> markerLayout;
        const float fontSize=FontSize(box.style);
        const float markerWidth=std::max(24.0f,fontSize*3.0f);
        const float markerHeight=std::max(1.0f,LineHeight(box.style));
        if(format&&SUCCEEDED(factory->CreateTextLayout(listMarker.c_str(),
                static_cast<UINT32>(listMarker.size()),format.Get(),markerWidth,
                markerHeight,&markerLayout))){
            markerLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            markerLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            markerLayout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                         markerHeight,TextBaselineOffset(box.style));
            ApplyFontFallback(factory,markerLayout.Get(),listMarker,box.style);
            ApplyCharacterSpacing(markerLayout.Get(),listMarker,box.style);
            const float gap=std::max(6.0f,fontSize*0.45f);
            const float markerLeft=box.content.x-gap-markerWidth;
            brush=SolidBrush(target,StyleSheet::Color(box.style.Get(L"color",L"#000"),
                                                       0xff000000));
            DrawWebTextLayout(target,factory,D2D1::Point2F(markerLeft,box.content.y),markerLayout.Get(),brush);
        }
    }
    if(appearance!=L"none"&&box.node->tag==L"input"&&
       (box.node->Attribute(L"type")==L"checkbox"||box.node->Attribute(L"type")==L"radio")){
        const auto type=box.node->Attribute(L"type");
        const auto aligned=PixelAlignedRect(box.rect,deviceScale_);
        // Checkbox/radio themes paint an integer unzoomed rectangle, then
        // restore the display scale and center the largest fitting square.
        const float width=std::max(1.0f,std::floor(aligned.right-aligned.left));
        const float height=std::max(1.0f,std::floor(aligned.bottom-aligned.top));
        const float size=std::min(width,height);
        const float left=aligned.left+(width-size)/2;
        const float top=aligned.top+(height-size)/2;
        const auto r=D2D1::RectF(left,top,left+size,top+size);
        const auto accent=box.node->disabled?0x4d767676:StyleSheet::Color(box.style.Get(L"accent-color",L"#0075ff"),0xff0075ff);
        if(type==L"radio"){
            brush=SolidBrush(target,0xff6b7280);
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((r.left+r.right)/2,(r.top+r.bottom)/2),size/2-1,size/2-1),brush,1);
            if(box.node->checked){brush=SolidBrush(target,accent);target->FillEllipse(D2D1::Ellipse(D2D1::Point2F((r.left+r.right)/2,(r.top+r.bottom)/2),4,4),brush);}
        }else{
            const float controlRadius=2.0f;
            if(box.node->indeterminate){
                brush=SolidBrush(target,accent);
                if(!PaintSoftwareRoundedRect(target,D2D1::RoundedRect(r,controlRadius,controlRadius),brush))
                    target->FillRoundedRectangle(D2D1::RoundedRect(r,controlRadius,controlRadius),brush);
                brush=SolidBrush(target,0xffffffff);const float inset=size*0.25f;
                target->FillRectangle(D2D1::RectF(r.left+inset,r.top+size*0.45f,
                    r.right-inset,r.top+size*0.55f),brush);
            }else if(box.node->checked){
                brush=SolidBrush(target,accent);
                if(!PaintSoftwareRoundedRect(target,D2D1::RoundedRect(r,controlRadius,controlRadius),brush))
                    target->FillRoundedRectangle(D2D1::RoundedRect(r,controlRadius,controlRadius),brush);
                brush=SolidBrush(target,0xffffffff);
                Microsoft::WRL::ComPtr<ID2D1Factory> checkFactory;target->GetFactory(&checkFactory);
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> check;
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if(checkFactory&&SUCCEEDED(checkFactory->CreatePathGeometry(&check))&&SUCCEEDED(check->Open(&sink))){
                    const auto start=D2D1::Point2F(r.left+size*0.2f,r.top+size*0.5f);
                    sink->BeginFigure(start,D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddLine(D2D1::Point2F(start.x+size*0.2f,start.y+size*0.2f));
                    sink->AddLine(D2D1::Point2F(r.right-size*0.2f,r.top+size*0.2f));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if(SUCCEEDED(sink->Close())&&(!activeRasterSurface||activeRasterSurface->target!=target||
                       !activeRasterSurface->DrawGeometry(check.Get(),brush,size*0.16f)))
                        target->DrawGeometry(check.Get(),brush,size*0.16f);
                }
            }else{
                const auto inner=D2D1::RectF(r.left+0.2f,r.top+0.2f,r.right-0.2f,r.bottom-0.2f);
                if(box.node->disabled){
                    brush=SolidBrush(target,0x33a9a9a9);
                    if(!PaintSoftwareRoundedRect(target,D2D1::RoundedRect(inner,controlRadius,controlRadius),brush))
                        target->FillRoundedRectangle(D2D1::RoundedRect(inner,controlRadius,controlRadius),brush);
                }
                brush=SolidBrush(target,box.node->disabled?0x99ffffff:0xffffffff);
                if(!PaintSoftwareRoundedRect(target,D2D1::RoundedRect(inner,controlRadius,controlRadius),brush))
                    target->FillRoundedRectangle(D2D1::RoundedRect(inner,controlRadius,controlRadius),brush);
                brush=SolidBrush(target,box.node->disabled?0x4d767676:0xff767676);
                const auto frame=D2D1::RoundedRect(D2D1::RectF(r.left+0.5f,r.top+0.5f,
                    r.right-0.5f,r.bottom-0.5f),controlRadius,controlRadius);
                if(!PaintSoftwareRoundedRect(target,frame,brush,1))target->DrawRoundedRectangle(frame,brush,1);
            }
        }
    }
    if(box.node->tag==L"svg")PaintSvg(target,box,styleSheet_,svgGeometryCache_);
    if(box.node->tag==L"canvas")PaintCanvas(target,factory,box);
    if(box.node->tag==L"img")PaintRasterImage(target,box,imageBitmapCacheTarget_,imageBitmapCache_);
    bool placeholderText=false;std::wstring text=BoxText(box,&placeholderText);
    if(box.node->tag==L"img"&&!box.node->image)text=box.node->Attribute(L"alt");
    if(box.node->type==NodeType::Text&&!text.empty()){
        const auto* owner=box.parent;
        if(owner&&owner->style.Is(L"text-overflow",L"ellipsis")&&
           owner->style.Is(L"white-space",L"nowrap")&&
           (owner->style.Is(L"overflow",L"hidden")||owner->style.Is(L"overflow-x",L"hidden"))){
            const float available=std::max(0.0f,clipBounds.x+clipBounds.width-box.content.x);
            if(TextWidth(text,box.style)>available+0.5f){
                const std::wstring marker=L"\u2026";
                if(TextWidth(marker,box.style)>available)text.clear();
                else{
                    size_t low=0,high=text.size();
                    while(low<high){
                        const size_t middle=(low+high+1)/2;
                        if(TextWidth(text.substr(0,middle)+marker,box.style)<=available)low=middle;
                        else high=middle-1;
                    }
                    if(low>0&&low<text.size()&&text[low-1]>=0xd800&&text[low-1]<=0xdbff)--low;
                    text=text.substr(0,low)+marker;
                }
            }
        }
    }
    if(!text.empty()){
        const bool formControl=IsFormControlText(box);
        EnsureTextLayout(box,factory,text);
        if(box.textLayout){
            auto textColor=StyleSheet::Color(box.style.Get(L"color",L"#000"),0xff000000);
            float textOpacity=1.0f;
            if(placeholderText){
                const auto placeholderStyle=styleSheet_.Compute(box.node,&box.style,L"placeholder");
                textColor=StyleSheet::Color(placeholderStyle.Get(L"color"),0xff757575);
                TryParseFloat(placeholderStyle.Get(L"opacity",L"1"),textOpacity);
                textOpacity=std::max(0.0f,std::min(1.0f,textOpacity));
            }
            auto resolvedTextColor=D2DColor(textColor);resolvedTextColor.a*=textOpacity;
            brush=SolidBrush(target,resolvedTextColor);
            const auto rect=D2D1::RectF(box.content.x,box.content.y,box.content.x+box.content.width,box.content.y+box.content.height);
            // A glyph's ink may extend beyond its advance, especially with
            // negative character spacing. Only CSS overflow clips text ink.
            // Use the same device edge as the ancestor's overflow clip. A
            // second, fractional clip can recompose untouched backdrop pixels
            // at a different edge when glyph painting flushes the surface.
            auto textClip=box.node->type==NodeType::Text?
                PixelAlignedRect(clipBounds,deviceScale_):rect;
            if(box.node->tag==L"textarea"){
                const auto paddingBox=PaddingBox(box);const auto sizes=ReadElementSizes(box.node);
                textClip=PixelAlignedRect({paddingBox.x,paddingBox.y,sizes.clientWidth,sizes.clientHeight},deviceScale_);
            }
            PushPaintClip(target,textClip,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const auto origin=TextOrigin(box);
            float textLeft=origin.x;
            float textTop=origin.y;
            if(formControl&&PhysicalTextAlignment(box.style)==L"center"){
                DWRITE_TEXT_METRICS metrics{};
                if(SUCCEEDED(box.textLayout->GetMetrics(&metrics))){
                    const float layoutWidth=std::ceil(metrics.widthIncludingTrailingWhitespace*64)/64;
                    textLeft+=(metrics.widthIncludingTrailingWhitespace-layoutWidth)/2;
                }
            }
            if(formControl){
                FLOAT dpiX=USER_DEFAULT_SCREEN_DPI,dpiY=USER_DEFAULT_SCREEN_DPI;target->GetDpi(&dpiX,&dpiY);
                // Center decorative glyphs on a physical half-pixel so their
                // symmetric strokes rasterize like browser toolbar icons at
                // every display scale without a CSS-pixel offset.
                if(dpiX>0&&IsDecorativeControlText(box.node))
                    textLeft+=0.5f*USER_DEFAULT_SCREEN_DPI/dpiX;
                if(dpiY>0){
                    float pixelTop=std::round(textTop*dpiY/USER_DEFAULT_SCREEN_DPI);
                    // Control text uses natural advances and the common rounded
                    // font baseline; snap only its paint origin to device pixels.
                    textTop=pixelTop*USER_DEFAULT_SCREEN_DPI/dpiY;
                }
            }
            if(box.node->tag==L"textarea"&&box.scrollTraversalCached&&
               OverflowX(box)==L"hidden"&&OverflowY(box)==L"hidden"){
                // Only an overflowing hidden editor creates the internal
                // scrolling layer that uses grayscale coverage. Text that
                // fits retains LCD coverage. Reuse the layout overflow cache.
                ScopedWebTextRendering editorTextRendering(target,factory,true);
                DrawWebTextLayout(target,factory,D2D1::Point2F(textLeft,textTop),box.textLayout.Get(),brush);
            }else DrawWebTextLayout(target,factory,D2D1::Point2F(textLeft,textTop),box.textLayout.Get(),brush);
            PopPaintClip(target);
        }
    }
    if(box.node->tag==L"select"&&appearance!=L"none"){
        const float scale=std::max(0.01f,box.style.deviceScale);
        const auto bounds=PixelAlignedRect(box.rect,scale);
        const float arrowWidth=std::floor(8*scale),arrowHeight=std::floor(arrowWidth/2);
        const float arrowLeft=std::floor(bounds.right*scale-std::floor(scale)-(15*scale+arrowWidth)/2);
        const float arrowMiddle=std::floor(bounds.top*scale)+std::floor((bounds.bottom-bounds.top)*scale/2);
        const float centerX=(arrowLeft+arrowWidth/2)/scale;
        const float centerY=(arrowMiddle-std::floor(arrowHeight/2)+arrowHeight/2)/scale;
        brush=SolidBrush(target,StyleSheet::Color(box.style.Get(L"color",L"#000"),0xff000000));
        if(selectArrowGeometry_){
            D2D1_MATRIX_3X2_F current{};target->GetTransform(&current);
            target->SetTransform(D2D1::Matrix3x2F::Translation(centerX,centerY)*current);
            if(!activeRasterSurface||activeRasterSurface->target!=target||
               !activeRasterSurface->DrawGeometry(selectArrowGeometry_.Get(),brush,2/scale))
                target->DrawGeometry(selectArrowGeometry_.Get(),brush,2/scale);
            target->SetTransform(current);
        }
    }
    const bool clip=ClipsOverflow(box);
    LayoutRect childClip=clipBounds;
    Microsoft::WRL::ComPtr<ID2D1Layer> roundedOverflowLayer;
    Microsoft::WRL::ComPtr<ID2D1Geometry> roundedOverflowGeometry;
    bool roundedOverflowClip=false;
    std::unique_ptr<ScopedWebTextRendering> roundedTextRendering;
    if(clip){
        // Overflow clips at the padding edge. The padding-box curve is the
        // border-box radius inset by the border; clipping to content instead
        // incorrectly removes padding, while reusing the outer curve lets a
        // descendant repaint the inside edge of the rounded border.
        const auto paddingBox=OverflowClipRect(box);
        childClip=IntersectRects(clipBounds,paddingBox);
        PushPaintClip(target,PixelAlignedRect(paddingBox,deviceScale_),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto clipRadius=InsetCornerRadii(radius,borders);
        if(clipRadius.Any()){
            roundedOverflowGeometry=RoundedBoxGeometry(target,PixelAlignedRect(paddingBox,deviceScale_),clipRadius);
            if(roundedOverflowGeometry&&SUCCEEDED(target->CreateLayer(nullptr,&roundedOverflowLayer))){
                PushPaintLayer(target,D2D1::LayerParameters(D2D1::InfiniteRect(),
                    roundedOverflowGeometry.Get()),roundedOverflowLayer.Get());
                roundedOverflowClip=true;
                // The mask layer has alpha even on an opaque page target.
                // Child document paints must inherit grayscale coverage.
                roundedTextRendering=std::make_unique<ScopedWebTextRendering>(target,factory,true);
            }
        }
    }
    const auto paintChild=[&](LayoutBox& child){
        if(IsDeferredContext(child,deferredScope))return;
        if(IsStackingContext(child))PaintStackingContext(target,factory,child,childClip);
        else PaintBox(target,factory,child,childClip,deferredScope);
    };
    if(!box.verticallyOrderedChildren.empty()){
        const float top=childClip.y,bottom=childClip.y+childClip.height;
        auto first=std::lower_bound(box.verticallyOrderedChildren.begin(),box.verticallyOrderedChildren.end(),top,
            [](const LayoutBox* child,float value){return child->subtreeBounds.y+
                child->subtreeBounds.height<=value;});
        for(auto it=first;it!=box.verticallyOrderedChildren.end()&&
            (*it)->subtreeBounds.y<bottom;++it)paintChild(**it);
        for(auto* child:box.overlayChildren)paintChild(*child);
    }else{
        for(auto* child:box.paintChildren)paintChild(*child);
    }
    if(collapsedTable==&box){
        // Paint shared grid edges after cell backgrounds. A filled, snapped
        // strip keeps odd and even device widths sharp without a half-pixel
        // stroke offset, and a spanning cell contributes no internal edge.
        const auto& model=BuildTableGrid(box);
        const auto edges=ResolveCollapsedTableEdges(box,model);
        const auto& columns=ResolveTableColumns(box,model,box.content.width,viewportWidth_);
        const bool rtl=box.style.Is(L"direction",L"rtl");
        std::vector<float> x(columns.size()+1,box.content.x+(rtl?box.content.width:0)),y(model.rows.size()+1,
            model.rows.empty()?box.content.y:model.rows.front().box->rect.y);
        for(size_t c=0;c<columns.size();++c)x[c+1]=x[c]+(rtl?-columns[c]:columns[c]);
        for(size_t r=0;r<model.rows.size();++r)y[r+1]=y[r]+model.rows[r].box->rect.height;
        const auto fill=[&](const CollapsedEdge& edge,const LayoutRect& rect){
            if(edge.style&&edge.width>0&&edge.rank!=100)
                target->FillRectangle(PixelAlignedRect(rect,deviceScale_),SolidBrush(target,BorderColor(*edge.style,edge.side)));
        };
        for(size_t r=0;r<edges.horizontal.size();++r)
            for(size_t c=0;c<columns.size();++c){
                const auto& edge=edges.horizontal[r][c];
                const float left=r<edges.vertical.size()?edges.vertical[r][c].width/2:
                    (r?edges.vertical[r-1][c].width/2:0);
                const float right=r<edges.vertical.size()?edges.vertical[r][c+1].width/2:
                    (r?edges.vertical[r-1][c+1].width/2:0);
                fill(edge,{(rtl?x[c+1]-right:x[c]-left),y[r]-edge.width/2,columns[c]+left+right,edge.width});
            }
        for(size_t r=0;r<edges.vertical.size();++r)
            for(size_t c=0;c<x.size();++c){
                const auto& edge=edges.vertical[r][c];
                const size_t adjoining=std::min(c,columns.size()-1);
                const float top=edges.horizontal[r][adjoining].width/2;
                const float bottom=edges.horizontal[r+1][adjoining].width/2;
                fill(edge,{x[c]-edge.width/2,y[r]-top,edge.width,y[r+1]-y[r]+top+bottom});
            }
    }
    if(clip){
        if(roundedOverflowClip)PopPaintLayer(target);
        roundedTextRendering.reset();
        PopPaintClip(target);
    }
    if(!box.viewportScrollContainer)PaintScrollbars(target,box);
    PaintOutline(target,box.style,box.rect,radius,viewportWidth_);
    if(opacityLayer)PopPaintLayer(target);
    opacityTextRendering.reset();
    if(transformed)target->SetTransform(previousTransform);
}

void LayoutEngine::PaintScrollbars(ID2D1RenderTarget* target,const LayoutBox& box){
    ID2D1SolidColorBrush* brush=nullptr;
    VerticalScrollbarGeometry scrollbar;if(VerticalScrollbarFor(box,styleSheet_,scrollbar)){
        const auto colorScheme=ToLower(Trim(box.style.Get(L"color-scheme",L"light")));
        const bool darkScheme=!colorScheme.empty()&&Words(colorScheme).front()==L"dark";
        unsigned int thumbColor=darkScheme?0xff9f9f9f:0xff8b8b8b;
        unsigned int trackColor=darkScheme?0xff2c2c2c:0xfffcfcfc;
        const bool customScrollbar=!scrollbar.standardStyling&&styleSheet_.HasPseudoRulesFor(box.node,L"-webkit-scrollbar");
        if(customScrollbar)thumbColor=trackColor=0;
        const auto colors=Words(box.style.Get(L"scrollbar-color"));
        if(colors.size()>=2){thumbColor=StyleSheet::Color(colors[0],thumbColor);trackColor=StyleSheet::Color(colors[1],trackColor);}
        const auto trackStyle=styleSheet_.HasPseudoRules(L"-webkit-scrollbar-track")?styleSheet_.Compute(box.node,&box.style,L"-webkit-scrollbar-track"):ComputedStyle{};
        const auto trackBackground=trackStyle.Get(L"background-color",trackStyle.Get(L"background"));
        if(!scrollbar.standardStyling&&!trackBackground.empty())trackColor=StyleSheet::Color(trackBackground,trackColor);
        const auto thumbStyle=styleSheet_.HasPseudoRules(L"-webkit-scrollbar-thumb")?styleSheet_.Compute(box.node,&box.style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
        const auto thumbBackground=thumbStyle.Get(L"background-color",thumbStyle.Get(L"background"));
        if(!scrollbar.standardStyling&&!thumbBackground.empty())thumbColor=StyleSheet::Color(thumbBackground,thumbColor);
        const float trackBottom=scrollbar.track.y+scrollbar.track.height;
        if((trackColor>>24)!=0){brush=SolidBrush(target,trackColor);target->FillRectangle(D2D1::RectF(scrollbar.track.x,scrollbar.track.y,scrollbar.track.x+scrollbar.track.width,trackBottom),brush);}
        brush=SolidBrush(target,thumbColor);const auto thumbRadii=ResolveCornerRadii(thumbStyle,scrollbar.thumb.width,scrollbar.thumb.height,viewportWidth_);const float thumbRadius=scrollbar.standardStyling?std::min(scrollbar.thumb.width,scrollbar.thumb.height)/2.0f:(customScrollbar?thumbRadii.x:(thumbRadii.x>0?thumbRadii.x:std::min(scrollbar.thumb.width,scrollbar.thumb.height)/2.0f));if(!customScrollbar||scrollbar.maximum>0)target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(scrollbar.thumb.x,scrollbar.thumb.y,scrollbar.thumb.x+scrollbar.thumb.width,scrollbar.thumb.y+scrollbar.thumb.height),thumbRadius,thumbRadius),brush);
        if(!customScrollbar&&scrollbar.arrowHeight>0&&verticalArrowGeometry_){
            const float center=scrollbar.track.x+scrollbar.track.width/2;
            const float halfWidth=scrollbar.thumb.width/2.0f;
            const float figureHeight=scrollbar.arrowHeight/3.0f;
            const float padding=(scrollbar.arrowHeight-figureHeight)/2.0f;
            const float topApex=scrollbar.track.y+padding,bottomApex=trackBottom-padding;
            D2D1_MATRIX_3X2_F current{};target->GetTransform(&current);
            target->SetTransform(D2D1::Matrix3x2F::Scale(halfWidth,figureHeight)*
                D2D1::Matrix3x2F::Translation(center,topApex)*current);
            target->FillGeometry(verticalArrowGeometry_.Get(),brush);
            target->SetTransform(D2D1::Matrix3x2F::Scale(halfWidth,-figureHeight)*
                D2D1::Matrix3x2F::Translation(center,bottomApex)*current);
            target->FillGeometry(verticalArrowGeometry_.Get(),brush);target->SetTransform(current);
        }
    }
    HorizontalScrollbarGeometry horizontalScrollbar;if(HorizontalScrollbarFor(box,styleSheet_,horizontalScrollbar)){
        const auto colorScheme=ToLower(Trim(box.style.Get(L"color-scheme",L"light")));
        const bool darkScheme=!colorScheme.empty()&&Words(colorScheme).front()==L"dark";
        unsigned int thumbColor=darkScheme?0xff9f9f9f:0xff8b8b8b;
        unsigned int trackColor=darkScheme?0xff2c2c2c:0xfffcfcfc;
        const bool customScrollbar=!horizontalScrollbar.standardStyling&&styleSheet_.HasPseudoRulesFor(box.node,L"-webkit-scrollbar");
        if(customScrollbar)thumbColor=trackColor=0;
        const auto colors=Words(box.style.Get(L"scrollbar-color"));
        if(colors.size()>=2){thumbColor=StyleSheet::Color(colors[0],thumbColor);trackColor=StyleSheet::Color(colors[1],trackColor);}
        const auto trackStyle=styleSheet_.HasPseudoRules(L"-webkit-scrollbar-track")?styleSheet_.Compute(box.node,&box.style,L"-webkit-scrollbar-track"):ComputedStyle{};
        const auto trackBackground=trackStyle.Get(L"background-color",trackStyle.Get(L"background"));
        if(!horizontalScrollbar.standardStyling&&!trackBackground.empty())trackColor=StyleSheet::Color(trackBackground,trackColor);
        const auto thumbStyle=styleSheet_.HasPseudoRules(L"-webkit-scrollbar-thumb")?styleSheet_.Compute(box.node,&box.style,L"-webkit-scrollbar-thumb"):ComputedStyle{};
        const auto thumbBackground=thumbStyle.Get(L"background-color",thumbStyle.Get(L"background"));
        if(!horizontalScrollbar.standardStyling&&!thumbBackground.empty())thumbColor=StyleSheet::Color(thumbBackground,thumbColor);
        const float trackRight=horizontalScrollbar.track.x+horizontalScrollbar.track.width;
        if((trackColor>>24)!=0){brush=SolidBrush(target,trackColor);target->FillRectangle(D2D1::RectF(horizontalScrollbar.track.x,horizontalScrollbar.track.y,trackRight,horizontalScrollbar.track.y+horizontalScrollbar.track.height),brush);}
        brush=SolidBrush(target,thumbColor);
        const bool nativeTheme=!horizontalScrollbar.compactArrows;
        struct NativeBatch {
            RasterSurface* surface;
            explicit NativeBatch(RasterSurface* value):surface(value){if(surface)surface->BeginGlyphBatch();}
            ~NativeBatch(){if(surface)surface->EndGlyphBatch();}
        } nativeBatch(nativeTheme&&activeRasterSurface&&activeRasterSurface->target==target?activeRasterSurface:nullptr);
        const auto thumbRect=D2D1::RectF(horizontalScrollbar.thumb.x,horizontalScrollbar.thumb.y,
            horizontalScrollbar.thumb.x+horizontalScrollbar.thumb.width,horizontalScrollbar.thumb.y+horizontalScrollbar.thumb.height);
        if(!customScrollbar||horizontalScrollbar.maximum>0){
            if(!nativeBatch.surface||!nativeBatch.surface->FillNativeCapsule(thumbRect,brush)){
                const auto thumbRadii=nativeTheme?CornerRadii{}:ResolveCornerRadii(thumbStyle,horizontalScrollbar.thumb.width,horizontalScrollbar.thumb.height,viewportWidth_);
                const float thumbRadius=horizontalScrollbar.standardStyling?std::min(horizontalScrollbar.thumb.width,horizontalScrollbar.thumb.height)/2.0f:
                    (customScrollbar?thumbRadii.x:(thumbRadii.x>0?thumbRadii.x:std::min(horizontalScrollbar.thumb.width,horizontalScrollbar.thumb.height)/2.0f));
                target->FillRoundedRectangle(D2D1::RoundedRect(thumbRect,thumbRadius,thumbRadius),brush);
            }
        }
    if(!customScrollbar&&horizontalScrollbar.arrowWidth>0&&horizontalArrowGeometry_&&
       !DrawHorizontalScrollbarIcons(target,SharedWriteFactory(),horizontalScrollbar,brush)){
            if(nativeBatch.surface)nativeBatch.surface->FlushGlyphBatch();
            const float center=horizontalScrollbar.track.y+horizontalScrollbar.track.height/2.0f;
            const float halfHeight=horizontalScrollbar.thumb.height/2.0f;
            const float figureWidth=horizontalScrollbar.arrowWidth/3.0f;
            const float padding=(horizontalScrollbar.arrowWidth-figureWidth)/2.0f;
            const float leftApex=horizontalScrollbar.track.x+padding,rightApex=trackRight-padding;
            D2D1_MATRIX_3X2_F current{};target->GetTransform(&current);
            target->SetTransform(D2D1::Matrix3x2F::Scale(figureWidth,halfHeight)*
                D2D1::Matrix3x2F::Translation(leftApex,center)*current);
            target->FillGeometry(horizontalArrowGeometry_.Get(),brush);
            target->SetTransform(D2D1::Matrix3x2F::Scale(-figureWidth,halfHeight)*
                D2D1::Matrix3x2F::Translation(rightApex,center)*current);
            target->FillGeometry(horizontalArrowGeometry_.Get(),brush);target->SetTransform(current);
        }
    }
}

std::shared_ptr<Node> LayoutEngine::HitTest(float x,float y)const{
    if(!root_)return {};
    for(auto it=modalBoxes_.rbegin();it!=modalBoxes_.rend();++it){
        const auto* dialog=*it;if(!dialog||!dialog->visible||!dialog->node)continue;
        if(auto node=HitTestStackingContext(*dialog,x,y))return node;
        const auto backdrop=styleSheet_.Compute(dialog->node,nullptr,L"backdrop");
        if(x>=0&&y>=0&&x<viewportWidth_&&y<viewportHeight_&&
           !backdrop.Is(L"pointer-events",L"none")&&
           !backdrop.Is(L"visibility",L"hidden"))return dialog->node;
    }
    return HitTestStackingContext(*root_,x,y);
}

bool LayoutEngine::HitTestText(const std::shared_ptr<Node>& scope,float x,float y,
                               std::shared_ptr<Node>& textNode,size_t& textOffset){
    textNode.reset();textOffset=0;if(!scope||!root_)return false;
    auto scopeEntry=boxIndex_.find(scope.get());if(scopeEntry==boxIndex_.end())return false;
    const bool textScope=scope->type==NodeType::Text;
    if(textScope){
        // Preserved line breaks split one DOM text node into multiple sibling
        // layout fragments.  BoxFor(text) names the first fragment, so use the
        // nearest rendered DOM parent as the search root and filter the walk
        // back to the requested source node.
        for(auto parent=scope->parent.lock();parent;parent=parent->parent.lock()){
            const auto found=boxIndex_.find(parent.get());
            if(found!=boxIndex_.end()){scopeEntry=found;break;}
        }
    }
    LayoutBox* best=nullptr;std::shared_ptr<Node> bestSource;
    float bestDistance=std::numeric_limits<float>::max();
    const LayoutRect viewport{0,0,viewportWidth_,viewportHeight_};
    std::function<void(LayoutBox&,LayoutRect,bool)> visit=
        [&](LayoutBox& box,LayoutRect clip,bool generated){
            if(!box.visible)return;
            generated=generated||!box.pseudo.empty();
            if(ClipsOverflow(box))clip=IntersectRects(clip,OverflowClipRect(box));
            const bool textRun=box.node->type==NodeType::Text;
            const bool textControl=box.node==scope&&(box.node->tag==L"input"||box.node->tag==L"textarea");
            if(!generated&&(textRun||textControl)){
                auto source=textRun&&box.generatedFrom&&box.generatedFrom->type==NodeType::Text?
                    box.generatedFrom:box.node;
                const auto visible=IntersectRects(box.rect,clip);
                if(source&&(!textScope||source==scope)&&visible.width>0&&visible.height>0){
                    const float dx=x<visible.x?visible.x-x:(x>visible.x+visible.width?x-visible.x-visible.width:0);
                    const float dy=y<visible.y?visible.y-y:(y>visible.y+visible.height?y-visible.y-visible.height:0);
                    // A line under the pointer wins over horizontally closer
                    // text on another line, matching browser caret placement.
                    const float distance=dy*dy*16.0f+dx*dx;
                    if(distance<bestDistance){bestDistance=distance;best=&box;bestSource=std::move(source);}
                }
            }
            if(generated)return;
            for(auto& child:box.children)visit(*child,clip,generated);
        };
    visit(*scopeEntry->second,viewport,false);
    if(!best||!bestSource)return false;
    const auto text=BoxText(*best);
    EnsureTextLayout(*best,SharedWriteFactory(),text);
    size_t localOffset=text.size();
    if(best->textLayout){
        const auto origin=TextOrigin(*best);BOOL trailing=FALSE,inside=FALSE;
        DWRITE_HIT_TEST_METRICS metrics{};
        if(SUCCEEDED(best->textLayout->HitTestPoint(x-origin.x,y-origin.y,
            &trailing,&inside,&metrics)))
            localOffset=std::min(text.size(),static_cast<size_t>(metrics.textPosition)+
                (trailing?static_cast<size_t>(metrics.length):0));
    }
    const size_t sourceLength=bestSource->type==NodeType::Text?bestSource->text.size():
        bestSource->Attribute(L"value").size();
    const size_t sourceOffset=bestSource->type==NodeType::Text?best->textSourceOffset:0;
    textNode=bestSource;textOffset=std::min(sourceLength,sourceOffset+TextLayoutToSourceOffset(*best,localOffset));return true;
}

namespace {
// Replaced SVG contents have no HTML boxes. Expose the simple, untransformed
// direct text run in SVG user coordinates through the same font/range engine.
// Multi-run tspan/textPath and transformed/viewBox ranges remain unsupported.
bool SimpleSvgTextRun(const LayoutBox& viewport,const std::shared_ptr<Node>& textNode,
                      StyleSheet& styles,LayoutBox& run) {
    if (!textNode || textNode->type != NodeType::Text || viewport.node->tag != L"svg" ||
        !viewport.visible || !viewport.node->Attribute(L"viewbox").empty() ||
        !viewport.node->Attribute(L"transform").empty()) return false;
    const auto element = textNode->parent.lock();
    if (!element || element->tag != L"text" || element->namespaceUri != L"http://www.w3.org/2000/svg" ||
        element->parent.lock() != viewport.node || element->children.size() != 1 ||
        !element->Attribute(L"transform").empty() || element->attributes.count(L"rotate") ||
        element->attributes.count(L"textlength")) return false;
    run.node = textNode; run.style = styles.Compute(element, &viewport.style);
    run.style.deviceScale = viewport.style.deviceScale;
    if (run.style.Is(L"display", L"none")) return false;
    const auto layout = SvgTextLayout(element, styles, run.style, SharedWriteFactory());
    DWRITE_TEXT_METRICS metrics{};
    if (!layout || FAILED(layout->GetMetrics(&metrics))) return false;
    const auto number = [&](const wchar_t* name,float reference) {
        return StyleSheet::Length(element->Attribute(name),reference,reference,0,FontSize(run.style));
    };
    float x = viewport.content.x + number(L"x",viewport.content.width) + number(L"dx",viewport.content.width);
    const auto anchor = run.style.Get(L"text-anchor",L"start");
    if (anchor == L"middle") x -= metrics.widthIncludingTrailingWhitespace/2;
    else if (anchor == L"end") x -= metrics.widthIncludingTrailingWhitespace;
    const float y = viewport.content.y + number(L"y",viewport.content.height) + number(L"dy",viewport.content.height);
    run.content = {x,y-TextBaselineOffset(run.style),metrics.widthIncludingTrailingWhitespace,LineHeight(run.style)};
    run.rect = run.content; run.inlineTextPositioned = true;
    return true;
}
}

bool LayoutEngine::TextRangeRects(const std::shared_ptr<Node>& textNode,size_t textStart,
                                  size_t textLength,std::vector<LayoutRect>& rects){
    rects.clear();if(!textNode||!root_||!textLength)return false;
    const size_t sourceLength=textNode->type==NodeType::Text?textNode->text.size():
        textNode->Attribute(L"value").size();
    const size_t rangeStart=std::min(textStart,sourceLength);
    const size_t rangeEnd=rangeStart+std::min(textLength,sourceLength-rangeStart);
    if(rangeStart==rangeEnd)return false;
    LayoutBox* searchRoot=root_.get();
    if(textNode->type==NodeType::Text){
        for(auto parent=textNode->parent.lock();parent;parent=parent->parent.lock()){
            const auto found=boxIndex_.find(parent.get());
            if(found!=boxIndex_.end()){searchRoot=found->second;break;}
        }
    }else if(const auto found=boxIndex_.find(textNode.get());found!=boxIndex_.end())
        searchRoot=found->second;
    LayoutBox svgRun;
    if (SimpleSvgTextRun(*searchRoot,textNode,styleSheet_,svgRun)) searchRoot = &svgRun;
    std::function<void(LayoutBox&,bool)> collect=[&](LayoutBox& box,bool generated){
        if(!box.visible)return;
        generated=generated||!box.pseudo.empty();
        if(!generated){
            auto source=box.node->type==NodeType::Text&&box.generatedFrom&&
                box.generatedFrom->type==NodeType::Text?box.generatedFrom:box.node;
            if(source==textNode){
                const auto text=BoxText(box);
                const size_t sourceStart=textNode->type==NodeType::Text?box.textSourceOffset:0;
                const size_t rawLength=box.node->type==NodeType::Text?box.node->text.size():text.size();
                const size_t sourceEnd=std::min(sourceLength,sourceStart+rawLength);
                const size_t first=std::max(rangeStart,sourceStart);
                const size_t last=std::min(rangeEnd,sourceEnd);
                if(first<last){
                    EnsureTextLayout(box,SharedWriteFactory(),text);
                    if(box.textLayout){
                        const UINT32 localStart=static_cast<UINT32>(TextSourceToLayoutOffset(box,first-sourceStart));
                        const UINT32 localLength=static_cast<UINT32>(TextSourceToLayoutOffset(box,last-sourceStart)-localStart);
                        const auto origin=TextOrigin(box);UINT32 count=0;
                        box.textLayout->HitTestTextRange(localStart,localLength,
                            origin.x,origin.y,nullptr,0,&count);
                        if(count){
                            std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                            if(SUCCEEDED(box.textLayout->HitTestTextRange(localStart,localLength,
                               origin.x,origin.y,metrics.data(),count,&count))){
                                for(UINT32 index=0;index<count;++index){
                                    const auto& hit=metrics[index];
                                    rects.push_back({hit.left,hit.top,hit.width,hit.height});
                                }
                            }
                        }
                    }
                }
            }
        }
        if(generated)return;
        for(auto& child:box.children)collect(*child,generated);
    };
    collect(*searchRoot,false);return !rects.empty();
}

bool LayoutEngine::VerticalCaretPosition(const std::shared_ptr<Node>& scope,
                                         const std::shared_ptr<Node>& currentNode,
                                         size_t currentOffset,float preferredX,bool upward,
                                         std::shared_ptr<Node>& targetNode,
                                         size_t& targetOffset){
    targetNode.reset();targetOffset=0;
    if(!scope||!currentNode||!root_)return false;
    const auto scopeEntry=boxIndex_.find(scope.get());
    if(scopeEntry==boxIndex_.end()||!scopeEntry->second)return false;
    LayoutRect currentCaret{};
    if(!TextCaretRect(currentNode,currentOffset,currentCaret))return false;
    struct CaretLine {
        LayoutBox* box=nullptr;
        std::shared_ptr<Node> source;
        size_t sourceStart=0;
        size_t localStart=0;
        size_t localLength=0;
        float top=0;
        float height=0;
        float baseline=0;
        bool boundary=false;
        size_t boundaryOffset=0;
    };
    std::vector<CaretLine> lines;
    std::function<bool(LayoutBox&,bool)> collect=[&](LayoutBox& box,bool generated){
        if(!box.visible)return false;
        generated=generated||!box.pseudo.empty();
        bool hasPosition=false;
        if(!generated){
            const bool textRun=box.node->type==NodeType::Text;
            const bool textControl=box.node==scope&&
                (box.node->tag==L"input"||box.node->tag==L"textarea");
            auto source=textRun&&box.generatedFrom&&box.generatedFrom->type==NodeType::Text?
                box.generatedFrom:box.node;
            if((textRun||textControl)&&source&&
               (textControl||ParticipatesInEditableContent(source))){
                const auto text=BoxText(box);
                EnsureTextLayout(box,SharedWriteFactory(),text);
                if(box.textLayout){
                    UINT32 count=0;box.textLayout->GetLineMetrics(nullptr,0,&count);
                    if(count){
                        std::vector<DWRITE_LINE_METRICS> metrics(count);
                        if(SUCCEEDED(box.textLayout->GetLineMetrics(metrics.data(),count,&count))){
                            const auto origin=TextOrigin(box);float top=origin.y;size_t local=0;
                            for(UINT32 index=0;index<count;++index){
                                const float height=std::max(1.0f,metrics[index].height);
                                lines.push_back({&box,source,
                                    textRun?box.textSourceOffset:0,local,
                                    static_cast<size_t>(metrics[index].length),top,height,
                                    top+metrics[index].baseline,false,0});
                                local+=metrics[index].length;top+=height;hasPosition=true;
                            }
                        }
                    }
                }
            }
        }
        if(!generated){
            for(auto& child:box.children)
                hasPosition=collect(*child,generated)||hasPosition;
            const auto display=ToLower(box.style.Get(L"display"));
            const bool lineContainer=display==L"block"||display==L"flow-root"||
                display==L"list-item"||box.node==scope;
            if(!hasPosition&&lineContainer&&box.node->type==NodeType::Element&&
               ParticipatesInEditableContent(box.node)){
                LayoutRect caret{};
                if(TextCaretRect(box.node,0,caret)){
                    lines.push_back({&box,box.node,0,0,0,caret.y,
                        std::max(1.0f,caret.height),
                        caret.y+TextBaselineOffset(box.style),true,0});
                    hasPosition=true;
                }
            }
        }
        return hasPosition;
    };
    collect(*scopeEntry->second,false);
    if(lines.empty())return false;

    float currentBaseline=currentCaret.y+currentCaret.height*0.8f;
    float currentMatch=std::numeric_limits<float>::max();
    for(const auto& line:lines){
        if(line.source!=currentNode)continue;
        bool contains=false;
        if(line.boundary)contains=currentOffset==line.boundaryOffset;
        else if(currentOffset>=line.sourceStart){
            const size_t local=currentOffset-line.sourceStart;
            contains=local>=line.localStart&&
                local<=line.localStart+line.localLength;
        }
        if(!contains)continue;
        const float distance=std::abs(line.top-currentCaret.y);
        if(distance<currentMatch){currentMatch=distance;currentBaseline=line.baseline;}
    }
    const float epsilon=0.5f/std::max(0.01f,deviceScale_);
    bool foundBaseline=false;float targetBaseline=0;
    for(const auto& line:lines){
        if(upward){
            if(line.baseline>=currentBaseline-epsilon)continue;
            if(!foundBaseline||line.baseline>targetBaseline){
                targetBaseline=line.baseline;foundBaseline=true;
            }
        }else{
            if(line.baseline<=currentBaseline+epsilon)continue;
            if(!foundBaseline||line.baseline<targetBaseline){
                targetBaseline=line.baseline;foundBaseline=true;
            }
        }
    }
    if(!foundBaseline)return false;
    const CaretLine* target=nullptr;float horizontalDistance=std::numeric_limits<float>::max();
    const float baselineTolerance=std::max(0.25f,epsilon);
    for(const auto& line:lines){
        if(std::abs(line.baseline-targetBaseline)>baselineTolerance)continue;
        float left=line.box->rect.x,right=line.box->rect.x+line.box->rect.width;
        if(line.boundary){
            LayoutRect caret{};if(TextCaretRect(line.source,line.boundaryOffset,caret))
                left=right=caret.x;
        }
        const float distance=preferredX<left?left-preferredX:
            (preferredX>right?preferredX-right:0.0f);
        if(distance<horizontalDistance){horizontalDistance=distance;target=&line;}
    }
    if(!target)return false;
    if(target->boundary){
        targetNode=target->source;targetOffset=target->boundaryOffset;return true;
    }
    const auto text=BoxText(*target->box);
    EnsureTextLayout(*target->box,SharedWriteFactory(),text);
    if(!target->box->textLayout)return false;
    const auto origin=TextOrigin(*target->box);BOOL trailing=FALSE,inside=FALSE;
    DWRITE_HIT_TEST_METRICS hit{};
    size_t local=target->localStart;
    if(SUCCEEDED(target->box->textLayout->HitTestPoint(
        preferredX-origin.x,target->top+target->height*0.5f-origin.y,
        &trailing,&inside,&hit)))
        local=std::min(text.size(),static_cast<size_t>(hit.textPosition)+
            (trailing?static_cast<size_t>(hit.length):0));
    const size_t sourceLength=target->source->type==NodeType::Text?
        target->source->text.size():target->source->Attribute(L"value").size();
    targetNode=target->source;
    targetOffset=std::min(sourceLength,target->sourceStart+local);return true;
}

bool LayoutEngine::TextCaretRect(const std::shared_ptr<Node>& textNode,size_t textOffset,
                                 LayoutRect& caretRect){
    caretRect={};if(!textNode||!root_)return false;
    // DOM Range offsets on elements describe a position between children.
    // Contenteditable keeps those boundaries directly (for example after an
    // inserted image), so resolve them from the surrounding CSS boxes without
    // relying on a disposable empty text node.
    if(textNode->type==NodeType::Element&&textNode->tag!=L"input"&&
       textNode->tag!=L"textarea"){
        const auto containerEntry=boxIndex_.find(textNode.get());
        if(containerEntry==boxIndex_.end()||!containerEntry->second||
           !containerEntry->second->visible)return false;
        const auto* containerBox=containerEntry->second;
        const float caretWidth=1.0f/std::max(0.01f,deviceScale_);
        const float lineHeight=std::max(1.0f,LineHeight(containerBox->style));
        const auto caretLine=CaretLineMetrics(containerBox->style);
        const auto boxCaret=[&](const LayoutBox& box,bool after,LayoutRect& result){
            const bool inlineLevel=IsInlineLevel(box.style.Get(L"display"));
            if(!inlineLevel){
                result={containerBox->content.x,
                    (after?box.rect.y+box.rect.height:box.rect.y)+caretLine.topInset,
                    caretWidth,caretLine.height};
                return true;
            }
            // Atomic inline boxes can be taller than the inherited line
            // height.  A caret at their DOM edge belongs to the line's text
            // track, not to the top or bottom of the replaced element. Recover
            // that track from the same baseline rule used by inline layout so
            // DOM-boundary and adjacent text carets remain identical.
            const auto margin=EdgeValues(box.style,L"margin",
                containerBox->content.width,viewportWidth_);
            const float outerTop=box.rect.y-margin.top;
            const float outerHeight=box.rect.height+margin.top+margin.bottom;
            const float inlineBaseline=outerTop+InlineOuterBaseline(
                box,outerHeight,containerBox->content.width,viewportWidth_);
            const auto alignment=AlignmentKeyword(
                box.style.Get(L"vertical-align",L"baseline"));
            float textLineTop=inlineBaseline-TextBaselineOffset(containerBox->style);
            if(alignment==L"top"||alignment==L"text-top")textLineTop=outerTop;
            else if(alignment==L"bottom"||alignment==L"text-bottom")
                textLineTop=outerTop+outerHeight-lineHeight;
            result={after?box.rect.x+box.rect.width:box.rect.x,
                    textLineTop+caretLine.topInset,caretWidth,caretLine.height};
            return true;
        };
        std::function<bool(const std::shared_ptr<Node>&,bool,LayoutRect&)> edgeCaret;
        edgeCaret=[&](const std::shared_ptr<Node>& node,bool after,LayoutRect& result){
            if(!node)return false;
            if(node->type==NodeType::Text)
                return TextCaretRect(node,after?node->text.size():0,result);
            const auto found=boxIndex_.find(node.get());
            if(found!=boxIndex_.end()&&found->second&&found->second->visible&&
               !IsInlineLevel(found->second->style.Get(L"display")))
                return boxCaret(*found->second,after,result);
            if(after){
                for(auto child=node->children.rbegin();child!=node->children.rend();++child)
                    if(edgeCaret(*child,true,result))return true;
            }else{
                for(const auto& child:node->children)
                    if(edgeCaret(child,false,result))return true;
            }
            return found!=boxIndex_.end()&&found->second&&found->second->visible?
                boxCaret(*found->second,after,result):false;
        };
        const size_t offset=std::min(textOffset,textNode->children.size());
        for(size_t index=offset;index>0;--index)
            if(edgeCaret(textNode->children[index-1],true,caretRect))return true;
        for(size_t index=offset;index<textNode->children.size();++index)
            if(edgeCaret(textNode->children[index],false,caretRect))return true;
        caretRect={containerBox->content.x,containerBox->content.y,caretWidth,lineHeight};
        return true;
    }
    LayoutBox* matched=nullptr;std::wstring matchedText;
    std::function<void(LayoutBox&,bool)> find=[&](LayoutBox& box,bool generated){
        if(matched||!box.visible)return;
        generated=generated||!box.pseudo.empty();
        if(!generated){
            auto source=box.node->type==NodeType::Text&&box.generatedFrom&&
                box.generatedFrom->type==NodeType::Text?box.generatedFrom:box.node;
            if(source==textNode){
                const auto text=BoxText(box);
                const size_t start=textNode->type==NodeType::Text?box.textSourceOffset:0;
                const size_t rawLength=box.node->type==NodeType::Text?box.node->text.size():text.size();
                if(textOffset>=start&&textOffset<=start+rawLength){
                    matched=&box;matchedText=text;return;
                }
            }
        }
        if(generated)return;
        for(auto& child:box.children)find(*child,generated);
    };
    find(*root_,false);
    if(matched){
        // An empty single-line input still owns a centered text line. Measure
        // that line with a zero-width probe so its initial caret uses exactly
        // the same font metrics and paragraph alignment as entered text.
        const auto caretText=matchedText.empty()&&matched->node->tag==L"input"?
            std::wstring(1,L'\x200b'):matchedText;
        EnsureTextLayout(*matched,SharedWriteFactory(),caretText);
        if(matched->textLayout){
            const size_t start=textNode->type==NodeType::Text?matched->textSourceOffset:0;
            const UINT32 local=matchedText.empty()?0:static_cast<UINT32>(
                TextSourceToLayoutOffset(*matched,textOffset-start));
            FLOAT hitX=0,hitY=0;DWRITE_HIT_TEST_METRICS metrics{};
            if(SUCCEEDED(matched->textLayout->HitTestTextPosition(local,FALSE,&hitX,&hitY,&metrics))){
                const auto origin=TextOrigin(*matched);
                caretRect={origin.x+hitX,origin.y+hitY,1.0f/std::max(0.01f,deviceScale_),
                    std::max(1.0f,metrics.height)};
                return true;
            }
        }
    }
    // Empty and wholly-collapsed text nodes intentionally have no CSS text
    // box. Their insertion point is the start of the nearest rendered
    // ancestor's content box, with that ancestor's computed line height.
    for(auto current=textNode;current;current=current->parent.lock()){
        const auto found=boxIndex_.find(current.get());if(found==boxIndex_.end())continue;
        const auto* box=found->second;if(!box||!box->visible)continue;
        const float lineHeight=std::max(1.0f,LineHeight(box->style));
        float top=box->content.y-(box->node->tag==L"textarea"?box->node->scrollTop:0.0f);
        // A single-line control centers its line box even before it contains
        // text. Keep the empty-control fallback on the same vertical track as
        // DirectWrite's centered paragraph used after the first character.
        if(box->node->tag==L"input")
            top+=std::max(0.0f,(box->content.height-lineHeight)/2.0f);
        caretRect={box->content.x,top,1.0f/std::max(0.01f,deviceScale_),lineHeight};
        return true;
    }
    return false;
}

std::shared_ptr<Node> LayoutEngine::HitTestStackingContext(const LayoutBox& box,float x,float y)const{
    auto matrix=PaintTransform(box);if(!matrix.Invert())return {};
    const auto point=matrix.TransformPoint({x,y});x=point.x;y=point.y;
    if(!HitRectContains(box.subtreeBounds,x,y,deviceScale_)||!BasicClipAllowsPoint(box,x,y,viewportWidth_))return {};
    for(auto it=box.nonNegativeStackingContexts.rbegin();it!=box.nonNegativeStackingContexts.rend();++it){
        const auto* context=*it;
        if((!ClipsOverflow(box)||OverflowClipRect(box).Contains(x,y))&&
           HitRectContains(context->subtreeBounds,x,y,deviceScale_)&&StackingContextAllowsPoint(*context,box,x,y))
            if(auto node=HitTestStackingContext(*context,x,y))return node;
    }
    return HitTestBox(box,x,y,true);
}
std::shared_ptr<Node> LayoutEngine::HitTestBox(const LayoutBox& box,float x,float y,bool transformApplied)const{
    if(!transformApplied){
        auto matrix=PaintTransform(box);if(!matrix.Invert())return {};
        const auto point=matrix.TransformPoint({x,y});x=point.x;y=point.y;
    }
    if(!box.visible||!HitRectContains(box.subtreeBounds,x,y,deviceScale_)||!BasicClipAllowsPoint(box,x,y,viewportWidth_))return {};
    const bool inside=HitRectContains(box.viewportScrollContainer?box.viewportScrollport:box.rect,x,y,deviceScale_);
    const bool clips=ClipsOverflow(box);
    if(!inside&&clips)return {};
    const bool childPointAllowed=!clips||OverflowClipRect(box).Contains(x,y);
    if(childPointAllowed&&!box.verticallyOrderedChildren.empty()){
        for(auto it=box.overlayChildren.rbegin();it!=box.overlayChildren.rend();++it)
            if(auto node=HitTestBox(**it,x,y))return node;
        auto first=std::lower_bound(box.verticallyOrderedChildren.begin(),box.verticallyOrderedChildren.end(),y,
            [](const LayoutBox* child,float value){return child->subtreeBounds.y+
                child->subtreeBounds.height<=value;});
        auto last=first;
        while(last!=box.verticallyOrderedChildren.end()&&(*last)->subtreeBounds.y<y+1)++last;
        while(last!=first){--last;if(auto node=HitTestBox(**last,x,y))return node;}
    }else if(childPointAllowed){
        for(auto it=box.paintChildren.rbegin();it!=box.paintChildren.rend();++it)
            if(auto node=HitTestBox(**it,x,y))return node;
    }
    if(!inside||box.style.Is(L"pointer-events",L"none")||box.style.Is(L"visibility",L"hidden"))return {};
    if(!RoundedBorderAllowsPoint(box,x,y,viewportWidth_,deviceScale_))return {};
    if(box.generatedFrom)return box.generatedFrom;
    return box.node&&box.node->type==NodeType::Element?box.node:box.node->parent.lock();
}
const LayoutBox* LayoutEngine::BoxFor(const std::shared_ptr<Node>& node)const{if(!root_||!node)return nullptr;const auto found=boxIndex_.find(node.get());return found==boxIndex_.end()?nullptr:found->second;}
bool LayoutEngine::VisualBounds(const std::shared_ptr<Node>& node,LayoutRect& bounds)const{
    if(!root_||!node)return false;
    const auto found=boxIndex_.find(node.get());if(found==boxIndex_.end())return false;
    bool initialized=false,safe=true;
    const auto include=[&](const LayoutRect& rect){
        if(rect.width<=0||rect.height<=0)return;
        if(!initialized){bounds=rect;initialized=true;return;}
        const float left=std::min(bounds.x,rect.x),top=std::min(bounds.y,rect.y);
        const float right=std::max(bounds.x+bounds.width,rect.x+rect.width);
        const float bottom=std::max(bounds.y+bounds.height,rect.y+rect.height);
        bounds={left,top,right-left,bottom-top};
    };
    std::function<void(const LayoutBox&)> visit=[&](const LayoutBox& box){
        if(!box.visible)return;
        const auto transform=ToLower(Trim(box.style.Get(L"transform")));
        if(!transform.empty()&&transform!=L"none")safe=false;
        LayoutRect painted{box.rect.x-2,box.rect.y-2,box.rect.width+4,box.rect.height+4};
        if(!ListMarkerText(box).empty()){
            const float markerExtent=std::max(30.0f,FontSize(box.style)*3.5f);
            const float left=std::min(painted.x,box.content.x-markerExtent);
            painted.width+=painted.x-left;painted.x=left;
        }
        const float stroke=std::max(0.0f,StyleSheet::Length(box.style.Get(L"stroke-width"),
            std::max(box.rect.width,box.rect.height),viewportWidth_,0));
        if(stroke>0){painted.x-=stroke/2;painted.y-=stroke/2;painted.width+=stroke;painted.height+=stroke;}
        include(painted);
        for(const auto& shadow:BoxShadows(box.style,viewportWidth_))if(!shadow.inset&&(shadow.color>>24)!=0){
            const float expansion=std::max(0.0f,shadow.spread+shadow.blur);
            include({box.rect.x+shadow.offsetX-expansion,box.rect.y+shadow.offsetY-expansion,
                     box.rect.width+expansion*2,box.rect.height+expansion*2});
        }
        for(const auto& child:box.children)visit(*child);
    };
    visit(*found->second);return safe&&initialized;
}
bool LayoutEngine::Restyle(const std::shared_ptr<Node>& node,bool* geometryChanged){
    if(geometryChanged)*geometryChanged=false;
    if(!root_||!node)return false;
    const auto found=boxIndex_.find(node.get());if(found==boxIndex_.end())return true;
    auto* box=found->second;const ComputedStyle* parentStyle=nullptr;
    if(auto parent=node->parent.lock()){
        const auto parentBox=boxIndex_.find(parent.get());
        if(parentBox!=boxIndex_.end())parentStyle=&parentBox->second->style;
    }
    const bool wasFixed=box->style.Is(L"position",L"fixed");
    bool fixedOffsetOnly=wasFixed;
    const bool layoutChanged=RestyleBox(*box,parentStyle,box,&fixedOffsetOnly);
    if(layoutChanged)for(auto* current=box;current;current=current->parent)current->inFlowBlockChildrenValid=false;
    if(box->parent){
        auto& siblings=box->parent->relativeChildren;
        const auto registered=std::find(siblings.begin(),siblings.end(),box);
        if(HasRelativeOffsets(*box)){
            if(registered==siblings.end())siblings.push_back(box);
        }else if(registered!=siblings.end())siblings.erase(registered);
    }
    if(!layoutChanged)return false;
    if(!fixedOffsetOnly||!box->style.Is(L"position",L"fixed"))return true;

    // A fixed box is outside every ancestor's normal flow. Changing only its
    // inset values cannot alter the layout of the document behind it, so move
    // the already-laid-out subtree instead of rebuilding the entire page.
    const auto positioned=PositionedRect(*box,AbsoluteContainingBlock(*box,viewportWidth_,viewportHeight_),
                                          viewportWidth_,viewportHeight_);
    if(std::abs(positioned.width-box->rect.width)>=0.001f||
       std::abs(positioned.height-box->rect.height)>=0.001f)return true;
    const float dx=positioned.x-box->rect.x,dy=positioned.y-box->rect.y;
    if(std::abs(dx)>=0.001f||std::abs(dy)>=0.001f){
        TranslateBox(*box,dx,dy);
        for(auto* ancestor=box->parent;ancestor;ancestor=ancestor->parent)
            UpdateSubtreeBounds(*ancestor);
        if(geometryChanged)*geometryChanged=true;
    }
    return false;
}
bool LayoutEngine::RestyleBox(LayoutBox& box,const ComputedStyle* parentStyle,
                              LayoutBox* localizedRoot,bool* fixedOffsetOnly){
    auto updated=box.generatedFrom?styleSheet_.Compute(box.generatedFrom,parentStyle,box.pseudo):
        styleSheet_.Compute(box.node,parentStyle);
    updated.deviceScale=deviceScale_;
    const bool ownLayoutChanged=HasLayoutStyleChange(box.style,updated);
    if(ownLayoutChanged&&fixedOffsetOnly&&
       (&box!=localizedRoot||!HasOnlyFixedOffsetLayoutStyleChange(box.style,updated)))
        *fixedOffsetOnly=false;
    box.style=updated;bool layoutChanged=ownLayoutChanged;
    box.inFlowBlockChildrenValid=false;
    if(ownLayoutChanged)box.overflowFlagsValid=false;
    RefreshOverflowClip(box,styleSheet_);
    box.relativeChildren.clear();
    for(auto& child:box.children){
        layoutChanged=RestyleBox(*child,&box.style,localizedRoot,fixedOffsetOnly)||layoutChanged;
        if(HasRelativeOffsets(*child))box.relativeChildren.push_back(child.get());
    }
    return layoutChanged;
}
bool LayoutEngine::SyncScroll(const std::shared_ptr<Node>& node){
    if(!root_||!node)return false;
    const auto found=boxIndex_.find(node.get());if(found==boxIndex_.end())return false;
    auto& box=*found->second;
    node->scrollLeft=std::max(0.0f,std::min(std::max(0.0f,box.scrollWidth-ScrollClientWidth(box)),node->scrollLeft));
    node->scrollTop=std::max(0.0f,std::min(std::max(0.0f,box.scrollHeight-ScrollClientHeight(box)),node->scrollTop));
    const bool changed=std::abs(node->scrollLeft-box.appliedScrollLeft)>=0.001f||
        std::abs(node->scrollTop-box.appliedScrollTop)>=0.001f;
    ApplyScrollOffset(box,box.appliedScrollLeft,box.appliedScrollTop,viewportHeight_);
    return changed;
}
bool LayoutEngine::ScrollAt(float x,float y,float wheelDelta,std::shared_ptr<Node>* scrolledNode,bool horizontal,bool* scrollChainStopped){
    if(scrolledNode)scrolledNode->reset();
    if(scrollChainStopped)*scrollChainStopped=false;
    if(!root_)return false;
    const auto target=HitTest(x,y);
    if(!target)return false;
    const auto found=boxIndex_.find(target.get());
    if(found==boxIndex_.end())return false;
    // Wheel scrolling follows the painted hit target's ancestor chain.  Searching
    // every box under the coordinates lets an exhausted popup fall through to a
    // covered sibling (for example, the page behind a menu), which browsers do
    // not include in the scroll chain.
    for(auto* box=found->second;box;box=box->parent){
        if(ScrollBox(*box,x,y,wheelDelta,scrolledNode,horizontal))return true;
        const auto overflow=horizontal?OverflowX(*box):OverflowY(*box);
        if(overflow!=L"auto"&&overflow!=L"scroll")continue;
        std::wistringstream behaviorTokens(box->style.Get(L"overscroll-behavior",L"auto"));
        std::wstring behaviorX=L"auto",behaviorY;
        behaviorTokens>>behaviorX;if(!(behaviorTokens>>behaviorY))behaviorY=behaviorX;
        const auto axis=box->style.Get(horizontal?L"overscroll-behavior-x":L"overscroll-behavior-y",
            horizontal?behaviorX:behaviorY);
        if(axis==L"contain"||axis==L"none"){
            if(scrollChainStopped)*scrollChainStopped=true;
            return false;
        }
    }
    return false;
}
bool LayoutEngine::ScrollBox(LayoutBox& box,float x,float y,float wheelDelta,std::shared_ptr<Node>* scrolledNode,bool horizontal){
    if(!box.visible||!PaddingBox(box).Contains(x,y))return false;
    const auto axisOverflow=horizontal?OverflowX(box):OverflowY(box);
    if(axisOverflow!=L"auto"&&axisOverflow!=L"scroll")return false;
    const float extent=horizontal?box.scrollWidth:box.scrollHeight;
    const float client=horizontal?ScrollClientWidth(box):ScrollClientHeight(box);
    if(extent<=client+1)return false;
    const float maximum=std::max(0.0f,extent-client),oldLeft=box.node->scrollLeft,oldTop=box.node->scrollTop;
    float& value=horizontal?box.node->scrollLeft:box.node->scrollTop;
    value=std::max(0.0f,std::min(maximum,value-wheelDelta/120.0f*90.0f));
    ApplyScrollOffset(box,oldLeft,oldTop,viewportHeight_);
    if((horizontal?box.node->scrollLeft!=oldLeft:box.node->scrollTop!=oldTop)){if(scrolledNode)*scrolledNode=box.node;return true;}
    return false;
}
bool LayoutEngine::BeginScrollbarInteraction(float x,float y,std::shared_ptr<Node>& dragNode,float& dragOffset,bool& horizontal){
    dragNode.reset();dragOffset=0;horizontal=false;if(!root_)return false;
    // Match the viewport scrollbar's final paint order before considering
    // scrollbars belonging to positioned descendants at the same point.
    if(root_->viewportScrollContainer){
        VerticalScrollbarGeometry vertical;HorizontalScrollbarGeometry horizontalGeometry;
        if((VerticalScrollbarFor(*root_,styleSheet_,vertical)&&vertical.track.Contains(x,y))||
           (HorizontalScrollbarFor(*root_,styleSheet_,horizontalGeometry)&&horizontalGeometry.track.Contains(x,y)))
            return BeginScrollbarBox(*root_,x,y,dragNode,dragOffset,horizontal);
    }
    for(auto it=root_->nonNegativeStackingContexts.rbegin();it!=root_->nonNegativeStackingContexts.rend();++it)
        if(StackingContextAllowsPoint(**it,*root_,x,y)&&BeginScrollbarBox(**it,x,y,dragNode,dragOffset,horizontal))return true;
    return BeginScrollbarBox(*root_,x,y,dragNode,dragOffset,horizontal);
}
bool LayoutEngine::BeginScrollbarInteraction(float x,float y,std::shared_ptr<Node>& dragNode,float& dragOffset){
    bool horizontal=false;return BeginScrollbarInteraction(x,y,dragNode,dragOffset,horizontal);
}
bool LayoutEngine::BeginScrollbarBox(LayoutBox& box,float x,float y,std::shared_ptr<Node>& dragNode,float& dragOffset,bool& horizontal){
    if(!box.visible||!(box.viewportScrollContainer?box.viewportScrollport:box.rect).Contains(x,y))return false;VerticalScrollbarGeometry geometry;
    if(VerticalScrollbarFor(box,styleSheet_,geometry)&&geometry.track.Contains(x,y)){
        if(y>=geometry.thumb.y&&y<geometry.thumb.y+geometry.thumb.height){dragNode=box.node;dragOffset=y-geometry.thumb.y;return true;}
        const float oldLeft=box.node->scrollLeft,oldTop=box.node->scrollTop;if(y<geometry.trackStart)box.node->scrollTop=std::max(0.0f,oldTop-90.0f);else if(y>=geometry.track.y+geometry.track.height-geometry.arrowHeight)box.node->scrollTop=std::min(geometry.maximum,oldTop+90.0f);else if(y<geometry.thumb.y)box.node->scrollTop=std::max(0.0f,oldTop-ScrollClientHeight(box)*0.9f);else box.node->scrollTop=std::min(geometry.maximum,oldTop+ScrollClientHeight(box)*0.9f);ApplyScrollOffset(box,oldLeft,oldTop,viewportHeight_);return true;
    }
    HorizontalScrollbarGeometry horizontalGeometry;
    if(HorizontalScrollbarFor(box,styleSheet_,horizontalGeometry)&&horizontalGeometry.track.Contains(x,y)){
        horizontal=true;
        if(x>=horizontalGeometry.thumb.x&&x<horizontalGeometry.thumb.x+horizontalGeometry.thumb.width){dragNode=box.node;dragOffset=x-horizontalGeometry.thumb.x;return true;}
        const float oldLeft=box.node->scrollLeft,oldTop=box.node->scrollTop;
        if(x<horizontalGeometry.trackStart)box.node->scrollLeft=std::max(0.0f,oldLeft-90.0f);
        else if(x>=horizontalGeometry.track.x+horizontalGeometry.track.width-horizontalGeometry.arrowWidth)box.node->scrollLeft=std::min(horizontalGeometry.maximum,oldLeft+90.0f);
        else if(x<horizontalGeometry.thumb.x)box.node->scrollLeft=std::max(0.0f,oldLeft-ScrollClientWidth(box)*0.9f);
        else box.node->scrollLeft=std::min(horizontalGeometry.maximum,oldLeft+ScrollClientWidth(box)*0.9f);
        ApplyScrollOffset(box,oldLeft,oldTop,viewportHeight_);return true;
    }
    for(auto it=box.children.rbegin();it!=box.children.rend();++it)if(BeginScrollbarBox(**it,x,y,dragNode,dragOffset,horizontal))return true;return false;
}
bool LayoutEngine::DragScrollbar(const std::shared_ptr<Node>& node,float x,float y,float dragOffset,bool horizontal){
    if(!root_||!node)return false;const auto found=boxIndex_.find(node.get());auto* box=found==boxIndex_.end()?nullptr:found->second;if(!box)return false;
    const float oldLeft=node->scrollLeft,oldTop=node->scrollTop;
    if(horizontal){HorizontalScrollbarGeometry geometry;if(!HorizontalScrollbarFor(*box,styleSheet_,geometry)||geometry.travel<=0||geometry.maximum<=0)return false;const float thumbX=std::max(geometry.trackStart,std::min(geometry.trackStart+geometry.travel,x-dragOffset));node->scrollLeft=std::max(0.0f,std::min(geometry.maximum,(thumbX-geometry.trackStart)/geometry.travel*geometry.maximum));}
    else{VerticalScrollbarGeometry geometry;if(!VerticalScrollbarFor(*box,styleSheet_,geometry)||geometry.travel<=0||geometry.maximum<=0)return false;const float thumbY=std::max(geometry.trackStart,std::min(geometry.trackStart+geometry.travel,y-dragOffset));node->scrollTop=std::max(0.0f,std::min(geometry.maximum,(thumbY-geometry.trackStart)/geometry.travel*geometry.maximum));}
    ApplyScrollOffset(*box,oldLeft,oldTop,viewportHeight_);return horizontal?std::abs(node->scrollLeft-oldLeft)>0.01f:std::abs(node->scrollTop-oldTop)>0.01f;
}
bool LayoutEngine::DragScrollbar(const std::shared_ptr<Node>& node,float y,float dragOffset){
    return DragScrollbar(node,0,y,dragOffset,false);
}

void LayoutEngine::DumpBox(const LayoutBox& box,std::wstring& output,bool& first,bool includeText)const{
    if(!box.visible)return;
    if(box.node->type==NodeType::Element||(includeText&&box.node->type==NodeType::Text)){
        if(!first)output+=L",";first=false;std::wostringstream s;
        s<<L"{\"tag\":\""<<EscapeJson(box.node->tag)<<L"\",\"id\":\""<<EscapeJson(box.node->Attribute(L"id"))
         <<L"\",\"x\":"<<std::lround(box.rect.x)<<L",\"y\":"<<std::lround(box.rect.y)
         <<L",\"width\":"<<std::lround(box.rect.width)<<L",\"height\":"<<std::lround(box.rect.height);
        if(includeText&&box.node->type==NodeType::Text)s<<L",\"text\":\""<<EscapeJson(BoxText(box))
            <<L"\",\"color\":\""<<EscapeJson(box.style.Get(L"color"))<<L"\",\"contentWidth\":"<<box.content.width
            <<L",\"contentHeight\":"<<box.content.height;
        // SVG descendants have no HTML layout boxes. Include their geometry
        // and stroke inputs so a renderer dump can diagnose their paint rules.
        if(box.node->tag==L"svg"){
            s<<L",\"svgNodes\":[";bool firstSvg=true;
            const std::function<void(const std::shared_ptr<Node>&,const ComputedStyle&)> collect=
                [&](const std::shared_ptr<Node>& node,const ComputedStyle& inherited){
                if(node->type!=NodeType::Element)return;
                const auto style=styleSheet_.Compute(node,&inherited);
                if(!firstSvg)s<<L',';firstSvg=false;s<<L"{\"tag\":\""<<EscapeJson(node->tag)<<L"\",\"attributes\":{";
                bool firstAttribute=true;for(const auto& attribute:node->attributes){
                    if(!firstAttribute)s<<L',';firstAttribute=false;
                    s<<L'\"'<<EscapeJson(attribute.first)<<L"\":\""<<EscapeJson(attribute.second)<<L'\"';
                }
                s<<L"},\"style\":{";bool firstStyle=true;
                for(const auto* property:{L"display",L"visibility",L"fill",L"fill-rule",L"stroke",L"stroke-width",
                    L"stroke-linecap",L"stroke-dasharray",L"stroke-dashoffset",L"transform"}){
                    if(!firstStyle)s<<L',';firstStyle=false;
                    s<<L'\"'<<property<<L"\":\""<<EscapeJson(style.Get(property))<<L'\"';
                }
                s<<L"}}";for(const auto& child:node->children)collect(child,style);
            };
            for(const auto& child:box.node->children)collect(child,box.style);
            s<<L']';
        }
        s<<L'}';output+=s.str();
    }
    for(const auto& child:box.children)DumpBox(*child,output,first,includeText);
}
std::wstring LayoutEngine::DumpJson(bool includeText)const{std::wstring out=L"[";bool first=true;if(root_)DumpBox(*root_,out,first,includeText);return out+L"]";}

ComputedStyle LayoutEngine::StyleForRendering(const std::shared_ptr<Node>& node) const {
    if(const auto* box=BoxFor(node))return box->style;
    // A display:none ancestor stops box construction. Connected descendants
    // still inherit computed styles from their actual ancestor chain.
    std::vector<std::shared_ptr<Node>> ancestors;
    for(auto current=node;current;current=current->parent.lock())ancestors.push_back(current);
    ComputedStyle inherited;bool hasParent=false;
    for(auto it=ancestors.rbegin();it!=ancestors.rend();++it){
        if(const auto* box=BoxFor(*it))inherited=box->style;
        else inherited=styleSheet_.Compute(*it,hasParent?&inherited:nullptr);
        hasParent=true;
    }
    inherited.deviceScale=deviceScale_;return inherited;
}

std::wstring LayoutEngine::DumpRenderingStyleJson(const ComputedStyle& style) const {
    // Use layout/paint's fallbacks and resolvers for this bounded projection.
    // The original sparse style map is retained separately in the capture.
    std::wostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9);
    const auto quote=[](const std::wstring& value){
        std::wstring result=L"\"";const wchar_t hex[]=L"0123456789abcdef";
        for(const auto c:value){if(c==L'"'||c==L'\\'){result+=L'\\';result+=c;}
            else if(c<32){result+=L"\\u00";result+=hex[(c>>4)&15];result+=hex[c&15];}
            else result+=c;}
        return result+L'"';
    };
    bool first=true;out<<L'{';
    const auto add=[&](const wchar_t* key,const std::wstring& value){
        if(!first)out<<L',';first=false;out<<quote(key)<<L':'<<quote(value);
    };
    const auto number=[](float value,const wchar_t* suffix=L""){
        std::wostringstream text;text.imbue(std::locale::classic());
        text<<std::setprecision(9)<<value<<suffix;return text.str();
    };
    for(const auto* key:{L"display",L"position",L"visibility",L"box-sizing",L"direction",
        L"text-align",L"font-style",L"font-weight",L"font-kerning",L"pointer-events",
        L"flex-direction",L"flex-wrap",L"flex-grow",L"flex-shrink",L"list-style-position",L"list-style-type"})
        add(key,style.Get(key));
    const auto whiteSpace=style.Get(L"white-space");
    add(L"white-space",whiteSpace.empty()?L"normal":whiteSpace);
    add(L"overflow-x",style.Get(L"overflow-x",style.Get(L"overflow",L"visible")));
    add(L"overflow-y",style.Get(L"overflow-y",style.Get(L"overflow",L"visible")));
    add(L"opacity",number(std::clamp(StyleSheet::Length(style.Get(L"opacity",L"1"),1,1,1),0.0f,1.0f)));
    auto spacing=style.Get(L"letter-spacing");if(spacing.empty())spacing=L"normal";
    add(L"letter-spacing",spacing==L"normal"?spacing:number(StyleSheet::Length(spacing,FontSize(style),viewportWidth_,0,FontSize(style)),L"px"));
    add(L"word-spacing",number(StyleSheet::Length(style.Get(L"word-spacing"),FontSize(style),viewportWidth_,0,FontSize(style)),L"px"));
    auto lineHeight=style.Get(L"line-height");if(lineHeight.empty())lineHeight=L"normal";
    add(L"line-height",lineHeight==L"normal"?lineHeight:number(LineHeight(style),L"px"));
    add(L"font-size",number(FontSize(style),L"px"));
    const auto color=[&](const wchar_t* key,unsigned int argb){
        std::wostringstream hex;hex<<L'#'<<std::hex<<std::setfill(L'0')<<std::setw(6)
            <<(argb&0xffffffu)<<std::setw(2)<<(argb>>24);add(key,hex.str());
    };
    color(L"color",StyleSheet::Color(style.Get(L"color",L"#000000")));
    color(L"background-color",BackgroundColor(style));
    out<<L'}';return out.str();
}

LayoutEngine::ElementSizes LayoutEngine::ReadElementSizes(const std::shared_ptr<Node>& node)const{
    ElementSizes sizes;
    if(!node)return sizes;
    if(node->tag==L"html"){
        sizes.clientWidth=std::round(root_?ScrollClientWidth(*root_):viewportWidth_);
        sizes.clientHeight=std::round(root_?ScrollClientHeight(*root_):viewportHeight_);
        sizes.scrollWidth=std::round(std::max(sizes.clientWidth,root_?root_->scrollWidth:0.0f));
        sizes.scrollHeight=std::round(std::max(sizes.clientHeight,root_?root_->scrollHeight:0.0f));
        return sizes;
    }
    const auto* box=BoxFor(node);
    if(!box||!box->visible)return sizes;
    for(auto* parent=box->parent;parent;parent=parent->parent)if(!parent->visible)return sizes;
    if(box->style.Is(L"display",L"inline")&&node->tag!=L"input"&&node->tag!=L"img"&&node->tag!=L"svg")return sizes;
    const auto border=IsTable(*box)?Edges{}:UsedBorderValues(*box);
    float width=std::max(0.0f,box->rect.width-border.left-border.right);
    float height=std::max(0.0f,box->rect.height-border.top-border.bottom);
    if(!box->viewportScrollContainer){
        const auto x=OverflowX(*box),y=OverflowY(*box);
        const bool textarea=node->tag==L"textarea";
        const bool vertical=textarea?box->textareaVerticalScrollbar:y==L"scroll"||(y==L"auto"&&box->cssScrollHeight>height);
        const bool horizontal=textarea?box->textareaHorizontalScrollbar:x==L"scroll"||(x==L"auto"&&box->cssScrollWidth>width);
        if(box->verticalGutterReserved)width=std::max(0.0f,width-box->scrollbarGutterLeft-box->scrollbarGutterRight);
        else if(vertical||(HasStableScrollbarGutter(box->style)&&(y==L"auto"||y==L"hidden")))width=std::max(0.0f,width-VerticalScrollbarMetricsFor(*box,styleSheet_).width);
        if(horizontal)height=std::max(0.0f,height-HorizontalScrollbarMetricsFor(*box,styleSheet_).height);
    }
    sizes.clientWidth=std::round(width);sizes.clientHeight=std::round(height);
    // Empty forced scrollbars reduce both the client and minimum scroll area.
    const bool table=IsTable(*box)||node->tag==L"textarea";
    sizes.scrollWidth=std::round(table?box->cssScrollWidth:std::max(width,box->cssScrollWidth));
    sizes.scrollHeight=std::round(table?box->cssScrollHeight:std::max(height,box->cssScrollHeight));
    return sizes;
}

bool LayoutEngine::ReadElementRect(const std::shared_ptr<Node>& node,LayoutRect& rect)const{
    if(!node)return false;
    if(node->tag==L"html"&&root_){
        const auto style=StyleForRendering(node);if(style.Is(L"display",L"none"))return false;
        const auto margin=UsedLayoutMargins(*root_,viewportWidth_,viewportWidth_);
        rect={0,0,viewportWidth_,root_->rect.height+margin.top+margin.bottom};return true;
    }
    const auto* box=BoxFor(node);if(!box||!box->visible)return false;
    for(auto* parent=box->parent;parent;parent=parent->parent)if(!parent->visible)return false;
    rect=box->rect;
    if(box->splitInlineRectValid){
        // The flow rectangle reserves complete first/last line boxes. An
        // inline split around a block exposes the union of its font fragments
        // and the block, excluding the outer line leading from DOM geometry.
        rect=box->splitInlineRectOffsets;rect.x+=box->rect.x;rect.y+=box->rect.y;
    }else if(box->style.Is(L"display",L"inline")&&!IsAtomicInlineLevel(*box)&&
             !IsBlockifiedItem(*box)&&box->children.size()==1&&
             box->rect.height>LineHeight(box->style)+0.01f&&
             box->children.front()->node->type==NodeType::Text){
        // A wrapped inline exposes the union of font boxes; the full first
        // and last line boxes are reserved by its containing block. Derive
        // those edges from the same shaped baselines used when painting.
        auto& text=*box->children.front();
        EnsureTextLayout(text,SharedWriteFactory(),BoxText(text));
        UINT32 count=0;
        if(text.textLayout)text.textLayout->GetLineMetrics(nullptr,0,&count);
        if(count>1){
            std::vector<DWRITE_LINE_METRICS> lines(count);
            if(SUCCEEDED(text.textLayout->GetLineMetrics(lines.data(),count,&count))){
                const auto origin=TextOrigin(text);const auto font=InlineFontBoxMetrics(text.style);
                const auto padding=EdgeValues(box->style,L"padding",box->parent?box->parent->content.width:viewportWidth_,viewportWidth_);
                const auto border=UsedBorderValues(*box);
                float lastTop=0;for(UINT32 i=0;i+1<count;++i)lastTop+=lines[i].height;
                const float top=origin.y+lines.front().baseline-font.baseline;
                const float bottom=origin.y+lastTop+lines.back().baseline-font.baseline+font.height;
                rect.y=top-padding.top-border.top;
                rect.height=bottom-top+padding.top+padding.bottom+border.top+border.bottom;
            }
        }
    }
    // DOM bounds include each ancestor's paint transform, while layout and
    // clip reference boxes remain in their original coordinate space.
    std::array<D2D1_POINT_2F,4> corners={D2D1::Point2F(rect.x,rect.y),
        D2D1::Point2F(rect.x+rect.width,rect.y),D2D1::Point2F(rect.x+rect.width,rect.y+rect.height),
        D2D1::Point2F(rect.x,rect.y+rect.height)};
    for(auto* ancestor=box;ancestor;ancestor=ancestor->parent){
        const auto matrix=PaintTransform(*ancestor);
        if(!matrix.IsIdentity())for(auto& corner:corners)corner=matrix.TransformPoint(corner);
    }
    float left=corners[0].x,right=left,top=corners[0].y,bottom=top;
    for(const auto& corner:corners){left=std::min(left,corner.x);right=std::max(right,corner.x);top=std::min(top,corner.y);bottom=std::max(bottom,corner.y);}
    rect={left,top,right-left,bottom-top};return true;
}

std::wstring LayoutEngine::DumpRenderingTextJson()const{
    std::map<const Node*,std::wstring> paths;
    const std::function<void(const std::shared_ptr<Node>&,const std::wstring&)> index=
        [&](const std::shared_ptr<Node>& node,const std::wstring& path){
            if(!node)return;paths[node.get()]=path;
            for(size_t i=0;i<node->children.size();++i)index(node->children[i],path+L"/"+std::to_wstring(i));
        };
    auto html=document_.QuerySelector(L"html");if(!html)html=document_.Body();index(html,L"0");
    std::wostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(9)<<L'[';
    bool first=true;
    const auto quote=[](const std::wstring& value){return L"\""+EscapeJson(value)+L"\"";};
    const std::function<void(const LayoutBox*)> walk=[&](const LayoutBox* box){
        if(!box||!box->visible)return;
        if(box->node&&box->node->type==NodeType::Text){
            const auto source=box->generatedFrom&&box->generatedFrom->type==NodeType::Text?box->generatedFrom:box->node;
            const auto path=paths.find(source.get());
            const auto text=BoxText(*box);
            auto& measured=*const_cast<LayoutBox*>(box);
            if(path!=paths.end()&&!text.empty()){
                EnsureTextLayout(measured,SharedWriteFactory(),text);
                if(measured.textLayout){
                    Microsoft::WRL::ComPtr<RenderingGlyphCollector> shaped;
                    shaped.Attach(new RenderingGlyphCollector(SharedWriteFactory(),text.size(),!box->style.Is(L"font-kerning",L"none")));
                    const bool glyphsCollected=SUCCEEDED(measured.textLayout->Draw(nullptr,shaped.Get(),0,0));
                    UINT32 lineCount=0;measured.textLayout->GetLineMetrics(nullptr,0,&lineCount);
                    std::vector<DWRITE_LINE_METRICS> lines(lineCount);
                    measured.textLayout->GetLineMetrics(lines.data(),lineCount,&lineCount);
                    const auto origin=TextOrigin(*box);
                    const auto font=InlineFontBoxMetrics(box->style);
                    size_t raw=box->textSourceOffset,lineStart=0;float lineTop=0;
                    for(UINT32 line=0;line<lineCount;++line){
                        const auto& metrics=lines[line];
                        for(size_t pos=lineStart;pos<lineStart+metrics.length&&pos<text.size();++pos){
                            if(IsCollapsibleTextSpace(text[pos])||text[pos]==L'\x200b')continue;
                            while(raw<source->text.size()&&IsCollapsibleTextSpace(source->text[raw]))++raw;
                            const size_t units=text[pos]>=0xd800&&text[pos]<=0xdbff&&pos+1<text.size()&&
                                text[pos+1]>=0xdc00&&text[pos+1]<=0xdfff?2:1;
                            DWRITE_HIT_TEST_METRICS hit{};UINT32 count=0;
                            if(SUCCEEDED(measured.textLayout->HitTestTextRange(static_cast<UINT32>(pos),
                                static_cast<UINT32>(units),origin.x,origin.y,&hit,1,&count))&&count==1){
                                if(!first)out<<L',';first=false;
                                const bool maps=raw+units<=source->text.size()&&source->text.compare(raw,units,text,pos,units)==0;
                                const auto& glyph=shaped->glyphs[pos];
                                const float nextOffset=pos+units<lineStart+metrics.length&&pos+units<text.size()?
                                    shaped->glyphs[pos+units].offset:0;
                                // HarfBuzz splits a legacy kern adjustment across
                                // the two logical advances; DirectWrite places it
                                // on the first. Normalize range boundaries while
                                // retaining raw advances and actual glyph ink data.
                                const double startFraction=static_cast<double>(pos-glyph.clusterStart)/glyph.clusterLength;
                                const double endFraction=static_cast<double>(pos+units-glyph.clusterStart)/glyph.clusterLength;
                                const auto legacyBoundary=[&](double fraction){
                                    return (-glyph.legacyIncoming+(glyph.legacyIncoming-glyph.legacyOutgoing)*fraction)/2.0;
                                };
                                const double left=hit.left+(glyph.rtl?-nextOffset:glyph.offset)+legacyBoundary(startFraction);
                                const double right=hit.left+hit.width+(glyph.rtl?-glyph.offset:nextOffset)+legacyBoundary(endFraction);
                                const double layoutUnits=64.0*std::max(0.01f,box->style.deviceScale);
                                const auto bound=[&](double value,bool upper){
                                    double scaled=value*layoutUnits;const auto nearest=std::round(scaled);
                                    if(std::abs(scaled-nearest)<0.001)scaled=nearest;
                                    return (upper?std::ceil(scaled):std::floor(scaled))/layoutUnits;
                                };
                                const auto rangeLeft=bound(left,false),rangeRight=bound(right,true);
                                out<<L"{\"path\":"<<quote(path->second)<<L",\"start\":"<<raw
                                   <<L",\"length\":"<<units<<L",\"text\":"<<quote(text.substr(pos,units))
                                   <<L",\"mapped\":"<<(maps?L"true":L"false")<<L",\"rect\":["
                                   <<rangeLeft<<L','<<hit.top+metrics.baseline-font.baseline
                                   <<L','<<rangeRight-rangeLeft<<L','<<font.height<<L"],\"rawHitRect\":["
                                   <<hit.left<<L','<<hit.top<<L','<<hit.width<<L','<<hit.height<<L"],\"glyph\":{\"collected\":"
                                   <<(glyphsCollected&&glyph.present?L"true":L"false")<<L",\"fontResolved\":"
                                   <<(glyph.fontResolved?L"true":L"false")<<L",\"family\":"<<quote(glyph.family)
                                   <<L",\"face\":"<<quote(glyph.face)<<L",\"postScript\":"<<quote(glyph.postScript)
                                   <<L",\"index\":"<<glyph.index<<L",\"advance\":"<<glyph.advance
                                   <<L",\"placementOffset\":"<<glyph.offset<<L",\"emSize\":"<<glyph.emSize
                                   <<L",\"legacyIncoming\":"<<glyph.legacyIncoming<<L",\"legacyOutgoing\":"<<glyph.legacyOutgoing
                                   <<L",\"clusterStart\":"<<glyph.clusterStart<<L",\"clusterLength\":"<<glyph.clusterLength
                                   <<L",\"rtl\":"<<(glyph.rtl?L"true":L"false")<<L"},\"baseline\":"
                                   <<hit.top+metrics.baseline<<L",\"lineBox\":["
                                   <<origin.x<<L','<<origin.y+lineTop<<L','<<box->content.width<<L','<<metrics.height
                                   <<L"],\"declaredFont\":"<<quote(box->style.Get(L"font-family"))<<L'}';
                            }
                            raw+=units;pos+=units-1;
                        }
                        lineStart+=metrics.length;lineTop+=metrics.height;
                    }
                }
            }
        }
        if (box->node && box->node->tag == L"svg") {
            for (const auto& element : box->node->children) {
                if (element->tag != L"text") continue;
                for (const auto& source : element->children) {
                    LayoutBox run;
                    if (SimpleSvgTextRun(*box,source,styleSheet_,run)) walk(&run);
                }
            }
        }
        for(const auto& child:box->children)walk(child.get());
    };
    walk(root_.get());out<<L']';return out.str();
}

} // namespace TWebFrame::Internal

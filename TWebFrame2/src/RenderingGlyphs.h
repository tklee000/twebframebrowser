#pragma once
#include <dwrite.h>
#include <dwrite_1.h>
#include <wrl/client.h>
#include <map>
#include <string>
#include <vector>
#include "FontFeatures.h"

namespace TWebFrame::Internal {
// Read the same shaped runs that DrawTextLayout paints. Hit-test advances alone
// omit GPOS placement offsets, which matter for browser character rectangles.
class RenderingGlyphCollector final : public IDWriteTextRenderer {
public:
    struct Glyph {
        bool present=false,rtl=false,fontResolved=false;
        float offset=0,advance=0,emSize=0;
        float legacyIncoming=0,legacyOutgoing=0;
        UINT32 clusterStart=0,clusterLength=1;
        UINT16 index=0;
        std::wstring family,face,postScript;
    };
    explicit RenderingGlyphCollector(IDWriteFactory* factory,size_t characters,bool kerning=true):glyphs(characters),kerning_(kerning){
        if(factory)factory->GetSystemFontCollection(&collection_);
    }
    std::vector<Glyph> glyphs;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDWritePixelSnapping)||iid==__uuidof(IDWriteTextRenderer)){
            *result=static_cast<IDWriteTextRenderer*>(this);AddRef();return S_OK;
        }return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&references_);}
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining=InterlockedDecrement(&references_);if(!remaining)delete this;return remaining;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled) override {
        if(!disabled)return E_POINTER;*disabled=TRUE;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* matrix) override {
        if(!matrix)return E_POINTER;*matrix={1,0,0,1,0,0};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* scale) override {
        if(!scale)return E_POINTER;*scale=1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT,FLOAT,DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*) override {
        if(!run||!description||!description->clusterMap)return E_INVALIDARG;
        auto found=fonts_.find(run->fontFace);
        if(found==fonts_.end()){
            Font info;Microsoft::WRL::ComPtr<IDWriteFont> font;
            if(collection_&&SUCCEEDED(collection_->GetFontFromFontFace(run->fontFace,&font))){
                Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
                Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
                if(SUCCEEDED(font->GetFontFamily(&family))&&SUCCEEDED(family->GetFamilyNames(&names)))info.family=Name(names.Get());
                names.Reset();if(SUCCEEDED(font->GetFaceNames(&names)))info.face=Name(names.Get());
                BOOL exists=FALSE;names.Reset();
                if(SUCCEEDED(font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME,&names,&exists))&&exists)
                    info.postScript=Name(names.Get());
                info.resolved=!info.family.empty();
            }
            info.legacyOnly=!FontHasGposKerning(run->fontFace);
            found=fonts_.emplace(run->fontFace,std::move(info)).first;
        }
        std::vector<INT32> adjustments(run->glyphCount,0);
        Microsoft::WRL::ComPtr<IDWriteFontFace1> extendedFace;
        if(kerning_&&found->second.legacyOnly&&!(run->bidiLevel&1)&&
           SUCCEEDED(run->fontFace->QueryInterface(IID_PPV_ARGS(&extendedFace)))&&extendedFace->HasKerningPairs())
            extendedFace->GetKerningPairAdjustments(run->glyphCount,run->glyphIndices,adjustments.data());
        DWRITE_FONT_METRICS metrics{};run->fontFace->GetMetrics(&metrics);
        const float designScale=metrics.designUnitsPerEm?run->fontEmSize/metrics.designUnitsPerEm:0;
        for(UINT32 i=0;i<description->stringLength&&description->textPosition+i<glyphs.size();++i){
            const auto index=description->clusterMap[i];if(index>=run->glyphCount)continue;
            auto& glyph=glyphs[description->textPosition+i];glyph.present=true;
            glyph.rtl=(run->bidiLevel&1)!=0;glyph.index=run->glyphIndices[index];
            glyph.offset=run->glyphOffsets?run->glyphOffsets[index].advanceOffset:0;
            glyph.advance=run->glyphAdvances?run->glyphAdvances[index]:0;glyph.emSize=run->fontEmSize;
            glyph.legacyIncoming=index?adjustments[index-1]*designScale:0;
            glyph.legacyOutgoing=adjustments[index]*designScale;
            UINT32 begin=i,end=i+1;
            while(begin&&description->clusterMap[begin-1]==index)--begin;
            while(end<description->stringLength&&description->clusterMap[end]==index)++end;
            glyph.clusterStart=description->textPosition+begin;glyph.clusterLength=end-begin;
            glyph.family=found->second.family;glyph.face=found->second.face;
            glyph.postScript=found->second.postScript;glyph.fontResolved=found->second.resolved;
        }return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {return S_OK;}
private:
    struct Font {bool resolved=false,legacyOnly=false;std::wstring family,face,postScript;};
    static std::wstring Name(IDWriteLocalizedStrings* names){
        if(!names||!names->GetCount())return {};
        UINT32 index=0,length=0;BOOL exists=FALSE;names->FindLocaleName(L"en-us",&index,&exists);if(!exists)index=0;
        if(FAILED(names->GetStringLength(index,&length)))return {};
        std::wstring name(length+1,L'\0');if(FAILED(names->GetString(index,name.data(),length+1)))return {};
        name.resize(length);return name;
    }
    LONG references_=1;
    bool kerning_=true;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> collection_;
    std::map<IDWriteFontFace*,Font> fonts_;
};
}

#pragma once
#include <dwrite_1.h>
#include <wrl/client.h>
#include <map>
#include <vector>
#include <cmath>
#include <algorithm>

namespace TWebFrame::Internal {
// The legacy kern table must not be applied in addition to a GPOS kern
// feature. Keep the actual face alive while its feature decision is cached.
inline bool FontHasGposKerning(IDWriteFontFace* face){
    if(!face)return true;
    struct Entry {Microsoft::WRL::ComPtr<IDWriteFontFace> face;bool gpos=false;};
    static thread_local std::map<IDWriteFontFace*,Entry> cache;
    if(const auto found=cache.find(face);found!=cache.end())return found->second.gpos;
    const void* bytes=nullptr;void* context=nullptr;UINT32 size=0;BOOL exists=FALSE;
    if(FAILED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('G','P','O','S'),&bytes,&size,&context,&exists)))return true;
    bool kern=false;
    if(exists&&bytes&&size>=10){
        const auto* data=static_cast<const BYTE*>(bytes);
        const auto word=[&](size_t offset){return static_cast<size_t>((data[offset]<<8)|data[offset+1]);};
        const auto features=word(6);
        if(features<=size-2){
            const auto count=word(features);
            for(size_t i=0;i<count&&features+2+(i+1)*6<=size;++i){
                const auto* tag=data+features+2+i*6;
                if(tag[0]=='k'&&tag[1]=='e'&&tag[2]=='r'&&tag[3]=='n'){kern=true;break;}
            }
        }
    }
    if(context)face->ReleaseFontTable(context);
    if(cache.size()>=128)cache.clear();cache.emplace(face,Entry{face,kern});return kern;
}

// Observe shaped fallback runs before enabling legacy kerning. Using the
// declared family alone would select the wrong setting for missing glyphs.
class TextRunObserver : public IDWriteTextRenderer {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDWritePixelSnapping)||iid==__uuidof(IDWriteTextRenderer)){
            *result=static_cast<IDWriteTextRenderer*>(this);AddRef();return S_OK;
        }return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return InterlockedIncrement(&references_);}
    ULONG STDMETHODCALLTYPE Release() override {return InterlockedDecrement(&references_);}
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
        const DWRITE_GLYPH_RUN*,const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {return S_OK;}
private:
    LONG references_=1;
};

class LegacyKerningRanges final : public TextRunObserver {
public:
    std::vector<DWRITE_TEXT_RANGE> ranges;
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT,FLOAT,DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*) override {
        if(!run||!description)return E_INVALIDARG;
        Microsoft::WRL::ComPtr<IDWriteFontFace1> extended;
        if(!FontHasGposKerning(run->fontFace)&&SUCCEEDED(run->fontFace->QueryInterface(IID_PPV_ARGS(&extended)))&&extended->HasKerningPairs())
            ranges.push_back({description->textPosition,description->stringLength});
        return S_OK;
    }
};

inline std::vector<UINT16> FontBitmapSizes(IDWriteFontFace* face){
    struct Entry {Microsoft::WRL::ComPtr<IDWriteFontFace> face;std::vector<UINT16> sizes;};
    static thread_local std::map<IDWriteFontFace*,Entry> cache;
    if(const auto found=cache.find(face);found!=cache.end())return found->second.sizes;
    Entry entry;entry.face=face;
    const void* bytes=nullptr;void* context=nullptr;UINT32 size=0;BOOL exists=FALSE;
    if(SUCCEEDED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('E','B','L','C'),&bytes,&size,&context,&exists))&&exists&&bytes&&size>=8){
        const auto* data=static_cast<const BYTE*>(bytes);
        const UINT32 count=(static_cast<UINT32>(data[4])<<24)|(static_cast<UINT32>(data[5])<<16)|(data[6]<<8)|data[7];
        for(UINT32 i=0;i<count&&static_cast<size_t>(8)+(static_cast<size_t>(i)+1)*48<=size;++i)
            if(data[8+i*48+44]==data[8+i*48+45])entry.sizes.push_back(data[8+i*48+45]);
    }
    if(context)face->ReleaseFontTable(context);
    if(cache.size()>=128)cache.clear();const auto result=entry.sizes;cache.emplace(face,std::move(entry));return result;
}

// Select coverage from the actual shaped face and physical em size. A blanket
// symmetric mode ignores the font's gasp table and changes small hinted ink.
inline DWRITE_RENDERING_MODE DetermineFontRasterMode(IDWriteFontFace* face,float physicalSize){
    if(!face||!std::isfinite(physicalSize)||physicalSize<=0)return DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    const auto ppem=static_cast<unsigned int>(std::min(65535.0f,std::round(physicalSize)));
    const auto bitmaps=FontBitmapSizes(face);
    if(std::find(bitmaps.begin(),bitmaps.end(),ppem)!=bitmaps.end())return DWRITE_RENDERING_MODE_GDI_CLASSIC;
    const void* bytes=nullptr;void* context=nullptr;UINT32 size=0;BOOL exists=FALSE;
    DWRITE_RENDERING_MODE mode=DWRITE_RENDERING_MODE_DEFAULT;
    if(SUCCEEDED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('g','a','s','p'),&bytes,&size,&context,&exists))&&exists&&bytes&&size>=4){
        const auto* data=static_cast<const BYTE*>(bytes);
        const auto word=[&](size_t offset){return static_cast<UINT16>((data[offset]<<8)|data[offset+1]);};
        const auto version=word(0),count=word(2);
        if(version==1&&count<=1024&&4+static_cast<size_t>(count)*4<=size){
            for(size_t i=0;i<count;++i)if(ppem<=word(4+i*4)){
                mode=(word(6+i*4)&8)?DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC:DWRITE_RENDERING_MODE_NATURAL;
                break;
            }
        }
    }
    if(context)face->ReleaseFontTable(context);
    if(mode!=DWRITE_RENDERING_MODE_DEFAULT)return mode;
    bytes=nullptr;context=nullptr;size=0;exists=FALSE;bool hinted=false;
    if(SUCCEEDED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('m','a','x','p'),&bytes,&size,&context,&exists))&&exists&&bytes&&size>=28){
        const auto* data=static_cast<const BYTE*>(bytes);
        hinted=data[0]==0&&data[1]==1&&data[2]==0&&data[3]==0&&(data[26]||data[27]);
    }
    if(context)face->ReleaseFontTable(context);
    return physicalSize>20||!hinted?DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC:DWRITE_RENDERING_MODE_NATURAL;
}

inline DWRITE_RENDERING_MODE FontRasterMode(IDWriteFontFace* face,float physicalSize){
    if(!face||!std::isfinite(physicalSize)||physicalSize<=0)return DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    struct Entry {Microsoft::WRL::ComPtr<IDWriteFontFace> face;DWRITE_RENDERING_MODE mode;};
    static thread_local std::map<std::pair<IDWriteFontFace*,float>,Entry> cache;
    const auto key=std::make_pair(face,physicalSize);
    if(const auto found=cache.find(key);found!=cache.end())return found->second.mode;
    const auto mode=DetermineFontRasterMode(face,physicalSize);
    if(cache.size()>=256)cache.clear();cache.emplace(key,Entry{face,mode});return mode;
}

class BitmapAdvanceRanges final : public TextRunObserver {
public:
    explicit BitmapAdvanceRanges(float scale):scale_(scale){}
    struct Adjustment {DWRITE_TEXT_RANGE range;float trailing=0;};
    std::vector<Adjustment> adjustments;
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT,FLOAT,DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*) override {
        if(!run||!description||!description->clusterMap||!run->glyphAdvances)return E_INVALIDARG;
        if(run->glyphCount!=description->stringLength)return S_OK;
        const auto sizes=FontBitmapSizes(run->fontFace);
        const float physicalSize=std::nearbyint(run->fontEmSize*scale_);
        if(!std::isfinite(physicalSize)||physicalSize<1||physicalSize>65535)return S_OK;
        const auto ppem=static_cast<UINT16>(physicalSize);
        if(std::find(sizes.begin(),sizes.end(),ppem)==sizes.end())return S_OK;
        std::vector<DWRITE_GLYPH_METRICS> metrics(run->glyphCount);
        if(FAILED(run->fontFace->GetGdiCompatibleGlyphMetrics(run->fontEmSize,scale_,nullptr,FALSE,
            run->glyphIndices,run->glyphCount,metrics.data(),run->isSideways)))return S_OK;
        DWRITE_FONT_METRICS font{};run->fontFace->GetMetrics(&font);if(!font.designUnitsPerEm)return S_OK;
        const float designScale=run->fontEmSize/font.designUnitsPerEm;
        for(UINT32 i=0;i<description->stringLength;++i){
            const UINT16 glyph=description->clusterMap[i];
            // Multi-character or multi-glyph clusters need a full shaping model;
            // do not redistribute them as independent character advances.
            if(glyph>=run->glyphCount||(i&&description->clusterMap[i-1]==glyph)||
               (i+1<description->stringLength&&description->clusterMap[i+1]==glyph))continue;
            const float target=std::round(metrics[glyph].advanceWidth*designScale*scale_)/scale_;
            const float delta=target-run->glyphAdvances[glyph];
            if(std::abs(delta)>0.0001f)adjustments.push_back({{description->textPosition+i,1},delta});
        }
        return S_OK;
    }
private:
    float scale_=1;
};
}

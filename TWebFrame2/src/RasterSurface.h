#pragma once

#include "FontFeatures.h"
#include "RasterSkia.h"
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace TWebFrame::Internal {

// The screen, print and snapshot paths share this surface. Pixel access is
// ordered with Direct2D commands; it never substitutes a reference capture.
class RasterSurface;
inline thread_local RasterSurface* activeRasterSurface=nullptr;

class RasterSurface {
public:
    struct Clip {D2D1_RECT_F rect;D2D1_ANTIALIAS_MODE mode;D2D1_MATRIX_3X2_F transform;};
    ID2D1RenderTarget* target=nullptr;
    IWICBitmap* bitmap=nullptr;
    UINT width=0,height=0;
    std::vector<Clip> clips;
    unsigned int layers=0;
    HRESULT error=S_OK;
    RasterSurface* previous=nullptr;

    RasterSurface(ID2D1RenderTarget* rt,IWICBitmap* pixels,UINT w,UINT h):
        target(rt),bitmap(pixels),width(w),height(h),previous(activeRasterSurface){activeRasterSurface=this;}
    ~RasterSurface(){activeRasterSurface=previous;}

    static float Linear(float value){
        return value<=0.04045f?value/12.92f:std::pow((value+0.055f)/1.055f,2.4f);
    }
    static float Encoded(float value){
        return value<=0.0031308f?value*12.92f:1.055f*std::pow(value,1.0f/2.4f)-0.055f;
    }
    static std::array<BYTE,256> CoverageTable(BYTE color){
        // Eight canonical luminance tables, with perceptually inverse backdrop.
        // Windows browser defaults use sRGB and contrast 1.0 in the mask stage.
        const unsigned int level=color>>5;
        const float source=((level<<5)|(level<<2)|(level>>1))/255.0f;
        const float destination=1-source;
        const float linearSource=Linear(source),linearDestination=Linear(destination);
        std::array<BYTE,256> table{};
        for(unsigned int i=0;i<256;++i){
            const float raw=i/255.0f;
            const float corrected=raw+(1-raw)*linearDestination*raw;
            const float output=Encoded(linearSource*corrected+(1-corrected)*linearDestination);
            const float coverage=std::abs(source-destination)<1.0f/256?corrected:
                (output-destination)/(source-destination);
            table[i]=static_cast<BYTE>(std::lround(std::clamp(coverage,0.0f,1.0f)*255));
        }
        return table;
    }

    static IDWriteFontFace* FileRasterFace(IDWriteFactory* factory,IDWriteFontFace* face){
        // Load the shaped face's bytes through a custom loader, as a sandboxed
        // font collection does. Preserve the index and simulations; layout
        // continues to use the original face and its existing metrics.
        struct Entry {Microsoft::WRL::ComPtr<IDWriteFontFace> source,raster;};
        struct Cache {
            Microsoft::WRL::ComPtr<IDWriteFactory5> factory;
            Microsoft::WRL::ComPtr<IDWriteInMemoryFontFileLoader> loader;
            std::vector<Entry> entries;
            ~Cache(){entries.clear();if(factory&&loader)factory->UnregisterFontFileLoader(loader.Get());}
        };
        static thread_local Cache cache;
        for(const auto& entry:cache.entries)if(entry.source.Get()==face)return entry.raster.Get();
        Microsoft::WRL::ComPtr<IDWriteFontFace5> variable;
        if(SUCCEEDED(face->QueryInterface(IID_PPV_ARGS(&variable)))&&variable->HasVariations())return face;
        if(!cache.loader){
            if(FAILED(factory->QueryInterface(IID_PPV_ARGS(&cache.factory)))||
               FAILED(cache.factory->CreateInMemoryFontFileLoader(&cache.loader)))return face;
            if(FAILED(cache.factory->RegisterFontFileLoader(cache.loader.Get()))){cache.loader.Reset();return face;}
        }
        UINT32 count=0;
        if(FAILED(face->GetFiles(&count,nullptr))||!count||count>64)return face;
        std::vector<IDWriteFontFile*> files(count);
        if(FAILED(face->GetFiles(&count,files.data())))return face;
        std::vector<Microsoft::WRL::ComPtr<IDWriteFontFile>> originals(count),custom(count);
        for(UINT32 i=0;i<count;++i)originals[i].Attach(files[i]);
        for(UINT32 i=0;i<count;++i){
            const void* key=nullptr;UINT32 keySize=0;
            Microsoft::WRL::ComPtr<IDWriteFontFileLoader> loader;
            Microsoft::WRL::ComPtr<IDWriteFontFileStream> stream;
            UINT64 size=0;
            if(FAILED(originals[i]->GetReferenceKey(&key,&keySize))||FAILED(originals[i]->GetLoader(&loader))||
               FAILED(loader->CreateStreamFromKey(key,keySize,&stream))||FAILED(stream->GetFileSize(&size))||
               !size||size>128*1024*1024)return face;
            const void* bytes=nullptr;void* fragment=nullptr;
            if(FAILED(stream->ReadFileFragment(&bytes,0,size,&fragment)))return face;
            const auto copied=cache.loader->CreateInMemoryFontFileReference(cache.factory.Get(),bytes,
                static_cast<UINT32>(size),nullptr,&custom[i]);
            stream->ReleaseFileFragment(fragment);
            if(FAILED(copied))return face;
            files[i]=custom[i].Get();
        }
        Microsoft::WRL::ComPtr<IDWriteFontFace> raster;
        const auto result=cache.factory->CreateFontFace(face->GetType(),count,files.data(),face->GetIndex(),face->GetSimulations(),&raster);
        if(FAILED(result))return face;
        if(cache.entries.size()>=128)cache.entries.clear();
        cache.entries.push_back({face,std::move(raster)});return cache.entries.back().raster.Get();
    }

    bool PaintSkia(const D2D1_RECT_F& rect,const std::array<D2D1_POINT_2F,4>& radii,
        unsigned int color,float sigma=0,const D2D1_RECT_F* inner=nullptr,
        const std::array<D2D1_POINT_2F,4>* innerRadii=nullptr,const std::vector<BYTE>* brushPixels=nullptr,
        UINT brushWidth=0,UINT brushHeight=0,const D2D1_POINT_2F* brushOrigin=nullptr,
        const D2D1_RECT_F* exclusion=nullptr,const std::array<D2D1_POINT_2F,4>* exclusionRadii=nullptr,
        const D2D1_POINT_2F* shadowOffset=nullptr){
        if(layers||!bitmap)return false;
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        if(transform._12!=0||transform._21!=0||transform._11<=0||transform._22<=0)return false;
        FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
        const float sx=dpiX/96*transform._11,sy=dpiY/96*transform._22;
        D2D1_RECT_F clip{0,0,static_cast<float>(width),static_cast<float>(height)};
        for(const auto& item:clips){
            if(item.transform._12!=0||item.transform._21!=0||item.mode!=D2D1_ANTIALIAS_MODE_ALIASED){
                // Fractional AA clips need coverage multiplication; integer
                // boundaries have identical aliased and antialiased coverage.
                const float edges[]={(item.rect.left*item.transform._11+item.transform._31)*dpiX/96,
                    (item.rect.top*item.transform._22+item.transform._32)*dpiY/96,
                    (item.rect.right*item.transform._11+item.transform._31)*dpiX/96,
                    (item.rect.bottom*item.transform._22+item.transform._32)*dpiY/96};
                if(item.transform._12!=0||item.transform._21!=0)return false;
                for(float edge:edges)if(std::abs(edge-std::round(edge))>0.0001f)return false;
            }
            clip.left=std::max(clip.left,(item.rect.left*item.transform._11+item.transform._31)*dpiX/96);
            clip.top=std::max(clip.top,(item.rect.top*item.transform._22+item.transform._32)*dpiY/96);
            clip.right=std::min(clip.right,(item.rect.right*item.transform._11+item.transform._31)*dpiX/96);
            clip.bottom=std::min(clip.bottom,(item.rect.bottom*item.transform._22+item.transform._32)*dpiY/96);
        }
        for(size_t i=0;i<clips.size();++i)target->PopAxisAlignedClip();
        const HRESULT ended=target->EndDraw();if(FAILED(ended)){error=ended;return true;}
        Microsoft::WRL::ComPtr<IWICBitmapLock> lock;const WICRect region{0,0,static_cast<INT>(width),static_cast<INT>(height)};
        bool painted=false;
        if(SUCCEEDED(bitmap->Lock(&region,WICBitmapLockRead|WICBitmapLockWrite,&lock))){
            UINT stride=0,size=0;BYTE* pixels=nullptr;
            if(SUCCEEDED(lock->GetStride(&stride))&&SUCCEEDED(lock->GetDataPointer(&size,&pixels)))
                painted=rasterSkia.RoundedRect(pixels,stride,width,height,rect,radii,color,sx,sy,
                    D2D1::Point2F(transform._31*dpiX/96,transform._32*dpiY/96),clip,sigma,inner,innerRadii,
                    brushPixels,brushWidth,brushHeight,brushOrigin,exclusion,exclusionRadii,shadowOffset);
        }
        lock.Reset();target->BeginDraw();
        for(const auto& item:clips){target->SetTransform(item.transform);target->PushAxisAlignedClip(item.rect,item.mode);}
        target->SetTransform(transform);return painted;
    }

    // The shared View target is software WIC. Keep curve/path coverage and
    // composition on that CPU target instead of uploading masks or reading GPU pixels.
    bool FillCircle(const D2D1_RECT_F& rect,ID2D1Brush* brush,float strokeWidth=0){
        if(!target||!brush)return false;
        const auto ellipse=D2D1::Ellipse(D2D1::Point2F((rect.left+rect.right)/2,
            (rect.top+rect.bottom)/2),(rect.right-rect.left)/2,(rect.bottom-rect.top)/2);
        if(strokeWidth>0)target->DrawEllipse(ellipse,brush,strokeWidth);
        else target->FillEllipse(ellipse,brush);
        return true;
    }
    bool FillRoundedRect(const D2D1_RECT_F& rect,float radius,ID2D1Brush* brush,float strokeWidth=0){
        if(!target||!brush)return false;
        const auto rounded=D2D1::RoundedRect(rect,radius,radius);
        if(strokeWidth>0)target->DrawRoundedRectangle(rounded,brush,strokeWidth);
        else target->FillRoundedRectangle(rounded,brush);
        return true;
    }
    bool DrawGeometry(ID2D1Geometry* geometry,ID2D1Brush* brush,float strokeWidth=0,ID2D1StrokeStyle* strokeStyle=nullptr){
        if(!target||!geometry||!brush)return false;
        if(strokeWidth>0)target->DrawGeometry(geometry,brush,strokeWidth,strokeStyle);
        else target->FillGeometry(geometry,brush);
        return true;
    }

    bool DrawGlyphRun(IDWriteFactory* factory,float x,float y,const DWRITE_GLYPH_RUN& run,
                      ID2D1Brush* brush){
        if(!factory||!brush||layers||!bitmap||!run.glyphCount||run.isSideways||
           !run.glyphAdvances||!run.glyphIndices)return false;
        Microsoft::WRL::ComPtr<IDWriteFontFace2> colorFace;
        if(SUCCEEDED(run.fontFace->QueryInterface(IID_PPV_ARGS(&colorFace)))&&colorFace->IsColorFont())return false;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solid;
        if(FAILED(brush->QueryInterface(IID_PPV_ARGS(&solid))))return false;
        auto color=solid->GetColor();color.a*=solid->GetOpacity();
        if(color.a<0.9999f)return false;
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        if(std::abs(transform._12)>0.00001f||std::abs(transform._21)>0.00001f||
           transform._11<=0||transform._22<=0)return false;
        FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
        const float sx=dpiX/96*transform._11,sy=dpiY/96*transform._22;
        if(std::abs(sx-sy)>0.00001f)return false;
        const float tx=dpiX/96*transform._31,ty=dpiY/96*transform._32;
        const float em=run.fontEmSize*sy;
        const auto mode=FontRasterMode(run.fontFace,em);
        auto* rasterFace=FileRasterFace(factory,run.fontFace);
        const std::array<BYTE,3> foreground{
            static_cast<BYTE>(std::lround(std::clamp(color.r,0.0f,1.0f)*255)),
            static_cast<BYTE>(std::lround(std::clamp(color.g,0.0f,1.0f)*255)),
            static_cast<BYTE>(std::lround(std::clamp(color.b,0.0f,1.0f)*255))};
        const std::array<std::array<BYTE,256>,3> tables{
            CoverageTable(foreground[0]),CoverageTable(foreground[1]),CoverageTable(foreground[2])};
        // Transform each saved clip to device coordinates. Fractional edges
        // receive pixel-area coverage; rotated clips use Direct2D.
        D2D1_RECT_F deviceClip{0,0,static_cast<float>(width),static_cast<float>(height)};
        for(const auto& item:clips){
            if(std::abs(item.transform._12)>0.00001f||std::abs(item.transform._21)>0.00001f)return false;
            const float l=(item.rect.left*item.transform._11+item.transform._31)*dpiX/96;
            const float t=(item.rect.top*item.transform._22+item.transform._32)*dpiY/96;
            const float r=(item.rect.right*item.transform._11+item.transform._31)*dpiX/96;
            const float b=(item.rect.bottom*item.transform._22+item.transform._32)*dpiY/96;
            deviceClip.left=std::max(deviceClip.left,l);deviceClip.top=std::max(deviceClip.top,t);
            deviceClip.right=std::min(deviceClip.right,r);deviceClip.bottom=std::min(deviceClip.bottom,b);
        }
        const RECT clip{static_cast<LONG>(std::floor(deviceClip.left)),static_cast<LONG>(std::floor(deviceClip.top)),
            static_cast<LONG>(std::ceil(deviceClip.right)),static_cast<LONG>(std::ceil(deviceClip.bottom))};
        struct GlyphMask {RECT bounds{};int left=0,top=0;std::vector<BYTE> alpha;};
        std::vector<GlyphMask> masks;masks.reserve(run.glyphCount);
        float pen=x;const bool rtl=(run.bidiLevel&1)!=0;
        for(UINT32 index=0;index<run.glyphCount;++index){
            if(rtl)pen-=run.glyphAdvances[index];
            const auto offset=run.glyphOffsets?run.glyphOffsets[index]:DWRITE_GLYPH_OFFSET{};
            const float px=std::floor(((pen+(rtl?-offset.advanceOffset:offset.advanceOffset))*sx+tx)*4+0.5f)/4;
            const float py=std::floor((y-offset.ascenderOffset)*sy+ty+0.5f);
            const float advance=0;auto glyph=run;glyph.fontEmSize=em;glyph.glyphCount=1;
            glyph.fontFace=rasterFace;
            const DWRITE_GLYPH_OFFSET zeroOffset{0,0};
            glyph.glyphIndices=run.glyphIndices+index;glyph.glyphAdvances=&advance;
            glyph.glyphOffsets=&zeroOffset;glyph.bidiLevel=0;
            const DWRITE_MATRIX matrix{1,0,0,1,px-std::floor(px),0};
            Microsoft::WRL::ComPtr<IDWriteGlyphRunAnalysis> analysis;
            const auto measuring=mode==DWRITE_RENDERING_MODE_GDI_CLASSIC?
                DWRITE_MEASURING_MODE_GDI_CLASSIC:DWRITE_MEASURING_MODE_NATURAL;
            if(FAILED(factory->CreateGlyphRunAnalysis(&glyph,1,&matrix,mode,measuring,0,0,&analysis)))return false;
            GlyphMask mask;mask.left=static_cast<int>(std::floor(px));mask.top=static_cast<int>(py);
            if(FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1,&mask.bounds)))return false;
            const int w=mask.bounds.right-mask.bounds.left,h=mask.bounds.bottom-mask.bounds.top;
            if(w>0&&h>0){
                if(w>4096||h>4096)return false;
                mask.alpha.resize(static_cast<size_t>(w)*h*3);
                if(FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1,&mask.bounds,mask.alpha.data(),static_cast<UINT32>(mask.alpha.size()))))return false;
                masks.push_back(std::move(mask));
            }
            if(!rtl)pen+=run.glyphAdvances[index];
        }
        for(size_t i=0;i<clips.size();++i)target->PopAxisAlignedClip();
        const auto finished=target->EndDraw();if(FAILED(finished)){error=finished;return true;}
        Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
        const WICRect bounds{0,0,static_cast<INT>(width),static_cast<INT>(height)};
        HRESULT result=bitmap->Lock(&bounds,WICBitmapLockRead|WICBitmapLockWrite,&lock);
        UINT stride=0,size=0;BYTE* pixels=nullptr;
        if(SUCCEEDED(result))result=lock->GetStride(&stride);
        if(SUCCEEDED(result))result=lock->GetDataPointer(&size,&pixels);
        if(SUCCEEDED(result)){
            // Compose LCD coverage directly into the CPU surface. Font painting
            // must not upload glyph textures or synchronously read GPU pixels.
            for(const auto& mask:masks){
                const int w=mask.bounds.right-mask.bounds.left,h=mask.bounds.bottom-mask.bounds.top;
                for(int yy=0;yy<h;++yy){
                    const int dy=mask.top+mask.bounds.top+yy;if(dy<clip.top||dy>=clip.bottom)continue;
                    for(int xx=0;xx<w;++xx){
                        const int dx=mask.left+mask.bounds.left+xx;if(dx<clip.left||dx>=clip.right)continue;
                        auto* destination=pixels+static_cast<size_t>(dy)*stride+dx*4;
                        const auto* raw=mask.alpha.data()+(static_cast<size_t>(yy)*w+xx)*3;
                        for(unsigned int channel=0;channel<3;++channel){
                            const unsigned int bits=channel==1?6:5;
                            const unsigned int coverage=tables[channel][raw[channel]]>>(8-bits);
                            const unsigned int expanded=(coverage<<(8-bits))|(coverage>>(2*bits-8));
                            const float clipCoverage=std::clamp(dx+1-deviceClip.left,0.0f,1.0f)*
                                std::clamp(deviceClip.right-dx,0.0f,1.0f)*
                                std::clamp(dy+1-deviceClip.top,0.0f,1.0f)*std::clamp(deviceClip.bottom-dy,0.0f,1.0f);
                            const float alpha=expanded/255.0f*clipCoverage;
                            const unsigned int bgra=2-channel;
                            destination[bgra]=static_cast<BYTE>(std::min(255L,
                                std::lround(destination[bgra]*(1-alpha))+std::lround(foreground[channel]*alpha)));
                        }
                        destination[3]=255;
                    }
                }
            }
        }else error=result;
        lock.Reset();target->BeginDraw();
        for(const auto& item:clips){target->SetTransform(item.transform);target->PushAxisAlignedClip(item.rect,item.mode);}
        target->SetTransform(transform);return true;
    }
};

inline void PushPaintClip(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,D2D1_ANTIALIAS_MODE mode){
    if(activeRasterSurface&&activeRasterSurface->target==target){
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        activeRasterSurface->clips.push_back({rect,mode,transform});
    }
    target->PushAxisAlignedClip(rect,mode);
}
inline void PopPaintClip(ID2D1RenderTarget* target){
    target->PopAxisAlignedClip();
    if(activeRasterSurface&&activeRasterSurface->target==target&&!activeRasterSurface->clips.empty())activeRasterSurface->clips.pop_back();
}
inline void PushPaintLayer(ID2D1RenderTarget* target,const D2D1_LAYER_PARAMETERS& parameters,ID2D1Layer* layer){
    if(activeRasterSurface&&activeRasterSurface->target==target)++activeRasterSurface->layers;
    target->PushLayer(parameters,layer);
}
inline void PopPaintLayer(ID2D1RenderTarget* target){
    target->PopLayer();
    if(activeRasterSurface&&activeRasterSurface->target==target&&activeRasterSurface->layers)--activeRasterSurface->layers;
}
}

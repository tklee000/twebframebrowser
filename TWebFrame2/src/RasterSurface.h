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
#include <map>
#include <memory>
#include <limits>
#include <optional>
#include <tuple>
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
    unsigned int glyphBatchDepth=0;
    Microsoft::WRL::ComPtr<IWICBitmapLock> glyphPixelLock;
    BYTE* glyphPixels=nullptr;
    UINT glyphStride=0;
    bool glyphSurfaceOpen=false;
    D2D1_MATRIX_3X2_F glyphTransform{};
    bool glyphClipValid=false;
    bool glyphClipBinary=true;
    bool glyphClipBinaryValid=false;
    FLOAT glyphClipDpiX=0,glyphClipDpiY=0;
    UINT glyphClipWidth=0,glyphClipHeight=0;
    D2D1_RECT_F glyphDeviceClip{};

    RasterSurface(ID2D1RenderTarget* rt,IWICBitmap* pixels,UINT w,UINT h):
        target(rt),bitmap(pixels),width(w),height(h),previous(activeRasterSurface){activeRasterSurface=this;}
    ~RasterSurface(){FlushGlyphBatch();activeRasterSurface=previous;}

    static bool NativeCapsuleInteger(float value,float magnitude=0){
        if(!std::isfinite(value)||!std::isfinite(magnitude))return false;
        const float rounded=std::round(value);
        if(value==rounded)return true;
        // Authored coordinates divided by DPI can return one ULP away from
        // their intended device integer. Bound that representation error,
        // retaining the absolute cap so large coordinates cannot snap a
        // visible fraction of a pixel.
        magnitude=std::max(std::abs(rounded),magnitude);
        const float ulp=std::nextafter(magnitude,std::numeric_limits<float>::infinity())-magnitude;
        return std::abs(value-rounded)<.0001f&&std::abs(value-rounded)<=ulp;
    }

    bool DeviceGlyphClip(FLOAT dpiX,FLOAT dpiY,D2D1_RECT_F& result,bool requireBinary=false){
        if(glyphClipValid&&glyphClipDpiX==dpiX&&glyphClipDpiY==dpiY&&glyphClipWidth==width&&glyphClipHeight==height&&
           (!requireBinary||glyphClipBinaryValid)){
            result=glyphDeviceClip;return true;
        }
        // Only finite positive DPI keys can be published below. Invalid DPI
        // cannot match a cached key, so validate on a miss or binary upgrade.
        if(!std::isfinite(dpiX)||!std::isfinite(dpiY)||dpiX<=0||dpiY<=0)return false;
        D2D1_RECT_F bounds{0,0,static_cast<float>(width),static_cast<float>(height)};
        const float deviceScaleX=dpiX/96,deviceScaleY=dpiY/96;
        bool binary=true;
        for(const auto& item:clips){
            const auto& m=item.transform;
            // Reject descriptor inputs before min/max can hide a NaN edge.
            for(float value:{item.rect.left,item.rect.top,item.rect.right,item.rect.bottom,
                m._11,m._12,m._21,m._22,m._31,m._32})
                if(!std::isfinite(value))return false;
            // A degenerate rectangle may gain an axis-aligned bounding box
            // under rotation/skew, just as Direct2D computes at clip push.
            if(item.rect.right<item.rect.left||item.rect.bottom<item.rect.top){bounds={0,0,0,0};break;}
            float left,top,right,bottom,leftMagnitude=0,topMagnitude=0,rightMagnitude=0,bottomMagnitude=0;
            if(m._12==0&&m._21==0){
                const float x0=(item.rect.left*m._11+m._31)*deviceScaleX;
                const float x1=(item.rect.right*m._11+m._31)*deviceScaleX;
                const float y0=(item.rect.top*m._22+m._32)*deviceScaleY;
                const float y1=(item.rect.bottom*m._22+m._32)*deviceScaleY;
                left=std::min(x0,x1);right=std::max(x0,x1);
                top=std::min(y0,y1);bottom=std::max(y0,y1);
                if(requireBinary&&item.mode!=D2D1_ANTIALIAS_MODE_ALIASED){
                    const float xm0=(std::abs(item.rect.left*m._11)+std::abs(m._31))*deviceScaleX;
                    const float xm1=(std::abs(item.rect.right*m._11)+std::abs(m._31))*deviceScaleX;
                    const float ym0=(std::abs(item.rect.top*m._22)+std::abs(m._32))*deviceScaleY;
                    const float ym1=(std::abs(item.rect.bottom*m._22)+std::abs(m._32))*deviceScaleY;
                    leftMagnitude=x0<=x1?xm0:xm1;rightMagnitude=x0<=x1?xm1:xm0;
                    topMagnitude=y0<=y1?ym0:ym1;bottomMagnitude=y0<=y1?ym1:ym0;
                }
            }else{
                // PushAxisAlignedClip saves the world-space bounding box,
                // including reflected, rotated and skewed rectangles.
                const D2D1::Matrix3x2F matrix(m._11,m._12,m._21,m._22,m._31,m._32);
                left=top=std::numeric_limits<float>::infinity();right=bottom=-left;
                for(const auto p:{D2D1::Point2F(item.rect.left,item.rect.top),D2D1::Point2F(item.rect.right,item.rect.top),
                    D2D1::Point2F(item.rect.left,item.rect.bottom),D2D1::Point2F(item.rect.right,item.rect.bottom)}){
                    const auto point=matrix.TransformPoint(p);
                    const float x=point.x*deviceScaleX,y=point.y*deviceScaleY;
                    if(requireBinary&&item.mode!=D2D1_ANTIALIAS_MODE_ALIASED){
                        const float xm=(std::abs(p.x*m._11)+std::abs(p.y*m._21)+std::abs(m._31))*deviceScaleX;
                        const float ym=(std::abs(p.x*m._12)+std::abs(p.y*m._22)+std::abs(m._32))*deviceScaleY;
                        if(x<left)leftMagnitude=xm;if(x>right)rightMagnitude=xm;
                        if(y<top)topMagnitude=ym;if(y>bottom)bottomMagnitude=ym;
                    }
                    left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);
                }
            }
            if(!std::isfinite(left)||!std::isfinite(top)||!std::isfinite(right)||!std::isfinite(bottom))return false;
            if(item.mode==D2D1_ANTIALIAS_MODE_ALIASED){
                // Binary clipping samples pixel centers; right/bottom edges
                // are exclusive, including exact half-pixel boundaries.
                left=std::ceil(left-.5f);right=std::ceil(right-.5f);
                top=std::ceil(top-.5f);bottom=std::ceil(bottom-.5f);
            }else if(requireBinary){
                // A native capsule can bypass only binary clip edges. Keep
                // fractional AA groups on the ordinary Direct2D path.
                // Off-surface edges cannot contribute fractional coverage.
                // Clamping also avoids magnifying cancellation error at a
                // reflected clip edge that is already outside the bitmap.
                if(!NativeCapsuleInteger(std::clamp(left,0.f,static_cast<float>(width)),leftMagnitude)||
                   !NativeCapsuleInteger(std::clamp(right,0.f,static_cast<float>(width)),rightMagnitude)||
                   !NativeCapsuleInteger(std::clamp(top,0.f,static_cast<float>(height)),topMagnitude)||
                   !NativeCapsuleInteger(std::clamp(bottom,0.f,static_cast<float>(height)),bottomMagnitude))binary=false;
            }
            bounds.left=std::max(bounds.left,left);bounds.top=std::max(bounds.top,top);
            bounds.right=std::min(bounds.right,right);bounds.bottom=std::min(bounds.bottom,bottom);
        }
        glyphDeviceClip=bounds;glyphClipBinary=binary;glyphClipBinaryValid=requireBinary;glyphClipDpiX=dpiX;glyphClipDpiY=dpiY;
        glyphClipWidth=width;glyphClipHeight=height;glyphClipValid=true;result=bounds;return true;
    }

    void BeginGlyphBatch(){++glyphBatchDepth;}
    void FlushGlyphBatch(){
        if(!glyphSurfaceOpen)return;
        // SetTransform may have changed while the CPU bitmap was locked.
        // Restore the caller's current transform after replaying saved clips.
        target->GetTransform(&glyphTransform);
        glyphPixelLock.Reset();glyphPixels=nullptr;glyphStride=0;glyphSurfaceOpen=false;
        target->BeginDraw();
        for(const auto& item:clips){target->SetTransform(item.transform);target->PushAxisAlignedClip(item.rect,item.mode);}
        target->SetTransform(glyphTransform);
    }
    void EndGlyphBatch(){if(glyphBatchDepth)--glyphBatchDepth;if(!glyphBatchDepth)FlushGlyphBatch();}
    bool LockGlyphSurface(){
        if(glyphSurfaceOpen)return glyphPixels!=nullptr;
        target->GetTransform(&glyphTransform);
        for(size_t i=0;i<clips.size();++i)target->PopAxisAlignedClip();
        HRESULT result=target->EndDraw();if(FAILED(result)){error=result;return false;}
        glyphSurfaceOpen=true;
        const WICRect bounds{0,0,static_cast<INT>(width),static_cast<INT>(height)};
        result=bitmap->Lock(&bounds,WICBitmapLockRead|WICBitmapLockWrite,&glyphPixelLock);
        UINT size=0;
        if(SUCCEEDED(result))result=glyphPixelLock->GetStride(&glyphStride);
        if(SUCCEEDED(result))result=glyphPixelLock->GetDataPointer(&size,&glyphPixels);
        if(FAILED(result))error=result;
        return SUCCEEDED(result);
    }

    static float Linear(float value){
        return value<=0.04045f?value/12.92f:std::pow((value+0.055f)/1.055f,2.4f);
    }
    static float Encoded(float value){
        return value<=0.0031308f?value*12.92f:1.055f*std::pow(value,1.0f/2.4f)-0.055f;
    }
    static std::array<BYTE,256> BuildCoverageTable(BYTE color){
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

    static const std::array<BYTE,256>& CoverageTable(BYTE color){
        static const auto tables=[]{
            std::array<std::array<BYTE,256>,8> result{};
            for(unsigned int level=0;level<result.size();++level)
                result[level]=BuildCoverageTable(static_cast<BYTE>(level<<5));
            return result;
        }();
        return tables[color>>5];
    }

    static const BYTE* GlyphBlendTable(BYTE color,unsigned int bits,bool nativeIcon=false){
        // LCD coverage has 32/64 levels; grayscale keeps all 256. Precompute
        // point rounding for every opaque backdrop byte; no paint math changes.
        // Native grayscale icons round the complete source-over result once.
        // Integer division retains ordinary text's separate point rounding.
        // Share ordinary backdrop rounding across colors. Black uses the
        // shared planes directly, including native black's identical result.
        // Three depth planes total 88 KiB; all palette/mode payloads together
        // remain below the previous 38 MiB per painting thread.
        static thread_local std::array<std::vector<BYTE>,3> backgrounds;
        static thread_local std::array<std::vector<BYTE>,1024> cache;
        const unsigned depth=bits==8?2:bits==6?1:0;
        const BYTE* background=nullptr;
        if(!nativeIcon||color==0){
            auto& values=backgrounds[depth];
            if(values.empty()){
                values.resize((1u<<bits)*256);
                for(unsigned int coverage=0;coverage<(1u<<bits);++coverage){
                    const auto expanded=(coverage<<(8-bits))|(coverage>>(2*bits-8));
                    for(unsigned int backdrop=0;backdrop<256;++backdrop)
                        values[(coverage<<8)|backdrop]=static_cast<BYTE>((backdrop*(255-expanded)+127)/255);
                }
            }
            background=values.data();
            if(color==0)return background;
        }
        auto& table=cache[static_cast<size_t>(color)*4+(nativeIcon?3:bits==8?2:bits==6?1:0)];
        if(table.empty()){
            table.resize((1u<<bits)*256);
            for(unsigned int coverage=0;coverage<(1u<<bits);++coverage){
                const unsigned int expanded=(coverage<<(8-bits))|(coverage>>(2*bits-8));
                const unsigned int ink=(color*expanded+127)/255;
                for(unsigned int backdrop=0;backdrop<256;++backdrop)
                    table[(coverage<<8)|backdrop]=static_cast<BYTE>(nativeIcon?
                        (color*expanded+backdrop*(255-expanded)+127)/255:
                        std::min(255u,background[(coverage<<8)|backdrop]+ink));
            }
        }
        return table.data();
    }

    struct GlyphTexture {
        // Retaining the face prevents a recycled COM address from matching an
        // unrelated font. Placement, paint color and clips are not raster inputs.
        Microsoft::WRL::ComPtr<IDWriteFontFace> face;
        RECT bounds{};
        unsigned int channels=3;
        std::vector<BYTE> alpha;
    };
    static std::shared_ptr<const GlyphTexture> GlyphCoverage(IDWriteFactory* factory,
        IDWriteFontFace* face,float em,UINT16 index,DWRITE_RENDERING_MODE mode,float phase,bool grayscale,bool nativeIcon=false,float horizontalScale=1,float verticalDirection=1){
        using Key=std::tuple<UINT_PTR,float,UINT16,DWRITE_RENDERING_MODE,unsigned int,bool,bool,float>;
        struct Cache {
            std::map<Key,std::shared_ptr<const GlyphTexture>> entries;
            size_t bytes=0;
            Key lastNativeKey{};
            std::shared_ptr<const GlyphTexture> lastNative;
        };
        static thread_local Cache cache;
        // Signed em in the key distinguishes vertical reflection without
        // increasing cache metadata. DirectWrite still receives positive em.
        const Key key{reinterpret_cast<UINT_PTR>(face),em*verticalDirection,index,mode,static_cast<unsigned int>(phase*4),grayscale,nativeIcon,horizontalScale};
        // The last native mask still belongs to this bounded cache. Controls
        // can reuse it without a tree walk for every identical glyph.
        if(nativeIcon&&cache.lastNative&&cache.lastNativeKey==key)return cache.lastNative;
        const auto found=cache.entries.find(key);
        if(found!=cache.entries.end()){
            if(nativeIcon){cache.lastNativeKey=key;cache.lastNative=found->second;}
            return found->second;
        }
        const float advance=0;const DWRITE_GLYPH_OFFSET offset{};
        const DWRITE_GLYPH_RUN glyph{face,em,1,&index,&advance,&offset,FALSE,0};
        const DWRITE_MATRIX matrix{horizontalScale,0,0,verticalDirection,phase,0};
        const auto measuring=mode==DWRITE_RENDERING_MODE_GDI_CLASSIC?
            DWRITE_MEASURING_MODE_GDI_CLASSIC:DWRITE_MEASURING_MODE_NATURAL;
        Microsoft::WRL::ComPtr<IDWriteGlyphRunAnalysis> analysis;
        if(nativeIcon){
            // Fluent SkFont icons request DirectWrite's grayscale hinting;
            // averaging an LCD mask produces different hinted edges.
            Microsoft::WRL::ComPtr<IDWriteFactory2> modern;
            if(FAILED(factory->QueryInterface(IID_PPV_ARGS(&modern)))||
               FAILED(modern->CreateGlyphRunAnalysis(&glyph,&matrix,mode,measuring,
                    DWRITE_GRID_FIT_MODE_ENABLED,DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,0,0,&analysis)))return {};
        }else if(FAILED(factory->CreateGlyphRunAnalysis(&glyph,1,&matrix,mode,measuring,0,0,&analysis)))return {};
        auto texture=std::make_shared<GlyphTexture>();texture->face=face;
        texture->channels=grayscale?1:3;
        const auto textureType=nativeIcon?DWRITE_TEXTURE_ALIASED_1x1:DWRITE_TEXTURE_CLEARTYPE_3x1;
        if(FAILED(analysis->GetAlphaTextureBounds(textureType,&texture->bounds)))return {};
        const int w=texture->bounds.right-texture->bounds.left,h=texture->bounds.bottom-texture->bounds.top;
        if(w>4096||h>4096)return {};
        if(w>0&&h>0){
            texture->alpha.resize(static_cast<size_t>(w)*h*(nativeIcon?1:3));
            if(FAILED(analysis->CreateAlphaTexture(textureType,&texture->bounds,
                texture->alpha.data(),static_cast<UINT32>(texture->alpha.size()))))return {};
            if(grayscale&&!nativeIcon){
                // Skia's RGBToA8 path averages raw LCD coverage before gamma
                // preblend. DirectWrite grayscale hinting changes the mask.
                const size_t count=static_cast<size_t>(w)*h;
                std::vector<BYTE> gray(count);
                for(size_t pixel=0;pixel<count;++pixel)
                    gray[pixel]=static_cast<BYTE>((static_cast<unsigned int>(texture->alpha[pixel*3])+
                        texture->alpha[pixel*3+1]+texture->alpha[pixel*3+2])/3);
                texture->alpha.swap(gray);
            }
        }
        // Bound both metadata and texture memory. Runs retain shared ownership
        // while painting, so eviction cannot invalidate their masks.
        constexpr size_t maxBytes=16*1024*1024,maxEntries=4096;
        if(texture->alpha.capacity()<=maxBytes){
            if(cache.entries.size()>=maxEntries||cache.bytes+texture->alpha.capacity()>maxBytes){
                cache.lastNative.reset();cache.entries.clear();cache.bytes=0;
            }
            cache.bytes+=texture->alpha.capacity();cache.entries.emplace(key,texture);
            if(nativeIcon){cache.lastNativeKey=key;cache.lastNative=texture;}
        }
        return texture;
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
        FlushGlyphBatch();
        if(layers||!bitmap||!target)return false;
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        if(transform._12!=0||transform._21!=0||transform._11<=0||transform._22<=0||
           !FiniteGlyphValue(transform._11)||!FiniteGlyphValue(transform._22)||
           !FiniteGlyphValue(transform._31)||!FiniteGlyphValue(transform._32))return false;
        FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
        const float sx=dpiX/96*transform._11,sy=dpiY/96*transform._22;
        if(!FiniteGlyphValue(sx)||!FiniteGlyphValue(sy)||sx<=0||sy<=0)return false;
        D2D1_RECT_F clip{0,0,static_cast<float>(width),static_cast<float>(height)};
        // PushAxisAlignedClip saves a device-space bounding box even for
        // reflected/rotated axes. Share its normalized pixel-center bounds
        // and invalidation cache with glyphs and native capsules.
        if(!clips.empty()&&(!DeviceGlyphClip(dpiX,dpiY,clip,true)||!glyphClipBinary))return false;
        if(clip.left>=clip.right||clip.top>=clip.bottom)return true;
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
    static __forceinline void ComposeNativeInk(BYTE* pixel,const std::array<BYTE,4>& ink){
        const unsigned int alpha=ink[3];
        if(!alpha)return;
        if(alpha==255){std::memcpy(pixel,ink.data(),4);return;}
        // UNORM composition quantizes premultiplied ink and alpha before
        // applying the destination factor. Keep destination premultiplied.
        for(unsigned int c=0;c<4;++c)pixel[c]=static_cast<BYTE>(std::min(255u,
            ink[c]+(pixel[c]*(255-alpha)+127)/255));
    }
    bool FillNativeCapsule(const D2D1_RECT_F& rect,ID2D1Brush* brush){
        if(!target||!bitmap||!brush||layers)return false;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solid;
        if(FAILED(brush->QueryInterface(IID_PPV_ARGS(&solid))))return false;
        auto color=solid->GetColor();color.a*=solid->GetOpacity();
        if(!std::isfinite(color.a)||!std::isfinite(color.r)||!std::isfinite(color.g)||!std::isfinite(color.b))return false;
        const float opacity=std::clamp(color.a,0.0f,1.0f);
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        if(!(rect.right>rect.left&&rect.bottom>rect.top))return false;
        FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
        const float a=dpiX/96*transform._11,b=dpiY/96*transform._12;
        const float c=dpiX/96*transform._21,d=dpiY/96*transform._22;
        // Signed axis permutations preserve a circular capsule. Other affine
        // transforms still require the general renderer's ellipse coverage.
        const bool diagonal=b==0&&c==0&&std::abs(a)>0&&std::abs(a)==std::abs(d);
        const bool swapped=a==0&&d==0&&std::abs(b)>0&&std::abs(b)==std::abs(c);
        if(!diagonal&&!swapped)return false;
        const float tx=dpiX/96*transform._31,ty=dpiY/96*transform._32;
        const auto integral=[](float v){
            // Keep the common exact grid case in this eligibility check;
            // call the larger numerical guard only for rounding residue.
            return std::isfinite(v)&&std::abs(v)<1e7f&&(v==std::round(v)||NativeCapsuleInteger(v));
        };
        const float ax=diagonal?rect.left*a+tx:rect.top*c+tx,ay=diagonal?rect.top*d+ty:rect.left*b+ty;
        const float bx=diagonal?rect.right*a+tx:rect.bottom*c+tx,by=diagonal?rect.bottom*d+ty:rect.right*b+ty;
        const D2D1_RECT_F bounds{std::min(ax,bx),std::min(ay,by),std::max(ax,bx),std::max(ay,by)};
        if(!integral(bounds.left)||!integral(bounds.top)||!integral(bounds.right)||!integral(bounds.bottom))return false;
        const int left=static_cast<int>(std::round(bounds.left)),top=static_cast<int>(std::round(bounds.top));
        const int right=static_cast<int>(std::round(bounds.right)),bottom=static_cast<int>(std::round(bounds.bottom));
        const bool vertical=bottom-top>right-left;
        const int thickness=std::min(bottom-top,right-left);
        if(thickness<=0||thickness>256)return false;
        RECT clip{0,0,static_cast<LONG>(width),static_cast<LONG>(this->height)};
        if(!clips.empty()){
            D2D1_RECT_F device{};
            if(!DeviceGlyphClip(dpiX,dpiY,device,true))return false;
            if(device.left>=device.right||device.top>=device.bottom)return true;
            if(!glyphClipBinary)return false;
            clip={static_cast<LONG>(std::round(device.left)),static_cast<LONG>(std::round(device.top)),
                  static_cast<LONG>(std::round(device.right)),static_cast<LONG>(std::round(device.bottom))};
        }
        const int x0=std::max<LONG>(left,clip.left),x1=std::min<LONG>(right,clip.right);
        const int y0=std::max<LONG>(top,clip.top),y1=std::min<LONG>(bottom,clip.bottom);
        if(x0>=x1||y0>=y1||opacity==0)return true;
        // Circular native-theme ends use signed distance coverage, not the
        // area integration of WIC's general rounded rectangle. Integer edges
        // make the mask depend only on thickness; position, color and clips
        // remain live inputs. Bound cache metadata and mask storage to 1 MiB.
        struct Mask {
            int capWidth=0;std::vector<float> coverage;
            std::vector<std::array<BYTE,4>> ink;
            std::array<BYTE,3> color{};float opacity=-1;
            std::array<BYTE,4> center{};
        };
        // Thickness is already restricted to 1..256. Direct slots avoid a
        // tree search and a node allocation; live masks retain the 32-entry
        // and 1 MiB payload bounds, including eviction and local ownership.
        struct Cache {std::array<std::shared_ptr<Mask>,256> entries{};size_t count=0,bytes=0;};
        static thread_local Cache cache;
        std::shared_ptr<Mask> mask;
        if(cache.entries[thickness-1])mask=cache.entries[thickness-1];
        else{
            auto value=std::make_shared<Mask>();value->capWidth=(thickness+1)/2;
            value->coverage.resize(static_cast<size_t>(thickness)*value->capWidth);
            value->ink.resize(value->coverage.size());
            const float radius=thickness/2.0f;
            for(int y=0;y<thickness;++y)for(int x=0;x<value->capWidth;++x){
                const float dx=x+.5f-radius,dy=y+.5f-radius;
                value->coverage[static_cast<size_t>(y)*value->capWidth+x]=
                    std::clamp(radius+.5f-std::sqrt(dx*dx+dy*dy),0.0f,1.0f);
            }
            const size_t bytes=value->coverage.capacity()*sizeof(float)+value->ink.capacity()*sizeof(std::array<BYTE,4>);
            if(cache.count>=32||cache.bytes+bytes>1024*1024){cache.entries.fill(nullptr);cache.count=cache.bytes=0;}
            ++cache.count;cache.bytes+=bytes;cache.entries[thickness-1]=value;mask=std::move(value);
        }
        const std::array<BYTE,3> foreground{
            static_cast<BYTE>(std::lround(std::clamp(color.b,0.0f,1.0f)*255)),
            static_cast<BYTE>(std::lround(std::clamp(color.g,0.0f,1.0f)*255)),
            static_cast<BYTE>(std::lround(std::clamp(color.r,0.0f,1.0f)*255))};
        if(mask->color!=foreground||mask->opacity!=opacity){
            mask->color=foreground;mask->opacity=opacity;
            auto premultiply=[&](float coverage){
                const float alpha=coverage*opacity;
                return std::array<BYTE,4>{static_cast<BYTE>(std::lround(foreground[0]*alpha)),
                    static_cast<BYTE>(std::lround(foreground[1]*alpha)),static_cast<BYTE>(std::lround(foreground[2]*alpha)),
                    static_cast<BYTE>(std::lround(255*alpha))};
            };
            for(size_t i=0;i<mask->coverage.size();++i)mask->ink[i]=premultiply(mask->coverage[i]);
            mask->center=premultiply(1);
        }
        if(!LockGlyphSurface()){if(!glyphBatchDepth)FlushGlyphBatch();return true;}
        auto centerSpan=[&](BYTE* row,int begin,int end){
            const auto& ink=mask->center;
            if(ink[3]==255){
                // Constant opaque spans let the compiler batch the stores.
                // memcpy keeps the WIC byte buffer free of alignment assumptions.
                for(int x=begin;x<end;++x)std::memcpy(row+x*4,ink.data(),4);
            }else if(ink[3]){
                for(int x=begin;x<end;++x)ComposeNativeInk(row+x*4,ink);
            }
        };
        for(int y=y0;y<y1;++y){
            auto* row=glyphPixels+static_cast<size_t>(y)*glyphStride;
            if(vertical){
                const int capY=std::min(y-top,bottom-1-y);
                if(capY>=mask->capWidth)centerSpan(row,x0,x1);
                else for(int x=x0;x<x1;++x)ComposeNativeInk(row+x*4,mask->ink[static_cast<size_t>(x-left)*mask->capWidth+capY]);
                continue;
            }
            if(right-left<=2*mask->capWidth+4){
                // Very short capsules have little central span. Keep one loop
                // to avoid splitting a row for one or two opaque pixels.
                for(int x=x0;x<x1;++x){
                    const int capX=std::min(x-left,right-1-x);
                    ComposeNativeInk(row+x*4,capX>=mask->capWidth?mask->center:
                        mask->ink[static_cast<size_t>(y-top)*mask->capWidth+capX]);
                }
                continue;
            }
            int x=x0;const size_t maskRow=static_cast<size_t>(y-top)*mask->capWidth;
            const int firstEnd=std::min(x1,left+mask->capWidth),centerEnd=std::min(x1,right-mask->capWidth);
            for(;x<firstEnd;++x)ComposeNativeInk(row+x*4,mask->ink[maskRow+std::min(x-left,right-1-x)]);
            if(x<centerEnd){centerSpan(row,x,centerEnd);x=centerEnd;}
            for(;x<x1;++x)ComposeNativeInk(row+x*4,mask->ink[maskRow+std::min(x-left,right-1-x)]);
        }
        if(!glyphBatchDepth)FlushGlyphBatch();
        return true;
    }
    bool DrawGeometry(ID2D1Geometry* geometry,ID2D1Brush* brush,float strokeWidth=0,ID2D1StrokeStyle* strokeStyle=nullptr){
        if(!target||!geometry||!brush)return false;
        if(strokeWidth>0)target->DrawGeometry(geometry,brush,strokeWidth,strokeStyle);
        else target->FillGeometry(geometry,brush);
        return true;
    }

    static __forceinline bool FiniteGlyphValue(float value){
        // IEEE exponent inspection avoids a CRT classification call in every
        // glyph placement; memcpy keeps the check free of aliasing assumptions.
        unsigned int bits=0;std::memcpy(&bits,&value,sizeof(bits));
        return (bits&0x7f800000u)!=0x7f800000u;
    }

    static IDWriteFontFace* NativeGlyphFace(IDWriteFactory* factory,IDWriteFontFace* source){
        // Faces are immutable. Keep one owned factory/face pair so a repeated
        // native paint avoids querying color support and the file-face map.
        // Pointer reuse cannot alias an expired face while these owners live.
        struct FaceState {Microsoft::WRL::ComPtr<IDWriteFactory> factory;
            Microsoft::WRL::ComPtr<IDWriteFontFace> source,raster;};
        static thread_local FaceState state;
        if(state.factory.Get()==factory&&state.source.Get()==source)return state.raster.Get();
        Microsoft::WRL::ComPtr<IDWriteFontFace2> colorFace;
        if(SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&colorFace)))&&colorFace->IsColorFont())return nullptr;
        auto* raster=FileRasterFace(factory,source);if(!raster)return nullptr;
        state.factory=factory;state.source=source;state.raster=raster;return raster;
    }

    struct NativeOpacityBlend {
        float opacity=-1;
        std::array<BYTE,3> foreground{};
        unsigned int repetitions=0;
        bool ready=false;
        std::array<std::array<BYTE,65536>,4> values{};
    };
    __declspec(noinline) static const NativeOpacityBlend* NativeOpacityTables(const std::array<BYTE,3>& foreground,float opacity){
        // One RGB/opacity combination per thread, bounded at 256 KiB. Use the
        // corrected coverage byte as key, without quantizing brush opacity.
        static thread_local NativeOpacityBlend state;
        if(state.opacity!=opacity||state.foreground!=foreground){
            state.opacity=opacity;state.foreground=foreground;
            state.repetitions=1;state.ready=false;return nullptr;
        }
        // Changing styles take the direct route. A stable combination earns
        // the larger lookup only after repeated visible paints.
        if(!state.ready){
            if(++state.repetitions<8)return nullptr;
            const bool monochrome=foreground[0]==foreground[1]&&foreground[1]==foreground[2];
            for(unsigned int coverage=0;coverage<256;++coverage){
                const double alpha=coverage*static_cast<double>(opacity);
                const double inverse=255-alpha;
                const std::array<double,3> ink{foreground[0]*alpha,foreground[1]*alpha,foreground[2]*alpha};
                for(unsigned int backdrop=0;backdrop<256;++backdrop){
                    const unsigned int index=(coverage<<8)|backdrop;
                    const double background=backdrop*inverse;
                    // Keep native single rounding and double evaluation order.
                    // Source terms depend only on coverage; all RGB and alpha
                    // planes reuse the same unrounded backdrop contribution.
                    for(unsigned int channel=0;channel<(monochrome?1u:3u);++channel)
                        state.values[channel][index]=static_cast<BYTE>(
                            (background+ink[channel])/255+.5);
                    // These nonnegative results are bounded by 255. Truncation
                    // after adding 1/2 implements the same half-away rounding
                    // without a CRT call for each of the 262,144 table bytes.
                    state.values[3][index]=static_cast<BYTE>(background/255+alpha+.5);
                }
            }
            // Equal RGB channels have identical tables; copy their bytes after
            // computing one plane rather than repeating the same rounding.
            if(monochrome){state.values[1]=state.values[0];state.values[2]=state.values[0];}
            state.ready=true;
        }
        return &state;
    }

    __declspec(noinline) void ComposeNativeCachedOpacity(const GlyphTexture& texture,int w,int left,int top,
        int firstX,int lastX,int firstY,int lastY,const BYTE* coverageTable,
        const NativeOpacityBlend* opacityBlends,bool monochromeIcon){
        // Select the full-clip cached loop once per mask. Keep its table-only
        // code separate from paints whose opacity changes before reuse.
        if(monochromeIcon){
            const auto& rgb=opacityBlends->values[0];
            for(int yy=firstY;yy<lastY;++yy){
                auto* row=glyphPixels+static_cast<size_t>(top+yy)*glyphStride;
                const auto* maskRow=texture.alpha.data()+static_cast<size_t>(yy)*w;
                for(int xx=firstX;xx<lastX;++xx){
                    auto* destination=row+(left+xx)*4;
                    const unsigned int rowIndex=static_cast<unsigned int>(coverageTable[maskRow[xx]])<<8;
                    // Coverage zero is the identity in every cached plane.
                    if(!rowIndex)continue;
                    if(destination[0]==destination[1]&&destination[1]==destination[2]){
                        const BYTE value=rgb[rowIndex|destination[0]];
                        destination[0]=destination[1]=destination[2]=value;
                    }else{
                        // Equal foreground channels share the same lookup plane
                        // even when the backdrop channels differ.
                        destination[0]=rgb[rowIndex|destination[0]];
                        destination[1]=rgb[rowIndex|destination[1]];
                        destination[2]=rgb[rowIndex|destination[2]];
                    }
                    if(destination[3]!=255)destination[3]=opacityBlends->values[3][rowIndex|destination[3]];
                }
            }
            return;
        }
        for(int yy=firstY;yy<lastY;++yy){
            auto* row=glyphPixels+static_cast<size_t>(top+yy)*glyphStride;
            const auto* maskRow=texture.alpha.data()+static_cast<size_t>(yy)*w;
            for(int xx=firstX;xx<lastX;++xx){
                auto* destination=row+(left+xx)*4;
                const unsigned int rowIndex=static_cast<unsigned int>(coverageTable[maskRow[xx]])<<8;
                // Coverage zero is the identity in every cached plane.
                if(!rowIndex)continue;
                for(unsigned int channel=0;channel<3;++channel){
                    const unsigned int bgra=2-channel;
                    destination[bgra]=opacityBlends->values[channel][rowIndex|destination[bgra]];
                }
                if(destination[3]!=255)destination[3]=opacityBlends->values[3][rowIndex|destination[3]];
            }
        }
    }

    __declspec(noinline) void ComposeNativeOpacity(const GlyphTexture& texture,int w,int left,int top,
        int firstX,int lastX,int firstY,int lastY,bool fullyCovered,const D2D1_RECT_F& deviceClip,
        const std::array<BYTE,3>& foreground,const BYTE* coverageTable,const double* opacityAlpha,
        const NativeOpacityBlend* opacityBlends,bool monochromeIcon){
        // Keep the partial-opacity loop out of the ordinary/opaque paint body.
        // It is called once per visible mask, rather than once per pixel.
        for(int yy=firstY;yy<lastY;++yy){
            const int dy=top+yy;
            const float clippedY=fullyCovered?1.f:std::max(0.f,
                std::min(dy+1.f,deviceClip.bottom)-std::max(static_cast<float>(dy),deviceClip.top));
            auto* row=glyphPixels+static_cast<size_t>(dy)*glyphStride;
            const auto* maskRow=texture.alpha.data()+static_cast<size_t>(yy)*w;
            for(int xx=firstX;xx<lastX;++xx){
                const int dx=left+xx;
                const double clipped=fullyCovered?1.0:static_cast<double>(clippedY*std::max(0.f,
                    std::min(dx+1.f,deviceClip.right)-std::max(static_cast<float>(dx),deviceClip.left)));
                const unsigned int coverage=coverageTable[maskRow[xx]];
                auto* destination=row+dx*4;
                if(clipped==1&&opacityBlends){
                    const unsigned int rowIndex=coverage<<8;
                    if(monochromeIcon&&destination[0]==destination[1]&&destination[1]==destination[2]){
                        const BYTE value=opacityBlends->values[0][rowIndex|destination[0]];
                        destination[0]=destination[1]=destination[2]=value;
                    }else for(unsigned int channel=0;channel<3;++channel){
                        const unsigned int bgra=2-channel;
                        destination[bgra]=opacityBlends->values[channel][rowIndex|destination[bgra]];
                    }
                    if(destination[3]!=255)destination[3]=opacityBlends->values[3][rowIndex|destination[3]];
                    continue;
                }
                const double alpha=opacityAlpha[coverage]*clipped;
                if(alpha==0)continue;
                if(monochromeIcon&&destination[0]==destination[1]&&destination[1]==destination[2]){
                    const BYTE value=static_cast<BYTE>(std::lround(
                        (destination[0]*(255-alpha)+foreground[0]*alpha)/255));
                    destination[0]=destination[1]=destination[2]=value;
                }else for(unsigned int channel=0;channel<3;++channel){
                    const unsigned int bgra=2-channel;
                    destination[bgra]=static_cast<BYTE>(std::lround(
                        (destination[bgra]*(255-alpha)+foreground[channel]*alpha)/255));
                }
                if(destination[3]!=255)destination[3]=static_cast<BYTE>(std::lround(destination[3]*(255-alpha)/255+alpha));
            }
        }
    }

    __declspec(noinline) static const NativeOpacityBlend* OrdinaryOpacityTables(const std::array<BYTE,3>& foreground,float opacity){
        // Ordinary text point-rounds source and backdrop separately. Keep its
        // 256 KiB table independent of native icons' single-round contract.
        static thread_local NativeOpacityBlend state;
        if(state.opacity!=opacity||state.foreground!=foreground){
            state.opacity=opacity;state.foreground=foreground;
            state.repetitions=1;state.ready=false;return nullptr;
        }
        if(!state.ready){
            if(++state.repetitions<8)return nullptr;
            const bool monochrome=foreground[0]==foreground[1]&&foreground[1]==foreground[2];
            for(unsigned int coverage=0;coverage<256;++coverage){
                const float alpha=coverage/255.f*opacity;
                // Source contributions depend on coverage, not the backdrop.
                // Keep the float products and separate point-rounding rule.
                std::array<long,3> source{};
                for(unsigned int channel=0;channel<(monochrome?1u:3u);++channel)
                    source[channel]=std::lround(foreground[channel]*alpha);
                const long sourceAlpha=std::lround(255*alpha);
                for(unsigned int backdrop=0;backdrop<256;++backdrop){
                    const unsigned int index=(coverage<<8)|backdrop;
                    const long background=std::lround(backdrop*(1-alpha));
                    for(unsigned int channel=0;channel<(monochrome?1u:3u);++channel)
                        state.values[channel][index]=static_cast<BYTE>(std::min(255L,
                            background+source[channel]));
                    state.values[3][index]=static_cast<BYTE>(std::min(255L,background+sourceAlpha));
                }
            }
            if(monochrome){state.values[1]=state.values[0];state.values[2]=state.values[0];}
            state.ready=true;
        }
        return &state;
    }

    __declspec(noinline) void ComposeOrdinaryOpacity(const GlyphTexture& texture,int w,int left,int top,
        int firstX,int lastX,int firstY,int lastY,bool fullyCovered,const D2D1_RECT_F& deviceClip,
        const std::array<BYTE,3>& foreground,const BYTE* coverageTable,float opacity,
        const NativeOpacityBlend* opacityBlends,bool monochrome){
        for(int yy=firstY;yy<lastY;++yy){
            const int dy=top+yy;
            const float clippedY=fullyCovered?1.f:std::max(0.f,
                std::min(dy+1.f,deviceClip.bottom)-std::max(static_cast<float>(dy),deviceClip.top));
            auto* row=glyphPixels+static_cast<size_t>(dy)*glyphStride;
            const auto* maskRow=texture.alpha.data()+static_cast<size_t>(yy)*w;
            for(int xx=firstX;xx<lastX;++xx){
                const int dx=left+xx;
                const float clipped=fullyCovered?1.f:clippedY*std::max(0.f,
                    std::min(dx+1.f,deviceClip.right)-std::max(static_cast<float>(dx),deviceClip.left));
                const unsigned int coverage=coverageTable[maskRow[xx]];
                auto* destination=row+dx*4;
                if(clipped==1&&opacityBlends){
                    const unsigned int index=coverage<<8;
                    if(monochrome&&destination[0]==destination[1]&&destination[1]==destination[2]){
                        const BYTE value=opacityBlends->values[0][index|destination[0]];
                        destination[0]=destination[1]=destination[2]=value;
                    }else for(unsigned int channel=0;channel<3;++channel){
                        const unsigned int bgra=2-channel;
                        destination[bgra]=opacityBlends->values[channel][index|destination[bgra]];
                    }
                    // Separate point rounding can change opaque alpha too.
                    destination[3]=opacityBlends->values[3][index|destination[3]];
                    continue;
                }
                const float alpha=coverage/255.f*opacity*clipped;
                if(alpha==0)continue;
                if(monochrome&&destination[0]==destination[1]&&destination[1]==destination[2]){
                    const BYTE value=static_cast<BYTE>(std::min(255L,
                        std::lround(destination[0]*(1-alpha))+std::lround(foreground[0]*alpha)));
                    destination[0]=destination[1]=destination[2]=value;
                }else for(unsigned int channel=0;channel<3;++channel){
                    const unsigned int bgra=2-channel;
                    destination[bgra]=static_cast<BYTE>(std::min(255L,
                        std::lround(destination[bgra]*(1-alpha))+std::lround(foreground[channel]*alpha)));
                }
                destination[3]=static_cast<BYTE>(std::min(255L,
                    std::lround(destination[3]*(1-alpha))+std::lround(255*alpha)));
            }
        }
    }

    bool DrawGlyphRun(IDWriteFactory* factory,float x,float y,const DWRITE_GLYPH_RUN& run,
                      ID2D1Brush* brush,bool nativeIcon=false){
        if(!factory||!target||!brush||layers||!bitmap||!run.fontFace||!run.glyphCount||run.isSideways||
           !run.glyphAdvances||!run.glyphIndices||!FiniteGlyphValue(x)||!FiniteGlyphValue(y)||
           !FiniteGlyphValue(run.fontEmSize)||run.fontEmSize<=0)return false;
        const auto antialias=target->GetTextAntialiasMode();
        if(antialias!=D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE&&antialias!=D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE)return false;
        const bool grayscale=antialias==D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE;
        if(nativeIcon&&!grayscale)return false;
        IDWriteFontFace* nativeFace=nullptr;
        if(nativeIcon){nativeFace=NativeGlyphFace(factory,run.fontFace);if(!nativeFace)return false;}
        else{
            Microsoft::WRL::ComPtr<IDWriteFontFace2> colorFace;
            if(SUCCEEDED(run.fontFace->QueryInterface(IID_PPV_ARGS(&colorFace)))&&colorFace->IsColorFont())return false;
        }
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solid;
        if(FAILED(brush->QueryInterface(IID_PPV_ARGS(&solid))))return false;
        auto color=solid->GetColor();const float brushOpacity=solid->GetOpacity();
        if(!FiniteGlyphValue(color.r)||!FiniteGlyphValue(color.g)||!FiniteGlyphValue(color.b)||
            !FiniteGlyphValue(color.a)||!FiniteGlyphValue(brushOpacity))return false;
        color.a=std::clamp(color.a*brushOpacity,0.0f,1.0f);
        if(!nativeIcon&&!grayscale&&color.a!=1)return false;
        const bool translucentIcon=nativeIcon&&color.a<1;
        const bool translucentText=!nativeIcon&&grayscale&&color.a<1;
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        // A tiny shear can still move a large authored glyph substantially.
        // Every cached mask requires exact diagonal axes; unsupported affine
        // transforms must reach the fallback rather than silently lose a term.
        if(transform._12!=0||transform._21!=0||transform._11==0||transform._22==0)return false;
        FLOAT dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
        if(!FiniteGlyphValue(dpiX)||!FiniteGlyphValue(dpiY)||dpiX<=0||dpiY<=0||
            !FiniteGlyphValue(transform._11)||!FiniteGlyphValue(transform._12)||!FiniteGlyphValue(transform._21)||
            !FiniteGlyphValue(transform._22)||!FiniteGlyphValue(transform._31)||!FiniteGlyphValue(transform._32))return false;
        const float sx=dpiX/96*transform._11,sy=dpiY/96*transform._22;
        if(!FiniteGlyphValue(sx)||!FiniteGlyphValue(sy)||sx==0||sy==0)return false;
        // Positive uniform masks retain their original path. All other diagonal
        // masks apply signed axes after positive em scaling, including reflection.
        const float verticalDirection=sy<0?-1.f:1.f;
        const float horizontalScale=sy<0||sx!=sy?sx/std::abs(sy):1.f;
        if(!FiniteGlyphValue(horizontalScale)||std::abs(horizontalScale)<.125f||std::abs(horizontalScale)>8)return false;
        const float tx=dpiX/96*transform._31,ty=dpiY/96*transform._32;
        const float em=run.fontEmSize*std::abs(sy);
        if(!FiniteGlyphValue(em)||em<=0||!FiniteGlyphValue(tx)||!FiniteGlyphValue(ty))return false;
        // Native-theme icons use SkFont's normal symmetric grayscale raster,
        // independently of the text face's gasp-driven layout raster mode.
        const auto mode=nativeIcon?DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC:FontRasterMode(run.fontFace,em);
        auto* rasterFace=nativeIcon?nativeFace:FileRasterFace(factory,run.fontFace);
        const std::array<BYTE,3> foreground{
            static_cast<BYTE>(std::lround(std::clamp(color.r,0.0f,1.0f)*255)),
            static_cast<BYTE>(std::lround(std::clamp(color.g,0.0f,1.0f)*255)),
            static_cast<BYTE>(std::lround(std::clamp(color.b,0.0f,1.0f)*255))};
        const auto luminance=static_cast<BYTE>((54u*foreground[0]+183u*foreground[1]+19u*foreground[2])>>8);
        // Reuse the saved clip bounds until the stack, DPI or surface changes.
        // Current glyph placement still uses the current render transform.
        D2D1_RECT_F deviceClip{0,0,static_cast<float>(width),static_cast<float>(height)};
        if(!clips.empty()&&!DeviceGlyphClip(dpiX,dpiY,deviceClip))return false;
        // Test the geometric intersection before floor/ceil can turn a
        // zero-width fractional clip into a one-pixel integer bounding box.
        if(deviceClip.right<=deviceClip.left||deviceClip.bottom<=deviceClip.top)return true;
        const RECT clip{static_cast<LONG>(std::floor(deviceClip.left)),static_cast<LONG>(std::floor(deviceClip.top)),
            static_cast<LONG>(std::ceil(deviceClip.right)),static_cast<LONG>(std::ceil(deviceClip.bottom))};
        if(clip.right<=clip.left||clip.bottom<=clip.top)return true;
        struct GlyphMask {std::shared_ptr<const GlyphTexture> texture;int left=0,top=0;};
        // Short runs avoid a heap allocation on every paint. Only construct the
        // inline references when used; longer runs retain bounded empty storage.
        std::optional<std::array<GlyphMask,8>> inlineMasks;
        std::vector<GlyphMask> localMasks;
        const bool shortRun=run.glyphCount<=8;
        // Reuse a bounded capacity for longer runs. Release every texture
        // reference on every exit; only the empty storage survives this paint.
        const bool reuseMasks=!shortRun&&run.glyphCount<=256;
        auto* reusableMasks=[&]()->std::vector<GlyphMask>*{
            if(!reuseMasks)return nullptr;
            static thread_local std::vector<GlyphMask> storage;return &storage;
        }();
        auto& heapMasks=reuseMasks?*reusableMasks:localMasks;
        struct ReleaseMasks {std::vector<GlyphMask>* masks;
            ~ReleaseMasks(){if(masks)masks->clear();}} releaseMasks{reuseMasks?&heapMasks:nullptr};
        if(shortRun)inlineMasks.emplace();
        else heapMasks.reserve(run.glyphCount);
        size_t maskCount=0;
        float pen=x;const bool rtl=(run.bidiLevel&1)!=0;
        for(UINT32 index=0;index<run.glyphCount;++index){
            if(rtl)pen-=run.glyphAdvances[index];
            const auto offset=run.glyphOffsets?run.glyphOffsets[index]:DWRITE_GLYPH_OFFSET{};
            if(!FiniteGlyphValue(run.glyphAdvances[index])||!FiniteGlyphValue(offset.advanceOffset)||
                !FiniteGlyphValue(offset.ascenderOffset))return false;
            const float px=std::floor(((pen+(rtl?-offset.advanceOffset:offset.advanceOffset))*sx+tx)*4+0.5f)/4;
            const float py=std::floor((y-offset.ascenderOffset)*sy+ty+0.5f);
            if(!(px>=-10000000&&px<=10000000&&py>=-10000000&&py<=10000000))return false;
            auto texture=GlyphCoverage(factory,rasterFace,em,run.glyphIndices[index],mode,px-std::floor(px),grayscale,nativeIcon,horizontalScale,verticalDirection);
            if(!texture)return false;
            const int left=static_cast<int>(std::floor(px)),top=static_cast<int>(py);
            const auto& bounds=texture->bounds;
            if(!texture->alpha.empty()&&left+bounds.right>clip.left&&left+bounds.left<clip.right&&
               top+bounds.bottom>clip.top&&top+bounds.top<clip.bottom)
            {
                if(shortRun)(*inlineMasks)[maskCount]={std::move(texture),left,top};
                else heapMasks.push_back({std::move(texture),left,top});
                ++maskCount;
            }
            if(!rtl)pen+=run.glyphAdvances[index];
        }
        if(!maskCount||color.a==0)return true;
        // Validate the complete run before preparing any composition tables.
        const std::array<const BYTE*,3> tables{
            CoverageTable(grayscale?luminance:foreground[0]).data(),CoverageTable(grayscale?luminance:foreground[1]).data(),
            CoverageTable(grayscale?luminance:foreground[2]).data()};
        const std::array<const BYTE*,3> blends{
            (translucentIcon||translucentText)?nullptr:GlyphBlendTable(foreground[0],grayscale?8:5,nativeIcon),
            (translucentIcon||translucentText)?nullptr:GlyphBlendTable(foreground[1],grayscale?8:6,nativeIcon),
            (translucentIcon||translucentText)?nullptr:GlyphBlendTable(foreground[2],grayscale?8:5,nativeIcon)};
        const std::array<unsigned int,3> bitDepth{grayscale?8u:5u,grayscale?8u:6u,grayscale?8u:5u};
        const std::array<unsigned int,3> rawChannel{0,grayscale?0u:1u,grayscale?0u:2u};
        const bool monochromeGray=grayscale&&foreground[0]==foreground[1]&&foreground[1]==foreground[2];
        // Keep the brush product continuous until the destination byte is
        // rounded. This small cache is independent of the current RGB color.
        struct OpacityCoverage {float opacity=-1;std::array<double,256> alpha{};};
        static thread_local OpacityCoverage opacityCoverage;
        if(translucentIcon&&opacityCoverage.opacity!=color.a){
            for(unsigned int i=0;i<256;++i)opacityCoverage.alpha[i]=i*static_cast<double>(color.a);
            opacityCoverage.opacity=color.a;
        }
        const auto* masks=shortRun?inlineMasks->data():heapMasks.data();
        const auto* opacityBlends=translucentIcon?NativeOpacityTables(foreground,color.a):
            translucentText?OrdinaryOpacityTables(foreground,color.a):nullptr;
        const bool locked=LockGlyphSurface();
        if(locked){
            // Compose LCD coverage directly into the CPU surface. Font painting
            // must not upload glyph textures or synchronously read GPU pixels.
            for(size_t maskIndex=0;maskIndex<maskCount;++maskIndex){
                const auto& mask=masks[maskIndex];
                const auto& texture=*mask.texture;
                const int w=texture.bounds.right-texture.bounds.left;
                const int left=mask.left+texture.bounds.left,top=mask.top+texture.bounds.top;
                const int firstX=std::max<LONG>(0,clip.left-left),lastX=std::min<LONG>(w,clip.right-left);
                const int firstY=std::max<LONG>(0,clip.top-top),lastY=std::min<LONG>(texture.bounds.bottom-texture.bounds.top,clip.bottom-top);
                const bool fullyCovered=left+firstX>=deviceClip.left&&left+lastX<=deviceClip.right&&
                    top+firstY>=deviceClip.top&&top+lastY<=deviceClip.bottom;
                if(translucentText){
                    if(fullyCovered&&opacityBlends)
                        ComposeNativeCachedOpacity(texture,w,left,top,firstX,lastX,firstY,lastY,
                            tables[0],opacityBlends,monochromeGray);
                    else ComposeOrdinaryOpacity(texture,w,left,top,firstX,lastX,firstY,lastY,fullyCovered,
                            deviceClip,foreground,tables[0],color.a,opacityBlends,monochromeGray);
                    continue;
                }
                if(translucentIcon){
                    if(fullyCovered&&opacityBlends)
                        ComposeNativeCachedOpacity(texture,w,left,top,firstX,lastX,firstY,lastY,
                            tables[0],opacityBlends,monochromeGray);
                    else ComposeNativeOpacity(texture,w,left,top,firstX,lastX,firstY,lastY,fullyCovered,
                            deviceClip,foreground,tables[0],opacityCoverage.alpha.data(),opacityBlends,monochromeGray);
                    continue;
                }
                for(int yy=firstY;yy<lastY;++yy){
                    const int dy=top+yy;
                    const float clippedY=fullyCovered?1.f:std::max(0.f,
                        std::min(dy+1.f,deviceClip.bottom)-std::max(static_cast<float>(dy),deviceClip.top));
                    auto* row=glyphPixels+static_cast<size_t>(dy)*glyphStride;
                    const auto* maskRow=texture.alpha.data()+static_cast<size_t>(yy)*w*texture.channels;
                    if(fullyCovered){
                        if(monochromeGray){
                            // Grayscale text and native icons have one coverage.
                            // Gray backdrops also share one blend result.
                            for(int xx=firstX;xx<lastX;++xx){
                                auto* destination=row+(left+xx)*4;
                                const unsigned int alpha=tables[0][maskRow[xx]];
                                const auto* blend=blends[0]+(alpha<<8);
                                if(destination[0]==destination[1]&&destination[1]==destination[2]){
                                    const BYTE value=blend[destination[0]];
                                    destination[0]=destination[1]=destination[2]=value;
                                }else{
                                    destination[0]=blend[destination[0]];
                                    destination[1]=blend[destination[1]];
                                    destination[2]=blend[destination[2]];
                                }
                                if(destination[3]!=255)
                                    destination[3]=static_cast<BYTE>(alpha+(destination[3]*(255-alpha)+127)/255);
                            }
                            continue;
                        }
                        if(grayscale){
                            // Color text still shares one grayscale coverage.
                            // Resolve it once and use direct BGRA lookups rather
                            // than repeating channel/bit-depth work three times.
                            for(int xx=firstX;xx<lastX;++xx){
                                auto* destination=row+(left+xx)*4;
                                const unsigned int alpha=tables[0][maskRow[xx]];
                                const unsigned int lookup=alpha<<8;
                                destination[0]=blends[2][lookup|destination[0]];
                                destination[1]=blends[1][lookup|destination[1]];
                                destination[2]=blends[0][lookup|destination[2]];
                                if(destination[3]!=255)
                                    destination[3]=static_cast<BYTE>(alpha+(destination[3]*(255-alpha)+127)/255);
                            }
                            continue;
                        }
                        // Most glyphs are wholly inside the clip. Keep the same
                        // coverage and blend lookup without four clamps per pixel.
                        for(int xx=firstX;xx<lastX;++xx){
                            auto* destination=row+(left+xx)*4;
                            const auto* raw=maskRow+xx*texture.channels;
                            for(unsigned int channel=0;channel<3;++channel){
                                const unsigned int coverage=tables[channel][raw[rawChannel[channel]]]>>(8-bitDepth[channel]);
                                const unsigned int bgra=2-channel;
                                destination[bgra]=blends[channel][(coverage<<8)|destination[bgra]];
                            }
                            if((nativeIcon||grayscale)&&destination[3]!=255){
                                const unsigned int alpha=tables[0][raw[0]];
                                destination[3]=static_cast<BYTE>(alpha+(destination[3]*(255-alpha)+127)/255);
                            }else if(!grayscale)destination[3]=255;
                        }
                        continue;
                    }
                    for(int xx=firstX;xx<lastX;++xx){
                        const int dx=left+xx;
                        auto* destination=row+dx*4;
                        const auto* raw=maskRow+xx*texture.channels;
                        const float clipCoverage=clippedY*std::max(0.f,
                            std::min(dx+1.f,deviceClip.right)-std::max(static_cast<float>(dx),deviceClip.left));
                        for(unsigned int channel=0;channel<3;++channel){
                            const unsigned int bits=bitDepth[channel];
                            const unsigned int coverage=tables[channel][raw[rawChannel[channel]]]>>(8-bits);
                            const unsigned int bgra=2-channel;
                            if(clipCoverage==1){
                                destination[bgra]=blends[channel][(coverage<<8)|destination[bgra]];
                                continue;
                            }
                            const unsigned int expanded=(coverage<<(8-bits))|(coverage>>(2*bits-8));
                            const float alpha=expanded/255.0f*clipCoverage;
                            const double clippedAlpha=expanded*static_cast<double>(clipCoverage);
                            destination[bgra]=static_cast<BYTE>(nativeIcon?
                                std::lround((destination[bgra]*(255-clippedAlpha)+foreground[channel]*clippedAlpha)/255):
                                std::min(255L,std::lround(destination[bgra]*(1-alpha))+std::lround(foreground[channel]*alpha)));
                        }
                        if((nativeIcon||grayscale)&&destination[3]!=255){
                            const double alpha=tables[0][raw[0]]*static_cast<double>(clipCoverage);
                            // Ordinary text retains separate point rounding for
                            // source and backdrop, just as its RGB channels do.
                            // Native icons round their complete source-over once.
                            destination[3]=static_cast<BYTE>(nativeIcon?
                                std::lround(destination[3]*(255-alpha)/255+alpha):
                                std::min(255L,std::lround(alpha)+std::lround(destination[3]*(255-alpha)/255)));
                        }else if(!grayscale)destination[3]=255;
                    }
                }
            }
        }
        if(!glyphBatchDepth)FlushGlyphBatch();
        return true;
    }
};

inline void PushPaintClip(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,D2D1_ANTIALIAS_MODE mode){
    if(activeRasterSurface&&activeRasterSurface->target==target){
        activeRasterSurface->FlushGlyphBatch();
        D2D1_MATRIX_3X2_F transform{};target->GetTransform(&transform);
        activeRasterSurface->clips.push_back({rect,mode,transform});
        activeRasterSurface->glyphClipValid=false;
    }
    target->PushAxisAlignedClip(rect,mode);
}
inline void PopPaintClip(ID2D1RenderTarget* target){
    if(activeRasterSurface&&activeRasterSurface->target==target)activeRasterSurface->FlushGlyphBatch();
    target->PopAxisAlignedClip();
    if(activeRasterSurface&&activeRasterSurface->target==target&&!activeRasterSurface->clips.empty()){
        activeRasterSurface->clips.pop_back();activeRasterSurface->glyphClipValid=false;
    }
}
inline void PushPaintLayer(ID2D1RenderTarget* target,const D2D1_LAYER_PARAMETERS& parameters,ID2D1Layer* layer){
    if(activeRasterSurface&&activeRasterSurface->target==target){activeRasterSurface->FlushGlyphBatch();++activeRasterSurface->layers;}
    target->PushLayer(parameters,layer);
}
inline void PopPaintLayer(ID2D1RenderTarget* target){
    if(activeRasterSurface&&activeRasterSurface->target==target)activeRasterSurface->FlushGlyphBatch();
    target->PopLayer();
    if(activeRasterSurface&&activeRasterSurface->target==target&&activeRasterSurface->layers)--activeRasterSurface->layers;
}
}

#pragma once
#include <windows.h>
#include <d2d1.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace TWebFrame::Internal {
// CPU image-brush, solid corner and shadow composition. The C ABI
// and licensed runtime are pinned in vendor/skia. Inputs are engine geometry,
// brushes and the shared View surface; no reference capture is read here.
class RasterSkia {
    struct Info {void* colorspace;int width,height,color,alpha;};
    struct Matrix {float scaleX,skewX,transX,skewY,scaleY,transY,persp0,persp1,persp2;};
    struct Sampling {int maxAniso;bool useCubic;float cubicB,cubicC;int filter,mipmap;};
    HMODULE library_=nullptr;bool attempted_=false;bool loaded_=false;
    template<class T> bool Load(T& fn,const char* name){fn=reinterpret_cast<T>(GetProcAddress(library_,name));return fn!=nullptr;}
    void* (*newRasterSurface_)(const Info*,void*,size_t,void*,void*,void*)=nullptr;
    void (*unrefSurface_)(void*)=nullptr;
    void* (*canvas_)(void*)=nullptr;
    void* (*newImage_)(const Info*,const void*,size_t)=nullptr;
    void (*unrefImage_)(void*)=nullptr;
    void* (*imageShader_)(void*,int,int,const void*,const Matrix*)=nullptr;
    void (*unrefShader_)(void*)=nullptr;
    void* (*newPaint_)()=nullptr;void (*deletePaint_)(void*)=nullptr;
    void (*setAA_)(void*,bool)=nullptr;void (*setColor_)(void*,const D2D1_COLOR_F*,void*)=nullptr;
    void (*setStyle_)(void*,int)=nullptr;void (*setStrokeWidth_)(void*,float)=nullptr;
    void (*setShader_)(void*,void*)=nullptr;void (*setMask_)(void*,void*)=nullptr;
    void* (*newBlur_)(int,float,bool)=nullptr;void (*unrefMask_)(void*)=nullptr;
    void* (*newRRect_)()=nullptr;void (*deleteRRect_)(void*)=nullptr;
    void (*setRadii_)(void*,const D2D1_RECT_F*,const D2D1_POINT_2F*)=nullptr;
    void (*drawRRect_)(void*,void*,void*)=nullptr;
    void (*drawDRRect_)(void*,void*,void*,void*)=nullptr;
    void (*scale_)(void*,float,float)=nullptr;
    void (*translate_)(void*,float,float)=nullptr;
    int (*save_)(void*)=nullptr;void (*restore_)(void*)=nullptr;
    void (*clip_)(void*,const D2D1_RECT_F*,int,bool)=nullptr;
    void (*clipRRect_)(void*,void*,int,bool)=nullptr;
    bool hitPathsAttempted_=false,hitPathsLoaded_=false;
    void* (*newPathBuilder_)()=nullptr;void (*deletePathBuilder_)(void*)=nullptr;
    void (*addOval_)(void*,const D2D1_RECT_F*,int)=nullptr;
    void* (*detachPath_)(void*)=nullptr;void (*deletePath_)(void*)=nullptr;
    bool (*pathContains_)(const void*,float,float)=nullptr;
    bool InitializeRuntime(){
        if(attempted_)return loaded_;attempted_=true;
        wchar_t executable[32768]{};const DWORD length=GetModuleFileNameW(nullptr,executable,32768);
        if(!length||length>=32768)return false;
        std::wstring path(executable,length);const auto slash=path.find_last_of(L"\\/");
        if(slash==std::wstring::npos)return false;path.resize(slash+1);path+=L"libSkiaSharp.dll";
        library_=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!library_)return false;
#define SK_LOAD(member,name) if(!Load(member,name))return false
        SK_LOAD(newRasterSurface_,"sk_surface_new_raster_direct");
        SK_LOAD(unrefSurface_,"sk_surface_unref");
        SK_LOAD(canvas_,"sk_surface_get_canvas");
        SK_LOAD(newImage_,"sk_image_new_raster_copy");SK_LOAD(unrefImage_,"sk_image_unref");
        SK_LOAD(imageShader_,"sk_image_make_shader");
        SK_LOAD(unrefShader_,"sk_shader_unref");SK_LOAD(newPaint_,"sk_paint_new");
        SK_LOAD(deletePaint_,"sk_paint_delete");SK_LOAD(setAA_,"sk_paint_set_antialias");
        SK_LOAD(setStyle_,"sk_paint_set_style");SK_LOAD(setStrokeWidth_,"sk_paint_set_stroke_width");
        SK_LOAD(setColor_,"sk_paint_set_color4f");SK_LOAD(setShader_,"sk_paint_set_shader");
        SK_LOAD(setMask_,"sk_paint_set_maskfilter");SK_LOAD(newBlur_,"sk_maskfilter_new_blur_with_flags");
        SK_LOAD(unrefMask_,"sk_maskfilter_unref");SK_LOAD(newRRect_,"sk_rrect_new");
        SK_LOAD(deleteRRect_,"sk_rrect_delete");SK_LOAD(setRadii_,"sk_rrect_set_rect_radii");
        SK_LOAD(drawRRect_,"sk_canvas_draw_rrect");SK_LOAD(drawDRRect_,"sk_canvas_draw_drrect");
        SK_LOAD(scale_,"sk_canvas_scale");SK_LOAD(translate_,"sk_canvas_translate");
        SK_LOAD(save_,"sk_canvas_save");SK_LOAD(restore_,"sk_canvas_restore");
        SK_LOAD(clip_,"sk_canvas_clip_rect_with_operation");
        SK_LOAD(clipRRect_,"sk_canvas_clip_rrect_with_operation");
#undef SK_LOAD
        loaded_=true;return true;
    }
public:
    RasterSkia()=default;RasterSkia(const RasterSkia&)=delete;RasterSkia& operator=(const RasterSkia&)=delete;
    ~RasterSkia(){if(library_)FreeLibrary(library_);}
    bool EllipseContains(const D2D1_RECT_F& rect,const D2D1_POINT_2F& point,bool& contains){
        if(!std::isfinite(rect.left)||!std::isfinite(rect.top)||
           !std::isfinite(rect.right)||!std::isfinite(rect.bottom)||
           !std::isfinite(point.x)||!std::isfinite(point.y)||
           rect.right<=rect.left||rect.bottom<=rect.top||!InitializeRuntime())return false;
        // Keep optional hit-test exports independent of CPU painting support.
        if(!hitPathsAttempted_){
            hitPathsAttempted_=true;
            hitPathsLoaded_=Load(newPathBuilder_,"sk_pathbuilder_new")&&
                Load(deletePathBuilder_,"sk_pathbuilder_delete")&&
                Load(addOval_,"sk_pathbuilder_add_oval")&&
                Load(detachPath_,"sk_pathbuilder_detach_path")&&
                Load(deletePath_,"sk_path_delete")&&Load(pathContains_,"sk_path_contains");
        }
        if(!hitPathsLoaded_)return false;
        void* builder=newPathBuilder_();if(!builder)return false;
        addOval_(builder,&rect,0);
        void* path=detachPath_(builder);deletePathBuilder_(builder);
        if(!path)return false;
        contains=pathContains_(path,point.x,point.y);deletePath_(path);return true;
    }
    bool RoundedRect(BYTE* pixels,UINT stride,UINT width,UINT height,const D2D1_RECT_F& cssRect,
        const std::array<D2D1_POINT_2F,4>& cssRadii,unsigned int color,float sx,float sy,
        const D2D1_POINT_2F& translation,const D2D1_RECT_F& clip,float sigma=0,
        const D2D1_RECT_F* inner=nullptr,const std::array<D2D1_POINT_2F,4>* innerRadii=nullptr,
        const std::vector<BYTE>* brushPixels=nullptr,UINT brushWidth=0,UINT brushHeight=0,const D2D1_POINT_2F* brushOrigin=nullptr,
        const D2D1_RECT_F* exclusion=nullptr,const std::array<D2D1_POINT_2F,4>* exclusionRadii=nullptr,
        const D2D1_POINT_2F* shadowOffset=nullptr){
        if(!pixels||!width||!height||width>8192||height>8192||sx<=0||sy<=0||!InitializeRuntime())return false;
        auto rect=cssRect;auto radii=cssRadii;
        D2D1_RECT_F deviceInner{};std::array<D2D1_POINT_2F,4> deviceInnerRadii{};
        D2D1_POINT_2F deviceBrushOrigin{};
        if(sigma==0&&sx==sy){
            const auto device=[](float coordinate,float scale){
                const float value=coordinate*scale,half=std::round(value*2)/2;
                const float tolerance=std::numeric_limits<float>::epsilon()*std::max(1.0f,std::abs(value))*2;
                return std::abs(value-half)<=tolerance?half:value;
            };
            const auto mapRect=[&](D2D1_RECT_F box){return D2D1::RectF(device(box.left,sx),device(box.top,sy),device(box.right,sx),device(box.bottom,sy));};
            rect=mapRect(rect);for(auto& corner:radii){corner.x*=sx;corner.y*=sy;}
            if(inner&&innerRadii){deviceInner=mapRect(*inner);deviceInnerRadii=*innerRadii;
                for(auto& corner:deviceInnerRadii){corner.x*=sx;corner.y*=sy;}inner=&deviceInner;innerRadii=&deviceInnerRadii;}
            if(brushOrigin){deviceBrushOrigin={brushOrigin->x*sx,brushOrigin->y*sy};brushOrigin=&deviceBrushOrigin;}
            sx=sy=1;
        }
        // The caller holds the CPU bitmap lock until this surface is released.
        // Prepare all resources before drawing, then paint into those pixels;
        // copying the entire viewport twice for each small rounded box is costly.
        const Info info{nullptr,static_cast<int>(width),static_cast<int>(height),6,1};
        void* surface=newRasterSurface_(&info,pixels,stride,nullptr,nullptr,nullptr);
        void* paint=newPaint_();void* outer=newRRect_();
        void* inside=inner?newRRect_():nullptr;void* mask=sigma>0?newBlur_(0,sigma,true):nullptr;
        void* excluded=exclusion?newRRect_():nullptr;
        void* image=nullptr;void* shader=nullptr;
        bool ok=surface&&paint&&outer&&(!inner||inside)&&(!sigma||mask)&&(!exclusion||excluded);
        const Sampling nearest{};
        bool stroke=false;
        if(ok){
            auto* canvas=canvas_(surface);
            clip_(canvas,&clip,1,false);setAA_(paint,true);
            const D2D1_COLOR_F rgba{((color>>16)&255)/255.f,((color>>8)&255)/255.f,(color&255)/255.f,(color>>24)/255.f};
            setColor_(paint,&rgba,nullptr);
            setRadii_(outer,&rect,radii.data());if(inside)setRadii_(inside,inner,innerRadii->data());
            if(excluded)setRadii_(excluded,exclusion,exclusionRadii->data());
            if(inside){
                const float borderWidth=inner->left-rect.left;
                stroke=borderWidth>0&&std::abs(inner->top-rect.top-borderWidth)<.0001f&&
                    std::abs(rect.right-inner->right-borderWidth)<.0001f&&std::abs(rect.bottom-inner->bottom-borderWidth)<.0001f;
                for(size_t k=0;k<4&&stroke;++k){
                    const auto a=radii[k],b=(*innerRadii)[k];
                    stroke=(a.x==0&&a.y==0&&b.x==0&&b.y==0)||
                        (a.x==a.y&&b.x==b.y&&std::abs(a.x-b.x-borderWidth)<.0001f);
                }
                if(stroke){
                    const float inset=borderWidth/2;
                    const auto center=D2D1::RectF(rect.left+inset,rect.top+inset,rect.right-inset,rect.bottom-inset);
                    auto corners=radii;for(auto& corner:corners){corner.x=std::max(0.0f,corner.x-inset);corner.y=std::max(0.0f,corner.y-inset);}
                    setRadii_(outer,&center,corners.data());setStyle_(paint,1);setStrokeWidth_(paint,borderWidth);
                }
            }
            if(mask)setMask_(paint,mask);
            if(brushPixels){
                const Info brushInfo{nullptr,static_cast<int>(brushWidth),static_cast<int>(brushHeight),6,1};
                image=newImage_(&brushInfo,brushPixels->data(),brushWidth*4);
                const Matrix matrix{1/sx,0,brushOrigin?brushOrigin->x:rect.left,0,1/sy,brushOrigin?brushOrigin->y:rect.top,0,0,1};
                if(image)shader=imageShader_(image,0,0,&nearest,&matrix);
                ok=shader!=nullptr;if(shader)setShader_(paint,shader);
            }
            if(ok){
                save_(canvas);
                translate_(canvas,translation.x,translation.y);scale_(canvas,sx,sy);
                if(excluded)clipRRect_(canvas,excluded,0,true);
                if(shadowOffset)translate_(canvas,shadowOffset->x,shadowOffset->y);
                if(inside&&!stroke)drawDRRect_(canvas,outer,inside,paint);else drawRRect_(canvas,outer,paint);
                restore_(canvas);
            }
        }
        if(shader)unrefShader_(shader);if(image)unrefImage_(image);if(mask)unrefMask_(mask);
        if(excluded)deleteRRect_(excluded);
        if(inside)deleteRRect_(inside);if(outer)deleteRRect_(outer);if(paint)deletePaint_(paint);
        if(surface)unrefSurface_(surface);return ok;
    }
};
inline thread_local RasterSkia rasterSkia;
}

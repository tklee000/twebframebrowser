#pragma once
#include <d2d1.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace TWebFrame::Internal {

inline bool TriangulateSimplePolygon(const std::vector<D2D1_POINT_2F>& points,
                                    std::vector<D2D1_TRIANGLE>& triangles){
    if(points.size()<3||points.size()>8192)return false;
    const auto cross=[](D2D1_POINT_2F a,D2D1_POINT_2F b,D2D1_POINT_2F c){
        return (static_cast<double>(b.x)-a.x)*(static_cast<double>(c.y)-a.y)-
               (static_cast<double>(b.y)-a.y)*(static_cast<double>(c.x)-a.x);
    };
    double area=0;for(size_t i=0;i<points.size();++i){const auto a=points[i],b=points[(i+1)%points.size()];area+=static_cast<double>(a.x)*b.y-static_cast<double>(a.y)*b.x;}
    if(area==0)return false;const double sign=area>0?1:-1;
    std::vector<size_t> remaining(points.size());for(size_t i=0;i<remaining.size();++i)remaining[i]=i;
    std::vector<D2D1_TRIANGLE> mesh;mesh.reserve(points.size()-2);
    while(remaining.size()>3){
        bool found=false;
        for(size_t i=0;i<remaining.size();++i){
            const auto a=points[remaining[(i+remaining.size()-1)%remaining.size()]],b=points[remaining[i]],c=points[remaining[(i+1)%remaining.size()]];
            if(cross(a,b,c)*sign<=0)continue;
            bool contains=false;
            for(size_t j=0;j<remaining.size();++j){
                if(j==i||j==(i+1)%remaining.size()||j==(i+remaining.size()-1)%remaining.size())continue;
                const auto p=points[remaining[j]];
                if(cross(a,b,p)*sign>=0&&cross(b,c,p)*sign>=0&&cross(c,a,p)*sign>=0){contains=true;break;}
            }
            if(contains)continue;mesh.push_back({a,b,c});remaining.erase(remaining.begin()+i);found=true;break;
        }
        if(!found)return false;
    }
    mesh.push_back({points[remaining[0]],points[remaining[1]],points[remaining[2]]});triangles=std::move(mesh);return true;
}

// Construct straight miter/butt outlines in float path coordinates. Keeping
// these vertices independent of Direct2D's widening tolerance makes the MSAA
// mask deterministic at sample locations exactly on a stroke boundary.
inline Microsoft::WRL::ComPtr<ID2D1PathGeometry> LinearStrokeOutline(
    ID2D1Geometry* geometry,float width,ID2D1StrokeStyle* style,
    std::vector<D2D1_TRIANGLE>* triangles=nullptr){
    using Microsoft::WRL::ComPtr;
    if(style&&(style->GetStartCap()!=D2D1_CAP_STYLE_FLAT||style->GetEndCap()!=D2D1_CAP_STYLE_FLAT||
        (style->GetLineJoin()!=D2D1_LINE_JOIN_MITER&&style->GetLineJoin()!=D2D1_LINE_JOIN_MITER_OR_BEVEL)||
        style->GetDashStyle()!=D2D1_DASH_STYLE_SOLID))return {};
    struct Lines final:ID2D1SimplifiedGeometrySink{
        std::vector<D2D1_POINT_2F> points;unsigned int figures=0;bool supported=true;ULONG references=1;
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override{
            if(!result)return E_POINTER;*result=nullptr;
            if(iid==__uuidof(IUnknown)||iid==__uuidof(ID2D1SimplifiedGeometrySink)){*result=this;AddRef();return S_OK;}
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
        ULONG STDMETHODCALLTYPE Release() override{return --references;}
        void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override{}
        void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT flags) override{if(flags!=D2D1_PATH_SEGMENT_NONE)supported=false;}
        void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F point,D2D1_FIGURE_BEGIN) override{++figures;points.push_back(point);}
        void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* values,UINT count) override{
            if(points.size()+count>65536){supported=false;return;}points.insert(points.end(),values,values+count);
        }
        void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT*,UINT) override{supported=false;}
        void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END end) override{if(end!=D2D1_FIGURE_END_OPEN)supported=false;}
        HRESULT STDMETHODCALLTYPE Close() override{return S_OK;}
    } lines;
    if(FAILED(geometry->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES,nullptr,0.01f,&lines))||
        !lines.supported||lines.figures!=1||lines.points.size()<2)return {};
    const auto normalize=[](D2D1_POINT_2F value,float length){
        const double magnitude=std::hypot(static_cast<double>(value.x),static_cast<double>(value.y));
        const double scale=magnitude>0?length/magnitude:0;
        return D2D1::Point2F(static_cast<float>(value.x*scale),static_cast<float>(value.y*scale));
    };
    const auto offset=[](D2D1_POINT_2F point,D2D1_POINT_2F normal,float length){
        return D2D1::Point2F(point.x+normal.x*length,point.y+normal.y*length);
    };
    std::vector<D2D1_POINT_2F> normals;normals.reserve(lines.points.size()-1);
    for(size_t i=1;i<lines.points.size();++i){
        const auto a=lines.points[i-1],b=lines.points[i];
        if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(b.x)||!std::isfinite(b.y)||
            std::hypot(b.x-a.x,b.y-a.y)<0.001f)return {};
        normals.push_back(normalize(D2D1::Point2F(a.y-b.y,b.x-a.x),1));
    }
    const float radius=width/2,limit=style?style->GetMiterLimit():4;
    std::vector<D2D1_POINT_2F> sides[2];
    for(unsigned int side=0;side<2;++side){
        const float sign=side==0?1.0f:-1.0f;
        auto& points=sides[side];points.push_back(offset(lines.points.front(),normals.front(),radius*sign));
        for(size_t i=1;i+1<lines.points.size();++i){
            const auto before=normals[i-1],after=normals[i],pivot=lines.points[i];
            const float dot=std::clamp(before.x*after.x+before.y*after.y,-1.0f,1.0f);
            const float halfCosine=std::sqrt((1+dot)/2);
            if(halfCosine<=0||halfCosine<1/limit)return {};
            const float cross=before.x*after.y-before.y*after.x;
            if(cross*sign>0){
                points.push_back(offset(pivot,before,radius*sign));points.push_back(pivot);
                points.push_back(offset(pivot,after,radius*sign));
            }else{
                const auto direction=dot<0?D2D1::Point2F(after.y-before.y,before.x-after.x):
                    D2D1::Point2F(before.x+after.x,before.y+after.y);
                auto bisector=dot==0?D2D1::Point2F(direction.x*radius,direction.y*radius):
                    normalize(direction,radius/halfCosine);
                if(dot<0&&cross<0){bisector.x=-bisector.x;bisector.y=-bisector.y;}
                points.push_back(offset(pivot,bisector,sign));
            }
        }
        points.push_back(offset(lines.points.back(),normals.back(),radius*sign));
    }
    std::vector<D2D1_POINT_2F> polygon=sides[0];polygon.insert(polygon.end(),sides[1].rbegin(),sides[1].rend());
    ComPtr<ID2D1Factory> factory;geometry->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> outline;ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(factory->CreatePathGeometry(&outline))||FAILED(outline->Open(&sink)))return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);sink->BeginFigure(polygon[0],D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(polygon.data()+1,static_cast<UINT>(polygon.size()-1));sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if(FAILED(sink->Close()))return {};
    if(triangles){
        // Direct2D's path tessellation snaps edges to 1/64 coordinates. Build
        // the straight stroke's union from its original float vertices so
        // MSAA sample locations retain the shaped outline's precision.
        std::vector<D2D1_TRIANGLE> mesh;mesh.reserve(normals.size()*4);
        for(size_t i=0;i<normals.size();++i){
            const auto a=offset(lines.points[i],normals[i],radius);
            const auto b=offset(lines.points[i+1],normals[i],radius);
            const auto c=offset(lines.points[i+1],normals[i],-radius);
            const auto d=offset(lines.points[i],normals[i],-radius);
            mesh.push_back({a,b,c});mesh.push_back({a,c,d});
            if(i==0)continue;
            const auto before=normals[i-1],after=normals[i],pivot=lines.points[i];
            const float cross=before.x*after.y-before.y*after.x,sign=cross>0?-1.0f:1.0f;
            const float dot=std::clamp(before.x*after.x+before.y*after.y,-1.0f,1.0f);
            const float halfCosine=std::sqrt((1+dot)/2);
            const auto direction=dot<0?D2D1::Point2F(after.y-before.y,before.x-after.x):
                D2D1::Point2F(before.x+after.x,before.y+after.y);
            auto miter=dot==0?D2D1::Point2F(direction.x*radius,direction.y*radius):
                normalize(direction,radius/halfCosine);
            if(dot<0&&cross<0){miter.x=-miter.x;miter.y=-miter.y;}
            const auto p=offset(pivot,before,radius*sign),q=offset(pivot,after,radius*sign);
            mesh.push_back({p,offset(pivot,miter,sign),q});mesh.push_back({p,q,pivot});
        }
        if(!TriangulateSimplePolygon(polygon,*triangles))*triangles=std::move(mesh);
    }
    return outline;
}
}

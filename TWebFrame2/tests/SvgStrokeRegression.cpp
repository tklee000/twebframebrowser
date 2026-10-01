#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace TWebFrame::Internal;
using Microsoft::WRL::ComPtr;
namespace {
int failures=0;
void Check(bool condition,const wchar_t* message){
    if(!condition){++failures;std::wcerr<<L"FAIL: "<<message<<L'\n';}
}
void Raster(Document& document,StyleSheet& styles,float scale,const std::filesystem::path& output,bool gridFixture){
    const UINT width=static_cast<UINT>(600*scale),height=static_cast<UINT>(420*scale);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> write;
    const bool ready=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))&&
        SUCCEEDED(wic->CreateBitmap(width,height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap))&&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,d2d.ReleaseAndGetAddressOf()))&&
        SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),
            96*scale,96*scale),&target))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write.ReleaseAndGetAddressOf())));
    Check(ready,L"SVG raster resources are available");if(!ready)return;
    LayoutEngine layout(document,styles);layout.Layout(600,420,scale);
    target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::White));layout.Paint(target.Get(),write.Get());
    Check(SUCCEEDED(target->EndDraw()),L"SVG renders at the requested device DPI");
    {
        WICRect bounds{0,0,static_cast<INT>(width),static_cast<INT>(height)};
        ComPtr<IWICBitmapLock> lock;Check(SUCCEEDED(bitmap->Lock(&bounds,WICBitmapLockRead,&lock)),L"SVG raster is readable");
        if(!lock)return;UINT stride=0,size=0;BYTE* bytes=nullptr;
        lock->GetStride(&stride);lock->GetDataPointer(&size,&bytes);if(!bytes)return;
        const auto ink=[&](float x,float y){
            const auto* p=bytes+static_cast<UINT>(std::lround(y*scale))*stride+static_cast<UINT>(std::lround(x*scale))*4;
            return p[0]<160&&p[2]<160;
        };
        if(gridFixture){
            for(const auto* id:{L"unchecked-box",L"checked-box",L"narrow-box"}){
                const auto* box=layout.BoxFor(document.GetElementById(id));
                Check(box&&std::abs(box->rect.width-24)<.01f&&std::abs(box->rect.height-24)<.01f,
                    L"grid items preserve their authored square dimensions in wide and narrow columns");
            }
            const auto* checked=layout.BoxFor(document.GetElementById(L"checked-box"));
            bool foundPseudo=false;
            if(checked)for(const auto& child:checked->children)if(child->pseudo==L"after"){
                foundPseudo=true;
                Check(std::abs(child->rect.width-10)<.01f&&std::abs(child->rect.height-16)<.01f,
                    L"absolutely positioned content-box pseudo elements include their border thickness");
            }
            Check(foundPseudo,L"the checked decoration has a generated pseudo box");
            size_t orangePixels=0;int minX=static_cast<int>(width),maxX=0,minY=static_cast<int>(height),maxY=0;
            for(UINT y=static_cast<UINT>(128*scale);y<static_cast<UINT>(163*scale);++y)
                for(UINT x=static_cast<UINT>(28*scale);x<static_cast<UINT>(62*scale);++x){
                    const auto* p=bytes+y*stride+x*4;
                    if(p[2]>130&&p[1]<140&&p[0]<80){
                        ++orangePixels;minX=std::min(minX,static_cast<int>(x));maxX=std::max(maxX,static_cast<int>(x));
                        minY=std::min(minY,static_cast<int>(y));maxY=std::max(maxY,static_cast<int>(y));
                    }
                }
            Check(orangePixels>25*scale*scale&&maxX-minX>10*scale&&maxY-minY>8*scale,
                L"the rotated asymmetric border paints a complete check shape at both DPIs");
        }else{
        Check(ink(40,25)&&ink(48,35)&&ink(40,45)&&ink(30,35),L"individual rotations and translated SVG groups preserve line directions");
        Check(!ink(55,42)&&!ink(40,57),L"transformed lines do not paint at their untransformed endpoints");
        Check(ink(25,90)&&!ink(35,90)&&ink(45,90)&&ink(25,110)&&!ink(35,110),
            L"inherited dash lengths and positive offsets use SVG units independent of stroke width");
        Check(ink(25,140)&&!ink(35,140)&&ink(42,140)&&!ink(50,140)&&ink(60,140),L"odd dash lists repeat to form an even pattern");
        Check(!ink(21,170)&&ink(31,170)&&!ink(42,170),L"negative dash offsets wrap through the complete pattern");
        Check(ink(25,200)&&ink(35,200)&&ink(90,200),L"all-zero dash lists produce a solid stroke");
        Check(ink(25,230)&&!ink(35,230)&&ink(45,230),L"CSS declarations override SVG presentation attributes");
        Check(ink(20,260)&&!ink(26,260)&&ink(32,260),L"zero-length dashes with round caps render separated dots");
        Check(ink(320,235)&&!ink(350,200),L"nested SVG transforms compose in author order");
        Check(ink(70,300)&&!ink(15,0),L"use translation composes with its transform and definitions remain unpainted");
        Check(ink(350,340)&&!ink(430,340),L"nonzero and evenodd SVG fill rules preserve their distinct interiors");
        }
    }
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    bool saved=SUCCEEDED(wic->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(output.c_str(),GENERIC_WRITE))&&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&
        SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))&&SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&
        SUCCEEDED(frame->Initialize(nullptr))&&SUCCEEDED(frame->SetSize(width,height));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppPBGRA;
    if(saved)saved=SUCCEEDED(frame->SetPixelFormat(&format))&&SUCCEEDED(frame->WriteSource(bitmap.Get(),nullptr))&&
        SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit());
    Check(saved,L"SVG raster artifact is saved");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3&&argc!=4)return 2;
    const bool gridFixture=argc==4&&std::wstring(argv[3])==L"grid";
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    std::ifstream source(std::filesystem::path(argv[1]),std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(source)),std::istreambuf_iterator<char>());
    std::wstring html(bytes.begin(),bytes.end());Document document;StyleSheet styles;std::wstring error;
    Check(document.Parse(html,&error)&&styles.Parse(document.StyleText(),&error),L"generic SVG fixture parses");
    const std::filesystem::path output=argv[2];std::filesystem::create_directories(output);
    Raster(document,styles,1,output/(gridFixture?L"grid-100.png":L"svg-100.png"),gridFixture);
    Raster(document,styles,1.5f,output/(gridFixture?L"grid-150.png":L"svg-150.png"),gridFixture);
    CoUninitialize();std::wcout<<L"SVG stroke regression failures="<<failures<<L'\n';return failures?1:0;
}

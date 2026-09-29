#include <TWebFrame/TWebFrame.h>

#include "CSS.h"
#include "DOM.h"
#include "Layout.h"
#include "RasterImage.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <objidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace TWebFrame::Internal;
using Microsoft::WRL::ComPtr;

namespace {

int failures=0;

void Check(bool condition,const wchar_t* message){
    if(!condition){std::wcerr<<L"FAIL: "<<message<<L'\n';++failures;}
}

bool Near(unsigned char actual,unsigned char expected,unsigned char tolerance=28){
    return std::abs(static_cast<int>(actual)-static_cast<int>(expected))<=tolerance;
}

std::vector<unsigned char> Encode(const GUID& container,const GUID& requestedFormat,
                                  UINT width,UINT height,UINT stride,
                                  const std::vector<unsigned char>& pixels,
                                  unsigned short orientation=1){
    std::vector<unsigned char> result;
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;
    ComPtr<IStream> stream;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))||FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream))||
       FAILED(factory->CreateEncoder(container,nullptr,&encoder))||
       FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))||
       FAILED(encoder->CreateNewFrame(&frame,&properties))||FAILED(frame->Initialize(properties.Get()))||
       FAILED(frame->SetSize(width,height))||FAILED(frame->SetResolution(96,96)))return result;
    GUID format=requestedFormat;
    if(FAILED(frame->SetPixelFormat(&format))||format!=requestedFormat)return result;
    if(orientation!=1){
        ComPtr<IWICMetadataQueryWriter> writer;PROPVARIANT value{};
        value.vt=VT_UI2;value.uiVal=orientation;
        if(FAILED(frame->GetMetadataQueryWriter(&writer))||
           FAILED(writer->SetMetadataByName(L"/app1/ifd/{ushort=274}",&value)))return result;
    }
    if(
       FAILED(frame->WritePixels(height,stride,static_cast<UINT>(pixels.size()),
                                 const_cast<BYTE*>(pixels.data())))||
       FAILED(frame->Commit())||FAILED(encoder->Commit()))return result;
    LARGE_INTEGER zero{};ULARGE_INTEGER position{};
    if(FAILED(stream->Seek(zero,STREAM_SEEK_SET,&position)))return result;
    STATSTG status{};if(FAILED(stream->Stat(&status,STATFLAG_NONAME))||
       status.cbSize.QuadPart>static_cast<ULONGLONG>(SIZE_MAX))return result;
    result.resize(static_cast<size_t>(status.cbSize.QuadPart));ULONG read=0;
    if(FAILED(stream->Read(result.data(),static_cast<ULONG>(result.size()),&read))||
       read!=result.size())result.clear();
    return result;
}

struct Pixel { unsigned char blue=0,green=0,red=0,alpha=0; };
struct PaintedImage {
    UINT width=0,height=0,stride=0;std::vector<unsigned char> bytes;
    Pixel At(float x,float y,float scale)const{
        const auto px=std::min(width-1,static_cast<UINT>(std::floor(x*scale)));
        const auto py=std::min(height-1,static_cast<UINT>(std::floor(y*scale)));
        const auto* value=bytes.data()+static_cast<size_t>(py)*stride+px*4;
        return {value[0],value[1],value[2],value[3]};
    }
};

PaintedImage Paint(LayoutEngine& layout,float scale,float cssWidth=24,float cssHeight=24){
    PaintedImage result;result.width=static_cast<UINT>(std::lround(cssWidth*scale));
    result.height=static_cast<UINT>(std::lround(cssHeight*scale));
    ComPtr<IWICImagingFactory> wic;ComPtr<IWICBitmap> surface;
    ComPtr<ID2D1Factory> d2d;ComPtr<ID2D1RenderTarget> target;ComPtr<IDWriteFactory> write;
    const bool created=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,
        CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))&&
        SUCCEEDED(wic->CreateBitmap(result.width,result.height,GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad,&surface))&&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    d2d.ReleaseAndGetAddressOf()))&&
        SUCCEEDED(d2d->CreateWicBitmapRenderTarget(surface.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),
                USER_DEFAULT_SCREEN_DPI*scale,USER_DEFAULT_SCREEN_DPI*scale),&target))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(write.ReleaseAndGetAddressOf())));
    Check(created,L"image raster target is created");if(!created)return result;
    target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::White));
    layout.Paint(target.Get(),write.Get());Check(SUCCEEDED(target->EndDraw()),L"image is painted");
    WICRect rectangle{0,0,static_cast<INT>(result.width),static_cast<INT>(result.height)};
    ComPtr<IWICBitmapLock> lock;Check(SUCCEEDED(surface->Lock(&rectangle,WICBitmapLockRead,&lock)),
        L"image pixels are readable");if(!lock)return result;
    UINT size=0;BYTE* bytes=nullptr;
    Check(SUCCEEDED(lock->GetStride(&result.stride))&&
          SUCCEEDED(lock->GetDataPointer(&size,&bytes))&&bytes,L"image pixel buffer is available");
    if(bytes)result.bytes.assign(bytes,bytes+size);return result;
}

void CheckScale(float scale,const std::shared_ptr<RasterImage>& image){
    Document document;std::wstring error;
    Check(document.Parse(L"<style>html,body{margin:0;padding:0;background:#fff}img{display:block;width:20px;height:20px}</style><img id='picture'>",&error),
          L"image layout fixture parses");
    auto node=document.QuerySelector(L"#picture");node->image=image;node->imageComplete=true;
    StyleSheet style;Check(style.Parse(document.StyleText(),&error),L"image CSS parses");
    LayoutEngine layout(document,style);layout.Layout(24,24,scale);
    const auto* box=layout.BoxFor(node);
    Check(box&&std::lround(box->rect.width)==20&&std::lround(box->rect.height)==20,
          scale>1?L"150 percent keeps image CSS dimensions":L"100 percent keeps image CSS dimensions");
    const auto raster=Paint(layout,scale);if(raster.bytes.empty())return;
    const auto opaque=raster.At(5,5,scale),half=raster.At(15,5,scale),quarter=raster.At(5,15,scale);
    Check(Near(opaque.red,255)&&Near(opaque.green,0)&&Near(opaque.blue,0)&&opaque.alpha>245,
          L"opaque PNG pixel is preserved");
    Check(Near(half.red,127)&&Near(half.green,255)&&Near(half.blue,127)&&half.alpha>245,
          L"half-transparent PNG pixel is composited correctly");
    Check(Near(quarter.red,191)&&Near(quarter.green,191)&&Near(quarter.blue,255)&&quarter.alpha>245,
          L"quarter-alpha PNG pixel is composited correctly");
}

void CheckBackgroundScale(float scale,const std::shared_ptr<RasterImage>& image){
    Document document;std::wstring error;
    Check(document.Parse(
        L"<style>html,body{box-sizing:border-box;margin:0;padding:0}"
        L"body{background:#fff url(\"assets/alpha.png\") 0 0/20px 20px no-repeat}</style><body></body>",
        &error),L"CSS background image fixture parses");
    StyleSheet style;Check(style.Parse(document.StyleText(),&error),L"background image CSS parses");
    const auto body=document.QuerySelector(L"body");const auto computed=style.Compute(body);
    Check(computed.Get(L"background-image")==L"url(\"assets/alpha.png\")"&&
          computed.Get(L"background-position")==L"0 0"&&
          computed.Get(L"background-size")==L"20px 20px"&&
          computed.Get(L"background-repeat")==L"no-repeat",
          L"background shorthand exposes canonical image longhands");
    LayoutEngine layout(document,style);std::wstring requested;
    layout.SetRasterImageResolver([&](const std::wstring& reference){
        requested=reference;return reference==L"assets/alpha.png"?image:std::shared_ptr<RasterImage>{};
    });
    layout.Layout(24,24,scale);const auto raster=Paint(layout,scale);
    Check(requested==L"assets/alpha.png",L"layout resolves a CSS image URL through the shared image resolver");
    if(raster.bytes.empty())return;
    const auto opaque=raster.At(5,5,scale),half=raster.At(15,5,scale);
    const auto quarter=raster.At(5,15,scale),outside=raster.At(22,22,scale);
    Check(Near(opaque.red,255)&&Near(opaque.green,0)&&Near(opaque.blue,0),
          scale>1?L"150 percent paints CSS background pixels once":L"100 percent paints CSS background pixels 1:1");
    Check(Near(half.red,127)&&Near(half.green,255)&&Near(half.blue,127)&&
          Near(quarter.red,191)&&Near(quarter.green,191)&&Near(quarter.blue,255),
          L"CSS background images preserve premultiplied alpha");
    Check(outside.red>245&&outside.green>245&&outside.blue>245,
          L"background no-repeat and authored size are respected");
}

void CheckResponsiveImageScale(float scale){
    auto image=std::make_shared<RasterImage>();image->width=1908;image->height=1026;
    auto measure=[&](float hostWidth,float viewportWidth,float expectedWidth,
                     const wchar_t* widthMessage){
        Document document;std::wstring error;
        Check(document.Parse(
            L"<style>*{box-sizing:border-box;margin:0;padding:0}"
            L"#host{display:block;width:"+std::to_wstring(static_cast<int>(hostWidth))+L"px}"
            L"#picture{max-width:100%;height:auto}</style>"
            L"<div id='host'><img id='picture'></div>",&error),
            L"responsive image fixture parses");
        const auto picture=document.QuerySelector(L"#picture");picture->image=image;
        picture->imageComplete=true;
        StyleSheet style;Check(style.Parse(document.StyleText(),&error),
            L"responsive image CSS parses");
        LayoutEngine layout(document,style);layout.Layout(viewportWidth,900,scale);
        const auto* box=layout.BoxFor(picture);
        const float expectedHeight=expectedWidth*1026.0f/1908.0f;
        Check(box&&std::fabs(box->rect.width-expectedWidth)<0.75f,widthMessage);
        Check(box&&std::fabs(box->rect.height-expectedHeight)<0.75f,
            scale>1?L"150 percent preserves the responsive image aspect ratio":
                    L"100 percent preserves the responsive image aspect ratio");
    };
    measure(1200,1300,1200,
        scale>1?L"150 percent resolves image max-width from its CSS container":
                L"100 percent resolves image max-width from its CSS container");
    measure(2200,2300,1908,L"max-width does not enlarge an image beyond its intrinsic width");
}

void CheckInlineImageIntrinsicHeight(float scale){
    auto image=std::make_shared<RasterImage>();image->width=13;image->height=16;
    Document document;std::wstring error;
    Check(document.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}"
        L"#notice{display:inline-block;max-width:455px;white-space:nowrap;line-height:20px}"
        L"#icon{vertical-align:middle}</style>"
        L"<a id='notice'><img id='icon'> Board notice title</a>",&error),
        L"inline image height fixture parses");
    const auto icon=document.QuerySelector(L"#icon");icon->image=image;icon->imageComplete=true;
    StyleSheet style;Check(style.Parse(document.StyleText(),&error),
        L"inline image height CSS parses");
    LayoutEngine layout(document,style);layout.Layout(900,120,scale);
    const auto* iconBox=layout.BoxFor(icon);
    const auto* noticeBox=layout.BoxFor(document.QuerySelector(L"#notice"));
    Check(iconBox&&std::fabs(iconBox->rect.width-13.0f)<0.75f&&
          std::fabs(iconBox->rect.height-16.0f)<0.75f,
          scale>1?L"150 percent keeps an inline icon at its intrinsic dimensions":
                  L"100 percent keeps an inline icon at its intrinsic dimensions");
    Check(noticeBox&&noticeBox->rect.height>=16.0f&&noticeBox->rect.height<32.0f,
          scale>1?L"150 percent keeps a notice link at one line of height":
                  L"100 percent keeps a notice link at one line of height");
}

void CheckFloatedPopupScale(float scale){
    Document document;std::wstring error;
    Check(document.Parse(
        L"<style>*{box-sizing:border-box;margin:0;padding:0}"
        L".nav{position:relative;width:320px;height:38px;overflow:visible;background:#333}"
        L".entry{position:relative;width:100px;height:38px}"
        L".popup{position:absolute;left:0;top:38px;width:206px;height:auto;overflow:hidden;"
        L"padding:3px 2px;border:2px solid #666;background:#f02010;z-index:100;line-height:1.43}"
        L".popup *{box-sizing:border-box;line-height:1.43}.popup ul{float:left;width:50%;height:inherit;list-style:none}"
        L".popup li{display:block;height:auto}.popup li.title{width:90%;padding:2px 0 2px 4px;font-size:11px}"
        L".popup li a{display:block;padding:2px 0 1px 5px;font-size:13px;white-space:nowrap}"
        L".content{height:350px;background:#1030d0}"
        L"</style><nav class='nav'><div class='entry'><div id='popup' class='popup'>"
        L"<ul id='left'><li class='title'>Left</li><li><a>one</a></li><li><a>two</a></li>"
        L"<li><a>three</a></li><li><a>four</a></li><li><a>five</a></li><li><a>six</a></li>"
        L"<li><a>seven</a></li><li><a>eight</a></li><li><a>nine</a></li><li><a>ten</a></li>"
        L"<li><a>eleven</a></li><li id='left-last'><a>twelve</a></li></ul>"
        L"<ul id='right'><li class='title'>Right</li><li><a>one</a></li><li><a>two</a></li>"
        L"<li><a>three</a></li><li><a>four</a></li><li><a>five</a></li><li><a>six</a></li>"
        L"<li><a>seven</a></li><li><a>eight</a></li><li><a>nine</a></li><li><a>ten</a></li>"
        L"<li><a>eleven</a></li><li><a>twelve</a></li></ul>"
        L"</div></div></nav><main class='content'></main>",&error),
        L"floated popup fixture parses");
    StyleSheet style;Check(style.Parse(document.StyleText(),&error),L"floated popup CSS parses");
    LayoutEngine layout(document,style);layout.Layout(320,200,scale);
    const auto popup=document.QuerySelector(L"#popup");
    const auto left=document.QuerySelector(L"#left");
    const auto right=document.QuerySelector(L"#right");
    const auto* popupBox=layout.BoxFor(popup);
    const auto* leftBox=layout.BoxFor(left);
    const auto* rightBox=layout.BoxFor(right);
    const auto* lastBox=layout.BoxFor(document.QuerySelector(L"#left-last"));
    Check(popupBox&&std::fabs(popupBox->rect.width-206.0f)<0.01f&&
          popupBox->rect.height>270.0f&&popupBox->rect.height<310.0f,
          scale>1?L"150 percent keeps an auto-height floated popup at CSS geometry":
                  L"100 percent keeps an auto-height floated popup at CSS geometry");
    Check(leftBox&&rightBox&&std::fabs(leftBox->rect.y-rightBox->rect.y)<0.01f&&
          std::fabs(leftBox->rect.x+leftBox->rect.width-rightBox->rect.x)<0.01f,
          L"left floats form adjacent popup columns instead of vertical blocks");
    Check(popupBox&&lastBox&&lastBox->rect.y+lastBox->rect.height<=
          popupBox->content.y+popupBox->content.height+0.01f,
          L"an overflow-clipping auto-height popup encloses every floated menu row");
    const auto raster=Paint(layout,scale,320,400);if(raster.bytes.empty())return;
    const auto overlap=raster.At(50,60,scale);
    Check(overlap.red>220&&overlap.green<80&&overlap.blue<60,
          scale>1?L"150 percent paints a z-index popup above following content":
                  L"100 percent paints a z-index popup above following content");
}

void CheckDialogBackdropScale(float scale){
    Document document;std::wstring error;
    Check(document.Parse(
        L"<style>*{box-sizing:border-box}html,body{margin:0;background:#fff}.modal{width:40px;height:20px;"
        L"padding:0;border:1px solid #123456;border-radius:6px;overflow:hidden;background:#f00;"
        L"box-shadow:0 8px 24px rgba(0,0,0,.52)}"
        L".modal::backdrop{background:rgba(3,6,10,.66)}"
        L".fill{width:40px;height:20px;background:#0f0}</style>"
        L"<dialog id='modal' class='modal' open><div class='fill'></div></dialog>",&error),
        L"modal dialog backdrop fixture parses");
    const auto dialog=document.QuerySelector(L"#modal");dialog->modal=true;
    StyleSheet style;Check(style.Parse(document.StyleText(),&error),L"dialog backdrop CSS parses");
    const auto backdrop=style.Compute(dialog,nullptr,L"backdrop");
    Check(StyleSheet::Color(backdrop.Get(L"background-color"),0)==0xa803060a,
        L"author ::backdrop color participates in the pseudo-element cascade");
    Check(style.Compute(dialog).Get(L"box-shadow")==L"0 8px 24px rgba(0,0,0,.52)",
        L"CSS box-shadow is retained by the common style cascade");
    LayoutEngine layout(document,style);layout.Layout(160,120,scale);
    const auto raster=Paint(layout,scale,160,120);if(raster.bytes.empty())return;
    const auto dimmed=raster.At(5,5,scale),dialogPixel=raster.At(80,60,scale);
    const auto roundedCorner=raster.At(60,50,scale),shadowPixel=raster.At(55,65,scale);
    std::wcerr<<L"dialog scale="<<scale<<L" base="<<static_cast<int>(dimmed.red)
        <<L" shadow="<<static_cast<int>(shadowPixel.red)<<L" corner="
        <<static_cast<int>(roundedCorner.red)<<L","<<static_cast<int>(roundedCorner.green);
    for(const auto point:std::vector<std::pair<float,float>>{
        {59.0f,68.0f},{80.0f,71.0f},{80.0f,75.0f},{80.0f,80.0f}}){
        const auto sample=raster.At(point.first,point.second,scale);
        std::wcerr<<L" p"<<point.first<<L","<<point.second<<L"="<<static_cast<int>(sample.red);
    }
    std::wcerr<<L"\n";
    Check(Near(dimmed.red,89,3)&&Near(dimmed.green,91,3)&&Near(dimmed.blue,93,3),
        scale>1?L"150 percent paints the modal backdrop once across the viewport":
                L"100 percent paints the modal backdrop once across the viewport");
    Check(dialogPixel.red<3&&Near(dialogPixel.green,255,3)&&dialogPixel.blue<3,
        L"the modal dialog paints above its backdrop");
    Check(roundedCorner.green<180,
        L"rounded overflow clips descendant backgrounds at the parent's curved border edge");
    Check(shadowPixel.red+5<dimmed.red,
        L"an outer CSS box shadow remains visible outside a top-layer dialog");
    Check(layout.HitTest(5,5)==dialog,
        L"the modal backdrop blocks and retargets input from the document behind it");
}

void CheckOutlineScale(float scale){
    Document document;std::wstring error;
    Check(document.Parse(
        L"<style>html,body{margin:0;background:#fff}.field{width:40px;height:20px;"
        L"margin:15px 0 0 20px;border:1px solid #c9d0da;border-radius:4px;"
        L"background:#f7f8fa}.field:focus-visible{outline:2px solid #e55235;"
        L"outline-offset:1px}</style><div id='field' class='field'></div>",&error),
        L"focus outline fixture parses");
    const auto field=document.QuerySelector(L"#field");field->focused=true;field->focusVisible=true;
    StyleSheet style;Check(style.Parse(document.StyleText(),&error),L"focus outline CSS parses");
    const auto computed=style.Compute(field);
    Check(computed.Get(L"outline-width")==L"2px"&&
          computed.Get(L"outline-style")==L"solid"&&
          computed.Get(L"outline-color")==L"#e55235",
          L"outline shorthand resolves to reusable paint longhands");
    LayoutEngine layout(document,style);layout.Layout(80,50,scale);
    const auto* box=layout.BoxFor(field);Check(box!=nullptr,L"focus outline box is laid out");
    const auto raster=Paint(layout,scale,80,50);if(!box||raster.bytes.empty())return;
    const auto outline=raster.At(box->rect.x-2,box->rect.y+box->rect.height/2,scale);
    const auto gap=raster.At(box->rect.x-0.5f,box->rect.y+box->rect.height/2,scale);
    const auto corner=raster.At(box->rect.x-3,box->rect.y-3,scale);
    Check(Near(outline.red,229,4)&&Near(outline.green,82,4)&&Near(outline.blue,53,4),
          scale>1?L"150 percent paints the authored focus outline color":
                  L"100 percent paints the authored focus outline color");
    Check(gap.red>180&&gap.green>180&&gap.blue>180,
          L"outline-offset preserves the gap outside the border box");
    Check(corner.red>240&&corner.green>240&&corner.blue>240,
          L"focus outlines follow the element's rounded corner");
}

} // namespace

int wmain(){
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const std::vector<unsigned char> pngPixels={
        0,0,255,255, 0,255,0,128,
        255,0,0,64, 0,0,0,0
    };
    const auto pngBytes=Encode(GUID_ContainerFormatPng,GUID_WICPixelFormat32bppBGRA,
                               2,2,8,pngPixels);
    Check(!pngBytes.empty(),L"PNG fixture is encoded");
    std::wstring error;const auto png=DecodeRasterImage(pngBytes,L"alpha.png",&error);
    Check(png&&png->width==2&&png->height==2&&png->frames.size()==1,
          L"PNG dimensions and frame decode");
    if(png){
        const auto& pixels=png->frames[0].pixels;
        Check(pixels.size()==16&&pixels[7]==128&&Near(pixels[5],128,2),
              L"PNG is decoded as premultiplied BGRA without losing alpha");
        CheckScale(1.0f,png);CheckScale(1.5f,png);
        CheckBackgroundScale(1.0f,png);CheckBackgroundScale(1.5f,png);
        CheckResponsiveImageScale(1.0f);CheckResponsiveImageScale(1.5f);
        CheckInlineImageIntrinsicHeight(1.0f);CheckInlineImageIntrinsicHeight(1.5f);
        CheckFloatedPopupScale(1.0f);CheckFloatedPopupScale(1.5f);
        CheckDialogBackdropScale(1.0f);CheckDialogBackdropScale(1.5f);
        CheckOutlineScale(1.0f);CheckOutlineScale(1.5f);
    }

    {
        Document legacy;Check(legacy.Parse(L"<body background='legacy.png'></body>",&error),
            L"legacy body background fixture parses");
        StyleSheet legacyStyle;Check(legacyStyle.Parse(L"",&error),L"empty legacy stylesheet parses");
        Check(legacyStyle.Compute(legacy.QuerySelector(L"body")).Get(L"background-image")==
              L"url(\"legacy.png\")",L"body background attribute maps to the common background image rule");
    }

    const std::vector<unsigned char> jpegPixels={
        20,30,220, 20,30,220, 20,30,220,
        20,30,220, 20,30,220, 20,30,220
    };
    const auto jpegBytes=Encode(GUID_ContainerFormatJpeg,GUID_WICPixelFormat24bppBGR,
                                3,2,9,jpegPixels,6);
    const auto jpeg=DecodeRasterImage(jpegBytes,L"photo.jpg",&error);
    Check(jpeg&&jpeg->width==2&&jpeg->height==3&&jpeg->frames.size()==1&&
          jpeg->frames[0].pixels[3]==255,
          L"JPEG decodes to an opaque raster frame with EXIF orientation");

    // Two 2x1 GIF frames with transparency, 80/120 ms delays, disposal 1/2,
    // and the Netscape infinite-loop extension.
    const std::vector<unsigned char> gifBytes={
        'G','I','F','8','9','a',2,0,1,0,0xf1,0,0,
        0,0,0, 255,0,0, 0,255,0, 0,0,255,
        0x21,0xff,0x0b,'N','E','T','S','C','A','P','E','2','.','0',3,1,0,0,0,
        0x21,0xf9,4,5,8,0,0,0, 0x2c,0,0,0,0,2,0,1,0,0,2,2,0x0c,0x0a,0,
        0x21,0xf9,4,9,12,0,0,0, 0x2c,0,0,0,0,2,0,1,0,0,2,2,0x84,0x0a,0,
        0x3b
    };
    std::wstring gifData=L"data:image/gif,";const wchar_t hex[]=L"0123456789ABCDEF";
    for(const auto byte:gifBytes){gifData+=L'%';gifData+=hex[byte>>4];gifData+=hex[byte&15];}
    std::vector<unsigned char> decodedData;
    Check(DecodeImageDataUrl(gifData,decodedData)&&decodedData==gifBytes,
          L"percent-encoded image data URL preserves binary bytes");
    const auto gif=DecodeRasterImage(gifBytes,L"animation.gif",&error);
    Check(gif&&gif->width==2&&gif->height==1&&gif->frames.size()==2&&gif->animated,
          L"animated GIF frames decode");
    if(gif&&gif->frames.size()==2){
        Check(gif->frames[0].delayMs==80&&gif->frames[1].delayMs==120,
              L"GIF frame delays are retained");
        const auto& second=gif->frames[1].pixels;
        Check(second.size()==8&&second[2]>240&&second[6]<20&&second[5]>240,
              L"GIF transparent frame is composited over the retained frame");
        gif->nextFrameTick=1080;
        Check(!gif->Advance(1079)&&gif->frameIndex==0,L"GIF waits for its authored delay");
        Check(gif->Advance(1080)&&gif->frameIndex==1,L"GIF advances at its frame deadline");
        Check(gif->Advance(1200)&&gif->frameIndex==0,L"GIF infinite loop restarts");
    }
    auto onceBytes=gifBytes;onceBytes.erase(onceBytes.begin()+25,onceBytes.begin()+44);
    const auto once=DecodeRasterImage(onceBytes,L"once.gif",&error);
    if(once&&once->frames.size()==2){
        once->nextFrameTick=1080;once->Advance(1080);
        Check(!once->Advance(1200)&&once->finished&&once->frameIndex==1,
              L"GIF without a loop extension stops after one play-through");
    }else Check(false,L"single-play GIF decodes");

    {
        HWND host=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"",WS_POPUP,
            -10000,-10000,80,60,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        RECT bounds{0,0,80,60};auto view=TWebFrame::View::Create(host,bounds);
        Check(view!=nullptr,L"image view is created");
        if(view){
            std::vector<std::wstring> requestedResources;
            view->SetBinaryResourceLoader([&](const std::wstring& resource,
                                              std::vector<unsigned char>& bytes){
                requestedResources.push_back(resource);
                if(resource==L"alpha.png"){bytes=pngBytes;return true;}
                if(resource==L"animation.gif"){bytes=gifBytes;return true;}
                if(resource==L"https://images.test/pages/assets/alpha.png"){bytes=pngBytes;return true;}
                if(resource==L"https://images.test/pages/assets/animation.gif"){bytes=gifBytes;return true;}
                return false;
            });
            Check(view->NavigateToString(
                L"<img id='picture' src='alpha.png' onload='window.imageLoads=(window.imageLoads||0)+1'>"),
                L"view loads an img resource through the binary loader");
            std::wstring value,scriptError;
            const bool initialScript=view->ExecuteScript(
                L"const image=document.getElementById('picture'); return image.complete && image.naturalWidth===2 && image.naturalHeight===2 && window.imageLoads===1",
                &value,&scriptError);
            Check(initialScript&&value==L"true",
                L"HTMLImageElement load state and natural dimensions are exposed to JavaScript");
            const bool sourceChanged=view->ExecuteScript(
                L"document.getElementById('picture').src='animation.gif';",nullptr,&scriptError);
            const bool changedScript=view->ExecuteScript(
                L"const changed=document.getElementById('picture'); return changed.complete+'|'+changed.naturalWidth+'|'+changed.naturalHeight+'|'+window.imageLoads",
                &value,&scriptError);
            Check(sourceChanged&&changedScript&&value==L"true|2|1|2",
                L"changing img.src reloads, relayouts, and dispatches load");
            const DWORD uiThread=GetCurrentThreadId();
            std::atomic<DWORD> imageWorkerThread{0};
            std::atomic<int> activeDecodes{0},maximumActiveDecodes{0},asyncRequests{0};
            view->SetParallelResourceLoading(true);
            view->SetBinaryResourceLoader([&](const std::wstring& resource,
                                              std::vector<unsigned char>& bytes){
                if(resource!=L"first.png"&&resource!=L"second.png"&&
                   resource!=L"detached.png")return false;
                imageWorkerThread.store(GetCurrentThreadId(),std::memory_order_relaxed);
                const int active=activeDecodes.fetch_add(1,std::memory_order_relaxed)+1;
                int maximum=maximumActiveDecodes.load(std::memory_order_relaxed);
                while(maximum<active&&!maximumActiveDecodes.compare_exchange_weak(
                    maximum,active,std::memory_order_relaxed)){}
                Sleep(30);bytes=pngBytes;
                asyncRequests.fetch_add(1,std::memory_order_relaxed);
                activeDecodes.fetch_sub(1,std::memory_order_relaxed);return true;
            });
            Check(view->NavigateToString(
                L"<img id='first' src='first.png' onload='window.asyncLoads=(window.asyncLoads||0)+1'>"
                L"<img id='second' src='second.png' onload='window.asyncLoads=(window.asyncLoads||0)+1'>"),
                L"parallel image fixture starts without waiting for decode");
            const auto imageDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            bool asyncImagesComplete=false;
            while(!asyncImagesComplete&&std::chrono::steady_clock::now()<imageDeadline){
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
                const bool queried=view->ExecuteScript(
                    L"const a=document.getElementById('first'),b=document.getElementById('second');"
                    L"return a.complete&&b.complete&&window.asyncLoads===2&&a.naturalWidth===2&&b.naturalWidth===2;",
                    &value,&scriptError);
                asyncImagesComplete=queried&&value==L"true";
                if(!asyncImagesComplete)Sleep(1);
            }
            Check(asyncImagesComplete&&asyncRequests.load(std::memory_order_relaxed)==2,
                  L"parallel image completions publish decoded pixels and load events on the view");
            Check(imageWorkerThread.load(std::memory_order_relaxed)!=0&&
                  imageWorkerThread.load(std::memory_order_relaxed)!=uiThread&&
                  maximumActiveDecodes.load(std::memory_order_relaxed)>=2,
                  L"independent image fetch/decode jobs overlap away from the UI thread");
            const bool detachedStarted=view->ExecuteScript(
                L"window.detachedImageLoads=0;(function(){"
                L"var target=document.createElement('img');target.id='preloaded';"
                L"document.body.appendChild(target);var preload=new Image(17,9);"
                L"preload.onload=function(){window.detachedImageLoads++;"
                L"window.detachedImageSize=preload.naturalWidth+'x'+preload.naturalHeight;"
                L"target.src=preload.src;};preload.src='detached.png';})();",
                nullptr,&scriptError);
            const auto detachedDeadline=std::chrono::steady_clock::now()+
                std::chrono::seconds(5);
            bool detachedComplete=false;
            while(!detachedComplete&&std::chrono::steady_clock::now()<detachedDeadline){
                MSG message{};
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
                const bool queried=view->ExecuteScript(
                    L"var image=document.getElementById('preloaded');"
                    L"return window.detachedImageLoads+'|'+window.detachedImageSize+'|'"
                    L"+image.complete+'|'+image.naturalWidth+'x'+image.naturalHeight;",
                    &value,&scriptError);
                detachedComplete=queried&&value==L"1|2x2|true|2x2";
                if(!detachedComplete)Sleep(1);
            }
            Check(detachedStarted&&detachedComplete&&
                  asyncRequests.load(std::memory_order_relaxed)==3,
                  L"detached Image preload keeps its handlers alive and publishes pixels on load");
            view->SetParallelResourceLoading(false);
            view->SetBinaryResourceLoader([&](const std::wstring& resource,
                                              std::vector<unsigned char>& bytes){
                requestedResources.push_back(resource);
                if(resource==L"alpha.png"){bytes=pngBytes;return true;}
                if(resource==L"animation.gif"){bytes=gifBytes;return true;}
                if(resource==L"https://images.test/pages/assets/alpha.png"){bytes=pngBytes;return true;}
                if(resource==L"https://images.test/pages/assets/animation.gif"){bytes=gifBytes;return true;}
                return false;
            });
            requestedResources.clear();
            Check(view->NavigateToString(
                L"<style>html,body{margin:0}body{background:#fff url('assets/alpha.png') 0 0/20px 20px no-repeat}</style>",
                L"https://images.test/pages/index.html"),
                L"view accepts a page-relative body background image");
            ShowWindow(host,SW_SHOWNOACTIVATE);view->SetVisible(true);
            RedrawWindow(view->Window(),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
            Check(std::find(requestedResources.begin(),requestedResources.end(),
                            L"https://images.test/pages/assets/alpha.png")!=requestedResources.end(),
                L"body CSS image paths use the same resolved binary resource loader as img");
            const bool backgroundChanged=view->ExecuteScript(
                L"document.body.style.backgroundImage='url(\"assets/animation.gif\")';"
                L"return document.body.style.backgroundImage;",&value,&scriptError);
            RedrawWindow(view->Window(),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
            Check(backgroundChanged&&value==L"url(\"assets/animation.gif\")"&&
                  std::find(requestedResources.begin(),requestedResources.end(),
                            L"https://images.test/pages/assets/animation.gif")!=requestedResources.end(),
                L"JavaScript CSS mutations resolve and paint new background image paths");
            Check(view->NavigateToString(
                L"<article id='editor' contenteditable='true'></article>"),
                L"contenteditable image deletion fixture loads");
            const bool inserted=view->ExecuteScript(
                L"window.lastInputType='';const editor=document.getElementById('editor');"
                L"editor.addEventListener('input',event=>window.lastInputType=event.inputType);"
                L"editor.focus();const range=document.createRange();range.selectNodeContents(editor);"
                L"range.collapse(false);const selection=window.getSelection();selection.removeAllRanges();"
                L"selection.addRange(range);document.execCommand('insertHTML',false,"
                L"'<img id=\"inserted\" src=\"alpha.png\">');"
                L"return document.getElementById('inserted')!==null;",
                &value,&scriptError);
            Check(inserted&&value==L"true",L"insertHTML places an image at the editable caret");
            SendMessageW(view->Window(),WM_CHAR,VK_BACK,1);
            const bool deleted=view->ExecuteScript(
                L"return (document.getElementById('inserted')===null)+'|'+window.lastInputType;",
                &value,&scriptError);
            Check(deleted&&value==L"true|deleteContentBackward",
                L"Backspace deletes the atomic image immediately before the editable caret");
        }
        view.reset();if(host)DestroyWindow(host);
    }

    if(initialized==S_OK||initialized==S_FALSE)CoUninitialize();
    if(failures){std::wcerr<<failures<<L" image regression test(s) failed\n";return 1;}
    std::wcout<<L"Image regression tests passed\n";return 0;
}

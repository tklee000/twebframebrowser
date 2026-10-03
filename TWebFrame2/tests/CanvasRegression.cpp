#include "Canvas.h"
#include "CSS.h"
#include "DOM.h"
#include "JavaScript.h"
#include "Layout.h"

#include <d2d1.h>
#include <dwrite.h>
#include <ole2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

using namespace TWebFrame::Internal;
using Microsoft::WRL::ComPtr;

namespace {

int failures = 0;

void Check(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L'\n';
        ++failures;
    }
}

bool Near(float left, float right) { return std::abs(left - right) < 0.01f; }

void CheckCheckmarkRaster(float scale){
    Document document;StyleSheet styles;std::wstring error;
    Check(document.Parse(LR"HTML(
        <style>html,body{margin:0;background:white}
        .host{position:relative;width:60px;height:60px;display:grid;place-items:center}
        .control{grid-area:1/1;box-sizing:border-box;width:24px;height:24px;border:2px solid #4a4a4a;
            border-radius:3px;background:white;transform:rotate(0deg) scale(1)}
        .control::after{content:'';position:absolute;left:5px;top:0;width:6px;height:12px;
            border:solid #c44d0e;border-width:0 4px 4px 0;transform:rotate(45deg) scale(1)}
        </style><label class='host'><span class='control'></span></label>
    )HTML",&error)&&styles.Parse(document.StyleText(),&error),L"generic transformed checkmark fixture parses");
    LayoutEngine layout(document,styles);layout.Layout(60,60,scale);
    const auto* control=layout.BoxFor(document.QuerySelector(L".control"));
    const UINT dimension=static_cast<UINT>(std::lround(60*scale));
    ComPtr<IWICImagingFactory> imaging;ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> factory;ComPtr<ID2D1RenderTarget> target;ComPtr<IDWriteFactory> writing;
    const bool ready=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))&&
        SUCCEEDED(imaging->CreateBitmap(dimension,dimension,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap))&&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.ReleaseAndGetAddressOf()))&&
        SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96*scale,96*scale),&target))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writing.ReleaseAndGetAddressOf())));
    Check(ready&&control,L"checkmark raster resources exist at the requested DPI");if(!ready||!control)return;
    target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::White));layout.Paint(target.Get(),writing.Get());
    Check(SUCCEEDED(target->EndDraw()),L"checkmark paints at the requested DPI");
    std::vector<BYTE> pixels(static_cast<size_t>(dimension)*dimension*4);
    Check(SUCCEEDED(bitmap->CopyPixels(nullptr,dimension*4,static_cast<UINT>(pixels.size()),pixels.data())),L"checkmark pixels can be read");
    int left=dimension,right=-1,top=dimension,bottom=-1;
    for(UINT y=0;y<dimension;++y)for(UINT x=0;x<dimension;++x){const auto* pixel=pixels.data()+(y*dimension+x)*4;
        if(pixel[2]>160&&pixel[1]<130&&pixel[0]<70){left=std::min(left,static_cast<int>(x));right=std::max(right,static_cast<int>(x));
            top=std::min(top,static_cast<int>(y));bottom=std::max(bottom,static_cast<int>(y));}}
    const float x=(left+right+1)/(2*scale),y=(top+bottom+1)/(2*scale);
    Check(right>=left&&bottom>=top&&std::abs(x-(control->rect.x+control->rect.width/2))<=1.1f&&
        std::abs(y-(control->rect.y+control->rect.height/2))<=1.1f,
        L"CSS checkmark pixels are centered in their control at 100% and 150% DPI");
    std::wcout<<L"Checkmark DPI "<<scale<<L" pixel center "<<x<<L","<<y<<L" control center "
        <<control->rect.x+control->rect.width/2<<L","<<control->rect.y+control->rect.height/2<<L'\n';
    const auto output=std::filesystem::path(L"TWebFrame2/tests/artifacts")/
        (scale==1?L"checkbox-transform-100.png":L"checkbox-transform-150.png");
    ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;
    bool saved=SUCCEEDED(imaging->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromFilename(output.c_str(),GENERIC_WRITE))&&
        SUCCEEDED(imaging->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))&&SUCCEEDED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))&&
        SUCCEEDED(encoder->CreateNewFrame(&frame,nullptr))&&SUCCEEDED(frame->Initialize(nullptr))&&SUCCEEDED(frame->SetSize(dimension,dimension));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(saved)saved=SUCCEEDED(frame->SetPixelFormat(&format))&&SUCCEEDED(frame->SetResolution(96*scale,96*scale))&&
        SUCCEEDED(frame->WritePixels(dimension,dimension*4,static_cast<UINT>(pixels.size()),pixels.data()))&&
        SUCCEEDED(frame->Commit())&&SUCCEEDED(encoder->Commit());
    Check(saved,L"checkmark raster evidence saves successfully");
}

void CheckCompositingRaster(float scale){
    Document document;std::wstring error,result;
    Check(document.Parse(L"<style>html,body{margin:0}canvas{width:40px;height:40px}</style><canvas id='blend' width='4' height='4'></canvas>",&error),error.c_str());
    JavaScriptRuntime runtime(document);runtime.SetDevicePixelRatio(scale);
    Check(runtime.Execute(L"var c=document.getElementById('blend'),x=c.getContext('2d');"
        L"x.fillStyle='red';x.fillRect(0,0,4,4);x.globalCompositeOperation='screen';"
        L"x.fillStyle='blue';x.fillRect(0,0,4,4);return Array.from(x.getImageData(2,2,1,1).data).join(',');",
        &result,&error)&&result==L"255,0,255,255",L"script readback returns the composited intrinsic pixel");
    StyleSheet styles;styles.Parse(document.StyleText());LayoutEngine layout(document,styles);layout.Layout(40,40,scale);
    const auto dimension=static_cast<UINT>(40*scale);
    ComPtr<IWICImagingFactory> imaging;ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> drawing;ComPtr<ID2D1RenderTarget> target;ComPtr<IDWriteFactory> writing;
    const bool ready=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))&&
        SUCCEEDED(imaging->CreateBitmap(dimension,dimension,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap))&&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,drawing.GetAddressOf()))&&
        SUCCEEDED(drawing->CreateWicBitmapRenderTarget(bitmap.Get(),D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96*scale,96*scale),&target))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(writing.GetAddressOf())));
    Check(ready,L"compositing raster resources exist");if(!ready)return;
    target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::White));layout.Paint(target.Get(),writing.Get());
    Check(SUCCEEDED(target->EndDraw()),L"compositing page painting succeeds");target.Reset();
    std::vector<unsigned char> pixels(static_cast<size_t>(dimension)*dimension*4);
    Check(SUCCEEDED(bitmap->CopyPixels(nullptr,dimension*4,static_cast<UINT>(pixels.size()),pixels.data())),L"composited page pixels are readable");
    const auto* pixel=pixels.data()+(static_cast<size_t>(dimension/2)*dimension+dimension/2)*4;
    Check(pixel[0]==255&&pixel[1]==0&&pixel[2]==255&&pixel[3]==255,
        L"page painting and script readback share composited colors at 100% and 150% DPI");
}

void CheckExport(JavaScriptRuntime& runtime,float scale){
    runtime.SetDevicePixelRatio(scale);std::wstring result,error;
    Check(runtime.Execute(LR"JS(
        var exportedCanvas=document.createElement('canvas');exportedCanvas.width=3;exportedCanvas.height=2;
        var exportedContext=exportedCanvas.getContext('2d');exportedContext.fillStyle='red';exportedContext.fillRect(0,0,1,1);
        exportedContext.globalCompositeOperation='screen';exportedContext.fillStyle='blue';exportedContext.fillRect(0,0,1,1);
        var encodedPng=atob(exportedCanvas.toDataURL().split(',')[1]);
        var exportedBlob=null,exportedEmpty='unset',exportOrder=[];
        exportedCanvas.toBlob(function(blob){exportedBlob=blob;exportOrder.push('blob');});
        var emptyCanvas=document.createElement('canvas');emptyCanvas.width=0;
        emptyCanvas.toBlob(function(blob){exportedEmpty=blob;});exportOrder.push('sync');
        return encodedPng;
    )JS",&result,&error),error.c_str());
    std::vector<unsigned char> png;for(const auto byte:result)png.push_back(static_cast<unsigned char>(byte));
    ComPtr<IWICImagingFactory> imaging;ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;ComPtr<IWICFormatConverter> converted;
    const bool loaded=!png.empty()&&SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))&&
        SUCCEEDED(imaging->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromMemory(png.data(),static_cast<DWORD>(png.size())))&&
        SUCCEEDED(imaging->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder))&&
        SUCCEEDED(decoder->GetFrame(0,&frame))&&SUCCEEDED(imaging->CreateFormatConverter(&converted))&&
        SUCCEEDED(converted->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    Check(loaded,L"canvas data URL decodes as a real PNG");
    if(loaded){UINT width=0,height=0;double dpiX=0,dpiY=0;unsigned char pixels[24]{};
        Check(SUCCEEDED(frame->GetSize(&width,&height))&&width==3&&height==2,L"PNG stores intrinsic dimensions at both screen scales");
        Check(SUCCEEDED(frame->GetResolution(&dpiX,&dpiY))&&std::abs(dpiX-96)<.1&&std::abs(dpiY-96)<.1,L"PNG pixel density stays at 96 DPI");
        Check(SUCCEEDED(converted->CopyPixels(nullptr,12,sizeof(pixels),pixels))&&
            pixels[0]==255&&pixels[1]==0&&pixels[2]==255&&pixels[3]==255&&pixels[7]==0,
            L"PNG preserves the blended magenta pixel and transparent neighboring pixel");
    }
    Check(runtime.Execute(L"return exportOrder.join(',');",&result,&error)&&result==L"sync",L"PNG Blob callbacks wait for the next task");
    for(unsigned attempt=0;attempt<4;++attempt)runtime.RunTimers();
    Check(runtime.Execute(L"return exportOrder.join(',')+'|'+(exportedBlob instanceof Blob)+'|'+exportedBlob.type+'|'+(exportedBlob.size>0)+'|'+(exportedEmpty===null);",&result,&error)&&
        result==L"sync,blob|true|image/png|true|true",L"toBlob returns actual PNG data and null for an empty bitmap");
    if(result!=L"sync,blob|true|image/png|true|true")std::wcerr<<L"Blob result: "<<result<<L" error: "<<error<<L'\n';
}

void CheckRaster(LayoutEngine& layout, float scale) {
    constexpr float viewportWidth = 700.0f;
    constexpr float viewportHeight = 460.0f;
    const auto pixelWidth = static_cast<UINT>(std::lround(viewportWidth * scale));
    const auto pixelHeight = static_cast<UINT>(std::lround(viewportHeight * scale));
    ComPtr<IWICImagingFactory> wicFactory;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> writeFactory;
    const bool resources =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wicFactory))) &&
        SUCCEEDED(wicFactory->CreateBitmap(pixelWidth, pixelHeight,
            GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                    d2dFactory.ReleaseAndGetAddressOf())) &&
        SUCCEEDED(d2dFactory->CreateWicBitmapRenderTarget(bitmap.Get(),
            D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                USER_DEFAULT_SCREEN_DPI * scale, USER_DEFAULT_SCREEN_DPI * scale),
            &target)) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(writeFactory.ReleaseAndGetAddressOf())));
    Check(resources, L"canvas raster resources are created");
    if (!resources) return;

    target->BeginDraw();
    target->Clear(D2D1::ColorF(0, 0.0f));
    layout.Paint(target.Get(), writeFactory.Get());
    Check(SUCCEEDED(target->EndDraw()), L"canvas commands paint successfully");

    WICRect lockRect{0, 0, static_cast<INT>(pixelWidth), static_cast<INT>(pixelHeight)};
    ComPtr<IWICBitmapLock> lock;
    Check(SUCCEEDED(bitmap->Lock(&lockRect, WICBitmapLockRead, &lock)),
          L"canvas pixels are readable");
    if (!lock) return;
    UINT stride = 0, bufferSize = 0;
    BYTE* bytes = nullptr;
    Check(SUCCEEDED(lock->GetStride(&stride)) &&
              SUCCEEDED(lock->GetDataPointer(&bufferSize, &bytes)) && bytes,
          L"canvas pixel buffer is available");
    if (!bytes) return;
    const auto pixel = [&](float cssX, float cssY) {
        const auto x = std::min(pixelWidth - 1,
            static_cast<UINT>(std::max(0.0f, std::round(cssX * scale))));
        const auto y = std::min(pixelHeight - 1,
            static_cast<UINT>(std::max(0.0f, std::round(cssY * scale))));
        return bytes + y * stride + x * 4;
    };

    const auto* background = pixel(10.0f, 10.0f);
    Check(background[3] > 240 && background[0] > 235 &&
              background[1] > 235 && background[2] > 235,
          L"the intrinsic canvas bitmap is composited into its CSS box");
    const auto* point = pixel(470.0f * 640.0f / 960.0f,
                              137.0f * 420.0f / 630.0f);
    Check(point[3] > 240 && point[0] > 100 && point[0] < 220 &&
              point[1] > 100 && point[1] < 220 && point[2] > 100 && point[2] < 220,
          L"canvas paths retain their intrinsic coordinates when CSS-scaled");
    bool foundStroke = false;
    const int strokeCenter = static_cast<int>(std::lround(550.0f * 420.0f / 630.0f * scale));
    const UINT strokeX = static_cast<UINT>(std::lround(320.0f * scale));
    for (int y = std::max(0, strokeCenter - 4);
         y <= std::min<int>(pixelHeight - 1, strokeCenter + 4); ++y) {
        const auto* sample = bytes + static_cast<UINT>(y) * stride + strokeX * 4;
        foundStroke = foundStroke ||
            (sample[3] > 240 && sample[0] < 80 && sample[1] < 80 && sample[2] < 80);
    }
    Check(foundStroke, L"canvas strokes scale consistently at 100% and 150% DPI");
    const auto* radialCenter=pixel(480.0f,300.0f);
    const auto* radialEdge=pixel(518.0f,300.0f);
    const auto* radialMiddle=pixel(500.0f,300.0f);
    Check(radialCenter[2]>230&&radialCenter[0]<20&&radialEdge[0]>230&&radialEdge[2]<20&&
              radialMiddle[0]>60&&radialMiddle[2]>60,
          L"radial gradient backing-store pixels retain their CSS positions and colors at 100% and 150% DPI");
    const auto* quadratic=pixel(360.0f,280.0f);
    const auto* cubic=pixel(580.0f,280.0f);
    Check(quadratic[2]>240&&quadratic[0]<20&&cubic[0]>240&&cubic[2]<20,
          L"quadratic and cubic canvas curves keep their transformed CSS positions at 100% and 150% DPI");
    CanvasDrawingState textState;textState.font=L"24px Arial";CanvasTextMetrics metrics;
    Check(MeasureCanvasText(textState,L"Mg",metrics),L"painted text has native glyph metrics");
    const double intrinsicScale=640.0/960.0*scale;
    const auto left=static_cast<int>(std::floor((100-metrics.actualBoundingBoxLeft)*intrinsicScale));
    const auto right=static_cast<int>(std::ceil((100+metrics.actualBoundingBoxRight)*intrinsicScale));
    const auto top=static_cast<int>(std::floor((360-metrics.actualBoundingBoxAscent)*intrinsicScale));
    const auto bottom=static_cast<int>(std::ceil((360+metrics.actualBoundingBoxDescent)*intrinsicScale));
    int inkLeft=right+2,inkRight=left-2,inkTop=bottom+2,inkBottom=top-2;
    for(int y=std::max(0,top-2);y<std::min<int>(pixelHeight,bottom+2);++y)
        for(int x=std::max(0,left-2);x<std::min<int>(pixelWidth,right+2);++x){
            const auto* sample=bytes+y*stride+x*4;
            if(sample[3]>240&&sample[0]<180&&sample[1]<180&&sample[2]<180){
                inkLeft=std::min(inkLeft,x);inkRight=std::max(inkRight,x);
                inkTop=std::min(inkTop,y);inkBottom=std::max(inkBottom,y);
            }
        }
    Check(inkLeft<=inkRight&&std::abs(inkLeft-left)<=2&&std::abs(inkRight+1-right)<=2&&
              std::abs(inkTop-top)<=2&&std::abs(inkBottom+1-bottom)<=2,
          L"measureText ink bounds match painted glyphs after CSS and 100% or 150% DPI scaling");
    Check(pixel(680.0f, 10.0f)[3] == 0,
          L"painting remains clipped to the canvas CSS content box");
}

} // namespace

int wmain() {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const wchar_t* html = LR"HTML(
        <style>
            html, body { margin: 0; }
            canvas { width: 640px; height: 420px; }
        </style>
        <canvas id="chart" width="960" height="630"></canvas>
        <script>
            var canvas = document.getElementById('chart');
            var context = canvas.getContext('2d');
            context.setTransform(1, 0, 0, 1, 0, 0);
            context.clearRect(0, 0, canvas.width, canvas.height);
            var gradient = context.createLinearGradient(0, 0, 0, canvas.height);
            gradient.addColorStop(0, '#fff');
            gradient.addColorStop(1, '#f7f7f7');
            context.fillStyle = gradient;
            context.fillRect(0, 0, canvas.width, canvas.height);
            context.beginPath();
            context.moveTo(90, 550);
            context.lineTo(930, 550);
            context.lineWidth = 3;
            context.strokeStyle = '#222';
            context.stroke();
            context.beginPath();
            context.arc(470, 137, 10, 0, Math.PI * 2, false);
            context.fillStyle = '#9e9e9e';
            context.fill();
            context.save();
            context.translate(24, 295);
            context.rotate(-Math.PI / 2);
            context.font = "20px Arial, '맑은 고딕', sans-serif";
            context.textAlign = 'center';
            context.textBaseline = 'top';
            context.fillStyle = '#000';
            context.fillText('빈도', 0, 0);
            context.restore();
            var radial=context.createRadialGradient(720,450,5,720,450,50);
            radial.addColorStop(0,'red');radial.addColorStop(1,'blue');context.fillStyle=radial;
            context.fillRect(660,390,120,120);
            context.font='24px Arial';context.textBaseline='alphabetic';context.fillStyle='black';
            context.fillText('Mg',100,360);
            context.fillStyle='red';context.beginPath();context.moveTo(480,450);
            context.quadraticCurveTo(540,330,600,450);context.closePath();context.fill();
            context.save();context.translate(810,330);context.fillStyle='blue';context.beginPath();
            context.moveTo(0,120);context.bezierCurveTo(0,0,120,0,120,120);context.closePath();context.fill();context.restore();
            var runtimeProbe = Infinity > 1 && Math.round(Math.cos(0)) === 1 &&
                               Math.round(Math.sin(Math.PI / 2)) === 1 &&
                               Math.round(Math.atan2(1, 0) * 2) === Math.round(Math.PI);
        </script>
    )HTML";

    std::wstring error;
    Document document;
    Check(document.Parse(html, &error), error.c_str());

    StyleSheet styles;
    Check(styles.Parse(document.StyleText(), &error), error.c_str());

    JavaScriptRuntime javascript(document);
    javascript.SetDevicePixelRatio(1.5);
    int paintMutations = 0;
    javascript.SetMutationSink([&](const JavaScriptRuntime::Mutation& mutation) {
        if (mutation.kind == JavaScriptRuntime::MutationKind::Paint) ++paintMutations;
    });
    Check(javascript.Load(document.ScriptText(), &error), error.c_str());

    const auto canvas = document.GetElementById(L"chart");
    Check(canvas && canvas->canvas, L"getContext creates a shared canvas surface");
    Check(canvas && canvas->canvas && canvas->canvas->width == 960 &&
              canvas->canvas->height == 630,
          L"canvas backing-store dimensions come from the width and height attributes");
    Check(canvas && canvas->canvas && canvas->canvas->commands.size() == 8,
          L"fill, stroke, arc, curves, radial gradient and text drawing commands are retained in order");
    Check(paintMutations >= 1, L"drawing invalidates the canvas paint region");

    std::wstring probe;
    Check(javascript.Execute(L"return runtimeProbe && window.devicePixelRatio === 1.5;",
                             &probe, &error) && probe == L"true",
          L"canvas scripts receive standard numeric globals and the monitor pixel ratio");

    Document resetDocument;
    Check(resetDocument.Parse(L"<canvas></canvas>", &error), error.c_str());
    JavaScriptRuntime resetRuntime(resetDocument);
    Check(resetRuntime.Execute(
              L"const canvas=document.querySelector('canvas');"
              L"const context=canvas.getContext('2d');"
              L"context.lineWidth=7;context.fillRect(0,0,10,10);"
              L"canvas.width=canvas.width;const propertyReset=context.lineWidth===1;"
              L"context.lineWidth=5;context.fillRect(0,0,10,10);"
              L"canvas.setAttribute('height','75');const attributeReset=context.lineWidth===1;"
              L"context.lineWidth=3;context.fillRect(0,0,10,10);"
              L"canvas.removeAttribute('height');"
              L"return propertyReset&&attributeReset&&context.lineWidth===1&&canvas.height===150;",
              &probe, &error) && probe == L"true",
          L"canvas dimension mutations reset drawing state and the backing store");
    const auto resetCanvas = resetDocument.QuerySelector(L"canvas");
    Check(resetCanvas && resetCanvas->canvas && resetCanvas->canvas->commands.empty(),
          L"resetting a canvas dimension clears retained pixels even when the size is unchanged");

    LayoutEngine layout(document, styles);
    for (const float scale : {1.0f, 1.5f}) {
        layout.Layout(700.0f, 460.0f, scale);
        const auto* box = layout.BoxFor(canvas);
        Check(box && Near(box->content.width, 640.0f) && Near(box->content.height, 420.0f),
              L"CSS canvas geometry stays in DIPs at 100% and 150% DPI");
        CheckRaster(layout, scale);
        CheckExport(javascript,scale);
        CheckCompositingRaster(scale);
        CheckCheckmarkRaster(scale);
    }

    if (SUCCEEDED(initialized)) CoUninitialize();

    if (failures) {
        std::wcerr << failures << L" test(s) failed\n";
        return 1;
    }
    std::wcout << L"Canvas regression tests passed at 100% and 150% scaling\n";
    return 0;
}

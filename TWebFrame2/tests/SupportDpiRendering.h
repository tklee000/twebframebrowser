#include "../src/CSS.h"
#include "../src/Layout.h"
#include <cmath>

bool CheckSupportDpiRendering(double scale){
    const auto width=static_cast<LONG>(400*scale),height=static_cast<LONG>(240*scale);
    HDC dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    HGDIOBJ previous=bitmap?SelectObject(dc,bitmap):nullptr;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;Microsoft::WRL::ComPtr<IDWriteFactory> text;
    const auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
    RECT bounds{0,0,width,height};bool ok=bitmap&&SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()))&&
        SUCCEEDED(factory->CreateDCRenderTarget(&props,&target))&&SUCCEEDED(target->BindDC(dc,&bounds))&&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(text.GetAddressOf())));
    if(ok){
        Document document;document.Parse(L"<html><body><div id='grid'></div></body></html>");
        JavaScriptRuntime runtime(document);runtime.SetViewportSize(400,240);runtime.SetDevicePixelRatio(scale);std::wstring error,result;
        ok=runtime.Execute(L"const policy=trustedTypes.createPolicy('dpi',{createHTML:function(s){return s;}});"
            L"document.getElementById('grid').innerHTML=policy.createHTML('<button id=left>A</button><button id=right>B</button>');"
            L"return devicePixelRatio;",&result,&error)&&std::abs(_wtof(result.c_str())-scale)<0.001;
        StyleSheet styles;styles.Parse(L"*{box-sizing:border-box;margin:0;padding:0}body{background:white}#grid{display:grid;grid-template-columns:120px 120px;gap:8px;margin:16px}button{height:48px;border:1px solid black;background:#00cc66}");
        LayoutEngine layout(document,styles);layout.Layout(400,240,static_cast<float>(scale));
        const auto left=document.GetElementById(L"left"),right=document.GetElementById(L"right");
        const auto* a=layout.BoxFor(left);const auto* b=layout.BoxFor(right);
        ok=ok&&a&&b&&std::abs(a->rect.width-120)<0.01&&std::abs(b->rect.x-a->rect.x-128)<0.01&&
            layout.HitTest(a->rect.x+20,a->rect.y+20)==left&&layout.HitTest(b->rect.x+20,b->rect.y+20)==right;
        runtime.SetGeometryProvider([&](const auto& node){JavaScriptRuntime::NodeGeometry geometry;
            if(const auto* box=layout.BoxFor(node)){geometry.x=box->rect.x;geometry.y=box->rect.y;geometry.width=box->rect.width;geometry.height=box->rect.height;}return geometry;});
        ok=ok&&runtime.Execute(L"return document.getElementById('left').getBoundingClientRect().width;",&result,&error)&&result==L"120";
        target->SetDpi(static_cast<float>(96*scale),static_cast<float>(96*scale));target->BeginDraw();target->Clear(D2D1::ColorF(D2D1::ColorF::White));
        layout.Paint(target.Get(),text.Get());ok=ok&&SUCCEEDED(target->EndDraw());GdiFlush();
        if(a&&pixels){const auto x=static_cast<size_t>((a->rect.x+20)*scale),y=static_cast<size_t>((a->rect.y+20)*scale);
            const auto pixel=static_cast<const uint32_t*>(pixels)[y*width+x];ok=ok&&(pixel&0xffffff)==0x00cc66;}
        // A DPI change keeps CSS dimensions and hit-test coordinates stable.
        const float other=scale==1?1.5f:1.0f;layout.Relayout(400,240,other);
        if(const auto* box=layout.BoxFor(left))ok=ok&&std::abs(box->rect.width-120)<0.01&&layout.HitTest(box->rect.x+20,box->rect.y+20)==left;
        else ok=false;layout.DiscardDeviceResources();
    }
    target.Reset();if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);DeleteDC(dc);return ok;
}

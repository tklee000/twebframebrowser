// Included in Layout.cpp's anonymous namespace after the Canvas drawing code.
// Draw each source with Direct2D, then combine premultiplied intrinsic pixels
// according to CSS Compositing and Blending. Page painting and export share it.
using CanvasColor=std::array<double,3>;

double CanvasLum(const CanvasColor& c){return 0.3*c[0]+0.59*c[1]+0.11*c[2];}
double CanvasSat(const CanvasColor& c){return *std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end());}
CanvasColor CanvasSetLum(CanvasColor c,double lum){
    const double delta=lum-CanvasLum(c);for(auto& channel:c)channel+=delta;
    const double l=CanvasLum(c),n=*std::min_element(c.begin(),c.end()),x=*std::max_element(c.begin(),c.end());
    if(n<0)for(auto& channel:c)channel=l+(channel-l)*l/(l-n);
    if(x>1)for(auto& channel:c)channel=l+(channel-l)*(1-l)/(x-l);
    return c;
}
CanvasColor CanvasSetSat(CanvasColor c,double sat){
    std::array<size_t,3> order{0,1,2};
    std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return c[a]<c[b];});
    auto& low=c[order[0]];auto& middle=c[order[1]];auto& high=c[order[2]];
    if(high>low){middle=(middle-low)*sat/(high-low);high=sat;}else middle=high=0;
    low=0;return c;
}
double CanvasBlendChannel(CanvasCompositeOperation mode,double b,double s){
    using Mode=CanvasCompositeOperation;
    switch(mode){
    case Mode::Multiply:return b*s;
    case Mode::Screen:return b+s-b*s;
    case Mode::Overlay:return b<=0.5?2*b*s:1-2*(1-b)*(1-s);
    case Mode::Darken:return std::min(b,s);
    case Mode::Lighten:return std::max(b,s);
    case Mode::ColorDodge:return b==0?0:s==1?1:std::min(1.0,b/(1-s));
    case Mode::ColorBurn:return b==1?1:s==0?0:1-std::min(1.0,(1-b)/s);
    case Mode::HardLight:return s<=0.5?2*b*s:1-2*(1-b)*(1-s);
    case Mode::SoftLight:{
        const double d=b<=0.25?((16*b-12)*b+4)*b:std::sqrt(b);
        return s<=0.5?b-(1-2*s)*b*(1-b):b+(2*s-1)*(d-b);
    }
    case Mode::Difference:return std::abs(b-s);
    case Mode::Exclusion:return b+s-2*b*s;
    default:return s;
    }
}
void CompositeCanvasPixel(unsigned char* destination,const unsigned char* source,
                          CanvasCompositeOperation mode,bool opaque){
    using Mode=CanvasCompositeOperation;
    const double s=source[3]/255.0,b=destination[3]/255.0;
    double fs=1,fb=1-s;
    switch(mode){
    case Mode::SourceIn:fs=b;fb=0;break;
    case Mode::SourceOut:fs=1-b;fb=0;break;
    case Mode::SourceAtop:fs=b;break;
    case Mode::DestinationOver:fs=1-b;fb=1;break;
    case Mode::DestinationIn:fs=0;fb=s;break;
    case Mode::DestinationOut:fs=0;break;
    case Mode::DestinationAtop:fs=1-b;fb=s;break;
    case Mode::Lighter:fb=1;break;
    case Mode::Copy:fb=0;break;
    case Mode::Xor:fs=1-b;break;
    default:break;
    }
    const auto byte=[](double value){return static_cast<unsigned char>(std::lround(std::max(0.0,std::min(1.0,value))*255));};
    if(mode<Mode::Multiply){
        for(unsigned channel=0;channel<3;++channel)
            destination[channel]=byte(source[channel]/255.0*fs+destination[channel]/255.0*fb);
    }else{
        CanvasColor cs{},cb{},blend{};
        // WIC stores BGR, whereas the non-separable blend equations use RGB.
        for(unsigned channel=0;channel<3;++channel){
            cs[channel]=s?source[2-channel]/(255*s):0;
            cb[channel]=b?destination[2-channel]/(255*b):0;
        }
        switch(mode){
        case Mode::Hue:blend=CanvasSetLum(CanvasSetSat(cs,CanvasSat(cb)),CanvasLum(cb));break;
        case Mode::Saturation:blend=CanvasSetLum(CanvasSetSat(cb,CanvasSat(cs)),CanvasLum(cb));break;
        case Mode::Color:blend=CanvasSetLum(cs,CanvasLum(cb));break;
        case Mode::Luminosity:blend=CanvasSetLum(cb,CanvasLum(cs));break;
        default:for(unsigned channel=0;channel<3;++channel)blend[channel]=CanvasBlendChannel(mode,cb[channel],cs[channel]);break;
        }
        for(unsigned channel=0;channel<3;++channel)
            destination[2-channel]=byte(s*(1-b)*cs[channel]+s*b*blend[channel]+(1-s)*b*cb[channel]);
    }
    destination[3]=opaque?255:byte(s*fs+b*fb);
}

bool NeedsCanvasCompositing(const CanvasSurface& surface){
    return std::any_of(surface.commands.begin(),surface.commands.end(),[](const CanvasDrawCommand& command){
        return command.kind!=CanvasCommandKind::ClearRect&&command.state.composite!=CanvasCompositeOperation::SourceOver;
    });
}

bool RasterizeCanvasBitmap(const CanvasSurface& surface,Microsoft::WRL::ComPtr<IWICBitmap>& bitmap){
    const auto bytes=static_cast<std::uint64_t>(surface.width)*surface.height*4;
    if(!surface.width||!surface.height||bytes>64*1024*1024)return false;
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
    Microsoft::WRL::ComPtr<ID2D1Factory> drawing;
    Microsoft::WRL::ComPtr<IDWriteFactory> text;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))||
       FAILED(imaging->CreateBitmap(surface.width,surface.height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&bitmap))||
       FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,drawing.GetAddressOf()))||
       FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(text.GetAddressOf()))))return false;
    const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    const auto render=[&](IWICBitmap* destination,const CanvasSurface& commands,bool clear){
        Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;
        return SUCCEEDED(drawing->CreateWicBitmapRenderTarget(destination,properties,&target))&&
            SUCCEEDED(ReplayCanvas(target.Get(),text.Get(),commands,clear));
    };
    if(!NeedsCanvasCompositing(surface))return render(bitmap.Get(),surface,true);
    CanvasSurface single;single.width=surface.width;single.height=surface.height;single.alpha=surface.alpha;
    if(!render(bitmap.Get(),single,true))return false;
    Microsoft::WRL::ComPtr<IWICBitmap> sourceBitmap;
    std::vector<unsigned char> source(static_cast<size_t>(bytes));
    for(const auto& command:surface.commands){
        single.commands.assign(1,command);
        if(command.kind==CanvasCommandKind::ClearRect||command.state.composite==CanvasCompositeOperation::SourceOver){
            single.alpha=surface.alpha;if(!render(bitmap.Get(),single,false))return false;continue;
        }
        if(!sourceBitmap&&FAILED(imaging->CreateBitmap(surface.width,surface.height,
                GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnLoad,&sourceBitmap)))return false;
        single.alpha=true;
        if(!render(sourceBitmap.Get(),single,true)||FAILED(sourceBitmap->CopyPixels(nullptr,surface.width*4,
                static_cast<UINT>(source.size()),source.data())))return false;
        Microsoft::WRL::ComPtr<IWICBitmapLock> lock;
        const WICRect bounds{0,0,static_cast<INT>(surface.width),static_cast<INT>(surface.height)};
        if(FAILED(bitmap->Lock(&bounds,WICBitmapLockRead|WICBitmapLockWrite,&lock)))return false;
        UINT stride=0,length=0;BYTE* pixels=nullptr;
        if(FAILED(lock->GetStride(&stride))||FAILED(lock->GetDataPointer(&length,&pixels))||
           static_cast<std::uint64_t>(stride)*(surface.height-1)+surface.width*4>length)return false;
        for(unsigned y=0;y<surface.height;++y)for(unsigned x=0;x<surface.width;++x)
            CompositeCanvasPixel(pixels+static_cast<size_t>(y)*stride+x*4,
                source.data()+(static_cast<size_t>(y)*surface.width+x)*4,command.state.composite,!surface.alpha);
    }
    return true;
}

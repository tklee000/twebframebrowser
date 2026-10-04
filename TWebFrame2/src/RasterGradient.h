#pragma once
#include <d2d1.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace TWebFrame::Internal {
// Opaque two-stop gradients are generated in CPU memory. The ordered dither
// is anchored to the generated image's grid, independently of GPU state.
inline bool RasterLinearGradient(UINT width,UINT height,float left,float top,
    const D2D1_POINT_2F& start,const D2D1_POINT_2F& end,
    const D2D1_POINT_2F& imageOrigin,const D2D1_COLOR_F& first,
    const D2D1_COLOR_F& last,std::vector<BYTE>& pixels){
    if(!width||!height||width>8192||height>8192||first.a!=1||last.a!=1)return false;
    const float dx=end.x-start.x,dy=end.y-start.y,length=dx*dx+dy*dy;
    if(!std::isfinite(length)||length<=0)return false;
    const float ax=dx/length,ay=dy/length,offset=-(start.x*dx+start.y*dy)/length;
    const float a[]={first.b,first.g,first.r},b[]={last.b,last.g,last.r};
    pixels.resize(static_cast<size_t>(width)*height*4);
    for(UINT y=0;y<height;++y){
        const float py=top+y+.5f;
        const auto qy=static_cast<UINT>(static_cast<int>(std::floor(py-imageOrigin.y)));
        for(UINT x=0;x<width;++x){
            const float px=left+x+.5f;
            const float t=std::clamp(px*ax+py*ay+offset,0.0f,1.0f);
            const auto qx=static_cast<UINT>(static_cast<int>(std::floor(px-imageOrigin.x)));
            const UINT m=((qy&1)<<5)|((qx&1)<<4)|((qy&2)<<2)|((qx&2)<<1)|((qy&4)>>1)|((qx&4)>>2);
            const float dither=(std::floor((m/64.f+1.f/128)*255+.5f)/255-.5f)/255;
            auto* pixel=pixels.data()+(static_cast<size_t>(y)*width+x)*4;
            for(size_t channel=0;channel<3;++channel){
                const float color=std::clamp(a[channel]*(1-t)+b[channel]*t+dither,0.0f,1.0f);
                pixel[channel]=static_cast<BYTE>(std::nearbyint(color*255));
            }
            pixel[3]=255;
        }
    }
    return true;
}
}

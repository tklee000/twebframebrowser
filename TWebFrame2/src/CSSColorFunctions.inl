namespace {
bool CssColorNumber(const std::wstring& text,double percentScale,double& value){
    if(text==L"none"){value=0;return true;}
    float number=0;size_t used=0;if(!TryParseFloat(text,number,&used))return false;
    const auto suffix=text.substr(used);
    if(suffix.empty())value=number;else if(suffix==L"%")value=number*percentScale/100;else return false;
    return std::isfinite(value);
}
bool CssHue(const std::wstring& text,double& hue){
    if(text==L"none"){hue=0;return true;}
    float number=0;size_t used=0;if(!TryParseFloat(text,number,&used))return false;
    const auto unit=text.substr(used);
    if(unit.empty()||unit==L"deg")hue=number;
    else if(unit==L"grad")hue=number*0.9;
    else if(unit==L"rad")hue=number*180/3.14159265358979323846;
    else if(unit==L"turn")hue=number*360;
    else return false;
    hue=std::fmod(hue,360);if(hue<0)hue+=360;return true;
}
unsigned int CssColorBytes(double r,double g,double b,double alpha){
    const auto byte=[](double value){return static_cast<unsigned>(std::lround(std::max(0.0,std::min(1.0,value))*255));};
    return (byte(alpha)<<24)|(byte(r)<<16)|(byte(g)<<8)|byte(b);
}
double CssSrgbEncode(double value){return std::abs(value)<=0.0031308?12.92*value:std::copysign(1.055*std::pow(std::abs(value),1/2.4)-0.055,value);}
double CssSrgbDecode(double value){return std::abs(value)<=0.04045?value/12.92:std::copysign(std::pow((std::abs(value)+0.055)/1.055,2.4),value);}
unsigned int CssFunctionalColor(const std::wstring& value,unsigned int fallback){
    const auto open=value.find(L'(');if(open==std::wstring::npos||CssSyntax::Close(value,open)!=value.size()-1)return fallback;
    const auto name=value.substr(0,open);const auto body=value.substr(open+1,value.size()-open-2);
    const bool legacy=CssSyntax::Find(body,L",")!=std::wstring::npos;
    auto parts=legacy?CssSyntax::Split(body,L',',true):SplitWhitespace(body);
    bool hasAlpha=legacy&&parts.size()==4;
    if(!legacy){
        const auto slash=CssSyntax::Find(body,L"/");
        if(slash!=std::wstring::npos){
            hasAlpha=true;
            const auto colors=SplitWhitespace(body.substr(0,slash));const auto alpha=SplitWhitespace(body.substr(slash+1));
            if(alpha.size()!=1)return fallback;parts=colors;parts.push_back(alpha[0]);
        }
    }
    if(name==L"color"){
        if(legacy||parts.size()<4||parts.size()>5||(parts.size()==5&&!hasAlpha))return fallback;
        const auto space=parts[0];double a=1,r=0,g=0,b=0;
        if(!CssColorNumber(parts[1],1,r)||!CssColorNumber(parts[2],1,g)||!CssColorNumber(parts[3],1,b)||(parts.size()==5&&!CssColorNumber(parts[4],1,a)))return fallback;
        if(space==L"srgb")return CssColorBytes(r,g,b,a);
        if(space==L"srgb-linear")return CssColorBytes(CssSrgbEncode(r),CssSrgbEncode(g),CssSrgbEncode(b),a);
        if(space==L"display-p3"){
            r=CssSrgbDecode(r);g=CssSrgbDecode(g);b=CssSrgbDecode(b);
            return CssColorBytes(CssSrgbEncode(1.2249401763*r-0.2249401763*g),CssSrgbEncode(-0.0420569547*r+1.0420569547*g),CssSrgbEncode(-0.0196375546*r-0.0786360456*g+1.0982736002*b),a);
        }
        if(space==L"xyz"||space==L"xyz-d65"||space==L"xyz-d50"){
            if(space==L"xyz-d50"){
                const auto x=r,y=g,z=b;
                r=0.9554734215*x-0.0230984549*y+0.0632592432*z;
                g=-0.0283697093*x+1.0099953981*y+0.0210414412*z;
                b=0.0123140149*x-0.0205076493*y+1.3303659262*z;
            }
            return CssColorBytes(CssSrgbEncode(3.2409699419*r-1.5373831776*g-0.4986107603*b),CssSrgbEncode(-0.9692436363*r+1.8759675015*g+0.0415550574*b),CssSrgbEncode(0.0556300797*r-0.2039769589*g+1.0569715142*b),a);
        }
        return fallback;
    }
    if(parts.size()!=3&&parts.size()!=4)return fallback;
    if(parts.size()==4&&!hasAlpha)return fallback;
    double alpha=1;if(parts.size()==4&&!CssColorNumber(parts[3],1,alpha))return fallback;
    if(name==L"rgb"||name==L"rgba"){
        double r=0,g=0,b=0;
        if(!CssColorNumber(parts[0],255,r)||!CssColorNumber(parts[1],255,g)||!CssColorNumber(parts[2],255,b))return fallback;
        if(legacy){const bool percent=parts[0].back()==L'%';for(size_t i=1;i<3;++i)if((parts[i].back()==L'%')!=percent)return fallback;}
        return CssColorBytes(r/255,g/255,b/255,alpha);
    }
    if(name==L"hsl"||name==L"hsla"||name==L"hwb"){
        if(name==L"hwb"&&legacy)return fallback;
        double hue=0,a=0,b=0;if(!CssHue(parts[0],hue)||!CssColorNumber(parts[1],1,a)||!CssColorNumber(parts[2],1,b))return fallback;
        if((parts[1].back()!=L'%'&&parts[1]!=L"none")||(parts[2].back()!=L'%'&&parts[2]!=L"none"))return fallback;
        a=std::max(0.0,std::min(1.0,a));b=std::max(0.0,std::min(1.0,b));
        const auto pure=[&](double offset){const auto k=std::fmod(hue/30+offset,12.0);return std::max(-1.0,std::min(1.0,std::min(k-3,9-k)));};
        if(name==L"hwb"){
            if(a+b>=1)return CssColorBytes(a/(a+b),a/(a+b),a/(a+b),alpha);
            return CssColorBytes((0.5-0.5*pure(0))*(1-a-b)+a,(0.5-0.5*pure(8))*(1-a-b)+a,(0.5-0.5*pure(4))*(1-a-b)+a,alpha);
        }
        const auto chroma=a*std::min(b,1-b);
        return CssColorBytes(b-chroma*pure(0),b-chroma*pure(8),b-chroma*pure(4),alpha);
    }
    if(name==L"lab"||name==L"lch"||name==L"oklab"||name==L"oklch"){
        if(legacy)return fallback;
        const bool ok=name.rfind(L"ok",0)==0;const bool polar=name==L"lch"||name==L"oklch";
        double l=0,a=0,b=0;
        if(!CssColorNumber(parts[0],ok?1:100,l)||!CssColorNumber(parts[1],ok?0.4:polar?150:125,a))return fallback;
        if(polar){double hue=0;if(!CssHue(parts[2],hue))return fallback;a=std::max(0.0,a);b=a*std::sin(hue*3.14159265358979323846/180);a*=std::cos(hue*3.14159265358979323846/180);}
        else if(!CssColorNumber(parts[2],ok?0.4:125,b))return fallback;
        l=std::max(0.0,std::min(ok?1.0:100.0,l));
        if(ok){
            const auto ll=std::pow(l+0.3963377774*a+0.2158037573*b,3);
            const auto mm=std::pow(l-0.1055613458*a-0.0638541728*b,3);
            const auto ss=std::pow(l-0.0894841775*a-1.2914855480*b,3);
            return CssColorBytes(CssSrgbEncode(4.0767416621*ll-3.3077115913*mm+0.2309699292*ss),CssSrgbEncode(-1.2684380046*ll+2.6097574011*mm-0.3413193965*ss),CssSrgbEncode(-0.0041960863*ll-0.7034186147*mm+1.7076147010*ss),alpha);
        }
        const auto fy=(l+16)/116,fx=fy+a/500,fz=fy-b/200;
        const auto inverse=[](double v){const auto cube=v*v*v;return cube>216.0/24389?cube:(116*v-16)/(24389.0/27);};
        const auto x=0.9642956764*inverse(fx),y=inverse(fy),z=0.8251046025*inverse(fz);
        const auto xd=0.9554734215*x-0.0230984549*y+0.0632592432*z;
        const auto yd=-0.0283697093*x+1.0099953981*y+0.0210414412*z;
        const auto zd=0.0123140149*x-0.0205076493*y+1.3303659262*z;
        return CssColorBytes(CssSrgbEncode(3.2409699419*xd-1.5373831776*yd-0.4986107603*zd),CssSrgbEncode(-0.9692436363*xd+1.8759675015*yd+0.0415550574*zd),CssSrgbEncode(0.0556300797*xd-0.2039769589*yd+1.0569715142*zd),alpha);
    }
    return fallback;
}
}

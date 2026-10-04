namespace {
class CssMath {
    struct Number {double value=0;int dimension=0;bool valid=false;};
    std::wstring text_;
    size_t position_=0;
    float reference_,viewport_,font_;
    unsigned depth_=0;
    void Space(){while(position_<text_.size()&&CssSyntax::Space(text_[position_]))++position_;}
    Number Expression(){
        if(++depth_>128){--depth_;return {};}
        auto result=Product();
        for(;;){
            const auto before=position_;Space();
            if(position_>=text_.size()||(text_[position_]!=L'+'&&text_[position_]!=L'-'))break;
            // CSS binary + and - require whitespace on both sides.
            if(before==position_||position_+1>=text_.size()||!CssSyntax::Space(text_[position_+1])){result.valid=false;break;}
            const auto op=text_[position_++];auto right=Product();
            if(!result.valid||!right.valid||result.dimension!=right.dimension){result.valid=false;break;}
            result.value+=op==L'+'?right.value:-right.value;
        }
        --depth_;return result;
    }
    Number Product(){
        auto result=Primary();
        for(;;){
            const auto before=position_;Space();
            if(position_>=text_.size()||(text_[position_]!=L'*'&&text_[position_]!=L'/')){position_=before;break;}
            const auto op=text_[position_++];const auto right=Primary();
            if(!result.valid||!right.valid)return {};
            if(op==L'*'){
                if(result.dimension&&right.dimension)return {};
                result.dimension+=right.dimension;result.value*=right.value;
            }else{
                if(right.value==0||right.dimension>result.dimension)return {};
                result.dimension-=right.dimension;result.value/=right.value;
            }
        }
        return result;
    }
    Number Primary(){
        Space();if(position_>=text_.size())return {};
        if(text_[position_]==L'('){++position_;auto result=Expression();Space();if(position_>=text_.size()||text_[position_++]!=L')')return {};return result;}
        const auto start=position_;
        if(std::iswalpha(text_[position_])){
            const auto end=CssSyntax::IdentifierEnd(text_,position_);auto name=text_.substr(position_,end-position_);position_=end;
            if(name==L"pi")return {3.14159265358979323846,0,true};
            if(name==L"e")return {2.71828182845904523536,0,true};
            if(position_>=text_.size()||text_[position_++]!=L'(')return {};
            std::wstring strategy;
            if(name==L"round"){
                Space();const auto modeEnd=CssSyntax::IdentifierEnd(text_,position_);
                if(modeEnd>position_){
                    const auto mode=text_.substr(position_,modeEnd-position_);
                    if(mode==L"nearest"||mode==L"up"||mode==L"down"||mode==L"to-zero"){
                        strategy=mode;position_=modeEnd;Space();if(position_>=text_.size()||text_[position_++]!=L',')return {};
                    }
                }
            }
            std::vector<Number> arguments;
            for(;;){
                auto value=Expression();if(!value.valid)return {};arguments.push_back(value);Space();
                if(position_>=text_.size())return {};
                if(text_[position_]==L')'){++position_;break;}
                if(text_[position_++]!=L',')return {};
            }
            if(name==L"calc")return arguments.size()==1?arguments[0]:Number{};
            const bool same=std::all_of(arguments.begin(),arguments.end(),[&](const auto& arg){return arg.dimension==arguments[0].dimension;});
            if(!same)return {};
            const auto count=arguments.size();auto result=arguments[0];
            if(name==L"min"||name==L"max"||name==L"hypot"){
                if(name==L"hypot")result.value=0;
                for(const auto& arg:arguments){
                    if(name==L"min")result.value=std::min(result.value,arg.value);
                    else if(name==L"max")result.value=std::max(result.value,arg.value);
                    else result.value=std::hypot(result.value,arg.value);
                }
            }else if(name==L"clamp"&&count==3)result.value=std::max(arguments[0].value,std::min(arguments[1].value,arguments[2].value));
            else if(name==L"abs"&&count==1)result.value=std::abs(result.value);
            else if(name==L"sign"&&count==1){result.value=result.value>0?1:result.value<0?-1:0;result.dimension=0;}
            else if((name==L"mod"||name==L"rem"||name==L"round")&&count==2){
                const auto interval=arguments[1].value;if(interval==0)return {};
                if(name==L"rem")result.value=std::fmod(result.value,interval);
                else if(name==L"mod")result.value-=interval*std::floor(result.value/interval);
                else{
                    if(interval<0)return {};
                    const auto q=result.value/interval;
                    result.value=interval*(strategy==L"up"?std::ceil(q):strategy==L"down"?std::floor(q):strategy==L"to-zero"?std::trunc(q):std::floor(q+0.5));
                }
            }else if(result.dimension==0&&count==1){
                if(name==L"sqrt")result.value=std::sqrt(result.value);
                else if(name==L"exp")result.value=std::exp(result.value);
                else if(name==L"log")result.value=std::log(result.value);
                else if(name==L"sin")result.value=std::sin(result.value);
                else if(name==L"cos")result.value=std::cos(result.value);
                else if(name==L"tan")result.value=std::tan(result.value);
                else return {};
            }else if(result.dimension==0&&count==2){
                if(name==L"pow")result.value=std::pow(result.value,arguments[1].value);
                else if(name==L"log")result.value=std::log(result.value)/std::log(arguments[1].value);
                else return {};
            }else return {};
            result.valid=std::isfinite(result.value);return result;
        }
        float value=0;size_t used=0;
        if(!TryParseFloat(text_.substr(start),value,&used)||used==0)return {};
        position_+=used;const auto unitStart=position_;
        while(position_<text_.size()&&(std::iswalpha(text_[position_])||text_[position_]==L'%'))++position_;
        const auto unit=text_.substr(unitStart,position_-unitStart);
        if(unit.empty())return {value,0,true};
        double multiplier=1;
        if(unit==L"px")multiplier=1;
        else if(unit==L"%")multiplier=reference_/100.0;
        else if(unit==L"vw"||unit==L"vh")multiplier=viewport_/100.0;
        else if(unit==L"em")multiplier=font_;
        else if(unit==L"rem")multiplier=16;
        else if(unit==L"pt")multiplier=96.0/72;
        else if(unit==L"pc")multiplier=16;
        else if(unit==L"in")multiplier=96;
        else if(unit==L"cm")multiplier=96.0/2.54;
        else if(unit==L"mm")multiplier=96.0/25.4;
        else if(unit==L"q")multiplier=96.0/101.6;
        else return {};
        return {value*multiplier,1,true};
    }
public:
    CssMath(std::wstring text,float reference,float viewport,float font):text_(std::move(text)),reference_(reference),viewport_(viewport),font_(font){}
    float Evaluate(float fallback){
        auto result=Primary();Space();
        return result.valid&&position_==text_.size()&&std::isfinite(result.value)&&std::abs(result.value)<=std::numeric_limits<float>::max()?static_cast<float>(result.value):fallback;
    }
};
}

float StyleSheet::Length(const std::wstring& raw,float reference,float viewport,float fallback,float fontSize){
    return CssMath(ToLower(Trim(raw)),reference,viewport,fontSize).Evaluate(fallback);
}

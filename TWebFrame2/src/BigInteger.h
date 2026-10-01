#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace TWebFrame::Internal {

// Signed arbitrary precision integers. Limbs store the unsigned magnitude;
// bitwise operations convert to a sufficiently wide two's complement value.
class BigInteger {
    std::vector<std::uint32_t> words_;
    bool negative_=false;
    void Normalize(){while(!words_.empty()&&!words_.back())words_.pop_back();if(words_.empty())negative_=false;}
    static int MagnitudeCompare(const BigInteger& a,const BigInteger& b){
        if(a.words_.size()!=b.words_.size())return a.words_.size()<b.words_.size()?-1:1;
        for(size_t i=a.words_.size();i--;)if(a.words_[i]!=b.words_[i])return a.words_[i]<b.words_[i]?-1:1;
        return 0;
    }
    static BigInteger MagnitudeAdd(const BigInteger& a,const BigInteger& b){
        BigInteger out;out.words_.resize((std::max)(a.words_.size(),b.words_.size()));std::uint64_t carry=0;
        for(size_t i=0;i<out.words_.size();++i){const auto sum=carry+(i<a.words_.size()?a.words_[i]:0)+(std::uint64_t)(i<b.words_.size()?b.words_[i]:0);out.words_[i]=(std::uint32_t)sum;carry=sum>>32;}
        if(carry)out.words_.push_back((std::uint32_t)carry);return out;
    }
    static BigInteger MagnitudeSubtract(const BigInteger& a,const BigInteger& b){
        BigInteger out;out.words_.resize(a.words_.size());std::uint64_t borrow=0;
        for(size_t i=0;i<a.words_.size();++i){const auto sub=(std::uint64_t)(i<b.words_.size()?b.words_[i]:0)+borrow;out.words_[i]=(std::uint32_t)(a.words_[i]-sub);borrow=a.words_[i]<sub;}
        out.Normalize();return out;
    }
    std::vector<std::uint32_t> TwosComplement(size_t width)const{
        auto out=words_;out.resize(width);if(negative_){std::uint64_t carry=1;for(auto& word:out){const auto value=(std::uint64_t)(word^0xffffffffu)+carry;word=(std::uint32_t)value;carry=value>>32;}}return out;
    }
    static BigInteger FromTwosComplement(std::vector<std::uint32_t> words){
        BigInteger out;out.negative_=!words.empty()&&(words.back()&0x80000000u)!=0;
        if(out.negative_){std::uint64_t carry=1;for(auto& word:words){const auto value=(std::uint64_t)(word^0xffffffffu)+carry;word=(std::uint32_t)value;carry=value>>32;}}
        out.words_=std::move(words);out.Normalize();return out;
    }
    void MultiplySmall(std::uint32_t multiplier){
        std::uint64_t carry=0;for(auto& word:words_){const auto value=(std::uint64_t)word*multiplier+carry;word=(std::uint32_t)value;carry=value>>32;}if(carry)words_.push_back((std::uint32_t)carry);
    }
    void AddSmall(std::uint32_t value){
        std::uint64_t carry=value;for(auto& word:words_){const auto sum=(std::uint64_t)word+carry;word=(std::uint32_t)sum;carry=sum>>32;if(!carry)return;}if(carry)words_.push_back((std::uint32_t)carry);
    }
    std::uint32_t DivideSmall(std::uint32_t divisor){
        std::uint64_t remainder=0;for(size_t i=words_.size();i--;){const auto value=(remainder<<32)|words_[i];words_[i]=(std::uint32_t)(value/divisor);remainder=value%divisor;}Normalize();return (std::uint32_t)remainder;
    }
public:
    explicit BigInteger(std::uint64_t value=0){if(value){words_.push_back((std::uint32_t)value);if(value>>32)words_.push_back((std::uint32_t)(value>>32));}}
    bool IsZero()const{return words_.empty();}
    bool IsNegative()const{return negative_;}
    size_t BitLength()const{if(IsZero())return 0;size_t bits=(words_.size()-1)*32;auto top=words_.back();while(top){++bits;top>>=1;}return bits;}
    int Compare(const BigInteger& other)const{if(negative_!=other.negative_)return negative_?-1:1;const auto comparison=MagnitudeCompare(*this,other);return negative_?-comparison:comparison;}
    BigInteger Negated()const{auto out=*this;if(!out.IsZero())out.negative_=!out.negative_;return out;}
    static bool Parse(const std::wstring& text,BigInteger& out){
        size_t begin=0,end=text.size();const auto space=[](wchar_t c){return std::iswspace(c)||c==0xfeff;};
        while(begin<end&&space(text[begin]))++begin;while(end>begin&&space(text[end-1]))--end;
        out=BigInteger();if(begin==end)return true;bool sign=false,negative=false;
        if(text[begin]==L'+'||text[begin]==L'-'){sign=true;negative=text[begin++]==L'-';if(begin==end)return false;}
        unsigned radix=10;if(end-begin>2&&text[begin]==L'0'){
            const auto prefix=text[begin+1];if(prefix==L'x'||prefix==L'X')radix=16;else if(prefix==L'o'||prefix==L'O')radix=8;else if(prefix==L'b'||prefix==L'B')radix=2;
            if(radix!=10){if(sign)return false;begin+=2;}
        }
        for(size_t i=begin;i<end;++i){const auto c=text[i];const int digit=c>=L'0'&&c<=L'9'?c-L'0':c>=L'a'&&c<=L'z'?c-L'a'+10:c>=L'A'&&c<=L'Z'?c-L'A'+10:-1;
            if(digit<0||(unsigned)digit>=radix)return false;out.MultiplySmall(radix);out.AddSmall((std::uint32_t)digit);}
        out.negative_=negative&&!out.IsZero();return true;
    }
    static bool FromNumber(double number,BigInteger& out){
        if(!std::isfinite(number)||std::trunc(number)!=number)return false;
        int exponent=0;const auto fraction=std::frexp(std::abs(number),&exponent);out=BigInteger((std::uint64_t)std::ldexp(fraction,53));
        out=exponent>=53?out.ShiftLeft(exponent-53):out.ShiftRight(53-exponent);out.negative_=number<0&&!out.IsZero();return true;
    }
    double ToNumber()const{double value=0;for(size_t i=words_.size();i--;)value=std::ldexp(value,32)+words_[i];return negative_?-value:value;}
    std::wstring ToString(unsigned radix=10)const{
        if(IsZero())return L"0";auto remaining=*this;remaining.negative_=false;std::wstring text;
        while(!remaining.IsZero()){const auto digit=remaining.DivideSmall(radix);text+=L"0123456789abcdefghijklmnopqrstuvwxyz"[digit];}
        if(negative_)text+=L'-';std::reverse(text.begin(),text.end());return text;
    }
    BigInteger Add(const BigInteger& other)const{
        if(negative_==other.negative_){auto out=MagnitudeAdd(*this,other);out.negative_=negative_;return out;}
        const auto comparison=MagnitudeCompare(*this,other);auto out=comparison>=0?MagnitudeSubtract(*this,other):MagnitudeSubtract(other,*this);out.negative_=!out.IsZero()&&(comparison>=0?negative_:other.negative_);return out;
    }
    BigInteger Subtract(const BigInteger& other)const{return Add(other.Negated());}
    BigInteger Multiply(const BigInteger& other)const{
        BigInteger out;if(IsZero()||other.IsZero())return out;out.words_.resize(words_.size()+other.words_.size());
        for(size_t i=0;i<words_.size();++i){std::uint64_t carry=0;for(size_t j=0;j<other.words_.size();++j){const auto product=(std::uint64_t)words_[i]*other.words_[j]+out.words_[i+j]+carry;out.words_[i+j]=(std::uint32_t)product;carry=product>>32;}out.words_[i+other.words_.size()]=(std::uint32_t)carry;}
        out.negative_=negative_!=other.negative_;out.Normalize();return out;
    }
    BigInteger ShiftLeft(size_t count)const{
        if(IsZero())return *this;BigInteger out;const auto whole=count/32;const unsigned bits=(unsigned)(count%32);out.words_.resize(words_.size()+whole+1);std::uint64_t carry=0;
        for(size_t i=0;i<words_.size();++i){const auto value=((std::uint64_t)words_[i]<<bits)|carry;out.words_[i+whole]=(std::uint32_t)value;carry=value>>32;}
        out.words_.back()=(std::uint32_t)carry;out.negative_=negative_;out.Normalize();return out;
    }
    BigInteger ShiftRight(size_t count)const{
        const auto whole=count/32;const unsigned bits=(unsigned)(count%32);bool discarded=false;
        for(size_t i=0;i<(std::min)(whole,words_.size());++i)discarded|=words_[i]!=0;
        if(bits&&whole<words_.size())discarded|=(words_[whole]&((std::uint32_t{1}<<bits)-1))!=0;
        BigInteger out;if(whole<words_.size()){out.words_.resize(words_.size()-whole);for(size_t i=whole;i<words_.size();++i){auto word=words_[i]>>bits;if(bits&&i+1<words_.size())word|=words_[i+1]<<(32-bits);out.words_[i-whole]=word;}out.Normalize();}
        if(negative_&&discarded)out.AddSmall(1);out.negative_=negative_&&!out.IsZero();return out;
    }
    bool DivRem(const BigInteger& divisor,BigInteger& quotient,BigInteger& remainder)const{
        if(divisor.IsZero())return false;quotient=BigInteger();remainder=*this;remainder.negative_=false;auto denominator=divisor;denominator.negative_=false;
        if(MagnitudeCompare(remainder,denominator)>=0){size_t shift=remainder.BitLength()-denominator.BitLength();auto aligned=denominator.ShiftLeft(shift);quotient.words_.resize(shift/32+1);
            for(;;){if(MagnitudeCompare(remainder,aligned)>=0){remainder=MagnitudeSubtract(remainder,aligned);quotient.words_[shift/32]|=std::uint32_t{1}<<(shift%32);}if(!shift)break;--shift;aligned=aligned.ShiftRight(1);}}
        quotient.negative_=negative_!=divisor.negative_;quotient.Normalize();remainder.negative_=negative_&&!remainder.IsZero();return true;
    }
    BigInteger Bitwise(const BigInteger& other,wchar_t op)const{
        const auto width=(std::max)(words_.size(),other.words_.size())+1;auto a=TwosComplement(width);const auto b=other.TwosComplement(width);
        for(size_t i=0;i<width;++i)a[i]=op==L'&'?a[i]&b[i]:op==L'|'?a[i]|b[i]:a[i]^b[i];return FromTwosComplement(std::move(a));
    }
    BigInteger BitwiseNot()const{return Negated().Subtract(BigInteger(1));}
};
}

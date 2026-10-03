#pragma once

#ifdef SUPPORT_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 16
#define PCRE2_STATIC
#include "../vendor/pcre2/pcre2.h"
#else
#include <regex>
#endif
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace TWebFrame::Internal {
// Match offsets remain UTF-16 offsets even when the expression consumes code
// points. This is the coordinate system used by JavaScript string indices.
class RuntimeRegex {
public:
    struct Match {
        size_t index=0,end=0;
        std::vector<std::optional<std::wstring>> captures;
        std::vector<std::optional<std::pair<size_t,size_t>>> offsets;
        std::vector<std::pair<std::wstring,size_t>> names;
    };
    RuntimeRegex(const std::wstring& pattern,const std::wstring& flags) {
#ifdef SUPPORT_PCRE2
        uint32_t options=PCRE2_ALT_BSUX|PCRE2_MATCH_UNSET_BACKREF;
#else
        auto options=std::regex_constants::ECMAScript;
#endif
        std::wstring seen;
        for(wchar_t flag:flags){
            if(seen.find(flag)!=std::wstring::npos||std::wstring(L"dgimsuy").find(flag)==std::wstring::npos)
                throw std::runtime_error("Invalid regular expression flags");
            seen+=flag;
#ifdef SUPPORT_PCRE2
            if(flag==L'i')options|=PCRE2_CASELESS;
            if(flag==L'm')options|=PCRE2_MULTILINE;
            if(flag==L's')options|=PCRE2_DOTALL;
            if(flag==L'u')options|=PCRE2_UTF|PCRE2_MATCH_INVALID_UTF;
#else
            if(flag==L'i')options|=std::regex_constants::icase;
#if !defined(_MSC_VER) || _MSC_VER > 1929
            if(flag==L'm')options|=std::regex_constants::multiline;
#endif
            // VS 2019 wregex already treats anchors as multiline and has no
            // multiline option. Keep its standard-library behavior there.
#endif
        }
#ifdef SUPPORT_PCRE2
        int error=0;PCRE2_SIZE offset=0;
        code_.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),pattern.size(),options,&error,&offset,nullptr));
        if(!code_){PCRE2_UCHAR message[256]{};pcre2_get_error_message(error,message,256);
            std::wstring text(reinterpret_cast<wchar_t*>(message));
            throw std::runtime_error("Invalid regular expression at offset "+std::to_string(offset));}
        uint32_t count=0,size=0;PCRE2_SPTR table=nullptr;
        pcre2_pattern_info(code_.get(),PCRE2_INFO_NAMECOUNT,&count);
        pcre2_pattern_info(code_.get(),PCRE2_INFO_NAMEENTRYSIZE,&size);
        pcre2_pattern_info(code_.get(),PCRE2_INFO_NAMETABLE,&table);
        for(uint32_t i=0;i<count;++i,table+=size)
            names_.emplace_back(reinterpret_cast<const wchar_t*>(table+1),table[0]);
#else
        // ECMAScript wregex works in UTF-16 code units on Windows. It does not
        // implement JavaScript Unicode mode, named groups or lookbehind.
        // dotAll is emulated without changing escaped dots or character classes.
        std::wstring translated;bool characterClass=false;
        for(size_t i=0;i<pattern.size();++i){
            const auto ch=pattern[i];
            if(ch==L'\\'){
                translated+=ch;if(i+1<pattern.size())translated+=pattern[++i];continue;
            }
            if(ch==L'[')characterClass=true;
            else if(ch==L']')characterClass=false;
            if(ch==L'.'&&!characterClass&&flags.find(L's')!=std::wstring::npos)translated+=L"[\\s\\S]";
            else translated+=ch;
        }
        try{code_.assign(translated,options);}
        catch(const std::regex_error&){throw std::runtime_error("Invalid regular expression");}
#endif
    }
    bool Search(const std::wstring& input,size_t start,bool sticky,Match& result)const{
        if(start>input.size())return false;
#ifdef SUPPORT_PCRE2
        const auto data=std::unique_ptr<pcre2_match_data,decltype(&pcre2_match_data_free)>(
            pcre2_match_data_create_from_pattern(code_.get(),nullptr),pcre2_match_data_free);
        const auto context=std::unique_ptr<pcre2_match_context,decltype(&pcre2_match_context_free)>(
            pcre2_match_context_create(nullptr),pcre2_match_context_free);
        pcre2_set_match_limit(context.get(),1000000);pcre2_set_depth_limit(context.get(),1000);
        const int matched=pcre2_match(code_.get(),reinterpret_cast<PCRE2_SPTR>(input.data()),input.size(),start,
            sticky?PCRE2_ANCHORED:0,data.get(),context.get());
        if(matched==PCRE2_ERROR_NOMATCH)return false;
        if(matched<0)throw std::runtime_error("Regular expression execution limit or invalid UTF-16 offset");
        const auto offsets=pcre2_get_ovector_pointer(data.get());
        const size_t count=pcre2_get_ovector_count(data.get());
        result.index=offsets[0];result.end=offsets[1];result.names=names_;result.captures.clear();result.offsets.clear();
        for(size_t i=0;i<count;++i){
            result.captures.push_back(offsets[i*2]==PCRE2_UNSET?std::nullopt:
                std::optional<std::wstring>(input.substr(offsets[i*2],offsets[i*2+1]-offsets[i*2])));
            result.offsets.push_back(offsets[i*2]==PCRE2_UNSET?std::nullopt:
                std::optional<std::pair<size_t,size_t>>(std::make_pair(offsets[i*2],offsets[i*2+1])));
        }
#else
        std::wsmatch matches;auto options=std::regex_constants::match_default;
        if(sticky)options|=std::regex_constants::match_continuous;
        // Preserve anchor and word-boundary context when lastIndex is nonzero.
        if(start>0)options|=std::regex_constants::match_prev_avail;
        try{
            if(!std::regex_search(input.cbegin()+start,input.cend(),matches,code_,options))return false;
        }catch(const std::regex_error&){throw std::runtime_error("Regular expression execution limit");}
        result.index=start+matches.position();result.end=result.index+matches.length();
        result.names.clear();result.captures.clear();result.offsets.clear();
        for(size_t i=0;i<matches.size();++i){
            if(!matches[i].matched){result.captures.push_back(std::nullopt);result.offsets.push_back(std::nullopt);continue;}
            const size_t begin=start+matches.position(i);
            result.captures.push_back(matches.str(i));
            result.offsets.push_back(std::make_pair(begin,begin+matches.length(i)));
        }
#endif
        return true;
    }
    static size_t Advance(const std::wstring& input,size_t index,bool unicode){
        if(unicode&&index+1<input.size()&&input[index]>=0xd800&&input[index]<=0xdbff&&
           input[index+1]>=0xdc00&&input[index+1]<=0xdfff)return index+2;
        return index+1;
    }
private:
#ifdef SUPPORT_PCRE2
    std::unique_ptr<pcre2_code,decltype(&pcre2_code_free)> code_{nullptr,pcre2_code_free};
    std::vector<std::pair<std::wstring,size_t>> names_;
#else
    std::wregex code_;
#endif
};
}

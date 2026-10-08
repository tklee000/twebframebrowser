#include "CaptureIO.h"
#include <dwrite.h>
#include <wrl/client.h>
#include <sstream>
#include <vector>
using Microsoft::WRL::ComPtr;

// Read-only DirectWrite diagnostic, independent of the common engine.
class RunCollector final:public IDWriteTextRenderer {
public:
    std::wostringstream runs;bool first=true;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IDWriteTextRenderer)||iid==__uuidof(IDWritePixelSnapping)){
            *out=static_cast<IDWriteTextRenderer*>(this);return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return 1;}
    ULONG STDMETHODCALLTYPE Release()override{return 1;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value)override{*value=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value)override{*value={1,0,0,1,0,0};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value)override{*value=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,
            const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* description,IUnknown*)override{
        if(!first)runs<<L',';first=false;
        runs<<L"{\"x\":"<<x<<L",\"baseline\":"<<y<<L",\"textPosition\":"
            <<(description?description->textPosition:0)<<L",\"glyphs\":[";
        for(UINT32 i=0;i<run->glyphCount;++i){
            if(i)runs<<L',';runs<<run->glyphIndices[i];
        }
        runs<<L"],\"advances\":[";
        for(UINT32 i=0;i<run->glyphCount;++i){if(i)runs<<L',';runs<<(run->glyphAdvances?run->glyphAdvances[i]:0);}
        runs<<L"]}";return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context,FLOAT x,FLOAT y,IDWriteInlineObject* object,
            BOOL sideways,BOOL rtl,IUnknown* effect)override{return object->Draw(context,this,x,y,sideways,rtl,effect);}
};

int wmain(int argc,wchar_t** argv){
    if(argc<4)return 2;
    const auto text=RegressionIO::ReadText(argv[1]);
    ComPtr<IDWriteFactory> factory;ComPtr<IDWriteTextFormat> format;ComPtr<IDWriteTextLayout> layout;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))return 3;
    if(FAILED(factory->CreateTextFormat(argc>4?argv[4]:L"Consolas",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,20,L"ko-kr",&format)))return 3;
    if(FAILED(factory->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format.Get(),
            std::stof(argv[3]),10000,&layout)))return 3;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WHOLE_WORD);
    RunCollector collector;if(FAILED(layout->Draw(nullptr,&collector,0,0)))return 3;
    UINT32 count=0;layout->GetLineMetrics(nullptr,0,&count);std::vector<DWRITE_LINE_METRICS> lines(count);
    if(FAILED(layout->GetLineMetrics(lines.data(),count,&count)))return 3;
    std::wostringstream out;out<<L"{\"sourceUtf16Length\":"<<text.size()<<L",\"lines\":[";
    for(size_t i=0;i<lines.size();++i){if(i)out<<L',';out<<L"{\"length\":"<<lines[i].length
        <<L",\"height\":"<<lines[i].height<<L",\"newlineLength\":"<<lines[i].newlineLength<<L'}';}
    out<<L"],\"runs\":["<<collector.runs.str()<<L"]}";
    RegressionIO::WriteText(argv[2],out.str());return 0;
}

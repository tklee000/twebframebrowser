#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#pragma comment(lib,"winmm.lib")

namespace TWebFrame::Internal {
struct RuntimeAudioNode {
    enum class Kind { Destination, Gain, Oscillator, Buffer } kind=Kind::Gain;
    std::vector<std::weak_ptr<RuntimeAudioNode>> inputs;
    double gain=1,frequency=440,detune=0,start=0,stop=INFINITY;
    bool started=false,loop=false;
    unsigned wave=0;
    struct ParameterEvent { double time=0,value=0; };
    std::vector<ParameterEvent> gainEvents,frequencyEvents,detuneEvents;
    static double Parameter(double base,const std::vector<ParameterEvent>& events,double time){
        for(const auto& event:events){if(event.time>time)break;base=event.value;}return base;
    }
    void Schedule(double RuntimeAudioNode::* field,double value,double time){
        auto& events=field==&RuntimeAudioNode::gain?gainEvents:field==&RuntimeAudioNode::frequency?frequencyEvents:detuneEvents;
        auto found=std::lower_bound(events.begin(),events.end(),time,[](const auto& event,double at){return event.time<at;});
        if(found!=events.end()&&found->time==time)found->value=value;else events.insert(found,{time,value});
    }
    std::vector<std::vector<float>> samples;
    double Phase(double time)const{
        double phase=0,at=start;
        while(at<time){double end=time;
            for(const auto& event:frequencyEvents)if(event.time>at){end=std::min(end,event.time);break;}
            for(const auto& event:detuneEvents)if(event.time>at){end=std::min(end,event.time);break;}
            phase+=(end-at)*Parameter(frequency,frequencyEvents,at)*std::pow(2,Parameter(detune,detuneEvents,at)/1200);at=end;
        }return phase;
    }
    double Sample(double time,unsigned channel,unsigned rate,unsigned depth=0)const{
        if(depth>32)return 0;
        if(kind==Kind::Oscillator){
            if(!started||time<start||time>=stop)return 0;
            const double phase=Phase(time),fraction=phase-std::floor(phase);
            if(wave==1)return fraction<0.5?1:-1;if(wave==2)return 2*fraction-1;if(wave==3)return 1-4*std::abs(fraction-0.5);
            return std::sin(phase*6.283185307179586);
        }
        if(kind==Kind::Buffer){
            if(!started||time<start||time>=stop||samples.empty()||samples[0].empty())return 0;
            size_t index=static_cast<size_t>((time-start)*rate);
            if(loop)index%=samples[0].size();if(index>=samples[0].size())return 0;
            return samples[std::min<size_t>(channel,samples.size()-1)][index];
        }
        double sample=0;for(const auto& input:inputs)if(const auto source=input.lock())sample+=source->Sample(time,channel,rate,depth+1);
        return kind==Kind::Gain?sample*Parameter(gain,gainEvents,time):sample;
    }
};
struct RuntimeAudioState {
    unsigned rate=48000,channels=2,offlineFrames=0;
    std::recursive_mutex mutex;
    std::shared_ptr<RuntimeAudioNode> destination=std::make_shared<RuntimeAudioNode>();
    std::vector<std::shared_ptr<RuntimeAudioNode>> nodes;
    std::atomic<int> state{0}; // suspended, running, closed
    std::atomic<uint64_t> frame{0};
    std::thread output;
    RuntimeAudioState(){destination->kind=RuntimeAudioNode::Kind::Destination;}
    ~RuntimeAudioState(){Close();}
    void Close(){state=2;if(output.joinable())output.join();}
    std::vector<std::vector<float>> Render(unsigned count){
        std::lock_guard<std::recursive_mutex> lock(mutex);
        std::vector<std::vector<float>> samples(channels,std::vector<float>(count));const auto first=frame.load();
        for(unsigned i=0;i<count;++i)for(unsigned c=0;c<channels;++c)samples[c][i]=
            static_cast<float>(destination->Sample(static_cast<double>(first+i)/rate,c,rate));
        frame+=count;return samples;
    }
    bool Resume(){
        if(state==2)return false;if(offlineFrames){state=1;return true;}state=1;
        if(output.joinable())return true;
        WAVEFORMATEX format{};format.wFormatTag=WAVE_FORMAT_PCM;format.nChannels=2;format.nSamplesPerSec=rate;
        format.wBitsPerSample=16;format.nBlockAlign=4;format.nAvgBytesPerSec=rate*4;HWAVEOUT device=nullptr;
        if(waveOutOpen(&device,WAVE_MAPPER,&format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){state=0;return false;}
        output=std::thread([this,device]{
            while(state!=2){
                if(state==0){Sleep(4);continue;}const auto samples=Render(512);std::vector<int16_t> pcm(1024);
                for(unsigned i=0;i<512;++i)for(unsigned c=0;c<2;++c)pcm[i*2+c]=static_cast<int16_t>(std::clamp(samples[c][i],-1.0f,1.0f)*32767);
                WAVEHDR header{};header.lpData=reinterpret_cast<LPSTR>(pcm.data());header.dwBufferLength=2048;
                if(waveOutPrepareHeader(device,&header,sizeof(header))!=MMSYSERR_NOERROR)break;
                if(waveOutWrite(device,&header,sizeof(header))!=MMSYSERR_NOERROR){waveOutUnprepareHeader(device,&header,sizeof(header));break;}
                while(!(header.dwFlags&WHDR_DONE)&&state!=2)Sleep(2);
                if(state==2)waveOutReset(device);waveOutUnprepareHeader(device,&header,sizeof(header));
            }
            waveOutReset(device);waveOutClose(device);
        });return true;
    }
};
}

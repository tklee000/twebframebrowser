    Value AudioBufferValue(const std::vector<std::vector<float>>& samples,unsigned rate){
        auto buffer=ObjectValue(ObjectKind::Plain);const size_t length=samples.empty()?0:samples[0].size();
        buffer.object->props[L"length"]=Value::Number(static_cast<double>(length));buffer.object->props[L"sampleRate"]=Value::Number(rate);
        buffer.object->props[L"duration"]=Value::Number(static_cast<double>(length)/rate);buffer.object->props[L"numberOfChannels"]=Value::Number(static_cast<double>(samples.size()));
        buffer.object->props[L"$host:audioBuffer"]=Value::Bool(true);
        for(size_t channel=0;channel<samples.size();++channel){std::vector<Value> values;values.reserve(length);for(float sample:samples[channel])values.push_back(Value::Number(sample));
            buffer.object->props[L"$host:channel:"+std::to_wstring(channel)]=Construct(global->values[L"Float32Array"],{ArrayValue(values)});}
        buffer.object->props[L"getChannelData"]=ObjectNative(buffer.object,[](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
            const unsigned channel=args.empty()?0:r.Uint32(args[0]);const auto found=receiver.object->props.find(L"$host:channel:"+std::to_wstring(channel));
            return found==receiver.object->props.end()?Value::Thrown(r.ErrorValue(L"IndexSizeError",L"Invalid audio channel")):found->second;});
        return buffer;
    }
    Value AudioNodeValue(const std::shared_ptr<RuntimeAudioState>& state,const std::shared_ptr<RuntimeAudioNode>& node){
        auto value=ObjectValue(ObjectKind::Plain);value.object->intlFormatter=node;value.object->nativeStateKind=Object::NativeStateKind::AudioNode;value.object->props[L"$host:audioNode"]=Value::Bool(true);
        value.object->props[L"connect"]=ObjectNative(value.object,[state,node](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.empty()||!args[0].object||args[0].object->nativeStateKind!=Object::NativeStateKind::AudioNode)return Value::Thrown(r.ErrorValue(L"TypeError",L"connect requires an AudioNode"));
            const auto destination=std::static_pointer_cast<RuntimeAudioNode>(args[0].object->intlFormatter);std::lock_guard<std::recursive_mutex> lock(state->mutex);
            if(destination!=state->destination&&std::find(state->nodes.begin(),state->nodes.end(),destination)==state->nodes.end())
                return Value::Thrown(r.ErrorValue(L"InvalidAccessError",L"Audio nodes belong to different contexts"));
            destination->inputs.push_back(node);return args[0];});
        value.object->props[L"disconnect"]=Native([state,node](RuntimeCore&,const Value&,const std::vector<Value>&){
            std::lock_guard<std::recursive_mutex> lock(state->mutex);for(const auto& destination:state->nodes){auto& inputs=destination->inputs;
                inputs.erase(std::remove_if(inputs.begin(),inputs.end(),[&](const auto& input){return input.lock()==node;}),inputs.end());}
            auto& inputs=state->destination->inputs;inputs.erase(std::remove_if(inputs.begin(),inputs.end(),[&](const auto& input){return input.lock()==node;}),inputs.end());return Value::Undefined();});
        const auto parameter=[&](double RuntimeAudioNode::* field){auto param=ObjectValue(ObjectKind::Plain);
            param.object->props[L"$get:value"]=Native([state,node,field](RuntimeCore&,const Value&,const std::vector<Value>&){std::lock_guard<std::recursive_mutex> lock(state->mutex);return Value::Number(node.get()->*field);});
            param.object->props[L"$set:value"]=Native([state,node,field](RuntimeCore& r,const Value&,const std::vector<Value>& args){std::lock_guard<std::recursive_mutex> lock(state->mutex);if(!args.empty())node.get()->*field=r.Number(args[0]);return Value::Undefined();});
            param.object->props[L"setValueAtTime"]=Native([state,node,field](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                if(args.size()<2)return Value::Thrown(r.ErrorValue(L"TypeError",L"setValueAtTime requires value and time"));
                const double value=r.Number(args[0]),time=r.Number(args[1]);
                if(!std::isfinite(value)||!std::isfinite(time)||time<0)return Value::Thrown(r.ErrorValue(L"RangeError",L"Invalid audio parameter event"));
                std::lock_guard<std::recursive_mutex> lock(state->mutex);node->Schedule(field,value,time);return receiver;});return param;};
        if(node->kind==RuntimeAudioNode::Kind::Gain)value.object->props[L"gain"]=parameter(&RuntimeAudioNode::gain);
        if(node->kind==RuntimeAudioNode::Kind::Oscillator){
            value.object->props[L"frequency"]=parameter(&RuntimeAudioNode::frequency);value.object->props[L"detune"]=parameter(&RuntimeAudioNode::detune);
            value.object->props[L"$get:type"]=Native([node](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(node->wave==1?L"square":node->wave==2?L"sawtooth":node->wave==3?L"triangle":L"sine");});
            value.object->props[L"$set:type"]=Native([state,node](RuntimeCore& r,const Value&,const std::vector<Value>& args){const auto type=args.empty()?L"":r.String(args[0]);
                if(type!=L"sine"&&type!=L"square"&&type!=L"sawtooth"&&type!=L"triangle")return Value::Thrown(r.ErrorValue(L"TypeError",L"Invalid oscillator type"));
                std::lock_guard<std::recursive_mutex> lock(state->mutex);node->wave=type==L"square"?1:type==L"sawtooth"?2:type==L"triangle"?3:0;return Value::Undefined();});}
        if(node->kind==RuntimeAudioNode::Kind::Oscillator||node->kind==RuntimeAudioNode::Kind::Buffer){
            value.object->props[L"start"]=ObjectNative(value.object,[state,node](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                std::lock_guard<std::recursive_mutex> lock(state->mutex);if(node->started)return Value::Thrown(r.ErrorValue(L"InvalidStateError",L"Audio source already started"));
                const double time=args.empty()?0:r.Number(args[0]);if(!std::isfinite(time)||time<0)return Value::Thrown(r.ErrorValue(L"RangeError",L"Invalid start time"));
                if(node->kind==RuntimeAudioNode::Kind::Buffer){const auto buffer=r.GetProperty(receiver,L"buffer");
                    if(buffer.object&&buffer.object->props.count(L"$host:audioBuffer")){
                        const unsigned channels=r.Uint32(r.GetProperty(buffer,L"numberOfChannels"));node->samples.resize(channels);
                        for(unsigned c=0;c<channels;++c){const auto values=r.GetProperty(buffer,L"$host:channel:"+std::to_wstring(c));
                            for(size_t i=0;i<values.object->items.size();++i)node->samples[c].push_back(static_cast<float>(r.Number(r.TypedElementRead(values.object,i))));}}
                    node->loop=r.Truth(r.GetProperty(receiver,L"loop"));}
                node->start=std::max(time,static_cast<double>(state->frame)/state->rate);node->started=true;return Value::Undefined();});
            value.object->props[L"stop"]=Native([state,node](RuntimeCore& r,const Value&,const std::vector<Value>& args){std::lock_guard<std::recursive_mutex> lock(state->mutex);
                if(!node->started)return Value::Thrown(r.ErrorValue(L"InvalidStateError",L"Audio source has not started"));node->stop=args.empty()?static_cast<double>(state->frame)/state->rate:r.Number(args[0]);return Value::Undefined();});}
        return value;
    }
    void InstallAudio(){
        for(const auto* name:{L"AudioContext",L"OfflineAudioContext"}){const bool offline=std::wstring(name)==L"OfflineAudioContext";
            auto ctor=Native([offline](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                auto state=std::make_shared<RuntimeAudioState>();
                if(offline){if(args.size()<3)return Value::Thrown(r.ErrorValue(L"TypeError",L"OfflineAudioContext requires channels, length and sampleRate"));
                    state->channels=r.Uint32(args[0]);state->offlineFrames=r.Uint32(args[1]);state->rate=r.Uint32(args[2]);}
                else if(!args.empty()){const auto rate=r.GetProperty(args[0],L"sampleRate");if(rate.type!=Value::Type::Undefined)state->rate=r.Uint32(rate);}
                if(!state->channels||state->channels>32||!state->rate||state->rate<8000||state->rate>192000||state->offlineFrames>10000000)
                    return Value::Thrown(r.ErrorValue(L"NotSupportedError",L"Unsupported audio dimensions"));
                auto context=r.ObjectValue(ObjectKind::Plain);context.object->intlFormatter=state;context.object->nativeStateKind=Object::NativeStateKind::AudioContext;
                context.object->prototype=r.GetProperty(r.global->values[offline?L"OfflineAudioContext":L"AudioContext"],L"prototype").object;
                context.object->props[L"sampleRate"]=Value::Number(state->rate);context.object->props[L"baseLatency"]=Value::Number(512.0/state->rate);
                context.object->props[L"destination"]=r.AudioNodeValue(state,state->destination);
                context.object->props[L"$get:state"]=r.Native([state](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::String(state->state==2?L"closed":state->state==1?L"running":L"suspended");});
                context.object->props[L"$get:currentTime"]=r.Native([state](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Number(static_cast<double>(state->frame)/state->rate);});
                for(const auto* method:{L"resume",L"suspend",L"close"}){const std::wstring op=method;
                    context.object->props[method]=r.Native([state,op](RuntimeCore& r,const Value&,const std::vector<Value>&){
                        if(state->state==2)return r.RejectedPromise(L"InvalidStateError",L"AudioContext is closed");
                        if(op==L"close")state->Close();else if(op==L"suspend")state->state=0;else if(!state->Resume())return r.RejectedPromise(L"NotSupportedError",L"Audio output device is unavailable");
                        return r.PromiseResolveValue(Value::Undefined());});}
                for(const auto* method:{L"createGain",L"createOscillator",L"createBufferSource"}){const std::wstring op=method;
                    context.object->props[method]=r.Native([state,op](RuntimeCore& r,const Value&,const std::vector<Value>&){
                        auto node=std::make_shared<RuntimeAudioNode>();node->kind=op==L"createGain"?RuntimeAudioNode::Kind::Gain:op==L"createOscillator"?RuntimeAudioNode::Kind::Oscillator:RuntimeAudioNode::Kind::Buffer;
                        std::lock_guard<std::recursive_mutex> lock(state->mutex);state->nodes.push_back(node);return r.AudioNodeValue(state,node);});}
                context.object->props[L"createBuffer"]=r.Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                    if(args.size()<3)return Value::Thrown(r.ErrorValue(L"TypeError",L"createBuffer requires three dimensions"));
                    const unsigned channels=r.Uint32(args[0]),length=r.Uint32(args[1]),rate=r.Uint32(args[2]);
                    if(!channels||channels>32||!length||length>10000000||rate<8000||rate>192000)return Value::Thrown(r.ErrorValue(L"NotSupportedError",L"Unsupported audio buffer dimensions"));
                    return r.AudioBufferValue(std::vector<std::vector<float>>(channels,std::vector<float>(length)),rate);});
                if(offline)context.object->props[L"startRendering"]=r.Native([state](RuntimeCore& r,const Value&,const std::vector<Value>&){
                    if(state->state!=0)return r.RejectedPromise(L"InvalidStateError",L"Offline context already rendered");state->state=1;
                    const auto samples=state->Render(state->offlineFrames);state->state=2;return r.PromiseResolveValue(r.AudioBufferValue(samples,state->rate));});
                return context;
            });
            ctor.native->props[L"$requiresNew"]=Value::Bool(true);ctor.native->props[L"name"]=Value::String(name);ctor.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);
            global->values[name]=ctor;GlobalWindowObject()->props[name]=ctor;
        }
    }

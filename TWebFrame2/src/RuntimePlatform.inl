    static std::weak_ptr<void> WeakIdentity(const Value& value){
        if(value.object)return value.object;if(value.function)return value.function;if(value.native)return value.native;return {};
    }
    void DeliverPort(const std::shared_ptr<Object>& port){
        if(!Truth(port->props[L"$host:started"])||Truth(port->props[L"$host:closed"])||port->items.empty())return;
        const auto data=port->items.front();port->items.erase(port->items.begin());
        auto event=CreateObject(ObjectKind::Event);event->props[L"type"]=Value::String(L"message");event->props[L"data"]=data;
        event->props[L"target"]=event->props[L"currentTarget"]=Value::FromObject(port);event->props[L"ports"]=ArrayValue({});
        event->props[L"origin"]=Value::String(L"");event->props[L"source"]=Value::Null();event->eventTrusted=true;
        const auto callback=GetProperty(Value::FromObject(port),L"onmessage");
        if(IsCallable(callback))InvokeCallback(callback,Value::FromObject(port),{Value::FromObject(event)});
        const auto listeners=objectListeners.find(port.get());
        if(listeners!=objectListeners.end())InvokeEventListeners(listeners->second,L"message",false,Value::FromObject(port),Value::FromObject(event),event);
        if(!port->items.empty())EnqueueTask([this,port]{DeliverPort(port);});
    }
    void QueueFinalizationCallbacks(){
        for(auto it=finalizationRegistries.begin();it!=finalizationRegistries.end();){
            const auto registry=it->lock();if(!registry){it=finalizationRegistries.erase(it);continue;}++it;
            for(size_t index=registry->weakCells.size();index>0;--index){const size_t i=index-1;
                if(!registry->weakCells[i].first.expired())continue;
                const auto held=registry->items[i],callback=registry->props[L"$host:cleanup"];
                registry->items.erase(registry->items.begin()+i);registry->weakCells.erase(registry->weakCells.begin()+i);
                EnqueueTask([this,callback,held]{InvokeCallback(callback,Value::Undefined(),{held});});
            }
        }
    }
    void InstallRuntimePlatform(){
        const auto window=GlobalWindowObject();
        const auto expose=[&](const std::wstring& name,Value value,int length,bool requiresNew=true){
            if(value.native){value.native->props[L"name"]=Value::String(name);value.native->props[L"length"]=Value::Number(length);
                if(requiresNew)value.native->props[L"$requiresNew"]=Value::Bool(true);
                if(!value.native->props.count(L"prototype"))value.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);
                value.native->props[L"prototype"].object->props[L"constructor"]=value;}
            global->values[name]=value;window->props[name]=value;return value;
        };
        const auto objectConstructor=global->values[L"Object"];
        const auto getDescriptor=GetProperty(objectConstructor,L"getOwnPropertyDescriptor");
        objectConstructor.object->props[L"getOwnPropertyDescriptors"]=Native([getDescriptor](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.empty()||args[0].type==Value::Type::Null||args[0].type==Value::Type::Undefined)return Value::Thrown(r.ErrorValue(L"TypeError",L"Cannot inspect null or undefined"));
            auto descriptors=r.ObjectValue(ObjectKind::Plain);
            const auto names=r.Call(r.GetProperty(r.global->values[L"Object"],L"getOwnPropertyNames"),Value::Undefined(),{args[0]});
            const auto symbols=r.Call(r.GetProperty(r.global->values[L"Object"],L"getOwnPropertySymbols"),Value::Undefined(),{args[0]});
            names.object->items.insert(names.object->items.end(),symbols.object->items.begin(),symbols.object->items.end());
            for(const auto& name:names.object->items){const auto descriptor=r.Call(getDescriptor,Value::Undefined(),{args[0],name});
                if(descriptor.type!=Value::Type::Undefined)descriptors.object->props[r.String(name)]=descriptor;}
            return descriptors;
        });
        global->values[L"Reflect"].object->props[L"getOwnPropertyDescriptor"]=Native([getDescriptor](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            const auto target=args.empty()?Value::Undefined():r.Deref(args[0]);
            if(target.type!=Value::Type::Object&&target.type!=Value::Type::Function&&target.type!=Value::Type::Native)
                return Value::Thrown(r.ErrorValue(L"TypeError",L"Reflect target must be an object"));
            return r.Call(getDescriptor,Value::Undefined(),args);
        });
        auto registryConstructor=expose(L"FinalizationRegistry",Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.empty()||!r.IsCallable(args[0]))return Value::Thrown(r.ErrorValue(L"TypeError",L"Cleanup callback must be callable"));
            auto registry=r.ObjectValue(ObjectKind::Plain);registry.object->props[L"$host:cleanup"]=args[0];
            registry.object->prototype=r.GetProperty(r.global->values[L"FinalizationRegistry"],L"prototype").object;r.finalizationRegistries.push_back(registry.object);
            registry.object->props[L"register"]=r.ObjectNative(registry.object,[](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                if(args.size()<2||r.WeakIdentity(r.Deref(args[0])).expired())return Value::Thrown(r.ErrorValue(L"TypeError",L"Target must be an object"));
                const auto target=r.Deref(args[0]),held=r.Deref(args[1]);
                if(r.EqualValues(target,held))return Value::Thrown(r.ErrorValue(L"TypeError",L"Target and held value must differ"));
                const auto token=args.size()>2?r.Deref(args[2]):Value::Undefined();
                if(token.type!=Value::Type::Undefined&&r.WeakIdentity(token).expired())return Value::Thrown(r.ErrorValue(L"TypeError",L"Unregister token must be an object"));
                receiver.object->weakCells.emplace_back(r.WeakIdentity(target),r.WeakIdentity(token));receiver.object->items.push_back(held);return Value::Undefined();
            });
            registry.object->props[L"unregister"]=r.ObjectNative(registry.object,[](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                if(args.empty()||r.WeakIdentity(r.Deref(args[0])).expired())return Value::Thrown(r.ErrorValue(L"TypeError",L"Unregister token must be an object"));
                const auto token=r.WeakIdentity(r.Deref(args[0])).lock();bool removed=false;
                for(size_t i=receiver.object->weakCells.size();i>0;--i)if(receiver.object->weakCells[i-1].second.lock()==token){
                    receiver.object->weakCells.erase(receiver.object->weakCells.begin()+i-1);receiver.object->items.erase(receiver.object->items.begin()+i-1);removed=true;}
                return Value::Bool(removed);
            });return registry;
        }),1);
        auto blob=global->values[L"Blob"];if(!blob.native->props.count(L"prototype"))blob.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);
        auto fileConstructor=expose(L"File",Native([blob](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.size()<2)return Value::Thrown(r.ErrorValue(L"TypeError",L"File requires parts and a name"));
            const auto options=args.size()>2?args[2]:r.ObjectValue(ObjectKind::Plain);
            auto file=r.Call(blob,Value::Undefined(),{args[0],options});if(file.abrupt)return file;
            file.object->kind=ObjectKind::File;file.object->prototype=r.GetProperty(r.global->values[L"File"],L"prototype").object;
            file.object->props[L"name"]=Value::String(r.String(args[1]));const auto modified=r.GetProperty(options,L"lastModified");
            file.object->props[L"lastModified"]=Value::Number(modified.type==Value::Type::Undefined?
                std::chrono::duration<double,std::milli>(std::chrono::system_clock::now().time_since_epoch()).count():r.Number(modified));
            return file;
        }),2);
        fileConstructor.native->props[L"prototype"].object->prototype=blob.native->props[L"prototype"].object;
        expose(L"FileList",Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return Value::Thrown(r.ErrorValue(L"TypeError",L"Illegal constructor"));}),0);
        expose(L"MessagePort",Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return Value::Thrown(r.ErrorValue(L"TypeError",L"Illegal constructor"));}),0);
        expose(L"MessageChannel",Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
            auto channel=r.ObjectValue(ObjectKind::Plain);channel.object->prototype=r.GetProperty(r.global->values[L"MessageChannel"],L"prototype").object;
            auto first=r.ObjectValue(ObjectKind::Plain),second=r.ObjectValue(ObjectKind::Plain);
            for(const auto& pair:std::vector<std::pair<Value,Value>>{{first,second},{second,first}}){auto port=pair.first.object;
                port->prototype=r.GetProperty(r.global->values[L"MessagePort"],L"prototype").object;
                port->props[L"$host:port"]=Value::Bool(true);port->props[L"onmessage"]=Value::Null();port->props[L"onmessageerror"]=Value::Null();
                const auto peer=std::weak_ptr<Object>(pair.second.object);
                port->props[L"postMessage"]=r.ObjectNative(port,[peer](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                    if(args.empty())return Value::Thrown(r.ErrorValue(L"TypeError",L"postMessage requires data"));
                    if(r.Truth(receiver.object->props[L"$host:closed"]))return Value::Undefined();
                    const auto target=peer.lock();if(!target||r.Truth(target->props[L"$host:closed"]))return Value::Undefined();
                    std::vector<std::shared_ptr<Object>> transfers;
                    if(args.size()>1){const auto option=r.Deref(args[1]);
                        r.ValidateTransfers(option.object&&option.object->kind!=ObjectKind::Array?r.GetProperty(option,L"transfer"):option,transfers);}
                    auto data=r.CloneValue(args[0]);r.DetachTransfers(transfers);
                    target->items.push_back(data);r.EnqueueTask([&r,target]{r.DeliverPort(target);});return Value::Undefined();
                });
                port->props[L"start"]=r.ObjectNative(port,[](RuntimeCore& r,const Value& receiver,const std::vector<Value>&){
                    receiver.object->props[L"$host:started"]=Value::Bool(true);r.EnqueueTask([&r,port=receiver.object]{r.DeliverPort(port);});return Value::Undefined();});
                port->props[L"close"]=r.ObjectNative(port,[](RuntimeCore&,const Value& receiver,const std::vector<Value>&){
                    receiver.object->props[L"$host:closed"]=Value::Bool(true);receiver.object->items.clear();return Value::Undefined();});
                for(const auto* method:{L"addEventListener",L"removeEventListener"}){const bool add=std::wstring(method)==L"addEventListener";
                    port->props[method]=r.ObjectNative(port,[add](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                        if(add)r.AddEventListener(r.objectListeners[receiver.object.get()],args);else r.RemoveEventListener(r.objectListeners[receiver.object.get()],args);return Value::Undefined();});}
            }
            channel.object->props[L"port1"]=first;channel.object->props[L"port2"]=second;return channel;
        }),0);
        expose(L"OffscreenCanvas",Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.size()<2)return Value::Thrown(r.ErrorValue(L"TypeError",L"OffscreenCanvas requires width and height"));
            const double width=r.Number(args[0]),height=r.Number(args[1]);
            if(!std::isfinite(width)||!std::isfinite(height)||width<0||height<0||width>32768||height>32768)
                return Value::Thrown(r.ErrorValue(L"RangeError",L"Invalid canvas dimensions"));
            auto node=std::make_shared<Node>();node->tag=L"canvas";node->type=NodeType::Element;
            node->SetAttribute(L"width",NumberString(std::floor(width)));node->SetAttribute(L"height",NumberString(std::floor(height)));
            auto canvas=r.NodeValue(node);canvas.object->props[L"$host:offscreen"]=Value::Bool(true);
            canvas.object->prototype=r.GetProperty(r.global->values[L"OffscreenCanvas"],L"prototype").object;
            canvas.object->props[L"convertToBlob"]=r.ObjectNative(canvas.object,[](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                if(!receiver.object->node||!r.EnsureCanvas(receiver.object->node)->width||!r.EnsureCanvas(receiver.object->node)->height)
                    return r.RejectedPromise(L"IndexSizeError",L"Canvas has no pixels");
                auto promise=r.PromiseValue();auto callback=r.Native([promise](RuntimeCore& r,const Value&,const std::vector<Value>& values){
                    if(values.empty()||values[0].type==Value::Type::Null)r.RejectPromise(promise.object,r.ErrorValue(L"EncodingError",L"Canvas encoding failed"));
                    else r.ResolvePromise(promise.object,values[0]);return Value::Undefined();});
                const auto options=args.empty()?r.ObjectValue(ObjectKind::Plain):args[0];
                r.Call(r.GetPropertyValue(receiver,L"toBlob",true),receiver,{callback,r.GetProperty(options,L"type"),r.GetProperty(options,L"quality")});return promise;
            });return canvas;
        }),2);
        for(const auto* name:{L"FileSystemHandle",L"FileSystemDirectoryHandle",L"FileSystemFileHandle",L"StorageManager"})
            expose(name,Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return Value::Thrown(r.ErrorValue(L"TypeError",L"Illegal constructor"));}),0);
        auto navigator=global->values[L"navigator"],storage=ObjectValue(ObjectKind::Plain);
        storage.object->prototype=GetProperty(global->values[L"StorageManager"],L"prototype").object;
        storage.object->props[L"getDirectory"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
            if(!r.IsSecureContext())return r.RejectedPromise(L"SecurityError",L"Origin file storage requires a secure context");
            const auto files=r.browserContext->OriginFiles(r.CurrentOrigin());
            return files?r.PromiseResolveValue(r.OriginFileHandle(files,files->root)):r.RejectedPromise(L"SecurityError",L"Opaque origins have no file storage");
        });
        storage.object->props[L"estimate"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){
            const auto files=r.browserContext->OriginFiles(r.CurrentOrigin());if(!files)return r.RejectedPromise(L"SecurityError",L"Opaque origin");
            std::lock_guard<std::recursive_mutex> lock(files->mutex);auto estimate=r.ObjectValue(ObjectKind::Plain);
            estimate.object->props[L"usage"]=Value::Number(static_cast<double>(files->Usage(files->root)));estimate.object->props[L"quota"]=Value::Number(OriginFileSystem::quota);
            return r.PromiseResolveValue(estimate);});
        for(const auto* method:{L"persist",L"persisted"})storage.object->props[method]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.PromiseResolveValue(Value::Bool(false));});
        navigator.object->props[L"storage"]=storage;
        navigator.object->props[L"sendBeacon"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.empty())return Value::Thrown(r.ErrorValue(L"TypeError",L"sendBeacon requires a URL"));
            auto request=r.RequestData(r.String(args[0]),L"POST",args.size()>1?args[1]:Value::Undefined(),true);
            if(request.url.rfind(L"http://",0)!=0&&request.url.rfind(L"https://",0)!=0)return Value::Thrown(r.ErrorValue(L"TypeError",L"Beacon requires HTTP(S)"));
            if(request.body.size()>65536||r.beaconBytes+request.body.size()>65536||(!r.requestLoader&&!r.asyncRequestLoader))return Value::Bool(false);
            request.credentials=ScriptRequest::Credentials::Include;request.mode=ScriptRequest::Mode::NoCors;
            const size_t size=request.body.size();r.beaconBytes+=size;
            const auto lifetime=std::weak_ptr<EventRuntimeLifetime>(r.eventRuntimeLifetime);
            r.EnqueueTask([&r,request,size,lifetime]{
                if(r.asyncRequestLoader)r.asyncRequestLoader(request,[lifetime,size](ScriptResponse){if(auto alive=lifetime.lock();alive&&alive->core){auto& bytes=alive->core->beaconBytes;bytes=bytes>=size?bytes-size:0;}});
                else{r.requestLoader(request);r.beaconBytes-=size;}
            });return Value::Bool(true);
        });
        InstallTrustedTypes();InstallFeaturePolicy();
#ifdef SUPPORT_WEB_ASSEMBLY
        InstallWasm();
#endif
        InstallAudio();
    }

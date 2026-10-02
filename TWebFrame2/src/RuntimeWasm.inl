    Value WasmValue(uint64_t bits,M3ValueType type){
        if(type==c_m3Type_i32)return Value::Number(static_cast<int32_t>(bits));
        if(type==c_m3Type_i64){auto value=BigInteger(bits);if(bits>>63)value=value.Subtract(BigInteger(1).ShiftLeft(64));return Value::BigInt(std::move(value));}
        if(type==c_m3Type_f32){float value=0;std::memcpy(&value,&bits,4);return Value::Number(value);}
        double value=0;std::memcpy(&value,&bits,8);return Value::Number(value);
    }
    uint64_t WasmBits(const Value& input,M3ValueType type){
        const auto value=Deref(input);if(type==c_m3Type_i32)return Uint32(value);
        if(type==c_m3Type_i64){if(value.type!=Value::Type::BigInt)throw JavaScriptException{ErrorValue(L"TypeError",L"WebAssembly i64 requires BigInt")};
            const auto text=value.bigint->Bitwise(BigInteger(UINT64_MAX),L'&').ToString(16);return _wcstoui64(text.c_str(),nullptr,16);}
        uint64_t bits=0;const double number=Number(value);if(type==c_m3Type_f32){const float single=static_cast<float>(number);std::memcpy(&bits,&single,4);}else std::memcpy(&bits,&number,8);return bits;
    }
    static const void* WasmImportCall(IM3Runtime,IM3ImportContext context,uint64_t* stack,void*){
        const auto binding=static_cast<const RuntimeWasmState::Import*>(context->userdata);
        auto& r=*static_cast<RuntimeCore*>(binding->core);const auto wrapper=std::static_pointer_cast<Object>(binding->state->wrapper.lock());
        if(!wrapper)return "WebAssembly instance is unavailable";
        try{
            const auto function=&binding->state->module->functions[binding->index];const auto returns=m3_GetRetCount(function);
            std::vector<Value> args;for(unsigned i=0;i<m3_GetArgCount(function);++i)args.push_back(r.WasmValue(stack[returns+i],m3_GetArgType(function,i)));
            const auto result=r.Call(wrapper->props[L"$host:wasmImport:"+std::to_wstring(binding->index)],Value::Undefined(),args);
            if(returns)stack[0]=r.WasmBits(result,m3_GetRetType(function,0));return nullptr;
        }catch(const JavaScriptException& exception){wrapper->props[L"$host:wasmException"]=exception.value;return "JavaScript import threw";}
        catch(...){return "WebAssembly import failed";}
    }
    void LinkWasm(const std::shared_ptr<RuntimeWasmState>& state,const std::shared_ptr<Object>& wrapper,const Value& imports,bool validating){
        state->wrapper=wrapper;
        for(unsigned i=0;i<state->module->numFuncImports;++i){auto function=&state->module->functions[i];
            Value callback;
            if(validating)callback=Native([](RuntimeCore&,const Value&,const std::vector<Value>&){return Value::Number(0);});
            else{
                const auto moduleName=Utf8ToWide(function->import.moduleUtf8),name=Utf8ToWide(function->import.fieldUtf8);
                const auto area=GetProperty(imports,moduleName);callback=area.type==Value::Type::Undefined?Value::Undefined():GetProperty(area,name);
                if(!IsCallable(callback))throw JavaScriptException{ErrorValue(L"LinkError",L"Missing WebAssembly import "+moduleName+L"."+name)};
            }
            wrapper->props[L"$host:wasmImport:"+std::to_wstring(i)]=callback;
            auto binding=std::make_unique<RuntimeWasmState::Import>();binding->core=this;binding->state=state.get();binding->index=i;
            const auto signature=state->Signature(function);
            if(const auto error=m3_LinkRawFunctionEx(state->module,function->import.moduleUtf8,function->import.fieldUtf8,signature.c_str(),WasmImportCall,binding.get()))
                throw JavaScriptException{ErrorValue(L"LinkError",Utf8ToWide(error))};state->imports.push_back(std::move(binding));
        }
    }
    Value WasmModuleValue(const Value& input){
        std::vector<unsigned char> bytes;if(!SnapshotBufferSource(input,bytes))return Value::Thrown(ErrorValue(L"TypeError",L"WebAssembly.Module requires a BufferSource"));
        try{
            auto state=std::make_shared<RuntimeWasmState>(bytes);auto object=ObjectValue(ObjectKind::Plain);
            object.object->props[L"$host:wasmModule"]=Value::Bool(true);object.object->intlFormatter=state;object.object->nativeStateKind=Object::NativeStateKind::WasmModule;
            object.object->prototype=GetProperty(GetProperty(global->values[L"WebAssembly"],L"Module"),L"prototype").object;
            // wasm3 validates instruction types when it compiles the module.
            state->Load();LinkWasm(state,object.object,Value::Undefined(),true);
            if(const auto error=m3_CompileModule(state->module))return Value::Thrown(ErrorValue(L"CompileError",Utf8ToWide(error)));
            return object;
        }catch(const JavaScriptException& exception){return Value::Thrown(exception.value);}
        catch(const std::exception& exception){return Value::Thrown(ErrorValue(L"CompileError",Utf8ToWide(exception.what())));}
    }
    Value WasmInstanceValue(const Value& moduleValue,const Value& imports){
        if(!moduleValue.object||moduleValue.object->nativeStateKind!=Object::NativeStateKind::WasmModule)return Value::Thrown(ErrorValue(L"TypeError",L"Expected a WebAssembly.Module"));
        try{
            const auto compiled=std::static_pointer_cast<RuntimeWasmState>(moduleValue.object->intlFormatter);
            auto state=std::make_shared<RuntimeWasmState>(compiled->bytes);auto instance=ObjectValue(ObjectKind::Plain);instance.object->intlFormatter=state;
            instance.object->nativeStateKind=Object::NativeStateKind::WasmInstance;
            instance.object->props[L"$host:wasmInstance"]=Value::Bool(true);instance.object->props[L"$host:imports"]=imports;
            instance.object->prototype=GetProperty(GetProperty(global->values[L"WebAssembly"],L"Instance"),L"prototype").object;
            if(state->module->memoryImported)return Value::Thrown(ErrorValue(L"LinkError",L"Imported WebAssembly memories are not supported"));
            for(unsigned i=0;i<state->module->numGlobals;++i)if(state->module->globals[i].imported)
                return Value::Thrown(ErrorValue(L"LinkError",L"Imported WebAssembly globals are not supported"));
            state->Load();LinkWasm(state,instance.object,imports,false);
            auto exports=ObjectValue(ObjectKind::Plain);exports.object->prototype.reset();instance.object->props[L"exports"]=exports;
            for(const auto& item:state->exports){
                const auto name=Utf8ToWide(item.name);
                if(item.kind==0){
                    auto function=ObjectNative(instance.object,[state,name,owner=instance.object](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                        IM3Function function=nullptr;if(const auto error=m3_FindFunction(&function,state->runtime,WideToUtf8(name).c_str()))return Value::Thrown(r.ErrorValue(L"RuntimeError",Utf8ToWide(error)));
                        const unsigned count=m3_GetArgCount(function);std::vector<uint64_t> bits(count);std::vector<const void*> pointers(count);
                        for(unsigned i=0;i<count;++i){bits[i]=r.WasmBits(i<args.size()?args[i]:Value::Undefined(),m3_GetArgType(function,i));pointers[i]=&bits[i];}
                        RuntimeWasmPollScope poll([&r]{++r.executedInstructions;return (!r.interruptionSignal||!r.interruptionSignal->load())&&(!r.executionYieldHandler||r.executionYieldHandler());});
                        if(const auto error=m3_Call(function,count,pointers.data())){
                            const auto wrapper=std::static_pointer_cast<Object>(state->wrapper.lock());
                            if(wrapper&&wrapper->props.count(L"$host:wasmException")){auto thrown=wrapper->props[L"$host:wasmException"];wrapper->props.erase(L"$host:wasmException");return Value::Thrown(thrown);}
                            return Value::Thrown(r.ErrorValue(L"RuntimeError",Utf8ToWide(error)));}
                        const unsigned returns=m3_GetRetCount(function);if(!returns)return Value::Undefined();std::vector<uint64_t> result(returns);std::vector<const void*> resultPointers(returns);
                        for(unsigned i=0;i<returns;++i)resultPointers[i]=&result[i];if(const auto error=m3_GetResults(function,returns,resultPointers.data()))return Value::Thrown(r.ErrorValue(L"RuntimeError",Utf8ToWide(error)));
                        std::vector<Value> values;for(unsigned i=0;i<returns;++i)values.push_back(r.WasmValue(result[i],m3_GetRetType(function,i)));
                        return returns==1?values[0]:r.ArrayValue(values);
                    });function.native->props[L"$notConstructor"]=Value::Bool(true);exports.object->props[name]=function;
                }else if(item.kind==2){
                    auto memory=ObjectValue(ObjectKind::Plain);memory.object->props[L"$get:buffer"]=Native([state](RuntimeCore& r,const Value& receiver,const std::vector<Value>&){
                        uint32_t size=0;m3_GetMemory(state->runtime,&size,0);
                        const auto cached=receiver.object->props.find(L"$host:buffer");
                        if(cached!=receiver.object->props.end()&&r.Number(r.GetProperty(cached->second,L"byteLength"))==size)return cached->second;
                        if(cached!=receiver.object->props.end()){cached->second.object->props[L"$detached"]=Value::Bool(true);cached->second.object->items.clear();}
                        auto buffer=r.Construct(r.global->values[L"ArrayBuffer"],{Value::Number(size)});
                        buffer.object->externalBytes=[state,size]{uint32_t current=0;const auto bytes=m3_GetMemory(state->runtime,&current,0);return std::make_pair(current==size?bytes:nullptr,current==size?static_cast<size_t>(current):0);};
                        receiver.object->props[L"$host:buffer"]=buffer;return buffer;});exports.object->props[name]=memory;
                }else if(item.kind==3){
                    if(item.index>=state->module->numGlobals)return Value::Thrown(ErrorValue(L"CompileError",L"Invalid global export"));
                    auto global=ObjectValue(ObjectKind::Plain);global.object->props[L"$get:value"]=Native([state,index=item.index](RuntimeCore& r,const Value&,const std::vector<Value>&){
                        const auto value=&state->module->globals[index];uint64_t raw=0;std::memcpy(&raw,&value->intValue,8);return r.WasmValue(raw,static_cast<M3ValueType>(value->type));});exports.object->props[name]=global;
                    global.object->props[L"$set:value"]=Native([state,index=item.index](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                        auto& value=state->module->globals[index];if(!value.isMutable)return Value::Thrown(r.ErrorValue(L"TypeError",L"WebAssembly global is immutable"));
                        const auto raw=r.WasmBits(args.empty()?Value::Undefined():args[0],static_cast<M3ValueType>(value.type));std::memcpy(&value.intValue,&raw,8);return Value::Undefined();});
                }else return Value::Thrown(ErrorValue(L"LinkError",L"WebAssembly table exports are not yet supported"));
            }
            RuntimeWasmPollScope poll([this]{++executedInstructions;return (!interruptionSignal||!interruptionSignal->load())&&(!executionYieldHandler||executionYieldHandler());});
            if(const auto error=m3_RunStart(state->module))return Value::Thrown(ErrorValue(L"RuntimeError",Utf8ToWide(error)));
            return instance;
        }catch(const JavaScriptException& exception){return Value::Thrown(exception.value);}
        catch(const std::exception& exception){return Value::Thrown(ErrorValue(L"LinkError",Utf8ToWide(exception.what())));}
    }
    void InstallWasm(){
        auto wasm=ObjectValue(ObjectKind::Plain);global->values[L"WebAssembly"]=wasm;GlobalWindowObject()->props[L"WebAssembly"]=wasm;
        for(const auto* name:{L"CompileError",L"LinkError",L"RuntimeError"}){
            auto ctor=Native([name](RuntimeCore& r,const Value&,const std::vector<Value>& args){return r.ErrorValue(name,args.empty()?L"":r.String(args[0]));});
            ctor.native->props[L"name"]=Value::String(name);wasm.object->props[name]=ctor;
        }
        auto module=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){return r.WasmModuleValue(args.empty()?Value::Undefined():args[0]);});
        module.native->props[L"$requiresNew"]=Value::Bool(true);module.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);wasm.object->props[L"Module"]=module;
        module.native->props[L"exports"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.empty()||!args[0].object||args[0].object->nativeStateKind!=Object::NativeStateKind::WasmModule)return Value::Thrown(r.ErrorValue(L"TypeError",L"Expected a WebAssembly.Module"));
            const auto state=std::static_pointer_cast<RuntimeWasmState>(args[0].object->intlFormatter);std::vector<Value> result;
            for(const auto& item:state->exports){auto value=r.ObjectValue(ObjectKind::Plain);value.object->props[L"name"]=Value::String(Utf8ToWide(item.name));
                value.object->props[L"kind"]=Value::String(item.kind==0?L"function":item.kind==1?L"table":item.kind==2?L"memory":L"global");result.push_back(value);}return r.ArrayValue(result);});
        auto instance=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){return r.WasmInstanceValue(args.empty()?Value::Undefined():args[0],args.size()>1?args[1]:r.ObjectValue(ObjectKind::Plain));});
        instance.native->props[L"$requiresNew"]=Value::Bool(true);instance.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);wasm.object->props[L"Instance"]=instance;
        wasm.object->props[L"validate"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.empty())return Value::Thrown(r.ErrorValue(L"TypeError",L"validate requires a BufferSource"));std::vector<unsigned char> bytes;
            if(!r.SnapshotBufferSource(args[0],bytes))return Value::Thrown(r.ErrorValue(L"TypeError",L"validate requires a BufferSource"));return Value::Bool(!r.WasmModuleValue(args[0]).abrupt);});
        wasm.object->props[L"compile"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            auto value=r.WasmModuleValue(args.empty()?Value::Undefined():args[0]);if(value.abrupt){value.abrupt=false;auto promise=r.PromiseValue();r.RejectPromise(promise.object,value);return promise;}return r.PromiseResolveValue(value);});
        wasm.object->props[L"instantiate"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            Value module=args.empty()?Value::Undefined():r.Deref(args[0]);const bool compiled=module.object&&module.object->nativeStateKind==Object::NativeStateKind::WasmModule;
            if(!compiled)module=r.WasmModuleValue(module);Value value=module.abrupt?module:r.WasmInstanceValue(module,args.size()>1?args[1]:r.ObjectValue(ObjectKind::Plain));
            if(value.abrupt){value.abrupt=false;auto promise=r.PromiseValue();r.RejectPromise(promise.object,value);return promise;}
            if(!compiled){auto result=r.ObjectValue(ObjectKind::Plain);result.object->props[L"module"]=module;result.object->props[L"instance"]=value;value=result;}return r.PromiseResolveValue(value);});
    }

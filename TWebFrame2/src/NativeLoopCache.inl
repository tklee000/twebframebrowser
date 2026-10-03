// Cache the prepared IR, with conservative dependencies for every ordinary
// value reachable from its invariant inputs and pure helpers. Weak owners keep
// the cache from retaining closures or page buffers after navigation. Numeric
// array cells remain live machine-code reads rather than cached constants.
struct NativeLoopGuardValue {
    Value::Type type=Value::Type::Undefined;
    bool boolean=false;std::uint64_t bits=0;const void* identity=nullptr;
    std::wstring text;
    bool Capture(const Value& value){
        if(value.realm||value.realmOwner||value.abrupt||value.type==Value::Type::Reference||value.type==Value::Type::BigInt)return false;
        type=value.type;boolean=value.boolean;
        if(type==Value::Type::Number)std::memcpy(&bits,&value.number,8);
        if(type==Value::Type::String)text=value.StringText();
        if(type==Value::Type::Object)identity=value.object.get();
        if(type==Value::Type::Function)identity=value.function.get();
        if(type==Value::Type::Native)identity=value.native.get();
        return true;
    }
    bool Matches(const Value& value)const{
        if(value.realm||value.realmOwner||value.abrupt||value.type!=type)return false;
        switch(type){
        case Value::Type::Boolean:return boolean==value.boolean;
        case Value::Type::Number:{std::uint64_t current=0;std::memcpy(&current,&value.number,8);return bits==current;}
        case Value::Type::String:return text==value.StringText();
        case Value::Type::Object:return identity==value.object.get();
        case Value::Type::Function:return identity==value.function.get();
        case Value::Type::Native:return identity==value.native.get();
        default:return true;
        }
    }
};
struct NativeLoopPrepared {
    struct Properties {
        std::weak_ptr<Object> object;std::weak_ptr<Function> function;std::weak_ptr<NativeFunction> native;
        std::vector<std::pair<std::wstring,NativeLoopGuardValue>> values;
        std::vector<std::pair<size_t,NativeLoopGuardValue>> cells;
        const Object* prototype=nullptr;const Object* byteSource=nullptr;
        const Prototype* code=nullptr;const Environment* closure=nullptr;
        size_t length=0,byteOffset=0;ObjectKind kind=ObjectKind::Plain;
        NativeFunction::Intrinsic intrinsic=NativeFunction::Intrinsic::None;
        bool descriptors=false;
    };
    struct Captured {std::weak_ptr<Environment> environment;std::wstring name;NativeLoopGuardValue value;};
    struct Array {std::weak_ptr<Object> owner,storage;};
    NativeLoopProgram program;
    std::vector<NativeLoopGuardValue> locals;
    std::vector<Properties> properties;
    std::vector<Captured> captures;
    std::vector<Array> arrays;
    size_t bytes=0;
};
static constexpr size_t kNativeLoopPreparedBudget=8*1024*1024;
size_t nativeLoopPreparedBytes=0;
std::uint64_t nativeLoopPrepareCacheHits=0,nativeLoopPrepareCacheMisses=0;
unsigned nativeLoopCacheLastReject=0;

std::unique_ptr<NativeLoopPrepared> CacheNativeLoop(const NativeLoopProgram& program){
    nativeLoopCacheLastReject=0;
    const auto reject=[&](unsigned line){if(!nativeLoopCacheLastReject)nativeLoopCacheLastReject=line;return false;};
    auto cache=std::make_unique<NativeLoopPrepared>();
    cache->bytes=sizeof(NativeLoopPrepared)+program.nodes.size()*sizeof(NativeLoopNode);
    std::unordered_set<const void*> visited,visitedInvariant;
    size_t guards=0;
    std::function<bool(const Value&,unsigned,bool)> observe;
    const auto snapshot=[&](const Value& value,NativeLoopGuardValue& guard){
        if(++guards>4096||!guard.Capture(value))return reject(__LINE__);
        cache->bytes+=sizeof(guard)+guard.text.size()*sizeof(wchar_t);
        return cache->bytes<=kNativeLoopPreparedBudget/4;
    };
    observe=[&](const Value& value,unsigned depth,bool invariant)->bool{
        const void* identity=value.object?static_cast<const void*>(value.object.get()):
            value.function?static_cast<const void*>(value.function.get()):static_cast<const void*>(value.native.get());
        if(!identity||!(invariant?visitedInvariant:visited).insert(identity).second)return true;
        if(depth>16)return reject(__LINE__);
        NativeLoopPrepared::Properties guard;
        const FastMap<std::wstring,Value>* properties=nullptr;
        if(value.type==Value::Type::Object&&value.object){
            const auto& object=*value.object;
            if((object.kind!=ObjectKind::Plain&&object.kind!=ObjectKind::Array)||object.externalBytes||
               object.nativeStateKind!=Object::NativeStateKind::None)return reject(__LINE__);
            guard.object=value.object;guard.kind=object.kind;guard.prototype=object.prototype.get();
            guard.byteSource=object.byteSource.lock().get();guard.byteOffset=object.byteOffset;
            guard.length=object.items.size();guard.descriptors=object.hasIndexedDescriptors;properties=&object.props;
            if(const auto backing=object.byteSource.lock())if(!observe(Value::FromObject(backing),depth+1,invariant))return false;
            std::vector<size_t> observed;
            for(const auto& read:program.indexed)if(read.first.object.get()==&object)observed.push_back(read.second);
            std::sort(observed.begin(),observed.end());observed.erase(std::unique(observed.begin(),observed.end()),observed.end());
            const bool port=std::any_of(program.arrays.begin(),program.arrays.end(),[&](const auto& array){return array.owner.object.get()==&object||array.storage.get()==&object;});
            const auto cellCount=!port&&!observed.empty()?observed.size():object.items.size();
            for(size_t cursor=0;cursor<cellCount;++cursor){
                const auto at=!port&&!observed.empty()?observed[cursor]:cursor;
                const auto& cell=object.items[at];
                // Direct numeric ports stay live. A captured helper may also
                // read a numeric cell while resolving another helper, so its
                // reachable arrays require exact numeric dependencies too.
                if(!invariant&&cell.type==Value::Type::Number&&!std::binary_search(observed.begin(),observed.end(),at))continue;
                NativeLoopGuardValue expected;
                if(!snapshot(cell,expected)||!observe(cell,depth+1,invariant))return false;
                guard.cells.push_back({at,std::move(expected)});
            }
        }else if(value.type==Value::Type::Function&&value.function){
            const auto& function=*value.function;
            if(!function.prototype)return reject(__LINE__);
            guard.function=value.function;guard.code=function.prototype.get();guard.closure=function.closure.get();properties=&function.props;
            const auto& prototype=*function.prototype;
            std::unordered_set<std::wstring> captured;
            for(const auto& instruction:prototype.chunk.code)if(instruction.op==Op::LoadReference&&
                instruction.text!=L"this"&&instruction.text!=prototype.selfBindingName&&
                std::find(prototype.parameters.begin(),prototype.parameters.end(),instruction.text)==prototype.parameters.end()&&
                std::find(prototype.chunk.variableDeclarations.begin(),prototype.chunk.variableDeclarations.end(),instruction.text)==prototype.chunk.variableDeclarations.end()&&
                captured.insert(instruction.text).second){
                const auto binding=function.closure?function.closure->FindValue(instruction.text):nullptr;
                NativeLoopPrepared::Captured dependency;dependency.environment=function.closure;dependency.name=instruction.text;
                if(!binding||!snapshot(*binding,dependency.value)||!observe(*binding,depth+1,true))return reject(__LINE__);
                cache->captures.push_back(std::move(dependency));
            }
        }else if(value.type==Value::Type::Native&&value.native){
            guard.native=value.native;guard.intrinsic=value.native->intrinsic;properties=&value.native->props;
        }else return false;
        for(const auto& property:*properties){
            NativeLoopGuardValue expected;
            if(!snapshot(property.second,expected))return false;
            // Unused methods on a helper dictionary are not dependencies of
            // this graph. Actually resolved helpers are observed separately.
            if((invariant||property.second.type!=Value::Type::Function)&&
               !observe(property.second,depth+1,invariant))return false;
            cache->bytes+=property.first.size()*sizeof(wchar_t);
            guard.values.push_back({property.first,std::move(expected)});
        }
        cache->properties.push_back(std::move(guard));return true;
    };
    for(size_t at=0;at<program.locals.size();++at){
        const auto& local=program.locals[at];NativeLoopGuardValue guard;
        if(local.input<0&&static_cast<int>(at)!=program.stringLocal){
            if(!snapshot(*local.binding,guard)||!observe(*local.binding,0,false))return {};
        }
        cache->locals.push_back(std::move(guard));
    }
    for(const auto& helper:program.helpers)if(!observe(helper,0,true))return {};
    // Function.call resolution depends on this prototype's own method, not
    // on the entire graph of constructors reachable through other methods.
    if(intrinsicFunctionPrototype){
        const auto& object=*intrinsicFunctionPrototype;
        NativeLoopPrepared::Properties guard;guard.object=intrinsicFunctionPrototype;
        guard.kind=object.kind;guard.prototype=object.prototype.get();guard.length=object.items.size();
        guard.byteSource=object.byteSource.lock().get();guard.byteOffset=object.byteOffset;guard.descriptors=object.hasIndexedDescriptors;
        for(const auto& property:object.props){NativeLoopGuardValue expected;
            if(!snapshot(property.second,expected))return {};
            guard.values.push_back({property.first,std::move(expected)});
        }
        cache->properties.push_back(std::move(guard));
    }
    cache->program=program;
    std::vector<Value>().swap(cache->program.helpers);
    std::vector<std::pair<Value,size_t>>().swap(cache->program.indexed);
    std::vector<Value>().swap(cache->program.hoistedArrays);
    for(auto& local:cache->program.locals){local.binding=nullptr;local.owner=nullptr;cache->bytes+=sizeof(local)+local.name.size()*sizeof(wchar_t);}
    for(auto& array:cache->program.arrays){
        cache->arrays.push_back({array.owner.object,array.storage});
        cache->bytes+=sizeof(array)+array.characters.size()*sizeof(wchar_t);
        array.owner=Value::Undefined();array.storage.reset();
    }
    // Account for vector capacity and string allocations, including guard
    // metadata. Counting inline string capacity again is conservative.
    cache->bytes=sizeof(*cache)+cache->program.nodes.capacity()*sizeof(NativeLoopNode)+
        cache->program.locals.capacity()*sizeof(NativeLoopProgram::Local)+
        cache->program.arrays.capacity()*sizeof(NativeLoopProgram::Array)+
        cache->program.writes.capacity()*sizeof(NativeLoopProgram::Write)+
        cache->locals.capacity()*sizeof(NativeLoopGuardValue)+
        cache->properties.capacity()*sizeof(NativeLoopPrepared::Properties)+
        cache->captures.capacity()*sizeof(NativeLoopPrepared::Captured)+
        cache->arrays.capacity()*sizeof(NativeLoopPrepared::Array);
    for(const auto& local:cache->program.locals)cache->bytes+=local.name.capacity()*sizeof(wchar_t);
    for(const auto& array:cache->program.arrays)cache->bytes+=array.characters.capacity()*sizeof(wchar_t);
    for(const auto& local:cache->locals)cache->bytes+=local.text.capacity()*sizeof(wchar_t);
    for(const auto& capture:cache->captures)cache->bytes+=(capture.name.capacity()+capture.value.text.capacity())*sizeof(wchar_t);
    for(const auto& guard:cache->properties){
        cache->bytes+=guard.values.capacity()*sizeof(decltype(guard.values)::value_type)+guard.cells.capacity()*sizeof(decltype(guard.cells)::value_type);
        for(const auto& property:guard.values)cache->bytes+=(property.first.capacity()+property.second.text.capacity())*sizeof(wchar_t);
        for(const auto& cell:guard.cells)cache->bytes+=cell.second.text.capacity()*sizeof(wchar_t);
    }
    // Small graphs can be cheaper to prepare than to validate a large helper
    // dictionary. Keep their existing preparation path instead of introducing
    // a cache-validation tax, particularly on character decoders.
    size_t validationWork=cache->locals.size()+cache->captures.size()+cache->properties.size();
    for(const auto& guard:cache->properties)validationWork+=guard.values.size()+guard.cells.size();
    if(validationWork>program.cost*2){nativeLoopCacheLastReject=__LINE__;return {};}
    if(cache->bytes>kNativeLoopPreparedBudget/4)return {};
    return cache;
}

bool RestoreNativeLoop(const std::shared_ptr<ExecutionFrame>& frame,const NativeLoopPrepared& cache,NativeLoopProgram& program){
    for(const auto& dependency:cache.captures){
        const auto environment=dependency.environment.lock();
        const auto value=environment?environment->FindValue(dependency.name):nullptr;
        if(!value||!dependency.value.Matches(*value))return false;
    }
    for(const auto& guard:cache.properties){
        const FastMap<std::wstring,Value>* properties=nullptr;
        const auto object=guard.object.lock();const auto function=guard.function.lock();const auto native=guard.native.lock();
        if(object){
            if(object->kind!=guard.kind||object->prototype.get()!=guard.prototype||object->externalBytes||
               object->nativeStateKind!=Object::NativeStateKind::None||object->items.size()!=guard.length||
               object->hasIndexedDescriptors!=guard.descriptors||object->byteOffset!=guard.byteOffset||
               object->byteSource.lock().get()!=guard.byteSource)return false;
            for(const auto& cell:guard.cells)if(!cell.second.Matches(object->items[cell.first]))return false;
            properties=&object->props;
        }else if(function){
            if(function->prototype.get()!=guard.code||function->closure.get()!=guard.closure)return false;
            properties=&function->props;
        }else if(native){
            if(native->intrinsic!=guard.intrinsic)return false;properties=&native->props;
        }else return false;
        if(properties->size()!=guard.values.size())return false;
        for(const auto& property:guard.values){const auto found=properties->find(property.first);
            if(found==properties->end()||!property.second.Matches(found->second))return false;}
    }
    program=cache.program;
    for(size_t at=0;at<program.locals.size();++at){
        auto& local=program.locals[at];
        for(auto scope=frame->env;scope;scope=scope->parent){
            const auto found=scope->values.find(local.name);if(found==scope->values.end())continue;
            local.binding=&found->second;local.owner=scope.get();break;
        }
        if(!local.binding||local.binding->realm||local.binding->realmOwner||local.binding->abrupt||
           (local.written&&local.owner->immutableBindings.count(local.name)))return false;
        if(local.input>=0){if(local.binding->type!=Value::Type::Number)return false;}
        else if(static_cast<int>(at)==program.stringLocal){if(local.binding->type!=Value::Type::String)return false;}
        else if(!cache.locals[at].Matches(*local.binding))return false;
    }
    for(size_t at=0;at<program.arrays.size();++at){
        auto& array=program.arrays[at];array.owner=Value::FromObject(cache.arrays[at].owner.lock());array.storage=cache.arrays[at].storage.lock();
        if(!array.owner.object||!array.storage||DetachedBuffer(array.owner.object)||DetachedBuffer(array.storage))return false;
    }
    return true;
}

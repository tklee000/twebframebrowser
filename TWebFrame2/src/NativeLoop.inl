// OSR tier for ordinary scalar loops. Symbols and pure helpers are resolved
// from bytecode, never from source/function/site names. Native batches perform
// no calls, allocation or observable property access. A failed bounds/type
// guard commits only completed iterations and resumes at the ordinary VM.
struct NativeLoopNode {
    enum Kind { Constant, Input, Snapshot, Numeric, ReadNumber, ReadCharacter, ReadObject, WriteGuard, Overlay } kind=Constant;
    double constant=0;
    Op operation=Op::NoOp;
    int left=-1,right=-1,port=-1;
    bool integer=false,unsignedInteger=false,finiteConversion=false;
    std::uintptr_t expected=0;int index=-1,writeIndex=-1;
};
static constexpr size_t kNativeLoopMaxNodes=1024;
static constexpr size_t kNativeLoopMaxInstructions=4096;
struct NativeLoopFrame {
    // Every live input is populated below and every temporary is written by
    // the emitter before use. Avoid clearing 20 KiB for each tiny hot loop.
    double values[kNativeLoopMaxNodes];
    double updates[256];
    std::uintptr_t objects[kNativeLoopMaxNodes];
    const Value* arrays[16];
    const wchar_t* characters[16];
    std::uint64_t lengths[16];
    wchar_t* output=nullptr;
    std::uint32_t count=0,completed=0,stopped=0;
};
struct NativeLoopCode {
    using Entry=void(*)(NativeLoopFrame*);
    void* memory=nullptr;size_t bytes=0,allocationSize=0;Entry entry=nullptr;
    ~NativeLoopCode(){if(memory)VirtualFree(memory,0,MEM_RELEASE);}
};
struct NativeLoopProgram {
    struct Local {std::wstring name;Value* binding=nullptr;Environment* owner=nullptr;int input=-1,current=-1;bool written=false;};
    struct Array {Value owner;std::shared_ptr<Object> storage;size_t offset=0,length=0;std::vector<wchar_t> characters;bool unsignedBytes=false,containsObjects=false;};
    std::vector<NativeLoopNode> nodes;
    std::vector<Local> locals;
    std::vector<Array> arrays;
    std::vector<Value> helpers;
    std::vector<std::pair<Value,size_t>> indexed;
    std::vector<Value> hoistedArrays;
    struct Write {int port=-1,index=-1,value=-1;};
    std::vector<Write> writes;
    int condition=-1,append=-1,stringLocal=-1;
    size_t conditionEnd=0,exit=0,cost=0;
};
#include "NativeLoopCache.inl"
struct NativeLoopPlan {
    size_t warm=0,nextWarm=64,nextCacheWarm=0;
    std::uint64_t lastUse=0;
    unsigned reject=0,operation=0;size_t offset=0,instructions=0;
    std::vector<std::pair<std::vector<std::uint64_t>,std::shared_ptr<NativeLoopCode>>> code;
    std::unique_ptr<NativeLoopPrepared> prepared;
};
std::map<std::pair<std::uint64_t,size_t>,NativeLoopPlan> nativeLoopPlans;
std::uint64_t nativeLoopCalls=0,nativeLoopIterations=0,nativeLoopGuardExits=0;unsigned nativeLoopLastReject=0;
unsigned nativeLoopLastOperation=0;size_t nativeLoopLastOffset=0;
std::uint64_t nativeLoopCompilations=0,nativeLoopCacheLimitExits=0;
std::uint64_t nativeLoopHeaderEntries=0;
std::uint64_t nativeLoopPlanSequence=0,nativeLoopPlanEvictions=0;

bool EvictNativeLoopPlan(const NativeLoopPlan* protectedPlan=nullptr){
    auto oldest=nativeLoopPlans.end();
    for(auto candidate=nativeLoopPlans.begin();candidate!=nativeLoopPlans.end();++candidate){
        if(&candidate->second==protectedPlan)continue;
        if(oldest==nativeLoopPlans.end()||candidate->second.lastUse<oldest->second.lastUse)oldest=candidate;
    }
    if(oldest==nativeLoopPlans.end())return false;
    for(const auto& code:oldest->second.code)jitAllocatedCodeBytes-=code.second->allocationSize;
    if(oldest->second.prepared)nativeLoopPreparedBytes-=oldest->second.prepared->bytes;
    nativeLoopPlans.erase(oldest);++nativeLoopPlanEvictions;return true;
}

std::wstring NativeLoopRejectionsJson()const{
    std::wstring result=L"[";bool first=true;
    for(const auto& entry:nativeLoopPlans){const auto& plan=entry.second;if(!plan.reject)continue;
        if(!first)result+=L",";first=false;
        result+=L"{\"line\":"+std::to_wstring(plan.reject)+L",\"operation\":"+std::to_wstring(plan.operation)+
            L",\"offset\":"+std::to_wstring(plan.offset)+L",\"instructions\":"+std::to_wstring(plan.instructions)+L"}";
    }return result+L"]";
}

bool RejectNativeLoop(unsigned line){if(!nativeLoopLastReject)nativeLoopLastReject=line;return false;}
static bool NativeLoopComparison(Op operation){
    return operation==Op::Less||operation==Op::LessEqual||operation==Op::Greater||operation==Op::GreaterEqual;
}
static double NativeLoopBinary(Op operation,double left,double right){
    switch(operation){
    case Op::Less:return left<right?1:0;case Op::LessEqual:return left<=right?1:0;
    case Op::Greater:return left>right?1:0;case Op::GreaterEqual:return left>=right?1:0;
    default:return NumberBinary(operation,left,right);
    }
}
bool NativeLoopHelperSafe(const Value& value,const NativeLoopProgram& program,unsigned depth=0){
    if(depth>8||value.realm||value.type!=Value::Type::Function||!value.function||!value.function->prototype)return RejectNativeLoop(__LINE__);
    const auto& prototype=*value.function->prototype;
    if(prototype.isAsync||prototype.isGenerator||!prototype.restParameter.empty()||!prototype.bindings.empty()||
       !prototype.chunk.handlers.empty()||!prototype.chunk.functionDeclarations.empty())return RejectNativeLoop(__LINE__);
    for(const auto& instruction:prototype.chunk.code)if(instruction.op==Op::LoadReference){
        if(std::find(prototype.parameters.begin(),prototype.parameters.end(),instruction.text)!=prototype.parameters.end()||
           std::find(prototype.chunk.variableDeclarations.begin(),prototype.chunk.variableDeclarations.end(),instruction.text)!=prototype.chunk.variableDeclarations.end())continue;
        const auto binding=value.function->closure?value.function->closure->FindValue(instruction.text):nullptr;
        for(const auto& local:program.locals)if(binding==local.binding&&
            binding->type!=Value::Type::Object&&binding->type!=Value::Type::Function&&binding->type!=Value::Type::Native)
            return RejectNativeLoop(__LINE__);
        if(binding&&binding->type==Value::Type::Function&&binding->function!=value.function&&
           !NativeLoopHelperSafe(*binding,program,depth+1))return RejectNativeLoop(__LINE__);
    }
    return true;
}
bool PrepareNativeLoop(const std::shared_ptr<ExecutionFrame>& frame,size_t end,NativeLoopProgram& program){
    const auto& chunk=*frame->chunk;
    struct CaptureReads {
        std::vector<std::pair<Value,size_t>>*& target;std::vector<std::pair<Value,size_t>>* previous;
        CaptureReads(std::vector<std::pair<Value,size_t>>*& pointer,std::vector<std::pair<Value,size_t>>& reads):target(pointer),previous(pointer){target=&reads;}
        ~CaptureReads(){target=previous;}
    } capture(nativeLoopIndexReads,program.indexed);
    if(!chunk.handlers.empty()||chunk.code[end].argument<0)return RejectNativeLoop(__LINE__);
    const auto start=static_cast<size_t>(chunk.code[end].argument);
    if(start>=end||end-start>kNativeLoopMaxInstructions)return RejectNativeLoop(__LINE__);
    // The compiler places for-loop updates before the body. Follow its edges
    // through the complete region instead of assuming physical instruction
    // order is execution order.
    size_t regionEnd=end;
    for(size_t ip=start;ip<=end;++ip)if(chunk.code[ip].op==Op::JumpFalse&&chunk.code[ip].argument>static_cast<int>(end)){
        regionEnd=static_cast<size_t>(chunk.code[ip].argument)-1;break;
    }
    if(regionEnd>=chunk.code.size()||regionEnd-start>kNativeLoopMaxInstructions)return RejectNativeLoop(__LINE__);
    struct Item {
        enum Kind { Ordinary, Number, Boolean, Character, LocalReference, TemporaryReference, MemberReference, Accumulator, Appended } kind=Ordinary;
        Value value,receiver;int node=-1,local=-1,base=-1,key=-1;std::wstring name;
    };
    std::vector<Item> stack,members,temporaries;
    const auto emit=[&](NativeLoopNode node){program.nodes.push_back(node);return static_cast<int>(program.nodes.size()-1);};
    const auto number=[&](double value){Item result;result.kind=Item::Number;result.node=emit({NativeLoopNode::Constant,value});return result;};
    const auto snapshot=[&](double value){Item result;result.kind=Item::Number;result.node=emit({NativeLoopNode::Snapshot,value});return result;};
    // Collect the complete local dependency set before resolving any helper.
    for(size_t ip=start;ip<=regionEnd;++ip)if(chunk.code[ip].op==Op::LoadReference){
        const auto& name=chunk.code[ip].text;
        if(std::find_if(program.locals.begin(),program.locals.end(),[&](const auto& local){return local.name==name;})!=program.locals.end())continue;
        for(auto scope=frame->env;scope;scope=scope->parent){
            auto found=scope->values.find(name);if(found==scope->values.end())continue;
            if(found->second.realm||found->second.realmOwner||found->second.abrupt||found->second.type==Value::Type::Reference)return RejectNativeLoop(__LINE__);
            if(program.locals.size()>=256)return RejectNativeLoop(__LINE__);
            program.locals.push_back({name,&found->second,scope.get()});break;
        }
    }
    const auto binary=[&](Op op,Item a,Item b)->Item{
        Item result;result.kind=NativeLoopComparison(op)?Item::Boolean:Item::Number;
        if(program.nodes[a.node].kind==NativeLoopNode::Constant&&program.nodes[b.node].kind==NativeLoopNode::Constant){
            const auto left=program.nodes[a.node].constant,right=program.nodes[b.node].constant;
            auto folded=number(NativeLoopBinary(op,left,right));
            if(NativeLoopComparison(op))folded.kind=Item::Boolean;return folded;
        }
        result.node=emit({NativeLoopNode::Numeric,0,op,a.node,b.node});return result;
    };
    const auto localValue=[&](int index)->Item{
        auto& local=program.locals[index];Item result;result.local=index;
        if(local.binding->type==Value::Type::Number){
            result.kind=Item::Number;
            if(local.current<0){local.input=emit({NativeLoopNode::Input,0,Op::NoOp,-1,-1,index});local.current=local.input;}
            result.node=local.current;
        }else if(local.written){result.kind=Item::Accumulator;}
        else result.value=*local.binding;
        return result;
    };
    const auto arrayPort=[&](const Value& base,bool characters)->int{
        if(base.realm||base.realmOwner||base.abrupt||!base.object||base.object->kind!=ObjectKind::Array||
           base.object->hasIndexedDescriptors||base.object->externalBytes||DetachedBuffer(base.object))return -1;
        // Preparation executes no JavaScript. A port already validated in
        // this preparation retains the same storage and character data.
        for(size_t at=0;at<program.arrays.size();++at)if(program.arrays[at].owner.object==base.object&&
            (!program.arrays[at].characters.empty())==characters)return static_cast<int>(at);
        NativeLoopProgram::Array port;port.owner=base;port.storage=base.object;port.length=base.object->items.size();
        if(base.object->props.count(L"$typedArrayBits")){
            port.unsignedBytes=true;
            const auto exact=[&](const wchar_t* name,double expected){const auto found=base.object->props.find(name);return found!=base.object->props.end()&&found->second.type==Value::Type::Number&&found->second.number==expected;};
            const auto no=[&](const wchar_t* name){const auto found=base.object->props.find(name);return found!=base.object->props.end()&&found->second.type==Value::Type::Boolean&&!found->second.boolean;};
            if(characters||!exact(L"$typedArrayBits",8)||!no(L"$typedArrayFloating")||!no(L"$typedArraySigned")||!no(L"$typedArrayClamped"))return -1;
            if(const auto storage=base.object->byteSource.lock()){
                if(storage->externalBytes||storage->hasIndexedDescriptors||DetachedBuffer(storage))return -1;
                const auto bits=storage->props.find(L"$typedArrayBits");
                if(bits!=storage->props.end()&&(bits->second.type!=Value::Type::Number||bits->second.number!=8||
                   (storage->props.count(L"$typedArrayFloating")&&Truth(storage->props.find(L"$typedArrayFloating")->second))||
                   (storage->props.count(L"$typedArraySigned")&&Truth(storage->props.find(L"$typedArraySigned")->second))))return -1;
                port.storage=storage;port.offset=base.object->byteOffset;
            }
            if(port.offset>port.storage->items.size()||port.length>port.storage->items.size()-port.offset)return -1;
        }else{
            if(!base.object->props.empty()||base.object->prototype!=arrayPrototype)return -1;
            // Own occupied elements have no inherited lookup. Every native read
            // checks its bounds and Number tag; a hole resumes the VM.
            if(characters){
                if(port.length>65536)return -1;
                port.characters.reserve(port.length);
                for(const auto& value:base.object->items){
                    if(value.realm||value.realmOwner||value.abrupt||value.type!=Value::Type::String||value.StringText().size()!=1)return -1;
                    port.characters.push_back(value.StringText()[0]);
                }
            }else port.containsObjects=std::any_of(base.object->items.begin(),base.object->items.end(),
                [](const Value& value){return value.type==Value::Type::Object;});
        }
        for(size_t at=0;at<program.arrays.size();++at)if(program.arrays[at].owner.object==base.object&&
            program.arrays[at].characters.empty()==port.characters.empty())return static_cast<int>(at);
        if(program.arrays.size()>=16)return -1;
        program.arrays.push_back(std::move(port));return static_cast<int>(program.arrays.size()-1);
    };
    const auto knownNumber=[&](int root,double& result)->bool{
        std::vector<double> values(program.nodes.size());
        for(size_t at=0;at<=static_cast<size_t>(root);++at){
            const auto& node=program.nodes[at];
            switch(node.kind){
            case NativeLoopNode::Constant:values[at]=node.constant;break;
            case NativeLoopNode::Snapshot:values[at]=node.constant;break;
            case NativeLoopNode::Input:values[at]=program.locals[node.port].binding->number;break;
            case NativeLoopNode::Numeric:values[at]=NativeLoopBinary(node.operation,values[node.left],values[node.right]);break;
            case NativeLoopNode::ReadNumber:{
                const auto& array=program.arrays[node.port];const auto index=values[node.left];
                if(!std::isfinite(index)||index<0||index!=std::trunc(index)||index>=array.length)return false;
                const auto& value=array.storage->items[array.offset+static_cast<size_t>(index)];
                if(value.realm||value.abrupt||value.type!=Value::Type::Number)return false;values[at]=value.number;break;
            }
            case NativeLoopNode::Overlay:values[at]=values[node.index]==values[node.writeIndex]?values[node.right]:values[node.left];break;
            case NativeLoopNode::ReadObject:case NativeLoopNode::WriteGuard:break;
            default:return false;
            }
        }
        result=values[root];return true;
    };
    const auto read=[&](Item item,Item& result)->bool{
        if(item.kind==Item::LocalReference){result=localValue(item.local);return true;}
        if(item.kind==Item::TemporaryReference){result=temporaries[item.local];return true;}
        if(item.kind!=Item::MemberReference){result=std::move(item);return true;}
        const auto& base=members[item.base];const auto& key=members[item.key];
        if(base.kind!=Item::Ordinary)return RejectNativeLoop(__LINE__);
        bool constantKey=key.kind==Item::Ordinary;
        Value keyValue=key.value;
        if(key.kind==Item::Number&&program.nodes[key.node].kind==NativeLoopNode::Constant){constantKey=true;keyValue=Value::Number(program.nodes[key.node].constant);}
        if(constantKey){
            if(keyValue.realm||(keyValue.type!=Value::Type::Number&&keyValue.type!=Value::Type::String))return RejectNativeLoop(__LINE__);
            const auto name=String(keyValue);
            if(TransformOrdinaryMember(base.value,name,result.value)){
                result.receiver=base.value;
                if(result.value.type==Value::Type::Number)result=snapshot(result.value.number);
                return true;
            }
            if(name==L"call"&&base.value.function&&!base.value.function->props.count(L"$get:call")&&
               !base.value.function->props.count(L"$prototypeValue")&&intrinsicFunctionPrototype){
                const auto method=intrinsicFunctionPrototype->props.find(name);
                if(method!=intrinsicFunctionPrototype->props.end()&&method->second.native&&
                   method->second.native->intrinsic==NativeFunction::Intrinsic::FunctionCall){
                    result.value=method->second;result.receiver=base.value;return true;
                }
            }
            if(base.value.object&&base.value.object->kind==ObjectKind::Array&&base.value.object->props.empty()&&
               !base.value.object->hasIndexedDescriptors&&base.value.object->prototype==arrayPrototype&&
               keyValue.type==Value::Type::Number&&keyValue.number>=0&&std::floor(keyValue.number)==keyValue.number&&
               keyValue.number<static_cast<double>(base.value.object->items.size())){
                // Numeric cells and object-valued cells stay live in native
                // code, even when their index is constant.
            }
            if(keyValue.type!=Value::Type::Number)return RejectNativeLoop(__LINE__);
        }
        Item numericKey=key;
        if(constantKey)numericKey=number(keyValue.number);
        if(numericKey.kind!=Item::Number)return RejectNativeLoop(__LINE__);
        const bool characters=base.value.object&&!base.value.object->items.empty()&&base.value.object->items[0].type==Value::Type::String;
        const auto port=arrayPort(base.value,characters);if(port<0)return RejectNativeLoop(__LINE__);
        // Apply every pending store in order. An earlier constant-index store
        // can be overwritten by a later dynamic index that aliases this read.
        double observed=0;
        if(!characters&&program.arrays[port].containsObjects&&knownNumber(numericKey.node,observed)&&observed>=0&&std::isfinite(observed)&&observed==std::trunc(observed)&&
           observed<static_cast<double>(program.arrays[port].length)){
            const auto& value=program.arrays[port].storage->items[program.arrays[port].offset+static_cast<size_t>(observed)];
            if(value.type==Value::Type::Object&&value.object&&!value.realm&&!value.realmOwner&&!value.abrupt){
                std::uintptr_t storedPointer=0;std::memcpy(&storedPointer,&value.object,sizeof(void*));
                if(storedPointer!=reinterpret_cast<std::uintptr_t>(value.object.get()))return false;
                NativeLoopNode guard{NativeLoopNode::ReadObject,0,Op::NoOp,numericKey.node,-1,port};
                guard.expected=reinterpret_cast<std::uintptr_t>(value.object.get());emit(guard);result.value=value;return true;
            }
        }
        result.kind=characters?Item::Character:Item::Number;
        result.node=emit({characters?NativeLoopNode::ReadCharacter:NativeLoopNode::ReadNumber,0,Op::NoOp,numericKey.node,-1,port});
        if(!characters)for(const auto& write:program.writes)if(write.port==port){
            NativeLoopNode overlay{NativeLoopNode::Overlay,0,Op::NoOp,result.node,write.value};
            overlay.index=numericKey.node;overlay.writeIndex=write.index;result.node=emit(overlay);
        }
        return true;
    };
    const auto store=[&](const Item& target,const Item& value)->bool{
        if(value.kind!=Item::Number||target.kind!=Item::MemberReference)return false;
        const auto& base=members[target.base];const auto& key=members[target.key];
        if(base.kind!=Item::Ordinary||key.kind!=Item::Number||!base.value.object||
           base.value.object->kind!=ObjectKind::Array||!base.value.object->props.empty()||
           base.value.object->hasIndexedDescriptors||base.value.object->prototype!=arrayPrototype)return false;
        const auto port=arrayPort(base.value,false);if(port<0||program.writes.size()>=8)return false;
        emit({NativeLoopNode::WriteGuard,0,Op::NoOp,key.node,value.node,port});
        program.writes.push_back({port,key.node,value.node});return true;
    };
    const auto pop=[&](){auto result=std::move(stack.back());stack.pop_back();return result;};
    const auto take=[&](Item& value){if(stack.empty())return RejectNativeLoop(__LINE__);return read(pop(),value);};
    std::function<bool(Item,std::vector<Item>,Item&,unsigned)> call;
    std::function<bool(const Value&,const Value&,const std::vector<Item>&,Item&,unsigned)> inlineFunction;
    call=[&](Item callee,std::vector<Item> arguments,Item& result,unsigned depth)->bool{
        if(depth>8)return false;
        if(callee.kind!=Item::Ordinary||callee.value.realm)return RejectNativeLoop(__LINE__);
        if(callee.value.function&&std::none_of(program.helpers.begin(),program.helpers.end(),[&](const auto& helper){return helper.function==callee.value.function;}))
            program.helpers.push_back(callee.value);
        if(callee.value.native&&callee.value.native->intrinsic==NativeFunction::Intrinsic::FunctionCall){
            if(arguments.empty()||arguments[0].kind!=Item::Ordinary)return false;
            const auto receiver=arguments[0].value;arguments.erase(arguments.begin());
            return inlineFunction(callee.receiver,receiver,arguments,result,depth+1);
        }
        if(callee.value.native&&callee.value.native->intrinsic==NativeFunction::Intrinsic::FromCharCode){
            if(arguments.size()!=1||arguments[0].kind!=Item::Number)return RejectNativeLoop(__LINE__);
            result=arguments[0];result.kind=Item::Character;return true;
        }
        if(!NativeLoopHelperSafe(callee.value,program))return RejectNativeLoop(__LINE__);
        TransformResolution resolution;TransformValue function;function.value=callee.value;function.receiver=callee.receiver;
        std::vector<TransformValue> values;std::vector<int> mapped;
        for(const auto& argument:arguments){
            TransformValue value;
            if(argument.kind==Item::Ordinary)value.value=argument.value;
            else if(argument.kind==Item::Number){
                const auto& node=program.nodes[argument.node];
                if(node.kind==NativeLoopNode::Constant)value.value=Value::Number(node.constant);
                else{
                    value.kind=TransformValue::Numeric;value.expression=static_cast<int>(resolution.numbers.size());
                    resolution.numbers.push_back({TransformNumber::Index});mapped.push_back(argument.node);
                }
            }else return RejectNativeLoop(__LINE__);
            values.push_back(std::move(value));
        }
        const auto firstRead=program.indexed.size();
        TransformValue returned;if(!ResolveTransformCall(resolution,function,std::move(values),returned,0)){
            // A frame snapshot can select a pure string/function table entry.
            // Numeric results still use symbolic inputs, never sampled data
            // embedded into a cached machine-code graph.
            std::vector<TransformValue> fixed;bool invariant=true;
            for(const auto& argument:arguments){TransformValue value;
                if(argument.kind==Item::Ordinary)value.value=argument.value;
                else if(argument.kind==Item::Number&&(program.nodes[argument.node].kind==NativeLoopNode::Constant||program.nodes[argument.node].kind==NativeLoopNode::Snapshot))
                    value.value=Value::Number(program.nodes[argument.node].constant);
                else {invariant=false;break;}
                fixed.push_back(std::move(value));
            }
            TransformResolution probe;TransformValue sampled;
            if(invariant&&ResolveTransformCall(probe,function,std::move(fixed),sampled,0)&&sampled.kind==TransformValue::Ordinary&&
               (sampled.value.type==Value::Type::String||sampled.value.type==Value::Type::Function||sampled.value.type==Value::Type::Native)){
                result.value=sampled.value;return true;
            }
            return inlineFunction(callee.value,callee.receiver,arguments,result,depth+1);
        }
        for(size_t at=mapped.size();at<resolution.numbers.size();++at){
            const auto& node=resolution.numbers[at];
            if(node.kind==TransformNumber::Constant)mapped.push_back(number(node.number).node);
            else if(node.kind==TransformNumber::Binary){Item left,right;left.kind=right.kind=Item::Number;left.node=mapped[node.left];right.node=mapped[node.right];mapped.push_back(binary(node.operation,left,right).node);}
            else return RejectNativeLoop(__LINE__);
        }
        if(returned.kind==TransformValue::Ordinary){
            // Read-plan results may depend on cells written by this loop.
            // Keep numeric/object reads live instead of folding the sampled
            // value for the entire native batch.
            if(returned.value.type==Value::Type::Number||returned.value.type==Value::Type::Object)
                return inlineFunction(callee.value,callee.receiver,arguments,result,depth+1);
            result.value=returned.value;return !result.value.realm;
        }
        if(returned.kind!=TransformValue::Numeric&&returned.kind!=TransformValue::Index&&returned.kind!=TransformValue::LessThan)return RejectNativeLoop(__LINE__);
        // A nested read-plan call may have contributed a numeric constant to
        // this expression. It cannot be hoisted over stores to the same array.
        for(size_t at=firstRead;at<program.indexed.size();++at){const auto& read=program.indexed[at];
            if(read.first.object&&read.second<read.first.object->items.size()&&read.first.object->items[read.second].type==Value::Type::Number)
                program.hoistedArrays.push_back(read.first);
        }
        if(returned.expression<0||static_cast<size_t>(returned.expression)>=mapped.size())return RejectNativeLoop(__LINE__);
        result.kind=returned.kind==TransformValue::LessThan?Item::Boolean:Item::Number;
        result.node=mapped[returned.expression];return true;
    };
    inlineFunction=[&](const Value& callee,const Value& receiver,const std::vector<Item>& arguments,Item& result,unsigned depth)->bool{
        if(depth>8||callee.realm||!callee.function||!callee.function->prototype||!NativeLoopHelperSafe(callee,program))return false;
        if(std::none_of(program.helpers.begin(),program.helpers.end(),[&](const auto& helper){return helper.function==callee.function;}))program.helpers.push_back(callee);
        auto& prototype=*callee.function->prototype;
        if(prototype.lexicalThis||!PrepareFastScope(prototype)||
           std::any_of(prototype.parameterDefaults.begin(),prototype.parameterDefaults.end(),[](const auto& value){return bool(value);}))return false;
        std::map<std::wstring,int> names;
        for(const auto& name:prototype.fastLocalNames){names[name]=static_cast<int>(temporaries.size());temporaries.emplace_back();}
        if(prototype.fastThisSlot>=0){Item item;item.value=receiver;temporaries[names[L"this"]]=std::move(item);}
        if(!prototype.selfBindingName.empty()){Item item;item.value=callee;temporaries[names[prototype.selfBindingName]]=std::move(item);}
        for(size_t at=0;at<prototype.parameters.size();++at)
            if(at<arguments.size())temporaries[names[prototype.parameters[at]]]=arguments[at];
        std::vector<Item> operands;
        const auto popOperand=[&](){auto item=std::move(operands.back());operands.pop_back();return item;};
        const auto takeOperand=[&](Item& item){return !operands.empty()&&read(popOperand(),item);};
        for(const auto& instruction:prototype.chunk.code){
            if(program.nodes.size()>kNativeLoopMaxNodes-16)return false;
            switch(instruction.op){
            case Op::NoOp:break;
            case Op::LoadReference:{
                Item item;const auto name=names.find(instruction.text);
                if(name!=names.end()){item.kind=Item::TemporaryReference;item.local=name->second;}
                else{const auto binding=callee.function->closure?callee.function->closure->FindValue(instruction.text):nullptr;
                    if(!binding||binding->realm||binding->realmOwner||binding->abrupt||binding->type==Value::Type::Reference)return false;
                    item.value=*binding;if(item.value.type==Value::Type::Number)item=snapshot(item.value.number);}
                operands.push_back(std::move(item));break;
            }
            case Op::Constant:{Item item;item.value=prototype.chunk.constants[instruction.argument];
                if(item.value.type==Value::Type::Number)item=number(item.value.number);operands.push_back(std::move(item));break;}
            case Op::Undefined:{operands.emplace_back();break;}
            case Op::GetValue:case Op::PrepareCall:{Item item;if(!takeOperand(item))return false;operands.push_back(std::move(item));break;}
            case Op::Duplicate:if(operands.empty())return false;operands.push_back(operands.back());break;
            case Op::Pop:{Item item;if(!takeOperand(item))return false;break;}
            case Op::GetProperty:case Op::GetIndex:{
                Item key,base;if(instruction.op==Op::GetIndex){if(!takeOperand(key))return false;}else key.value=Value::String(instruction.text);
                if(!takeOperand(base))return false;Item item;item.kind=Item::MemberReference;
                item.base=static_cast<int>(members.size());members.push_back(std::move(base));
                item.key=static_cast<int>(members.size());members.push_back(std::move(key));operands.push_back(std::move(item));break;
            }
            case Op::Assign:{
                if(operands.size()<2)return false;Item value;if(!takeOperand(value))return false;const auto target=popOperand();
                if(target.kind==Item::TemporaryReference)temporaries[target.local]=value;
                else if(!store(target,value))return false;
                operands.push_back(std::move(value));break;
            }
            case Op::Declare:{Item value;if(!takeOperand(value)||!names.count(instruction.text))return false;temporaries[names[instruction.text]]=std::move(value);break;}
            case Op::PostIncrement:case Op::PreIncrement:case Op::PostDecrement:case Op::PreDecrement:{
                if(operands.empty())return false;const auto target=popOperand();
                if(target.kind!=Item::TemporaryReference)return false;const auto old=temporaries[target.local];
                if(old.kind!=Item::Number)return false;
                const auto next=binary(instruction.op==Op::PreIncrement||instruction.op==Op::PostIncrement?Op::Add:Op::Subtract,old,number(1));
                temporaries[target.local]=next;operands.push_back(instruction.op==Op::PostIncrement||instruction.op==Op::PostDecrement?old:next);break;
            }
            case Op::Call:{
                if(instruction.argument<0||instruction.argument>4||instruction.text==L"direct-eval"||operands.size()<static_cast<size_t>(instruction.argument)+1)return false;
                std::vector<Item> nested(instruction.argument);for(int at=instruction.argument-1;at>=0;--at)if(!takeOperand(nested[at]))return false;
                Item function,returned;if(!takeOperand(function)||!call(std::move(function),std::move(nested),returned,depth+1))return false;
                operands.push_back(std::move(returned));break;
            }
            case Op::Return:if(operands.empty()){result=Item{};return true;}return takeOperand(result);
            default:{
                if((!TransformNumericOp(instruction.op)&&!NativeLoopComparison(instruction.op))||operands.size()<2)return false;
                Item right,left;if(!takeOperand(right)||!takeOperand(left)||right.kind!=Item::Number||left.kind!=Item::Number)return false;
                operands.push_back(binary(instruction.op,std::move(left),std::move(right)));break;
            }
            }
        }
        return false;
    };
    bool conditioned=false;
    std::vector<std::uint8_t> visited(regionEnd-start+1);size_t instructionCount=0;
    for(size_t ip=start;;++ip){
        if(ip>regionEnd||instructionCount>=kNativeLoopMaxInstructions||visited[ip-start]||program.nodes.size()>kNativeLoopMaxNodes-16)return RejectNativeLoop(__LINE__);
        visited[ip-start]=1;++instructionCount;
        const auto& instruction=chunk.code[ip];
        nativeLoopLastOperation=static_cast<unsigned>(instruction.op);nativeLoopLastOffset=instruction.offset;
        switch(instruction.op){
        case Op::NoOp:case Op::BeginBlock:case Op::EndBlock:break;
        case Op::LoadReference:{
            Item item;
            const auto local=std::find_if(program.locals.begin(),program.locals.end(),[&](const auto& slot){return slot.name==instruction.text;});
            if(local!=program.locals.end()){item.kind=Item::LocalReference;item.local=static_cast<int>(local-program.locals.begin());}
            else {const auto binding=frame->env->FindValue(instruction.text);if(!binding||binding->realm||binding->type==Value::Type::Reference)return RejectNativeLoop(__LINE__);item.value=*binding;if(item.value.type==Value::Type::Number)item=number(item.value.number);}
            stack.push_back(std::move(item));break;
        }
        case Op::Constant:{
            Item item;item.value=chunk.constants[instruction.argument];
            if(item.value.type==Value::Type::Number)item=number(item.value.number);
            stack.push_back(std::move(item));break;
        }
        case Op::GetValue:case Op::PrepareCall:case Op::ToPropertyKey:{Item item;if(!take(item))return RejectNativeLoop(__LINE__);stack.push_back(std::move(item));break;}
        case Op::Duplicate:if(stack.empty())return RejectNativeLoop(__LINE__);stack.push_back(stack.back());break;
        case Op::Pop:{Item item;if(!take(item))return RejectNativeLoop(__LINE__);break;}
        case Op::GetProperty:case Op::GetIndex:{
            Item key,base;if(instruction.op==Op::GetIndex){if(!take(key))return RejectNativeLoop(__LINE__);}else key.value=Value::String(instruction.text);
            if(!take(base))return RejectNativeLoop(__LINE__);
            Item item;item.kind=Item::MemberReference;item.base=static_cast<int>(members.size());members.push_back(std::move(base));
            item.key=static_cast<int>(members.size());members.push_back(std::move(key));stack.push_back(std::move(item));break;
        }
        case Op::Call:{
            if(instruction.argument<0||instruction.argument>4||instruction.text==L"direct-eval"||stack.size()<static_cast<size_t>(instruction.argument)+1)return RejectNativeLoop(__LINE__);
            std::vector<Item> arguments(instruction.argument);
            for(int at=instruction.argument-1;at>=0;--at)if(!take(arguments[at]))return RejectNativeLoop(__LINE__);
            Item callee,result;if(!take(callee)||!call(std::move(callee),std::move(arguments),result,0))return RejectNativeLoop(__LINE__);
            stack.push_back(std::move(result));break;
        }
        case Op::Assign:{
            if(!conditioned||stack.size()<2)return RejectNativeLoop(__LINE__);
            Item value;if(!take(value))return RejectNativeLoop(__LINE__);const auto target=pop();
            if(target.kind==Item::MemberReference){if(!store(target,value))return RejectNativeLoop(__LINE__);stack.push_back(std::move(value));break;}
            if(target.kind!=Item::LocalReference||program.locals[target.local].owner->immutableBindings.count(program.locals[target.local].name))return RejectNativeLoop(__LINE__);
            auto& local=program.locals[target.local];
            if(value.kind==Item::Number&&local.binding->type==Value::Type::Number){local.current=value.node;local.written=true;}
            else if(value.kind==Item::Appended&&value.local==target.local&&program.stringLocal==target.local){local.written=true;}
            else return RejectNativeLoop(__LINE__);
            stack.push_back(std::move(value));break;
        }
        case Op::PreIncrement:case Op::PostIncrement:case Op::PreDecrement:case Op::PostDecrement:{
            if(!conditioned||stack.empty())return RejectNativeLoop(__LINE__);const auto target=pop();
            if(target.kind!=Item::LocalReference||program.locals[target.local].owner->immutableBindings.count(program.locals[target.local].name))return RejectNativeLoop(__LINE__);
            auto old=localValue(target.local);if(old.kind!=Item::Number)return RejectNativeLoop(__LINE__);
            const auto next=binary(instruction.op==Op::PreIncrement||instruction.op==Op::PostIncrement?Op::Add:Op::Subtract,old,number(1));
            program.locals[target.local].current=next.node;program.locals[target.local].written=true;
            stack.push_back(instruction.op==Op::PostIncrement||instruction.op==Op::PostDecrement?old:next);break;
        }
        case Op::JumpFalse:{
            if(conditioned||stack.size()!=1||instruction.argument<=static_cast<int>(end))return RejectNativeLoop(__LINE__);
            Item value;if(!take(value)||(value.kind!=Item::Number&&value.kind!=Item::Boolean))return RejectNativeLoop(__LINE__);
            program.condition=value.node;program.conditionEnd=program.nodes.size();program.exit=static_cast<size_t>(instruction.argument);conditioned=true;break;
        }
        case Op::Jump:
            if(ip==end){
                for(const auto& hoisted:program.hoistedArrays)for(const auto& write:program.writes){const auto& array=program.arrays[write.port];
                    if(hoisted.object==array.owner.object||hoisted.object==array.storage)return RejectNativeLoop(__LINE__);
                }
                program.cost=instructionCount;return conditioned&&stack.empty()&&
                ((program.append>=0&&program.stringLocal>=0)||!program.writes.empty()||
                 std::any_of(program.locals.begin(),program.locals.end(),[](const auto& local){return local.written&&local.input>=0;}))&&
                program.nodes.size()<kNativeLoopMaxNodes;}
            if(instruction.argument<static_cast<int>(start)||instruction.argument>static_cast<int>(regionEnd))return RejectNativeLoop(__LINE__);
            ip=static_cast<size_t>(instruction.argument)-1;break;
        default:{
            if((!TransformNumericOp(instruction.op)&&!NativeLoopComparison(instruction.op))||stack.size()<2)return RejectNativeLoop(__LINE__);
            Item right,left;if(!take(right)||!take(left))return RejectNativeLoop(__LINE__);
            if(instruction.op==Op::Add&&right.kind==Item::Ordinary&&right.value.type==Value::Type::String&&right.value.StringText().size()==1){
                right=number(static_cast<unsigned short>(right.value.StringText()[0]));right.kind=Item::Character;
            }
            if(instruction.op==Op::Add&&right.kind==Item::Character&&left.local>=0&&
               (left.kind==Item::Accumulator||(left.kind==Item::Ordinary&&left.value.type==Value::Type::String))){
                if(program.append>=0)return RejectNativeLoop(__LINE__);
                program.append=right.node;program.stringLocal=left.local;Item result;result.kind=Item::Appended;result.local=left.local;stack.push_back(result);
            }else {if(left.kind!=Item::Number||right.kind!=Item::Number)return RejectNativeLoop(__LINE__);stack.push_back(binary(instruction.op,std::move(left),std::move(right)));}
            break;
        }
        }
    }
}

void TagNativeLoop(NativeLoopProgram& program){
    const auto integerConstant=[](double value){return std::isfinite(value)&&value>=-2147483648.0&&value<=2147483647.0&&
        value==std::trunc(value)&&!(value==0&&std::signbit(value));};
    for(auto& node:program.nodes){
        node.integer=node.kind==NativeLoopNode::ReadCharacter||
            (node.kind==NativeLoopNode::Constant&&integerConstant(node.constant))||
            (node.kind==NativeLoopNode::Numeric&&(node.operation==Op::BitwiseAnd||node.operation==Op::BitwiseOr||
             node.operation==Op::BitwiseXor||node.operation==Op::ShiftLeft||node.operation==Op::ShiftRight||node.operation==Op::UnsignedShiftRight));
        node.unsignedInteger=node.integer&&node.operation==Op::UnsignedShiftRight;
        // Runtime-invariant data belongs in the frame, not in machine-code
        // immediates. A different key/length/receiver can reuse the same graph.
        if(node.kind==NativeLoopNode::Snapshot||
           (node.kind==NativeLoopNode::Input&&!program.locals[node.port].written)){
            const auto value=node.kind==NativeLoopNode::Snapshot?node.constant:program.locals[node.port].binding->number;
            if(integerConstant(value))node.integer=true;
            else if(std::isfinite(value)&&value>=0&&value<=4294967295.0&&value==std::trunc(value)&&!std::signbit(value))
                node.integer=node.unsignedInteger=true;
        }
    }
    for(const auto& local:program.locals)if(local.written&&local.input>=0&&program.nodes[local.current].integer){
        auto& input=program.nodes[local.input];const auto number=local.binding->number;
        const bool unsignedInteger=program.nodes[local.current].unsignedInteger;
        if(std::isfinite(number)&&number==std::trunc(number)&&!(number==0&&std::signbit(number))&&
           number>=(unsignedInteger?0.0:-2147483648.0)&&number<=(unsignedInteger?4294967295.0:2147483647.0)){
            input.integer=true;input.unsignedInteger=unsignedInteger;
        }
    }
    // Finite interval proof removes overflow/NaN conversion branches from
    // Number graphs bounded well within int64. Unproved data uses the exact
    // ECMAScript conversion in the baseline emitter.
    std::vector<std::pair<double,double>> bounds(program.nodes.size(),{-INFINITY,INFINITY});
    for(size_t at=0;at<program.nodes.size();++at){
        auto& node=program.nodes[at];auto& range=bounds[at];
        if(node.integer)range=node.unsignedInteger?std::make_pair(0.0,4294967295.0):std::make_pair(-2147483648.0,2147483647.0);
        if(node.kind==NativeLoopNode::Constant)range={node.constant,node.constant};
        else if(node.kind==NativeLoopNode::ReadNumber&&program.arrays[node.port].unsignedBytes)range={0,255};
        else if(node.kind==NativeLoopNode::Numeric&&!node.integer){
            const auto a=bounds[node.left],b=bounds[node.right];
            switch(node.operation){
            case Op::Add:range={a.first+b.first,a.second+b.second};break;
            case Op::Subtract:range={a.first-b.second,a.second-b.first};break;
            case Op::Multiply:{const double products[]={a.first*b.first,a.first*b.second,a.second*b.first,a.second*b.second};
                if(std::all_of(products,products+4,[](double value){return !std::isnan(value);}))
                    range={*std::min_element(products,products+4),*std::max_element(products,products+4)};break;}
            case Op::Less:case Op::LessEqual:case Op::Greater:case Op::GreaterEqual:range={0,1};break;
            default:break;
            }
        }
        node.finiteConversion=std::isfinite(range.first)&&std::isfinite(range.second)&&range.first>-9e18&&range.second<9e18;
    }
}
std::shared_ptr<NativeLoopCode> CompileNativeLoop(NativeLoopProgram& program){
#if defined(_M_X64)
    X64BaselineEmitter out;
    std::vector<size_t> failure,done,back;
    const auto branch=[&](std::uint8_t condition,std::vector<size_t>& list){
        out.Byte(0x0f);out.Byte(static_cast<std::uint8_t>(0x80+condition));list.push_back(out.code.size());out.Dword(0);
    };
    const auto patch=[&](const std::vector<size_t>& list,size_t target){
        for(const auto position:list){const auto value=static_cast<std::uint32_t>(static_cast<std::int32_t>(target-position-4));
            for(unsigned shift=0;shift<32;shift+=8)out.code[position+shift/8]=static_cast<std::uint8_t>(value>>shift);}
    };
    const auto offset=[](int at){return static_cast<std::uint32_t>(offsetof(NativeLoopFrame,values)+at*sizeof(double));};
    const auto loadNumber=[&](int at,bool second){
        const auto& node=program.nodes[at];
        if(node.integer){
            out.Byte(0x8b);out.Byte(0x81);out.Dword(offset(at));
            if(node.unsignedInteger){out.Byte(0xf2);out.Byte(0x48);out.Byte(0x0f);out.Byte(0x2a);out.Byte(second?0xc8:0xc0);}
            else{out.Byte(0xf2);out.Byte(0x0f);out.Byte(0x2a);out.Byte(second?0xc8:0xc0);}
        }else if(second)out.MovXmm1FromFrame(offset(at));else out.MovXmm0FromFrame(offset(at));
    };
    const auto convertInteger=[&](int at,bool second){
        if(program.nodes[at].integer){out.Byte(0x8b);out.Byte(0x81);out.Dword(offset(at));}
        else{
            loadNumber(at,second);
            if(program.nodes[at].finiteConversion){out.Byte(0xf2);out.Byte(0x48);out.Byte(0x0f);out.Byte(0x2c);out.Byte(second?0xc1:0xc0);}
            else {out.ToUint32(second);out.Byte(0x4c);out.Byte(0x89);out.Byte(0xd1);}
        }
    };
    // Loop-invariant constants are initialized once per native batch.
    for(size_t at=0;at<program.nodes.size();++at)if(program.nodes[at].kind==NativeLoopNode::Constant){
        const auto& node=program.nodes[at];const auto target=offset(static_cast<int>(at));
        if(node.integer)out.StoreDword(target,static_cast<std::uint32_t>(static_cast<std::int32_t>(node.constant)));
        else{std::uint64_t bits=0;std::memcpy(&bits,&node.constant,8);out.MovRax(bits);out.MovFrameFromRax(target);}
    }
    const auto loop=out.code.size();
    out.Byte(0x8b);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,completed)));
    out.Byte(0x3b);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,count)));branch(3,done);
    for(size_t at=0;at<program.nodes.size();++at){
        if(at==program.conditionEnd){
            loadNumber(program.condition,false);out.XorXmm1();out.Compare();branch(4,done);branch(10,done);
        }
        const auto& node=program.nodes[at];const auto target=offset(static_cast<int>(at));
        switch(node.kind){
        case NativeLoopNode::Input:case NativeLoopNode::Snapshot:break;
        case NativeLoopNode::Constant:break;
        case NativeLoopNode::Numeric:{
            const bool bitwise=node.integer;
            if(bitwise){
                // ToUint32 is exact for every double, including huge values.
                // Integer graph edges avoid that conversion entirely.
                out.Byte(0x49);out.Byte(0x89);out.Byte(0xca);
                convertInteger(node.left,false);
                out.Byte(0x41);out.Byte(0x89);out.Byte(0xc1);
                convertInteger(node.right,true);
                if(node.operation==Op::BitwiseAnd||node.operation==Op::BitwiseOr||node.operation==Op::BitwiseXor){
                    out.Byte(0x44);out.Byte(node.operation==Op::BitwiseAnd?0x21:node.operation==Op::BitwiseOr?0x09:0x31);out.Byte(0xc8);
                }else{
                    out.Byte(0x89);out.Byte(0xc1);out.Byte(0x44);out.Byte(0x89);out.Byte(0xc8);
                    out.Byte(0xd3);out.Byte(node.operation==Op::ShiftLeft?0xe0:node.operation==Op::ShiftRight?0xf8:0xe8);
                }
                out.Byte(0x4c);out.Byte(0x89);out.Byte(0xd1);
                out.Byte(0x89);out.Byte(0x81);out.Dword(target);
            }else{
                loadNumber(node.left,false);loadNumber(node.right,true);
                switch(node.operation){
                case Op::Add:out.Add();break;case Op::Subtract:out.Subtract();break;
                case Op::Multiply:out.Multiply();break;case Op::Divide:out.Divide();break;
                case Op::Less:case Op::LessEqual:case Op::Greater:case Op::GreaterEqual:
                    out.Compare();
                    out.Set(node.operation==Op::Less?2:node.operation==Op::LessEqual?6:node.operation==Op::Greater?7:3,0xc0);
                    out.Set(11,0xc2);out.AndAlDl();out.BooleanToXmm0();break;
                default:return {};
                }
                out.MovFrameFromXmm0(target);
            }break;
        }
        case NativeLoopNode::Overlay:{
            loadNumber(node.index,false);loadNumber(node.writeIndex,true);out.Compare();
            std::vector<size_t> different,merged;branch(5,different);
            loadNumber(node.right,false);out.Byte(0xe9);merged.push_back(out.code.size());out.Dword(0);
            patch(different,out.code.size());loadNumber(node.left,false);patch(merged,out.code.size());
            out.MovFrameFromXmm0(target);break;
        }
        case NativeLoopNode::ReadNumber:case NativeLoopNode::ReadCharacter:case NativeLoopNode::ReadObject:case NativeLoopNode::WriteGuard:{
            loadNumber(node.left,false);
            // Index is nonnegative, finite, integral and in bounds. Compare the
            // converted integer to the original to exclude fractions and -0.
            out.Byte(0xf2);out.Byte(0x48);out.Byte(0x0f);out.Byte(0x2c);out.Byte(0xc0);
            out.Byte(0x48);out.Byte(0x85);out.Byte(0xc0);branch(8,failure);
            out.Byte(0xf2);out.Byte(0x48);out.Byte(0x0f);out.Byte(0x2a);out.Byte(0xc8);out.Compare();branch(5,failure);branch(10,failure);
            out.Byte(0x48);out.Byte(0x3b);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,lengths)+node.port*sizeof(std::uint64_t)));branch(3,failure);
            if(node.kind==NativeLoopNode::ReadCharacter){
                out.Byte(0x48);out.Byte(0x8b);out.Byte(0x91);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,characters)+node.port*sizeof(void*)));
                out.Byte(0x0f);out.Byte(0xb7);out.Byte(0x04);out.Byte(0x42);
                out.Byte(0x89);out.Byte(0x81);out.Dword(target);
            }else{
                out.Byte(0x48);out.Byte(0x69);out.Byte(0xc0);out.Dword(static_cast<std::uint32_t>(sizeof(Value)));
                out.Byte(0x48);out.Byte(0x03);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,arrays)+node.port*sizeof(void*)));
                out.Byte(0x83);out.Byte(0xb8);out.Dword(static_cast<std::uint32_t>(offsetof(Value,type)));
                out.Byte(static_cast<std::uint8_t>(node.kind==NativeLoopNode::ReadObject?Value::Type::Object:Value::Type::Number));branch(5,failure);
                out.Byte(0x48);out.Byte(0x83);out.Byte(0xb8);out.Dword(static_cast<std::uint32_t>(offsetof(Value,realm)));out.Byte(0);branch(5,failure);
                out.Byte(0x80);out.Byte(0xb8);out.Dword(static_cast<std::uint32_t>(offsetof(Value,abrupt)));out.Byte(0);branch(5,failure);
                if(node.kind==NativeLoopNode::ReadObject){
                    out.Byte(0x4c);out.Byte(0x8b);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,objects)+at*sizeof(std::uintptr_t)));
                    out.Byte(0x4c);out.Byte(0x39);out.Byte(0x80);out.Dword(static_cast<std::uint32_t>(offsetof(Value,object)));branch(5,failure);
                }else if(node.kind==NativeLoopNode::ReadNumber){
                    out.Byte(0xf2);out.Byte(0x0f);out.Byte(0x10);out.Byte(0x80);out.Dword(static_cast<std::uint32_t>(offsetof(Value,number)));
                    out.MovFrameFromXmm0(target);
                }
            }break;
        }
        }
    }
    // No side effects occurred above. Commit this iteration atomically after
    // every data guard has passed; the VM sees precisely this prefix on exit.
    for(const auto& write:program.writes){
        loadNumber(write.index,false);
        out.Byte(0xf2);out.Byte(0x48);out.Byte(0x0f);out.Byte(0x2c);out.Byte(0xc0);
        out.Byte(0x48);out.Byte(0x69);out.Byte(0xc0);out.Dword(static_cast<std::uint32_t>(sizeof(Value)));
        out.Byte(0x48);out.Byte(0x03);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,arrays)+write.port*sizeof(void*)));
        out.Byte(0x48);out.Byte(0x89);out.Byte(0xc2);
        loadNumber(write.value,false);
        out.Byte(0xf2);out.Byte(0x0f);out.Byte(0x11);out.Byte(0x82);out.Dword(static_cast<std::uint32_t>(offsetof(Value,number)));
    }
    if(program.append>=0){
    loadNumber(program.append,false);
    out.Byte(0xf2);out.Byte(0x48);out.Byte(0x0f);out.Byte(0x2c);out.Byte(0xc0);
    // CharCode tables already contain uint16; fromCharCode may use any Number.
    if(!program.nodes[program.append].integer){out.Byte(0x49);out.Byte(0x89);out.Byte(0xca);out.ToUint32(false);out.Byte(0x4c);out.Byte(0x89);out.Byte(0xd1);}
    out.Byte(0x48);out.Byte(0x8b);out.Byte(0x91);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,output)));
    out.Byte(0x44);out.Byte(0x8b);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,completed)));
    out.Byte(0x66);out.Byte(0x42);out.Byte(0x89);out.Byte(0x04);out.Byte(0x42);
    }
    for(size_t at=0;at<program.locals.size();++at){const auto& local=program.locals[at];if(!local.written||local.input<0)continue;
        const auto update=static_cast<std::uint32_t>(offsetof(NativeLoopFrame,updates)+at*sizeof(double));
        if(program.nodes[local.input].integer){
            out.Byte(0x8b);out.Byte(0x81);out.Dword(offset(local.current));
            out.Byte(0x89);out.Byte(0x81);out.Dword(update);
        }else{loadNumber(local.current,false);out.MovFrameFromXmm0(update);}
    }
    // Snapshot all next-iteration inputs before replacing any old input.
    // This preserves assignments that exchange or depend on two locals.
    for(size_t at=0;at<program.locals.size();++at){const auto& local=program.locals[at];if(!local.written||local.input<0)continue;
        const auto update=static_cast<std::uint32_t>(offsetof(NativeLoopFrame,updates)+at*sizeof(double));
        out.Byte(0x48);out.Byte(0x8b);out.Byte(0x81);out.Dword(update);
        out.MovFrameFromRax(offset(local.input));
    }
    out.Byte(0xff);out.Byte(0x81);out.Dword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,completed)));
    out.Byte(0xe9);back.push_back(out.code.size());out.Dword(0);
    patch(failure,out.code.size());out.StoreDword(static_cast<std::uint32_t>(offsetof(NativeLoopFrame,stopped)),2);
    const auto finish=out.code.size();patch(done,finish);patch(back,loop);out.Byte(0xc3);
    if(out.code.size()>131072)return {};
    auto code=std::make_shared<NativeLoopCode>();code->bytes=out.code.size();
    SYSTEM_INFO system{};GetSystemInfo(&system);
    code->allocationSize=(code->bytes+system.dwPageSize-1)/system.dwPageSize*system.dwPageSize;
    code->memory=VirtualAlloc(nullptr,code->bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!code->memory)return {};
    std::memcpy(code->memory,out.code.data(),code->bytes);DWORD previous=0;
    if(!VirtualProtect(code->memory,code->bytes,PAGE_EXECUTE_READ,&previous)||!FlushInstructionCache(GetCurrentProcess(),code->memory,code->bytes))return {};
    // Leaf code changes no nonvolatile register or stack pointer, so Windows
    // can unwind it without a dynamic function table.
    code->entry=reinterpret_cast<NativeLoopCode::Entry>(code->memory);return code;
#else
    (void)program;return {};
#endif
}
bool TryNativeLoop(const std::shared_ptr<ExecutionFrame>& frame,size_t end,bool headerEntry=false){
#if defined(_M_X64)
    if(!jitCompilationThreshold||diagnosticInstructionLimit||traceFunctions||traceErrors||profileExecution||hostInterrupted||
       !frame->stack.empty()||frame->env==global||frame->chunk->code[end].argument<0)return false;
    const auto start=static_cast<size_t>(frame->chunk->code[end].argument);
    if(start>=end||end-start>kNativeLoopMaxInstructions||!frame->chunk->handlers.empty())return false;
    const auto key=std::make_pair(frame->chunk->identity,end);
    auto found=nativeLoopPlans.find(key);
    if(found==nativeLoopPlans.end()){
        if(headerEntry)return false;
        if(nativeLoopPlans.size()>=256&&!EvictNativeLoopPlan())return false;
        found=nativeLoopPlans.emplace(key,NativeLoopPlan{}).first;
        // Charge warmup by bytecode work, so an expensive short loop need
        // not interpret tens of thousands of instructions before compiling.
        found->second.nextWarm=std::max<size_t>(2,std::min<size_t>(64,4096/(end-start)));
    }
    auto& plan=found->second;
    plan.lastUse=++nativeLoopPlanSequence;
    if(headerEntry&&plan.code.empty())return false;
    if(++plan.warm<plan.nextWarm)return false;
    plan.nextWarm=plan.warm+64;
    NativeLoopProgram program;nativeLoopLastReject=0;
    const bool restored=plan.prepared&&RestoreNativeLoop(frame,*plan.prepared,program);
    if(restored)++nativeLoopPrepareCacheHits;
    else {
        if(plan.prepared)++nativeLoopPrepareCacheMisses;
        program=NativeLoopProgram{};
    }
    if(!restored&&!PrepareNativeLoop(frame,end,program)){
        plan.reject=nativeLoopLastReject;plan.operation=nativeLoopLastOperation;
        plan.offset=nativeLoopLastOffset;plan.instructions=end-start;
        // A value guard can change after host reentry. Never permanently
        // reject the bytecode just because its current data is unsupported.
        return false;
    }
    plan.reject=0;
    plan.nextWarm=plan.warm+1;
    TagNativeLoop(program);
    // Compare the complete specialization key; a hash collision must never
    // let one numeric graph execute another graph's machine code.
    std::vector<std::uint64_t> signature;
    const auto hash=[&](std::uint64_t value){signature.push_back(value);};
    hash(program.condition);hash(program.conditionEnd);hash(program.append);
    for(const auto& node:program.nodes){
        hash(node.kind);hash(static_cast<unsigned>(node.operation));hash(node.left);hash(node.right);hash(node.port);
        hash(node.integer);hash(node.unsignedInteger);hash(node.finiteConversion);
        hash(node.index);hash(node.writeIndex);
        std::uint64_t bits=0;if(node.kind==NativeLoopNode::Constant)std::memcpy(&bits,&node.constant,8);hash(bits);
    }
    for(const auto& local:program.locals){hash(local.input);hash(local.current);hash(local.written);}
    for(const auto& write:program.writes){hash(write.port);hash(write.index);hash(write.value);}
    auto compiled=std::find_if(plan.code.begin(),plan.code.end(),[&](const auto& code){return code.first==signature;});
    std::shared_ptr<NativeLoopCode> code;
    if(compiled!=plan.code.end()){
        code=compiled->second;
    }else{
        if(plan.code.size()>=4){++nativeLoopCacheLimitExits;plan.nextWarm=plan.warm+64;return false;}
        while(jitAllocatedCodeBytes>=jitCodeBudget&&EvictNativeLoopPlan(&plan)){}
        if(jitAllocatedCodeBytes>=jitCodeBudget){++nativeLoopCacheLimitExits;plan.nextWarm=plan.warm+64;return false;}
        code=CompileNativeLoop(program);if(!code){plan.nextWarm=plan.warm+64;return false;}
        while(code->allocationSize>jitCodeBudget-jitAllocatedCodeBytes&&EvictNativeLoopPlan(&plan)){}
        if(code->allocationSize>jitCodeBudget-jitAllocatedCodeBytes){++nativeLoopCacheLimitExits;plan.nextWarm=plan.warm+64;return false;}
        jitAllocatedCodeBytes+=code->allocationSize;++jitStatistics.compiledFunctions;jitStatistics.generatedCodeBytes+=code->bytes;
        plan.code.push_back({signature,code});++nativeLoopCompilations;
    }
    if(!restored&&plan.warm>=plan.nextCacheWarm){
        if(plan.prepared){nativeLoopPreparedBytes-=plan.prepared->bytes;plan.prepared.reset();}
        auto prepared=CacheNativeLoop(program);
        if(prepared&&prepared->bytes<=kNativeLoopPreparedBudget-nativeLoopPreparedBytes){
            nativeLoopPreparedBytes+=prepared->bytes;plan.prepared=std::move(prepared);
        }else plan.nextCacheWarm=plan.warm+64;
    }
    NativeLoopFrame native;
    for(size_t at=0;at<program.nodes.size();++at){
        const auto& node=program.nodes[at];native.objects[at]=node.expected;
        if(node.kind!=NativeLoopNode::Input&&node.kind!=NativeLoopNode::Snapshot)continue;
        const auto number=node.kind==NativeLoopNode::Input?program.locals[node.port].binding->number:node.constant;
        if(node.integer){const auto bits=node.unsignedInteger?static_cast<std::uint32_t>(number):
            static_cast<std::uint32_t>(static_cast<std::int32_t>(number));std::memcpy(&native.values[at],&bits,4);}
        else native.values[at]=number;
    }
    for(size_t at=0;at<program.arrays.size();++at){
        const auto& array=program.arrays[at];native.arrays[at]=array.storage->items.data()+array.offset;
        native.lengths[at]=array.length;native.characters[at]=array.characters.data();
    }
    std::array<wchar_t,4096> output;
    native.output=output.data();native.count=static_cast<std::uint32_t>(output.size());
    code->entry(&native);
    if(!native.completed){if(native.stopped){++nativeLoopGuardExits;plan.nextWarm=plan.warm+64;}return false;}
    for(const auto& local:program.locals)if(local.written&&local.input>=0){
        const auto& node=program.nodes[local.input];
        if(node.integer){std::uint32_t bits=0;std::memcpy(&bits,&native.values[local.input],4);
            local.binding->number=node.unsignedInteger?static_cast<double>(bits):static_cast<double>(static_cast<std::int32_t>(bits));}
        else local.binding->number=native.values[local.input];
    }
    if(program.stringLocal>=0){
    auto& destination=*program.locals[program.stringLocal].binding;
    // One immutable string assignment per batch. Existing aliases keep their
    // original backing storage, including frozen/const-owned shared strings.
    std::wstring joined=destination.StringText();joined.append(output.data(),native.completed);
    destination=Value::String(std::move(joined));
    }
    ++nativeLoopCalls;nativeLoopIterations+=native.completed;
    if(native.stopped)++nativeLoopGuardExits;
    executedInstructions+=static_cast<std::uint64_t>(native.completed)*program.cost;
    if(headerEntry)++nativeLoopHeaderEntries;
    if(!native.stopped&&native.completed<native.count)JumpFrame(frame,program.exit);
    else frame->ip=start;
    return true;
#else
    (void)frame;(void)end;(void)headerEntry;return false;
#endif
}

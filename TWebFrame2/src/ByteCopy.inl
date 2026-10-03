// Guarded bytecode tier for a UTF-16 charCodeAt loop writing a Uint8Array.
// Resolve only ordinary bindings/members and proved pure helper functions.
// Short batches return to the VM for cancellation, collection and host reentry.
struct ByteCopyPlan {
    StringTransformPlan expressions;
    int condition=-1,target=-1,value=-1;
    size_t start=0,cost=0,warm=0;
    bool checked=false,supported=false;
};
std::map<std::pair<std::uint64_t,size_t>,ByteCopyPlan> byteCopyPlans;
std::uint64_t byteCopyCalls=0,byteCopyIterations=0;
std::uint64_t simdByteCopyIterations=0,parallelByteCopyIterations=0;
static void CopyUtf16ByteSlots(const wchar_t* text,size_t first,size_t count,Value* destination,Value* backing){
    size_t at=0;
#if defined(_M_X64) || defined(_M_IX86)
    const auto mask=_mm_set1_epi16(255);
    alignas(16) unsigned char bytes[16];
    for(;at+16<=count;at+=16){
        const auto low=_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(text+first+at)),mask);
        const auto high=_mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(text+first+at+8)),mask);
        _mm_store_si128(reinterpret_cast<__m128i*>(bytes),_mm_packus_epi16(low,high));
        for(size_t lane=0;lane<16;++lane){destination[at+lane].number=bytes[lane];if(backing)backing[at+lane].number=bytes[lane];}
    }
#endif
    for(;at<count;++at){const auto byte=static_cast<unsigned short>(text[first+at])&255;
        destination[at].number=byte;if(backing)backing[at].number=byte;}
}
bool PrepareByteCopy(const Chunk& chunk,size_t end,ByteCopyPlan& plan){
    if(!chunk.handlers.empty()||chunk.code[end].argument<0)return false;
    plan.start=static_cast<size_t>(chunk.code[end].argument);
    if(plan.start>=end||end-plan.start>256)return false;
    struct Operand {int expression=-1;std::wstring binding;};
    std::vector<Operand> stack;
    auto& expressions=plan.expressions.expressions;
    const auto emit=[&](TransformExpression node){expressions.push_back(std::move(node));return static_cast<int>(expressions.size()-1);};
    const auto read=[&](const Operand& operand){
        if(operand.binding.empty())return operand.expression;
        TransformExpression node{TransformExpression::Binding};node.name=operand.binding;return emit(std::move(node));
    };
    const auto pop=[&](){auto result=stack.back();stack.pop_back();return result;};
    bool incremented=false;
    std::vector<size_t> visited;
    for(size_t ip=plan.start;;++ip){
        if(ip>=chunk.code.size()||visited.size()>=256||std::find(visited.begin(),visited.end(),ip)!=visited.end())return false;
        visited.push_back(ip);const auto& instruction=chunk.code[ip];
        switch(instruction.op){
        case Op::NoOp:break;
        case Op::LoadReference:stack.push_back({-1,instruction.text});break;
        case Op::Constant:{TransformExpression node{TransformExpression::Constant};node.constant=chunk.constants[instruction.argument];stack.push_back({emit(std::move(node)),{}});break;}
        case Op::GetValue:case Op::PrepareCall:case Op::ToPropertyKey:
            if(stack.empty())return false;stack.back()={read(stack.back()),{}};break;
        case Op::GetProperty:case Op::GetIndex:{
            if(stack.size()<(instruction.op==Op::GetIndex?2u:1u))return false;
            TransformExpression node{TransformExpression::Member};
            if(instruction.op==Op::GetIndex)node.right=read(pop());else node.name=instruction.text;
            node.left=read(pop());stack.push_back({emit(std::move(node)),{}});break;
        }
        case Op::Call:{
            if(instruction.argument<0||instruction.argument>4||instruction.text==L"direct-eval"||stack.size()<static_cast<size_t>(instruction.argument)+1)return false;
            TransformExpression node{TransformExpression::Call};node.arguments.resize(instruction.argument);
            for(int at=instruction.argument-1;at>=0;--at)node.arguments[at]=read(pop());
            node.left=read(pop());stack.push_back({emit(std::move(node)),{}});break;
        }
        case Op::Less:{
            if(plan.condition>=0||plan.target>=0||stack.size()<2)return false;
            TransformExpression node{TransformExpression::Binary};node.operation=Op::Less;
            node.right=read(pop());node.left=read(pop());stack.push_back({emit(std::move(node)),{}});break;
        }
        case Op::JumpFalse:
            if(plan.condition>=0||stack.size()!=1||instruction.argument<=static_cast<int>(end))return false;
            plan.condition=read(pop());break;
        case Op::Assign:
            if(plan.condition<0||plan.target>=0||incremented||stack.size()<2)return false;
            plan.value=read(pop());plan.target=read(pop());stack.push_back({plan.value,{}});break;
        case Op::PostIncrement:case Op::PreIncrement:
            if(plan.target<0||incremented||stack.empty()||stack.back().binding.empty())return false;
            plan.expressions.counter=pop().binding;incremented=true;
            stack.push_back({emit({TransformExpression::Counter}),{}});break;
        case Op::Pop:
            if(stack.size()!=1)return false;pop();break;
        case Op::Jump:
            if(ip==end){plan.cost=visited.size();return plan.condition>=0&&plan.target>=0&&incremented&&stack.empty();}
            if(instruction.argument<0||static_cast<size_t>(instruction.argument)>=chunk.code.size())return false;
            ip=static_cast<size_t>(instruction.argument)-1;break;
        default:{
            if(!TransformNumericOp(instruction.op)||stack.size()<2)return false;
            TransformExpression node{TransformExpression::Binary};node.operation=instruction.op;
            node.right=read(pop());node.left=read(pop());stack.push_back({emit(std::move(node)),{}});break;
        }
        }
    }
}
bool TryByteCopy(const std::shared_ptr<ExecutionFrame>& frame,size_t end){
    if(!jitCompilationThreshold||diagnosticInstructionLimit||traceFunctions||traceErrors||profileExecution||hostInterrupted||
       !frame->stack.empty()||frame->env==global||frame->chunk->code[end].argument<0||
       end-static_cast<size_t>(frame->chunk->code[end].argument)>256)return false;
    auto& eligibility=frame->chunk->byteCopyEligibility;
    if(eligibility.empty())eligibility.resize(frame->chunk->code.size());
    if(eligibility[end])return false;
    const auto key=std::make_pair(frame->chunk->identity,end);
    auto found=byteCopyPlans.find(key);
    if(found==byteCopyPlans.end()){
        if(byteCopyPlans.size()>=256)return false;
        found=byteCopyPlans.emplace(key,ByteCopyPlan{}).first;
    }
    auto& plan=found->second;
    if(!plan.checked){if(++plan.warm<64)return false;plan.checked=true;plan.supported=PrepareByteCopy(*frame->chunk,end,plan);}
    if(!plan.supported){eligibility[end]=1;byteCopyPlans.erase(found);return false;}
    auto counter=frame->env->values.find(plan.expressions.counter);
    if(counter==frame->env->values.end()||frame->env->immutableBindings.count(plan.expressions.counter)||
       counter->second.realm||counter->second.type!=Value::Type::Number||counter->second.number<0||
       !std::isfinite(counter->second.number)||std::floor(counter->second.number)!=counter->second.number)return false;
    const auto& expressions=plan.expressions.expressions;
    const auto& target=expressions[plan.target];
    if(target.kind!=TransformExpression::Member||target.right<0||
       expressions[target.right].kind!=TransformExpression::Binding||expressions[target.right].name!=plan.expressions.counter)return false;
    TransformResolution resolution;resolution.counterBinding=&counter->second;
    std::vector<std::optional<TransformValue>> cache(expressions.size());
    const auto resolve=[&](int index,TransformValue& value){return ResolveTransformExpression(plan.expressions,index,frame->env,resolution,cache,value);};
    TransformValue value,destination,condition;
    if(!resolve(plan.value,value)||value.kind!=TransformValue::Character||resolution.input.type!=Value::Type::String||
       !resolve(target.left,destination)||destination.kind!=TransformValue::Ordinary||destination.value.realm||
       !destination.value.object||destination.value.object->kind!=ObjectKind::Array)return false;
    if(!resolve(plan.condition,condition)||condition.kind!=TransformValue::LessThan)return false;
    const auto& comparison=resolution.numbers[condition.expression];
    const auto& left=resolution.numbers[comparison.left];const auto& right=resolution.numbers[comparison.right];
    if(comparison.operation!=Op::Less||left.kind!=TransformNumber::Index||right.kind!=TransformNumber::Constant||
       right.number!=static_cast<double>(resolution.input.StringText().size()))return false;
    const auto array=destination.value.object;
    const auto bits=array->props.find(L"$typedArrayBits");
    const auto ordinaryFalse=[&](const wchar_t* name){const auto flag=array->props.find(name);return flag!=array->props.end()&&flag->second.type==Value::Type::Boolean&&!flag->second.boolean;};
    const auto storage=array->byteSource.lock();
    if(bits==array->props.end()||bits->second.type!=Value::Type::Number||bits->second.number!=8||
       array->hasIndexedDescriptors||DetachedBuffer(array)||array->externalBytes||(storage&&storage->externalBytes)||
       !ordinaryFalse(L"$typedArraySigned")||!ordinaryFalse(L"$typedArrayFloating")||!ordinaryFalse(L"$typedArrayClamped"))return false;
    const auto& text=resolution.input.StringText();
    if(text.size()>array->items.size()||counter->second.number>=static_cast<double>(text.size()))return false;
    const auto first=static_cast<size_t>(counter->second.number);
    // A host callback or Worker termination signal requires short safepoints.
    // Only an uninterrupted, private bulk copy can lease independent lanes.
    const bool uninterrupted=!executionYieldHandler&&!interruptionSignal;
    const auto count=uninterrupted?text.size()-first:std::min<size_t>(4096,text.size()-first);
    Value* destinationSlots=array->items.data()+first;Value* backing=nullptr;
    bool direct=true;
    if(storage){
        const auto sourceBits=storage->props.find(L"$typedArrayBits");
        const auto sourceFlag=[&](const wchar_t* name){const auto flag=storage->props.find(name);return flag!=storage->props.end()&&Truth(flag->second);};
        direct=(sourceBits==storage->props.end()||(sourceBits->second.type==Value::Type::Number&&sourceBits->second.number==8))&&
            !sourceFlag(L"$typedArrayFloating")&&!sourceFlag(L"$typedArraySigned")&&!storage->hasIndexedDescriptors&&
            array->byteOffset<=storage->items.size()&&first<=storage->items.size()-array->byteOffset&&
            count<=storage->items.size()-array->byteOffset-first;
        if(direct){backing=storage->items.data()+array->byteOffset+first;if(backing==destinationSlots)backing=nullptr;
            else if(storage==array)direct=false;}
    }
    const auto ordinaryNumber=[](const Value& value){return value.type==Value::Type::Number&&!value.realm&&!value.realmOwner&&!value.abrupt;};
    if(direct)for(size_t at=0;at<count;++at)if(!ordinaryNumber(destinationSlots[at])||(backing&&!ordinaryNumber(backing[at]))){direct=false;break;}
    if(direct){
        const auto workers=uninterrupted&&count>=262144?std::min<unsigned>(4,std::max<unsigned>(1,std::thread::hardware_concurrency())):1u;
        if(workers>1){
            // Threads touch only disjoint, already guarded numeric slots.
            // They never enter RuntimeCore, allocate JS values or call the host.
            std::vector<std::thread> threads;
            struct Join {std::vector<std::thread>& threads;~Join(){for(auto& thread:threads)if(thread.joinable())thread.join();}} join{threads};
            threads.reserve(workers-1);
            for(unsigned lane=1;lane<workers;++lane){const auto begin=count*lane/workers,finish=count*(lane+1)/workers;
                try{threads.emplace_back([&,begin,finish]{CopyUtf16ByteSlots(text.data(),first+begin,finish-begin,destinationSlots+begin,backing?backing+begin:nullptr);});}
                catch(...){CopyUtf16ByteSlots(text.data(),first+begin,finish-begin,destinationSlots+begin,backing?backing+begin:nullptr);}}
            const auto finish=count/workers;CopyUtf16ByteSlots(text.data(),first,finish,destinationSlots,backing);
            // Join before committing bindings or allowing any observer.
            for(auto& thread:threads)thread.join();
            if(!threads.empty())parallelByteCopyIterations+=count;
        }else CopyUtf16ByteSlots(text.data(),first,count,destinationSlots,backing);
#if defined(_M_X64) || defined(_M_IX86)
        simdByteCopyIterations+=count;
#endif
    }else for(size_t index=first;index<first+count;++index)
        WriteTypedElement(array,static_cast<double>(index),Value::Number(static_cast<unsigned short>(text[index])));
    counter->second.number+=count;
    ++byteCopyCalls;byteCopyIterations+=count;executedInstructions+=count*plan.cost;
    frame->ip=plan.start;return true;
}

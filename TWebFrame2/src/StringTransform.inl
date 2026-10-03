// A guarded tier for character-to-character loops. Compile bytecode, not source
// names. Every property/call is proved ordinary before bypassing the VM, and
// short batches return to its cancellation, GC and host-yield safepoints.
struct TransformExpression {
    enum Kind { Constant, Binding, Counter, Member, Call, Binary, Not } kind;
    Value constant;
    std::wstring name;
    Op operation=Op::NoOp;
    int left=-1,right=-1;
    std::vector<int> arguments;
};
struct StringTransformPlan {
    std::vector<TransformExpression> expressions;
    std::wstring counter,sample;
    int sampled=-1,condition=-1,body=-1;
    int terminal=-1;
    size_t exit=0,returnInstruction=0;
    size_t start=0,warm=0;
    bool checked=false,supported=false;
};
std::map<std::pair<std::uint64_t,size_t>,StringTransformPlan> stringTransformPlans;
std::uint64_t stringTransformCalls=0,stringTransformIterations=0;
std::uint64_t packedStringTransformCalls=0,packedStringTransformIterations=0;

static bool TransformNumericOp(Op op) {
    switch(op) {
    case Op::Add:case Op::Subtract:case Op::Multiply:case Op::Divide:case Op::Modulo:
    case Op::BitwiseAnd:case Op::BitwiseOr:case Op::BitwiseXor:
    case Op::ShiftLeft:case Op::ShiftRight:case Op::UnsignedShiftRight:return true;
    default:return false;
    }
}
bool PrepareStringTransform(const Chunk& chunk,size_t end,StringTransformPlan& plan) {
    if(!chunk.handlers.empty()||chunk.code[end].argument<0)return false;
    plan.start=static_cast<size_t>(chunk.code[end].argument);
    if(plan.start>=end||end-plan.start>256)return false;
    struct Operand {int expression=-1;std::wstring binding;};
    std::vector<Operand> stack;
    const auto emit=[&](TransformExpression node){plan.expressions.push_back(std::move(node));return static_cast<int>(plan.expressions.size()-1);};
    const auto read=[&](const Operand& value){
        if(value.binding.empty())return value.expression;
        TransformExpression node{TransformExpression::Binding};node.name=value.binding;return emit(std::move(node));
    };
    const auto pop=[&](){const auto value=stack.back();stack.pop_back();return value;};
    bool conditioned=false,incremented=false;
    std::vector<size_t> visited;
    for(size_t ip=plan.start;;++ip) {
        if(ip>=chunk.code.size()||visited.size()>=256||std::find(visited.begin(),visited.end(),ip)!=visited.end())return false;
        visited.push_back(ip);const auto& instruction=chunk.code[ip];
        switch(instruction.op) {
        case Op::NoOp:break;
        case Op::LoadReference:stack.push_back({-1,instruction.text});break;
        case Op::Constant:{TransformExpression node{TransformExpression::Constant};node.constant=chunk.constants[instruction.argument];stack.push_back({emit(std::move(node)),{}});break;}
        case Op::GetValue:case Op::PrepareCall:case Op::ToPropertyKey:
            if(stack.empty())return false;stack.back()={read(stack.back()),{}};break;
        case Op::Duplicate:if(stack.empty())return false;stack.push_back(stack.back());break;
        case Op::PreIncrement:{
            if(conditioned||incremented||stack.empty()||stack.back().binding.empty())return false;
            plan.counter=pop().binding;incremented=true;
            stack.push_back({emit({TransformExpression::Counter}),{}});break;
        }
        case Op::Assign:{
            if(conditioned||!plan.sample.empty()||stack.size()<2)return false;
            const auto value=read(pop());const auto target=pop();
            if(target.binding.empty()||target.binding==plan.counter)return false;
            plan.sample=target.binding;plan.sampled=value;stack.push_back({value,{}});break;
        }
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
        case Op::Not:{if(stack.empty())return false;TransformExpression node{TransformExpression::Not};node.left=read(pop());stack.push_back({emit(std::move(node)),{}});break;}
        case Op::JumpFalse:{
            if(conditioned||!incremented||plan.sample.empty()||stack.size()!=1||instruction.argument<=static_cast<int>(end))return false;
            plan.condition=read(pop());plan.exit=static_cast<size_t>(instruction.argument);conditioned=true;break;
        }
        case Op::Pop:
            if(!conditioned||stack.size()!=1||plan.body>=0)return false;plan.body=read(pop());break;
        case Op::Jump:
            if(ip==end)return conditioned&&plan.body>=0&&stack.empty();
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

struct TransformValue {
    enum Kind { Ordinary, Index, Character, Numeric, CharacterString, NanTest, NegatedNanTest, Push, Join, LessThan } kind=Ordinary;
    Value value;
    int expression=-1;
    Value receiver;
};
struct TransformNumber {
    enum Kind { Constant, Index, Character, Binary } kind;
    double number=0;
    Op operation=Op::NoOp;
    int left=-1,right=-1;
};
struct TransformResolution {
    std::vector<TransformNumber> numbers;
    Value input,output;
    int transformed=-1;
    const Value* counterBinding=nullptr;
    const Value* sampleBinding=nullptr;
    std::wstring outputBinding;
};
bool TransformOrdinaryMember(const Value& base,const std::wstring& key,Value& value) {
    if(base.realm||base.type==Value::Type::Reference)return false;
    const FastMap<std::wstring,Value>* properties=nullptr;
    if(base.type==Value::Type::Object&&base.object&&base.object->kind==ObjectKind::Plain)properties=&base.object->props;
    else if(base.type==Value::Type::Native&&base.native)properties=&base.native->props;
    else if(base.type==Value::Type::Function&&base.function)properties=&base.function->props;
    if(properties){const auto found=properties->find(key);if(found==properties->end())return false;value=found->second;}
    else if(base.type==Value::Type::String&&key==L"length")value=Value::Number(static_cast<double>(base.StringText().size()));
    else if(base.type==Value::Type::String&&key==L"charCodeAt") {
        const auto constructor=global->values.find(L"String");
        if(constructor==global->values.end()||!constructor->second.native)return false;
        const auto prototype=constructor->second.native->props.find(L"prototype");
        if(prototype==constructor->second.native->props.end()||!prototype->second.object)return false;
        const auto method=prototype->second.object->props.find(key);
        if(method==prototype->second.object->props.end())return false;value=method->second;
    } else if(base.type==Value::Type::Object&&base.object&&base.object->kind==ObjectKind::Array&&
              base.object->props.empty()&&base.object->prototype==arrayPrototype&&(key==L"push"||key==L"join")) {
        const auto method=arrayPrototype->props.find(key);
        if(method==arrayPrototype->props.end())return false;value=method->second;
    } else return false;
    return !value.realm&&value.type!=Value::Type::Reference;
}
TransformValue TransformNumberValue(TransformResolution& resolution,TransformNumber number) {
    TransformValue result;result.kind=number.kind==TransformNumber::Index?TransformValue::Index:
        number.kind==TransformNumber::Character?TransformValue::Character:TransformValue::Numeric;
    resolution.numbers.push_back(number);result.expression=static_cast<int>(resolution.numbers.size()-1);return result;
}
bool TransformAsNumber(TransformResolution& resolution,TransformValue& value) {
    if(value.kind==TransformValue::Numeric||value.kind==TransformValue::Index||value.kind==TransformValue::Character)return true;
    if(value.kind!=TransformValue::Ordinary||value.value.realm||value.value.type!=Value::Type::Number)return false;
    value=TransformNumberValue(resolution,{TransformNumber::Constant,value.value.number});return true;
}
bool ResolveTransformCall(TransformResolution& resolution,const TransformValue& callee,
                          std::vector<TransformValue> arguments,TransformValue& result,unsigned depth) {
    if(depth>8||resolution.numbers.size()>96||callee.kind!=TransformValue::Ordinary||callee.value.realm)return false;
    const auto& value=callee.value;
    if(value.type==Value::Type::Native&&value.native) {
        using Intrinsic=NativeFunction::Intrinsic;
        switch(value.native->intrinsic) {
        case Intrinsic::CharCodeAt:
            if(arguments.size()!=1||arguments[0].kind!=TransformValue::Index||callee.receiver.type!=Value::Type::String||callee.receiver.realm||
               resolution.input.type!=Value::Type::Undefined)return false;
            resolution.input=callee.receiver;result=TransformNumberValue(resolution,{TransformNumber::Character});return true;
        case Intrinsic::IsNaN:
            if(arguments.size()!=1||arguments[0].kind!=TransformValue::Character)return false;
            result.kind=TransformValue::NanTest;return true;
        case Intrinsic::FromCharCode:
            if(arguments.size()!=1||!TransformAsNumber(resolution,arguments[0]))return false;
            result.kind=TransformValue::CharacterString;result.expression=arguments[0].expression;return true;
        case Intrinsic::ArrayPush:
            if(arguments.size()!=1||arguments[0].kind!=TransformValue::CharacterString||!callee.receiver.object||
               callee.receiver.object->kind!=ObjectKind::Array||resolution.output.type!=Value::Type::Undefined)return false;
            resolution.output=callee.receiver;resolution.transformed=arguments[0].expression;result.kind=TransformValue::Push;return true;
        case Intrinsic::ArrayJoin:
            if(arguments.size()!=1||arguments[0].kind!=TransformValue::Ordinary||arguments[0].value.type!=Value::Type::String||
               !arguments[0].value.StringText().empty()||callee.receiver.object!=resolution.output.object)return false;
            result.kind=TransformValue::Join;return true;
        default:return false;
        }
    }
    if(value.type!=Value::Type::Function||!value.function||!value.function->prototype)return false;
    auto& prototype=*value.function->prototype;
    if(prototype.isAsync||prototype.isGenerator||!prototype.restParameter.empty()||!prototype.bindings.empty()||
       !prototype.chunk.handlers.empty()||!prototype.chunk.functionDeclarations.empty()||!prototype.chunk.variableDeclarations.empty()||
       std::any_of(prototype.parameterDefaults.begin(),prototype.parameterDefaults.end(),[](const auto& p){return bool(p);}))return false;
    // Constant table lookups have an existing proof that excludes accessors,
    // Proxies, mutations, coercion and arbitrary JavaScript calls.
    if(std::all_of(arguments.begin(),arguments.end(),[](const auto& argument){return argument.kind==TransformValue::Ordinary;})&&PrepareReadPlan(prototype)) {
        for(const auto& operation:prototype.readPlan)if(operation.kind==Prototype::ReadOperation::Kind::Captured){
            const auto binding=value.function->closure?value.function->closure->FindValue(operation.name):nullptr;
            if(binding==resolution.counterBinding||binding==resolution.sampleBinding)return false;
        }
        std::vector<Value> values;for(const auto& argument:arguments)values.push_back(argument.value);
        Value returned;if(RunReadPlan(value,values.data(),values.size(),returned,true)){result.value=std::move(returned);return true;}
    }
    if(prototype.chunk.code.size()>48)return false;
    std::vector<TransformValue> stack;
    for(const auto& instruction:prototype.chunk.code) {
        switch(instruction.op) {
        case Op::NoOp:break;
        case Op::LoadReference:{
            if(instruction.text==L"this"||instruction.text==L"arguments"||instruction.text==L"eval"||instruction.text==prototype.selfBindingName)return false;
            const auto parameter=std::find(prototype.parameters.begin(),prototype.parameters.end(),instruction.text);
            if(parameter!=prototype.parameters.end()){const auto index=static_cast<size_t>(parameter-prototype.parameters.begin());if(index>=arguments.size())return false;stack.push_back(arguments[index]);}
            else {const auto binding=value.function->closure?value.function->closure->FindValue(instruction.text):nullptr;
                if(!binding||binding==resolution.counterBinding||binding==resolution.sampleBinding||binding->realm||binding->type==Value::Type::Reference)return false;TransformValue item;item.value=*binding;stack.push_back(std::move(item));}
            break;
        }
        case Op::Constant:{TransformValue item;item.value=prototype.chunk.constants[instruction.argument];stack.push_back(std::move(item));break;}
        case Op::GetValue:case Op::PrepareCall:break;
        case Op::Call:{
            if(instruction.argument<0||instruction.argument>4||instruction.text==L"direct-eval"||stack.size()<static_cast<size_t>(instruction.argument)+1)return false;
            std::vector<TransformValue> nested(instruction.argument);
            for(int at=instruction.argument-1;at>=0;--at){nested[at]=stack.back();stack.pop_back();}
            auto function=stack.back();stack.pop_back();TransformValue returned;
            if(!ResolveTransformCall(resolution,function,std::move(nested),returned,depth+1))return false;stack.push_back(std::move(returned));break;
        }
        case Op::Return:if(stack.size()!=1)return false;result=stack.back();return true;
        case Op::Less:{
            if(stack.size()<2)return false;
            auto right=stack.back();stack.pop_back();auto left=stack.back();stack.pop_back();
            if(!TransformAsNumber(resolution,left)||!TransformAsNumber(resolution,right))return false;
            auto comparison=TransformNumberValue(resolution,{TransformNumber::Binary,0,Op::Less,left.expression,right.expression});
            comparison.kind=TransformValue::LessThan;stack.push_back(std::move(comparison));break;
        }
        default:{
            if(!TransformNumericOp(instruction.op)||stack.size()<2)return false;
            auto right=stack.back();stack.pop_back();auto left=stack.back();stack.pop_back();
            if(!TransformAsNumber(resolution,left)||!TransformAsNumber(resolution,right))return false;
            stack.push_back(TransformNumberValue(resolution,{TransformNumber::Binary,0,instruction.op,left.expression,right.expression}));break;
        }
        }
    }
    return false;
}
bool ResolveTransformExpression(const StringTransformPlan& plan,int at,const std::shared_ptr<Environment>& env,
                                TransformResolution& resolution,std::vector<std::optional<TransformValue>>& cache,TransformValue& result,unsigned depth=0) {
    if(at<0||depth>64||resolution.numbers.size()>96)return false;
    if(cache[at]){result=*cache[at];return true;}
    const auto& expression=plan.expressions[at];
    const auto resolve=[&](int index,TransformValue& value){return ResolveTransformExpression(plan,index,env,resolution,cache,value,depth+1);};
    switch(expression.kind) {
    case TransformExpression::Constant:result.value=expression.constant;break;
    case TransformExpression::Binding:
        if(expression.name==plan.counter){result=TransformNumberValue(resolution,{TransformNumber::Index});break;}
        if(expression.name==plan.sample){if(!resolve(plan.sampled,result))return false;break;}
        {const auto binding=env->FindValue(expression.name);if(!binding||binding->realm||binding->type==Value::Type::Reference)return false;result.value=*binding;}break;
    case TransformExpression::Counter:result=TransformNumberValue(resolution,{TransformNumber::Index});break;
    case TransformExpression::Member:{
        TransformValue base,key;if(!resolve(expression.left,base)||base.kind!=TransformValue::Ordinary)return false;
        std::wstring name=expression.name;
        if(expression.right>=0){if(!resolve(expression.right,key)||key.kind!=TransformValue::Ordinary||key.value.realm||
            (key.value.type!=Value::Type::String&&key.value.type!=Value::Type::Number))return false;name=String(key.value);}
        if(!TransformOrdinaryMember(base.value,name,result.value))return false;result.receiver=base.value;
        if(result.value.native&&result.value.native->intrinsic==NativeFunction::Intrinsic::ArrayPush&&
           plan.expressions[expression.left].kind==TransformExpression::Binding)
            resolution.outputBinding=plan.expressions[expression.left].name;
        break;
    }
    case TransformExpression::Call:{
        TransformValue callee;if(!resolve(expression.left,callee))return false;
        std::vector<TransformValue> arguments(expression.arguments.size());
        for(size_t index=0;index<arguments.size();++index)if(!resolve(expression.arguments[index],arguments[index]))return false;
        if(!ResolveTransformCall(resolution,callee,std::move(arguments),result,0))return false;break;
    }
    case TransformExpression::Binary:{
        TransformValue left,right;if(!resolve(expression.left,left)||!resolve(expression.right,right)||
            !TransformAsNumber(resolution,left)||!TransformAsNumber(resolution,right))return false;
        result=TransformNumberValue(resolution,{TransformNumber::Binary,0,expression.operation,left.expression,right.expression});
        if(expression.operation==Op::Less)result.kind=TransformValue::LessThan;break;
    }
    case TransformExpression::Not:{TransformValue operand;if(!resolve(expression.left,operand)||operand.kind!=TransformValue::NanTest)return false;result.kind=TransformValue::NegatedNanTest;break;}
    }
    cache[at]=result;return true;
}
bool PrepareTransformReturn(const Chunk& chunk,StringTransformPlan& plan) {
    std::vector<int> stack;
    const auto emit=[&](TransformExpression node){plan.expressions.push_back(std::move(node));return static_cast<int>(plan.expressions.size()-1);};
    const auto pop=[&](){const auto at=stack.back();stack.pop_back();return at;};
    for(size_t ip=plan.exit;ip<chunk.code.size()&&ip<plan.exit+64;++ip){
        const auto& instruction=chunk.code[ip];
        switch(instruction.op){
        case Op::NoOp:case Op::GetValue:case Op::PrepareCall:case Op::ToPropertyKey:break;
        case Op::LoadReference:{TransformExpression node{TransformExpression::Binding};node.name=instruction.text;stack.push_back(emit(std::move(node)));break;}
        case Op::Constant:{TransformExpression node{TransformExpression::Constant};node.constant=chunk.constants[instruction.argument];stack.push_back(emit(std::move(node)));break;}
        case Op::Template:{
            if(!chunk.constants[instruction.argument].StringText().empty())return false;
            TransformExpression node{TransformExpression::Constant};node.constant=Value::String(L"");stack.push_back(emit(std::move(node)));break;
        }
        case Op::GetProperty:case Op::GetIndex:{
            if(stack.size()<(instruction.op==Op::GetIndex?2u:1u))return false;
            TransformExpression node{TransformExpression::Member};
            if(instruction.op==Op::GetIndex)node.right=pop();else node.name=instruction.text;
            node.left=pop();stack.push_back(emit(std::move(node)));break;
        }
        case Op::Call:{
            if(instruction.argument<0||instruction.argument>4||instruction.text==L"direct-eval"||stack.size()<static_cast<size_t>(instruction.argument)+1)return false;
            TransformExpression node{TransformExpression::Call};node.arguments.resize(instruction.argument);
            for(int at=instruction.argument-1;at>=0;--at)node.arguments[at]=pop();
            node.left=pop();stack.push_back(emit(std::move(node)));break;
        }
        case Op::Return:
            if(stack.size()!=1)return false;plan.terminal=stack.back();plan.returnInstruction=ip;return true;
        default:return false;
        }
    }
    return false;
}
bool TransformPrivateArray(const Chunk& chunk,size_t start,const std::wstring& binding) {
    if(binding.empty())return false;
    size_t allocation=chunk.code.size();
    const auto closureSafe=[&](const auto& self,const Chunk& body,unsigned depth)->bool{
        if(depth>16)return false;
        for(const auto& instruction:body.code){
            if(instruction.op==Op::LoadReference&&(instruction.text==binding||instruction.text==L"arguments"||instruction.text==L"eval"))return false;
            if(instruction.op==Op::MakeFunction){
                if(instruction.argument<0||static_cast<size_t>(instruction.argument)>=module->prototypes.size()||
                   !self(self,module->prototypes[instruction.argument]->chunk,depth+1))return false;
            }
        }
        for(const auto& declaration:body.functionDeclarations)
            if(declaration.prototype<0||static_cast<size_t>(declaration.prototype)>=module->prototypes.size()||
               !self(self,module->prototypes[declaration.prototype]->chunk,depth+1))return false;
        return true;
    };
    for(const auto& declaration:chunk.functionDeclarations)
        if(declaration.prototype<0||static_cast<size_t>(declaration.prototype)>=module->prototypes.size()||
           !closureSafe(closureSafe,module->prototypes[declaration.prototype]->chunk,0))return false;
    for(size_t ip=0;ip<start;++ip){const auto& instruction=chunk.code[ip];
        switch(instruction.op){
        case Op::Jump:case Op::JumpFalse:case Op::JumpFalseKeep:case Op::JumpTrueKeep:case Op::JumpNotNullishKeep:
        case Op::Return:case Op::ThrowValue:case Op::Await:case Op::Yield:case Op::YieldSpread:return false;
        case Op::MakeFunction:
            if(instruction.argument<0||static_cast<size_t>(instruction.argument)>=module->prototypes.size()||
               !closureSafe(closureSafe,module->prototypes[instruction.argument]->chunk,0))return false;
            break;
        case Op::LoadReference:
            if(instruction.text==L"arguments"||instruction.text==L"eval")return false;
            if(instruction.text==binding&&!(ip+2<start&&chunk.code[ip+1].op==Op::NewArray&&
                chunk.code[ip+1].argument==0&&chunk.code[ip+2].op==Op::Assign))return false;
            break;
        case Op::NewArray:
            if(instruction.argument==0&&ip+1<start&&
               ((chunk.code[ip+1].op==Op::Declare&&chunk.code[ip+1].text==binding)||
                (ip&&chunk.code[ip-1].op==Op::LoadReference&&chunk.code[ip-1].text==binding&&chunk.code[ip+1].op==Op::Assign)))allocation=ip;
            break;
        case Op::Declare:
            if(instruction.text==binding&&(ip==0||chunk.code[ip-1].op!=Op::NewArray||chunk.code[ip-1].argument!=0))return false;
            break;
        default:break;
        }
    }
    return allocation<start;
}
// A bounded integer graph avoids repeated floating-point remainder/conversion.
// Prove the entire graph first: division and possible overflow keep the double
// path, preserving signed zero and all Number semantics that could observe it.
bool PrepareIntegerTransform(const TransformResolution& resolution,size_t length) {
    if(length>2147483647)return false;
    std::array<std::pair<double,double>,128> bounds{};
    const double low=-2147483648.0,high=2147483647.0;
    for(size_t at=0;at<resolution.numbers.size();++at){
        const auto& number=resolution.numbers[at];auto& bound=bounds[at];
        switch(number.kind){
        case TransformNumber::Constant:
            if(!std::isfinite(number.number)||std::trunc(number.number)!=number.number)return false;
            bound={number.number,number.number};break;
        case TransformNumber::Index:bound={0,static_cast<double>(length)};break;
        case TransformNumber::Character:bound={0,65535};break;
        case TransformNumber::Binary:{
            const auto a=bounds[number.left],b=bounds[number.right];
            switch(number.operation){
            case Op::Add:bound={a.first+b.first,a.second+b.second};break;
            case Op::Subtract:bound={a.first-b.second,a.second-b.first};break;
            case Op::Multiply:{
                const double products[]={a.first*b.first,a.first*b.second,a.second*b.first,a.second*b.second};
                bound={*std::min_element(products,products+4),*std::max_element(products,products+4)};break;
            }
            case Op::Modulo:{
                if((b.first<=0&&b.second>=0)||(a.first==low&&b.first<=-1&&b.second>=-1))return false;
                const double magnitude=std::max(std::abs(b.first),std::abs(b.second))-1;
                bound={a.first>=0?0:-magnitude,a.second<=0?0:magnitude};break;
            }
            case Op::BitwiseAnd:
                if(a.first>=0&&b.first>=0)bound={0,std::min(a.second,b.second)};
                else bound={low,high};break;
            case Op::BitwiseOr:case Op::BitwiseXor:bound={low,high};break;
            default:return false;
            }
            break;
        }
        }
        if(bound.first<low||bound.second>high)return false;
    }
    return true;
}
bool TryStringTransform(const std::shared_ptr<ExecutionFrame>& frame,size_t end) {
    if(!jitCompilationThreshold||diagnosticInstructionLimit||traceFunctions||traceErrors||profileExecution||hostInterrupted||
       callStack.size()>=96||!frame->stack.empty()||frame->env==global||frame->chunk->code[end].argument<0||
       end-static_cast<size_t>(frame->chunk->code[end].argument)>256)return false;
    const auto key=std::make_pair(frame->chunk->identity,end);
    auto& eligibility=frame->chunk->transformEligibility;
    if(eligibility.empty())eligibility.resize(frame->chunk->code.size());
    if(eligibility[end])return false;
    auto found=stringTransformPlans.find(key);
    if(found==stringTransformPlans.end()){
        if(stringTransformPlans.size()>=256)return false;
        found=stringTransformPlans.emplace(key,StringTransformPlan{}).first;
    }
    auto& plan=found->second;
    if(!plan.checked){if(++plan.warm<64)return false;plan.checked=true;plan.supported=PrepareStringTransform(*frame->chunk,end,plan);
        if(plan.supported)PrepareTransformReturn(*frame->chunk,plan);}
    if(!plan.supported){eligibility[end]=1;stringTransformPlans.erase(found);return false;}
    auto counter=frame->env->values.find(plan.counter),sample=frame->env->values.find(plan.sample);
    if(counter==frame->env->values.end()||sample==frame->env->values.end()||frame->env->immutableBindings.count(plan.counter)||
       frame->env->immutableBindings.count(plan.sample)||counter->second.realm||counter->second.type!=Value::Type::Number||
       !std::isfinite(counter->second.number)||counter->second.number<0||std::floor(counter->second.number)!=counter->second.number)return false;
    TransformResolution resolution;TransformValue condition,body;
    resolution.counterBinding=&counter->second;resolution.sampleBinding=&sample->second;
    std::vector<std::optional<TransformValue>> cache(plan.expressions.size());
    if(!ResolveTransformExpression(plan,plan.condition,frame->env,resolution,cache,condition)||condition.kind!=TransformValue::NegatedNanTest||
       !ResolveTransformExpression(plan,plan.body,frame->env,resolution,cache,body)||body.kind!=TransformValue::Push||
       resolution.input.type!=Value::Type::String||resolution.output.realm||resolution.transformed<0)return false;
    const auto array=resolution.output.object;
    if(!array||array->kind!=ObjectKind::Array||!array->props.empty()||array->hasIndexedDescriptors||array->prototype!=arrayPrototype)return false;
    // A prototype setter, inherited element, nonordinary prototype or replaced
    // prototype link could observe push. Reject all such cases conservatively.
    size_t ancestors=0;
    for(auto prototype=arrayPrototype;prototype;prototype=prototype->prototype){
        if(++ancestors>8||(prototype!=arrayPrototype&&prototype->kind!=ObjectKind::Plain)||prototype->props.count(L"$prototypeValue"))return false;
        for(const auto& property:prototype->props){
            const auto& name=property.first;
            const size_t offset=name.rfind(L"$get:",0)==0||name.rfind(L"$set:",0)==0?5:0;
            if(name.size()>offset&&name[offset]>=L'0'&&name[offset]<=L'9')return false;
        }
    }
    const auto& input=resolution.input.StringText();
    if(counter->second.number>=static_cast<double>(input.size()))return false;
    const auto first=static_cast<size_t>(counter->second.number)+1;
    if(first>=input.size())return false;
    TransformValue terminal;
    const bool packed=plan.terminal>=0&&input.size()<=10000000&&array->items.size()==first&&
        frame->env->values.count(resolution.outputBinding)&&
        TransformPrivateArray(*frame->chunk,plan.start,resolution.outputBinding)&&
        ResolveTransformExpression(plan,plan.terminal,frame->env,resolution,cache,terminal)&&terminal.kind==TransformValue::Join&&
        std::all_of(array->items.begin(),array->items.end(),[](const Value& value){return value.type==Value::Type::String&&!value.realm&&value.StringText().size()==1;});
    const auto count=packed?input.size()-first:std::min<size_t>(4096,input.size()-first);
    if(array->items.size()+count>=4294967295ull)return false;
    std::wstring joined;
    if(packed){joined.reserve(input.size());for(const auto& value:array->items)joined+=value.StringText();}
    const auto required=array->items.size()+count;
    if(!packed&&required>array->items.capacity())
        array->items.reserve(std::max(required,std::min<size_t>(4294967294ull,std::max<size_t>(256,array->items.capacity()*2))));
    std::array<double,128> values{};
    std::array<std::int32_t,128> integers{};
    const bool integral=PrepareIntegerTransform(resolution,input.size());
    for(size_t at=0;at<resolution.numbers.size();++at)
        if(resolution.numbers[at].kind==TransformNumber::Constant){values[at]=resolution.numbers[at].number;if(integral)integers[at]=static_cast<std::int32_t>(values[at]);}
    for(size_t index=first;index<first+count;++index){
        if(packed&&((index-first)&4095)==0){
            if(interruptionSignal&&interruptionSignal->load(std::memory_order_relaxed))
                throw JavaScriptException{ErrorValue(L"AbortError",L"Worker terminated")};
            if(executionYieldHandler){const auto now=GetTickCount64();if(now>=nextExecutionYield){
                nextExecutionYield=now+32;
                counter->second.number=static_cast<double>(index-1);sample->second=Value::Number(static_cast<unsigned short>(input[index-1]));
                const auto beforeYield=executedInstructions;
                const auto resume=plan.start;
                if(!executionYieldHandler()){hostInterrupted=true;throw JavaScriptException{ErrorValue(L"AbortError",L"Script interrupted by the host")};}
                if(executedInstructions!=beforeYield){
                    // Host reentry can replace an intrinsic or a captured
                    // helper. Materialize the private array at this safepoint
                    // and let the VM observe those changes from the next test.
                    for(size_t at=array->items.size();at<joined.size();++at)
                        array->items.push_back(Value::String(std::wstring(1,joined[at])));
                    ++stringTransformCalls;stringTransformIterations+=index-first;
                    executedInstructions+=index-first;
                    frame->ip=resume;return true;
                }
            }}
        }
        const double character=static_cast<unsigned short>(input[index]);
        if(integral)for(size_t at=0;at<resolution.numbers.size();++at){const auto& number=resolution.numbers[at];
            switch(number.kind){
            case TransformNumber::Constant:break;
            case TransformNumber::Index:integers[at]=static_cast<std::int32_t>(index);break;
            case TransformNumber::Character:integers[at]=static_cast<std::int32_t>(character);break;
            case TransformNumber::Binary:{
                const auto a=integers[number.left],b=integers[number.right];
                switch(number.operation){
                case Op::Add:integers[at]=a+b;break;case Op::Subtract:integers[at]=a-b;break;
                case Op::Multiply:integers[at]=a*b;break;case Op::Modulo:integers[at]=a%b;break;
                case Op::BitwiseAnd:integers[at]=a&b;break;case Op::BitwiseOr:integers[at]=a|b;break;case Op::BitwiseXor:integers[at]=a^b;break;
                default:break;
                }
                break;
            }
            }
        }
        else for(size_t at=0;at<resolution.numbers.size();++at){const auto& number=resolution.numbers[at];
            switch(number.kind){
            case TransformNumber::Constant:break;
            case TransformNumber::Index:values[at]=static_cast<double>(index);break;
            case TransformNumber::Character:values[at]=character;break;
            case TransformNumber::Binary:values[at]=NumberBinary(number.operation,values[number.left],values[number.right]);break;
            }
        }
        const auto raw=integral?static_cast<double>(integers[resolution.transformed]):values[resolution.transformed];
        // ECMA ToUint16, including negative, nonfinite and very large doubles.
        auto code=integral?static_cast<double>(static_cast<std::uint16_t>(integers[resolution.transformed])):
            std::isfinite(raw)?std::fmod(std::trunc(raw),65536.0):0;
        if(code<0)code+=65536.0;
        if(packed)joined.push_back(static_cast<wchar_t>(code));
        else array->items.push_back(Value::String(std::wstring(1,static_cast<wchar_t>(code))));
    }
    counter->second.number=static_cast<double>(first+count-1);sample->second=Value::Number(static_cast<unsigned short>(input[first+count-1]));
    ++stringTransformCalls;stringTransformIterations+=count;
    executedInstructions+=count+resolution.numbers.size();
    if(packed){
        ++packedStringTransformCalls;packedStringTransformIterations+=count;
        // The array cannot escape, and its sole remaining use is the guarded
        // builtin join(""). Keep the terminal test's local state and let the
        // ordinary VM perform Return/async completion with the resulting string.
        counter->second.number=static_cast<double>(input.size());sample->second=Value::Number(std::numeric_limits<double>::quiet_NaN());
        frame->stack.push_back(Value::String(std::move(joined)));frame->ip=plan.returnInstruction;
    }else frame->ip=plan.start;
    return true;
}

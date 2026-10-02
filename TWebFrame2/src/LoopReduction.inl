// Included inside RuntimeCore. Compile a narrowly proved, side-effect-free
// periodic string-length reduction from bytecode, independent of source names.
struct TableReductionPlan {
    struct Node {enum Kind {Number,Binding,Binary,Call,Length} kind;double number=0;std::wstring name;Op op=Op::NoOp;int left=-1,right=-1;};
    std::vector<Node> nodes;
    std::wstring counter,sum,lookup;
    int bound=-1,argument=-1;
    size_t start=0,exit=0,instructions=0,warm=0;
    bool checked=false,supported=false;
};
std::map<std::pair<std::uint64_t,size_t>,TableReductionPlan> tableReductionPlans;
std::uint64_t tableReductionCalls=0,tableReductionIterations=0;
bool PrepareTableReduction(const Chunk& chunk,size_t end,TableReductionPlan& plan){
    using Node=TableReductionPlan::Node;
    if(!chunk.handlers.empty()||end>=chunk.code.size()||chunk.code[end].op!=Op::Jump||chunk.code[end].argument<0)return false;
    plan.start=static_cast<size_t>(chunk.code[end].argument);
    if(plan.start>=end||end-plan.start>128)return false;
    struct Operand {int expression=-1;std::wstring reference;};
    std::vector<Operand> stack;bool condition=false,added=false,incremented=false;
    const auto emit=[&](Node node){plan.nodes.push_back(std::move(node));return static_cast<int>(plan.nodes.size()-1);};
    const auto read=[&](const Operand& value){if(value.reference.empty())return value.expression;Node node{Node::Binding};node.name=value.reference;return emit(std::move(node));};
    const auto pop=[&](){const auto value=stack.back();stack.pop_back();return value;};
    const auto isBinding=[&](int at,const std::wstring& name){return at>=0&&plan.nodes[at].kind==Node::Binding&&plan.nodes[at].name==name;};
    std::vector<size_t> visited;size_t ip=plan.start;
    for(;;){if(ip>=chunk.code.size()||visited.size()>=128||std::find(visited.begin(),visited.end(),ip)!=visited.end())return false;
        visited.push_back(ip);const auto& instruction=chunk.code[ip];
        switch(instruction.op){
        case Op::NoOp:break;
        case Op::LoadReference:stack.push_back({-1,instruction.text});break;
        case Op::Constant:{if(instruction.argument<0||static_cast<size_t>(instruction.argument)>=chunk.constants.size())return false;
            const auto& v=chunk.constants[instruction.argument];if(v.type!=Value::Type::Number)return false;
            Node node{Node::Number};node.number=v.number;stack.push_back({emit(std::move(node))});break;}
        case Op::GetValue:case Op::PrepareCall:if(stack.empty())return false;stack.back()={read(stack.back())};break;
        case Op::Duplicate:if(stack.empty())return false;stack.push_back(stack.back());break;
        case Op::Pop:if(stack.empty())return false;pop();break;
        case Op::Add:case Op::Subtract:case Op::BitwiseAnd:case Op::Less:{
            if(stack.size()<2)return false;const auto right=read(pop()),left=read(pop());Node node{Node::Binary};node.op=instruction.op;node.left=left;node.right=right;
            stack.push_back({emit(std::move(node))});break;}
        case Op::Call:{if(instruction.argument!=1||instruction.text==L"direct-eval"||stack.size()<2)return false;
            const auto argument=read(pop()),callee=read(pop());if(plan.nodes[callee].kind!=Node::Binding)return false;
            Node node{Node::Call};node.name=plan.nodes[callee].name;node.left=argument;stack.push_back({emit(std::move(node))});break;}
        case Op::GetProperty:{if(instruction.text!=L"length"||stack.empty())return false;
            Node node{Node::Length};node.left=read(pop());stack.push_back({emit(std::move(node))});break;}
        case Op::JumpFalse:{if(condition||added||incremented||stack.empty()||instruction.argument<=static_cast<int>(plan.start))return false;
            const auto at=read(pop());const auto& test=plan.nodes[at];
            if(test.kind!=Node::Binary||test.op!=Op::Less||plan.nodes[test.left].kind!=Node::Binding)return false;
            plan.counter=plan.nodes[test.left].name;plan.bound=test.right;plan.exit=instruction.argument;condition=true;break;}
        case Op::Assign:{if(!condition||added||incremented||stack.size()<2)return false;
            const auto value=read(pop());const auto target=pop();if(target.reference.empty()||target.reference==plan.counter)return false;
            const auto& sum=plan.nodes[value];if(sum.kind!=Node::Binary||sum.op!=Op::Add||!isBinding(sum.left,target.reference))return false;
            const auto& length=plan.nodes[sum.right];if(length.kind!=Node::Length)return false;
            const auto& call=plan.nodes[length.left];if(call.kind!=Node::Call)return false;
            plan.sum=target.reference;plan.lookup=call.name;plan.argument=call.left;added=true;stack.push_back({value});break;}
        case Op::PostIncrement:case Op::PreIncrement:{if(!added||incremented||stack.empty())return false;
            const auto operand=pop();if(operand.reference!=plan.counter)return false;incremented=true;
            // The increment result must be discarded, not used in another operation.
            // The ordinary VM may insert a GetValue before discarding the result.
            const auto next=ip+1<chunk.code.size()&&chunk.code[ip+1].op==Op::GetValue?ip+2:ip+1;
            if(next>=chunk.code.size()||chunk.code[next].op!=Op::Pop)return false;stack.push_back({});break;}
        case Op::Jump:{if(instruction.argument<0)return false;const auto target=static_cast<size_t>(instruction.argument);
            if(target==plan.start)goto completed;
            ip=target;continue;}
        default:return false;
        }
        ++ip;
    }
completed:
    if(!condition||!added||!incremented||!stack.empty())return false;
    const auto& bound=plan.nodes[plan.bound];
    if(bound.kind!=Node::Number&&(bound.kind!=Node::Binding||bound.name==plan.sum||bound.name==plan.counter))return false;
    if(std::find(visited.begin(),visited.end(),plan.exit)!=visited.end())return false;
    plan.instructions=visited.size();return true;
}
struct ReductionIndex {bool valid=false,constant=false;double offset=0;std::uint32_t mask=0;bool masked=false;};
static ReductionIndex CombineReductionIndex(Op op,ReductionIndex a,ReductionIndex b){
    if(!a.valid||!b.valid)return {};
    if(op==Op::Add){if(a.constant)std::swap(a,b);if(!b.constant)return {};a.offset+=b.offset;return a;}
    if(op==Op::Subtract&&b.constant){a.offset-=b.offset;return a;}return {};
}
ReductionIndex ReductionArgument(const TableReductionPlan& plan,int at){
    using Node=TableReductionPlan::Node;const auto& node=plan.nodes[at];
    if(node.kind==Node::Number)return {true,true,node.number};
    if(node.kind==Node::Binding)return node.name==plan.counter?ReductionIndex{true,false,0}:ReductionIndex{};
    if(node.kind!=Node::Binary)return {};
    auto a=ReductionArgument(plan,node.left),b=ReductionArgument(plan,node.right);
    if(node.op==Op::BitwiseAnd){if(a.constant)std::swap(a,b);
        if(!a.valid||a.constant||a.masked||a.offset!=0||!b.valid||!b.constant||b.offset<0||b.offset>65535||std::floor(b.offset)!=b.offset)return {};
        const auto mask=static_cast<std::uint32_t>(b.offset);if((mask&(mask+1))!=0)return {};
        a.mask=mask;a.masked=true;return a;}
    return CombineReductionIndex(node.op,a,b);
}
const Value* ReductionArray(const Value& callee,const std::vector<Prototype::ReadOperation>& operations,int at){
    using Kind=Prototype::ReadOperation::Kind;const auto& op=operations[at];
    if(op.kind==Kind::Captured)return callee.function->closure?callee.function->closure->FindValue(op.name):nullptr;
    if(op.kind!=Kind::Getter)return nullptr;
    const auto getter=ReductionArray(callee,operations,op.left);
    if(!getter||getter->realm||getter->type!=Value::Type::Function||!getter->function||!getter->function->prototype||!PrepareCapturedGetter(*getter->function->prototype))return nullptr;
    const auto& name=getter->function->prototype->chunk.code[0].text;
    return getter->function->closure?getter->function->closure->FindValue(name):nullptr;
}
ReductionIndex ReductionLookupIndex(const std::vector<Prototype::ReadOperation>& operations,int at,ReductionIndex argument){
    using Kind=Prototype::ReadOperation::Kind;const auto& op=operations[at];
    if(op.kind==Kind::Parameter)return op.left==0?argument:ReductionIndex{};
    if(op.kind==Kind::Constant)return {true,true,op.number};
    if(op.kind!=Kind::Numeric)return {};
    return CombineReductionIndex(op.operation,ReductionLookupIndex(operations,op.left,argument),ReductionLookupIndex(operations,op.right,argument));
}
bool TryTableReduction(const std::shared_ptr<ExecutionFrame>& frame,size_t end){
    static const bool traceLoop=GetEnvironmentVariableW(L"TWEBFRAME_TRACE_LOOP",nullptr,0)!=0;
    if(!jitCompilationThreshold||diagnosticInstructionLimit||traceFunctions||traceErrors||profileExecution||hostInterrupted||callStack.size()>=103||!frame->stack.empty()||frame->env==global)return false;
    const auto& code=frame->chunk->code;
    if(code[end].argument<0||end-static_cast<size_t>(code[end].argument)>128)return false;
    auto& eligibility=frame->chunk->reductionEligibility;
    if(eligibility.empty())eligibility.resize(code.size());
    if(eligibility[end]==1)return false;
    const auto key=std::make_pair(frame->chunk->identity,end);
    auto found=tableReductionPlans.find(key);
    if(found==tableReductionPlans.end()){if(tableReductionPlans.size()>=256){eligibility[end]=1;return false;}found=tableReductionPlans.emplace(key,TableReductionPlan{}).first;}
    auto& plan=found->second;
    if(!plan.checked){if(++plan.warm<64)return false;plan.checked=true;plan.supported=PrepareTableReduction(*frame->chunk,end,plan);
        if(traceLoop){std::fwprintf(stderr,L"LOOP_PLAN supported=%d start=%zu end=%zu counter=%ls sum=%ls lookup=%ls\n",plan.supported,plan.start,end,plan.counter.c_str(),plan.sum.c_str(),plan.lookup.c_str());
            for(size_t i=plan.start;i<=end&&i<plan.start+130;++i){const auto& in=frame->chunk->code[i];std::fwprintf(stderr,L"LOOP_OP %zu op=%u arg=%d name=%ls\n",i,unsigned(in.op),in.argument,in.text.c_str());}}
    }
    if(!plan.supported){eligibility[end]=1;return false;}
    auto counter=frame->env->values.find(plan.counter),sum=frame->env->values.find(plan.sum);
    if(counter==frame->env->values.end()||sum==frame->env->values.end())return false;
    const auto numeric=[](const Value& v){return v.type==Value::Type::Number&&!v.realm&&std::isfinite(v.number)&&std::floor(v.number)==v.number;};
    if(!numeric(counter->second)||!numeric(sum->second)||counter->second.number<0||std::abs(sum->second.number)>9007199254740991.0)return false;
    using Node=TableReductionPlan::Node;const auto& boundNode=plan.nodes[plan.bound];
    const auto boundValue=boundNode.kind==Node::Binding?frame->env->FindValue(boundNode.name):nullptr;
    if(boundNode.kind==Node::Binding&&(!boundValue||!numeric(*boundValue)))return false;
    const double bound=boundNode.kind==Node::Number?boundNode.number:boundValue->number;
    if(!std::isfinite(bound)||std::floor(bound)!=bound||bound<=counter->second.number||bound>2147483647)return false;
    const auto callable=frame->env->FindValue(plan.lookup);
    if(!callable||callable->realm||callable->type!=Value::Type::Function||!callable->function||!callable->function->prototype)return false;
    auto& prototype=*callable->function->prototype;
    if(prototype.isAsync||prototype.isGenerator||!prototype.restParameter.empty()||!prototype.bindings.empty()||
        std::any_of(prototype.parameterDefaults.begin(),prototype.parameterDefaults.end(),[](const auto& p){return bool(p);})||!PrepareReadPlan(prototype))return false;
    const auto& ops=prototype.readPlan;const auto& read=ops.back();
    const auto index=ReductionLookupIndex(ops,read.right,ReductionArgument(plan,plan.argument));
    if(!index.valid||index.constant||!index.masked||index.offset<0||index.offset>4294967295.0||std::floor(index.offset)!=index.offset)return false;
    const auto array=ReductionArray(*callable,ops,read.left);
    if(!array||array->realm||array->type!=Value::Type::Object||!array->object||array->object->kind!=ObjectKind::Array||array->object->hasIndexedDescriptors||!array->object->props.empty())return false;
    const auto offset=static_cast<size_t>(index.offset),period=static_cast<size_t>(index.mask)+1;
    if(offset>array->object->items.size()||period>array->object->items.size()-offset)return false;
    const auto& items=array->object->items;
    double total=0;std::vector<double> lengths;lengths.reserve(period);
    for(size_t i=0;i<period;++i){const auto& v=items[offset+i];if(v.type!=Value::Type::String||v.realm)return false;
        const auto length=static_cast<double>(v.StringText().size());lengths.push_back(length);}
    const auto range=[&](size_t start,size_t count){double value=0;size_t i=0;
#if defined(_M_X64) || defined(_M_IX86)
        auto vector=_mm_setzero_pd();for(;i+2<=count;i+=2)vector=_mm_add_pd(vector,_mm_loadu_pd(lengths.data()+start+i));
        alignas(16) double lanes[2];_mm_store_pd(lanes,vector);value=lanes[0]+lanes[1];
#endif
        for(;i<count;++i)value+=lengths[start+i];return value;};
    const auto count=static_cast<size_t>(std::min(65536.0,bound-counter->second.number));
    const auto first=static_cast<size_t>(counter->second.number)&index.mask;
    const auto initial=std::min(count,period-first),remaining=count-initial;
    total=range(first,initial)+static_cast<double>(remaining/period)*range(0,period)+range(0,remaining%period);
    const auto result=sum->second.number+total;
    if(!std::isfinite(total)||total>9007199254740991.0||!std::isfinite(result)||std::abs(result)>9007199254740991.0)return false;
    // All reads and type/bounds guards completed before either local is written.
    sum->second.number=result;counter->second.number+=static_cast<double>(count);
    // Count executed VM work, as the numeric JIT already does, rather than
    // triggering GC for millions of instructions and allocations we eliminated.
    executedInstructions+=plan.instructions;++tableReductionCalls;tableReductionIterations+=count;
    frame->ip=counter->second.number>=bound?plan.exit:plan.start;return true;
}

function check(value,message){if(!value)throw new Error(message);}
var writes=0,storage=4,keyReads=0,key={toString:function(){keyReads++;return 'live';}};
var object={get 1(){return storage;},set 1(value){writes++;storage=value;},get 'quoted key'(){return 19;},set 'quoted key'(value){storage=value;},get [key](){return storage;},set [key](value){storage=value;}};
check(object[1]===4 && object['quoted key']===19 && object.live===4 && keyReads===2,'literal and computed accessor keys');
object[1]=11;check(storage===11 && writes===1 && object.live===11,'numeric setter');
object['quoted key']=17;object.live=23;check(storage===23,'quoted and computed setters');
var descriptor=Object.getOwnPropertyDescriptor(object,'1');
check(typeof descriptor.get==='function' && typeof descriptor.set==='function' && descriptor.enumerable && descriptor.configurable,'accessor descriptors');
var symbol=Symbol('accessor'),symbolObject={get [symbol](){return 31;},set [symbol](value){storage=value;}};
symbolObject[symbol]=37;check(symbolObject[symbol]===31 && storage===37,'symbol accessor keys');
var named={get(){return 41;},set(){return 43;}};check(named.get()===41 && named.set()===43,'methods named get and set remain ordinary methods');
class Holder{get 'quoted'(){return 47;}set 'quoted'(value){this.saved=value;}get 1(){return 53;}set 1(value){this.saved=value;}}
var instance=new Holder();instance.quoted=59;check(instance.quoted===47 && instance[1]===53 && instance.saved===59,'class quoted and numeric accessors');
var invalid=0;try{Function('return {get x(value){}}');}catch(error){if(error instanceof SyntaxError)invalid++;}
try{Function('return {set x(){}}');}catch(error){if(error instanceof SyntaxError)invalid++;}
check(invalid===2,'accessor parameter arity is checked');
var order='',orderedKey={toString:function(){order+='key';return 'value';}},ordered={[orderedKey]:(order+='initializer',61)};
check(order==='keyinitializer' && ordered[orderedKey]===61 && order==='keyinitializerkey','computed key conversion precedes initializer and runs on indexed lookup');
var numeric={get 1.0(){return 67;},get 0x10(){return 71;}};check(numeric[1]===67 && numeric[16]===71,'numeric literal keys are canonicalized');
var marker={},valueRan=false,badKey={toString:function(){throw marker;}};
var caught=false;try{var bad={[badKey]:(valueRan=true)};}catch(error){caught=error===marker;}check(caught && !valueRan,'key coercion errors skip initializer');
var hinted={[Symbol.toPrimitive]:function(hint){check(hint==='string','property key conversion hint');return symbol;}};
var hintedObject={[hinted]:73};check(hintedObject[symbol]===73,'symbol returned from primitive conversion preserves its identity');
return 'PASS|object and class accessor syntax';

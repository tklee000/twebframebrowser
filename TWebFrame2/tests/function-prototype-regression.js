function check(value,message){if(!value)throw new Error(message);}
function target(value){return this.base+value;}
var prototype=Function.prototype;
check(target.call===prototype.call, 'call method identity');
check(target.apply===prototype.apply && target.bind===prototype.bind, 'apply and bind method identity');
check(String.call===prototype.call, 'native functions share call');
check(target.call({base:11},3)===14 && target.apply({base:11},[3])===14, 'call and apply receiver');
check(target.bind({base:11},3)()===14, 'bind receiver');
var original=Object.getOwnPropertyDescriptor(prototype,'call');
prototype.call=function(){return 71;};
check(target.call({base:11},3)===71, 'prototype method replacement');
Object.defineProperty(prototype,'call',original);
delete prototype.call;
check(target.call===undefined, 'prototype method deletion');
Object.defineProperty(prototype,'call',original);
Object.prototype.inheritedFunctionValue=83;
check(target.inheritedFunctionValue===83, 'function prototype inherits ordinary properties');
delete Object.prototype.inheritedFunctionValue;
var reads=0;
var wrapped=new Proxy(target,{get:function(fn,key,receiver){reads++;return fn[key];}});
check(wrapped.call===prototype.call && reads===1, 'callable proxy get trap');
check(wrapped.call({base:5},4)===9 && reads===2, 'callable proxy method receiver');
var rejected=false;
try{prototype.call.call({},null);}catch(error){rejected=error.name==='TypeError';}
check(rejected, 'call rejects non-callable receiver');
Object.defineProperty(prototype,'receiverForLookup',{get:function(){return this;},configurable:true});
check(target.receiverForLookup===target && String.receiverForLookup===String, 'prototype getter receives original function');
delete prototype.receiverForLookup;
check(target.__proto__===prototype && String.__proto__===prototype, 'function prototype accessor');
var originalConstructor=Function;
Function=null;
check(target.call===prototype.call && target.__proto__===prototype && Object.getPrototypeOf(target)===prototype,
      'intrinsic prototype survives global constructor replacement');
Function=originalConstructor;
function isolated(){}
Object.setPrototypeOf(isolated,null);
check(isolated.call===undefined, 'explicit null prototype has no inherited methods');
return 'PASS';

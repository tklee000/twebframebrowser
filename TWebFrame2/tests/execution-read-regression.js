function check(condition, message) { if (!condition) throw new Error(message); }
var source=['first','second'], object={base:7}, reads=0, marker={};
function captured(){return source;}
function lookup(index, offset, values, result){index=index-offset;values=captured();result=values[index];return result;}
function method(target, value){return target.add(value);}
function computed(target, key, value){return target[key](value);}
function strictTarget(value){'use strict';return this.base+value;}
object.add=strictTarget;
function same(left,right){return left===right;}
function less(left,right){return left<right;}
function unary(value){return !value;}
for(var warm=0;warm<2000;warm++){
    check(lookup(3,2)==='second','indexed captured binding');
    check(method(object,3)===10 && computed(object,'add',4)===11,'method receiver');
    check(same(object,object) && !same(object,{}) && less('10','2') && unary(0),'value comparisons');
}
source=['changed','updated'];
check(lookup(2,2)==='changed','hot captured binding reassignment');
function switchTarget(){object.add=function(){return 31;};return 5;}
function callBeforeArguments(){return object.add(switchTarget());}
for(var warm=0;warm<2000;warm++){object.add=strictTarget;check(callBeforeArguments()===12,'callee read before arguments');}
var holder={};
Object.defineProperty(holder,'method',{get:function(){reads++;return strictTarget;},configurable:true});
holder.base=9;
for(var warm=0;warm<2000;warm++) check(computed(holder,'method',2)===11,'getter receiver');
check(reads===2000,'one getter per method read');
Object.defineProperty(holder,'method',{get:function(){throw marker;},configurable:true});
var thrown=false;try{computed(holder,'method',2);}catch(error){thrown=error===marker;}
check(thrown,'getter throws original value');
var proxy=new Proxy(object,{get:function(target,key,receiver){return target[key];}});
object.add=strictTarget;
check(computed(proxy,'add',6)===13,'proxy preserves call receiver');
function receiver(){'use strict';return this;}
function detached(target){return (0,target.add)();}
object.add=receiver;
for(var warm=0;warm<2000;warm++) check(detached(object)===undefined,'comma detaches receiver');
var initial=Date.now(),checksum=0;
for(var index=0;index<100000;index++) checksum+=lookup(3,2).length;
check(checksum===700000,'benchmark checksum');
var lookupMs=Date.now()-initial;
object.add=strictTarget;initial=Date.now();
for(var index=0;index<100000;index++) checksum+=computed(object,'add',3);
check(checksum===1700000,'method benchmark checksum');
return 'PASS|lookupMs='+lookupMs+'|methodMs='+(Date.now()-initial)+'|checksum='+checksum;

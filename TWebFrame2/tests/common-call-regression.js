function check(value, message) { if (!value) throw new Error(message); }
var operations={
    add:function(a,b){return a+b;},
    xor:function(a,b){return a^b;},
    shift:function(a,b){return a>>>b;},
    less:function(a,b){return a<b;}
};
function dispatch(target,key,a,b){return target[key](a,b);}
for(var warm=0;warm<2000;warm++) {
    check(dispatch(operations,'add',3,4)===7,'nested call arguments');
    check(dispatch(operations,'xor',3,4)===7,'numeric leaf result');
    check(operations.less(warm,warm+1)===true,'boolean leaf result preserves its type');
}
check(isNaN(operations.add(1)),'missing argument keeps ordinary initialization');
check(operations.add('a',4)==='a4','nonnumeric guard fallback');
check(isNaN(operations.add(NaN,4)),'NaN leaf argument');
check(operations.add(Infinity,4)===Infinity,'infinite leaf argument');
check(1/operations.add(-0,-0)===-Infinity,'negative zero');
check(operations.shift(-1,0)===4294967295,'unsigned result');
check(operations.less('10','2')===true,'string comparison fallback');
var conversions=0;
check(operations.add({valueOf:function(){conversions++;return 9;}},2)===11 && conversions===1,
    'observable numeric conversion executes once');
var replacement=function(a,b){return a-b;}, marker={}, reads=0;
var holder={};
Object.defineProperty(holder,'method',{get:function(){reads++;return operations.add;},configurable:true});
function replace(){operations.add=replacement;return 4;}
check(holder.method(3,replace())===7 && reads===1,'callee getter precedes argument effects');
check(operations.add(3,4)===-1,'function replacement is observed');
operations.add=function(a,b){return a+b;};
Object.defineProperty(holder,'method',{get:function(){throw marker;},configurable:true});
var caught=false;try{holder.method(3,4);}catch(error){caught=error===marker;}
check(caught,'getter exception identity');
function recursive(depth, object, text){
    if(depth===0)return object.value+'|'+text;
    var child=recursive(depth-1,{value:object.value+1},text+'x');
    return child+'|'+object.value;
}
check(recursive(3,{value:10},'a')==='13|axxx|12|11|10','recursive arguments remain independent');
function fail(value){throw value;}
for(var index=0;index<2000;index++) {
    caught=false;try{fail(marker);}catch(error){caught=error===marker;}
    check(caught && operations.add(index,2)===index+2,'exception releases argument storage');
}
var retained=[];
function retain(value){retained.push(function(){return value;});}
for(var index=0;index<200;index++)retain({value:index});
check(retained[0]().value===0 && retained[199]().value===199,'escaping argument objects remain alive');
function Record(name,value){this.name=name;this.value=value;}
for(var index=0;index<2000;index++) {
    var record=new Record(index%2?'short':'a much longer immutable string value',{value:index});
    check(record.value.value===index && record.name.length>0,'constructor arguments remain independent');
}
function extras(value){return arguments[1];}
check(extras(1,marker)===marker,'arguments object retains extra arguments');
function defaults(value,other=7){return value+other;}
check(defaults(3)===10 && defaults(3,4)===7,'default arguments use ordinary initialization');
function receiver(value){'use strict';return this.base+value;}
var owner={base:8,method:receiver};
check(dispatch(owner,'method',2,0)===10,'unsupported receiver function uses ordinary call');
function detach(target){return (0,target.method)();}
owner.method=function(){'use strict';return this;};
check(detach(owner)===undefined,'detached call receiver');
var proxyReads=0;
var proxy=new Proxy(operations,{get:function(target,key){proxyReads++;return target[key];}});
check(proxy.xor(3,4)===7 && proxyReads===1,'Proxy lookup is observable once');
var started=Date.now(), checksum=0;
for(var index=0;index<300000;index++) {
    checksum+=dispatch(operations,'xor',index&255,17);
    checksum+=operations.shift(index&255,1);
}
check(checksum===57294624,'dispatch benchmark checksum');
return 'PASS|dispatchMs='+(Date.now()-started)+'|checksum='+checksum;

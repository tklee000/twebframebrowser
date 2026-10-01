function check(condition, message) { if (!condition) throw new Error(message); }
var reads=0, marker={}, receiver={value:10};
function sum(a,b) { return this.value+a+b; }
var list=[1,2];
Object.defineProperty(list,'0',{get:function(){reads++;return 7;},configurable:true});
check(sum.apply(receiver,list)===19 && reads===1,'apply reads an array accessor exactly once');
reads=0;
check(Function.prototype.apply.call(sum,receiver,list)===19 && reads===1,'intrinsic apply reads an array accessor');
Object.defineProperty(list,'0',{get:function(){throw marker;},configurable:true});
var caught=false;try{sum.apply(receiver,list);}catch(error){caught=error===marker;}
check(caught,'apply propagates the original thrown getter value');
var order=[];
var proxy=new Proxy({length:2,0:3,1:4},{get:function(target,key){order.push(key);return target[key];}});
check(sum.apply(receiver,proxy)===17 && order.join(',')==='length,0,1','array-like reads use length then ascending indices');
check(sum.apply(receiver,{length:2.9,0:3,1:4})===17,'array-like length is truncated');
check(Reflect.apply(sum,receiver,{length:2,0:3,1:4})===17,'Reflect.apply accepts ordinary array-like objects');
function Box(a,b){this.total=a+b;}
check(Reflect.construct(Box,{length:2,0:3,1:4}).total===7,'Reflect.construct accepts ordinary array-like objects');
check(sum.apply(receiver,{length:-1})!==sum.apply(receiver,{length:-1}),'negative length supplies no arguments');
var typeError=false;try{sum.apply(receiver,'12');}catch(error){typeError=error instanceof TypeError;}
check(typeError,'primitive argumentsList throws TypeError');
var rangeError=false;try{sum.apply(receiver,{length:Infinity});}catch(error){rangeError=error instanceof RangeError;}
check(rangeError,'an unbounded arguments list fails promptly');
var conversions=0;
check(sum.apply(receiver,{length:{valueOf:function(){conversions++;return 2;}},0:3,1:4})===17 && conversions===1,'length is converted once');
return 'PASS|applyArrayLikeReads='+reads+'|lengthConversions='+conversions;

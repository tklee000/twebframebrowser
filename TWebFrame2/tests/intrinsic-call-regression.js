function check(value,message){if(!value)throw new Error(message);}
function add(a,b){return a+b;}
function xor(a,b){return a^b;}
function makeReader(){var values=[11,22,33];return {read:function(i){return values[i];},set:function(value){values=value;}};}
var reader=makeReader();for(var warm=0;warm<200;warm++){add(1,2);xor(1,2);reader.read(1);}
var begin=Date.now(),sum=0;for(var i=0;i<500000;i++){sum+=add.call(null,i,1);sum+=xor.call(undefined,i,7);sum+=reader.read.call(null,1);}
var elapsed=Date.now()-begin;
check(sum===250011000000,'forwarded pure calls preserve results');
check(add.call(null,'a',3)==='a3' && Object.is(add.call(null,-0,-0),-0),'nonnumeric guards and signed zero');
function receiver(){return this.value;}check(receiver.call({value:9})===9,'this-dependent functions use ordinary execution');
var gets=0,holder={};Object.defineProperty(holder,'call',{get:function(){gets++;return Function.prototype.call;}});
check(holder.call.call(add,null,3,4)===7 && gets===1,'observable method getter runs once');
var savedCall=Function.prototype.call,customCalls=0;
add.call=function(){customCalls++;return 91;};check(add.call(null,3,4)===91 && customCalls===1,'own method overrides are observed');
check(savedCall.call(add,null,3,4)===7,'saved intrinsic retains its behavior');
delete add.call;reader.set([4,5,6]);check(reader.read.call(null,1)===5,'captured bindings are checked on each call');
var marker={},caught=false;reader.set({get 1(){throw marker;}});try{reader.read.call(null,1);}catch(error){caught=error===marker;}check(caught,'accessor errors preserve their identity');
return 'PASS|intrinsic-call-ms='+elapsed;

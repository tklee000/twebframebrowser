function check(value, message) { if (!value) throw new Error(message); }
var strings=['short', 'a longer immutable string', '\uD55C\uAE00\uD83D\uDE00 string', 'x'.repeat(400)];
var saved=strings.slice();
var owner={value:strings[1]}, calls=0;
Object.defineProperty(owner,'current',{get:function(){ calls++; return this.value; }});
function readCaptured(){return owner;}
function readText(){return strings[1];}
for(var i=0;i<200;i++) {
    check(readCaptured()===owner, 'captured object identity');
    check(readText()===saved[1], 'immutable string copied through function and array');
    check(owner.current===saved[1], 'getter returns current medium string');
}
check(calls===200, 'getter invoked once per read');
owner={value:'changed'};
check(readCaptured()===owner, 'captured binding is read after reassignment');
var copied=saved[1]; strings[1]='replacement';
check(copied==='a longer immutable string' && readText()==='replacement', 'assignment preserves earlier string value');
var nativeMethod=saved[2].slice;
check(nativeMethod.call('different value',0,9)==='different', 'string method receiver remains dynamic');
var marker={};
Object.defineProperty(owner,'failure',{get:function(){throw marker;}});
var caught=false;
try { var unused=owner.failure; } catch(error) { caught=error===marker; }
check(caught, 'property getter exception identity');
var target=[], container={held:target}, originalPush=Array.prototype.push;
var proxy=new Proxy(target,{get:function(array,key,receiver){
    container.held=undefined;
    for(var i=0;i<100;i++)container['entry'+i]=i;
    return key==='push'?originalPush:array[key];
}});
container.held=proxy;
var push=container.held.push;
push.call(target,23);
check(target.length===1 && target[0]===23, 'receiver survives proxy mutation before native method return');
function strictReceiver(){'use strict';return this;}
check(strictReceiver()===undefined, 'plain strict call retains undefined receiver');
function sloppyReceiver(){return this;}
check(sloppyReceiver()===window, 'plain sloppy call retains global receiver');
var currentCall=function(){return 17;};
function replaceCall(){currentCall=function(){return 29;};return 1;}
check(currentCall(replaceCall())===17 && currentCall()===29, 'callee resolved before arguments');
function scopedEvaluation(){let localValue=31;return eval('localValue+1');}
check(scopedEvaluation()===32, 'direct eval retains local scope');
check(saved[2].length===11 && saved[3].length===400, 'UTF-16 and long string lengths');
return 'PASS';

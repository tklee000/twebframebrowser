function check(value,message){if(!value)throw new Error(message);}
var array=[],push=Array.prototype.push;
check(array.push===push && array.push===array.push && push.length===1 && push.name==='push','push is a stable prototype method');
check(array.push(3,5)===2 && array[0]===3 && array[1]===5 && array.push()===2,'dense push and empty argument list');
array.push=function(){return 7;};check(array.push(11)===7 && array.length===2,'own method overrides remain observable');
var calls=0,coercions=0,writes='',lengthValue=1.9,object={};
Object.defineProperty(object,'length',{get:function(){calls++;return {valueOf:function(){coercions++;return lengthValue;}};},set:function(value){writes+='length:'+value;}});
Object.defineProperty(object,'1',{set:function(value){writes+='index:'+value+';';}});
check(push.call(object,13)===2 && calls===1 && coercions===1 && writes==='index:13;length:2','array-like length coercion and write order');
var negative={length:-4};check(push.call(negative,17)===1 && negative[0]===17 && negative.length===1,'negative length converts to zero');
var overflow={length:9007199254740991},caught=false;try{push.call(overflow,19);}catch(error){caught=error instanceof TypeError;}check(caught && !overflow.hasOwnProperty('9007199254740991'),'safe length overflow rejects before index writes');
var marker={},bad={};Object.defineProperty(bad,'length',{get:function(){throw marker;}});caught=false;try{push.call(bad,23);}catch(error){caught=error===marker;}check(caught,'length getter exception identity');
var inherited=0;Object.defineProperty(Array.prototype,'0',{set:function(value){inherited=value;},configurable:true});var guarded=[];push.call(guarded,29);delete Array.prototype['0'];check(inherited===29 && guarded.length===1,'inherited numeric setter is not skipped by dense path');
var incompatible=0;try{push.call(null,31);}catch(error){if(error instanceof TypeError)incompatible++;}try{push.call('a',31);}catch(error){if(error instanceof TypeError)incompatible++;}check(incompatible===2,'null and immutable string receivers reject');
return 'PASS|array push prototype and coercion rules';

function check(value,message){if(!value)throw new Error(message);}
var effects=[];
function argument(){effects.push('argument');return 1;}
function mustThrow(callback,message){var caught=false;try{callback();}catch(error){caught=error instanceof TypeError;}check(caught,message);}
mustThrow(function(){return undefined.value;},'undefined property read throws');
mustThrow(function(){return null.value;},'null property read throws');
mustThrow(function(){return typeof undefined.value;},'typeof does not suppress a nullish property error');
mustThrow(function(){return undefined.call(argument());},'undefined method access throws');
check(effects.length===0,'invalid method access precedes argument evaluation');
mustThrow(function(){return null['call'](argument());},'computed null method access throws');
check(effects.length===0,'computed invalid method access precedes arguments');
var key={toString:function(){effects.push('key');return 'value';}};
mustThrow(function(){return undefined[key];},'computed undefined read throws');
check(effects.length===0,'nullish receiver fails before key coercion');
check(({})['absent']===undefined,'missing ordinary properties remain undefined');
mustThrow(function(){return ({}).absent(argument());},'missing ordinary method is not callable');
check(effects.join(',')==='argument','ordinary missing callee evaluates arguments');
effects=[];
check(undefined?.[argument()]===undefined && null?.call(argument())===undefined,'optional chains short circuit');
check(effects.length===0,'optional chains have no argument effects');
var captured=[7];
function indexed(n){return captured[n];}
function named(value){return value.value;}
for(var i=0;i<30;i++){check(indexed(0)===7,'warm indexed read');check(named({value:9})===9,'warm named read');}
captured=null;
mustThrow(function(){indexed(0);},'optimized captured read rechecks nullish receiver');
mustThrow(function(){named(undefined);},'optimized named read throws');
var getterReads=0,object={};
Object.defineProperty(object,'value',{get:function(){getterReads++;return null;}});
mustThrow(function(){object.value.call(argument());},'null returned by a getter throws');
check(getterReads===1 && effects.length===0,'getter is read once before failed method arguments');
return 'PASS|nullish member reads and call order';

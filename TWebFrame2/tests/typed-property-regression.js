var checks=0;function check(ok,label){if(!ok)throw new Error(label);checks++;}
function rejects(action,label){var rejected=false;try{action();}catch(e){rejected=e instanceof TypeError;}check(rejected,label);}
var conversions=0,typed=new Uint8Array(3);typed[0]={valueOf:function(){conversions++;return 258;}};
check(typed[0]===2 && conversions===1,'typed assignment uses ToNumber once');
rejects(function(){typed[0]=Symbol('invalid');},'Symbol assignment throws');
rejects(function(){new Uint8Array([Symbol('invalid')]);},'Symbol constructor element throws');
var marker=new Error('original'),caught=false;try{typed[0]={valueOf:function(){throw marker;}};}catch(e){caught=e===marker;}
check(caught && typed[0]===2,'element conversion preserves exception identity and previous data');
var built=new Int16Array([{valueOf:function(){return 65535;}}]);check(built[0]===-1,'constructor elements use numeric conversion');
var buffer=new ArrayBuffer(8),ints=new Int32Array(buffer),bytes=new Uint8Array(buffer);
Object.defineProperty(ints,'1',{value:0x12345678});check(ints[1]===0x12345678 && bytes[4]===0x78 && bytes[7]===0x12,'defineProperty writes the real shared buffer');
var d=Object.getOwnPropertyDescriptor(ints,'1');check(d.value===0x12345678 && d.writable && d.enumerable && d.configurable,'typed element descriptor');
rejects(function(){Object.defineProperty(ints,'0',{get:function(){return 77;}});},'typed element accessor is rejected');
rejects(function(){Object.defineProperty(ints,'0',{writable:false});},'typed element readonly descriptor is rejected');
rejects(function(){Object.defineProperty(ints,'0',{enumerable:false});},'typed element nonenumerable descriptor is rejected');
rejects(function(){Object.defineProperty(ints,'0',{configurable:false});},'typed element nonconfigurable descriptor is rejected');
rejects(function(){Object.defineProperty(ints,'2',{value:1});},'out-of-bounds element definition does not extend the view');
check(ints.length===2 && Object.getOwnPropertyDescriptor(ints,'2')===undefined,'failed descriptor leaves length and storage unchanged');
var inherited=0,prototype=Object.create(Object.getPrototypeOf(ints));
Object.defineProperty(prototype,'1',{get:function(){inherited++;return 999;},set:function(){inherited++;},configurable:true});
Object.defineProperty(prototype,'-1',{get:function(){inherited++;return 999;},configurable:true});
Object.defineProperty(prototype,'NaN',{get:function(){inherited++;return 999;},configurable:true});
Object.setPrototypeOf(ints,prototype);check(ints[1]===0x12345678 && ints['1']===0x12345678 && ints[-1]===undefined && ints.NaN===undefined && inherited===0,'integer indexed reads do not consult inherited properties');
ints[1]=17;check(ints[1]===17 && inherited===0,'integer indexed writes do not invoke inherited setters');
check(!('-1' in ints) && !('NaN' in ints) && '1' in ints,'integer indexed HasProperty uses view bounds');
check(ints['-0']===undefined && ints[-0]===0,'string negative zero is distinct from numeric zero');
var invalidConversions=0;ints['-1']={valueOf:function(){invalidConversions++;return 9;}};
check(invalidConversions===1 && ints[-1]===undefined,'invalid indexed assignment still converts its value');
rejects(function(){ints['NaN']=Symbol('invalid');},'invalid indexed Symbol assignment still throws');
check(delete ints['-1'] && !(delete ints[1]) && ints[1]===17,'integer indexed deletion respects bounds');
rejects(function(){(function(){'use strict';delete ints[1];})();},'strict deletion throws on a live element');
ints['01']=23;ints['+1']=29;ints['1.0']=31;check(ints['01']===23 && ints['+1']===29 && ints['1.0']===31,'noncanonical numeric keys remain ordinary properties');
var order=[],desc={};['enumerable','configurable','value','writable','get','set'].forEach(function(key){Object.defineProperty(desc,key,{get:function(){order.push(key);return key==='value'?37:key==='get'||key==='set'?undefined:true;}});});
rejects(function(){Object.defineProperty(ints,'0',desc);},'mixed data and accessor descriptor is rejected');
check(order.join(',')==='enumerable,configurable,value,writable,get,set','descriptor getters are read in standard order');
return 'PASS|typed-properties|'+checks;

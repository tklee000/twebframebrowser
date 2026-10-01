function check(value, message) { if (!value) throw new Error(message); }
var table=['zero','one'], accessorCalls=0, getterCalls=0, marker={};
function captured(){return table;}
function lookup(index, offset, values, result){index=index-offset;values=captured();result=values[index];return result;}
function fixed(index, unused, values, result){index=index-7;values=captured();result=values[index];return result;}
for(var warm=0;warm<2000;warm++){
    check(lookup(5,4)==='one' && fixed(8)==='one','numeric normalization and captured index');
}
table=['changed','updated'];
check(lookup(4,4)==='changed' && fixed(8)==='updated','captured array reassignment');
check(lookup('8',7)==='updated' && fixed('8')==='updated','conversion uses ordinary fallback');
var original=captured;
captured=function(){getterCalls++;return table;};
check(fixed(8)==='updated' && getterCalls===1,'effectful callable fallback executes once');
captured=original;
Object.defineProperty(table,'1',{get:function(){accessorCalls++;return 'getter';},configurable:true});
check(fixed(8)==='getter' && accessorCalls===1,'one observable index getter');
Object.defineProperty(table,'1',{get:function(){throw marker;},configurable:true});
var sameError=false;try{fixed(8);}catch(error){sameError=error===marker;}
check(sameError,'original thrown getter object');
var proxyReads=0;
table=new Proxy(['proxy-zero','proxy-one'],{get:function(target,key){proxyReads++;return target[key];}});
check(fixed(8)==='proxy-one' && proxyReads===1,'one Proxy get operation');
table=['zero','one'];
function discarded(index, values){index=marker+1;values=captured();return values[0];}
var conversions=0;marker.valueOf=function(){conversions++;return 4;};
check(discarded(0)==='zero' && conversions===1,'discarded observable conversion is preserved');
function twoReads(index, values){values=captured();values[index];return values[index];}
Object.defineProperty(table,'1',{get:function(){accessorCalls++;return 'two';},configurable:true});
accessorCalls=0;check(twoReads(1)==='two' && accessorCalls===2,'multiple reads keep ordinary execution');
table=['zero','one'];
function shifted(index, values){index=(index>>>0)&1;values=captured();return values[index];}
for(var bitWarm=0;bitWarm<100;bitWarm++)check(shifted(4294967297)==='one','unsigned normalization');
check(shifted(NaN)==='zero' && shifted(Infinity)==='zero' && shifted(-0)==='zero','nonfinite and signed zero normalization');
table='ab';check(fixed(8)==='b','captured string index');
table={one:'named'};check(lookup('one',0)===undefined,'unsupported numeric operands retain ordinary conversion');
function named(index, values){values=captured();return values[index];}
check(named('one')==='named','string property keys use ordinary lookup');
table=['zero','one'];
var recursiveReads=0;
Object.defineProperty(table,'1',{get:function(){
    recursiveReads++;table=['replacement','value'];return fixed(8)+'-outer';
},configurable:true});
check(fixed(8)==='value-outer' && recursiveReads===1,'reentrant getter preserves the outer operands');
table=['zero','one'];
var started=Date.now(),checksum=0;
for(var index=0;index<500000;index++)checksum+=fixed(8).length;
check(checksum===1500000,'captured read checksum');
return 'PASS|capturedReadMs='+(Date.now()-started)+'|checksum='+checksum;

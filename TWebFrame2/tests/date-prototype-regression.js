function check(value,message){if(!value)throw new Error(message);}
check(Date.prototype && Object.getPrototypeOf(Date.prototype)===Object.prototype,'Date has its intrinsic prototype');
check(Number.isNaN(Date.prototype.getTime()),'Date prototype has an invalid time value');
var date=new Date(0),getTime=date.getTime;
check(Object.getPrototypeOf(date)===Date.prototype && date.constructor===Date,'Date instance inherits its prototype');
check(getTime===Date.prototype.getTime && getTime.call(date)===0,'date methods have stable callable identity');
date.setTime(1234);check(getTime.call(date)===1234,'saved methods read the current time value');
var brand=false;try{Date.prototype.getTime.call({});}catch(error){brand=error instanceof TypeError;}
check(brand,'Date methods reject an incompatible receiver');
check(Date.prototype.toISOString.call(new Date(0))==='1970-01-01T00:00:00.000Z','prototype ISO formatting uses the actual date');
var range=false;try{new Date(NaN).toISOString();}catch(error){range=error instanceof RangeError;}
check(range && new Date(NaN).toJSON()===null,'invalid dates have correct ISO and JSON behavior');
var marker={},caught=false;try{date.setTime({valueOf:function(){throw marker;}});}catch(error){caught=error===marker;}
check(caught && date.getTime()===1234,'setTime propagates coercion exceptions without changing the date');
check(date.setTime(1.9)===1 && Number.isNaN(date.setTime(Infinity)),'setTime performs TimeClip');
check(Date.now===Date.now && Date.parse===Date.parse && Date.now.length===0 && Date.parse.length===1,'Date statics retain their identity and metadata');
check(typeof Date()==='string','Date called without new returns a string');
var isoCalls=0,generic={valueOf:function(){return 'text';},toISOString:function(){isoCalls++;return 'generic';}};
check(Date.prototype.toJSON.call(generic)==='generic' && isoCalls===1,'toJSON performs primitive conversion without converting a string to a number');
check(Date.prototype[Symbol.toPrimitive].call(new Date(123),'number')===123,'Date numeric primitive conversion uses its time value');
var hintError=false;try{Date.prototype[Symbol.toPrimitive].call(date,'other');}catch(error){hintError=error instanceof TypeError;}
check(hintError,'Date rejects an unknown primitive hint');
return 'PASS|Date prototype and time clipping';

function check(value,message){if(!value)throw new Error(message);}
var element=document.createElement('button');document.body.appendChild(element);
check(document.onclick===null && document.onchange===null && document.onload===null && window.onclick===null && window.onmessage===null && element.onclick===null,'event handlers default to null');
var descriptor=Object.getOwnPropertyDescriptor(Document.prototype,'onclick');
check(typeof descriptor.get==='function' && typeof descriptor.set==='function' && descriptor.enumerable && descriptor.configurable,'handler IDL attributes live on interface prototypes');
var hits=0,lastThis,lastEvent;function handler(event){hits++;lastThis=this;lastEvent=event;return false;}
element.onclick=handler;var event=new Event('click',{cancelable:true});
check(element.onclick===handler && element.dispatchEvent(event)===false && hits===1 && lastThis===element && lastEvent===event,'handler identity receiver arguments and cancellation');
element.onclick=7;check(element.onclick===null,'primitive callback assignment clears handler');element.dispatchEvent(new Event('click'));check(hits===1,'cleared callback is not invoked');
element.onclick={};check(typeof element.onclick==='object','callback object is retained without coercion');element.dispatchEvent(new Event('click'));check(hits===1,'noncallable callback object does not run');
element.onclick=undefined;check(element.onclick===null,'undefined clears callback');
element.setAttribute('onclick','window.inlineHits=(window.inlineHits||0)+1;return false;');
var inline=element.onclick;check(typeof inline==='function' && inline===element.onclick,'inline attribute compiles to a stable callable');
check(element.dispatchEvent(new Event('click',{cancelable:true}))===false && window.inlineHits===1,'inline callback runs once');
element.onclick=null;element.dispatchEvent(new Event('click'));check(window.inlineHits===1,'IDL clearing overrides inline attribute');
element.setAttribute('onclick',element.getAttribute('onclick'));check(typeof element.onclick==='function','setting same inline attribute reactivates its handler');element.removeAttribute('onclick');check(element.onclick===null,'removing inline attribute clears handler');
var fresh=document.createElement('button');fresh.setAttribute('onclick','return 83;');var first=fresh.onclick;fresh.setAttribute('onclick','return 89;');check(fresh.onclick!==first && fresh.onclick()===89,'attribute changes update compiled callback');
var svg=document.createElementNS('http://www.w3.org/2000/svg','svg');check(svg.onclick===null,'SVG elements share the handler mixin');
var globalHits=0;window.onmessage=function(e){globalHits++;check(this===window && e.type==='message','window handler context');};window.dispatchEvent(new Event('message'));check(globalHits===1,'window handler dispatch');window.onmessage=null;
var rejected=false;try{descriptor.get.call({});}catch(error){rejected=error instanceof TypeError;}check(rejected,'IDL handler getter validates receiver');
var documentHits=0;document.onclick=function(e){documentHits++;check(this===document && e.target===document,'document handler context');};document.dispatchEvent(new Event('click'));check(documentHits===1,'document handler dispatch');document.onclick=null;
var payload={value:97},message=new MessageEvent('message',{data:payload}),retained;
element.onmessage=function(e){retained=e;};element.dispatchEvent(message);check(retained===message && retained.data===payload && retained.currentTarget===null && retained.eventPhase===0 && !retained.isTrusted,'DOM dispatch retains event payload identity and resets dispatch state');
var pointer=new MouseEvent('click',{clientX:101,clientY:103}),point;
element.onclick=function(e){point=e;return false;};check(element.dispatchEvent(pointer) && point===pointer && point.clientX===101 && point.clientY===103 && !point.defaultPrevented,'synthetic event fields remain intact and noncancelable handler cannot cancel');
var nested=false;window.onclick=function(e){try{window.dispatchEvent(e);}catch(error){nested=error.name==='InvalidStateError';}};var windowEvent=new Event('click');window.dispatchEvent(windowEvent);check(nested,'nested dispatch of same event is rejected');window.onclick=null;
return 'PASS|HTML event handler attributes';

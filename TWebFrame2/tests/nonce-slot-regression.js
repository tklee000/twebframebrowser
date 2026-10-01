var count=0;function check(ok,label){if(!ok)throw new Error(label);count++;}
var script=document.createElement('script');
check(script.nonce==='' && 'nonce' in script && !script.hasOwnProperty('nonce'),'nonce is an inherited attribute with an empty default');
script.setAttribute('nonce','content');check(script.nonce==='content','attribute changes set the internal slot');
script.nonce='private';check(script.nonce==='private' && script.getAttribute('nonce')==='content','IDL setter changes only the internal slot');
var clone=script.cloneNode(true);check(clone.nonce==='private' && clone.getAttribute('nonce')==='content','cloning preserves a distinct internal nonce');
script.removeAttribute('nonce');check(script.nonce==='' && clone.nonce==='private','attribute removal clears only the source slot');
var holder=document.createElement('div');holder.innerHTML='<script nonce="parsed"></script>';
check(holder.firstChild.nonce==='parsed','HTML parsing initializes the nonce');
var svg=document.createElementNS('http://www.w3.org/2000/svg','script');svg.setAttribute('nonce','svg-content');svg.nonce='svg-slot';
check(svg.nonce==='svg-slot' && svg.getAttribute('nonce')==='svg-content' && svg.cloneNode(false).nonce==='svg-slot','SVG elements share the slot semantics');
var descriptor=Object.getOwnPropertyDescriptor(HTMLElement.prototype,'nonce');
check(typeof descriptor.get==='function' && typeof descriptor.set==='function' && descriptor.enumerable && descriptor.configurable,'nonce prototype descriptor');
var conversions=0;descriptor.set.call(script,{toString:function(){conversions++;return 'converted';}});
check(script.nonce==='converted' && conversions===1,'IDL setter converts to DOMString once');
var original=new Error('conversion'),caught=false;
try{script.nonce={toString:function(){throw original;}};}catch(e){caught=e===original;}
check(caught && script.nonce==='converted','conversion failure preserves the original exception and slot');
var invalid=0;try{descriptor.get.call({});}catch(e){if(e instanceof TypeError)invalid++;}
try{descriptor.set.call(svg,'wrong-interface');}catch(e){if(e instanceof TypeError)invalid++;}
try{script.nonce=Symbol('invalid');}catch(e){if(e instanceof TypeError)invalid++;}
check(invalid===3 && script.nonce==='converted' && svg.nonce==='svg-slot','WebIDL receiver and Symbol conversion checks');
script.nonce=null;check(script.nonce==='null' && script.getAttribute('nonce')===null,'nullable JavaScript value uses DOMString conversion');
return 'PASS|nonce-slot|'+count;

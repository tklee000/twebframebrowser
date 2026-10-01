var checks=0;
function check(value,message){checks++;if(!value)throw new Error(message);}
var fragment=document.createDocumentFragment();
check(typeof fragment.getElementById==='function','DocumentFragment exposes getElementById');
check(fragment.getElementById===DocumentFragment.prototype.getElementById && fragment.getElementById.length===1,'shared prototype method and arity');
var branch=document.createElement('section'),first=document.createElement('span'),second=document.createElement('b');
first.id=second.id='Case:[]';branch.appendChild(first);fragment.appendChild(branch);fragment.appendChild(second);
check(fragment.getElementById('Case:[]')===first,'literal ID match returns first descendant in tree order');
check(fragment.getElementById('case:[]')===null,'ID matching is case sensitive');
fragment.insertBefore(second,branch);check(fragment.getElementById('Case:[]')===second,'reordering is observed');
second.remove();check(fragment.getElementById('Case:[]')===first,'removal is observed');
first.id='updated';check(fragment.getElementById('Case:[]')===null && fragment.getElementById('updated')===first,'ID changes are observed');
check(fragment.getElementById('')===null,'empty IDs never match');
check(document.getElementById('updated')===null,'fragment lookup does not pollute document index');
var conversions=0;check(fragment.getElementById({toString:function(){conversions++;return 'updated';}})===first && conversions===1,'argument converts once');
var marker={},caught=null;try{fragment.getElementById({toString:function(){throw marker;}});}catch(error){caught=error;}
check(caught===marker,'original conversion exception is preserved');
caught=null;try{fragment.getElementById(Symbol('id'));}catch(error){caught=error;}
check(caught instanceof TypeError,'Symbol ID conversion rejects');
caught=null;try{fragment.getElementById();}catch(error){caught=error;}
check(caught instanceof TypeError,'missing required ID rejects');
for(var receiver of [{},document,document.createElement('div')]){
  caught=null;try{DocumentFragment.prototype.getElementById.call(receiver,'updated');}catch(error){caught=error;}
  check(caught instanceof TypeError,'prototype method validates fragment receiver');
}
var host=document.createElement('div');document.body.appendChild(host);
var shadow=host.attachShadow({mode:'open'}),outer=document.createElement('div'),nestedHost=document.createElement('div');
outer.id='scope-id';shadow.appendChild(outer);shadow.appendChild(nestedHost);
var nestedShadow=nestedHost.attachShadow({mode:'closed'}),nested=document.createElement('span');
nested.id='inner-id';nestedShadow.appendChild(nested);
check(shadow.getElementById===DocumentFragment.prototype.getElementById && shadow.getElementById('scope-id')===outer,'ShadowRoot inherits fragment lookup');
check(shadow.getElementById('inner-id')===null && nestedShadow.getElementById('inner-id')===nested,'lookup respects nested shadow boundaries');
check(document.getElementById('scope-id')===null && document.getElementById('inner-id')===null,'document lookup does not pierce shadow roots');
document.body.appendChild(fragment);check(fragment.getElementById('updated')===null && document.getElementById('updated')===first,'insertion empties the fragment and updates the document');
return 'PASS|shadow-id|'+checks;

function check(value,message){if(!value)throw new Error(message);}
var element=document.createElement('section');
element.setAttribute('title','a"&');element.textContent='a<b&c';
check(element.outerHTML==='<section title="a&quot;&amp;">a&lt;b&amp;c</section>','outerHTML includes element and escaped content');
var original=element.outerHTML;element.outerHTML='<p>replacement</p>';
check(element.outerHTML===original,'detached outerHTML setter leaves the node intact');
var parent=document.createElement('div');document.body.appendChild(parent);parent.appendChild(element);
element.outerHTML='<p data-key="first">one</p>text<strong>two</strong>';
check(parent.children.length===2 && parent.childNodes.length===3,'setter replaces an element with a parsed fragment');
check(element.parentNode===null && element.textContent==='a<b&c','existing references still refer to the detached original');
check(parent.innerHTML==='<p data-key="first">one</p>text<strong>two</strong>','replacement is placed at the original position');
parent.children[0].outerHTML=null;
check(parent.children.length===1 && parent.textContent==='texttwo','null converts to an empty replacement');
var descriptor=Object.getOwnPropertyDescriptor(Element.prototype,'outerHTML');
check(descriptor && typeof descriptor.get==='function' && typeof descriptor.set==='function' && descriptor.enumerable && descriptor.configurable,'outerHTML is a prototype accessor');
check(descriptor.get.call(element)===original,'borrowed getter uses its receiver');
var rejected=false;try{descriptor.get.call({});}catch(error){rejected=error instanceof TypeError;}
check(rejected,'accessor rejects incompatible receivers');
rejected=false;try{document.documentElement.outerHTML='<html></html>';}catch(error){rejected=error.name==='NoModificationAllowedError';}
check(rejected,'document child replacement throws');
var shadowHost=document.createElement('div');var shadow=shadowHost.attachShadow({mode:'open'});shadow.innerHTML='<b>shadow</b>';
check(shadowHost.outerHTML==='<div></div>','outerHTML excludes shadow roots');
var holder=document.createElement('div');document.body.appendChild(holder);holder.innerHTML='<span>old</span>';
window.outerHtmlScriptRan=false;holder.children[0].outerHTML='<script>window.outerHtmlScriptRan=true;</script>';
check(!window.outerHtmlScriptRan,'fragment replacement scripts remain inert');
return 'PASS|outerHTML serialization and replacement';

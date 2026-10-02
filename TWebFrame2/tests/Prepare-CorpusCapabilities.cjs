const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(process.argv[2]);
const inventory = JSON.parse(fs.readFileSync(path.join(root, 'inventory.json'), 'utf8'));
const cases = [
 ['unresolved-reference', `function(){try{return __corpusDefinitelyMissingName;}catch(e){return e.name;}}`],
 ['strict-this', `function(){return (function(){'use strict';return this===undefined;})()}`],
 ['const-assignment', `function(){try{const value=1;value=2;return 'accepted';}catch(e){return e.name;}}`],
 ['array-destructuring-default', `function(){var [a=3,b,...rest]=[undefined,4,5,6];return [a,b,rest];}`],
 ['object-spread', `function(){var a={x:1};return {...a,y:2};}`],
 ['optional-chain', `function(){var a=null,b={v:4};return [a?.x,b?.v,a?.m?.()];}`],
 ['numeric-separator', `function(){return 1_234_567+0xAB_CD;}`],
 ['bigint-arithmetic', `function(){return String(9007199254740993n+2n);}`],
 ['class-super', `function(){class A{constructor(v){this.v=v;}get(){return this.v;}}class B extends A{get(){return super.get()+1;}}return new B(4).get();}`],
 ['class-static', `function(){class A{static get(){return 3;}}return A.get();}`],
 ['class-async-method-name', `function(){class A{async(){return 5;}}return new A().async();}`],
 ['class-base-method', `function(){class A{get(){return 4;}}return new A().get();}`],
 ['async-await', `async function(){return (await Promise.resolve(4))+1;}`],
 ['for-of', `function(){var a=[];for(var v of [3,4])a.push(v);return a;}`],
 ['spread-call', `function(){function f(a,b,c){return a+b+c;}return f(...[1,2,3]);}`],
 ['rest-parameter', `function(){return (function(a,...rest){return [a,rest];})(1,2,3);}`],
 ['function-constructor', `function(){return new Function('a','return a+1;')(2);}`],
 ['eval-scope', `function(){var x=3;return eval('x+4');}`],
 ['proxy-get', `function(){return new Proxy({x:3},{get:function(o,k){return o[k]+1;}}).x;}`],
 ['descriptor-getter', `function(){var o={};Object.defineProperty(o,'x',{get:function(){return 3;}});return [o.x,typeof Object.getOwnPropertyDescriptor(o,'x').get];}`],
 ['regexp-unicode', `function(){return /^.$/u.test('\ud83d\ude00');}`],
 ['regexp-named-group', `function(){return new RegExp('(?<word>a)').exec('a').groups.word;}`],
 ['regexp-lookbehind', `function(){return new RegExp('(?<=a)b').test('ab');}`],
 ['regexp-zero-width-matchall', `function(){return Array.from('ab'.matchAll(/(?:)/g)).map(function(m){return m.index;});}`],
 ['string-replaceall', `function(){return 'abab'.replaceAll('a','x');}`],
 ['typedarray-offset', `function(){var a=new Uint8Array([1,2,3,4]);return Array.from(new Uint8Array(a.buffer,1,2));}`],
 ['dataview-endianness', `function(){var b=new ArrayBuffer(4),v=new DataView(b);v.setUint32(0,0x12345678,true);return Array.from(new Uint8Array(b));}`],
 ['json-surrogate', `function(){return JSON.stringify('\ud800');}`],
 ['number-shortest', `function(){return [0.30000000000000004,1.7976931348623157e308,5e-324].map(String);}`]
];
const script = `(function(){
var results=window.__compatResults={chains:[],cases:[],complete:false,pending:0};
var paths=${JSON.stringify(inventory.apiChains.map(a => a.path).sort())};
for(var i=0;i<paths.length;i++){var p=paths[i],parts=p.split('.'),value=globalThis,error=null;try{for(var j=0;j<parts.length;j++)value=value[parts[j]];}catch(e){error=e.name+': '+e.message;}results.chains.push({path:p,type:typeof value,error:error});}
function check(name,source){var record={name:name};results.cases.push(record);try{var value=new Function('return ('+source+')();')();if(value&&typeof value.then==='function'){++results.pending;value.then(function(v){record.value=v;if(--results.pending===0)results.complete=true;},function(e){record.error=e.name;record.message=e.message;if(--results.pending===0)results.complete=true;});}else record.value=value;}catch(e){record.error=e.name;record.message=e.message;}}
${cases.map(([name, source]) => `check(${JSON.stringify(name)},${JSON.stringify(source)});`).join('\n')}
results.complete=results.pending===0;
})();`;
fs.writeFileSync(path.join(root, 'capabilities.js'), script + '\nreturn JSON.stringify(window.__compatResults);\n');
fs.writeFileSync(path.join(root, 'capabilities.html'), '<!doctype html><html><head><meta charset="utf-8"></head><body><pre id="results"></pre><script>' + script + '</script></body></html>');
fs.writeFileSync(path.join(root, 'capabilities.tsv'), ['capabilities', 'program', path.join(root, 'capabilities.js'), 'https://audit.invalid/capabilities'].join('\t'));
Promise.all(cases.map(async ([name, source]) => { try { return { name, value: await vm.runInNewContext('(' + source + ')()') }; } catch (e) { return { name, error: e.name, message: e.message }; } }))
    .then(reference => fs.writeFileSync(path.join(root, 'semantic-reference.json'), JSON.stringify(reference, null, 2)));
console.log(JSON.stringify({ chains: inventory.apiChains.length, semanticCases: cases.length }));

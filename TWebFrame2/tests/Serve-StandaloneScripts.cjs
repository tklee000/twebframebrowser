const fs=require('node:fs'),http=require('node:http'),path=require('node:path');
const folder=path.resolve('TWebFrame2/tests/artifacts/cloudflare-stage-comparison-20261002/standalone/fixtures');
const allowed=new Set(['archived-lookup','archived-eval','archived-numeric-loop']);
http.createServer((req,res)=>{
 const name=new URL(req.url,'http://127.0.0.1').searchParams.get('case');if(!allowed.has(name)){res.writeHead(404);res.end();return;}
 const source=fs.readFileSync(path.join(folder,name+'.js'),'utf8');
 const html='<!doctype html><meta charset="utf-8"><pre id="result"></pre><script>window.__compatResults={case:'+JSON.stringify(name)+',runs:[]};for(var i=0;i<5;i++){var start=performance.now();try{var result=(0,eval)('+JSON.stringify(source).replaceAll('<','\\u003c')+');window.__compatResults.runs.push({ok:true,ms:performance.now()-start,result:String(result)});}catch(e){window.__compatResults.runs.push({ok:false,ms:performance.now()-start,error:String(e)});break;}}window.__compatResults.complete=true;document.getElementById("result").textContent=JSON.stringify(window.__compatResults);</script>';
 res.writeHead(200,{'Content-Type':'text/html; charset=utf-8','Cache-Control':'no-store'});res.end(html);
}).listen(8878,'127.0.0.1',()=>console.log('Standalone fixtures: http://127.0.0.1:8878/?case=archived-lookup'));

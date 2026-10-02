const fs=require('node:fs');
const path=require('node:path');
const crypto=require('node:crypto');
const root=path.resolve(process.argv[2]||'TWebFrame2/tests/artifacts/cloudflare-stage-comparison-20261002');
const readWarnings=[];
const readJSON=p=>{if(!fs.existsSync(p))return null;const raw=fs.readFileSync(p,'utf8').replace(/^\ufeff/,'');
 try{return JSON.parse(raw);}catch(error){
  // Keep raw diagnostics intact. Repair only the summary's copy of older dumps.
  const fixed=raw.replace(/[\x00-\x1f]/g,c=>'\\u'+c.charCodeAt(0).toString(16).padStart(4,'0'));
  try{const value=JSON.parse(fixed);readWarnings.push({file:path.relative(root,p),repair:'escaped control characters in summary copy'});return value;}
  catch{readWarnings.push({file:path.relative(root,p),error:error.message});return null;}
 }};
const readLines=p=>fs.existsSync(p)?fs.readFileSync(p,'utf8').split(/\r?\n/).filter(Boolean).map(JSON.parse):[];
const sha=b=>crypto.createHash('sha256').update(b).digest('hex');
const files=d=>fs.existsSync(d)?fs.readdirSync(d):[];
const shortURL=u=>{try{const x=new URL(u);return x.origin+x.pathname.replace(/\/fo\/.*/, '/fo/[session]').replace(/\/pat\/.*/, '/pat/[session]').replace(/\/ci\/.*/, '/ci/[session]').replace(/\/turnstile\/f\/.*\/light/, '/turnstile/f/[widget]/light');}catch{return u;}};
const visits=[];
for(const engine of ['webview2','twebframe2'])for(const visit of files(path.join(root,engine)).filter(x=>x.startsWith('visit-')).sort()){
 const folder=path.join(root,engine,visit),events=readLines(path.join(folder,'events.jsonl')),sources=[],resources=[];
 const sessions=new Map(events.filter(e=>e.kind==='Target.attachedToTarget').map(e=>[e.data.sessionId,e.data.targetInfo]));
 if(engine==='webview2'){
  for(const file of files(path.join(folder,'scripts')).filter(f=>f.endsWith('.js.meta.json')||f.endsWith('.wasm.meta.json'))){
   const meta=readJSON(path.join(folder,'scripts',file)),sourceFile=file.slice(0,-10),sourcePath=path.join(folder,'scripts',sourceFile);
   const cached=fs.existsSync(sourcePath),bytes=cached?fs.readFileSync(sourcePath):null;
   const fileSession=sourceFile.split('-')[1],session=fileSession==='main'?'':fileSession;
   const parsed=events.find(e=>e.kind==='Debugger.scriptParsed'&&e.session===session&&e.data.scriptId===meta.scriptId&&e.data.executionContextId===meta.executionContextId);
   const target=sessions.get(session);
   const text=bytes?.toString('utf8')||'';
   const diagnostic=text.startsWith('(function(){\nvar checks={}')||text.startsWith('(function(){\nwindow.__stageMessages=[];');
   sources.push({file:'scripts/'+sourceFile,url:meta.url,language:meta.scriptLanguage||'JavaScript',kind:meta.scriptLanguage==='WebAssembly'?'wasm':target?.type==='worker'?'worker':meta.url?'page-script':'dynamic-or-internal',session,contextId:meta.executionContextId,ms:parsed?.ms,cached,diagnostic,bytes:bytes?.length,sha256:bytes?sha(bytes):null,cdpHash:meta.hash});
  }
  const responseEvents=events.filter(e=>e.kind==='Network.responseReceived');
  for(const event of events.filter(e=>e.kind==='response-cached')){
   const response=responseEvents.find(e=>e.session===event.session&&e.data.requestId===event.data.requestId);
   const request=events.find(e=>e.kind==='Network.requestWillBeSent'&&e.session===event.session&&e.data.requestId===event.data.requestId);
   const file=event.data.file,p=path.join(folder,file),bytes=fs.existsSync(p)?fs.readFileSync(p):null;
   resources.push({...response?.data,method:request?.data.method,file,ms:response?.ms,bytes:bytes?.length,sha256:bytes?sha(bytes):null});
  }
 }else{
  for(const meta of readLines(path.join(folder,'scripts','sources.jsonl'))){const p=path.join(folder,'scripts',meta.file),bytes=fs.existsSync(p)?fs.readFileSync(p):null,text=bytes?.toString('utf8')||'';
   sources.push({...meta,file:'scripts/'+meta.file,cached:!!bytes,diagnostic:text.startsWith('(function(){window.__stageMessages=[];')||text.startsWith('return JSON.stringify({url:'),bytes:bytes?.length,sha256:bytes?sha(bytes):null});}
  for(const resource of readLines(path.join(folder,'network.jsonl'))){const p=path.join(folder,resource.file),bytes=fs.existsSync(p)?fs.readFileSync(p):null;resources.push({...resource,sha256:bytes?sha(bytes):null});}
 }
 const result=readJSON(path.join(folder,'result.json')),summary=readJSON(path.join(folder,'summary.json'));
 const allFrames=[];const collect=d=>{if(!d)return;if(d.runtime){const f=d.runtime;allFrames.push({longestScriptJobMs:f.longestScriptJobMs,longestScriptCpuMs:f.longestScriptCpuMs,instructions:f.instructions,workerStarts:f.workerStarts,receivedMessages:f.receivedMessages});}for(const f of d.frames||[])collect(f);};
 for(const file of files(folder).filter(f=>/^progress-\d+\.json$|^page-script-layout\.json$/.test(f)))collect(readJSON(path.join(folder,file)));
 const metrics={maxJobMs:Math.max(0,...allFrames.map(f=>f.longestScriptJobMs||0)),maxJobCpuMs:Math.max(0,...allFrames.map(f=>f.longestScriptCpuMs||0)),maxInstructions:Math.max(0,...allFrames.map(f=>f.instructions||0)),workers:Math.max(0,...allFrames.map(f=>f.workerStarts||0)),phaseNames:result?.phases?.map(p=>p.phase)||[...new Set(allFrames.flatMap(f=>f.receivedMessages||[]))]};
 const failures=events.filter(e=>e.kind.includes('error')),phaseEvents=events.filter(e=>e.kind==='Runtime.bindingCalled').map(e=>({ms:e.ms,...JSON.parse(e.data.payload)}));
 const metadata={engine,visit,folder:path.relative(root,folder).replaceAll('\\','/'),summary,result,metrics,sources,resources,phaseEvents,failures};
 fs.writeFileSync(path.join(folder,'manifest.json'),JSON.stringify(metadata,null,2));visits.push(metadata);
}
const groups=new Map();for(const v of visits)for(const s of v.sources.filter(s=>s.cached&&!s.diagnostic)){const owners=groups.get(s.sha256)||[];owners.push({engine:v.engine,visit:v.visit,file:s.file,url:s.url,kind:s.kind});groups.set(s.sha256,owners);}
const stable=[...groups].filter(([h,o])=>new Set(o.map(x=>x.engine+'/'+x.visit)).size>=3).map(([hash,owners])=>({hash,owners}));
const comparison={inputURL:'https://www.ppomppu.co.kr/zboard/login.php',visits:visits.map(v=>({engine:v.engine,visit:v.visit,scripts:v.sources.length,cached:v.sources.filter(s=>s.cached).length,uniqueHashes:new Set(v.sources.map(s=>s.sha256).filter(Boolean)).size,workers:v.sources.filter(s=>s.kind==='worker'||s.kind==='worker-program').length,resources:v.resources.length,completionMs:v.result?.phases?.find(p=>p.phase==='complete')?.at,tokenPresent:v.result?.tokenPresent,cdpErrors:v.failures.length,metrics:v.metrics})),sharedSources:stable};
fs.writeFileSync(path.join(root,'comparison.json'),JSON.stringify(comparison,null,2));
fs.writeFileSync(path.join(root,'summary-read-warnings.json'),JSON.stringify(readWarnings,null,2));
const variations=visits.flatMap(v=>v.resources.filter(r=>/\/turnstile\/f\/|\/fo\/|\/api\.js|\/sec-turnstile\.js/.test(r.url||'')).map(r=>({engine:v.engine,visit:v.visit,stage:/\/fo\//.test(r.url)?'verification-response':/\/turnstile\/f\//.test(r.url)?'challenge-document':/sec-turnstile/.test(r.url)?'site-integration':'api-loader',sha256:r.sha256,bytes:r.bytes,file:v.folder+'/'+r.file,url:shortURL(r.url),status:r.status,ms:r.ms})));
fs.writeFileSync(path.join(root,'source-variations.json'),JSON.stringify(variations,null,2));
let markdown='# 반복 방문 및 단계 비교\n\n입력: `'+comparison.inputURL+'`\n\n각 방문은 별도 프로필/메모리 컨텍스트를 사용하며 쿠키나 성공 토큰을 다른 방문 또는 엔진에 옮기지 않았습니다.\n\n|엔진|방문|스크립트 기록/저장|고유 해시|Worker 소스|응답 캐시|완료(ms)|토큰 존재|최장 JS 작업(ms)|수집 오류|\n|---|---|---:|---:|---:|---:|---:|---|---:|---:|\n';
for(const v of comparison.visits)markdown+=`|${v.engine}|${v.visit}|${v.scripts}/${v.cached}|${v.uniqueHashes}|${v.workers}|${v.resources}|${v.completionMs?.toFixed(1)||'관찰되지 않음'}|${v.tokenPresent===undefined?'최종 JSON 미수집':v.tokenPresent}|${v.metrics.maxJobMs||'—'}|${v.cdpErrors}|\n`;
markdown+='\n## 부모 창에서 관찰한 단계\n\n';
for(const v of visits){markdown+=`- ${v.engine}/${v.visit}: `+(v.result?.phases?.map(p=>`${p.phase}@${p.at.toFixed(1)}ms${p.code?'(code='+p.code+')':''}`).join(' → ')||v.metrics.phaseNames.join(' → '))+'\n';}
markdown+='\n## 저장물 읽기\n\n각 방문의 `manifest.json`에 모든 소스/응답 파일, 원본 URL, SHA-256, 종류와 타이밍을 연결했습니다. `scripts`에는 외부·인라인·eval·Function·Worker 실행 소스가 있으며, `responses`에는 해당 방문에서 받은 응답 본문이 있습니다. WebView2의 소스 수에는 브라우저 내부 코드와 진단 코드도 포함됩니다. TWebFrame2의 program URL은 문서 URL이며 요청 원본 URL은 network.jsonl에서 확인합니다.\n\n초기 WebView2 visit-01~03은 빠르게 종료되는 Worker 일부를 놓쳤으므로 실패를 그대로 보존했습니다. visit-04~06은 Worker 시작 및 스크립트 실행 전에 수집기를 연결한 별도 회차입니다. 이 방식의 실행 시간에는 디버거 정지/재개 비용이 포함됩니다.\n\n여기서 캐시는 방문별 진단용 원본 보관소입니다. 과거 방문의 검증 코드나 응답을 다른 실서비스 세션에 재생하지 않습니다. 방문하지 않은 서버 분기, WASM 바이너리, 서버 내부 판정 기준을 모두 확보했다는 의미는 아닙니다.\n';
markdown+='\nTWebFrame2 visit-01~03의 최종 JSON 출력식에는 프로브 오류가 있어 결과가 누락됐습니다. 원본 소스, 응답, 진행/최종 런타임 덤프는 보존했으며 출력식을 고친 visit-04~06에서 부모 메시지 시간과 토큰 존재 여부를 확인했습니다. 일부 기존 상세 덤프의 문자열에 JSON 제어 문자 이스케이프 문제가 있어 요약용 복사본만 보정했으며 `summary-read-warnings.json`에 파일별 내역을 남겼습니다.\n';
fs.writeFileSync(path.join(root,'comparison.md'),markdown);
console.log(JSON.stringify(comparison.visits,null,2));

const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),util=require('node:util');
const root=path.resolve(process.argv[2]||'TWebFrame2/tests/artifacts/cloudflare-stage-comparison-20261002');
const read=file=>JSON.parse(fs.readFileSync(path.join(root,file),'utf8'));
const lines=file=>fs.existsSync(path.join(root,file))?fs.readFileSync(path.join(root,file),'utf8').split(/\r?\n/).filter(Boolean).map(JSON.parse):[];
const hash=file=>crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const median=values=>{const ordered=[...values].sort((a,b)=>a-b);return ordered[Math.floor(ordered.length/2)];};
const benchmarks=['lookup','numeric-loop','eval'].map(name=>{
 const native=read(`standalone/final-${name}.json`),web=read(`standalone/webview2-${name}/result.json`).compatibility;
 const baseline=name==='eval'?null:read(`standalone/baseline-${name}.json`);
 const runs=[...native.runs,...web.runs,...(baseline?.runs||[])];
 return {name,checksum:runs[0].result,equal:runs.every(r=>r.ok&&r.result===runs[0].result),networkBlocked:native.networkBlocked,
 baselineMs:baseline?median(baseline.runs.map(r=>r.ms)):null,nativeMs:median(native.runs.map(r=>r.ms)),webMs:median(web.runs.map(r=>r.ms)),
 nativeRunsMs:native.runs.map(r=>r.ms),webRunsMs:web.runs.map(r=>r.ms),nativeCalls:native.nativeCalls,
 tableReductionIterations:native.runtime.tableReductionIterations,evalCompileCacheHits:native.runtime.evalCompileCacheHits};
});
const optimized=path.join(root,'optimized');
const visits=fs.readdirSync(optimized).filter(n=>n.startsWith('twebframe2-visit-')).map(name=>{
 const folder=`optimized/${name}`,sources=lines(`${folder}/scripts/sources.jsonl`),network=lines(`${folder}/network.jsonl`);
 const manifest=[...sources.map(s=>({...s,file:`scripts/${s.file}`,type:'source'})),...network.map(r=>({...r,type:'response'}))].map(item=>{
  const filename=path.join(root,folder,item.file);return {...item,sha256:hash(filename),bytes:fs.statSync(filename).size};
 });
 fs.writeFileSync(path.join(root,folder,'manifest.json'),JSON.stringify(manifest,null,2));
 const result=read(`${folder}/result.json`),jobs=lines(`${folder}/scripts/jobs.jsonl`).sort((a,b)=>b.wallMs-a.wallMs);
 return {name,sources:sources.length,responseBodies:network.length,sourceHashes:new Set(manifest.filter(x=>x.type==='source').map(x=>x.sha256)).size,
 tokenPresent:result.tokenPresent,phases:result.phases.map(p=>({phase:p.phase,ms:p.at})),longestJobs:jobs.slice(0,4).map(j=>({wallMs:j.wallMs,cpuMs:j.cpuMs,instructions:j.instructions})),
 verificationResponseHashes:manifest.filter(r=>r.type==='response'&&r.url.includes('/h/b/fo/')).map(r=>r.sha256)};
});
const w=read('compatibility/webview2-optimized/result.json').compatibility,t=read('compatibility/twebframe2-final/result.json').compatibility;
const compatibility=w.cases.map(c=>{const other=t.cases.find(x=>x.name===c.name);return {name:c.name,equal:util.isDeepStrictEqual(c.value,other?.value),webMs:c.elapsedMs,nativeMs:other?.elapsedMs,
 ...(c.name==='number-roundtrip'?{tested:c.value.length,mismatches:c.value.filter((v,i)=>v!==other?.value?.[i]).length}:{})};});
const report={benchmarks,visits,compatibility,compatibilityComplete:[w.complete,t.complete],fixtures:read('standalone/fixtures/metadata.json')};
fs.writeFileSync(path.join(root,'optimization-results.json'),JSON.stringify(report,null,2));
const format=n=>n===null?'—':n.toFixed(3);
const rows=benchmarks.map(b=>`|${b.name}|${format(b.baselineMs)}|${format(b.nativeMs)}|${format(b.webMs)}|${b.equal?'일치':'불일치'}|`).join('\n');
const liveRows=visits.map(v=>`|${v.name}|${v.sources}|${v.responseBodies}|${v.longestJobs[0]?.wallMs}|${v.longestJobs[0]?.cpuMs}|${v.tokenPresent}|`).join('\n');
const text=`# 저장된 스크립트의 해석 오류 및 실행 속도 조사

전체 검증은 아직 통과하지 못했습니다. 분리한 표 조회 반복문은 WebView2와 가까워졌지만, 실제 검증의 동적 프로그램 전체와 일반 문자열·typed array 반복문은 여전히 크게 느립니다. 전체 엔진이 WebView2와 동급이라는 결론을 내리지 않습니다.

## 확인된 실패 경로

초기 6회씩의 별도 방문에서 WebView2는 모두 토큰 존재를 확인했습니다. 수집 오류가 없는 visit-04~06은 부모 메시지 complete가 1778.1, 1842.5, 1957.4ms에 도착했습니다. TWebFrame2의 추적을 끈 visit-04~06은 90초 관찰에서도 토큰이 없었습니다.

사이트의 sec-turnstile.js는 RESOLVE_DEADLINE_MS=10000을 사용합니다. TWebFrame2의 CPU 작업이 이 제한을 넘으면 부모 타이머가 뒤늦게 실행되고 iframe을 제거·생성합니다. HTTP POST는 200으로 응답했고, 긴 작업의 CPU 시간이 거의 벽시계 시간과 같습니다. 따라서 확인된 주요 병목은 동기 JavaScript 실행입니다. 서버의 최종 판정 기준까지 확인한 것은 아닙니다.

최적화 후에도 init → requestExtraParams → translationInit → food 다음에 complete 대신 반복 init 또는 overrun이 나타납니다. 방문마다 코드가 달라지므로 다른 방문의 작업 시간을 정확히 동일 입력의 성능 비교로 취급하지 않습니다.

## 같은 입력을 사용한 단독 실행

단위: ms. 중앙값이며 원본 JSON에는 매 실행 시간이 있습니다. 외부 통신은 차단했습니다.

|입력|기존 TWebFrame2|최종 TWebFrame2|WebView2|반환값|
|---|---:|---:|---:|---|
${rows}

lookup은 저장된 함수 P의 원문을 유지하고, 결정적인 2048개 문자열 표와 3551538회 반복 어댑터를 붙였습니다. checksum=31665484입니다. 안전한 주기적 합산으로 증명된 경우 함수 호출을 줄이는 최적화이므로 실제 난독화 VM 전체의 개선 배율을 뜻하지 않습니다.

numeric-loop는 저장된 Pw의 원문을 유지하고 입력 정수 200000개를 처리하는 어댑터를 붙였습니다. checksum=6553410560입니다. 원본 검증 프로그램 전체가 아닙니다. 여전히 WebView2보다 느립니다.

eval은 1337359문자 원본을 수정하지 않았습니다. 최종 첫 실행 ${format(benchmarks.find(b=>b.name==='eval').nativeRunsMs[0])}ms, 이후 컴파일 캐시가 적용됐습니다. WebView2의 0ms 기록은 측정 해상도 아래이며 실제 시간이 0이라는 뜻이 아닙니다. 기존 eval 측정에는 큰 문자열을 다시 인용한 래퍼 파싱까지 들어가 있어 개선 배율에서 제외했습니다. lookup/numeric-loop의 기존 래퍼는 작지만 역시 그 비용이 포함됩니다.

## 수정한 일반 엔진 오류와 병목

- 숫자 문자열 변환: 15자리 출력 손실과 지수 끝의 0을 지우는 오류를 제거했습니다. shortest round-trip 숫자를 ECMAScript의 소수·지수 표기로 구성합니다. WebView2와 무작위 binary64 값 1000개 및 경계값의 문자열 결과가 일치했습니다.
- JSON: 모든 제어 문자, lone surrogate를 이스케이프하고 NaN/Infinity는 null로 출력합니다. 비교 fixture에서 결과가 일치했습니다.
- 정규식 리터럴: 실행마다 새 객체를 만듭니다. 이전에는 같은 컴파일 상수를 재사용해 객체 상태가 공유될 수 있었습니다. Unicode 및 named group 정규식의 기존 비호환은 남아 있습니다.
- JIT: 반복문 안에서 중단 콜백을 확인하도록 했습니다. Windows x64 호출 규약과 unwind 정보를 등록하며, 콜백 재진입은 별도 scratch frame을 사용합니다. 초기값을 읽기 전에 덮어쓰는 인자는 numeric guard 대상에서 제외합니다.
- 인터프리터: 키를 매번 확인하는 인덱스 캐시로 지역 변수 조회를 줄이고, 부작용 없는 숫자 상수 연산·지역 증감을 묶어 처리합니다.
- SIMD: 긴 UTF-16 ASCII 공백 구간을 SSE2로 읽습니다. 안전한 표 합산에도 SSE2를 사용합니다.
- 큰 eval 입력의 컴파일 캐시는 같은 immutable 문자열만 재사용합니다. 중첩 함수 의존성이 없는 작은 bytecode만 캐시하며 원본 문자열 합계 8MiB/16개로 제한합니다. 환경과 정규식 객체는 매번 새로 구성합니다.

JavaScript 작업 순서는 유지합니다. 기존 Worker와 병렬 리소스 로딩을 사용하며, 한 realm의 JavaScript를 임의의 여러 스레드에 나눠 실행하는 변경은 하지 않았습니다.

숫자/JSON 수정은 아래 규격에 근거했습니다: [Number::toString](https://tc39.es/ecma262/multipage/ecmascript-data-types-and-values.html#sec-numeric-types-number-tostring), [SerializeJSONProperty](https://tc39.es/ecma262/multipage/structured-data.html#sec-serializejsonproperty).

## 최적화 후 실제 방문

각 방문은 새 메모리 컨텍스트이며 원본 응답과 스크립트를 별도 저장했습니다. visit-01은 초기 표/SIMD 수정, visit-02는 JIT·숫자/JSON 변경, visit-03은 최종 숫자 표기 변경까지 반영했습니다. 같은 바이너리의 반복 결과로 합치지 않습니다.

|방문|소스|응답 본문|최장 작업(ms)|CPU(ms)|토큰 존재|
|---|---:|---:|---:|---:|---|
${liveRows}

## 남은 차이

최종 일반 workload에서 typed array 숫자 루프는 ${format(compatibility.find(c=>c.name==='numeric-workload').nativeMs)}ms 대 ${format(compatibility.find(c=>c.name==='numeric-workload').webMs)}ms, 문자열 루프는 ${format(compatibility.find(c=>c.name==='string-workload').nativeMs)}ms 대 ${format(compatibility.find(c=>c.name==='string-workload').webMs)}ms입니다. 결과는 같지만 속도 차이는 큽니다. 현재 숫자 JIT는 객체/typed array/문자열 접근을 포함한 큰 동적 함수까지 컴파일하지 못합니다. 실제 페이지에서도 수억 개의 명령과 수천만 번의 작은 호출이 남았습니다.

Unicode 정규식, named capture group, OffscreenCanvas, WebAssembly도 비교에서 차이가 있습니다. 선택 기능의 부재나 의도적으로 발생시켜 잡은 예외 하나만으로 Cloudflare 실패 원인이라고 단정하지 않습니다. [Cloudflare 지원 브라우저 안내](https://developers.cloudflare.com/cloudflare-challenges/reference/supported-browsers/)도 자체·수정·임베디드 엔진에 제한을 명시합니다. 이는 관찰한 실행 병목을 고칠 필요를 대체하지 않습니다.

## 보관 범위와 재현

초기 각 엔진 6회 원본은 comparison.md와 source-variations.json에 정리돼 있습니다. WebView2 visit-04~06은 각 153/153 scriptParsed 소스를 저장했고 수집 오류는 0입니다. 초기 visit-01~03의 빠르게 종료된 Worker 일부는 놓쳤으며 그대로 보존했습니다. WebView2 초기 iframe HTML 응답 일부는 attach 이전에 전달돼 network body에 없습니다. Worker·inline·eval·Function 실행 소스는 저장했으나 서버의 모든 미래 분기나 내부 판정을 확보했다는 뜻은 아닙니다.

manifest.json은 파일·URL·SHA-256을 연결합니다. 새 방문도 최적화 보고서 생성 시 별도 manifest를 만듭니다. 과거 검증 응답을 실서비스의 다른 세션에 재사용하거나 성공 토큰/쿠키를 옮기지 않았습니다.

도구: WebView2TraceProbe.cpp, PageScriptProbe.cpp --archive, StandaloneScriptProbe.cpp, Prepare-StandaloneBenchmarks.cjs, Run-CloudflareStageComparison.ps1, Summarize-Optimization.cjs. standalone fixtures/metadata.json에 원본 함수와 변경 여부, SHA-256을 기록했습니다. --regressions는 일반 인터프리터와 최적화 경로의 결과, 부작용 fallback, 중단/재진입, JSON/숫자, eval 환경 분리를 검사합니다.

검증 로그: standalone/regressions.log, core-tests-final.log, http-tests-optimized.log, heap-tests-final.log. Release 브라우저는 bin/Browser/x64/Release/TWebFrameBrowser.exe로 빌드했습니다. 별도 ScrollRenderingRegression 전체 실행의 기존 화면 밖 조상 overflow 관련 2건은 이 조사 범위에서 수정하지 않았습니다.
`;
fs.writeFileSync(path.join(root,'performance-report.md'),text);
console.log(JSON.stringify({benchmarks:benchmarks.map(b=>({name:b.name,equal:b.equal,baselineMs:b.baselineMs,nativeMs:b.nativeMs,webMs:b.webMs})),visits:visits.map(v=>({name:v.name,sources:v.sources,tokenPresent:v.tokenPresent,longestMs:v.longestJobs[0]?.wallMs})),numberRoundtrip:compatibility.find(c=>c.name==='number-roundtrip')},null,2));

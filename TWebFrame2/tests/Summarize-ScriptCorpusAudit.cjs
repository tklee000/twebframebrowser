const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const root = path.resolve(process.argv[2]);
const read = file => JSON.parse(fs.readFileSync(path.join(root, file), 'utf8').replace(/^\ufeff/, ''));
const lines = file => fs.readFileSync(path.join(root, file), 'utf8').replace(/^\ufeff/, '').split(/\r?\n/).filter(Boolean).map(JSON.parse);
const inventory = read('inventory.json');
const rawRuntime = lines('runtime-audit.jsonl');
const compile = new Map(lines('compile-audit.jsonl').map(r => [r.id, r]));
const extended = new Map(fs.existsSync(path.join(root, 'extended-audit.jsonl')) ? lines('extended-audit.jsonl').map(r => [r.id, r]) : []);
const runtime = new Map(rawRuntime.map(r => [r.id, extended.get(r.id) || r]));
const nativeRecord = lines('twebframe-capabilities.jsonl')[0];
const native = JSON.parse(nativeRecord.result);
const web = read('webview2-capabilities-final/result.json').compatibility;
if (!native.complete || !web.complete) throw new Error('Capability checks did not complete');
const webCases = new Map(web.cases.map(c => [c.name, c]));
const nativeChains = new Map(native.chains.map(c => [c.path, c]));
const semantic = native.cases.map(actual => {
    const expected = webCases.get(actual.name);
    const equal = !!expected && JSON.stringify({ value: actual.value, error: actual.error }) === JSON.stringify({ value: expected.value, error: expected.error });
    return { name: actual.name, equal, webview2: expected, twebframe2: actual };
});
const apis = web.chains.map(expected => {
    const actual = nativeChains.get(expected.path);
    const owners = inventory.apiChains.find(c => c.path === expected.path)?.owners || [];
    const diagnostic = /(?:^|\.)(__stageMessages|__compatResults|__corpus)/.test(expected.path);
    const hostSpecific = /^(window\.chrome|self\.toolbar)/.test(expected.path);
    return { path: expected.path, webview2: expected, twebframe2: actual, owners, diagnostic, hostSpecific,
        missing: expected.type !== 'undefined' && !expected.error && actual?.type === 'undefined',
        equal: expected.type === actual?.type && Boolean(expected.error) === Boolean(actual?.error) };
});
const sources = inventory.sources.map(source => {
    const syntax = compile.get(source.id), execution = runtime.get(source.id);
    if (source.mode === 'wasm-placeholder') return { ...source, status: 'not-javascript', reason: 'WebAssembly scriptParsed metadata has an empty JS source; binary was not captured' };
    let status;
    if (!syntax) status = 'missing-audit-result';
    else if (!syntax.syntaxOk) status = source.referenceSyntaxOk ? 'engine-syntax-gap' : 'invalid-in-reference-also';
    else if (!execution || execution.resultMissing || execution.exitCode && !extended.has(source.id)) status = 'process-failure';
    else if (/instruction limit|execution interrupted|interrupted by host/i.test(execution.error || '')) status = 'execution-budget';
    else if (source.mode === 'worker' && execution.executionOk && !execution.workerFinished) status = 'worker-completion-unobserved';
    else if (!execution.executionOk) {
        const text = fs.readFileSync(source.file, 'utf8');
        if(source.mode === 'worker' && text.includes('navigator.storage.getDirectory')) status = 'confirmed-api-gap';
        else if(/\$ is not a function|require is not a function|__unsupportedDecode|Could not find Turnstile valid script tag/.test(execution.error || '') ||
                text.startsWith('/*! jQuery Migrate') || text.includes('$.fn.cheditor =') || text.startsWith("return digests.join")) status = 'dependency-exception';
        else status = 'execution-exception';
    }
    else status = 'completed';
    return { ...source, status, syntaxOk: syntax?.syntaxOk, syntaxError: syntax?.syntaxError, executionOk: execution?.executionOk,
        error: execution?.error, lastError: execution?.lastError, ms: execution?.ms, networkBlocked: execution?.networkBlocked,
        extendedRun: extended.has(source.id), failedCalls: execution?.diagnostics?.failedCalls || [],
        missingProperties: execution?.diagnostics?.missingProperties || [], createdErrors: execution?.diagnostics?.createdErrors || [] };
});
const sourceById = new Map(sources.map(s => [s.id, s]));
const fileResults = inventory.files.map(file => ({ ...file, status: sourceById.get(file.id).status, syntaxOk: sourceById.get(file.id).syntaxOk,
    syntaxError: sourceById.get(file.id).syntaxError, executionError: sourceById.get(file.id).error }));
const counts = {}; for (const source of sources) counts[source.status] = (counts[source.status] || 0) + 1;
const javascriptSources = sources.filter(s => s.mode !== 'wasm-placeholder');
const javascriptFiles = fileResults.filter(f => sourceById.get(f.id).mode !== 'wasm-placeholder');
const missingApis = apis.filter(a => a.missing && !a.diagnostic && !a.hostSpecific);
const standardApiPaths = new Set(missingApis.map(a => a.path.replace(/^(window|self|globalThis)\./, '')));
const latest = fileResults.filter(f => f.file.startsWith('cloudflare-recapture-20261002-123000/'));
const latestCounts = {}; for (const file of latest) latestCounts[file.status] = (latestCounts[file.status] || 0) + 1;
const report = {
    createdUtc: new Date().toISOString(), originalFiles: inventory.fileCount, originalDistinctHashes: inventory.distinctSources,
    javascriptFiles: javascriptFiles.length, javascriptDistinctHashes: new Set(javascriptSources.map(s => s.sha256)).size,
    sourceModeChecks: javascriptSources.length, counts, latestCapture: { files: latest.length, counts: latestCounts },
    missingArchivedSources: inventory.missingSources, inventoryWarnings: inventory.warnings,
    semanticCases: semantic.length, semanticDifferences: semantic.filter(c => !c.equal).length,
    apiPathsChecked: apis.length, missingPublicApiPaths: missingApis.length, canonicalMissingApiPaths: [...standardApiPaths],
    failedCallSourceCount: sources.filter(s => s.failedCalls?.length).length,
    auditComplete: javascriptSources.every(s => s.syntaxOk !== undefined && !['process-failure', 'missing-audit-result'].includes(s.status)),
    rawProbeProcessFailures: rawRuntime.filter(r => r.exitCode !== 0).map(r => ({ id: r.id, mode: r.mode, exitCode: r.exitCode, signal: r.signal })),
    engineSourceSha256: crypto.createHash('sha256').update(fs.readFileSync(path.join(__dirname, '../src/JavaScript.cpp'))).digest('hex')
};
for (const [file, value] of [['audit-summary.json', report], ['source-results.json', sources], ['file-results.json', fileResults], ['semantic-differences.json', semantic], ['api-differences.json', apis]]) fs.writeFileSync(path.join(root, file), JSON.stringify(value, null, 2));
const link = file => `[${path.basename(file)}](<${file.replaceAll('\\', '/')}>)`;
let markdown = '# 저장된 스크립트 전수 검사\n\n';
markdown += `저장된 .js 파일 ${inventory.fileCount}개(고유 해시 ${inventory.distinctSources}개)를 인벤토리로 정리했습니다. WebAssembly 메타데이터에 대응하는 빈 파일을 제외하면 JavaScript 원본 ${javascriptFiles.length}개, 고유 해시 ${report.javascriptDistinctHashes}개입니다. 동일한 원본도 페이지/Worker 환경이 다르면 따로 검사하여 ${javascriptSources.length}건을 검사했습니다. 최신 10회뿐 아니라 이전 수집본과 저장된 검사·벤치마크 코드도 포함합니다.\n\n`;
markdown += '문법 검사는 현재 엔진의 실제 Compiler로 모든 함수 본문을 포함해 컴파일했습니다. 실행 검사는 원본별 별도 프로세스와 새 DOM/Worker 환경에서 수행했습니다. 외부 네트워크를 차단했으며 DOMContentLoaded/load, 타이머와 microtask 처리를 관찰했습니다. 처음에는 1초/500만 명령을 상한으로 두었고, 상한에 걸린 별도 벤치마크 원본은 JIT를 허용하여 20초 상한으로 재검사했습니다.\n\n';
markdown += '|검사 결과|고유 원본/환경 수|\n|---|---:|\n';
for (const [status, count] of Object.entries(counts)) markdown += `|${status}|${count}|\n`;
markdown += '\n`completed`는 해당 환경에서 관찰한 최상위 실행과 준비된 이벤트가 예외 없이 끝났다는 뜻입니다. 미호출 함수 본문, 모든 이벤트 입력, 서버가 제공할 추가 데이터까지 실행했다는 뜻은 아닙니다. `dependency-exception` 9종은 jQuery 의존 코드, 앞선 검사에서 만든 digests 변수, Node 전용 require, 진단용 __unsupportedDecode, Turnstile 로더의 currentScript 문맥이 없는 독립 실행입니다. 실제 API 누락을 재현한 Worker 3종은 `confirmed-api-gap`으로 별도 기록했습니다.\n\n';
markdown += '구문 실패 원본은 다음과 같습니다.\n\n';
for (const source of sources.filter(s => s.status === 'engine-syntax-gap' || s.status === 'invalid-in-reference-also')) markdown += `- ${source.status}: ${link(source.file)} — ${source.syntaxError}; 동일 원본 ${source.owners.length}개 파일에 결과를 연결했습니다.\n`;
markdown += '\nWebView2와 비교한 의미 검사 결과는 다음과 같습니다.\n\n|검사|WebView2|현재 엔진|\n|---|---|---|\n';
const show = value => JSON.stringify(value).replaceAll('|', '\\|').replaceAll('\n', ' ');
for (const item of semantic.filter(c => !c.equal)) markdown += `|${item.name}|${show(item.webview2)}|${show(item.twebframe2)}|\n`;
markdown += `\n총 ${semantic.length}개 의미 검사 중 ${report.semanticDifferences}개가 다릅니다. async/await, BigInt, optional chaining, 일반 클래스 메서드, eval, Function 생성, Proxy, typed array 및 JSON 등의 통과 결과도 semantic-differences.json에 보존했습니다.\n\n`;
markdown += `저장 코드의 정적으로 식별 가능한 ${apis.length}개 API 경로를 같은 WebView2와 비교했습니다. 진단 변수와 WebView2 전용 호스트/UI 경로를 제외한 미구현 공개 API 경로는 ${missingApis.length}개이며 별칭을 합치면 ${standardApiPaths.size}개입니다. 존재 여부 검사이며 모든 메서드의 전체 인수 조합을 검증한 것은 아닙니다.\n\n|경로|WebView2|현재 엔진|참조 원본 수|\n|---|---|---|---:|\n`;
for (const api of missingApis) markdown += `|${api.path}|${api.webview2.type}|${api.twebframe2.type}|${api.owners.length}|\n`;
markdown += '\n특히 실제 Cloudflare Worker 원본의 `navigator.storage.getDirectory()` 호출에서 현재 엔진이 예외를 발생시키는 것을 확인했습니다. 해당 Worker 원본 3종은 다음 파일이며 페이지용 빈 DOM과 무관하게 실제 Worker 환경에서 재현했습니다.\n\n';
for (const source of sources.filter(s => s.status === 'confirmed-api-gap')) markdown += `- ${link(source.file)} — ${source.error}\n`;
markdown += '\n이는 구현 누락의 직접 증거입니다. 해당 API만 추가하면 Cloudflare 검증을 통과한다는 의미는 아니며 서버 판정 원인은 이 독립 검사로 확정할 수 없습니다.\n\n';
markdown += `원본 ${inventory.missingSources.length}개는 이전 WebView2 visit-01~02 수집에서 이미 빠져 있어 검사할 수 없습니다. 최신 10회 수집의 누락과 구분해서 audit-summary.json에 위치를 남겼습니다. WebAssembly 바이너리는 캐시에 없으므로 빈 .js 파일만으로 검사하지 않았습니다. 초기 프로브가 wasm:// URL을 문서 URL로 사용하면서 실패한 기록은 보존했고, URL을 보정한 프로브로 전체 컴파일 검사를 완료했습니다.\n\n`;
markdown += `파일별 결과: ${link(path.join(root, 'file-results.json'))}\n\n원본별 결과 및 실패 호출: ${link(path.join(root, 'source-results.json'))}\n\nAPI 경로별 참조 원본: ${link(path.join(root, 'api-differences.json'))}\n\n재현용 의미 검사: ${link(path.join(root, 'capabilities.js'))}\n`;
fs.writeFileSync(path.join(root, 'README.md'), markdown);
console.log(JSON.stringify(report, null, 2));
if (!report.auditComplete) process.exitCode = 1;

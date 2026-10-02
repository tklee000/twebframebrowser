const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(process.argv[2]);
const read = name => JSON.parse(fs.readFileSync(path.join(root, name), 'utf8').replace(/^\ufeff/, ''));
execFileSync(process.execPath, [path.join(__dirname, 'Summarize-StageCaptures.cjs'), root], { stdio: 'pipe' });
const session = read('capture-session.json');
const comparison = read('comparison.json');
comparison.inputURL = session.url;
fs.writeFileSync(path.join(root, 'comparison.json'), JSON.stringify(comparison, null, 2));
const visits = comparison.visits.filter(v => v.engine === 'webview2');
const sourceHashes = new Set();
const allSourceHashes = new Set();
const sourceVariants = new Map();
const records = visits.map(visit => {
    const manifest = read(`webview2/${visit.visit}/manifest.json`);
    const events = fs.readFileSync(path.join(root, 'webview2', visit.visit, 'events.jsonl'), 'utf8').split(/\r?\n/).filter(Boolean).map(JSON.parse);
    const attempted = session.attempts.find(a => a.visit === visit.visit);
    for (const source of manifest.sources) {
        if (source.sha256) allSourceHashes.add(source.sha256);
        if (source.sha256 && !source.diagnostic) sourceHashes.add(source.sha256);
        if (source.sha256) {
            const variant = sourceVariants.get(source.sha256) || { sha256: source.sha256, bytes: source.bytes, owners: [] };
            variant.owners.push({ visit: visit.visit, file: `webview2/${visit.visit}/${source.file}`, kind: source.kind, diagnostic: source.diagnostic, url: source.url });
            sourceVariants.set(source.sha256, variant);
        }
    }
    const observed = events.filter(e => e.kind === 'Debugger.scriptParsed').length;
    const cached = events.filter(e => e.kind === 'script-cached'||e.kind === 'wasm-cached').length;
    const parsedFailures = events.filter(e => e.kind === 'Debugger.scriptFailedToParse').length;
    const errors = events.filter(e => e.kind === 'cdp-error');
    // A snapshot of an already detached iframe does not affect source/body storage.
    // Retain every error; only this evidenced lifecycle case is classified separately.
    const staleSnapshots = errors.filter(e => e.data.method === 'Runtime.evaluate' &&
        e.data.result?.error?.code === -32001 && e.data.result.error.message === 'Session with given id not found.' &&
        events.some(detached => detached.kind === 'Target.detachedFromTarget' && detached.data.sessionId === e.session && detached.ms <= e.ms));
    const unexpectedErrors = errors.filter(e => !staleSnapshots.includes(e));
    const sourceReadErrors = errors.filter(e => e.data.method === 'Debugger.getScriptSource').length;
    const responseReadErrors = errors.filter(e => e.data.method === 'Network.getResponseBody').length;
    const responseFilesPresent = manifest.resources.every(r => r.sha256 && fs.existsSync(path.join(root, 'webview2', visit.visit, r.file)));
    const valid = attempted?.exitCode === 0 && !attempted.error && manifest.result && manifest.summary &&
        observed === manifest.summary.scripts && observed === manifest.sources.length && observed === cached && observed === visit.cached &&
        manifest.summary.responses === manifest.resources.length && responseFilesPresent &&
        manifest.summary.cdpErrors === errors.length && unexpectedErrors.length === 0 && manifest.summary.pending === 0;
    return { ...visit, observedScripts: observed, savedScriptEvents: cached, scriptParseFailures: parsedFailures,
        sourceReadErrors, responseReadErrors, staleSnapshotErrors: staleSnapshots.length, unexpectedCdpErrors: unexpectedErrors.length,
        captureValid: Boolean(valid) };
});
const variations = read('source-variations.json');
const stageCounts = {};
for (const item of variations) {
    const stage = stageCounts[item.stage] ||= { bodies: 0, hashes: new Set(), visits: new Set() };
    ++stage.bodies;
    if (item.sha256) stage.hashes.add(item.sha256);
    stage.visits.add(item.visit);
}
const stages = Object.fromEntries(Object.entries(stageCounts).map(([name, value]) => [name, {
    savedBodies: value.bodies, uniqueSha256: value.hashes.size, visits: value.visits.size
}]));
const valid = records.length === session.requestedVisits && session.attempts.length === session.requestedVisits && records.every(r => r.captureValid);
const summary = {
    inputURL: session.url, requestedVisits: session.requestedVisits, completedVisits: records.length,
    observationSeconds: session.observationSeconds, startedUtc: session.startedUtc, endedUtc: session.endedUtc,
    captureValid: valid, savedScripts: records.reduce((sum, v) => sum + v.cached, 0),
    uniqueSourceSha256: allSourceHashes.size, uniqueNonObserverSourceSha256: sourceHashes.size,
    savedResponseBodies: records.reduce((sum, v) => sum + v.resources, 0),
    cdpErrors: records.reduce((sum, v) => sum + v.cdpErrors, 0),
    staleSnapshotErrors: records.reduce((sum, v) => sum + v.staleSnapshotErrors, 0),
    sourceReadErrors: records.reduce((sum, v) => sum + v.sourceReadErrors, 0),
    responseReadErrors: records.reduce((sum, v) => sum + v.responseReadErrors, 0),
    unexpectedCdpErrors: records.reduce((sum, v) => sum + v.unexpectedCdpErrors, 0),
    successfulVerificationVisits: records.filter(v => v.tokenPresent === true).length,
    stages, visits: records
};
fs.writeFileSync(path.join(root, 'capture-summary.json'), JSON.stringify(summary, null, 2));
fs.writeFileSync(path.join(root, 'script-variants.json'), JSON.stringify([...sourceVariants.values()].sort((a, b) => a.sha256.localeCompare(b.sha256)), null, 2));
let markdown = '# WebView2 보안 스크립트 재수집\n\n';
markdown += `입력: \`${session.url}\`\n\n`;
markdown += `${session.requestedVisits}회 요청 중 ${records.length}회를 수집했습니다. 각 방문은 새 WebView2 프로필에서 ${session.observationSeconds}초간 관찰했습니다.\n\n`;
markdown += `스크립트 원본 ${summary.savedScripts}개, 고유 SHA-256 ${summary.uniqueSourceSha256}개, 응답 본문 ${summary.savedResponseBodies}개를 저장했습니다. 이 수에는 브라우저 내부 코드와 수집기의 관찰 코드도 포함됩니다.\n\n`;
markdown += '|방문|스크립트 기록/저장|Worker 소스|응답 본문|완료(ms)|토큰 존재|CDP 오류|원본 검증|\n|---|---:|---:|---:|---:|---|---:|---|\n';
for (const v of records) markdown += `|${v.visit}|${v.scripts}/${v.cached}|${v.workers}|${v.resources}|${v.completionMs?.toFixed(1) || '관찰되지 않음'}|${v.tokenPresent === true ? '예' : '아니오'}|${v.cdpErrors}|${v.captureValid ? '정상' : '확인 필요'}|\n`;
markdown += '\n|수신 단계|저장 응답 수|서로 다른 SHA-256|방문 수|\n|---|---:|---:|---:|\n';
for (const [name, value] of Object.entries(stages)) markdown += `|${name}|${value.savedBodies}|${value.uniqueSha256}|${value.visits}|\n`;
markdown += '\n각 방문의 `scripts/`에 외부·인라인·eval·Function·Worker 실행 소스를 보관했습니다. `responses/`는 수집한 HTTP 응답 본문, `events.jsonl`은 단계·네트워크 기록입니다. `manifest.json`에서 원본 URL, 파일, SHA-256, 타이밍을 연결할 수 있습니다. `source-variations.json`은 방문마다 받은 검증 응답의 변화, `script-variants.json`은 스크립트 해시별 원본 위치를 연결합니다.\n\n';
markdown += '수집 검증은 관찰된 `Debugger.scriptParsed` 이벤트와 저장 파일 수가 일치하는지, 응답 파일이 존재하는지, 원본 읽기 오류와 미완료 요청이 없는지 확인했습니다. 완료 시간에는 디버거 연결·정지·재개 비용이 포함됩니다.\n\n';
markdown += `CDP 오류 총 ${summary.cdpErrors}건 중 종료된 iframe의 상태 조회 오류가 ${summary.staleSnapshotErrors}건입니다. 원본 스크립트 읽기 오류 ${summary.sourceReadErrors}건, 응답 본문 읽기 오류 ${summary.responseReadErrors}건, 그 밖의 CDP 오류 ${summary.unexpectedCdpErrors}건입니다. 종료 이벤트가 오류보다 앞선 기록을 확인한 상태 조회 오류만 별도로 분류했고 원본 오류 기록은 보존했습니다.\n\n`;
markdown += `이 기록은 해당 ${session.requestedVisits}회 방문에서 관찰된 실행 소스와 응답입니다. 관찰되지 않은 서버 분기나 WASM 바이너리까지 모두 확보한 것은 아닙니다. 초기 iframe 문서 응답이 수집기 연결보다 먼저 도착하면 HTML 본문을 놓칠 수 있지만, 관찰된 인라인 스크립트 원본도 별도로 저장·검증했습니다.\n`;
fs.writeFileSync(path.join(root, 'comparison.md'), markdown);
fs.writeFileSync(path.join(root, 'README.md'), markdown);
console.log(JSON.stringify({ ...summary, visits: records.map(({ metrics, ...record }) => record) }, null, 2));
if (!valid) process.exitCode = 1;

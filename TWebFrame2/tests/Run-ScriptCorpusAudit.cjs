const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const root = path.resolve(process.argv[2]);
const probe = path.resolve(__dirname, 'bin/x64/Release/ScriptCorpusAudit.exe');
const inventory = JSON.parse(fs.readFileSync(path.join(root, 'inventory.json'), 'utf8'));
const output = path.join(root, 'isolated');
fs.mkdirSync(output, { recursive: true });
let next = 0, done = 0;
const results = [];
async function audit(source) {
    if (source.mode === 'wasm-placeholder') return { id: source.id, mode: source.mode, file: source.file,
        syntaxOk: null, executed: false, skipped: 'WebAssembly metadata; binary source was not archived', exitCode: 0 };
    const label = source.id.replace(':', '-');
    const input = path.join(output, label + '.tsv'), result = path.join(output, label + '.jsonl');
    fs.writeFileSync(input, [source.id, source.mode, source.file, source.mode === 'worker' && !/^https?:/.test(source.url) ? 'https://challenges.cloudflare.com/offline-audit' : source.url].join('\t'));
    return new Promise(resolve => {
        const child = spawn(probe, [input, result, '1000'], { windowsHide: true, timeout: 6000 });
        let log = '', launchError = '';
        child.stdout.on('data', b => { log += b; });
        child.stderr.on('data', b => { if (log.length < 20000) log += b; });
        child.on('error', e => { launchError = e.message; });
        child.on('close', (exitCode, signal) => {
            let record = null;
            try { const lines = fs.readFileSync(result, 'utf8').trim().split(/\r?\n/).filter(Boolean); if (lines.length) record = JSON.parse(lines[0]); } catch {}
            record = { ...(record || { id: source.id, mode: source.mode, file: source.file, resultMissing: true }), exitCode, signal, launchError };
            if (exitCode !== 0 || launchError || !record.syntaxOk || record.resultMissing) fs.writeFileSync(path.join(output, label + '.log'), log);
            resolve(record);
        });
    });
}
async function lane() {
    while (next < inventory.sources.length) {
        const source = inventory.sources[next++];
        results.push(await audit(source));
        ++done;
        if (done % 100 === 0 || done === inventory.sources.length) console.log(`Checked ${done}/${inventory.sources.length}`);
    }
}
(async () => {
    await Promise.all([lane(), lane()]);
    results.sort((a, b) => a.id.localeCompare(b.id));
    fs.writeFileSync(path.join(root, 'runtime-audit.jsonl'), results.map(r => JSON.stringify(r)).join('\n') + '\n');
    console.log(JSON.stringify({ checked: results.length, processFailures: results.filter(r => r.exitCode !== 0).length, syntaxFailures: results.filter(r => r.syntaxOk === false).length, executionFailures: results.filter(r => r.executed && !r.executionOk).length }, null, 2));
})().catch(error => { console.error(error); process.exitCode = 1; });

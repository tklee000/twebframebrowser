const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const vm = require('node:vm');
const acorn = require('internal/deps/acorn/acorn/dist/acorn');
const root = path.resolve(process.argv[2] || 'TWebFrame2/tests/artifacts');
const output = path.resolve(process.argv[3]);
fs.mkdirSync(output, { recursive: true });
const all = [];
function walk(folder) {
    for (const entry of fs.readdirSync(folder, { withFileTypes: true })) {
        if (['profile', 'node_modules', '.git'].includes(entry.name)) continue;
        const file = path.join(folder, entry.name);
        if (file === output) continue;
        if (entry.isDirectory()) walk(file); else all.push(file);
    }
}
walk(root);
const read = file => JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\ufeff/, ''));
const metadata = new Map(), missing = [], warnings = [];
for (const file of all.filter(file => path.basename(file) === 'sources.jsonl')) {
    for (const line of fs.readFileSync(file, 'utf8').split(/\r?\n/).filter(Boolean)) {
        try { const meta = JSON.parse(line); metadata.set(path.join(path.dirname(file), meta.file), meta); }
        catch (error) { warnings.push({ file, error: error.message }); }
    }
}
const sessionsByFolder = new Map();
for (const file of all.filter(file => file.endsWith('.js.meta.json'))) {
    const sourceFile = file.slice(0, -10), folder = path.dirname(path.dirname(file));
    if (!sessionsByFolder.has(folder)) {
        const sessions = new Map(), events = path.join(folder, 'events.jsonl');
        if (fs.existsSync(events)) for (const line of fs.readFileSync(events, 'utf8').split(/\r?\n/).filter(Boolean)) {
            try { const event = JSON.parse(line); if (event.kind === 'Target.attachedToTarget') sessions.set(event.data.sessionId, event.data.targetInfo); }
            catch (error) { warnings.push({ file: events, error: error.message }); }
        }
        sessionsByFolder.set(folder, sessions);
    }
    const meta = read(file), session = path.basename(sourceFile).split('-')[1];
    metadata.set(sourceFile, { ...meta, kind: sessionsByFolder.get(folder).get(session)?.type === 'worker' ? 'worker-program' : 'program' });
}
for (const [file, meta] of metadata) if (!fs.existsSync(file)) missing.push({ file: path.relative(root, file), kind: meta.kind, url: meta.url });
const files = [], unique = new Map();
for (const file of all.filter(file => file.endsWith('.js')).sort()) {
    const bytes = fs.readFileSync(file), sha256 = crypto.createHash('sha256').update(bytes).digest('hex');
    const meta = metadata.get(file) || {}, mode = meta.scriptLanguage === 'WebAssembly' || (meta.url || '').startsWith('wasm:') ? 'wasm-placeholder' : meta.kind === 'worker-program' ? 'worker' : 'program';
    const key = sha256 + ':' + mode;
    const source = unique.get(key) || { id: key, sha256, mode, bytes: bytes.length, file, url: meta.url || '', kinds: [], owners: [] };
    if (!source.kinds.includes(meta.kind || 'stored-fixture')) source.kinds.push(meta.kind || 'stored-fixture');
    source.owners.push(path.relative(root, file).replaceAll('\\', '/'));
    unique.set(key, source);
    files.push({ file: path.relative(root, file).replaceAll('\\', '/'), id: key, sha256, mode, bytes: bytes.length });
}
const roots = new Set(('window self globalThis navigator document crypto performance console Object Array String Number Boolean Math JSON Date RegExp Function Reflect Promise Map Set WeakMap WeakSet Symbol BigInt Intl ArrayBuffer SharedArrayBuffer DataView Uint8Array Uint8ClampedArray Uint16Array Uint32Array Int8Array Int16Array Int32Array Float32Array Float64Array TextEncoder TextDecoder URL URLSearchParams Blob File Worker OffscreenCanvas WebAssembly AudioContext Event MessageChannel MutationObserver PerformanceObserver').split(' '));
const nodeTypes = {}, features = {}, chains = new Map();
function chain(node) {
    if (node?.type === 'Identifier') return roots.has(node.name) ? node.name : null;
    if (node?.type !== 'MemberExpression') return null;
    const base = chain(node.object), key = !node.computed ? node.property.name : node.property.type === 'Literal' && typeof node.property.value === 'string' ? node.property.value : null;
    return base && key && /^[A-Za-z_$][\w$]*$/.test(key) ? base + '.' + key : null;
}
function visit(node, source, text) {
    if (!node || typeof node !== 'object') return;
    if (node.type) {
        nodeTypes[node.type] = (nodeTypes[node.type] || 0) + 1;
        const flags = [];
        if (node.async) flags.push('async-function');
        if (node.generator) flags.push('generator');
        if (node.optional) flags.push('optional-chain');
        if (node.type === 'Literal' && node.bigint !== undefined) flags.push('bigint');
        if (node.type === 'Literal' && /\d_\d/.test(node.raw || '')) flags.push('numeric-separator');
        if (node.type === 'Literal' && node.regex) { if (node.regex.flags.includes('u')) flags.push('regexp-unicode'); if (node.regex.pattern.includes('(?<')) flags.push('regexp-named-or-lookbehind'); }
        for (const flag of flags) { const value = features[flag] ||= { occurrences: 0, examples: [] }; ++value.occurrences; if (value.examples.length < 8) value.examples.push({ id: source.id, offset: node.start, source: text.slice(node.start, Math.min(node.end, node.start + 160)) }); }
        const api = chain(node);
        if (api) { const value = chains.get(api) || { path: api, occurrences: 0, owners: new Set() }; ++value.occurrences; value.owners.add(source.id); chains.set(api, value); }
    }
    for (const [key, value] of Object.entries(node)) if (!['start', 'end', 'loc'].includes(key)) {
        if (Array.isArray(value)) value.forEach(child => visit(child, source, text));
        else if (value && typeof value === 'object') visit(value, source, text);
    }
}
let count = 0;
for (const source of unique.values()) {
    const text = fs.readFileSync(source.file, 'utf8').replace(/^\ufeff/, '');
    try { new vm.Script(text); source.referenceSyntaxOk = true; source.referenceSourceMode = 'script'; }
    catch (error) {
        try { new vm.Script('(function(){\n' + text + '\n})'); source.referenceSyntaxOk = true; source.referenceSourceMode = 'function-body'; }
        catch { source.referenceSyntaxOk = false; source.referenceSyntaxError = error.message; }
    }
    try { visit(acorn.parse(text, { ecmaVersion: 'latest', allowReturnOutsideFunction: true }), source, text); }
    catch (error) { source.inventoryParserError = error.message; }
    if (++count % 100 === 0) console.log('Inventoried ' + count + ' distinct source/mode combinations');
}
const sources = [...unique.values()];
const inventory = { root, fileCount: files.length, distinctSources: new Set(files.map(f => f.sha256)).size, distinctSourceModes: sources.length, missingSources: missing, warnings, files, sources, astNodeTypes: nodeTypes, features, apiChains: [...chains.values()].map(v => ({ ...v, owners: [...v.owners] })) };
fs.writeFileSync(path.join(output, 'inventory.json'), JSON.stringify(inventory, null, 2));
fs.writeFileSync(path.join(output, 'input.tsv'), sources.map(s => [s.id, s.mode, s.file, s.url].join('\t')).join('\n'));
console.log(JSON.stringify({ files: files.length, distinctSources: inventory.distinctSources, distinctSourceModes: sources.length, missingSources: missing.length, referenceSyntaxErrors: sources.filter(s => !s.referenceSyntaxOk).length, nodeTypes: Object.keys(nodeTypes), features: Object.keys(features), apiChains: chains.size }, null, 2));

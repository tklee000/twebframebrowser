const fs = require('node:fs');
const path = require('node:path');
const http = require('node:http');
const root = path.resolve(process.argv[2]);
http.createServer((request, response) => {
    if (request.url === '/capabilities.html' || request.url === '/wasm-fixture.html') { response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' }); response.end(fs.readFileSync(path.join(root, request.url.slice(1)))); }
    else { response.writeHead(404); response.end(); }
}).listen(8879, '127.0.0.1', () => console.log('Corpus capabilities on 127.0.0.1:8879'));

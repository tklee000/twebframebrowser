// Only the documented public test-key fixture is served. No production keys,
// application cookies or accounts are used by this server.
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const fixture = fs.readFileSync(path.join(__dirname, 'turnstile-integration-test.html'));
const port = Number(process.argv[2] || 8768);
const server = http.createServer((req, res) => {
  const pathname = new URL(req.url, 'http://127.0.0.1').pathname;
  if (pathname !== '/turnstile-integration-test.html' && pathname !== '/') {
    res.writeHead(404, {'Content-Type': 'text/plain'});
    res.end('Not found');
    return;
  }
  res.writeHead(200, {'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store'});
  res.end(fixture);
});
server.listen(port, '127.0.0.1', () => {
  console.log(`TEST_ONLY server PID=${process.pid} http://127.0.0.1:${port}/turnstile-integration-test.html`);
});

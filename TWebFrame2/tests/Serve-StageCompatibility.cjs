const http=require('node:http');
const fs=require('node:fs');
const path=require('node:path');
const server=http.createServer((req,res)=>{res.writeHead(200,{'Content-Type':'text/html; charset=utf-8','Cache-Control':'no-store'});res.end(fs.readFileSync(path.join(__dirname,'BrowserStageCompatibility.html')));});
server.listen(8877,'127.0.0.1',()=>process.stdout.write('Compatibility fixture: http://127.0.0.1:8877/\n'));

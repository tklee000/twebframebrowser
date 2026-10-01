function check(value,message){if(!value)throw new Error(message);}
var text='AZ'.repeat(150000),output=[],begin=Date.now();
for(var i=0;i<text.length;i++)output.push(String.fromCharCode(text.charCodeAt(i)^32));
var elapsed=Date.now()-begin;
check(output.length===300000 && output.join('')==='az'.repeat(150000),'string loop and array push preserve the decoded bytes');
return 'PASS|array-push-loop-ms='+elapsed;

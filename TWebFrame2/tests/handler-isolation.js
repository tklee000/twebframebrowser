var node=document.createElement('button');
return 'PASS|handlers='+[document.onclick,document.onchange,document.onload,window.onclick,window.onmessage,node.onclick].map(function(x){return x===null?'null':typeof x;}).join(',');

function check(condition,message){if(!condition)throw new Error(message);}
var canvas=document.createElement('canvas');canvas.width=3;canvas.height=2;
var context=canvas.getContext('2d');context.fillStyle='red';context.fillRect(0,0,1,1);
var url=canvas.toDataURL(),prefix='data:image/png;base64,';
check(url.indexOf(prefix)===0,'actual PNG data URL');
var bytes=atob(url.slice(prefix.length));
check(bytes.charCodeAt(0)===137 && bytes.slice(1,4)==='PNG','PNG signature');
check(bytes.charCodeAt(19)===3 && bytes.charCodeAt(23)===2,'intrinsic bitmap dimensions');
check(canvas.toDataURL('image/unsupported')===url,'unsupported format uses PNG');
check(HTMLCanvasElement.prototype.toDataURL.call(canvas)===url,'prototype receiver');
var typeError=false;try{HTMLCanvasElement.prototype.toDataURL.call({});}catch(error){typeError=error.name==='TypeError';}
check(typeError,'invalid receiver');
canvas.width=0;check(canvas.toDataURL()==='data:,','zero-size data URL');canvas.width=3;
var exportOrder=[],pngBlob;
canvas.toBlob(function(value){pngBlob=value;exportOrder.push('callback');});exportOrder.push('sync');
check(exportOrder.join(',')==='sync','toBlob is asynchronous');
return 'PASS|canvasExport|ratio='+window.devicePixelRatio;

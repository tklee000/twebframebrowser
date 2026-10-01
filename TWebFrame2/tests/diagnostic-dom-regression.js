function check(value,message){if(!value)throw new Error(message);}
check(window.clientInformation===navigator && clientInformation===navigator,'clientInformation aliases the real Navigator');
check(window.frameElement===null,'top-level window has no embedding element');
check(typeof Document.prototype.hasFocus==='function' && Document.prototype.hasFocus.call(document)===document.hasFocus(),'prototype focus method uses the real document focus provider');
var forms=document.forms,images=document.images,links=document.links,scripts=document.scripts;
check(forms===document.forms && images===document.images && links===document.links,'document collections preserve identity');
check(Object.getOwnPropertyDescriptor(Document.prototype,'forms').get.call(document)===forms,'document collection is a prototype accessor');
var form=document.createElement('form'),image=document.createElement('img'),link=document.createElement('a');
form.id='collection-form';image.setAttribute('name','collection-image');link.href='https://example.test/';
document.body.appendChild(form);document.body.appendChild(image);document.body.appendChild(link);
check(forms.length===1 && forms[0]===form && forms.item(0)===form && forms.namedItem('collection-form')===form,'forms collection tracks insertions and names');
check(images.length===1 && images.namedItem('collection-image')===image && links.length===1,'image and link collections read real elements');
form.remove();image.remove();link.remove();
check(forms.length===0 && images.length===0 && links.length===0 && forms.item(0)===null && forms.namedItem('collection-form')===null,'retained collections track removals');
var rejected=false;try{forms.item.call({},0);}catch(error){rejected=error instanceof TypeError;}
check(rejected,'collection methods reject an incompatible receiver');
var canvas=document.createElement('canvas');canvas.width=64;canvas.height=64;
var context=canvas.getContext('2d');
check(context.ellipse===CanvasRenderingContext2D.prototype.ellipse && context.ellipse.length===7,'ellipse has a shared prototype method');
context.fillStyle='#00ff00';context.beginPath();context.ellipse(32,32,12,5,Math.PI/2,0,Math.PI*2);context.fill();
function alpha(x,y){return context.getImageData(x,y,1,1).data[3];}
check(alpha(32,32)>200 && alpha(32,40)>200 && alpha(40,32)===0,'rotated ellipse produces actual intrinsic pixels');
context.clearRect(0,0,64,64);context.setTransform(1,0,0,1,10,0);context.beginPath();context.ellipse(20,20,8,4,0,0,Math.PI*2);context.fill();
check(alpha(30,20)>200 && alpha(20,20)===0,'ellipse composes with the current transform');
var rejected=false;try{context.ellipse(0,0,-1,2,0,0,1);}catch(error){rejected=error.name==='IndexSizeError';}
check(rejected,'negative radius throws');
rejected=false;try{context.ellipse(0,0,1);}catch(error){rejected=error instanceof TypeError;}
check(rejected,'missing required coordinates throw');
var conversions=[];function coordinate(value,index){return {valueOf:function(){conversions.push(index);return value;}};}
context.ellipse(coordinate(0,0),coordinate(0,1),coordinate(1,2),coordinate(1,3),coordinate(0,4),coordinate(0,5),coordinate(1,6));
check(conversions.join(',')==='0,1,2,3,4,5,6','coordinates convert once in argument order');
return 'PASS|diagnostic DOM APIs and real ellipse pixels';

function check(value,message){if(!value)throw new Error(message);}
check(typeof Navigator==='function' && Object.getPrototypeOf(navigator)===Navigator.prototype && navigator instanceof Navigator,'Navigator has its actual interface prototype');
check(clientInformation===navigator,'clientInformation retains Navigator identity');
check(navigator.constructor===Navigator && screen.constructor===Screen && performance.constructor===Performance,'interface constructor identities');
check(Object.prototype.toString.call(navigator)==='[object Navigator]' && Object.prototype.toString.call(screen)==='[object Screen]' && Object.prototype.toString.call(performance)==='[object Performance]','interface object tags');
check(Object.keys(navigator).length===0 && !navigator.hasOwnProperty('userAgent'),'Navigator attributes live on its interface prototype');
var descriptor=Object.getOwnPropertyDescriptor(Navigator.prototype,'userAgent');
check(descriptor && descriptor.get.call(navigator)===navigator.userAgent && navigator.userAgent.indexOf('TWebFrame')>=0,'Navigator prototype getter exposes the engine identity');
var brand=false;try{descriptor.get.call({});}catch(error){brand=error instanceof TypeError;}check(brand,'Navigator getter receiver check');
check(typeof Screen==='function' && screen instanceof Screen && Object.getPrototypeOf(screen)===Screen.prototype,'Screen has its actual interface prototype');
check(Object.getOwnPropertyDescriptor(Screen.prototype,'width').get.call(screen)===screen.width,'Screen getter returns actual display metrics');
check(typeof Performance==='function' && performance instanceof Performance && performance instanceof EventTarget,'Performance implements its interface and EventTarget');
check(performance.now===Performance.prototype.now && Number.isFinite(Performance.prototype.now.call(performance)),'Performance prototype method invokes the actual monotonic clock');
var origin=Object.getOwnPropertyDescriptor(Performance.prototype,'timeOrigin');check(origin.get.call(performance)===performance.timeOrigin,'Performance origin getter uses the actual clock origin');
performance.mark('interface-mark');check(Performance.prototype.getEntriesByName.call(performance,'interface-mark').length===1,'Performance methods use actual entries');
var count=0;performance.addEventListener('interface-event',function(){count++;});performance.dispatchEvent(new Event('interface-event'));check(count===1,'Performance EventTarget uses real listeners');
var names=[Navigator,Screen,Performance],rejected=0;for(var i=0;i<names.length;i++)try{new names[i]();}catch(error){if(error instanceof TypeError)rejected++;}
check(rejected===names.length,'non-constructible host interfaces reject construction');
return 'PASS|host interface prototypes and real values';

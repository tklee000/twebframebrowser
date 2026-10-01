function check(value,message){if(!value)throw new Error(message);}
var target=new EventTarget(),order=[];
function second(event){order.push('second');event.preventDefault();}
target.addEventListener('work',function(event){order.push('first');check(event.target===target && event.currentTarget===target && this===target,'event dispatch receiver and target');},{once:true});
target.addEventListener('work',second);
var event=new Event('work',{cancelable:true,bubbles:true,composed:true});
check(event instanceof Event && event.isTrusted===false && event.composed,'constructed Event has its real prototype and flags');
check(target.dispatchEvent(event)===false && order.join(',')==='first,second' && event.defaultPrevented,'cancelable event dispatch runs listeners');
check(event.currentTarget===null && event.eventPhase===0,'dispatch cleanup');
order=[];target.dispatchEvent(new Event('work'));check(order.join(',')==='second','once listener is removed');
target.removeEventListener('work',second);order=[];target.dispatchEvent(new Event('work'));check(order.length===0,'listener removal');
var plain=new Event('plain');plain.preventDefault();check(!plain.defaultPrevented,'non-cancelable Event cannot be canceled');
var detail={},custom=new CustomEvent('custom',{detail:detail});check(custom instanceof CustomEvent && custom instanceof Event && custom.detail===detail,'CustomEvent has its own prototype and preserves detail');
var payload={},message=new MessageEvent('message',{data:payload,origin:'https://example.test',lastEventId:'id'});
check(message instanceof MessageEvent && message instanceof Event && message.data===payload && message.origin==='https://example.test' && message.lastEventId==='id','MessageEvent initializes actual message data');
var mouse=new MouseEvent('click',{clientX:12,clientY:34,ctrlKey:true,button:1,buttons:2});
check(mouse instanceof MouseEvent && mouse instanceof UIEvent && mouse instanceof Event && mouse.clientX===12 && mouse.clientY===34 && mouse.ctrlKey && mouse.button===1 && mouse.buttons===2,'MouseEvent initialization and inheritance');
var keyboard=new KeyboardEvent('keydown',{key:'Enter',code:'Enter',location:1,repeat:true,altKey:true});
check(keyboard instanceof KeyboardEvent && keyboard instanceof UIEvent && keyboard.key==='Enter' && keyboard.code==='Enter' && keyboard.location===1 && keyboard.repeat && keyboard.altKey,'KeyboardEvent initialization');
check(document.body instanceof EventTarget && document instanceof EventTarget,'DOM nodes inherit EventTarget');
var rejected=false;try{Event('work');}catch(error){rejected=error instanceof TypeError;}check(rejected,'Event requires new');
rejected=false;try{new Event();}catch(error){rejected=error instanceof TypeError;}check(rejected,'Event requires a type');
rejected=false;try{EventTarget.prototype.addEventListener.call({},'x',function(){});}catch(error){rejected=error instanceof TypeError;}check(rejected,'EventTarget method brand check');
rejected=false;try{target.dispatchEvent(new Event(''));}catch(error){rejected=error.name==='InvalidStateError';}check(rejected,'empty event type rejects dispatch');
var getters=0,options={};Object.defineProperty(options,'data',{get:function(){getters++;return payload;}});
check(new MessageEvent('message',options).data===payload && getters===1,'dictionary getter reads once');
var globalCalls=0;function globalListener(){globalCalls++;}
addEventListener('global-event',globalListener);
var bareDispatch=dispatchEvent;bareDispatch(new Event('global-event'));
EventTarget.prototype.removeEventListener.call(null,'global-event',globalListener);
bareDispatch(new Event('global-event'));check(globalCalls===1,'nullish Web IDL receiver uses the realm global');
var marker={},caught=false;try{new PointerEvent('pointer',{width:{valueOf:function(){throw marker;}}});}catch(error){caught=error===marker;}
check(caught,'event number coercion preserves the original exception');
var stoppedTarget=new EventTarget(),stoppedCount=0,stoppedEvent=new Event('stop');
stoppedTarget.addEventListener('stop',function(event){stoppedCount++;event.stopImmediatePropagation();},{once:true});
stoppedTarget.addEventListener('stop',function(){stoppedCount++;});
stoppedTarget.dispatchEvent(stoppedEvent);stoppedTarget.dispatchEvent(stoppedEvent);
check(stoppedCount===2,'dispatch clears the immediate stop flag for reuse');
return 'PASS|event interfaces and dispatch';

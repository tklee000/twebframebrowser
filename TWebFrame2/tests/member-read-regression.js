function check(value,message){if(!value)throw new Error(message);}
var object={deep:{value:7},method:function(i){return i+1;}};
for(var warm=0;warm<200;warm++)object.method(warm);
var begin=Date.now(),sum=0;for(var i=0;i<500000;i++){sum+=object.deep.value;sum+=object.method(i);}var elapsed=Date.now()-begin;
check(sum===125003750000,'member reads and calls preserve results');
var reads=0,changed={value:13},holder={};Object.defineProperty(holder,'current',{get:function(){reads++;return changed;}});
check(holder.current.value===13 && reads===1,'nested member getter runs once');
var target={value:17,method:function(){return this.value;}},proxies=0;
var proxy=new Proxy(target,{get:function(base,key,receiver){proxies++;return base[key];}});
check(proxy.method()===17 && proxies===2,'proxy member call preserves receiver');
var marker={},caught=false;Object.defineProperty(holder,'failure',{get:function(){throw marker;}});try{var fail=holder.failure.value;}catch(error){caught=error===marker;}check(caught,'nested getter error identity');
var first={value:19},reference=first;check(reference[(reference={value:23},'value')]===19,'computed member base evaluates before key');
return 'PASS|member-read-ms='+elapsed;

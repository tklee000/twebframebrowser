function check(value,message){if(!value)throw new Error(message);}
var literalMethod={run(){}};
var values=[undefined,null,true,17,'name',{},[],()=>{},literalMethod.run],failures=0,argumentsRead=0;
function argument(){argumentsRead++;return 1;}
for(var i=0;i<values.length;i++){try{new values[i](argument());}catch(error){if(error instanceof TypeError)failures++;}}
check(failures===values.length,'non-constructors must throw TypeError');
check(argumentsRead===values.length,'new evaluates arguments before checking constructor');
var asyncFn=async function(){},generatorFn=function*(){};
try{new asyncFn();}catch(error){failures++;}
try{new generatorFn();}catch(error){failures++;}
check(failures===values.length+2,'async and generator functions are not constructors');
var calls=0;function Real(value){this.value=value;calls++;}
var Bound=Real.bind(null,3),instance=new Bound();
check(instance.value===3 && calls===1 && instance instanceof Real,'bound constructor forwards construction');
var rejected=false;try{Reflect.construct(Real,[],()=>{});}catch(error){rejected=error instanceof TypeError;}
check(rejected && calls===1,'Reflect.construct checks newTarget before invoking target');
var listRead=0,list={get length(){listRead++;return 0;}};
try{Reflect.construct(Real,list,()=>{});}catch(error){}
check(listRead===0,'invalid newTarget is rejected before array-like argument reads');
rejected=false;var invalidProxy=new Proxy({}, {construct:function(){return {};}});
try{new invalidProxy();}catch(error){rejected=error instanceof TypeError;}
check(rejected,'a construct trap cannot make a non-constructor constructible');
rejected=false;var scalarProxy=new Proxy(Real,{construct:function(){return 1;}});
try{new scalarProxy();}catch(error){rejected=error instanceof TypeError;}
check(rejected,'construct trap must return an object');
return 'PASS|constructor errors and bound construction';

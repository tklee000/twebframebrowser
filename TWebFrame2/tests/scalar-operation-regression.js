function check(value,message){if(!value)throw new Error(message);}
function kernel(callback,count){var total=0;for(var index=0;index<count;index++)total+=((callback(index)+index*3)^7)>>>0;return total;}
function identity(value){return value;}
check(Object.is(-(-0),0) && Object.is(-(+0),-0),'signed zero unary operations');
check(Object.is(-0*3,-0) && Object.is(-0/3,-0) && Object.is(-4%2,-0),'signed zero arithmetic');
check(NaN!==NaN && !(NaN===NaN) && !(NaN<0) && !(NaN<=0) && !(NaN>0) && !(NaN>=0),'NaN comparisons');
check((Infinity-Infinity)!==(Infinity-Infinity) && 1/0===Infinity && 1/-0===-Infinity,'nonfinite arithmetic');
check((4294967297|0)===1 && (-1>>>0)===4294967295 && (Infinity&3)===0 && (~NaN)===-1,'ToUint32 boundaries');
check((2**3)===8 && (1<<33)===2 && (-4>>1)===-2,'power and shift operators');
check(2=='2' && 2!=='2' && '2'+3==='23','mixed operands keep coercion and string semantics');
var order=[];
var source={get left(){order.push('left');return 2;},get right(){order.push('right');return 3;}};
check(source.left+source.right===5 && order.join(',')==='left,right','numeric getters execute in order once');
var conversions=0,object={valueOf:function(){conversions++;return 4;}};
check(object+2===6 && conversions===1,'object addition conversion stays observable');
check(2n+3n===5n && (-4n>>1n)===-2n,'BigInt operations keep their ordinary execution path');
var started=Date.now(),checksum=kernel(identity,300000);
check(checksum===180000300000,'mixed numeric call loop checksum');
return 'PASS|scalarOperationMs='+(Date.now()-started)+'|checksum='+checksum;

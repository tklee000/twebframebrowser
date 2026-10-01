function check(value, message) { if (!value) throw new Error(message); }
var expected = 1790837747000;
check(Date.parse('Thu, 01 Oct 2026 06:55:47 GMT') === expected, 'HTTP date parsing');
check(new Date('Thu, 01 Oct 2026 06:55:47 GMT').getTime() === expected, 'HTTP date constructor');
check(Date.parse(new Date(expected).toUTCString()) === expected, 'UTC date round trip');
check(Date.parse('1969-12-31T23:59:59.000Z') === -1000, 'date before Unix epoch');
check(Date.parse('Thu, 01 Oct 2026 06:55:47 GMT trailing') !== expected, 'invalid HTTP date suffix');
var now = Date.now();
check(Math.abs(performance.timeOrigin + performance.now() - now) < 100, 'wall and monotonic clock alignment');
var reads = 0, original = {value:7};
Object.defineProperty(original, 'computed', {get:function(){ reads++; return this.value; }});
check(original.computed + original.computed === 14 && reads === 2, 'property getter side effects');
var objects = [original];
check(objects[0].computed === 7 && reads === 3, 'indexed property read');
original.method = function(){ return this.value; };
check(original.method() === 7 && objects[0]['method']() === 7, 'method receiver retention');
var caught = false;
Object.defineProperty(original, 'failure', {get:function(){ throw 42; }});
try { var failure = original.failure + 1; } catch (error) { caught = error === 42; }
check(caught, 'getter exception handling');
original.value = 11; check(original.value === 11, 'assignment retains property reference');
delete original.value; check(original.value === undefined, 'delete retains property reference');
return 'PASS';

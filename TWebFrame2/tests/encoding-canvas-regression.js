function assert(value, message) { if (!value) throw new Error(message); }
function bytes(value) { return Array.prototype.join.call(value, ','); }
var encoder = new TextEncoder();
assert(encoder.encoding === 'utf-8', 'encoder encoding');
assert(encoder instanceof TextEncoder, 'encoder prototype');
assert(bytes(encoder.encode('A\uD55C\uD83D\uDE00')) === '65,237,149,156,240,159,152,128', 'Unicode UTF-8');
assert(bytes(encoder.encode('\uD800x\uDC00')) === '239,191,189,120,239,191,189', 'isolated surrogate replacement');
assert(encoder.encode().length === 0 && encoder.encode(undefined).length === 0, 'optional input');
assert(bytes(encoder.encode(null)) === '110,117,108,108', 'string conversion');
var storage = new Uint8Array([9,9,9,9,9,9,9,9]);
var destination = new Uint8Array(storage.buffer, 2, 5);
var progress = encoder.encodeInto('\uD83D\uDE00AB', destination);
assert(progress.read === 3 && progress.written === 5, 'encodeInto progress in UTF-16 units');
assert(bytes(storage) === '9,9,240,159,152,128,65,9', 'encodeInto writes shared view only');
progress = encoder.encodeInto('\uD55C', new Uint8Array(2));
assert(progress.read === 0 && progress.written === 0, 'encodeInto does not split scalar');
var invalid = false;
try { TextEncoder.prototype.encode.call({}, 'x'); } catch (error) { invalid = error.name === 'TypeError'; }
assert(invalid, 'encode receiver validation');
invalid = false;
try { encoder.encodeInto('x', new Uint8ClampedArray(3)); } catch (error) { invalid = error.name === 'TypeError'; }
assert(invalid, 'encodeInto destination validation');
var canvas = document.createElement('canvas'); canvas.width = 100; canvas.height = 100;
var ctx = canvas.getContext('2d');
var attributes = ctx.getContextAttributes();
assert(attributes.alpha === true && attributes.colorSpace === 'srgb' && attributes.colorType === 'unorm8', 'actual context attributes');
assert(attributes !== ctx.getContextAttributes(), 'attributes are snapshots');
ctx.fillStyle = 'red'; ctx.beginPath(); ctx.moveTo(10, 80);
ctx.quadraticCurveTo(50, 0, 90, 80); ctx.lineTo(10, 80); ctx.fill();
var center = ctx.getImageData(50, 60, 1, 1).data;
assert(center[0] > 240 && center[3] > 240, 'quadratic curve fills real pixels');
var outside = ctx.getImageData(50, 20, 1, 1).data;
assert(outside[3] === 0, 'quadratic control point is not a vertex');
ctx.clearRect(0, 0, 100, 100); ctx.beginPath(); ctx.moveTo(10,80);
ctx.bezierCurveTo(10,0,90,0,90,80); ctx.lineTo(10,80); ctx.fill();
assert(ctx.getImageData(50,30,1,1).data[3] > 240, 'cubic curve rasterization');
invalid = false;
try { CanvasRenderingContext2D.prototype.quadraticCurveTo.call({}, 1,2,3,4); } catch (error) { invalid = error.name === 'TypeError'; }
assert(invalid, 'canvas curve receiver validation');
var opaque = document.createElement('canvas'); opaque.width = 4; opaque.height = 4;
var opaqueCtx = opaque.getContext('2d', {alpha:false,willReadFrequently:true});
assert(opaqueCtx.getContextAttributes().alpha === false && opaqueCtx.getContextAttributes().willReadFrequently === true, 'context options');
assert(opaqueCtx.getImageData(0,0,1,1).data[3] === 255, 'opaque backing store');
opaqueCtx.fillStyle='red'; opaqueCtx.fillRect(0,0,4,4); opaqueCtx.clearRect(0,0,4,4);
assert(bytes(opaqueCtx.getImageData(0,0,1,1).data) === '0,0,0,255', 'opaque clear is black');
opaque.width=4;
assert(opaqueCtx.getContextAttributes().alpha === false, 'resize preserves context settings');
return 'PASS';

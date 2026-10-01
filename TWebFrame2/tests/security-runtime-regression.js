function encode(value) {
    if (value === null) return 'null';
    if (typeof value !== 'object') return String(value);
    var result = '{', comma = '';
    for (var key in value) if (Object.prototype.hasOwnProperty.call(value, key)) {
        result += comma + key + ':' + encode(value[key]);
        comma = ',';
    }
    return result + '}';
}
function nestedBlocks() {
    {
        let retained = 42;
        {
            if (false) {}
        }
        return retained;
    }
}
var retained = nestedBlocks();
if (retained !== 42) throw new Error('nested block jump: ' + retained);
var encoded = encode({first: {a: 1, b: 2}, second: 3});
if (encoded !== '{first:{a:1,b:2},second:3}') throw new Error('recursive for-in: ' + encoded);
var caught = [];
for (var item of [1, 2, 3]) {
    try { throw item; } catch (error) {
        for (var property in {a: 1, b: 2}) {
            caught.push(item + property + error);
        }
        continue;
    }
}
if (caught.join(',') !== '1a1,1b1,2a2,2b2,3a3,3b3') throw new Error('catch iteration: ' + caught);
return 'PASS';

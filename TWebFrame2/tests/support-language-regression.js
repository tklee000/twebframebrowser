(function(){
    var results={};
    var pcre2=typeof __supportPcre2==='undefined'||__supportPcre2;
    function unsupported(pattern){try{new RegExp(pattern);return 'accepted';}catch(e){return e.name;}}
    function check(name, run){try{results[name]=run();}catch(e){results[name]='ERROR:'+e.name+':'+e.message;}}
    check('rest', function(){var [a,...rest]=[1,2,3];return a+'|'+rest.join(',');});
    check('restParameter', function(){function f([a,,...tail]){return a+'|'+tail.join(',');}return f([1,2,3,4]);});
    check('iterableRest', function(){var [a,...rest]=new Set([1,2,3]);return a+'|'+rest.join(',');});
    check('asyncName', function(){class A{async(){return 5;} static(){return 7;}}var a=new A();return a.async()+'|'+a.static();});
    check('missing', function(){try{return __corpusDefinitelyMissingName;}catch(e){return e.name;}});
    check('typeofMissing', function(){return typeof __corpusDefinitelyMissingName;});
    check('const', function(){const value=1;try{value=2;}catch(e){return e.name+'|'+value;}return value;});
    check('constIncrement', function(){const value=1;try{value++;}catch(e){return e.name+'|'+value;}return value;});
    check('constClosure', function(){const value=1;function set(){value=2;}try{set();}catch(e){return e.name+'|'+value;}return value;});
    check('constWarm', function(){const value=1;function set(){value++;}var errors=0;for(var i=0;i<40;i++){try{set();}catch(e){if(e.name==='TypeError')errors++;}}return errors+'|'+value;});
    check('constForOf', function(){var readers=[],errors=0;for(const value of [1,2,3]){readers.push(function(){return value;});try{value=9;}catch(e){if(e.name==='TypeError')errors++;}}return readers.map(function(f){return f();}).join(',')+'|'+errors;});
    check('super', function(){class A{constructor(v){this.v=v;}get(){return this.v;}}class B extends A{get(){return super.get()+1;}}return new B(4).get();});
    check('unicode', function(){return /^.$/u.test('\ud83d\ude00');});
    check('named', function(){return pcre2?new RegExp('(?<word>a)').exec('a').groups.word:unsupported('(?<word>a)');});
    check('lookbehind', function(){return pcre2?new RegExp('(?<=a)b').test('ab'):unsupported('(?<=a)b');});
    check('negativeLookbehind', function(){return pcre2?/(?<!a)b/.test('cb')+'|'+/(?<!a)b/.test('ab'):unsupported('(?<!a)b');});
    check('staticCaptures', function(){/(a)(b)/.test('ab');return RegExp.$1+'|'+RegExp.$2;});
    check('lastIndex', function(){var r=/a/g;return r.exec('aba').index+'|'+r.lastIndex+'|'+r.exec('aba').index+'|'+r.lastIndex+'|'+r.exec('aba')+'|'+r.lastIndex;});
    check('stringNamed', function(){return pcre2?'ab'.match(/(?<word>a)/).groups.word+'|'+'ab'.replace(/(?<word>a)/,'<$<word>>')+'|'+'ab'.search(/(?<=a)b/):'ab'.match(/(a)/)[1]+'|'+'ab'.replace(/(a)/,'<$1>')+'|'+'ab'.search(/b/);});
    check('unicodeMatches', function(){return '😀😀'.match(/./gu).length;});
    check('invalidRegex', function(){try{new RegExp('(');}catch(e){return e.name;}});
    check('regexIndices', function(){var m=(pcre2?/(?<a>x)(z)?/d:/(x)(z)?/d).exec('xx');return JSON.stringify([m.indices[0],pcre2?m.indices.groups.a:m.indices[1],m.indices[2]]);});
    check('regexPrototype', function(){return pcre2?RegExp.prototype.exec.call(/(?<a>x)/,'x').groups.a:RegExp.prototype.exec.call(/(x)/,'x')[1];});
    check('matchAllIterator', function(){var it='xx'.matchAll(/x/g);return it.next().value.index+'|'+it.next().value.index+'|'+it.next().done;});
    check('regexFlags', function(){return /alpha/i.test('ALPHA')&&/^b/m.test('a\nb')&&/a.b/s.test('a\nb')&&!/a.b/.test('a\nb')&&/\./s.test('.')&&!/\./s.test('\n')&&/[.]/s.test('.')&&!/[.]/s.test('\n');});
    check('regexContext', function(){var anchor=/^a/g,boundary=/\ba/g,multiline=/^a/gm,sticky=/a/y;anchor.lastIndex=1;boundary.lastIndex=1;multiline.lastIndex=2;sticky.lastIndex=1;var first=sticky.exec('ba');sticky.lastIndex=0;return anchor.exec('ba')===null&&boundary.exec('ba')===null&&multiline.exec('b\na').index===2&&first.index===1&&sticky.exec('ba')===null;});
    check('regexCaptureOffsets', function(){var r=/(a)(z)?/dg;r.lastIndex=1;var m=r.exec('ba');return JSON.stringify([m.index,r.lastIndex,m[1],m[2],m.indices,m.groups===undefined]);});
    check('regexInvalidFlags', function(){var count=0;['gg','ii','z'].forEach(function(flags){try{new RegExp('a',flags);}catch(e){if(e.name==='SyntaxError')count++;}});return count===3;});
    return JSON.stringify(results);
})()

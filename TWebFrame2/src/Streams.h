#pragma once

namespace TWebFrame::Internal {

// Realm-local stream algorithms use the interpreter's Promise job queue. Source
// and sink callbacks therefore run in their own realm, including worker realms.
inline constexpr wchar_t StreamBuiltins[] = LR"JS(
(function(scope){
    'use strict';
    var readableStates=new WeakMap(),writableStates=new WeakMap();
    function deferred(){var resolve,reject,promise=new Promise(function(a,b){resolve=a;reject=b;});return {promise:promise,resolve:resolve,reject:reject};}
    function state(map,value){var s=map.get(value);if(!s)throw new TypeError('Incompatible stream receiver');return s;}
    function invoke(owner,name,args){try{return Promise.resolve(typeof owner[name]==='function'?owner[name].apply(owner,args):undefined);}catch(error){return Promise.reject(error);}}
    function finishReads(s){if(!s.queue.length&&s.closing){s.closed=true;while(s.reads.length)s.reads.shift().resolve({value:undefined,done:true});if(s.reader)s.reader._closed.resolve(undefined);}}
    function failReads(s,error){if(s.closed||s.failed)return;s.failed=true;s.error=error;s.queue=[];while(s.reads.length)s.reads.shift().reject(error);if(s.reader)s.reader._closed.reject(error);}
    function pull(s){
        if(s.closed||s.failed||s.closing||!s.started||typeof s.source.pull!=='function')return;
        if(s.pulling){s.pullAgain=true;return;}if(!s.reads.length&&s.queue.length>=s.highWaterMark)return;
        s.pulling=true;invoke(s.source,'pull',[s.controller]).then(function(){s.pulling=false;if(s.pullAgain){s.pullAgain=false;pull(s);}},function(error){s.pulling=false;failReads(s,error);});
    }
    function ReadableStream(source,strategy){
        if(!(this instanceof ReadableStream))throw new TypeError('ReadableStream requires new');
        source=source||{};strategy=strategy||{};
        if(source.type!==undefined)throw new TypeError('Byte streams are not supported');
        var hwm=strategy.highWaterMark===undefined?1:Number(strategy.highWaterMark);
        if(hwm<0||isNaN(hwm))throw new RangeError('Invalid high water mark');
        var s={source:source,queue:[],reads:[],closing:false,closed:false,failed:false,reader:null,started:false,pulling:false,pullAgain:false,highWaterMark:hwm};
        readableStates.set(this,s);
        var controller={enqueue:function(chunk){
            if(s.closed||s.closing||s.failed)throw new TypeError('Stream is not readable');
            if(s.reads.length)s.reads.shift().resolve({value:chunk,done:false});else s.queue.push(chunk);pull(s);
        },close:function(){if(s.closed||s.closing||s.failed)throw new TypeError('Stream is not readable');s.closing=true;finishReads(s);},error:function(error){failReads(s,error);}};
        Object.defineProperty(controller,'desiredSize',{get:function(){return s.failed?null:s.closed?0:s.highWaterMark-s.queue.length;}});
        s.controller=controller;
        invoke(source,'start',[controller]).then(function(){s.started=true;pull(s);},function(error){failReads(s,error);});
    }
    Object.defineProperty(ReadableStream.prototype,'locked',{get:function(){return !!state(readableStates,this).reader;}});
    ReadableStream.prototype.getReader=function(options){
        var s=state(readableStates,this);if(s.reader)throw new TypeError('Stream is locked');
        if(options&&options.mode!==undefined)throw new TypeError('Unsupported reader mode');
        var reader={_closed:deferred(),read:function(){
            if(s.reader!==reader)return Promise.reject(new TypeError('Reader lock was released'));
            if(s.failed)return Promise.reject(s.error);
            if(s.queue.length){var value=s.queue.shift();finishReads(s);pull(s);return Promise.resolve({value:value,done:false});}
            if(s.closed)return Promise.resolve({value:undefined,done:true});
            var request=deferred();s.reads.push(request);pull(s);return request.promise;
        },cancel:function(reason){if(s.reader!==reader)return Promise.reject(new TypeError('Reader lock was released'));return cancel(s,reason);},releaseLock:function(){
            if(s.reader!==reader)return;s.reader=null;while(s.reads.length)s.reads.shift().reject(new TypeError('Reader lock was released'));
        }};
        Object.defineProperty(reader,'closed',{get:function(){return reader._closed.promise;}});
        // A rejection of reader.closed is observable when consumed, but does not
        // report a spurious unhandled rejection when the reader is discarded.
        reader._closed.promise.catch(function(){});s.reader=reader;
        if(s.failed)reader._closed.reject(s.error);else if(s.closed)reader._closed.resolve(undefined);
        return reader;
    };
    function cancel(s,reason){if(s.failed)return Promise.reject(s.error);if(s.closed)return Promise.resolve();s.queue=[];s.closing=true;finishReads(s);return invoke(s.source,'cancel',[reason]).then(function(){});}
    ReadableStream.prototype.cancel=function(reason){var s=state(readableStates,this);return s.reader?Promise.reject(new TypeError('Stream is locked')):cancel(s,reason);};
    function WritableStream(sink){
)JS"
LR"JS(
        if(!(this instanceof WritableStream))throw new TypeError('WritableStream requires new');sink=sink||{};
        var s={sink:sink,writer:null,closed:false,closing:false,failed:false,error:undefined,closedPromise:deferred()};
        writableStates.set(this,s);s.closedPromise.promise.catch(function(){});
        s.controller={error:function(error){s.failed=true;s.error=error;s.closedPromise.reject(error);}};
        s.tail=invoke(sink,'start',[s.controller]);s.tail.catch(function(error){s.failed=true;s.error=error;s.closedPromise.reject(error);});
    }
    Object.defineProperty(WritableStream.prototype,'locked',{get:function(){return !!state(writableStates,this).writer;}});
    function write(s,chunk){
        if(s.closed||s.closing||s.failed)return Promise.reject(s.failed?s.error:new TypeError('Stream is not writable'));
        var next=s.tail.then(function(){if(s.failed)throw s.error;return invoke(s.sink,'write',[chunk,s.controller]);});
        s.tail=next;next.catch(function(error){s.failed=true;s.error=error;s.closedPromise.reject(error);});return next;
    }
    function close(s){
        if(s.closed||s.closing||s.failed)return Promise.reject(s.failed?s.error:new TypeError('Stream is not writable'));s.closing=true;
        var next=s.tail.then(function(){if(s.failed)throw s.error;return invoke(s.sink,'close',[]);}).then(function(){s.closed=true;s.closedPromise.resolve(undefined);});
        s.tail=next;next.catch(function(error){s.failed=true;s.error=error;s.closedPromise.reject(error);});return next;
    }
    function abort(s,reason){if(s.closed)return Promise.resolve();if(s.failed)return Promise.reject(s.error);s.failed=true;s.error=reason;s.closedPromise.reject(reason);return s.tail.catch(function(){}).then(function(){return invoke(s.sink,'abort',[reason]);});}
    WritableStream.prototype.getWriter=function(){
        var s=state(writableStates,this);if(s.writer)throw new TypeError('Stream is locked');
        var writer={write:function(chunk){return s.writer===writer?write(s,chunk):Promise.reject(new TypeError('Writer lock was released'));},close:function(){return s.writer===writer?close(s):Promise.reject(new TypeError('Writer lock was released'));},abort:function(reason){return s.writer===writer?abort(s,reason):Promise.reject(new TypeError('Writer lock was released'));},releaseLock:function(){if(s.writer===writer)s.writer=null;}};
        Object.defineProperty(writer,'closed',{get:function(){return s.closedPromise.promise;}});
        Object.defineProperty(writer,'ready',{get:function(){return s.tail.then(function(){});}});
        Object.defineProperty(writer,'desiredSize',{get:function(){return s.failed?null:s.closed?0:1;}});s.writer=writer;return writer;
    };
    WritableStream.prototype.close=function(){var s=state(writableStates,this);return s.writer?Promise.reject(new TypeError('Stream is locked')):close(s);};
    WritableStream.prototype.abort=function(reason){var s=state(writableStates,this);return s.writer?Promise.reject(new TypeError('Stream is locked')):abort(s,reason);};
    ReadableStream.prototype.pipeTo=function(destination,options){
        var source=this,s;try{s=state(readableStates,source);state(writableStates,destination);}catch(error){return Promise.reject(error);}
        if(source.locked||destination.locked)return Promise.reject(new TypeError('Stream is locked'));
        options=options||{};var reader=source.getReader(),writer=destination.getWriter(),signal=options.signal,stopped=false;
        var completion=deferred();
        function release(){reader.releaseLock();writer.releaseLock();if(signal)signal.removeEventListener('abort',onAbort);}
        function fail(error,fromSource){
            if(stopped)return;stopped=true;
            var cleanup=fromSource?(options.preventAbort?Promise.resolve():writer.abort(error)):(options.preventCancel?Promise.resolve():reader.cancel(error));
            cleanup.catch(function(){}).then(function(){release();completion.reject(error);});
        }
        function onAbort(){if(stopped)return;stopped=true;var error=signal.reason===undefined?new Error('The operation was aborted'):signal.reason;
            Promise.all([options.preventAbort?Promise.resolve():writer.abort(error),options.preventCancel?Promise.resolve():reader.cancel(error)]).catch(function(){}).then(function(){release();completion.reject(error);});}
        function step(){if(stopped)return;reader.read().then(function(record){
            if(stopped)return;
            if(record.done){var closing=options.preventClose?Promise.resolve():writer.close();closing.then(function(){stopped=true;release();completion.resolve(undefined);},function(error){fail(error,false);});}
            else writer.write(record.value).then(step,function(error){fail(error,false);});
        },function(error){fail(error,true);});}
        if(signal){signal.addEventListener('abort',onAbort);if(signal.aborted)onAbort();}
        step();return completion.promise;
    };
    scope.ReadableStream=ReadableStream;scope.WritableStream=WritableStream;
})(globalThis);
)JS";

}

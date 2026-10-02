    Value RejectedPromise(const std::wstring& name,const std::wstring& message){
        auto promise=PromiseValue();RejectPromise(promise.object,ErrorValue(name,message));return promise;
    }
    Value BytesBlob(const std::vector<unsigned char>& bytes,const std::wstring& name=L"",double modified=0){
        auto blob=ObjectValue(name.empty()?ObjectKind::Plain:ObjectKind::File);
        blob.object->blobBytes.assign(bytes.begin(),bytes.end());blob.object->props[L"$blob"]=Value::Bool(true);
        blob.object->props[L"size"]=Value::Number(static_cast<double>(bytes.size()));blob.object->props[L"type"]=Value::String(L"");
        const auto constructor=global->values.find(name.empty()?L"Blob":L"File");
        if(constructor!=global->values.end()&&constructor->second.native)blob.object->prototype=constructor->second.native->props[L"prototype"].object;
        if(!name.empty()){blob.object->props[L"name"]=Value::String(name);blob.object->props[L"lastModified"]=Value::Number(modified);}
        return blob;
    }
    Value OriginFileHandle(const std::shared_ptr<OriginFileSystem>& fs,const std::shared_ptr<OriginFileEntry>& entry){
        auto handle=ObjectValue(ObjectKind::Plain);auto object=handle.object;
        const auto ctor=global->values.find(entry->directory?L"FileSystemDirectoryHandle":L"FileSystemFileHandle");
        if(ctor!=global->values.end())object->prototype=GetProperty(ctor->second,L"prototype").object;
        // Native state contains no JavaScript edges; wrapper/prototype ownership
        // remains visible to the common cycle collector.
        object->intlFormatter=entry;object->nativeStateKind=Object::NativeStateKind::FileEntry;object->props[L"name"]=Value::String(entry->name);
        object->props[L"kind"]=Value::String(entry->directory?L"directory":L"file");
        object->props[L"isSameEntry"]=Native([entry](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            const auto other=args.empty()?Value::Undefined():r.Deref(args[0]);
            return r.PromiseResolveValue(Value::Bool(other.object&&other.object->nativeStateKind==Object::NativeStateKind::FileEntry&&other.object->intlFormatter.get()==entry.get()));});
        for(const auto* method:{L"queryPermission",L"requestPermission"})object->props[method]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return r.PromiseResolveValue(Value::String(L"granted"));});
        if(entry->directory){
            for(const auto* method:{L"getFileHandle",L"getDirectoryHandle"}){
                const bool directory=std::wstring(method)==L"getDirectoryHandle";
                object->props[method]=Native([fs,entry,directory](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                    const auto name=args.empty()?L"":r.String(args[0]);
                    if(name.empty()||name==L"."||name==L".."||name.find_first_of(L"/\\")!=std::wstring::npos)
                        return r.RejectedPromise(L"TypeError",L"Invalid file name");
                    std::lock_guard<std::recursive_mutex> lock(fs->mutex);
                    if(entry->removed)return r.RejectedPromise(L"NotFoundError",L"Directory was removed");
                    auto found=entry->children.find(name);
                    if(found==entry->children.end()){
                        if(args.size()<2||!r.Truth(r.GetProperty(args[1],L"create")))return r.RejectedPromise(L"NotFoundError",L"Entry does not exist");
                        auto child=std::make_shared<OriginFileEntry>();child->directory=directory;child->name=name;child->parent=entry;
                        found=entry->children.emplace(name,child).first;
                        fs->Save();
                    }
                    if(found->second->directory!=directory)return r.RejectedPromise(L"TypeMismatchError",L"Entry has a different kind");
                    return r.PromiseResolveValue(r.OriginFileHandle(fs,found->second));
                });
            }
            object->props[L"removeEntry"]=Native([fs,entry](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                std::lock_guard<std::recursive_mutex> lock(fs->mutex);const auto name=args.empty()?L"":r.String(args[0]);
                const auto found=entry->children.find(name);if(found==entry->children.end())return r.RejectedPromise(L"NotFoundError",L"Entry does not exist");
                const auto child=found->second;
                std::function<bool(const std::shared_ptr<OriginFileEntry>&)> locked=[&](const auto& value){
                    if(value->locked)return true;for(const auto& nested:value->children)if(locked(nested.second))return true;return false;};
                if(locked(child))return r.RejectedPromise(L"NoModificationAllowedError",L"Entry is locked");
                if(child->directory&&!child->children.empty()&&(args.size()<2||!r.Truth(r.GetProperty(args[1],L"recursive"))))
                    return r.RejectedPromise(L"InvalidModificationError",L"Directory is not empty");
                std::function<void(const std::shared_ptr<OriginFileEntry>&)> remove=[&](const auto& value){
                    value->removed=true;for(const auto& nested:value->children)remove(nested.second);};
                remove(child);entry->children.erase(found);fs->Save();return r.PromiseResolveValue(Value::Undefined());
            });
            object->props[L"resolve"]=Native([entry](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                if(args.empty()||!args[0].object||args[0].object->nativeStateKind!=Object::NativeStateKind::FileEntry)return r.RejectedPromise(L"TypeError",L"Expected a file-system handle");
                auto other=std::static_pointer_cast<OriginFileEntry>(args[0].object->intlFormatter);std::vector<Value> parts;
                while(other&&other!=entry){parts.push_back(Value::String(other->name));other=other->parent.lock();}
                if(!other)return r.PromiseResolveValue(Value::Null());std::reverse(parts.begin(),parts.end());return r.PromiseResolveValue(r.ArrayValue(parts));
            });
            for(const auto* method:{L"entries",L"keys",L"values"}){
                const std::wstring mode=method;object->props[method]=Native([fs,entry,mode](RuntimeCore& r,const Value&,const std::vector<Value>&){
                    std::lock_guard<std::recursive_mutex> lock(fs->mutex);std::vector<Value> values;
                    for(const auto& child:entry->children){auto item=mode==L"keys"?Value::String(child.first):r.OriginFileHandle(fs,child.second);
                        if(mode==L"entries")item=r.ArrayValue({Value::String(child.first),item});values.push_back(item);}
                    auto iterator=r.ArrayValue(values);auto index=std::make_shared<size_t>(0);
                    iterator.object->props[L"next"]=r.ObjectNative(iterator.object,[index](RuntimeCore& r,const Value& receiver,const std::vector<Value>&){
                        auto result=r.ObjectValue(ObjectKind::Plain);const bool done=*index>=receiver.object->items.size();
                        result.object->props[L"done"]=Value::Bool(done);result.object->props[L"value"]=done?Value::Undefined():receiver.object->items[(*index)++];
                        return r.PromiseResolveValue(result);});
                    iterator.object->props[L"\uffffsymbol.wellKnown:asyncIterator"]=r.Native([](RuntimeCore&,const Value& receiver,const std::vector<Value>&){return receiver;});return iterator;
                });
            }
            object->props[L"\uffffsymbol.wellKnown:asyncIterator"]=object->props[L"entries"];
        }else{
            object->props[L"getFile"]=Native([fs,entry](RuntimeCore& r,const Value&,const std::vector<Value>&){
                std::lock_guard<std::recursive_mutex> lock(fs->mutex);
                return entry->removed?r.RejectedPromise(L"NotFoundError",L"File was removed"):r.PromiseResolveValue(r.BytesBlob(entry->bytes,entry->name,entry->modified));});
            for(const auto* method:{L"createWritable",L"createSyncAccessHandle"}){
                const bool sync=std::wstring(method)==L"createSyncAccessHandle";
                object->props[method]=Native([fs,entry,sync](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                    if(sync&&!r.workerGlobal.object)return r.RejectedPromise(L"InvalidStateError",L"Synchronous file access requires a dedicated worker");
                    std::lock_guard<std::recursive_mutex> lock(fs->mutex);
                    if(entry->removed)return r.RejectedPromise(L"NotFoundError",L"File was removed");
                    if(entry->locked)return r.RejectedPromise(L"NoModificationAllowedError",L"File is locked");entry->locked=true;
                    struct Lease {
                        std::shared_ptr<OriginFileSystem> fs;std::shared_ptr<OriginFileEntry> entry;
                        bool closed=false;size_t position=0;std::vector<unsigned char> staging;
                        ~Lease(){std::lock_guard<std::recursive_mutex> lock(fs->mutex);if(!closed)entry->locked=false;}
                    };
                    auto lease=std::make_shared<Lease>();lease->fs=fs;lease->entry=entry;
                    if(sync||(args.size()&&r.Truth(r.GetProperty(args[0],L"keepExistingData"))))lease->staging=entry->bytes;
                    auto stream=r.ObjectValue(ObjectKind::Plain);stream.object->intlFormatter=lease;
                    const auto complete=[sync](RuntimeCore& r,Value result){return sync?result:r.PromiseResolveValue(result);};
                    const auto failure=[sync](RuntimeCore& r,const wchar_t* name,const wchar_t* message){return sync?Value::Thrown(r.ErrorValue(name,message)):r.RejectedPromise(name,message);};
                    for(const auto* operation:{L"write",L"read",L"truncate",L"getSize",L"flush",L"close",L"abort",L"seek"}){
                        const std::wstring op=operation;
                        if(!sync&&(op==L"read"||op==L"getSize"||op==L"flush"))continue;
                        if(sync&&(op==L"abort"||op==L"seek"))continue;
                        stream.object->props[op]=r.Native([lease,sync,op,complete,failure](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                            std::lock_guard<std::recursive_mutex> lock(lease->fs->mutex);
                            if(lease->closed)return failure(r,L"InvalidStateError",L"File handle is closed");
                            auto& bytes=sync?lease->entry->bytes:lease->staging;
                            if(op==L"getSize")return complete(r,Value::Number(static_cast<double>(bytes.size())));
                            if(op==L"close"||op==L"abort"){
                                if(!sync&&op==L"close"){
                                    if(lease->fs->Usage(lease->fs->root)-lease->entry->bytes.size()+lease->staging.size()>OriginFileSystem::quota)return failure(r,L"QuotaExceededError",L"Origin file quota exceeded");
                                    lease->entry->bytes=lease->staging;}
                                if(op==L"close")lease->entry->modified=std::chrono::duration<double,std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
                                lease->entry->locked=false;lease->closed=true;if(op==L"close")lease->fs->Save();return complete(r,Value::Undefined());
                            }
                            if(op==L"flush"){lease->fs->Save();return complete(r,Value::Undefined());}
                            Value data=args.empty()?Value::Undefined():r.Deref(args[0]);std::wstring command=op;
                            double offset=static_cast<double>(lease->position);
                            if(sync&&args.size()>1){const auto at=r.GetProperty(args[1],L"at");if(at.type!=Value::Type::Undefined)offset=r.Number(at);}
                            if(!sync&&data.object&&!data.object->props.count(L"$blob")&&!data.object->props.count(L"$typedArrayBits")&&!data.object->props.count(L"$arrayBuffer")){
                                command=r.String(r.GetProperty(data,L"type"));
                                if(command==L"write"){const auto position=r.GetProperty(data,L"position");if(position.type!=Value::Type::Undefined)offset=r.Number(position);data=r.GetProperty(data,L"data");}
                                else data=r.GetProperty(data,command==L"seek"?L"position":L"size");
                            }
                            if(command==L"seek"||command==L"truncate")offset=r.Number(data);
                            if(!std::isfinite(offset)||offset<0||std::floor(offset)!=offset||offset>OriginFileSystem::quota)
                                return failure(r,L"TypeError",L"Invalid file offset");
                            const auto position=static_cast<size_t>(offset);
                            if(command==L"seek"){lease->position=position;return complete(r,Value::Undefined());}
                            if(command==L"truncate"){
                                if(lease->fs->Usage(lease->fs->root)-lease->entry->bytes.size()+position>OriginFileSystem::quota)return failure(r,L"QuotaExceededError",L"Origin file quota exceeded");
                                bytes.resize(position);lease->position=std::min(lease->position,position);if(sync)lease->fs->Save();return complete(r,Value::Undefined());
                            }
                            std::vector<unsigned char> input;
                            if(command==L"read"){
                                if(!r.SnapshotBufferSource(data,input))return failure(r,L"TypeError",L"read requires a BufferSource");
                                const size_t count=position>=bytes.size()?0:std::min(input.size(),bytes.size()-position);
                                for(size_t i=0;i<count;++i)r.WriteBufferBits(data.object,i,1,bytes[position+i]);lease->position=position+count;
                                return Value::Number(static_cast<double>(count));
                            }
                            if(command!=L"write")return failure(r,L"TypeError",L"Invalid write command");
                            if(!r.SnapshotBufferSource(data,input)){
                                const auto text=data.object&&data.object->props.count(L"$blob")?data.object->blobBytes:WideToUtf8(r.String(data));input.assign(text.begin(),text.end());}
                            const size_t newSize=std::max(bytes.size(),position+input.size());
                            if(newSize>OriginFileSystem::quota||lease->fs->Usage(lease->fs->root)-lease->entry->bytes.size()+newSize>OriginFileSystem::quota)
                                return failure(r,L"QuotaExceededError",L"Origin file quota exceeded");
                            bytes.resize(newSize);std::copy(input.begin(),input.end(),bytes.begin()+position);lease->position=position+input.size();
                            if(sync)lease->fs->Save();
                            return complete(r,sync?Value::Number(static_cast<double>(input.size())):Value::Undefined());
                        });
                    }
                    return r.PromiseResolveValue(stream);
                });
            }
        }
        return handle;
    }

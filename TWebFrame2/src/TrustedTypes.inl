    Value TrustedValue(const std::wstring& kind,const Value& input){
        auto value=ObjectValue(ObjectKind::Plain);value.object->props[L"$host:trustedKind"]=Value::String(kind);
        value.object->nativeStateKind=kind==L"TrustedHTML"?Object::NativeStateKind::TrustedHTML:kind==L"TrustedScript"?Object::NativeStateKind::TrustedScript:Object::NativeStateKind::TrustedScriptURL;
        value.object->props[L"$primitive"]=Value::String(String(input));
        const auto ctor=global->values.find(kind);if(ctor!=global->values.end())value.object->prototype=GetProperty(ctor->second,L"prototype").object;
        value.object->props[L"toString"]=ObjectNative(value.object,[](RuntimeCore&,const Value& receiver,const std::vector<Value>&){return receiver.object->props[L"$primitive"];});
        value.object->props[L"toJSON"]=value.object->props[L"toString"];return value;
    }
    void InstallTrustedTypes(){
        for(const auto* name:{L"TrustedHTML",L"TrustedScript",L"TrustedScriptURL"}){
            auto ctor=Native([](RuntimeCore& r,const Value&,const std::vector<Value>&){return Value::Thrown(r.ErrorValue(L"TypeError",L"Illegal constructor"));});
            ctor.native->props[L"name"]=Value::String(name);ctor.native->props[L"prototype"]=ObjectValue(ObjectKind::Plain);
            global->values[name]=ctor;GlobalWindowObject()->props[name]=ctor;
        }
        auto factory=ObjectValue(ObjectKind::Plain);auto policies=ObjectValue(ObjectKind::Plain);
        factory.object->props[L"$host:policies"]=policies;factory.object->props[L"defaultPolicy"]=Value::Null();
        factory.object->props[L"createPolicy"]=ObjectNative(factory.object,[](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
            if(args.size()<2)return Value::Thrown(r.ErrorValue(L"TypeError",L"createPolicy requires a name and rules"));
            const auto name=r.String(args[0]);auto& policies=receiver.object->props[L"$host:policies"].object->props;
            if(policies.count(name))return Value::Thrown(r.ErrorValue(L"TypeError",L"A policy with that name already exists"));
            auto policy=r.ObjectValue(ObjectKind::Plain);policy.object->props[L"name"]=Value::String(name);policy.object->props[L"$host:rules"]=r.Deref(args[1]);
            for(const auto& pair:std::vector<std::pair<std::wstring,std::wstring>>{{L"createHTML",L"TrustedHTML"},{L"createScript",L"TrustedScript"},{L"createScriptURL",L"TrustedScriptURL"}}){
                const auto callback=r.GetProperty(args[1],pair.first);policy.object->props[L"$host:"+pair.first]=callback;
                policy.object->props[pair.first]=r.ObjectNative(policy.object,[method=pair.first,kind=pair.second](RuntimeCore& r,const Value& receiver,const std::vector<Value>& args){
                    const auto callback=receiver.object->props[L"$host:"+method];
                    if(!r.IsCallable(callback))return Value::Thrown(r.ErrorValue(L"TypeError",L"Policy has no rule for this type"));
                    auto forwarded=args;if(forwarded.empty())forwarded.push_back(Value::String(L"undefined"));else forwarded[0]=Value::String(r.String(forwarded[0]));
                    const auto result=r.Call(callback,receiver,forwarded);return r.TrustedValue(kind,result.type==Value::Type::Null||result.type==Value::Type::Undefined?Value::String(L""):result);
                });
            }
            policies[name]=policy;if(name==L"default")receiver.object->props[L"defaultPolicy"]=policy;return policy;
        });
        for(const auto& pair:std::vector<std::pair<std::wstring,std::wstring>>{{L"isHTML",L"TrustedHTML"},{L"isScript",L"TrustedScript"},{L"isScriptURL",L"TrustedScriptURL"}})
            factory.object->props[pair.first]=Native([kind=pair.second](RuntimeCore& r,const Value&,const std::vector<Value>& args){
                const auto value=args.empty()?Value::Undefined():r.Deref(args[0]);const auto expected=kind==L"TrustedHTML"?Object::NativeStateKind::TrustedHTML:kind==L"TrustedScript"?Object::NativeStateKind::TrustedScript:Object::NativeStateKind::TrustedScriptURL;
                return Value::Bool(value.object&&value.object->nativeStateKind==expected);});
        factory.object->props[L"emptyHTML"]=TrustedValue(L"TrustedHTML",Value::String(L""));factory.object->props[L"emptyScript"]=TrustedValue(L"TrustedScript",Value::String(L""));
        factory.object->props[L"getAttributeType"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.size()<2)return Value::Null();const auto tag=ToLower(r.String(args[0])),attribute=ToLower(r.String(args[1]));
            if(attribute.rfind(L"on",0)==0)return Value::String(L"TrustedScript");
            if(tag==L"iframe"&&attribute==L"srcdoc")return Value::String(L"TrustedHTML");
            if(tag==L"script"&&attribute==L"src")return Value::String(L"TrustedScriptURL");return Value::Null();});
        factory.object->props[L"getPropertyType"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            if(args.size()<2)return Value::Null();const auto tag=ToLower(r.String(args[0])),property=r.String(args[1]);
            if(property==L"innerHTML"||property==L"outerHTML"||(tag==L"iframe"&&property==L"srcdoc"))return Value::String(L"TrustedHTML");
            if(tag==L"script"&&(property==L"text"||property==L"textContent"||property==L"innerText"))return Value::String(L"TrustedScript");
            if(tag==L"script"&&property==L"src")return Value::String(L"TrustedScriptURL");return Value::Null();});
        global->values[L"trustedTypes"]=factory;GlobalWindowObject()->props[L"trustedTypes"]=factory;
    }
    static std::vector<std::wstring> PolicyFeatures(){return {L"accelerometer",L"autoplay",L"camera",L"clipboard-read",L"clipboard-write",L"display-capture",L"encrypted-media",L"fullscreen",L"geolocation",L"gyroscope",L"magnetometer",L"microphone",L"midi",L"payment",L"picture-in-picture",L"publickey-credentials-get",L"screen-wake-lock",L"usb"};}
    bool PolicyAllows(const std::wstring& feature,const std::wstring& origin){
        const auto features=PolicyFeatures();if(std::find(features.begin(),features.end(),feature)==features.end())return false;
        if(origin!=CurrentOrigin()&&feature!=L"autoplay"&&feature!=L"fullscreen"&&feature!=L"picture-in-picture")return false;
        const auto frame=embeddingFrame.lock();if(!frame)return true;
        const auto parent=embeddingParent.lock();if(parent&&parent->core&&!parent->core->PolicyAllows(feature,origin))return false;
        const auto allow=frame->Attribute(L"allow");std::wistringstream directives(allow);std::wstring directive;
        while(std::getline(directives,directive,L';')){std::wistringstream tokens(directive);std::wstring name,token;tokens>>name;
            if(name!=feature)continue;if(!(tokens>>token))return true;if(token==L"'none'")return false;
            do{if(token==L"*"||(token==L"'src'"&&origin==CurrentOrigin())||token==origin||
                (token==L"'self'"&&parent&&parent->core&&origin==parent->core->CurrentOrigin()))return true;}while(tokens>>token);return false;}
        return !parent||!parent->core||CurrentOrigin()==parent->core->CurrentOrigin()||feature==L"autoplay"||feature==L"picture-in-picture";
    }
    void InstallFeaturePolicy(){
        auto policy=ObjectValue(ObjectKind::Plain);
        policy.object->props[L"allowsFeature"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            return Value::Bool(!args.empty()&&r.PolicyAllows(r.String(args[0]),args.size()>1?r.String(args[1]):r.CurrentOrigin()));});
        for(const auto* method:{L"features",L"allowedFeatures"}){const bool allowed=std::wstring(method)==L"allowedFeatures";
            policy.object->props[method]=Native([allowed](RuntimeCore& r,const Value&,const std::vector<Value>&){std::vector<Value> features;
                for(const auto& feature:r.PolicyFeatures())if(!allowed||r.PolicyAllows(feature,r.CurrentOrigin()))features.push_back(Value::String(feature));return r.ArrayValue(features);});}
        policy.object->props[L"getAllowlistForFeature"]=Native([](RuntimeCore& r,const Value&,const std::vector<Value>& args){
            return r.ArrayValue(!args.empty()&&r.PolicyAllows(r.String(args[0]),r.CurrentOrigin())?std::vector<Value>{Value::String(r.CurrentOrigin())}:std::vector<Value>{});});
        global->values[L"document"].object->props[L"featurePolicy"]=policy;
    }

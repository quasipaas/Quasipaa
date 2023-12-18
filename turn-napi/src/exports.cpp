//
//  turn-napi.cpp
//  turn-napi
//
//  Created by Mr.Panda on 2023/12/16.
//

#include <condition_variable>
#include <future>

#include "exports.h"

void run_promise(Napi::Function& async_func,
                 const std::vector<Napi::Value>& args,
                 std::function<void(const Napi::Value&)> resolve,
                 std::function<void(const Napi::Error&)> reject)
{
    auto env = async_func.Env();
    Napi::Object promise = async_func.Call(args).As<Napi::Object>();
    Napi::Function then_func = promise.Get("then").As<Napi::Function>();
    Napi::Function catch_func = promise.Get("catch").As<Napi::Function>();

    then_func.Call(promise, { Napi::Function::New(env,
                                                  [=](const Napi::CallbackInfo& info)
                                                  {
                                                      resolve(info[0].As<Napi::Value>());
                                                  }) });

    catch_func.Call(promise, { Napi::Function::New(env,
                                                   [=](const Napi::CallbackInfo& info)
                                                   {
                                                       reject(info[0].As<Napi::Error>());
                                                   }) });
}

bool args_checker(const Napi::CallbackInfo& info, std::vector<JsTypes> types)
{
#define IF_TYPE(TYPE) \
    if (types[i] == JsTypes::TYPE) { \
        if (!info[i].Is##TYPE()) { \
            return false; \
        } \
    }

    auto size = info.Length();
    if (size != types.size())
    {
        return false;
    }
    else
    {
        for (int i = 0; i < size; i++)
        {
            IF_TYPE(String)
                IF_TYPE(Number)
                IF_TYPE(Boolean)
                IF_TYPE(Object)
                IF_TYPE(Array)
                IF_TYPE(Buffer)
        }

        return true;
    }
}

void throw_as_javascript_exception(Napi::Env& env, std::string message)
{
    Napi::TypeError::New(env, message).ThrowAsJavaScriptException();
}

/* NapiTurnObserver */

NapiTurnObserver::NapiTurnObserver(Napi::ObjectReference observer)
{
    _observer.Reset(observer.Value());
    _observer.Ref();
}

NapiTurnObserver::~NapiTurnObserver()
{
    _observer.Unref();
}

void NapiTurnObserver::GetPassword(std::string& addr,
                                   std::string& name,
                                   std::function<void(std::optional<std::string>)> callback) const
{
    Napi::Env env = _observer.Env();
    Napi::Function func = _observer.Get("get_password").As<Napi::Function>();
    run_promise(func,
                { Napi::String::New(env, addr), Napi::String::New(env, name) },
                [=](const Napi::Value& value)
                {
                    callback(value.IsNull()
                             ? std::nullopt
                             : std::optional(value.As<Napi::String>().Utf8Value()));
                },
                [=](const Napi::Error& _error)
                {
                    callback(std::nullopt);
                });
}

/* NapiTurnProcesser::ProcessAsyncWorker */

NapiTurnProcesser::ProcessAsyncWorker::ProcessAsyncWorker(const Napi::Env& env,
                                                          TurnProcessor* processer,
                                                          std::string addr,
                                                          uint8_t* buf,
                                                          size_t buf_size)
    : Napi::AsyncWorker(env)
    , _deferred(Napi::Promise::Deferred(env))
    , _processer(processer)
    , _addr(addr)
    , _buf(buf)
    , _buf_size(buf_size)
{
}

void NapiTurnProcesser::ProcessAsyncWorker::Execute()
{
    std::promise<std::shared_ptr<TurnProcessor::Results>> promise;
    auto future = promise.get_future();

    _processer->Process(_buf,
                        _buf_size,
                        _addr,
                        [&](std::shared_ptr<TurnProcessor::Results> ret)
                        {
                            promise.set_value(ret);
                        });

    _result = future.get();
    if (_result == nullptr || _result->Ret->is_success)
    {
        return;
    }

    SetError(stun_err_into_str(_result->Ret->result.error));
}

Napi::Promise NapiTurnProcesser::ProcessAsyncWorker::GetPromise()
{
    return _deferred.Promise();
}

void NapiTurnProcesser::ProcessAsyncWorker::OnOK()
{
    Napi::Env env = Env();

    if (_result == nullptr)
    {
        _deferred.Resolve(env.Null());
        return;
    }

    Napi::Object response = Napi::Object::New(env);
    auto tresponse = _result->Ret->result.response;

    response.Set("interface", tresponse.interface == nullptr ? env.Null() : Napi::String::New(env, tresponse.interface));
    response.Set("relay", tresponse.relay == nullptr ? env.Null() : Napi::String::New(env, tresponse.relay));
    response.Set("data", Napi::Buffer<uint8_t>::NewOrCopy(env, tresponse.data, tresponse.data_len));
    response.Set("kind", Napi::String::New(env, stun_class_into_str(tresponse.kind)));
    _deferred.Resolve(response);
}

void NapiTurnProcesser::ProcessAsyncWorker::OnError(const Napi::Error& err)
{
    _deferred.Reject(err.Value());
}

/* NapiTurnProcesser */

Napi::Object NapiTurnProcesser::CreateInstance(Napi::Env env, TurnProcessor* processor)
{
    Napi::Function func = DefineClass(env,
                                      "TurnProcesser",
                                      { InstanceMethod("process", &NapiTurnProcesser::Process) });
    Napi::FunctionReference* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);

    Napi::External<TurnProcessor> processer_ = Napi::External<TurnProcessor>::New(env, processor);
    return constructor->New({ processer_ });
}

NapiTurnProcesser::NapiTurnProcesser(const Napi::CallbackInfo& info) : Napi::ObjectWrap<NapiTurnProcesser>(info)
{
    Napi::Env env = info.Env();

    if (info.Length() != 1 || !info[0].IsExternal())
    {
        throw_as_javascript_exception(env, "Wrong arguments");
        return;
    }

    Napi::External<TurnProcessor> external = info[0].As<Napi::External<TurnProcessor>>();
    _processor = external.Data();
}

NapiTurnProcesser::~NapiTurnProcesser()
{
    if (_processor != nullptr)
    {
        delete _processor;
    }
}

Napi::Value NapiTurnProcesser::Process(const Napi::CallbackInfo& info)
{
    Napi::Env env = info.Env();

    if (!args_checker(info, { JsTypes::Buffer, JsTypes::String }))
    {
        throw_as_javascript_exception(env, "Wrong arguments");
        return env.Null();
    }

    if (_processor == nullptr)
    {
        return env.Null();
    }

    Napi::Buffer<uint8_t> buffer = info[0].As<Napi::Buffer<uint8_t>>();
    std::string addr = info[1].As<Napi::String>().Utf8Value();
    ProcessAsyncWorker* worker = new ProcessAsyncWorker(env,
                                                        _processor,
                                                        std::move(addr),
                                                        buffer.Data(),
                                                        buffer.Length());
    worker->Queue();
    return worker->GetPromise();
}

/* NapiTurnService */

Napi::Object NapiTurnService::Init(Napi::Env env, Napi::Object exports)
{
    Napi::Function func = DefineClass(env,
                                      "TurnService",
                                      { InstanceMethod("get_processer", &NapiTurnService::GetProcesser) });
    Napi::FunctionReference* constructor = new Napi::FunctionReference();
    *constructor = Napi::Persistent(func);
    env.SetInstanceData(constructor);
    exports.Set("TurnService", func);
    return exports;
}

NapiTurnService::NapiTurnService(const Napi::CallbackInfo& info) : Napi::ObjectWrap<NapiTurnService>(info)
{
    Napi::Env env = info.Env();

    if (!args_checker(info, { JsTypes::String, JsTypes::Array, JsTypes::Object }))
    {
        throw_as_javascript_exception(env, "Wrong arguments");
        return;
    }

    std::string realm = info[0].As<Napi::String>().Utf8Value();
    Napi::Array externals = info[1].As<Napi::Array>();
    Napi::ObjectReference observer = Napi::Persistent(info[2].As<Napi::Object>());

    std::vector<std::string> externals_;
    for (size_t i = 0; i < externals.Length(); i++)
    {
        externals_.push_back(externals.Get(i).As<Napi::String>().Utf8Value());
    }

    _observer = std::make_shared<NapiTurnObserver>(std::move(observer));
    _servive = TurnService::Create(realm, externals_, _observer.get());
    if (_servive == nullptr)
    {
        throw_as_javascript_exception(env, "Failed to create turn service");
    }
}

Napi::Value NapiTurnService::GetProcesser(const Napi::CallbackInfo& info)
{
    Napi::Env env = info.Env();

    if (!args_checker(info, { JsTypes::String, JsTypes::String }))
    {
        throw_as_javascript_exception(env, "Wrong arguments");
        return env.Null();
    }

    std::string interface = info[0].As<Napi::String>().Utf8Value();
    std::string external = info[1].As<Napi::String>().Utf8Value();
    TurnProcessor* processor = _servive->GetProcessor(interface, external);
    if (processor == nullptr)
    {
        throw_as_javascript_exception(env, "Failed to get turn processer");
        return env.Null();
    }

    return NapiTurnProcesser::CreateInstance(info.Env(), processor);
}

Napi::Object Init(Napi::Env env, Napi::Object exports)
{
    return NapiTurnService::Init(env, exports);
}

NODE_API_MODULE(turn, Init);

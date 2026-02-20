/*
 * Authored by Alex Hultman, 2018-2021.
 * Intellectual property of third-party.

 * Modified for Akeno: copyright (c) 2026 Lukas Zloch (https://lstv.space)

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "akeno/DomainHandler.h"
#include "akeno/App.h"
#include <v8.h>
#include "Utilities.h"
#include <memory>
#include <functional>
#include <utility>
#include <filesystem>

// todo
#include "akeno/WebApp.h"

using namespace v8;

static int kWebAppTag;

inline v8::Local<v8::String>
utf8(v8::Isolate* isolate, std::string_view sv) {
    return v8::String::NewFromUtf8(
        isolate,
        sv.data(),
        v8::NewStringType::kNormal,
        static_cast<int>(sv.size())
    ).ToLocalChecked();
}

/**
 * WARNING: the following code is mostly still a prototype.
 */

/* Helper for query decoding for resolve() logic, when needed */
std::string decodeURIComponent(std::string_view url) {
    std::string decoded;
    decoded.reserve(url.length());
    for (size_t i = 0; i < url.length(); ++i) {
        if (url[i] == '%' && i + 2 < url.length()) {
            char key[3] = {url[i + 1], url[i + 2], '\0'};
            char *end;
            unsigned long value = strtoul(key, &end, 16);
            if (end == key + 2) {
                decoded += (char)value;
                i += 2;
            } else {
                decoded += url[i];
            }
        } else {
            decoded += url[i];
        }
    }
    return decoded;
}

inline v8::Local<v8::String>
oneByte(v8::Isolate* isolate, std::string_view sv) {
    return v8::String::NewFromOneByte(
        isolate,
        reinterpret_cast<const uint8_t*>(sv.data()),
        v8::NewStringType::kNormal,
        static_cast<int>(sv.size())
    ).ToLocalChecked();
}

struct ReqKeys {
    v8::Eternal<v8::String> method, origin, secure, host, domain, path, contentType, contentLength;
};

ReqKeys& getReqKeys(v8::Isolate* isolate);

template <bool SSL>
static inline void initReqResObjects(PerContextData *perContextData, uWS::HttpResponse<SSL> *res, uWS::HttpRequest *req, Local<Object> *reqObjectOut, Local<Object> *resObjectOut) {
    Isolate *isolate = perContextData->isolate;
    Local<Context> context = isolate->GetCurrentContext();

    Local<Object> reqObject = perContextData->reqTemplate[0].Get(isolate)->Clone();
    reqObject->SetAlignedPointerInInternalField(0, req);

    Local<Object> resObject = perContextData->resTemplate[SSL ? 1 : 0].Get(isolate)->Clone();
    resObject->SetAlignedPointerInInternalField(0, res);

    std::string_view method = req->getCaseSensitiveMethod();
    std::string_view url = req->getUrl();
    std::string_view host = req->getHeader("host");
    std::string_view domain = host;
    size_t colonPos = host.find(':');
    if (colonPos != std::string_view::npos) {
        domain = host.substr(0, colonPos);
    }

    ReqKeys &keys = getReqKeys(isolate);

    std::string_view origin = req->getHeader("origin");
    reqObject->Set(context, keys.method.Get(isolate), oneByte(isolate, method)).Check();
    reqObject->Set(context, keys.origin.Get(isolate), oneByte(isolate, origin)).Check();
    reqObject->Set(context, keys.secure.Get(isolate), Boolean::New(isolate, SSL)).Check();
    reqObject->Set(context, keys.host.Get(isolate), oneByte(isolate, host)).Check();
    reqObject->Set(context, keys.domain.Get(isolate), oneByte(isolate, domain)).Check();

    if (url.find('%') != std::string_view::npos) {
        std::string decoded = decodeURIComponent(url);
        reqObject->Set(context, keys.path.Get(isolate),
                    oneByte(isolate, decoded))
            .Check();
    } else {
        reqObject->Set(context, keys.path.Get(isolate),
                    oneByte(isolate, url))
            .Check();
    }

    if (method == "POST" ||
        method == "PUT" ||
        method == "PATCH" ||
        method == "DELETE") {
        std::string_view ct = req->getHeader("content-type");
        reqObject->Set(context, keys.contentType.Get(isolate),
            oneByte(isolate, ct)).Check();

        std::string_view cl = req->getHeader("content-length");
        reqObject->Set(context, keys.contentLength.Get(isolate),
            oneByte(isolate, cl)).Check();
    }

    *reqObjectOut = reqObject;
    *resObjectOut = resObject;
}

/* App wrapper functions — protocol-agnostic */

/* app.route(pattern, handler) — adds a domain route. */
/* TODO: This NEEDS cleanup; the current code is mostly a PoC */
void uWS_App_route(const FunctionCallbackInfo<Value> &args) {
    uWS::App *app = (uWS::App *) args.This()->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();

    /* pattern, handler */
    if (missingArguments(2, args)) {
        return;
    }

    NativeString pattern(isolate, args[0]);
    if (pattern.isInvalid(args)) {
        return;
    }

    std::string patternStr(pattern.getString());

    /* If the handler is null, unroute */
    if (args[1]->IsNull() || args[1]->IsUndefined()) {
        app->unroute(patternStr);
        args.GetReturnValue().Set(args.This());
        return;
    }

    DomainHandler handler;

    // TODO: Support DeclarativeResponse
    if (args[1]->IsArrayBuffer()) {
        NativeString staticBuf(isolate, args[1]);
        if (staticBuf.isInvalid(args)) {
            return;
        }

        std::string staticBufStr = std::string(staticBuf.getString());

        handler = DomainHandler::fromStaticBuffer(staticBufStr);

        app->route(patternStr, std::move(handler));
        args.GetReturnValue().Set(args.This());
        return;
    }

    if (args[1]->IsFunction()) {
        Callback checkedCallback(args.GetIsolate(), args[1]);
        if (checkedCallback.isInvalid(args)) return;

        /* This function requires perContextData */
        auto* perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

        // Use shared_ptr to allow both HTTP and HTTPS lambdas to share the Global<Function>
        auto cbPtr = std::make_shared<Global<Function>>(checkedCallback.getFunction());

        // TODO: Optimize calls

        // Create a unified template lambda that works with both HTTP and HTTPS (C++20)
        auto sharedHandler = [cbPtr, perContextData]<bool SSL>(uWS::HttpResponse<SSL> *res, uWS::HttpRequest *req) {
            Isolate *isolate = perContextData->isolate;
            HandleScope hs(isolate);
            Local<Object> reqObject;
            Local<Object> resObject;
            initReqResObjects<SSL>(perContextData, res, req, &reqObject, &resObject);

            // IMPORTANT NOTE: We switched to the more common order "req, res" in contrast to the reverse order that µWS uses.
            // This is to align with how most other frameworks work, but it is something to keep in mind - Akeno-uWS differs from the uWS API.
            Local<Value> argv[] = {reqObject, resObject};
            CallJS(isolate, cbPtr->Get(isolate), 2, argv);

            // Invalidate request
            reqObject->SetAlignedPointerInInternalField(0, nullptr);
        };

        // Instantiate the template lambda for both HTTP and HTTPS
        handler = DomainHandler::onRequestBoth(
            [sharedHandler](uWS::HttpResponse<false> *res, uWS::HttpRequest *req) {
                sharedHandler.template operator()<false>(res, req);
            },
            [sharedHandler](uWS::HttpResponse<true> *res, uWS::HttpRequest *req) {
                sharedHandler.template operator()<true>(res, req);
            }
        );
    } else if (args[1]->IsObject()) {
        Local<Object> handlerObject = Local<Object>::Cast(args[1]);

        /* Fast-path: WebApp wrapper object (routes through C++ WebServer) */
        if (handlerObject->InternalFieldCount() >= 2 &&
            handlerObject->GetAlignedPointerFromInternalField(1) == (void *)&kWebAppTag) {

            Akeno::WebApp *webAppPtr = (Akeno::WebApp *) handlerObject->GetAlignedPointerFromInternalField(0);
            if (!webAppPtr) {
                std::cerr << "Warning: Attempted to route to a WebApp with a null pointer. Make sure your WebApp wrapper object is valid and properly initialized. See documentation for app.registerWebApp and consult the user manual." << std::endl;
                args.GetReturnValue().Set(args.This());
                return;
            }

            /* This function requires perContextData */
            auto *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();
            auto it = perContextData->webAppsByPtr.find(webAppPtr);
            if (it == perContextData->webAppsByPtr.end()) {
                std::cerr << "Warning: Attempted to route to a WebApp that is not registered. Make sure to register your WebApp using app.registerWebApp() before routing to it. See documentation for app.registerWebApp and consult the user manual." << std::endl;
                args.GetReturnValue().Set(args.This());
                return;
            }

            handler = DomainHandler::fromWebApp(it->second);
            app->route(patternStr, std::move(handler));
            args.GetReturnValue().Set(args.This());
            return;
        }

        /* This function requires perContextData */
        auto* perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();
        auto &callbackPtr = perContextData->appObjectCallbacks[app];
        if (!callbackPtr) {
            callbackPtr = std::make_shared<Global<Function>>();
        }

        auto objectPtr = std::make_shared<Global<Object>>();
        objectPtr->Reset(isolate, Local<Object>::Cast(args[1]));

        auto sharedHandler = [objectPtr, callbackPtr, perContextData]<bool SSL>(uWS::HttpResponse<SSL> *res, uWS::HttpRequest *req) {
            if (!callbackPtr || callbackPtr->IsEmpty()) {
                res->end();
                return;
            }

            Isolate *isolate = perContextData->isolate;
            HandleScope hs(isolate);
            Local<Object> reqObject;
            Local<Object> resObject;
            initReqResObjects<SSL>(perContextData, res, req, &reqObject, &resObject);
            Local<Function> onObjectLf = Local<Function>::New(isolate, *callbackPtr);
            Local<Object> objectValue = Local<Object>::New(isolate, *objectPtr);
            Local<Value> argv[] = {reqObject, resObject, objectValue};
            CallJS(isolate, onObjectLf, 3, argv);

            reqObject->SetAlignedPointerInInternalField(0, nullptr);
        };

        handler = DomainHandler::onRequestBoth(
            [sharedHandler](uWS::HttpResponse<false> *res, uWS::HttpRequest *req) {
                sharedHandler.template operator()<false>(res, req);
            },
            [sharedHandler](uWS::HttpResponse<true> *res, uWS::HttpRequest *req) {
                sharedHandler.template operator()<true>(res, req);
            }
        );
    } else {
        // Unsupported handler type
        args.GetReturnValue().Set(args.This());
        return;
    }

    app->route(patternStr, std::move(handler));
    args.GetReturnValue().Set(args.This());
}

/* app.registerFileProcessor(cb) — cb(id, url, path) */
void uWS_App_registerFileProcessor(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();

    if (missingArguments(1, args)) {
        return;
    }

    auto *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    if (args[0]->IsNull() || args[0]->IsUndefined()) {
        perContextData->fileProcessorCallback.reset();
        args.GetReturnValue().Set(args.This());
        return;
    }

    Callback checkedCallback(isolate, args[0]);
    if (checkedCallback.isInvalid(args)) {
        return;
    }

    if (!perContextData->fileProcessorCallback) {
        perContextData->fileProcessorCallback = std::make_shared<Global<Function>>();
    }

    UniquePersistent<Function> cb = checkedCallback.getFunction();
    perContextData->fileProcessorCallback->Reset();
    perContextData->fileProcessorCallback->Reset(isolate, Local<Function>::New(isolate, cb));

    args.GetReturnValue().Set(args.This());
}

// Temporary
static inline bool extractBufferToString(Isolate *isolate, const Local<Value> &value, std::string *out) {
    if (value->IsArrayBuffer()) {
        Local<ArrayBuffer> ab = Local<ArrayBuffer>::Cast(value);
        std::shared_ptr<BackingStore> bs = ab->GetBackingStore();
        out->assign((const char *) bs->Data(), bs->ByteLength());
        return true;
    }

    if (value->IsArrayBufferView()) {
        Local<ArrayBufferView> view = value.As<ArrayBufferView>();
        std::shared_ptr<BackingStore> bs = view->Buffer()->GetBackingStore();
        size_t offset = view->ByteOffset();
        size_t length = view->ByteLength();
        out->assign((const char *) bs->Data() + offset, length);
        return true;
    }

    if (value->IsString()) {
        NativeString s(isolate, value);
        out->assign(s.getString());
        return true;
    }

    return false;
}

/* app.completeProcessing(id, result, [linkedPaths]) */
// TODO: We could respond to all pending requests to avoid duplicate work
// TODO: Pass the WebApp object if possible
void uWS_App_completeProcessing(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();

    if (missingArguments(2, args)) {
        return;
    }

    auto *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    uint64_t id = (uint64_t) args[0]->IntegerValue(isolate->GetCurrentContext()).ToChecked();
    auto it = perContextData->pendingFileProcesses.find(id);
    if (it == perContextData->pendingFileProcesses.end()) {
        args.GetReturnValue().Set(Boolean::New(isolate, false));
        return;
    }

    PerContextData::PendingFileProcess pending = std::move(it->second);
    perContextData->pendingFileProcesses.erase(it);

    if (!pending.webApp) {
        args.GetReturnValue().Set(Boolean::New(isolate, false));
        return;
    }

    // TODO: if buffer is "true", we shuold read the file directly (no processing done from JS)
    // As of now JS always sends the file buffer which is inefficient (though only done for the first request)
    // Problem is path handling which will be resolved later at some point but currently causes issues

    std::string buffer;
    if (!extractBufferToString(isolate, args[1], &buffer)) {
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(
            String::NewFromUtf8(isolate, "completeProcessing() requires result as String/ArrayBuffer/TypedArray", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    std::vector<std::string> linkedPaths;
    if (args.Length() > 2 && args[2]->IsArray()) {
        Local<Array> arr = Local<Array>::Cast(args[2]);
        linkedPaths.reserve(arr->Length());
        for (uint32_t i = 0; i < arr->Length(); i++) {
            Local<Value> v;
            if (!arr->Get(isolate->GetCurrentContext(), i).ToLocal(&v) || !v->IsString()) {
                continue;
            }
            v8::String::Utf8Value path(isolate, v);
            linkedPaths.emplace_back(*path, static_cast<size_t>(path.length()));
            // TODO: Fix linked paths
            // std::cout << "Registered linked path: " << linkedPaths.back() << std::endl;
        }
    }

    std::string mimeType = pending.mimeType;
    if (args.Length() > 3 && args[3]->IsString()) {
        v8::String::Utf8Value mt(isolate, args[3]);
        if (*mt && mt.length() > 0) {
            mimeType.assign(*mt, static_cast<size_t>(mt.length()));
        }
    }

    linkedPaths.emplace_back(pending.fullPath);
    Akeno::FileCache::CacheEntry* entry = pending.webApp->fileCache.update(pending.fullPath, std::move(buffer), linkedPaths, mimeType);

    if(!pending.res) {
        // Request was aborted, but we can still store cache for future requests
        args.GetReturnValue().Set(Boolean::New(isolate, true));
        return;
    }

    // Now finally try to respond to the pending request
    // TODO: We *could* try to send a 304 but this is enough and more reliable+ we shouldn't rely on req here
    if (pending.ssl) {
        auto *res = (uWS::HttpResponse<true> *) pending.res;
        if (!pending.webApp->fileCache.tryServeWithCompression(pending.fullPath, pending.variant, res, pending.status)) {
            res->end();
        }
    } else {
        auto *res = (uWS::HttpResponse<false> *) pending.res;
        if (!pending.webApp->fileCache.tryServeWithCompression(pending.fullPath, pending.variant, res, pending.status)) {
            res->end();
        }
    }

    args.GetReturnValue().Set(Boolean::New(isolate, true));
}

// Shared helper to parse options and applying them to a WebApp instance
void configureWebApp(Isolate *isolate, Akeno::WebApp *webApp, Local<Object> optionsObject) {
    Local<Context> context = isolate->GetCurrentContext();

    // browserCompatibility: [int, int, bool]
    MaybeLocal<Value> maybeCompat = optionsObject->Get(context, String::NewFromUtf8(isolate, "browserCompatibility", NewStringType::kNormal).ToLocalChecked());
    if (!maybeCompat.IsEmpty() && maybeCompat.ToLocalChecked()->IsArray()) {
        Local<Array> compatArr = Local<Array>::Cast(maybeCompat.ToLocalChecked());
        if (compatArr->Length() >= 3) {
            int botScore = compatArr->Get(context, 0).ToLocalChecked()->Int32Value(context).ToChecked();
            int humanScore = compatArr->Get(context, 1).ToLocalChecked()->Int32Value(context).ToChecked();
            bool enable = compatArr->Get(context, 2).ToLocalChecked()->BooleanValue(isolate);
            webApp->options.browserCompatibility = std::make_tuple(botScore, humanScore, enable);
        }
    }

    // root: string
    MaybeLocal<Value> maybeRoot = optionsObject->Get(context, String::NewFromUtf8(isolate, "root", NewStringType::kNormal).ToLocalChecked());
    if (!maybeRoot.IsEmpty() && maybeRoot.ToLocalChecked()->IsString()) {
        NativeString rootStr(isolate, maybeRoot.ToLocalChecked());
        webApp->root = rootStr.getString();
    }

    // enabled: bool
    MaybeLocal<Value> maybeEnabled = optionsObject->Get(context, String::NewFromUtf8(isolate, "enabled", NewStringType::kNormal).ToLocalChecked());
    if (!maybeEnabled.IsEmpty() && !maybeEnabled.ToLocalChecked()->IsUndefined()) {
        webApp->enabled = maybeEnabled.ToLocalChecked()->BooleanValue(isolate);
    }

    // redirectToHttps: bool
    MaybeLocal<Value> maybeRedirect = optionsObject->Get(context, String::NewFromUtf8(isolate, "redirectToHttps", NewStringType::kNormal).ToLocalChecked());
    if (!maybeRedirect.IsEmpty() && !maybeRedirect.ToLocalChecked()->IsUndefined()) {
        webApp->options.redirectToHttps = maybeRedirect.ToLocalChecked()->BooleanValue(isolate);
    }
}

void uWS_WebApp_setOptions(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();

    if (missingArguments(1, args)) {
        return;
    }

    Local<Object> self = args.This();
    if (self->InternalFieldCount() < 2 || self->GetAlignedPointerFromInternalField(1) != (void *)&kWebAppTag) {
        args.GetReturnValue().Set(args.This());
        return;
    }

    Akeno::WebApp *webApp = (Akeno::WebApp *) self->GetAlignedPointerFromInternalField(0);
    if (!webApp) {
        args.GetReturnValue().Set(args.This());
        return;
    }

    if (!args[0]->IsObject()) {
        args.GetReturnValue().Set(args.This());
        return;
    }

    configureWebApp(isolate, webApp, Local<Object>::Cast(args[0]));

    args.GetReturnValue().Set(args.This());
}

/* uWS.WebApp(path, [options]) */
void uWS_WebApp_constructor(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    auto *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    if (missingArguments(1, args)) {
        return;
    }

    NativeString pathValue(isolate, args[0]);
    if (pathValue.isInvalid(args)) {
        return;
    }

    // Default options
    Akeno::WebAppOptions options{};
    auto webAppShared = std::make_shared<Akeno::WebApp>(std::string(pathValue.getString()), options);
    Akeno::WebApp *webApp = webAppShared.get();

    // Apply options if provided
    if (args.Length() > 1 && args[1]->IsObject()) {
        configureWebApp(isolate, webApp, Local<Object>::Cast(args[1]));
    }

    /* Wire file processor hook (optional, callback stored on PerContextData) */
    webApp->fileProcessorHttp = [perContextData, webApp](uWS::HttpResponse<false> *res, uWS::HttpRequest *req, std::string_view url, std::string_view fullPath, std::string_view mimeType, int variant, std::string_view status) -> bool {
        if (!perContextData->fileProcessorCallback || perContextData->fileProcessorCallback->IsEmpty()) {
            return false;
        }

        uint64_t id = perContextData->nextFileProcessId++;
        PerContextData::PendingFileProcess pending;
        pending.ssl = false;
        pending.res = res;
        pending.webApp = webApp;
        pending.url.assign(url.data(), url.size());
        pending.fullPath.assign(fullPath.data(), fullPath.size());
        pending.mimeType.assign(mimeType.data(), mimeType.size());
        pending.status.assign(status.data(), status.size());
        pending.variant = variant;

        perContextData->pendingFileProcesses.emplace(id, std::move(pending));

        res->onAborted([perContextData, id]() {
            perContextData->pendingFileProcesses.erase(id);
        });

        Isolate *isolate = perContextData->isolate;
        HandleScope hs(isolate);
        Local<Function> cb = Local<Function>::New(isolate, *perContextData->fileProcessorCallback);
        Local<Value> argv[] = {Number::New(isolate, (double) id), utf8(isolate, url), utf8(isolate, fullPath), utf8(isolate, mimeType)};
        CallJS(isolate, cb, 4, argv);
        return true;
    };

    webApp->fileProcessorHttps = [perContextData, webApp](uWS::HttpResponse<true> *res, uWS::HttpRequest *req, std::string_view url, std::string_view fullPath, std::string_view mimeType, int variant, std::string_view status) -> bool {
        if (!perContextData->fileProcessorCallback || perContextData->fileProcessorCallback->IsEmpty()) {
            return false;
        }

        uint64_t id = perContextData->nextFileProcessId++;
        PerContextData::PendingFileProcess pending;
        pending.ssl = true;
        pending.res = res;
        pending.webApp = webApp;
        pending.url.assign(url.data(), url.size());
        pending.fullPath.assign(fullPath.data(), fullPath.size());
        pending.mimeType.assign(mimeType.data(), mimeType.size());
        pending.status.assign(status.data(), status.size());
        pending.variant = variant;

        perContextData->pendingFileProcesses.emplace(id, std::move(pending));

        res->onAborted([perContextData, id]() {
            perContextData->pendingFileProcesses.erase(id);
        });

        Isolate *isolate = perContextData->isolate;
        HandleScope hs(isolate);
        Local<Function> cb = Local<Function>::New(isolate, *perContextData->fileProcessorCallback);
        Local<Value> argv[] = {Number::New(isolate, (double) id), utf8(isolate, url), utf8(isolate, fullPath), utf8(isolate, mimeType)};
        CallJS(isolate, cb, 4, argv);
        return true;
    };

    /* Keep alive and allow lookup by raw pointer */
    perContextData->webAppsByPtr.emplace(webApp, webAppShared);

    Local<FunctionTemplate> webAppTemplate = FunctionTemplate::New(isolate);
    webAppTemplate->SetClassName(String::NewFromUtf8(isolate, "uWS.WebApp", NewStringType::kNormal).ToLocalChecked());
    webAppTemplate->InstanceTemplate()->SetInternalFieldCount(2);

    webAppTemplate->PrototypeTemplate()->Set(
        String::NewFromUtf8(isolate, "setOptions", NewStringType::kNormal).ToLocalChecked(),
        FunctionTemplate::New(isolate, uWS_WebApp_setOptions, args.Data()));

    webAppTemplate->PrototypeTemplate()->Set(
        String::NewFromUtf8(isolate, "applyAttributes", NewStringType::kNormal).ToLocalChecked(),
        FunctionTemplate::New(isolate, [](const FunctionCallbackInfo<Value> &args) {
            Isolate *isolate = args.GetIsolate();
            if (missingArguments(2, args)) return;
            Akeno::WebApp *webApp = (Akeno::WebApp *) args.This()->GetAlignedPointerFromInternalField(0);
            if (!webApp) return;

            NativeString pathVal(isolate, args[0]);
            if (pathVal.isInvalid(args)) return;

            if (args[1]->IsNull() || args[1]->IsUndefined()) {
                webApp->removeAttributes(pathVal.getString());
            } else if (args[1]->IsObject()) {
                Local<Context> ctx = isolate->GetCurrentContext();
                Local<Object> obj = args[1].As<Object>();
                
                Akeno::PathAttributes attr; // Default constructed
                
                Local<Value> denyVal;
                if (obj->Get(ctx, String::NewFromUtf8(isolate, "deny", NewStringType::kNormal).ToLocalChecked()).ToLocal(&denyVal)) {
                    if (!denyVal->IsUndefined()) attr.deny = denyVal->BooleanValue(isolate);
                }

                Local<Value> typeVal;
                if (obj->Get(ctx, String::NewFromUtf8(isolate, "type", NewStringType::kNormal).ToLocalChecked()).ToLocal(&typeVal)) {
                    if (!typeVal->IsUndefined()) attr.transformType = (uint8_t) typeVal->Uint32Value(ctx).FromMaybe(0);
                }

                Local<Value> targetVal;
                if (obj->Get(ctx, String::NewFromUtf8(isolate, "target", NewStringType::kNormal).ToLocalChecked()).ToLocal(&targetVal)) {
                    if (!targetVal->IsUndefined() && targetVal->IsString()) {
                         NativeString tVal(isolate, targetVal);
                         attr.transformTarget = tVal.getString();
                    }
                }

                webApp->applyAttributes(pathVal.getString(), attr);
            }
            
            args.GetReturnValue().Set(args.This());
        }, args.Data()));

    webAppTemplate->PrototypeTemplate()->Set(
        String::NewFromUtf8(isolate, "removeAttributes", NewStringType::kNormal).ToLocalChecked(),
        FunctionTemplate::New(isolate, [](const FunctionCallbackInfo<Value> &args) {
            Isolate *isolate = args.GetIsolate();
            if (missingArguments(1, args)) return;
            Akeno::WebApp *webApp = (Akeno::WebApp *) args.This()->GetAlignedPointerFromInternalField(0);
            if (!webApp) return;

            NativeString pathVal(isolate, args[0]);
            if (pathVal.isInvalid(args)) return;

            webApp->removeAttributes(pathVal.getString());
            args.GetReturnValue().Set(args.This());
        }, args.Data()));

    webAppTemplate->PrototypeTemplate()->Set(
        String::NewFromUtf8(isolate, "clearAttributes", NewStringType::kNormal).ToLocalChecked(),
        FunctionTemplate::New(isolate, [](const FunctionCallbackInfo<Value> &args) {
            Isolate *isolate = args.GetIsolate();
            Akeno::WebApp *webApp = (Akeno::WebApp *) args.This()->GetAlignedPointerFromInternalField(0);
            if (!webApp) return;
            
            webApp->clearAttributes();
            args.GetReturnValue().Set(args.This());
        }, args.Data()));

    webAppTemplate->PrototypeTemplate()->Set(
        String::NewFromUtf8(isolate, "setErrorPage", NewStringType::kNormal).ToLocalChecked(),
        FunctionTemplate::New(isolate, [](const FunctionCallbackInfo<Value> &args) {
            Isolate *isolate = args.GetIsolate();
            if (missingArguments(2, args)) return;
            Akeno::WebApp *webApp = (Akeno::WebApp *) args.This()->GetAlignedPointerFromInternalField(0);
            if (!webApp) return;

            int code = args[0]->Int32Value(isolate->GetCurrentContext()).FromMaybe(0);
            NativeString pageVal(isolate, args[1]);
            if (pageVal.isInvalid(args)) return;

            webApp->setErrorPage(code, std::string(pageVal.getString()));
            args.GetReturnValue().Set(args.This());
        }, args.Data()));

    Local<Object> localWebApp = webAppTemplate->GetFunction(isolate->GetCurrentContext())
                                   .ToLocalChecked()
                                   ->NewInstance(isolate->GetCurrentContext())
                                   .ToLocalChecked();

    localWebApp->SetAlignedPointerInInternalField(0, webApp);
    localWebApp->SetAlignedPointerInInternalField(1, (void *) &kWebAppTag);

    args.GetReturnValue().Set(localWebApp);
}

/* app.unroute(pattern) — removes a domain route. */
void uWS_App_unroute(const FunctionCallbackInfo<Value> &args) {
    uWS::App *app = (uWS::App *) args.This()->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();

    if (missingArguments(1, args)) {
        return;
    }

    NativeString pattern(isolate, args[0]);
    if (pattern.isInvalid(args)) {
        return;
    }

    app->unroute(std::string(pattern.getString()));

    args.GetReturnValue().Set(args.This());
}

/* app.onObject(handler) — handler is called with (req, res, object) */
void uWS_App_onObject(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();

    if (missingArguments(1, args)) {
        return;
    }

    auto* perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();
    uWS::App *app = (uWS::App *) args.This()->GetAlignedPointerFromInternalField(0);

    auto &callbackPtr = perContextData->appObjectCallbacks[app];
    if (!callbackPtr) {
        callbackPtr = std::make_shared<Global<Function>>();
    }

    if (args[0]->IsNull() || args[0]->IsUndefined()) {
        callbackPtr->Reset();
        args.GetReturnValue().Set(args.This());
        return;
    }

    Callback checkedCallback(isolate, args[0]);
    if (checkedCallback.isInvalid(args)) {
        return;
    }

    UniquePersistent<Function> cb = checkedCallback.getFunction();
    callbackPtr->Reset();
    callbackPtr->Reset(isolate, Local<Function>::New(isolate, cb));

    args.GetReturnValue().Set(args.This());
}

/* app.publish(topic, message, isBinary, compress) */
void uWS_App_publish(const FunctionCallbackInfo<Value> &args) {
    uWS::App *app = (uWS::App *) args.This()->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();

    /* topic, message [isBinary, compress] */
    if (missingArguments(2, args)) {
        return;
    }

    NativeString topic(isolate, args[0]);
    if (topic.isInvalid(args)) {
        return;
    }

    NativeString message(isolate, args[1]);
    if (message.isInvalid(args)) {
        return;
    }

    bool ok = app->publish(topic.getString(), message.getString(), args[2]->BooleanValue(isolate) ? uWS::OpCode::BINARY : uWS::OpCode::TEXT, args[3]->BooleanValue(isolate));

    args.GetReturnValue().Set(Boolean::New(isolate, ok));
}

/* app.numSubscribers(topic) */
void uWS_App_numSubscribers(const FunctionCallbackInfo<Value> &args) {
    uWS::App *app = (uWS::App *) args.This()->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();

    /* topic */
    if (missingArguments(1, args)) {
        return;
    }

    NativeString topic(isolate, args[0]);
    if (topic.isInvalid(args)) {
        return;
    }

    args.GetReturnValue().Set(Integer::New(isolate, app->numSubscribers(topic.getString())));
}

/* uWS.App() constructor */
void uWS_App_constructor(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    Local<FunctionTemplate> appTemplate = FunctionTemplate::New(isolate);
    appTemplate->SetClassName(String::NewFromUtf8(isolate, "uWS.App", NewStringType::kNormal).ToLocalChecked());

    /* 1 internal field: App* */
    appTemplate->InstanceTemplate()->SetInternalFieldCount(1);

    /* App methods — protocol agnostic */
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "route", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_route, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "unroute", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_unroute, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "onObject", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_onObject, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "publish", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_publish, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "numSubscribers", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_numSubscribers, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "registerFileProcessor", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_registerFileProcessor, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "completeProcessing", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_completeProcessing, args.Data()));

    Local<Object> localApp = appTemplate->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();

    /* Create the App */
    uWS::App *app = new uWS::App();

    /* Set domain router on app from the global domainRouter */
    extern Akeno::DomainRouter<DomainHandler> domainRouter;
    app->setDomainRouter(&domainRouter);

    localApp->SetAlignedPointerInInternalField(0, app);

    /* Store for cleanup */
    perContextData->apps.emplace_back(app);
    perContextData->appObjectCallbacks.emplace(app, std::make_shared<Global<Function>>());

    args.GetReturnValue().Set(localApp);
}

std::pair<uWS::SocketContextOptions, bool> readOptionsObject(const FunctionCallbackInfo<Value> &args, int index) {
    Isolate *isolate = args.GetIsolate();

    uWS::SocketContextOptions options = {};
    thread_local std::string keyFileName, certFileName, passphrase, dhParamsFileName, caFileName, sslCiphers;
    if (args.Length() > index) {

        Local<Object> optionsObject = Local<Object>::Cast(args[index]);

        /* Key file name */
        NativeString keyFileNameValue(isolate, optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "key_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked());
        if (keyFileNameValue.isInvalid(args)) {
            return {};
        }
        if (keyFileNameValue.getString().length()) {
            keyFileName = keyFileNameValue.getString();
            options.key_file_name = keyFileName.c_str();
        }

        /* Cert file name */
        NativeString certFileNameValue(isolate, optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "cert_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked());
        if (certFileNameValue.isInvalid(args)) {
            return {};
        }
        if (certFileNameValue.getString().length()) {
            certFileName = certFileNameValue.getString();
            options.cert_file_name = certFileName.c_str();
        }

        /* Passphrase */
        NativeString passphraseValue(isolate, optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "passphrase", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked());
        if (passphraseValue.isInvalid(args)) {
            return {};
        }
        if (passphraseValue.getString().length()) {
            passphrase = passphraseValue.getString();
            options.passphrase = passphrase.c_str();
        }

        /* DH params file name */
        NativeString dhParamsFileNameValue(isolate, optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "dh_params_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked());
        if (dhParamsFileNameValue.isInvalid(args)) {
            return {};
        }
        if (dhParamsFileNameValue.getString().length()) {
            dhParamsFileName = dhParamsFileNameValue.getString();
            options.dh_params_file_name = dhParamsFileName.c_str();
        }

        /* CA file name */
        NativeString caFileNameValue(isolate, optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ca_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked());
        if (caFileNameValue.isInvalid(args)) {
            return {};
        }
        if (caFileNameValue.getString().length()) {
            caFileName = caFileNameValue.getString();
            options.ca_file_name = caFileName.c_str();
        }

        /* ssl_prefer_low_memory_usage */
        options.ssl_prefer_low_memory_usage = optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ssl_prefer_low_memory_usage", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()->BooleanValue(isolate);

        /* ssl_ciphers */
        NativeString sslCiphersValue(isolate, optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ssl_ciphers", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked());
        if (sslCiphersValue.isInvalid(args)) {
            return {};
        }
        if (sslCiphersValue.getString().length()) {
            sslCiphers = sslCiphersValue.getString();
            options.ssl_ciphers = sslCiphers.c_str();
        }
    }

    return {options, true};
}

/* protocol.ws('/pattern', behavior) */
template <typename PROTO>
void uWS_Proto_ws(const FunctionCallbackInfo<Value> &args) {

    /* pattern, behavior */
    if (missingArguments(2, args)) {
        return;
    }

    Isolate *isolate = args.GetIsolate();

    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    /* This one is default constructed with defaults */
    typename PROTO::template WebSocketBehavior<PerSocketData> behavior = {};

    NativeString pattern(args.GetIsolate(), args[0]);
    if (pattern.isInvalid(args)) {
        return;
    }

    Global<Function> upgradePf;
    Global<Function> openPf;
    Global<Function> messagePf;
    Global<Function> drainPf;
    Global<Function> closePf;
    Global<Function> droppedPf;
    Global<Function> pingPf;
    Global<Function> pongPf;
    Global<Function> subscriptionPf;

    /* Get the behavior object */
    if (args.Length() == 2) {
        Local<Object> behaviorObject = Local<Object>::Cast(args[1]);

        /* maxPayloadLength or default */
        MaybeLocal<Value> maybeMaxPayloadLength = behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "maxPayloadLength", NewStringType::kNormal).ToLocalChecked());
        if (!maybeMaxPayloadLength.IsEmpty() && !maybeMaxPayloadLength.ToLocalChecked()->IsUndefined()) {
            behavior.maxPayloadLength = maybeMaxPayloadLength.ToLocalChecked()->Int32Value(isolate->GetCurrentContext()).ToChecked();
        }

        /* idleTimeout or default */
        MaybeLocal<Value> maybeIdleTimeout = behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "idleTimeout", NewStringType::kNormal).ToLocalChecked());
        if (!maybeIdleTimeout.IsEmpty() && !maybeIdleTimeout.ToLocalChecked()->IsUndefined()) {
            behavior.idleTimeout = maybeIdleTimeout.ToLocalChecked()->Int32Value(isolate->GetCurrentContext()).ToChecked();
        }

        /* maxLifetime or default */
        MaybeLocal<Value> maybeMaxLifetime = behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "maxLifetime", NewStringType::kNormal).ToLocalChecked());
        if (!maybeMaxLifetime.IsEmpty() && !maybeMaxLifetime.ToLocalChecked()->IsUndefined()) {
            behavior.maxLifetime = maybeMaxLifetime.ToLocalChecked()->Int32Value(isolate->GetCurrentContext()).ToChecked();
        }

        /* closeOnBackpressureLimit or default */
        MaybeLocal<Value> maybeCloseOnBackpressureLimit = behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "closeOnBackpressureLimit", NewStringType::kNormal).ToLocalChecked());
        if (!maybeCloseOnBackpressureLimit.IsEmpty() && !maybeCloseOnBackpressureLimit.ToLocalChecked()->IsUndefined()) {
            behavior.closeOnBackpressureLimit = maybeCloseOnBackpressureLimit.ToLocalChecked()->Int32Value(isolate->GetCurrentContext()).ToChecked();
        }

        /* sendPingsAutomatically or default */
        MaybeLocal<Value> maybeSendPingsAutomatically = behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "sendPingsAutomatically", NewStringType::kNormal).ToLocalChecked());
        if (!maybeSendPingsAutomatically.IsEmpty() && !maybeSendPingsAutomatically.ToLocalChecked()->IsUndefined()) {
            behavior.sendPingsAutomatically = maybeSendPingsAutomatically.ToLocalChecked()->Int32Value(isolate->GetCurrentContext()).ToChecked();
        }

        /* Compression or default */
        MaybeLocal<Value> maybeCompression = behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "compression", NewStringType::kNormal).ToLocalChecked());
        if (!maybeCompression.IsEmpty() && !maybeCompression.ToLocalChecked()->IsUndefined()) {
            behavior.compression = (uWS::CompressOptions) maybeCompression.ToLocalChecked()->Int32Value(isolate->GetCurrentContext()).ToChecked();
        }

        /* maxBackpressure or default */
        MaybeLocal<Value> maybeMaxBackpressure = behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "maxBackpressure", NewStringType::kNormal).ToLocalChecked());
        if (!maybeMaxBackpressure.IsEmpty() && !maybeMaxBackpressure.ToLocalChecked()->IsUndefined()) {
            behavior.maxBackpressure = maybeMaxBackpressure.ToLocalChecked()->Int32Value(isolate->GetCurrentContext()).ToChecked();
        }

        /* Upgrade */
        upgradePf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "upgrade", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
        /* Open */
        openPf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "open", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
        /* Message */
        messagePf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "message", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
        /* Drain */
        drainPf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "drain", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
        /* Close */
        closePf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "close", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
        /* Dropped */
        droppedPf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "dropped", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
        /* Ping */
        pingPf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ping", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
        /* Pong */
        pongPf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "pong", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));
    	/* Subscription */
        subscriptionPf.Reset(args.GetIsolate(), Local<Function>::Cast(behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "subscription", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked()));

    }

    constexpr bool SSL = std::is_same<PROTO, uWS::HTTPSProtocol>::value;

    /* Upgrade handler is always optional */
    if (upgradePf != Undefined(isolate)) {
        behavior.upgrade = [upgradePf = std::move(upgradePf), perContextData](auto *res, auto *req, auto *context) {
            Isolate *isolate = perContextData->isolate;
            HandleScope hs(isolate);

            Local<Function> upgradeLf = Local<Function>::New(isolate, upgradePf);
            Local<Object> resObject = perContextData->resTemplate[SSL ? 1 : 0].Get(isolate)->Clone();
            resObject->SetAlignedPointerInInternalField(0, res);

            Local<Object> reqObject = perContextData->reqTemplate[0].Get(isolate)->Clone();
            reqObject->SetAlignedPointerInInternalField(0, req);

            Local<Value> argv[3] = {resObject, reqObject, External::New(isolate, (void *) context)};
            CallJS(isolate, upgradeLf, 3, argv);

            /* Properly invalidate req */
            reqObject->SetAlignedPointerInInternalField(0, nullptr);
        };
    }

    /* Open handler is NOT optional for the wrapper */
    behavior.open = [openPf = std::move(openPf), perContextData](auto *ws) {
        Isolate *isolate = perContextData->isolate;
        HandleScope hs(isolate);

        constexpr int wsIdx = SSL ? 1 : 0;

        /* Create a new websocket object */
        Local<Object> wsObject = perContextData->wsTemplate[wsIdx].Get(isolate)->Clone();
        wsObject->SetAlignedPointerInInternalField(0, ws);

        /* Retrieve temporary userData object */
        PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();

        /* Copy entires from userData, only if we have it set (not the case for default constructor) */
        if (!perSocketData->socketPf.IsEmpty()) {
            Local<Object> userData = Local<Object>::New(isolate, perSocketData->socketPf);

            /* Merge userData and wsObject */
            Local<Array> keys;
            if (userData->GetOwnPropertyNames(isolate->GetCurrentContext()).ToLocal(&keys)) {
                for (int i = 0; i < (int)keys->Length(); i++) {
                    wsObject->Set(isolate->GetCurrentContext(),
                        keys->Get(isolate->GetCurrentContext(), i).ToLocalChecked(),
                        userData->Get(isolate->GetCurrentContext(), keys->Get(isolate->GetCurrentContext(), i).ToLocalChecked()).ToLocalChecked()
                        ).ToChecked();
                }
            }
        }

        /* Attach a new V8 object with pointer to us, to it */
        perSocketData->socketPf.Reset(isolate, wsObject);

        Local<Function> openLf = Local<Function>::New(isolate, openPf);
        if (!openLf->IsUndefined()) {
            Local<Value> argv[] = {wsObject};
            CallJS(isolate, openLf, 1, argv);
        }
    };

    /* Message handler is always optional */
    if (messagePf != Undefined(isolate)) {
        behavior.message = [messagePf = std::move(messagePf), isolate](auto *ws, std::string_view message, uWS::OpCode opCode) {
            HandleScope hs(isolate);

            Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[3] = {Local<Object>::New(isolate, perSocketData->socketPf),
                                    messageArrayBuffer,
                                    Boolean::New(isolate, opCode == uWS::OpCode::BINARY)};

            CallJS(isolate, Local<Function>::New(isolate, messagePf), 3, argv);

            messageArrayBuffer->Detach();
        };
    }

    /* Dropped handler is always optional */
    if (droppedPf != Undefined(isolate)) {
        behavior.dropped = [droppedPf = std::move(droppedPf), isolate](auto *ws, std::string_view message, uWS::OpCode opCode) {
            HandleScope hs(isolate);

            Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[3] = {Local<Object>::New(isolate, perSocketData->socketPf),
                                    messageArrayBuffer,
                                    Boolean::New(isolate, opCode == uWS::OpCode::BINARY)};

            CallJS(isolate, Local<Function>::New(isolate, droppedPf), 3, argv);

            messageArrayBuffer->Detach();
        };
    }

    /* Drain handler is always optional */
    if (drainPf != Undefined(isolate)) {
        behavior.drain = [drainPf = std::move(drainPf), isolate](auto *ws) {
            HandleScope hs(isolate);

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[1] = {Local<Object>::New(isolate, perSocketData->socketPf)
                                    };
            CallJS(isolate, Local<Function>::New(isolate, drainPf), 1, argv);
        };
    }

    /* Subscription handler is always optional */
    if (subscriptionPf != Undefined(isolate)) {
        behavior.subscription = [subscriptionPf = std::move(subscriptionPf), isolate](auto *ws, std::string_view topic, int newCount, int oldCount) {
            HandleScope hs(isolate);

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[4] = {Local<Object>::New(isolate, perSocketData->socketPf), ArrayBuffer_New(isolate, (void *) topic.data(), topic.length()), Integer::New(isolate, newCount), Integer::New(isolate, oldCount)};
            CallJS(isolate, Local<Function>::New(isolate, subscriptionPf), 4, argv);
        };
    }

    /* Ping handler is always optional */
    if (pingPf != Undefined(isolate)) {
        behavior.ping = [pingPf = std::move(pingPf), isolate](auto *ws, std::string_view message) {
            HandleScope hs(isolate);

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[2] = {Local<Object>::New(isolate, perSocketData->socketPf), ArrayBuffer_New(isolate, (void *) message.data(), message.length())};
            CallJS(isolate, Local<Function>::New(isolate, pingPf), 2, argv);
        };
    }

    /* Pong handler is always optional */
    if (pongPf != Undefined(isolate)) {
        behavior.pong = [pongPf = std::move(pongPf), isolate](auto *ws, std::string_view message) {
            HandleScope hs(isolate);

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[2] = {Local<Object>::New(isolate, perSocketData->socketPf), ArrayBuffer_New(isolate, (void *) message.data(), message.length())};
            CallJS(isolate, Local<Function>::New(isolate, pongPf), 2, argv);
        };
    }

    /* Close handler is NOT optional for the wrapper */
    behavior.close = [closePf = std::move(closePf), isolate](auto *ws, int code, std::string_view message) {
        HandleScope hs(isolate);

        Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());
        PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
        Local<Object> wsObject = Local<Object>::New(isolate, perSocketData->socketPf);

        /* Invalidate this wsObject */
        wsObject->SetAlignedPointerInInternalField(0, nullptr);

        /* Only call close handler if we have one set */
        Local<Function> closeLf = Local<Function>::New(isolate, closePf);
        if (!closeLf->IsUndefined()) {
            Local<Value> argv[3] = {wsObject, Integer::New(isolate, code), messageArrayBuffer};
            CallJS(isolate, closeLf, 3, argv);
        }

        perSocketData->socketPf.Reset();

        messageArrayBuffer->Detach();
    };

    proto->template ws<PerSocketData>(std::string(pattern.getString()), std::move(behavior));

    /* Return this */
    args.GetReturnValue().Set(args.This());
}

/* protocol.close() */
template <typename PROTO>
void uWS_Proto_close(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    proto->close();
    args.GetReturnValue().Set(args.This());
}

/* protocol.listen(cb, path) — Unix domain socket */
template <typename PROTO>
void uWS_Proto_listen_unix(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    if (missingArguments(2, args)) {
        return;
    }

    auto cb = [&args, isolate](auto *token) {
        Local<Value> argv[] = {token ? Local<Value>::Cast(External::New(isolate, token)) : Local<Value>::Cast(Boolean::New(isolate, false))};
        Local<Function>::Cast(args[0])->Call(isolate->GetCurrentContext(), isolate->GetCurrentContext()->Global(), 1, argv).IsEmpty();
    };

    std::string path;
    NativeString h(isolate, args[args.Length() - 1]);
    if (h.isInvalid(args)) {
        return;
    }
    path = h.getString();

    proto->listen(std::move(cb), path);

    args.GetReturnValue().Set(args.This());
}

/* protocol.listen([host], port, [options], callback) */
template <typename PROTO>
void uWS_Proto_listen(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    if (missingArguments(2, args)) {
        return;
    }

    /* Callback is last */
    auto cb = [&args, isolate](auto *token) {
        Local<Value> argv[] = {token ? Local<Value>::Cast(External::New(isolate, token)) : Local<Value>::Cast(Boolean::New(isolate, false))};
        Local<Function>::Cast(args[args.Length() - 1])->Call(isolate->GetCurrentContext(), isolate->GetCurrentContext()->Global(), 1, argv).IsEmpty();
    };

    /* Host is first, if present */
    std::string host;
    if (!args[0]->IsNumber()) {
        NativeString h(isolate, args[0]);
        if (h.isInvalid(args)) {
            return;
        }
        host = h.getString();
    }

    /* Port, options are in the middle, if present */
    std::vector<int> numbers;
    for (int i = std::min<int>(1, host.length()); i < args.Length() - 1; i++) {
        numbers.push_back(args[i]->Uint32Value(args.GetIsolate()->GetCurrentContext()).ToChecked());
    }

    proto->listen(host, numbers.size() ? numbers[0] : 0,
                numbers.size() > 1 ? numbers[1] : 0, std::move(cb));

    args.GetReturnValue().Set(args.This());
}

/* protocol.filter(handler) */
template <typename PROTO>
void uWS_Proto_filter(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);

    Callback checkedCallback(args.GetIsolate(), args[0]);
    if (checkedCallback.isInvalid(args)) {
        return;
    }
    UniquePersistent<Function> cb = checkedCallback.getFunction();

    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    proto->filter([cb = std::move(cb), perContextData](auto *res, int count) {
        Isolate *isolate = perContextData->isolate;
        HandleScope hs(isolate);

        constexpr int idx = getProtoTypeIndex<PROTO>();
        Local<Object> resObject = perContextData->resTemplate[idx].Get(isolate)->Clone();
        resObject->SetAlignedPointerInInternalField(0, res);

        Local<Value> argv[] = {resObject, Local<Value>::Cast(Integer::New(isolate, count))};
        CallJS(isolate, cb.Get(isolate), 2, argv);
    });

    args.GetReturnValue().Set(args.This());
}

/* protocol.bind(appObject) — bind this protocol to an App */
template <typename PROTO>
void uWS_Proto_bind(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);

    if (missingArguments(1, args)) {
        return;
    }

    /* The first argument should be an App JS object with an App* at internal field 0 */
    if (!args[0]->IsObject()) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "bind() requires an App object", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    Local<Object> appObject = Local<Object>::Cast(args[0]);
    uWS::App *app = (uWS::App *) appObject->GetAlignedPointerFromInternalField(0);

    proto->bind(app);

    args.GetReturnValue().Set(args.This());
}

/* protocol.unbind() — unbind this protocol from its current App */
template <typename PROTO>
void uWS_Proto_unbind(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    proto->unbind();
    args.GetReturnValue().Set(args.This());
}

/* protocol.adoptSocket(fd) */
template <typename PROTO>
void uWS_Proto_adoptSocket(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    int32_t fd = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();
    proto->adoptSocket(fd);

    args.GetReturnValue().Set(args.This());
}

/* protocol.removeChildAppDescriptor(descriptor) */
template <typename PROTO>
void uWS_Proto_removeChildApp(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    double descriptor = args[0]->NumberValue(isolate->GetCurrentContext()).ToChecked();

    PROTO *receivingProto;
    memcpy(&receivingProto, &descriptor, sizeof(receivingProto));

    proto->removeChildProtocol(receivingProto);

    args.GetReturnValue().Set(args.This());
}

/* protocol.addChildAppDescriptor(descriptor) */
template <typename PROTO>
void uWS_Proto_addChildApp(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    double descriptor = args[0]->NumberValue(isolate->GetCurrentContext()).ToChecked();

    PROTO *receivingProto;
    memcpy(&receivingProto, &descriptor, sizeof(receivingProto));

    proto->addChildProtocol(receivingProto);

    args.GetReturnValue().Set(args.This());
}

/* protocol.getDescriptor() */
template <typename PROTO>
void uWS_Proto_getDescriptor(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    static_assert(sizeof(double) >= sizeof(proto));

    UniquePersistent<Object> *persistentProto = new UniquePersistent<Object>;
    persistentProto->Reset(args.GetIsolate(), args.This());

    double descriptor = 0;
    memcpy(&descriptor, &proto, sizeof(proto));

    args.GetReturnValue().Set(Number::New(isolate, descriptor));
}

/* protocol.addServerName(hostname, options) */
template <typename PROTO>
void uWS_Proto_addServerName(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    NativeString hostnamePatternValue(isolate, args[0]);
    if (hostnamePatternValue.isInvalid(args)) {
        return;
    }
    std::string hostnamePattern;
    if (hostnamePatternValue.getString().length()) {
        hostnamePattern = hostnamePatternValue.getString();
    }

    auto [options, valid] = readOptionsObject(args, 1);
    if (!valid) {
        return;
    }

    proto->addServerName(hostnamePattern.c_str(), options);

    args.GetReturnValue().Set(args.This());
}

/* protocol.removeServerName(hostname) */
template <typename PROTO>
void uWS_Proto_removeServerName(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    NativeString hostnamePatternValue(isolate, args[0]);
    if (hostnamePatternValue.isInvalid(args)) {
        return;
    }
    std::string hostnamePattern;
    if (hostnamePatternValue.getString().length()) {
        hostnamePattern = hostnamePatternValue.getString();
    }

    proto->removeServerName(hostnamePattern.c_str());

    args.GetReturnValue().Set(args.This());
}

/* protocol.missingServerName(handler) */
template <typename PROTO>
void uWS_Proto_missingServerName(const FunctionCallbackInfo<Value> &args) {
    PROTO *proto = (PROTO *) args.This()->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    UniquePersistent<Function> missingPf;
    missingPf.Reset(args.GetIsolate(), Local<Function>::Cast(args[0]));

    proto->missingServerName([missingPf = std::move(missingPf), isolate](const char *hostname) {
        HandleScope hs(isolate);
        Local<Function> missingLf = Local<Function>::New(isolate, missingPf);
        Local<Value> argv[1] = {String::NewFromUtf8(isolate, hostname, NewStringType::kNormal).ToLocalChecked()};
        CallJS(isolate, missingLf, 1, argv);
    });

    args.GetReturnValue().Set(args.This());
}

/* uWS.HTTPProtocol() or uWS.HTTPSProtocol() constructor */
template <typename PROTO>
void uWS_Proto_constructor(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();

    auto [options, valid] = readOptionsObject(args, 0);
    if (!valid) {
        return;
    }

    /* Create the Protocol */
    PROTO *proto = new PROTO(options);

    if (proto->constructorFailed()) {
        delete proto;
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "Protocol construction failed", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    constexpr bool isSSL = std::is_same<PROTO, uWS::HTTPSProtocol>::value;

    Local<FunctionTemplate> protoTemplate = FunctionTemplate::New(isolate);
    protoTemplate->SetClassName(String::NewFromUtf8(isolate, isSSL ? "uWS.HTTPSProtocol" : "uWS.HTTPProtocol", NewStringType::kNormal).ToLocalChecked());

    /* 1 internal field: Protocol* */
    protoTemplate->InstanceTemplate()->SetInternalFieldCount(1);

    /* Protocol methods */
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "listen", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_listen<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "listen_unix", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_listen_unix<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "close", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_close<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "filter", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_filter<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "ws", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_ws<PROTO>, args.Data()));

    /* App binding */
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "bind", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_bind<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "unbind", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_unbind<PROTO>, args.Data()));

    /* Load balancing */
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "removeChildAppDescriptor", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_removeChildApp<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "addChildAppDescriptor", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_addChildApp<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "getDescriptor", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_getDescriptor<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "adoptSocket", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_adoptSocket<PROTO>, args.Data()));

    /* SNI */
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "addServerName", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_addServerName<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "removeServerName", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_removeServerName<PROTO>, args.Data()));
    protoTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "missingServerName", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Proto_missingServerName<PROTO>, args.Data()));

    Local<Object> localProto = protoTemplate->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
    localProto->SetAlignedPointerInInternalField(0, proto);

    /* Store for cleanup */
    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();
    if constexpr (isSSL) {
        perContextData->sslProtocols.emplace_back(proto);
    } else {
        perContextData->protocols.emplace_back(proto);
    }

    args.GetReturnValue().Set(localProto);
}

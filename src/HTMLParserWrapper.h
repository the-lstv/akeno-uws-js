#pragma once

#include <iostream>
#include <string>
#include <memory>
#include <functional>

#include <v8.h>
#include <node_buffer.h>
#include "Utilities.h"

#include "akeno/parser/x-parser.h"

using namespace v8;

struct HTMLParserUserData {
    Isolate *isolate = nullptr;
    Global<Object> ctxObject;

    HTMLParserUserData(Isolate *isolate, Local<Object> ctxObject) : isolate(isolate) {
        this->ctxObject.Reset(isolate, ctxObject);
    }

    ~HTMLParserUserData() {
        ctxObject.Reset();
    }
};

struct HTMLParserWrapper {
    Isolate *isolate = nullptr;
    Akeno::HTMLParserOptions options;
    Akeno::HTMLParsingContext ctx;

    UniquePersistent<Function> onTextRef;
    UniquePersistent<Function> onOpeningTagRef;
    UniquePersistent<Function> onClosingTagRef;
    UniquePersistent<Function> onInlineRef;
    UniquePersistent<Function> onEndRef;

    HTMLParserWrapper(Isolate *isolate, Local<Object> opts)
        : isolate(isolate),
          options(getBoolOption(isolate, opts, "buffer", false)),
          ctx(options) {
        applyOptions(opts);
    }

    static bool getBoolOption(Isolate *isolate, Local<Object> opts, const char *name, bool defaultValue) {
        if (opts.IsEmpty()) {
            return defaultValue;
        }

        Local<Context> context = isolate->GetCurrentContext();
        Local<String> key = String::NewFromUtf8(isolate, name, NewStringType::kNormal).ToLocalChecked();
        if (!opts->Has(context, key).FromMaybe(false)) {
            return defaultValue;
        }

        Local<Value> value = opts->Get(context, key).ToLocalChecked();
        return value->BooleanValue(isolate);
    }

    static bool getOptionBool(Isolate *isolate, Local<Object> opts, const char *name, bool *out) {
        if (opts.IsEmpty()) {
            return false;
        }

        Local<Context> context = isolate->GetCurrentContext();
        Local<String> key = String::NewFromUtf8(isolate, name, NewStringType::kNormal).ToLocalChecked();
        if (!opts->Has(context, key).FromMaybe(false)) {
            return false;
        }

        Local<Value> value = opts->Get(context, key).ToLocalChecked();
        *out = value->BooleanValue(isolate);
        return true;
    }

    void applyOptions(Local<Object> opts) {
        Local<Context> context = isolate->GetCurrentContext();

        bool boolValue = false;
        if (getOptionBool(isolate, opts, "compact", &boolValue)) {
            options.compact = boolValue;
        }

        if (getOptionBool(isolate, opts, "vanilla", &boolValue)) {
            options.vanilla = boolValue;
        }

        if (getOptionBool(isolate, opts, "enableImport", &boolValue)) {
            options.enableImport = boolValue;
        }

        Local<String> headerKey = String::NewFromUtf8(isolate, "header", NewStringType::kNormal).ToLocalChecked();
        if (opts->Has(context, headerKey).FromMaybe(false)) {
            Local<Value> headerValue = opts->Get(context, headerKey).ToLocalChecked();
            Local<Value> headerString;
            if (headerValue->ToString(context).ToLocal(&headerString)) {
                String::Utf8Value headerStr(isolate, headerString);
                if (*headerStr) {
                    options.header.assign(*headerStr, headerStr.length());
                }
            }
        }

        attachCallback(opts, "onText", onTextRef, [&](Local<Function> /*fn*/) {
            options.onText = [this](std::string &buffer, std::stack<std::string_view> &tagStack, std::string_view value, void *userData) {
                if (userData == nullptr || value.empty()) {
                    return;
                }

                bool isScriptOrStyle = false;
                if (!tagStack.empty()) {
                    const auto &top = tagStack.top();
                    isScriptOrStyle = (top == "script" || top == "style");
                }

                bool hasAtSymbol = value.find('@') != std::string_view::npos;
                if (!hasAtSymbol && !isScriptOrStyle) {
                    buffer.append(value);
                    return;
                }

                Isolate *isolate = this->isolate;
                HTMLParserUserData *ctxUser = static_cast<HTMLParserUserData *>(userData);
                Local<Object> ctxObj = Local<Object>::New(isolate, ctxUser->ctxObject);
                Local<Value> argv[3];
                argv[0] = String::NewFromUtf8(isolate, value.data(), NewStringType::kNormal, static_cast<int>(value.size())).ToLocalChecked();

                if (!tagStack.empty()) {
                    const auto &top = tagStack.top();
                    argv[1] = String::NewFromUtf8(isolate, top.data(), NewStringType::kNormal, static_cast<int>(top.size())).ToLocalChecked();
                } else {
                    argv[1] = Null(isolate);
                }

                argv[2] = ctxObj;

                Local<Function> cb = onTextRef.Get(isolate);
                MaybeLocal<Value> maybeResult = CallJS(isolate, cb, 3, argv);
                if (maybeResult.IsEmpty()) {
                    return;
                }

                Local<Value> result = maybeResult.ToLocalChecked();
                if (appendResultToBuffer(isolate, result, buffer)) {
                    return;
                }

                if (result->IsBoolean() && result->BooleanValue(isolate)) {
                    buffer.append(value);
                }
            };
        });

        attachCallback(opts, "onOpeningTag", onOpeningTagRef, [&](Local<Function> /*fn*/) {
            options.onOpeningTag = [this](std::string &buffer, std::stack<std::string_view> &tagStack, std::string_view tag, void *userData) {
                if (userData == nullptr) {
                    return;
                }

                Isolate *isolate = this->isolate;
                HTMLParserUserData *ctxUser = static_cast<HTMLParserUserData *>(userData);
                Local<Object> ctxObj = Local<Object>::New(isolate, ctxUser->ctxObject);
                Local<Value> argv[3];
                argv[0] = String::NewFromUtf8(isolate, tag.data(), NewStringType::kNormal, static_cast<int>(tag.size())).ToLocalChecked();

                if (!tagStack.empty()) {
                    const auto &top = tagStack.top();
                    argv[1] = String::NewFromUtf8(isolate, top.data(), NewStringType::kNormal, static_cast<int>(top.size())).ToLocalChecked();
                } else {
                    argv[1] = Null(isolate);
                }

                argv[2] = ctxObj;

                Local<Function> cb = onOpeningTagRef.Get(isolate);
                MaybeLocal<Value> maybeResult = CallJS(isolate, cb, 3, argv);
                if (maybeResult.IsEmpty()) {
                    return;
                }

                Local<Value> result = maybeResult.ToLocalChecked();
                appendResultToBuffer(isolate, result, buffer);
            };
        });

        attachCallback(opts, "onClosingTag", onClosingTagRef, [&](Local<Function> /*fn*/) {
            options.onClosingTag = [this](std::string &buffer, std::stack<std::string_view> &tagStack, std::string_view tag, void *userData) {
                if (userData == nullptr) {
                    return;
                }

                Isolate *isolate = this->isolate;
                HTMLParserUserData *ctxUser = static_cast<HTMLParserUserData *>(userData);
                Local<Object> ctxObj = Local<Object>::New(isolate, ctxUser->ctxObject);
                Local<Value> argv[3];
                argv[0] = String::NewFromUtf8(isolate, tag.data(), NewStringType::kNormal, static_cast<int>(tag.size())).ToLocalChecked();

                if (!tagStack.empty()) {
                    const auto &top = tagStack.top();
                    argv[1] = String::NewFromUtf8(isolate, top.data(), NewStringType::kNormal, static_cast<int>(top.size())).ToLocalChecked();
                } else {
                    argv[1] = Null(isolate);
                }

                argv[2] = ctxObj;

                Local<Function> cb = onClosingTagRef.Get(isolate);
                MaybeLocal<Value> maybeResult = CallJS(isolate, cb, 3, argv);
                if (maybeResult.IsEmpty()) {
                    return;
                }

                Local<Value> result = maybeResult.ToLocalChecked();
                appendResultToBuffer(isolate, result, buffer);
            };
        });

        attachCallback(opts, "onInline", onInlineRef, [&](Local<Function> /*fn*/) {
            options.onInline = [this](std::string &buffer, std::stack<std::string_view> &tagStack, std::string_view tag, void *userData) {
                if (userData == nullptr) {
                    return;
                }

                Isolate *isolate = this->isolate;
                HTMLParserUserData *ctxUser = static_cast<HTMLParserUserData *>(userData);
                Local<Object> ctxObj = Local<Object>::New(isolate, ctxUser->ctxObject);
                Local<Value> argv[3];
                argv[0] = String::NewFromUtf8(isolate, tag.data(), NewStringType::kNormal, static_cast<int>(tag.size())).ToLocalChecked();

                if (!tagStack.empty()) {
                    const auto &top = tagStack.top();
                    argv[1] = String::NewFromUtf8(isolate, top.data(), NewStringType::kNormal, static_cast<int>(top.size())).ToLocalChecked();
                } else {
                    argv[1] = Null(isolate);
                }

                argv[2] = ctxObj;

                Local<Function> cb = onInlineRef.Get(isolate);
                MaybeLocal<Value> maybeResult = CallJS(isolate, cb, 3, argv);
                if (maybeResult.IsEmpty()) {
                    return;
                }

                Local<Value> result = maybeResult.ToLocalChecked();
                appendResultToBuffer(isolate, result, buffer);
            };
        });

        attachCallback(opts, "onEnd", onEndRef, [&](Local<Function> /*fn*/) {
            options.onEnd = [this](void *userData) {
                if (userData == nullptr) {
                    return;
                }

                Isolate *isolate = this->isolate;
                HTMLParserUserData *ctxUser = static_cast<HTMLParserUserData *>(userData);
                Local<Object> ctxObj = Local<Object>::New(isolate, ctxUser->ctxObject);
                Local<Value> argv[1] = { ctxObj };

                Local<Function> cb = onEndRef.Get(isolate);
                CallJS(isolate, cb, 1, argv);
            };
        });
    }

    static bool appendResultToBuffer(Isolate *isolate, Local<Value> value, std::string &buffer) {
        if (value->IsString()) {
            String::Utf8Value str(isolate, value);
            if (*str) {
                buffer.append(*str, str.length());
            }
            return true;
        }

        if (value->IsArrayBufferView()) {
            Local<ArrayBufferView> view = value.As<ArrayBufferView>();
            std::shared_ptr<BackingStore> store = view->Buffer()->GetBackingStore();
            buffer.append(static_cast<char *>(store->Data()) + view->ByteOffset(), view->ByteLength());
            return true;
        }

        if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> ab = value.As<ArrayBuffer>();
            std::shared_ptr<BackingStore> store = ab->GetBackingStore();
            buffer.append(static_cast<char *>(store->Data()), store->ByteLength());
            return true;
        }

        return false;
    }

    template <typename T>
    void attachCallback(Local<Object> opts, const char *name, UniquePersistent<Function> &storage, T attach) {
        Local<Context> context = isolate->GetCurrentContext();
        Local<String> key = String::NewFromUtf8(isolate, name, NewStringType::kNormal).ToLocalChecked();
        if (!opts->Has(context, key).FromMaybe(false)) {
            return;
        }

        Local<Value> value = opts->Get(context, key).ToLocalChecked();
        if (!value->IsFunction()) {
            return;
        }

        Local<Function> fn = Local<Function>::Cast(value);
        storage.Reset(isolate, fn);
        attach(fn);
    }
};

static inline void ThrowTypeError(Isolate *isolate, const char *message) {
    isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, message, NewStringType::kNormal).ToLocalChecked()));
}

static HTMLParserWrapper *getParserWrapper(const FunctionCallbackInfo<Value> &args) {
    return static_cast<HTMLParserWrapper *>(args.This()->GetAlignedPointerFromInternalField(0));
}

static void Akeno_HTMLParser_context_write(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    if (args.Length() < 1) {
        ThrowTypeError(isolate, "Expected a string or a buffer");
        return;
    }

    if (!parser->ctx.output) {
        ThrowTypeError(isolate, "ParserContext is not active.");
        return;
    }

    NativeString data(isolate, args[0]);
    if (data.isInvalid(args)) {
        return;
    }

    parser->ctx.output->append(data.getString());
}

static void Akeno_HTMLParser_context_getTagName(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    if (!parser->ctx.tagStack.empty()) {
        const auto &top = parser->ctx.tagStack.top();
        args.GetReturnValue().Set(String::NewFromUtf8(isolate, top.data(), NewStringType::kNormal, static_cast<int>(top.size())).ToLocalChecked());
        return;
    }

    args.GetReturnValue().Set(Null(isolate));
}

static void Akeno_HTMLParser_context_setBodyAttributes(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    if (args.Length() < 1 || !args[0]->IsString()) {
        ThrowTypeError(isolate, "Expected a string");
        return;
    }

    String::Utf8Value attr(isolate, args[0]);
    parser->ctx.body_attributes.assign(*attr ? *attr : "", attr.length());
}

static void Akeno_HTMLParser_context_import(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    if (args.Length() < 1 || !args[0]->IsString()) {
        ThrowTypeError(isolate, "Expected a string");
        return;
    }

    String::Utf8Value path(isolate, args[0]);
    std::string filePath(*path ? *path : "", path.length());

    try {
        parser->ctx.inlineFile(filePath);
    } catch (const std::runtime_error &e) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, e.what(), NewStringType::kNormal).ToLocalChecked()));
    }
}

static void Akeno_HTMLParser_createContext(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    Local<FunctionTemplate> ctxTemplate = FunctionTemplate::New(isolate);
    ctxTemplate->SetClassName(String::NewFromUtf8(isolate, "HTMLParserContext", NewStringType::kNormal).ToLocalChecked());
    ctxTemplate->InstanceTemplate()->SetInternalFieldCount(1);
    ctxTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "write", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_context_write));
    ctxTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "onText", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_context_write));
    ctxTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "getTagName", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_context_getTagName));
    ctxTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "setBodyAttributes", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_context_setBodyAttributes));
    ctxTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "import", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_context_import));

    Local<Object> ctxObject = ctxTemplate->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()
        ->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();

    Local<Object> dataObject;
    if (args.Length() > 0 && args[0]->IsObject()) {
        dataObject = Local<Object>::Cast(args[0]);
    } else {
        dataObject = Object::New(isolate);
    }

    ctxObject->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "data", NewStringType::kNormal).ToLocalChecked(), dataObject).ToChecked();
    ctxObject->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "embedded", NewStringType::kNormal).ToLocalChecked(), Boolean::New(isolate, true)).ToChecked();
    ctxObject->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "strict", NewStringType::kNormal).ToLocalChecked(), Boolean::New(isolate, false)).ToChecked();

    ctxObject->SetAlignedPointerInInternalField(0, parser);
    args.GetReturnValue().Set(ctxObject);
}

static void Akeno_HTMLParser_fromStringInternal(const FunctionCallbackInfo<Value> &args, bool isMarkdown) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsObject()) {
        ThrowTypeError(isolate, "Expected a string and a ParserContext instance");
        return;
    }

    String::Utf8Value input(isolate, args[0]);
    std::string source(*input ? *input : "", input.length());
    Local<Object> ctxObject = Local<Object>::Cast(args[1]);

    std::string result;
    HTMLParserUserData userData(isolate, ctxObject);

    parser->ctx.in_markdown = isMarkdown;

    try {
        parser->ctx.write(source, &result, &userData);
        parser->ctx.end();
    } catch (const std::exception &e) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, e.what(), NewStringType::kNormal).ToLocalChecked()));
        return;
    }

    auto maybeBuffer = node::Buffer::Copy(isolate, result.data(), result.size());
    if (maybeBuffer.IsEmpty()) {
        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    args.GetReturnValue().Set(maybeBuffer.ToLocalChecked());
}

static void Akeno_HTMLParser_fromFileInternal(const FunctionCallbackInfo<Value> &args, bool isMarkdown) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    if (args.Length() < 2 || !args[0]->IsString() || !args[1]->IsObject()) {
        ThrowTypeError(isolate, "Expected a string and a ParserContext instance");
        return;
    }

    String::Utf8Value path(isolate, args[0]);
    std::string filePath(*path ? *path : "", path.length());
    Local<Object> ctxObject = Local<Object>::Cast(args[1]);

    std::string appPath;
    Local<Context> context = isolate->GetCurrentContext();
    Local<Value> dataValue = ctxObject->Get(context, String::NewFromUtf8(isolate, "data", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked();
    if (dataValue->IsObject()) {
        Local<Object> dataObj = Local<Object>::Cast(dataValue);
        Local<Value> pathValue = dataObj->Get(context, String::NewFromUtf8(isolate, "path", NewStringType::kNormal).ToLocalChecked()).ToLocalChecked();
        if (pathValue->IsString()) {
            String::Utf8Value appPathStr(isolate, pathValue);
            appPath.assign(*appPathStr ? *appPathStr : "", appPathStr.length());
        }
    }

    parser->ctx.in_markdown = isMarkdown;
    parser->ctx.sanitize_html = (args.Length() > 2 && args[2]->IsBoolean()) ? args[2]->BooleanValue(isolate) : false;
    parser->ctx.template_enabled = (args.Length() > 3 && args[3]->IsBoolean()) ? args[3]->BooleanValue(isolate) : false;

    HTMLParserUserData userData(isolate, ctxObject);
    Akeno::FileCache *cache = nullptr;

    try {
        cache = &parser->ctx.fromFile(filePath, &userData, appPath);
    } catch (const std::exception &e) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, e.what(), NewStringType::kNormal).ToLocalChecked()));
        return;
    }

    auto resultPtr = std::make_shared<std::string>(std::move(parser->ctx.exportCopy(std::shared_ptr<Akeno::FileCache>(cache, [](Akeno::FileCache *) {}))));
    auto *storage = new std::shared_ptr<std::string>(std::move(resultPtr));

    auto maybeBuffer = node::Buffer::New(
        isolate,
        const_cast<char *>((*storage)->data()),
        (*storage)->size(),
        [](char *data, void *hint) {
            auto *sp = static_cast<std::shared_ptr<std::string> *>(hint);
            delete sp;
        },
        storage);

    if (maybeBuffer.IsEmpty()) {
        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    args.GetReturnValue().Set(maybeBuffer.ToLocalChecked());
}

static void Akeno_HTMLParser_fromString(const FunctionCallbackInfo<Value> &args) {
    Akeno_HTMLParser_fromStringInternal(args, false);
}

static void Akeno_HTMLParser_fromMarkdownString(const FunctionCallbackInfo<Value> &args) {
    Akeno_HTMLParser_fromStringInternal(args, true);
}

static void Akeno_HTMLParser_fromFile(const FunctionCallbackInfo<Value> &args) {
    Akeno_HTMLParser_fromFileInternal(args, false);
}

static void Akeno_HTMLParser_fromMarkdownFile(const FunctionCallbackInfo<Value> &args) {
    Akeno_HTMLParser_fromFileInternal(args, true);
}

static void Akeno_HTMLParser_needsUpdate(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    HTMLParserWrapper *parser = getParserWrapper(args);

    if (!parser) {
        ThrowTypeError(isolate, "Parser instance is not initialized.");
        return;
    }

    if (args.Length() < 1 || !args[0]->IsString()) {
        ThrowTypeError(isolate, "Expected a string");
        return;
    }

    String::Utf8Value path(isolate, args[0]);
    std::string filePath(*path ? *path : "", path.length());
    bool needsUpdate = parser->ctx.needsUpdate(filePath);
    args.GetReturnValue().Set(Boolean::New(isolate, needsUpdate));
}

void Akeno_HTMLParser_constructor(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();

    Local<FunctionTemplate> parserTemplate = FunctionTemplate::New(isolate);
    parserTemplate->SetClassName(String::NewFromUtf8(isolate, "HTMLParser", NewStringType::kNormal).ToLocalChecked());
    parserTemplate->InstanceTemplate()->SetInternalFieldCount(1);

    parserTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "fromString", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_fromString));
    parserTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "fromFile", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_fromFile));
    parserTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "fromMarkdownString", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_fromMarkdownString));
    parserTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "fromMarkdownFile", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_fromMarkdownFile));
    parserTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "createContext", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_createContext));
    parserTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "needsUpdate", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, Akeno_HTMLParser_needsUpdate));

    Local<Object> parserObject = parserTemplate->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()
        ->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();

    Local<Object> opts = (args.Length() > 0 && args[0]->IsObject()) ? Local<Object>::Cast(args[0]) : Object::New(isolate);
    HTMLParserWrapper *parser = new HTMLParserWrapper(isolate, opts);
    parserObject->SetAlignedPointerInInternalField(0, parser);

    args.GetReturnValue().Set(parserObject);
}
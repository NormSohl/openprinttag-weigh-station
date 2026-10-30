// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Host shim: the slice of ESPAsyncWebServer that src/web_app.cpp uses, served
// by cpp-httplib (sim_server.cpp). See tools/sim/README.md.
//
// Fidelity rules, because the point is to catch firmware bugs on the Mac:
//  - Route matching copies AsyncURIMatcher's BackwardCompatible mode: a plain
//    route matches the exact path OR anything under "path/", "/*" matches all,
//    and the FIRST registered handler that matches wins. That is what makes
//    the /config vs /config/export ordering gotcha reproducible here.
//  - getParam(name) reads the query string only; getParam(name, true) reads
//    the POST body only — same split as the real library.
//  - Handlers run one at a time (one global lock), like the async_tcp task.
#pragma once
#include "Arduino.h"
#include "LittleFS.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

typedef enum {
    HTTP_GET     = 0b00000001,
    HTTP_POST    = 0b00000010,
    HTTP_DELETE  = 0b00000100,
    HTTP_PUT     = 0b00001000,
    HTTP_PATCH   = 0b00010000,
    HTTP_HEAD    = 0b00100000,
    HTTP_OPTIONS = 0b01000000,
    HTTP_ANY     = 0b01111111,
} WebRequestMethod;
typedef uint8_t WebRequestMethodComposite;

class AsyncWebParameter {
public:
    AsyncWebParameter(const String& n, const String& v, bool post, bool file = false)
        : name_(n), value_(v), post_(post), file_(file) {}
    const String& name()  const { return name_; }
    const String& value() const { return value_; }
    bool isPost() const { return post_; }
    bool isFile() const { return file_; }
private:
    String name_, value_;
    bool post_, file_;
};

class AsyncWebServerResponse {
public:
    AsyncWebServerResponse(int code, const String& type, std::string body)
        : code_(code), type_(type.c_str()), body_(std::move(body)) {}
    void addHeader(const char* n, const char* v) { headers_.emplace_back(n, v); }
    void addHeader(const String& n, const String& v) { headers_.emplace_back(n.c_str(), v.c_str()); }
    int code_;
    std::string type_, body_;
    std::vector<std::pair<std::string, std::string>> headers_;
};

class AsyncWebServerRequest {
public:
    // ── params ──
    bool hasParam(const char* name, bool post = false, bool file = false) const {
        return getParam(name, post, file) != nullptr;
    }
    bool hasParam(const String& name, bool post = false, bool file = false) const {
        return hasParam(name.c_str(), post, file);
    }
    const AsyncWebParameter* getParam(const char* name, bool post = false, bool file = false) const {
        for (auto& p : params_)
            if (p.isPost() == post && p.isFile() == file && p.name() == name) return &p;
        return nullptr;
    }
    const AsyncWebParameter* getParam(const String& name, bool post = false, bool file = false) const {
        return getParam(name.c_str(), post, file);
    }
    size_t params() const { return params_.size(); }
    const AsyncWebParameter* getParam(size_t i) const { return i < params_.size() ? &params_[i] : nullptr; }

    // ── headers / auth ──
    bool   hasHeader(const char* name) const;
    String header(const char* name) const;
    bool   authenticate(const char* user, const char* pass) const;
    void   requestAuthentication(const char* realm = nullptr, bool digest = false);

    // ── responses ──
    void send(int code, const char* type = "", const char* body = "") {
        sendBody_(code, type, body ? body : "");
    }
    void send(int code, const char* type, const String& body) { sendBody_(code, type, body.c_str()); }
    void send(int code, const String& type, const String& body) { sendBody_(code, type.c_str(), body.c_str()); }
    void send(AsyncWebServerResponse* r);
    void redirect(const char* url);
    void redirect(const String& url) { redirect(url.c_str()); }
    AsyncWebServerResponse* beginResponse(int code, const char* type, const String& body) {
        return new AsyncWebServerResponse(code, type, body.c_str());
    }
    AsyncWebServerResponse* beginResponse(int code, const String& type, const String& body) {
        return new AsyncWebServerResponse(code, type, body.c_str());
    }
    AsyncWebServerResponse* beginResponse(LittleFSShim& fs, const char* path,
                                          const char* type, bool download = false);
    AsyncWebServerResponse* beginResponse(LittleFSShim& fs, const String& path,
                                          const char* type, bool download = false) {
        return beginResponse(fs, path.c_str(), type, download);
    }

    String url() const { return String(path_.c_str()); }
    WebRequestMethodComposite method() const { return method_; }

    // ── filled in by sim_server.cpp ──
    std::string path_;
    WebRequestMethodComposite method_ = HTTP_GET;
    std::vector<AsyncWebParameter> params_;
    std::map<std::string, std::string> headers_;   // lower-cased names
    bool sent_ = false;
    std::unique_ptr<AsyncWebServerResponse> response_;
private:
    void sendBody_(int code, const char* type, const char* body);
};

typedef std::function<void(AsyncWebServerRequest*)> ArRequestHandlerFunction;
typedef std::function<void(AsyncWebServerRequest*, const String& filename, size_t index,
                           uint8_t* data, size_t len, bool final)> ArUploadHandlerFunction;

// ── Server-Sent Events ───────────────────────────────────────────────────────
class AsyncEventSource;
class AsyncEventSourceClient {
public:
    explicit AsyncEventSourceClient(AsyncEventSource* src) : src_(src) {}
    void send(const char* message, const char* event = nullptr, uint32_t id = 0,
              uint32_t reconnect = 0);
    // Pending SSE frames, drained by the streaming connection in sim_server.cpp.
    std::vector<std::string> queue_;
    bool closed_ = false;
private:
    AsyncEventSource* src_;
};

typedef std::function<void(AsyncEventSourceClient*)> ArEventHandlerFunction;

class AsyncEventSource {
public:
    enum SendStatus { DISCARDED = 0, ENQUEUED = 1, PARTIALLY_ENQUEUED = 2 };
    explicit AsyncEventSource(const char* url) : url_(url) {}
    void onConnect(ArEventHandlerFunction cb)    { onConnect_ = cb; }
    void onDisconnect(ArEventHandlerFunction cb) { onDisconnect_ = cb; }
    SendStatus send(const char* message, const char* event = nullptr, uint32_t id = 0,
                    uint32_t reconnect = 0);
    size_t count() const;
    const std::string& url() const { return url_; }

    // sim_server.cpp
    std::string url_;
    ArEventHandlerFunction onConnect_, onDisconnect_;
    std::vector<std::shared_ptr<AsyncEventSourceClient>> clients_;
};

// ── Server ───────────────────────────────────────────────────────────────────
class AsyncWebServer {
public:
    explicit AsyncWebServer(uint16_t port) : port_(port) {}
    void on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction fn) {
        routes_.push_back({uri, method, fn, nullptr});
    }
    void on(const char* uri, WebRequestMethodComposite method, ArRequestHandlerFunction fn,
            ArUploadHandlerFunction upload) {
        routes_.push_back({uri, method, fn, upload});
    }
    void onNotFound(ArRequestHandlerFunction fn) { notFound_ = fn; }
    void addHandler(AsyncEventSource* src) { events_.push_back(src); }
    void begin();   // registers this server with the sim; sim_main.cpp listens

    struct Route {
        std::string uri;
        WebRequestMethodComposite method;
        ArRequestHandlerFunction fn;
        ArUploadHandlerFunction upload;
    };
    uint16_t port_;
    std::vector<Route> routes_;
    ArRequestHandlerFunction notFound_;
    std::vector<AsyncEventSource*> events_;
};

class DefaultHeaders {
public:
    static DefaultHeaders& Instance() { static DefaultHeaders d; return d; }
    void addHeader(const char* n, const char* v) { headers_.emplace_back(n, v); }
    std::vector<std::pair<std::string, std::string>> headers_;
};

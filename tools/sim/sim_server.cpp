// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// The ESPAsyncWebServer shim's implementation, over cpp-httplib. See
// shim/ESPAsyncWebServer.h for the fidelity rules this keeps.
#include "sim.h"
#include "httplib.h"
#include <ESPAsyncWebServer.h>
#include <chrono>
#include <condition_variable>

std::recursive_mutex gSimLock;

static AsyncWebServer* sFirmwareServer = nullptr;
void AsyncWebServer::begin() { sFirmwareServer = this; }

// SSE queues are filled under gSimLock (from handlers) and drained by the
// streaming connection's own thread, which must not hold gSimLock while it
// waits -- so they get a lock of their own.
static std::mutex              sSseMutex;
static std::condition_variable sSseCv;

// ── request: headers / auth ──────────────────────────────────────────────────
static std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
bool AsyncWebServerRequest::hasHeader(const char* name) const {
    return headers_.count(lower(name)) > 0;
}
String AsyncWebServerRequest::header(const char* name) const {
    auto it = headers_.find(lower(name));
    return it == headers_.end() ? String() : String(it->second.c_str());
}

static std::string b64decode(const std::string& in) {
    static const std::string tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        size_t p = tbl.find(c);
        if (p == std::string::npos) break;   // '=' padding or junk ends it
        val = (val << 6) + (int)p;
        bits += 6;
        if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

// Basic auth only: the firmware never asks for digest.
bool AsyncWebServerRequest::authenticate(const char* user, const char* pass) const {
    auto it = headers_.find("authorization");
    if (it == headers_.end()) return false;
    const std::string& h = it->second;
    if (h.compare(0, 6, "Basic ") != 0) return false;
    std::string cred = b64decode(h.substr(6));
    return cred == std::string(user) + ":" + pass;
}
void AsyncWebServerRequest::requestAuthentication(const char* realm, bool) {
    auto* r = new AsyncWebServerResponse(401, "text/plain", "Unauthorized");
    r->addHeader("WWW-Authenticate",
                 (std::string("Basic realm=\"") + (realm ? realm : "Login Required") + "\"").c_str());
    send(r);
}

// ── request: responses ───────────────────────────────────────────────────────
void AsyncWebServerRequest::send(AsyncWebServerResponse* r) {
    if (sent_) {
        // On the device a second send() on one request is a bug (the first
        // response is already queued). Say so rather than hide it.
        Serial.printf("[sim] WARNING: second response for %s ignored\n", path_.c_str());
        delete r;
        return;
    }
    sent_ = true;
    response_.reset(r);
}
void AsyncWebServerRequest::sendBody_(int code, const char* type, const char* body) {
    send(new AsyncWebServerResponse(code, type ? type : "", body ? body : ""));
}
void AsyncWebServerRequest::redirect(const char* url) {
    auto* r = new AsyncWebServerResponse(302, "text/plain", "");
    r->addHeader("Location", url);
    send(r);
}
AsyncWebServerResponse* AsyncWebServerRequest::beginResponse(LittleFSShim& fs, const char* path,
                                                             const char* type, bool download) {
    std::string body;
    File f = fs.open(path, "r");
    const bool found = (bool)f;
    if (found) {
        uint8_t buf[4096];
        size_t n;
        while ((n = f.read(buf, sizeof(buf))) > 0) body.append((const char*)buf, n);
        f.close();
    }
    auto* r = new AsyncWebServerResponse(found ? 200 : 404, type, body);
    if (download) {
        std::string p(path);
        r->addHeader("Content-Disposition",
                     ("attachment; filename=" + p.substr(p.find_last_of('/') + 1)).c_str());
    }
    return r;
}

// ── SSE ──────────────────────────────────────────────────────────────────────
static std::string sseFrame(const char* message, const char* event, uint32_t id, uint32_t reconnect) {
    std::string f;
    if (reconnect) f += "retry: " + std::to_string(reconnect) + "\n";
    if (id)        f += "id: " + std::to_string(id) + "\n";
    if (event && *event) f += std::string("event: ") + event + "\n";
    f += std::string("data: ") + (message ? message : "") + "\n\n";
    return f;
}
void AsyncEventSourceClient::send(const char* message, const char* event, uint32_t id,
                                  uint32_t reconnect) {
    {
        std::lock_guard<std::mutex> lk(sSseMutex);
        queue_.push_back(sseFrame(message, event, id, reconnect));
    }
    sSseCv.notify_all();
}
AsyncEventSource::SendStatus AsyncEventSource::send(const char* message, const char* event,
                                                    uint32_t id, uint32_t reconnect) {
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(sSseMutex);
        for (auto& c : clients_)
            if (!c->closed_) { c->queue_.push_back(sseFrame(message, event, id, reconnect)); n++; }
    }
    sSseCv.notify_all();
    return n ? ENQUEUED : DISCARDED;
}
size_t AsyncEventSource::count() const {
    std::lock_guard<std::mutex> lk(sSseMutex);
    size_t n = 0;
    for (auto& c : clients_) if (!c->closed_) n++;
    return n;
}

static void serveEvents(AsyncEventSource* src, httplib::Response& res) {
    auto client = std::make_shared<AsyncEventSourceClient>(src);
    {
        std::lock_guard<std::mutex> lk(sSseMutex);
        src->clients_.push_back(client);
    }
    bool connected = false;
    res.set_header("Cache-Control", "no-cache");
    res.set_chunked_content_provider(
        "text/event-stream",
        [src, client, connected](size_t, httplib::DataSink& sink) mutable {
            if (!connected) {
                connected = true;
                std::lock_guard<std::recursive_mutex> g(gSimLock);
                if (src->onConnect_) src->onConnect_(client.get());
            }
            std::vector<std::string> out;
            {
                std::unique_lock<std::mutex> lk(sSseMutex);
                sSseCv.wait_for(lk, std::chrono::seconds(15),
                                [&] { return !client->queue_.empty(); });
                out.swap(client->queue_);
            }
            if (out.empty()) out.push_back(":\n\n");   // keep-alive comment
            for (auto& f : out)
                if (!sink.write(f.data(), f.size())) return false;
            return true;
        },
        [src, client](bool) {
            {
                std::lock_guard<std::mutex> lk(sSseMutex);
                client->closed_ = true;
                auto& v = src->clients_;
                v.erase(std::remove(v.begin(), v.end(), client), v.end());
            }
            std::lock_guard<std::recursive_mutex> g(gSimLock);
            if (src->onDisconnect_) src->onDisconnect_(client.get());
        });
}

// ── dispatch ─────────────────────────────────────────────────────────────────
// AsyncURIMatcher, BackwardCompatible mode.
static bool uriMatches(const std::string& uri, const std::string& path) {
    if (!uri.empty() && uri.back() == '*')
        return path.compare(0, uri.size() - 1, uri, 0, uri.size() - 1) == 0;
    return path == uri || path.compare(0, uri.size() + 1, uri + "/") == 0;
}
static WebRequestMethodComposite methodOf(const std::string& m) {
    if (m == "GET")     return HTTP_GET;
    if (m == "POST")    return HTTP_POST;
    if (m == "DELETE")  return HTTP_DELETE;
    if (m == "PUT")     return HTTP_PUT;
    if (m == "PATCH")   return HTTP_PATCH;
    if (m == "HEAD")    return HTTP_HEAD;
    if (m == "OPTIONS") return HTTP_OPTIONS;
    return 0;
}

static void dispatch(const httplib::Request& hreq, httplib::Response& hres) {
    std::lock_guard<std::recursive_mutex> g(gSimLock);
    if (!sFirmwareServer) { hres.status = 503; hres.set_content("firmware not started", "text/plain"); return; }

    AsyncWebServerRequest req;
    req.path_   = hreq.path;
    req.method_ = methodOf(hreq.method);
    for (auto& h : hreq.headers) req.headers_[lower(h.first)] = h.second;

    // Query string -> GET params; body -> POST params. Kept apart, as on the device.
    size_t q = hreq.target.find('?');
    if (q != std::string::npos) {
        httplib::Params qp;
        httplib::detail::parse_query_text(hreq.target.substr(q + 1), qp);
        for (auto& p : qp) req.params_.emplace_back(p.first.c_str(), p.second.c_str(), false);
    }
    if (hreq.get_header_value("Content-Type").find("application/x-www-form-urlencoded") == 0) {
        httplib::Params bp;
        httplib::detail::parse_query_text(hreq.body, bp);
        for (auto& p : bp) req.params_.emplace_back(p.first.c_str(), p.second.c_str(), true);
    } else if (hreq.is_multipart_form_data()) {
        for (auto& f : hreq.form.fields)
            req.params_.emplace_back(f.first.c_str(), f.second.content.c_str(), true);
    }

    bool handled = false;
    for (auto& r : sFirmwareServer->routes_) {
        if (!(r.method & req.method_) || !uriMatches(r.uri, req.path_)) continue;
        if (r.upload && hreq.is_multipart_form_data()) {
            // One chunk per file: the device streams it in pieces, but the
            // handlers only branch on index == 0 and append the rest.
            for (auto& f : hreq.form.files) {
                const std::string& c = f.second.content;
                r.upload(&req, String(f.second.filename.c_str()), 0,
                         (uint8_t*)c.data(), c.size(), true);
            }
        }
        r.fn(&req);
        handled = true;
        break;
    }
    if (!handled && req.method_ == HTTP_GET) {
        for (auto* src : sFirmwareServer->events_)
            if (src->url() == req.path_) { serveEvents(src, hres); return; }
    }
    if (!handled && sFirmwareServer->notFound_) sFirmwareServer->notFound_(&req);

    if (!req.sent_) {
        Serial.printf("[sim] WARNING: %s %s sent no response\n", hreq.method.c_str(), req.path_.c_str());
        hres.status = 500;
        hres.set_content("handler sent no response (on the device this request would hang)", "text/plain");
        return;
    }
    auto& r = *req.response_;
    hres.status = r.code_;
    for (auto& h : DefaultHeaders::Instance().headers_) hres.set_header(h.first, h.second);
    for (auto& h : r.headers_) hres.set_header(h.first, h.second);
    if (!r.body_.empty() || !r.type_.empty())
        hres.set_content(r.body_, r.type_.empty() ? "text/plain" : r.type_);
}

void simServeFirmware(httplib::Server& svr) {
    // Everything except /_sim/ goes to the firmware's own router.
    const char* pat = R"(^/(?!_sim(/|$)).*)";
    svr.Get(pat, dispatch);
    svr.Post(pat, dispatch);
    svr.Options(pat, dispatch);
}

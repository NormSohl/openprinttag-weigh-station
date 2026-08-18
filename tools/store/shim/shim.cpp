// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Host shim implementation. See README.md.
#include "Arduino.h"
#include "LittleFS.h"
#include "Preferences.h"
#include <chrono>
#include <map>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <dirent.h>

SerialShim   Serial;
LittleFSShim LittleFS;

// Defined in main.cpp on the device; store.cpp reads it for LOGSTATS.
// False here, which is the honest default: a host run has no NTP either.
volatile bool gClockSet = false;

static std::string root() {
    const char* r = getenv("STORE_TEST_ROOT");
    return r ? r : "fsroot";
}
std::string fsPath(const char* p) { return root() + (p[0] == '/' ? "" : "/") + p; }

uint32_t millis() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

// ── LittleFS ──────────────────────────────────────────────────────────────────
// mingw's mkdir() takes only the path (no mode); POSIX takes (path, mode).
#ifdef _WIN32
static inline int shimMkdir(const char* p) { return ::mkdir(p); }
#else
static inline int shimMkdir(const char* p) { return ::mkdir(p, 0755); }
#endif

static void mkdirp(const std::string& d) {
    std::string cur;
    for (size_t i = 0; i <= d.size(); i++) {
        if (i == d.size() || d[i] == '/') { if (!cur.empty()) shimMkdir(cur.c_str()); }
        if (i < d.size()) cur += d[i];
    }
}
bool LittleFSShim::begin(bool) { mkdirp(root()); return true; }
bool LittleFSShim::exists(const char* p) {
    struct stat st; return ::stat(fsPath(p).c_str(), &st) == 0;
}
File LittleFSShim::open(const char* p, const char* mode) {
    const std::string full = fsPath(p);
    // Create the parent directory the way mkdir() would have on the device.
    size_t slash = full.find_last_of('/');
    if (slash != std::string::npos) mkdirp(full.substr(0, slash));
    FILE* f = fopen(full.c_str(), mode);
    if (!f) return File();
    long sz = 0;
    struct stat st;
    if (::stat(full.c_str(), &st) == 0) sz = (long)st.st_size;
    return File(f, sz);
}
bool LittleFSShim::remove(const char* p) { return ::remove(fsPath(p).c_str()) == 0; }
bool LittleFSShim::rename(const char* a, const char* b) {
    return ::rename(fsPath(a).c_str(), fsPath(b).c_str()) == 0;
}
bool LittleFSShim::mkdir(const char* p) { mkdirp(fsPath(p)); return true; }
size_t LittleFSShim::usedBytes() {
    size_t n = 0;
    std::string logdir = root() + "/log";
    if (DIR* d = opendir(logdir.c_str())) {
        while (dirent* e = readdir(d)) {
            struct stat st;
            if (::stat((logdir + "/" + e->d_name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
                n += (size_t)st.st_size;
        }
        closedir(d);
    }
    return n;
}

// ── Preferences ───────────────────────────────────────────────────────────────
// One file per namespace, "key=value" lines. Persists across runs, like NVS
// across power cycles — which is exactly the divergence reconcileIdCounter_()
// exists to repair, so it must not be faked away.
static std::string nvsPath(const std::string& ns) { return root() + "/nvs." + ns; }

bool Preferences::begin(const char* ns, bool) { ns_ = ns; mkdirp(root()); return true; }
void Preferences::end() { ns_.clear(); }

// Stored as strings regardless of the typed accessor used to write them —
// real NVS is mixed-type per namespace too, and this keeps one file format
// for whichever of getUInt/getBool/getString a caller reaches for.
static std::map<std::string, std::string> nvsLoad(const std::string& ns) {
    std::map<std::string, std::string> m;
    std::ifstream in(nvsPath(ns));
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos)
            m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}
static void nvsSave(const std::string& ns, const std::map<std::string, std::string>& m) {
    std::ofstream out(nvsPath(ns), std::ios::trunc);
    for (auto& kv : m) out << kv.first << "=" << kv.second << "\n";
}
bool Preferences::isKey(const char* key) {
    auto m = nvsLoad(ns_);
    return m.find(key) != m.end();
}
uint32_t Preferences::getUInt(const char* key, uint32_t def) {
    auto m = nvsLoad(ns_);
    auto it = m.find(key);
    return it == m.end() ? def : (uint32_t)strtoul(it->second.c_str(), nullptr, 10);
}
void Preferences::putUInt(const char* key, uint32_t v) {
    auto m = nvsLoad(ns_);
    m[key] = std::to_string(v);
    nvsSave(ns_, m);
}
bool Preferences::getBool(const char* key, bool def) {
    auto m = nvsLoad(ns_);
    auto it = m.find(key);
    return it == m.end() ? def : (it->second == "1");
}
void Preferences::putBool(const char* key, bool v) {
    auto m = nvsLoad(ns_);
    m[key] = v ? "1" : "0";
    nvsSave(ns_, m);
}
String Preferences::getString(const char* key, const char* def) {
    auto m = nvsLoad(ns_);
    auto it = m.find(key);
    return String(it == m.end() ? def : it->second.c_str());
}
void Preferences::putString(const char* key, const String& v) {
    auto m = nvsLoad(ns_);
    m[key] = v.c_str();
    nvsSave(ns_, m);
}

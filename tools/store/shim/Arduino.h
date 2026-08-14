// Host shim: just enough Arduino for src/store.cpp to compile and run natively.
// See tools/store/README.md for why this exists.
#pragma once
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>

// strlcpy/strlcat are BSD; glibc only gained them in 2.38. Provide our own
// everywhere they are missing (older glibc, mingw, macOS). The __GLIBC_PREREQ
// test must be nested inside defined(__GLIBC__) — on mingw that macro does not
// exist, and referencing it in the #if is a hard preprocessor error.
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 38)
#    define OPT_SHIM_HAVE_STRLCPY 1
#  endif
#endif
#ifndef OPT_SHIM_HAVE_STRLCPY
static inline size_t strlcpy(char* d, const char* s, size_t n) {
    size_t sl = strlen(s);
    if (n) { size_t c = (sl >= n) ? n - 1 : sl; memcpy(d, s, c); d[c] = 0; }
    return sl;
}
static inline size_t strlcat(char* d, const char* s, size_t n) {
    size_t dl = strnlen(d, n), sl = strlen(s);
    if (dl == n) return n + sl;
    if (sl < n - dl) { memcpy(d + dl, s, sl + 1); }
    else { memcpy(d + dl, s, n - dl - 1); d[n - 1] = 0; }
    return dl + sl;
}
#endif

// mingw has no gmtime_r (store.cpp uses it); it does have the C11 gmtime_s,
// whose argument order is the reverse. Bridge it so src/ stays unchanged.
#ifdef _WIN32
#include <ctime>
static inline struct tm* gmtime_r(const time_t* t, struct tm* out) {
    return gmtime_s(out, t) == 0 ? out : nullptr;
}
#endif

// ── String ────────────────────────────────────────────────────────────────────
// Arduino's String over std::string. Only the members store.cpp uses.
class String {
public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    String(const std::string& s) : s_(s) {}
    String(char c) : s_(1, c) {}
    explicit String(int v)      { char b[24]; snprintf(b, sizeof(b), "%d", v);  s_ = b; }
    explicit String(unsigned v) { char b[24]; snprintf(b, sizeof(b), "%u", v);  s_ = b; }
    explicit String(long v)     { char b[24]; snprintf(b, sizeof(b), "%ld", v); s_ = b; }
    String(double v, int dp)    { char b[40]; snprintf(b, sizeof(b), "%.*f", dp, v); s_ = b; }

    const char* c_str() const { return s_.c_str(); }
    size_t length() const     { return s_.size(); }
    void   reserve(size_t n)  { s_.reserve(n); }
    char   operator[](size_t i) const { return s_[i]; }

    String& operator+=(const String& o) { s_ += o.s_; return *this; }
    String& operator+=(const char* o)   { s_ += o;    return *this; }
    String& operator+=(char c)          { s_ += c;    return *this; }

    bool operator==(const char* o) const { return s_ == o; }
    bool operator==(const String& o) const { return s_ == o.s_; }
    bool operator!=(const char* o) const { return s_ != o; }

    // Arduino's trim() mutates in place and strips ASCII whitespace.
    void trim() {
        size_t b = s_.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) { s_.clear(); return; }
        size_t e = s_.find_last_not_of(" \t\r\n");
        s_ = s_.substr(b, e - b + 1);
    }
    void toUpperCase() { for (auto& c : s_) c = (char)toupper((unsigned char)c); }
    String substring(size_t from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
    String substring(size_t from, size_t to) const {
        if (from >= s_.size() || to <= from) return String();
        if (to > s_.size()) to = s_.size();
        return String(s_.substr(from, to - from));
    }
    int indexOf(const char* needle) const {
        size_t p = s_.find(needle);
        return p == std::string::npos ? -1 : (int)p;
    }
    int indexOf(char c, size_t from = 0) const {
        size_t p = s_.find(c, from);
        return p == std::string::npos ? -1 : (int)p;
    }
    long  toInt()   const { return strtol(s_.c_str(), nullptr, 10); }
    float toFloat() const { return strtof(s_.c_str(), nullptr); }
    bool  startsWith(const char* p) const { return s_.rfind(p, 0) == 0; }
    bool  equalsIgnoreCase(const char* o) const { return strcasecmp(s_.c_str(), o) == 0; }

    // ArduinoJson's ArduinoStringWriter calls concat() when serialising into
    // a String. store.cpp never serialises that way, but the template is
    // instantiated regardless, so it has to exist.
    // NUL-terminated form, which is the one it actually calls.
    size_t concat(const char* p) { size_t n = strlen(p); s_.append(p, n); return n; }

    const std::string& std_str() const { return s_; }
private:
    std::string s_;
};

inline String operator+(const String& a, const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, const char* b)   { String r(a); r += b; return r; }
inline String operator+(const char* a,   const String& b) { String r(a); r += b; return r; }
inline String operator+(const String& a, char b)          { String r(a); r += b; return r; }

// ── Serial ────────────────────────────────────────────────────────────────────
struct SerialShim {
    void printf(const char* f, ...) {
        va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap);
    }
    void println(const char* s = "") { ::printf("%s\n", s); }
    void println(const String& s)    { ::printf("%s\n", s.c_str()); }
    void print(const char* s)        { ::printf("%s", s); }
    void flush()                     { fflush(stdout); }
};
extern SerialShim Serial;

uint32_t millis();

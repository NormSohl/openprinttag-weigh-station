#include "api_key.h"
#include <Preferences.h>

// Cached in RAM: the guard runs on every mutating request and NVS reads are
// far too slow to sit in that path.
static String sKey;

void apiKeyBegin() {
    // Read-write so a fresh device creates the namespace quietly. Read-only
    // open of a namespace that does not exist yet logs an ESP_LOGE at boot,
    // which reads as a fault on an otherwise clean log.
    Preferences p;
    p.begin("api", false);
    // isKey() first. getString() on a missing key logs
    //   [E][Preferences.cpp:483] getString(): nvs_get_str len fail: key NOT_FOUND
    // and then returns the default anyway — so the common case, a station with
    // no API key set, printed an ESP_LOGE on every single boot for a condition
    // that is entirely normal. Same shape as LittleFS.exists() logging the
    // error it was meant to prevent: ask whether the thing is there before
    // reading it, where the API lets you.
    sKey = p.isKey("key") ? p.getString("key", "") : String("");
    p.end();
}

bool   apiKeyIsSet() { return sKey.length() > 0; }
String apiKeyGet()   { return sKey; }

void apiKeySet(const char* key) {
    sKey = key ? key : "";
    sKey.trim();
    Preferences p;
    p.begin("api", false);
    if (sKey.length()) p.putString("key", sKey);
    else               p.remove("key");
    p.end();
}

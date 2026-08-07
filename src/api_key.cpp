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
    sKey = p.getString("key", "");
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

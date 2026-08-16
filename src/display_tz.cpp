#include "display_tz.h"
#include <Preferences.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

// US zones covering every realistic deployment plus UTC as an explicit
// escape hatch. "/3" on the DST end rules matches the ESP32 examples this
// was cross-checked against -- the transition instant is expressed in the
// zone's OWN (pre-transition) wall clock, so a 2 AM standard-time fall-back
// reads as "M11.1.0/3" (3 AM daylight time, the same instant).
static const TzZone kZones[] = {
    { "pacific",  "Pacific (Seattle, LA)",      "PST8PDT,M3.2.0,M11.1.0/3" },
    { "mountain", "Mountain (Denver)",          "MST7MDT,M3.2.0,M11.1.0/3" },
    { "arizona",  "Mountain, no DST (Phoenix)", "MST7" },
    { "central",  "Central (Chicago)",          "CST6CDT,M3.2.0,M11.1.0/3" },
    { "eastern",  "Eastern (New York)",         "EST5EDT,M3.2.0,M11.1.0/3" },
    { "alaska",   "Alaska (Anchorage)",         "AKST9AKDT,M3.2.0,M11.1.0/3" },
    { "hawaii",   "Hawaii (no DST)",            "HST10" },
    { "utc",      "UTC",                        "UTC0" },
};
static const size_t kZoneCount  = sizeof(kZones) / sizeof(kZones[0]);
static const char*  kDefaultId  = "pacific";   // Seattle Makers

size_t        tzZoneCount()        { return kZoneCount; }
const TzZone& tzZoneAt(size_t i)   { return kZones[i]; }

static String sId   = kDefaultId;
static bool   sH24  = false;   // 12-hour AM/PM by default

static const TzZone* find(const char* id) {
    for (size_t i = 0; i < kZoneCount; i++)
        if (!strcmp(kZones[i].id, id)) return &kZones[i];
    return nullptr;
}

void tzApply() {
    const TzZone* z = find(sId.c_str());
    if (!z) z = find(kDefaultId);   // stale/unknown id somehow in sId — fall back rather than a null deref
    setenv("TZ", z->posix, 1);
    tzset();
}

void tzBegin() {
    // Read-write so a fresh device creates the namespace quietly, same
    // reasoning as apiKeyBegin(): a read-only open of a namespace that
    // doesn't exist yet logs an ESP_LOGE at boot for a condition that is
    // completely normal on first boot.
    Preferences p;
    p.begin("tz", false);
    sId  = p.isKey("id")  ? p.getString("id", kDefaultId) : String(kDefaultId);
    sH24 = p.isKey("h24") ? p.getBool("h24", false)        : false;
    p.end();
    if (!find(sId.c_str())) sId = kDefaultId;   // stale id from a table edit
    tzApply();
}

bool tzGet24Hour() { return sH24; }

void tzSet24Hour(bool h24) {
    sH24 = h24;
    Preferences p;
    p.begin("tz", false);
    p.putBool("h24", sH24);
    p.end();
    // No apply step: nothing to re-derive from the OS clock state, unlike
    // tzSet() -- callers (displayTask, storeLocalizeIso()) just read
    // tzGet24Hour() fresh each time they format a string.
}

bool tzSet(const char* id) {
    if (!find(id)) return false;
    sId = id;
    Preferences p;
    p.begin("tz", false);
    p.putString("id", sId);
    p.end();
    tzApply();   // takes effect immediately, no reboot needed
    return true;
}

const char* tzGetId() { return sId.c_str(); }

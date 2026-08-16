#pragma once
#include <Arduino.h>

// The station's LOCAL display timezone -- affects only how timestamps are
// SHOWN (the TFT corner clock, and web-app timestamps via
// storeLocalizeIso()). What gets LOGGED never changes: storeNowIso() always
// writes UTC via gmtime_r(), independent of this setting, so log lines stay
// unambiguous and string-sortable across a DST transition -- the popularity
// window math, the audit "found" comparison, and storeCompact()'s retention
// floor all depend on that property. Only localtime_r() (which this drives
// via setenv("TZ", ...)/tzset()) is affected.
//
// A curated list, not a free-text POSIX TZ string or an IANA tzdata picker:
// this firmware carries no tzdata database, and a lab volunteer typing
// "PST8PDT,M3.2.0,M11.1.0/3" by hand is one typo away from silently
// breaking DST for six months. Add a row to the table in display_tz.cpp if a
// station ever deploys outside the continental US zones + Alaska/Hawaii/UTC
// covered there.

struct TzZone {
    const char* id;      // stored in NVS, matched against the table
    const char* label;   // shown in the Settings picker
    const char* posix;   // POSIX TZ string, e.g. "PST8PDT,M3.2.0,M11.1.0/3"
};

size_t        tzZoneCount();
const TzZone& tzZoneAt(size_t i);

void        tzBegin();               // load from NVS (or default) and apply; call once at boot
void        tzApply();               // re-apply the currently loaded zone (setenv+tzset)
bool        tzSet(const char* id);   // validate against the table, persist, apply. false if unknown
const char* tzGetId();               // current zone id, e.g. "pacific"

// Clock format (12-hour AM/PM by default, 24-hour opt-in) -- lives here
// rather than its own module because it is set from the same Settings-page
// card as the timezone, and both are read by the same displays (TFT corner
// clock, storeLocalizeIso()). Loaded by tzBegin(), no separate begin call.
bool tzGet24Hour();
void tzSet24Hour(bool h24);

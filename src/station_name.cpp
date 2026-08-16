#include "station_name.h"
#include <Preferences.h>

static const char* kDefault = "Seattle Makers";
static String       sName    = kDefault;

void stationNameBegin() {
    // Read-write so a fresh device creates the namespace quietly, same
    // reasoning as apiKeyBegin()/tzBegin(): a read-only open of a namespace
    // that doesn't exist yet logs an ESP_LOGE at boot for a condition that
    // is completely normal on first boot.
    Preferences p;
    p.begin("station", false);
    sName = p.isKey("name") ? p.getString("name", kDefault) : String(kDefault);
    p.end();
    // Stale/corrupt value on disk (e.g. from a firmware downgrade after
    // STATION_NAME_MAX_LEN shrank) -- fall back rather than draw it.
    if (sName.length() == 0 || sName.length() > STATION_NAME_MAX_LEN) sName = kDefault;
}

const char* stationNameGet() { return sName.c_str(); }

bool stationNameSet(const char* name) {
    if (!name) return false;
    String n(name);
    n.trim();
    if (n.length() == 0 || n.length() > STATION_NAME_MAX_LEN) return false;
    sName = n;
    Preferences p;
    p.begin("station", false);
    p.putString("name", sName);
    p.end();
    return true;
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#include "last_onboard.h"
#include <Preferences.h>

static String sVendor, sMaterial, sColor, sProfile;

// Read-write, same reasoning as station_name.cpp/api_key.cpp: a read-only
// open of a namespace that doesn't exist yet (first boot) logs an ESP_LOGE
// for a condition that is completely normal.
static String loadOne(Preferences& p, const char* key) {
    return p.isKey(key) ? p.getString(key, "") : String();
}

void lastOnboardBegin() {
    Preferences p;
    p.begin("last_ob", false);
    sVendor   = loadOne(p, "vendor");
    sMaterial = loadOne(p, "material");
    sColor    = loadOne(p, "color");
    sProfile  = loadOne(p, "profile");
    p.end();
}

const char* lastOnboardVendor()   { return sVendor.c_str(); }
const char* lastOnboardMaterial() { return sMaterial.c_str(); }
const char* lastOnboardColor()    { return sColor.c_str(); }
const char* lastOnboardProfile()  { return sProfile.c_str(); }

void lastOnboardSet(const char* vendor, const char* material,
                     const char* color, const char* profile) {
    sVendor   = vendor   ? vendor   : "";
    sMaterial = material ? material : "";
    sColor    = color    ? color    : "";
    sProfile  = profile  ? profile  : "";
    Preferences p;
    p.begin("last_ob", false);
    p.putString("vendor",   sVendor);
    p.putString("material", sMaterial);
    p.putString("color",    sColor);
    p.putString("profile",  sProfile);
    p.end();
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#include "last_stock.h"
#include <Preferences.h>

static String sVendor, sMaterial, sColor;

// Read-write, same reasoning as last_onboard.cpp/station_name.cpp: a
// read-only open of a namespace that doesn't exist yet (first boot) logs an
// ESP_LOGE for a condition that is completely normal.
static String loadOne(Preferences& p, const char* key) {
    return p.isKey(key) ? p.getString(key, "") : String();
}

void lastStockBegin() {
    Preferences p;
    p.begin("last_stk", false);
    sVendor   = loadOne(p, "vendor");
    sMaterial = loadOne(p, "material");
    sColor    = loadOne(p, "color");
    p.end();
}

const char* lastStockVendor()   { return sVendor.c_str(); }
const char* lastStockMaterial() { return sMaterial.c_str(); }
const char* lastStockColor()    { return sColor.c_str(); }

void lastStockSet(const char* vendor, const char* material, const char* color) {
    sVendor   = vendor   ? vendor   : "";
    sMaterial = material ? material : "";
    sColor    = color    ? color    : "";
    Preferences p;
    p.begin("last_stk", false);
    p.putString("vendor",   sVendor);
    p.putString("material", sMaterial);
    p.putString("color",    sColor);
    p.end();
}

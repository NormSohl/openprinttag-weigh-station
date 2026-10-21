// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#include "last_onboard.h"
#include <Preferences.h>

static String sVendor, sMaterial, sColor;
static String sCatVendor, sCatType, sCatPkg;

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
    sCatVendor = loadOne(p, "cat_vendor");
    sCatType   = loadOne(p, "cat_type");
    sCatPkg    = loadOne(p, "cat_pkg");
    p.end();
}

const char* lastOnboardVendor()   { return sVendor.c_str(); }
const char* lastOnboardMaterial() { return sMaterial.c_str(); }
const char* lastOnboardColor()    { return sColor.c_str(); }

void lastOnboardSet(const char* vendor, const char* material,
                     const char* color) {
    sVendor   = vendor   ? vendor   : "";
    sMaterial = material ? material : "";
    sColor    = color    ? color    : "";
    Preferences p;
    p.begin("last_ob", false);
    p.putString("vendor",   sVendor);
    p.putString("material", sMaterial);
    p.putString("color",    sColor);
    p.end();
}

const char* lastOnboardCatVendor()  { return sCatVendor.c_str(); }
const char* lastOnboardCatType()    { return sCatType.c_str(); }
const char* lastOnboardCatPackage() { return sCatPkg.c_str(); }

void lastOnboardSetCatalog(const char* vendorSlug, const char* typeTok,
                           const char* pkgPath) {
    sCatVendor = vendorSlug ? vendorSlug : "";
    sCatType   = typeTok    ? typeTok    : "";
    sCatPkg    = pkgPath    ? pkgPath    : "";
    Preferences p;
    p.begin("last_ob", false);
    p.putString("cat_vendor", sCatVendor);
    p.putString("cat_type",   sCatType);
    p.putString("cat_pkg",    sCatPkg);
    p.end();
}

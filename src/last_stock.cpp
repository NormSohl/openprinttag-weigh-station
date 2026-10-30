// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#include "last_stock.h"
#include <Preferences.h>

static String sVendor, sMaterial, sColor;
static String sCatVendor, sCatType, sCatPkg;

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
    sCatVendor = loadOne(p, "cat_vendor");
    sCatType   = loadOne(p, "cat_type");
    sCatPkg    = loadOne(p, "cat_pkg");
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

const char* lastStockCatVendor()  { return sCatVendor.c_str(); }
const char* lastStockCatType()    { return sCatType.c_str(); }
const char* lastStockCatPackage() { return sCatPkg.c_str(); }

void lastStockSetCatalog(const char* vendorSlug, const char* typeTok, const char* pkgPath) {
    sCatVendor = vendorSlug ? vendorSlug : "";
    sCatType   = typeTok    ? typeTok    : "";
    sCatPkg    = pkgPath    ? pkgPath    : "";
    Preferences p;
    p.begin("last_stk", false);
    p.putString("cat_vendor", sCatVendor);
    p.putString("cat_type",   sCatType);
    p.putString("cat_pkg",    sCatPkg);
    p.end();
}

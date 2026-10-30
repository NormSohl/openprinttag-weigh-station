// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include <Arduino.h>

// Remembers the last Vendor/Material/Color chosen on the Stock List page's
// manual-entry picklists, same reasoning and shape as last_onboard.h — a
// separate module, not shared state, because Stock List and Onboarding are
// different workflows that can reasonably be adding different filament at
// different times (a morning of onboarding one vendor's new shipment is not
// the same session as topping up a Stock List threshold for something else
// entirely).
//
// The catalog search has its own memory here too (lastStockCat*): the
// catalog's own keys, replayed by the page exactly like last_onboard.h's.
// Until a Stock List catalog pick is saved, the page falls back to
// onboarding's last pick (2026-10-30: before this, the Stock List only ever
// replayed onboarding's, so adding PLA then PETG kept offering PLA).
void        lastStockBegin();   // load from NVS; call once at boot
const char* lastStockVendor();
const char* lastStockMaterial();
const char* lastStockColor();
// Called once a manual-entry stock item resolves its final vendor/material/
// colour names -- after any "+ Add new ..." entry has already been created,
// so this always records a name that exists in the local catalog.
void lastStockSet(const char* vendor, const char* material, const char* color);

const char* lastStockCatVendor();    // catalog brand slug
const char* lastStockCatType();      // catalog type token
const char* lastStockCatPackage();   // catalog package path; empty = none yet
void lastStockSetCatalog(const char* vendorSlug, const char* typeTok, const char* pkgPath);

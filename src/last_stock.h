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
// Deliberately scoped to the manual-entry path only: a local-product pick or
// an OpenPrintTag catalog search resolves everything from a product lookup
// with no use for a remembered default, and doesn't route through these
// fields at all.
void        lastStockBegin();   // load from NVS; call once at boot
const char* lastStockVendor();
const char* lastStockMaterial();
const char* lastStockColor();
// Called once a manual-entry stock item resolves its final vendor/material/
// colour names -- after any "+ Add new ..." entry has already been created,
// so this always records a name that exists in the local catalog.
void lastStockSet(const char* vendor, const char* material, const char* color);

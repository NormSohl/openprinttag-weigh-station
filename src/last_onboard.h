// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include <Arduino.h>

// Remembers the last Vendor/Material/Color chosen on the
// Onboard page's manual-entry picklists, so the NEXT onboarding's dropdowns
// default to them instead of the browser's default (whichever option happens
// to sort first in the local catalog). Streamlines onboarding a batch of new
// stock that just arrived from one supplier -- the next spool pulled out of
// the box is usually the same vendor and material as the one just finished,
// and often the same colour too.
//
// Not the spool profile: that picker deliberately starts on a neutral
// "choose" option (see handleOnboardForm). A preselected profile that had not
// filled the tare box read as though it had been applied. The "profile" key
// may still sit in NVS from older firmware; nothing reads it.
//
// These three are the manual-entry picklists only. The catalog search has its
// own remembered pick below (lastOnboardCat*); "another spool of X" resolves
// everything from the product and uses neither.
void        lastOnboardBegin();   // load from NVS; call once at boot
const char* lastOnboardVendor();
const char* lastOnboardMaterial();
const char* lastOnboardColor();
// Called once a manual-entry onboarding resolves its final vendor/material/
// colour names -- after any "+ Add new ..." entry has already been
// created, so this always records a name that exists in the local catalog.
void lastOnboardSet(const char* vendor, const char* material,
                     const char* color);

// The last OpenPrintTag catalog pick, so the catalog search on the next
// onboarding starts on it (on direct user request, 2026-10-20): the next spool
// out of a box is usually the same product. Stored as the catalog's own keys —
// the vendor's brand slug, the type token ("" = all types) and the package
// path — because the browser replays the three picks against the live
// catalog; a display name could not be looked up there. Saved only by a
// successful catalog-path onboard; an empty package path means "nothing
// remembered". Station-wide in NVS rather than per browser, so every device
// at the station sees the same last pick.
const char* lastOnboardCatVendor();
const char* lastOnboardCatType();
const char* lastOnboardCatPackage();
void lastOnboardSetCatalog(const char* vendorSlug, const char* typeTok,
                           const char* pkgPath);

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include <Arduino.h>

// Remembers the last Vendor/Material/Color/Spool-profile chosen on the
// Onboard page's manual-entry picklists, so the NEXT onboarding's dropdowns
// default to them instead of the browser's default (whichever option happens
// to sort first in the local catalog). Streamlines onboarding a batch of new
// stock that just arrived from one supplier -- the next spool pulled out of
// the box is usually the same vendor and material as the one just finished,
// and often the same colour and container too.
//
// Deliberately scoped to the manual-entry path only: a catalog-search pick or
// "another spool of X" resolves everything from a product/GTIN lookup with no
// use for a remembered default, and doesn't route through these fields at all.
void        lastOnboardBegin();   // load from NVS; call once at boot
const char* lastOnboardVendor();
const char* lastOnboardMaterial();
const char* lastOnboardColor();
const char* lastOnboardProfile();
// Called once a manual-entry onboarding resolves its final vendor/material/
// colour/profile names -- after any "+ Add new ..." entry has already been
// created, so this always records a name that exists in the local catalog.
void lastOnboardSet(const char* vendor, const char* material,
                     const char* color, const char* profile);

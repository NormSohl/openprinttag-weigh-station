// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include <Arduino.h>

// The Idle screen's green heading ("Seattle Makers" by default) -- the one
// place the station greets by its owning org's name. Every other screen
// title is deliberately generic ("Weigh Station", "WiFi Setup", ...), same
// as the web app's own header, so this is the only string that needed to
// become configurable: this firmware is meant to eventually run at more
// than one lab, and a hardcoded name would be exactly backwards for that.
//
// STATION_NAME_MAX_LEN exists because title() has no wrap or clip -- see the
// "Rows 0-1 are full width" note in docs/design/tft-display-states.md. At
// text size 3 (18px/char) this leaves a clear margin on a 480px-wide panel.
#define STATION_NAME_MAX_LEN 20

void        stationNameBegin();               // load from NVS (or default); call once at boot
const char* stationNameGet();                 // never null or empty
bool        stationNameSet(const char* name);  // false: empty, or over STATION_NAME_MAX_LEN chars

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include <Arduino.h>

// Shared secret guarding the web app's state-changing endpoints.
//
// Threat model, stated plainly: the station sits on a lab LAN and has no
// physical controls, so this is not trying to stop a determined attacker with
// network access. It stops the things that actually go wrong — a script aimed
// at the wrong host, someone poking endpoints out of curiosity, and a browser
// following a link that triggers a destructive action. Read-only endpoints stay
// open so dashboards and scrapers need no credentials.
//
// **Unset means open.** A device in a cabinet with no BOOT button access must
// never be able to lock its operators out, so an empty key disables the check
// and the web app says so. Set one via the Config page or `APIKEY <secret>`
// over serial.
//
// Accepted three ways, so nothing is awkward to call:
//   X-API-Key: <secret>          header  (scripts; no CORS credential rules)
//   ?key=<secret>                query   (quick curl / browser testing)
//   HTTP Basic, any username     header  (curl -u, and browsers prompt for it)

void   apiKeyBegin();               // load from NVS; call once at boot
bool   apiKeyIsSet();
String apiKeyGet();                 // "" when unset
void   apiKeySet(const char* key);  // "" or nullptr clears it (reopens the API)

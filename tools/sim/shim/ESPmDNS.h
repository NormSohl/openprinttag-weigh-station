// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Host shim: mDNS is a no-op; the sim is reached at localhost.
#pragma once
#include "Arduino.h"

struct MdnsShim {
    bool begin(const char*) { return true; }
    void addService(const char*, const char*, uint16_t) {}
};
extern MdnsShim MDNS;

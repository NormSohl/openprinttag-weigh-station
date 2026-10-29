// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Host shim: the WiFi / ESP calls src/web_app.cpp makes. Nothing here talks
// to a network; the sim's web server listens on the Mac's own socket. The
// station reports itself as joined to "sim" at 127.0.0.1.
#pragma once
#include "Arduino.h"

#define WL_CONNECTED 3

struct IPAddressShim {
    const char* s;
    String toString() const { return String(s); }
};

struct WiFiShim {
    int           status()   { return WL_CONNECTED; }
    String        SSID()     { return String("sim"); }
    IPAddressShim localIP()  { return {"127.0.0.1"}; }
    IPAddressShim softAPIP() { return {"192.168.4.1"}; }
    int           RSSI()     { return -50; }
    void disconnect(bool = false, bool = false) {
        Serial.println("[sim] WiFi.disconnect() -- credentials would be erased here");
    }
};
extern WiFiShim WiFi;

struct EspShim {
    // The device reboots into the captive portal. The sim keeps running, so
    // a page that triggers this still gets its response.
    void     restart()     { Serial.println("[sim] ESP.restart() -- ignored"); }
    // A fixed, plausible figure. The sim cannot say anything about the
    // ESP32's real heap -- that is what the bench is for.
    uint32_t getFreeHeap() { return 200000; }
};
extern EspShim ESP;

void delay(uint32_t ms);

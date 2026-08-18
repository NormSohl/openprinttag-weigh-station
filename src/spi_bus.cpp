// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#include <Arduino.h>
#include <SPI.h>
#include <esp32-hal-spi.h>
#include <TFT_eSPI.h>

#include "spi_bus.h"
#include "config.h"

extern SemaphoreHandle_t gSpiMutex;

// Which peripheral currently drives GPIO 11/12. Tracked so a run of
// same-owner transactions (the common case — a screen repaint, a block read)
// costs nothing beyond the mutex.
//
// Guarded by gSpiMutex itself: it is only ever read or written while the mutex
// is held, so no separate synchronisation is needed.
enum class BusOwner : uint8_t { None, Nfc, Tft };
static BusOwner sOwner = BusOwner::None;

// Take gSpiMutex, complaining if it takes unreasonably long.
//
// gSpiMutex is NOT recursive: taking it twice from one task blocks that task
// against itself forever, and because it holds the bus while doing so every
// other SPI user stops too. The device simply freezes — no panic, no backtrace,
// nothing on the wire. That happened once, when writeSection() (called with the
// bus held) grew an inner take.
//
// portMAX_DELAY makes that failure completely silent. Waiting in bounded steps
// and naming the caller turns it into an obvious message, while still blocking
// forever as a correct mutex must — we never proceed without the lock.
static void takeOrComplain(const char* who) {
    while (xSemaphoreTake(gSpiMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        Serial.printf("[spi] %s has waited 5 s for the bus — if this repeats, "
                      "something is holding it (a nested take deadlocks: "
                      "gSpiMutex is not recursive)\n", who);
    }
}

// Re-point the shared output pins at `bus`. spiAttach* are no-ops on a null
// handle, which is what we want before a peripheral has been started.
static inline void routeOutputs(spi_t* bus) {
    if (!bus) return;
    spiAttachSCK(bus, SPI_SCK);
    spiAttachMOSI(bus, SPI_MOSI);
}

void spiBusTakeNfc() {
    takeOrComplain("nfc");
    if (sOwner == BusOwner::Nfc) return;

    // The PN5180 library talks through the global SPI object.
    spi_t* bus = SPI.bus();
    if (!bus) return;              // SPI.begin() hasn't run yet; leave sOwner
    routeOutputs(bus);
    spiAttachMISO(bus, SPI_MISO);  // only the reader ever reads
    sOwner = BusOwner::Nfc;
}

void spiBusTakeTft() {
    takeOrComplain("tft");
    if (sOwner == BusOwner::Tft) return;

    // Null until tft.init() has run — displayBegin() takes the bus around that
    // call, so the first take legitimately finds no handle. Leaving sOwner as
    // None makes the next take re-route, which is correct either way.
    spi_t* bus = TFT_eSPI::getSPIinstance().bus();
    if (!bus) return;
    routeOutputs(bus);
    sOwner = BusOwner::Tft;
}

void spiBusGive() {
    xSemaphoreGive(gSpiMutex);
}

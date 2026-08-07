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

// Re-point the shared output pins at `bus`. spiAttach* are no-ops on a null
// handle, which is what we want before a peripheral has been started.
static inline void routeOutputs(spi_t* bus) {
    if (!bus) return;
    spiAttachSCK(bus, SPI_SCK);
    spiAttachMOSI(bus, SPI_MOSI);
}

void spiBusTakeNfc() {
    xSemaphoreTake(gSpiMutex, portMAX_DELAY);
    if (sOwner == BusOwner::Nfc) return;

    // The PN5180 library talks through the global SPI object.
    spi_t* bus = SPI.bus();
    if (!bus) return;              // SPI.begin() hasn't run yet; leave sOwner
    routeOutputs(bus);
    spiAttachMISO(bus, SPI_MISO);  // only the reader ever reads
    sOwner = BusOwner::Nfc;
}

void spiBusTakeTft() {
    xSemaphoreTake(gSpiMutex, portMAX_DELAY);
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

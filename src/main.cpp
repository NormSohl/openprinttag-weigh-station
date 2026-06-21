#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"

// ── Shared state ──────────────────────────────────────────────
// Tasks read/write gState under gStateMutex.
volatile DeviceState gState       = DeviceState::Boot;
SemaphoreHandle_t    gStateMutex  = nullptr;

// Latest weight from scaleTask, consumed by syncTask and displayTask.
volatile float    gWeightGrams  = 0.0f;
SemaphoreHandle_t gWeightMutex  = nullptr;

// ── Shared tag data (written by nfcTask, read by syncTask / displayTask) ──────
uint8_t           gTagUid[8]    = {};
OptMeta           gTagMeta      = {};
OptMain           gTagMain      = {};
OptAuxiliary      gTagAux       = {};
SemaphoreHandle_t gTagMutex     = nullptr;

// Write-back requests: syncTask sets these flags; nfcTask clears them after writing.
volatile bool gWriteMainPending = false;
volatile bool gWriteAuxPending  = false;

// Spoolman spool ID for the tag currently on the scale (-1 = none / unknown).
// Set by syncTask; read by displayTask for the "Spool #N" line.
volatile int  gSpoolId             = -1;
// True while the spool's Spoolman record still has needs_onboarding=true.
// Drives the "Registered! Edit in Spoolman" display variant.
volatile bool gSpoolNeedsOnboarding = false;

// ── Task forward declarations (defined in their own .cpp files) ──
void nfcTask(void* param);
void scaleTask(void* param);
void displayTask(void* param);
void syncTask(void* param);

void setup() {
    Serial.begin(115200);
    Wire.begin(I2C_SDA, I2C_SCL);
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    gStateMutex = xSemaphoreCreateMutex();
    gWeightMutex = xSemaphoreCreateMutex();
    gTagMutex = xSemaphoreCreateMutex();

    // Core 1: time-sensitive hardware polling
    xTaskCreatePinnedToCore(nfcTask,     "nfc",     6144, nullptr, 2, nullptr, 1);
    xTaskCreatePinnedToCore(scaleTask,   "scale",   3072, nullptr, 2, nullptr, 1);

    // Core 0: I/O — co-located with the WiFi stack
    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(syncTask,    "sync",    8192, nullptr, 1, nullptr, 0);
}

void loop() {
    vTaskDelay(portMAX_DELAY);  // all work happens in FreeRTOS tasks
}

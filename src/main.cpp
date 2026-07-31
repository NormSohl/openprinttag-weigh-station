#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"
#include "store.h"          // local-storage core (redesign Phase 1)
#include "config_store.h"   // config catalog (redesign Phase 3)
#include "sd_backup.h"      // SD snapshot/restore (redesign Phase 6)

// ── Shared state ──────────────────────────────────────────────
// Tasks read/write gState under gStateMutex.
volatile DeviceState gState       = DeviceState::Boot;
SemaphoreHandle_t    gStateMutex  = nullptr;

// Latest weight from scaleTask, consumed by syncTask and displayTask.
volatile float    gWeightGrams  = 0.0f;
SemaphoreHandle_t gWeightMutex  = nullptr;

// False until a real calibration is stored. displayTask shows an idle-screen
// "not calibrated" banner while false, so the uncalibrated first boot isn't silent.
volatile bool     gScaleCalibrated = false;

// Calibration requests from the web UI (or serial), consumed by scaleTask.
volatile bool     gCalZeroReq  = false;   // tare with an empty scale
volatile float    gCalSetGrams = 0.0f;    // >0: calibrate against this known weight

// SD backup requests from the web UI, consumed by syncTask (keeps all SD I/O on
// one task, off the AsyncTCP callback). Snapshot/restore run when the scale is idle.
volatile bool     gSdSnapshotReq = false;
volatile bool     gSdRestoreReq  = false;

// ── Shared tag data (written by nfcTask, read by syncTask / displayTask) ──────
uint8_t           gTagUid[8]    = {};
OptMeta           gTagMeta      = {};
OptMain           gTagMain      = {};
OptAuxiliary      gTagAux       = {};
SemaphoreHandle_t gTagMutex     = nullptr;

// Write-back requests: syncTask sets these flags; nfcTask clears them after writing.
volatile bool gWriteMainPending = false;
volatile bool gWriteAuxPending  = false;

// SPI bus mutex — nfcTask (Core 1) and displayTask (Core 0) share the bus.
// Both tasks must take this mutex before any SPI transaction.
SemaphoreHandle_t gSpiMutex = nullptr;

// Local spool ID for the tag currently on the scale (-1 = none / unknown).
// Set by syncTask; read by displayTask for the "Spool #N" line.
volatile int  gSpoolId             = -1;
// True while the spool's local record still has needs_onboarding=true.
// Drives the "Registered! add details in web app" display variant.
volatile bool gSpoolNeedsOnboarding = false;

// Web-UI reachability, set by syncTask once the network is up and shown on the
// idle screen so anyone can find the app. gApSsid is non-empty only in SoftAP
// fallback (join that SSID, then browse to gWebAddr).
char gWebAddr[48] = {};
char gApSsid[24]  = {};

// ── Task forward declarations (defined in their own .cpp files) ──
void nfcTask(void* param);
void scaleTask(void* param);
void displayTask(void* param);
void syncTask(void* param);

// Boot progress marker. Each init step announces itself BEFORE it runs, so if
// the board hangs the last line printed names the culprit. Flushed immediately
// because a hang would otherwise leave the message sitting in the USB buffer.
static void bootMark(const char* step) {
    Serial.printf("[boot] %s\n", step);
    Serial.flush();
}

void setup() {
    Serial.begin(115200);
    // Native USB (USB-Serial/JTAG on this board): the host re-enumerates after
    // the post-flash reset and a monitor typically attaches ~1-2 s in, so
    // anything printed before that is lost. Hold here so the first boot is
    // actually observable — the cost is 3 s on a device that runs for months.
    delay(3000);
    Serial.println();
    Serial.println("=== weigh station boot ===");

    bootMark("i2c (Wire.begin)");
    Wire.begin(I2C_SDA, I2C_SCL);

    bootMark("spi (shared PN5180 + TFT bus)");
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    bootMark("mutexes");
    gStateMutex = xSemaphoreCreateMutex();
    gWeightMutex = xSemaphoreCreateMutex();
    gTagMutex = xSemaphoreCreateMutex();
    gSpiMutex = xSemaphoreCreateMutex();

    // Local-storage core (LittleFS log + indices + NVS counter). Headless in
    // Phase 1 — driven via the serial harness (EV / DUMP / REBUILD / …).
    // First boot after an erase formats the FS partition — expect a pause here.
    bootMark("store (mount/format LittleFS + NVS)");
    if (storeBegin())
        Serial.printf("[store] ready: %u spools, %u log lines, next id #%u\n",
                      (unsigned)storeSpoolCount(), (unsigned)storeLogLineCount(),
                      (unsigned)storePeekSpoolId());
    else
        Serial.println("[store] LittleFS mount FAILED");

    // Config catalog (vendors/materials/profiles/colors/stock-items on LittleFS).
    bootMark("config catalog (seeds on first boot)");
    cfgBegin();
    Serial.printf("[cfg] ready: %u vendors, %u materials, %u profiles, %u colors, %u stock\n",
                  (unsigned)cfgVendorCount(), (unsigned)cfgMaterialCount(),
                  (unsigned)cfgProfileCount(), (unsigned)cfgColorCount(),
                  (unsigned)cfgStockCount());

    // SD backup on the display board's card (dedicated 2nd SPI host). Optional —
    // the station runs fine with no card; snapshots begin once one is present.
#if SD_BACKUP_ENABLED
    bootMark("sd (2nd SPI host; skipped fast if no card)");
    if (sdBackupBegin()) {
        SdStatus s; sdGetStatus(s);
        Serial.printf("[sd] card ready: %llu MB free / %llu MB, gen #%u\n",
                      (unsigned long long)(s.freeBytes >> 20),
                      (unsigned long long)(s.cardBytes >> 20), (unsigned)s.generation);
    } else {
        Serial.println("[sd] no card (backup to SD disabled until inserted)");
    }
#else
    Serial.println("[sd] disabled at build time (SD_BACKUP_ENABLED=0)");
#endif

    // Core 1: time-sensitive hardware polling
    bootMark("starting nfcTask (PN5180)");
    xTaskCreatePinnedToCore(nfcTask,     "nfc",     6144, nullptr, 2, nullptr, 1);
    bootMark("starting scaleTask (NAU7802)");
    xTaskCreatePinnedToCore(scaleTask,   "scale",   3072, nullptr, 2, nullptr, 1);

    // Core 0: I/O — co-located with the WiFi stack
    bootMark("starting displayTask (TFT)");
    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);
    bootMark("starting syncTask (WiFi + web)");
    xTaskCreatePinnedToCore(syncTask,    "sync",    8192, nullptr, 1, nullptr, 0);

    bootMark("setup() complete — tasks running");
}

void loop() {
    vTaskDelay(portMAX_DELAY);  // all work happens in FreeRTOS tasks
}

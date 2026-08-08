#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"
#include "store.h"          // local-storage core (redesign Phase 1)
#include "config_store.h"   // config catalog (redesign Phase 3)
#include "api_key.h"        // shared secret for mutating HTTP endpoints

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

// ── Shared tag data (written by nfcTask, read by syncTask / displayTask) ──────
uint8_t           gTagUid[8]    = {};
OptMeta           gTagMeta      = {};
OptMain           gTagMain      = {};
OptAuxiliary      gTagAux       = {};
SemaphoreHandle_t gTagMutex     = nullptr;

// Write-back requests: syncTask sets these flags; nfcTask clears them after writing.
volatile bool gWriteMainPending = false;
volatile bool gWriteAuxPending  = false;

// Set by the TAGFORMAT serial command, consumed by nfcTask: reformat whatever
// tag is present regardless of how it classifies. Recovery for a tag left
// half-written by a format that failed partway — those stop reading as blank
// and stop decoding, so nothing else can get them back.
volatile bool gTagForceFormat = false;

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

// Display/NeoPixel/buzzer hardware init — run on the main task before any task
// starts, so TFT_eSPI and the PN5180 library can't initialise the shared SPI
// bus concurrently from two cores. See displayBegin() in display_task.cpp.
void displayBegin();

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

    // Starts the PN5180's peripheral (SPI2) and gives it GPIO 11/12/13.
    // displayBegin() later hands the same two output pins to the display's
    // peripheral (SPI3) — the S3 lets only one peripheral drive a pin, so from
    // then on ownership is arbitrated per-transaction by spi_bus.cpp. This call
    // just has to happen first so SPI.bus() is a valid handle to route to.
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

    apiKeyBegin();
    Serial.printf("[api] write endpoints are %s\n",
                  apiKeyIsSet() ? "protected by an API key"
                                : "UNPROTECTED (no key set — see APIKEY over serial)");

    Serial.printf("[store] filesystem: %u kB free / %u kB\n",
                  (unsigned)(storeFreeBytes() >> 10), (unsigned)(storeTotalBytes() >> 10));

    // Display first, on this task, while nothing else can touch the SPI bus.
    bootMark("display init (TFT + NeoPixel + buzzer)");
    displayBegin();
    // TFT_eSPI asks the HAL for "no MISO" (TFT_MISO is -1 because the ILI9488's
    // SDO never tri-states), and the S3 HAL has no default MISO pin for HSPI to
    // fall back on, so it logs an error and returns — which is exactly what we
    // want it to do. Say so, because an unexplained [E] in a boot log sends the
    // next person debugging in the wrong direction.
    Serial.println("[display] the 'HSPI Does not have default pins' error above "
                   "is expected — the display is write-only by design");
    bootMark("display ok");

    // Core 1: time-sensitive hardware polling
    bootMark("starting nfcTask (PN5180)");
    xTaskCreatePinnedToCore(nfcTask,     "nfc",     6144, nullptr, 2, nullptr, 1);
    bootMark("starting scaleTask (NAU7802)");
    // 8192, not 3072: the serial harness runs on this task, and SEED / COMPACT /
    // DUMP call into the store. storeAppendEvent -> FS::open -> lfs_dir_fetchmatch
    // -> esp_flash_read is a deep chain on top of the String and JSON work, and
    // 3072 overflowed the stack canary on the first SEED.
    xTaskCreatePinnedToCore(scaleTask,   "scale",   8192, nullptr, 2, nullptr, 1);

    // Core 0: I/O — co-located with the WiFi stack
    bootMark("starting displayTask (TFT)");
    // 6144, not 4096: drawQr() calls into ricmoo's encoder, which builds its
    // working buffers as C99 variable-length arrays on the CALLER'S stack —
    // codewordBytes[101] + isFunctionGridBytes[137] in qrcode_initBytes(), and
    // another result[101] + coeff[20] nested inside performErrorCorrection().
    // That is ~600 bytes of transient on top of the TFT call frames, on a task
    // that used to do nothing deeper than fillRect. scaleTask already cost us
    // one canary panic for exactly this kind of hidden depth.
    xTaskCreatePinnedToCore(displayTask, "display", 6144, nullptr, 1, nullptr, 0);
    bootMark("starting syncTask (WiFi + web)");
    xTaskCreatePinnedToCore(syncTask,    "sync",    8192, nullptr, 1, nullptr, 0);

    bootMark("setup() complete — tasks running");
}

void loop() {
    vTaskDelay(portMAX_DELAY);  // all work happens in FreeRTOS tasks
}

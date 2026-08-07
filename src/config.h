#pragma once

// ── PN5180 SPI ────────────────────────────────────────────────
// Adjust to match your actual wiring.
#define PN5180_NSS    5
#define PN5180_BUSY   6
#define PN5180_RESET  7

// SparkFun Thing Plus ESP32-S3 default SPI pins (board silk: 12/SCK, 11/PICO,
// 13/POCI). GPIO 33-37 are NOT broken out on this board (module flash/PSRAM).
#define SPI_SCK   12
#define SPI_MISO  13
#define SPI_MOSI  11

// ── I²C / Qwiic (NAU7802) ────────────────────────────────────
#define I2C_SDA  8
#define I2C_SCL  9

// ── 3.5" ILI9488 SPI TFT display ─────────────────────────────
// Shares MOSI/SCK with the PN5180, on a DIFFERENT SPI peripheral — see
// spi_bus.h. Take the bus via spiBusTakeTft(), never gSpiMutex directly.
// TFT_CS/TFT_DC/TFT_RST are also passed to TFT_eSPI as -D flags (platformio.ini).
#define TFT_CS   15
#define TFT_DC   16
#define TFT_RST  17

// Landscape orientation. 1 and 3 are both landscape, 180° apart — which one
// reads right-way-up depends purely on how the panel sits in the mount, not on
// the panel itself. Confirmed on the assembled unit: 1 is correct.
// (Set once at init from this constant, so it cannot vary between boots of the
// same binary — a flip across a reboot means the panel moved, not the code.)
#define TFT_ROTATION 1

// ── WS2812 NeoPixel (onboard) ────────────────────────────────
// The external porch NeoPixel is superseded by the TFT display.
// SparkFun Thing Plus ESP32-S3 onboard RGB is GPIO46 (GPIO48 is the onboard
// SD card-detect on this board, not the LED).
#define NEOPIXEL_PIN     46
#define NEOPIXEL_COUNT    1

// ── Passive piezo buzzer ──────────────────────────────────────
#define BUZZER_PIN  14

// ── WiFi reset trigger ────────────────────────────────────────
// Hold the BOOT button (GPIO 0, already on the SparkFun Thing Plus ESP32-S3
// board) for this many milliseconds at power-on to erase stored WiFi
// credentials and force the captive portal to reopen.
#define WIFI_RESET_PIN      0
#define WIFI_RESET_HOLD_MS  3000

// Captive-portal exit policy (enforced in syncTask, not by WiFiManager).
// Two independent limits, because the device is cabinet-installed and the BOOT
// button is inside the case — the portal must always close on its own.
//
//   IDLE: no client associated to the AP. Nobody is setting it up, so fall back
//         to the SoftAP quickly and get the web app reachable.
//   MAX:  absolute cap regardless of clients. Covers the case where someone
//         joins, wanders off, and leaves a phone associated — without this the
//         portal would stay open forever with no way to escape.
#define WIFI_PORTAL_TIMEOUT_SEC 120   // idle: nobody joined
#define WIFI_PORTAL_MAX_SEC     600   // absolute: 10 min, then give up

// ── Web interface ─────────────────────────────────────────────
// mDNS hostname; device is reachable at http://<DEVICE_HOSTNAME>.local/
// The /reset endpoint clears WiFi credentials and reboots into the portal.
#define DEVICE_HOSTNAME  "weighstation"

// Reported by GET /api/status so a fleet dashboard can tell builds apart.
// NB: deliberately NOT called FIRMWARE_VERSION — the PN5180 library uses
// that name for a chip EEPROM register address.
#define FW_VERSION       "1.0.0"

// ── Behaviour constants ───────────────────────────────────────
#define RECONCILE_POLL_MS      1000  // local-store reconciliation cadence (~1 Hz)
// ── Event-log capacity ────────────────────────────────────────
// The log is append-only and never shrinks on its own. A weigh line is about
// 150 bytes, so the 2 MB LittleFS partition holds on the order of 13k events —
// months, not years, at plausible lab volume. Two mechanisms keep that from
// becoming silent data loss:
//
//   - storeAppendEvent() checks every byte it writes, so a full filesystem is
//     reported instead of quietly dropping records.
//   - Above the high-water mark below, syncTask compacts the log while the
//     scale is idle: one checkpoint per spool plus the most recent events.
//
// Sizing: ~2000 retained events (~300 kB) plus a checkpoint per spool
// (~250 bytes each) lands well under the mark, so compaction is rare and each
// run frees a large margin rather than thrashing near the threshold.
#define STORE_LOG_COMPACT_BYTES  (900UL * 1024)  // compact once the log exceeds this
#define STORE_LOG_KEEP_EVENTS         2000       // recent events kept verbatim
#define STORE_FREE_WARN_BYTES    (128UL * 1024)  // warn below this much free space

#define BLANK_TAG_CONFIRM_SEC     5  // countdown before auto-format proceeds
#define NFC_DEBOUNCE_READS        3  // consecutive consistent reads required
#define SCALE_SAMPLES            10  // load cell samples averaged per weighing
// Calibration factor for NAU7802 → grams conversion (raw counts per gram).
// Determine empirically: place a known weight on the scale, read the raw
// average with zero subtracted, divide by the known mass in grams.
// Stored in NVS under the "scale" namespace and updated via serial commands
// (ZERO / CAL <grams>); this constant is only used on first boot.
#define SCALE_CAL_FACTOR         1.0f

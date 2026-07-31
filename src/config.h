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
// Shares MOSI/SCK/MISO with PN5180 — gSpiMutex guards the bus.
// TFT_CS/TFT_DC/TFT_RST are also consumed by User_Setup.h for TFT_eSPI.
#define TFT_CS   15
#define TFT_DC   16
#define TFT_RST  17

// ── microSD on the display board — dedicated second SPI host ──
// The Hosyond ILI9488's SD lines are on a *separate* header, NOT bonded to
// the display SPI bus (see docs/datasheets/display-hosyond-ili9488.md), so
// the SD gets its own SPI host (FSPI via a separate SPIClass) — no gSpiMutex
// traffic, isolated from NFC/TFT I/O. Backup/archive only; the device runs
// fine with no card. Assigned to free header GPIOs on the Thing Plus ESP32-S3.
//
// NOTE for the build: the Thing Plus has its OWN onboard microSD slot on the
// SDIO bus (GPIO 33/34/38/39/40/47, detect 48) — those are NOT broken out. We
// use the *display's* SD instead (front-panel accessible), so LEAVE THE ONBOARD
// SLOT EMPTY. These four are free broken-out header pins (42 = "FREEBIE" spare).
#define SD_SCK   10
#define SD_MOSI  18
#define SD_MISO  21
#define SD_CS    42

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

// ── Web interface ─────────────────────────────────────────────
// mDNS hostname; device is reachable at http://<DEVICE_HOSTNAME>.local/
// The /reset endpoint clears WiFi credentials and reboots into the portal.
#define DEVICE_HOSTNAME  "weighstation"

// ── Behaviour constants ───────────────────────────────────────
#define RECONCILE_POLL_MS      1000  // local-store reconciliation cadence (~1 Hz)
// ── SD backup (Phase 6) ───────────────────────────────────────
// Set to 0 to skip SD bring-up entirely. A FAILED SD.begin() (no card, or
// mis-wired) tears down SPI state on its way out and has been observed to
// leave the *global* SPI bus handle NULL — which then panics TFT_eSPI with a
// null-pointer StoreProhibited inside begin_tft_write(). Until the card is
// wired and verified, leave this off so a missing card can't break the display.
#define SD_BACKUP_ENABLED 0
#define SD_SPI_FREQ_HZ        20000000  // SD SPI clock (20 MHz; drop to 10M if flaky)
#define SD_HISTORY_KEEP             20  // dated snapshots retained under /backup/history
#define SD_SNAPSHOT_MIN_INTERVAL_MS (5*60*1000UL) // auto-snapshot throttle when idle
#define BLANK_TAG_CONFIRM_SEC     5  // countdown before auto-format proceeds
#define NFC_DEBOUNCE_READS        3  // consecutive consistent reads required
#define SCALE_SAMPLES            10  // load cell samples averaged per weighing
// Calibration factor for NAU7802 → grams conversion (raw counts per gram).
// Determine empirically: place a known weight on the scale, read the raw
// average with zero subtracted, divide by the known mass in grams.
// Stored in NVS under the "scale" namespace and updated via serial commands
// (ZERO / CAL <grams>); this constant is only used on first boot.
#define SCALE_CAL_FACTOR         1.0f

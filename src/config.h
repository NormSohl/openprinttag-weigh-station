#pragma once

// ── PN5180 SPI ────────────────────────────────────────────────
// Adjust to match your actual wiring.
#define PN5180_NSS    5
#define PN5180_BUSY   6
#define PN5180_RESET  7

// SparkFun Thing Plus ESP32-S3 default VSPI pins
#define SPI_SCK   36
#define SPI_MISO  37
#define SPI_MOSI  35

// ── I²C / Qwiic (NAU7802 + SSD1306) ──────────────────────────
#define I2C_SDA  8
#define I2C_SCL  9

// ── Onboard WS2812 NeoPixel ───────────────────────────────────
#define NEOPIXEL_PIN    48
#define NEOPIXEL_COUNT   1

// ── Passive piezo buzzer ──────────────────────────────────────
#define BUZZER_PIN  13

// ── WiFi reset trigger ────────────────────────────────────────
// Hold the BOOT button (GPIO 0, already on the SparkFun Thing Plus ESP32-S3
// board) for this many milliseconds at power-on to erase stored WiFi
// credentials and force the captive portal to reopen.
#define WIFI_RESET_PIN      0
#define WIFI_RESET_HOLD_MS  3000

// ── Spoolman ──────────────────────────────────────────────────
#define SPOOLMAN_BASE_URL  "http://spoolman.local:7912"

// ── Behaviour constants ───────────────────────────────────────
#define SPOOLMAN_POLL_MS       1000  // reconciliation cadence (~1 Hz)
#define BLANK_TAG_CONFIRM_SEC     5  // countdown before auto-format proceeds
#define NFC_DEBOUNCE_READS        3  // consecutive consistent reads required
#define SCALE_SAMPLES            10  // load cell samples averaged per weighing
// Calibration factor for NAU7802 → grams conversion (raw counts per gram).
// Determine empirically: place a known weight on the scale, read the raw
// average with zero subtracted, divide by the known mass in grams.
// Stored in NVS under the "scale" namespace and updated via serial commands
// (ZERO / CAL <grams>); this constant is only used on first boot.
#define SCALE_CAL_FACTOR         1.0f

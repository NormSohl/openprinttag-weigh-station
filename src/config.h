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

// storeMaterialPopularity() (Stock List curation) replays raw log lines within
// a trailing window to compute grams-consumed and stockout-corrected available
// days — it has no permanent rollup the way consumption/Usage does, so once a
// line is folded into a Checkpoint (a state SNAPSHOT, not grams or crossing
// history) that line's contribution to popularity is gone. storeCompact()
// therefore refuses to fold anything with a timestamp inside this window,
// regardless of STORE_LOG_KEEP_EVENTS — see the "never fold the popularity
// window" comment in storeCompact(). Shared with the caller-supplied
// `windowDays` on every storeMaterialPopularity() call (web_app.cpp) on
// purpose: the compaction floor and the query window must never drift apart,
// or that guarantee silently stops holding for whichever one moved.
#define STOCK_POPULARITY_WINDOW_DAYS 90

// ── Clock ─────────────────────────────────────────────────────────────────────
// The ESP32-S3 has no battery-backed RTC, so the clock reads 1970 on every
// power-up until SNTP answers. That matters more than it looks: the consumption
// rollup buckets by CALENDAR MONTH, and grams weighed before the first sync
// cannot be attributed to one (periodOf_ files them as "unknown" rather than
// inventing a 1970-01 row).
//
// UTC, deliberately — storeNowIso() formats with gmtime_r and writes a trailing
// "Z", so a local-time offset here would produce timestamps that lie about
// their own zone. Two servers because a station in a cabinet gets no second
// chance to be babysat.
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.nist.gov"

// Display-only local time (the TFT clock corner, and web-app timestamps).
// Everything that gets LOGGED still uses UTC via gmtime_r (see NTP_SERVER_1
// above and storeNowIso()). The configurable zone itself lives in
// display_tz.h/.cpp, picked on the Settings page and persisted to NVS.

#define BLANK_TAG_CONFIRM_SEC     5  // countdown before auto-format proceeds
#define NFC_DEBOUNCE_READS        3  // consecutive consistent reads required
#define SCALE_SAMPLES            10  // load cell samples averaged per weighing
// Calibration factor for NAU7802 → grams conversion (raw counts per gram).
// Determine empirically: place a known weight on the scale, read the raw
// average with zero subtracted, divide by the known mass in grams.
// Stored in NVS under the "scale" namespace and updated via serial commands
// (ZERO / CAL <grams>); this constant is only used on first boot.
#define SCALE_CAL_FACTOR         1.0f

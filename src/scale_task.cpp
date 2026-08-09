#include <Arduino.h>
#include <Preferences.h>
#include <SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>
#include "config.h"
#include "api_key.h"

extern volatile bool gTagForceFormat;
// Task handles, for the STACK command's high-water report (defined in main.cpp).
extern TaskHandle_t gNfcTask, gScaleTask, gDisplayTask, gSyncTask;

// False until the ADC answers. ZERO/CAL are still reachable over serial
// while it is missing, and averaging 32 samples from a chip that is not
// there just wastes time — refuse with a reason instead.
static bool sNauPresent = false;
#include "device_state.h"
#include "opt_tag.h"        // DUMP TAG prints the decoded Main/Aux/Meta
#include "store.h"          // route store test commands (EV / DUMP / …) here too
#include "config_store.h"   // …and CFG commands

extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern OptMain              gTagMain;
extern OptAuxiliary         gTagAux;
extern OptMeta              gTagMeta;
extern SemaphoreHandle_t    gTagMutex;
extern volatile int         gSpoolId;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern volatile bool        gScaleCalibrated;
extern volatile bool        gCalZeroReq;
extern volatile float       gCalSetGrams;

// ── DUMP TAG ──────────────────────────────────────────────────────────────────
// Decode-and-print whatever nfcTask last read off a tag.
//
// This exists because nothing else can see the Auxiliary section. The display
// derives remaining weight from the Main section's tare, and the web app never
// reads consumed_weight at all — so a perfectly correct Present screen says
// nothing about whether the 8-byte Aux map ever landed on the tag. The only
// other evidence was the ABSENCE of "section write FAILED" on the wire, and an
// error that didn't happen is weak proof of a write that did.
//
// Prints the globals rather than re-reading the tag: nfcTask owns the reader,
// and a second reader here would need the SPI bus and the RF field. That means
// the values survive the spool being lifted, so the state is printed alongside
// — stale data is fine to inspect, as long as it says it is stale.
static void dumpTag() {
    xSemaphoreTake(gStateMutex, portMAX_DELAY);
    DeviceState st = gState;
    xSemaphoreGive(gStateMutex);

    xSemaphoreTake(gTagMutex, portMAX_DELAY);
    OptMain      m = gTagMain;
    OptAuxiliary a = gTagAux;
    OptMeta      t = gTagMeta;
    xSemaphoreGive(gTagMutex);

    const bool live = (st == DeviceState::Present ||
                       st == DeviceState::WeighingAndSync ||
                       st == DeviceState::ReconcilingMainSection ||
                       st == DeviceState::ValidTagFound);

    char uuid[33];
    for (int i = 0; i < 16; i++) snprintf(uuid + i * 2, 3, "%02x", m.instance_uuid[i]);

    Serial.printf("[tag] state=%s  spool=#%d  %s\n", deviceStateName(st), (int)gSpoolId,
                  live ? "(live)" : "(LAST READ — nothing on the scale now)");
    Serial.printf("[tag] Meta  main@%u (size %u)  aux@%u (size %u)\n",
                  (unsigned)t.main_region_offset, (unsigned)t.main_region_size,
                  (unsigned)t.aux_region_offset,  (unsigned)t.aux_region_size);
    Serial.printf("[tag] Main  uuid %s\n", uuid);
    {
        // Product identity, when the vendor wrote any. package_uuid is the one
        // the product model keys on; the others are shown for completeness.
        char hex[33];
        auto puuid = [&](const char* label, const uint8_t* u) {
            if (optUuidIsNil(u)) return;
            for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", u[i]);
            Serial.printf("[tag]       %-9s %s\n", label, hex);
        };
        puuid("package",  m.package_uuid);
        puuid("material", m.material_uuid);
        puuid("brand",    m.brand_uuid);
        if (m.gtin) Serial.printf("[tag]       gtin      %llu\n",
                                  (unsigned long long)m.gtin);
    }
    Serial.printf("[tag]       brand \"%s\"  name \"%s\"  abbr \"%s\"\n",
                  m.brand_name, m.material_name, m.material_abbreviation);
    if (m.primary_color_rgba[3])
        Serial.printf("[tag]       colour %02x%02x%02x (alpha %u)\n",
                      m.primary_color_rgba[0], m.primary_color_rgba[1],
                      m.primary_color_rgba[2], (unsigned)m.primary_color_rgba[3]);
    else
        Serial.println("[tag]       colour NOT ASSIGNED (alpha 0)");
    Serial.printf("[tag]       dia %.2f mm  nominal %.1f g  actual %.1f g  empty %.1f g\n",
                  m.filament_diameter, m.nominal_netto_full_weight,
                  m.actual_netto_full_weight, m.empty_container_weight);
    Serial.printf("[tag]       nozzle %d-%d C  bed %d-%d C  class %d  type %d\n",
                  m.min_print_temperature, m.max_print_temperature,
                  m.min_bed_temperature, m.max_bed_temperature,
                  m.material_class, m.material_type);
    if (m.has_lab)
        Serial.printf("[tag]       lab L*=%.2f a*=%.2f b*=%.2f (measured, D65/2)\n",
                      m.primary_color_lab[0], m.primary_color_lab[1],
                      m.primary_color_lab[2]);
    else
        Serial.println("[tag]       lab NOT MEASURED");
    if (m.extra_len || m.extra_overflow)
        Serial.printf("[tag]       %u B of unmodelled vendor fields preserved%s\n",
                      (unsigned)m.extra_len,
                      m.extra_overflow ? "  <- OVERFLOW: Main will not be rewritten" : "");
    Serial.printf("[tag]       write_protection %d (Main %s)\n",
                  m.write_protection, optMainWritable(m) ? "writable" : "PROTECTED");

    // The line this command exists for.
    Serial.printf("[tag] Aux   consumed %.1f g%s\n", a.consumed_weight,
                  a.consumed_weight > 0.0f ? ""
                    : "   <- zero: either an unused spool, or Aux never wrote");

    if (m.actual_netto_full_weight > 0.0f)
        Serial.printf("[tag] calc  remaining = actual - consumed = %.1f g\n",
                      m.actual_netto_full_weight - a.consumed_weight);
}

// ── Serial calibration helpers ────────────────────────────────────────────────
// Commands accepted over Serial at 115200 baud:
//   ZERO          — tare the scale (store new zero offset to NVS)
//   CAL <grams>   — calibrate with a known weight currently on the scale
//
// Example workflow: place nothing on scale, send "ZERO".
// Then place a known 100g weight, send "CAL 100".

static void saveCalibration(NAU7802& nau) {
    Preferences prefs;
    prefs.begin("scale", false);
    prefs.putInt("zero",  nau.getZeroOffset());
    prefs.putFloat("cal", nau.getCalibrationFactor());
    prefs.putBool("valid", true);
    prefs.end();
}

// Shared zero/cal operations — driven by both the serial console and the web UI.
static void doZero(NAU7802& nau) {
    if (!sNauPresent) { Serial.println("[scale] no NAU7802 — command ignored"); return; }
    nau.calculateZeroOffset(32);
    saveCalibration(nau);
    Serial.printf("[scale] Zero offset set: %ld\n", (long)nau.getZeroOffset());
}

static bool doCalibrate(NAU7802& nau, float knownGrams) {
    if (!sNauPresent) { Serial.println("[scale] no NAU7802 — command ignored"); return false; }
    if (knownGrams <= 0) return false;
    nau.calculateCalibrationFactor(knownGrams, 32);
    saveCalibration(nau);
    gScaleCalibrated = true;   // clears the idle-screen "not calibrated" banner
    Serial.printf("[scale] Cal factor set: %.4f (for %.1fg)\n",
                  nau.getCalibrationFactor(), knownGrams);
    return true;
}

// Web UI sets request flags (gCalZeroReq / gCalSetGrams); apply them here on the
// scale task so all NAU7802 access stays on one core.
static void handleCalRequests(NAU7802& nau) {
    if (gCalZeroReq) { gCalZeroReq = false; doZero(nau); }
    float g = gCalSetGrams;
    if (g > 0.0f) { gCalSetGrams = 0.0f; doCalibrate(nau, g); }
}

// Dispatch one complete command line.
// Report how close each task has come to overflowing its stack.
//
// This project's characteristic failure is a stack overflow, and its symptom is
// a reboot loop with no useful backtrace — scaleTask blew its canary on the
// first SEED, and displayTask nearly did on a QR code whose encoder puts its
// working buffers on the CALLER's stack. Both were found the expensive way.
// uxTaskGetStackHighWaterMark reports the smallest free margin ever observed,
// so this turns "is there headroom" into a question with an answer.
//
// Exercise the deep paths first — place a spool, DUMP, COMPACT, load the web
// app — then run STACK. A margin under ~512 bytes is where to worry.
static void reportStacks() {
    struct { const char* name; TaskHandle_t h; uint32_t size; } t[] = {
        { "nfc",     gNfcTask,     6144 },
        { "scale",   gScaleTask,   8192 },
        { "display", gDisplayTask, 6144 },
        { "sync",    gSyncTask,    8192 },
    };
    Serial.println("[stack] smallest free margin seen since boot:");
    for (auto& e : t) {
        if (!e.h) { Serial.printf("  %-8s (not running)\n", e.name); continue; }
        // Returns words on some ports and bytes on ESP-IDF; ESP-IDF's is bytes.
        const uint32_t free_b = uxTaskGetStackHighWaterMark(e.h);
        Serial.printf("  %-8s %5u B free of %u  (%u%% used)%s\n",
                      e.name, (unsigned)free_b, (unsigned)e.size,
                      (unsigned)(100 - (free_b * 100 / e.size)),
                      free_b < 512 ? "   <-- TIGHT" : "");
    }
    Serial.printf("[stack] heap: %u kB free, %u kB largest block\n",
                  (unsigned)(ESP.getFreeHeap() >> 10),
                  (unsigned)(ESP.getMaxAllocHeap() >> 10));
}

static void dispatchCommand(const String& cmd, NAU7802& nau) {
    if (cmd.equalsIgnoreCase("ZERO")) {
        doZero(nau);
    } else if (cmd.startsWith("CAL ") || cmd.startsWith("cal ")) {
        doCalibrate(nau, cmd.substring(4).toFloat());
    } else if (cmd.equalsIgnoreCase("DUMP TAG")) {
        // Ahead of the storeSerialCommand fallback, which owns DUMP / DUMP usage
        // but cannot see the tag globals.
        dumpTag();
    } else if (cmd.equalsIgnoreCase("TAGFORMAT")) {
        gTagForceFormat = true;
        Serial.println("[nfc] TAGFORMAT armed — place the tag (or leave it in "
                       "place and lift/replace it) to reformat it.");
    } else if (cmd.equalsIgnoreCase("STACK")) {
        reportStacks();
    } else if (cmd.equalsIgnoreCase("APIKEY")) {
        Serial.printf("[api] key is %s%s\n",
                      apiKeyIsSet() ? "set: " : "NOT set (write endpoints are open)",
                      apiKeyIsSet() ? apiKeyGet().c_str() : "");
    } else if (cmd.startsWith("APIKEY ") || cmd.startsWith("apikey ")) {
        String k = cmd.substring(7); k.trim();
        // "APIKEY none" clears it — a station in a cabinet must always have a
        // way back to an open API if the secret is lost.
        if (k.equalsIgnoreCase("none") || k.equalsIgnoreCase("clear")) k = "";
        apiKeySet(k.c_str());
        Serial.printf("[api] key %s\n", k.length() ? "set" : "cleared (endpoints now open)");
    } else if (!storeSerialCommand(cmd) && !cfgSerialCommand(cmd)) {
        // Nothing claimed it. Say so — a silently ignored command is
        // indistinguishable from a broken device, and a mistyped or truncated
        // line used to just vanish.
        Serial.printf("[cmd] unknown: \"%s\"\n", cmd.c_str());
    }
}

// Accumulate serial input a character at a time and dispatch on newline.
//
// NOT readStringUntil('\n'): that returns whatever it has after a 1 second
// timeout, and PlatformIO's monitor transmits each keystroke as it is typed
// rather than buffering the line. Any pause longer than a second while typing
// therefore delivered a partial command and turned the remainder into a second,
// bogus one — "SEED 20 200" arriving as "SEED" (which seeded nothing) followed
// by "20 200" (which matched nothing and was silently dropped).
//
// Buffering here removes the timing dependency entirely: a line is a line,
// however slowly it was typed.
static void handleSerialCommand(NAU7802& nau) {
    static String buf;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\r') continue;                 // CRLF terminals
        if (c == '\n') {
            String cmd = buf;
            buf = "";
            cmd.trim();
            if (cmd.length()) dispatchCommand(cmd, nau);
            continue;
        }
        // Cap the buffer so a stuck sender or binary noise can't grow it without
        // bound on a device with no spare heap.
        if (buf.length() < 160) buf += c;
    }
}

// ── Task ──────────────────────────────────────────────────────────────────────

// Bring the ADC up and restore its calibration. Split out so it can be retried
// if the chip is missing at boot and appears later (a nudged Qwiic cable).
static bool scaleInit(NAU7802& nau);

void scaleTask(void* param) {
    NAU7802 nau;

    // A missing load cell must NOT take this task down. The serial console is
    // hosted here — ZERO, CAL, SEED, COMPACT, DUMP, APIKEY, TAGFORMAT all run
    // on this task — so deleting it on a failed nau.begin() removed the only
    // diagnostic channel the device has, precisely when something is wrong.
    // Worse in the cabinet, where serial over USB is the last way in.
    //
    // Instead: stay alive, keep servicing commands, and retry the ADC. Weight
    // stays 0 and the display's "not calibrated" banner is already the signal
    // that readings are not to be trusted.
    bool nauOk = scaleInit(nau);
    if (!nauOk)
        Serial.println("[scale] NAU7802 not found — weighing disabled, retrying every 5 s. "
                       "Serial commands still work. Check the Qwiic cable.");

    TickType_t lastRetry = xTaskGetTickCount();
    while (!nauOk) {
        handleSerialCommand(nau);
        if (xTaskGetTickCount() - lastRetry >= pdMS_TO_TICKS(5000)) {
            lastRetry = xTaskGetTickCount();
            nauOk = scaleInit(nau);
            if (nauOk) Serial.println("[scale] NAU7802 appeared — weighing enabled.");
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    for (;;) {
        handleSerialCommand(nau);
        handleCalRequests(nau);   // web-initiated zero/calibrate

        // getWeight() blocks internally while averaging SCALE_SAMPLES readings.
        // At 80 SPS each sample is ~12.5 ms; 10 samples ≈ 125 ms.  The
        // Arduino delay(1) inside getAverage() cooperates with FreeRTOS, so
        // nfcTask stays responsive while we average.
        float w = nau.getWeight(false, SCALE_SAMPLES);
        if (w < 0.0f) w = 0.0f;

        xSemaphoreTake(gWeightMutex, portMAX_DELAY);
        gWeightGrams = w;
        xSemaphoreGive(gWeightMutex);

        vTaskDelay(pdMS_TO_TICKS(10));  // brief yield between averaging cycles
    }
}

static bool scaleInit(NAU7802& nau) {
    if (!nau.begin()) { sNauPresent = false; return false; }
    sNauPresent = true;
    nau.setSampleRate(NAU7802_SPS_80);
    nau.calibrateAFE();

    // Load stored calibration from NVS, or perform a first-boot auto-tare
    // and apply the config.h default calibration factor.
    Preferences prefs;
    prefs.begin("scale", true);
    bool hasStoredCal = prefs.getBool("valid", false);
    gScaleCalibrated = hasStoredCal;
    if (hasStoredCal) {
        nau.setZeroOffset((int32_t)prefs.getInt("zero", 0));
        nau.setCalibrationFactor(prefs.getFloat("cal", SCALE_CAL_FACTOR));
    }
    prefs.end();

    if (!hasStoredCal) {
        // Assume the scale is empty at boot; auto-tare with 32-sample average.
        vTaskDelay(pdMS_TO_TICKS(500));  // let load cell settle
        nau.calculateZeroOffset(32);
        nau.setCalibrationFactor(SCALE_CAL_FACTOR);
        Serial.println("[scale] First boot: auto-zeroed. Send 'CAL <grams>' to calibrate.");
    }

    Serial.printf("[scale] Ready. Zero=%ld Cal=%.4f\n",
                  (long)nau.getZeroOffset(), nau.getCalibrationFactor());
    return true;
}

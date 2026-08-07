#include <Arduino.h>
#include <Preferences.h>
#include <SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>
#include "config.h"
#include "api_key.h"

extern volatile bool gTagForceFormat;

// False until the ADC answers. ZERO/CAL are still reachable over serial
// while it is missing, and averaging 32 samples from a chip that is not
// there just wastes time — refuse with a reason instead.
static bool sNauPresent = false;
#include "device_state.h"
#include "store.h"          // route store test commands (EV / DUMP / …) here too
#include "config_store.h"   // …and CFG commands

extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern volatile bool        gScaleCalibrated;
extern volatile bool        gCalZeroReq;
extern volatile float       gCalSetGrams;

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
static void dispatchCommand(const String& cmd, NAU7802& nau) {
    if (cmd.equalsIgnoreCase("ZERO")) {
        doZero(nau);
    } else if (cmd.startsWith("CAL ") || cmd.startsWith("cal ")) {
        doCalibrate(nau, cmd.substring(4).toFloat());
    } else if (cmd.equalsIgnoreCase("TAGFORMAT")) {
        gTagForceFormat = true;
        Serial.println("[nfc] TAGFORMAT armed — place the tag (or leave it in "
                       "place and lift/replace it) to reformat it.");
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

#include <Arduino.h>
#include <Preferences.h>
#include <SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>
#include "config.h"
#include "device_state.h"
#include "store.h"          // route store test commands (EV / DUMP / …) here too
#include "config_store.h"   // …and CFG commands

extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern volatile bool        gScaleCalibrated;

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

static void handleSerialCommand(NAU7802& nau) {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd.equalsIgnoreCase("ZERO")) {
        nau.calculateZeroOffset(32);
        saveCalibration(nau);
        Serial.printf("[scale] Zero offset set: %ld\n", (long)nau.getZeroOffset());
    } else if (cmd.startsWith("CAL ") || cmd.startsWith("cal ")) {
        float knownGrams = cmd.substring(4).toFloat();
        if (knownGrams > 0) {
            nau.calculateCalibrationFactor(knownGrams, 32);
            saveCalibration(nau);
            gScaleCalibrated = true;   // clears the idle-screen "run CAL" banner
            Serial.printf("[scale] Cal factor set: %.4f (for %.1fg)\n",
                          nau.getCalibrationFactor(), knownGrams);
        }
    } else if (!storeSerialCommand(cmd)) {
        // Not a scale or store command — try the config catalog harness.
        cfgSerialCommand(cmd);
    }
}

// ── Task ──────────────────────────────────────────────────────────────────────

void scaleTask(void* param) {
    NAU7802 nau;
    if (!nau.begin()) {
        Serial.println("[scale] NAU7802 not found — task halted");
        vTaskDelete(nullptr);
        return;
    }
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

    for (;;) {
        handleSerialCommand(nau);

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

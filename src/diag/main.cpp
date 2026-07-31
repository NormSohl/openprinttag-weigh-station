// Minimal bring-up diagnostic — build with:  pio run -e diag -t upload -t monitor
//
// Deliberately depends on NOTHING: no SPI, no I2C, no LittleFS, no WiFi, no
// libraries, no FreeRTOS tasks. Its only job is to answer one question:
//
//   Does *any* code run on this board, and does its output reach the console?
//
// If this prints and blinks, the board definition (flash mode, PSRAM mode,
// partition table) and the USB console are sound, and the fault is somewhere in
// the application. If this is silent too, the fault is below the application —
// board config or hardware — and no amount of debugging main.cpp will find it.
//
// It also reports the runtime-detected flash/PSRAM sizes, which is the direct
// check on the two board-json values we just changed.

#include <Arduino.h>

#define DIAG_LED 46   // onboard WS2812 (SparkFun Thing Plus ESP32-S3)

void setup() {
    Serial.begin(115200);
    // Native USB re-enumerates after reset; hold so the monitor can attach
    // before the first output.
    delay(3000);
    Serial.println();
    Serial.println("=== DIAG: code is running ===");
    Serial.printf("chip      : %s rev %d @ %u MHz\n",
                  ESP.getChipModel(), ESP.getChipRevision(), getCpuFrequencyMhz());
    Serial.printf("flash     : %u bytes (%u MB)\n",
                  ESP.getFlashChipSize(), ESP.getFlashChipSize() >> 20);
    Serial.printf("psram     : %u bytes (%u MB)  %s\n",
                  ESP.getPsramSize(), ESP.getPsramSize() >> 20,
                  ESP.getPsramSize() ? "OK" : "NOT DETECTED");
    Serial.printf("free heap : %u bytes\n", ESP.getFreeHeap());
    Serial.println("blinking the onboard RGB (GPIO 46) red/green/blue…");
    Serial.flush();
}

void loop() {
    static uint32_t n = 0;
    // neopixelWrite() is built into the ESP32 Arduino core — no library needed.
    neopixelWrite(DIAG_LED, (n % 3 == 0) ? 40 : 0,
                            (n % 3 == 1) ? 40 : 0,
                            (n % 3 == 2) ? 40 : 0);
    Serial.printf("[diag] tick %u  uptime %lu s  heap %u\n",
                  n++, millis() / 1000, ESP.getFreeHeap());
    Serial.flush();
    delay(1000);
}

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"
#include "device_state.h"

extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;

static Adafruit_SSD1306  oled(128, 64, &Wire, -1);
static Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

void displayTask(void* param) {
    pixel.begin();
    pixel.show();

    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[display] SSD1306 not found — task halted");
        vTaskDelete(nullptr);
    }
    oled.clearDisplay();
    oled.display();

    for (;;) {
        // TODO: snapshot gState, render OLED content and NeoPixel colour per
        //       docs/design/oled-display-states.md.
        //       AwaitingFormatConfirm: drive the live 5→0 countdown on line 4
        //       and accelerating NeoPixel blink (pattern unique to this state).
        vTaskDelay(pdMS_TO_TICKS(50));  // ~20 fps ceiling
    }
}

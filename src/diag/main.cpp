// Display isolation test — build with:  pio run -e diag -t upload -t monitor
//
// The real firmware panics with a null pointer inside TFT_eSPI::begin_tft_write()
// during tft.init(). Three hypotheses (SD teardown, init race, User_Setup not
// reaching the library) were each falsified on hardware. This strips everything
// away to answer one question:
//
//   Does TFT_eSPI init at all on this board and wiring?
//
// Difference from the real firmware that matters most: this does NOT call
// SPI.begin() first. TFT_eSPI expects to own bus initialisation; our setup()
// pre-claims the bus with SPI.begin(SCK,MISO,MOSI) for the PN5180's benefit,
// which may leave TFT_eSPI's own SPIClass with an uninitialised handle.
//
// Set DIAG_PRECLAIM_SPI to 1 to reproduce the firmware's ordering and confirm
// that's the trigger.

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

#define DIAG_LED           46   // onboard WS2812
#define DIAG_PRECLAIM_SPI   0   // 1 = call SPI.begin() before tft.init(), like the firmware

static TFT_eSPI tft;

void setup() {
    Serial.begin(115200);
    delay(3000);                       // let native USB enumerate before we print
    Serial.println();
    Serial.println("=== DIAG: TFT isolation ===");
    Serial.printf("flash %u MB, psram %u MB, heap %u\n",
                  ESP.getFlashChipSize() >> 20, ESP.getPsramSize() >> 20,
                  ESP.getFreeHeap());
    Serial.printf("pins: SCK=%d MOSI=%d MISO=%d  CS=%d DC=%d RST=%d\n",
                  TFT_SCLK, TFT_MOSI, TFT_MISO, TFT_CS, TFT_DC, TFT_RST);

#if DIAG_PRECLAIM_SPI
    Serial.println("pre-claiming SPI bus (firmware ordering)…");
    Serial.flush();
    SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI);
    Serial.println("SPI.begin() returned");
#else
    Serial.println("NOT pre-claiming SPI — TFT_eSPI owns bus init");
#endif

    Serial.println("calling tft.init()…");
    Serial.flush();                    // flush first: if init panics, this is the last line
    tft.init();
    Serial.println("tft.init() RETURNED — no panic");

    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(12, 12);
    tft.println("TFT OK");
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(12, 60);
    tft.println("ILI9488 480x320");
    tft.setCursor(12, 90);
    tft.println("display isolation test");

    // Colour bars — proves pixels, not just a command that didn't crash.
    const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW, TFT_CYAN, TFT_MAGENTA};
    for (int i = 0; i < 6; i++) tft.fillRect(12 + i * 76, 140, 70, 60, bars[i]);

    Serial.println("drew text + colour bars — look at the screen");
    Serial.flush();
}

void loop() {
    static uint32_t n = 0;
    neopixelWrite(DIAG_LED, (n % 3 == 0) ? 40 : 0,
                            (n % 3 == 1) ? 40 : 0,
                            (n % 3 == 2) ? 40 : 0);
    Serial.printf("[diag] alive %u s\n", millis() / 1000);
    Serial.flush();
    n++;
    delay(1000);
}

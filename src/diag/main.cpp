// Shared-SPI-bus test — build with:  pio run -e diag -t upload -t monitor
//
// WHAT CAME BEFORE
// Bring-up hit two independent faults on the shared PN5180 + TFT bus, and
// fixing either alone still left the reader dead:
//
//   1. The ILI9488's SDO does not tri-state when its CS is high, and the
//      netlist wired it into the MISO splice. The panel drove MISO
//      permanently, so every PN5180 reply read back as 0x00.
//      Fixed in hardware (SDO off the splice) and in config (TFT_MISO=-1).
//
//   2. The display sits on SPI3 and the PN5180 on SPI2, yet both drive GPIO
//      11/12. spiAttachSCK() ends in pinMatrixOutAttach(), a write to the pin's
//      single-valued func_out_sel register, so the last attach wins silently.
//      displayBegin() runs after SPI.begin(), so the display took the pins and
//      the PN5180 never saw a clock edge.
//
//      Neither device can move. TFT_eSPI's S3 port resolves REG_SPI_BASE() to
//      DR_REG_SPI3_BASE for every legal SPI_PORT value, so its register writes
//      always hit SPI3 — building without USE_HSPI_PORT just makes the Arduino
//      layer drive SPI2 while those writes still go to SPI3, which panics.
//      The PN5180 library hardcodes the global `SPI` (SPI2). So the fix is to
//      re-point the pins on every handoff, which is what claimNfc/claimTft
//      below do — the same thing src/spi_bus.cpp does for the real firmware.
//
// A PN5180-only diag proved (1) — it read a tag UID with the display unlinked.
// It could not test (2) at all, because it had no display.
//
// WHAT THIS TESTS
// Both devices, both drivers, one bus — the exact contention the real firmware
// creates, with none of the application code. The display is initialised FIRST
// (as displayBegin() does), then the PN5180, then both run concurrently: tag
// polling on loop() with a display repaint on every pass.
//
//   both work            -> the handoff is correct; the real firmware should
//                           behave identically.
//   display blank/frozen -> the TFT lost the pins. Bus config, not app code.
//   NFC silent           -> the PN5180 lost the pins (or reset() hangs again).
//   TFT panic in
//   begin_tft_write()    -> the null bus-handle crash is back; SPI.begin()
//                           must run before tft.init().
//
// Whichever half fails, it fails here without the app in the way.

#include <Arduino.h>
#include <SPI.h>
#include <esp32-hal-spi.h>
#include <TFT_eSPI.h>
#include <PN5180.h>
#include <PN5180ISO15693.h>
#include "../config.h"

#define DIAG_LED 46   // onboard WS2812

static PN5180ISO15693 nfc(PN5180_NSS, PN5180_BUSY, PN5180_RESET);
static TFT_eSPI       tft;

// Pin-ownership handoff, the single-threaded twin of src/spi_bus.cpp. No mutex
// needed here — this sketch has one thread — but the routing is identical.
static void claimNfc() {
    spi_t* bus = SPI.bus();
    if (!bus) return;
    spiAttachSCK(bus, SPI_SCK);
    spiAttachMOSI(bus, SPI_MOSI);
    spiAttachMISO(bus, SPI_MISO);
}
static void claimTft() {
    spi_t* bus = TFT_eSPI::getSPIinstance().bus();
    if (!bus) return;
    spiAttachSCK(bus, SPI_SCK);
    spiAttachMOSI(bus, SPI_MOSI);
}

static uint32_t sTagCount = 0;
static uint32_t sPollCount = 0;
static char     sLastUid[24] = "(none yet)";

static void drawScreen(const char* status, uint16_t colour) {
    claimTft();
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(TL_DATUM);

    tft.drawString("SHARED BUS DIAG", 10, 10, 4);
    tft.drawFastHLine(10, 40, 460, TFT_DARKGREY);

    tft.setTextColor(colour, TFT_BLACK);
    tft.drawString(status, 10, 55, 4);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String("uid:   ") + sLastUid, 10, 100, 2);
    tft.drawString(String("tags:  ") + sTagCount, 10, 125, 2);
    tft.drawString(String("polls: ") + sPollCount, 10, 150, 2);
    tft.drawString(String("up:    ") + (millis() / 1000) + " s", 10, 175, 2);

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("This text repainting = TFT still owns the bus.", 10, 285, 2);
}

void setup() {
    Serial.begin(115200);
    delay(3000);                       // let native USB enumerate before printing
    Serial.println();
    Serial.println("=== DIAG: PN5180 + TFT sharing one SPI host ===");
    Serial.printf("pins: NSS=%d BUSY=%d RST=%d | SPI SCK=%d MOSI=%d MISO=%d"
                  " | TFT CS=%d DC=%d RST=%d\n",
                  PN5180_NSS, PN5180_BUSY, PN5180_RESET,
                  SPI_SCK, SPI_MOSI, SPI_MISO,
                  TFT_CS, TFT_DC, TFT_RST);
    Serial.flush();

    // Same order as main.cpp: claim the bus explicitly, THEN let TFT_eSPI
    // init (its own spi.begin() will early-return), THEN the PN5180.
    Serial.println("SPI.begin()…");
    Serial.flush();
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    Serial.printf("SPI.begin() ok — bus handle %s\n", SPI.bus() ? "valid" : "NULL (bad)");

    Serial.println("tft.init()…  <-- a panic here is the null bus-handle crash");
    Serial.flush();
    tft.init();
    tft.setRotation(TFT_ROTATION);
    drawScreen("display OK, NFC starting", TFT_YELLOW);
    Serial.println("tft.init() ok — screen should show 'display OK, NFC starting'");
    Serial.flush();

    // MISO must float now that the panel's SDO is off the splice. If this still
    // reads held-LOW, the SDO wire is still connected.
    pinMode(SPI_MISO, INPUT_PULLUP);   delay(5);
    const int misoUp = digitalRead(SPI_MISO);
    pinMode(SPI_MISO, INPUT_PULLDOWN); delay(5);
    const int misoDn = digitalRead(SPI_MISO);
    pinMode(SPI_MISO, INPUT);
    Serial.printf("MISO GPIO %d: pullup=%d pulldown=%d -> %s\n",
                  SPI_MISO, misoUp, misoDn,
                  (misoUp && !misoDn) ? "FLOATING (correct — SDO is off the splice)"
                                      : "STILL DRIVEN (is the TFT SDO wire still on MISO?)");
    Serial.flush();

    Serial.println("nfc.begin()…");
    Serial.flush();
    claimNfc();
    nfc.begin();

    Serial.println("nfc.reset()…  <-- if this is the last line, the reader is deaf");
    Serial.flush();
    claimNfc();
    nfc.reset();
    Serial.println("nfc.reset() ok");

    uint8_t prod[2] = {}, fw[2] = {}, eep[2] = {};
    claimNfc();
    nfc.readEEprom(PRODUCT_VERSION,  prod, 2);
    nfc.readEEprom(FIRMWARE_VERSION, fw,   2);
    nfc.readEEprom(EEPROM_VERSION,   eep,  2);
    Serial.printf("product %d.%d  firmware %d.%d  eeprom %d.%d\n",
                  prod[1], prod[0], fw[1], fw[0], eep[1], eep[0]);

    claimNfc();
    nfc.setupRF();
    Serial.println("setupRF() ok — polling. Present a tag; watch the SCREEN too.");
    Serial.flush();

    drawScreen("both up - polling", TFT_GREEN);
}

void loop() {
    uint8_t uid[8] = {};

    // No mutex here on purpose: this sketch is single-threaded, so the two
    // drivers already interleave strictly. The real firmware runs them from
    // two tasks on two cores and needs gSpiMutex.
    claimNfc();
    ISO15693ErrorCode rc = nfc.getInventory(uid);
    sPollCount++;

    if (rc == ISO15693_EC_OK) {
        snprintf(sLastUid, sizeof(sLastUid), "%02x%02x%02x%02x%02x%02x%02x%02x",
                 uid[7], uid[6], uid[5], uid[4], uid[3], uid[2], uid[1], uid[0]);
        sTagCount++;
        Serial.printf("[diag] TAG uid %s  (poll %lu)\n", sLastUid, (unsigned long)sPollCount);
        neopixelWrite(DIAG_LED, 0, 60, 0);              // green: tag seen
        drawScreen("TAG PRESENT", TFT_GREEN);
    } else {
        if (sPollCount % 10 == 0) {
            Serial.printf("[diag] no tag (rc=%d), %lu s, %lu polls\n",
                          (int)rc, millis() / 1000, (unsigned long)sPollCount);
        }
        neopixelWrite(DIAG_LED, 0, 0, (sPollCount & 1) ? 30 : 0);  // blue blink
        drawScreen("polling - no tag", TFT_CYAN);
    }

    delay(500);
}

// PN5180 isolation test — build with:  pio run -e diag -t upload -t monitor
//
// The display half of bring-up is done (TFT_eSPI works with USE_HSPI_PORT).
// This build now isolates the remaining failure: nfcTask hangs inside
// PN5180::reset(), which spins forever waiting for an IDLE interrupt the chip
// never raises.
//
// THE HYPOTHESIS
// A GPIO can only be driven by one SPI peripheral at a time. In the real
// firmware displayBegin() runs first and TFT_eSPI (USE_HSPI_PORT) routes
// SCK/MOSI/MISO = 12/11/13 to SPI3. The PN5180 library uses the global `SPI`
// object, which is SPI2 — so once the display claims those pins, the PN5180
// never sees a clock edge and cannot answer.
//
// THE TEST
// This sketch initialises the PN5180 and NOTHING ELSE. No TFT, so nothing
// steals the pins.
//
//   gets past reset() -> the chip is fine; the shared-pin conflict is real and
//                        the fix is bus topology (or rewiring the PN5180 to
//                        its own pins), not the PN5180 itself.
//   hangs at reset()  -> the chip genuinely is not responding even with sole
//                        ownership of the bus: power (3.3 V logic and the RF
//                        rail), or the NSS/BUSY/RST wiring.
//
// The hang itself is the signal — the last line printed tells you which.

#include <Arduino.h>
#include <SPI.h>
#include <PN5180.h>
#include <PN5180ISO15693.h>
#include "../config.h"

#define DIAG_LED 46   // onboard WS2812

static PN5180ISO15693 nfc(PN5180_NSS, PN5180_BUSY, PN5180_RESET);

void setup() {
    Serial.begin(115200);
    delay(3000);                       // let native USB enumerate before printing
    Serial.println();
    Serial.println("=== DIAG: PN5180 isolation (no TFT — sole owner of the bus) ===");
    Serial.printf("pins: NSS=%d BUSY=%d RST=%d   SPI SCK=%d MOSI=%d MISO=%d\n",
                  PN5180_NSS, PN5180_BUSY, PN5180_RESET,
                  SPI_SCK, SPI_MOSI, SPI_MISO);

    pinMode(PN5180_BUSY, INPUT);
    Serial.printf("BUSY before init: %s\n",
                  digitalRead(PN5180_BUSY) ? "HIGH" : "LOW (normal idle)");
    Serial.flush();

    // ── Is the wire even connected? (software continuity test) ────────────────
    // Read each pin twice, once with the ESP32's internal pull-up and once with
    // its pull-down. A pin with nothing on the far end simply follows whichever
    // resistor is enabled; a pin connected to a powered chip is held at the
    // chip's level and ignores both. This distinguishes "open wire" from "chip
    // present but idle low", which a plain digitalRead() cannot.
    //
    //   pullup=1 pulldown=0 -> FLOATING: no connection, or the far end is
    //                         unpowered/high-impedance
    //   pullup=0 pulldown=0 -> something is actively driving it LOW
    //   pullup=1 pulldown=1 -> something is actively driving it HIGH
    auto pinProbe = [](const char* name, int pin) {
        pinMode(pin, INPUT_PULLUP);   delay(5);
        const int up = digitalRead(pin);
        pinMode(pin, INPUT_PULLDOWN); delay(5);
        const int dn = digitalRead(pin);
        pinMode(pin, INPUT);
        const char* verdict = (up && !dn) ? "FLOATING (nothing driving it)"
                            : (!up && !dn) ? "driven LOW  (chip present)"
                            : (up && dn)   ? "driven HIGH (chip present)"
                                           : "inconsistent";
        Serial.printf("  %-4s GPIO %2d: pullup=%d pulldown=%d -> %s\n",
                      name, pin, up, dn, verdict);
    };

    Serial.println("pin continuity probe (internal pull-up vs pull-down):");
    pinProbe("BUSY", PN5180_BUSY);
    pinProbe("MISO", SPI_MISO);
    Serial.println("  BUSY FLOATING => the PN5180 is unpowered or that wire is open;");
    Serial.println("  BUSY driven   => the chip is alive, look at NSS/SCK/MOSI instead.");
    Serial.flush();

    // ── Hang-proof liveness probe ─────────────────────────────────────────────
    // Drive RST ourselves and watch BUSY. Per the PN5180 datasheet the chip
    // raises BUSY while it starts up after reset and drops it when ready, so a
    // powered, correctly-wired chip MUST move that pin. Pure GPIO — no SPI, no
    // library, nothing that can block.
    //
    //   BUSY changes      -> the chip is alive and RST/BUSY are wired; any
    //                        remaining failure is on the SPI lines or CS.
    //   BUSY never moves  -> nothing is driving the pin: unpowered, or RST/BUSY
    //                        not actually connected. No amount of SPI debugging
    //                        will help until that is fixed.
    pinMode(PN5180_RESET, OUTPUT);
    pinMode(PN5180_BUSY,  INPUT);

    digitalWrite(PN5180_RESET, LOW);          // assert reset (active low)
    delay(10);
    const int busyInReset = digitalRead(PN5180_BUSY);
    digitalWrite(PN5180_RESET, HIGH);         // release

    int  lo = 0, hi = 0;
    for (int i = 0; i < 200; i++) {           // 200 ms of sampling
        digitalRead(PN5180_BUSY) ? hi++ : lo++;
        delay(1);
    }
    Serial.printf("BUSY probe: in-reset=%d, after release %d HIGH / %d LOW samples\n",
                  busyInReset, hi, lo);
    if (hi == 0 || lo == 0) {
        Serial.println("BUSY NEVER CHANGED — the chip is not driving it.");
        Serial.println("  -> check PN5180 power (3.3 V logic, and the separate");
        Serial.println("     5 V / TVDD transmitter pin if the module has one),");
        Serial.printf("     and that RST=%d and BUSY=%d are really connected.\n",
                      PN5180_RESET, PN5180_BUSY);
    } else {
        Serial.println("BUSY TOGGLED — chip is powered and responding to reset.");
        Serial.println("  -> power/RST/BUSY are good; suspect SCK/MOSI/MISO or NSS.");
    }
    Serial.flush();

    Serial.println("SPI.begin()…");
    Serial.flush();
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    Serial.println("SPI.begin() ok");

    Serial.println("nfc.begin()…");
    Serial.flush();
    nfc.begin();
    Serial.println("nfc.begin() ok");

    // ── Talk to the chip WITHOUT resetting it first ───────────────────────────
    // BUSY probes as "driven LOW" = the chip is powered and idling normally, yet
    // pulsing RST produces no BUSY movement at all. A live chip must react to
    // reset, so the reset pulse isn't reaching it — and PN5180::reset() then
    // waits forever for a reboot that never happens, which is the only reason
    // everything downstream fails.
    //
    // If the chip answers here, it was never broken: RST is simply not
    // connected (or is on the wrong module pin), and that single wire is the
    // whole bug. Safe to try because the chip is idle — BUSY is already low, so
    // the library's BUSY waits should pass straight through.
    Serial.println("reading version registers WITHOUT a reset…");
    Serial.flush();
    {
        uint8_t p[2] = {0xFF, 0xFF}, f[2] = {0xFF, 0xFF}, e[2] = {0xFF, 0xFF};
        nfc.readEEprom(PRODUCT_VERSION,  p, 2);
        nfc.readEEprom(FIRMWARE_VERSION, f, 2);
        nfc.readEEprom(EEPROM_VERSION,   e, 2);
        Serial.printf("  product %d.%d  firmware %d.%d  eeprom %d.%d\n",
                      p[1], p[0], f[1], f[0], e[1], e[0]);
        const bool dead = (p[0] == 0xFF && p[1] == 0xFF) ||
                          (p[0] == 0x00 && p[1] == 0x00);
        if (dead) {
            Serial.println("  no answer -> SPI itself isn't reaching the chip:");
            Serial.printf("     check NSS=%d, and SCK=%d / MOSI=%d at the module end\n",
                          PN5180_NSS, SPI_SCK, SPI_MOSI);
        } else {
            Serial.println("  *** CHIP ANSWERS — it works. The fault is the RST wire. ***");
            Serial.printf("     check GPIO %d -> the module's RST pin\n", PN5180_RESET);
        }
    }
    Serial.flush();

    // Sample BUSY across the reset pulse. A chip that is alive toggles BUSY;
    // a flat line means nothing is driving the pin.
    Serial.println("nfc.reset()…  <-- if this is the last line, the chip is silent");
    Serial.flush();
    nfc.reset();
    Serial.println("nfc.reset() ok  <-- CHIP IS ALIVE: the shared-pin conflict is real");

    uint8_t prod[2] = {0xFF, 0xFF}, fw[2] = {0xFF, 0xFF}, eep[2] = {0xFF, 0xFF};
    nfc.readEEprom(PRODUCT_VERSION,  prod, 2);
    nfc.readEEprom(FIRMWARE_VERSION, fw,   2);
    nfc.readEEprom(EEPROM_VERSION,   eep,  2);
    Serial.printf("product %d.%d  firmware %d.%d  eeprom %d.%d\n",
                  prod[1], prod[0], fw[1], fw[0], eep[1], eep[0]);

    Serial.println("setupRF()…");
    Serial.flush();
    nfc.setupRF();
    Serial.println("setupRF() ok — now polling for tags, present one");
    Serial.flush();
}

void loop() {
    static uint32_t n = 0;
    uint8_t uid[8] = {};

    ISO15693ErrorCode rc = nfc.getInventory(uid);
    if (rc == ISO15693_EC_OK) {
        Serial.printf("[diag] TAG uid %02x%02x%02x%02x%02x%02x%02x%02x\n",
                      uid[7], uid[6], uid[5], uid[4], uid[3], uid[2], uid[1], uid[0]);
        neopixelWrite(DIAG_LED, 0, 60, 0);          // green: tag seen
    } else {
        if (n % 10 == 0) Serial.printf("[diag] no tag (rc=%d), %lu s\n",
                                       (int)rc, millis() / 1000);
        neopixelWrite(DIAG_LED, 0, 0, (n & 1) ? 30 : 0);  // blue blink: alive, polling
    }
    n++;
    delay(500);
}

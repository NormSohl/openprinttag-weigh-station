#include <Arduino.h>
#include <TFT_eSPI.h>
#include <qrcode.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"
#include "spi_bus.h"
#include "store.h"
#include "device_state.h"
#include "opt_tag.h"

// ── Shared globals ────────────────────────────────────────────────────────────
extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern OptMain              gTagMain;
extern OptAuxiliary         gTagAux;
extern SemaphoreHandle_t    gTagMutex;
extern volatile int         gSpoolId;
extern volatile bool        gSpoolNeedsOnboarding;
extern SemaphoreHandle_t    gSpiMutex;
extern char                 gWebAddr[48];
extern char                 gApSsid[24];
extern volatile bool        gScaleCalibrated;

// TFT_eSPI configured via -D flags in platformio.ini (ILI9488, 480x320).
// Landscape (TFT_ROTATION in config.h): width=480, height=320.
static TFT_eSPI          tft;
static Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ── Buzzer helpers ────────────────────────────────────────────────────────────
// The LEDC API changed between arduino-esp32 2.x (channel-based) and 3.x
// (pin-based). Support both so CI (currently 2.x) and newer cores both build.
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  #define BZ_ATTACH()  ledcAttach(BUZZER_PIN, 4000, 8)
  #define BZ_TONE(hz)  ledcWriteTone(BUZZER_PIN, (hz))
  #define BZ_OFF()     ledcWriteTone(BUZZER_PIN, 0)
#else
  static const uint8_t BUZZER_CH = 0;
  #define BZ_ATTACH()  do { ledcSetup(BUZZER_CH, 4000, 8); ledcAttachPin(BUZZER_PIN, BUZZER_CH); } while (0)
  #define BZ_TONE(hz)  ledcWriteTone(BUZZER_CH, (hz))
  #define BZ_OFF()     ledcWrite(BUZZER_CH, 0)
#endif

static void bzTone(uint32_t hz, uint32_t ms) {
    BZ_TONE(hz);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void buzz(DeviceState prev, DeviceState curr) {
    switch (curr) {
    case DeviceState::Idle:
        // Boot-ready chime only on the WiFi-provisioning → Idle transition.
        // Spool-removed returns to Idle silently (no sound on every removal).
        if (prev == DeviceState::WiFiSetupMode) {
            bzTone(523, 80); bzTone(659, 80); bzTone(784, 120);  // C5-E5-G5
        }
        break;
    case DeviceState::IdleNoWiFi:
        bzTone(392, 80); bzTone(330, 160);                       // G4-E4 descending
        break;
    case DeviceState::TagReadError:
        bzTone(330, 80); bzTone(262, 180);                       // E4-C4 low error
        break;
    case DeviceState::AwaitingFormatConfirm:
        bzTone(880, 60);                                          // A5 double-pip
        BZ_OFF();
        vTaskDelay(pdMS_TO_TICKS(50));
        bzTone(880, 60);
        break;
    case DeviceState::ForeignTagFound:
        bzTone(659, 70); bzTone(784, 100);                       // E5-G5 attention
        break;
    case DeviceState::Present:
        if (prev == DeviceState::WeighingAndSync) {
            bzTone(659, 70); bzTone(784, 70); bzTone(880, 120);  // E5-G5-A5 weigh done
        } else {
            // Newly registered or reconciled spool: fuller fanfare
            bzTone(523, 60); bzTone(659, 60);
            bzTone(784, 60); bzTone(880, 100);                   // C5-E5-G5-A5
        }
        break;
    default:
        break;
    }
    BZ_OFF();
}

// ── Layout helpers ────────────────────────────────────────────────────────────
// Size-2 body rows: 16px tall, 28px pitch, 12px left margin.
static const int MARGIN = 12;
static const int ROW_H  = 28;

// The right-hand strip is reserved for the QR panel, so row() must not clear
// into it — otherwise every text redraw would wipe the code.
//
// The longest string any screen prints is 26 characters
// ("weighstation.local/onboard"). At text size 2 the GLCD font is 12 px per
// character, so that ends at MARGIN + 312 = 324 px. TEXT_W is 330 to cover it
// with a little slack; anything longer than 26 characters will be clipped by
// the panel and leave uncleared pixels behind on redraw.
//
// The panel gave up 6 px of left edge and 8 px of width to buy that character.
// It costs the code nothing: the module scale is an integer divide, and every
// URL this firmware builds encodes at version 2 (25 modules + 8 quiet = 33),
// where 148/33 and 140/33 both floor to 4. The drawn code is the same 132 px
// square either way, just sitting 6 px further right — still 12 px clear of
// the longest text line and 12 px inside the right edge of the screen.
static const int TEXT_W = 330;
static const int QR_X   = 332;
static const int QR_Y   = 70;
static const int QR_BOX = 140;   // fits 480-332-8 wide and 320-70-... tall

static void cls() { tft.fillScreen(TFT_BLACK); }

// Render `text` as a QR code inside a QR_BOX square at (x, y).
//
// Picks the smallest version that holds the string so the modules come out as
// large as possible — on a 3.5" panel this is the difference between a code a
// phone grabs instantly and one it has to hunt for.
//
// Drawn light-on-dark with a white quiet zone: scanners need ~4 modules of
// light margin, and the screen background is black.
static void drawQr(const char* text, int x, int y, int box) {
    // Byte-mode payload capacity at ECC_LOW for versions 1..QR_VERSION_MAX,
    // derived from the encoder's own tables: dataCapacity is
    // NUM_RAW_DATA_MODULES/8 - NUM_ERROR_CORRECTION_CODEWORDS[Low], less the
    // 12-bit (mode + 8-bit count) byte-mode header, floored to whole bytes.
    //
    // We must range-check the string ourselves, because the encoder will not.
    // qrcode_initText() returns 0 for ANY input: its only failure path is
    // `mode < 0`, and encodeDataCodewords() returns a mode constant
    // unconditionally in byte mode. Worse, bb_appendBits() has no bounds check
    // at all — it writes straight to data[offset >> 3]. So an over-long string
    // does not fail, it silently overruns codewordBytes[], which is a VLA on
    // THIS TASK'S STACK. Trusting the return code (as this function first did)
    // pinned every code at version 2 and turned a 33-character URL into an
    // unscannable one, and a 44-character URL into a stack smash.
    static const uint8_t QR_VERSION_MAX = 4;
    static const uint8_t QR_CAPACITY[QR_VERSION_MAX] = { 17, 32, 53, 78 };

    const size_t len = strlen(text);
    uint8_t version = 0;
    for (uint8_t v = 1; v <= QR_VERSION_MAX; v++) {
        if (len <= QR_CAPACITY[v - 1]) { version = v; break; }
    }
    if (version == 0) return;              // too long to encode — draw nothing

    // Sized for QR_VERSION_MAX (v4 = 33 modules): ((33*33)+7)/8 = 137 bytes.
    // qrcode_getBufferSize() is a runtime call, so this is sized by hand —
    // raising QR_VERSION_MAX means resizing this too.
    static uint8_t qrData[160];
    QRCode qr;
    if (qrcode_initText(&qr, qrData, version, ECC_LOW, text) != 0) return;

    const int quiet = 4;                   // modules of light margin, per spec
    const int total = qr.size + quiet * 2;
    const int scale = box / total;
    if (scale < 1) return;
    const int side = total * scale;
    const int ox   = x + (box - side) / 2;
    const int oy   = y + (box - side) / 2;

    tft.fillRect(ox, oy, side, side, TFT_WHITE);
    for (uint8_t my = 0; my < qr.size; my++) {
        // Coalesce runs of dark modules into one rect: 29x29 individual
        // fillRects is ~840 SPI transactions, and this screen redraws on every
        // state change.
        uint8_t mx = 0;
        while (mx < qr.size) {
            if (!qrcode_getModule(&qr, mx, my)) { mx++; continue; }
            uint8_t run = 0;
            while (mx + run < qr.size && qrcode_getModule(&qr, mx + run, my)) run++;
            tft.fillRect(ox + (quiet + mx) * scale, oy + (quiet + my) * scale,
                         run * scale, scale, TFT_BLACK);
            mx += run;
        }
    }
}

static void row(int r, const char* text, uint16_t color = TFT_WHITE) {
    int y = MARGIN + r * ROW_H;
    tft.fillRect(0, y, TEXT_W, ROW_H, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(MARGIN, y);
    tft.print(text);
}

static void rowf(int r, uint16_t color, const char* fmt, ...) {
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    row(r, buf, color);
}

// How many size-2 characters fit a body row before the QR panel.
static const int ROW_CHARS = (TEXT_W - MARGIN) / 12;   // 26

// Draw `text` over up to `maxRows` rows, breaking at spaces. Returns the next
// free row.
//
// OPT material_name is a DISPLAY string up to 63 characters — the spec's own
// example is "PC Blend Carbon Fiber Black" — and a body row holds 26. Handing
// the raw string to row() draws straight through TEXT_W into the QR panel, and
// row() only clears out to TEXT_W, so the overflow also survives the next
// redraw. That reads as a display fault rather than a long name.
//
// Breaking at a space keeps the name legible; a word longer than a row is cut
// hard, because there is nothing better to do with it.
static int rowWrap(int r, const char* text, uint16_t color, int maxRows = 2) {
    char buf[ROW_CHARS + 1];
    const size_t len = strlen(text);
    size_t pos = 0;
    int drawn = 0;
    while (pos < len && drawn < maxRows) {
        size_t take = len - pos;
        if (take > (size_t)ROW_CHARS) {
            take = (size_t)ROW_CHARS;
            size_t brk = take;
            while (brk > 0 && text[pos + brk] != ' ') brk--;
            if (brk > 0) take = brk;          // else no space: hard break
        }
        memcpy(buf, text + pos, take);
        buf[take] = 0;
        row(r + drawn, buf, color);
        drawn++;
        pos += take;
        while (pos < len && text[pos] == ' ') pos++;
    }
    return r + drawn;
}

// Size-3 title on row 0: 24px tall.
static void title(const char* text, uint16_t color) {
    tft.fillRect(0, MARGIN, 480, 32, TFT_BLACK);
    tft.setTextSize(3);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(MARGIN, MARGIN);
    tft.print(text);
}

// ── Task ──────────────────────────────────────────────────────────────────────

// Hardware init for the display, NeoPixel and buzzer. Called from setup() on
// the main task BEFORE any task is created — deliberately not from displayTask.
//
// TFT_eSPI and the PN5180 library each initialise their own SPI peripheral,
// and both grab the shared GPIO 11/12. Doing that from two tasks on two cores
// races, so init is serialised here: the display sets itself up before any
// task exists, and from then on spi_bus.cpp re-points the pins on every
// handoff. tft.init() must run under spiBusTakeTft() like any other TFT work.
void displayBegin() {
    pixel.begin();
    pixel.setBrightness(80);
    pixel.show();

    BZ_ATTACH();

    spiBusTakeTft();
    tft.init();
    tft.setRotation(TFT_ROTATION);
    tft.fillScreen(TFT_BLACK);
    spiBusGive();
}

void displayTask(void* param) {
    DeviceState prevState  = DeviceState::Boot;
    bool        rendered   = false;
    TickType_t  awaitStart = 0;
    bool        blinkOn    = false;
    TickType_t  lastBlink  = 0;
    int         lastCount  = -1;
    bool        lastCal    = gScaleCalibrated;
    bool        lastWrFail = storeWriteFailed();

    for (;;) {
        xSemaphoreTake(gStateMutex, portMAX_DELAY);
        DeviceState state = gState;
        xSemaphoreGive(gStateMutex);

        // TagDetecting is the one state with no screen of its own, deliberately:
        // it returns BEFORE cls(), so the idle screen stays up through the
        // debounce instead of flashing. Every other state must have a case in
        // the switch below, or the default there will name it on screen.
        if (state == DeviceState::TagDetecting) {
            prevState = state;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool stateChanged = (state != prevState);
        if (stateChanged) {
            buzz(prevState, state);
            spiBusTakeTft();
            cls();
            spiBusGive();
            rendered  = false;
            lastCount = -1;
            if (state == DeviceState::AwaitingFormatConfirm) {
                awaitStart = xTaskGetTickCount();
                blinkOn    = true;
                lastBlink  = awaitStart;
            }
        }
        prevState = state;

        // Calibration flips asynchronously (CAL over serial). Force a clean
        // redraw so the idle "not calibrated" banner appears/clears immediately.
        bool calNow = gScaleCalibrated;
        if (calNow != lastCal) {
            lastCal = calNow;
            spiBusTakeTft();
            cls();
            spiBusGive();
            rendered = false;
        }

        // Same for the storage banner. A log that has stopped accepting writes
        // is silent everywhere else — nobody has the web app open when it
        // happens — so the idle screen has to say so.
        bool wrFail = storeWriteFailed();
        if (wrFail != lastWrFail) {
            lastWrFail = wrFail;
            spiBusTakeTft();
            cls();
            spiBusGive();
            rendered = false;
        }

        xSemaphoreTake(gTagMutex, portMAX_DELAY);
        OptMain      snap    = gTagMain;
        OptAuxiliary auxSnap = gTagAux;
        xSemaphoreGive(gTagMutex);

        xSemaphoreTake(gWeightMutex, portMAX_DELAY);
        float weight = gWeightGrams;
        xSemaphoreGive(gWeightMutex);

        int  spoolId         = gSpoolId;
        bool needsOnboarding = gSpoolNeedsOnboarding;

        float remaining = 0.0f;
        if (snap.empty_container_weight > 0.0f)
            remaining = weight - snap.empty_container_weight;
        else if (snap.actual_netto_full_weight > 0.0f)
            remaining = snap.actual_netto_full_weight - auxSnap.consumed_weight;
        else
            remaining = weight;
        if (remaining < 0.0f) remaining = 0.0f;

        uint32_t pixelColor = 0;

        // ── Static states: render once on entry ───────────────────────────────
        if (!rendered) {
            spiBusTakeTft();

            switch (state) {

            case DeviceState::Boot:
                title("Weigh Station", TFT_WHITE);
                row(2, "Starting...", TFT_DARKGREY);
                pixelColor = 0;
                break;

            case DeviceState::WiFiSetupMode:
                title("WiFi Setup", tft.color565(0, 100, 220));
                row(2, "Join network:", TFT_WHITE);
                row(3, "WeighStation-Setup", TFT_CYAN);
                row(5, "Then open browser to", TFT_DARKGREY);
                row(6, "192.168.4.1", TFT_DARKGREY);
                pixelColor = pixel.Color(0, 0, 60);
                break;

            case DeviceState::Idle:
                title("Seattle Makers", TFT_GREEN);
                row(2, "Place spool to begin", TFT_WHITE);
                if (storeWriteFailed()) {
                    row(3, "STORAGE FULL - not", TFT_RED);
                    row(4, "recording weighs!", TFT_RED);
                } else if (!gScaleCalibrated) {
                    row(3, "Scale not calibrated", tft.color565(220, 140, 0));
                    row(4, "Calibrate in web app", tft.color565(220, 140, 0));
                }
                if (gWebAddr[0]) {
                    // Both, deliberately. The .local name is the one worth
                    // reading and typing, but mDNS is not universal — plenty of
                    // Android browsers won't resolve it — so the numeric address
                    // is there as the fallback that always works.
                    row(5, "Web app:", TFT_DARKGREY);
                    rowf(6, TFT_CYAN, "%s.local", DEVICE_HOSTNAME);
                    rowf(7, TFT_DARKGREY, "or %s", gWebAddr);
                    char url[64];
                    snprintf(url, sizeof(url), "http://%s/", gWebAddr);
                    drawQr(url, QR_X, QR_Y, QR_BOX);   // IP, not mDNS: must just work
                }
                pixelColor = pixel.Color(0, 20, 0);
                break;

            case DeviceState::IdleNoWiFi:
                title("Weigh Station", tft.color565(220, 140, 0));
                row(2, "Place spool to weigh", TFT_WHITE);
                if (storeWriteFailed())
                    row(3, "STORAGE FULL - not saving", TFT_RED);
                else if (!gScaleCalibrated)
                    row(3, "Uncalibrated - web app", tft.color565(220, 140, 0));
                if (gApSsid[0]) {
                    row(4, "Join WiFi:", TFT_DARKGREY);
                    rowf(5, TFT_CYAN, "%s", gApSsid);
                    rowf(6, TFT_CYAN, "http://%s", gWebAddr);
                    // Only useful after joining the AP, but that is exactly when
                    // someone is standing here squinting at an IP address.
                    char url[64];
                    snprintf(url, sizeof(url), "http://%s/", gWebAddr);
                    drawQr(url, QR_X, QR_Y, QR_BOX);
                }
                pixelColor = pixel.Color(40, 15, 0);
                break;

            case DeviceState::TagReadError:
                title("Read Error", TFT_RED);
                row(2, "Reposition spool", TFT_WHITE);
                row(3, "or remove tag", TFT_WHITE);
                pixelColor = pixel.Color(80, 0, 0);
                break;

            case DeviceState::BlankTagFound:
                title("New tag found", tft.color565(220, 140, 0));
                pixelColor = pixel.Color(60, 40, 0);
                break;

            case DeviceState::AwaitingFormatConfirm:
                title("New tag found", tft.color565(220, 140, 0));
                row(2, "Remove to cancel", TFT_WHITE);
                row(3, "Registering in:", TFT_WHITE);
                // Countdown digit drawn in the dynamic section below.
                break;

            case DeviceState::FormattingAndRegistering:
                title("Registering...", tft.color565(0, 100, 220));
                row(2, "Please wait", TFT_WHITE);
                pixelColor = pixel.Color(0, 0, 80);
                break;

            case DeviceState::ForeignTagFound:
            case DeviceState::RegisteringForeignTag:
                title("New spool found", TFT_CYAN);
                row(2, snap.brand_name[0] ? snap.brand_name : "Unknown brand", TFT_WHITE);
                // A foreign tag carries whatever product name its vendor wrote,
                // which is exactly the case OPT's 63-char limit exists for.
                {
                    int r = rowWrap(3, snap.material_name[0] ? snap.material_name
                                                             : "Unknown material", TFT_WHITE);
                    row(r + 1, "Registering spool...", TFT_DARKGREY);
                }
                pixelColor = pixel.Color(0, 50, 50);
                break;

            case DeviceState::ValidTagFound:
                title("Tag read", TFT_CYAN);
                {
                    int r = rowWrap(2, snap.material_name[0] ? snap.material_name
                                                             : "Reading spool...", TFT_WHITE);
                    if (snap.brand_name[0]) row(r, snap.brand_name, TFT_DARKGREY);
                }
                pixelColor = pixel.Color(0, 40, 60);
                break;

            case DeviceState::WeighingAndSync: {
                title("Weighing...", tft.color565(0, 100, 220));
                char wbuf[24];
                snprintf(wbuf, sizeof(wbuf), "%.0f g", weight);
                row(2, wbuf, TFT_WHITE);
                pixelColor = pixel.Color(0, 0, 80);
                break;
            }

            case DeviceState::Present:
                if (needsOnboarding) {
                    title("Registered!", TFT_YELLOW);
                    // Identity and weight on one line: they are read together
                    // ("which spool, and how much is on it"), and pairing them
                    // buys back a row for the two addresses below.
                    if (spoolId > 0) rowf(2, TFT_WHITE, "Spool #%d  %.0f g", spoolId, remaining);
                    else             rowf(2, TFT_WHITE, "%.0f g", remaining);
                    // Name the state outright. "Registered!" alone reads as
                    // finished, when in fact the spool has no vendor, material
                    // or colour yet and is useless for inventory until someone
                    // fills the form in. Say what is missing, not just what
                    // happened.
                    row(3, "NEEDS ONBOARDING", tft.color565(220, 140, 0));
                    // Not everyone scans QR codes, and a phone camera is not
                    // always to hand, so give the typed route too — named by the
                    // page it lands on, so it matches the web app's own nav.
                    // Both addresses carry the /onboard path: the home page does
                    // not say which spool it would act on, and the whole point
                    // here is to reach the form for THIS one.
                    row(4, "Scan QR to add details,", TFT_DARKGREY);
                    row(5, "or visit the Onboard page:", TFT_DARKGREY);
                    // mDNS only resolves on a real LAN. In SoftAP fallback the
                    // numeric address is the only one that works, so don't
                    // advertise a name that would just fail there.
                    if (!gApSsid[0]) {
                        rowf(6, TFT_CYAN, "%s.local/onboard", DEVICE_HOSTNAME);
                        if (gWebAddr[0]) rowf(7, TFT_DARKGREY, "or %s/onboard", gWebAddr);
                    } else if (gWebAddr[0]) {
                        rowf(6, TFT_CYAN, "%s/onboard", gWebAddr);
                    }
                    // This is the one screen where someone is definitely about to
                    // go to the web app, so the QR deep-links to the form itself
                    // rather than the home page. The Onboard page targets whatever
                    // is on the scale, which is this spool.
                    if (gWebAddr[0]) {
                        char url[72];
                        snprintf(url, sizeof(url), "http://%s/onboard", gWebAddr);
                        drawQr(url, QR_X, QR_Y, QR_BOX);
                    }
                    pixelColor = pixel.Color(50, 50, 0);
                } else {
                    if (spoolId > 0) rowf(0, TFT_WHITE, "Spool #%d", spoolId);
                    else             row(0, "Spool", TFT_WHITE);
                    // The full product name, wrapped — "PLA Summer Grass", not
                    // "PLA". OPT says brand_name and material_name should be
                    // shown together ("Prusament PLA Galaxy Black"), so the
                    // brand goes underneath rather than being dropped.
                    int r = rowWrap(2, snap.material_name[0] ? snap.material_name
                                                             : "Unknown", TFT_WHITE);
                    if (snap.brand_name[0]) row(r++, snap.brand_name, TFT_DARKGREY);
                    rowf(r + 1, TFT_GREEN, "%.0f g remaining", remaining);
                    // "Saved locally" was left from the Spoolman era, where the
                    // alternative was "pushed to the server". There is no
                    // server now, so it distinguished nothing — and being
                    // unconditional it was worse than useless: a full log makes
                    // storeAppendEvent() drop the event, and the screen still
                    // claimed the weight had been recorded. Say what actually
                    // happened, and say it loudly when it didn't.
                    if (storeWriteFailed()) row(r + 3, "NOT SAVED - storage full", TFT_RED);
                    else                    row(r + 3, "Weight recorded", TFT_DARKGREY);
                    pixelColor = pixel.Color(0, 80, 0);
                }
                break;

            case DeviceState::ReconcilingMainSection:
                title("Updating tag...", TFT_YELLOW);
                pixelColor = pixel.Color(50, 50, 0);
                break;

            // A state with no case above used to fall through to a bare break,
            // leaving the screen black after cls() — indistinguishable from a
            // crashed display or dead backlight. ValidTagFound did exactly that,
            // and it cost a debugging session. Draw the state name instead: an
            // unhandled state is now self-identifying.
            default:
                title("...", TFT_DARKGREY);
                row(2, deviceStateName(state), TFT_WHITE);
                row(3, "(no screen for this state)", TFT_DARKGREY);
                pixelColor = pixel.Color(30, 30, 30);
                break;
            }

            spiBusGive();
            rendered = true;
            pixel.setPixelColor(0, pixelColor);
            pixel.show();
        }

        // ── Dynamic: AwaitingFormatConfirm countdown + NeoPixel blink ─────────
        if (state == DeviceState::AwaitingFormatConfirm) {
            TickType_t now       = xTaskGetTickCount();
            uint32_t   elapsedMs = (uint32_t)((now - awaitStart) * portTICK_PERIOD_MS);
            int        countdown = (int)(BLANK_TAG_CONFIRM_SEC - (int)(elapsedMs / 1000));
            if (countdown < 0) countdown = 0;

            if (countdown != lastCount) {
                // Size-8 digit: 48x64px per char; center at x≈216 for single digit.
                spiBusTakeTft();
                tft.fillRect(0, 160, 480, 100, TFT_BLACK);
                tft.setTextSize(8);
                tft.setTextColor(tft.color565(220, 140, 0), TFT_BLACK);
                tft.setCursor(216, 168);
                tft.print(countdown);
                spiBusGive();
                lastCount = countdown;
            }

            uint32_t period = (uint32_t)(countdown * 80 + 100);
            if ((uint32_t)((now - lastBlink) * portTICK_PERIOD_MS) >= period) {
                blinkOn   = !blinkOn;
                lastBlink = now;
                pixel.setPixelColor(0, blinkOn ? pixel.Color(80, 40, 0) : 0);
                pixel.show();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

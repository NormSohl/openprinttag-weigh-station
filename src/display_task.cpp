// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <qrcode.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include "config.h"
#include "spi_bus.h"
#include "store.h"
#include "device_state.h"
#include "opt_tag.h"
#include "display_tz.h"
#include "station_name.h"

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
extern volatile int         gPortalSecsLeft;
extern volatile bool        gScaleCalibrated;
extern volatile bool        gClockSet;

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
        // Boot-ready chime only on the way in from bring-up — from Boot on an
        // ordinary start, or from the portal when one had to be run. Spool-
        // removed returns to Idle silently (no sound on every removal).
        //
        // Boot is in this list because a successful join no longer passes
        // through WiFiSetupMode: that state now means "the portal is actually
        // up", so the common path is Boot -> Idle and would otherwise have gone
        // quiet.
        if (prev == DeviceState::WiFiSetupMode || prev == DeviceState::Boot) {
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
// 78, not 70: rows 0-1 clear down to y 68, and the panel is drawn 4 px inside
// QR_BOX, so this leaves a 14 px gap below the full-width header band. At 70 it
// was 6 px — enough, but not enough to be obviously enough.
static const int QR_Y   = 78;
static const int QR_BOX = 140;   // fits 480-332-8 wide, panel ends y 214

// Corner clock, every screen. Bottom-right, below and clear of everything
// any screen draws: the lowest other content is WiFiSetupMode's row 8
// countdown (y 236) and AwaitingFormatConfirm's big digit (ends y 260) --
// both well above CLOCK_Y.
//
// Sized for the longer of the two formats tzGet24Hour() picks between --
// "MM/DD/YYYY hh:mm PM" (19 chars, strftime zero-pads) beats "MM/DD/YYYY
// HH:MM" (16) -- so switching modes on the Settings page never leaves a
// stray tail of the previous, longer string behind (the clear-rect always
// covers the max width, not whichever string happens to be drawn this time).
static const int CLOCK_CHARS = 20;
static const int CLOCK_W = CLOCK_CHARS * 12;      // text size 2
static const int CLOCK_H = 18;
static const int CLOCK_X = 480 - MARGIN - CLOCK_W;
static const int CLOCK_Y = 320 - MARGIN - CLOCK_H;

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

// Rows 0 and 1 clear y 12..68, and the QR panel starts at y 82 — so those two
// rows are ABOVE the panel and may use the whole screen width. That is 38
// characters instead of 26, which is the difference between "PLA" and
// "Spool #42  PLA Summer Grass  eSun" on one line.
//
// Row 2 onwards overlaps the panel vertically and must stay inside TEXT_W.
static const int TEXT_W_WIDE = 468;   // 480 less a 12 px right margin
static const int WIDE_ROWS   = 2;     // rows 0..1 only

static void rowAt(int r, const char* text, uint16_t color, int clearW) {
    int y = MARGIN + r * ROW_H;
    tft.fillRect(0, y, clearW, ROW_H, TFT_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(MARGIN, y);
    tft.print(text);
}

static void row(int r, const char* text, uint16_t color = TFT_WHITE) {
    rowAt(r, text, color, TEXT_W);
}

// Full-width row. Silently narrows below the header band rather than trusting
// the caller — clearing 468 px at row 2 would erase the left edge of the QR
// code on every redraw.
static void rowWide(int r, const char* text, uint16_t color = TFT_WHITE) {
    rowAt(r, text, color, r < WIDE_ROWS ? TEXT_W_WIDE : TEXT_W);
}

static void rowf(int r, uint16_t color, const char* fmt, ...) {
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    row(r, buf, color);
}

// How many size-2 characters fit a body row before the QR panel, and in the
// full-width header band above it.
static const int ROW_CHARS      = (TEXT_W      - MARGIN) / 12;   // 26
static const int ROW_CHARS_WIDE = (TEXT_W_WIDE - MARGIN) / 12;   // 38

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
static int rowWrapCore(int r, const char* text, uint16_t color, int maxRows,
                       int cols, bool wide) {
    char buf[ROW_CHARS_WIDE + 1];
    const size_t len = strlen(text);
    size_t pos = 0;
    int drawn = 0;
    while (pos < len && drawn < maxRows) {
        size_t take = len - pos;
        if (take > (size_t)cols) {
            take = (size_t)cols;
            size_t brk = take;
            while (brk > 0 && text[pos + brk] != ' ') brk--;
            if (brk > 0) take = brk;          // else no space: hard break
        }
        memcpy(buf, text + pos, take);
        buf[take] = 0;
        if (wide) rowWide(r + drawn, buf, color);
        else      row(r + drawn, buf, color);
        drawn++;
        pos += take;
        while (pos < len && text[pos] == ' ') pos++;
    }
    return r + drawn;
}

static int rowWrap(int r, const char* text, uint16_t color, int maxRows = 2) {
    return rowWrapCore(r, text, color, maxRows, ROW_CHARS, false);
}

// Wrap inside the full-width header band. Never spills past row 1: below that
// the QR panel is in the way, and a 38-column line would run straight into it.
static int rowWrapWide(int r, const char* text, uint16_t color) {
    int room = WIDE_ROWS - r;
    if (room < 1) room = 1;
    return rowWrapCore(r, text, color, room, ROW_CHARS_WIDE, true);
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
    char        lastClock[CLOCK_CHARS + 1] = {};
    char        lastStationName[STATION_NAME_MAX_LEN + 1];
    strlcpy(lastStationName, stationNameGet(), sizeof(lastStationName));

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

        // Every cls() below blanks the corner clock along with everything
        // else, so each site sets this to force an immediate redraw of it —
        // otherwise it would sit blank until the minute happens to roll over.
        bool clsHappened = false;

        bool stateChanged = (state != prevState);
        if (stateChanged) {
            buzz(prevState, state);
            spiBusTakeTft();
            cls();
            spiBusGive();
            rendered  = false;
            lastCount = -1;
            clsHappened = true;
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
            clsHappened = true;
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
            clsHappened = true;
        }

        // Same idea, for the Idle screen's own greeting: a Settings-page save
        // is silent otherwise. Unlike cal/wrFail this can only ever actually
        // change the pixels while state == Idle, but forcing the redraw
        // unconditionally (matching the two checks above) is simpler than
        // gating it, and costs nothing extra on any other screen.
        if (strcmp(stationNameGet(), lastStationName) != 0) {
            strlcpy(lastStationName, stationNameGet(), sizeof(lastStationName));
            spiBusTakeTft();
            cls();
            spiBusGive();
            rendered = false;
            clsHappened = true;
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
                row(2, "Starting...", TFT_SILVER);
                pixelColor = 0;
                break;

            case DeviceState::WiFiSetupMode:
                title("WiFi Setup", tft.color565(0, 100, 220));
                row(2, "Join network:", TFT_WHITE);
                row(3, "WeighStation-Setup", TFT_CYAN);
                row(5, "Then open browser to", TFT_SILVER);
                row(6, "192.168.4.1", TFT_SILVER);
                // Scan-to-join. A phone camera reads the WIFI: URI and offers to
                // join the open setup AP directly, so nobody has to type the SSID.
                // A web-URL QR would be useless on THIS screen — 192.168.4.1 is
                // unreachable until you are already on the AP, which is the whole
                // point of the QR. SSID matches wm.autoConnect() in sync_task; the
                // AP is passwordless, hence T:nopass.
                drawQr("WIFI:S:WeighStation-Setup;T:nopass;;", QR_X, QR_Y, QR_BOX);
                // Row 8 carries the countdown, written by the dynamic block
                // below — this screen is a window that closes, and until it
                // said so the only cue was the screen changing on its own.
                pixelColor = pixel.Color(0, 0, 60);
                break;

            case DeviceState::Idle:
                title(stationNameGet(), TFT_GREEN);
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
                    row(5, "Web app:", TFT_SILVER);
                    rowf(6, TFT_CYAN, "%s.local", DEVICE_HOSTNAME);
                    rowf(7, TFT_SILVER, "or %s", gWebAddr);
                    char url[64];
                    snprintf(url, sizeof(url), "http://%s/", gWebAddr);
                    drawQr(url, QR_X, QR_Y, QR_BOX);   // IP, not mDNS: must just work
                }
                pixelColor = pixel.Color(0, 20, 0);
                break;

            case DeviceState::IdleNoWiFi:
                title("Weigh Station", tft.color565(220, 140, 0));
                // Row 1 is free here (only Present's wide header uses it) --
                // name the cause. Reaching this state always means the setup
                // portal was offered and closed without new credentials being
                // entered, whether this is a first join or a previously-saved
                // network that dropped and fell back to a retry portal — see
                // runConfigPortal() in sync_task.cpp.
                row(1, "Setup timed out", TFT_SILVER);
                row(2, "Place spool to weigh", TFT_WHITE);
                if (storeWriteFailed())
                    row(3, "STORAGE FULL - not saving", TFT_RED);
                else if (!gScaleCalibrated)
                    row(3, "Uncalibrated - web app", tft.color565(220, 140, 0));
                else
                    // Weighing and local saving never depend on WiFi (nfcTask
                    // has no network awareness at all) -- only NTP does, so
                    // without it new events still record, just filed as
                    // "unknown" period until the clock syncs. Worth saying
                    // explicitly: this screen looks like a degraded state,
                    // and someone should not hesitate to use the scale on it.
                    row(3, "Weighs & saves locally", TFT_SILVER);
                if (gApSsid[0]) {
                    row(4, "Join WiFi:", TFT_SILVER);
                    rowf(5, TFT_CYAN, "%s", gApSsid);
                    rowf(6, TFT_CYAN, "http://%s", gWebAddr);
                    // The way back to network setup. Reaching this screen means
                    // the setup window has already closed, and without this the
                    // only route onward is knowing the /reset URL by heart —
                    // which is exactly how a unit ends up stranded on its own
                    // AP in a space whose WiFi it was carried there to join.
                    row(7, "Change network:", TFT_SILVER);
                    rowf(8, TFT_CYAN, "%s/reset", gWebAddr);
                    // Scan-to-join the station's own AP. Joining is the friction
                    // here — the URL above is printed as text and is unreachable
                    // until you are on the AP anyway, so a join QR beats a web-URL
                    // one. The AP is passwordless (WiFi.softAP with no key), hence
                    // T:nopass; gApSsid holds its SSID.
                    char join[48];
                    snprintf(join, sizeof(join), "WIFI:S:%s;T:nopass;;", gApSsid);
                    drawQr(join, QR_X, QR_Y, QR_BOX);
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
                    row(r + 1, "Registering spool...", TFT_SILVER);
                }
                pixelColor = pixel.Color(0, 50, 50);
                break;

            case DeviceState::ValidTagFound:
                title("Tag read", TFT_CYAN);
                // Brand before material, same order and fallback strings as
                // ForeignTagFound and Present's header -- OPT's own "Prusament
                // PLA Galaxy Black" convention. Neither line color-differentiated
                // from the other, matching those two screens as well.
                {
                    row(2, snap.brand_name[0] ? snap.brand_name : "Unknown brand", TFT_WHITE);
                    rowWrap(3, snap.material_name[0] ? snap.material_name
                                                      : "Reading spool...", TFT_WHITE);
                }
                pixelColor = pixel.Color(0, 40, 60);
                break;

            case DeviceState::WeighingAndSync: {
                title("Weighing...", tft.color565(0, 100, 220));
                char wbuf[24];
                snprintf(wbuf, sizeof(wbuf), "%.0f grams", weight);
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
                    if (spoolId > 0) rowf(2, TFT_WHITE, "Spool #%d  %.0f grams", spoolId, remaining);
                    else             rowf(2, TFT_WHITE, "%.0f grams", remaining);
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
                    row(4, "Scan QR to add details,", TFT_SILVER);
                    row(5, "or visit the Onboard page:", TFT_SILVER);
                    // mDNS only resolves on a real LAN. In SoftAP fallback the
                    // numeric address is the only one that works, so don't
                    // advertise a name that would just fail there.
                    if (!gApSsid[0]) {
                        rowf(6, TFT_CYAN, "%s.local/onboard", DEVICE_HOSTNAME);
                        if (gWebAddr[0]) rowf(7, TFT_SILVER, "or %s/onboard", gWebAddr);
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
                    // Identity on the full-width header band: spool number,
                    // product name and brand on one line, wrapping to a second
                    // if it needs to. 38 columns up here against 26 below, which
                    // is what makes all three fit at once — and it reads as one
                    // fact ("which spool is this") instead of three stacked
                    // fragments. OPT asks for brand_name and material_name to be
                    // shown together, and this is that, literally.
                    // Brand BEFORE the material name, which is also the order
                    // OPT shows the two in: "Prusament PLA Galaxy Black". It
                    // reads as one product description that way, and when the
                    // line wraps the brand stays on the first row with the
                    // spool number rather than being orphaned on the second.
                    char hdr[160] = {};
                    if (spoolId > 0) snprintf(hdr, sizeof(hdr), "Spool #%d", spoolId);
                    if (snap.brand_name[0]) {
                        if (hdr[0]) strlcat(hdr, "  ", sizeof(hdr));
                        strlcat(hdr, snap.brand_name, sizeof(hdr));
                    }
                    if (hdr[0]) strlcat(hdr, "  ", sizeof(hdr));
                    strlcat(hdr, snap.material_name[0] ? snap.material_name : "Unknown",
                            sizeof(hdr));
                    int r = rowWrapWide(0, hdr, TFT_WHITE);
                    rowf(r + 1, TFT_GREEN, "%.0f grams remaining", remaining);
                    // "Saved locally" was left from the Spoolman era, where the
                    // alternative was "pushed to the server". There is no
                    // server now, so it distinguished nothing — and being
                    // unconditional it was worse than useless: a full log makes
                    // storeAppendEvent() drop the event, and the screen still
                    // claimed the weight had been recorded. Say what actually
                    // happened, and say it loudly when it didn't.
                    if (storeWriteFailed()) row(r + 3, "NOT SAVED - storage full", TFT_RED);
                    else                    row(r + 3, "Weight recorded", TFT_SILVER);
                    pixelColor = pixel.Color(0, 80, 0);
                }
                break;

            case DeviceState::ReconcilingMainSection:
                // Blue, not yellow: this is the same "working silently in the
                // background, nothing needed from you" case as WiFiSetupMode/
                // FormattingAndRegistering/WeighingAndSync, not a "success but
                // incomplete" case like Present's NEEDS ONBOARDING banner —
                // yellow is reserved for that one. NeoPixel matches the other
                // blue states' color for the same reason.
                title("Updating tag...", tft.color565(0, 100, 220));
                pixelColor = pixel.Color(0, 0, 80);
                break;

            // A state with no case above used to fall through to a bare break,
            // leaving the screen black after cls() — indistinguishable from a
            // crashed display or dead backlight. ValidTagFound did exactly that,
            // and it cost a debugging session. Draw the state name instead: an
            // unhandled state is now self-identifying.
            default:
                title("...", TFT_SILVER);
                row(2, deviceStateName(state), TFT_WHITE);
                row(3, "(no screen for this state)", TFT_SILVER);
                pixelColor = pixel.Color(30, 30, 30);
                break;
            }

            spiBusGive();
            rendered = true;
            pixel.setPixelColor(0, pixelColor);
            pixel.show();
        }

        // ── Dynamic: how long the setup window has left ───────────────────────
        // One row, rewritten only when the second changes. row() clears its own
        // strip first, so this needs no cls() — a full redraw once a second
        // would flicker and cost ~840 SPI transactions each time.
        if (state == DeviceState::WiFiSetupMode) {
            const int secs = gPortalSecsLeft;
            if (secs != lastCount) {
                lastCount = secs;
                spiBusTakeTft();
                if (secs >= 0) rowf(8, tft.color565(220, 140, 0),
                                    "Closes in %d:%02d", secs / 60, secs % 60);
                else           row(8, "", TFT_BLACK);
                spiBusGive();
            }
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

        // ── Dynamic: corner clock, every screen ────────────────────────────────
        // Unconditional on `state`, unlike the two blocks above — every screen
        // gets this. Formatted before taking the bus so an unchanged minute
        // costs nothing but a string compare, not an SPI transaction; only
        // redraws on an actual minute rollover or right after a cls() wiped it
        // (clsHappened), never on every 50 ms tick.
        //
        // gClockSet gates it exactly like periodOf_() gates Usage's calendar
        // bucketing: no battery-backed RTC means the clock reads a small
        // boot-relative value until NTP answers, and a fabricated date would
        // be worse than no clock at all. Local time (DISPLAY_TZ), not UTC —
        // this is the one place on the whole device someone reads the time at
        // a glance, and everything that gets LOGGED still uses gmtime_r/UTC
        // regardless of the TZ env var this reads.
        {
            char buf[CLOCK_CHARS + 1] = {};
            if (gClockSet) {
                time_t     now = time(nullptr);
                struct tm  tmv;
                localtime_r(&now, &tmv);
                strftime(buf, sizeof(buf),
                         tzGet24Hour() ? "%m/%d/%Y %H:%M" : "%m/%d/%Y %I:%M %p", &tmv);
            }
            if (clsHappened || strcmp(buf, lastClock) != 0) {
                strlcpy(lastClock, buf, sizeof(lastClock));
                spiBusTakeTft();
                tft.fillRect(CLOCK_X, CLOCK_Y, CLOCK_W, CLOCK_H, TFT_BLACK);
                if (buf[0]) {
                    tft.setTextSize(2);
                    tft.setTextColor(TFT_SILVER, TFT_BLACK);
                    tft.setCursor(CLOCK_X, CLOCK_Y);
                    tft.print(buf);
                }
                spiBusGive();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

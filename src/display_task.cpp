#include <Arduino.h>
#include <TFT_eSPI.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"
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

// TFT_eSPI configured via include/User_Setup.h (ILI9488, 480x320).
// Landscape rotation (setRotation(1)): width=480, height=320.
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

static void cls() { tft.fillScreen(TFT_BLACK); }

static void row(int r, const char* text, uint16_t color = TFT_WHITE) {
    int y = MARGIN + r * ROW_H;
    tft.fillRect(0, y, 480, ROW_H, TFT_BLACK);
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

// Size-3 title on row 0: 24px tall.
static void title(const char* text, uint16_t color) {
    tft.fillRect(0, MARGIN, 480, 32, TFT_BLACK);
    tft.setTextSize(3);
    tft.setTextColor(color, TFT_BLACK);
    tft.setCursor(MARGIN, MARGIN);
    tft.print(text);
}

// ── Task ──────────────────────────────────────────────────────────────────────

void displayTask(void* param) {
    pixel.begin();
    pixel.setBrightness(80);
    pixel.show();

    BZ_ATTACH();

    xSemaphoreTake(gSpiMutex, portMAX_DELAY);
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(gSpiMutex);

    DeviceState prevState  = DeviceState::Boot;
    bool        rendered   = false;
    TickType_t  awaitStart = 0;
    bool        blinkOn    = false;
    TickType_t  lastBlink  = 0;
    int         lastCount  = -1;
    bool        lastCal    = gScaleCalibrated;

    for (;;) {
        xSemaphoreTake(gStateMutex, portMAX_DELAY);
        DeviceState state = gState;
        xSemaphoreGive(gStateMutex);

        if (state == DeviceState::TagDetecting) {
            prevState = state;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool stateChanged = (state != prevState);
        if (stateChanged) {
            buzz(prevState, state);
            xSemaphoreTake(gSpiMutex, portMAX_DELAY);
            cls();
            xSemaphoreGive(gSpiMutex);
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
            xSemaphoreTake(gSpiMutex, portMAX_DELAY);
            cls();
            xSemaphoreGive(gSpiMutex);
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
            xSemaphoreTake(gSpiMutex, portMAX_DELAY);

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
                if (!gScaleCalibrated) {
                    row(3, "Scale not calibrated", tft.color565(220, 140, 0));
                    row(4, "Calibrate in web app", tft.color565(220, 140, 0));
                }
                if (gWebAddr[0]) {
                    row(5, "Web app:", TFT_DARKGREY);
                    rowf(6, TFT_CYAN, "http://%s", gWebAddr);
                }
                pixelColor = pixel.Color(0, 20, 0);
                break;

            case DeviceState::IdleNoWiFi:
                title("Weigh Station", tft.color565(220, 140, 0));
                row(2, "Place spool to weigh", TFT_WHITE);
                if (!gScaleCalibrated)
                    row(3, "Uncalibrated - web app", tft.color565(220, 140, 0));
                if (gApSsid[0]) {
                    row(4, "Join WiFi:", TFT_DARKGREY);
                    rowf(5, TFT_CYAN, "%s", gApSsid);
                    rowf(6, TFT_CYAN, "http://%s", gWebAddr);
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
                row(2, snap.brand_name[0]    ? snap.brand_name    : "Unknown brand",    TFT_WHITE);
                row(3, snap.material_name[0] ? snap.material_name : "Unknown material", TFT_WHITE);
                row(4, "Registering spool...", TFT_DARKGREY);
                pixelColor = pixel.Color(0, 50, 50);
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
                    if (spoolId > 0) rowf(2, TFT_WHITE, "Spool #%d", spoolId);
                    row(3, "Add details in web app:", TFT_DARKGREY);
                    if (gWebAddr[0]) rowf(4, TFT_CYAN, "http://%s", gWebAddr);
                    rowf(5, TFT_WHITE, "%.0f g", remaining);
                    pixelColor = pixel.Color(50, 50, 0);
                } else {
                    if (spoolId > 0) rowf(0, TFT_WHITE, "Spool #%d", spoolId);
                    else             row(0, "Spool", TFT_WHITE);
                    char matLine[28] = {};
                    strlcpy(matLine, snap.material_name[0] ? snap.material_name : "Unknown", sizeof(matLine));
                    row(2, matLine, TFT_WHITE);
                    rowf(3, TFT_GREEN, "%.0f g remaining", remaining);
                    row(5, "Saved locally", TFT_DARKGREY);
                    pixelColor = pixel.Color(0, 80, 0);
                }
                break;

            case DeviceState::ReconcilingMainSection:
                title("Updating tag...", TFT_YELLOW);
                pixelColor = pixel.Color(50, 50, 0);
                break;

            default:
                break;
            }

            xSemaphoreGive(gSpiMutex);
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
                xSemaphoreTake(gSpiMutex, portMAX_DELAY);
                tft.fillRect(0, 160, 480, 100, TFT_BLACK);
                tft.setTextSize(8);
                tft.setTextColor(tft.color565(220, 140, 0), TFT_BLACK);
                tft.setCursor(216, 168);
                tft.print(countdown);
                xSemaphoreGive(gSpiMutex);
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

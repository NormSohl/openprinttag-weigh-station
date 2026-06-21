#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
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

static Adafruit_SSD1306  oled(128, 64, &Wire, -1);
static Adafruit_NeoPixel pixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ── Helpers ───────────────────────────────────────────────────────────────────

// Convenience: write one line of text at a given y pixel position.
static void line(int y, const char* text) {
    oled.setCursor(0, y);
    oled.print(text);
}

static void linef(int y, const char* fmt, ...) {
    char buf[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    oled.setCursor(0, y);
    oled.print(buf);
}

// ── Task ──────────────────────────────────────────────────────────────────────

void displayTask(void* param) {
    pixel.begin();
    pixel.setBrightness(80);
    pixel.show();

    if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[display] SSD1306 not found — task halted");
        vTaskDelete(nullptr);
        return;
    }
    oled.clearDisplay();
    oled.display();

    // Per-render state
    DeviceState prevState    = DeviceState::Boot;
    TickType_t  awaitStart   = 0;   // tick when AwaitingFormatConfirm entered
    bool        blinkOn      = false;
    TickType_t  lastBlinkTick = 0;

    for (;;) {
        // ── Snapshot shared state ─────────────────────────────────────────────
        xSemaphoreTake(gStateMutex, portMAX_DELAY);
        DeviceState state = gState;
        xSemaphoreGive(gStateMutex);

        // TagDetecting is sub-second — hold the previous frame to avoid flicker.
        if (state == DeviceState::TagDetecting) {
            prevState = state;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Detect state entry for one-shot timer resets.
        if (state != prevState && state == DeviceState::AwaitingFormatConfirm) {
            awaitStart   = xTaskGetTickCount();
            blinkOn      = true;
            lastBlinkTick = awaitStart;
        }
        prevState = state;

        // Snapshot tag data under mutex once per frame.
        xSemaphoreTake(gTagMutex, portMAX_DELAY);
        OptMain      snap  = gTagMain;
        OptAuxiliary auxSnap = gTagAux;
        xSemaphoreGive(gTagMutex);

        xSemaphoreTake(gWeightMutex, portMAX_DELAY);
        float weight = gWeightGrams;
        xSemaphoreGive(gWeightMutex);

        int  spoolId         = gSpoolId;
        bool needsOnboarding = gSpoolNeedsOnboarding;

        // Computed remaining filament weight for display.
        float remaining = 0.0f;
        if (snap.empty_container_weight > 0.0f)
            remaining = weight - snap.empty_container_weight;
        else if (snap.actual_netto_full_weight > 0.0f)
            remaining = snap.actual_netto_full_weight - auxSnap.consumed_weight;
        else
            remaining = weight;
        if (remaining < 0.0f) remaining = 0.0f;

        // ── OLED render ───────────────────────────────────────────────────────
        oled.clearDisplay();
        oled.setTextColor(SSD1306_WHITE);
        oled.setTextSize(1);

        uint32_t pixelColor = 0;  // NeoPixel colour for this frame

        switch (state) {

        case DeviceState::Boot:
            line(0,  "Weigh Station");
            line(8,  "Starting...");
            pixelColor = 0;
            break;

        case DeviceState::WiFiSetupMode:
            line(0,  "WiFi Setup");
            line(8,  "Join AP:");
            line(16, "WeighStation-Setup");
            pixelColor = pixel.Color(0, 0, 60);  // blue
            break;

        case DeviceState::Idle:
            line(0,  "Seattle Makers");
            line(8,  "Weigh Station");
            line(16, "Place spool");
            line(24, "to begin");
            pixelColor = pixel.Color(0, 20, 0);  // dim green
            break;

        case DeviceState::IdleNoWiFi:
            line(0,  "No WiFi");
            line(8,  "Working offline");
            line(16, "Place spool");
            line(24, "to weigh");
            pixelColor = pixel.Color(40, 15, 0);  // amber
            break;

        case DeviceState::TagReadError:
            line(0,  "Read error");
            line(8,  "Reposition spool");
            line(16, "or remove tag");
            pixelColor = pixel.Color(80, 0, 0);  // red
            break;

        case DeviceState::BlankTagFound:
            // Transient — nfcTask immediately moves to AwaitingFormatConfirm.
            // Show a neutral message in the unlikely event we linger here.
            line(0, "New tag found");
            pixelColor = pixel.Color(60, 40, 0);
            break;

        case DeviceState::AwaitingFormatConfirm: {
            TickType_t now       = xTaskGetTickCount();
            uint32_t   elapsedMs = (uint32_t)((now - awaitStart) * portTICK_PERIOD_MS);
            int        countdown = (int)(BLANK_TAG_CONFIRM_SEC - (int)(elapsedMs / 1000));
            if (countdown < 0) countdown = 0;

            line(0,  "New tag found");
            line(8,  "Remove to cancel");
            line(16, "Registering in:");

            // Large countdown digit — setTextSize(4) = 24x32 px per char.
            oled.setTextSize(4);
            oled.setCursor(52, 32);
            oled.print(countdown);

            // Accelerating blink: period shrinks from 500 ms at 5 s → 100 ms at 0 s.
            uint32_t blinkPeriodMs = (uint32_t)(countdown * 80 + 100);  // 500→100 ms
            if ((uint32_t)((now - lastBlinkTick) * portTICK_PERIOD_MS) >= blinkPeriodMs) {
                blinkOn      = !blinkOn;
                lastBlinkTick = now;
            }
            pixelColor = blinkOn ? pixel.Color(80, 40, 0) : 0;  // amber blink
            break;
        }

        case DeviceState::FormattingAndRegistering:
            line(0, "Registering...");
            line(8, "Please wait");
            pixelColor = pixel.Color(0, 0, 80);  // blue
            break;

        case DeviceState::ForeignTagFound:
        case DeviceState::RegisteringForeignTag:
            line(0,  "New spool found");
            // Show decoded vendor + material from tag's own Main data.
            if (snap.brand_name[0])    line(8,  snap.brand_name);
            else                        line(8,  "Unknown brand");
            if (snap.material_name[0]) line(16, snap.material_name);
            else                        line(16, "Unknown material");
            line(24, "Adding to DB...");
            pixelColor = pixel.Color(0, 50, 50);  // cyan
            break;

        case DeviceState::WeighingAndSync:
            line(0, "Weighing...");
            linef(8, "%.0fg", weight);
            pixelColor = pixel.Color(0, 0, 80);  // blue
            break;

        case DeviceState::Present:
            if (needsOnboarding) {
                // Freshly registered stub — prompt the team to fill in Spoolman.
                line(0, "Registered!");
                if (spoolId > 0) linef(8, "Spool #%d", spoolId);
                else             line(8, "");
                line(16, "Edit in Spoolman");
                linef(24, "%.0fg", remaining);
                pixelColor = pixel.Color(50, 50, 0);  // yellow
            } else {
                // Known, synced spool.
                if (spoolId > 0) linef(0, "Spool #%d", spoolId);
                else             line(0, "Spool");
                // Material name: if too long, truncate to 21 chars (full OLED width at size 1).
                char matLine[22] = {};
                strlcpy(matLine, snap.material_name[0] ? snap.material_name : "Unknown", sizeof(matLine));
                line(8, matLine);
                linef(16, "%.0fg remaining", remaining);
                line(24, "Synced");
                pixelColor = pixel.Color(0, 80, 0);  // green
            }
            break;

        case DeviceState::ReconcilingMainSection:
            line(0, "Updating tag...");
            pixelColor = pixel.Color(50, 50, 0);  // yellow
            break;

        case DeviceState::SpoolmanUnreachable:
            line(0,  "Spoolman offline");
            linef(8, "%.0fg (local)", remaining);
            line(16, "Will sync later");
            line(32, "Fix SpoolMan URL:");
            line(40, DEVICE_HOSTNAME ".local");
            pixelColor = pixel.Color(80, 30, 0);  // orange
            break;

        default:
            break;
        }

        oled.display();
        pixel.setPixelColor(0, pixelColor);
        pixel.show();

        vTaskDelay(pdMS_TO_TICKS(50));  // ~20 fps ceiling
    }
}

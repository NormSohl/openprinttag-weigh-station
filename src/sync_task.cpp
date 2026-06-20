#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "device_state.h"

extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;

static void setState(DeviceState s) {
    xSemaphoreTake(gStateMutex, portMAX_DELAY);
    gState = s;
    xSemaphoreGive(gStateMutex);
}

void syncTask(void* param) {
    WiFiManager wm;
    wm.setConfigPortalTimeout(120);  // 120 s timeout matches IdleNoWiFi transition

    if (!wm.autoConnect("WeighStation-Setup")) {
        setState(DeviceState::IdleNoWiFi);
    } else {
        setState(DeviceState::Idle);
    }

    for (;;) {
        // TODO: Spoolman HTTP client — implement in this order:
        //   1. PATCH /api/v1/spool/{id}  → update remaining_weight / used_weight
        //   2. GET   /api/v1/spool?extra[nfc_id]=<uid>  → look up known tag
        //   3. GET   /api/v1/spool?extra[needs_onboarding]=true  → find stubs
        //   4. POST  /api/v1/vendor + /api/v1/filament + /api/v1/spool
        //            → find-or-create flow for blank and foreign tags
        //
        // While state == Present: poll Spoolman at SPOOLMAN_POLL_MS, diff
        // filament fields against in-memory Main-section snapshot; rewrite
        // tag only when a change is detected (ReconcilingMainSection).
        vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
    }
}

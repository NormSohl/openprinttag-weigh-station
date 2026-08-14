#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "device_state.h"
#include "controller.h"

// ── Shared globals (defined in main.cpp) ─────────────────────────────────────
extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;

QueueHandle_t gCtrlQueue = nullptr;

// The controller's own gState writer. Same shape as the other tasks' setState()
// (mutex + optional trace) — the point of this phase is that for the WiFi/idle
// group, this is the ONLY one that runs.
static void setState(DeviceState s) {
    xSemaphoreTake(gStateMutex, portMAX_DELAY);
#ifdef STATE_TRACE
    if (gState != s)
        Serial.printf("[ctrl] %s -> %s\n", deviceStateName(gState), deviceStateName(s));
#endif
    gState = s;
    xSemaphoreGive(gStateMutex);
}

void ctrlPost(CtrlEvent e) {
    if (gCtrlQueue) xQueueSend(gCtrlQueue, &e, pdMS_TO_TICKS(100));
}

void controllerTask(void* param) {
    // Track the WiFi mode so a ReturnToIdle from a finished tag flow resolves to
    // the correct resting screen without reading gApSsid — the decision
    // nfcTask::idleState() used to make. Starts in Portal: nothing posts
    // ReturnToIdle before WiFi is decided (nfcTask does not poll in Boot/
    // WiFiSetupMode), so the Portal case below is only a defensive guard.
    enum { WM_PORTAL, WM_JOINED, WM_SOFTAP } wifiMode = WM_PORTAL;

    for (;;) {
        CtrlEvent e;
        if (xQueueReceive(gCtrlQueue, &e, portMAX_DELAY) != pdTRUE) continue;
        switch (e) {
            case CtrlEvent::WifiPortalUp:
                wifiMode = WM_PORTAL; setState(DeviceState::WiFiSetupMode); break;
            case CtrlEvent::WifiJoined:
                wifiMode = WM_JOINED; setState(DeviceState::Idle);          break;
            case CtrlEvent::WifiSoftAP:
                wifiMode = WM_SOFTAP; setState(DeviceState::IdleNoWiFi);     break;
            case CtrlEvent::ReturnToIdle:
                if      (wifiMode == WM_JOINED) setState(DeviceState::Idle);
                else if (wifiMode == WM_SOFTAP) setState(DeviceState::IdleNoWiFi);
                // WM_PORTAL: ignore — no tag flow runs while the portal is up.
                break;

            // Tag-detection facts from nfcTask -> the matching gState.
            case CtrlEvent::TagDetecting:
                setState(DeviceState::TagDetecting);            break;
            case CtrlEvent::TagBlank:
                setState(DeviceState::BlankTagFound);           break;
            case CtrlEvent::AwaitConfirm:
                setState(DeviceState::AwaitingFormatConfirm);   break;
            case CtrlEvent::FormatConfirmed:
                setState(DeviceState::FormattingAndRegistering);break;
            case CtrlEvent::TagValid:
                setState(DeviceState::ValidTagFound);           break;
            case CtrlEvent::TagReadErr:
                setState(DeviceState::TagReadError);            break;
        }
    }
}

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Phase 2 of the state-machine-ownership refactor (docs/design/
// state-machine-ownership.md). controllerTask is the SINGLE writer of the
// WiFi/idle group of gState — WiFiSetupMode, Idle, IdleNoWiFi. Producers must
// never write those states directly; they post an event and the controller
// decides. (Tag/weigh states still belong to nfcTask/syncTask in this phase, so
// no single state has two writers.)
enum class CtrlEvent : uint8_t {
    WifiPortalUp,   // captive portal is up          -> WiFiSetupMode
    WifiJoined,     // station joined a network       -> Idle
    WifiSoftAP,     // SoftAP fallback is serving      -> IdleNoWiFi
    ReturnToIdle,   // a tag flow ended; controller picks Idle vs IdleNoWiFi from
                    // the WiFi mode it has been tracking (was nfcTask::idleState)
};

extern QueueHandle_t gCtrlQueue;

// Post an event to the controller. Non-blocking-ish (short timeout); the
// controller is always draining, so the queue should never actually fill.
void ctrlPost(CtrlEvent e);

void controllerTask(void* param);

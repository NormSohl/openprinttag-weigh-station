// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

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

    // Phase 3: nfcTask reports tag-detection facts; the controller writes the
    // matching gState. nfcTask no longer writes these itself.
    TagDetecting,   // a tag appeared, debouncing        -> TagDetecting
    TagBlank,       // confirmed blank/unformatted        -> BlankTagFound
    AwaitConfirm,   // blank countdown started            -> AwaitingFormatConfirm
    FormatConfirmed,// countdown elapsed / forced format  -> FormattingAndRegistering
    TagValid,       // decoded OK (or just formatted)     -> ValidTagFound
    TagReadErr,     // unreadable / undecodable           -> TagReadError

    // Phase 4: syncTask reports weigh/reconcile facts; the controller writes the
    // matching gState. syncTask drives its own SyncPhase synchronously so it
    // never double-processes on gState lag.
    StubReady,      // blank onboarded, stub record made  -> Present
    BeginWeigh,     // resolved to a known/registered spool-> WeighingAndSync
    SpoolForeign,   // valid tag, not in the store        -> ForeignTagFound
    ForeignRegistering, // adopting the foreign tag       -> RegisteringForeignTag
    Weighed,        // weighed once, Aux + log written    -> Present
    NeedsReconcile, // a web-side edit diverged from tag  -> ReconcilingMainSection
    ReconcileDone,  // nfcTask finished the Main write     -> Present
};

extern QueueHandle_t gCtrlQueue;

// Post an event to the controller. Non-blocking-ish (short timeout); the
// controller is always draining, so the queue should never actually fill.
void ctrlPost(CtrlEvent e);

void controllerTask(void* param);

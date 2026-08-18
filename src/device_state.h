// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include <stdint.h>

// Mirrors docs/design/device-states.mermaid exactly.
enum class DeviceState : uint8_t {
    Boot,
    WiFiSetupMode,
    Idle,
    IdleNoWiFi,
    TagDetecting,
    TagReadError,
    BlankTagFound,
    AwaitingFormatConfirm,
    FormattingAndRegistering,
    ValidTagFound,
    ForeignTagFound,
    RegisteringForeignTag,
    WeighingAndSync,
    Present,
    ReconcilingMainSection,
};

// Stable machine-readable names for the state machine. Used by /api/status, so
// treat these as part of the HTTP API: external dashboards match on them.
// Keep them in step with docs/design/device-states.mermaid.
inline const char* deviceStateName(DeviceState s) {
    switch (s) {
        case DeviceState::Boot:                     return "boot";
        case DeviceState::WiFiSetupMode:            return "wifi_setup";
        case DeviceState::Idle:                     return "idle";
        case DeviceState::IdleNoWiFi:               return "idle_no_wifi";
        case DeviceState::TagDetecting:             return "tag_detecting";
        case DeviceState::TagReadError:             return "tag_read_error";
        case DeviceState::BlankTagFound:            return "blank_tag_found";
        case DeviceState::AwaitingFormatConfirm:    return "awaiting_format_confirm";
        case DeviceState::FormattingAndRegistering: return "formatting_and_registering";
        case DeviceState::ValidTagFound:            return "valid_tag_found";
        case DeviceState::ForeignTagFound:          return "foreign_tag_found";
        case DeviceState::RegisteringForeignTag:    return "registering_foreign_tag";
        case DeviceState::WeighingAndSync:          return "weighing_and_sync";
        case DeviceState::Present:                  return "present";
        case DeviceState::ReconcilingMainSection:   return "reconciling_main_section";
    }
    return "unknown";
}

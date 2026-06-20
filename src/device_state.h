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
    SpoolmanUnreachable,
};

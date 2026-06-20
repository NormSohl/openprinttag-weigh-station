#pragma once
#include <stdint.h>
#include <stddef.h>

// OpenPrintTag CBOR/NDEF encode-decode.
//
// Field names and wire layout must match the reference implementation exactly:
//   prusa3d/OpenPrintTag — utils/record.py, nfc_initialize.py, rec_update.py
// Read those files before adding or renaming any field below.

#define OPT_STR_MAX  64

// ── Main section — static spool identity ──────────────────────
// Written at onboarding; rewritten when Spoolman filament data changes.
struct OptMain {
    char    vendor[OPT_STR_MAX];
    char    material[OPT_STR_MAX];
    char    color[OPT_STR_MAX];
    char    gtin[OPT_STR_MAX];
    char    nfc_id[OPT_STR_MAX];    // tag UID used as Spoolman lookup key
    float   spool_weight;            // empty spool weight, grams
    float   filament_weight;         // total filament on new spool, grams
    float   diameter;                // mm
    // TODO: verify remaining fields against record.py
    //       (print temperatures, bed temp, min/max flow rate, etc.)
};

// ── Auxiliary section — dynamic per-weigh data ─────────────────
// Rewritten on every weigh event.
struct OptAuxiliary {
    float    remaining_weight;   // grams
    float    used_weight;        // grams
    uint32_t timestamp;          // Unix epoch, seconds
    // TODO: verify field names against rec_update.py
};

// ── API ────────────────────────────────────────────────────────

// True if buf contains no recognisable NDEF/OPT data (blank or erased tag).
bool optIsBlank(const uint8_t* buf, size_t len);

// Decode a raw NDEF record payload into Main and Auxiliary structs.
// Either output pointer may be nullptr if that section isn't needed.
// Returns true on success.
bool optDecode(const uint8_t* buf, size_t len, OptMain* main, OptAuxiliary* aux);

// Encode the Main section into buf. Returns bytes written, 0 on error.
size_t optEncodeMain(const OptMain& main, uint8_t* buf, size_t maxLen);

// Encode the Auxiliary section into buf. Returns bytes written, 0 on error.
size_t optEncodeAux(const OptAuxiliary& aux, uint8_t* buf, size_t maxLen);

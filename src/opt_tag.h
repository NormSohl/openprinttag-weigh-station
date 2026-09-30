// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

#pragma once
#include <stdint.h>
#include <stddef.h>

// OpenPrintTag CBOR/NDEF encode-decode.
//
// Wire format uses INTEGER CBOR keys (not strings).
// Key numbers come from OpenPrintTag/openprinttag-specification
// (https://github.com/OpenPrintTag/openprinttag-specification, moved from
// prusa3d/OpenPrintTag -- old URL still redirects but this is current):
//   data/meta_fields.yaml, data/main_fields.yaml, data/aux_fields.yaml

// ── Meta section (data/meta_fields.yaml) ─────────────────────────────────────
struct OptMeta {
    uint16_t main_region_offset;  // key 0 — offset from NDEF payload start, bytes
    uint16_t main_region_size;    // key 1 — 0 if absent (region extends to next or end)
    uint16_t aux_region_offset;   // key 2 — 0 if no aux region present
    uint16_t aux_region_size;     // key 3 — 0 if absent
};

// ── Main section (data/main_fields.yaml) ─────────────────────────────────────
// Only the fields this project reads or writes.  Full field table is in the yaml.
struct OptMain {
    uint8_t  instance_uuid[16];          // key 0  — local store lookup key / nfc_id (UUID bytes)
    // ── Product identity (keys 1-4) ──────────────────────────────────────────
    // All-zero / 0 means absent. Read so foreign-tag adoption can resolve a
    // product from what the vendor wrote instead of inferring one.
    //
    // package_uuid is the level that matches "a different size is a different
    // product": the spec deduces it from brand_uuid + GTIN, and GTIN is per-SKU,
    // whereas material_uuid comes from brand_uuid + material_name and is shared
    // by every size of the same filament.
    uint8_t  package_uuid[16];           // key 1  — the SKU: OUR product identity
    uint8_t  material_uuid[16];          // key 2  — the material, one level coarser
    uint8_t  brand_uuid[16];             // key 3
    uint64_t gtin;                       // key 4  — per-SKU, so package_uuid's basis
    char     brand_name[64];             // key 11
    char     material_name[64];          // key 10
    char     material_abbreviation[16];  // key 52 — e.g. "PETG", "ASA"
    uint8_t  primary_color_rgba[4];      // key 19 — R G B A
    // key 59 — measured colour, CIE L*a*b* (D65/2 degree). NOT populated today.
    //
    // has_lab is a separate flag rather than a sentinel in the values, because
    // L* is legitimately 0 for a measured black and these structs are
    // zero-initialised — so "all zeroes" has to read as UNMEASURED, not as
    // "perfectly black". OPT forbids deriving this from RGB, so an approximated
    // value would be worse than an absent one.
    float    primary_color_lab[3];
    bool     has_lab;
    float    nominal_netto_full_weight;  // key 16 — grams (label weight)
    float    actual_netto_full_weight;   // key 17 — grams (weighed at factory)
    float    empty_container_weight;     // key 18 — grams (bare spool tare)
    float    filament_diameter;          // key 30 — mm
    int16_t  min_print_temperature;      // key 34 — °C
    int16_t  max_print_temperature;      // key 35 — °C
    int16_t  min_bed_temperature;        // key 37 — °C
    int16_t  max_bed_temperature;        // key 38 — °C
    int8_t   material_class;             // key 8  — enum; see data/material_class_enum.yaml
    int8_t   material_type;              // key 9  — enum; see data/material_type_enum.yaml
    // key 13 — 0 none, 1 irreversible, 2 PROTECT PAGE (SLIX2, password-unlockable).
    // Non-zero means the Main section is NOT ours to rewrite: see optMainWritable().
    int8_t   write_protection;

    // ── Verbatim passthrough of Main keys this firmware does not model ────────
    //
    // We encode 20 of the spec's 61 Main keys. Without this, rewriting Main on a
    // compliant vendor tag would DESTROY every other field it carried — GTIN,
    // the four UUIDs, manufactured and expiry dates, density, drying and chamber
    // temperatures, certifications, RAL reference, secondary colours — silently
    // and irreversibly.
    //
    // optDecode() copies each unrecognised key/value pair here byte for byte;
    // optEncodeMain() splices them back in before closing the map. They are
    // already valid CBOR at map level, so no interpretation is needed — which is
    // the point, since not interpreting them is exactly the situation.
    //
    // Sized above the largest Main region the layout can produce (234 B on an
    // 80x4) less our own ~113 B of output, so overflow should be unreachable.
    // If it happens anyway `extra_overflow` says so, and nfcTask refuses the
    // write rather than dropping a vendor's data on the floor.
    // Which modelled Main keys this tag actually carried, one bit per key
    // number (bit 9 = material_type, ...). Set by optDecode(); a writer that
    // assigns a field whose zero is a real value calls optMarkPresent().
    //
    // Needed because these structs are zero-initialised, so a key the tag never
    // had decodes as 0 — and 0 is not "unknown" for every field: material_type
    // 0 is PLA. optEncodeMain() writes such a field only if the tag carried it
    // or a writer set it, so a rewrite never adds values nobody stated (0 °C
    // temperatures, "PLA" for a material of unknown type) to someone's tag.
    uint64_t present;
    uint8_t  extra[192];
    uint16_t extra_len;
    bool     extra_overflow;
};

// True if the Main section of a tag carrying this record may be written.
//
// Only ever false for a tag someone else wrote — we never set key 13 and never
// call lockICODESLIX2(), so our own tags stay rewritable for the life of the
// spool. A genuine vendor tag may be protected irreversibly, and Main is not
// ours to overwrite even when the hardware would allow it.
//
// Auxiliary is deliberately NOT covered: OPT requires the aux section to stay
// writable ("everything except aux section, that one should be always
// writable"), so consumed_weight still records on a protected spool and
// weighing keeps working.
// All-zero UUID means "not present on the tag".
inline bool optUuidIsNil(const uint8_t u[16]) {
    for (int i = 0; i < 16; i++) if (u[i]) return false;
    return true;
}

inline bool optMainWritable(const OptMain& m) { return m.write_protection == 0; }

// True if rewriting Main would preserve everything the tag already carried.
// False when unmodelled fields were seen but could not all be held — writing
// then would silently discard a vendor's data, which is worse than not writing.
inline bool optMainPreservesAll(const OptMain& m) { return !m.extra_overflow; }

// ── Auxiliary section (data/aux_fields.yaml) ──────────────────────────────────
// remaining_weight is NOT stored on the tag.
// Calculate it as:  remaining = actual_netto_full_weight - consumed_weight
struct OptAuxiliary {
    float consumed_weight;  // key 0 — grams used so far; written on every weigh event

    // key 6/7 — "not filled by the manufacturer, for customer private
    // tracking only" per the spec, which is exactly cost tracking entered
    // at onboarding. has_purchase is a separate flag for the same reason
    // OptMain::has_lab is: a real $0 is implausible but a zero-initialised
    // struct needs a way to say UNSET, not FREE. Deliberately no
    // purchase_time (key 5): the local event log's own timestamp already
    // records when the price was entered, and there is no byte budget left
    // for it anyway (see AUX_REGION_SIZE's comment in opt_tag.cpp).
    float    purchase_price;
    char     purchase_currency[4];  // ISO 4217, e.g. "USD" (3 chars + NUL)
    bool     has_purchase;

    // Verbatim passthrough of Aux keys this firmware does not model — same
    // reasoning as OptMain::extra, at a much smaller scale: optEncodeAux()
    // used to rewrite the WHOLE map from consumed_weight alone, silently
    // destroying anything else present (an unused-until-now latent bug —
    // OPT allows customer-private Aux keys we don't model, e.g. workgroup,
    // storage_location). Sized for what's left of AUX_REGION_SIZE after
    // consumed_weight + purchase_price + purchase_currency (~19 of 24
    // bytes) — deliberately tiny, so overflow is expected and refuses the
    // write rather than corrupting the map, same as Main's extra_overflow.
    uint8_t  extra[5];
    uint8_t  extra_len;
    bool     extra_overflow;
};

// True if rewriting Aux would preserve everything the tag already carried.
// Same contract as optMainPreservesAll(): false means unmodelled Aux keys
// were seen but didn't all fit, so a caller should refuse the write.
inline bool optAuxPreservesAll(const OptAuxiliary& a) { return !a.extra_overflow; }

// ── NDEF/tag layout constants ─────────────────────────────────────────────────
#define OPT_CC_SIZE            4       // capability container (first 4 bytes of tag)
#define OPT_MIME_TYPE          "application/vnd.openprinttag"
#define OPT_MIME_TYPE_LEN      (sizeof(OPT_MIME_TYPE) - 1)

// ── API ───────────────────────────────────────────────────────────────────────

// True if raw tag bytes contain no recognisable NDEF/OPT record (blank tag).
bool optIsBlank(const uint8_t* tagBytes, size_t len);

// Decode a raw tag read (full ISO15693 block dump) into structs.
// Either output pointer may be nullptr if that section is not needed.
// Returns true on success.
bool optDecode(const uint8_t* tagBytes, size_t len,
               OptMeta* meta, OptMain* main, OptAuxiliary* aux);

// Encode the Main section CBOR map into buf.
// Returns bytes written, 0 on error.
size_t optEncodeMain(const OptMain& main, uint8_t* buf, size_t maxLen);

// Encode the Auxiliary section CBOR map into buf.
// Returns bytes written, 0 on error.
size_t optEncodeAux(const OptAuxiliary& aux, uint8_t* buf, size_t maxLen);

// Returns the byte offset of the NDEF OPT payload within tagBytes, or SIZE_MAX if not found.
// Add OptMeta.main_region_offset / aux_region_offset to this to get absolute write positions.
size_t optPayloadOffset(const uint8_t* tagBytes, size_t len);

// OPT's max_length for the Main text fields we write (data/main_fields.yaml),
// in bytes. optEncodeMain() never writes past them; anything comparing a
// record against a tag read back must clamp the same way (optClampText), or
// a long name differs from its own stored copy forever.
static constexpr size_t OPT_MAX_BRAND_NAME            = 31;
static constexpr size_t OPT_MAX_MATERIAL_NAME         = 63;
static constexpr size_t OPT_MAX_MATERIAL_ABBREVIATION = 7;

// Length of s once cut to at most maxBytes, never splitting a UTF-8 sequence.
inline size_t optClampedLen(const char* s, size_t maxBytes) {
    size_t n = 0;
    while (s[n] && n < maxBytes) n++;
    if (s[n])                                        // it was cut: back up over
        while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;   // continuation bytes
    return n;
}
// Truncate s in place to what optEncodeMain() would write.
inline void optClampText(char* s, size_t maxBytes) { s[optClampedLen(s, maxBytes)] = 0; }

// Record that a writer has set a Main field whose zero value is meaningful
// (see OptMain::present). key is the MAIN_KEY_* number.
inline void optMarkPresent(OptMain& m, int key) {
    if (key >= 0 && key < 64) m.present |= (uint64_t)1 << key;
}
inline bool optIsPresent(const OptMain& m, int key) {
    return key >= 0 && key < 64 && (m.present >> key) & 1;
}
// material_type (key 9) is the one modelled field a writer sets whose 0 is a
// real value (PLA), so it gets a setter that records the assignment.
inline void optSetMaterialType(OptMain& m, int8_t type) {
    m.material_type = type;
    optMarkPresent(m, 9);
}

// Offset AND length of the OPT CBOR payload. Returns false if no OPT record.
//
// The length matters as much as the offset, because "how much of this tag is
// FORMATTED" is not the same as "how big this tag is". A tag formatted one block
// short — which is what the block-79 workaround produces — has a payload that
// ends before the physical end, and a write bounded only by the tag size can
// run past the formatted region into a block that refuses it. See writeSection().
bool optPayloadExtent(const uint8_t* tagBytes, size_t len,
                      size_t* outOffset, size_t* outLength);

// The bytes one section may occupy, [*outStart, *outEnd), relative to the NDEF
// payload start — taken from the tag's own Meta, so it holds for a vendor's
// layout as well as ours. A region with no declared size runs to the start of
// the other region if that comes after it, else to the end of the payload.
// Returns false when the tag declares no such region (e.g. no Aux).
//
// Bounding by the whole payload is not enough: a Main rewrite longer than the
// Main region would run straight into the Aux region and overwrite it, and a
// vendor tag can pack its Main region tight. See writeSection().
bool optRegionBounds(const OptMeta& meta, bool aux, size_t payloadLen,
                     size_t* outStart, size_t* outEnd);

// Build a complete initialised (data-empty) OPT tag byte array for a blank tag.
// outBuf must be at least numBlocks * blockSize bytes.
// On success: outMeta is populated with region offsets; returns the NDEF payload
// byte offset within outBuf.  Returns SIZE_MAX on error.
// Layout mirrors nfc_initialize.py: CC → NDEF TLV → Meta CBOR → empty Main → empty Aux → 0xFE.
size_t optBuildBlankTag(uint8_t numBlocks, uint8_t blockSize,
                        uint8_t* outBuf, size_t outBufLen, OptMeta* outMeta);

#include "opt_tag.h"
#include <cbor.h>
#include <string.h>

// ── CBOR key constants ────────────────────────────────────────────────────────
// Meta keys (data/meta_fields.yaml)
static constexpr int META_KEY_MAIN_REGION_OFFSET = 0;
static constexpr int META_KEY_MAIN_REGION_SIZE   = 1;
static constexpr int META_KEY_AUX_REGION_OFFSET  = 2;
static constexpr int META_KEY_AUX_REGION_SIZE    = 3;

// Main keys (data/main_fields.yaml)
static constexpr int MAIN_KEY_INSTANCE_UUID              = 0;
static constexpr int MAIN_KEY_MATERIAL_CLASS             = 8;
static constexpr int MAIN_KEY_MATERIAL_TYPE              = 9;
static constexpr int MAIN_KEY_MATERIAL_NAME              = 10;
static constexpr int MAIN_KEY_BRAND_NAME                 = 11;
static constexpr int MAIN_KEY_NOMINAL_NETTO_FULL_WEIGHT  = 16;
static constexpr int MAIN_KEY_ACTUAL_NETTO_FULL_WEIGHT   = 17;
static constexpr int MAIN_KEY_EMPTY_CONTAINER_WEIGHT     = 18;
static constexpr int MAIN_KEY_PRIMARY_COLOR_RGBA         = 19;
static constexpr int MAIN_KEY_FILAMENT_DIAMETER          = 30;
static constexpr int MAIN_KEY_MIN_PRINT_TEMPERATURE      = 34;
static constexpr int MAIN_KEY_MAX_PRINT_TEMPERATURE      = 35;
static constexpr int MAIN_KEY_MIN_BED_TEMPERATURE        = 37;
static constexpr int MAIN_KEY_MAX_BED_TEMPERATURE        = 38;
static constexpr int MAIN_KEY_MATERIAL_ABBREVIATION      = 52;

// Auxiliary keys (data/aux_fields.yaml)
static constexpr int AUX_KEY_CONSUMED_WEIGHT = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

// The reference implementation (fields.py CompactFloat) may encode floats as
// int (whole numbers), CBOR float16, float32, or float64.  Read whichever type
// is present and return it as float.
static bool cborGetFloat(const CborValue* val, float* out) {
    if (cbor_value_is_integer(val)) {
        int64_t i;
        if (cbor_value_get_int64(val, &i) != CborNoError) return false;
        *out = (float)i;
        return true;
    }
    if (cbor_value_is_float(val)) {
        return cbor_value_get_float(val, out) == CborNoError;
    }
    if (cbor_value_is_double(val)) {
        double d;
        if (cbor_value_get_double(val, &d) != CborNoError) return false;
        *out = (float)d;
        return true;
    }
    if (cbor_value_is_half_float(val)) {
        // tinycbor stores half-float as uint16; decode manually
        uint16_t hf;
        if (cbor_value_get_half_float(val, &hf) != CborNoError) return false;
        // Convert IEEE 754 half to float
        uint32_t sign     = (hf >> 15) & 0x1;
        uint32_t exp      = (hf >> 10) & 0x1F;
        uint32_t mantissa = hf & 0x3FF;
        uint32_t bits;
        if (exp == 0)       bits = (sign << 31) | ((mantissa) << 13);
        else if (exp == 31) bits = (sign << 31) | (0xFF << 23) | (mantissa << 13);
        else                bits = (sign << 31) | ((exp + 112) << 23) | (mantissa << 13);
        memcpy(out, &bits, 4);
        return true;
    }
    return false;
}

// Locate the OPT NDEF record payload within a raw tag block dump.
// Sets *payloadOut and *payloadLen on success, returns true.
static bool findNdefPayload(const uint8_t* tagBytes, size_t len,
                             const uint8_t** payloadOut, size_t* payloadLen) {
    if (len < OPT_CC_SIZE + 2) return false;

    // TODO: parse the full NDEF TLV structure and locate the record with
    //       MIME type OPT_MIME_TYPE ("application/vnd.openprinttag").
    //       Structure (from nfc_initialize.py):
    //         [0..3]   Capability Container: 0xE1, 0x40, size/8, 0x01
    //         [4..]    TLV sequence:
    //                    0x03 <len> <NDEF message bytes>
    //                    0xFE      Terminator
    //       NDEF record header (TNF=0x02 MIME, SR bit determines payload len width).
    //       Match type field against OPT_MIME_TYPE before returning payload pointer.
    (void)payloadOut;
    (void)payloadLen;
    return false;
}

// ── Public API ────────────────────────────────────────────────────────────────

bool optIsBlank(const uint8_t* tagBytes, size_t len) {
    if (!tagBytes || len < OPT_CC_SIZE) return true;

    // A formatted tag has 0xE1 as the first CC byte (NDEF magic).
    if (tagBytes[0] != 0xE1) return true;

    // If no payload can be located, treat as blank.
    const uint8_t* payload;
    size_t payloadLen;
    if (!findNdefPayload(tagBytes, len, &payload, &payloadLen)) return true;

    // An initialised-but-empty tag has a CBOR payload of just an empty map (0xA0 or 0xBF 0xFF).
    return (payloadLen == 0 || payload[0] == 0xA0 || (payloadLen >= 2 && payload[0] == 0xBF));
}

bool optDecode(const uint8_t* tagBytes, size_t len,
               OptMeta* meta, OptMain* main, OptAuxiliary* aux) {
    const uint8_t* payload;
    size_t payloadLen;
    if (!findNdefPayload(tagBytes, len, &payload, &payloadLen)) return false;

    // ── Decode Meta region (always at offset 0 of payload) ──────────────────
    CborParser parser;
    CborValue  it, map;
    if (cbor_parser_init(payload, payloadLen, 0, &parser, &it) != CborNoError) return false;
    if (!cbor_value_is_map(&it)) return false;

    uint16_t mainOffset = 0, auxOffset = 0;
    if (cbor_value_enter_container(&it, &map) == CborNoError) {
        while (!cbor_value_at_end(&map)) {
            int64_t key;
            if (cbor_value_get_int64(&map, &key) != CborNoError) break;
            cbor_value_advance_fixed(&map);
            switch (key) {
                case META_KEY_MAIN_REGION_OFFSET: {
                    int64_t v; cbor_value_get_int64(&map, &v); mainOffset = (uint16_t)v;
                    if (meta) meta->main_region_offset = mainOffset;
                    break;
                }
                case META_KEY_MAIN_REGION_SIZE:
                    if (meta) { int64_t v; cbor_value_get_int64(&map, &v); meta->main_region_size = (uint16_t)v; }
                    break;
                case META_KEY_AUX_REGION_OFFSET: {
                    int64_t v; cbor_value_get_int64(&map, &v); auxOffset = (uint16_t)v;
                    if (meta) meta->aux_region_offset = auxOffset;
                    break;
                }
                case META_KEY_AUX_REGION_SIZE:
                    if (meta) { int64_t v; cbor_value_get_int64(&map, &v); meta->aux_region_size = (uint16_t)v; }
                    break;
            }
            cbor_value_advance(&map);
        }
        cbor_value_leave_container(&it, &map);
    }

    // ── Decode Main region ────────────────────────────────────────────────────
    if (main && mainOffset < payloadLen) {
        CborParser mp; CborValue mi, mm;
        cbor_parser_init(payload + mainOffset, payloadLen - mainOffset, 0, &mp, &mi);
        if (cbor_value_is_map(&mi) && cbor_value_enter_container(&mi, &mm) == CborNoError) {
            while (!cbor_value_at_end(&mm)) {
                int64_t key;
                if (cbor_value_get_int64(&mm, &key) != CborNoError) break;
                cbor_value_advance_fixed(&mm);
                size_t slen;
                switch (key) {
                    case MAIN_KEY_INSTANCE_UUID:
                        slen = 16;
                        cbor_value_copy_byte_string(&mm, main->instance_uuid, &slen, &mm);
                        continue;
                    case MAIN_KEY_BRAND_NAME:
                        slen = sizeof(main->brand_name);
                        cbor_value_copy_text_string(&mm, main->brand_name, &slen, &mm);
                        continue;
                    case MAIN_KEY_MATERIAL_NAME:
                        slen = sizeof(main->material_name);
                        cbor_value_copy_text_string(&mm, main->material_name, &slen, &mm);
                        continue;
                    case MAIN_KEY_MATERIAL_ABBREVIATION:
                        slen = sizeof(main->material_abbreviation);
                        cbor_value_copy_text_string(&mm, main->material_abbreviation, &slen, &mm);
                        continue;
                    case MAIN_KEY_PRIMARY_COLOR_RGBA:
                        slen = 4;
                        cbor_value_copy_byte_string(&mm, main->primary_color_rgba, &slen, &mm);
                        continue;
                    case MAIN_KEY_NOMINAL_NETTO_FULL_WEIGHT: cborGetFloat(&mm, &main->nominal_netto_full_weight); break;
                    case MAIN_KEY_ACTUAL_NETTO_FULL_WEIGHT:  cborGetFloat(&mm, &main->actual_netto_full_weight);  break;
                    case MAIN_KEY_EMPTY_CONTAINER_WEIGHT:    cborGetFloat(&mm, &main->empty_container_weight);    break;
                    case MAIN_KEY_FILAMENT_DIAMETER:         cborGetFloat(&mm, &main->filament_diameter);         break;
                    case MAIN_KEY_MIN_PRINT_TEMPERATURE: { int64_t v; cbor_value_get_int64(&mm, &v); main->min_print_temperature = (int16_t)v; break; }
                    case MAIN_KEY_MAX_PRINT_TEMPERATURE: { int64_t v; cbor_value_get_int64(&mm, &v); main->max_print_temperature = (int16_t)v; break; }
                    case MAIN_KEY_MIN_BED_TEMPERATURE:   { int64_t v; cbor_value_get_int64(&mm, &v); main->min_bed_temperature   = (int16_t)v; break; }
                    case MAIN_KEY_MAX_BED_TEMPERATURE:   { int64_t v; cbor_value_get_int64(&mm, &v); main->max_bed_temperature   = (int16_t)v; break; }
                    case MAIN_KEY_MATERIAL_CLASS: { int64_t v; cbor_value_get_int64(&mm, &v); main->material_class = (int8_t)v; break; }
                    case MAIN_KEY_MATERIAL_TYPE:  { int64_t v; cbor_value_get_int64(&mm, &v); main->material_type  = (int8_t)v; break; }
                }
                cbor_value_advance(&mm);
            }
            cbor_value_leave_container(&mi, &mm);
        }
    }

    // ── Decode Auxiliary region ───────────────────────────────────────────────
    if (aux && auxOffset > 0 && auxOffset < payloadLen) {
        CborParser ap; CborValue ai, am;
        cbor_parser_init(payload + auxOffset, payloadLen - auxOffset, 0, &ap, &ai);
        if (cbor_value_is_map(&ai) && cbor_value_enter_container(&ai, &am) == CborNoError) {
            while (!cbor_value_at_end(&am)) {
                int64_t key;
                if (cbor_value_get_int64(&am, &key) != CborNoError) break;
                cbor_value_advance_fixed(&am);
                if (key == AUX_KEY_CONSUMED_WEIGHT) cborGetFloat(&am, &aux->consumed_weight);
                cbor_value_advance(&am);
            }
            cbor_value_leave_container(&ai, &am);
        }
    }

    return true;
}

size_t optEncodeMain(const OptMain& m, uint8_t* buf, size_t maxLen) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, maxLen, 0);
    cbor_encoder_create_map(&enc, &map, CborIndefiniteLength);

    cbor_encode_int(&map, MAIN_KEY_INSTANCE_UUID);
    cbor_encode_byte_string(&map, m.instance_uuid, 16);

    cbor_encode_int(&map, MAIN_KEY_BRAND_NAME);
    cbor_encode_text_stringz(&map, m.brand_name);

    cbor_encode_int(&map, MAIN_KEY_MATERIAL_NAME);
    cbor_encode_text_stringz(&map, m.material_name);

    cbor_encode_int(&map, MAIN_KEY_MATERIAL_ABBREVIATION);
    cbor_encode_text_stringz(&map, m.material_abbreviation);

    cbor_encode_int(&map, MAIN_KEY_PRIMARY_COLOR_RGBA);
    cbor_encode_byte_string(&map, m.primary_color_rgba, 4);

    // Encode floats as float32; reference uses CompactFloat (may use int for whole numbers)
    // TODO: mirror CompactFloat logic to minimise tag bytes if space becomes an issue
    cbor_encode_int(&map, MAIN_KEY_NOMINAL_NETTO_FULL_WEIGHT);
    cbor_encode_float(&map, m.nominal_netto_full_weight);

    cbor_encode_int(&map, MAIN_KEY_ACTUAL_NETTO_FULL_WEIGHT);
    cbor_encode_float(&map, m.actual_netto_full_weight);

    cbor_encode_int(&map, MAIN_KEY_EMPTY_CONTAINER_WEIGHT);
    cbor_encode_float(&map, m.empty_container_weight);

    cbor_encode_int(&map, MAIN_KEY_FILAMENT_DIAMETER);
    cbor_encode_float(&map, m.filament_diameter);

    cbor_encode_int(&map, MAIN_KEY_MATERIAL_CLASS);
    cbor_encode_int(&map, m.material_class);

    cbor_encode_int(&map, MAIN_KEY_MATERIAL_TYPE);
    cbor_encode_int(&map, m.material_type);

    cbor_encode_int(&map, MAIN_KEY_MIN_PRINT_TEMPERATURE);
    cbor_encode_int(&map, m.min_print_temperature);

    cbor_encode_int(&map, MAIN_KEY_MAX_PRINT_TEMPERATURE);
    cbor_encode_int(&map, m.max_print_temperature);

    cbor_encode_int(&map, MAIN_KEY_MIN_BED_TEMPERATURE);
    cbor_encode_int(&map, m.min_bed_temperature);

    cbor_encode_int(&map, MAIN_KEY_MAX_BED_TEMPERATURE);
    cbor_encode_int(&map, m.max_bed_temperature);

    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

size_t optEncodeAux(const OptAuxiliary& aux, uint8_t* buf, size_t maxLen) {
    CborEncoder enc, map;
    cbor_encoder_init(&enc, buf, maxLen, 0);
    cbor_encoder_create_map(&enc, &map, CborIndefiniteLength);

    cbor_encode_int(&map, AUX_KEY_CONSUMED_WEIGHT);
    cbor_encode_float(&map, aux.consumed_weight);

    cbor_encoder_close_container(&enc, &map);
    return cbor_encoder_get_buffer_size(&enc, buf);
}

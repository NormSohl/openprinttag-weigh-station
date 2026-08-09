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
static constexpr int MAIN_KEY_WRITE_PROTECTION           = 13;
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

// ── Guarded tinycbor accessors ────────────────────────────────────────────────
//
// tinycbor reports precondition violations with cbor_assert(), which on ESP-IDF
// aborts the firmware. There is no build flag that makes this safe: under NDEBUG
// cbor_assert() degrades to unreachable(), i.e. undefined behaviour, which is
// worse than the abort. The only defence is to test the type before every call.
//
// This is not theoretical. A tag left one block short by the block-79 write
// refusal decoded far enough to reach the Auxiliary map, then ran off the end of
// it, and cbor_value_advance() aborted at cborparser.c:508. nfcTask reads
// whatever is on the scale, so the reboot came straight back on the next boot:
// a loop that only stopped when the spool was lifted off.
//
// A tag whose CBOR is truncated or garbled must decode to "no". Never to a
// reboot.

// cbor_value_get_int64() asserts cbor_value_is_integer(), so checking its return
// value is not a guard — it aborts before it can return anything.
static bool cborTakeInt(const CborValue* v, int64_t* out) {
    if (!cbor_value_is_integer(v)) return false;
    return cbor_value_get_int64(v, out) == CborNoError;
}

// The copy_*_string calls assert the matching string type, and advance the
// iterator themselves when handed `v` as their `next`. True means "value
// consumed, do not step again".
static bool cborTakeText(CborValue* v, char* buf, size_t cap) {
    if (!cbor_value_is_text_string(v)) return false;
    size_t n = cap;
    return cbor_value_copy_text_string(v, buf, &n, v) == CborNoError;
}

// `outLen` receives how many bytes actually arrived, which matters for
// primary_color: OPT lets the alpha channel be left out, so a 3-byte colour is
// legal and means fully opaque.
static bool cborTakeBytes(CborValue* v, uint8_t* buf, size_t cap, size_t* outLen = nullptr) {
    if (!cbor_value_is_byte_string(v)) return false;
    size_t n = cap;
    if (cbor_value_copy_byte_string(v, buf, &n, v) != CborNoError) return false;
    if (outLen) *outLen = n;
    return true;
}

// Step over whatever the iterator points at. cbor_value_advance() handles fixed
// and container types alike (advance_recursive short-circuits to advance_internal
// for fixed ones), so we never need cbor_value_advance_fixed() and never have to
// prove "is this fixed?" — only that the position is valid at all, which is
// exactly the assert that fired.
static bool cborStep(CborValue* v) {
    if (cbor_value_get_type(v) == CborInvalidType) return false;
    return cbor_value_advance(v) == CborNoError;
}

// cbor_value_leave_container() asserts that the inner iterator is exhausted
// (recursed->type == CborInvalidType), so bailing out of a decode loop early and
// then "tidying up" is itself an abort. Only leave a container we actually
// finished; the outer iterator is unused afterwards either way.
static void cborLeave(CborValue* outer, CborValue* inner) {
    if (cbor_value_is_container(outer) && cbor_value_get_type(inner) == CborInvalidType)
        cbor_value_leave_container(outer, inner);
}

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
//
// Tag layout (nfc_initialize.py):
//   [0..3]  Capability Container — must start with 0xE1
//   [4..]   TLV sequence:
//             0x03 <1- or 3-byte length> <NDEF message>
//             0xFE  Terminator
//
// NDEF record header bytes (TNF=0x02 = MIME type):
//   [0]  flags: MB|ME|CF|SR|IL|TNF[2:0]
//   [1]  type_length
//   [2]  payload_length  (1 byte when SR=1, 4 bytes when SR=0)
//   ...  id_length       (only present when IL=1)
//   ...  type            (type_length bytes) — must equal OPT_MIME_TYPE
//   ...  id              (id_length bytes, if IL=1)
//   ...  payload         (payload_length bytes) — our CBOR data
static bool findNdefPayload(const uint8_t* tagBytes, size_t len,
                             const uint8_t** payloadOut, size_t* payloadLen) {
    if (len < OPT_CC_SIZE + 2) return false;
    if (tagBytes[0] != 0xE1) return false;   // not a valid NDEF tag

    // ── Walk TLV sequence starting after the 4-byte Capability Container ──────
    size_t pos = OPT_CC_SIZE;
    while (pos < len) {
        uint8_t tlvType = tagBytes[pos++];
        if (tlvType == 0xFE) break;                        // Terminator
        if (tlvType == 0x00) continue;                     // Null TLV — skip

        // Read TLV length (1-byte or 3-byte form)
        if (pos >= len) return false;
        size_t tlvLen;
        if (tagBytes[pos] == 0xFF) {
            if (pos + 2 >= len) return false;
            tlvLen = ((size_t)tagBytes[pos + 1] << 8) | tagBytes[pos + 2];
            pos += 3;
        } else {
            tlvLen = tagBytes[pos++];
        }
        if (pos + tlvLen > len) return false;

        if (tlvType != 0x03) { pos += tlvLen; continue; }  // not an NDEF TLV

        // ── Parse NDEF message ────────────────────────────────────────────────
        const uint8_t* msg    = tagBytes + pos;
        size_t         msgLen = tlvLen;
        size_t         mpos   = 0;

        while (mpos < msgLen) {
            if (mpos >= msgLen) break;
            uint8_t flags      = msg[mpos++];
            uint8_t tnf        = flags & 0x07;
            bool    sr         = (flags >> 4) & 1;   // Short Record
            bool    il         = (flags >> 3) & 1;   // ID Length present

            if (mpos >= msgLen) break;
            uint8_t typeLen    = msg[mpos++];

            uint32_t plen;
            if (sr) {
                if (mpos >= msgLen) break;
                plen = msg[mpos++];
            } else {
                if (mpos + 3 >= msgLen) break;
                plen = ((uint32_t)msg[mpos] << 24) | ((uint32_t)msg[mpos+1] << 16)
                     | ((uint32_t)msg[mpos+2] << 8) | msg[mpos+3];
                mpos += 4;
            }

            uint8_t idLen = 0;
            if (il) {
                if (mpos >= msgLen) break;
                idLen = msg[mpos++];
            }

            const uint8_t* recType = msg + mpos;   mpos += typeLen;
            mpos += idLen;                          // skip ID field
            const uint8_t* payload = msg + mpos;   mpos += plen;

            if (mpos > msgLen) break;

            // TNF 0x02 = MIME type record
            if (tnf == 0x02
                && typeLen == OPT_MIME_TYPE_LEN
                && memcmp(recType, OPT_MIME_TYPE, OPT_MIME_TYPE_LEN) == 0) {
                *payloadOut = payload;
                *payloadLen = plen;
                return true;
            }
        }

        pos += tlvLen;
    }
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

    // A blank payload is an empty CBOR map: definite 0xA0, or indefinite 0xBF 0xFF.
    // Do NOT treat a non-empty indefinite map (0xBF <key>...) as blank — that is the Meta section.
    if (payloadLen == 0 || payload[0] == 0xA0) return true;
    return (payload[0] == 0xBF && payloadLen >= 2 && payload[1] == 0xFF);
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
            if (!cborTakeInt(&map, &key)) break;   // key must be an integer
            if (!cborStep(&map)) break;            // now on the value
            int64_t v;
            switch (key) {
                case META_KEY_MAIN_REGION_OFFSET:
                    if (cborTakeInt(&map, &v)) {
                        mainOffset = (uint16_t)v;
                        if (meta) meta->main_region_offset = mainOffset;
                    }
                    break;
                case META_KEY_MAIN_REGION_SIZE:
                    if (cborTakeInt(&map, &v) && meta) meta->main_region_size = (uint16_t)v;
                    break;
                case META_KEY_AUX_REGION_OFFSET:
                    if (cborTakeInt(&map, &v)) {
                        auxOffset = (uint16_t)v;
                        if (meta) meta->aux_region_offset = auxOffset;
                    }
                    break;
                case META_KEY_AUX_REGION_SIZE:
                    if (cborTakeInt(&map, &v) && meta) meta->aux_region_size = (uint16_t)v;
                    break;
            }
            if (!cborStep(&map)) break;
        }
        cborLeave(&it, &map);
    }

    // ── Decode Main region ────────────────────────────────────────────────────
    if (main && mainOffset < payloadLen) {
        CborParser mp; CborValue mi, mm;
        if (cbor_parser_init(payload + mainOffset, payloadLen - mainOffset, 0, &mp, &mi) == CborNoError &&
            cbor_value_is_map(&mi) && cbor_value_enter_container(&mi, &mm) == CborNoError) {
            while (!cbor_value_at_end(&mm)) {
                int64_t key;
                if (!cborTakeInt(&mm, &key)) break;
                if (!cborStep(&mm)) break;
                // The string cases advance the iterator themselves; the scalar
                // cases leave it on the value for the step at the bottom. A
                // failed take (wrong type on the tag) falls through to that step
                // too, so an unexpected value is skipped rather than fatal.
                bool consumed = false;
                int64_t v;
                switch (key) {
                    case MAIN_KEY_INSTANCE_UUID:
                        consumed = cborTakeBytes(&mm, main->instance_uuid, 16); break;
                    case MAIN_KEY_BRAND_NAME:
                        consumed = cborTakeText(&mm, main->brand_name, sizeof(main->brand_name)); break;
                    case MAIN_KEY_MATERIAL_NAME:
                        consumed = cborTakeText(&mm, main->material_name, sizeof(main->material_name)); break;
                    case MAIN_KEY_MATERIAL_ABBREVIATION:
                        consumed = cborTakeText(&mm, main->material_abbreviation, sizeof(main->material_abbreviation)); break;
                    case MAIN_KEY_PRIMARY_COLOR_RGBA: {
                        size_t got = 0;
                        consumed = cborTakeBytes(&mm, main->primary_color_rgba, 4, &got);
                        // "The alpha channel can be left out, in which case the
                        // data should have 3 bytes instead of 4 and the color
                        // will be considered fully opaque." Without this a
                        // 3-byte colour from another vendor's tag lands with
                        // alpha 0 and reads as "no colour assigned".
                        if (consumed && got == 3) main->primary_color_rgba[3] = 0xFF;
                        break;
                    }
                    case MAIN_KEY_NOMINAL_NETTO_FULL_WEIGHT: cborGetFloat(&mm, &main->nominal_netto_full_weight); break;
                    case MAIN_KEY_ACTUAL_NETTO_FULL_WEIGHT:  cborGetFloat(&mm, &main->actual_netto_full_weight);  break;
                    case MAIN_KEY_EMPTY_CONTAINER_WEIGHT:    cborGetFloat(&mm, &main->empty_container_weight);    break;
                    case MAIN_KEY_FILAMENT_DIAMETER:         cborGetFloat(&mm, &main->filament_diameter);         break;
                    case MAIN_KEY_MIN_PRINT_TEMPERATURE: if (cborTakeInt(&mm, &v)) main->min_print_temperature = (int16_t)v; break;
                    case MAIN_KEY_MAX_PRINT_TEMPERATURE: if (cborTakeInt(&mm, &v)) main->max_print_temperature = (int16_t)v; break;
                    case MAIN_KEY_MIN_BED_TEMPERATURE:   if (cborTakeInt(&mm, &v)) main->min_bed_temperature   = (int16_t)v; break;
                    case MAIN_KEY_MAX_BED_TEMPERATURE:   if (cborTakeInt(&mm, &v)) main->max_bed_temperature   = (int16_t)v; break;
                    case MAIN_KEY_MATERIAL_CLASS: if (cborTakeInt(&mm, &v)) main->material_class = (int8_t)v; break;
                    case MAIN_KEY_MATERIAL_TYPE:  if (cborTakeInt(&mm, &v)) main->material_type  = (int8_t)v; break;
                    // Whether this tag's Main section may be rewritten at
                    // all. Decoded but never encoded: we do not protect our
                    // own tags, and an absent key means "not protected".
                    case MAIN_KEY_WRITE_PROTECTION: if (cborTakeInt(&mm, &v)) main->write_protection = (int8_t)v; break;
                }
                if (consumed) continue;
                if (!cborStep(&mm)) break;
            }
            cborLeave(&mi, &mm);
        }
    }

    // ── Decode Auxiliary region ───────────────────────────────────────────────
    if (aux && auxOffset > 0 && auxOffset < payloadLen) {
        CborParser ap; CborValue ai, am;
        if (cbor_parser_init(payload + auxOffset, payloadLen - auxOffset, 0, &ap, &ai) == CborNoError &&
            cbor_value_is_map(&ai) && cbor_value_enter_container(&ai, &am) == CborNoError) {
            while (!cbor_value_at_end(&am)) {
                int64_t key;
                if (!cborTakeInt(&am, &key)) break;
                if (!cborStep(&am)) break;
                if (key == AUX_KEY_CONSUMED_WEIGHT) cborGetFloat(&am, &aux->consumed_weight);
                if (!cborStep(&am)) break;   // <- this call aborted the firmware
            }
            cborLeave(&ai, &am);
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

    // Only when a colour is actually assigned. OPT says primary_color "can be
    // null" when a material has no single colour, and an absent key is the
    // honest way to say "unknown" — writing 00 00 00 00 would tell every other
    // reader this spool is transparent black.
    if (m.primary_color_rgba[3] != 0) {
        cbor_encode_int(&map, MAIN_KEY_PRIMARY_COLOR_RGBA);
        cbor_encode_byte_string(&map, m.primary_color_rgba, 4);
    }

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

size_t optPayloadOffset(const uint8_t* tagBytes, size_t len) {
    const uint8_t* payload;
    size_t payloadLen;
    if (!findNdefPayload(tagBytes, len, &payload, &payloadLen)) return SIZE_MAX;
    return (size_t)(payload - tagBytes);
}

size_t optBuildBlankTag(uint8_t numBlocks, uint8_t blockSize,
                        uint8_t* outBuf, size_t outBufLen, OptMeta* outMeta) {
    const size_t tagSize = (size_t)numBlocks * blockSize;
    if (!outBuf || tagSize > outBufLen || numBlocks < 2) return SIZE_MAX;

    // 16 bytes reserved for Meta CBOR at the start of the OPT payload.
    // Meta CBOR with 4 integer keys encodes in ≤13 bytes; 16 gives comfortable headroom.
    static const size_t META_SECTION_SIZE  = 16;
    static const size_t MAIN_REGION_OFFSET = META_SECTION_SIZE;  // = 16
    static const size_t EMPTY_MAP_SIZE     = 2;   // 0xBF 0xFF

    // Bytes reserved for the Auxiliary region.
    //
    // This has to hold a WRITTEN Aux map, not the empty one the format leaves
    // behind. {0: float32} encodes as BF 00 FA xx xx xx xx FF — 8 bytes — and
    // the region used to reserve 2, at the very last block. That is why
    // Auxiliary write-back never worked, and how a tag got bricked: on the
    // shortened layout the block-79 workaround produces, the 8-byte write
    // spanned the final two blocks, the last one refused, and the tag was left
    // carrying a CBOR map that opens and never closes. Every later read then
    // aborted the firmware inside tinycbor.
    //
    // 24 leaves room for the aux fields OPT defines beyond consumed_weight.
    static const size_t AUX_REGION_SIZE = 24;

    // Aux goes at the end, block-aligned (writeSection() addresses whole
    // blocks), with the TLV terminator after it — and the whole of it inside
    // the layout, so no write ever has to reach past the last block. Rounding
    // the start DOWN to a block boundary is what guarantees that.
    const size_t auxTail = AUX_REGION_SIZE + 1;          // region + terminator
    if (tagSize < auxTail + blockSize) return SIZE_MAX;
    const size_t auxAbsStart = ((tagSize - auxTail) / blockSize) * blockSize;

    // ── Choose SR bit and TLV length encoding ────────────────────────────────
    // Start with SR=1 (1-byte payload length, payload ≤ 255 bytes), 1-byte TLV length.
    bool   useSR1       = true;
    size_t ndefHdrSize  = 1 + 1 + 1 + OPT_MIME_TYPE_LEN;  // 31 bytes (SR=1)
    size_t tlvLenSize   = 1;
    size_t payloadStart = OPT_CC_SIZE + 1 + tlvLenSize + ndefHdrSize;

    if (auxAbsStart < payloadStart + MAIN_REGION_OFFSET + EMPTY_MAP_SIZE) return SIZE_MAX;
    size_t cborPayloadLen = auxAbsStart + AUX_REGION_SIZE - payloadStart;
    size_t ndefMsgLen     = ndefHdrSize + cborPayloadLen;

    if (cborPayloadLen > 255) {
        // Payload too large for SR=1; switch to SR=0 (4-byte payload length field).
        useSR1       = false;
        ndefHdrSize  = 1 + 1 + 4 + OPT_MIME_TYPE_LEN;  // 34 bytes (SR=0)
        payloadStart = OPT_CC_SIZE + 1 + tlvLenSize + ndefHdrSize;
        if (auxAbsStart < payloadStart + MAIN_REGION_OFFSET + EMPTY_MAP_SIZE) return SIZE_MAX;
        cborPayloadLen = auxAbsStart + AUX_REGION_SIZE - payloadStart;
        ndefMsgLen     = ndefHdrSize + cborPayloadLen;
    }

    if (ndefMsgLen > 254) {
        // NDEF message too large for 1-byte TLV length; switch to 3-byte (0xFF hi lo).
        tlvLenSize   = 3;
        payloadStart = OPT_CC_SIZE + 1 + tlvLenSize + ndefHdrSize;
        if (auxAbsStart < payloadStart + MAIN_REGION_OFFSET + EMPTY_MAP_SIZE) return SIZE_MAX;
        cborPayloadLen = auxAbsStart + AUX_REGION_SIZE - payloadStart;
        ndefMsgLen     = ndefHdrSize + cborPayloadLen;
    }

    const size_t auxOffset     = auxAbsStart - payloadStart;
    const size_t terminatorPos = auxAbsStart + AUX_REGION_SIZE;
    if (terminatorPos >= tagSize) return SIZE_MAX;

    // ── Write tag bytes ───────────────────────────────────────────────────────
    memset(outBuf, 0, tagSize);

    // Capability Container
    outBuf[0] = 0xE1;         // NDEF magic
    outBuf[1] = 0x40;         // version 1.0, read/write access
    outBuf[2] = numBlocks;
    outBuf[3] = 0x01;         // block size / 8 → 4-byte blocks

    // NDEF TLV
    size_t p = OPT_CC_SIZE;
    outBuf[p++] = 0x03;
    if (tlvLenSize == 1) {
        outBuf[p++] = (uint8_t)ndefMsgLen;
    } else {
        outBuf[p++] = 0xFF;
        outBuf[p++] = (uint8_t)(ndefMsgLen >> 8);
        outBuf[p++] = (uint8_t)(ndefMsgLen & 0xFF);
    }

    // NDEF record header
    outBuf[p++] = useSR1 ? 0xD2 : 0xC2;       // MB=ME=1, SR=(0/1), TNF=0x02
    outBuf[p++] = (uint8_t)OPT_MIME_TYPE_LEN;
    if (useSR1) {
        outBuf[p++] = (uint8_t)cborPayloadLen;
    } else {
        outBuf[p++] = (uint8_t)(cborPayloadLen >> 24);
        outBuf[p++] = (uint8_t)(cborPayloadLen >> 16);
        outBuf[p++] = (uint8_t)(cborPayloadLen >> 8);
        outBuf[p++] = (uint8_t)(cborPayloadLen & 0xFF);
    }
    memcpy(outBuf + p, OPT_MIME_TYPE, OPT_MIME_TYPE_LEN);
    p += OPT_MIME_TYPE_LEN;
    // p == payloadStart here

    // Meta CBOR (at payload offset 0)
    uint8_t metaBuf[24];
    CborEncoder enc, map;
    cbor_encoder_init(&enc, metaBuf, sizeof(metaBuf), 0);
    cbor_encoder_create_map(&enc, &map, CborIndefiniteLength);
    cbor_encode_int(&map, META_KEY_MAIN_REGION_OFFSET); cbor_encode_int(&map, (int64_t)MAIN_REGION_OFFSET);
    cbor_encode_int(&map, META_KEY_MAIN_REGION_SIZE);   cbor_encode_int(&map, 0);
    cbor_encode_int(&map, META_KEY_AUX_REGION_OFFSET);  cbor_encode_int(&map, (int64_t)auxOffset);
    cbor_encode_int(&map, META_KEY_AUX_REGION_SIZE);    cbor_encode_int(&map, (int64_t)AUX_REGION_SIZE);
    cbor_encoder_close_container(&enc, &map);
    size_t metaLen = cbor_encoder_get_buffer_size(&enc, metaBuf);
    if (metaLen > META_SECTION_SIZE) return SIZE_MAX;
    memcpy(outBuf + p, metaBuf, metaLen);

    // Empty Main CBOR (at payload offset MAIN_REGION_OFFSET)
    outBuf[p + MAIN_REGION_OFFSET]     = 0xBF;
    outBuf[p + MAIN_REGION_OFFSET + 1] = 0xFF;

    // Empty Aux CBOR (block-aligned at auxAbsStart)
    outBuf[auxAbsStart]     = 0xBF;
    outBuf[auxAbsStart + 1] = 0xFF;

    // TLV terminator
    outBuf[terminatorPos] = 0xFE;

    if (outMeta) {
        outMeta->main_region_offset = (uint16_t)MAIN_REGION_OFFSET;
        outMeta->main_region_size   = 0;
        outMeta->aux_region_offset  = (uint16_t)auxOffset;
        outMeta->aux_region_size    = (uint16_t)AUX_REGION_SIZE;
    }

    return payloadStart;  // byte offset of OPT CBOR payload within outBuf
}

#include "opt_tag.h"
#include <cbor.h>
#include <string.h>

// Implementation notes:
//   - tinycbor decode: cbor_parser_init → CborParser/CborValue → walk map
//   - tinycbor encode: cbor_encoder_init → cbor_encoder_create_map → encode fields
//     → cbor_encoder_close_container → cbor_encoder_get_buffer_size
//   - All CBOR key strings and value types must match prusa3d/OpenPrintTag exactly.

bool optIsBlank(const uint8_t* buf, size_t len) {
    if (!buf || len == 0) return true;

    // TODO: locate the NDEF TLV wrapper and check for a record with
    //       MIME type "application/vnd.openprinttag".
    //       A blank tag will either be all 0x00/0xFF or carry no valid NDEF message.
    //       Return true in both cases.
    return false;
}

bool optDecode(const uint8_t* buf, size_t len, OptMain* main, OptAuxiliary* aux) {
    if (!buf || len == 0) return false;

    // TODO:
    //   1. Strip the NDEF TLV wrapper to get the raw CBOR payload.
    //   2. Per record.py: the payload encodes a top-level map with three keys —
    //      Meta, Main, Auxiliary — each containing their own CBOR map.
    //   3. Use cbor_parser_init + CborValue iteration to walk the map.
    //   4. For each key string, match against known field names and populate
    //      the appropriate output struct.

    CborParser parser;
    CborValue  it;
    CborError  err = cbor_parser_init(buf, len, 0, &parser, &it);
    if (err != CborNoError) return false;

    // TODO: walk the top-level map, dispatch to Main/Auxiliary sub-maps
    (void)main;
    (void)aux;
    return false;
}

size_t optEncodeMain(const OptMain& main, uint8_t* buf, size_t maxLen) {
    CborEncoder encoder, map;
    cbor_encoder_init(&encoder, buf, maxLen, 0);

    // TODO: open a CBOR map, encode each OptMain field using the exact key
    //       strings from record.py, then close the map.
    //       Example pattern:
    //         cbor_encoder_create_map(&encoder, &map, CborIndefiniteLength);
    //         cbor_encode_text_stringz(&map, "vendor");
    //         cbor_encode_text_stringz(&map, main.vendor);
    //         ...
    //         cbor_encoder_close_container(&encoder, &map);
    (void)main;
    (void)map;

    return cbor_encoder_get_buffer_size(&encoder, buf);
}

size_t optEncodeAux(const OptAuxiliary& aux, uint8_t* buf, size_t maxLen) {
    CborEncoder encoder, map;
    cbor_encoder_init(&encoder, buf, maxLen, 0);

    // TODO: encode remaining_weight, used_weight, timestamp as a CBOR map
    //       using the exact key strings from rec_update.py.
    (void)aux;
    (void)map;

    return cbor_encoder_get_buffer_size(&encoder, buf);
}

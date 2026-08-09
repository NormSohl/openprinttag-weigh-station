// Fuzz optDecode() against malformed OpenPrintTag data, natively.
//
// Why this exists: tinycbor reports precondition violations with cbor_assert(),
// which aborts. A tag left one block short by the block-79 write refusal ran
// optDecode off the end of the Auxiliary map and aborted inside
// cbor_value_advance() (cborparser.c:508), rebooting the station in a loop that
// only stopped when the spool was physically lifted off the scale.
//
// opt_tag.cpp has no Arduino dependency, so the decoder and the real tinycbor
// can both be built on a laptop. Asserts are left ENABLED on purpose: any
// surviving precondition violation aborts this program, which is the signal.
//
// Build (see README.md):
//   g++ -std=c++17 -g -fsanitize=address,undefined -I ../../src -I tinycbor/src \
//       -o optfuzz optfuzz.cpp ../../src/opt_tag.cpp tinycbor/src/*.c
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <random>
#include <cmath>
#include <cbor.h>
#include "opt_tag.h"

// Report the layout for a geometry, and check an Aux write actually fits inside
// it — including the block it would end in, which is what the block-79 refusal
// turns into a corrupt tag.
static bool checkLayout(uint8_t numBlocks, uint8_t blockSize) {
    std::vector<uint8_t> buf((size_t)numBlocks * blockSize, 0);
    OptMeta meta{};
    size_t payloadOff = optBuildBlankTag(numBlocks, blockSize, buf.data(), buf.size(), &meta);
    if (payloadOff == SIZE_MAX) {
        std::printf("  %2ux%u: optBuildBlankTag refused\n", numBlocks, blockSize);
        return false;
    }
    OptAuxiliary a{}; a.consumed_weight = 123.5f;
    uint8_t probe[64];
    size_t auxLen = optEncodeAux(a, probe, sizeof probe);

    size_t auxAbs   = payloadOff + meta.aux_region_offset;
    size_t endByte  = auxAbs + auxLen - 1;
    size_t lastBlk  = endByte / blockSize;
    bool   fits     = auxLen <= meta.aux_region_size && endByte < buf.size();
    bool   clearsEnd = lastBlk < (size_t)numBlocks - 1;   // never needs the final block

    std::printf("  %ux%u: tag %zu B, aux@%zu region %u B, write needs %zu B "
                "ending in block %zu (last is %u) -> %s%s\n",
                numBlocks, blockSize, buf.size(), auxAbs, meta.aux_region_size,
                auxLen, lastBlk, numBlocks - 1,
                fits ? "fits" : "DOES NOT FIT",
                clearsEnd ? ", clears the final block" : ", TOUCHES THE FINAL BLOCK");
    return fits && clearsEnd;
}

// Build a well-formed tag image, then let callers damage it.
static std::vector<uint8_t> goodTag() {
    OptMain m{};
    std::snprintf(m.brand_name, sizeof m.brand_name, "Prusament");
    std::snprintf(m.material_name, sizeof m.material_name, "PETG");
    std::snprintf(m.material_abbreviation, sizeof m.material_abbreviation, "PETG");
    for (int i = 0; i < 16; i++) m.instance_uuid[i] = (uint8_t)(i * 7 + 1);
    m.primary_color_rgba[0] = 0xFF; m.primary_color_rgba[1] = 0x66;
    m.primary_color_rgba[2] = 0x00; m.primary_color_rgba[3] = 0xFF;
    m.nominal_netto_full_weight = 1000.0f;
    m.actual_netto_full_weight  = 1000.0f;
    m.empty_container_weight    = 201.0f;
    m.filament_diameter         = 1.75f;
    m.min_print_temperature = 230; m.max_print_temperature = 250;
    m.min_bed_temperature   = 70;  m.max_bed_temperature   = 90;
    m.material_class = 1; m.material_type = 2;
    // Measured colour: not populated in service yet, but the encode/decode path
    // exists and the round-trip has to hold before anything relies on it.
    m.primary_color_lab[0] = 80.45f;
    m.primary_color_lab[1] = -34.22f;
    m.primary_color_lab[2] = 20.57f;
    m.has_lab = true;

    OptAuxiliary a{};
    a.consumed_weight = 123.5f;

    // Same assembly nfcTask performs: blank layout first, then the two regions
    // written at the offsets Meta advertises.
    const uint8_t numBlocks = 80, blockSize = 4;
    std::vector<uint8_t> buf((size_t)numBlocks * blockSize, 0);
    OptMeta meta{};
    size_t payloadOff = optBuildBlankTag(numBlocks, blockSize, buf.data(), buf.size(), &meta);
    if (payloadOff == SIZE_MAX) { std::fprintf(stderr, "optBuildBlankTag failed\n"); std::exit(2); }

    size_t mainAbs = payloadOff + meta.main_region_offset;
    size_t auxAbs  = payloadOff + meta.aux_region_offset;
    if (mainAbs >= buf.size() || auxAbs >= buf.size() || mainAbs >= auxAbs) {
        std::fprintf(stderr, "region offsets outside the image\n"); std::exit(2);
    }

    // Bound Main by where Aux starts, exactly as writeSection() does on the
    // device — handing the encoder the whole rest of the buffer would let Main
    // run over Aux and produce an image no real tag could hold.
    size_t mainRoom = auxAbs - mainAbs;
    size_t mainLen  = optEncodeMain(m, buf.data() + mainAbs, mainRoom);
    if (!mainLen || mainLen > mainRoom) {
        std::fprintf(stderr, "optEncodeMain: %zu bytes into %zu of room\n", mainLen, mainRoom);
        std::exit(2);
    }

    // Aux is deliberately left as the blank layout's empty map. On an 80x4
    // ICODE SLIX2 the aux region is the last block — 2 bytes of empty map plus
    // a terminator — and writeSection() refuses a write that would run past the
    // end of the tag, so this is what a real onboarded tag actually carries.
    size_t auxRoom = buf.size() - auxAbs;
    uint8_t probe[32];
    size_t auxLen = optEncodeAux(a, probe, sizeof probe);
    std::printf("layout: payload@%zu main@%zu (%zu B used / %zu room)  "
                "aux@%zu (needs %zu B / %zu room)%s\n",
                payloadOff, mainAbs, mainLen, mainRoom, auxAbs, auxLen, auxRoom,
                auxLen > auxRoom ? "   <-- Aux does not fit" : "");
    return buf;
}

static int decodeNoCrash(const uint8_t* p, size_t n) {
    OptMeta meta{}; OptMain main{}; OptAuxiliary aux{};
    return optDecode(p, n, &meta, &main, &aux) ? 1 : 0;
}

int main() {
    // The Aux region has to hold a written map with room to spare, on both the
    // nominal geometry and the one-block-shorter layout nfcTask falls back to
    // when the final block refuses writes.
    std::printf("Aux region fit:\n");
    bool layoutOk = true;
    layoutOk &= checkLayout(80, 4);   // ICODE SLIX2 as reported by getSystemInfo
    layoutOk &= checkLayout(79, 4);   // after the block-79 workaround
    layoutOk &= checkLayout(64, 4);
    layoutOk &= checkLayout(32, 8);
    if (!layoutOk) { std::printf("\nFAIL: Aux does not fit its region\n"); return 1; }
    std::printf("\n");

    std::vector<uint8_t> good = goodTag();
    std::printf("built a %zu-byte reference tag image\n", good.size());

    if (!decodeNoCrash(good.data(), good.size())) {
        std::printf("FAIL: the well-formed image does not decode\n");
        return 1;
    }
    std::printf("reference image decodes: ok\n");

    // Verbatim passthrough. We encode 16 of the spec's 61 Main keys; rewriting a
    // compliant vendor tag must not destroy the other 45. Build a Main map that
    // carries keys we do not model, decode it, re-encode, and check they came
    // back byte for byte.
    {
        // key 57 drying_temperature (int), key 55 country_of_origin (text),
        // key 29 density (float) — three shapes with no case in the decoder.
        // Deliberately NOT key 4: gtin is modelled now, and a modelled key must
        // leave the passthrough or it would be written twice.
        uint8_t hand[64];
        CborEncoder e, m2;
        cbor_encoder_init(&e, hand, sizeof hand, 0);
        cbor_encoder_create_map(&e, &m2, CborIndefiniteLength);
        cbor_encode_int(&m2, 11); cbor_encode_text_stringz(&m2, "Prusament");
        cbor_encode_int(&m2, 57); cbor_encode_int(&m2, 55);
        cbor_encode_int(&m2, 55); cbor_encode_text_stringz(&m2, "CZ");
        cbor_encode_int(&m2, 29); cbor_encode_float(&m2, 1.27f);
        cbor_encoder_close_container(&e, &m2);
        size_t handLen = cbor_encoder_get_buffer_size(&e, hand);

        std::vector<uint8_t> t = good;
        OptMeta meta{};
        size_t payloadOff = optBuildBlankTag(80, 4, t.data(), t.size(), &meta);
        std::memcpy(t.data() + payloadOff + meta.main_region_offset, hand, handLen);

        OptMain in{};
        optDecode(t.data(), t.size(), nullptr, &in, nullptr);

        uint8_t out[320];
        size_t outLen = optEncodeMain(in, out, sizeof out);

        OptMeta m3{}; OptMain back{};
        std::vector<uint8_t> t2 = good;
        optBuildBlankTag(80, 4, t2.data(), t2.size(), &m3);
        std::memcpy(t2.data() + payloadOff + m3.main_region_offset, out, outLen);
        optDecode(t2.data(), t2.size(), nullptr, &back, nullptr);

        const bool ok = in.extra_len > 0 && !in.extra_overflow
                     && back.extra_len == in.extra_len
                     && std::memcmp(back.extra, in.extra, in.extra_len) == 0
                     && std::strcmp(back.brand_name, "Prusament") == 0;
        std::printf("passthrough: %u B of unmodelled keys survived a re-encode "
                    "(%zu B Main) -> %s\n",
                    (unsigned)in.extra_len, outLen, ok ? "ok" : "FAIL");
        if (!ok) return 1;
    }

    // Identity keys 1-4. These used to ride the passthrough; modelling them
    // means they must now survive on their own, and must NOT also appear in
    // extra[] or a rewrite would emit them twice.
    {
        uint8_t hand[96];
        CborEncoder e, m2;
        cbor_encoder_init(&e, hand, sizeof hand, 0);
        cbor_encoder_create_map(&e, &m2, CborIndefiniteLength);
        uint8_t pk[16], ma[16], br[16];
        for (int i = 0; i < 16; i++) { pk[i] = (uint8_t)(0xA0 + i);
                                      ma[i] = (uint8_t)(0xB0 + i);
                                      br[i] = (uint8_t)(0xC0 + i); }
        cbor_encode_int(&m2, 1); cbor_encode_byte_string(&m2, pk, 16);
        cbor_encode_int(&m2, 2); cbor_encode_byte_string(&m2, ma, 16);
        cbor_encode_int(&m2, 3); cbor_encode_byte_string(&m2, br, 16);
        cbor_encode_int(&m2, 4); cbor_encode_uint(&m2, 8594179870000ULL);
        cbor_encoder_close_container(&e, &m2);
        size_t handLen = cbor_encoder_get_buffer_size(&e, hand);

        std::vector<uint8_t> t = good;
        OptMeta meta{};
        size_t off = optBuildBlankTag(80, 4, t.data(), t.size(), &meta);
        std::memcpy(t.data() + off + meta.main_region_offset, hand, handLen);

        OptMain in{};
        optDecode(t.data(), t.size(), nullptr, &in, nullptr);
        uint8_t out[320];
        size_t outLen = optEncodeMain(in, out, sizeof out);
        std::vector<uint8_t> t2 = good;
        OptMeta m3{};
        optBuildBlankTag(80, 4, t2.data(), t2.size(), &m3);
        std::memcpy(t2.data() + off + m3.main_region_offset, out, outLen);
        OptMain back{};
        optDecode(t2.data(), t2.size(), nullptr, &back, nullptr);

        const bool ok = std::memcmp(back.package_uuid,  pk, 16) == 0
                     && std::memcmp(back.material_uuid, ma, 16) == 0
                     && std::memcmp(back.brand_uuid,    br, 16) == 0
                     && back.gtin == 8594179870000ULL
                     && in.extra_len == 0;   // modelled: must leave passthrough
        std::printf("identity round-trip: package/material/brand uuid + gtin "
                    "survive, extra_len=%u -> %s\n",
                    (unsigned)in.extra_len, ok ? "ok" : "FAIL");
        if (!ok) return 1;
    }

    // L*a*b* round-trip. Absent must read as unmeasured, not as a measured
    // black — L*=0 is a legal measurement, so the flag carries that, not the
    // values.
    {
        OptMeta meta{}; OptMain m{}; OptAuxiliary aux{};
        optDecode(good.data(), good.size(), &meta, &m, &aux);
        const bool ok = m.has_lab
                     && std::fabs(m.primary_color_lab[0] - 80.45f) < 0.01f
                     && std::fabs(m.primary_color_lab[1] + 34.22f) < 0.01f
                     && std::fabs(m.primary_color_lab[2] - 20.57f) < 0.01f;
        std::printf("lab round-trip: has_lab=%d L*=%.2f a*=%.2f b*=%.2f -> %s\n",
                    m.has_lab, m.primary_color_lab[0], m.primary_color_lab[1],
                    m.primary_color_lab[2], ok ? "ok" : "FAIL");
        if (!ok) return 1;

        OptMain none{};
        uint8_t buf[256];
        size_t n = optEncodeMain(none, buf, sizeof buf);
        OptMain back{};
        // A Main with no measurement must not emit key 59 at all.
        bool absent = true;
        for (size_t i = 0; i + 1 < n; i++)
            if (buf[i] == 0x18 && buf[i + 1] == 59) absent = false;
        std::printf("lab omitted when unmeasured: %s (%zu B Main)\n",
                    absent ? "ok" : "FAIL — key 59 present", n);
        if (!absent) return 1;
        (void)back;
    }

    long ok = 0;

    // 0) The specific shape the station hit on hardware.
    //
    // Block 79 of the ICODE SLIX2 tags in use refuses writes, so nfcTask
    // reformats one block shorter and Aux lands at block 78 (offset 312). A
    // written Aux map is 8 bytes, which spans blocks 78 AND 79 — so block 78
    // takes the first 4 bytes and block 79 refuses the rest, leaving a map that
    // opens and never closes. Every later read then decodes it.
    {
        OptAuxiliary a{}; a.consumed_weight = 123.5f;
        uint8_t full[32];
        size_t n = optEncodeAux(a, full, sizeof full);
        std::vector<uint8_t> t = good;
        size_t auxAbs = t.size() - 4;          // last block of the shortened layout
        for (size_t i = 0; i < 4 && i < n; i++) t[auxAbs + i] = full[i];  // block 78 only
        std::printf("half-written Aux: %zu-byte map, first 4 bytes land, rest refused\n", n);
        decodeNoCrash(t.data(), t.size());
        ok++;
        std::printf("half-written Aux decoded without abort\n");
    }

    // 1) Every truncation. This is the block-79 failure mode: the tail of the
    //    image never made it onto the tag.
    for (size_t n = 0; n <= good.size(); n++) { decodeNoCrash(good.data(), n); ok++; }
    std::printf("truncations: %zu lengths, no abort\n", good.size() + 1);

    // 2) Every single-byte corruption, at every byte position. Catches damaged
    //    length prefixes and type bytes, which is what steers an iterator into
    //    CborInvalidType.
    for (size_t i = 0; i < good.size(); i++) {
        for (int b = 0; b < 256; b += 7) {
            std::vector<uint8_t> t = good;
            t[i] = (uint8_t)b;
            decodeNoCrash(t.data(), t.size());
            ok++;
        }
    }
    std::printf("single-byte corruptions: %zu variants, no abort\n", good.size() * 37);

    // 3) Random damage: several bytes at once, plus random truncation.
    std::mt19937 rng(12345);
    for (int iter = 0; iter < 200000; iter++) {
        std::vector<uint8_t> t = good;
        int hits = 1 + (int)(rng() % 8);
        for (int h = 0; h < hits; h++) t[rng() % t.size()] = (uint8_t)(rng() & 0xFF);
        if (rng() % 3 == 0) t.resize(1 + rng() % t.size());
        decodeNoCrash(t.data(), t.size());
        ok++;
    }
    std::printf("random damage: 200000 iterations, no abort\n");

    // 4) Pure noise that still carries the 0xE1 NDEF magic, so it gets past
    //    findNdefPayload() and reaches the CBOR parsers.
    for (int iter = 0; iter < 200000; iter++) {
        size_t n = 8 + rng() % 300;
        std::vector<uint8_t> t(n);
        for (auto& c : t) c = (uint8_t)(rng() & 0xFF);
        t[0] = 0xE1;
        decodeNoCrash(t.data(), t.size());
        ok++;
    }
    std::printf("random NDEF-magic noise: 200000 iterations, no abort\n");

    std::printf("\nPASS: %ld decodes, none aborted\n", ok);
    return 0;
}

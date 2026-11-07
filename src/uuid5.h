// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl
//
// UUIDv5 (RFC 9562: SHA-1 of namespace + name), for the one OpenPrintTag
// identifier a reader derives itself: a tag that carries no instance_uuid has
// UUIDv5(OPT_NS_INSTANCE, nfc_uid) (nfc_data_format.md 3.2.0.2), with the
// ISO 15693 UID as 8 bytes, most significant first (E0 ...). A writer putting
// the same data on many tags is told to leave the key out, so a compliant
// vendor tag may arrive without one.
//
// Self-contained (a small SHA-1 here rather than mbedtls) so it compiles the
// same on the board and in the native test harnesses.
#pragma once
#include <stdint.h>
#include <string.h>

namespace uuid5_detail {
inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline void sha1(const uint8_t* msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const uint64_t bits = (uint64_t)len * 8;
    const size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (size_t i = 0; i < 64; i++) {
            const size_t p = off + i;
            blk[i] = p < len ? msg[p] : p == len ? 0x80 : 0;
        }
        if (off + 64 == total)
            for (int i = 0; i < 8; i++) blk[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 |
                   (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}
}  // namespace uuid5_detail

// namespace: 16 bytes; name: any bytes. out: 16 bytes.
inline void uuid5(const uint8_t ns[16], const uint8_t* name, size_t nameLen, uint8_t out[16]) {
    uint8_t buf[16 + 80];
    if (nameLen > 80) nameLen = 80;
    memcpy(buf, ns, 16);
    memcpy(buf + 16, name, nameLen);
    uint8_t d[20];
    uuid5_detail::sha1(buf, 16 + nameLen, d);
    memcpy(out, d, 16);
    out[6] = (out[6] & 0x0F) | 0x50;
    out[8] = (out[8] & 0x3F) | 0x80;
}

// OpenPrintTag's instance namespace, 31062f81-b5bd-4f86-a5f8-46367e841508.
static const uint8_t OPT_NS_INSTANCE[16] = {0x31, 0x06, 0x2f, 0x81, 0xb5, 0xbd, 0x4f, 0x86,
                                            0xa5, 0xf8, 0x46, 0x36, 0x7e, 0x84, 0x15, 0x08};

// The other three namespaces (nfc_data_format.md 3.2.1), for a tag that leaves
// its brand / material / package UUID out: brand = UUIDv5(NS_BRAND, brand
// name), material = UUIDv5(NS_MATERIAL, brand_uuid bytes + material name),
// package = UUIDv5(NS_PACKAGE, brand_uuid bytes + GTIN as decimal text).
// tools/esun-catalogue/station_catalogue.py derives the same way, so a tag and
// a catalogue pick of the same item meet on the same IDs.
static const uint8_t OPT_NS_BRAND[16]    = {0x52, 0x69, 0xdf, 0xb7, 0x15, 0x59, 0x44, 0x0a,
                                            0x85, 0xbe, 0xab, 0xa5, 0xf3, 0xef, 0xf2, 0xd2};
static const uint8_t OPT_NS_MATERIAL[16] = {0x61, 0x6f, 0xc8, 0x6d, 0x7d, 0x99, 0x49, 0x53,
                                            0x96, 0xc7, 0x46, 0xd2, 0x83, 0x6b, 0x9b, 0xe9};
static const uint8_t OPT_NS_PACKAGE[16]  = {0x6f, 0x7d, 0x48, 0x5e, 0xdb, 0x8d, 0x49, 0x79,
                                            0x90, 0x4e, 0xa2, 0x31, 0xcd, 0x66, 0x02, 0xb2};

// brand_uuid bytes followed by `text`, for the material and package UUIDs.
inline void uuid5After(const uint8_t ns[16], const uint8_t brand[16], const char* text, uint8_t out[16]) {
    uint8_t name[16 + 64];
    size_t n = strlen(text);
    if (n > 64) n = 64;
    memcpy(name, brand, 16);
    memcpy(name + 16, text, n);
    uuid5(ns, name, 16 + n, out);
}

// The derived instance_uuid for a chip. `uid` as the reader returns it: the
// PN5180 library hands it over least significant byte first (uid[7] == 0xE0,
// the ISO 15693 manufacturer prefix), so it is reversed to the spec's
// MSB-first order; a UID already MSB-first (uid[0] == 0xE0) is used as is.
inline void optDeriveInstanceUuid(const uint8_t uid[8], uint8_t out[16]) {
    uint8_t msb[8];
    if (uid[0] == 0xE0 && uid[7] != 0xE0) memcpy(msb, uid, 8);
    else for (int i = 0; i < 8; i++) msb[i] = uid[7 - i];
    uuid5(OPT_NS_INSTANCE, msb, 8, out);
}

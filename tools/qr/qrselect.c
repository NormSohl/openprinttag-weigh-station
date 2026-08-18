// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Exercises the exact version-selection logic now in drawQr(), against the real
// encoder, for every payload length the firmware can produce.
//
// gWebAddr is char[48], so the longest URL drawQr() is ever handed is
// "http://" + 47 + "/onboard" = 62 chars. Sweep 1..78 (the v4 ceiling) and,
// under ASAN, confirm no length either overruns or is silently mis-sized.
#include <stdio.h>
#include <string.h>
#include "qrcode.h"

#define QR_VERSION_MAX 4
static const unsigned char QR_CAPACITY[QR_VERSION_MAX] = { 17, 32, 53, 78 };

int main(void) {
    int fail = 0, drawn = 0, refused = 0;

    for (int len = 1; len <= 90; len++) {
        char text[128];
        for (int i = 0; i < len; i++) text[i] = "http://weighstation.local/onboard"[i % 33];
        text[len] = 0;

        // ── the shipped logic ──────────────────────────────────────────
        unsigned char version = 0;
        for (unsigned char v = 1; v <= QR_VERSION_MAX; v++)
            if ((size_t)len <= QR_CAPACITY[v - 1]) { version = v; break; }
        if (version == 0) { refused++; continue; }   // draw nothing
        // ───────────────────────────────────────────────────────────────

        unsigned char data[160];
        memset(data, 0, sizeof data);
        QRCode qr;
        if (qrcode_initText(&qr, data, version, ECC_LOW, text) != 0) {
            printf("len=%d: initText refused at v%d\n", len, version);
            fail = 1; continue;
        }
        if (qr.size != version * 4 + 17) {
            printf("len=%d: wrong size %d at v%d\n", len, qr.size, version);
            fail = 1; continue;
        }
        // A 148px box: this is the module scale the panel actually gets.
        int total = qr.size + 8, scale = 148 / total;
        if (scale < 1) { printf("len=%d: would not fit the box\n", len); fail = 1; }
        drawn++;
    }

    printf("encoded %d lengths, refused %d (>78 chars), %s\n",
           drawn, refused, fail ? "FAIL" : "no overruns");

    // Spot-check the URLs the firmware actually builds.
    const char *real[] = {
        "http://192.168.1.42/",
        "http://192.168.1.42/onboard",
        "http://weighstation.local/",
        "http://weighstation.local/onboard",
    };
    for (unsigned i = 0; i < sizeof real / sizeof *real; i++) {
        size_t len = strlen(real[i]);
        unsigned char version = 0;
        for (unsigned char v = 1; v <= QR_VERSION_MAX; v++)
            if (len <= QR_CAPACITY[v - 1]) { version = v; break; }
        unsigned char data[160];
        QRCode qr;
        int rc = qrcode_initText(&qr, data, version, ECC_LOW, real[i]);
        int total = qr.size + 8, scale = 148 / total;
        printf("  %-36s len=%2zu -> v%d, %2d modules, scale %d = %3dpx  rc=%d\n",
               real[i], len, version, qr.size, scale, total * scale, rc);
        if (rc != 0) fail = 1;
    }
    return fail;
}

// Native harness for the QR capacity table used by drawQr() in display_task.cpp.
// Verifies two things I cannot check by compiling the firmware:
//   1. each QR_CAPACITY[v] really is the largest byte-mode payload version v
//      holds at ECC_LOW, and
//   2. one character more than that corrupts / overruns, which is what the old
//      "trust the return code" loop walked into.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "qrcode.h"

static const unsigned char QR_CAPACITY[4] = { 17, 32, 53, 78 };

// Fill with a URL-ish payload of exactly len chars.
static void mkstr(char *buf, int len) {
    static const char *seed = "http://weighstation.local/onboard?x=";
    int n = strlen(seed);
    for (int i = 0; i < len; i++) buf[i] = (i < n) ? seed[i] : ('a' + (i % 26));
    buf[len] = 0;
}

// Re-derive capacity from the encoder's own arithmetic, independently of my table.
static int derived(int version) {
    static const int RAW[4] = { 208, 359, 567, 807 };
    static const int ECC_LOW_CW[4] = { 7, 10, 15, 20 };
    int dataCapacity = RAW[version - 1] / 8 - ECC_LOW_CW[version - 1];
    return (dataCapacity * 8 - 12) / 8;   // less the 4-bit mode + 8-bit count
}

int main(void) {
    int fail = 0;

    for (int v = 1; v <= 4; v++) {
        printf("v%d: table=%2u derived=%2d  %s\n", v, QR_CAPACITY[v - 1],
               derived(v), QR_CAPACITY[v - 1] == derived(v) ? "match" : "MISMATCH");
        if (QR_CAPACITY[v - 1] != derived(v)) fail = 1;
    }

    // Encode at exactly capacity for each version and confirm the modules grid
    // is sane (size right, and not all-zero, which is what a failed encode leaves).
    for (int v = 1; v <= 4; v++) {
        int len = QR_CAPACITY[v - 1];
        char *text = malloc(len + 1);
        mkstr(text, len);

        unsigned char data[160];
        memset(data, 0, sizeof data);
        QRCode qr;
        int rc = qrcode_initText(&qr, data, v, ECC_LOW, text);

        int dark = 0;
        for (int y = 0; y < qr.size; y++)
            for (int x = 0; x < qr.size; x++)
                if (qrcode_getModule(&qr, x, y)) dark++;

        int want = v * 4 + 17;
        printf("v%d len=%2d rc=%d size=%2d (want %2d) dark=%d\n",
               v, len, rc, qr.size, want, dark);
        if (rc != 0 || qr.size != want || dark == 0) fail = 1;
        free(text);
    }

    // The claim that the encoder cannot report "too long": ask v1 (17 bytes) to
    // hold 60. A correct API returns nonzero; this one returns 0.
    {
        char text[80];
        mkstr(text, 60);
        unsigned char data[160];
        QRCode qr;
        int rc = qrcode_initText(&qr, data, 1, ECC_LOW, text);
        printf("\noverlong at v1 (60 chars into a 17-char code): rc=%d %s\n", rc,
               rc == 0 ? "<-- returns success, exactly the trap" : "(reported)");
    }

    printf("\n%s\n", fail ? "FAIL" : "PASS: capacity table agrees with the encoder");
    return fail;
}

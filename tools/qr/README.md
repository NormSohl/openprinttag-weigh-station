# QR encoder harnesses

Bench tests for the QR code `drawQr()` puts on the TFT. The encoder
(ricmoo/QRCode) is plain C with no Arduino dependency, so this runs on a
laptop — **no board, no toolchain, no flashing.**

Worth having because the encoder has a trap in it:

`qrcode_initText()` **cannot report "string too long."** Its only failure
path is `mode < 0`, and `encodeDataCodewords()` returns a mode constant
unconditionally in byte mode, so it returns 0 for any input. And
`bb_appendBits()` has no bounds check — it writes straight to
`data[offset >> 3]`. An over-long string does not fail; it overruns
`codewordBytes[]`, a C99 variable-length array **on the calling task's
stack**.

So `drawQr()` must range-check the payload itself, against a table of
byte-mode capacities. These two programs check that it does.

## Running

```sh
git clone --depth 1 https://github.com/ricmoo/QRCode.git
gcc -std=c99 -O0 -g -fsanitize=address,undefined -I QRCode/src \
    -o qrselect qrselect.c QRCode/src/qrcode.c && ./qrselect
```

`qrtest.c` builds the same way. Keep the sanitizers on — the whole point is
to catch a buffer overrun, and without them the failure is a silent wrong
answer (or, at 60 characters, a bare `Segmentation fault`).

## What each one checks

**`qrtest.c`** — that `QR_CAPACITY` in `display_task.cpp` is right. It
re-derives each number from the encoder's own tables
(`NUM_RAW_DATA_MODULES/8 - NUM_ERROR_CORRECTION_CODEWORDS[Low]`, less the
12-bit byte-mode header) and then encodes a string of exactly that length at
that version. It also demonstrates the trap directly: 60 characters into a
version-1 code returns **rc=0** and overruns.

```
v1: table=17 derived=17  match      v1 len=17 rc=0 size=21 dark=242
v2: table=32 derived=32  match      v2 len=32 rc=0 size=25 dark=322
v3: table=53 derived=53  match      v3 len=53 rc=0 size=29 dark=439
v4: table=78 derived=78  match      v4 len=78 rc=0 size=33 dark=541
```

**`qrselect.c`** — the version-selection loop exactly as shipped, swept over
every payload length the firmware can produce (`gWebAddr` is `char[48]`, so
the longest URL is `"http://" + 47 + "/onboard"` = 62), plus the four real
URLs. Expected:

```
encoded 78 lengths, refused 12 (>78 chars), no overruns
  http://192.168.1.42/                 len=20 -> v2, 25 modules, scale 4 = 132px
  http://192.168.1.42/onboard          len=27 -> v2, 25 modules, scale 4 = 132px
  http://weighstation.local/           len=26 -> v2, 25 modules, scale 4 = 132px
  http://weighstation.local/onboard    len=33 -> v3, 29 modules, scale 4 = 148px
```

That last line is why this matters. 33 characters is one over version 2's
32-byte capacity. The original selection loop trusted the return code, so it
stopped at the first version that "succeeded" — which was always the first
one tried — and produced an unscannable code for precisely the mDNS-plus-
`/onboard` URL the NEEDS ONBOARDING screen shows. Past ~44 characters it
stopped being a display bug and became a stack smash in `displayTask`.

## If you change `QR_VERSION_MAX`

Resize `qrData[]` in `drawQr()` to match — it is hand-sized because
`qrcode_getBufferSize()` is a runtime call. The requirement is
`((4*version + 17)^2 + 7) / 8` bytes: 137 at v4, hence 160. Then extend
`QR_CAPACITY` and re-run both harnesses.

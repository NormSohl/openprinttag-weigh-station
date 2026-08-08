# optDecode fuzz harness

Feeds malformed OpenPrintTag images to `optDecode()` on a laptop — **no board,
no toolchain, no flashing**. `src/opt_tag.cpp` includes only `opt_tag.h`,
`<cbor.h>` and `<string.h>`, so the decoder and the real tinycbor both build
natively.

## Why it exists

tinycbor reports precondition violations with `cbor_assert()`, which aborts.
There is no build flag that makes this safe — under `NDEBUG` `cbor_assert()`
degrades to `unreachable()`, i.e. undefined behaviour, which is worse than the
abort. The only defence is to check the type before every call, and the only way
to know you actually did is to try to break it.

This caught a live reboot loop. `nfcTask` reads whatever is on the scale, so an
undecodable tag crashed the station on every boot — a loop that only stopped
when the spool was physically lifted off.

## Running

```sh
git clone --depth 1 https://github.com/intel/tinycbor.git
g++ -std=c++17 -O0 -g -fsanitize=address,undefined -I ../../src -I shim -I tinycbor/src \
    -o optfuzz optfuzz.cpp ../../src/opt_tag.cpp \
    tinycbor/src/cborparser.c tinycbor/src/cborencoder.c \
    tinycbor/src/cborparser_dup_string.c tinycbor/src/cborerrorstrings.c
./optfuzz
```

Name those four `.c` files explicitly — `tinycbor/src/*.c` pulls in
`cbortojson.c`, which does not compile as C++.

`shim/` supplies the two headers tinycbor's build system normally generates
(`tinycbor-export.h`, `tinycbor-version.h`); without them `cbor.h` will not
include.

**Leave asserts enabled** — do not add `-DNDEBUG`. A surviving precondition
violation aborting this program *is* the signal.

If ASan complains that its runtime "does not come first", preload it:

```sh
LD_PRELOAD=$(gcc -print-file-name=libasan.so) ./optfuzz
```

## What it checks

A reference tag is built the way `nfcTask` builds one — `optBuildBlankTag()`,
then `optEncodeMain()` bounded by where Aux starts — and then damaged five ways:

| Case | Coverage |
|---|---|
| **Half-written Aux** | The exact hardware failure; see below |
| Truncation | Every length from 0 to the full image |
| Single-byte corruption | Every byte position × 37 values |
| Random damage | 200 000 images, 1–8 random bytes, sometimes truncated |
| NDEF-magic noise | 200 000 random buffers starting `0xE1`, so they reach the CBOR parsers |

Expected tail:

```
PASS: 412162 decodes, none aborted
```

## The half-written Aux case

This is the one that reproduces the field crash, and it is worth understanding
because the layout that causes it is still there.

Block 79 of the ICODE SLIX2 tags in use refuses writes, so `nfcTask` reformats
one block shorter and Aux lands at block 78. But a written Aux map is **8 bytes
and the aux region is 4** — it spans blocks 78 *and* 79. Block 78 takes the
first four bytes, block 79 refuses the rest, and the tag is left carrying a CBOR
map that opens and never closes. Every later read decodes it.

Against the pre-fix decoder that reproduces the field backtrace exactly:

```
optfuzz_old: cborparser.c:508: cbor_value_advance: Assertion `it->type != CborInvalidType' failed
```

The harness prints the layout arithmetic on every run, so the shortfall is
visible without reading any code:

```
layout: payload@42 main@58 (95 B used / 258 room)  aux@316 (needs 8 B / 4 room)   <-- Aux does not fit
```

Hardening `optDecode()` stops the crash — a bad tag now reads as "undecodable"
and the station stays up. It does **not** make Auxiliary write-back work; that
needs the region sized to hold a real Aux map.

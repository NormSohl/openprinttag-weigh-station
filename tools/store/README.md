# tools/store — `store.cpp`, compiled and run on the host

`./run.sh` builds and runs everything here. It needs ArduinoJson:

```
git clone --depth 1 https://github.com/bblanchon/ArduinoJson ../../.deps/ArduinoJson
./run.sh
```

## Why this exists

The PlatformIO **registry** is unreachable from the Claude Code sandbox, so
firmware changes made there get no compiler at all — only a real `pio run`
proves them. But the registry is the only thing blocked; GitHub is reachable,
and `store.cpp`'s dependencies are small enough to shim. `shim/` supplies just
enough `Arduino.h` / `LittleFS.h` / `Preferences.h` / FreeRTOS for the real
`src/store.cpp` to compile and **run**, with LittleFS over a scratch directory
and NVS over a flat file — so the counters diverge across a simulated reboot
the way the real partitions do, which is the exact condition
`reconcileIdCounter_()` exists to repair.

That buys two things the sandbox otherwise cannot have:

1. **`store.cpp` is compile-verified.** It is the largest and most-changed file
   in the project.
2. **The compaction fold is verifiable without hardware.** `run.sh` runs
   `SEED 20 200` → `DUMP usage` → `COMPACT` → `DUMP usage` and diffs the two,
   which is the one check whose regression cannot be recovered afterwards: once
   raw events are folded away, the `Usage` rows are the only evidence left. The
   figures match the 2026-08-08 hardware run exactly — 4 buckets, 4477.5 g and
   995 weighs each, 17910.0 g total.

`--products` covers what no serial command reaches: adoption converging on one
product rather than one per spool, a disagreeing tag being reported without
updating anything, an edit propagating to its spools, and products surviving a
fold. It found a real bug on its first run — `productDiffers_()` was not
comparing the tare, the field where a silent disagreement does the most damage,
since remaining weight is gross minus tare.

**This does not replace `pio run`.** Everything outside `store.cpp` — the tasks,
the web app, the display — still has no compiler here.

## The matcher slice

`storeFindProduct()` decides whether the tag on the scale is *another spool of
something we already stock* or *a new product*. Both wrong answers are silent:

- **Missed match** → a second product is created, and the inventory lists the
  same filament twice. The reorder threshold then guards half a shelf each.
- **Over-match** → two different filaments merge into one row. A 1 kg and a
  5 kg of the same product carry the identical `material_name`, so this is the
  easy mistake to make, and it defeats the decision that a different size is a
  different product.

Neither shows up as an error, on the bench or in service — which is why this is
tested off the board.

`extract_match.py` slices `ProductRecord`,
`normEq_`, `nomEq_` and `findProduct_` **verbatim** out of `src/store.h` and
`src/store.cpp` into a generated header. The test therefore exercises the
shipping bytes, and it cannot drift the way a transcribed copy would. If a
refactor moves the markers the extractor brackets on, it exits with an error
rather than testing something stale.

This also makes those functions compile-verified, which is otherwise not
available for anything in `src/`.

## Run

```
python3 extract_match.py
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -o match_test match_test.cpp
LD_PRELOAD=$(gcc -print-file-name=libasan.so) ./match_test
```

`match_extract.h` and `match_test` are build products; they are not committed.

## What it asserts

Each rung of the ladder in isolation, and the cases where the rungs disagree:

| check | why it is here |
|---|---|
| `eSun` matches `ESUN`, `PLA Summer` matches `PLA  Summer` | vendors and product names are typed by hand |
| `PLA` does **not** match `PLA+` | whitespace is skipped, punctuation is not — these are different filaments |
| nominal weight 0 does not block a match | a tag carrying no weight offers nothing to distinguish on |
| 1 kg and 5 kg stay apart under rules 3 and 4 | the whole reason nominal weight is in the key |
| `package_uuid` beats `gtin` beats `material_uuid` beats names | the ladder order is the design's, not an accident |
| an unknown `package_uuid` still falls through to the name rule | a vendor UUID we have not seen must not veto a good name match |
| an empty probe matches nothing | must not silently adopt row 0 |

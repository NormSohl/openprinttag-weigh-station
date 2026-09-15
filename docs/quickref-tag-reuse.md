# Quick Reference — Tag Reuse

> **Not part of the current day-to-day workflow (as of 2026-09-15).** The
> feature exists and works, but the lab has decided not to use it for now —
> the default is still one new tag per new spool (see
> [quickref-onboarding.md](quickref-onboarding.md)). Keep this page for if
> and when that changes.

Tags are expensive enough that reusing a spent one — rather than buying a new
one for every reel — can be worth the handling effort. This page covers the
*digital* side: wiping a tag's identity so it can be onboarded fresh. It says
nothing about the physical handling (peeling, re-adhering) — that's a
separate, unresolved cost and is up to whoever's doing it.

## What it does

Turns **every tag placed on the scale** into "treat as blank," so it goes
through the exact same erase-and-reformat step a genuinely blank tag gets —
no separate mechanism, no per-tag confirmation click. It's a batch **mode**,
meant for processing a bin of already-decided-empty spools at once, not a
one-off button.

## Turning it on

1. Browse to `http://weighstation.local/reuse`.
2. Click **Start reuse mode.** One confirmation dialog — that's the entire
   "are you sure," deliberately at the batch level rather than per tag.
3. The page now shows **"Reuse mode: ON"** and polls live status.

## Processing a stack of spools

For each spool you've decided is genuinely spent:

1. **Place it on the scale.** Because reuse mode is on, the display shows the
   same **"New tag found / Remove to cancel / Registering in: 2…"** countdown
   a real blank tag gets — same red-at-1s warning, same cancel-by-removal.
2. **Leave it in place** to confirm — the tag is erased.
3. **Remove it, place the next one.** Repeat.

**The tag comes out genuinely blank** — no placeholder "Unknown / needs
onboarding" record is created. It re-enters the normal new-tag pipeline the
next time it's placed for real, same as an out-of-the-box tag.

## Turning it off

Click **Stop reuse mode** on the same `/reuse` page. Do this before normal
onboarding resumes — while the mode is on, *any* tag placed (including a
spool you meant to weigh normally) is treated as blank and erased.

## Safety notes

- No automatic "this spool looks empty" detection — a person decides a spool
  is spent and physically sets it aside; the station never erases based on
  weight alone.
- Identity, not the physical chip, is what's reused: erasing just clears the
  OpenPrintTag `instance_uuid` on the tag. A fresh one is minted at the next
  real onboarding, same as any new tag.
- If a tag with the same physical chip ID as an existing spool record goes
  blank by any means (this mode, `TAGFORMAT`, or a third-party tool), that
  record is automatically retired — no separate step needed.

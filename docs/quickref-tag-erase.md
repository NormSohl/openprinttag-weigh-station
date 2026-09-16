# Quick Reference — Tag Erase

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

Turns **every tag placed on the scale** into "erase it immediately" — no
countdown, no per-tag confirmation click, no separate mechanism. It's a batch
**mode**, meant for processing a bin of already-decided-empty spools at once,
not a one-off button.

## Turning it on

1. Browse to `http://weighstation.local/erase`.
2. Click **Start erase mode.** One confirmation dialog — that's the entire
   "are you sure," deliberately at the batch level rather than per tag.
3. The page now shows **"Erase mode: ON"** and polls live status.
4. **The device screen changes immediately too** — the idle screen shows
   **"ERASE MODE"** in red the instant you click Start, not just once a tag
   is placed, so it's obvious to anyone standing at the station that every
   tag placed from now on is about to be wiped.

## Processing a stack of spools

For each spool you've decided is genuinely spent:

1. **Place it on the scale.** Because erase mode is on, it's erased
   **immediately** — there is no countdown and nothing to cancel. The screen
   shows "Erasing tag..." briefly, then **"Tag erased / Remove tag, place the
   next one."**
2. **Remove it, place the next one.** Repeat.

**The tag comes out genuinely blank** — no placeholder "Unknown / needs
onboarding" record is created. It re-enters the normal new-tag pipeline the
next time it's placed for real, same as an out-of-the-box tag.

## Turning it off

Click **Stop erase mode** on the same `/erase` page. Do this before normal
onboarding resumes — while the mode is on, *any* tag placed (including a
spool you meant to weigh normally) is erased immediately, with no chance to
back out.

## Safety notes

- No automatic "this spool looks empty" detection — a person decides a spool
  is spent and physically sets it aside; the station never erases based on
  weight alone.
- **No confirm window on the tag itself.** Because a real blank tag's
  countdown exists to protect someone who *isn't* watching when it lands,
  and erase mode is the opposite case — staff are standing right there
  feeding a pre-sorted bin through one at a time — placing a tag while this
  mode is on erases it right away, same as the `TAGFORMAT` recovery command.
  Stop the mode before setting the station down.
- Identity, not the physical chip, is what's reused: erasing just clears the
  OpenPrintTag `instance_uuid` on the tag. A fresh one is minted at the next
  real onboarding, same as any new tag.
- If a tag with the same physical chip ID as an existing spool record goes
  blank by any means (this mode, `TAGFORMAT`, or a third-party tool), that
  record is automatically retired — no separate step needed.

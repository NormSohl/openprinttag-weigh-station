# Quick Reference — Tag Erase

## What it does

Erases **every tag placed on the scale** — you can quickly erase one spool
or a whole batch.

## Turning it on

1. Browse to `http://weighstation.local/erase`.
2. Click **Start erase mode.** One confirmation dialog — the entire "are you
   sure," deliberately at the batch level rather than per tag.
3. The page shows **"Erase mode: ON"** and polls live status.
4. **The device screen changes immediately too** — the idle screen shows
   **"ERASE MODE"** in red, not just once a tag is placed — obvious to
   anyone standing at the station.

## Erasing a spool

1. **Place a spool on the scale.** Erased **immediately** — no countdown,
   nothing to cancel. The screen shows "Erasing tag...", then "Tag erased /
   Remove tag, place the next one."
2. **Remove it, place the next one.** Repeat.

The tag comes out **genuinely blank** — no placeholder "Unknown / needs
onboarding" record is created. It re-enters the normal new-tag pipeline the
next time it's placed for real.

## Turning it off

Click **Stop erase mode** on the same page. Do this before normal onboarding
resumes — while it's on, *any* tag placed (including a spool you meant to
weigh normally) is erased immediately, with no chance to back out.

**If you forget:** the station now turns erase mode off on its own after 10
minutes with no tag placed, so it can't be left armed indefinitely if
someone walks away. Placing a tag (or turning the mode on) resets that clock.

## Safety notes

- **No confirm window on the tag itself.** Staff are standing right there
  feeding a pre-sorted bin through, so placing a tag while this mode is on
  erases it right away, same as the `TAGFORMAT` recovery command.
- If a tag with the same physical chip ID as an existing spool record is
  erased by any means, that record is automatically retired — no separate
  step needed.

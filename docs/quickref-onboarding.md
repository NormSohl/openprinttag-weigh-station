# Quick Reference — Onboarding a New Spool

*Print this page and keep it at the station. Full detail: [user-manual.md](user-manual.md).*

For each new, untagged reel.

## Steps

1. **Stick an OpenPrintTag sticker** on the flat hub face, centered. Press firmly.
2. **Place the spool on the scale.**
3. The display counts down from 2 ("New tag found / Remove to cancel").
   - **Remove the spool** to cancel.
   - **Leave it** to register.
4. The display shows **"Registered! Spool #N / NEEDS ONBOARDING"** and a QR code.
5. **Scan the QR code**, or open `weighstation.local/onboard`. Keep the spool
   on the scale.
6. Under **"This spool is…"**:
   - **Same product as one on file?** Pick it from the list.
   - **New product?** Use the catalog search. The catalog fields show the
     last product picked. Same product? Leave them. Different? Click a field
     to change it.
   - **Not in the catalog?** Open **Or enter manually**. Use **+ Add new** for
     anything missing.
7. **Set the tare** (required): pick a **spool profile**, type it, or put an
   empty spool of the same type on the scale and press **Capture tare**.
8. **Cost** (optional): type what the reel cost. Blank keeps the current
   price; `0` clears it.
9. Press **Save & write tag.**

**Next spool:** place it. The page moves to it.

**Spool already has a vendor tag?** Just place it. No onboarding needed.

**Spool already onboarded?** Click **Change this spool's details** to edit it.

## If it goes wrong

**Don't rewrite station tags with the Prusa app.** It retires the spool.

**"Read Error" after a failed registration:** open **Erase Tag**, press
**Start erase mode**, place the spool, press **Stop erase mode**, then place
it again.

# Station kit: firmware v0.1.0-195-gafbafab

Built 2026-11-25 in the cloud from commit `afbafab`: `8b7b437`, built on
**Arduino core 2.0.17** (espressif32 6.13.0) with every library pinned to its
Nov 8 version.

**Why a third build:** the first two kits (`-193`, `-194`) were built on core
3.3.12 and left the screen blank: the display task waited on the SPI bus
forever. In core 3.x, handing the shared SPI pins to the display *stops* the
NFC reader's SPI bus, and the reader's next transfer then waits forever while
holding the bus lock. The station's working firmware was built on core 2.x,
where handing over the pins only reroutes them. It carries the two fixes from Nov 18 that never
reached the station:

- **Restock page cut off** (`318984e`): pages are built in 8 KB blocks and
  streamed. A page that can't be built whole gets a short "station is busy"
  page that reloads itself, never a page cut off partway.
- **A tag alone on the scale** (`8b7b437`): under 50 g the tag is read but
  not weighed. Old such weighs drop out of Usage (about 26.2 kg after).

Toolchain: PlatformIO 6.2.0, espressif32 6.13.0 (arduino-esp32 2.0.17),
ESPAsyncWebServer 3.12.1 (`75dafb4`), AsyncTCP 3.5.0, TFT_eSPI 2.5.43. PSRAM is enabled (allocations over 4 KB go to PSRAM). This
binary has not been flashed onto hardware yet.

| File | |
|---|---|
| `firmware.bin` | 1,441,568 bytes. Flash at **0x10000** only. |
| `SHA256.txt` | `c6e18684a22fdb92f46dfb9fba6463bb0e87e5f25db0f280b7b7dc4404277db5` (esptool's MD5: `97a74b7c40ca605f308e043e68f7fadf`) |

## Flashing (studio PC or a Mac, Chrome or Edge; not an iPad)

1. **Download:** on this page, open `firmware.bin` and click the download button.
   Optional check on Windows: `certutil -hashfile firmware.bin SHA256` must
   print the SHA256 above.
2. **Back up:** Administration → Backup, download the log and the config.
   Note the spool count, the product count, this month's Usage total, and one
   spool's number and weight.
3. **Flash:**
   - Nobody weighing. Connect a USB-C data cable to the station's panel port.
   - Open https://espressif.github.io/esptool-js/ and click Connect. Pick
     "USB JTAG/serial debug unit"; it should report ESP32-S3.
   - One row only: Flash Address `0x10000`, file `firmware.bin`. Click Program.
   - When it's done, click Disconnect, unplug the station's power for 5 s and
     plug it back in.
   - **Never click Erase Flash, and never write at any other address.** The
     log, Stock List and settings live in the rest of the chip.
4. **Check:** the screen comes back green ("Seattle Makers / Place spool to
   begin"). The counts match, and the noted spool weighs the same within a few
   grams. Download another backup.

## Testing

1. **Restock:** put a spool on the scale and open Restock on the iPad. The page
   arrives complete, the list is styled and tappable, and search works.
2. **Usage:** reads about 26.2 kg.
3. **Tag alone:** a spare tag laid alone on the scale shows on screen, and
   Usage doesn't change.
4. **Send Claude** `http://weighstation.local/api/status`. `build_version` should
   read `v0.1.0-195-gafbafab`. `heap_max_block` and `psram_free` show whether
   PSRAM is working.
5. **Cleanup:** Restock #112, #113 and #114 (product #94 "Unknown"), then
   delete product #94.

Not part of the code: this folder should not be merged into `main`.

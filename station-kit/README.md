# Station kit: firmware v0.1.0-194-ge6e6810

Built 2026-11-25 in the cloud from commit `e6e6810`: `8b7b437` with every
library pinned to the versions the Nov 8 firmware used.

**Why a second build:** the first kit (`v0.1.0-193-g8b7b437`, flashed Nov 25)
left the station's screen blank. Serial showed the display task waiting on
the SPI bus forever, while the web app ran. The display and NFC code are
unchanged since Nov 8, so the suspects are the libraries that unpinned
builds pulled in after Nov 8: AsyncTCP 3.6 and the web server's Nov 9-15
changes. This build uses the Nov 8 ones. It carries the two fixes from Nov 18 that never
reached the station:

- **Restock page cut off** (`318984e`): pages are built in 8 KB blocks and
  streamed. A page that can't be built whole gets a short "station is busy"
  page that reloads itself, never a page cut off partway.
- **A tag alone on the scale** (`8b7b437`): under 50 g the tag is read but
  not weighed. Old such weighs drop out of Usage (about 26.2 kg after).

Toolchain: PlatformIO 6.2.0, espressif32 8.0.0 (arduino-esp32 3.3.12),
ESPAsyncWebServer 3.12.1 (`75dafb4`), AsyncTCP 3.5.0, TFT_eSPI 2.5.43. PSRAM is enabled (allocations over 4 KB go to PSRAM). This
binary has not been flashed onto hardware yet.

| File | |
|---|---|
| `firmware.bin` | 1,620,480 bytes. Flash at **0x10000** only. |
| `SHA256.txt` | `481df3a25f85e8472985548ec9e416c86420559c4b962fe94fd0e34099e37ee9` (esptool's MD5: `b151f988cccfff7bab641e671a81ca48`) |

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
   read `v0.1.0-194-ge6e6810`. `heap_max_block` and `psram_free` show whether
   PSRAM is working.
5. **Cleanup:** Restock #112, #113 and #114 (product #94 "Unknown"), then
   delete product #94.

Not part of the code: this folder should not be merged into `main`.

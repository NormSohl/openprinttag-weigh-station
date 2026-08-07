#pragma once

// Shared SPI bus arbitration for the PN5180 and the TFT.
//
// These two devices share GPIO 11 (MOSI) and 12 (SCK) but sit on DIFFERENT SPI
// peripherals, and neither can be moved:
//
//   TFT    — SPI3, forced by TFT_eSPI's S3 port. Its register macros resolve
//            REG_SPI_BASE() to DR_REG_SPI3_BASE for every legal SPI_PORT value,
//            so building without USE_HSPI_PORT makes it write SPI3's registers
//            while the Arduino layer drives SPI2 — an instant StoreProhibited.
//   PN5180 — SPI2, because ATrappmann's library hardcodes the global `SPI`
//            object, which the Arduino core constructs as SPIClass(FSPI) on the
//            ESP32-S3 (FSPI == bus 0 == the SPI2 peripheral).
//
// The ESP32-S3 routes each pin's OUTPUT from exactly one peripheral: pin
// routing lives in that pin's func_out_sel register, which holds a single
// signal index. spiAttachSCK()/spiAttachMOSI() end in pinMatrixOutAttach(),
// which overwrites it. So whichever peripheral initialises last takes GPIO
// 11/12 and the other device goes silent — no error, no warning. That is what
// killed the NFC reader: displayBegin() runs after SPI.begin(), so the display
// won and PN5180::reset() spun forever on an IRQ that could never arrive.
//
// The fix is to treat pin ownership as part of the lock. Every caller takes the
// bus through one of the helpers below, which re-points SCK/MOSI at that
// caller's peripheral before handing over. Three register writes, only on an
// actual owner change.
//
// MISO needs no handoff: input routing is per-signal (each peripheral has its
// own func_in_sel), so both can listen to GPIO 13 at once. The TFT is
// write-only here anyway — TFT_MISO is -1 because the ILI9488's SDO never
// tri-states.
//
// Anything else that wants the bus must go through these too; a raw
// xSemaphoreTake(gSpiMutex) will get the lock but not the pins.

void spiBusTakeNfc();   // take the bus, route SCK/MOSI to the PN5180 (SPI2)
void spiBusTakeTft();   // take the bus, route SCK/MOSI to the display (SPI3)
void spiBusGive();      // release the bus (routing is left as-is)

// TFT_eSPI configuration for SparkFun Thing Plus ESP32-S3
// + Hosyond / MSP3520-type 3.5" ILI9488 SPI TFT (480x320).
//
// SPI bus is SHARED with the PN5180 NFC module (same MOSI/SCK/MISO pins).
// gSpiMutex in main.cpp ensures only one task drives the bus at a time.
// DMA is intentionally left disabled to avoid async transfers on a shared bus.

#define USER_SETUP_LOADED

#define ILI9488_DRIVER

// Shared SPI pins (same as PN5180). SparkFun Thing Plus ESP32-S3 default SPI:
// PICO/MOSI = GPIO11, SCK = GPIO12, POCI/MISO = GPIO13 (per board silk + variant).
#define TFT_MOSI  11
#define TFT_SCLK  12
#define TFT_MISO  13

// TFT-only control pins
#define TFT_CS    15
#define TFT_DC    16
#define TFT_RST   17

// 27 MHz is safe for ILI9488 over short jumper wires.
// Raise to 40000000 only after confirming signal integrity.
#define SPI_FREQUENCY  27000000

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#include <Arduino.h>
#include <PN5180.h>
#include <PN5180ISO15693.h>
#include <string.h>
#include "config.h"
#include "spi_bus.h"
#include "device_state.h"
#include "opt_tag.h"

// ── Shared globals (defined in main.cpp) ─────────────────────────────────────
extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern uint8_t              gTagUid[8];
extern OptMeta              gTagMeta;
extern OptMain              gTagMain;
extern OptAuxiliary         gTagAux;
extern SemaphoreHandle_t    gTagMutex;
extern volatile bool        gWriteMainPending;
extern volatile bool        gWriteAuxPending;
extern SemaphoreHandle_t    gSpiMutex;
// Set by the TAGFORMAT serial command: force a reformat of whatever tag is
// present, regardless of how it currently classifies. The escape hatch for a
// tag left half-written by a failed format.
extern volatile bool        gTagForceFormat;

// Raw ISO15693 block dump for the spool currently on the scale.
// Sized for the largest expected ICODE SLIX2 tag (40 blocks × 4 B = 160 B; 512 is comfortable headroom).
static uint8_t sRawBuf[512];
static size_t  sRawLen        = 0;
static size_t  sPayloadOffset = SIZE_MAX;  // byte offset of NDEF payload in sRawBuf

// ── Helpers ───────────────────────────────────────────────────────────────────

static void setState(DeviceState s) {
    xSemaphoreTake(gStateMutex, portMAX_DELAY);
    gState = s;
    xSemaphoreGive(gStateMutex);
}

static DeviceState getState() {
    xSemaphoreTake(gStateMutex, portMAX_DELAY);
    DeviceState s = gState;
    xSemaphoreGive(gStateMutex);
    return s;
}

// Human-readable ISO15693 status. The distinction matters for diagnosis:
//   NO_CARD              -> the tag never answered. RF energy, not logic. A write
//                           needs far more field strength than a read, so a tag
//                           that inventories fine can still fail to program.
//   BLOCK_NOT_PROGRAMMED -> it answered but the cell didn't take. Also usually
//                           power, at the margin.
//   BLOCK_IS_LOCKED      -> the tag is write-protected. No amount of repositioning
//                           will help; the tag is not usable for OPT.
//   BLOCK_NOT_AVAILABLE  -> we addressed past the end of memory, i.e. our
//                           numBlocks is wrong.
//   OPTION_NOT_SUPPORTED -> the tag wants the Option flag set on writes, which
//                           this library does not do (it sends flags 0x22).
static const char* isoErrName(ISO15693ErrorCode rc) {
    switch (rc) {
        case EC_NO_CARD:                          return "NO_CARD (no answer — RF/coupling)";
        case ISO15693_EC_OK:                      return "OK";
        case ISO15693_EC_NOT_SUPPORTED:           return "NOT_SUPPORTED";
        case ISO15693_EC_NOT_RECOGNIZED:          return "NOT_RECOGNIZED";
        case ISO15693_EC_OPTION_NOT_SUPPORTED:    return "OPTION_NOT_SUPPORTED";
        case ISO15693_EC_UNKNOWN_ERROR:           return "UNKNOWN";
        case ISO15693_EC_BLOCK_NOT_AVAILABLE:     return "BLOCK_NOT_AVAILABLE (past end)";
        case ISO15693_EC_BLOCK_ALREADY_LOCKED:    return "BLOCK_ALREADY_LOCKED";
        case ISO15693_EC_BLOCK_IS_LOCKED:         return "BLOCK_IS_LOCKED (write-protected)";
        case ISO15693_EC_BLOCK_NOT_PROGRAMMED:    return "BLOCK_NOT_PROGRAMMED (write didn't take)";
        case ISO15693_EC_BLOCK_NOT_LOCKED:        return "BLOCK_NOT_LOCKED";
        case ISO15693_EC_CUSTOM_CMD_ERROR:        return "CUSTOM_CMD_ERROR";
    }
    return "?";
}

// Read every ISO15693 block into sRawBuf. Returns true on success.
// Verify signatures against installed ATrappmann/PN5180-Library version if compile errors occur.
static bool readAllBlocks(PN5180ISO15693& nfc, uint8_t* uid,
                          uint8_t numBlocks, uint8_t blockSize) {
    size_t total = (size_t)numBlocks * blockSize;
    if (total > sizeof(sRawBuf)) total = sizeof(sRawBuf);
    for (uint8_t b = 0; b < numBlocks && (size_t)b * blockSize < sizeof(sRawBuf); b++) {
        ISO15693ErrorCode rc = nfc.readSingleBlock(uid, b, sRawBuf + b * blockSize, blockSize);
        if (rc != ISO15693_EC_OK) {
            Serial.printf("[nfc] read FAILED at block %u of %u: rc=%d %s\n",
                          (unsigned)b, (unsigned)numBlocks, (int)rc, isoErrName(rc));
            return false;
        }
    }
    sRawLen = total;
    return true;
}

// Write one block, retrying a few times before giving up.
//
// EEPROM programming sits much closer to the RF power budget than a read does,
// so an occasional failure on an otherwise fine tag is normal and a retry
// usually takes. Retrying is standard practice for ISO15693 writes; failing a
// whole onboarding because one block out of eighty flickered is not.
//
// Takes and releases the bus itself, per attempt: each write costs ~10 ms inside
// the library, and holding gSpiMutex across a whole 80-block format would freeze
// displayTask for a second or more.
#define TAG_WRITE_RETRIES 3
static ISO15693ErrorCode writeBlockRetry(PN5180ISO15693& nfc, uint8_t* uid,
                                         uint8_t blockNo, uint8_t* data,
                                         uint8_t blockSize) {
    ISO15693ErrorCode rc = ISO15693_EC_UNKNOWN_ERROR;
    for (int attempt = 0; attempt < TAG_WRITE_RETRIES; attempt++) {
        spiBusTakeNfc();
        rc = nfc.writeSingleBlock(uid, blockNo, data, blockSize);
        spiBusGive();
        if (rc == ISO15693_EC_OK) {
            if (attempt) Serial.printf("[nfc] block %u wrote on attempt %d\n",
                                       (unsigned)blockNo, attempt + 1);
            return rc;
        }
        vTaskDelay(pdMS_TO_TICKS(20));   // let the field settle before retrying
    }
    return rc;
}

// Write a CBOR section back into the tag at the position given by the Meta offsets.
// sectionOffset is relative to the NDEF payload start (from OptMeta).
static bool writeSection(PN5180ISO15693& nfc, uint8_t* uid,
                         uint8_t blockSize, size_t sectionOffset,
                         const uint8_t* cborData, size_t cborLen) {
    if (sPayloadOffset == SIZE_MAX) return false;
    size_t absStart = sPayloadOffset + sectionOffset;
    if (absStart + cborLen > sRawLen) return false;

    memcpy(sRawBuf + absStart, cborData, cborLen);

    size_t firstBlock = absStart / blockSize;
    size_t lastBlock  = (absStart + cborLen - 1) / blockSize;
    for (size_t b = firstBlock; b <= lastBlock; b++) {
        ISO15693ErrorCode rc = writeBlockRetry(nfc, uid, (uint8_t)b,
                                               sRawBuf + b * blockSize, blockSize);
        if (rc != ISO15693_EC_OK) {
            Serial.printf("[nfc] section write FAILED at block %u/%u: rc=%d %s\n",
                          (unsigned)b, (unsigned)lastBlock, (int)rc, isoErrName(rc));
            return false;
        }
    }
    return true;
}

// ── Task ──────────────────────────────────────────────────────────────────────

// Read the PN5180's version registers over SPI and report them. This separates
// the two very different failure modes behind "no tag detected":
//   - versions read back plausibly -> SPI/CS/BUSY/RST wiring and 3.3 V logic are
//     fine, so a dead reader means RF: the TVDD/5 V transmitter rail, the
//     antenna, or the tag itself.
//   - all 0x00 or all 0xFF       -> the chip isn't talking at all; look at the
//     SPI bus, chip select, reset, or power before suspecting anything else.
// Returns true if the chip answered.
static bool nfcSelfTest(PN5180ISO15693& nfc) {
    uint8_t prod[2] = {0xFF, 0xFF}, fw[2] = {0xFF, 0xFF}, eep[2] = {0xFF, 0xFF};

    spiBusTakeNfc();
    nfc.readEEprom(PRODUCT_VERSION,  prod, 2);
    nfc.readEEprom(FIRMWARE_VERSION, fw,   2);
    nfc.readEEprom(EEPROM_VERSION,   eep,  2);
    spiBusGive();

    Serial.printf("[nfc] PN5180 product %d.%d  firmware %d.%d  eeprom %d.%d\n",
                  prod[1], prod[0], fw[1], fw[0], eep[1], eep[0]);

    const bool dead = (prod[0] == 0xFF && prod[1] == 0xFF) ||
                      (prod[0] == 0x00 && prod[1] == 0x00);
    if (dead) {
        Serial.println("[nfc] NOT RESPONDING — chip is not answering over SPI.");
        Serial.printf("[nfc]   check: NSS=%d BUSY=%d RST=%d, shared SPI "
                      "SCK=%d MOSI=%d MISO=%d, and 3.3 V power\n",
                      PN5180_NSS, PN5180_BUSY, PN5180_RESET,
                      SPI_SCK, SPI_MOSI, SPI_MISO);
    } else {
        Serial.println("[nfc] responding — SPI wiring OK. If tags still aren't "
                       "seen, suspect the RF rail/antenna, not the bus.");
    }
    return !dead;
}

void nfcTask(void* param) {
    PN5180ISO15693 nfc(PN5180_NSS, PN5180_BUSY, PN5180_RESET);

    // Every step here is announced BEFORE it runs, because the PN5180 library
    // waits on the chip with unbounded loops — reset() spins on
    // `while (0 == (IDLE_IRQ_STAT & getIRQStatus()))` and the SPI helpers spin
    // on BUSY. If the chip is absent, unpowered or mis-wired, the task simply
    // never returns and prints nothing at all. Whatever line appears last is
    // the call that hung.
    Serial.printf("[nfc] init: NSS=%d BUSY=%d RST=%d (shared SPI %d/%d/%d)\n",
                  PN5180_NSS, PN5180_BUSY, PN5180_RESET,
                  SPI_SCK, SPI_MOSI, SPI_MISO);

    // Raw pin read before the library touches anything. The PN5180 idles BUSY
    // LOW; a pin stuck HIGH (or floating) with no chip on the other end is the
    // cheapest possible evidence that it isn't powered or wired.
    pinMode(PN5180_BUSY, INPUT);
    Serial.printf("[nfc] BUSY reads %s before init\n",
                  digitalRead(PN5180_BUSY) ? "HIGH (suspect: unpowered/miswired)"
                                           : "LOW (normal idle)");

    // These three MUST hold the bus. displayBegin() has already run and left
    // GPIO 11/12 routed to the display's peripheral (SPI3); spiBusTakeNfc()
    // points them back at the PN5180's (SPI2). Without it the reader never
    // sees a clock edge and reset() spins forever. See spi_bus.h.
    //
    // Tradeoff: reset() waits on the chip with an unbounded loop, so a reader
    // that never answers now holds the bus and stalls displayTask too. Before
    // the handoff the display stayed alive because reset() ran unlocked — but
    // it also ran without the pins, which is precisely why it hung. A dead
    // reader has to be visible somewhere; the boot log names the call.
    Serial.println("[nfc] begin()…");
    spiBusTakeNfc();
    nfc.begin();
    spiBusGive();
    Serial.println("[nfc] begin() ok");

    Serial.println("[nfc] reset()… (spins forever if the chip never answers)");
    spiBusTakeNfc();
    nfc.reset();
    spiBusGive();
    Serial.println("[nfc] reset() ok");

    nfcSelfTest(nfc);   // logs whether the chip is alive before we poll for tags

    Serial.println("[nfc] setupRF()…");
    spiBusTakeNfc();
    nfc.setupRF();
    spiBusGive();
    Serial.println("[nfc] setupRF() ok — polling for tags");

    uint8_t uid[8]       = {};
    uint8_t numBlocks    = 0;
    uint8_t blockSize    = 0;
    uint8_t debounceHits = 0;
    uint8_t missCount    = 0;
    TickType_t confirmStart = 0;

    for (;;) {
        DeviceState state = getState();

        // ── Poll for tag presence ─────────────────────────────────────────────
        uint8_t  detectedUid[8] = {};
        uint8_t  detectedNumBlocks, detectedBlockSize;
        spiBusTakeNfc();
        bool tagPresent = (nfc.getInventory(detectedUid) == ISO15693_EC_OK);
        if (tagPresent) {
            tagPresent = (nfc.getSystemInfo(detectedUid, &detectedNumBlocks, &detectedBlockSize)
                          == ISO15693_EC_OK);
        }
        spiBusGive();

        // ── States where we're waiting for a tag ──────────────────────────────
        if (state == DeviceState::Idle || state == DeviceState::IdleNoWiFi) {
            if (tagPresent) {
                memcpy(uid, detectedUid, 8);
                numBlocks    = detectedNumBlocks;
                blockSize    = detectedBlockSize;
                debounceHits = 1;
                setState(DeviceState::TagDetecting);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ── Debounce ──────────────────────────────────────────────────────────
        if (state == DeviceState::TagDetecting) {
            if (!tagPresent || memcmp(detectedUid, uid, 8) != 0) {
                setState(DeviceState::Idle);  // false trigger or UID changed
                debounceHits = 0;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (++debounceHits < NFC_DEBOUNCE_READS) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            // Tag confirmed — read all blocks and classify
            spiBusTakeNfc();
            bool readOk = readAllBlocks(nfc, uid, numBlocks, blockSize);
            spiBusGive();
            if (!readOk) {
                Serial.printf("[nfc] could not read the tag (%u blocks x %u B). "
                              "RF/coupling, or the tag moved mid-read.\n",
                              (unsigned)numBlocks, (unsigned)blockSize);
                setState(DeviceState::TagReadError);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // Forced reformat wins over classification: the whole point is to
            // recover a tag whose current contents we cannot make sense of.
            if (gTagForceFormat) {
                gTagForceFormat = false;
                Serial.println("[nfc] TAGFORMAT: forcing a reformat of this tag");
                setState(DeviceState::FormattingAndRegistering);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            if (optIsBlank(sRawBuf, sRawLen)) {
                setState(DeviceState::BlankTagFound);
            } else {
                OptMeta  meta = {};
                OptMain  main = {};
                OptAuxiliary aux = {};
                if (!optDecode(sRawBuf, sRawLen, &meta, &main, &aux)) {
                    // Read cleanly but makes no sense. The likeliest cause by far
                    // is a tag left half-written by a format that failed partway:
                    // block 0 carries the 0xE1 NDEF magic and a Meta section, so
                    // optIsBlank() no longer calls it blank, but Main/Aux were
                    // never written — and there is no automatic way out of that.
                    //
                    // Deliberately NOT auto-reformatted. A tag we merely fail to
                    // parse might be a legitimate foreign format, and wiping it
                    // would be irreversible. Recovery is the explicit TAGFORMAT
                    // serial command.
                    Serial.printf("[nfc] tag read OK (%u B) but does not decode as OPT.\n",
                                  (unsigned)sRawLen);
                    Serial.print("[nfc]   first bytes:");
                    for (size_t i = 0; i < 24 && i < sRawLen; i++)
                        Serial.printf(" %02x", sRawBuf[i]);
                    Serial.println();
                    Serial.printf("[nfc]   %s\n",
                                  (sRawLen && sRawBuf[0] == 0xE1)
                                      ? "starts 0xE1 — NDEF-formatted, so probably a "
                                        "PARTIAL format. Send TAGFORMAT to rewrite it."
                                      : "no NDEF magic — foreign or corrupt content.");
                    setState(DeviceState::TagReadError);
                } else {
                    sPayloadOffset = optPayloadOffset(sRawBuf, sRawLen);
                    xSemaphoreTake(gTagMutex, portMAX_DELAY);
                    memcpy(gTagUid, uid, 8);
                    gTagMeta = meta;
                    gTagMain = main;
                    gTagAux  = aux;
                    xSemaphoreGive(gTagMutex);
                    // syncTask resolves instance_uuid against the local store and
                    // transitions to WeighingAndSync (known) or ForeignTagFound (new)
                    setState(DeviceState::ValidTagFound);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ── Blank tag countdown ───────────────────────────────────────────────
        if (state == DeviceState::BlankTagFound) {
            confirmStart = xTaskGetTickCount();
            setState(DeviceState::AwaitingFormatConfirm);
            continue;
        }

        if (state == DeviceState::AwaitingFormatConfirm) {
            if (!tagPresent) {
                setState(DeviceState::Idle);  // removed during countdown — cancelled
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            if (xTaskGetTickCount() - confirmStart >= pdMS_TO_TICKS(BLANK_TAG_CONFIRM_SEC * 1000UL)) {
                setState(DeviceState::FormattingAndRegistering);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (state == DeviceState::FormattingAndRegistering) {
            uint8_t  initBuf[512] = {};
            OptMeta  initMeta     = {};

            // Both failures below land in TagReadError, which on screen reads
            // only "Read Error" — so say on the wire which one it was and why.
            // Without this the two are indistinguishable, and they have nothing
            // in common: one is our layout maths, the other is RF.
            Serial.printf("[nfc] formatting blank tag: %u blocks x %u B = %u B\n",
                          (unsigned)numBlocks, (unsigned)blockSize,
                          (unsigned)numBlocks * blockSize);

            size_t payloadOff = optBuildBlankTag(numBlocks, blockSize,
                                                 initBuf, sizeof(initBuf), &initMeta);
            if (payloadOff == SIZE_MAX) {
                Serial.printf("[nfc] optBuildBlankTag REFUSED this geometry "
                              "(%u blocks x %u B, buffer %u B). Nothing was written "
                              "to the tag.\n",
                              (unsigned)numBlocks, (unsigned)blockSize,
                              (unsigned)sizeof(initBuf));
                setState(DeviceState::TagReadError);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            const uint32_t t0 = millis();
            bool writeOk = true;
            for (uint8_t b = 0; b < numBlocks; b++) {
                ISO15693ErrorCode rc = writeBlockRetry(nfc, uid, b,
                                                       initBuf + b * blockSize, blockSize);
                if (rc != ISO15693_EC_OK) {
                    Serial.printf("[nfc] format FAILED at block %u of %u after %d "
                                  "attempts: rc=%d %s\n",
                                  (unsigned)b, (unsigned)numBlocks, TAG_WRITE_RETRIES,
                                  (int)rc, isoErrName(rc));
                    // Block 0 failing means it never programmed anything; a later
                    // block means the tag is now half-written and must be
                    // re-formatted before use. Worth knowing which.
                    if (b == 0)
                        Serial.println("[nfc]   nothing was written — tag is untouched.");
                    else
                        Serial.printf("[nfc]   blocks 0..%u were written — the tag is "
                                      "PARTIALLY formatted and needs another pass.\n",
                                      (unsigned)(b - 1));
                    writeOk = false;
                    break;
                }
            }
            if (writeOk)
                Serial.printf("[nfc] format ok: %u blocks in %u ms\n",
                              (unsigned)numBlocks, (unsigned)(millis() - t0));
            if (!writeOk) {
                setState(DeviceState::TagReadError);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // Mirror the written bytes into the task-local raw buffer so subsequent
            // aux/main writes via writeSection() use the correct offsets.
            memcpy(sRawBuf, initBuf, (size_t)numBlocks * blockSize);
            sRawLen        = (size_t)numBlocks * blockSize;
            sPayloadOffset = payloadOff;

            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            memcpy(gTagUid, uid, 8);
            gTagMeta = initMeta;
            gTagMain = {};   // nil UUID — syncTask creates a stub Spool with needs_onboarding=true
            gTagAux  = {};
            xSemaphoreGive(gTagMutex);

            // syncTask sees the nil UUID, mints a local stub record, and
            // transitions on once the record exists.
            setState(DeviceState::ValidTagFound);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ── Tag read error — wait for removal ─────────────────────────────────
        if (state == DeviceState::TagReadError) {
            if (!tagPresent) setState(DeviceState::Idle);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // ── ForeignTagFound / RegisteringForeignTag / ValidTagFound ───────────
        // syncTask owns the store/record side of these transitions.
        // If the tag is removed before syncTask finishes, abandon and go idle.
        if (state == DeviceState::ForeignTagFound
            || state == DeviceState::RegisteringForeignTag
            || state == DeviceState::ValidTagFound) {
            if (!tagPresent) setState(DeviceState::Idle);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // ── Present / WeighingAndSync / Reconciling ───────────────────────────
        if (!tagPresent) {
            if (++missCount >= 2) {
                missCount = 0;
                sRawLen = 0;
                sPayloadOffset = SIZE_MAX;
                setState(DeviceState::Idle);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        missCount = 0;

        // Write Aux when syncTask has updated gTagAux (after weighing)
        if (gWriteAuxPending) {
            gWriteAuxPending = false;
            uint8_t cborBuf[64];
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptAuxiliary aux = gTagAux;
            uint16_t auxOffset = gTagMeta.aux_region_offset;
            xSemaphoreGive(gTagMutex);
            if (auxOffset > 0) {
                size_t n = optEncodeAux(aux, cborBuf, sizeof(cborBuf));
                if (n > 0) {
                    spiBusTakeNfc();
                    writeSection(nfc, uid, blockSize, auxOffset, cborBuf, n);
                    spiBusGive();
                }
            }
        }

        if (gWriteMainPending) {
            gWriteMainPending = false;
            uint8_t cborBuf[256];
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain  main      = gTagMain;
            uint16_t mainOffset = gTagMeta.main_region_offset;
            xSemaphoreGive(gTagMutex);
            size_t n = optEncodeMain(main, cborBuf, sizeof(cborBuf));
            if (n > 0) {
                spiBusTakeNfc();
                writeSection(nfc, uid, blockSize, mainOffset, cborBuf, n);
                spiBusGive();
                // Leave ReconcilingMainSection — syncTask's next poll will confirm
                if (getState() == DeviceState::ReconcilingMainSection)
                    setState(DeviceState::Present);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

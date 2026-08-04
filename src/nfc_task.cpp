#include <Arduino.h>
#include <PN5180.h>
#include <PN5180ISO15693.h>
#include <string.h>
#include "config.h"
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

// Read every ISO15693 block into sRawBuf. Returns true on success.
// Verify signatures against installed ATrappmann/PN5180-Library version if compile errors occur.
static bool readAllBlocks(PN5180ISO15693& nfc, uint8_t* uid,
                          uint8_t numBlocks, uint8_t blockSize) {
    size_t total = (size_t)numBlocks * blockSize;
    if (total > sizeof(sRawBuf)) total = sizeof(sRawBuf);
    for (uint8_t b = 0; b < numBlocks && (size_t)b * blockSize < sizeof(sRawBuf); b++) {
        if (nfc.readSingleBlock(uid, b, sRawBuf + b * blockSize, blockSize) != ISO15693_EC_OK)
            return false;
    }
    sRawLen = total;
    return true;
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
        if (nfc.writeSingleBlock(uid, (uint8_t)b,
                                 sRawBuf + b * blockSize, blockSize) != ISO15693_EC_OK)
            return false;
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

    xSemaphoreTake(gSpiMutex, portMAX_DELAY);
    nfc.readEEprom(PRODUCT_VERSION,  prod, 2);
    nfc.readEEprom(FIRMWARE_VERSION, fw,   2);
    nfc.readEEprom(EEPROM_VERSION,   eep,  2);
    xSemaphoreGive(gSpiMutex);

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
    nfc.begin();
    nfc.reset();

    nfcSelfTest(nfc);   // logs whether the chip is alive before we poll for tags

    nfc.setupRF();

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
        xSemaphoreTake(gSpiMutex, portMAX_DELAY);
        bool tagPresent = (nfc.getInventory(detectedUid) == ISO15693_EC_OK);
        if (tagPresent) {
            tagPresent = (nfc.getSystemInfo(detectedUid, &detectedNumBlocks, &detectedBlockSize)
                          == ISO15693_EC_OK);
        }
        xSemaphoreGive(gSpiMutex);

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
            xSemaphoreTake(gSpiMutex, portMAX_DELAY);
            bool readOk = readAllBlocks(nfc, uid, numBlocks, blockSize);
            xSemaphoreGive(gSpiMutex);
            if (!readOk) {
                setState(DeviceState::TagReadError);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            if (optIsBlank(sRawBuf, sRawLen)) {
                setState(DeviceState::BlankTagFound);
            } else {
                OptMeta  meta = {};
                OptMain  main = {};
                OptAuxiliary aux = {};
                if (!optDecode(sRawBuf, sRawLen, &meta, &main, &aux)) {
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
            size_t   payloadOff   = optBuildBlankTag(numBlocks, blockSize,
                                                     initBuf, sizeof(initBuf), &initMeta);
            if (payloadOff == SIZE_MAX) {
                setState(DeviceState::TagReadError);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            xSemaphoreTake(gSpiMutex, portMAX_DELAY);
            bool writeOk = true;
            for (uint8_t b = 0; b < numBlocks; b++) {
                if (nfc.writeSingleBlock(uid, b, initBuf + b * blockSize, blockSize)
                        != ISO15693_EC_OK) {
                    writeOk = false;
                    break;
                }
            }
            xSemaphoreGive(gSpiMutex);
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
                    xSemaphoreTake(gSpiMutex, portMAX_DELAY);
                    writeSection(nfc, uid, blockSize, auxOffset, cborBuf, n);
                    xSemaphoreGive(gSpiMutex);
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
                xSemaphoreTake(gSpiMutex, portMAX_DELAY);
                writeSection(nfc, uid, blockSize, mainOffset, cborBuf, n);
                xSemaphoreGive(gSpiMutex);
                // Leave ReconcilingMainSection — syncTask's next poll will confirm
                if (getState() == DeviceState::ReconcilingMainSection)
                    setState(DeviceState::Present);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

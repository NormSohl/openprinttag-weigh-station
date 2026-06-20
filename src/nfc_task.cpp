#include <Arduino.h>
#include <PN5180.h>
#include <PN5180ISO15693.h>
#include "config.h"
#include "device_state.h"

extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;

void nfcTask(void* param) {
    PN5180ISO15693 nfc(PN5180_NSS, PN5180_BUSY, PN5180_RESET);
    nfc.begin();
    nfc.reset();
    nfc.setupRF();

    for (;;) {
        // TODO: poll for tag presence (getSystemInfo / readSingleBlock)
        //       Debounce NFC_DEBOUNCE_READS consecutive reads before acting.
        //       Classify result:
        //         blank/unformatted  → BlankTagFound → AwaitingFormatConfirm
        //         valid OPT data     → ValidTagFound  → WeighingAndSync or ForeignTagFound
        //         corrupt/inconsistent → TagReadError
        //       Never call lockICODESLIX2 — tags must remain rewritable.
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

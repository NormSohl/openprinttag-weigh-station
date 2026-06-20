#include <Arduino.h>
#include <SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>
#include "config.h"
#include "device_state.h"

extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;

void scaleTask(void* param) {
    NAU7802 nau;
    if (!nau.begin()) {
        Serial.println("[scale] NAU7802 not found — task halted");
        vTaskDelete(nullptr);
    }
    nau.calibrateAFE();

    for (;;) {
        // TODO: weigh once when state == WeighingAndSync.
        //       Average SCALE_SAMPLES readings, convert raw counts to grams
        //       using a calibration factor, write result to gWeightGrams under
        //       gWeightMutex.  Do not re-weigh while the spool remains Present
        //       (Spoolman-only poll loop handles that phase).
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

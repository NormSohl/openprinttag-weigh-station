// Host shim: NVS over a JSON-ish flat file, so counters survive a simulated
// reboot the way the real NVS partition does.
#pragma once
#include "Arduino.h"
class Preferences {
public:
    bool     begin(const char* ns, bool readOnly = false);
    void     end();
    bool     isKey(const char* key);
    uint32_t getUInt(const char* key, uint32_t def = 0);
    void     putUInt(const char* key, uint32_t v);
private:
    std::string ns_;
};

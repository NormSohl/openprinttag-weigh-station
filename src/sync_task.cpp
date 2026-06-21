#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <string.h>
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"

// ── Shared globals ────────────────────────────────────────────────────────────
extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern OptMain              gTagMain;
extern OptAuxiliary         gTagAux;
extern SemaphoreHandle_t    gTagMutex;
extern volatile bool        gWriteMainPending;
extern volatile bool        gWriteAuxPending;
extern volatile int         gSpoolId;
extern volatile bool        gSpoolNeedsOnboarding;

// ── State helpers ─────────────────────────────────────────────────────────────

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

// ── UUID helpers ──────────────────────────────────────────────────────────────

static bool isNilUUID(const uint8_t* uuid) {
    for (int i = 0; i < 16; i++) if (uuid[i]) return false;
    return true;
}

static void generateUUIDv4(uint8_t* uuid) {
    esp_fill_random(uuid, 16);
    uuid[6] = (uuid[6] & 0x0F) | 0x40;  // version 4
    uuid[8] = (uuid[8] & 0x3F) | 0x80;  // variant 10xx
}

static void uuidToStr(const uint8_t* uuid, char* out37) {
    snprintf(out37, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        uuid[0],  uuid[1],  uuid[2],  uuid[3],
        uuid[4],  uuid[5],
        uuid[6],  uuid[7],
        uuid[8],  uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

// ── Colour helpers ────────────────────────────────────────────────────────────

static uint8_t hexByte(const char* p) {
    char b[3] = {p[0], p[1], 0};
    return (uint8_t)strtol(b, nullptr, 16);
}

// Parses "#RRGGBB" or "RRGGBB" (Spoolman stores without #). Alpha is fixed 0xFF.
static void parseColorHex(const char* hex, uint8_t* rgba) {
    if (!hex) { memset(rgba, 0, 4); return; }
    const char* h = (*hex == '#') ? hex + 1 : hex;
    if (strlen(h) < 6) { memset(rgba, 0, 4); return; }
    rgba[0] = hexByte(h);
    rgba[1] = hexByte(h + 2);
    rgba[2] = hexByte(h + 4);
    rgba[3] = 0xFF;
}

// ── URL encoding ──────────────────────────────────────────────────────────────

static String urlEncode(const char* s) {
    String out;
    for (const char* p = s; *p; p++) {
        if (isalnum((unsigned char)*p)
                || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out += *p;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)*p);
            out += buf;
        }
    }
    return out;
}

// ── HTTP wrappers ─────────────────────────────────────────────────────────────

// Loaded from NVS at boot; updated via the WiFiManager captive portal.
static String sBase(SPOOLMAN_BASE_URL);

// Returns HTTP status code, or -1 on connection failure.
static int httpGet(const String& url, String& body) {
    HTTPClient h;
    if (!h.begin(url)) return -1;
    int code = h.GET();
    if (code > 0) body = h.getString();
    h.end();
    return code;
}

static int httpPost(const String& url, const String& payload, String& body) {
    HTTPClient h;
    if (!h.begin(url)) return -1;
    h.addHeader("Content-Type", "application/json");
    int code = h.sendRequest("POST", payload);
    if (code > 0) body = h.getString();
    h.end();
    return code;
}

static int httpPatch(const String& url, const String& payload, String& body) {
    HTTPClient h;
    if (!h.begin(url)) return -1;
    h.addHeader("Content-Type", "application/json");
    int code = h.sendRequest("PATCH", payload);
    if (code > 0) body = h.getString();
    h.end();
    return code;
}

// ── Spoolman data helpers ─────────────────────────────────────────────────────

// Fill OptMain from a Spoolman spool JSON object (nested filament + vendor).
// Does NOT touch instance_uuid, material_class, or material_type.
static void populateMainFromSpool(JsonVariantConst spool, OptMain* m) {
    JsonVariantConst fil  = spool["filament"];
    JsonVariantConst vend = fil["vendor"];

    strlcpy(m->brand_name,            vend["name"] | "",    sizeof(m->brand_name));
    strlcpy(m->material_name,         fil["name"] | "",     sizeof(m->material_name));
    strlcpy(m->material_abbreviation, fil["material"] | "", sizeof(m->material_abbreviation));

    parseColorHex(fil["color_hex"] | "", m->primary_color_rgba);

    m->nominal_netto_full_weight = fil["weight"]         | 0.0f;
    m->filament_diameter         = fil["diameter"]       | 1.75f;
    m->empty_container_weight    = spool["spool_weight"]   | 0.0f;
    m->actual_netto_full_weight  = spool["initial_weight"] | m->nominal_netto_full_weight;

    m->min_print_temperature = (int16_t)(fil["settings_extruder_temp_min"] | 0);
    m->max_print_temperature = (int16_t)(fil["settings_extruder_temp_max"] | 0);
    m->min_bed_temperature   = (int16_t)(fil["settings_bed_temp_min"]      | 0);
    m->max_bed_temperature   = (int16_t)(fil["settings_bed_temp_max"]      | 0);
}

// True if any Spoolman-sourced field differs between a and b.
// instance_uuid / material_class / material_type are excluded (not in Spoolman).
static bool mainDiffers(const OptMain& a, const OptMain& b) {
    return strcmp(a.brand_name,            b.brand_name)            != 0
        || strcmp(a.material_name,         b.material_name)         != 0
        || strcmp(a.material_abbreviation, b.material_abbreviation) != 0
        || memcmp(a.primary_color_rgba,    b.primary_color_rgba, 4) != 0
        || a.nominal_netto_full_weight != b.nominal_netto_full_weight
        || a.actual_netto_full_weight  != b.actual_netto_full_weight
        || a.empty_container_weight    != b.empty_container_weight
        || a.filament_diameter         != b.filament_diameter
        || a.min_print_temperature     != b.min_print_temperature
        || a.max_print_temperature     != b.max_print_temperature
        || a.min_bed_temperature       != b.min_bed_temperature
        || a.max_bed_temperature       != b.max_bed_temperature;
}

// ── Spoolman API calls ────────────────────────────────────────────────────────

// Find vendor by name or create if absent. Returns id, or -1 on error.
static int findOrCreateVendor(const char* name) {
    String body;
    int code = httpGet(sBase + "/api/v1/vendor?name=" + urlEncode(name), body);
    if (code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
            JsonArray items = doc["items"].as<JsonArray>();
            if (items && items.size() > 0) return items[0]["id"] | -1;
        }
    } else if (code < 0) {
        return -1;
    }

    JsonDocument req;
    req["name"] = name;
    String reqStr;
    serializeJson(req, reqStr);
    code = httpPost(sBase + "/api/v1/vendor", reqStr, body);
    if (code >= 200 && code < 300) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) return doc["id"] | -1;
    }
    return -1;
}

// Find filament by vendor + name, or create from OptMain fields. Returns id, or -1.
static int findOrCreateFilament(int vendorId, const OptMain& m) {
    const char* name = m.material_name[0] ? m.material_name : "Unknown";
    String body;
    int code = httpGet(sBase + "/api/v1/filament?vendor_id=" + String(vendorId)
                       + "&name=" + urlEncode(name), body);
    if (code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) {
            JsonArray items = doc["items"].as<JsonArray>();
            if (items && items.size() > 0) return items[0]["id"] | -1;
        }
    } else if (code < 0) {
        return -1;
    }

    JsonDocument req;
    req["vendor_id"] = vendorId;
    req["name"]      = name;
    if (m.material_abbreviation[0])      req["material"] = m.material_abbreviation;
    if (m.filament_diameter > 0)         req["diameter"] = m.filament_diameter;
    if (m.nominal_netto_full_weight > 0) req["weight"]   = (int)m.nominal_netto_full_weight;
    if (m.primary_color_rgba[3] > 0) {
        char hex[8];
        snprintf(hex, sizeof(hex), "%02X%02X%02X",
            m.primary_color_rgba[0], m.primary_color_rgba[1], m.primary_color_rgba[2]);
        req["color_hex"] = hex;
    }
    if (m.min_print_temperature) req["settings_extruder_temp_min"] = m.min_print_temperature;
    if (m.max_print_temperature) req["settings_extruder_temp_max"] = m.max_print_temperature;
    if (m.min_bed_temperature)   req["settings_bed_temp_min"]      = m.min_bed_temperature;
    if (m.max_bed_temperature)   req["settings_bed_temp_max"]      = m.max_bed_temperature;

    String reqStr;
    serializeJson(req, reqStr);
    code = httpPost(sBase + "/api/v1/filament", reqStr, body);
    if (code >= 200 && code < 300) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) return doc["id"] | -1;
    }
    return -1;
}

// Create spool record. Returns Spoolman spool id, or -1 on error.
static int createSpool(int filamentId, const uint8_t* uuid,
                       float initialWeight, float spoolWeight, bool needsOnboarding) {
    char uuidStr[37];
    uuidToStr(uuid, uuidStr);

    JsonDocument req;
    req["filament_id"] = filamentId;
    if (initialWeight > 0) req["initial_weight"] = initialWeight;
    if (spoolWeight   > 0) req["spool_weight"]   = spoolWeight;
    req["extra"]["nfc_id"] = uuidStr;
    if (needsOnboarding)   req["extra"]["needs_onboarding"] = true;

    String reqStr, body;
    serializeJson(req, reqStr);
    int code = httpPost(sBase + "/api/v1/spool", reqStr, body);
    if (code >= 200 && code < 300) {
        JsonDocument doc;
        if (!deserializeJson(doc, body)) return doc["id"] | -1;
    }
    return -1;
}

// PATCH remaining_weight. Returns true on HTTP 2xx.
static bool patchSpoolWeight(int spoolId, float remainingGrams) {
    JsonDocument req;
    req["remaining_weight"] = remainingGrams;
    String reqStr, body;
    serializeJson(req, reqStr);
    int code = httpPatch(sBase + "/api/v1/spool/" + String(spoolId), reqStr, body);
    return (code >= 200 && code < 300);
}

// GET /api/v1/spool/{id}. Returns true on success; doc is populated.
static bool getSpoolById(int id, JsonDocument& doc) {
    String body;
    if (httpGet(sBase + "/api/v1/spool/" + String(id), body) != 200) return false;
    return !deserializeJson(doc, body);
}

// GET spool by extra.nfc_id. Returns spool id, -1 if not found, -2 on network error.
// On success, the spool object is deep-copied into spoolOut.
static int findSpoolByNfcId(const char* uuidStr, JsonDocument& spoolOut) {
    String body;
    int code = httpGet(sBase + "/api/v1/spool?extra[nfc_id]=" + urlEncode(uuidStr), body);
    if (code < 0)    return -2;
    if (code != 200) return -1;
    JsonDocument list;
    if (deserializeJson(list, body)) return -1;
    JsonArray items = list["items"].as<JsonArray>();
    if (!items || items.size() == 0) return -1;
    spoolOut.set(items[0]);
    return spoolOut["id"] | -1;
}

// ── Task ──────────────────────────────────────────────────────────────────────

void syncTask(void* param) {
    // Load persisted Spoolman URL from NVS; fall back to compile-time default.
    {
        Preferences prefs;
        prefs.begin("weigh", true);
        sBase = prefs.getString("spoolman_url", SPOOLMAN_BASE_URL);
        prefs.end();
    }

    // Set WiFiSetupMode before blocking in autoConnect so displayTask can show
    // the captive-portal SSID while we wait for provisioning.
    setState(DeviceState::WiFiSetupMode);
    WiFiManager wm;
    wm.setConfigPortalTimeout(120);

    // Hold BOOT (GPIO 0) for WIFI_RESET_HOLD_MS to erase stored WiFi credentials
    // and force the captive portal to reopen — useful when moving the device to a
    // new network or correcting a wrong Spoolman URL.
    pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
    if (digitalRead(WIFI_RESET_PIN) == LOW) {
        uint32_t held = 0;
        while (digitalRead(WIFI_RESET_PIN) == LOW && held < WIFI_RESET_HOLD_MS) {
            vTaskDelay(pdMS_TO_TICKS(100));
            held += 100;
        }
        if (held >= WIFI_RESET_HOLD_MS) {
            Serial.println("WiFi reset: credentials cleared. Release BOOT to continue.");
            wm.resetSettings();
        }
    }

    // Add Spoolman URL as a captive-portal field so it can be set (or corrected)
    // at install time without reflashing.
    char urlBuf[128];
    strlcpy(urlBuf, sBase.c_str(), sizeof(urlBuf));
    WiFiManagerParameter urlParam("spoolman_url", "Spoolman URL", urlBuf, sizeof(urlBuf));
    wm.addParameter(&urlParam);

    bool saveNeeded = false;
    wm.setSaveConfigCallback([&saveNeeded]() { saveNeeded = true; });

    if (!wm.autoConnect("WeighStation-Setup")) {
        setState(DeviceState::IdleNoWiFi);
    } else {
        setState(DeviceState::Idle);
    }

    // Persist the URL if the user edited it in the captive portal.
    if (saveNeeded) {
        const char* val = urlParam.getValue();
        if (val && *val) {
            String url(val);
            while (url.endsWith("/")) url.remove(url.length() - 1);
            sBase = url;
            Preferences prefs;
            prefs.begin("weigh", false);
            prefs.putString("spoolman_url", url.c_str());
            prefs.end();
        }
    }

    // Per-tag state, valid while a spool is on the scale.
    int     sSpoolId         = -1;
    float   sLastRemaining   = 0.0f;
    bool    sNeedWeightPatch  = false;
    OptMain sSnapshot        = {};

    for (;;) {
        DeviceState state = getState();

        // ── Quiescent / boot states ───────────────────────────────────────────
        switch (state) {
            case DeviceState::Boot:
            case DeviceState::WiFiSetupMode:
            case DeviceState::TagDetecting:
            case DeviceState::TagReadError:
            case DeviceState::BlankTagFound:
            case DeviceState::AwaitingFormatConfirm:
            case DeviceState::FormattingAndRegistering:
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;

            case DeviceState::Idle:
                sSpoolId = -1; sLastRemaining = 0.0f; sNeedWeightPatch = false; sSnapshot = {};
                gSpoolId = -1; gSpoolNeedsOnboarding = false;
                vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
                continue;

            case DeviceState::IdleNoWiFi:
                sSpoolId = -1; sLastRemaining = 0.0f; sNeedWeightPatch = false; sSnapshot = {};
                gSpoolId = -1; gSpoolNeedsOnboarding = false;
                if (WiFi.status() == WL_CONNECTED) setState(DeviceState::Idle);
                vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
                continue;

            default:
                break;
        }

        // ── ValidTagFound ─────────────────────────────────────────────────────
        // nfcTask confirmed an OPT tag.  gTagMain is populated from the NFC read
        // (nil UUID = freshly formatted blank; real UUID = known or foreign spool).
        if (state == DeviceState::ValidTagFound) {
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain main = gTagMain;
            xSemaphoreGive(gTagMutex);

            if (isNilUUID(main.instance_uuid)) {
                // Blank tag just formatted by nfcTask — create a stub Spool.
                // The admin fills in real data through Spoolman's web UI; the
                // reconciliation loop in Present will write it back to the tag.
                generateUUIDv4(main.instance_uuid);

                int vid = findOrCreateVendor("Unknown");
                if (vid < 0) { setState(DeviceState::SpoolmanUnreachable); continue; }

                OptMain stub = {};
                strlcpy(stub.material_name, "Unknown", sizeof(stub.material_name));
                int fid = findOrCreateFilament(vid, stub);
                if (fid < 0) { setState(DeviceState::SpoolmanUnreachable); continue; }

                int sid = createSpool(fid, main.instance_uuid, 0.0f, 0.0f, true);
                if (sid < 0) { setState(DeviceState::SpoolmanUnreachable); continue; }
                sSpoolId = sid;
                gSpoolId = sid;
                gSpoolNeedsOnboarding = true;

                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                memcpy(gTagMain.instance_uuid, main.instance_uuid, 16);
                xSemaphoreGive(gTagMutex);
                gWriteMainPending = true;

                sSnapshot = {};
                setState(DeviceState::Present);

            } else {
                char uuidStr[37];
                uuidToStr(main.instance_uuid, uuidStr);

                JsonDocument spoolDoc;
                int sid = findSpoolByNfcId(uuidStr, spoolDoc);

                if (sid == -2) {
                    setState(DeviceState::SpoolmanUnreachable);
                } else if (sid < 0) {
                    setState(DeviceState::ForeignTagFound);
                } else {
                    sSpoolId = sid;
                    gSpoolId = sid;
                    gSpoolNeedsOnboarding =
                        (spoolDoc["extra"]["needs_onboarding"] | false);

                    // Sync authoritative Spoolman data down to the tag.
                    OptMain updated = main;
                    populateMainFromSpool(spoolDoc.as<JsonVariantConst>(), &updated);
                    memcpy(updated.instance_uuid, main.instance_uuid, 16);
                    updated.material_class = main.material_class;
                    updated.material_type  = main.material_type;

                    if (mainDiffers(updated, main)) {
                        xSemaphoreTake(gTagMutex, portMAX_DELAY);
                        gTagMain = updated;
                        xSemaphoreGive(gTagMutex);
                        gWriteMainPending = true;
                    }
                    sSnapshot = updated;
                    setState(DeviceState::WeighingAndSync);
                }
            }
            continue;
        }

        // ── ForeignTagFound → RegisteringForeignTag ───────────────────────────
        if (state == DeviceState::ForeignTagFound) {
            setState(DeviceState::RegisteringForeignTag);
            continue;
        }

        if (state == DeviceState::RegisteringForeignTag) {
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain main = gTagMain;
            xSemaphoreGive(gTagMutex);

            const char* vendorName = main.brand_name[0] ? main.brand_name : "Unknown";
            int vid = findOrCreateVendor(vendorName);
            if (vid < 0) { setState(DeviceState::SpoolmanUnreachable); continue; }

            int fid = findOrCreateFilament(vid, main);
            if (fid < 0) { setState(DeviceState::SpoolmanUnreachable); continue; }

            if (isNilUUID(main.instance_uuid)) generateUUIDv4(main.instance_uuid);

            int sid = createSpool(fid, main.instance_uuid,
                                  main.actual_netto_full_weight,
                                  main.empty_container_weight,
                                  false);
            if (sid < 0) { setState(DeviceState::SpoolmanUnreachable); continue; }
            sSpoolId = sid;
            gSpoolId = sid;
            gSpoolNeedsOnboarding = false;

            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            memcpy(gTagMain.instance_uuid, main.instance_uuid, 16);
            xSemaphoreGive(gTagMutex);
            gWriteMainPending = true;

            sSnapshot = main;
            setState(DeviceState::WeighingAndSync);
            continue;
        }

        // ── WeighingAndSync ───────────────────────────────────────────────────
        if (state == DeviceState::WeighingAndSync) {
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain main = gTagMain;
            xSemaphoreGive(gTagMutex);

            xSemaphoreTake(gWeightMutex, portMAX_DELAY);
            float weight = gWeightGrams;
            xSemaphoreGive(gWeightMutex);

            float remaining = (main.empty_container_weight > 0.0f)
                            ? weight - main.empty_container_weight
                            : weight;
            if (remaining < 0.0f) remaining = 0.0f;

            float consumed = (main.actual_netto_full_weight > 0.0f)
                           ? main.actual_netto_full_weight - remaining
                           : 0.0f;
            if (consumed < 0.0f) consumed = 0.0f;

            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            gTagAux.consumed_weight = consumed;
            xSemaphoreGive(gTagMutex);
            gWriteAuxPending = true;

            sLastRemaining   = remaining;
            sNeedWeightPatch = true;

            if (sSpoolId > 0 && patchSpoolWeight(sSpoolId, remaining)) {
                sNeedWeightPatch = false;
                setState(DeviceState::Present);
            } else if (sSpoolId > 0) {
                setState(DeviceState::SpoolmanUnreachable);
            } else {
                setState(DeviceState::Present);
            }
            continue;
        }

        // ── ReconcilingMainSection ────────────────────────────────────────────
        // nfcTask owns the physical write and transitions back to Present when done.
        if (state == DeviceState::ReconcilingMainSection) {
            vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
            continue;
        }

        // ── Present ───────────────────────────────────────────────────────────
        if (state == DeviceState::Present) {
            // Retry weight PATCH that failed during WeighingAndSync.
            if (sNeedWeightPatch && sSpoolId > 0) {
                if (patchSpoolWeight(sSpoolId, sLastRemaining)) sNeedWeightPatch = false;
            }

            if (sSpoolId < 0) {
                vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
                continue;
            }

            JsonDocument spoolDoc;
            if (!getSpoolById(sSpoolId, spoolDoc)) {
                setState(DeviceState::SpoolmanUnreachable);
                vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
                continue;
            }

            // Merge Spoolman's latest data; preserve tag-only fields.
            OptMain latest = sSnapshot;
            populateMainFromSpool(spoolDoc.as<JsonVariantConst>(), &latest);
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            memcpy(latest.instance_uuid, gTagMain.instance_uuid, 16);
            latest.material_class = gTagMain.material_class;
            latest.material_type  = gTagMain.material_type;
            xSemaphoreGive(gTagMutex);

            // Track whether admin has cleared the onboarding flag.
            if (gSpoolNeedsOnboarding && !(spoolDoc["extra"]["needs_onboarding"] | false))
                gSpoolNeedsOnboarding = false;

            if (mainDiffers(latest, sSnapshot)) {
                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                gTagMain = latest;
                xSemaphoreGive(gTagMutex);
                gWriteMainPending = true;
                sSnapshot = latest;
                setState(DeviceState::ReconcilingMainSection);
            }

            vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
            continue;
        }

        // ── SpoolmanUnreachable ───────────────────────────────────────────────
        if (state == DeviceState::SpoolmanUnreachable) {
            if (WiFi.status() == WL_CONNECTED && sSpoolId > 0) {
                JsonDocument doc;
                if (getSpoolById(sSpoolId, doc)) {
                    if (sNeedWeightPatch) {
                        if (patchSpoolWeight(sSpoolId, sLastRemaining)) sNeedWeightPatch = false;
                    }
                    setState(DeviceState::Present);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS * 3));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SPOOLMAN_POLL_MS));
    }
}

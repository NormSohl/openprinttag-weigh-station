#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_random.h>
#include <string.h>
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"
#include "store.h"
#include "web_app.h"

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
extern char                 gWebAddr[48];
extern char                 gApSsid[24];

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

// 32 lowercase hex chars, no dashes — matches the store's uuid key format.
static void uuidToHex32(const uint8_t* uuid, char* out33) {
    for (int i = 0; i < 16; i++)
        snprintf(out33 + i * 2, 3, "%02x", uuid[i]);
}

// ── Tag ⇄ store field mapping ─────────────────────────────────────────────────

// Overlay the store record's identity fields onto an OptMain, preserving the
// tag-only fields (instance_uuid, material_class/type, actual weight, temps).
static void overlayRecordOntoMain(const SpoolRecord& r, OptMain& m) {
    strlcpy(m.brand_name,            r.vendor,   sizeof(m.brand_name));
    strlcpy(m.material_name,         r.material, sizeof(m.material_name));
    strlcpy(m.material_abbreviation, r.abbr,     sizeof(m.material_abbreviation));
    memcpy(m.primary_color_rgba, r.rgba, 4);
    m.filament_diameter         = r.dia;
    m.empty_container_weight    = r.empty_g;
    m.nominal_netto_full_weight = r.nom_g;
    // A freshly weighed/full spool has no factory "actual" — seed it from the
    // nominal so remaining/used maths have a full-weight reference.
    if (r.nom_g > 0.0f && m.actual_netto_full_weight <= 0.0f)
        m.actual_netto_full_weight = r.nom_g;
}

// True if the store record's identity differs from what's on the tag's Main.
// Only the fields the store tracks are compared (temps/class/type are tag-only).
static bool recordDiffersFromMain(const SpoolRecord& r, const OptMain& m) {
    return strcmp(r.vendor,   m.brand_name)            != 0
        || strcmp(r.material, m.material_name)         != 0
        || strcmp(r.abbr,     m.material_abbreviation) != 0
        || memcmp(r.rgba,     m.primary_color_rgba, 4) != 0
        || r.dia     != m.filament_diameter
        || r.empty_g != m.empty_container_weight
        || r.nom_g   != m.nominal_netto_full_weight;
}

// Fill an Onboard/Reconcile event's identity fields from a tag's Main section.
static void identityFromMain(const OptMain& m, StoreEvent& e) {
    strlcpy(e.vendor,   m.brand_name[0]    ? m.brand_name    : "Unknown", sizeof(e.vendor));
    strlcpy(e.material, m.material_name[0] ? m.material_name : "Unknown", sizeof(e.material));
    strlcpy(e.abbr,     m.material_abbreviation, sizeof(e.abbr));
    memcpy(e.rgba, m.primary_color_rgba, 4);
    e.dia     = m.filament_diameter;
    e.empty_g = m.empty_container_weight;
    e.nom_g   = m.nominal_netto_full_weight;   // label weight; actual stays on the tag
}

// ── Captive portal ────────────────────────────────────────────────────────────

// Pump WiFiManager's non-blocking portal until the user finishes, or until one
// of two deadlines expires. Returns true if the device joined a network.
//
// Why two deadlines: the unit lives in a cabinet with the BOOT button out of
// reach, so the portal can never be allowed to stay open indefinitely — but it
// also must not cut someone off while they're typing a password.
//
//   idle     — restarted whenever a client is associated to the AP, so an
//              active setup session is never interrupted.
//   absolute — never restarted. Bounds the "joined, then walked away with the
//              phone still associated" case, which the idle timer alone would
//              keep alive forever.
//
// On expiry the caller falls back to the SoftAP, so the web app stays reachable
// either way — a timeout costs configurability, never availability.
static bool runConfigPortal(WiFiManager& wm) {
    const uint32_t idleMs = (uint32_t)WIFI_PORTAL_TIMEOUT_SEC * 1000UL;
    const uint32_t maxMs  = (uint32_t)WIFI_PORTAL_MAX_SEC     * 1000UL;

    const uint32_t start = millis();
    uint32_t lastClient  = start;
    bool     sawClient   = false;

    for (;;) {
        wm.process();                       // serves the portal + captive DNS

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("[wifi] portal: credentials accepted, connected");
            return true;
        }

        const uint32_t now = millis();      // unsigned math: rollover-safe

        if (WiFi.softAPgetStationNum() > 0) {
            lastClient = now;               // someone is here; hold the idle timer
            if (!sawClient) {
                sawClient = true;
                Serial.println("[wifi] portal: client joined, idle timer suspended");
            }
        }

        if (now - lastClient >= idleMs) {
            Serial.printf("[wifi] portal: idle %u s with no client — giving up\n",
                          (unsigned)(idleMs / 1000));
            return false;
        }
        if (now - start >= maxMs) {
            Serial.printf("[wifi] portal: hit the %u s cap — giving up\n",
                          (unsigned)(maxMs / 1000));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── Task ──────────────────────────────────────────────────────────────────────

void syncTask(void* param) {
    // Set WiFiSetupMode before the portal runs so displayTask can show the
    // captive-portal SSID while we wait for provisioning.
    setState(DeviceState::WiFiSetupMode);
    WiFiManager wm;
    // Drive the portal ourselves rather than letting WiFiManager block: its
    // built-in timeout either cuts off someone mid-password (what happened on
    // the bench) or, with setAPClientCheck(true), never fires while a phone
    // stays associated. Neither is acceptable on a cabinet-installed unit whose
    // BOOT button is unreachable — the portal MUST close on its own. See
    // runConfigPortal() for the idle/absolute policy.
    wm.setConfigPortalBlocking(false);
    wm.setConfigPortalTimeout(0);      // no internal timeout; we own the policy

    // Hold BOOT (GPIO 0) for WIFI_RESET_HOLD_MS to erase stored WiFi credentials
    // and force the captive portal to reopen — useful when moving the device to a
    // new network.
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

    // Join lab WiFi if provisioned; otherwise fall back to our own SoftAP so the
    // device stays usable (and the web app reachable) with no infrastructure.
    // In non-blocking mode autoConnect() returns immediately: true if stored
    // credentials worked, false once it has *started* the portal — so a false
    // here means "portal is up", not "failed". runConfigPortal() decides.
    bool joined = wm.autoConnect("WeighStation-Setup");

    // autoConnect() only raises the portal when it cannot join a saved network,
    // so a false here means "portal is up". Remember that: stopConfigPortal()
    // dereferences the portal's server/DNS objects and null-derefs if they were
    // never allocated — which is exactly what happens on a boot that connects
    // straight to a saved AP.
    const bool portalRan = !joined;
    if (portalRan) joined = runConfigPortal(wm);

    // Release WiFiManager's portal web server before our own tries to bind :80.
    // Blocking autoConnect() does this internally on the way out; the
    // non-blocking path does NOT, so without this the portal keeps the port,
    // webAppBegin() fails to bind, and once WiFiManager's server goes away
    // nothing is listening at all — the browser gets ERR_CONNECTION_REFUSED.
    if (portalRan) {
        wm.stopConfigPortal();
        vTaskDelay(pdMS_TO_TICKS(200));   // let the socket actually close
    }

    if (!joined) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("WeighStation");
        strlcpy(gApSsid, "WeighStation", sizeof(gApSsid));
        WiFi.softAPIP().toString().toCharArray(gWebAddr, sizeof(gWebAddr));
        setState(DeviceState::IdleNoWiFi);
        Serial.printf("[wifi] SoftAP '%s' — web app at http://%s\n",
                      gApSsid, gWebAddr);
    } else {
        gApSsid[0] = '\0';
        // Show the IP rather than the mDNS name: .local depends on the *client*
        // resolving it (Windows in particular is unreliable), while the IP works
        // from anything on the network. mDNS still runs, so weighstation.local
        // keeps working for clients that can resolve it.
        WiFi.localIP().toString().toCharArray(gWebAddr, sizeof(gWebAddr));
        setState(DeviceState::Idle);
        Serial.printf("[wifi] joined '%s' — web app at http://%s  (or http://%s.local)\n",
                      WiFi.SSID().c_str(), gWebAddr, DEVICE_HOSTNAME);
    }
    // The web app runs regardless of station vs AP mode.
    webAppBegin();
    Serial.println("[web] server started on :80");

    // Per-tag state, valid while a spool is on the scale.
    int     sSpoolId  = -1;
    OptMain sSnapshot = {};

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
            case DeviceState::IdleNoWiFi:
                sSpoolId = -1; sSnapshot = {};
                gSpoolId = -1; gSpoolNeedsOnboarding = false;
                // If station WiFi comes up later, upgrade to Idle and switch the
                // shown address from the SoftAP IP to the mDNS hostname.
                if (state == DeviceState::IdleNoWiFi && WiFi.status() == WL_CONNECTED) {
                    gApSsid[0] = '\0';
                    snprintf(gWebAddr, sizeof(gWebAddr), "%s.local", DEVICE_HOSTNAME);
                    setState(DeviceState::Idle);
                }
                // Event-log compaction, only while nothing is on the scale:
                // it rewrites the whole log and holds the store lock for the
                // duration, so it must never land mid-weigh.
                //
                // Throttled to once a minute rather than riding the ~1 Hz
                // reconcile tick. The check opens the log to stat it, and the
                // log takes months to reach the threshold — checking 60x more
                // often buys nothing and just churns the filesystem.
                //
                // Braced: `nowMs` has a runtime initialiser, and a bare
                // declaration in a case block that is followed by another case
                // label is a "jump to case label crosses initialization" error.
                {
                    static uint32_t lastCompactCheck = 0;
                    const uint32_t nowMs = millis();
                    if (nowMs - lastCompactCheck >= 60000UL) {
                        lastCompactCheck = nowMs;
                        if (storeCompactNeeded()) {
                            const size_t before = storeLogBytes();
                            Serial.printf("[store] compacting log (%u kB)…\n",
                                          (unsigned)(before >> 10));
                            const bool ok = storeCompact();
                            Serial.printf("[store] compaction %s: %u kB -> %u kB, "
                                          "%u lines, %u kB free\n",
                                          ok ? "ok" : "FAILED",
                                          (unsigned)(before >> 10),
                                          (unsigned)(storeLogBytes() >> 10),
                                          (unsigned)storeLogLineCount(),
                                          (unsigned)(storeFreeBytes() >> 10));
                        }
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(RECONCILE_POLL_MS));
                continue;

            default:
                break;
        }

        // ── ValidTagFound ─────────────────────────────────────────────────────
        // nfcTask confirmed an OPT tag. gTagMain is populated from the NFC read
        // (nil UUID = freshly formatted blank; real UUID = known or foreign spool).
        if (state == DeviceState::ValidTagFound) {
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain main = gTagMain;
            xSemaphoreGive(gTagMutex);

            if (isNilUUID(main.instance_uuid)) {
                // Blank tag just formatted by nfcTask — mint an identity and a
                // local stub record. A person completes the details later in the
                // web form (Phase 4); the reconcile loop writes them back to the
                // tag while it's still on the scale.
                generateUUIDv4(main.instance_uuid);
                char hex[33]; uuidToHex32(main.instance_uuid, hex);

                StoreEvent e; e.ev = StoreEv::Onboard;
                strlcpy(e.uuid, hex, sizeof(e.uuid));
                strlcpy(e.vendor,   "Unknown", sizeof(e.vendor));
                strlcpy(e.material, "Unknown", sizeof(e.material));
                e.needs_ob = true;
                e.spool = storeNextSpoolId();
                storeAppendEvent(e);

                sSpoolId = (int)e.spool;
                gSpoolId = sSpoolId;
                gSpoolNeedsOnboarding = true;

                // Stamp the new UUID (and the stub identity) onto the tag.
                SpoolRecord r;
                storeFindByUuid(hex, r);
                overlayRecordOntoMain(r, main);
                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                gTagMain = main;
                xSemaphoreGive(gTagMutex);
                gWriteMainPending = true;

                sSnapshot = main;
                setState(DeviceState::Present);

            } else {
                char hex[33]; uuidToHex32(main.instance_uuid, hex);
                SpoolRecord r;
                if (storeFindByUuid(hex, r)) {
                    // Known spool. Push any web-side identity edits down to the tag.
                    sSpoolId = (int)r.spool;
                    gSpoolId = sSpoolId;
                    gSpoolNeedsOnboarding = r.needs_ob;

                    OptMain updated = main;
                    overlayRecordOntoMain(r, updated);
                    if (recordDiffersFromMain(r, main)) {
                        xSemaphoreTake(gTagMutex, portMAX_DELAY);
                        gTagMain = updated;
                        xSemaphoreGive(gTagMutex);
                        gWriteMainPending = true;
                    }
                    sSnapshot = updated;
                    setState(DeviceState::WeighingAndSync);
                } else {
                    setState(DeviceState::ForeignTagFound);
                }
            }
            continue;
        }

        // ── ForeignTagFound → RegisteringForeignTag ───────────────────────────
        if (state == DeviceState::ForeignTagFound) {
            setState(DeviceState::RegisteringForeignTag);
            continue;
        }

        // A well-formed tag we've never seen (genuine Prusament, another maker's
        // tooling). Decode its own Main data and create a local record from it.
        if (state == DeviceState::RegisteringForeignTag) {
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain main = gTagMain;
            xSemaphoreGive(gTagMutex);

            if (isNilUUID(main.instance_uuid)) generateUUIDv4(main.instance_uuid);
            char hex[33]; uuidToHex32(main.instance_uuid, hex);

            StoreEvent e; e.ev = StoreEv::Onboard;
            strlcpy(e.uuid, hex, sizeof(e.uuid));
            identityFromMain(main, e);
            e.needs_ob = false;
            e.spool = storeNextSpoolId();
            storeAppendEvent(e);

            sSpoolId = (int)e.spool;
            gSpoolId = sSpoolId;
            gSpoolNeedsOnboarding = false;

            // Ensure the (possibly newly minted) UUID is written to the tag.
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            memcpy(gTagMain.instance_uuid, main.instance_uuid, 16);
            xSemaphoreGive(gTagMutex);
            gWriteMainPending = true;

            sSnapshot = main;
            setState(DeviceState::WeighingAndSync);
            continue;
        }

        // ── WeighingAndSync ───────────────────────────────────────────────────
        // Weigh once; write remaining/used to both the tag Aux and the log.
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

            float used = (main.actual_netto_full_weight > 0.0f)
                       ? main.actual_netto_full_weight - remaining
                       : 0.0f;
            if (used < 0.0f) used = 0.0f;

            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            gTagAux.consumed_weight = used;
            xSemaphoreGive(gTagMutex);
            gWriteAuxPending = true;

            StoreEvent w; w.ev = StoreEv::Weigh;
            char hex[33]; uuidToHex32(main.instance_uuid, hex);
            strlcpy(w.uuid, hex, sizeof(w.uuid));
            w.spool       = (sSpoolId > 0) ? (uint32_t)sSpoolId : 0;
            w.gross_g     = weight;
            w.remaining_g = remaining;
            w.used_g      = used;
            storeAppendEvent(w);

            setState(DeviceState::Present);
            continue;
        }

        // ── ReconcilingMainSection ────────────────────────────────────────────
        // nfcTask owns the physical write and returns to Present when done.
        if (state == DeviceState::ReconcilingMainSection) {
            vTaskDelay(pdMS_TO_TICKS(RECONCILE_POLL_MS));
            continue;
        }

        // ── Present ───────────────────────────────────────────────────────────
        // Steady state: poll the local store (cheap) and, if a web-side edit has
        // changed this spool's identity since placement, rewrite the tag's Main.
        if (state == DeviceState::Present) {
            if (sSpoolId > 0) {
                char hex[33];
                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                uuidToHex32(gTagMain.instance_uuid, hex);
                xSemaphoreGive(gTagMutex);

                SpoolRecord r;
                if (storeFindByUuid(hex, r)) {
                    gSpoolNeedsOnboarding = r.needs_ob;
                    if (recordDiffersFromMain(r, sSnapshot)) {
                        OptMain updated;
                        xSemaphoreTake(gTagMutex, portMAX_DELAY);
                        updated = gTagMain;
                        overlayRecordOntoMain(r, updated);
                        gTagMain = updated;
                        xSemaphoreGive(gTagMutex);
                        gWriteMainPending = true;
                        sSnapshot = updated;
                        setState(DeviceState::ReconcilingMainSection);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(RECONCILE_POLL_MS));
            continue;
        }

        // Any unhandled state falls through to a slow idle poll.
        vTaskDelay(pdMS_TO_TICKS(RECONCILE_POLL_MS));
    }
}

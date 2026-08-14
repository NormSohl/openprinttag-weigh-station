#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_random.h>
#include <string.h>
#include <time.h>
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"
#include "store.h"
#include "web_app.h"
#include "controller.h"   // WiFi/idle state is now owned by controllerTask

// ── Shared globals ────────────────────────────────────────────────────────────
extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern OptMeta              gTagMeta;
extern OptMain              gTagMain;
extern OptAuxiliary         gTagAux;
extern SemaphoreHandle_t    gTagMutex;
extern volatile bool        gWriteMainPending;
extern volatile bool        gWriteAuxPending;
extern volatile int         gSpoolId;
extern volatile bool        gSpoolNeedsOnboarding;
extern char                 gWebAddr[48];
extern char                 gApSsid[24];
extern volatile int         gPortalSecsLeft;
extern volatile bool        gClockSet;

// ── State helpers ─────────────────────────────────────────────────────────────
// syncTask no longer WRITES gState (phase 4): it drives its own SyncPhase and
// posts events to the controller, the single writer. It still reads gState to
// detect entry into / exit from the weigh flow.

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

// Build a product probe from a tag's Main section: everything the matching
// ladder can key on, in the order it tries them.
//
// The three UUIDs are stored as 32 hex chars to match the store's uuid format;
// a nil UUID on the tag means "absent" and must stay an EMPTY STRING rather
// than 32 zeroes, or every tag that carries no package_uuid would match every
// other one on rung 1.
static void productFromMain(const OptMain& m, ProductRecord& p) {
    if (!optUuidIsNil(m.package_uuid))  uuidToHex32(m.package_uuid,  p.pkg_uuid);
    if (!optUuidIsNil(m.material_uuid)) uuidToHex32(m.material_uuid, p.mat_uuid);
    if (!optUuidIsNil(m.brand_uuid))    uuidToHex32(m.brand_uuid,    p.brand_uuid);
    p.gtin = m.gtin;
    strlcpy(p.vendor,   m.brand_name[0]    ? m.brand_name    : "Unknown", sizeof(p.vendor));
    strlcpy(p.material, m.material_name[0] ? m.material_name : "Unknown", sizeof(p.material));
    strlcpy(p.abbr,     m.material_abbreviation, sizeof(p.abbr));
    memcpy(p.rgba, m.primary_color_rgba, 4);
    if (m.has_lab) { memcpy(p.lab, m.primary_color_lab, sizeof(p.lab)); p.has_lab = true; }
    p.dia     = m.filament_diameter;
    p.empty_g = m.empty_container_weight;
    p.nom_g   = m.nominal_netto_full_weight;
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
// ── WiFi diagnostics ──────────────────────────────────────────────────────────
// BEACON_TIMEOUT fired ~9.4 s after EVERY association on the bench — with and
// without a spool, at uptimes 45 ms apart across separate boots, and again 9.5 s
// after each reconnect. That regularity rules out marginal signal, and
// setSleep(false) did not stop it, which rules out power save. What is left
// needs measuring rather than guessing: is the link weak, or is the station
// associating and then never hearing the AP at all?
//
// getSleep()'s return type changed between core 2.x (bool) and 3.x
// (wifi_ps_type_t), same split the buzzer's LEDC calls straddle.
static bool wifiIsSleeping() {
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    return WiFi.getSleep() != WIFI_PS_NONE;
#else
    return WiFi.getSleep();
#endif
}

static void logWifiState(const char* what) {
    Serial.printf("[wifi] %s: rssi=%d dBm  ch=%d  bssid=%s  powersave=%s\n",
                  what, (int)WiFi.RSSI(), (int)WiFi.channel(),
                  WiFi.BSSIDstr().c_str(), wifiIsSleeping() ? "ON" : "off");
}

// The raw 802.11 reason code. 200 is BEACON_TIMEOUT; 3/4/15/205 point at the AP
// or the key exchange instead, and telling them apart decides whether this is
// ours to fix in firmware or a setting on the access point.
static void onWifiDisconnected(WiFiEvent_t, WiFiEventInfo_t info) {
    Serial.printf("[wifi] disconnect reason=%u  rssi=%d dBm\n",
                  (unsigned)info.wifi_sta_disconnected.reason, (int)WiFi.RSSI());
}

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
            gPortalSecsLeft = -1;
            return false;
        }
        if (now - start >= maxMs) {
            Serial.printf("[wifi] portal: hit the %u s cap — giving up\n",
                          (unsigned)(maxMs / 1000));
            gPortalSecsLeft = -1;
            return false;
        }

        // Publish whichever deadline is actually governing, so the screen shows
        // the one that will fire: the idle timer until somebody associates,
        // then the absolute cap, which never restarts.
        const uint32_t leftMs = sawClient && WiFi.softAPgetStationNum() > 0
                                  ? (maxMs  - (now - start))
                                  : (idleMs - (now - lastClient));
        gPortalSecsLeft = (int)(leftMs / 1000);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── Task ──────────────────────────────────────────────────────────────────────

void syncTask(void* param) {
    // NOT WiFiSetupMode here. That screen says "Join network: WeighStation-Setup",
    // and setting it before autoConnect() made every ordinary boot flash that
    // instruction for the half-second the join took — naming an access point
    // that does not exist yet, on a device that is about to connect fine.
    //
    // It cost a field trip. Debugging a station that had come up in the lab, the
    // half-second flash read as "the setup screen appeared and vanished", which
    // pointed at a portal that had opened and timed out. It had done no such
    // thing: it joined a network immediately, and the only real evidence — the
    // GREEN "Seattle Makers" idle screen rather than the orange "Weigh Station /
    // Join WiFi" one — was buried under a screen that was lying.
    //
    // The state is now set where it is true: after autoConnect() has actually
    // raised the portal. The boot screen covers the join attempt.
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
    if (portalRan) {
        // Only now is "Join WeighStation-Setup" a true statement.
        ctrlPost(CtrlEvent::WifiPortalUp);
        joined = runConfigPortal(wm);
    }

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
        ctrlPost(CtrlEvent::WifiSoftAP);
        Serial.printf("[wifi] SoftAP '%s' — web app at http://%s\n",
                      gApSsid, gWebAddr);
    } else {
        gApSsid[0] = '\0';
        // Keep the radio awake. The ESP32 defaults to WIFI_PS_MIN_MODEM, which
        // sleeps between DTIM beacons and wakes to catch them — and when it
        // misses enough in a row the stack logs
        //   [W][WiFiGeneric.cpp] Reason: 200 - BEACON_TIMEOUT
        // and drops the link, taking the web app with it until it reassociates.
        //
        // That trade is wrong for this device. It is mains-powered and cabinet-
        // mounted, and it exists to answer HTTP requests at unpredictable
        // times; the ~30 mA saved is noise next to the TFT backlight and the
        // PN5180's RF field, while an unreachable web app is the whole product.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        // Start the clock. Asynchronous — this returns immediately and SNTP
        // answers a second or two later, so anything weighed in that window is
        // still stamped 1970 and files as "unknown". That is correct: those
        // grams genuinely have no known month.
        //
        // (0, 0, ...) is UTC with no DST. storeNowIso() formats with gmtime_r
        // and writes a trailing "Z", so an offset here would produce timestamps
        // that contradict their own zone marker.
        configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
        // Show the IP rather than the mDNS name: .local depends on the *client*
        // resolving it (Windows in particular is unreliable), while the IP works
        // from anything on the network. mDNS still runs, so weighstation.local
        // keeps working for clients that can resolve it.
        WiFi.localIP().toString().toCharArray(gWebAddr, sizeof(gWebAddr));
        ctrlPost(CtrlEvent::WifiJoined);
        Serial.printf("[wifi] joined '%s' — web app at http://%s  (or http://%s.local)\n",
                      WiFi.SSID().c_str(), gWebAddr, DEVICE_HOSTNAME);
    }
    // The web app runs regardless of station vs AP mode.
    webAppBegin();
    Serial.println("[web] server started on :80");

    // Station mode only — in SoftAP fallback there is no AP to measure, and
    // RSSI/BSSID would just print noise.
    if (!gApSsid[0]) logWifiState("link");
    WiFi.onEvent(onWifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

    // Per-tag state, valid while a spool is on the scale.
    int     sSpoolId  = -1;
    OptMain sSnapshot = {};
    // True while the spool on the scale is a FOREIGN tag (valid OPT, adopted from
    // its own data, not one we formatted). Such a tag carries another writer's
    // layout — writing our Main/Aux encoding onto it corrupts it (observed: a
    // Prusa tag stopped reading after we "adopted" it, its UUID rewritten). A
    // foreign tag is therefore READ-ONLY: we record it and its weighs in our log,
    // and never write back to it. See "Valid tags not yet in the store" in CLAUDE.md.
    bool    sForeign  = false;

    // Link watch. A station-mode drop is otherwise completely silent: the
    // display keeps showing the address it had at boot, the QR keeps encoding
    // it, and the only symptom is a web app that stopped answering. Worse, DHCP
    // can hand back a DIFFERENT address on reassociation, so the shown one goes
    // from stale to wrong.
    bool wasUp = (WiFi.status() == WL_CONNECTED);

    // Phase 4: syncTask drives the weigh/reconcile flow off this LOCAL phase and
    // posts events; the controller writes the weigh states. Advancing the phase
    // synchronously — unlike gState, which the controller updates a beat later —
    // is what stops syncTask re-entering the Weighing block and appending a
    // DUPLICATE weigh, which would corrupt the consumption rollup (primary data).
    // Mapping: Resolving = ValidTagFound handling, Foreign = ForeignTagFound,
    // Registering = RegisteringForeignTag, Weighing = WeighingAndSync,
    // Holding = Present, Reconciling = ReconcilingMainSection. Idle = nothing on
    // the scale (the switch below resets to it whenever gState reads idle).
    enum class SyncPhase { Idle, Resolving, Foreign, Registering, Weighing,
                           Holding, Reconciling };
    SyncPhase sphase = SyncPhase::Idle;

    for (;;) {
        DeviceState state = getState();

        // Say when the clock lands. SNTP is asynchronous and silent, and the
        // difference between "no usage data yet" and "usage data filed under
        // unknown because the clock never set" is otherwise invisible until
        // someone reads the Usage page a month later and finds one row.
        if (!gClockSet && time(nullptr) > 1700000000L) {   // ~2023-11, i.e. real
            gClockSet = true;
            char iso[25];
            storeNowIso(iso, sizeof(iso));
            Serial.printf("[time] clock set from NTP: %s — usage now buckets by "
                          "real month\n", iso);
        }

        if (!gApSsid[0]) {                       // station mode only
            const bool up = (WiFi.status() == WL_CONNECTED);
            if (up != wasUp) {
                wasUp = up;
                if (up) {
                    // Re-apply: a reassociation can restore the driver default,
                    // and the whole point is that this radio never sleeps.
                    WiFi.setSleep(false);
                    char ip[48];
                    WiFi.localIP().toString().toCharArray(ip, sizeof(ip));
                    if (strcmp(ip, gWebAddr) != 0)
                        Serial.printf("[wifi] reconnected — address changed %s -> %s\n",
                                      gWebAddr, ip);
                    else
                        Serial.println("[wifi] reconnected");
                    strlcpy(gWebAddr, ip, sizeof(gWebAddr));
                    logWifiState("link");
                } else {
                    Serial.println("[wifi] link lost — web app unreachable until it "
                                   "reassociates (auto-reconnect is on)");
                }
            }
        }

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
                sphase = SyncPhase::Idle;   // tag gone (or never arrived)
                sForeign = false;
                // If station WiFi comes up later, upgrade to Idle and switch the
                // shown address from the SoftAP IP to the mDNS hostname.
                if (state == DeviceState::IdleNoWiFi && WiFi.status() == WL_CONNECTED) {
                    gApSsid[0] = '\0';
                    // gWebAddr always holds the NUMERIC address. The display
                    // shows the .local hostname separately and the QR encodes
                    // the IP, because mDNS is the part that isn't universal.
                    // This used to write the hostname here and the IP at join
                    // time, so which one you saw depended on whether WiFi was
                    // up at boot — an accident, not a decision.
                    WiFi.localIP().toString().toCharArray(gWebAddr, sizeof(gWebAddr));
                    ctrlPost(CtrlEvent::WifiJoined);
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

        // Reaching past the switch means gState is a weigh state (default case).
        // Enter the local flow on the first one; from here sphase advances
        // synchronously and syncTask no longer keys the flow on gState.
        if (sphase == SyncPhase::Idle) sphase = SyncPhase::Resolving;

        // ── Resolving (controller shows ValidTagFound) ────────────────────────
        // nfcTask confirmed an OPT tag. gTagMain is populated from the NFC read
        // (nil UUID = freshly formatted blank; real UUID = known or foreign spool).
        if (sphase == SyncPhase::Resolving) {
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain main = gTagMain;
            const uint16_t mainOff = gTagMeta.main_region_offset;
            xSemaphoreGive(gTagMutex);

            // XXX BROKEN SIGNAL — needs a redesign (see below). This once assumed
            // "a foreign writer (Prusa app) puts Main at offset 0 with no Meta,
            // ours puts a Meta first so Main is at a non-zero offset." The OPT
            // reference (utils/nfc_initialize.py + record.py) proves that false:
            // Prusa ALSO writes a Meta first (just {aux_region_offset}) with Main
            // defaulting to the meta size (~4) — and as of 2026-08-14 we match that
            // layout byte-for-byte on purpose. So mainOff is ~4 for BOTH, this is
            // essentially always false now, and there is no longer any byte-level
            // way to tell our tag from a genuine vendor tag.
            //
            // Consequence until redesigned: foreign tags are NO LONGER protected —
            // placing a genuine Prusa spool whose Main differs from our adopted
            // record will rewrite its Main. The real signal has to be ownership
            // (did WE mint this UUID / did a human adopt it), tracked on the record,
            // not the tag bytes. Do not place a vendor tag you care about until then.
            sForeign = (mainOff == 0);

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

                // Stamp the new UUID (and the stub identity) onto the tag — but
                // only if it is ours to write (our layout). A nil-UUID tag with a
                // foreign layout is left untouched.
                SpoolRecord r;
                storeFindByUuid(hex, r);
                overlayRecordOntoMain(r, main);
                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                gTagMain = main;
                xSemaphoreGive(gTagMutex);
                if (!sForeign) gWriteMainPending = true;

                sSnapshot = main;
                ctrlPost(CtrlEvent::StubReady);
                sphase = SyncPhase::Holding;

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
                    // Known, but if it is a foreign-layout tag we adopted earlier,
                    // never push edits down to it — that would rewrite another
                    // vendor's Main in our encoding.
                    if (!sForeign && recordDiffersFromMain(r, main)) {
                        xSemaphoreTake(gTagMutex, portMAX_DELAY);
                        gTagMain = updated;
                        xSemaphoreGive(gTagMutex);
                        gWriteMainPending = true;
                    }
                    sSnapshot = updated;
                    ctrlPost(CtrlEvent::BeginWeigh);
                    sphase = SyncPhase::Weighing;
                } else {
                    ctrlPost(CtrlEvent::SpoolForeign);
                    sphase = SyncPhase::Foreign;
                }
            }
            continue;
        }

        // ── Foreign → Registering (controller shows ForeignTagFound) ──────────
        if (sphase == SyncPhase::Foreign) {
            ctrlPost(CtrlEvent::ForeignRegistering);
            sphase = SyncPhase::Registering;
            continue;
        }

        // A well-formed tag we've never seen (genuine Prusament, another maker's
        // tooling). Decode its own Main data and create a local record from it.
        if (sphase == SyncPhase::Registering) {
            xSemaphoreTake(gTagMutex, portMAX_DELAY);
            OptMain main = gTagMain;
            xSemaphoreGive(gTagMutex);

            const bool wasNil = isNilUUID(main.instance_uuid);
            if (wasNil) generateUUIDv4(main.instance_uuid);
            char hex[33]; uuidToHex32(main.instance_uuid, hex);

            // Resolve the product this spool is an instance of, creating one
            // (provisional) if we have never seen this filament. This is what
            // makes an inventory of four genuine Prusament spools roll up as one
            // product instead of four identical rows — the second spool finds
            // the product the first one created.
            //
            // Adoption never UPDATES an existing product: a product edit
            // propagates to every tag of that product, so one odd or damaged tag
            // could otherwise rewrite a whole shelf. A disagreement is logged for
            // a human to adjudicate and nothing is written.
            ProductRecord probe;
            productFromMain(main, probe);
            bool differs = false;
            const uint32_t pid = storeAdoptProduct(probe, &differs);
            if (differs)
                Serial.printf("[sync] tag %s disagrees with product #%u "
                              "(%s / %s) — product left as-is, tag adopted\n",
                              hex, (unsigned)pid, probe.vendor, probe.material);

            StoreEvent e; e.ev = StoreEv::Onboard;
            strlcpy(e.uuid, hex, sizeof(e.uuid));
            identityFromMain(main, e);
            e.needs_ob = false;
            e.product  = pid;
            e.spool = storeNextSpoolId();
            storeAppendEvent(e);

            sSpoolId = (int)e.spool;
            gSpoolId = sSpoolId;
            gSpoolNeedsOnboarding = false;

            // Write the minted UUID back ONLY if the tag arrived with a nil UUID
            // AND carries our layout (!sForeign) — a tag with no identity of its
            // own that is safe for us to stamp, e.g. one of ours after a store
            // wipe. A genuine foreign tag has its own UUID and its own layout;
            // rewriting its Main in our encoding corrupts it (this is what
            // destroyed a Prusa tag during debugging). Such a tag stays read-only:
            // adopted into the store, never written back.
            if (wasNil && !sForeign) {
                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                memcpy(gTagMain.instance_uuid, main.instance_uuid, 16);
                xSemaphoreGive(gTagMutex);
                gWriteMainPending = true;
            }

            sSnapshot = main;
            ctrlPost(CtrlEvent::BeginWeigh);
            sphase = SyncPhase::Weighing;
            continue;
        }

        // ── Weighing (controller shows WeighingAndSync) ───────────────────────
        // Weigh ONCE; write remaining/used to both the tag Aux and the log. sphase
        // advances to Holding synchronously below, so this block cannot re-run for
        // the same placement even while gState still reads WeighingAndSync — which
        // is exactly the duplicate-weigh the local phase exists to prevent.
        if (sphase == SyncPhase::Weighing) {
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
            // A foreign tag is read-only: the weigh below still records in our log
            // (the source of truth), but we do NOT write consumed_weight back onto
            // another vendor's tag — its Aux layout is not ours to assume.
            if (!sForeign) gWriteAuxPending = true;

            StoreEvent w; w.ev = StoreEv::Weigh;
            char hex[33]; uuidToHex32(main.instance_uuid, hex);
            strlcpy(w.uuid, hex, sizeof(w.uuid));
            w.spool       = (sSpoolId > 0) ? (uint32_t)sSpoolId : 0;
            w.gross_g     = weight;
            w.remaining_g = remaining;
            w.used_g      = used;
            storeAppendEvent(w);

            ctrlPost(CtrlEvent::Weighed);
            sphase = SyncPhase::Holding;
            continue;
        }

        // ── Reconciling (controller shows ReconcilingMainSection) ─────────────
        // nfcTask does the physical Main write and posts ReconcileDone; the
        // controller then shows Present again. Watch for that and resume Holding.
        if (sphase == SyncPhase::Reconciling) {
            if (state == DeviceState::Present) sphase = SyncPhase::Holding;
            vTaskDelay(pdMS_TO_TICKS(RECONCILE_POLL_MS));
            continue;
        }

        // ── Holding (controller shows Present) ────────────────────────────────
        // Steady state: poll the local store (cheap) and, if a web-side edit has
        // changed this spool's identity since placement, rewrite the tag's Main.
        if (sphase == SyncPhase::Holding) {
            if (sSpoolId > 0) {
                char hex[33];
                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                uuidToHex32(gTagMain.instance_uuid, hex);
                xSemaphoreGive(gTagMutex);

                SpoolRecord r;
                if (storeFindByUuid(hex, r)) {
                    gSpoolNeedsOnboarding = r.needs_ob;
                    // Never reconcile-write a foreign tag: a web-side edit to its
                    // record must not rewrite another vendor's Main in our layout.
                    if (!sForeign && recordDiffersFromMain(r, sSnapshot)) {
                        OptMain updated;
                        xSemaphoreTake(gTagMutex, portMAX_DELAY);
                        updated = gTagMain;
                        overlayRecordOntoMain(r, updated);
                        gTagMain = updated;
                        xSemaphoreGive(gTagMutex);
                        gWriteMainPending = true;
                        sSnapshot = updated;
                        ctrlPost(CtrlEvent::NeedsReconcile);
                        sphase = SyncPhase::Reconciling;
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

#include "sd_backup.h"
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "store.h"

// ── SD layout (see sd-local-ecosystem.md) ─────────────────────────────────────
static const char* DIR_BACKUP  = "/backup";
static const char* DIR_HISTORY = "/backup/history";
static const char* DIR_CONFIG  = "/backup/config";
static const char* F_LIVE      = "/backup/events.ndjson";   // current good backup
static const char* F_STAGING   = "/backup/events.staging";  // in-progress write
static const char* F_MANIFEST  = "/backup/manifest.json";
static const char* IMPORT_TMP  = "/log/sd-restore.staging"; // LittleFS scratch for restore

// ── State ─────────────────────────────────────────────────────────────────────
// WARNING — this host is NOT actually free. On the ESP32-S3 Arduino core HSPI
// == bus 1 == the SPI3 peripheral, which is exactly the peripheral TFT_eSPI is
// pinned to (see the tft_flags comment in platformio.ini). Bringing the card up
// would attach GPIO 10/18 to SPI3 alongside the display's GPIO 11/12, and the
// two would share one peripheral's clock/mode config with no arbitration.
//
// SD_BACKUP_ENABLED is 0 for this reason. Resolving it means either putting the
// card on bus 0 with the PN5180 and extending spi_bus.cpp to a three-way
// handoff, or dropping the display's SD in favour of the Thing Plus's own SDIO
// slot. Not yet decided.
static SPIClass          sSdSpi(HSPI);
static SemaphoreHandle_t sMtx = nullptr;
static SdStatus          sStatus;
static size_t            sLastSnapLogBytes = SIZE_MAX;  // log size at last snapshot
static uint32_t          sLastMountAttempt = 0;         // millis of last mount try

struct SdLock {
    SdLock()  { if (sMtx) xSemaphoreTake(sMtx, portMAX_DELAY); }
    ~SdLock() { if (sMtx) xSemaphoreGive(sMtx); }
};

static void setMsg(bool ok, const char* m) {
    sStatus.lastOk = ok;
    strlcpy(sStatus.lastMsg, m, sizeof(sStatus.lastMsg));
}

// ── CRC-32 (IEEE 802.3, reflected) over a file, streamed ──────────────────────
static bool crc32Stream(fs::FS& fs, const char* path,
                        uint32_t& crcOut, size_t& bytesOut, size_t& linesOut) {
    File f = fs.open(path, "r");
    if (!f) return false;
    uint32_t crc = 0xFFFFFFFFu;
    size_t bytes = 0, lines = 0;
    uint8_t buf[512];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
            crc ^= buf[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
        bytes += n;
    }
    f.close();
    crcOut = ~crc; bytesOut = bytes; linesOut = lines;
    return true;
}

// Copy src→dst, returning the CRC/byte/line counts of what was written.
static bool copyFile(fs::FS& srcFs, const char* srcPath,
                     fs::FS& dstFs, const char* dstPath,
                     uint32_t& crcOut, size_t& bytesOut, size_t& linesOut) {
    File in = srcFs.open(srcPath, "r");
    if (!in) return false;
    File out = dstFs.open(dstPath, "w");
    if (!out) { in.close(); return false; }
    uint32_t crc = 0xFFFFFFFFu;
    size_t bytes = 0, lines = 0;
    uint8_t buf[512];
    bool ok = true;
    while (in.available()) {
        size_t n = in.read(buf, sizeof(buf));
        if (out.write(buf, n) != n) { ok = false; break; }
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
            crc ^= buf[i];
            for (int b = 0; b < 8; b++)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
        bytes += n;
    }
    out.flush(); out.close(); in.close();
    crcOut = ~crc; bytesOut = bytes; linesOut = lines;
    return ok;
}

// ── History listing / pruning ─────────────────────────────────────────────────
// Dated snapshot basenames sort chronologically (ISO ts + generation suffix).
static std::vector<String> listHistory() {
    std::vector<String> names;
    File dir = SD.open(DIR_HISTORY);
    if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (f.isDirectory()) continue;
            String nm = f.name();                       // may be full path or basename
            int slash = nm.lastIndexOf('/');
            names.push_back(slash >= 0 ? nm.substring(slash + 1) : nm);
        }
    }
    if (dir) dir.close();
    std::sort(names.begin(), names.end());
    return names;
}

static String historyPath(const String& basename) {
    return String(DIR_HISTORY) + "/" + basename;
}

// Keep the newest SD_HISTORY_KEEP snapshots; delete older ones.
static void pruneHistory() {
    std::vector<String> h = listHistory();
    if (h.size() <= SD_HISTORY_KEEP) return;
    size_t drop = h.size() - SD_HISTORY_KEEP;
    for (size_t i = 0; i < drop; i++) SD.remove(historyPath(h[i]));
}

// Free the card down until `need` bytes are available, evicting oldest history
// first. Never touches the live backup, staging, or manifest. Returns true if
// enough space is (now) free.
static bool ensureSpace(uint64_t need) {
    uint64_t freeB = SD.totalBytes() - SD.usedBytes();
    if (freeB >= need) return true;
    std::vector<String> h = listHistory();           // oldest → newest
    for (size_t i = 0; i < h.size() && freeB < need; i++) {
        SD.remove(historyPath(h[i]));
        freeB = SD.totalBytes() - SD.usedBytes();
    }
    return freeB >= need;
}

// ── Manifest ──────────────────────────────────────────────────────────────────
static void writeManifest(uint32_t gen, const char* ts, uint32_t crc,
                          size_t lines, size_t bytes) {
    JsonDocument doc;
    doc["gen"]   = gen;
    doc["ts"]    = ts;
    char hx[9]; snprintf(hx, sizeof(hx), "%08x", (unsigned)crc);
    doc["crc"]   = hx;
    doc["lines"] = (uint32_t)lines;
    doc["bytes"] = (uint32_t)bytes;
    File f = SD.open(F_MANIFEST, "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

static void readManifestIntoStatus() {
    File f = SD.open(F_MANIFEST, "r");
    if (!f) return;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;
    sStatus.generation = doc["gen"] | 0u;
    strlcpy(sStatus.lastTs, doc["ts"] | "", sizeof(sStatus.lastTs));
    sStatus.lastLines = doc["lines"] | 0u;
    sLastSnapLogBytes = doc["bytes"] | (uint32_t)SIZE_MAX; // don't re-snapshot unchanged log
}

// Best-effort mirror of the config catalog (plain copies; no history/CRC).
static void mirrorConfig() {
    File dir = LittleFS.open("/config");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
    SD.mkdir(DIR_CONFIG);
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) continue;
        String nm = f.name();
        int slash = nm.lastIndexOf('/');
        String base = slash >= 0 ? nm.substring(slash + 1) : nm;
        String src = String("/config/") + base;
        String dst = String(DIR_CONFIG) + "/" + base;
        uint32_t c; size_t b, l;
        copyFile(LittleFS, src.c_str(), SD, dst.c_str(), c, b, l);
    }
    dir.close();
}

static void refreshSpace() {
    sStatus.cardBytes = SD.totalBytes();
    sStatus.freeBytes = SD.totalBytes() - SD.usedBytes();
    sStatus.history   = (uint16_t)listHistory().size();
}

// ── Mount ─────────────────────────────────────────────────────────────────────
static bool tryMount() {
    sSdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sSdSpi, SD_SPI_FREQ_HZ)) {
        sStatus.present = false;
        return false;
    }
    if (SD.cardType() == CARD_NONE) { SD.end(); sStatus.present = false; return false; }
    SD.mkdir(DIR_BACKUP);
    SD.mkdir(DIR_HISTORY);
    sStatus.present = true;
    readManifestIntoStatus();
    refreshSpace();
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────
bool sdBackupBegin() {
    if (!sMtx) sMtx = xSemaphoreCreateMutex();
    SdLock lk;
    sLastMountAttempt = millis();
    if (tryMount()) { setMsg(true, sStatus.generation ? "backup present" : "card ready"); return true; }
    setMsg(true, "no card");
    return false;
}

bool sdCardPresent() { SdLock lk; return sStatus.present; }

void sdGetStatus(SdStatus& out) { SdLock lk; out = sStatus; }

bool sdSnapshot(bool force) {
    SdLock lk;
    if (!sStatus.present) { setMsg(false, "no card"); return false; }

    const char* logPath = storeLogPath();
    size_t logBytes = 0;
    { File lf = LittleFS.open(logPath, "r"); if (lf) { logBytes = lf.size(); lf.close(); } }
    if (logBytes == 0) { setMsg(false, "log empty"); return false; }
    if (!force && logBytes == sLastSnapLogBytes) { setMsg(true, "up to date"); return true; }

    // 0) Make room: peak usage holds the staging copy alongside the live backup.
    if (!ensureSpace((uint64_t)logBytes + 8192)) {
        refreshSpace();
        setMsg(false, "card full");
        return false;
    }

    // 1) Stage: copy the log to a temp file on the card, recording its CRC.
    uint32_t wantCrc; size_t wantBytes, wantLines;
    SD.remove(F_STAGING);
    if (!copyFile(LittleFS, logPath, SD, F_STAGING, wantCrc, wantBytes, wantLines)) {
        SD.remove(F_STAGING); setMsg(false, "stage write failed"); return false;
    }

    // 2) Verify: re-read the staged file from the card and recompute independently.
    uint32_t gotCrc; size_t gotBytes, gotLines;
    if (!crc32Stream(SD, F_STAGING, gotCrc, gotBytes, gotLines)
        || gotCrc != wantCrc || gotBytes != wantBytes) {
        SD.remove(F_STAGING); setMsg(false, "verify failed"); return false;
    }

    // 3) Archive the current good backup to dated history (rename = tiny op).
    char ts[25]; storeNowIso(ts, sizeof(ts));
    if (SD.exists(F_LIVE)) {
        char safeTs[25]; strlcpy(safeTs, ts, sizeof(safeTs));
        for (char* p = safeTs; *p; ++p) if (*p == ':') *p = '-';   // FAT-safe
        String arch = String(DIR_HISTORY) + "/events-" + safeTs
                    + "-g" + String((unsigned)sStatus.generation) + ".ndjson";
        SD.rename(F_LIVE, arch.c_str());
    }

    // 4) Promote staging → live.
    if (!SD.rename(F_STAGING, F_LIVE)) {
        // Fall back to a copy if rename fails; leave staging for the next attempt.
        uint32_t c; size_t b, l;
        if (!copyFile(SD, F_STAGING, SD, F_LIVE, c, b, l)) { setMsg(false, "promote failed"); return false; }
        SD.remove(F_STAGING);
    }

    // 5) Record + prune + mirror config.
    uint32_t gen = sStatus.generation + 1;
    writeManifest(gen, ts, wantCrc, wantLines, wantBytes);
    pruneHistory();
    mirrorConfig();

    sStatus.generation = gen;
    strlcpy(sStatus.lastTs, ts, sizeof(sStatus.lastTs));
    sStatus.lastLines = wantLines;
    sLastSnapLogBytes = logBytes;
    refreshSpace();
    setMsg(true, "snapshot ok");
    return true;
}

bool sdRestoreLatest() {
    SdLock lk;
    if (!sStatus.present)        { setMsg(false, "no card");     return false; }
    if (!SD.exists(F_LIVE))      { setMsg(false, "no backup");   return false; }

    // Copy the SD backup to a LittleFS scratch file, then run it through the same
    // validated import path a host upload uses (CRC-checks each line, rebuilds
    // indices, advances the spool-ID counter). A bad backup leaves the log intact.
    uint32_t c; size_t b, l;
    LittleFS.remove(IMPORT_TMP);
    if (!copyFile(SD, F_LIVE, LittleFS, IMPORT_TMP, c, b, l)) {
        LittleFS.remove(IMPORT_TMP); setMsg(false, "read failed"); return false;
    }
    bool ok = storeImportLogFile(IMPORT_TMP);
    LittleFS.remove(IMPORT_TMP);
    setMsg(ok, ok ? "restore ok" : "restore rejected");
    if (ok) sLastSnapLogBytes = SIZE_MAX;   // force a fresh snapshot of the restored log
    return ok;
}

void sdBackupTick() {
    uint32_t now = millis();
    // Not mounted: retry a mount at the snapshot cadence so a late-inserted card
    // starts working without a reboot (no card-detect line on this header).
    if (!sdCardPresent()) {
        if (now - sLastMountAttempt < SD_SNAPSHOT_MIN_INTERVAL_MS) return;
        SdLock lk; sLastMountAttempt = now; tryMount();
        return;
    }
    // Mounted: throttle, then snapshot only if the log actually grew.
    static uint32_t sLastTick = 0;
    if (now - sLastTick < SD_SNAPSHOT_MIN_INTERVAL_MS) return;
    sLastTick = now;
    size_t logBytes = 0;
    { File lf = LittleFS.open(storeLogPath(), "r"); if (lf) { logBytes = lf.size(); lf.close(); } }
    if (logBytes && logBytes != sLastSnapLogBytes) sdSnapshot(false);
}

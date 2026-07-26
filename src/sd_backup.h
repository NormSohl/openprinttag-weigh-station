#pragma once
#include <Arduino.h>
#include <stdint.h>

// SD-card backup/archive (redesign Phase 6). The microSD on the display board
// sits on its own SPI host (SD_SCK/MOSI/MISO/CS in config.h), isolated from the
// PN5180 + TFT bus, so nothing here touches gSpiMutex.
//
// SD is pure backup: the event log on LittleFS stays the source of truth. A
// snapshot is a fresh, CRC-verified copy promoted by rename, with the previous
// good copy archived to dated history — so an intact backup exists at every
// instant, even across a power cut mid-write. Nothing the device needs to run
// lives only on SD; it runs fine with no card. See sd-local-ecosystem.md.
//
// Threading: all SD work runs on syncTask (single-threaded). Every public call
// is guarded by an internal mutex, so the web task may read status concurrently.

struct SdStatus {
    bool     present   = false;   // card mounted this session
    uint64_t cardBytes = 0;       // total capacity
    uint64_t freeBytes = 0;       // free space
    uint32_t generation = 0;      // snapshot generation (from manifest)
    char     lastTs[25] = {};     // ISO-8601 UTC of the last good snapshot
    uint32_t lastLines  = 0;      // log lines in the last snapshot
    uint16_t history    = 0;      // dated snapshots retained
    bool     lastOk     = true;   // did the most recent op succeed?
    char     lastMsg[56] = {};    // human-readable status / error
};

// Bring up the SD SPI host and mount the card; read the backup manifest if any.
// Returns true if a card mounted. Safe (and cheap) to call with no card — the
// device stays fully functional; snapshots simply no-op until a card appears.
bool sdBackupBegin();

// Is a card currently mounted?
bool sdCardPresent();

// Take a verified snapshot now (stage → CRC-verify → archive old → promote →
// manifest + prune, with oldest-first reclaim if the card is full). `force`
// snapshots even when the log hasn't changed since the last one. Returns true
// on success; on any failure the previous good backup is left intact.
bool sdSnapshot(bool force);

// Restore the store's event log from the current good SD backup
// (/backup/events.ndjson), validated through the same path as a host upload.
// Returns false (store untouched) if there's no card or no valid backup.
bool sdRestoreLatest();

// Throttled idle-time auto-snapshot: if a card is present and the log has grown
// since the last snapshot, and the throttle window has elapsed, take one. Call
// from syncTask's idle branch; cheap and self-rate-limiting otherwise.
void sdBackupTick();

// Copy the latest status for display (web /backup page).
void sdGetStatus(SdStatus& out);

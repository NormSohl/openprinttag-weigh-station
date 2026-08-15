#pragma once
#include <Arduino.h>
#include <stdint.h>

// Config catalog (redesign Phase 3). Web-editable reference tables on
// LittleFS that make onboarding pick-not-type and drive reordering.
// Files: /config/{vendors,materials,spool-profiles,colors,stock-items}.json
// Loaded once at boot (seeded with defaults if missing). Small, hand-edited
// data — held in RAM, rewritten whole on save.

struct CfgMaterial {
    char    name[48];   // "PETG"
    char    abbr[16];   // "PETG"
    int8_t  cls;        // material_class enum (OPT key 8)
    int8_t  type;       // material_type enum  (OPT key 9)
    float   dia;        // 1.75
    int16_t print_min, print_max;   // °C (OPT keys 34/35)
    int16_t bed_min,   bed_max;     // °C (OPT keys 37/38)
};

struct CfgProfile {     // a spool "size" → tare + nominal-full
    char  label[48];    // "Prusament 1kg PETG"
    float nominal_full_g;
    float empty_g;
};

struct CfgColor {
    char    name[32];   // "Prusa Orange"
    uint8_t rgba[4];
};

struct CfgStock {       // a standard-stock SKU to keep + reorder threshold
    char     vendor[48];
    char     material[48];
    char     color[32];
    float    dia;
    float    spool_g;       // nominal full weight of one spool
    uint16_t min_spools;    // reorder threshold (0 = use min_grams)
    float    min_grams;     // alt threshold (0 = use min_spools)
    char     sku[32];
    char     gtin[16];
    uint8_t  pack_qty;
};

// ── Lifecycle ─────────────────────────────────────────────────────────────────
bool cfgBegin();        // load all tables; seed defaults + save if absent

// ── Read / iterate ────────────────────────────────────────────────────────────
size_t cfgVendorCount();
bool   cfgVendorAt(size_t i, char* out, size_t outlen);
size_t cfgMaterialCount();
bool   cfgMaterialAt(size_t i, CfgMaterial& out);
bool   cfgMaterialByName(const char* name, CfgMaterial& out);
size_t cfgProfileCount();
bool   cfgProfileAt(size_t i, CfgProfile& out);
size_t cfgColorCount();
bool   cfgColorAt(size_t i, CfgColor& out);
bool   cfgColorByName(const char* name, CfgColor& out);
size_t cfgStockCount();
bool   cfgStockAt(size_t i, CfgStock& out);

// ── Mutation ──────────────────────────────────────────────────────────────────
// Add helpers (onboarding save-back). Return false on duplicate/full.
bool cfgVendorAdd(const char* name);
bool cfgProfileAdd(const CfgProfile& p);
bool cfgMaterialAdd(const CfgMaterial& m);
bool cfgColorAdd(const CfgColor& c);
bool cfgStockAdd(const CfgStock& s);

// Replace a whole table from a JSON array string (web CRUD posts this).
// `which` ∈ vendors|materials|spool-profiles|colors|stock-items.
bool cfgReplaceTable(const char* which, const String& json);
// Serialize a table to a JSON array string (web GET returns this).
String cfgTableJson(const char* which);

// ── Combined export/import (all five tables in one file) ────────────────────
// A separate artifact from the event log (store.h's storeExport/storeImportLogFile):
// nothing in a SpoolRecord/ProductRecord points back into these tables by ID —
// vendor/material/colour/temps are copied VALUES, resolved once at onboarding
// time — so the two backups are independent and never need to be the same age.
// This is what the Config page's "Download" / "Restore" buttons call.
String cfgExportAll();                    // one JSON object, all 5 tables
bool   cfgImportAll(const String& json);  // all-or-nothing: any missing/malformed
                                           // table rejects the whole file untouched

bool cfgSave(const char* which);   // persist one table to its file
bool cfgSaveAll();

// ── Serial harness ────────────────────────────────────────────────────────────
// CFG dump [vendors|materials|profiles|colors|stock] | CFG reload
bool cfgSerialCommand(const String& line);

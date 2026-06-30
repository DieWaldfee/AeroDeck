#include "config_manager.h"
#include "config.h"
#include "credentials.h"
#include <Preferences.h>
#include <Arduino.h>
#include <esp_log.h>
#include <cstring>

static const char* TAG     = "CFG";
static const char* NVS_NS  = "bridge";    // NVS-Namespace (max. 15 Zeichen)

RuntimeConfig g_rtCfg;

// ── ConfigManager::load ───────────────────────────────────────────────────────
void ConfigManager::load() {
    Preferences p;
    p.begin(NVS_NS, true);  // read-only

    const String ssid = p.getString("ssid", WIFI_SSID);
    const String pass = p.getString("pass", WIFI_PASSWORD);
    g_rtCfg.ssid = ssid.c_str();
    g_rtCfg.pass = pass.c_str();

    g_rtCfg.can1IdMin = p.getUInt("c1min", (uint32_t)CAN_MODULE_CFG[0].idMin);
    g_rtCfg.can1IdMax = p.getUInt("c1max", (uint32_t)CAN_MODULE_CFG[0].idMax);
    g_rtCfg.can2IdMin = p.getUInt("c2min", (uint32_t)CAN_MODULE_CFG[1].idMin);
    g_rtCfg.can2IdMax = p.getUInt("c2max", (uint32_t)CAN_MODULE_CFG[1].idMax);

    p.end();

    ESP_LOGI(TAG, "Config geladen: SSID='%s'  CAN1:[0x%03lX-0x%03lX]  CAN2:[0x%03lX-0x%03lX]",
             g_rtCfg.ssid.c_str(),
             (unsigned long)g_rtCfg.can1IdMin, (unsigned long)g_rtCfg.can1IdMax,
             (unsigned long)g_rtCfg.can2IdMin, (unsigned long)g_rtCfg.can2IdMax);
}

// ── ConfigManager::save ───────────────────────────────────────────────────────
void ConfigManager::save() {
    Preferences p;
    p.begin(NVS_NS, false);

    p.putString("ssid",  g_rtCfg.ssid.c_str());
    p.putString("pass",  g_rtCfg.pass.c_str());
    p.putUInt  ("c1min", g_rtCfg.can1IdMin);
    p.putUInt  ("c1max", g_rtCfg.can1IdMax);
    p.putUInt  ("c2min", g_rtCfg.can2IdMin);
    p.putUInt  ("c2max", g_rtCfg.can2IdMax);

    p.end();
    ESP_LOGI(TAG, "Config in NVS gespeichert");
}

// ── ConfigManager::reset ──────────────────────────────────────────────────────
void ConfigManager::reset() {
    Preferences p;
    p.begin(NVS_NS, false);
    p.clear();
    p.end();
    ESP_LOGI(TAG, "NVS geleert — compile-time-Defaults beim naechsten Boot");
}

// ── ConfigManager::show ───────────────────────────────────────────────────────
void ConfigManager::show() {
    Serial0.printf("\r\n=== Bridge-Konfiguration ===\r\n");
    Serial0.printf("SSID      : %s\r\n", g_rtCfg.ssid.c_str());
    Serial0.printf("PASS      : %s\r\n", g_rtCfg.pass.empty() ? "(leer)" : "****");
    Serial0.printf("CAN1_MIN  : 0x%03lX\r\n", (unsigned long)g_rtCfg.can1IdMin);
    Serial0.printf("CAN1_MAX  : 0x%03lX\r\n", (unsigned long)g_rtCfg.can1IdMax);
    Serial0.printf("CAN2_MIN  : 0x%03lX\r\n", (unsigned long)g_rtCfg.can2IdMin);
    Serial0.printf("CAN2_MAX  : 0x%03lX\r\n", (unsigned long)g_rtCfg.can2IdMax);
    Serial0.printf("===========================\r\n\r\n");
}

// ── ConfigManager::loadDecodeTable ────────────────────────────────────────────
void ConfigManager::loadDecodeTable(ButtonDecodeEntry table[MAX_DECODE_ENTRIES],
                                    uint8_t& count) {
    Preferences p;
    p.begin(NVS_NS, true);
    const size_t entrySize = sizeof(ButtonDecodeEntry);   // 20 Bytes
    const size_t maxBytes  = entrySize * MAX_DECODE_ENTRIES;
    const size_t got       = p.getBytes("btndec", table, maxBytes);
    p.end();

    if (got == 0 || (got % entrySize) != 0) {
        count = 0;
        memset(table, 0, maxBytes);
        ESP_LOGI(TAG, "Button-Decode-Tabelle: kein NVS-Eintrag, leer");
    } else {
        count = (uint8_t)(got / entrySize);
        ESP_LOGI(TAG, "Button-Decode-Tabelle: %u Eintraege aus NVS geladen", count);
    }
}

// ── ConfigManager::saveDecodeTable ────────────────────────────────────────────
void ConfigManager::saveDecodeTable(const ButtonDecodeEntry table[MAX_DECODE_ENTRIES],
                                    uint8_t count) {
    const size_t newLen = sizeof(ButtonDecodeEntry) * count;
    const size_t maxLen = sizeof(ButtonDecodeEntry) * MAX_DECODE_ENTRIES;

    Preferences p;
    p.begin(NVS_NS, false);

    // Redundante Flash-Schreibzyklen vermeiden: nur schreiben wenn sich Blob geaendert hat.
    // static: 3800 Bytes im BSS statt auf dem Stack (taskHidDecoder hat nur 4096 Bytes).
    static uint8_t oldBuf[sizeof(ButtonDecodeEntry) * MAX_DECODE_ENTRIES];
    memset(oldBuf, 0, maxLen);
    const size_t oldLen = p.getBytes("btndec", oldBuf, maxLen);
    const bool unchanged = (oldLen == newLen) && (newLen == 0 || memcmp(oldBuf, table, newLen) == 0);
    if (unchanged) {
        p.end();
        ESP_LOGI(TAG, "Button-Decode-Tabelle: unveraendert (%u Eintraege), NVS-Write uebersprungen", count);
        return;
    }

    p.putBytes("btndec", table, newLen);
    p.end();
    ESP_LOGI(TAG, "Button-Decode-Tabelle: %u Eintraege in NVS gespeichert", count);
}

// ── ConfigManager::resetDecodeTable ───────────────────────────────────────────
void ConfigManager::resetDecodeTable(ButtonDecodeEntry table[MAX_DECODE_ENTRIES],
                                     uint8_t& count) {
    count = 0;
    memset(table, 0, sizeof(ButtonDecodeEntry) * MAX_DECODE_ENTRIES);
    ESP_LOGI(TAG, "Button-Decode-Tabelle: zurueckgesetzt (RAM, kein NVS-Schreiben)");
}

// ── ConfigManager::loadAxisMap ────────────────────────────────────────────────
void ConfigManager::loadAxisMap(AxisMapEntry map[MAX_AXIS_ENTRIES], uint8_t& count) {
    Preferences p;
    p.begin(NVS_NS, true);
    const size_t entrySize = sizeof(AxisMapEntry);   // 14 Bytes
    const size_t maxBytes  = entrySize * MAX_AXIS_ENTRIES;
    const size_t got       = p.getBytes("axismap", map, maxBytes);
    p.end();

    if (got == 0 || (got % entrySize) != 0) {
        count = 0;
        memset(map, 0, maxBytes);
        ESP_LOGI(TAG, "Achsen-Map: kein NVS-Eintrag, leer");
    } else {
        count = (uint8_t)(got / entrySize);
        ESP_LOGI(TAG, "Achsen-Map: %u Eintraege aus NVS geladen", count);
    }
}

// ── ConfigManager::saveAxisMap ────────────────────────────────────────────────
void ConfigManager::saveAxisMap(const AxisMapEntry map[MAX_AXIS_ENTRIES], uint8_t count) {
    const size_t newLen = sizeof(AxisMapEntry) * count;
    const size_t maxLen = sizeof(AxisMapEntry) * MAX_AXIS_ENTRIES;

    Preferences p;
    p.begin(NVS_NS, false);

    // Redundante Flash-Schreibzyklen vermeiden: nur schreiben wenn sich Blob geaendert hat.
    static uint8_t oldBuf[sizeof(AxisMapEntry) * MAX_AXIS_ENTRIES];
    memset(oldBuf, 0, maxLen);
    const size_t oldLen = p.getBytes("axismap", oldBuf, maxLen);
    const bool unchanged = (oldLen == newLen) && (newLen == 0 || memcmp(oldBuf, map, newLen) == 0);
    if (unchanged) {
        p.end();
        ESP_LOGI(TAG, "Achsen-Map: unveraendert (%u Eintraege), NVS-Write uebersprungen", count);
        return;
    }

    p.putBytes("axismap", map, newLen);
    p.end();
    ESP_LOGI(TAG, "Achsen-Map: %u Eintraege in NVS gespeichert", count);
}

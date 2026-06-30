#pragma once
#include <cstdint>
#include <string>

// ══════════════════════════════════════════════════════════════════════════════
// Button-Decode-Tabelle
// Definiert die vollständige Kette: CAN-ID + Payload-Byte + Bit → HID-Button(s)
// Wird von bridge.exe beim Start übertragen und im NVS gespeichert.
// Beim Reboot sofort aus NVS geladen — HID-Joystick funktioniert ohne bridge.exe.
//
// canId:    CAN-ID des Moduls (muss in JOY_CAN_ID_MIN..MAX liegen)
// payByte:  Byte-Nummer im CAN-Payload (1-basiert: 1 = Data[0], 2 = Data[1], …)
//           0 ist Sentinel für "leer" und wird beim Dekodieren übersprungen.
// payBit:   Bit im Byte (0=LSB, 7=MSB)
// hidA:     Erster HID-Joystick-Button (1..128)
// hidB:     Zweiter HID-Button (1..128, 0 = kein zweiter / Single-Press)
// physIdx:  Referenznummer (0..127) — für SHOW MAP und Diagnose
// name:     Lesbare Bezeichnung (max. 9 Zeichen)
// ══════════════════════════════════════════════════════════════════════════════
// MAX_DECODE_ENTRIES × sizeof(ButtonDecodeEntry) muss ≤ 3800 Bytes bleiben (NVS-Blob-Limit).
// 190 × 20 = 3800 Bytes.
constexpr uint8_t MAX_DECODE_ENTRIES = 190;
constexpr uint8_t MAX_AXIS_ENTRIES   = 16;

// Keyboard-Modifier-Bits (HID-Spec, Byte 0 des Keyboard-Reports).
// Muss mit KB_* in bridge_exe/src/module.h identisch sein.
constexpr uint8_t KB_NONE   = 0x00;
constexpr uint8_t KB_LCTRL  = 0x01;
constexpr uint8_t KB_LSHIFT = 0x02;
constexpr uint8_t KB_LALT   = 0x04;
constexpr uint8_t KB_RCTRL  = 0x10;
constexpr uint8_t KB_RSHIFT = 0x20;

struct __attribute__((packed)) ButtonDecodeEntry {
    uint32_t canId;        // 4 Bytes
    uint8_t  payByte;      // 1 Byte
    uint8_t  payBit;       // 1 Byte  (0..7)
    uint8_t  hidA;         // 1 Byte  (1..128)
    uint8_t  hidB;         // 1 Byte  (1..128, 0=Single)
    uint8_t  physIdx;      // 1 Byte  (Referenznummer)
    uint8_t  kbMod;        // 1 Byte  (Keyboard-Modifier-Bitmask, 0=keine Taste)
    char     name[10];     // 10 Bytes (9 Zeichen + Null-Terminator)
};  // 20 Bytes gesamt (packed)
static_assert(sizeof(ButtonDecodeEntry) == 20, "ButtonDecodeEntry muss 20 Bytes sein");
// NVS-Blob: MAX_DECODE_ENTRIES × 20 = 3800 Bytes max.

// ══════════════════════════════════════════════════════════════════════════════
// Achsen-Map
// Definiert die Kette: CAN-ID → (axisCount, HID-Achsen-Indizes, kbMod)
// Wird von bridge.exe beim Start übertragen und im NVS gespeichert.
// Beim Reboot sofort aus NVS geladen — Achsen funktionieren ohne bridge.exe.
//
// canId:      CAN-ID des Moduls (= can_rx_id in bridge.ini)
// axisCount:  Anzahl Achsen dieses Moduls (1..8)
// hidIdx[i]:  HID-Achsen-Index für Achse i (0..7)
// kbMod:      Keyboard-Modifier-Bitmask; gedrückt solange Achswert ≠ 0
// ══════════════════════════════════════════════════════════════════════════════
struct __attribute__((packed)) AxisMapEntry {
    uint32_t canId;         // 4 Bytes
    uint8_t  axisCount;     // 1 Byte  (1..8)
    uint8_t  hidIdx[8];     // 8 Bytes (je Achse ein HID-Index 0..7)
    uint8_t  kbMod;         // 1 Byte  (Keyboard-Modifier-Bitmask, 0=keine Taste)
};  // 14 Bytes gesamt (packed)
static_assert(sizeof(AxisMapEntry) == 14, "AxisMapEntry muss 14 Bytes sein");
// NVS-Blob: MAX_AXIS_ENTRIES × 14 = 224 Bytes max.

// ══════════════════════════════════════════════════════════════════════════════
// Laufzeit-Konfiguration
// Beim Boot aus NVS geladen (Fallback: compile-time-Defaults aus credentials.h).
// Änderbar per Serial-Kommando, persistent via SAVE.
// ══════════════════════════════════════════════════════════════════════════════
struct RuntimeConfig {
    // WiFi
    std::string ssid;
    std::string pass;
    // CAN-FD ID-Routing-Bereiche (aus NVS, Fallback: CAN_MODULE_CFG)
    uint32_t can1IdMin;
    uint32_t can1IdMax;
    uint32_t can2IdMin;
    uint32_t can2IdMax;
};

// Globale Instanz — überall per extern sichtbar
extern RuntimeConfig g_rtCfg;

namespace ConfigManager {
    // WiFi + CAN-ID-Konfiguration
    void load();    // aus NVS laden (Fallback: credentials.h / config.h)
    void save();    // in NVS schreiben
    void reset();   // NVS-Namespace löschen → compile-time-Defaults beim nächsten Boot
    void show();    // auf Serial ausgeben

    // Button-Decode-Tabelle (NVS-Key "btndec", bis zu 3800 Bytes)
    // Fallback wenn kein NVS-Eintrag: leere Tabelle (count = 0)
    void loadDecodeTable (ButtonDecodeEntry table[MAX_DECODE_ENTRIES], uint8_t& count);
    void saveDecodeTable (const ButtonDecodeEntry table[MAX_DECODE_ENTRIES], uint8_t count);
    void resetDecodeTable(ButtonDecodeEntry table[MAX_DECODE_ENTRIES], uint8_t& count);

    // Achsen-Map (NVS-Key "axismap", bis zu 208 Bytes)
    // Fallback wenn kein NVS-Eintrag: leere Map (count = 0)
    void loadAxisMap(AxisMapEntry map[MAX_AXIS_ENTRIES], uint8_t& count);
    void saveAxisMap(const AxisMapEntry map[MAX_AXIS_ENTRIES], uint8_t count);
}

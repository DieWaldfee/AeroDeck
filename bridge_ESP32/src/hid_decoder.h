#pragma once
#include "config_manager.h"   // ButtonDecodeEntry, MAX_DECODE_ENTRIES

// ══════════════════════════════════════════════════════════════════════════════
// HID-Decoder-Task
//
// Empfängt CAN-Frames aus g_canToHidQueue und verarbeitet zwei Frame-Typen:
//
// 1. Daten-Frames von Modulen (CAN-ID im JOY_CAN_ID_MIN..MAX-Bereich):
//    Byte 0:     0x02  — Pflicht-Typ-Byte (Frames ohne dieses Byte werden verworfen)
//    Byte 1..N:  Achswerte int16 LE je Achse, Reihenfolge laut axisMap
//    Byte N+1..: Button-Bytes; Position = btnStart + payByte aus decodeTable
//                btnStart = 1 + axisCount * 2
//    Dekodierung via axisMap (RAM-only, befüllt durch bridge.exe)
//                    + decodeTable (NVS-persistent, Schlüssel "btndec")
//
// 2. Mapping-Kommandos von bridge.exe (CAN-ID = HID_MAP_CAN_ID = 0x7F0):
//    Byte 0: 0xFE  — Mapping-Kommando-Marker
//    Byte 1: Subtyp:
//      0xFF  Reset   — decodeTable + axisMap leeren (RAM; NVS unberührt)
//      0x01  Entry   — Button-Decode-Eintrag eintragen (→ NVS sofort)
//      0x02  Axis    — Achsen-Map-Eintrag eintragen (→ NVS sofort)
//      0xFE  Commit  — kein weiterer NVS-Write nötig (Bestätigung/Log)
//
// HID-Report: 8 Achsen (int16) + 128 Buttons (1 Bit je) = 32 Bytes
// Keepalive: alle 20 ms auch ohne Zustandsänderung senden.
// ══════════════════════════════════════════════════════════════════════════════

// Decode-Tabelle (aus NVS geladen, von bridge.exe befüllt)
// Definiert: CAN-ID + Byte + Bit → HID-Button(s)
extern ButtonDecodeEntry decodeTable[MAX_DECODE_ENTRIES];
extern uint8_t           decodeTableSize;

// Achsen-Map (aus NVS geladen, von bridge.exe befüllt)
// Definiert: CAN-ID → (axisCount, HID-Achsen-Indizes)
extern AxisMapEntry axisMap[MAX_AXIS_ENTRIES];
extern uint8_t      axisMapSize;

void taskHidDecoder(void* param);

// ── Simulator-Hilfsfunktion (nur ohne CAN_HW_ENABLED verfügbar) ──────────────
// Fügt einen Button-Decode-Eintrag direkt in RAM ein — KEIN NVS-Save.
// Wird von canSimulatorInit() aufgerufen um HID-Mappings zu registrieren.
#ifndef CAN_HW_ENABLED
void hidDecoderAddSimEntry(uint32_t canId, uint8_t byte, uint8_t bit, uint8_t hidButton);
#endif

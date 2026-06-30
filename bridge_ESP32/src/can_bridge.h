#pragma once

// ══════════════════════════════════════════════════════════════════════════════
// can_bridge.h — MCP2518FD CAN-FD Treiber
//
// NUR aktiv wenn -DCAN_HW_ENABLED in platformio.ini gesetzt ist.
// Andernfalls läuft can_simulator (can_simulator.h/.cpp) stattdessen.
//
// REAKTIVIERUNG wenn MCP2518FD angekommen:
//   platformio.ini → Semikolon vor "-DCAN_HW_ENABLED" entfernen → neu flashen.
// ══════════════════════════════════════════════════════════════════════════════
#ifdef CAN_HW_ENABLED

// MCP2518FD CAN-FD-Controller über SPI initialisieren.
// Gibt true zurück bei Erfolg, false bei Hardware-Fehler.
bool canBridgeInit();

// ── FreeRTOS Tasks ────────────────────────────────────────────────────────────

// CAN-TX: liest aus g_udpToCanQueue → sendet CAN-FD-Frame via MCP2518FD
void taskCanTx(void* param);

// CAN-RX: empfängt CAN-FD-Frames via MCP2518FD → schreibt in g_canToUdpQueue
void taskCanRx(void* param);

#endif // CAN_HW_ENABLED

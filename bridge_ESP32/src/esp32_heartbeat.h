#pragma once
#include <cstdint>

// ══════════════════════════════════════════════════════════════════════════════
// esp32_heartbeat — ESP32-eigener Heartbeat an bridge.exe
//
// Sendet periodisch einen Heartbeat-Frame auf ESP32_HB_CAN_ID (config.h).
// Identisches Format wie Modul-Heartbeats: [0x01][uptime_ms 4B LE]
//
// bridge.exe konfiguriert Interval und Timeout über [module.esp32] in bridge.ini —
// kein Code-Eingriff in bridge.exe nötig.
//
// Aufgerufen aus:
//   can_simulator.cpp / can_bridge.cpp → esp32HbSetInterval + esp32HbTriggerNow
//   main.cpp                           → taskEsp32Heartbeat starten
// ══════════════════════════════════════════════════════════════════════════════

// Interval aus MODULE_CONFIG (0xFD) von bridge.exe setzen.
// Threadsicher (atomic).
void esp32HbSetInterval(uint32_t ms);

// Sofort-Heartbeat auslösen (Reconnect-Signal).
// Threadsicher (atomic flag).
void esp32HbTriggerNow();

// FreeRTOS-Task — in main.cpp starten, läuft in beiden Modi (Simulator + Hardware).
void taskEsp32Heartbeat(void* param);

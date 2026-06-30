#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "config.h"

// ══════════════════════════════════════════════════════════════════════════════
// Gemeinsame Datentypen, Queues und globale Zustandsvariablen
// ══════════════════════════════════════════════════════════════════════════════

// ── Roher UDP/CAN-Puffer ──────────────────────────────────────────────────────
// Format: [4B CAN-ID Little-Endian][≤64B Payload]  — identisch UDP ↔ USB
struct RawPacket {
    uint8_t  buf[MAX_UDP_PACKET];
    uint16_t len;
};

// ── Eingehende Queue (gemeinsam für UDP-RX und USB-RX) ────────────────────────
// Beide Transporte schreiben hier hinein → CanTx liest daraus.
extern QueueHandle_t g_udpToCanQueue;

// ── Simulator-Kommando-Queue (nur Hybrid-Modus: CAN_HW_ENABLED + CAN_SIM_INPUTS)
// taskCanTx leitet JOY-Bereich-Frames (0x600-0x6FF) hierher um.
// taskCanSimulator liest daraus statt aus g_udpToCanQueue.
// nullptr wenn nicht benötigt (reiner Hardware- oder reiner Simulator-Modus).
extern QueueHandle_t g_simCmdQueue;

// ── Ausgehende Queues (getrennt für jeden Transport) ──────────────────────────
// CanRx schreibt in BEIDE Queues. Jeder Transport-Task liest seine eigene.
// Inaktiver Transport: liest und verwirft (kein Stau in der aktiven Queue).
extern QueueHandle_t g_canToUdpQueue;   // → TaskNetTx (WiFi/UDP)
extern QueueHandle_t g_canToUsbQueue;   // → TaskUsbTx (UART-Brücken-Port)
extern QueueHandle_t g_canToHidQueue;   // → TaskHidDecoder (nur JOY-CAN-ID-Bereich)
extern QueueHandle_t g_consoleRxQueue; // → taskSerialConsole (Bytes von taskUsbRx weitergeleitet)

// ── Per-Modul-Mutex (je ein Mutex pro MCP2518FD / SPI-Bus) ───────────────────
extern SemaphoreHandle_t g_canMutex[2];

// ── Task-Handle für taskUsbTx (wird von logSilent/logVerbose zum Suspend/Resume genutzt) ──
extern TaskHandle_t g_taskUsbTxHandle;

// ── HID-Joystick-Zustand ──────────────────────────────────────────────────────
// Wird von taskHidDecoder aus CAN-Frames aufgebaut und per USB HID gesendet.
// axes:    8 × int16  (X,Y,Z,Rx,Ry,Rz,Slider,Dial)  −32767..+32767, 0=Mitte
// buttons: 128 Bit   (Button 1 = buttons[0] Bit 0)
struct JoystickState {
    int16_t axes[8];       // 16 Bytes
    uint8_t buttons[16];   // 16 Bytes — 128 Bits
};
static_assert(sizeof(JoystickState) == 32, "JoystickState muss 32 Bytes sein");

extern JoystickState g_joystickState;  // definiert in hid_decoder.cpp

// ── Transport-Status (std::atomic: Multi-Core-sicher ohne Mutex) ──────────────
extern std::atomic<bool> g_bridgeIpKnown;  // Bridge-IP aus erstem UDP-Paket gelernt
extern std::atomic<uint32_t> g_bridgeIpAddr;

extern std::atomic<bool>     g_wifiReady;      // WiFi verbunden, UDP-Socket offen
extern std::atomic<bool>     g_wifiDisabled;   // WiFi-Timeout → WIFI_OFF gesetzt
extern std::atomic<bool>     g_usbTxActive;    // UART-TX aktiv (Frame gesendet) — kein Indikator für Host-Verbindung
extern std::atomic<TickType_t> g_lastUartRxTick;  // Tick des letzten empfangenen UART-Frames (0 = noch nie)

// ── Utility: CAN-ID ↔ 4-Byte-Little-Endian ───────────────────────────────────
inline uint32_t readCanId(const uint8_t* buf) {
    return  (uint32_t)buf[0]
          | ((uint32_t)buf[1] <<  8)
          | ((uint32_t)buf[2] << 16)
          | ((uint32_t)buf[3] << 24);
}

inline void writeCanId(uint8_t* buf, uint32_t id) {
    buf[0] = (uint8_t)(id >>  0);
    buf[1] = (uint8_t)(id >>  8);
    buf[2] = (uint8_t)(id >> 16);
    buf[3] = (uint8_t)(id >> 24);
}

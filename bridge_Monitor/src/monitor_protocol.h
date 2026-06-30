#pragma once
#include <cstdint>

// ── Pakettypen: bridge_exe → bridge_monitor ───────────────────────────────────
// Format: [1B type][4B can_id LE][payload]
static constexpr uint8_t MON_FRAME_FORWARD    = 0x01;  // CAN-Frame gesendet an ESP32
static constexpr uint8_t MON_FEEDBACK_FORWARD = 0x02;  // Feedback empfangen vom ESP32
static constexpr uint8_t MON_STATUS_CHANGE    = 0x03;  // Online/Offline-Wechsel [1B: 1=online]

// ── Pakettypen: bridge_monitor → bridge_exe ───────────────────────────────────
static constexpr uint8_t MON_POLL_REQUEST     = 0x81;  // Status-Poll für Modul (can_id)
static constexpr uint8_t MON_LWT              = 0x82;  // Monitor-Heartbeat [4B timestamp LE]

// ── Pakettypen: bridge_exe → bridge_monitor (Verbindungscheck) ────────────────
static constexpr uint8_t MON_BRIDGE_PONG      = 0x04;  // Echo auf MON_LWT [4B echo_ts LE]
static constexpr uint8_t MON_RX_FRAME        = 0x05;  // CAN-Frame empfangen vom ESP32 (Eingangsmodul)

// ── Feedback-Pakettypen vom ESP32 (Byte 0 im Payload von MON_FEEDBACK_FORWARD) ──
static constexpr uint8_t FB_HEARTBEAT         = 0x01;  // [4B uptime LE in ms]
static constexpr uint8_t FB_STATUS_RESPONSE   = 0x04;  // [1B type=0x04][1B seq][4B ts_echo][Nutzdaten…]
static constexpr uint8_t FB_ERROR             = 0x03;  // [1B code][text…]

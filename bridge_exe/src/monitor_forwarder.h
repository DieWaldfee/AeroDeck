#pragma once
#include <winsock2.h>
#include <string>
#include <cstdint>

// ── Pakettypen: bridge_exe → bridge_monitor ───────────────────────────────────
// Format: [1B type][4B can_id/ts LE][payload]
static constexpr uint8_t MON_FRAME_FORWARD    = 0x01;  // CAN-Frame gesendet an ESP32
static constexpr uint8_t MON_FEEDBACK_FORWARD = 0x02;  // Feedback empfangen vom ESP32
static constexpr uint8_t MON_STATUS_CHANGE    = 0x03;  // Online/Offline-Wechsel [1B]
static constexpr uint8_t MON_BRIDGE_PONG      = 0x04;  // Echo auf MON_LWT [4B echo_ts LE]
static constexpr uint8_t MON_RX_FRAME        = 0x05;  // CAN-Frame empfangen vom ESP32 (Button-Modul)

// ── Pakettypen: bridge_monitor → bridge_exe ───────────────────────────────────
static constexpr uint8_t MON_POLL_REQUEST     = 0x81;  // Status-Poll für Modul
static constexpr uint8_t MON_LWT              = 0x82;  // Monitor-Heartbeat [4B timestamp LE]

// Sendet Kopien aller CAN-Frames und Feedbacks per UDP an bridge_monitor.
// Empfängt Poll-Kommandos vom Monitor auf cmd_port.
class MonitorForwarder {
public:
    // monitorIp/monitorPort: Ziel-UDP-Adresse des bridge_monitor
    // cmdPort: lokaler Port auf dem wir Kommandos empfangen
    bool open(const std::string& monitorIp, int monitorPort, int cmdPort);
    void close();
    bool isOpen() const { return m_sock != INVALID_SOCKET; }

    void forwardFrame(uint32_t canId, const uint8_t* frame, size_t len);
    void forwardRxFrame(uint32_t canId, const uint8_t* frame, size_t len);
    void forwardFeedback(uint32_t canId, const uint8_t* payload, size_t len);
    void sendStatusChange(uint32_t canId, bool online);

    // Antwortet auf MON_LWT — echo_ts ist der vom Monitor gesendete Timestamp
    void sendPong(uint32_t echoTs);

    // Nicht-blockierend. Gibt true zurück wenn ein Kommando empfangen wurde.
    bool tryReceiveCommand(uint8_t& type, uint32_t& canId);

private:
    void sendPacket(uint8_t type, uint32_t canId, const uint8_t* payload, size_t len);

    SOCKET      m_sock    = INVALID_SOCKET;  // Sende-Socket → monitor
    SOCKET      m_cmdSock = INVALID_SOCKET;  // Empfangs-Socket ← monitor
    sockaddr_in m_dest    = {};
};

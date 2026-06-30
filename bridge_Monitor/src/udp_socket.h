#pragma once
#include <winsock2.h>
#include <string>
#include <cstdint>

// Empfangs-Socket für Monitor-Pakete von bridge_exe.
// Sende-Socket für Kommandos an bridge_exe.
class UdpSocket {
public:
    // recvPort: lokaler Port auf dem wir Frames empfangen (monitor.port)
    // bridgeIp/cmdPort: Ziel-Adresse für Kommandos an bridge_exe
    bool open(int recvPort, const std::string& bridgeIp, int cmdPort);
    void close();
    bool isOpen() const { return m_recvSock != INVALID_SOCKET; }

    // Nicht-blockierend. Gibt true zurück wenn Paket empfangen.
    // Paketformat: [1B type][4B can_id LE][payload]
    bool tryReceive(uint8_t& type, uint32_t& canId, uint8_t* payload, size_t& payloadLen);

    // Sendet Kommando an bridge_exe: [1B type][4B can_id LE]
    void sendCommand(uint8_t type, uint32_t canId);

private:
    SOCKET      m_recvSock = INVALID_SOCKET;
    SOCKET      m_sendSock = INVALID_SOCKET;
    sockaddr_in m_dest     = {};
};

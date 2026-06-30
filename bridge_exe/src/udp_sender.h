#pragma once
#include <winsock2.h>
#include <string>
#include "transport.h"

// UDP-Implementierung von ITransport.
// Paketformat: [4 Byte CAN-ID LE][Payload]
// Sende-Socket: open()        → sendet an ESP32 udp_port
// Empfangs-Socket: openReceive() → empfängt von ESP32 auf receive_port
class UdpTransport : public ITransport {
public:
    bool open(const std::string& ip, int sendPort);
    bool openReceive(int recvPort);

    bool        isOpen()     const override { return m_sock != INVALID_SOCKET; }
    bool        send(uint32_t canId, const uint8_t* payload, size_t len) override;
    void        close()      override;
    std::string lastError()    const override { return m_lastError; }
    std::string logCategory() const override { return "UDP-ERROR"; }
    std::string logDevice()   const override { return m_lastDevice; }
    uint64_t    framesSent()  const override { return m_framesSent; }
    long long   lastSendMs()  const override { return m_lastSendMs; }

    bool tryReceive(uint32_t& canId, uint8_t* payload, size_t& payloadLen) override;

    // Aktuelles Sendeziel als "ip:port"-String (wird nach IP-Discovery aktualisiert).
    std::string destStr() const { return m_destStr; }

    // Setzt Sendeziel zurück auf 255.255.255.255 (gleicher Port).
    // Aufrufen wenn ESP32 offline geht — Bridge broadcastet dann wieder,
    // bis der ESP32 neu verbindet und seine IP per erstem Paket bekannt gibt.
    void resetToBroadcast();

private:
    SOCKET      m_sock       = INVALID_SOCKET;   // Sende-Socket
    SOCKET      m_recvSock   = INVALID_SOCKET;   // Empfangs-Socket
    sockaddr_in m_dest       = {};
    long long   m_lastSendMs = 0;
    uint64_t    m_framesSent = 0;
    std::string m_lastError;
    std::string m_lastDevice;
    std::string m_destStr;
};

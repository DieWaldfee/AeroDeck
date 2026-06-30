#include "udp_sender.h"
#include <ws2tcpip.h>
#include <chrono>
#include <cstring>
#include <algorithm>

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static const char* udpErrMsg(int err) {
    switch (err) {
        case WSAEADDRINUSE:    return "port already in use";
        case WSAECONNRESET:    return "connection reset by remote";
        case WSAENETUNREACH:   return "network unreachable";
        case WSAEHOSTUNREACH:  return "host unreachable";
        case WSAETIMEDOUT:     return "connection timed out";
        case WSAECONNREFUSED:  return "connection refused";
        default:               return "network error";
    }
}

bool UdpTransport::open(const std::string& ip, int port) {
    m_destStr = ip + ":" + std::to_string(port);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        m_lastError  = "Winsock initialization failed";
        m_lastDevice = "---";
        return false;
    }
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) {
        m_lastError  = "cannot create UDP send socket";
        m_lastDevice = m_destStr;
        return false;
    }
    u_long mode = 1;
    ioctlsocket(m_sock, FIONBIO, &mode);

    // Broadcast-Sends erlauben (nötig wenn udp_ip = 255.255.255.255).
    // Schadet nicht bei Unicast-Zielen.
    int bcast = 1;
    setsockopt(m_sock, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    m_dest.sin_family = AF_INET;
    m_dest.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, ip.c_str(), &m_dest.sin_addr);
    return true;
}

bool UdpTransport::openReceive(int recvPort) {
    // Gleicher Socket wie open() - bindet m_sock an INADDR_ANY:recvPort.
    // Ein Socket für Senden UND Empfangen: Ausgehende Pakete haben dann Port recvPort
    // als Absender. Windows Firewall erkennt eingehende Antworten als Reaktion auf
    // den eigenen Sendevorgang und lässt sie durch - ohne explizite Firewall-Regel.
    if (m_sock == INVALID_SOCKET) {
        m_lastError  = "cannot bind - open() must be called first";
        m_lastDevice = "0.0.0.0:" + std::to_string(recvPort);
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)recvPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        m_lastError  = std::string("cannot bind UDP socket - ") + udpErrMsg(err);
        m_lastDevice = "0.0.0.0:" + std::to_string(recvPort);
        return false;
    }
    m_recvSock = m_sock;
    return true;
}

void UdpTransport::close() {
    // m_recvSock == m_sock → nur einmal schließen
    m_recvSock = INVALID_SOCKET;
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        WSACleanup();
    }
}

bool UdpTransport::send(uint32_t canId, const uint8_t* payload, size_t len) {
    if (m_sock == INVALID_SOCKET) return false;

    // Paket: [4 Byte CAN-ID little-endian][Payload bis 64 Byte (CAN FD)]
    constexpr size_t MAX_PAYLOAD = 64;
    if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;

    uint8_t buf[4 + MAX_PAYLOAD];
    buf[0] = (uint8_t)(canId >>  0);
    buf[1] = (uint8_t)(canId >>  8);
    buf[2] = (uint8_t)(canId >> 16);
    buf[3] = (uint8_t)(canId >> 24);
    std::memcpy(buf + 4, payload, len);

    int total = (int)(4 + len);
    int result = sendto(m_sock, reinterpret_cast<const char*>(buf), total, 0,
                        reinterpret_cast<sockaddr*>(&m_dest), sizeof(m_dest));
    if (result == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            m_lastError  = std::string("send failed - ") + udpErrMsg(err);
            m_lastDevice = m_destStr;
        }
        return false;
    }

    m_lastError.clear();
    m_lastSendMs = nowMs();
    ++m_framesSent;
    return true;
}

void UdpTransport::resetToBroadcast() {
    m_dest.sin_addr.s_addr = INADDR_BROADCAST;
    m_destStr = "255.255.255.255:" + std::to_string(ntohs(m_dest.sin_port));
}

bool UdpTransport::tryReceive(uint32_t& canId, uint8_t* payload, size_t& payloadLen) {
    if (m_recvSock == INVALID_SOCKET) return false;

    // Puffer: 4B CAN-ID + max. 64B CAN-FD-Payload
    uint8_t     buf[4 + 64];
    sockaddr_in sender = {};
    int         slen   = (int)sizeof(sender);
    int result = recvfrom(m_recvSock, reinterpret_cast<char*>(buf), sizeof(buf),
                          0, reinterpret_cast<sockaddr*>(&sender), &slen);
    if (result == SOCKET_ERROR) return false;   // WSAEWOULDBLOCK = kein Paket, kein Fehler
    if (result < 5) return false;               // mind. 4B ID + 1B Payload

    // Bridge-seitige IP-Discovery: ESP32-Adresse aus dem ersten empfangenen Paket lernen.
    // Aktualisiert m_dest wenn ESP32-IP noch unbekannt oder per DHCP geändert.
    // 255.255.255.255 und 0.0.0.0 werden ignoriert — nur echte Unicast-IPs übernommen.
    const uint32_t srcIp = sender.sin_addr.s_addr;
    if (srcIp != 0 && srcIp != INADDR_BROADCAST && m_dest.sin_addr.s_addr != srcIp) {
        m_dest.sin_addr.s_addr = srcIp;
        char ipStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sender.sin_addr, ipStr, sizeof(ipStr));
        m_destStr = std::string(ipStr) + ":" + std::to_string(ntohs(m_dest.sin_port));
    }

    canId = (uint32_t)buf[0]
          | ((uint32_t)buf[1] <<  8)
          | ((uint32_t)buf[2] << 16)
          | ((uint32_t)buf[3] << 24);

    payloadLen = (size_t)(result - 4);
    std::memcpy(payload, buf + 4, payloadLen);
    return true;
}

#include "udp_socket.h"
#include <ws2tcpip.h>
#include <cstring>

bool UdpSocket::open(int recvPort, const std::string& bridgeIp, int cmdPort) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    // Empfangs-Socket
    m_recvSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_recvSock == INVALID_SOCKET) return false;
    u_long mode = 1;
    ioctlsocket(m_recvSock, FIONBIO, &mode);
    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)recvPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_recvSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_recvSock);
        m_recvSock = INVALID_SOCKET;
        return false;
    }

    // Sende-Socket für Kommandos an bridge_exe
    m_sendSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sendSock == INVALID_SOCKET) return false;
    ioctlsocket(m_sendSock, FIONBIO, &mode);
    m_dest.sin_family = AF_INET;
    m_dest.sin_port   = htons((u_short)cmdPort);
    inet_pton(AF_INET, bridgeIp.c_str(), &m_dest.sin_addr);

    return true;
}

void UdpSocket::close() {
    if (m_recvSock != INVALID_SOCKET) { closesocket(m_recvSock); m_recvSock = INVALID_SOCKET; }
    if (m_sendSock != INVALID_SOCKET) { closesocket(m_sendSock); m_sendSock = INVALID_SOCKET; }
    WSACleanup();
}

bool UdpSocket::tryReceive(uint8_t& type, uint32_t& canId,
                            uint8_t* payload, size_t& payloadLen) {
    if (m_recvSock == INVALID_SOCKET) return false;
    // Format: [1B type][4B can_id LE][payload ≤ 64B] → max 69B
    uint8_t buf[1 + 4 + 64];
    int result = recvfrom(m_recvSock, reinterpret_cast<char*>(buf), sizeof(buf),
                          0, nullptr, nullptr);
    if (result < 5) return false;
    type  = buf[0];
    canId = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8)
          | ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
    payloadLen = (size_t)(result - 5);
    if (payloadLen > 0) std::memcpy(payload, buf + 5, payloadLen);
    return true;
}

void UdpSocket::sendCommand(uint8_t type, uint32_t canId) {
    if (m_sendSock == INVALID_SOCKET) return;
    uint8_t buf[5];
    buf[0] = type;
    buf[1] = (uint8_t)(canId >>  0);
    buf[2] = (uint8_t)(canId >>  8);
    buf[3] = (uint8_t)(canId >> 16);
    buf[4] = (uint8_t)(canId >> 24);
    sendto(m_sendSock, reinterpret_cast<const char*>(buf), 5, 0,
           reinterpret_cast<sockaddr*>(&m_dest), sizeof(m_dest));
}

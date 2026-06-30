#include "monitor_forwarder.h"
#include <ws2tcpip.h>
#include <cstring>

bool MonitorForwarder::open(const std::string& ip, int port, int cmdPort) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_sock == INVALID_SOCKET) return false;
    u_long mode = 1;
    ioctlsocket(m_sock, FIONBIO, &mode);
    m_dest.sin_family = AF_INET;
    m_dest.sin_port   = htons((u_short)port);
    inet_pton(AF_INET, ip.c_str(), &m_dest.sin_addr);

    m_cmdSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_cmdSock == INVALID_SOCKET) return false;
    ioctlsocket(m_cmdSock, FIONBIO, &mode);
    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)cmdPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(m_cmdSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_cmdSock);
        m_cmdSock = INVALID_SOCKET;
    }
    return true;
}

void MonitorForwarder::close() {
    if (m_cmdSock != INVALID_SOCKET) { closesocket(m_cmdSock); m_cmdSock = INVALID_SOCKET; }
    if (m_sock    != INVALID_SOCKET) { closesocket(m_sock);    m_sock    = INVALID_SOCKET; WSACleanup(); }
}

void MonitorForwarder::sendPacket(uint8_t type, uint32_t canId,
                                   const uint8_t* payload, size_t len) {
    if (m_sock == INVALID_SOCKET) return;
    if (len > 64) len = 64;
    uint8_t buf[1 + 4 + 64];
    buf[0] = type;
    buf[1] = (uint8_t)(canId >>  0);
    buf[2] = (uint8_t)(canId >>  8);
    buf[3] = (uint8_t)(canId >> 16);
    buf[4] = (uint8_t)(canId >> 24);
    if (len > 0) std::memcpy(buf + 5, payload, len);
    sendto(m_sock, reinterpret_cast<const char*>(buf), (int)(5 + len), 0,
           reinterpret_cast<sockaddr*>(&m_dest), sizeof(m_dest));
}

void MonitorForwarder::forwardFrame(uint32_t canId, const uint8_t* frame, size_t len) {
    sendPacket(MON_FRAME_FORWARD, canId, frame, len);
}

void MonitorForwarder::forwardRxFrame(uint32_t canId, const uint8_t* frame, size_t len) {
    sendPacket(MON_RX_FRAME, canId, frame, len);
}

void MonitorForwarder::forwardFeedback(uint32_t canId, const uint8_t* payload, size_t len) {
    sendPacket(MON_FEEDBACK_FORWARD, canId, payload, len);
}

void MonitorForwarder::sendStatusChange(uint32_t canId, bool online) {
    uint8_t b = online ? 1u : 0u;
    sendPacket(MON_STATUS_CHANGE, canId, &b, 1);
}

void MonitorForwarder::sendPong(uint32_t echoTs) {
    // echoTs ist der vom Monitor gesendete Timestamp → wird 1:1 zurückgesendet
    // Monitor berechnet RTT = nowMs() - echoTs
    sendPacket(MON_BRIDGE_PONG, echoTs, nullptr, 0);
}

bool MonitorForwarder::tryReceiveCommand(uint8_t& type, uint32_t& canId) {
    if (m_cmdSock == INVALID_SOCKET) return false;
    uint8_t buf[16];
    int result = recvfrom(m_cmdSock, reinterpret_cast<char*>(buf), sizeof(buf),
                          0, nullptr, nullptr);
    if (result < 5) return false;
    type  = buf[0];
    canId = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8)
          | ((uint32_t)buf[3] << 16) | ((uint32_t)buf[4] << 24);
    return true;
}

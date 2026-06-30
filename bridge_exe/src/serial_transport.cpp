#include "serial_transport.h"
#include <chrono>
#include <cstring>
#include <cstdio>

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Fehler die anzeigen dass der COM-Port nicht mehr vorhanden ist
static const char* serialOpenErrMsg(DWORD err) {
    switch (err) {
        case ERROR_ACCESS_DENIED:          return "port already in use by another process";
        case ERROR_FILE_NOT_FOUND:         return "port not found - device not connected";
        case ERROR_DEVICE_NOT_CONNECTED:   return "device not connected";
        default:                           return "cannot open port";
    }
}

static const char* serialIoErrMsg(DWORD err) {
    switch (err) {
        case ERROR_ACCESS_DENIED:          return "port no longer accessible";
        case ERROR_GEN_FAILURE:            return "USB connection failure";
        case ERROR_DEVICE_NOT_CONNECTED:   return "device disconnected";
        case ERROR_BROKEN_PIPE:            return "connection broken";
        default:                           return "I/O error";
    }
}

static bool isFatalSerialError(DWORD err) {
    return err == ERROR_DEVICE_NOT_CONNECTED   // 1167 - Gerät getrennt
        || err == ERROR_FILE_NOT_FOUND         //    2 - Port verschwunden
        || err == ERROR_GEN_FAILURE            //   31 - USB-Verbindungsabbruch
        || err == ERROR_ACCESS_DENIED          //    5 - Port nicht mehr zugreifbar
        || err == ERROR_BROKEN_PIPE;           //  109 - Verbindung unterbrochen
}

static constexpr uint8_t  MAGIC_HI  = 0xAA;
static constexpr uint8_t  MAGIC_LO  = 0x55;
static constexpr uint16_t FRAME_MIN = 4 + 1;    // CAN-ID + 1B Payload
static constexpr uint16_t FRAME_MAX = 4 + 64;   // = 68

// ── open ─────────────────────────────────────────────────────────────────────
bool SerialTransport::open(const std::string& portName, int baud) {
    m_portName = portName;

    // Windows-API: "\\\\.\\COM3" auch für COM10+ nötig
    std::string fullPath = "\\\\.\\" + portName;

    m_handle = CreateFileA(
        fullPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,           // kein Sharing
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (m_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        m_lastError = std::string("cannot open - ") + serialOpenErrMsg(err);
        return false;
    }

    // Serielle Parameter (USB CDC ignoriert Baudrate, aber DCB muss gesetzt sein)
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_handle, &dcb)) {
        m_lastError = "cannot read port state";
        close();
        return false;
    }
    dcb.BaudRate = (DWORD)baud;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;   // DTR HIGH → signalisiert CH343: Host aktiv, UART weiterleiten
    if (!SetCommState(m_handle, &dcb)) {
        m_lastError = "cannot configure port";
        close();
        return false;
    }
    // Kurz warten: CH343/CP2102N benötigt ~100 ms bis UART-Signale stabil sind.
    // Verhindert dass sendInitAll() läuft bevor der ESP32 bereit ist.
    Sleep(200);

    // Timeouts: RX sofort zurück (MAXDWORD + 0 + 0 = non-blocking)
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 200;  // 200 ms TX-Timeout
    if (!SetCommTimeouts(m_handle, &to)) {
        m_lastError = "cannot set port timeouts";
        close();
        return false;
    }

    // RX-State-Machine zurücksetzen
    m_rxState = WAIT_MAGIC1;
    m_lastError.clear();
    return true;
}

// ── close ─────────────────────────────────────────────────────────────────────
void SerialTransport::close() {
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

// ── send ──────────────────────────────────────────────────────────────────────
bool SerialTransport::send(uint32_t canId, const uint8_t* payload, size_t len) {
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    if (len > 64) len = 64;

    uint16_t frameLen = (uint16_t)(4 + len);

    // Frame aufbauen: [0xAA][0x55][len_lo][len_hi][CAN-ID 4B][Payload][XOR]
    uint8_t buf[2 + 2 + 4 + 64 + 1];
    int pos = 0;
    buf[pos++] = MAGIC_HI;
    buf[pos++] = MAGIC_LO;
    buf[pos++] = (uint8_t)(frameLen & 0xFF);
    buf[pos++] = (uint8_t)(frameLen >> 8);
    buf[pos++] = (uint8_t)(canId >>  0);
    buf[pos++] = (uint8_t)(canId >>  8);
    buf[pos++] = (uint8_t)(canId >> 16);
    buf[pos++] = (uint8_t)(canId >> 24);
    memcpy(buf + pos, payload, len);
    pos += (int)len;

    uint8_t xorVal = 0;
    for (int i = 4; i < pos; ++i) xorVal ^= buf[i];  // XOR über CAN-ID + Payload
    buf[pos++] = xorVal;

    DWORD written;
    if (!WriteFile(m_handle, buf, (DWORD)pos, &written, nullptr)
        || written != (DWORD)pos) {
        DWORD err = GetLastError();
        m_lastError = std::string("write failed - ") + serialIoErrMsg(err);
        if (isFatalSerialError(err)) close();  // isOpen() → false → Fallback auf UDP
        return false;
    }

    m_lastError.clear();
    m_lastSendMs = nowMs();
    ++m_framesSent;
    return true;
}

// ── sendText ──────────────────────────────────────────────────────────────────
bool SerialTransport::sendText(const char* text) {
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    const DWORD len = (DWORD)strlen(text);
    if (len == 0) return true;
    DWORD written = 0;
    return WriteFile(m_handle, text, len, &written, nullptr) && written == len;
}

// ── tryReceive ────────────────────────────────────────────────────────────────
// Nicht-blockierend: verarbeitet alle verfügbaren Bytes aus dem RX-Puffer.
// Gibt true zurück sobald ein vollständiger, gültiger Frame dekodiert wurde.
bool SerialTransport::tryReceive(uint32_t& canId, uint8_t* payload, size_t& payloadLen) {
    if (m_handle == INVALID_HANDLE_VALUE) return false;

    for (;;) {
        uint8_t b;
        DWORD   bytesRead = 0;
        BOOL    ok = ReadFile(m_handle, &b, 1, &bytesRead, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (isFatalSerialError(err)) {
                m_lastError = std::string("read failed - ") + serialIoErrMsg(err);
                close();  // isOpen() → false → Fallback auf UDP
            }
            return false;
        }
        if (bytesRead == 0) return false;  // kein Byte verfügbar (Timeout)

        switch (m_rxState) {
            case WAIT_MAGIC1:
                if (b == MAGIC_HI) m_rxState = WAIT_MAGIC2;
                break;

            case WAIT_MAGIC2:
                m_rxState = (b == MAGIC_LO) ? READ_LEN_LO : WAIT_MAGIC1;
                break;

            case READ_LEN_LO:
                m_rxLen   = b;
                m_rxState = READ_LEN_HI;
                break;

            case READ_LEN_HI:
                m_rxLen |= ((uint16_t)b << 8);
                if (m_rxLen < FRAME_MIN || m_rxLen > FRAME_MAX) {
                    ++m_frameLenErrors;
                    m_rxState = WAIT_MAGIC1;
                    break;
                }
                m_rxCount = 0;
                m_rxXor   = 0;
                m_rxState = READ_PAYLOAD;
                break;

            case READ_PAYLOAD:
                m_rxBuf[m_rxCount++] = b;
                m_rxXor ^= b;
                if (m_rxCount >= m_rxLen) m_rxState = VERIFY_XOR;
                break;

            case VERIFY_XOR:
                m_rxState = WAIT_MAGIC1;
                if (b != m_rxXor) { ++m_checksumErrors; break; }   // Prüfsummenfehler → verwerfen
                // Gültiger Frame — CAN-ID und Payload extrahieren
                canId = (uint32_t)m_rxBuf[0]
                      | ((uint32_t)m_rxBuf[1] <<  8)
                      | ((uint32_t)m_rxBuf[2] << 16)
                      | ((uint32_t)m_rxBuf[3] << 24);
                payloadLen = m_rxLen - 4;
                memcpy(payload, m_rxBuf + 4, payloadLen);
                m_lastError.clear();
                return true;
        }
    }
}

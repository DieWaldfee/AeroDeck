#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include "transport.h"

// ══════════════════════════════════════════════════════════════════════════════
// SerialTransport — implementiert ITransport über einen Windows-COM-Port
//
// Framing-Protokoll (identisch zur ESP32-Seite):
//   [0xAA][0x55][len_lo][len_hi][CAN-ID 4B LE][Payload ≤64B][XOR]
//   len = 4 (CAN-ID) + payload_len
//   XOR = XOR aller Bytes von CAN-ID bis Payload-Ende
//
// Konfiguration in bridge.ini:
//   usb_port = COM3
//
// Nutzung in main.cpp: identisch zu UdpTransport — gleiche ITransport-Schnittstelle.
// ══════════════════════════════════════════════════════════════════════════════
class SerialTransport : public ITransport {
public:
    // Öffnet den COM-Port (z.B. "COM3", "COM10") mit konfigurierbarer Baudrate.
    // Konfiguriert: 8N1, DTR-on, TX-Timeout 200 ms, RX nicht-blockierend.
    // Gibt true zurück bei Erfolg.
    bool open(const std::string& portName, int baud = 921600);

    void        close()   override;
    bool        isOpen()  const override { return m_handle != INVALID_HANDLE_VALUE; }
    bool        send(uint32_t canId, const uint8_t* payload, size_t len) override;
    bool        tryReceive(uint32_t& canId, uint8_t* payload, size_t& payloadLen) override;
    std::string lastError()    const override { return m_lastError; }
    std::string logCategory() const override { return "USB-PORT"; }
    std::string logDevice()   const override { return m_portName; }
    uint64_t    framesSent()  const override { return m_framesSent; }
    long long   lastSendMs()  const override { return m_lastSendMs; }

    uint32_t takeChecksumErrors() { uint32_t n = m_checksumErrors; m_checksumErrors = 0; return n; }
    uint32_t takeFrameLenErrors()  { uint32_t n = m_frameLenErrors;  m_frameLenErrors  = 0; return n; }

    bool sendText(const char* text);  // Klartext ohne Framing (für Serial-Konsole, z.B. "RESET\r\n")

private:
    HANDLE      m_handle    = INVALID_HANDLE_VALUE;
    uint64_t    m_framesSent = 0;
    long long   m_lastSendMs = 0;
    std::string m_lastError;
    std::string m_portName;

    // RX-State-Machine (Framing-Dekodierung, byte-by-byte)
    enum RxState : uint8_t {
        WAIT_MAGIC1, WAIT_MAGIC2,
        READ_LEN_LO, READ_LEN_HI,
        READ_PAYLOAD,
        VERIFY_XOR
    };
    RxState  m_rxState  = WAIT_MAGIC1;
    uint16_t m_rxLen    = 0;
    uint16_t m_rxCount  = 0;
    uint8_t  m_rxXor    = 0;
    uint8_t  m_rxBuf[4 + 64] = {};  // CAN-ID + Payload Puffer
    uint32_t m_checksumErrors = 0;
    uint32_t m_frameLenErrors  = 0;
};

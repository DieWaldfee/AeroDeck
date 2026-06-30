#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

// Abstrakte Transportschicht.
// UDP-Pakete enthalten: [4 Byte CAN-ID little-endian][N Byte Payload]
// Der Kommunikations-ESP32-S3 liest CAN-ID und Payload und sendet den Frame auf den CAN-Bus.
class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool        isOpen()                                                  const = 0;
    virtual bool        send(uint32_t canId, const uint8_t* payload, size_t len)       = 0;
    virtual void        close()                                                         = 0;
    virtual std::string lastError()    const = 0;
    virtual std::string logCategory() const = 0;
    virtual std::string logDevice()   const = 0;
    virtual uint64_t    framesSent()  const = 0;
    virtual long long   lastSendMs()  const = 0;

    // Nicht-blockierend. Gibt true zurück wenn ein Paket ankam.
    // Paketformat: [4B CAN-ID LE][Payload] — identisch zur Senderichtung.
    virtual bool tryReceive(uint32_t& canId, uint8_t* payload, size_t& payloadLen) = 0;
};

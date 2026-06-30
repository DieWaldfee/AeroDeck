#include "usb_transport.h"
#include "bridge_types.h"
#include "config.h"
#include <Arduino.h>
#include <esp_log.h>
#include <cstring>

static const char* TAG = "USB";

// Framing-Konstanten
static constexpr uint8_t  MAGIC_HI  = 0xAA;
static constexpr uint8_t  MAGIC_LO  = 0x55;
static constexpr uint16_t FRAME_MIN = UDP_HEADER_BYTES + 1;   // CAN-ID + 1 Byte Payload
static constexpr uint16_t FRAME_MAX = UDP_HEADER_BYTES + MAX_CANFD_PAYLOAD;  // = 68

// ══════════════════════════════════════════════════════════════════════════════
// Task: USB-RX  (USB CDC → Framing-Decoder → g_udpToCanQueue)
// Prio: TASK_PRIO_NET_RX | Core 1
//
// State-Machine zum Dekodieren des Framing-Protokolls.
// Nicht-blockierend: 1 ms Pause wenn kein Byte anliegt.
// Aktiv auch wenn kein Host verbunden — dann liefert Serial.read() einfach -1.
// ══════════════════════════════════════════════════════════════════════════════
void taskUsbRx(void* /*param*/) {
    enum State : uint8_t {
        WAIT_MAGIC1, WAIT_MAGIC2,
        READ_LEN_LO, READ_LEN_HI,
        READ_PAYLOAD,
        VERIFY_XOR
    };

    State    state   = WAIT_MAGIC1;
    uint16_t rxLen   = 0;   // erwartete Nutzbytes (CAN-ID + Payload)
    uint16_t rxCount = 0;   // bereits empfangene Nutzbytes
    uint8_t  xorAcc  = 0;
    uint8_t  rxBuf[FRAME_MAX];  // CAN-ID + Payload Puffer

    for (;;) {
        int raw = Serial0.read();
        if (raw < 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        uint8_t b = (uint8_t)raw;

        switch (state) {
            case WAIT_MAGIC1:
                if (b == MAGIC_HI) {
                    state = WAIT_MAGIC2;
                } else {
                    // Kein Protokoll-Frame → Byte zur Serial-Konsole weiterleiten
                    xQueueSend(g_consoleRxQueue, &b, 0);
                }
                break;

            case WAIT_MAGIC2:
                state = (b == MAGIC_LO) ? READ_LEN_LO : WAIT_MAGIC1;
                break;

            case READ_LEN_LO:
                rxLen = b;
                state = READ_LEN_HI;
                break;

            case READ_LEN_HI:
                rxLen |= ((uint16_t)b << 8);
                if (rxLen < FRAME_MIN || rxLen > FRAME_MAX) {
                    ESP_LOGW(TAG, "USB-RX: ungueltige Frame-Laenge %u, reset", rxLen);
                    state = WAIT_MAGIC1;
                    break;
                }
                rxCount = 0;
                xorAcc  = 0;
                state   = READ_PAYLOAD;
                break;

            case READ_PAYLOAD:
                rxBuf[rxCount++] = b;
                xorAcc ^= b;
                if (rxCount >= rxLen) state = VERIFY_XOR;
                break;

            case VERIFY_XOR:
                state = WAIT_MAGIC1;
                if (b != xorAcc) {
                    ESP_LOGW(TAG, "USB-RX: XOR-Fehler (erwartet 0x%02X, bekommen 0x%02X)",
                             xorAcc, b);
                    break;
                }
                // Gültiger Frame → Timestamp aktualisieren + in Queue
                {
                    g_lastUartRxTick.store(xTaskGetTickCount());
                    RawPacket pkt;
                    pkt.len = rxLen;   // 4 (CAN-ID) + payload
                    memcpy(pkt.buf, rxBuf, rxLen);
                    if (xQueueSend(g_udpToCanQueue, &pkt, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "USB-RX: Queue voll, Frame 0x%08lX verworfen",
                                 (unsigned long)readCanId(pkt.buf));
                    }
                }
                break;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: USB-TX  (g_canToUsbQueue → Framing-Encoder → USB CDC)
// Prio: TASK_PRIO_NET_TX | Core 1
//
// Blockiert auf Queue, sendet dann encoded Frame.
// Verwirft Frame wenn kein USB-Host verbunden (power-only Kabel).
// Setzt g_usbTxActive als Statusanzeige (TX-Aktivität, kein Host-Verbindungsindikator).
// ══════════════════════════════════════════════════════════════════════════════
void taskUsbTx(void* /*param*/) {
    // Sendepuffer: Magic(2) + Len(2) + CAN-ID+Payload(≤68) + XOR(1)
    uint8_t txBuf[2 + 2 + FRAME_MAX + 1];

    for (;;) {
        RawPacket pkt;
        if (xQueueReceive(g_canToUsbQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;

        // UART-Brücken-Port: keine Connection-Detection (anders als USB CDC).
        // bridge.exe öffnet einfach den COM-Port — dann laufen die Daten.
        // Frames bei geschlossenem Port landen im UART-TX-FIFO und gehen verloren.
        g_usbTxActive.store(true);

        if (pkt.len < UDP_HEADER_BYTES + 1 || pkt.len > FRAME_MAX) continue;

        // ── Frame kodieren ────────────────────────────────────────────────────
        uint16_t frameLen = pkt.len;   // = 4 + payload_len
        uint8_t  xorVal   = 0;
        for (uint16_t i = 0; i < frameLen; ++i) xorVal ^= pkt.buf[i];

        int pos = 0;
        txBuf[pos++] = MAGIC_HI;
        txBuf[pos++] = MAGIC_LO;
        txBuf[pos++] = (uint8_t)(frameLen & 0xFF);
        txBuf[pos++] = (uint8_t)(frameLen >> 8);
        memcpy(txBuf + pos, pkt.buf, frameLen);
        pos += frameLen;
        txBuf[pos++] = xorVal;

        // Schreiben — blockiert kurz wenn TX-FIFO voll (921600 Baud)
        Serial0.write(txBuf, (size_t)pos);
    }
}

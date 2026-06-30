#pragma once

// ══════════════════════════════════════════════════════════════════════════════
// USB-CDC-Transport (ESP32-S3 native USB, GPIO19/20)
//
// Framing-Protokoll (Bytestream → Pakete):
//
//   Byte 0:    0xAA   Magic High
//   Byte 1:    0x55   Magic Low
//   Byte 2-3:  len    uint16 LE  = 4 (CAN-ID) + payload_len
//   Byte 4-7:  CAN-ID uint32 LE  (identisch zum UDP-Format)
//   Byte 8..N: Payload ≤ 64 Byte
//   Byte N+1:  XOR    Byte 4..N  (CAN-ID + Payload, Einzelbyte-Prüfsumme)
//
// Overhead: 5 Byte pro Frame (Magic 2 + Len 2 + XOR 1)
// Max. Frame: 5 + 4 + 64 = 73 Byte
// ══════════════════════════════════════════════════════════════════════════════

// Empfang: UART-Brücken-Port → Framing-Decoder → g_udpToCanQueue
void taskUsbRx(void* param);

// Senden: g_canToUsbQueue → Framing-Encoder → UART-Brücken-Port → bridge.exe
void taskUsbTx(void* param);

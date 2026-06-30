#pragma once
#include <cstdint>
#include <cstddef>

// ══════════════════════════════════════════════════════════════════════════════
// Konfiguration bridge_ESP32
// ══════════════════════════════════════════════════════════════════════════════

// ── Netzwerk ──────────────────────────────────────────────────────────────────
constexpr uint16_t UDP_LISTEN_PORT = 4210;
constexpr uint16_t UDP_RETURN_PORT = 4211;

// ══════════════════════════════════════════════════════════════════════════════
// CAN-FD-Module (MCP2518FD über SPI)
//
// ESP32-S3 hat zwei unabhängige User-SPI-Busse:
//   SPI2  →  FSPI  →  Bus-ID 0  (Standard, IOMUX-Pins GPIO11/12/13 = bis 80 MHz)
//   SPI3  →  HSPI  →  Bus-ID 1  (frei wählbare Pins über GPIO-Matrix = bis 40 MHz)
//
// Jedes Modul bekommt seinen eigenen SPI-Bus → echter Parallelbetrieb.
// TaskCanTx an Modul 1 und TaskCanRx von Modul 2 laufen gleichzeitig.
//
// Anzahl aktiver Module: 1 oder 2
// ══════════════════════════════════════════════════════════════════════════════
constexpr uint8_t CAN_MODULE_COUNT = 1;   // ← 1 oder 2

// Konfigurationsstruktur pro CAN-FD-Modul
struct CanModuleCfg {
    const char* name;       // Bezeichner für Logging (z.B. "CAN1")
    int  spiBusId;          // SPI-Bus: 0 = FSPI (SPI2), 1 = HSPI (SPI3)
    int  sckPin;            // SPI SCK  → MCP2518FD SCK
    int  misoPin;           // SPI MISO ← MCP2518FD SDO
    int  mosiPin;           // SPI MOSI → MCP2518FD SDI
    int  csPin;             // SPI CS   → MCP2518FD nCS
    int  intPin;            // MCP2518FD INT ← ESP32 GPIO  (-1 = Polling, kein IRQ)
    uint32_t idMin;         // Untere CAN-ID-Grenze (einschließlich)
    uint32_t idMax;         // Obere  CAN-ID-Grenze (einschließlich)
};

// ── Pin-Zuweisung und ID-Bereiche ─────────────────────────────────────────────
//
// Modul 1 — SPI2 (FSPI, Bus-ID 0):
//   IOMUX-Pins GPIO11/12/13 → maximale SPI-Geschwindigkeit (bis 80 MHz)
//   Empfohlene CS/INT-Pins: GPIO10 / GPIO9
//
// Modul 2 — SPI3 (HSPI, Bus-ID 1):
//   GPIO-Matrix → beliebige freie Pins bis 40 MHz
//   Sichere GPIOs: 1, 2, 4–18, 21–25, 38–42
//   Reserviert/vermeiden: 0, 3, 19, 20, 26–37, 43–46
//
// ID-Bereiche dürfen sich NICHT überlappen.
// CAN-IDs ohne zugeordneten Bereich werden verworfen (Log-Warnung).
//
// Standard 11-Bit CAN: 0x000–0x7FF (2048 IDs)
//
//                name    busId  SCK   MISO  MOSI  CS   INT   idMin   idMax
constexpr CanModuleCfg CAN_MODULE_CFG[2] = {
    { "CAN1",       0,     12,    13,   11,   10,   9,  0x000, 0x3FF },
    { "CAN2",       0,      6,     8,    5,    4,   7,  0x400, 0x7FF },
};

// ── MCP2518FD Quarzfrequenz ───────────────────────────────────────────────────
// Gilt für beide Module (typisch identisch bestückt).
// Muss mit dem aufgelöteten Quarz übereinstimmen!
//   ACAN2517FDSettings::OSC_4MHz
//   ACAN2517FDSettings::OSC_20MHz
//   ACAN2517FDSettings::OSC_40MHz
#define CAN_OSC_FREQ ACAN2517FDSettings::OSC_40MHz
constexpr uint32_t CAN_OSC_MHZ = 40;   // Quarzfrequenz in MHz — muss mit CAN_OSC_FREQ übereinstimmen

// ── CAN-FD Bitraten ───────────────────────────────────────────────────────────
// Alle Busteilnehmer MÜSSEN identische Arbitrations-Rate haben (hier + horizon/src/main.cpp).
// TODO Produktion: 500UL * 1000UL Arb + x4 (2 Mbit/s Data) → TDC konfigurieren + verdrillte Leitung.
constexpr uint32_t CAN_ARB_BPS = 125UL * 1000UL;
// Daten-Phase-Faktor (BRS = Bit Rate Switch):
//   DataBitRateFactor::x1 →  Data-Phase = Arb-Phase (125 kbit/s, kein Geschwindigkeitsgewinn)
//   DataBitRateFactor::x4 →  Data-Phase = 4× Arb-Phase (500 kbit/s Arb → 2 Mbit/s Data)
// Bei x1 wird in can_bridge.cpp bewusst CANFD_NO_BIT_RATE_SWITCH (BRS=0) gesendet.
// Erst bei echtem Produktions-Setup (500 kbit/s Arb + x4) lohnt sich BRS=1 — dann TDC
// konfigurieren und auf BRS=1 in can_bridge.cpp umstellen.
#define CAN_DATA_RATE_FACTOR DataBitRateFactor::x1

// ── Paketgrenzen ──────────────────────────────────────────────────────────────
constexpr size_t UDP_HEADER_BYTES  = 4;
constexpr size_t MAX_CANFD_PAYLOAD = 64;
constexpr size_t MAX_UDP_PACKET    = UDP_HEADER_BYTES + MAX_CANFD_PAYLOAD;
constexpr size_t MIN_UDP_PACKET    = UDP_HEADER_BYTES + 1;

// ── FreeRTOS ──────────────────────────────────────────────────────────────────
constexpr uint8_t TASK_PRIO_NET_RX = 5;
constexpr uint8_t TASK_PRIO_NET_TX = 5;
constexpr uint8_t TASK_PRIO_CAN_TX = 5;
constexpr uint8_t TASK_PRIO_CAN_RX = 6;   // höchste: CAN-Timing

constexpr uint32_t    TASK_STACK_NET   = 8192;
constexpr uint32_t    TASK_STACK_CAN   = 4096;
constexpr uint8_t     QUEUE_DEPTH      = 64;

constexpr uint32_t CAN_POLL_DELAY_MS    = 1;
constexpr uint32_t CAN_MUTEX_TIMEOUT_MS = 10;

constexpr uint8_t WIFI_RETRY_COUNT = 40;   // × 500 ms = 20 s max.

// ── HID Joystick + Mapping-Protokoll ─────────────────────────────────────────
//
// CAN-ID-Bereich der Joystick-Eingangsmodule.
// Frames in diesem Bereich werden ZUSÄTZLICH an taskHidDecoder weitergeleitet.
// Darf sich nicht mit Modul-IDs (0x200 ff.) überlappen.
constexpr uint32_t JOY_CAN_ID_MIN = 0x600;
constexpr uint32_t JOY_CAN_ID_MAX = 0x6FF;

// Baudrate für den UART-Brücken-Port (GPIO43/44 → CP2102N → bridge.exe).
// bridge.ini: usb_baud = 921600  (muss übereinstimmen)
constexpr uint32_t JOY_UART_BAUD  = 921600;

// FreeRTOS-Parameter für den HID-Decoder-Task
constexpr uint8_t  TASK_PRIO_HID_DEC = 3;    // zwischen WiFiMgr(2) und CAN(5/6)
constexpr uint32_t TASK_STACK_HID    = 4096;

// CAN-ID für Mapping-Kommandos von bridge.exe → ESP32
// Frames mit dieser ID werden in taskCanTx abgefangen und NICHT auf den CAN-Bus gesendet.
// Muss mit HID_MAP_CAN_ID in bridge_exe/src/config.h übereinstimmen.
constexpr uint32_t HID_MAP_CAN_ID    = 0x7F0;

// CAN-ID für Mapping-Empfangsbestätigungen ESP32 → bridge.exe
// ESP32 sendet nach jedem verarbeiteten Mapping-Subtyp einen ACK-Frame auf dieser ID.
// Muss mit HID_MAP_ACK_CAN_ID in bridge_exe/src/config.h übereinstimmen.
constexpr uint32_t HID_MAP_ACK_CAN_ID = 0x7F1;

// CAN-ID für den ESP32-eigenen Heartbeat (bridge_ESP32 → bridge.exe).
// Hardcoded — keine bridge.ini-Entsprechung. Systemweit reserviert:
// darf NICHT als can_tx_id oder can_rx_id irgendeines Moduls genutzt werden.
// bridge.exe sendet 0xFD MODULE_CONFIG an diese ID um das HB-Intervall zu konfigurieren.
constexpr uint32_t ESP32_HB_CAN_ID   = 0x01;

// CAN-ID für Mapping-Anforderungen ESP32 → bridge.exe (HID_MAP_REQ_CAN_ID)
// ESP32 sendet nach jedem Boot REQUEST-Frames auf dieser ID bis bridge.exe antwortet.
// Muss mit HID_MAP_REQ_CAN_ID in bridge_exe/src/config.h übereinstimmen.
constexpr uint32_t HID_MAP_REQ_CAN_ID = 0x7F2;

// Wiederholungsintervall für HID-Mapping-Anforderungen (ms).
// Hardcoded im ESP32-Firmware — nicht per bridge.ini konfigurierbar.
// ESP32 sendet alle MAP_REQUEST_INTERVAL_MS einen REQUEST bis COMMIT empfangen.
constexpr uint32_t MAP_REQUEST_INTERVAL_MS = 3000;

// ── Compile-Zeit-Prüfungen ────────────────────────────────────────────────────
static_assert(CAN_MODULE_COUNT >= 1 && CAN_MODULE_COUNT <= 2,
              "CAN_MODULE_COUNT muss 1 oder 2 sein");
static_assert(CAN_MODULE_CFG[0].idMax >= CAN_MODULE_CFG[0].idMin,
              "CAN_MODULE_CFG[0]: idMax muss >= idMin sein");
static_assert(CAN_MODULE_CFG[1].idMax >= CAN_MODULE_CFG[1].idMin,
              "CAN_MODULE_CFG[1]: idMax muss >= idMin sein");

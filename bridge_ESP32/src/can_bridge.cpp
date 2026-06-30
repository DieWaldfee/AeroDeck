// ══════════════════════════════════════════════════════════════════════════════
// can_bridge.cpp — MCP2518FD CAN-FD Treiber
// Nur aktiv bei -DCAN_HW_ENABLED. Sonst übernimmt can_simulator.cpp.
// ══════════════════════════════════════════════════════════════════════════════
#ifdef CAN_HW_ENABLED

#include "can_bridge.h"
#include "config.h"
#include "config_manager.h"
#include "bridge_types.h"
#include "esp32_heartbeat.h"

#include <ACAN2517FD.h>
#include <SPI.h>
#include <esp_log.h>

static const char* TAG = "CAN";

// ── SPI-Busse (ein Bus pro Modul) ─────────────────────────────────────────────
static SPIClass g_spi[2] = {
    SPIClass(static_cast<uint8_t>(CAN_MODULE_CFG[0].spiBusId)),
    SPIClass(static_cast<uint8_t>(CAN_MODULE_CFG[1].spiBusId)),
};

// ── MCP2518FD-Instanzen ───────────────────────────────────────────────────────
static ACAN2517FD g_canDev[2] = {
    ACAN2517FD(static_cast<uint8_t>(CAN_MODULE_CFG[0].csPin), g_spi[0],
               static_cast<uint8_t>(CAN_MODULE_CFG[0].intPin < 0
                                    ? 255 : CAN_MODULE_CFG[0].intPin)),
    ACAN2517FD(static_cast<uint8_t>(CAN_MODULE_CFG[1].csPin), g_spi[1],
               static_cast<uint8_t>(CAN_MODULE_CFG[1].intPin < 0
                                    ? 255 : CAN_MODULE_CFG[1].intPin)),
};

// ── ISR-Funktionen (IRAM_ATTR: müssen aus dem Interrupt-Kontext aufrufbar sein) ──
// poll() ist für den ISR-Kontext designed (xSemaphoreGiveFromISR + portYIELD_FROM_ISR).
// Aufruf aus Task-Kontext (taskCanRx) weckt die interne ACAN2517FD-Task nicht korrekt.
static void IRAM_ATTR canIsr0() { g_canDev[0].poll(); }
static void IRAM_ATTR canIsr1() { g_canDev[1].poll(); }
static void (*const kCanIsr[2])() = { canIsr0, canIsr1 };

// ── ID-Routing via RuntimeConfig ──────────────────────────────────────────────
// Verwendet g_rtCfg (aus NVS geladen) statt compile-time-Konstanten.
// Damit sind CAN-ID-Bereiche zur Laufzeit per Serial konfigurierbar.
static int findModuleIdx(uint32_t canId) {
    if (canId >= g_rtCfg.can1IdMin && canId <= g_rtCfg.can1IdMax) return 0;
    if (CAN_MODULE_COUNT >= 2 &&
        canId >= g_rtCfg.can2IdMin && canId <= g_rtCfg.can2IdMax) return 1;
    return -1;
}

static ACAN2517FDSettings buildSettings() {
    // CAN_ARB_BPS / CAN_DATA_RATE_FACTOR in config.h:
    //   Aktuell: 125 kbit/s × 1  — Entwicklungsstand Jumperkabel/Breadboard
    //   Ziel:    500 kbit/s × 4  — 2 Mbit/s Data-Phase, TDC erforderlich, verdrillte Leitung
    ACAN2517FDSettings s(CAN_OSC_FREQ, CAN_ARB_BPS, CAN_DATA_RATE_FACTOR);
    s.mRequestedMode              = ACAN2517FDSettings::NormalFD;
    s.mControllerTransmitFIFOSize = 8;   // max. 32; QUEUE_DEPTH=64 überschreitet MCP2518FD-RAM
    s.mControllerReceiveFIFOSize  = 8;   // Default=27: 27×72B=1944B + TX 576B = 2520B > 2048B RAM
    return s;
}

// ── Loopback-Selbsttest: InternalLoopBack → Frame senden/empfangen → NormalFD ──
// Wird aus canBridgeInit() aufgerufen, bevor Tasks gestartet werden (kein Mutex nötig).
// Nutzt ausschließlich Konfig-Konstanten (CAN_OSC_FREQ, CAN_ARB_BPS, CAN_DATA_RATE_FACTOR).
static bool runLoopbackTest(ACAN2517FD& dev, const char* name, void(*isr)()) {
    ACAN2517FDSettings lbSettings(CAN_OSC_FREQ, CAN_ARB_BPS, CAN_DATA_RATE_FACTOR);
    lbSettings.mRequestedMode              = ACAN2517FDSettings::InternalLoopBack;
    lbSettings.mControllerTransmitFIFOSize = 8;
    lbSettings.mControllerReceiveFIFOSize  = 8;
    ACAN2517FDFilters lbFilters;
    lbFilters.appendPassAllFilter(nullptr);

    if (dev.begin(lbSettings, isr, lbFilters) != 0) {
        ESP_LOGE(TAG, "%s Loopback FEHLER — InternalLoopBack begin() fehlgeschlagen  Prüfen:CS/SPI/Quarz/VCC", name);
        dev.end();
        return false;
    }

    CANFDMessage txMsg;
    txMsg.id      = 0x7FF;
    txMsg.ext     = false;
    txMsg.len     = 4;
    txMsg.type    = CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
    txMsg.data[0] = 0xDE; txMsg.data[1] = 0xAD;
    txMsg.data[2] = 0xBE; txMsg.data[3] = 0xEF;
    dev.tryToSend(txMsg);

    CANFDMessage rxMsg;
    bool ok = false;
    for (int i = 0; i < 20 && !ok; ++i) {
        vTaskDelay(pdMS_TO_TICKS(2));
        bool rx = dev.receive(rxMsg);
        if (!rx) rx = dev.receive(rxMsg);   // double-poll im Polling-Modus
        if (rx && rxMsg.id == txMsg.id && rxMsg.len >= 4
            && rxMsg.data[0] == 0xDE && rxMsg.data[1] == 0xAD
            && rxMsg.data[2] == 0xBE && rxMsg.data[3] == 0xEF)
            ok = true;
    }

    if (ok) {
        ESP_LOGI(TAG, "%s Loopback OK  OSC:%lu MHz  Arb:%lu kbit/s",
                 name,
                 (unsigned long)CAN_OSC_MHZ,
                 CAN_ARB_BPS / 1000UL);
    } else {
        ESP_LOGE(TAG, "%s Loopback FEHLER — Frame nicht zurückgekommen  Prüfen:CS/SPI/Quarz/VCC", name);
    }
    // end() stoppt myESP32Task und ISR — verhindert SPI-Konflikte beim NormalFD-Restore
    dev.end();
    return ok;
}

// ══════════════════════════════════════════════════════════════════════════════
bool canBridgeInit() {
    bool allOk = true;
    const ACAN2517FDSettings settings = buildSettings();

    for (uint8_t i = 0; i < CAN_MODULE_COUNT; ++i) {
        const CanModuleCfg& cfg = CAN_MODULE_CFG[i];

        // Aktuellen ID-Bereich aus RuntimeConfig anzeigen
        uint32_t idMin = (i == 0) ? g_rtCfg.can1IdMin : g_rtCfg.can2IdMin;
        uint32_t idMax = (i == 0) ? g_rtCfg.can1IdMax : g_rtCfg.can2IdMax;

        g_spi[i].begin(cfg.sckPin, cfg.misoPin, cfg.mosiPin, -1);

        ESP_LOGI(TAG, "%s  Bus=SPI%d  CS=GPIO%d  IDs:0x%03lX-0x%03lX  Arb:%lu kbit/s  OSC:%lu MHz",
                 cfg.name, cfg.spiBusId + 2, cfg.csPin,
                 (unsigned long)idMin, (unsigned long)idMax,
                 CAN_ARB_BPS / 1000UL,
                 (unsigned long)CAN_OSC_MHZ);

        ACAN2517FDFilters filters;
        filters.appendPassAllFilter(nullptr);

        const auto canIsr = (cfg.intPin < 0) ? nullptr : kCanIsr[i];
        const uint32_t err = g_canDev[i].begin(settings, canIsr, filters);
        if (err != 0) {
            ESP_LOGE(TAG, "%s Init FEHLER  Bitmaske:0x%08lX  Prüfen:CS/SPI/Quarz/VCC",
                     cfg.name, (unsigned long)err);
            allOk = false;
        } else {
            // end() stoppt myESP32Task des NormalFD-begin() — verhindert SPI-Konflikte
            // im Loopback-begin() (gleiche Ursache wie Horizon: kReadBackErrorWith1MHzSPIClock)
            g_canDev[i].end();
            const bool lbOk = runLoopbackTest(g_canDev[i], cfg.name, canIsr);
            // runLoopbackTest() hat bereits end() aufgerufen → NormalFD sauber starten
            g_canDev[i].begin(settings, canIsr, filters);

            if (!lbOk) allOk = false;
        }
    }
    return allOk;
}

// ══════════════════════════════════════════════════════════════════════════════
// Bus-Off-Recovery: MCP2518FD neu initialisieren wenn TX-FIFO dauerhaft voll.
// SPI wurde bereits in canBridgeInit() gestartet — nur MCP2518FD wird resettet.
// ══════════════════════════════════════════════════════════════════════════════
static uint16_t s_fifoFullCount[2] = {0, 0};
static constexpr uint16_t FIFO_FULL_RESET_THRESHOLD = 16;  // ~3 s bei 200 ms/Frame

static void reinitCanModule(int modIdx) {
    const CanModuleCfg&      cfg      = CAN_MODULE_CFG[modIdx];
    const auto               isr      = (cfg.intPin < 0) ? nullptr : kCanIsr[modIdx];
    const ACAN2517FDSettings settings = buildSettings();
    ACAN2517FDFilters        filters;
    filters.appendPassAllFilter(nullptr);

    ESP_LOGW(TAG, "CAN-TX %s: Bus-Off-Recovery — MCP2518FD wird neu gestartet", cfg.name);
    if (xSemaphoreTake(g_canMutex[modIdx], pdMS_TO_TICKS(200)) == pdTRUE) {
        g_canDev[modIdx].end();
        const uint32_t err = g_canDev[modIdx].begin(settings, isr, filters);
        xSemaphoreGive(g_canMutex[modIdx]);
        if (err != 0)
            ESP_LOGE(TAG, "CAN-TX %s: Recovery begin() Fehler 0x%08lX",
                     cfg.name, (unsigned long)err);
        else
            ESP_LOGI(TAG, "CAN-TX %s: Recovery OK", cfg.name);
    } else {
        ESP_LOGE(TAG, "CAN-TX %s: Recovery — Mutex-Timeout", cfg.name);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: CAN-TX  (g_udpToCanQueue → MCP2518FD)
// Routing über g_rtCfg ID-Bereiche (konfigurierbar per Serial).
// ══════════════════════════════════════════════════════════════════════════════
void taskCanTx(void* /*param*/) {
    RawPacket pkt;

    for (;;) {
        if (xQueueReceive(g_udpToCanQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;
        if (pkt.len < MIN_UDP_PACKET) continue;

        const uint32_t canId   = readCanId(pkt.buf);
        const uint8_t* payload = pkt.buf + UDP_HEADER_BYTES;
        uint8_t        payLen  = static_cast<uint8_t>(pkt.len - UDP_HEADER_BYTES);
        if (payLen > MAX_CANFD_PAYLOAD) payLen = MAX_CANFD_PAYLOAD;

        // ── Mapping-Kommando von bridge.exe abfangen ──────────────────────────
        // HID_MAP_CAN_ID-Frames kommen via UART von bridge.exe und sollen NICHT
        // auf den CAN-Bus — nur zum hid_decoder weitergeleitet werden.
        if (canId == HID_MAP_CAN_ID) {
            if (xQueueSend(g_canToHidQueue, &pkt, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "CAN-TX: HID-Map-Queue voll, Kommando verworfen");
            }
            continue;
        }

        // ── ESP32-eigene MODULE_CONFIG abfangen — NICHT auf CAN-Bus senden ──
        if (canId == ESP32_HB_CAN_ID) {
            if (payLen >= 5 && payload[0] == 0xFD) {
                uint32_t iv;
                memcpy(&iv, payload + 1, 4);
                if (iv >= 100u && iv <= 60000u) {
                    esp32HbSetInterval(iv);
                    esp32HbTriggerNow();
                    ESP_LOGI(TAG, "ESP32-HB Interval: %lu ms + Sofort-HB",
                             (unsigned long)iv);
                }
            }
            continue;
        }

        const int modIdx = findModuleIdx(canId);
        if (modIdx < 0) {
#ifdef CAN_SIM_INPUTS
            // JOY-Bereich (0x600-0x6FF): Kommandos für Input-Simulator-Module
            // (STATUS_REQUEST, MODULE_CONFIG). g_simCmdQueue → taskCanSimulator.
            if (canId >= JOY_CAN_ID_MIN && canId <= JOY_CAN_ID_MAX && g_simCmdQueue) {
                if (xQueueSend(g_simCmdQueue, &pkt, 0) != pdTRUE)
                    ESP_LOGW(TAG, "CAN-TX: SimCmd-Queue voll, ID 0x%03lX verworfen",
                             (unsigned long)canId);
            } else {
                ESP_LOGW(TAG, "CAN-TX: ID 0x%03lX → kein Modul-Bereich, verworfen",
                         (unsigned long)canId);
            }
#else
            ESP_LOGW(TAG, "CAN-TX: ID 0x%03lX → kein Modul-Bereich, verworfen",
                     (unsigned long)canId);
#endif
            continue;
        }

        CANFDMessage msg;
        msg.id   = canId & 0x1FFFFFFFu;
        msg.ext  = (canId > 0x7FFu);
        msg.len  = payLen;
        // BRS=0 (CANFD_NO_BIT_RATE_SWITCH): kein Timing-Wechsel zwischen
        // Arbitration- und Data-Phase. Mit DataBitRateFactor::x1 hätte BRS=1
        // keinen Geschwindigkeitsvorteil, erzeugt aber einen internen Resync-
        // Punkt im MCP2518FD. Auf Jumperkabeln können dabei Sampling-Fehler
        // bei langen Frames entstehen. Erst bei 500 kbit/s Arb + x4 (2 Mbit/s
        // Data) lohnt sich BRS=1 — dann TDC konfigurieren, verdrillte Leitung.
        msg.type = CANFDMessage::CANFD_NO_BIT_RATE_SWITCH;
        memcpy(msg.data, payload, payLen);
        // ── CAN-FD Payload-Längen-Padding (Wurzel-Ursache des FIFO-voll-Bugs) ──
        // CAN-FD erlaubt nur bestimmte Payload-Längen (DLC-Kodierung):
        //   0-8, 12, 16, 20, 24, 32, 48, 64 Byte
        // Jeder andere Wert (z.B. 30 Byte für horizon-Datenframes) macht
        // CANFDMessage::isValid() = false → tryToSend() bricht sofort ab
        // (vor dem SPI-Transfer!) und gibt false zurück — Frame nie gesendet.
        // Das "FIFO voll"-Log war irreführend: der FIFO war leer, Frames wurden
        // nie queued. Diagnose: bridge_ESP32 loggte false, horizon sah 0xFB/0xFD
        // (4/5 Byte → gültig), aber kein 0x00/0xFC (30/35 Byte → ungültig).
        // pad() rundet auf die nächste gültige Länge auf und nullt Padding-Bytes:
        //   30 → 32: Bytes [30][31] = 0x00  (DLC=13, CRC-21)
        //   35 → 48: Bytes [35]..[47] = 0x00 (DLC=14, CRC-21)
        // Empfänger liest nur die ersten payLen Bytes — Padding ist transparent.
        msg.pad();
        // Guard: pad() scheitert nur wenn payLen > 64 (durch MAX_CANFD_PAYLOAD-
        // Clamp oben ausgeschlossen). Sicherheitsnetz für künftige Protokoll-
        // änderungen, die unerwartete Längen einführen könnten.
        if (!msg.isValid()) {
            ESP_LOGE(TAG, "CAN-TX %s: Ungültige CAN-FD-Länge %u (nach pad: %u)"
                     " — ID 0x%03lX verworfen. Erlaubt: 0-8,12,16,20,24,32,48,64",
                     CAN_MODULE_CFG[modIdx].name,
                     payLen, msg.len, (unsigned long)canId);
            continue;
        }

        if (xSemaphoreTake(g_canMutex[modIdx], pdMS_TO_TICKS(CAN_MUTEX_TIMEOUT_MS)) == pdTRUE) {
            const bool ok = g_canDev[modIdx].tryToSend(msg);
            xSemaphoreGive(g_canMutex[modIdx]);
            if (ok) {
                s_fifoFullCount[modIdx] = 0;
                ESP_LOGD(TAG, "CAN-TX %s: ID=0x%03lX len=%u type=0x%02X",
                         CAN_MODULE_CFG[modIdx].name, (unsigned long)canId,
                         payLen, payLen > 0 ? payload[0] : 0xFF);
            } else {
                ESP_LOGW(TAG, "CAN-TX %s: FIFO voll, ID 0x%03lX verworfen",
                         CAN_MODULE_CFG[modIdx].name, (unsigned long)canId);
                if (++s_fifoFullCount[modIdx] >= FIFO_FULL_RESET_THRESHOLD) {
                    s_fifoFullCount[modIdx] = 0;
                    reinitCanModule(modIdx);
                }
            }
        } else {
            ESP_LOGW(TAG, "CAN-TX %s: Mutex-Timeout", CAN_MODULE_CFG[modIdx].name);
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: CAN-RX  (MCP2518FD → g_canToUdpQueue + g_canToUsbQueue)
// Schreibt empfangene Frames in BEIDE ausgehenden Queues.
// Jeder Transport-Task liest seine eigene Queue und verwirft wenn inaktiv.
// ══════════════════════════════════════════════════════════════════════════════
void taskCanRx(void* /*param*/) {
    CANFDMessage msg;
    RawPacket    pkt;

    for (;;) {
        bool anyReceived = false;

        for (uint8_t i = 0; i < CAN_MODULE_COUNT; ++i) {
            // ISR-Modus: canIsr0/1 haben poll() bereits ausgeführt → Frame liegt im Buffer.
            bool received = false;
            if (xSemaphoreTake(g_canMutex[i], pdMS_TO_TICKS(CAN_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                received = g_canDev[i].receive(msg);
                xSemaphoreGive(g_canMutex[i]);
            }

            if (!received) continue;
            anyReceived = true;

            ESP_LOGD(TAG, "CAN-RX %s: ID=0x%03lX len=%u type=0x%02X",
                     CAN_MODULE_CFG[i].name,
                     (unsigned long)msg.id,
                     msg.len,
                     msg.len > 0 ? msg.data[0] : 0xFF);

            uint8_t payLen = msg.len;
            if (payLen > MAX_CANFD_PAYLOAD) payLen = MAX_CANFD_PAYLOAD;
            writeCanId(pkt.buf, msg.id);
            memcpy(pkt.buf + UDP_HEADER_BYTES, msg.data, payLen);
            pkt.len = static_cast<uint16_t>(UDP_HEADER_BYTES + payLen);

            // ── In ausgehende Queues schreiben ────────────────────────────
            // UDP-Queue → TaskNetTx (liest + verwirft wenn WiFi disabled)
            if (xQueueSend(g_canToUdpQueue, &pkt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "CAN-RX %s: UDP-Queue voll, ID 0x%03lX verworfen",
                         CAN_MODULE_CFG[i].name, (unsigned long)msg.id);
            }
            // UART-Queue → TaskUsbTx (→ bridge.exe über UART-Brücken-Port)
            if (xQueueSend(g_canToUsbQueue, &pkt, 0) != pdTRUE) {
                ESP_LOGW(TAG, "CAN-RX %s: UART-Queue voll, ID 0x%03lX verworfen",
                         CAN_MODULE_CFG[i].name, (unsigned long)msg.id);
            }
            // HID-Queue → TaskHidDecoder (nur Achs-/Button-Frames 0x02 im Joystick-ID-Bereich)
            // Heartbeats (0x01), Poll-Responses und Konfigframes nicht in HID-Queue
            if (msg.id >= JOY_CAN_ID_MIN && msg.id <= JOY_CAN_ID_MAX
                && payLen > 0 && msg.data[0] == 0x02u) {
                if (xQueueSend(g_canToHidQueue, &pkt, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "CAN-RX %s: HID-Queue voll, ID 0x%03lX verworfen",
                             CAN_MODULE_CFG[i].name, (unsigned long)msg.id);
                }
            }
        }

        if (!anyReceived) vTaskDelay(pdMS_TO_TICKS(CAN_POLL_DELAY_MS));
    }
}

#endif // CAN_HW_ENABLED

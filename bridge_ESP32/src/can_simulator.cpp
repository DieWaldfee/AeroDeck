// ══════════════════════════════════════════════════════════════════════════════
// can_simulator.cpp — CAN-FD Software-Simulator (Ersatz für MCP2518FD)
//
// Input-Simulator: Achsen + Buttons für sim_joystick / sim_axes14 / sim_axis5.
// Läuft parallel zur MCP2518FD-Hardware (CAN_SIM_INPUTS) oder als Vollersatz
// ohne Hardware (kein CAN_HW_ENABLED).
//
// Horizon-Simulation wurde entfernt — das reale CAN-Gerät übernimmt.
//
// CAN-IDs:
//   0x620 — Joystick-Buttons (BTN1 Single, BTN2 Chord 3+4)
//   0x621 — Regler Achsen 0–3 (Sinus, verschiedene Perioden)
//   0x622 — Regler Achse 5 (Dreieckswelle) + Button 6 (Chord 10+11)
// ══════════════════════════════════════════════════════════════════════════════
#if !defined(CAN_HW_ENABLED) || defined(CAN_SIM_INPUTS)

#include "can_simulator.h"
#include "bridge_types.h"
#include "config.h"
#include "hid_decoder.h"
#include "esp32_heartbeat.h"
#include <cmath>
#include <cstring>
#include <esp_log.h>


static const char* TAG = "SIM";

// ── CAN-IDs ───────────────────────────────────────────────────────────────────
// TX-IDs: bridge.exe → Gerät (bridge.ini: can_tx_id)
static constexpr uint32_t SIM_JOY_CAN_ID    = 0x620;  // Buttons
static constexpr uint32_t SIM_AXES14_CAN_ID = 0x621;  // Achsen 0-3
static constexpr uint32_t SIM_AXIS5_CAN_ID  = 0x622;  // Achse 5 + Button 6

// RX-IDs: Gerät → bridge.exe (bridge.ini: can_rx_id = can_tx_id | 0x80)
static constexpr uint32_t SIM_JOY_RET_ID    = SIM_JOY_CAN_ID    | 0x80u;  // 0x6A0
static constexpr uint32_t SIM_AXES14_RET_ID = SIM_AXES14_CAN_ID | 0x80u;  // 0x6A1
static constexpr uint32_t SIM_AXIS5_RET_ID  = SIM_AXIS5_CAN_ID  | 0x80u;  // 0x6A2

// ── Zeitkonstanten ────────────────────────────────────────────────────────────
static constexpr uint32_t SIM_AXIS_INTERVAL_MS      =   50;
static constexpr uint32_t SIM_HEARTBEAT_INTERVAL_MS = 1000;
static constexpr uint32_t SIM_STATUS_INTERVAL_MS    = 5000;
static constexpr uint32_t SIM_TICK_MS               =   10;

// Sinus-Phasen-Inkrement pro Achsen-Tick (50 ms):
//   Achse 0: Periode  8000 ms  →  2π / (8000/50)  = 0.03927 rad/Tick
//   Achse 1: Periode 12000 ms  →  2π / (12000/50) = 0.02618 rad/Tick
//   Achse 2: Periode  6000 ms  →  2π / (6000/50)  = 0.05236 rad/Tick
//   Achse 3: Periode 16000 ms  →  2π / (16000/50) = 0.01963 rad/Tick
static constexpr float SIM_AXIS0_DPHI = 0.03926990816f;
static constexpr float SIM_AXIS1_DPHI = 0.02617993878f;
static constexpr float SIM_AXIS2_DPHI = 0.05235987756f;
static constexpr float SIM_AXIS3_DPHI = 0.01963495408f;
static constexpr float TWO_PI         = 6.28318530718f;

// Button-Bitmask für 4-Schritt-Muster auf 0x620 (4s Zyklus, 1s pro Schritt)
static constexpr uint8_t BTN_MASK_TABLE[4] = { 0x01u, 0x03u, 0x02u, 0x00u };

// ── Hilfsfunktion: Frame in Transport-Queues schreiben ────────────────────────
static void pushToQueues(uint32_t canId, const uint8_t* payload, uint8_t payLen,
                         bool toHid, bool toTransport) {
    if (payLen > MAX_CANFD_PAYLOAD) payLen = MAX_CANFD_PAYLOAD;

    RawPacket pkt;
    writeCanId(pkt.buf, canId);
    memcpy(pkt.buf + UDP_HEADER_BYTES, payload, payLen);
    pkt.len = static_cast<uint16_t>(UDP_HEADER_BYTES + payLen);

    if (toHid) {
        if (xQueueSend(g_canToHidQueue, &pkt, 0) != pdTRUE)
            ESP_LOGW(TAG, "HID-Queue voll, Frame ID 0x%03lX verworfen", (unsigned long)canId);
    }
    if (toTransport) {
        if (xQueueSend(g_canToUdpQueue, &pkt, 0) != pdTRUE)
            ESP_LOGW(TAG, "UDP-Queue voll, Frame ID 0x%03lX verworfen", (unsigned long)canId);
        if (xQueueSend(g_canToUsbQueue, &pkt, 0) != pdTRUE)
            ESP_LOGW(TAG, "UART-Queue voll, Frame ID 0x%03lX verworfen", (unsigned long)canId);
    }
}

// ══════════════════════════════════════════════════════════════════════════════
void canSimulatorInit() {
    // Kein Fallback — Button- und Achsen-Konfiguration kommt ausschliesslich
    // aus bridge.ini via bridge.exe (RESET + ENTRY + COMMIT / MAP_SUB_AXIS).
    // NVS leer nach Fresh-Flash → kein HID-Output bis bridge.exe verbindet.

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════╗");
#ifdef CAN_HW_ENABLED
    ESP_LOGI(TAG, "║   INPUT-SIMULATOR (Hybrid: HW + Sim)        ║");
#else
    ESP_LOGI(TAG, "║       CAN-FD SIMULATOR AKTIV                ║");
    ESP_LOGI(TAG, "║  MCP2518FD noch nicht vorhanden             ║");
#endif
    ESP_LOGI(TAG, "╠══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║  0x620  BTN1 (Single)  BTN2 (Chord 3+4)    ║");
    ESP_LOGI(TAG, "║  0x621  Achsen 0-3  (8/12/6/16 s Sinus)    ║");
    ESP_LOGI(TAG, "║  0x622  Achse 5  (4s Dreieck)  BTN6 (10+11)║");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════╝");
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: CAN-FD Simulator
// Prio: TASK_PRIO_CAN_TX (5) | Core 1
// ══════════════════════════════════════════════════════════════════════════════
void taskCanSimulator(void* /*param*/) {
    TickType_t lastAxisTick      = xTaskGetTickCount();
    TickType_t lastJoyHbTick     = xTaskGetTickCount();
    TickType_t lastAxes14HbTick  = xTaskGetTickCount();
    TickType_t lastAxis5HbTick   = xTaskGetTickCount();
    TickType_t lastStatusTick    = xTaskGetTickCount();

    float phi0 = 0.0f;
    float phi1 = 0.0f;
    float phi2 = 0.0f;
    float phi3 = 0.0f;

    uint32_t lastBtnSection = 0xFF;   // Buttons 0x620
    uint8_t  lastBtn6State  = 0xFF;   // Button 6 auf 0x622

    // Per-Modul Heartbeat-Intervalle (konfigurierbar via MODULE_CONFIG 0xFD)
    uint32_t joyHbIntervalMs    = SIM_HEARTBEAT_INTERVAL_MS;
    uint32_t axes14HbIntervalMs = SIM_HEARTBEAT_INTERVAL_MS;
    uint32_t axis5HbIntervalMs  = SIM_HEARTBEAT_INTERVAL_MS;

    // Sofort-Heartbeat beim Task-Start
    {
        uint8_t hbPkt[5] = { 0x01u };
        const uint32_t msNow = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        memcpy(hbPkt + 1, &msNow, 4);
        pushToQueues(SIM_JOY_RET_ID,    hbPkt, sizeof(hbPkt), false, true);
        pushToQueues(SIM_AXES14_RET_ID, hbPkt, sizeof(hbPkt), false, true);
        pushToQueues(SIM_AXIS5_RET_ID,  hbPkt, sizeof(hbPkt), false, true);
        ESP_LOGI(TAG, "Boot-Heartbeat gesendet");
    }

    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        const uint32_t   ms  = (uint32_t)(now * portTICK_PERIOD_MS);

        // ── Achsen 0–3 (0x621): alle SIM_AXIS_INTERVAL_MS → HID + Transport ──
        if ((now - lastAxisTick) >= pdMS_TO_TICKS(SIM_AXIS_INTERVAL_MS)) {
            lastAxisTick = now;

            phi0 += SIM_AXIS0_DPHI; if (phi0 > TWO_PI) phi0 -= TWO_PI;
            phi1 += SIM_AXIS1_DPHI; if (phi1 > TWO_PI) phi1 -= TWO_PI;
            phi2 += SIM_AXIS2_DPHI; if (phi2 > TWO_PI) phi2 -= TWO_PI;
            phi3 += SIM_AXIS3_DPHI; if (phi3 > TWO_PI) phi3 -= TWO_PI;

            const int16_t axis0 = static_cast<int16_t>(sinf(phi0) * 32767.0f);
            const int16_t axis1 = static_cast<int16_t>(sinf(phi1) * 32767.0f);
            const int16_t axis2 = static_cast<int16_t>(sinf(phi2) * 32767.0f);
            const int16_t axis3 = static_cast<int16_t>(sinf(phi3) * 32767.0f);

            // Format: [0x02][int16 axis0..3 LE]  (INI-Reihenfolge, kein Offset/Count)
            uint8_t axes14Payload[9];
            axes14Payload[0] = 0x02;
            memcpy(axes14Payload + 1, &axis0, 2);
            memcpy(axes14Payload + 3, &axis1, 2);
            memcpy(axes14Payload + 5, &axis2, 2);
            memcpy(axes14Payload + 7, &axis3, 2);
            pushToQueues(SIM_AXES14_RET_ID, axes14Payload, sizeof(axes14Payload),
                         true, true);  // HID + Transport (bridge.exe sieht Achswerte)

            // Achse 5 (0x622): Dreieckswelle 4s — Trigger 1 (kombinierter Frame)
            //   0–2s: −32767 → +32767  |  2–4s: +32767 → −32767
            // Format: [0x02][int16 axis5 LE 2B][00][btn6 1B]  — 5 Byte
            // BTN6 = 5, 0 → B5 = 5. Byte (1-indexiert) → Array-Index 4
            const uint32_t sliderPos = ms % 4000u;
            const int16_t  axis5     = (sliderPos < 2000u)
                                       ? static_cast<int16_t>(-32767 + (int32_t)sliderPos          * 32767 / 1000)
                                       : static_cast<int16_t>( 32767 - (int32_t)(sliderPos - 2000u) * 32767 / 1000);
            uint8_t axis5Payload[5];
            memset(axis5Payload, 0, sizeof(axis5Payload));
            axis5Payload[0] = 0x02;
            memcpy(axis5Payload + 1, &axis5, 2);
            axis5Payload[4] = lastBtn6State & 0x01u;   // B5 → Index 4
            pushToQueues(SIM_AXIS5_RET_ID, axis5Payload, sizeof(axis5Payload),
                         true, true);  // HID + Transport
        }

        // ── Buttons 0x620: 4s-Zyklus, 1s pro Schritt ─────────────────────────
        {
            const uint32_t section = (ms / 1000u) % 4u;
            if (section != lastBtnSection) {
                lastBtnSection = section;
                const uint8_t bitmask = BTN_MASK_TABLE[section];
                uint8_t btnPkt[2] = { 0x02u, bitmask };
                pushToQueues(SIM_JOY_RET_ID, btnPkt, sizeof(btnPkt), true, true);
                ESP_LOGD(TAG, "BTN0x620 Schritt %lu mask=0x%02X",
                         (unsigned long)section, bitmask);
            }
        }

        // ── Button 6 (0x622): Trigger 2 — sofortiger Frame bei Zustandswechsel
        {
            const uint8_t btn6State = ((ms / 2000u) % 2u) ? 0x01u : 0x00u;
            if (btn6State != lastBtn6State) {
                lastBtn6State = btn6State;
                const uint32_t sp  = ms % 4000u;
                const int16_t  ax5 = (sp < 2000u)
                    ? static_cast<int16_t>(-32767 + (int32_t)sp           * 32767 / 1000)
                    : static_cast<int16_t>( 32767 - (int32_t)(sp - 2000u) * 32767 / 1000);
                uint8_t pkt[5];
                memset(pkt, 0, sizeof(pkt));
                pkt[0] = 0x02;
                memcpy(pkt + 1, &ax5, 2);
                pkt[4] = lastBtn6State & 0x01u;   // B5 → Index 4
                pushToQueues(SIM_AXIS5_RET_ID, pkt, sizeof(pkt), true, true);
                ESP_LOGD(TAG, "BTN6 = %u → sofortiger Frame 0x622", lastBtn6State);
            }
        }

        // ── Heartbeats: pro Modul individuelles Intervall (via 0xFD konfigurierbar)
        // Format: [0x01][uptime_ms 4B LE] — identisch zum realen Gerät
        {
            uint8_t hbPkt[5];
            hbPkt[0] = 0x01u;
            memcpy(hbPkt + 1, &ms, 4);
            if ((now - lastJoyHbTick) >= pdMS_TO_TICKS(joyHbIntervalMs)) {
                lastJoyHbTick = now;
                pushToQueues(SIM_JOY_RET_ID, hbPkt, sizeof(hbPkt), false, true);
            }
            if ((now - lastAxes14HbTick) >= pdMS_TO_TICKS(axes14HbIntervalMs)) {
                lastAxes14HbTick = now;
                pushToQueues(SIM_AXES14_RET_ID, hbPkt, sizeof(hbPkt), false, true);
            }
            if ((now - lastAxis5HbTick) >= pdMS_TO_TICKS(axis5HbIntervalMs)) {
                lastAxis5HbTick = now;
                pushToQueues(SIM_AXIS5_RET_ID, hbPkt, sizeof(hbPkt), false, true);
            }
        }

        // ── Status-Log: alle SIM_STATUS_INTERVAL_MS ──────────────────────────
        if ((now - lastStatusTick) >= pdMS_TO_TICKS(SIM_STATUS_INTERVAL_MS)) {
            lastStatusTick = now;
            const uint32_t spLog   = ms % 4000u;
            const int16_t  ax5Log  = (spLog < 2000u)
                                     ? static_cast<int16_t>(-32767 + (int32_t)spLog   * 32767 / 1000)
                                     : static_cast<int16_t>( 32767 - (int32_t)(spLog - 2000u) * 32767 / 1000);
            const uint8_t  bmLog   = (lastBtnSection < 4u) ? BTN_MASK_TABLE[lastBtnSection] : 0u;
            ESP_LOGI(TAG, "[SIM] A0=%d A1=%d A2=%d A3=%d A5=%d  BTN0x620=0x%02X  BTN6=%u",
                     (int)(sinf(phi0) * 32767.0f), (int)(sinf(phi1) * 32767.0f),
                     (int)(sinf(phi2) * 32767.0f), (int)(sinf(phi3) * 32767.0f),
                     (int)ax5Log, bmLog, lastBtn6State & 0x01u);
        }

        // ── Kommandos von bridge.exe verarbeiten ──────────────────────────────
        // Im Hybrid-Modus (CAN_HW_ENABLED + CAN_SIM_INPUTS) leitet taskCanTx
        // JOY-Bereich-Frames (0x600-0x6FF) nach g_simCmdQueue um.
        // g_udpToCanQueue bleibt exklusiv für taskCanTx (Frames an echte CAN-Hardware).
        // Im reinen Simulator-Modus (kein CAN_HW_ENABLED): direkt aus g_udpToCanQueue.
        {
            RawPacket mapPkt;
#ifdef CAN_HW_ENABLED
            QueueHandle_t cmdQ = g_simCmdQueue;
#else
            QueueHandle_t cmdQ = g_udpToCanQueue;
#endif
            while (cmdQ && xQueueReceive(cmdQ, &mapPkt, 0) == pdTRUE) {
                const uint32_t  rxId = readCanId(mapPkt.buf);
                const uint8_t*  p    = mapPkt.buf + UDP_HEADER_BYTES;
                const uint16_t  pLen = static_cast<uint16_t>(mapPkt.len - UDP_HEADER_BYTES);

                // ESP32-eigene MODULE_CONFIG abfangen — nicht als Simulator-Modul behandeln
                if (rxId == ESP32_HB_CAN_ID) {
                    if (pLen >= 5 && p[0] == 0xFD) {
                        uint32_t iv;
                        memcpy(&iv, p + 1, 4);
                        if (iv >= 100u && iv <= 60000u) {
                            esp32HbSetInterval(iv);
                            esp32HbTriggerNow();
                            ESP_LOGI(TAG, "ESP32-HB Interval: %lu ms + Sofort-HB",
                                     (unsigned long)iv);
                        }
                    }
                    continue;
                }

                if (rxId == HID_MAP_CAN_ID) {
                    if (xQueueSend(g_canToHidQueue, &mapPkt, pdMS_TO_TICKS(10)) != pdTRUE)
                        ESP_LOGW(TAG, "HID-Map-Queue voll, Kommando verworfen");
                    continue;
                }

                // MODULE_CONFIG (0xFD): Heartbeat-Interval pro Modul setzen
                if (pLen >= 5 && p[0] == 0xFD) {
                    uint32_t iv;
                    memcpy(&iv, p + 1, 4);
                    if (iv >= 100u && iv <= 60000u) {
                        uint8_t hbPkt[5] = { 0x01u };
                        memcpy(hbPkt + 1, &ms, 4);
                        if (rxId == SIM_JOY_CAN_ID) {
                            joyHbIntervalMs = iv;
                            lastJoyHbTick   = now;
                            pushToQueues(SIM_JOY_RET_ID, hbPkt, sizeof(hbPkt), false, true);
                        } else if (rxId == SIM_AXES14_CAN_ID) {
                            axes14HbIntervalMs = iv;
                            lastAxes14HbTick   = now;
                            pushToQueues(SIM_AXES14_RET_ID, hbPkt, sizeof(hbPkt), false, true);
                        } else if (rxId == SIM_AXIS5_CAN_ID) {
                            axis5HbIntervalMs = iv;
                            lastAxis5HbTick   = now;
                            pushToQueues(SIM_AXIS5_RET_ID, hbPkt, sizeof(hbPkt), false, true);
                        }
                        ESP_LOGI(TAG, "MODULE_CONFIG 0x%03lX → HB-Interval %lu ms + Sofort-HB",
                                 (unsigned long)rxId, (unsigned long)iv);
                    }
                    continue;
                }

                if (pLen < 6 || p[0] != 0xFE) continue;   // kein STATUS_REQUEST

                if (rxId == SIM_JOY_CAN_ID) {
                    // 0x620 sim_joystick: aktueller Button-Bitmask
                    const uint8_t bm = (lastBtnSection < 4u) ? BTN_MASK_TABLE[lastBtnSection] : 0u;
                    uint8_t resp[7];
                    resp[0] = 0x04; resp[1] = p[1];  // STATUS_RESPONSE
                    memcpy(resp + 2, p + 2, 4);
                    resp[6] = bm;
                    pushToQueues(rxId | 0x80u, resp, sizeof(resp), false, true);
                    ESP_LOGD(TAG, "POLL 0x%03lX(JOY) → BTN=0x%02X", (unsigned long)rxId, bm);

                } else if (rxId == SIM_AXES14_CAN_ID) {
                    // 0x621 sim_axes14: aktuelle Sinus-Achswerte 0-3
                    const int16_t ax0 = static_cast<int16_t>(sinf(phi0) * 32767.0f);
                    const int16_t ax1 = static_cast<int16_t>(sinf(phi1) * 32767.0f);
                    const int16_t ax2 = static_cast<int16_t>(sinf(phi2) * 32767.0f);
                    const int16_t ax3 = static_cast<int16_t>(sinf(phi3) * 32767.0f);
                    uint8_t resp[14];
                    resp[0] = 0x04; resp[1] = p[1];  // STATUS_RESPONSE
                    memcpy(resp + 2,  p + 2, 4);
                    memcpy(resp + 6,  &ax0,  2);
                    memcpy(resp + 8,  &ax1,  2);
                    memcpy(resp + 10, &ax2,  2);
                    memcpy(resp + 12, &ax3,  2);
                    pushToQueues(rxId | 0x80u, resp, sizeof(resp), false, true);
                    ESP_LOGD(TAG, "POLL 0x%03lX(AXES14) → A0=%d A1=%d A2=%d A3=%d",
                             (unsigned long)rxId, (int)ax0, (int)ax1, (int)ax2, (int)ax3);

                } else if (rxId == SIM_AXIS5_CAN_ID) {
                    // 0x622 sim_axis5: aktuelle Achse 5 + Button-6-Zustand
                    const uint32_t sliderPos = ms % 4000u;
                    const int16_t  ax5 = (sliderPos < 2000u)
                        ? static_cast<int16_t>(-32767 + (int32_t)sliderPos          * 32767 / 1000)
                        : static_cast<int16_t>( 32767 - (int32_t)(sliderPos - 2000u) * 32767 / 1000);
                    // STATUS_RESPONSE: Layout wie Spontandaten, aber mit 5-Byte-Header (seq+ts_echo).
                    // BTN6: B5 = Index 4 in Spontan → Index 4+5=9 in STATUS_RESPONSE.
                    const uint8_t btn6 = lastBtn6State & 0x01u;
                    uint8_t resp[10];
                    memset(resp, 0, sizeof(resp));
                    resp[0] = 0x04; resp[1] = p[1];  // STATUS_RESPONSE
                    memcpy(resp + 2, p + 2, 4);     // ts_echo
                    memcpy(resp + 6, &ax5, 2);       // Achse 5 (spontan-Index 1–2 → resp-Index 6–7)
                    resp[9] = btn6;                   // BTN6 (spontan-Index 4 → resp-Index 9)
                    pushToQueues(rxId | 0x80u, resp, sizeof(resp), false, true);  // 10 Byte
                    ESP_LOGD(TAG, "POLL 0x%03lX(AXIS5) → A5=%d BTN6=%u",
                             (unsigned long)rxId, (int)ax5, btn6);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SIM_TICK_MS));
    }
}

#endif // !CAN_HW_ENABLED || CAN_SIM_INPUTS

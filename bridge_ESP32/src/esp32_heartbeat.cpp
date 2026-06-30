#include "esp32_heartbeat.h"
#include "bridge_types.h"
#include "config.h"
#include <atomic>
#include <cstring>
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ESP-HB";

static std::atomic<uint32_t> g_hbIntervalMs{1000};
static std::atomic<bool>     g_hbTrigger{false};

void esp32HbSetInterval(uint32_t ms) { g_hbIntervalMs.store(ms); }
void esp32HbTriggerNow()             { g_hbTrigger.store(true); }

static void sendHb() {
    const uint32_t ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint8_t payload[5] = { 0x01u };
    memcpy(payload + 1, &ms, 4);

    RawPacket pkt;
    writeCanId(pkt.buf, ESP32_HB_CAN_ID);
    memcpy(pkt.buf + UDP_HEADER_BYTES, payload, sizeof(payload));
    pkt.len = static_cast<uint16_t>(UDP_HEADER_BYTES + sizeof(payload));

    xQueueSend(g_canToUsbQueue, &pkt, 0);
    xQueueSend(g_canToUdpQueue, &pkt, 0);
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: ESP32-Heartbeat
// Prio: TASK_PRIO_NET_TX | Core 1
// Sendet Heartbeats auf ESP32_HB_CAN_ID an g_canToUsbQueue + g_canToUdpQueue.
// ══════════════════════════════════════════════════════════════════════════════
void taskEsp32Heartbeat(void* /*param*/) {
    sendHb();
    ESP_LOGI(TAG, "Boot-Heartbeat gesendet (CAN-ID 0x%03X)", ESP32_HB_CAN_ID);

    TickType_t lastHbTick = xTaskGetTickCount();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10));

        const TickType_t now        = xTaskGetTickCount();
        const uint32_t   interval   = g_hbIntervalMs.load();
        const bool       trigger    = g_hbTrigger.exchange(false);
        const bool       timerFired = (now - lastHbTick) >= pdMS_TO_TICKS(interval);

        if (trigger || timerFired) {
            sendHb();
            lastHbTick = now;
        }
    }
}

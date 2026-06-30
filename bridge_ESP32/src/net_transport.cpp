#include "net_transport.h"
#include "config.h"
#include "config_manager.h"
#include "bridge_types.h"
#include "credentials.h"

#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <esp_log.h>

static const char* TAG = "NET";

// WiFi-Timeout: 10 s in 500 ms Schritten
static constexpr uint8_t  WIFI_STEPS    = 20;
static constexpr uint32_t WIFI_STEP_MS  = 500;

// UDP-Discovery / Reconnect:
//   Solange Bridge-IP unbekannt → Broadcast senden (bridge.exe empfängt auf demselben LAN).
//   Nach BRIDGE_TIMEOUT_MS ohne eingehendes Paket → IP vergessen → zurück auf Broadcast.
//   Sobald bridge.exe ein Paket sendet, lernt taskNetRx die echte IP (auch nach DHCP-Wechsel).
static constexpr uint32_t BRIDGE_TIMEOUT_MS   = 10000;

// UART-Vorrang: Wenn bridge.exe über UART kommuniziert, soll WiFi-Senden unterdrückt werden.
//   Nach UART_ACTIVE_TIMEOUT_MS ohne neuen UART-Frame → UART als inaktiv betrachten → WiFi-Senden fortsetzen.
//   bridge.exe sendet mindestens alle 5 s (UDP-Init-Retry), also reicht 10 s als Toleranz.
static constexpr uint32_t UART_ACTIVE_TIMEOUT_MS = 10000;
static constexpr uint32_t BRIDGE_BROADCAST_IP = 0xFFFFFFFFu;  // 255.255.255.255

static std::atomic<TickType_t> s_lastNetRxTick{0};  // Zeitpunkt letztes empfangenes Paket

// ══════════════════════════════════════════════════════════════════════════════
// Task: WiFi-Manager  (Hintergrund-Task, beendet sich nach Ergebnis)
// Setzt g_wifiReady ODER g_wifiDisabled.
// Kein Blockieren von setup() — System ist via USB bereits ~400 ms nach Boot aktiv.
// ══════════════════════════════════════════════════════════════════════════════
void taskWifiManager(void* /*param*/) {
    WiFi.mode(WIFI_STA);

    // Aus credentials.h stammende Konstanten werden über RuntimeConfig überschrieben
    if (WIFI_STATIC_IP) {
        IPAddress ip(STATIC_IP[0],      STATIC_IP[1],      STATIC_IP[2],      STATIC_IP[3]);
        IPAddress gw(STATIC_GATEWAY[0], STATIC_GATEWAY[1], STATIC_GATEWAY[2], STATIC_GATEWAY[3]);
        IPAddress sn(STATIC_SUBNET[0],  STATIC_SUBNET[1],  STATIC_SUBNET[2],  STATIC_SUBNET[3]);
        WiFi.config(ip, gw, sn);
    }

    // SSID + Pass aus NVS/RuntimeConfig
    WiFi.begin(g_rtCfg.ssid.c_str(), g_rtCfg.pass.c_str());
    ESP_LOGI(TAG, "WiFi verbinden: SSID='%s' (max. %u s)",
             g_rtCfg.ssid.c_str(), WIFI_STEPS * WIFI_STEP_MS / 1000);

    for (uint8_t i = 0; i < WIFI_STEPS; ++i) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_STEP_MS));
        if (WiFi.status() == WL_CONNECTED) break;
    }

    if (WiFi.status() == WL_CONNECTED) {
        ESP_LOGI(TAG, "WiFi OK  IP: %s  RSSI: %d dBm",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI());
        g_wifiReady.store(true);   // signalisiert taskNetRx/Tx: Socket öffnen
    } else {
        ESP_LOGW(TAG, "WiFi-Timeout nach %u s — WiFi deaktiviert, nur UART-Transport aktiv",
                 WIFI_STEPS * WIFI_STEP_MS / 1000);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        g_wifiDisabled.store(true);  // signalisiert taskNetRx/Tx: suspendieren
    }

    vTaskDelete(nullptr);  // Task beendet sich selbst
}

// ── Hilfsfunktionen: Sockets ──────────────────────────────────────────────────
static int createRxSocket() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "RX socket() errno=%d", errno); return -1; }

    struct timeval tv = { 0, 10000 };  // 10 ms Empfangs-Timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int rcvBuf = 65536;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(UDP_LISTEN_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "RX bind() Port %u errno=%d", UDP_LISTEN_PORT, errno);
        close(sock);
        return -1;
    }
    ESP_LOGI(TAG, "UDP-RX gebunden auf Port %u", UDP_LISTEN_PORT);
    return sock;
}

static int createTxSocket() {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "TX socket() errno=%d", errno); return -1; }
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));
    return sock;
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: UDP-RX  (Bridge-IP lernen → g_udpToCanQueue)
// Wartet auf g_wifiReady; suspendiert sich wenn g_wifiDisabled.
// ══════════════════════════════════════════════════════════════════════════════
void taskNetRx(void* /*param*/) {
    // Warten bis WiFi bereit oder deaktiviert
    while (!g_wifiReady.load() && !g_wifiDisabled.load()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (g_wifiDisabled.load()) {
        ESP_LOGI(TAG, "taskNetRx: WiFi deaktiviert — Task suspendiert");
        vTaskSuspend(nullptr);
        return;
    }

    int sock = createRxSocket();
    if (sock < 0) { vTaskDelete(nullptr); return; }

    RawPacket            pkt;
    struct sockaddr_in   senderAddr = {};
    socklen_t            senderLen  = sizeof(senderAddr);

    for (;;) {
        int n = recvfrom(sock, pkt.buf, sizeof(pkt.buf), 0,
                         reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

        if (n < static_cast<int>(MIN_UDP_PACKET)) continue;

        // Zeitstempel aktualisieren — für Timeout-Erkennung in taskNetTx
        s_lastNetRxTick.store(xTaskGetTickCount());

        // Bridge-IP aus Absender lernen (oder bei DHCP-Wechsel aktualisieren)
        const uint32_t newIp = senderAddr.sin_addr.s_addr;
        if (!g_bridgeIpKnown.load()) {
            g_bridgeIpAddr.store(newIp);
            g_bridgeIpKnown.store(true);
            ESP_LOGI(TAG, "Bridge-IP gelernt: %s", inet_ntoa(senderAddr.sin_addr));
        } else if (g_bridgeIpAddr.load() != newIp) {
            g_bridgeIpAddr.store(newIp);
            ESP_LOGW(TAG, "Bridge-IP geaendert (DHCP?): %s", inet_ntoa(senderAddr.sin_addr));
        }

        pkt.len = static_cast<uint16_t>(n);
        if (xQueueSend(g_udpToCanQueue, &pkt, 0) != pdTRUE) {
            ESP_LOGW(TAG, "UDP-RX: Queue voll, Frame verworfen");
        }
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: UDP-TX  (g_canToUdpQueue → bridge.exe)
// Wartet auf g_wifiReady; suspendiert sich wenn g_wifiDisabled.
// Sendet Broadcast (255.255.255.255) solange Bridge-IP unbekannt oder nach Timeout.
// ══════════════════════════════════════════════════════════════════════════════
void taskNetTx(void* /*param*/) {
    while (!g_wifiReady.load() && !g_wifiDisabled.load()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (g_wifiDisabled.load()) {
        // WiFi dauerhaft deaktiviert → Queue endlos drainieren damit CanRx/Simulator
        // nie auf einen vollen g_canToUdpQueue blockieren, obwohl UART-Transport läuft.
        ESP_LOGI(TAG, "taskNetTx: WiFi deaktiviert — UDP-Queue wird dauerhaft geleert");
        RawPacket discard;
        while (true) {
            xQueueReceive(g_canToUdpQueue, &discard, pdMS_TO_TICKS(100));
        }
    }

    int sock = createTxSocket();
    if (sock < 0) { vTaskDelete(nullptr); return; }

    ESP_LOGI(TAG, "UDP-TX bereit — sende Broadcast bis Bridge-IP gelernt");

    bool uartWasActive = false;  // Logging: nur einmal bei Wechsel
    TickType_t lastBcastTick = 0;

    RawPacket pkt;
    for (;;) {
        if (xQueueReceive(g_canToUdpQueue, &pkt, portMAX_DELAY) != pdTRUE) continue;

        // UART-Vorrang: Wenn bridge.exe innerhalb der letzten UART_ACTIVE_TIMEOUT_MS
        // einen UART-Frame gesendet hat, nutzt sie UART als Transport → WiFi unterdrücken.
        {
            const TickType_t lastUart = g_lastUartRxTick.load();
            const bool uartActive = (lastUart != 0) &&
                ((xTaskGetTickCount() - lastUart) <= pdMS_TO_TICKS(UART_ACTIVE_TIMEOUT_MS));

            if (uartActive) {
                if (!uartWasActive) {
                    ESP_LOGI(TAG, "UART aktiv — WiFi-Senden unterdrückt");
                    uartWasActive = true;
                }
                continue;  // Frame verwerfen, UART hat Vorrang
            }
            if (uartWasActive) {
                ESP_LOGI(TAG, "UART inaktiv — WiFi-Senden aktiv");
                uartWasActive = false;
            }
        }

        // Timeout-Prüfung: Bridge-IP vergessen wenn kein Paket seit BRIDGE_TIMEOUT_MS.
        // Danach: Broadcast bis bridge.exe wieder antwortet (DHCP-Wechsel abgedeckt).
        if (g_bridgeIpKnown.load()) {
            const TickType_t lastRx = s_lastNetRxTick.load();
            if (lastRx != 0 &&
                (xTaskGetTickCount() - lastRx) > pdMS_TO_TICKS(BRIDGE_TIMEOUT_MS)) {
                g_bridgeIpKnown.store(false);
                ESP_LOGW(TAG, "Bridge-Timeout (%lus) — Broadcast bis neue Verbindung",
                         (unsigned long)(BRIDGE_TIMEOUT_MS / 1000));
            }
        }

        // Ziel: Unicast (bekannte IP) oder Broadcast (IP unbekannt / Timeout)
        const bool ipKnown = g_bridgeIpKnown.load();
        if (!ipKnown) {
            // Broadcast rate-limitieren: max. 1 Paket pro 500ms um lwIP-pbuf-Pool zu schonen.
            const TickType_t now = xTaskGetTickCount();
            if ((now - lastBcastTick) < pdMS_TO_TICKS(500)) continue;
            lastBcastTick = now;
        }
        const uint32_t destIp = ipKnown ? g_bridgeIpAddr.load() : BRIDGE_BROADCAST_IP;

        struct sockaddr_in dest = {};
        dest.sin_family      = AF_INET;
        dest.sin_port        = htons(UDP_RETURN_PORT);
        dest.sin_addr.s_addr = destIp;

        if (sendto(sock, pkt.buf, pkt.len, 0,
                   reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) < 0) {
            ESP_LOGW(TAG, "UDP-TX sendto() errno=%d", errno);
        }
    }
}

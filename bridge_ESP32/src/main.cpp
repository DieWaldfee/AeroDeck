#include <Arduino.h>
#include <USB.h>
#include <atomic>
#include <esp_log.h>

#include "config.h"
#include "config_manager.h"
#include "bridge_types.h"
#include "net_transport.h"
#include "usb_transport.h"
#include "serial_console.h"
#include "hid_joystick.h"
#include "hid_decoder.h"    // hidMap (extern)
#include "esp32_heartbeat.h"

// ── CAN-FD: Hardware und/oder Simulator ──────────────────────────────────────
// CAN_HW_ENABLED  → MCP2518FD aktiv (taskCanTx + taskCanRx)
// CAN_SIM_INPUTS  → Input-Simulator parallel (Achsen + Buttons, kein Horizon)
// Ohne beides     → reiner Software-Simulator
#ifdef CAN_HW_ENABLED
#  include "can_bridge.h"
#endif
#if !defined(CAN_HW_ENABLED) || defined(CAN_SIM_INPUTS)
#  include "can_simulator.h"
#endif

static const char* TAG = "MAIN";

// ══════════════════════════════════════════════════════════════════════════════
// Globale Definitionen (extern in bridge_types.h deklariert)
// ══════════════════════════════════════════════════════════════════════════════
QueueHandle_t     g_udpToCanQueue = nullptr;
QueueHandle_t     g_canToUdpQueue = nullptr;
QueueHandle_t     g_canToUsbQueue = nullptr;
QueueHandle_t     g_canToHidQueue  = nullptr;
QueueHandle_t     g_consoleRxQueue = nullptr;
QueueHandle_t     g_simCmdQueue   = nullptr;
SemaphoreHandle_t g_canMutex[2]    = { nullptr, nullptr };
TaskHandle_t      g_taskUsbTxHandle = nullptr;

std::atomic<uint32_t> g_bridgeIpAddr{0};
std::atomic<bool>     g_bridgeIpKnown{false};
std::atomic<bool>     g_wifiReady{false};
std::atomic<bool>     g_wifiDisabled{false};
std::atomic<bool>      g_usbTxActive{false};
std::atomic<TickType_t> g_lastUartRxTick{0};

[[noreturn]] static void haltWithError(const char* reason) {
    ESP_LOGE(TAG, "KRITISCHER FEHLER: %s — Neustart in 3 s", reason);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

// ══════════════════════════════════════════════════════════════════════════════
// setup()
//
// Boot-Reihenfolge:
//   t≈  0ms   HID-Interface registrieren (VOR USB.begin()!)
//   t≈  0ms   USB.begin() → nativer USB-Port startet als HID-Joystick
//             Windows sieht sofort: "ESP32 Joystick" (kein CDC auf diesem Port)
//   t≈100ms   Serial.begin() → UART0 (GPIO43/44) → UART-Brücken-Port (CP2102N)
//             bridge.exe öffnet diesen COM-Port
//   t≈150ms   NVS-Config laden
//   t≈350ms   CAN-Module initialisieren
//   t≈400ms   Tasks starten
//             ► taskUsbRx/Tx  → bridge.exe via UART
//             ► taskHidDecoder → HID-Joystick via native USB
//             ► taskWifiManager im Hintergrund
// ══════════════════════════════════════════════════════════════════════════════
void setup() {
    // ── 1. HID registrieren (muss vor USB.begin() erfolgen) ──────────────────
    // Keyboard-Collection (Report ID 2) ist Teil des Joystick-Deskriptors,
    // kein separates begin() nötig.
    g_hidJoystick.begin();

    // ── 2. USB-Stack starten (Joystick Report ID 1 + Keyboard Report ID 2) ───
    USB.begin();
    neopixelWrite(RGB_BUILTIN, 0, 0, 0);   // Framework-USB-Status-LED ausschalten (GPIO48)

    // ── 3. UART-Brücken-Port für bridge.exe + Serial-Konsole ─────────────────
    // Serial0 = UART0 (GPIO43/44, CH343) — explizit, unabhängig von
    // ARDUINO_USB_CDC_ON_BOOT (Board-Default für esp32-s3-devkitc-1 ist =1/CDC).
    Serial0.begin(JOY_UART_BAUD);
    delay(100);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== bridge_ESP32 Boot ===");
#if defined(CAN_HW_ENABLED) && defined(CAN_SIM_INPUTS)
    ESP_LOGI(TAG, "CAN: %u × MCP2518FD + Input-Sim  |  HID: 128 Buttons + 8 Achsen  |  UART: %lu Baud",
             CAN_MODULE_COUNT, (unsigned long)JOY_UART_BAUD);
#elif defined(CAN_HW_ENABLED)
    ESP_LOGI(TAG, "CAN: %u × MCP2518FD  |  HID: 128 Buttons + 8 Achsen  |  UART: %lu Baud",
             CAN_MODULE_COUNT, (unsigned long)JOY_UART_BAUD);
#else
    ESP_LOGI(TAG, "CAN: SIMULATOR (kein MCP2518FD)  |  HID: 128 Buttons + 8 Achsen  |  UART: %lu Baud",
             (unsigned long)JOY_UART_BAUD);
#endif

    // ── 4. Konfiguration aus NVS laden ───────────────────────────────────────
    ConfigManager::load();
    ConfigManager::loadDecodeTable(decodeTable, decodeTableSize);  // Button-Decode-Tabelle aus NVS
    ConfigManager::loadAxisMap(axisMap, axisMapSize);              // Achsen-Map aus NVS

    // ── 5. FreeRTOS-Objekte anlegen ──────────────────────────────────────────
    g_udpToCanQueue  = xQueueCreate(QUEUE_DEPTH, sizeof(RawPacket));
    g_canToUdpQueue  = xQueueCreate(QUEUE_DEPTH, sizeof(RawPacket));
    g_canToUsbQueue  = xQueueCreate(QUEUE_DEPTH, sizeof(RawPacket));
    g_canToHidQueue  = xQueueCreate(QUEUE_DEPTH, sizeof(RawPacket));
    g_consoleRxQueue = xQueueCreate(128, sizeof(uint8_t));
#if defined(CAN_HW_ENABLED) && defined(CAN_SIM_INPUTS)
    g_simCmdQueue    = xQueueCreate(QUEUE_DEPTH, sizeof(RawPacket));
#endif
    for (uint8_t i = 0; i < CAN_MODULE_COUNT; ++i)
        g_canMutex[i] = xSemaphoreCreateMutex();

    if (!g_udpToCanQueue || !g_canToUdpQueue || !g_canToUsbQueue || !g_canToHidQueue
        || !g_consoleRxQueue
#if defined(CAN_HW_ENABLED) && defined(CAN_SIM_INPUTS)
        || !g_simCmdQueue
#endif
        || !g_canMutex[0] || (CAN_MODULE_COUNT > 1 && !g_canMutex[1])) {
        haltWithError("FreeRTOS-Objekte: Heap voll");
    }

    // ── 6. CAN-FD: Hardware und/oder Input-Simulator ─────────────────────────
#ifdef CAN_HW_ENABLED
    const bool canHwOk = canBridgeInit();
    if (!canHwOk)
        ESP_LOGW(TAG, "MCP2518FD nicht verfügbar — Betrieb ohne CAN-Hardware");
#else
    constexpr bool canHwOk = false;
#endif
#if !defined(CAN_HW_ENABLED) || defined(CAN_SIM_INPUTS)
    canSimulatorInit();   // Achsen/Button-Simulator + Startmeldung
#endif

    // ── 7. Tasks starten ──────────────────────────────────────────────────────
    //
    // Prioritäten:
    //   6 = CanRx  (höchste: CAN-Timing)
    //   5 = CanTx, NetRx, NetTx, UsbRx, UsbTx
    //   3 = HidDecoder
    //   2 = WifiManager (Hintergrund)
    //   1 = SerialConsole (interaktiv, nicht zeitkritisch)

    BaseType_t ok = pdTRUE;

    // WiFi-Manager: Hintergrund
    ok &= xTaskCreatePinnedToCore(taskWifiManager,   "WiFiMgr",  4096,
                                  nullptr, 2, nullptr, 1);

    // Netzwerk-Tasks (warten intern auf g_wifiReady)
    ok &= xTaskCreatePinnedToCore(taskNetRx,         "NetRx",    TASK_STACK_NET,
                                  nullptr, TASK_PRIO_NET_RX, nullptr, 1);
    ok &= xTaskCreatePinnedToCore(taskNetTx,         "NetTx",    TASK_STACK_NET,
                                  nullptr, TASK_PRIO_NET_TX, nullptr, 1);

    // UART-Transport-Tasks (→ bridge.exe, sofort aktiv)
    ok &= xTaskCreatePinnedToCore(taskUsbRx,         "UartRx",   TASK_STACK_NET,
                                  nullptr, TASK_PRIO_NET_RX, nullptr, 1);
    ok &= xTaskCreatePinnedToCore(taskUsbTx,         "UartTx",   TASK_STACK_NET,
                                  nullptr, TASK_PRIO_NET_TX, &g_taskUsbTxHandle, 1);

    // CAN-Hardware-Tasks (nur wenn MCP2518FD-Init erfolgreich)
#ifdef CAN_HW_ENABLED
    if (canHwOk) {
        ok &= xTaskCreatePinnedToCore(taskCanTx,    "CanTx",  TASK_STACK_CAN,
                                      nullptr, TASK_PRIO_CAN_TX, nullptr, 1);
        ok &= xTaskCreatePinnedToCore(taskCanRx,    "CanRx",  TASK_STACK_CAN,
                                      nullptr, TASK_PRIO_CAN_RX, nullptr, 1);
    }
#endif
    // Input-Simulator (Achsen + Buttons) — parallel zur Hardware oder als Vollersatz
#if !defined(CAN_HW_ENABLED) || defined(CAN_SIM_INPUTS)
    ok &= xTaskCreatePinnedToCore(taskCanSimulator, "CanSim", TASK_STACK_CAN,
                                  nullptr, TASK_PRIO_CAN_TX, nullptr, 1);
#endif

    // ESP32-eigener Heartbeat → bridge.exe (CAN-ID ESP32_HB_CAN_ID)
    ok &= xTaskCreatePinnedToCore(taskEsp32Heartbeat, "Esp32Hb", 2048,
                                  nullptr, TASK_PRIO_NET_TX, nullptr, 1);

    // HID-Decoder: CAN-Frames → Joystick-State → native USB
    ok &= xTaskCreatePinnedToCore(taskHidDecoder,    "HidDec",   TASK_STACK_HID,
                                  nullptr, TASK_PRIO_HID_DEC, nullptr, 1);

    // Serial-Konsole: UART-Port, niedrigste Prio
    ok &= xTaskCreatePinnedToCore(taskSerialConsole, "SerCon",   4096,
                                  nullptr, 1, nullptr, 1);

    if (ok != pdTRUE) haltWithError("Task-Erstellung fehlgeschlagen");

    ESP_LOGI(TAG, "Alle Tasks gestartet — UART + HID bereit, WiFi im Hintergrund");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGD(TAG, "Heap: %lu B frei (Min: %lu B)  UART:%s  WiFi:%s  HID:aktiv",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             g_usbTxActive.load()  ? "aktiv" : "---",
             g_wifiReady.load()    ? "aktiv" :
             g_wifiDisabled.load() ? "off"   : "warte");
}

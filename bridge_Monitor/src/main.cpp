#include <windows.h>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <algorithm>
#include "config.h"
#include "module_registry.h"
#include "udp_socket.h"
#include "console_ui.h"
#include "monitor_protocol.h"

static std::atomic<bool> g_running{ true };

static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) { g_running = false; return TRUE; }
    return FALSE;
}

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    SetConsoleTitleA("Bridge Monitor");

    // ── Konfiguration laden ───────────────────────────────────────────────────
    std::string iniPath = "bridge.ini";
    if (argc > 1) iniPath = argv[1];

    Config cfg;
    if (!cfg.load(iniPath)) {
        printf("Fehler: bridge.ini nicht gefunden: %s\n", iniPath.c_str());
        printf("bridge_monitor benoetigt bridge.ini im gleichen Verzeichnis.\n");
        return 1;
    }
    if (cfg.modules.empty()) {
        printf("Fehler: Keine Module in bridge.ini konfiguriert.\n");
        return 1;
    }

    if (!cfg.enabled) {
        printf("Warnung: [monitor] enabled = false in bridge.ini\n");
        printf("bridge_exe sendet keine Monitor-Daten. Auf 'enabled = true' setzen.\n");
        printf("Starte trotzdem im Beobachtungsmodus...\n");
        Sleep(2000);
    }

    // ── Module-Registry initialisieren ───────────────────────────────────────
    ModuleRegistry registry;
    registry.init(cfg.modules);

    int maxSimvars = 0, maxConfig = 0, maxButtons = 0, maxAxes = 0;
    for (const auto& mod : cfg.modules) {
        maxSimvars = std::max(maxSimvars, (int)mod.simvars.size());
        maxConfig  = std::max(maxConfig,  (int)mod.config.size());
        maxButtons = std::max(maxButtons, (int)mod.buttons.size());
        maxAxes    = std::max(maxAxes,   (int)mod.axes.size());
    }

    // ── UDP-Socket öffnen ─────────────────────────────────────────────────────
    UdpSocket udp;
    const std::string bridgeHost = (cfg.ip == "0.0.0.0" || cfg.ip.empty())
                                   ? "127.0.0.1" : cfg.ip;
    if (!udp.open(cfg.port, bridgeHost, cfg.cmdPort)) {
        printf("Fehler: UDP-Socket konnte nicht auf Port %d geoeffnet werden.\n", cfg.port);
        printf("Laeuft bereits eine andere Instanz von bridge_monitor?\n");
        return 1;
    }

    // ── Konsolen-UI initialisieren ────────────────────────────────────────────
    ConsoleUi ui;
    ui.init((int)cfg.modules.size(), maxSimvars, maxConfig, maxButtons, maxAxes);
    ui.setUnitSizes(cfg.unitSizes);

    // ── Haupt-Zustand ─────────────────────────────────────────────────────────
    MonitorState monState;
    monState.port     = cfg.port;
    monState.cmdPort  = cfg.cmdPort;
    monState.bridgeIp = bridgeHost;

    int selectedIdx = 0;

    long long lastDataMs = 0;
    constexpr long long DATA_TIMEOUT_MS = 3000;

    long long lastLwtSentMs = 0;
    long long lastPongMs    = 0;

    long long fpsTs    = nowMs();
    int       fpsCount = 0;

    // ── Hauptschleife ─────────────────────────────────────────────────────────
    while (g_running) {
        long long now = nowMs();

        // ── Periodischer LWT-Heartbeat → bridge_exe ───────────────────────────
        if (now - lastLwtSentMs >= cfg.lwtMs) {
            lastLwtSentMs = now;
            uint32_t ts = (uint32_t)(now & 0xFFFFFFFFu);
            udp.sendCommand(MON_LWT, ts);
        }

        // ── UDP-Pakete empfangen ──────────────────────────────────────────────
        {
            uint8_t  type, payload[64];
            uint32_t canId;
            size_t   payLen;
            while (udp.tryReceive(type, canId, payload, payLen)) {
                lastDataMs = now;
                ++fpsCount;

                switch (type) {
                    case MON_BRIDGE_PONG: {
                        uint32_t echoTs = canId;
                        int rtt = (int)((uint32_t)(now & 0xFFFFFFFFu) - echoTs);
                        lastPongMs              = now;
                        monState.bridgeOnline   = true;
                        monState.bridgeEverSeen = true;
                        monState.rttMs          = (rtt >= 0 && rtt < 9999) ? rtt : -1;
                        break;
                    }
                    case MON_FRAME_FORWARD:
                        registry.updateFrame(canId, payload, payLen, now);
                        break;
                    case MON_RX_FRAME:
                        registry.updateRxFrame(canId, payload, payLen, now);
                        break;
                    case MON_FEEDBACK_FORWARD:
                        registry.updateFeedback(canId, payload, payLen, now);
                        break;
                    case MON_STATUS_CHANGE:
                        if (payLen >= 1)
                            registry.updateStatus(canId, payload[0] != 0);
                        break;
                    default:
                        break;
                }
            }
        }

        // ── Bridge-Verbindungs-Timeout prüfen ─────────────────────────────────
        if (monState.bridgeEverSeen && monState.bridgeOnline) {
            if (now - lastPongMs > cfg.timeoutMs)
                monState.bridgeOnline = false;
        }
        monState.bridgeSilenceSecs = (monState.bridgeEverSeen && !monState.bridgeOnline)
                                     ? (now - lastPongMs) / 1000LL : 0LL;

        monState.connected = (lastDataMs > 0 && (now - lastDataMs) < DATA_TIMEOUT_MS);

        // ── Hz-Messung ────────────────────────────────────────────────────────
        if (now - fpsTs >= 1000) {
            monState.hz = (float)fpsCount * 1000.f / (float)(now - fpsTs);
            fpsCount    = 0;
            fpsTs       = now;
        }

        // ── Tastatureingaben ──────────────────────────────────────────────────
        UiEvent ev = ui.pollInput();
        switch (ev) {
            case UiEvent::SELECT_UP:
                if (selectedIdx > 0) --selectedIdx;
                break;
            case UiEvent::SELECT_DOWN:
                if (selectedIdx < registry.count() - 1) ++selectedIdx;
                break;
            case UiEvent::PULL:
                if (selectedIdx >= 0 && selectedIdx < registry.count()) {
                    udp.sendCommand(MON_POLL_REQUEST, registry.stateAt(selectedIdx).cfg->canTxId);
                    monState.pullSent      = true;
                    monState.pullSentMs    = now;
                    monState.pullModuleIdx = selectedIdx;
                }
                break;
            case UiEvent::QUIT:
                g_running = false;
                break;
            default:
                break;
        }

        ui.update(registry, monState, selectedIdx, 100);

        Sleep(1);
    }

    udp.close();
    return 0;
}

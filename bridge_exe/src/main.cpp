#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstring>
#include <string>
#include <conio.h>
#include "config.h"
#include "simconnect_client.h"
#include "can_frame.h"
#include "transport.h"
#include "udp_sender.h"
#include "serial_transport.h"
#include "debug_console.h"
#include "file_logger.h"
#include "error_logger.h"
#include "monitor_forwarder.h"
#include "configure_mode.h"
#include "ini_checker.h"

static std::atomic<bool> g_running{ true };

static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) { g_running = false; return TRUE; }
    return FALSE;
}

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static std::string toConsoleCp(const std::string& s) {
    if (s.empty()) return s;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return s;
    std::wstring w(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], wlen);
    UINT cp = GetConsoleOutputCP();
    int alen = WideCharToMultiByte(cp, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (alen <= 0) return s;
    std::string r(alen, 0);
    WideCharToMultiByte(cp, 0, w.c_str(), -1, &r[0], alen, nullptr, nullptr);
    if (!r.empty() && r.back() == '\0') r.pop_back();
    return r;
}

// ── Laufzeit-Zustand pro Modul (vereint Output-Rückkanal und Input-Seite) ─────
struct ModuleRuntime {
    const ModuleConfig* mod;

    // Rückkanal (Modul → bridge, auf mod->canRxId)
    // lastHeartbeatMs: Zeitstempel des letzten Heartbeats — Grundlage für Online/Offline.
    // Abweichung im Monitor: bridge_monitor nutzt zusätzlich lastFeedbackMs für
    // Latenz-Diagnose ("vor X ms") — gewollte Sonderrolle für die Fehlersuche.
    long long lastHeartbeatMs = 0;
    bool      online          = false;
    bool      offlineLogged   = false;
    long long wentOfflineMs   = 0;
    int       lastRttMs       = -1;
    uint32_t  lastEspTickMs   = 0;   // ESP32-Tick aus Heartbeat-Payload, für Reboot-Erkennung

    // Generische Feedback-Daten aus letzter STATUS_RESPONSE (INI-Reihenfolge)
    std::vector<double>      lastFeedbackValues;
    std::vector<std::string> lastFeedbackNames;
    std::vector<std::string> lastFeedbackUnits;

    // Input-Seite (CAN → HID, nur wenn mod->hasInput())
    long long lastRxMs        = 0;
    bool      everSeen        = false;
    int16_t   axisValues[8]   = {};
};

// ── Kommandozeilen-Modus ──────────────────────────────────────────────────────
struct RunMode {
    bool        showHelp        = false;
    bool        debugConsole    = false;
    std::string debugModule;
    bool        logToFile       = false;
    std::string logFile         = "bridgeDump.log";
    bool        configure       = false;
    bool        iniCheck        = false;
};

static RunMode parseArgs(int argc, char** argv) {
    RunMode rm;
    if (argc < 2) return rm;

    std::string first(argv[1]);
    if (first == "help" || first == "/h" || first == "/?") {
        rm.showHelp = true;
        return rm;
    }

    if (first == "configure") {
        rm.configure = true;
        return rm;
    }

    if (first == "inicheck") {
        rm.iniCheck = true;
        return rm;
    }

    if (first == "debug") {
        rm.debugConsole = true;
        for (int i = 2; i < argc; ++i) {
            std::string a(argv[i]);
            if (a == "toFile") {
                rm.logToFile = true;
                if (i + 1 < argc) {
                    std::string n(argv[i + 1]);
                    if (n != "debug" && n != "toFile" && n != "help" &&
                        n != "/h"   && n != "/?") {
                        rm.logFile = n;
                        ++i;
                    }
                }
            } else if (rm.debugModule.empty()) {
                rm.debugModule = a;
            }
        }
    }
    return rm;
}

static void printHelp() {
    printf(
        "\n"
        "MSFS SimConnect Bridge\n"
        "Liest Flugdaten aus MSFS ueber SimConnect und sendet sie per UDP\n"
        "an einen ESP32-S3, der sie als CAN-Pakete an Anzeigegeraete verteilt.\n"
        "\n"
        "Verwendung:\n"
        "  bridge.exe                            Normalbetrieb (keine Ausgabe)\n"
        "  bridge.exe configure                  ESP32 interaktiv konfigurieren\n"
        "  bridge.exe inicheck                   bridge.ini pruefen -> inicheck.log\n"
        "  bridge.exe debug <name>               Debug-Anzeige fuer Modul <name>\n"
        "  bridge.exe debug toFile [datei]       Alle Daten in Datei protokollieren\n"
        "  bridge.exe debug <name> toFile [d]    Anzeige und Protokoll kombiniert\n"
        "  bridge.exe help | /h | /?             Diese Hilfe anzeigen\n"
        "\n"
        "Parameter:\n"
        "  <name>    Modul-Schluessel aus bridge.ini  (z.B. horizon)\n"
        "  [datei]   Protokolldatei  (Standard: bridgeDump.log)\n"
        "\n"
        "Beispiele:\n"
        "  bridge.exe debug horizon\n"
        "  bridge.exe debug toFile\n"
        "  bridge.exe debug toFile messung_01.log\n"
        "  bridge.exe debug horizon toFile debug.log\n"
        "\n"
        "Konfiguration:\n"
        "  bridge.ini liegt im gleichen Verzeichnis wie bridge.exe.\n"
        "  Alle Module, SimVars und Verbindungsparameter sind dort definiert.\n"
        "\n"
    );
}

// ── main ─────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        COORD  origin = {0, 0};
        DWORD  written;
        if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
            DWORD cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
            FillConsoleOutputCharacterA(hOut, ' ',              cells, origin, &written);
            FillConsoleOutputAttribute (hOut, csbi.wAttributes, cells, origin, &written);
        }
        SetConsoleCursorPosition(hOut, origin);
    }

    RunMode rm = parseArgs(argc, argv);
    if (rm.showHelp) { printHelp(); return 0; }
    if (rm.iniCheck) { return runIniCheck("bridge.ini"); }

    // ── 1. Konfiguration laden ────────────────────────────────────────────────
    // bridge.ini wird ausschließlich im aktuellen Arbeitsverzeichnis (.\) gesucht.
    Config cfg;
    if (!cfg.load("bridge.ini")) {
        printf("Fehler: bridge.ini nicht gefunden im aktuellen Verzeichnis.\n");
        return 1;
    }

    // ── ErrorLogger früh initialisieren (wird ab Schritt 5 benötigt) ──────────
    ErrorLogger errLog;
    errLog.setPaths("BridgeError.log", "BridgeStatus.log");
    for (const auto& w : cfg.warnings)
        errLog.logTransportError("CONFIG", "---", w);

    if (rm.configure) { runConfigureMode(cfg); return 0; }

    // ── 2. Debug-Target suchen ────────────────────────────────────────────────
    const ModuleConfig* primaryMod = nullptr;

    if (rm.debugConsole && !rm.debugModule.empty()) {
        for (const auto& mod : cfg.modules)
            if (mod.iniKey == rm.debugModule) { primaryMod = &mod; break; }
        if (!primaryMod) {
            printf("Fehler: '%s' nicht in bridge.ini gefunden.\n",
                   rm.debugModule.c_str());
            if (!cfg.modules.empty()) {
                printf("Verfuegbare Module:\n");
                for (const auto& mod : cfg.modules)
                    printf("  %-14s  %s\n", mod.iniKey.c_str(), toConsoleCp(mod.description).c_str());
            }
            return 1;
        }
    } else if (rm.debugConsole && rm.debugModule.empty() && !rm.logToFile) {
        printf("Fehler: 'debug' benoetigt einen Namen oder 'toFile'.\n");
        printf("Verwendung: bridge.exe debug <name> | bridge.exe debug toFile [datei]\n");
        if (!cfg.modules.empty()) {
            printf("Verfuegbare Module:\n");
            for (const auto& mod : cfg.modules)
                printf("  %-14s  %s\n", mod.iniKey.c_str(), mod.description.c_str());
        }
        return 1;
    }

    if (cfg.modules.empty()) {
        printf("Fehler: Keine Module in bridge.ini konfiguriert.\n");
        return 1;
    }

    if (!primaryMod)
        primaryMod = &cfg.modules[0];

    // ── 3. Transport öffnen (USB wenn usb_port gesetzt und Port verfügbar, sonst UDP) ──
    std::unique_ptr<ITransport> transport;
    char transportTarget[64];

    if (!cfg.usbPort.empty()) {
        // ── USB-Serial-Transport (bridge.ini: usb_port = COMx) ───────────────
        auto ser = std::make_unique<SerialTransport>();
        if (!ser->open(cfg.usbPort, cfg.usbBaud)) {
            errLog.logTransportError("USB-PORT", cfg.usbPort,
                "USB not available at startup - fallback to UDP "
                + cfg.udpIp + ":" + std::to_string(cfg.udpPort));
        } else {
            printf("USB-Transport: %s  %d Baud\n", cfg.usbPort.c_str(), cfg.usbBaud);
            snprintf(transportTarget, sizeof(transportTarget),
                     "USB:%s", cfg.usbPort.c_str());
            transport = std::move(ser);
        }
    }
    if (!transport) {
        // ── UDP-Transport (Standard) ──────────────────────────────────────────
        auto udp = std::make_unique<UdpTransport>();
        if (!udp->open(cfg.udpIp, cfg.udpPort)) {
            printf("UDP-Fehler: %s\n", udp->lastError().c_str());
            return 1;
        }
        if (!udp->openReceive(cfg.receivePort))
            printf("Warnung: Empfangs-Socket nicht geoeffnet: %s\n",
                   udp->lastError().c_str());
        snprintf(transportTarget, sizeof(transportTarget), "%s:%d",
                 cfg.udpIp.c_str(), cfg.udpPort);
        transport = std::move(udp);
    }

    // ── usingSerial: true wenn aktiver Transport ein Serial-Port ist ─────────
    bool usingSerial = dynamic_cast<SerialTransport*>(transport.get()) != nullptr;

    // Nicht-besitzender Zeiger auf den aktiven UDP-Transport (nullptr wenn Serial aktiv).
    // Erlaubt Live-Zugriff auf destStr() und resetToBroadcast() ohne dynamic_cast in der Schleife.
    UdpTransport* udpXport = dynamic_cast<UdpTransport*>(transport.get());

    // Wiederholte Init-Retries koennen dieselbe Mapping-Tabelle mehrfach senden.
    // Diese Meldung daher nur beim ersten Senden und bei geaenderter Eintragszahl ausgeben.
    size_t lastLoggedDecodeEntries = static_cast<size_t>(-1);

    // ── HID-Mapping senden: RESET + ENTRY×N + COMMIT + AXIS×N ───────────────────
    // Aufgerufen von sendInitAll() beim Start und bei Reboot-Erkennung (espRebooted).
    // Kein globaler Zustand — Aufruf-Kontrolle liegt vollständig beim Aufrufer.
    auto sendHidMapping = [&]() {
        uint8_t mf[22];
        size_t totalEntries = 0;
        memset(mf, 0, sizeof(mf));
        mf[0] = 0xFE; mf[1] = 0xFF;
        transport->send(HID_MAP_CAN_ID, mf, 2);
        for (const auto& mod : cfg.modules) {
            for (const auto& btn : mod.buttons) {
                memset(mf, 0, sizeof(mf));
                mf[0] = 0xFE; mf[1] = 0x01;
                mf[2] = (uint8_t)(mod.canRxId >>  0);
                mf[3] = (uint8_t)(mod.canRxId >>  8);
                mf[4] = (uint8_t)(mod.canRxId >> 16);
                mf[5] = (uint8_t)(mod.canRxId >> 24);
                mf[6]  = btn.payloadByte;
                mf[7]  = btn.payloadBit & 0x07u;
                mf[8]  = btn.hidA;
                mf[9]  = btn.hidB;
                mf[10] = btn.physicalIdx;
                mf[11] = btn.kbMod;
                const size_t nameLen = std::min(btn.name.size(), (size_t)9);
                memcpy(mf + 12, btn.name.c_str(), nameLen);
                mf[12 + nameLen] = '\0';
                transport->send(HID_MAP_CAN_ID, mf, 22);
                ++totalEntries;
            }
        }
        memset(mf, 0, sizeof(mf));
        mf[0] = 0xFE; mf[1] = 0xFE;
        transport->send(HID_MAP_CAN_ID, mf, 2);
        if (totalEntries > 0 && !rm.debugConsole && totalEntries != lastLoggedDecodeEntries) {
            printf("HID-Mapping: %zu Button-Eintraege uebertragen.\n", totalEntries);
            lastLoggedDecodeEntries = totalEntries;
        }
        for (const auto& mod : cfg.modules) {
            if (mod.axes.empty()) continue;
            const uint8_t axCnt = (uint8_t)std::min(mod.axes.size(), (size_t)8);
            uint8_t af[16] = {};
            af[0] = 0xFE; af[1] = 0x02;
            af[2] = (uint8_t)(mod.canRxId >>  0);
            af[3] = (uint8_t)(mod.canRxId >>  8);
            af[4] = (uint8_t)(mod.canRxId >> 16);
            af[5] = (uint8_t)(mod.canRxId >> 24);
            af[6] = axCnt;
            uint8_t axKbMod = 0;
            for (uint8_t i = 0; i < axCnt; i++) {
                af[7 + i] = mod.axes[i].hidAxisIdx;
                axKbMod  |= mod.axes[i].kbMod;
            }
            af[7 + axCnt] = axKbMod;
            transport->send(HID_MAP_CAN_ID, af, 8 + axCnt);
        }
        mf[0] = 0xFE; mf[1] = 0xFD;
        transport->send(HID_MAP_CAN_ID, mf, 2);
    };

    // ── Alle Init-Pakete senden (bei Start und Transport-Wechsel) ────────────────
    // HID-Mapping wird NICHT hier gesendet — erst nach dem ersten Heartbeat vom ESP32
    // (triggerInit/firstTime), um sicherzustellen dass der ESP32 empfangsbereit ist.
    auto sendInitAll = [&]() {
        uint8_t initBuf[64];
        for (const auto& mod : cfg.modules) {
            if (!mod.hasOutput()) continue;
            size_t initLen = assembleInitPacket(mod, initBuf, sizeof(initBuf));
            if (initLen > 0)
                transport->send(mod.canTxId, initBuf, initLen);
            size_t cfgLen = assembleConfigPacket(mod, initBuf, sizeof(initBuf));
            if (cfgLen > 0)
                transport->send(mod.canTxId, initBuf, cfgLen);
        }
        for (const auto& mod : cfg.modules) {
            if (mod.heartbeatIntervalMs <= 0) continue;
            uint8_t hbCfg[5];
            hbCfg[0] = 0xFD;
            uint32_t iv = (uint32_t)mod.heartbeatIntervalMs;
            std::memcpy(hbCfg + 1, &iv, 4);
            transport->send(mod.canTxId, hbCfg, sizeof(hbCfg));
        }
    };

    // HID-Mapping-Trigger: ESP32 fordert Mapping aktiv per REQUEST an (CAN-ID 0x7F2).
    // Debounce verhindert Mehrfach-Sends bei schnell aufeinanderfolgenden REQUESTs
    // (ESP32-Retry-Intervall = MAP_REQUEST_INTERVAL_MS = 3000 ms, hardcoded im Firmware).
    long long hidMappingLastSentMs = -10000LL;   // weit in Vergangenheit → erster Send sofort erlaubt

    sendInitAll();

    // ── 4. Laufzeitzustände aufbauen (ein Runtime-Eintrag pro Modul) ──────────
    std::vector<ModuleRuntime> runtimes;
    for (const auto& mod : cfg.modules)
        runtimes.push_back({&mod});

    // deviceCount = alle Module + Monitor (für Debug-Status-Block)
    int deviceCount = (cfg.monitorEnabled ? 1 : 0) + (int)cfg.modules.size();

    // ── 5. MonitorForwarder öffnen (wenn aktiviert) ───────────────────────────
    MonitorForwarder monitor;
    if (cfg.monitorEnabled) {
        if (!monitor.open(cfg.monitorIp, cfg.monitorPort, cfg.monitorCmdPort)) {
            printf("Warnung: Monitor-Socket konnte nicht geöffnet werden.\n");
            errLog.logTransportError("UDP-ERROR",
                cfg.monitorIp + ":" + std::to_string(cfg.monitorPort),
                "cannot open monitor forward socket");
        }
    }

    // ── 7. Debug-Konsole initialisieren ───────────────────────────────────────
    DebugConsole dbg;

    if (rm.debugConsole && primaryMod) {
        dbg.init(cfg.refreshMs,
                 (int)primaryMod->simvars.size(),
                 deviceCount,
                 (int)primaryMod->axes.size(),
                 (int)primaryMod->buttons.size());
    }

    // ── 8. File-Logger öffnen ─────────────────────────────────────────────────
    FileLogger logger;
    if (rm.logToFile) {
        if (!logger.open(rm.logFile, cfg, transportTarget))
            printf("Warnung: Konnte Protokolldatei '%s' nicht oeffnen.\n", rm.logFile.c_str());
        else
            printf("Protokoll: %s\n", rm.logFile.c_str());
    }

    // ── 9. SimConnect + Frames vorbereiten ────────────────────────────────────
    SimConnectClient sim;
    SimData          currentData;

    // Frames nur für Module mit Output-Fähigkeit
    struct FrameEntry { const ModuleConfig* mod; uint8_t frame[64]; size_t size; };
    std::vector<FrameEntry> frames;
    for (const auto& mod : cfg.modules) {
        if (!mod.hasOutput()) continue;
        FrameEntry fe;
        fe.mod  = &mod;
        fe.size = 0;
        std::memset(fe.frame, 0, 64);
        frames.push_back(fe);
    }

    // ── 10. Debug-State ───────────────────────────────────────────────────────
    DebugState ds;
    ds.transportTarget = transportTarget;
    if (primaryMod) ds.primaryMod = *primaryMod;
    ds.moduleCount = (int)frames.size();
    ds.status          = BridgeStatus::WAITING;

    BridgeStatus prevStatus = BridgeStatus::WAITING;

    // ── 11. Zeitvariablen + Monitor-Laufzeitzustand ───────────────────────────
    bool      prevSimConnected = false;
    long long lastSendMs       = 0;
    long long fpsTs            = nowMs();
    int       fpsCount         = 0;
    long long bridgeStartMs    = nowMs();
    std::string lastUdpError;

    // Monitor-Verbindungszustand (gesteuert via MON_LWT / Timeout)
    long long monLastLwtMs    = 0;
    int       monLastRttMs    = -1;
    bool      monOnline       = false;
    bool      monEverSeen     = false;
    bool      monOffLogged    = false;
    long long monWentOffMs    = 0;

    long long lastSerialRetryMs  = 0;
    long long lastUdpInitMs      = nowMs();
    long long statusMsgClearMs   = 0;   // Zeitpunkt ab dem statusMsg gelöscht wird
    bool      startupCheckDone   = false;

    constexpr long long SERIAL_RETRY_MS  = 3000;   // Intervall für Serial-Reconnect-Versuche
    constexpr long long UDP_INIT_RETRY_MS = 5000;  // Intervall für sendInitAll() im UDP-Modus
    constexpr long long STARTUP_GRACE_MS = 10000;  // 10 s Anlaufzeit vor Timeout-Überwachung

    // ── 12. SimConnect im Hintergrund starten (non-blocking) ─────────────────
    // SimConnect_Open blockiert 3-4 s wenn MSFS nicht läuft — daher eigener Thread.
    sim.startAsync(cfg.appName, cfg.modules);

    // ── 13. Hauptschleife ─────────────────────────────────────────────────────
    while (g_running) {
        long long now = nowMs();

        // ── SimConnect: Status vom Hintergrund-Thread abholen (non-blocking) ────
        {
            const bool nowConn = sim.isConnected();
            if (nowConn && !prevSimConnected) {
                // Erstverbindung oder Reconnect
                currentData = sim.getData();  // Pool sofort verfügbar → has() korrekt
                ds.status   = BridgeStatus::CONNECTED;
            } else if (!nowConn && prevSimConnected) {
                // Verbindung zu MSFS verloren
                ds.status = BridgeStatus::DISCONNECTED;
                errLog.logTransportError("SIMCONNECT", "---", "MSFS disconnected");
            } else if (!nowConn) {
                ds.status = BridgeStatus::WAITING;
            }
            prevSimConnected = nowConn;
        }

        if (ds.status != prevStatus) {
            if (logger.isOpen()) {
                const char* names[] = { "WAITING", "CONNECTED", "DISCONNECTED" };
                logger.logStatus(names[(int)ds.status]);
            }
            prevStatus = ds.status;
        }

        // Neue SimVar-Daten abholen (non-blocking — kommen asynchron vom Thread)
        if (sim.hasNewData()) {
            currentData = sim.getData();
            ++fpsCount;
            if (now - fpsTs >= 1000) {
                ds.hz    = (float)fpsCount * 1000.f / (float)(now - fpsTs);
                fpsCount = 0;
                fpsTs    = now;
            }
        }

        // ── Transport-Umschaltung Serial ↔ UDP (nur wenn usb_port konfiguriert) ─
        if (!cfg.usbPort.empty()) {
            // Serial-Port verschwunden → auf UDP zurückfallen
            if (usingSerial && !transport->isOpen()) {
                transport->close();
                auto udp = std::make_unique<UdpTransport>();
                if (udp->open(cfg.udpIp, cfg.udpPort)) {
                    if (!udp->openReceive(cfg.receivePort))
                        errLog.logTransportError("UDP-PORT", cfg.udpIp,
                            "Empfangs-Socket konnte nicht gebunden werden: "
                            + udp->lastError());
                    transport   = std::move(udp);
                    usingSerial = false;
                    udpXport    = dynamic_cast<UdpTransport*>(transport.get());
                    snprintf(transportTarget, sizeof(transportTarget), "%s:%d",
                             cfg.udpIp.c_str(), cfg.udpPort);
                    ds.transportTarget = transportTarget;
                    char msg[80];
                    snprintf(msg, sizeof(msg), "USB %s getrennt -> Fallback auf UDP %s:%d",
                             cfg.usbPort.c_str(), cfg.udpIp.c_str(), cfg.udpPort);
                    if (rm.debugConsole) {
                        ds.statusMsg = msg;
                        statusMsgClearMs = now + 5000;
                    } else {
                        printf("%s\n", msg);
                    }
                    errLog.logTransportError("USB-PORT", cfg.usbPort,
                        "USB disconnected - fallback to UDP "
                        + cfg.udpIp + ":" + std::to_string(cfg.udpPort));
                    sendInitAll();
                    lastUdpInitMs = now;
                }
                lastSerialRetryMs = now;
            }
            // UDP aktiv → periodisch auf Serial-Port prüfen
            if (!usingSerial && (now - lastSerialRetryMs >= SERIAL_RETRY_MS)) {
                lastSerialRetryMs = now;
                auto ser = std::make_unique<SerialTransport>();
                if (ser->open(cfg.usbPort, cfg.usbBaud)) {
                    transport->close();
                    transport   = std::move(ser);
                    usingSerial = true;
                    udpXport    = nullptr;
                    snprintf(transportTarget, sizeof(transportTarget),
                             "USB:%s", cfg.usbPort.c_str());
                    ds.transportTarget = transportTarget;
                    char msg[80];
                    snprintf(msg, sizeof(msg), "USB %s %d Baud aktiv",
                             cfg.usbPort.c_str(), cfg.usbBaud);
                    if (rm.debugConsole) {
                        ds.statusMsg = msg;
                        statusMsgClearMs = now + 5000;
                    } else {
                        printf("%s\n", msg);
                    }
                    errLog.logTransportError("USB-PORT", cfg.usbPort,
                        "USB reconnected - transport restored from UDP");
                    sendInitAll();
                }
            }
        }

        // ── UDP-Init-Retry: sendInitAll() periodisch wiederholen bis mind. ein Modul online ─
        // Feuert alle UDP_INIT_RETRY_MS so lange kein Modul aktuell online ist.
        // Abdeckt: Startup ohne USB, USB→UDP-Wechsel während ESP32 noch WiFi aufbaut (~5 s).
        if (!usingSerial && (now - lastUdpInitMs >= UDP_INIT_RETRY_MS)) {
            const bool anyCurrentlyOnline = std::any_of(runtimes.begin(), runtimes.end(),
                                                        [](const ModuleRuntime& r){ return r.online; });
            if (!anyCurrentlyOnline) {
                lastUdpInitMs = now;
                sendInitAll();
            }
        }

        // ── Rückkanal empfangen ───────────────────────────────────────────────
        // Protokoll: ALLE Frames kommen auf canRxId zurück (bridge.ini: can_rx_id).
        //   Real-Hardware: canRxId = canTxId | 0x80  (z.B. 0x620 → 0x6A0)
        //   Simulator:     sendet Heartbeats + Spontandaten auf canRxId (RET-IDs)
        //
        // Frame-Typen (Byte 0 = type):
        //   0x01  HEARTBEAT:       [type][uptime_ms 4B LE]
        //   0x02  DATEN:
        //         Output-Module:   STATUS_RESPONSE  [type][seq 1B][ts_echo 4B LE][simvars]
        //         Input-Module:    Spontandaten      [type][ax0_int16][...][btn_bytes]
        //         Input + Poll:    STATUS_RESPONSE  [type][seq 1B][ts_echo 4B LE][ax][btn]
        //   0x03  ERROR:           [type][code 1B][text...]
        //
        // Frame-Typ-Dispatch:
        //   0x01 = HEARTBEAT
        //   0x02 = Spontandaten (Input-Module)   → dataOffset = 1
        //   0x04 = STATUS_RESPONSE (alle Module) → dataOffset = 6
        //
        // Monitor-Forwarding:
        //   Heartbeat, Output-Response, Error → forwardFeedback(canTxId, ...)
        //   Input-Spontandaten               → forwardRxFrame(canTxId, ...)
        //   Input-Response                   → forwardFeedback(canTxId, ...)
        {
            uint32_t rxCanId;
            uint8_t  rxBuf[68];
            size_t   rxLen;
            while (transport->tryReceive(rxCanId, rxBuf, rxLen)) {
                // ── HID-Mapping REQUEST vom ESP32 (0x7F2) ────────────────────────────
                // ESP32 fordert nach Boot aktiv das Mapping an und wiederholt alle 3 s.
                // Debounce 3 s verhindert Mehrfach-Sends bei schnell aufeinanderfolgenden REQUESTs.
                if (rxCanId == HID_MAP_REQ_CAN_ID && rxLen >= 1) {
                    if (now - hidMappingLastSentMs >= 3000LL) {
                        printf("[HID-MAP] ESP32 fordert Mapping an -- sende Konfiguration\n");
                        sendHidMapping();
                        hidMappingLastSentMs = now;
                    }
                    continue;
                }
                // ── HID-Mapping ACK vom ESP32 (0x7F1) ────────────────────────────────
                if (rxCanId == HID_MAP_ACK_CAN_ID && rxLen >= 2) {
                    const uint8_t sub   = rxBuf[0];
                    const uint8_t count = rxBuf[1];
                    const char* label =
                        (sub == 0xFF) ? "RESET"  :
                        (sub == 0x01) ? "ENTRY"  :
                        (sub == 0xFE) ? "COMMIT" :
                        (sub == 0x02) ? "AXIS"   :
                        (sub == 0xFD) ? "DONE"   : "?";
                    printf("[HID-ACK] ESP32: %s empfangen -- %u Eintraege im RAM\n",
                           label, (unsigned)count);
                    continue;
                }
                for (auto& rt : runtimes) {
                    if (rt.mod->canRxId == 0 || rt.mod->canRxId != rxCanId) continue;
                    if (rxLen < 1) break;

                    const uint8_t type       = rxBuf[0];
                    const bool    wasOffline = !rt.online;
                    const bool    firstTime  = !rt.everSeen;

                    // Empfangszeitstempel + everSeen für jeden Frame aktualisieren
                    rt.everSeen = true;
                    rt.lastRxMs = now;

                    if (type == 0x01) {
                        // HEARTBEAT [0x01][uptime_ms 4B LE] — Output- und Input-Module
                        // Reboot-Erkennung: Tick-Sprung > 100 ms zurück → sofortiger Re-Init
                        // 100 ms ist klein genug um auch Schnell-Reboots zu erkennen
                        // (ESP32 kann in <5 s wieder laufen), aber groß genug um
                        // Jitter bei normalem Betrieb zu tolerieren.
                        bool espRebooted = false;
                        if (rxLen >= 5) {
                            uint32_t espTick;
                            std::memcpy(&espTick, rxBuf + 1, 4);
                            if (rt.lastEspTickMs > 0 && espTick + 100u < rt.lastEspTickMs)
                                espRebooted = true;
                            rt.lastEspTickMs = espTick;
                        }
                        const bool triggerInit = firstTime || wasOffline || espRebooted;
                        if (!rt.online && rt.lastHeartbeatMs > 0)
                            errLog.logOnline(rt.mod->canTxId, rt.mod->deviceName,
                                             now - rt.wentOfflineMs);
                        rt.online          = true;
                        rt.offlineLogged   = false;
                        rt.lastHeartbeatMs = now;
                        if (rm.debugConsole && rt.mod == primaryMod)
                            ds.lastHeartbeatMs = now;
                        if (triggerInit)
                            monitor.sendStatusChange(rt.mod->canTxId, true);
                        if (triggerInit) {
                            uint8_t initBuf[64];
                            size_t  initLen = assembleInitPacket(*rt.mod,
                                                                  initBuf,
                                                                  sizeof(initBuf));
                            if (initLen > 0) {
                                transport->send(rt.mod->canTxId, initBuf, initLen);
                                if (rm.debugConsole && rt.mod == primaryMod)
                                    ++ds.framesSentMod;
                            }
                            size_t cfgLen = assembleConfigPacket(*rt.mod, initBuf, sizeof(initBuf));
                            if (cfgLen > 0) {
                                transport->send(rt.mod->canTxId, initBuf, cfgLen);
                                if (rm.debugConsole && rt.mod == primaryMod)
                                    ++ds.framesSentMod;
                            }
                            if (rt.mod->heartbeatIntervalMs > 0) {
                                uint8_t hbCfg[5];
                                hbCfg[0] = 0xFD;
                                uint32_t iv = (uint32_t)rt.mod->heartbeatIntervalMs;
                                std::memcpy(hbCfg + 1, &iv, 4);
                                transport->send(rt.mod->canTxId, hbCfg, sizeof(hbCfg));
                                if (rm.debugConsole && rt.mod == primaryMod)
                                    ++ds.framesSentMod;
                            }
                            // Achsen-Map neu senden (idempotent dank ESP32-Change-Detection)
                            if (!rt.mod->axes.empty()) {
                                const uint8_t axCnt =
                                    (uint8_t)std::min(rt.mod->axes.size(), (size_t)8);
                                uint8_t af[16] = {};
                                af[0] = 0xFE; af[1] = 0x02;
                                af[2] = (uint8_t)(rt.mod->canRxId >>  0);
                                af[3] = (uint8_t)(rt.mod->canRxId >>  8);
                                af[4] = (uint8_t)(rt.mod->canRxId >> 16);
                                af[5] = (uint8_t)(rt.mod->canRxId >> 24);
                                af[6] = axCnt;
                                uint8_t axKbMod = 0;
                                for (uint8_t i = 0; i < axCnt; i++) {
                                    af[7 + i] = rt.mod->axes[i].hidAxisIdx;
                                    axKbMod  |= rt.mod->axes[i].kbMod;
                                }
                                af[7 + axCnt] = axKbMod;
                                transport->send(HID_MAP_CAN_ID, af, 8 + axCnt);
                                if (rm.debugConsole && rt.mod == primaryMod)
                                    ++ds.framesSentMod;
                            }
                        }
                        monitor.forwardFeedback(rt.mod->canTxId, rxBuf, rxLen);
                    }
                    else if (type == 0x02 || type == 0x04) {
                        if (rt.mod->hasOutput() && type == 0x04 && rxLen >= 6) {
                            // STATUS_RESPONSE Output-Modul (z.B. Horizon):
                            // [0x04][seq 1B][ts_echo 4B LE][simvars nach INI-Reihenfolge]
                            uint32_t echoTs;
                            std::memcpy(&echoTs, rxBuf + 2, 4);
                            rt.lastRttMs = (int)((uint32_t)(now & 0xFFFFFFFFu) - echoTs);

                            std::string decoded;
                            char        tmp[80];
                            rt.lastFeedbackValues.clear();
                            rt.lastFeedbackNames.clear();
                            rt.lastFeedbackUnits.clear();
                            uint8_t fbOff = 6;   // nach type(1) + seq(1) + ts_echo(4)
                            for (const auto& sv : rt.mod->simvars) {
                                if (sv.fieldId == 0) continue;
                                FieldType ft    = fieldTypeFromSv(sv);
                                uint8_t   bytes = fieldTypeBytes(ft);
                                if (fbOff + bytes > rxLen) break;
                                double val = 0.0;
                                if (ft == FieldType::Float32) {
                                    float v; std::memcpy(&v, rxBuf + fbOff, 4); val = v;
                                } else if (ft == FieldType::Int16) {
                                    int16_t v; std::memcpy(&v, rxBuf + fbOff, 2); val = v;
                                } else {
                                    val = rxBuf[fbOff];
                                }
                                rt.lastFeedbackValues.push_back(val);
                                rt.lastFeedbackNames.push_back(sv.name);
                                rt.lastFeedbackUnits.push_back(sv.unit);
                                fbOff += bytes;
                                if (sv.unit == "radians")
                                    snprintf(tmp, sizeof(tmp), "%s=%+.1f\xC2\xB0  ", sv.name.c_str(), val * 57.2957795);
                                else if (sv.unit == "bool")
                                    snprintf(tmp, sizeof(tmp), "%s=%s  ", sv.name.c_str(), val > 0.5 ? "ON" : "OFF");
                                else
                                    snprintf(tmp, sizeof(tmp), "%s=%g  ", sv.name.c_str(), val);
                                decoded += tmp;
                            }
                            const std::string& devName = rt.mod->deviceName.empty()
                                                         ? rt.mod->iniKey : rt.mod->deviceName;
                            errLog.logStatus(rt.mod->canTxId, devName, rt.lastRttMs, decoded);
                            monitor.forwardFeedback(rt.mod->canTxId, rxBuf, rxLen);
                            if (rm.debugConsole && rt.mod == primaryMod) {
                                const size_t copyLen = std::min(rxLen, (size_t)64);
                                std::memcpy(ds.feedbackPayload, rxBuf, copyLen);
                                ds.feedbackPayloadLen = copyLen;
                                ds.lastRxMs           = now;
                                ++ds.framesReceived;
                                ds.feedbackValues = rt.lastFeedbackValues;
                                ds.feedbackUnits  = rt.lastFeedbackUnits;
                            }
                        }
                        if (rt.mod->hasInput()) {
                            // Input-Modul: Spontandaten (0x02) ODER STATUS_RESPONSE (0x04) nach Poll.
                            // Unterscheidung per Typ-Byte — kein Längenvergleich nötig.
                            //   0x02 Spontan:   [type][ax*int16][btn]           → dataOffset = 1
                            //   0x04 Response:  [type][seq][ts_echo 4B][ax][btn] → dataOffset = 6
                            const bool   isResponse = (rxBuf[0] == 0x04u);
                            const size_t dataOff    = isResponse ? 6u : 1u;

                            // Online-Tracking: jeder Frame bestätigt das Gerät als aktiv
                            rt.online = true;
                            if (wasOffline || firstTime) {
                                if (!firstTime && wasOffline)
                                    errLog.logOnline(rt.mod->canTxId, rt.mod->description,
                                                     now - rt.wentOfflineMs);
                                monitor.sendStatusChange(rt.mod->canTxId, true);
                                if (rt.mod->heartbeatIntervalMs > 0) {
                                    uint8_t hbCfg[5];
                                    hbCfg[0] = 0xFD;
                                    uint32_t iv = (uint32_t)rt.mod->heartbeatIntervalMs;
                                    std::memcpy(hbCfg + 1, &iv, 4);
                                    transport->send(rt.mod->canTxId, hbCfg, sizeof(hbCfg));
                                    if (rm.debugConsole && rt.mod == primaryMod)
                                        ++ds.framesSentMod;
                                }
                            }

                            // Achswerte dekodieren (INI-Reihenfolge, ab dataOffset)
                            for (size_t ai = 0; ai < rt.mod->axes.size(); ai++) {
                                const size_t off = dataOff + ai * 2;
                                if (off + 1 < rxLen)
                                    std::memcpy(&rt.axisValues[rt.mod->axes[ai].hidAxisIdx],
                                                rxBuf + off, 2);
                            }

                            if (isResponse) {
                                // STATUS_RESPONSE: RTT + Button-Dekodierung + Feedback
                                // Layout Response: [6B Header] | [axes×2B] | [Button-Bytes]
                                uint32_t echoTs;
                                std::memcpy(&echoTs, rxBuf + 2, 4);
                                rt.lastRttMs = (int)((uint32_t)(now & 0xFFFFFFFFu) - echoTs);

                                std::string decoded;
                                char        tmp[80];
                                for (const auto& ax : rt.mod->axes) {
                                    snprintf(tmp, sizeof(tmp), "%s=%+d  ",
                                             ax.name.c_str(),
                                             (int)rt.axisValues[ax.hidAxisIdx]);
                                    decoded += tmp;
                                }
                                if (!rt.mod->buttons.empty()) {
                                    // payload_byte ist 1-indexiert: B{N} → spontan-Index N-1 → Response-Index N-1+5 = N+4.
                                    for (const auto& btn : rt.mod->buttons) {
                                        const size_t bytePos = (size_t)btn.payloadByte + 4u;
                                        const bool   pressed = (bytePos < rxLen)
                                                               && ((rxBuf[bytePos] >> btn.payloadBit) & 0x01u);
                                        snprintf(tmp, sizeof(tmp), "%s=%s  ",
                                                 btn.name.c_str(), pressed ? "ON" : "off");
                                        decoded += tmp;
                                    }
                                }
                                const std::string& devName = rt.mod->deviceName.empty()
                                                             ? rt.mod->iniKey : rt.mod->deviceName;
                                errLog.logStatus(rt.mod->canTxId, devName, rt.lastRttMs, decoded);
                                monitor.forwardFeedback(rt.mod->canTxId, rxBuf, rxLen);
                                if (rm.debugConsole && rt.mod == primaryMod) {
                                    std::memcpy(ds.axisValues, rt.axisValues, sizeof(rt.axisValues));
                                    const size_t copyLen = std::min(rxLen, (size_t)64);
                                    std::memcpy(ds.feedbackPayload, rxBuf, copyLen);
                                    ds.feedbackPayloadLen = copyLen;
                                    ds.lastRxMs = now;
                                    ++ds.framesReceived;
                                }
                            } else {
                                // Spontandaten: als RX-Frame weiterleiten
                                monitor.forwardRxFrame(rt.mod->canTxId, rxBuf, rxLen);
                                if (rm.debugConsole && rt.mod == primaryMod) {
                                    std::memcpy(ds.axisValues, rt.axisValues, sizeof(rt.axisValues));
                                    ds.rxLen = std::min(rxLen, (size_t)64);
                                    std::memcpy(ds.rxPayload, rxBuf, ds.rxLen);
                                    ++ds.framesReceived;
                                    ds.lastRxMs = now;
                                }
                            }
                        }
                        if (!rt.mod->hasOutput() && !rt.mod->hasInput())
                            monitor.forwardFeedback(rt.mod->canTxId, rxBuf, rxLen);
                    }
                    else if (type == 0x03) {
                        // ERROR [0x03][code 1B][text...]
                        const uint8_t code = (rxLen >= 2) ? rxBuf[1] : 0;
                        std::string text(reinterpret_cast<char*>(rxBuf + 2),
                                         reinterpret_cast<char*>(rxBuf + rxLen));
                        errLog.logError(rt.mod->canTxId, rt.mod->deviceName, code, text);
                        monitor.forwardFeedback(rt.mod->canTxId, rxBuf, rxLen);
                    }
                    else {
                        monitor.forwardFeedback(rt.mod->canTxId, rxBuf, rxLen);
                    }
                    break;
                }
            }

            // ── USB-Framing-Fehler (items 6 & 7) ─────────────────────────────
            if (usingSerial) {
                if (auto* ser = dynamic_cast<SerialTransport*>(transport.get())) {
                    uint32_t cErr = ser->takeChecksumErrors();
                    uint32_t lErr = ser->takeFrameLenErrors();
                    if (cErr) {
                        char fmsg[80];
                        snprintf(fmsg, sizeof(fmsg),
                                 "frame checksum error (%u frame%s discarded)",
                                 cErr, cErr == 1 ? "" : "s");
                        errLog.logTransportError("USB-PORT", cfg.usbPort, fmsg);
                    }
                    if (lErr) {
                        char fmsg[80];
                        snprintf(fmsg, sizeof(fmsg),
                                 "frame length invalid (%u frame%s discarded)",
                                 lErr, lErr == 1 ? "" : "s");
                        errLog.logTransportError("USB-PORT", cfg.usbPort, fmsg);
                    }
                }
            }
        }

        // ── UDP-IP live in Kopfzeile spiegeln (aktualisiert nach IP-Discovery) ──
        if (udpXport && rm.debugConsole) {
            const std::string live = udpXport->destStr();
            if (live != ds.transportTarget)
                ds.transportTarget = live;
        }

        // ── Timeout-Überwachung (nach Anlaufzeit) ─────────────────────────────
        if (now - bridgeStartMs > STARTUP_GRACE_MS) {
            for (auto& rt : runtimes) {
                // Rückkanal-Timeout (Output-Module — Heartbeat kommt auf canRxId)
                if (rt.mod->hasOutput()) {
                    if (rt.online) {
                        if (now - rt.lastHeartbeatMs > rt.mod->heartbeatTimeoutMs) {
                            rt.online        = false;
                            rt.offlineLogged = false;
                            rt.wentOfflineMs = now;
                            monitor.sendStatusChange(rt.mod->canTxId, false);
                            // Sendeziel auf Broadcast zurücksetzen: ESP32 broadcastet beim
                            // nächsten Start, Bridge lernt neue IP beim ersten eingehenden Paket.
                            if (udpXport) {
                                udpXport->resetToBroadcast();
                                const std::string bc = udpXport->destStr();
                                ds.transportTarget = bc;
                            }
                        }
                    }
                    if (!rt.online && !rt.offlineLogged) {
                        rt.offlineLogged = true;
                        const long long sil = (rt.lastHeartbeatMs > 0)
                                              ? now - rt.lastHeartbeatMs
                                              : now - bridgeStartMs;
                        errLog.logOffline(rt.mod->canTxId, rt.mod->deviceName, sil);
                    }
                }
                // Input-Timeout (Module mit hasInput)
                // Neuestes beider Zeitstempel verwenden: lastHeartbeatMs hält das Gerät
                // bei Datenstillstand (Autopilot) online; lastRxMs verhindert Fehlalarm
                // nach Reconnect wenn noch keine neuen Heartbeats eingetroffen sind.
                if (rt.mod->hasInput() && rt.mod->heartbeatTimeoutMs > 0) {
                    const long long aliveMs = std::max(rt.lastHeartbeatMs, rt.lastRxMs);
                    if (rt.online && aliveMs > 0 &&
                        (now - aliveMs) > rt.mod->heartbeatTimeoutMs) {
                        rt.online        = false;
                        rt.wentOfflineMs = now;
                        errLog.logOffline(rt.mod->canTxId, rt.mod->description,
                                          now - aliveMs);
                        monitor.sendStatusChange(rt.mod->canTxId, false);
                    }
                }
            }
        }

        // ── Startup-Check: einmalig nach Grace-Period für nie gesehene Geräte ──
        if (!startupCheckDone && (now - bridgeStartMs > STARTUP_GRACE_MS)) {
            startupCheckDone = true;
            for (const auto& rt : runtimes) {
                // Input-Module ohne returnId: werden von der regulären Timeout-Logik
                // nicht erfasst, wenn sie nie online waren
                if (rt.mod->hasInput() && rt.mod->heartbeatTimeoutMs > 0 && !rt.everSeen)
                    errLog.logOffline(rt.mod->canTxId, rt.mod->description,
                                      now - bridgeStartMs);
            }
            if (cfg.monitorEnabled && monitor.isOpen() && !monEverSeen) {
                errLog.logOffline(0, "Monitor", now - bridgeStartMs);
                monOffLogged = true;
            }
        }

        // ── Monitor-Kommandos empfangen (LWT + Poll-Request) ─────────────────
        if (monitor.isOpen()) {
            uint8_t  monType;
            uint32_t monValue;   // je nach Typ: can_id (POLL) oder timestamp (LWT)
            while (monitor.tryReceiveCommand(monType, monValue)) {
                if (monType == MON_LWT) {
                    // monValue enthält den vom Monitor gesendeten Timestamp
                    const bool wasOffline = !monOnline;
                    if (wasOffline && monOffLogged)
                        errLog.logOnline(0, "Monitor", now - monWentOffMs);
                    monOnline    = true;
                    monEverSeen  = true;
                    monOffLogged = false;
                    monLastLwtMs = now;
                    // RTT direkt hier berechnen (LWT-Timestamp echo → PONG)
                    uint32_t echoTs = monValue;
                    monLastRttMs = (int)((uint32_t)(now & 0xFFFFFFFFu) - echoTs);
                    monitor.sendPong(echoTs);
                    // Bei jedem LWT aktuellen Status aller Geräte senden.
                    // Deckt Erstverbindung, Schnellneustart und Reconnect ab
                    // (wasOffline greift nicht wenn Monitor innerhalb monitorTimeoutMs neustartet).
                    for (const auto& rt : runtimes)
                        monitor.sendStatusChange(rt.mod->canTxId, rt.online);
                } else if (monType == MON_POLL_REQUEST) {
                    static uint8_t pollSeq = 0;
                    for (const auto& rt : runtimes) {
                        if (rt.mod->canTxId == monValue) {
                            uint8_t  req[6];
                            uint32_t ts = (uint32_t)(now & 0xFFFFFFFFu);
                            req[0] = 0xFE;           // STATUS_REQUEST
                            req[1] = ++pollSeq;
                            std::memcpy(req + 2, &ts, 4);
                            transport->send(rt.mod->canTxId, req, sizeof(req));
                            if (rm.debugConsole && rt.mod == primaryMod)
                                ++ds.framesSentMod;
                            break;
                        }
                    }
                }
            }
        }

        // ── Monitor-Timeout-Überwachung ───────────────────────────────────────
        if (monitor.isOpen() && monEverSeen) {
            if (monOnline && (now - monLastLwtMs > cfg.monitorTimeoutMs)) {
                monOnline    = false;
                monOffLogged = false;
                monWentOffMs = now;
            }
            if (!monOnline && !monOffLogged) {
                monOffLogged = true;
                errLog.logOffline(0, "Monitor", now - monLastLwtMs);
            }
        }

        // ── Transportfehler in ErrorLog (dedupliziert) ───────────────────────
        {
            const std::string& err = transport->lastError();
            if (!err.empty() && err != lastUdpError) {
                errLog.logTransportError(transport->logCategory(),
                                         transport->logDevice(), err);
                lastUdpError = err;
            } else if (err.empty()) {
                lastUdpError.clear();
            }
        }

        // ── Heartbeat: Frames assemblieren, senden, protokollieren ────────────
        if (now - lastSendMs >= cfg.sendIntervalMs) {
            lastSendMs = now;

            if (logger.isOpen()) logger.logInput(currentData);

            for (auto& fe : frames) {
                fe.size = assembleCanFrame(*fe.mod, currentData, fe.frame, sizeof(fe.frame));
                if (fe.size > 0) {
                    transport->send(fe.mod->canTxId, fe.frame, fe.size);
                    if (rm.debugConsole && fe.mod == primaryMod)
                        ++ds.framesSentMod;
                    monitor.forwardFrame(fe.mod->canTxId, fe.frame, fe.size);
                    if (logger.isOpen()) logger.logOutput(*fe.mod, fe.frame, fe.size);
                }
            }

            ds.lastSendMs     = transport->lastSendMs();
            ds.transportError = transport->lastError();
        }

        // ── Debug-State aktualisieren ─────────────────────────────────────────
        if (primaryMod && primaryMod->hasOutput()) {
            ds.simData = currentData;
            for (const auto& fe : frames) {
                if (fe.mod == primaryMod && fe.size > 0) {
                    std::memcpy(ds.primaryFrame, fe.frame, fe.size);
                    ds.primaryFrameSize = fe.size;
                    break;
                }
            }
            auto fields = getFrameFields(*primaryMod);
            ds.simvarFrameOffset.resize(fields.size());
            ds.simvarFrameBytes.resize(fields.size());
            for (size_t i = 0; i < fields.size(); ++i) {
                ds.simvarFrameOffset[i] = fields[i].offset;
                ds.simvarFrameBytes[i]  = fields[i].bytes;
            }
        }

        // Geräte-Status für Debug-Konsole — alle Module einheitlich
        ds.devices.clear();
        for (const auto& rt : runtimes) {
            DeviceStatus dev;
            dev.canTxId = rt.mod->canTxId;
            dev.name    = rt.mod->deviceName.empty()
                          ? (rt.mod->description.empty() ? rt.mod->iniKey : rt.mod->description)
                          : rt.mod->deviceName;
            dev.online  = rt.online;
            if (rt.mod->hasOutput()) {
                dev.everSeen    = (rt.lastHeartbeatMs > 0);
                dev.silenceSecs = (!rt.online)
                    ? ((rt.lastHeartbeatMs > 0)
                       ? (now - rt.lastHeartbeatMs) / 1000LL
                       : (now - bridgeStartMs)      / 1000LL)
                    : 0LL;
            } else {
                const long long aliveMs = (rt.lastHeartbeatMs > 0)
                                          ? rt.lastHeartbeatMs : rt.lastRxMs;
                dev.everSeen    = rt.everSeen;
                dev.silenceSecs = (!rt.online && rt.everSeen && aliveMs > 0)
                    ? (now - aliveMs) / 1000LL : 0LL;
            }
            ds.devices.push_back(dev);
        }

        // Monitor-Verbindungsstatus als letzten Eintrag (wenn aktiviert)
        if (cfg.monitorEnabled) {
            DeviceStatus mon;
            mon.isMonitor   = true;
            mon.name        = "Monitor";
            mon.online      = monOnline;
            mon.everSeen    = monEverSeen;
            mon.silenceSecs = (!monOnline && monEverSeen)
                              ? (now - monLastLwtMs) / 1000LL : 0LL;
            ds.devices.push_back(mon);
        }

        // ── statusMsg nach Ablaufzeit löschen ────────────────────────────────────
        if (statusMsgClearMs > 0 && now >= statusMsgClearMs) {
            ds.statusMsg.clear();
            statusMsgClearMs = 0;
        }

        // ── Tastatureingabe (nur im Debug-Modus) ──────────────────────────────────
        if (rm.debugConsole) ds.usbActive = usingSerial;

        if (rm.debugConsole && _kbhit()) {
            const int ch = _getch();
            if (ch == 'q' || ch == 'Q') {
                g_running = false;
            } else if ((ch == 'p' || ch == 'P') && primaryMod) {
                // STATUS_REQUEST an primäres Modul senden
                static uint8_t dbgPollSeq = 0;
                uint8_t  req[6];
                uint32_t ts = (uint32_t)(now & 0xFFFFFFFFu);
                req[0] = 0xFE;
                req[1] = ++dbgPollSeq;
                std::memcpy(req + 2, &ts, 4);
                transport->send(primaryMod->canTxId, req, sizeof(req));
                ds.pollSent   = true;
                ds.pollSentMs = now;
            } else if ((ch == 'r' || ch == 'R') && usingSerial) {
                auto* ser = dynamic_cast<SerialTransport*>(transport.get());
                if (ser && ser->sendText("RESET\r\n")) {
                    ds.statusMsg     = "RESET gesendet -> ESP32 startet neu...";
                    statusMsgClearMs = now + 6000;
                }
            }
        }

        if (rm.debugConsole && primaryMod) {
            // Online-Status des primären Moduls in ds spiegeln
            for (const auto& rt : runtimes) {
                if (rt.mod != primaryMod) continue;
                ds.online = rt.online;
                if (primaryMod->hasOutput()) {
                    ds.everSeen    = (rt.lastHeartbeatMs > 0);
                    ds.silenceSecs = (!rt.online && rt.lastHeartbeatMs > 0)
                        ? (now - rt.lastHeartbeatMs) / 1000LL : 0LL;
                } else {
                    ds.everSeen = rt.everSeen;
                    const long long aliveMs = std::max(rt.lastHeartbeatMs, rt.lastRxMs);
                    ds.silenceSecs = (!rt.online && rt.everSeen && aliveMs > 0)
                        ? (now - aliveMs) / 1000LL : 0LL;
                }
                break;
            }
            dbg.update(ds);
        }
        Sleep(1);
    }

    logger.close();
    sim.stopAsync();
    transport->close();
    monitor.close();
    return 0;
}

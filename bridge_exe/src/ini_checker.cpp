#include "ini_checker.h"
#include "config.h"
#include "can_frame.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cctype>

// ── Hilfsmittel ──────────────────────────────────────────────────────────────

static const std::set<std::string> KNOWN_UNITS = {
    "radians", "degrees", "feet", "knots", "percent", "mbar", "celsius",
    "feet per minute", "gallons", "number", "bool", "status"
};

static const std::set<uint32_t> RESERVED_IDS = {
    0x0000u, ESP32_HB_CAN_ID,      // 0x0001
    HID_MAP_CAN_ID,                 // 0x7F0
    HID_MAP_ACK_CAN_ID,             // 0x7F1
    HID_MAP_REQ_CAN_ID              // 0x7F2
};

static std::string hexId(uint32_t id) {
    char buf[12];
    snprintf(buf, sizeof(buf), "0x%03X", id);
    return buf;
}

static std::string hexByte(uint8_t b) {
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", b);
    return buf;
}

// ── Log-Struktur ─────────────────────────────────────────────────────────────

struct Checker {
    FILE* f     = nullptr;
    int nErrors = 0;
    int nWarns  = 0;
    int nInfos  = 0;
    int nOks    = 0;

    void section(const std::string& title) {
        fprintf(f, "\n── %s ", title.c_str());
        int used = 4 + (int)title.size() + 1;
        for (int i = used; i < 60; ++i) fputc('-', f);
        fprintf(f, "\n");
    }

    void fehler(const std::string& msg) { ++nErrors; fprintf(f, "  FEHLER  %s\n", msg.c_str()); }
    void warn  (const std::string& msg) { ++nWarns;  fprintf(f, "  WARN    %s\n", msg.c_str()); }
    void info  (const std::string& msg) { ++nInfos;  fprintf(f, "  INFO    %s\n", msg.c_str()); }
    void ok    (const std::string& msg) { ++nOks;    fprintf(f, "  OK      %s\n", msg.c_str()); }
};

// ── Sonderzeichen-Helfer ─────────────────────────────────────────────────────

// Prüft ob ein Identifier nur alphanumerische Zeichen und _ enthält.
// Gibt das erste Sonderzeichen als druckbares Token zurück oder "" wenn OK.
static std::string firstSpecialIdent(const std::string& s) {
    for (char c : s) {
        if (!std::isalnum((unsigned char)c) && c != '_') {
            char buf[8];
            if (std::isprint((unsigned char)c))
                snprintf(buf, sizeof(buf), "'%c'", c);
            else
                snprintf(buf, sizeof(buf), "0x%02X", (unsigned char)c);
            return buf;
        }
    }
    return "";
}

// Prüft ob ein beliebiger Text nur druckbare ASCII-Zeichen enthält.
// Gibt das erste auffällige Zeichen zurück oder "" wenn OK.
static std::string firstNonAscii(const std::string& s) {
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (u > 127 || (u < 32 && u != '\t' && u != ' ')) {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X", u);
            return buf;
        }
    }
    return "";
}

// ── Hauptroutine ─────────────────────────────────────────────────────────────

int runIniCheck(const std::string& iniPath) {
    Config cfg;
    const bool loaded = cfg.load(iniPath);

    FILE* logFile = fopen("inicheck.log", "w");
    if (!logFile) {
        printf("FEHLER: inicheck.log kann nicht erstellt werden.\n");
        return 1;
    }
    // UTF-8 BOM: stellt sicher dass die Log-Datei in altem Notepad und allen
    // Windows-Editoren korrekt als UTF-8 erkannt wird.
    fwrite("\xEF\xBB\xBF", 1, 3, logFile);

    // Header
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(logFile,
        "══════════════════════════════════════════════════════════════\n"
        "  bridge.ini — INI-Check  [%s]\n"
        "══════════════════════════════════════════════════════════════\n",
        ts);

    Checker c;
    c.f = logFile;

    if (!loaded) {
        c.fehler("bridge.ini nicht gefunden oder nicht lesbar: " + iniPath);
        fprintf(logFile, "\n── Zusammenfassung ──────────────────────────────────────────\n");
        fprintf(logFile, "  FEHLER: 1\n  → Bitte bridge.ini im Verzeichnis von bridge.exe ablegen.\n");
        fclose(logFile);
        printf("INI-Check abgeschlossen -> inicheck.log  (1 FEHLER)\n");
        return 1;
    }

    // ── Parser-Warnungen weitergeben ─────────────────────────────────────────
    if (!cfg.warnings.empty()) {
        c.section("Parser-Warnungen");
        for (const auto& w : cfg.warnings)
            c.warn(w);
    }

    // ── A: [bridge] ──────────────────────────────────────────────────────────
    c.section("[bridge]");
    {
        // A1/A2: Ports
        if (cfg.udpPort < 1 || cfg.udpPort > 65535)
            c.fehler("udp_port = " + std::to_string(cfg.udpPort) + "  (gültig: 1–65535)");
        else
            c.ok("udp_port = " + std::to_string(cfg.udpPort));

        if (cfg.receivePort < 1 || cfg.receivePort > 65535)
            c.fehler("receive_port = " + std::to_string(cfg.receivePort) + "  (gültig: 1–65535)");
        else if (cfg.receivePort == cfg.udpPort)
            c.fehler("receive_port == udp_port (" + std::to_string(cfg.udpPort) + ") — bridge würde eigene Pakete empfangen");
        else
            c.ok("receive_port = " + std::to_string(cfg.receivePort) + "  (≠ udp_port)");

        // A4: Sende-Intervall
        if (cfg.sendIntervalMs < 50)
            c.fehler("send_interval_ms = " + std::to_string(cfg.sendIntervalMs) + "  (Minimum: 50 ms)");
        else if (cfg.sendIntervalMs < 100)
            c.warn("send_interval_ms = " + std::to_string(cfg.sendIntervalMs) + "  — niedrig, kann Sendeüberlastung verursachen (empfohlen: ≥ 100)");
        else if (cfg.sendIntervalMs > 5000)
            c.warn("send_interval_ms = " + std::to_string(cfg.sendIntervalMs) + "  — sehr hoch (> 5 s), Flugdaten sehr selten");
        else
            c.ok("send_interval_ms = " + std::to_string(cfg.sendIntervalMs));

        // A5/A6: USB-Port
        if (!cfg.usbPort.empty()) {
            c.ok("usb_port = " + cfg.usbPort + "  (UART-Modus aktiv)");
            if (cfg.usbBaud != 921600)
                c.warn("usb_baud = " + std::to_string(cfg.usbBaud) +
                       "  — muss mit JOY_UART_BAUD in bridge_ESP32/src/config.h übereinstimmen (Standard: 921600)");
            else
                c.ok("usb_baud = " + std::to_string(cfg.usbBaud));
        } else {
            c.info("usb_port nicht gesetzt — Transport: UDP (" + cfg.udpIp + ")");
        }

        // A7: UDP-IP bei UART-Modus
        if (!cfg.usbPort.empty() &&
            (cfg.udpIp == "255.255.255.255" || cfg.udpIp == "192.168.1.100"))
            c.info("udp_ip = " + cfg.udpIp + "  — UDP-Fallback-Adresse (nicht aktiv solange UART verbunden)");
    }

    // ── B: [monitor] ─────────────────────────────────────────────────────────
    c.section("[monitor]");
    {
        if (!cfg.monitorEnabled)
            c.info("enabled = false — bridge_monitor empfängt keine Daten");

        const std::set<int> bridgePorts = {cfg.udpPort, cfg.receivePort};

        if (cfg.monitorPort < 1 || cfg.monitorPort > 65535)
            c.fehler("port = " + std::to_string(cfg.monitorPort) + "  (gültig: 1–65535)");
        else if (bridgePorts.count(cfg.monitorPort))
            c.fehler("port = " + std::to_string(cfg.monitorPort) + "  kollidiert mit [bridge]-Port");
        else
            c.ok("port = " + std::to_string(cfg.monitorPort));

        if (cfg.monitorCmdPort < 1 || cfg.monitorCmdPort > 65535)
            c.fehler("cmd_port = " + std::to_string(cfg.monitorCmdPort) + "  (gültig: 1–65535)");
        else if (cfg.monitorCmdPort == cfg.monitorPort)
            c.fehler("cmd_port == port (" + std::to_string(cfg.monitorPort) + ") — gegenseitige Blockierung");
        else if (bridgePorts.count(cfg.monitorCmdPort))
            c.fehler("cmd_port = " + std::to_string(cfg.monitorCmdPort) + "  kollidiert mit [bridge]-Port");
        else
            c.ok("cmd_port = " + std::to_string(cfg.monitorCmdPort) + "  (≠ port, ≠ bridge-Ports)");

        if (cfg.monitorTimeoutMs < 1000)
            c.warn("timeout_ms = " + std::to_string(cfg.monitorTimeoutMs) + "  — sehr niedrig (< 1000 ms), häufige false-OFFLINE-Meldungen");
        else
            c.ok("timeout_ms = " + std::to_string(cfg.monitorTimeoutMs));
    }

    // ── C: CAN-ID Konsistenz ─────────────────────────────────────────────────
    c.section("CAN-ID Konsistenz");
    {
        std::map<uint32_t, std::string> txOwner;   // id → "module.key"
        std::map<uint32_t, std::string> rxOwner;
        bool txUnique = true, rxUnique = true, noXcross = true;

        for (const auto& mod : cfg.modules) {
            // Reservierte IDs
            if (RESERVED_IDS.count(mod.canTxId))
                c.fehler("module." + mod.iniKey + ": can_tx_id " + hexId(mod.canTxId) +
                         " ist reserviert (ESP32-interne Protokoll-ID)");
            if (mod.canRxId != 0 && RESERVED_IDS.count(mod.canRxId))
                c.fehler("module." + mod.iniKey + ": can_rx_id " + hexId(mod.canRxId) +
                         " ist reserviert (ESP32-interne Protokoll-ID)");

            // TX == RX innerhalb Modul
            if (mod.canTxId != 0 && mod.canRxId != 0 && mod.canTxId == mod.canRxId)
                c.fehler("module." + mod.iniKey + ": can_tx_id == can_rx_id (" +
                         hexId(mod.canTxId) + ") — Frame-Loop möglich");

            // TX-Eindeutigkeit
            auto txIt = txOwner.find(mod.canTxId);
            if (txIt != txOwner.end()) {
                c.fehler("can_tx_id " + hexId(mod.canTxId) + " doppelt: module." +
                         txIt->second + " und module." + mod.iniKey);
                txUnique = false;
            } else if (mod.canTxId != 0) {
                txOwner[mod.canTxId] = mod.iniKey;
            }

            // RX-Eindeutigkeit
            auto rxIt = rxOwner.find(mod.canRxId);
            if (rxIt != rxOwner.end()) {
                c.fehler("can_rx_id " + hexId(mod.canRxId) + " doppelt: module." +
                         rxIt->second + " und module." + mod.iniKey);
                rxUnique = false;
            } else if (mod.canRxId != 0) {
                rxOwner[mod.canRxId] = mod.iniKey;
            }
        }

        // Kreuzkolllision TX einer ID == RX einer anderen
        for (const auto& tx : txOwner) {
            auto rxIt = rxOwner.find(tx.first);
            if (rxIt != rxOwner.end() && rxIt->second != tx.second) {
                c.fehler("ID " + hexId(tx.first) + " ist can_tx_id von module." + tx.second +
                         " UND can_rx_id von module." + rxIt->second + " — Kreuzkolllision");
                noXcross = false;
            }
        }

        if (txUnique && !txOwner.empty())
            c.ok(std::to_string(txOwner.size()) + " CAN-TX-IDs systemweit eindeutig");
        if (rxUnique && !rxOwner.empty())
            c.ok(std::to_string(rxOwner.size()) + " CAN-RX-IDs systemweit eindeutig");
        if (noXcross && !txOwner.empty())
            c.ok("Keine TX/RX-Kreuzkolllisionen");

        // HID-Routing und Konventionen je Modul
        for (const auto& mod : cfg.modules) {
            const bool hasInput  = !mod.buttons.empty() || !mod.axes.empty();
            const bool hasOutput = !mod.simvars.empty();

            if (hasInput) {
                if (mod.canTxId < 0x600 || mod.canTxId > 0x6FF)
                    c.fehler("module." + mod.iniKey + ": Input-Modul hat can_tx_id " +
                             hexId(mod.canTxId) + " — muss in 0x600..0x6FF liegen für ESP32-HID-Routing");
                else
                    c.ok("module." + mod.iniKey + ": Input-Modul can_tx_id " +
                         hexId(mod.canTxId) + " im HID-Bereich 0x600..0x6FF");

                // Konvention: can_rx_id = can_tx_id | 0x80
                if (mod.canTxId != 0 && mod.canRxId != 0) {
                    const uint32_t conv = mod.canTxId | 0x80u;
                    if (mod.canRxId != conv)
                        c.warn("module." + mod.iniKey + ": can_rx_id " + hexId(mod.canRxId) +
                               " weicht von Konvention (can_tx_id | 0x80 = " + hexId(conv) + ") ab");
                }
            }

            if (hasOutput && mod.canTxId >= 0x600 && mod.canTxId <= 0x6FF)
                c.warn("module." + mod.iniKey + ": Anzeige-Modul (simvars) mit can_tx_id " +
                       hexId(mod.canTxId) + " im HID-Bereich — unüblich");
        }
    }

    // ── D/E/F/G: Pro-Modul-Checks ────────────────────────────────────────────

    // Globale HID-Nutzungs-Tabellen (modulübergreifend)
    std::map<uint8_t, std::string> hidAOwner;                    // hidA → ref
    std::set<std::pair<uint8_t,uint8_t>> chordSet;               // {min,max}
    std::map<uint8_t, std::string> axisOwner;                    // hidAxisIdx → ref

    if (cfg.modules.empty()) {
        c.section("Module");
        c.fehler("Keine Module in bridge.ini konfiguriert — bridge.exe wird nicht starten");
    }

    for (const auto& mod : cfg.modules) {
        c.section("[module." + mod.iniKey + "]");

        // D: Basis
        if (mod.canTxId == 0)
            c.fehler("can_tx_id fehlt oder ist 0  (Pflichtfeld)");
        if (mod.canRxId == 0)
            c.fehler("can_rx_id fehlt oder ist 0  (Pflichtfeld)");

        const bool hasInput  = !mod.buttons.empty() || !mod.axes.empty();
        const bool hasOutput = !mod.simvars.empty();

        if (!hasInput && !hasOutput)
            c.warn("Modul hat weder .simvars noch .buttons/.axes — ohne Funktion");

        if (hasOutput && mod.heartbeatTimeoutMs <= 0)
            c.warn("heartbeat_timeout_ms nicht gesetzt — kein OFFLINE-Logging für dieses Anzeige-Modul");

        if (mod.heartbeatIntervalMs > 0 && mod.heartbeatTimeoutMs > 0 &&
            mod.heartbeatIntervalMs >= mod.heartbeatTimeoutMs)
            c.fehler("heartbeat_interval_ms (" + std::to_string(mod.heartbeatIntervalMs) +
                     ") >= heartbeat_timeout_ms (" + std::to_string(mod.heartbeatTimeoutMs) +
                     ") — Modul würde sofort OFFLINE gehen");

        if (mod.description.empty() && mod.deviceName.empty())
            c.info("Kein 'name' oder 'description' gesetzt — für Diagnose empfohlen");

        {
            std::string cap;
            if (hasOutput) cap += " Output(" + std::to_string(mod.simvars.size()) + " SimVar)";
            if (hasInput)  cap += " Input(" + std::to_string(mod.buttons.size()) +
                                  " Btn / " + std::to_string(mod.axes.size()) + " Achsen)";
            c.info("Fähigkeiten:" + cap);
        }

        // E: Buttons ──────────────────────────────────────────────────────────
        {
            std::set<std::pair<uint8_t,uint8_t>> localPayloadBits;
            for (const auto& btn : mod.buttons) {
                const std::string ref = "Button '" + btn.name + "'";

                if (btn.payloadByte == 0)
                    c.fehler(ref + ": payload_byte = 0 ist reserviert — wird zur Laufzeit ignoriert");

                if (btn.payloadBit > 7)
                    c.warn(ref + ": payload_bit = " + std::to_string(btn.payloadBit) + "  (gültig: 0–7)");

                if (btn.physicalIdx > 127)
                    c.warn(ref + ": physical_idx = " + std::to_string(btn.physicalIdx) + "  (gültig: 0–127)");

                if (btn.hidA < 1 || btn.hidA > 128)
                    c.fehler(ref + ": hid_a = " + std::to_string(btn.hidA) + "  (gültig: 1–128)");

                if (btn.hidB != 0 && (btn.hidB < 1 || btn.hidB > 128))
                    c.fehler(ref + ": hid_b = " + std::to_string(btn.hidB) + "  (gültig: 0 = Single oder 1–128 = Chord)");

                if (btn.hidB != 0 && btn.hidA == btn.hidB)
                    c.fehler(ref + ": hid_a == hid_b (" + std::to_string(btn.hidA) +
                             ") — Chord mit demselben Button sinnlos");

                // Globale hid_a-Eindeutigkeit
                const std::string owner = "module." + mod.iniKey + ":'" + btn.name + "'";
                auto it = hidAOwner.find(btn.hidA);
                if (it != hidAOwner.end())
                    c.warn(ref + ": hid_a = " + std::to_string(btn.hidA) +
                           " bereits verwendet von " + it->second +
                           " — mehrere Quellen aktivieren denselben HID-Button");
                else
                    hidAOwner[btn.hidA] = owner;

                // Chord-Paar-Eindeutigkeit
                if (btn.hidB != 0) {
                    auto chord = std::make_pair(std::min(btn.hidA, btn.hidB),
                                                std::max(btn.hidA, btn.hidB));
                    if (chordSet.count(chord))
                        c.fehler(ref + ": Chord JS" + std::to_string(btn.hidA) + "+" +
                                 std::to_string(btn.hidB) + " bereits verwendet — doppeltes Binding");
                    else
                        chordSet.insert(chord);
                }

                // Name-Länge
                if (btn.name.size() > 9)
                    c.warn(ref + ": Name länger als 9 Zeichen — wird im ESP32 auf 9 Zeichen abgeschnitten");

                // Payload-Bit-Kollision innerhalb Modul
                auto pb = std::make_pair(btn.payloadByte, btn.payloadBit);
                if (localPayloadBits.count(pb))
                    c.warn(ref + ": payload_byte=" + std::to_string(btn.payloadByte) +
                           " / payload_bit=" + std::to_string(btn.payloadBit) +
                           " bereits von anderem Button im Modul belegt — Zustandskonflikt");
                else
                    localPayloadBits.insert(pb);
            }
        }

        // F: Achsen ───────────────────────────────────────────────────────────
        for (const auto& ax : mod.axes) {
            const std::string ref = "Achse '" + ax.name + "'";

            if (ax.hidAxisIdx > 7) {
                c.fehler(ref + ": hid_axis_idx = " + std::to_string(ax.hidAxisIdx) +
                         "  (gültig: 0–7, ESP32 hat 8 Achsen)");
            } else {
                auto it = axisOwner.find(ax.hidAxisIdx);
                if (it != axisOwner.end())
                    c.fehler(ref + ": hid_axis_idx = " + std::to_string(ax.hidAxisIdx) +
                             " bereits verwendet von " + it->second + " — Datenkollision");
                else
                    axisOwner[ax.hidAxisIdx] = "module." + mod.iniKey + ":'" + ax.name + "'";
            }

            if (ax.name.size() > 9)
                c.info(ref + ": Name länger als 9 Zeichen — wird in der Debug-Anzeige abgeschnitten");
        }

        // G: SimVars ──────────────────────────────────────────────────────────
        if (!mod.simvars.empty()) {
            std::set<uint8_t>    usedFieldIds;
            std::set<std::string> usedNames;
            size_t payloadBytes = 1u;  // Byte 0 = Pakettyp

            for (const auto& sv : mod.simvars) {
                const std::string ref = "SimVar '" + sv.name + "'";

                // Einheit
                if (!KNOWN_UNITS.count(sv.unit))
                    c.fehler(ref + ": unbekannte Einheit '" + sv.unit +
                             "' — kein CAN-Byte-Bedarf berechenbar");

                // Field-ID
                if (sv.fieldId == 0) {
                    c.warn(ref + ": field_id nicht auflösbar (weder inline noch in [field_ids])"
                           " — fehlt im Init-Paket an das Instrument");
                } else {
                    if (usedFieldIds.count(sv.fieldId)) {
                        c.fehler(ref + ": field_id " + hexByte(sv.fieldId) +
                                 " bereits im selben Modul verwendet — Payload-Kollision");
                    } else {
                        usedFieldIds.insert(sv.fieldId);
                    }
                }

                // Doppelter SimVar-Name
                if (usedNames.count(sv.name))
                    c.warn(ref + ": SimVar-Name doppelt im Modul — Redundanz oder Tippfehler");
                else
                    usedNames.insert(sv.name);

                // Payload-Größe akkumulieren
                payloadBytes += fieldTypeBytes(fieldTypeFromSv(sv));
            }

            // G4: Payload-Limit
            if (payloadBytes > 63)
                c.fehler("Payload-Größe = " + std::to_string(payloadBytes) +
                         " Byte — CAN-FD-Maximum: 63 Byte (Überschreitung um " +
                         std::to_string(payloadBytes - 63) + " Byte)");
            else
                c.ok("Payload-Größe: " + std::to_string(payloadBytes) + " / 63 Byte  (" +
                     std::to_string(63 - (int)payloadBytes) + " Byte frei)");

            // G5: SimVar-Anzahl
            if ((int)mod.simvars.size() > 15)
                c.fehler("SimVar-Anzahl = " + std::to_string(mod.simvars.size()) +
                         " — Maximum: 15 pro Modul");
            else
                c.ok("SimVar-Anzahl: " + std::to_string(mod.simvars.size()) + " / 15");
        }
    }

    // ── H: [field_ids] ───────────────────────────────────────────────────────
    c.section("[field_ids]");
    {
        if (cfg.fieldIds.empty()) {
            c.info("Keine [field_ids] konfiguriert");
        } else {
            std::map<uint8_t, std::string> idToName;
            bool allUnique = true;

            for (const auto& kv : cfg.fieldIds) {
                if (kv.second == 0)
                    c.fehler("'" + kv.first + "': field_id = 0x00 ist reserviert (bedeutet 'nicht registriert')");

                auto it = idToName.find(kv.second);
                if (it != idToName.end()) {
                    c.fehler("Field-ID " + hexByte(kv.second) + " doppelt: '" +
                             it->second + "' und '" + kv.first + "' — Kollision im Init-Paket");
                    allUnique = false;
                } else {
                    idToName[kv.second] = kv.first;
                }
            }

            if (allUnique)
                c.ok(std::to_string(cfg.fieldIds.size()) + " Field-IDs eindeutig");

            // Ungenutzte Field-IDs melden
            std::set<uint8_t> activeIds;
            for (const auto& mod : cfg.modules)
                for (const auto& sv : mod.simvars)
                    if (sv.fieldId != 0) activeIds.insert(sv.fieldId);

            for (const auto& kv : cfg.fieldIds)
                if (!activeIds.count(kv.second))
                    c.info("Field-ID " + hexByte(kv.second) + " '" + kv.first +
                           "' — nicht von aktivem Modul verwendet (auskommentiert?)");
        }
    }

    // ── J: Sonderzeichen ─────────────────────────────────────────────────────
    c.section("Sonderzeichen");
    {
        // Identifier: Modul-Schlüssel, Button-/Achsen-Namen → nur a-z, A-Z, 0-9, _
        // Text-Felder: description, SimVar-Namen → nur druckbares ASCII
        // Alle Treffer: INFO

        auto checkIdent = [&](const std::string& ctx, const std::string& val) {
            if (val.empty()) return;
            const std::string sp = firstSpecialIdent(val);
            if (!sp.empty())
                c.info(ctx + " '" + val + "': Sonderzeichen " + sp +
                       " — nur A-Z, a-z, 0-9, _ empfohlen für ESP32-Kompatibilität");
        };

        auto checkText = [&](const std::string& ctx, const std::string& val) {
            if (val.empty()) return;
            const std::string na = firstNonAscii(val);
            if (!na.empty())
                c.info(ctx + " '" + val.substr(0, 30) + "': nicht-ASCII-Zeichen " + na);
        };

        // [bridge]
        if (!cfg.usbPort.empty()) checkIdent("[bridge] usb_port", cfg.usbPort);
        checkIdent("[simconnect] app_name", cfg.appName);
        checkText ("[simconnect] app_name", cfg.appName);

        for (const auto& mod : cfg.modules) {
            // Modul-Schlüssel
            checkIdent("Modul-Schlüssel", mod.iniKey);
            // name (deviceName): wird im Netz und im Protokoll verwendet
            if (!mod.deviceName.empty()) {
                checkIdent("module." + mod.iniKey + " name", mod.deviceName);
                checkText ("module." + mod.iniKey + " name", mod.deviceName);
            }
            // description: nur Anzeige
            checkText("module." + mod.iniKey + " description", mod.description);

            // Button-Namen: werden als C-String an ESP32 übertragen
            for (const auto& btn : mod.buttons) {
                checkIdent("module." + mod.iniKey + " button", btn.name);
                checkText ("module." + mod.iniKey + " button '" + btn.name + "'", btn.name);
            }
            // Achsen-Namen: Debug-Anzeige
            for (const auto& ax : mod.axes) {
                checkIdent("module." + mod.iniKey + " achse", ax.name);
                checkText ("module." + mod.iniKey + " achse '" + ax.name + "'", ax.name);
            }
            // SimVar-Namen: MSFS-Bezeichner, Leerzeichen erwartet, kein non-ASCII
            for (const auto& sv : mod.simvars)
                checkText("module." + mod.iniKey + " SimVar", sv.name);
        }

        // [field_ids]-Schlüssel
        for (const auto& kv : cfg.fieldIds)
            checkText("[field_ids] '" + kv.first + "'", kv.first);
    }

    // ── I: Gesamtsystem-Zusammenfassung ──────────────────────────────────────
    c.section("Zusammenfassung");
    {
        size_t totalSimvars = 0, totalButtons = 0, totalAxes = 0;
        size_t inputMods = 0, outputMods = 0;
        for (const auto& mod : cfg.modules) {
            totalSimvars += mod.simvars.size();
            totalButtons += mod.buttons.size();
            totalAxes    += mod.axes.size();
            if (!mod.simvars.empty())  ++outputMods;
            if (!mod.buttons.empty() || !mod.axes.empty()) ++inputMods;
        }

        fprintf(logFile, "\n  Module:  %zu  (%zu Output, %zu Input)\n",
                cfg.modules.size(), outputMods, inputMods);
        fprintf(logFile, "  SimVars: %zu  |  Buttons: %zu  |  Achsen: %zu\n",
                totalSimvars, totalButtons, totalAxes);
        fprintf(logFile, "\n  FEHLER:  %d  |  WARN:  %d  |  INFO:  %d\n",
                c.nErrors, c.nWarns, c.nInfos);

        if (c.nErrors > 0)
            fprintf(logFile, "\n  → Bitte alle FEHLER beheben vor dem Start.\n");
        else if (c.nWarns > 0)
            fprintf(logFile, "\n  → Keine Fehler. Warnungen prüfen und ggf. korrigieren.\n");
        else
            fprintf(logFile, "\n  → bridge.ini ist fehlerfrei.\n");

        if (!cfg.modules.empty()) {
            fprintf(logFile, "\n  Debug-Targets (bridge.exe debug <name>):\n");
            for (const auto& mod : cfg.modules) {
                const std::string& desc = mod.description.empty() ? mod.deviceName : mod.description;
                fprintf(logFile, "    %-18s  %s\n", mod.iniKey.c_str(), desc.c_str());
            }
        }
    }

    fclose(logFile);

    // Konsolen-Zusammenfassung
    if (c.nErrors > 0)
        printf("INI-Check abgeschlossen -> inicheck.log  (%d FEHLER, %d WARN, %d INFO)\n",
               c.nErrors, c.nWarns, c.nInfos);
    else if (c.nWarns > 0)
        printf("INI-Check abgeschlossen -> inicheck.log  (%d WARN, %d INFO)\n",
               c.nWarns, c.nInfos);
    else
        printf("INI-Check abgeschlossen -> inicheck.log  (OK)\n");

    return (c.nErrors > 0) ? 1 : 0;
}

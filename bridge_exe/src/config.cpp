#include "config.h"
#include "module.h"   // KB_* Konstanten
#include <fstream>
#include <algorithm>
#include <cctype>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string stripComment(const std::string& s) {
    for (char c : {';', '#'}) {
        auto p = s.find(c);
        if (p != std::string::npos) return trim(s.substr(0, p));
    }
    return s;
}

static ModuleConfig& findOrCreateModule(std::vector<ModuleConfig>& v,
                                        const std::string& key) {
    for (auto& m : v) if (m.iniKey == key) return m;
    v.push_back({});
    v.back().iniKey = key;
    return v.back();
}

// Komma-separierte Integer-Liste parsen: "8, 0, 37, 16, 17" → {8, 0, 37, 16, 17}
static std::vector<int> parseIntList(const std::string& s) {
    std::vector<int> result;
    std::string::size_type pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ',' || s[pos] == ' ' || s[pos] == '\t'))
            ++pos;
        if (pos >= s.size()) break;
        try {
            size_t consumed = 0;
            int v = std::stoi(s.substr(pos), &consumed);
            result.push_back(v);
            pos += consumed;
        } catch (...) { break; }
    }
    return result;
}

// Komma-separierte Token aufteilen, jedes Token getrimmt.
// "5, 0, 5, 10, LSHIFT" → {"5","0","5","10","LSHIFT"}
static std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> result;
    std::string::size_type start = 0;
    while (true) {
        auto comma = s.find(',', start);
        std::string tok = (comma == std::string::npos) ? s.substr(start) : s.substr(start, comma - start);
        // trim
        size_t a = tok.find_first_not_of(" \t\r\n");
        size_t b = tok.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) result.push_back(tok.substr(a, b - a + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return result;
}

// Modifier-Schlüsselwort → KB_*-Bitmask.
// Unterstützt '+'-Kombinationen: "LSHIFT+LALT" → KB_LSHIFT|KB_LALT
static uint8_t parseKbMod(const std::string& s) {
    uint8_t mod = KB_NONE;
    std::string::size_type pos = 0;
    while (pos <= s.size()) {
        auto plus = s.find('+', pos);
        std::string tok = s.substr(pos, (plus == std::string::npos) ? std::string::npos : plus - pos);
        // uppercase trim
        std::string t;
        for (char c : tok)
            if (!std::isspace((unsigned char)c)) t += (char)std::toupper((unsigned char)c);
        if      (t == "LCTRL"  || t == "LCONTROL") mod |= KB_LCTRL;
        else if (t == "LSHIFT")                     mod |= KB_LSHIFT;
        else if (t == "LALT"   || t == "ALT")       mod |= KB_LALT;
        else if (t == "RCTRL"  || t == "RCONTROL")  mod |= KB_RCTRL;
        else if (t == "RSHIFT")                     mod |= KB_RSHIFT;
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    return mod;
}

// Prüft ob ein Token ein Modifier-Schlüsselwort ist (und keine Zahl).
static bool isModifierToken(const std::string& t) {
    std::string u;
    for (char c : t) u += (char)std::toupper((unsigned char)c);
    return u == "LCTRL" || u == "LCONTROL" || u == "LSHIFT" || u == "LALT" ||
           u == "ALT"   || u == "RCTRL"    || u == "RCONTROL" || u == "RSHIFT";
}

// Zerlegt "NAME|unit|field_id" oder Varianten → SimVarDef
static SimVarDef parseSimVarDef(const std::string& s) {
    SimVarDef sv;
    auto p1 = s.find('|');
    if (p1 == std::string::npos) {
        sv.name = trim(s);
        sv.unit = "number";
        return sv;
    }
    sv.name = trim(s.substr(0, p1));
    std::string rest = s.substr(p1 + 1);
    auto p2 = rest.find('|');
    if (p2 == std::string::npos) {
        sv.unit = trim(rest);
        return sv;
    }
    sv.unit = trim(rest.substr(0, p2));
    std::string idStr = trim(rest.substr(p2 + 1));
    if (!idStr.empty()) {
        try { sv.fieldId = (uint8_t)std::stoul(idStr, nullptr, 16); }
        catch (...) {}
    }
    return sv;
}

// Wendet Schlüssel/Wert auf eine ModuleConfig-Untersektion an.
// sub = "" → Basis-Parameter; "simvars" → nur ohne '=' (Listeneintrag, separat behandelt);
// "config", "buttons", "axes" → Untersektion-Einträge.
static void applyModuleSub(ModuleConfig& mod,
                           const std::string& sub,
                           const std::string& key,
                           const std::string& val,
                           std::vector<std::string>* warnings) {
    if (sub.empty()) {
        if      (key == "can_tx_id")             mod.canTxId            = (uint32_t)std::stoul(val, nullptr, 16);
        else if (key == "name")                 mod.deviceName         = val;
        else if (key == "description")          mod.description        = val;
        else if (key == "type")                 mod.type               = val;
        else if (key == "can_rx_id")             mod.canRxId             = (uint32_t)std::stoul(val, nullptr, 16);
        else if (key == "heartbeat_interval_ms") mod.heartbeatIntervalMs = std::stoi(val);
        else if (key == "heartbeat_timeout_ms")  mod.heartbeatTimeoutMs  = std::stoi(val);
    }
    else if (sub == "config") {
        mod.config[key] = val;
    }
    else if (sub == "axes") {
        // Format: name = hid_axis_idx [, MODIFIER]
        auto tokens = splitComma(val);
        if (!tokens.empty()) {
            uint8_t kbMod = KB_NONE;
            // Letztes Token: optionaler Modifier-String
            if (tokens.size() >= 2 && isModifierToken(tokens.back()))
                kbMod = parseKbMod(tokens.back());
            int axIdx = -1;
            try { axIdx = std::stoi(tokens[0]); } catch (...) {}
            if (axIdx >= 0 && axIdx <= 7) {
                AxisDef ax;
                ax.name       = key;
                ax.hidAxisIdx = (uint8_t)axIdx;
                ax.kbMod      = kbMod;
                mod.axes.push_back(ax);
            } else if (warnings) {
                warnings->push_back("module." + mod.iniKey + ".axes: entry '" + key
                                    + "' discarded - hid_axis_idx must be 0-7");
            }
        }
    }
    else if (sub == "buttons") {
        // Format: name = payload_byte, payload_bit, physical_idx, hid_a [, hid_b] [, MODIFIER]
        // MODIFIER ist ein optionaler abschliessender Text-Token (kein Integer).
        auto tokens = splitComma(val);
        uint8_t kbMod = KB_NONE;
        // Letztes Token: optionaler Modifier-String
        if (!tokens.empty() && isModifierToken(tokens.back())) {
            kbMod = parseKbMod(tokens.back());
            tokens.pop_back();
        }
        // Verbleibende Tokens als Integer parsen
        std::vector<int> parts;
        for (const auto& t : tokens) {
            try { parts.push_back(std::stoi(t)); } catch (...) {}
        }
        if (parts.size() >= 4) {
            ButtonDef btn;
            btn.name        = key;
            btn.payloadByte = (uint8_t)parts[0];
            btn.payloadBit  = (uint8_t)(parts[1] & 0x07);
            btn.physicalIdx = (uint8_t)parts[2];
            btn.hidA        = (uint8_t)parts[3];
            btn.hidB        = (parts.size() >= 5) ? (uint8_t)parts[4] : 0;
            btn.kbMod       = kbMod;
            if (btn.hidA >= 1 && btn.hidA <= 128
                && (btn.hidB == 0 || (btn.hidB >= 1 && btn.hidB <= 128)))
                mod.buttons.push_back(btn);
            else if (warnings)
                warnings->push_back("module." + mod.iniKey + ".buttons: entry '" + key
                                    + "' discarded - HID code out of range (1-128)");
        } else if (warnings && !val.empty()) {
            warnings->push_back("module." + mod.iniKey + ".buttons: entry '" + key
                                + "' discarded - expected 4+ integers, got: " + val);
        }
    }
}

bool Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(stripComment(line));
        if (line.empty()) continue;

        // Sektion
        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        // Modul-Sektionen: einziger unterstützter Präfix ist "module."
        size_t pfxLen = 0;
        if (section.size() > 7 && section.substr(0, 7) == "module.")
            pfxLen = 7;

        if (pfxLen > 0) {
            std::string rest  = section.substr(pfxLen);
            auto        dot   = rest.find('.');
            std::string mname = (dot == std::string::npos) ? rest : rest.substr(0, dot);
            std::string sub   = (dot == std::string::npos) ? "" : rest.substr(dot + 1);
            auto& mod = findOrCreateModule(modules, mname);

            // [module.X.simvars]: Zeilen ohne '=' sind Listeneintrag
            if (sub == "simvars" && line.find('=') == std::string::npos) {
                SimVarDef sv = parseSimVarDef(line);
                if (!sv.name.empty()) mod.simvars.push_back(sv);
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            applyModuleSub(mod, sub,
                           trim(line.substr(0, eq)),
                           trim(line.substr(eq + 1)),
                           &warnings);
            continue;
        }

        // Schlüssel = Wert für Nicht-Modul-Sektionen
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        // ── [field_ids] ───────────────────────────────────────────────────────
        if (section == "field_ids") {
            try { fieldIds[key] = (uint8_t)std::stoul(val, nullptr, 16); }
            catch (...) { warnings.push_back("field_ids: invalid hex value for '" + key + "': " + val); }
        }
        // ── [config_ids] ──────────────────────────────────────────────────────
        else if (section == "config_ids") {
            try { configIds[key] = (uint8_t)std::stoul(val, nullptr, 16); }
            catch (...) { warnings.push_back("config_ids: invalid hex value for '" + key + "': " + val); }
        }
        // ── [bridge] ─────────────────────────────────────────────────────────
        else if (section == "bridge") {
            if      (key == "udp_ip")        udpIp       = val;
            else if (key == "udp_port")      udpPort     = std::stoi(val);
            else if (key == "receive_port")  receivePort = std::stoi(val);
            else if (key == "send_interval_ms") sendIntervalMs = std::stoi(val);
            else if (key == "usb_port")      usbPort     = val;
            else if (key == "usb_baud")      usbBaud     = std::stoi(val);
        }
        // ── [simconnect] ──────────────────────────────────────────────────────
        else if (section == "simconnect") {
            if (key == "app_name") appName = val;
        }
        // ── [debug] ───────────────────────────────────────────────────────────
        else if (section == "debug") {
            if (key == "refresh_ms") refreshMs = std::stoi(val);
        }
        // ── [monitor] ─────────────────────────────────────────────────────────
        else if (section == "monitor") {
            if      (key == "enabled")    monitorEnabled   = (val == "true" || val == "1" || val == "yes");
            else if (key == "ip")         monitorIp        = val;
            else if (key == "port")       monitorPort      = std::stoi(val);
            else if (key == "cmd_port")   monitorCmdPort   = std::stoi(val);
            else if (key == "timeout_ms") monitorTimeoutMs = std::stoi(val);
        }
    }

    // Post-Processing: fieldId aus globaler Tabelle auflösen wenn inline nicht gesetzt
    for (auto& mod : modules) {
        for (auto& sv : mod.simvars) {
            if (sv.fieldId == 0 && !fieldIds.empty()) {
                auto it = fieldIds.find(sv.name);
                if (it != fieldIds.end())
                    sv.fieldId = it->second;
            }
        }
        // configEntries: config-Schlüssel gegen configIds auflösen
        mod.configEntries.clear();
        for (const auto& kv : mod.config) {
            auto it = configIds.find(kv.first);
            if (it == configIds.end()) {
                warnings.push_back("module." + mod.iniKey + ".config: '" + kv.first
                                   + "' hat keine config_id in [config_ids] — wird ignoriert");
                continue;
            }
            int val = 0;
            try { val = std::stoi(kv.second); } catch (...) {}
            mod.configEntries.push_back({kv.first, it->second, val});
        }
        if (mod.canRxId == 0)
            warnings.push_back("module." + mod.iniKey + ": can_rx_id fehlt — Rückkanal nicht konfiguriert");
    }
    return true;
}

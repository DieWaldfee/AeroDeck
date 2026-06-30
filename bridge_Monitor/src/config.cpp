#include "config.h"
#include <fstream>
#include <algorithm>

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

static std::vector<int> parseIntList(const std::string& s) {
    std::vector<int> result;
    std::string::size_type pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && (s[pos] == ',' || s[pos] == ' ' || s[pos] == '\t')) ++pos;
        if (pos >= s.size()) break;
        try {
            size_t consumed = 0;
            result.push_back(std::stoi(s.substr(pos), &consumed));
            pos += consumed;
        } catch (...) { break; }
    }
    return result;
}

static SimVarDef parseSimVarDef(const std::string& s) {
    SimVarDef sv;
    auto p1 = s.find('|');
    if (p1 == std::string::npos) { sv.name = trim(s); sv.unit = "number"; return sv; }
    sv.name = trim(s.substr(0, p1));
    std::string rest = s.substr(p1 + 1);
    auto p2 = rest.find('|');
    if (p2 == std::string::npos) { sv.unit = trim(rest); return sv; }
    sv.unit = trim(rest.substr(0, p2));
    std::string idStr = trim(rest.substr(p2 + 1));
    if (!idStr.empty()) {
        try { sv.fieldId = (uint8_t)std::stoul(idStr, nullptr, 16); } catch (...) {}
    }
    return sv;
}

static ModuleConfig& findOrCreate(std::vector<ModuleConfig>& v, const std::string& key) {
    for (auto& m : v) if (m.iniKey == key) return m;
    v.push_back({});
    v.back().iniKey = key;
    return v.back();
}

bool Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::vector<ModuleConfig> tmpMods;
    std::string line, section;

    while (std::getline(f, line)) {
        line = trim(stripComment(line));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        // ── [module.*] ────────────────────────────────────────────────────────
        if (section.size() > 7 && section.substr(0, 7) == "module.") {
            std::string rest  = section.substr(7);
            auto        dot   = rest.find('.');
            std::string mname = (dot == std::string::npos) ? rest : rest.substr(0, dot);
            std::string sub   = (dot == std::string::npos) ? "" : rest.substr(dot + 1);
            auto& m = findOrCreate(tmpMods, mname);

            // Simvars: Zeilen ohne '=' sind Listeneinträge
            if (sub == "simvars" && line.find('=') == std::string::npos) {
                SimVarDef sv = parseSimVarDef(line);
                if (!sv.name.empty()) m.simvars.push_back(sv);
                continue;
            }

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = trim(line.substr(0, eq));
            std::string val = trim(line.substr(eq + 1));

            if (sub.empty()) {
                if      (key == "can_tx_id")             m.canTxId            = (uint32_t)std::stoul(val, nullptr, 16);
                else if (key == "name")                  m.deviceName         = val;
                else if (key == "description")           m.description        = val;
                else if (key == "type")                  m.type               = val;
                else if (key == "can_rx_id")             m.canRxId            = (uint32_t)std::stoul(val, nullptr, 16);
                else if (key == "heartbeat_interval_ms") m.heartbeatIntervalMs = std::stoi(val);
                else if (key == "heartbeat_timeout_ms")  m.heartbeatTimeoutMs  = std::stoi(val);
            } else if (sub == "config") {
                m.config[key] = val;
            } else if (sub == "axes") {
                auto parts = parseIntList(val);
                if (!parts.empty() && parts[0] >= 0 && parts[0] <= 7) {
                    AxisDef ax;
                    ax.name       = key;
                    ax.hidAxisIdx = (uint8_t)parts[0];
                    m.axes.push_back(ax);
                }
            } else if (sub == "buttons") {
                auto parts = parseIntList(val);
                if (parts.size() >= 4) {
                    ButtonDef btn;
                    btn.name        = key;
                    btn.payloadByte = (uint8_t)parts[0];
                    btn.payloadBit  = (uint8_t)(parts[1] & 0x07);
                    btn.physicalIdx = (uint8_t)parts[2];
                    btn.hidA        = (uint8_t)parts[3];
                    btn.hidB        = (parts.size() >= 5) ? (uint8_t)parts[4] : 0;
                    if (btn.hidA >= 1 && btn.hidA <= 128
                        && (btn.hidB == 0 || (btn.hidB >= 1 && btn.hidB <= 128)))
                        m.buttons.push_back(btn);
                }
            }
            continue;
        }

        // ── Schlüssel = Wert für Nicht-Modul-Sektionen ────────────────────────
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (section == "monitor") {
            if      (key == "enabled")    enabled   = (val == "true" || val == "1" || val == "yes");
            else if (key == "ip")         ip        = val;
            else if (key == "port")       port      = std::stoi(val);
            else if (key == "cmd_port")   cmdPort   = std::stoi(val);
            else if (key == "lwt_ms")     lwtMs     = std::stoi(val);
            else if (key == "timeout_ms") timeoutMs = std::stoi(val);
        } else if (section == "field_ids") {
            try { fieldIds[key] = (uint8_t)std::stoul(val, nullptr, 16); } catch (...) {}
        } else if (section == "unit_sizes") {
            try { unitSizes[key] = (size_t)std::stoul(val); } catch (...) {}
        }
    }

    // ── Post-Processing: fieldId aus globaler Tabelle auflösen ────────────────
    for (auto& m : tmpMods) {
        for (auto& sv : m.simvars) {
            if (sv.fieldId == 0 && !fieldIds.empty()) {
                auto it = fieldIds.find(sv.name);
                if (it != fieldIds.end()) sv.fieldId = it->second;
            }
        }
    }

    // Alle Module mit definiertem canTxId übernehmen
    for (auto& m : tmpMods) {
        if (m.canTxId != 0 && (!m.simvars.empty() || !m.buttons.empty() || !m.axes.empty()))
            modules.push_back(std::move(m));
    }

    return true;
}

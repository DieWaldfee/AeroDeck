#pragma once
#include <string>
#include <vector>
#include <map>
#include "module.h"

struct Config {
    // [monitor]
    bool        enabled    = false;
    std::string ip         = "127.0.0.1";  // wo dieser Monitor lauscht (eigene IP)
    int         port       = 4220;         // UDP-Port auf dem wir Frames empfangen
    int         cmdPort    = 4221;         // Port auf dem bridge_exe Kommandos empfängt
    int         lwtMs      = 2000;         // Intervall für MON_LWT Heartbeat → bridge_exe
    int         timeoutMs  = 6000;         // nach N ms ohne PONG → bridge_exe OFFLINE

    // Globale Field-ID-Tabelle
    std::map<std::string, uint8_t> fieldIds;

    // Einheiten → Byte-Größe im CAN-Frame (aus [unit_sizes] in bridge.ini)
    std::map<std::string, size_t> unitSizes;

    // Alle konfigurierten Module
    std::vector<ModuleConfig> modules;

    bool load(const std::string& path);
};

#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include "module.h"

// CAN-ID für den ESP32-Bridge-eigenen Heartbeat (ESP32_HB_CAN_ID)
// Muss mit ESP32_HB_CAN_ID in bridge_ESP32/src/config.h übereinstimmen.
constexpr uint32_t ESP32_HB_CAN_ID = 0x01;

// CAN-ID für Mapping-Kommandos bridge.exe → ESP32 (HID_MAP_CAN_ID)
// Muss mit HID_MAP_CAN_ID in bridge_ESP32/src/config.h übereinstimmen.
constexpr uint32_t HID_MAP_CAN_ID = 0x7F0;

// CAN-ID für Mapping-Empfangsbestätigungen ESP32 → bridge.exe (HID_MAP_ACK_CAN_ID)
// Muss mit HID_MAP_ACK_CAN_ID in bridge_ESP32/src/config.h übereinstimmen.
constexpr uint32_t HID_MAP_ACK_CAN_ID = 0x7F1;

// CAN-ID für Mapping-Anforderungen ESP32 → bridge.exe (HID_MAP_REQ_CAN_ID)
// ESP32 sendet nach jedem Boot REQUEST-Frames bis bridge.exe das Mapping schickt.
// Muss mit HID_MAP_REQ_CAN_ID in bridge_ESP32/src/config.h übereinstimmen.
constexpr uint32_t HID_MAP_REQ_CAN_ID = 0x7F2;

struct Config {
    // [bridge]
    std::string udpIp       = "192.168.1.100";
    int         udpPort     = 4210;
    int         receivePort = 4211;
    int         sendIntervalMs = 50;
    std::string usbPort;
    int         usbBaud     = 921600;

    // [simconnect]
    std::string appName = "MSFS_Bridge";

    // [debug]
    int refreshMs = 100;

    // [monitor]
    bool        monitorEnabled   = false;
    std::string monitorIp        = "127.0.0.1";
    int         monitorPort      = 4220;
    int         monitorCmdPort   = 4221;
    int         monitorTimeoutMs = 6000;

    // Field-ID-Tabelle aus [field_ids]
    std::map<std::string, uint8_t> fieldIds;

    // Config-ID-Tabelle aus [config_ids]
    std::map<std::string, uint8_t> configIds;

    // Alle Module aus [module.X]
    std::vector<ModuleConfig> modules;

    // Warnungen die während des Parsens entstanden sind (ungültige Einträge)
    std::vector<std::string> warnings;

    bool load(const std::string& path);
};

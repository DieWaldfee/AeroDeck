#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

struct SimVarDef {
    std::string name;
    std::string unit    = "number";
    uint8_t     fieldId = 0;
};

struct ButtonDef {
    std::string name;
    uint8_t     payloadByte = 0;
    uint8_t     payloadBit  = 0;
    uint8_t     physicalIdx = 0;
    uint8_t     hidA        = 0;
    uint8_t     hidB        = 0;
};

struct AxisDef {
    std::string name;
    uint8_t     hidAxisIdx = 0;
};

// Einheitlicher Modultyp — gilt für alle CAN-FD-Geräte.
// hasOutput() → sendet SimConnect-Daten (Anzeigegeräte, Signalleuchten)
// hasInput()  → empfängt Achsen/Buttons (Eingabegeräte)
// Beides gleichzeitig möglich (gemischte Geräte).
struct ModuleConfig {
    std::string iniKey;
    std::string deviceName;
    uint32_t    canTxId             = 0;
    uint32_t    canRxId             = 0;    // Pflichtfeld, systemweit eindeutig
    std::string description;
    std::string type;
    int         heartbeatIntervalMs = 1000;
    int         heartbeatTimeoutMs  = 3000;
    std::map<std::string, std::string> config;
    std::vector<SimVarDef>   simvars;
    std::vector<ButtonDef>   buttons;
    std::vector<AxisDef>     axes;

    bool hasOutput() const { return !simvars.empty(); }
    bool hasInput()  const { return !buttons.empty() || !axes.empty(); }
};

// Einheitlicher Laufzeit-Zustand eines Moduls im Monitor
struct ModuleState {
    const ModuleConfig* cfg          = nullptr;
    bool      online                 = false;
    bool      everSeen               = false;
    // CAN-Frame (bridge → Modul, via MON_FRAME_FORWARD)
    long long lastFrameMs            = 0;
    uint32_t  frameCount             = 0;
    uint8_t   lastFrame[64]          = {};
    size_t    lastFrameLen           = 0;
    // Feedback (via MON_FEEDBACK_FORWARD / MON_RX_FRAME)
    long long lastFeedbackMs         = 0;
    uint8_t   lastFeedback[64]       = {};
    size_t    lastFeedbackLen        = 0;
    // Heartbeat [0x01][uptime_ms 4B LE]
    long long lastHeartbeatMs        = 0;
    uint32_t  heartbeatUptime        = 0;
    // Poll-Response [0x02][seq][ts_echo 4B][Nutzdaten...]
    long long lastPollMs             = 0;
    uint8_t   lastPollFeedback[64]   = {};
    size_t    lastPollFeedbackLen    = 0;
    // Dekodierte Achswerte (nur wenn hasInput())
    int16_t   axisValues[8]          = {};
};

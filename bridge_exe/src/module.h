#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

// Keyboard-Modifier-Bits (HID-Spec, Byte 0 des Keyboard-Reports).
// Muss mit KB_* in bridge_ESP32/src/config_manager.h identisch sein.
constexpr uint8_t KB_NONE   = 0x00;
constexpr uint8_t KB_LCTRL  = 0x01;
constexpr uint8_t KB_LSHIFT = 0x02;
constexpr uint8_t KB_LALT   = 0x04;
constexpr uint8_t KB_RCTRL  = 0x10;
constexpr uint8_t KB_RSHIFT = 0x20;

// Eine SimConnect-Variable mit ihrer Einheit und optionaler Field-ID.
// Einheit optional: "PLANE BANK DEGREES|radians" → name="PLANE BANK DEGREES", unit="radians"
//                   "TURN COORDINATOR BALL"       → name="TURN COORDINATOR BALL", unit="number"
struct SimVarDef {
    std::string name;
    std::string unit    = "number";
    uint8_t     fieldId = 0;   // 0 = kein Init-Paket-Eintrag für diese Variable
};

struct ButtonDef {
    std::string name;          // Lesbare Bezeichnung (z.B. "COM", "FLAP1")
    uint8_t     payloadByte;   // Byte-Offset im CAN-Payload (0-basiert)
    uint8_t     payloadBit;    // Bit im Byte (0=LSB, 7=MSB)
    uint8_t     physicalIdx;   // Referenznummer 0..127 (für SHOW MAP / Diagnose)
    uint8_t     hidA;          // Erster HID-Joystick-Button (1..128)
    uint8_t     hidB;          // Zweiter HID-Button (0=Single, 1..128=Chord)
    uint8_t     kbMod = 0;     // Keyboard-Modifier-Bitmask (KB_* Konstanten, 0=keine Taste)
};

struct AxisDef {
    std::string name;          // Lesbare Bezeichnung (z.B. "AXIS1", "SLIDER")
    uint8_t     hidAxisIdx;    // HID-Achsen-Index (0..7)
    uint8_t     kbMod = 0;     // Keyboard-Modifier-Bitmask (KB_* Konstanten, 0=keine Taste)
};

// Beschreibt ein CAN-FD-Gerät. Kann SimConnect-Werte empfangen (Output → Signalleuchten,
// Anzeigen), Buttons/Achsen senden (Input → HID), oder beides gleichzeitig.
// Wird aus bridge.ini geladen; eine Instanz pro [module.X]-Block.
struct ModuleConfig {
    std::string  iniKey;
    std::string  deviceName;          // Kurzname für Debug/CAN-Init (z.B. "k_Horizon")
    uint32_t     canTxId            = 0;
    std::string  description;
    std::string  type;                // "attitude_indicator", … → bestimmt Frame-Format

    int          heartbeatIntervalMs = 1000; // Modul sendet alle N ms einen Heartbeat
    int          heartbeatTimeoutMs  = 3000; // bridge.exe → OFFLINE nach N ms ohne Heartbeat
    uint32_t     canRxId             = 0;    // Rückkanal-CAN-ID (Pflichtfeld, systemweit eindeutig)

    // Output: SimConnect-Werte → CAN-FD (Anzeigen, Signalleuchten, Lichtleisten)
    std::vector<SimVarDef>   simvars;

    // Input: CAN-FD → HID (Buttons, Achsen)
    std::vector<ButtonDef>   buttons;
    std::vector<AxisDef>     axes;

    // Init-Parameter (beim Start einmalig per CAN gesendet, z.B. brightness, orientation)
    std::map<std::string, std::string> config;

    // Aufgelöste Config-Einträge (befüllt nach INI-Laden aus config + [config_ids])
    struct ConfigEntry {
        std::string name;
        uint8_t     configId;
        int         value;
    };
    std::vector<ConfigEntry> configEntries;

    bool hasOutput() const { return !simvars.empty(); }
    bool hasInput()  const { return !buttons.empty() || !axes.empty(); }

    int cfgInt(const std::string& key, int def = 0) const {
        auto it = config.find(key);
        if (it == config.end()) return def;
        try { return std::stoi(it->second); } catch (...) { return def; }
    }
};

#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include "module.h"
#include "simconnect_client.h"
#include "config.h"

enum class BridgeStatus { WAITING, CONNECTED, DISCONNECTED };

struct DeviceStatus {
    uint32_t    canTxId     = 0;
    std::string name;
    bool        online      = false;
    bool        everSeen    = false;
    long long   silenceSecs = 0;
    bool        isMonitor   = false;
};

struct DebugState {
    BridgeStatus    status          = BridgeStatus::WAITING;
    SimData         simData;
    float           hz              = 0.f;

    std::string     transportTarget;
    std::string     transportError;
    std::string     statusMsg;
    uint64_t        framesSentMod   = 0;
    long long       lastSendMs      = 0;
    int             errorCount      = 0;

    ModuleConfig     primaryMod;
    uint8_t          primaryFrame[64] = {};
    size_t           primaryFrameSize = 0;
    int              moduleCount      = 0;

    std::vector<DeviceStatus> devices;
    std::vector<size_t>      simvarFrameOffset;
    std::vector<size_t>      simvarFrameBytes;

    // RX-Seite: letzter empfangener Frame des primären Moduls
    uint8_t          rxPayload[64]    = {};
    size_t           rxLen            = 0;
    uint64_t         framesReceived   = 0;
    long long        lastRxMs         = 0;
    int16_t          axisValues[8]    = {};
    bool             online           = false;
    bool             everSeen         = false;
    long long        silenceSecs      = 0;
    long long        lastHeartbeatMs  = 0;     // letzter HB des primären Moduls

    // Dekodierte Feedback-Werte aus letzter STATUS_RESPONSE (für Nutzdaten RX-Spalte)
    std::vector<double>      feedbackValues;
    std::vector<std::string> feedbackUnits;

    // Roher 0x04-Frame des letzten Poll-Feedbacks (für rxHex + Achsen/Button-[..])
    uint8_t          feedbackPayload[64] = {};
    size_t           feedbackPayloadLen  = 0;

    // Poll-Indikator: gesetzt wenn [P] gedrückt, 3 Sekunden sichtbar
    bool             pollSent         = false;
    long long        pollSentMs       = 0;

    bool             usbActive        = false;  // true wenn Serial-Transport aktiv
};

class DebugConsole {
public:
    // simvarCount: Anzahl SimVars des primären Moduls (0 = Input-Only)
    // deviceCount: Anzahl Geräte im Geräte-Status-Block
    // axisCount/buttonCount: für Input-Module
    void init(int refreshMs, int simvarCount, int deviceCount,
              int axisCount = 0, int buttonCount = 0);
    void update(const DebugState& state);

private:
    void setupConsole(int rows);
    void render(const DebugState& state);
    void renderHexLine(int row, int lineStart, const uint8_t* data,
                       int usedLen, const WORD* colors);
    void setColor(WORD attr);
    void printAt(int x, int y, const char* fmt, ...);

    HANDLE    m_hOut           = INVALID_HANDLE_VALUE;
    int       m_refreshMs      = 100;
    long long m_lastRenderMs   = 0;
    int       m_consoleRows    = 30;
    int       m_renderRows     = 30;
    int       m_renderCols     = 80;
    int       m_winCols        = 80;
    SHORT     m_lastWindowRows = 0;
    SHORT     m_lastWindowCols = 0;
};

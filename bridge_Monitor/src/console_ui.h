#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include "module_registry.h"

struct MonitorState {
    bool          connected         = false;
    float         hz                = 0.f;
    int           port              = 4220;
    int           cmdPort           = 4221;
    std::string   bridgeIp          = "127.0.0.1";
    bool          pullSent          = false;
    long long     pullSentMs        = 0;
    int           pullModuleIdx     = -1;
    bool          bridgeOnline      = false;
    bool          bridgeEverSeen    = false;
    int           rttMs             = -1;
    long long     bridgeSilenceSecs = 0;
};

enum class UiEvent { NONE, SELECT_UP, SELECT_DOWN, PULL, QUIT };

class ConsoleUi {
public:
    // moduleCount:   Anzahl Module gesamt
    // maxSimvars:    max. SimVars über alle Module
    // maxConfig:     max. Config-Einträge über alle Module
    // maxButtons:    max. Buttons pro Modul
    // maxAxes:       max. Achsen pro Modul
    void init(int moduleCount, int maxSimvars, int maxConfig,
              int maxButtons = 0, int maxAxes = 0);

    // Übergibt die Byte-Größen-Tabelle aus bridge.ini.
    void setUnitSizes(const std::map<std::string, size_t>& sizes);

    void update(const ModuleRegistry& reg, const MonitorState& state,
                int selectedIdx, int refreshMs);

    UiEvent pollInput();

private:
    bool render(const ModuleRegistry& reg, const MonitorState& state, int selectedIdx);
    void renderList(const ModuleRegistry& reg, int selectedIdx, int& row);
    void renderDetail(const ModuleState& s, int& row);
    void renderSimvars(const ModuleConfig& cfg, int& row);
    void renderSentFrame(const ModuleState& s, int& row);

    size_t unitBytes(const std::string& unit) const;

    void setColor(WORD attr);
    void printAt(int x, int y, const char* fmt, ...);
    void hline(int& row);
    void boxTop(int& row);
    void boxBot(int& row);

    HANDLE    m_hOut            = INVALID_HANDLE_VALUE;
    std::map<std::string, size_t> m_unitSizes;
    int       m_W               = 76;
    int       m_consoleRows     = 40;
    int       m_renderRows      = 40;
    int       m_renderCols      = 80;
    int       m_winCols         = 80;   // tatsächliche Fensterbreite (für Gutter-Clear)
    bool      m_clipped         = false;
    long long m_lastRenderMs    = 0;
    int       m_lastSelectedIdx = -1;
    SHORT     m_lastWindowRows  = 0;
    SHORT     m_lastWindowCols  = 0;
};

#include "console_ui.h"
#include "monitor_protocol.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <chrono>
#include <conio.h>
#include <algorithm>
#include <string>

// ── Farben ────────────────────────────────────────────────────────────────────
static constexpr WORD C_WHITE  = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static constexpr WORD C_GREEN  = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static constexpr WORD C_RED    = FOREGROUND_RED   | FOREGROUND_INTENSITY;
static constexpr WORD C_YELLOW = FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static constexpr WORD C_CYAN   = FOREGROUND_GREEN | FOREGROUND_BLUE  | FOREGROUND_INTENSITY;
static constexpr WORD C_GRAY   = FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_BLUE;

static constexpr WORD C_FIELD[] = {
    FOREGROUND_GREEN | FOREGROUND_BLUE  | FOREGROUND_INTENSITY,  // Hellcyan
    FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // Hellgelb
    FOREGROUND_RED   | FOREGROUND_BLUE  | FOREGROUND_INTENSITY,  // Hellmagenta
    FOREGROUND_GREEN                    | FOREGROUND_INTENSITY,  // Hellgrün
    FOREGROUND_RED                      | FOREGROUND_INTENSITY,  // Hellrot
    FOREGROUND_BLUE                     | FOREGROUND_INTENSITY,  // Hellblau
    FOREGROUND_GREEN | FOREGROUND_BLUE,                          // Dunkeltürkis
    FOREGROUND_RED   | FOREGROUND_GREEN,                         // Dunkelgelb
    FOREGROUND_RED   | FOREGROUND_BLUE,                          // Dunkelmagenta
    FOREGROUND_GREEN,                                            // Dunkelgrün
    FOREGROUND_RED,                                              // Dunkelrot
    FOREGROUND_BLUE,                                             // Dunkelblau
    FOREGROUND_RED   | FOREGROUND_INTENSITY,                     // (Wiederholung)
};
static constexpr int C_FIELD_N = 13;

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void ConsoleUi::setUnitSizes(const std::map<std::string, size_t>& sizes) {
    m_unitSizes = sizes;
}

size_t ConsoleUi::unitBytes(const std::string& unit) const {
    auto it = m_unitSizes.find(unit);
    if (it != m_unitSizes.end()) return it->second;
    return 1;
}

static std::string decodeBytes(const uint8_t* frame, size_t frameLen,
                                size_t offset, const std::string& unit, size_t nbytes) {
    if (offset + nbytes > frameLen) return "?";
    char buf[24];
    if (nbytes == 4) {
        float v; std::memcpy(&v, frame + offset, 4);
        if (unit == "radians") snprintf(buf, sizeof(buf), "%+7.2f\xF8", v * 57.2957795f);
        else                   snprintf(buf, sizeof(buf), "%+9.3f",      v);
    } else if (nbytes == 2) {
        int16_t v; std::memcpy(&v, frame + offset, 2);
        snprintf(buf, sizeof(buf), "%+6d", (int)v);
    } else {
        uint8_t v = frame[offset];
        if (unit == "bool")   snprintf(buf, sizeof(buf), "%s", v ? "ON " : "OFF");
        else                  snprintf(buf, sizeof(buf), "0x%02X", v);
    }
    return buf;
}

static std::string hexStr(const uint8_t* data, size_t len) {
    char buf[64] = {};
    int  pos     = 0;
    for (size_t i = 0; i < len && pos < 60; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X%s",
                        data[i], (i + 1 < len) ? " " : "");
    return buf;
}

// ── ConsoleUi ─────────────────────────────────────────────────────────────────

void ConsoleUi::init(int moduleCount, int maxSimvars, int maxConfig,
                     int maxButtons, int maxAxes) {
    m_hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    const int listRows   = 2 + moduleCount;
    const int svRows     = 1 + maxSimvars;
    const int cfgRows    = maxConfig > 0 ? 1 + maxConfig : 0;
    // Detail-Höhe Ausgangsmodule (simvars + config + frame + nutzdaten)
    const int outDetail  = (maxSimvars > 0)
        ? (3 + 1 + svRows + cfgRows + 1 + 2 + (1 + maxSimvars) + 1 + (3 + maxSimvars) + 1)
        : 0;
    // Detail-Höhe Eingangsmodule (achsen + buttons + RX-frame)
    const int inDetail   = (maxAxes > 0 || maxButtons > 0)
        ? (4 + (maxAxes    > 0 ? 1 + maxAxes    : 0)
             + (maxButtons > 0 ? 1 + maxButtons : 0) + 2)
        : 0;
    // Feedback-Block (immer angezeigt): hline + header + HB + Poll + Werte
    const int fbDetail   = 1 + 1 + 1 + 1 + std::max(maxSimvars, maxAxes + maxButtons);
    const int detailRows = std::max(outDetail, inDetail) + fbDetail;
    m_consoleRows = 4 + listRows + 1 + detailRows + 1;

    SetConsoleOutputCP(437);
    SetConsoleCP(437);

    CONSOLE_FONT_INFOEX cfi = {};
    cfi.cbSize      = sizeof(cfi);
    cfi.dwFontSize  = { 0, 16 };
    cfi.FontFamily  = FF_DONTCARE;
    cfi.FontWeight  = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"Lucida Console");
    SetCurrentConsoleFontEx(m_hOut, FALSE, &cfi);

    CONSOLE_CURSOR_INFO ci = { 1, FALSE };
    SetConsoleCursorInfo(m_hOut, &ci);

    COORD      size    = { 80, (SHORT)m_consoleRows };
    SMALL_RECT winRect = { 0, 0, 79, (SHORT)(m_consoleRows - 1) };

    COORD  origin = { 0, 0 };
    DWORD  written;

    // Schritt 1: Vollständiges Clear via VT-Escape-Sequenz.
    // \033[2J = sichtbaren Puffer leeren, \033[3J = Scrollback leeren, \033[H = Cursor home.
    // Wirkt auf die echte Terminalbreite, unabhängig davon was csbi.dwSize.X meldet
    // (wichtig bei Windows Terminal / ConPTY, wo der Puffer nach vorherigem Resize
    // bereits auf 80 gesetzt sein kann, das Terminal-Fenster aber deutlich breiter ist).
    {
        DWORD mode = 0;
        GetConsoleMode(m_hOut, &mode);
        SetConsoleMode(m_hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        const char* clr = "\033[2J\033[3J\033[H";
        WriteConsoleA(m_hOut, clr, (DWORD)strlen(clr), &written, nullptr);
        SetConsoleMode(m_hOut, mode);   // Originalmodus wiederherstellen
    }

    // Schritt 2: Resize — Fenster zuerst, dann Puffer.
    SetConsoleWindowInfo(m_hOut, TRUE, &winRect);
    SetConsoleScreenBufferSize(m_hOut, size);
    SetConsoleWindowInfo(m_hOut, TRUE, &winRect);

    SetConsoleCursorPosition(m_hOut, origin);
}

void ConsoleUi::setColor(WORD attr) {
    SetConsoleTextAttribute(m_hOut, attr);
}

void ConsoleUi::printAt(int x, int y, const char* fmt, ...) {
    if (y < 0 || y >= m_renderRows) return;
    if (x < 0 || x >= m_renderCols) return;
    char buf[256];
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    int maxLen = m_renderCols - x;
    int len    = (int)strlen(buf);
    if (len > maxLen) {
        buf[maxLen] = '\0';
        if (maxLen > 0) buf[maxLen - 1] = '>';
    }
    COORD pos = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(m_hOut, pos);
    DWORD written;
    WriteConsoleA(m_hOut, buf, (DWORD)strlen(buf), &written, nullptr);
}

void ConsoleUi::hline(int& row) {
    setColor(C_WHITE);
    printAt(0, row++, "\xCC%s\xB9", std::string(m_W, '\xCD').c_str());
}

void ConsoleUi::boxTop(int& row) {
    setColor(C_WHITE);
    printAt(0, row++, "\xC9%s\xBB", std::string(m_W, '\xCD').c_str());
}

void ConsoleUi::boxBot(int& row) {
    setColor(C_WHITE);
    printAt(0, row++, "\xC8%s\xBC", std::string(m_W, '\xCD').c_str());
}

void ConsoleUi::update(const ModuleRegistry& reg, const MonitorState& state,
                        int selectedIdx, int refreshMs) {
    if (m_hOut == INVALID_HANDLE_VALUE) return;
    long long now = nowMs();
    if (refreshMs > 0 && (now - m_lastRenderMs) < refreshMs) return;
    m_lastRenderMs = now;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    SHORT visibleRows = m_consoleRows;
    SHORT visibleCols = 80;
    if (GetConsoleScreenBufferInfo(m_hOut, &csbi)) {
        visibleRows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        visibleCols = csbi.srWindow.Right  - csbi.srWindow.Left  + 1;
    }
    m_renderRows = std::min(m_consoleRows, (int)visibleRows);
    m_renderCols = std::min(80,            (int)visibleCols);
    m_winCols    = (int)csbi.dwSize.X;

    if (selectedIdx != m_lastSelectedIdx
     || visibleRows  != m_lastWindowRows
     || visibleCols  != m_lastWindowCols) {
        DWORD written;
        COORD origin = { 0, 0 };
        DWORD cells  = (DWORD)(csbi.dwSize.X * csbi.dwSize.Y);
        FillConsoleOutputCharacterA(m_hOut, ' ',     cells, origin, &written);
        FillConsoleOutputAttribute (m_hOut, C_WHITE, cells, origin, &written);
        m_lastSelectedIdx = selectedIdx;
        m_lastWindowRows  = visibleRows;
        m_lastWindowCols  = visibleCols;
    }

    m_clipped = false;
    bool clipped = render(reg, state, selectedIdx);

    if (clipped) {
        setColor(C_RED | FOREGROUND_INTENSITY);
        printAt(0, m_renderRows - 1, "\xCC%-*.*s\xB9", m_W, m_W,
                " Fenster zu klein \xB3 Inhalt abgeschnitten ");
        setColor(C_GRAY);
    }
}

// ── Haupt-Render ──────────────────────────────────────────────────────────────
bool ConsoleUi::render(const ModuleRegistry& reg, const MonitorState& state,
                        int selectedIdx) {
    int row = 0;
    const int W = m_W;

    // Gutter-Clear: Spalten rechts vom Box-Rahmen (W+2 = 78) leeren.
    // Verhindert Fragmente wenn das Terminal breiter als 80 Zeichen ist.
    if (m_winCols > W + 2) {
        DWORD wr;
        const DWORD gutterLen = (DWORD)(m_winCols - (W + 2));
        for (int r = 0; r < m_renderRows; r++) {
            COORD pos = { (SHORT)(W + 2), (SHORT)r };
            FillConsoleOutputCharacterA(m_hOut, ' ', gutterLen, pos, &wr);
        }
    }

    // ── Oberer Rahmen + Header ────────────────────────────────────────────────
    boxTop(row);

    setColor(C_WHITE);
    printAt(0, row, "\xBA  Bridge Monitor  \xB3  %d Modul%s  \xB3  UDP :%d  \xB3  Cmd :%d",
            reg.count(), (reg.count() == 1 ? "" : "e"), state.port, state.cmdPort);
    {
        long long elapsed = nowMs() - state.pullSentMs;
        long long modPollMs = (state.pullModuleIdx >= 0 && state.pullModuleIdx < reg.count())
                              ? reg.stateAt(state.pullModuleIdx).lastPollMs : 0LL;
        bool respReceived = (modPollMs > state.pullSentMs);
        bool showPull = state.pullSent
                        && selectedIdx == state.pullModuleIdx
                        && (elapsed < 1000 || !respReceived);
        const char* pullTxt = showPull ? "  \xB3 PULL " : "         ";
        printAt(68, row, "%s", pullTxt);
    }
    setColor(C_WHITE);
    printAt(W + 1, row++, "\xBA");

    setColor(C_WHITE);
    printAt(0, row, "\xBA  ");
    if (state.connected) {
        setColor(C_GREEN);
        printAt(3, row, "\xFE Datenfluss: AKTIV  %5.1f Hz", state.hz);
    } else {
        setColor(C_YELLOW);
        printAt(3, row, "\xF9 Datenfluss: kein Signal            ");
    }
    setColor(C_WHITE);
    printAt(34, row, "  \xB3  ");
    if (state.bridgeOnline) {
        setColor(C_GREEN);
        if (state.rttMs >= 0)
            printAt(39, row, "\xFE bridge_exe: ONLINE  RTT:%3d ms", state.rttMs);
        else
            printAt(39, row, "\xFE bridge_exe: ONLINE           ");
    } else if (state.bridgeEverSeen) {
        setColor(C_RED);
        printAt(39, row, "\xF9 bridge_exe: OFFLINE  %3lld s", state.bridgeSilenceSecs);
    } else {
        setColor(C_YELLOW);
        printAt(39, row, "\xF9 bridge_exe: warte auf PONG...");
    }
    setColor(C_WHITE);
    printAt(W + 1, row++, "\xBA");

    hline(row);
    setColor(C_GRAY);
    printAt(0, row, "\xBA  \x18\x19: Modul ausw\x84hlen   P: Pull   Q: Beenden");
    setColor(C_WHITE);
    printAt(W + 1, row++, "\xBA");

    // ── Modul-Liste ───────────────────────────────────────────────────────────
    if (row >= m_renderRows - 1) return true;
    hline(row);
    renderList(reg, selectedIdx, row);

    // ── Detail-Ansicht ────────────────────────────────────────────────────────
    if (reg.count() > 0 && selectedIdx >= 0 && selectedIdx < reg.count()) {
        if (row >= m_renderRows - 1) return true;
        hline(row);
        renderDetail(reg.stateAt(selectedIdx), row);
    }

    // ── Unterer Rahmen ────────────────────────────────────────────────────────
    if (row < m_renderRows && !m_clipped) boxBot(row);

    setColor(C_GRAY);
    return m_clipped;
}

// ── Modul-Liste ───────────────────────────────────────────────────────────────
void ConsoleUi::renderList(const ModuleRegistry& reg, int selectedIdx, int& row) {
    const int W = m_W;
    setColor(C_WHITE);
    printAt(0, row++, "\xBA%-*s\xBA", W, "  MODULE");

    for (int i = 0; i < reg.count(); ++i) {
        if (row >= m_renderRows - 1) { m_clipped = true; break; }
        const ModuleState&  s = reg.stateAt(i);
        const ModuleConfig& c = *s.cfg;
        const bool  sel   = (i == selectedIdx);
        const char* arrow = sel ? "\xAF " : "  ";
        const char* dot   = s.online ? "\xFE" : "\xF9";
        WORD        col   = s.online ? C_GREEN : (s.everSeen ? C_YELLOW : C_RED);

        // Typ-Kennzeichen
        const char* typeTag = "";
        if      (c.hasOutput() && c.hasInput())            typeTag = "[OUT+IN]";
        else if (c.hasOutput())                            typeTag = "        ";
        else if (!c.axes.empty() && !c.buttons.empty())   typeTag = "[AX+BTN]";
        else if (!c.axes.empty())                          typeTag = "[AXIS]  ";
        else                                               typeTag = "[BTN]   ";

        char inner[80];
        const char* name = c.deviceName.empty() ? c.iniKey.c_str() : c.deviceName.c_str();
        if (s.online) {
            long long ago = (s.lastFeedbackMs > s.lastFrameMs && s.lastFeedbackMs > 0)
                            ? (nowMs() - s.lastFeedbackMs)
                            : (s.lastFrameMs > 0 ? (nowMs() - s.lastFrameMs) : -1LL);
            if (ago >= 0)
                snprintf(inner, sizeof(inner), "%s%s 0x%03X  %-14s %s  \xFE ONLINE  Frames:%6u  vor %lld ms",
                         arrow, dot, c.canTxId, name, typeTag, s.frameCount, ago);
            else
                snprintf(inner, sizeof(inner), "%s%s 0x%03X  %-14s %s  \xFE ONLINE  Frames:%6u",
                         arrow, dot, c.canTxId, name, typeTag, s.frameCount);
        } else {
            snprintf(inner, sizeof(inner), "%s%s 0x%03X  %-14s %s  \xF9 OFFLINE  Frames:%6u",
                     arrow, dot, c.canTxId, name, typeTag, s.frameCount);
        }

        setColor(C_WHITE);
        printAt(0, row, "\xBA");
        if (sel) setColor(C_WHITE | BACKGROUND_BLUE);
        else     setColor(col);
        printAt(1, row, "%-*.*s", W, W, inner);
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    }
}

// ── SimVars-Block ─────────────────────────────────────────────────────────────
void ConsoleUi::renderSimvars(const ModuleConfig& cfg, int& row) {
    const int W = m_W;
    setColor(C_WHITE);
    printAt(0, row++, "\xBA%-*s\xBA", W, "  SIMVARS (INI-Konfiguration)");

    int colorIdx = 0;
    for (const auto& sv : cfg.simvars) {
        if (row >= m_renderRows - 2) { m_clipped = true; break; }
        char line[120];
        if (sv.fieldId != 0)
            snprintf(line, sizeof(line), "  %-36.36s |%-16.16s  0x%02X  (%zu Byte)",
                     sv.name.c_str(), sv.unit.c_str(), sv.fieldId, unitBytes(sv.unit));
        else
            snprintf(line, sizeof(line), "  %-36.36s |%-16.16s  (kein Field-ID)",
                     sv.name.c_str(), sv.unit.c_str());

        WORD fc = C_FIELD[colorIdx++ % C_FIELD_N];
        setColor(C_WHITE);
        printAt(0, row, "\xBA");
        setColor(fc);
        printAt(1, row, "%-*.*s", W, W, line);
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    }
}

// ── Gesendeter CAN-Frame (bridge_exe → Modul) ────────────────────────────────
void ConsoleUi::renderSentFrame(const ModuleState& s, int& row) {
    const ModuleConfig& c = *s.cfg;
    const int W = m_W;

    {
        char hdr[120];
        if (s.lastFrameLen > 0) {
            long long ago = (nowMs() - s.lastFrameMs);
            snprintf(hdr, sizeof(hdr), "  > CAN-FRAME gesendet (0x%03X)  \xB3  %zu Byte  \xB3  vor %lld ms",
                     c.canTxId, s.lastFrameLen, ago);
        } else {
            snprintf(hdr, sizeof(hdr), "  > CAN-FRAME gesendet (0x%03X)  \xB3  (noch kein Frame empfangen)",
                     c.canTxId);
        }
        setColor(C_WHITE);
        printAt(0, row++, "\xBA%-*.*s\xBA", W, W, hdr);
    }

    if (s.lastFrameLen == 0) return;

    // Hex-Dump mit SimVar-Farbmarkierung
    {
        WORD byteColor[64];
        std::fill(byteColor, byteColor + 64, C_GRAY);
        if (s.lastFrameLen > 0) byteColor[0] = C_WHITE;
        size_t offset = 1;
        int    ci     = 0;
        for (const auto& sv : c.simvars) {
            size_t nb = unitBytes(sv.unit);
            WORD   fc = C_FIELD[ci++ % C_FIELD_N];
            for (size_t b = 0; b < nb && offset + b < 64; ++b)
                byteColor[offset + b] = fc;
            offset += nb;
        }

        DWORD wr;
        int usedLen = (int)s.lastFrameLen;
        for (int lineStart = 0; lineStart < usedLen; lineStart += 16) {
            if (row >= m_renderRows - 1) { m_clipped = true; break; }
            setColor(C_WHITE);
            printAt(0, row, "\xBA");
            COORD pos = {1, (SHORT)row};
            SetConsoleCursorPosition(m_hOut, pos);
            WriteConsoleA(m_hOut, "  ", 2, &wr, nullptr);
            int col = 3;
            for (int i = 0; i < 16; i++) {
                int idx = lineStart + i;
                setColor(idx < usedLen ? byteColor[idx] : C_GRAY);
                char hexByte[4];
                snprintf(hexByte, 4, "%02X ", (idx < 64) ? s.lastFrame[idx] : 0);
                COORD bpos = {(SHORT)col, (SHORT)row};
                SetConsoleCursorPosition(m_hOut, bpos);
                WriteConsoleA(m_hOut, hexByte, 3, &wr, nullptr);
                col += 3;
            }
            setColor(C_GRAY);
            for (; col <= W; col++) {
                COORD ppos = {(SHORT)col, (SHORT)row};
                SetConsoleCursorPosition(m_hOut, ppos);
                WriteConsoleA(m_hOut, " ", 1, &wr, nullptr);
            }
            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
        }
    }

    // Nutzdaten-Decode
    if (row < m_renderRows - 1) {
        setColor(C_WHITE);
        printAt(0, row++, "\xBA%-*s\xBA", W, "  Nutzdaten:");
    } else { m_clipped = true; return; }
    constexpr int FILL = 26;

    if (row < m_renderRows - 1) {
        const uint8_t b0  = s.lastFrame[0];
        const char*   lbl = (b0 == 0x00) ? "Daten" : (b0 == 0xFC) ? "Init" : "???";
        setColor(C_WHITE);
        printAt(0,  row, "\xBA");
        printAt(1,  row, "  %-36.36s:", "Byte 0  Pakettyp");
        printAt(40, row, " %8s  %-*.*s", lbl, FILL, FILL, hexStr(&b0, 1).c_str());
        printAt(W + 1, row++, "\xBA");
    } else { m_clipped = true; return; }

    size_t offset = 1;
    int    ci     = 0;
    for (const auto& sv : c.simvars) {
        if (row >= m_renderRows - 1) { m_clipped = true; break; }
        size_t nb  = unitBytes(sv.unit);
        WORD   fc  = C_FIELD[ci++ % C_FIELD_N];
        std::string valStr = decodeBytes(s.lastFrame, s.lastFrameLen, offset, sv.unit, nb);
        std::string hx     = hexStr(s.lastFrame + offset,
                                    std::min(nb, s.lastFrameLen > offset
                                                 ? s.lastFrameLen - offset : (size_t)0));
        char hxFill[32];
        snprintf(hxFill, sizeof(hxFill), "%-*.*s", FILL, FILL, hx.c_str());

        setColor(C_WHITE);
        printAt(0,  row, "\xBA");
        printAt(1,  row, "  %-36.36s:", sv.name.c_str());
        setColor(fc);
        printAt(40, row, " %8.8s  %s", valStr.c_str(), hxFill);
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
        offset += nb;
    }
}

// ── Einheitliche Detail-Ansicht ───────────────────────────────────────────────
void ConsoleUi::renderDetail(const ModuleState& s, int& row) {
    const ModuleConfig& c = *s.cfg;
    const int W = m_W;

    // Header-Zeile 1: Name + Typ
    {
        const char* typeStr = (c.hasOutput() && c.hasInput()) ? "OUT+IN"
                            : c.hasOutput() ? "OUT" : "IN";
        char hdr[120];
        snprintf(hdr, sizeof(hdr), "  DETAIL: %s (%s)  [%s]  \xB3  %s",
                 c.deviceName.empty() ? c.iniKey.c_str() : c.deviceName.c_str(),
                 c.iniKey.c_str(), typeStr, c.type.c_str());
        setColor(C_WHITE);
        printAt(0, row++, "\xBA%-*.*s\xBA", W, W, hdr);
    }
    // Header-Zeile 2: CAN-IDs + Timeouts
    {
        char hdr[120];
        if (c.canRxId != 0)
            snprintf(hdr, sizeof(hdr), "  CAN: 0x%03X  \xB3  Return: 0x%03X  \xB3  HB-Timeout: %d ms  \xB3  %s",
                     c.canTxId, c.canRxId, c.heartbeatTimeoutMs, c.description.c_str());
        else
            snprintf(hdr, sizeof(hdr), "  CAN: 0x%03X  \xB3  HB-Timeout: %d ms  \xB3  %s",
                     c.canTxId, c.heartbeatTimeoutMs, c.description.c_str());
        setColor(C_WHITE);
        printAt(0, row++, "\xBA%-*.*s\xBA", W, W, hdr);
    }

    // ── Ausgangs-Sektion (simvars + config + gesendeter frame) ────────────────
    if (c.hasOutput()) {
        hline(row);
        renderSimvars(c, row);

        if (!c.config.empty()) {
            hline(row);
            setColor(C_WHITE);
            printAt(0, row++, "\xBA%-*s\xBA", W, "  CONFIG-PARAMETER (INI)");
            for (const auto& kv : c.config) {
                if (row >= m_renderRows - 2) { m_clipped = true; break; }
                char line[120];
                snprintf(line, sizeof(line), "    %-30s = %s",
                         kv.first.c_str(), kv.second.c_str());
                setColor(C_GRAY);
                printAt(0, row, "\xBA");
                printAt(1, row, "%-*.*s", W, W, line);
                setColor(C_WHITE);
                printAt(W + 1, row++, "\xBA");
            }
        }

        hline(row);
        renderSentFrame(s, row);
    }

    // ── Eingangs-Sektion (achsen + buttons + empfangener frame) ──────────────
    if (c.hasInput()) {
        hline(row);

        if (!c.axes.empty()) {
            setColor(C_WHITE);
            printAt(0, row++, "\xBA%-*s\xBA", W, "  Achsen");
            for (const auto& ax : c.axes) {
                if (row >= m_renderRows - 2) { m_clipped = true; break; }
                const int16_t val = s.axisValues[ax.hidAxisIdx];
                char left[80];
                snprintf(left, sizeof(left), "  %-9.9s  HID %u", ax.name.c_str(), ax.hidAxisIdx);
                setColor(C_WHITE);
                printAt(0,  row, "\xBA");
                printAt(1,  row, "%-60.60s", left);
                setColor(val != 0 ? C_CYAN : C_GRAY);
                printAt(61, row, " [%+6d]  ", (int)val);
                setColor(C_WHITE);
                printAt(W + 1, row++, "\xBA");
            }
        }

        if (!c.buttons.empty()) {
            setColor(C_WHITE);
            printAt(0, row++, "\xBA%-*s\xBA", W, "  Buttons");
            for (const auto& btn : c.buttons) {
                if (row >= m_renderRows - 2) { m_clipped = true; break; }
                const bool pressed = (btn.payloadByte > 0 && s.lastFrameLen >= btn.payloadByte)
                                     && ((s.lastFrame[btn.payloadByte - 1u] >> btn.payloadBit) & 0x01);
                const WORD  stColor = pressed ? C_GREEN : C_GRAY;
                const char* stTxt   = pressed ? "ON " : "off";
                char left[80];
                if (btn.hidB > 0)
                    snprintf(left, sizeof(left), "  %-9.9s  B%u.%u  phys:%3u  HID:%3u+%3u",
                             btn.name.c_str(), btn.payloadByte, btn.payloadBit,
                             btn.physicalIdx, btn.hidA, btn.hidB);
                else
                    snprintf(left, sizeof(left), "  %-9.9s  B%u.%u  phys:%3u  HID:%3u",
                             btn.name.c_str(), btn.payloadByte, btn.payloadBit,
                             btn.physicalIdx, btn.hidA);
                setColor(C_WHITE);
                printAt(0,  row, "\xBA");
                printAt(1,  row, "%-60.60s", left);
                setColor(stColor);
                printAt(61, row, "  [%s]   ", stTxt);
                setColor(C_WHITE);
                printAt(69, row, "%-*s", W - 68, "");
                printAt(W + 1, row++, "\xBA");
            }
        }

        // Letzter empfangener Frame
        {
            char hdr[120];
            if (s.lastFrameLen > 0)
                snprintf(hdr, sizeof(hdr), "  < CAN RX (0x%03X)  \xB3  %zu Byte  \xB3  vor %lld ms",
                         c.canTxId, s.lastFrameLen, nowMs() - s.lastFrameMs);
            else
                snprintf(hdr, sizeof(hdr), "  < CAN RX (0x%03X)  \xB3  noch kein Frame empfangen",
                         c.canTxId);
            setColor(C_WHITE);
            printAt(0, row, "\xBA");
            setColor(C_CYAN);
            printAt(1, row, "%-*.*s", W, W, hdr);
            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
        }

        if (s.lastFrameLen > 0 && row < m_renderRows - 1) {
            DWORD wr;
            const int usedLen = (int)s.lastFrameLen;
            setColor(C_WHITE);
            printAt(0, row, "\xBA");
            COORD pos = {1, (SHORT)row};
            SetConsoleCursorPosition(m_hOut, pos);
            WriteConsoleA(m_hOut, "  ", 2, &wr, nullptr);
            int col = 3;
            for (int i = 0; i < 16; i++) {
                setColor(i < usedLen ? C_CYAN : C_GRAY);
                char hb[4];
                snprintf(hb, 4, "%02X ", i < usedLen ? s.lastFrame[i] : 0);
                COORD bp = {(SHORT)col, (SHORT)row};
                SetConsoleCursorPosition(m_hOut, bp);
                WriteConsoleA(m_hOut, hb, 3, &wr, nullptr);
                col += 3;
            }
            setColor(C_GRAY);
            for (; col <= W; col++) {
                COORD pp = {(SHORT)col, (SHORT)row};
                SetConsoleCursorPosition(m_hOut, pp);
                WriteConsoleA(m_hOut, " ", 1, &wr, nullptr);
            }
            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
        } else if (s.lastFrameLen > 0) { m_clipped = true; }
    }

    // ── Feedback-Sektion (Heartbeat + Poll-Response) ──────────────────────────
    if (row >= m_renderRows - 2) { m_clipped = true; return; }
    hline(row);
    {
        char hdr[120];
        const uint32_t fbCan = (c.canRxId != 0) ? c.canRxId : c.canTxId;
        snprintf(hdr, sizeof(hdr), "  < FEEDBACK (0x%03X)", fbCan);
        setColor(C_WHITE);
        printAt(0, row, "\xBA");
        setColor(C_CYAN);
        printAt(1, row, "%-*.*s", W, W, hdr);
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    }

    if (row < m_renderRows - 1) {
        char line[120];
        if (s.lastHeartbeatMs > 0) {
            long long agoS = (nowMs() - s.lastHeartbeatMs + 500) / 1000;
            snprintf(line, sizeof(line), "  01 HEARTBEAT   vor %4lld s  \xB3  Uptime: %u s",
                     agoS, s.heartbeatUptime / 1000u);
        } else {
            snprintf(line, sizeof(line), "  01 HEARTBEAT   noch kein Heartbeat empfangen");
        }
        setColor(C_WHITE);
        printAt(0, row, "\xBA");
        setColor(s.lastHeartbeatMs > 0 ? C_CYAN : C_GRAY);
        printAt(1, row, "%-*.*s", W, W, line);
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    } else { m_clipped = true; }

    if (row < m_renderRows - 1) {
        char line[120];
        if (s.lastPollMs > 0) {
            long long agoS = (nowMs() - s.lastPollMs + 500) / 1000;
            snprintf(line, sizeof(line), "  02 PULL RESPONSE   vor %4lld s", agoS);
        } else {
            snprintf(line, sizeof(line), "  02 PULL RESPONSE   noch keine Antwort");
        }
        setColor(C_WHITE);
        printAt(0, row, "\xBA");
        setColor(s.lastPollMs > 0 ? C_CYAN : C_GRAY);
        printAt(1, row, "%-*.*s", W, W, line);
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    } else { m_clipped = true; }

    // Pull-Werte für Ausgangsmodule (simvars)
    if (c.hasOutput() && s.lastPollFeedbackLen >= 7) {
        size_t fbOff = 6;
        int    ci    = 0;
        for (const auto& sv : c.simvars) {
            if (row >= m_renderRows - 1) { m_clipped = true; break; }
            if (fbOff >= s.lastPollFeedbackLen) break;
            size_t nb = unitBytes(sv.unit);
            if (fbOff + nb > s.lastPollFeedbackLen) break;
            std::string valStr = decodeBytes(s.lastPollFeedback, s.lastPollFeedbackLen,
                                             fbOff, sv.unit, nb);
            WORD fc = C_FIELD[ci++ % C_FIELD_N];
            char line[120];
            snprintf(line, sizeof(line), "  %-34.34s : %10.10s  %-16.16s",
                     sv.name.c_str(), valStr.c_str(), sv.unit.c_str());
            setColor(C_WHITE);
            printAt(0, row, "\xBA");
            setColor(fc);
            printAt(1, row, "%-*.*s", W, W, line);
            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
            fbOff += nb;
        }
    }

    // Pull-Werte für Eingangsmodule (achsen + buttons)
    // Format Pull-Response: [0x04][seq][ts_echo 4B] | N×int16 Achsen | Button-Bytes
    if (c.hasInput() && s.lastPollFeedbackLen >= 6
        && s.lastPollFeedback[0] == 0x04u) {
        size_t offset = 6;
        int    ci     = 0;

        for (const auto& ax : c.axes) {
            if (row >= m_renderRows - 1) { m_clipped = true; break; }
            if (offset + 2 > s.lastPollFeedbackLen) break;
            int16_t val;
            std::memcpy(&val, s.lastPollFeedback + offset, 2);
            offset += 2;
            WORD fc = C_FIELD[ci++ % C_FIELD_N];
            char line[120];
            snprintf(line, sizeof(line), "  %-32.32s HID-Achse %u  :  %+6d",
                     ax.name.c_str(), (unsigned)ax.hidAxisIdx, (int)val);
            setColor(C_WHITE);
            printAt(0, row, "\xBA");
            setColor(fc);
            printAt(1, row, "%-*.*s", W, W, line);
            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
        }

        if (!c.buttons.empty()) {
            // payload_byte ist 1-indexiert: B{N} → spontan-Index N-1 → Response-Index N-1+5 = N+4.
            for (const auto& btn : c.buttons) {
                if (row >= m_renderRows - 1) { m_clipped = true; break; }
                const size_t bytePos = (btn.payloadByte > 0u) ? (size_t)btn.payloadByte + 4u : 0u;
                const bool pressed = (btn.payloadByte > 0 && bytePos < s.lastPollFeedbackLen)
                                     && ((s.lastPollFeedback[bytePos] >> btn.payloadBit) & 0x01);
                const char* stTxt = pressed ? "ON " : "off";
                const WORD  stCol = pressed ? C_GREEN : C_GRAY;
                char line[120];
                snprintf(line, sizeof(line), "  %-32.32s HID-Btn %u  :  [%s]",
                         btn.name.c_str(), (unsigned)btn.hidA, stTxt);
                setColor(C_WHITE);
                printAt(0, row, "\xBA");
                setColor(stCol);
                printAt(1, row, "%-*.*s", W, W, line);
                setColor(C_WHITE);
                printAt(W + 1, row++, "\xBA");
            }
        }
    }
}

// ── Tastatureingabe ───────────────────────────────────────────────────────────
UiEvent ConsoleUi::pollInput() {
    if (!_kbhit()) return UiEvent::NONE;
    int ch = _getch();
    if (ch == 0xE0 || ch == 0) {
        ch = _getch();
        if (ch == 72) return UiEvent::SELECT_UP;
        if (ch == 80) return UiEvent::SELECT_DOWN;
        return UiEvent::NONE;
    }
    ch = tolower(ch);
    if (ch == 'p') return UiEvent::PULL;
    if (ch == 'q') return UiEvent::QUIT;
    return UiEvent::NONE;
}

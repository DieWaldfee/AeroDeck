#include "debug_console.h"
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <cstring>
#include <string>
#include <cmath>
#include <algorithm>

static std::string modLabel(uint8_t kbMod) {
    if (!kbMod) return "";
    std::string s;
    if (kbMod & KB_LCTRL)  { if (!s.empty()) s += "+"; s += "CTL"; }
    if (kbMod & KB_RCTRL)  { if (!s.empty()) s += "+"; s += "RCT"; }
    if (kbMod & KB_LSHIFT) { if (!s.empty()) s += "+"; s += "LSH"; }
    if (kbMod & KB_RSHIFT) { if (!s.empty()) s += "+"; s += "RSH"; }
    if (kbMod & KB_LALT)   { if (!s.empty()) s += "+"; s += "ALT"; }
    return s;
}

static long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static constexpr WORD C_WHITE  = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
static constexpr WORD C_GREEN  = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static constexpr WORD C_RED    = FOREGROUND_RED   | FOREGROUND_INTENSITY;
static constexpr WORD C_YELLOW = FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
static constexpr WORD C_CYAN   = FOREGROUND_GREEN | FOREGROUND_BLUE  | FOREGROUND_INTENSITY;
static constexpr WORD C_GRAY   = FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_BLUE;

// Farbpalette für SimVar-Felder
static constexpr WORD C_FIELD[] = {
    FOREGROUND_GREEN | FOREGROUND_BLUE  | FOREGROUND_INTENSITY,  //  0 Hellcyan
    FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  //  1 Hellgelb
    FOREGROUND_RED   | FOREGROUND_BLUE  | FOREGROUND_INTENSITY,  //  2 Hellmagenta
    FOREGROUND_GREEN                    | FOREGROUND_INTENSITY,  //  3 Hellgrün
    FOREGROUND_RED                      | FOREGROUND_INTENSITY,  //  4 Hellrot
    FOREGROUND_BLUE                     | FOREGROUND_INTENSITY,  //  5 Hellblau
    FOREGROUND_GREEN | FOREGROUND_BLUE,                          //  6 Dunkeltürkis
    FOREGROUND_RED   | FOREGROUND_GREEN,                         //  7 Dunkelgelb
    FOREGROUND_RED   | FOREGROUND_BLUE,                          //  8 Dunkelmagenta
    FOREGROUND_GREEN,                                            //  9 Dunkelgrün
    FOREGROUND_RED,                                              // 10 Dunkelrot
    FOREGROUND_BLUE  | FOREGROUND_INTENSITY,                     // 11 Hellblau
    FOREGROUND_BLUE,                                             // 12 Dunkelblau
};
static constexpr int C_FIELD_N = 13;

// Formatiert einen Datenwert passend zur Einheit. Ergebnis max. 8 Zeichen.
static std::string fmtVal(double v, const std::string& unit) {
    char buf[16];
    if (unit == "bool") {
        return (v > 0.5) ? "ON " : "OFF";
    } else if (unit == "radians") {
        snprintf(buf, sizeof(buf), "%+7.2f\xF8", v * 57.2957795);
    } else if (unit == "degrees") {
        snprintf(buf, sizeof(buf), "%7.2f\xF8", v);
    } else if (unit == "feet per minute") {
        snprintf(buf, sizeof(buf), "%+8.0f", v);
    } else if (unit == "feet" || unit == "gallons") {
        snprintf(buf, sizeof(buf), "%+8.0f", v);
    } else if (unit == "number" || unit == "status") {
        snprintf(buf, sizeof(buf), "%+8.0f", v);
    } else {
        snprintf(buf, sizeof(buf), "%+8.3g", v);
    }
    buf[8] = '\0';   // hart auf 8 Zeichen begrenzen
    return buf;
}

// Kompakter Hex-String ohne Leerzeichen: "(AABBCCDD)"
static std::string compactHex(const uint8_t* data, size_t len) {
    char buf[32];
    int  pos = 0;
    buf[pos++] = '(';
    for (size_t i = 0; i < len && i < 4; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", data[i]);
    buf[pos++] = ')';
    buf[pos]   = '\0';
    return buf;
}

// ── DebugConsole ──────────────────────────────────────────────────────────────

void DebugConsole::setupConsole(int rows) {
    m_consoleRows = rows;
    m_hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    COORD  origin = { 0, 0 };
    DWORD  written;

    // VT-Clear: sichtbaren Puffer + Scrollback leeren
    {
        DWORD mode = 0;
        GetConsoleMode(m_hOut, &mode);
        SetConsoleMode(m_hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        const char* clr = "\033[2J\033[3J\033[H";
        WriteConsoleA(m_hOut, clr, (DWORD)strlen(clr), &written, nullptr);
        SetConsoleMode(m_hOut, mode);
    }

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

    // Breite 80, Höhe = rows
    COORD      size    = { 80, (SHORT)m_consoleRows };
    SMALL_RECT winRect = { 0, 0, 79, (SHORT)(m_consoleRows - 1) };
    SetConsoleWindowInfo(m_hOut, TRUE, &winRect);
    SetConsoleScreenBufferSize(m_hOut, size);
    SetConsoleWindowInfo(m_hOut, TRUE, &winRect);

    SetConsoleCursorPosition(m_hOut, origin);
}

void DebugConsole::init(int refreshMs, int simvarCount, int deviceCount,
                        int axisCount, int buttonCount) {
    m_refreshMs = refreshMs;
    const bool hasOutput = simvarCount > 0;
    const bool hasInput  = axisCount > 0 || buttonCount > 0;

    int rows = 2;                                        // Top + Bottom Rahmen
    rows += 1;                                           // Kopfzeile
    rows += 1;                                           // Sep
    rows += 1;                                           // SIMCONNECT-Header
    rows += (simvarCount > 0 ? simvarCount : 1);         // SimVars oder "keine SimVars"
    rows += 1;                                           // Sep
    rows += 1;                                           // CAN-TX Header
    rows += 4;                                           // TX Hex-Dump (4×16 Byte)
    rows += 1;                                           // CAN-RX Header
    rows += 4;                                           // RX Hex-Dump
    if (hasOutput) rows += simvarCount;                  // Nutzdaten: SimVars TX↔RX
    if (hasInput)  rows += axisCount + buttonCount;      // Nutzdaten: Achsen+Buttons im CAN-Block
    rows += 1;                                           // Sep vor Geräte-Status
    rows += 1;                                           // GERÄTE-STATUS Header
    rows += (deviceCount > 0) ? (deviceCount + 1) / 2 : 1;   // Geräte (2 pro Zeile)
    rows += 1;                                           // Sep
    rows += 1;                                           // Status-Zeile

    setupConsole(rows);
}

void DebugConsole::setColor(WORD attr) {
    SetConsoleTextAttribute(m_hOut, attr);
}

void DebugConsole::printAt(int x, int y, const char* fmt, ...) {
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

// Rendert eine Hex-Zeile: 16 Byte ab lineStart, Rahmen links/rechts.
// colors: Farbe pro Byte (nullptr = cyan für genutzte, grau für ungenuzte Bytes)
void DebugConsole::renderHexLine(int row, int lineStart, const uint8_t* data,
                                  int usedLen, const WORD* colors) {
    const int W = 76;
    DWORD wr;
    setColor(C_WHITE);
    printAt(0, row, "\xBA");
    COORD p2 = { 1, (SHORT)row };
    SetConsoleCursorPosition(m_hOut, p2);
    WriteConsoleA(m_hOut, "  ", 2, &wr, nullptr);
    int col = 3;
    for (int i = 0; i < 16 && col < W; i++) {
        int idx = lineStart + i;
        WORD clr = (idx < usedLen)
                   ? (colors ? colors[idx] : C_CYAN)
                   : C_GRAY;
        setColor(clr);
        char hb[4];
        snprintf(hb, sizeof(hb), "%02X ", data[idx]);
        COORD bp = { (SHORT)col, (SHORT)row };
        SetConsoleCursorPosition(m_hOut, bp);
        WriteConsoleA(m_hOut, hb, 3, &wr, nullptr);
        col += 3;
    }
    // Leerauffüllung bis rechter Rand
    setColor(C_GRAY);
    for (; col < W + 1; col++) {
        COORD pp = { (SHORT)col, (SHORT)row };
        SetConsoleCursorPosition(m_hOut, pp);
        WriteConsoleA(m_hOut, " ", 1, &wr, nullptr);
    }
    setColor(C_WHITE);
    printAt(W + 1, row, "\xBA");
}

void DebugConsole::update(const DebugState& state) {
    if (m_hOut == INVALID_HANDLE_VALUE) return;
    long long now = nowMs();
    if (m_refreshMs > 0 && (now - m_lastRenderMs) < m_refreshMs) return;
    m_lastRenderMs = now;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(m_hOut, &csbi)) {
        SHORT visRows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        SHORT visCols = csbi.srWindow.Right  - csbi.srWindow.Left  + 1;
        m_renderRows = std::min(m_consoleRows, (int)visRows);
        m_renderCols = std::min(78,            (int)visCols);
        m_winCols    = (int)csbi.dwSize.X;
        if (visRows != m_lastWindowRows || visCols != m_lastWindowCols) {
            COORD  origin = { 0, 0 };
            DWORD  cells  = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
            DWORD  written;
            FillConsoleOutputCharacterA(m_hOut, ' ',     cells, origin, &written);
            FillConsoleOutputAttribute (m_hOut, C_WHITE, cells, origin, &written);
            m_lastWindowRows = visRows;
            m_lastWindowCols = visCols;
        }
    }
    render(state);
    if (m_renderRows < m_consoleRows) {
        setColor(C_RED | FOREGROUND_INTENSITY);
        printAt(0, m_renderRows - 1, "\xCC%-*.*s\xB9", 76, 76,
                " Fenster zu klein \xB3 Inhalt abgeschnitten ");
        setColor(C_GRAY);
    }
}

void DebugConsole::render(const DebugState& state) {
    const int  W           = 76;
    const int  CONSOLE_ROWS = (m_renderRows < m_consoleRows) ? m_renderRows - 1 : m_renderRows;
    const long long now    = nowMs();

    // Gutter-Clear: rechts von Spalte 78+
    if (m_winCols > W + 2) {
        DWORD wr;
        const DWORD gutterLen = (DWORD)(m_winCols - (W + 2));
        for (int r = 0; r < CONSOLE_ROWS; r++) {
            COORD pos = { (SHORT)(W + 2), (SHORT)r };
            FillConsoleOutputCharacterA(m_hOut, ' ', gutterLen, pos, &wr);
        }
    }

    const ModuleConfig& mod       = state.primaryMod;
    const bool          hasOutput = !mod.simvars.empty();
    const bool          hasInput  = !mod.axes.empty() || !mod.buttons.empty();

    int row = 0;

    // ── Oberer Rahmen ─────────────────────────────────────────────────────────
    setColor(C_WHITE);
    printAt(0, row++, "\xC9%s\xBB", std::string(W, '\xCD').c_str());
    if (row >= CONSOLE_ROWS) return;

    // ── Kopfzeile ─────────────────────────────────────────────────────────────
    {
        const bool isUsb = state.transportTarget.size() >= 4 &&
                           state.transportTarget.compare(0, 4, "USB:") == 0;
        const char* tLabel = isUsb ? "USB" : "UDP";
        const bool pollActive = state.pollSent && (now - state.pollSentMs) < 3000;

        printAt(0, row, "\xBA");

        // " MSFS SimConnect Bridge │ "
        setColor(C_WHITE);
        printAt(1, row, " MSFS SimConnect Bridge \xB3 ");

        // Status (farbig)
        const char* stTxt; WORD stCol;
        switch (state.status) {
            case BridgeStatus::CONNECTED:    stCol = C_GREEN;  stTxt = "verbunden   "; break;
            case BridgeStatus::DISCONNECTED: stCol = C_RED;    stTxt = "getrennt    "; break;
            default:                         stCol = C_YELLOW; stTxt = "wartet...   "; break;
        }
        setColor(stCol);
        printAt(27, row, "%s", stTxt);           // 12 Zeichen, col 27–38

        // Hz + Transport
        setColor(C_WHITE);
        printAt(39, row, " %5.1f Hz \xB3 %-3s", state.hz, tLabel);
        // "  8.4 Hz │ USB" = 1+5+3+3+3 = 15 Zeichen, col 39–53

        // col 54: Abstand
        setColor(C_WHITE);
        printAt(54, row, " ");

        // col 55-63: Poll-Indikator
        if (pollActive) {
            setColor(C_YELLOW);
            printAt(55, row, "\xB3 Poll...");
        } else {
            setColor(C_WHITE);
            printAt(55, row, "         ");
        }

        // col 64-76: Heartbeat-Indikator (\xDB=█, \xFE=■)
        {
            const bool recentHb = state.lastHeartbeatMs > 0 &&
                                  (now - state.lastHeartbeatMs) < 500;
            setColor(C_WHITE);
            printAt(64, row, "  \xB3 HB:");
            if (!state.online) {
                setColor(C_RED);
                printAt(71, row, "\xFE");
            } else if (recentHb) {
                setColor(C_GREEN);
                printAt(71, row, "\xDB");
            } else {
                setColor(FOREGROUND_GREEN);
                printAt(71, row, "\xFE");
            }
            setColor(C_WHITE);
            printAt(72, row, "     ");
        }
        printAt(W + 1, row++, "\xBA");
    }
    if (row >= CONSOLE_ROWS) return;

    // ── Trennlinie ────────────────────────────────────────────────────────────
    setColor(C_WHITE);
    printAt(0, row++, "\xCC%s\xB9", std::string(W, '\xCD').c_str());
    if (row >= CONSOLE_ROWS) return;

    // ── SIMCONNECT-Block ──────────────────────────────────────────────────────
    {
        const std::string& dname = mod.deviceName.empty() ? mod.iniKey : mod.deviceName;
        char hdr[80];
        snprintf(hdr, sizeof(hdr), "  SIMCONNECT: %s", dname.c_str());
        printAt(0, row++, "\xBA%-*.*s\xBA", W, W, hdr);
    }
    if (row >= CONSOLE_ROWS) return;

    if (hasOutput) {
        const SimData& simData = state.simData;
        for (const auto& sv : mod.simvars) {
            if (row >= CONSOLE_ROWS) return;
            std::string val = fmtVal(simData.get(sv.name), sv.unit);
            // "  %-32.32s :  %8.8s" = 2+32+4+8 = 46 Zeichen, Rest Leerzeichen
            setColor(C_WHITE);
            printAt(0, row, "\xBA");
            printAt(1, row, "  %-32.32s :  %8.8s",
                    sv.name.c_str(), val.c_str());
            printAt(W + 1, row++, "\xBA");
        }
    } else {
        setColor(C_GRAY);
        printAt(0, row, "\xBA");
        printAt(1, row, "  keine SimVars%-*s", W - 16, "");
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    }
    if (row >= CONSOLE_ROWS) return;

    // ── Trennlinie vor CAN-Block ──────────────────────────────────────────────
    setColor(C_WHITE);
    printAt(0, row++, "\xCC%s\xB9", std::string(W, '\xCD').c_str());
    if (row >= CONSOLE_ROWS) return;

    // ── Farb-Map für TX-Frame (SimVars) und RX-Frame (Achsen + Buttons) ─────────
    const size_t svN  = mod.simvars.size();
    const size_t axN  = mod.axes.size();
    const size_t btnN = mod.buttons.size();
    std::vector<int> svColorIdx(svN, -1);
    {
        int ci = 0;
        for (size_t si = 0; si < svN; si++)
            if (si < state.simvarFrameBytes.size() && state.simvarFrameBytes[si] > 0)
                svColorIdx[si] = ci++ % C_FIELD_N;
    }
    const int usedTxLen = (state.primaryFrameSize > 0 && state.primaryFrameSize <= 64)
                          ? (int)state.primaryFrameSize : 0;
    WORD txByteColor[64];
    std::fill(txByteColor, txByteColor + 64, C_GRAY);
    if (usedTxLen > 0) txByteColor[0] = C_WHITE;
    for (size_t si = 0; si < svN; si++) {
        if (svColorIdx[si] < 0) continue;
        size_t off = state.simvarFrameOffset[si];
        size_t len = state.simvarFrameBytes[si];
        WORD   fc  = C_FIELD[svColorIdx[si]];
        for (size_t b = 0; b < len && off + b < 64; b++)
            txByteColor[off + b] = fc;
    }

    // ── Farb-Map für RX-Frame (Achsen + Buttons) ─────────────────────────────
    const uint8_t rxFrameType  = (hasInput && state.rxLen > 0) ? state.rxPayload[0] : 0xFF;

    std::vector<int> axColorIdx (axN,  -1);
    std::vector<int> btnColorIdx(btnN, -1);
    WORD rxByteColor[64];
    std::fill(rxByteColor, rxByteColor + 64, C_GRAY);

    if (hasInput) {
        int ci = 0;
        for (size_t ai = 0; ai < axN; ai++) axColorIdx[ai] = ci++ % C_FIELD_N;
        for (size_t bi = 0; bi < btnN; bi++) {
            // Gleiche payloadByte → gleiche Farbe (Bit-Anzeige stimmt mit Hex-Dump überein)
            uint8_t pb = mod.buttons[bi].payloadByte;
            bool found = false;
            for (size_t bj = 0; bj < bi; bj++) {
                if (mod.buttons[bj].payloadByte == pb) {
                    btnColorIdx[bi] = btnColorIdx[bj];
                    found = true;
                    break;
                }
            }
            if (!found)
                btnColorIdx[bi] = ci++ % C_FIELD_N;
        }

        if (rxFrameType == 0x01) {                            // Heartbeat
            rxByteColor[0] = C_WHITE;
            for (size_t i = 1; i < state.rxLen && i < 5; i++)
                rxByteColor[i] = C_YELLOW;
        } else if (rxFrameType == 0x02) {
            // Spontandaten + konvertierte STATUS_RESPONSE (immer 0x02 im Puffer)
            rxByteColor[0] = C_WHITE;
            for (size_t ai = 0; ai < axN; ai++) {
                WORD fc = (axColorIdx[ai] >= 0) ? C_FIELD[axColorIdx[ai]] : C_GRAY;
                const size_t bOff = 1u + ai * 2;
                if (bOff + 1 < 64) {
                    rxByteColor[bOff]     = fc;
                    rxByteColor[bOff + 1] = fc;
                }
            }
            for (size_t bi = 0; bi < btnN; bi++) {
                uint8_t pb = mod.buttons[bi].payloadByte;
                if (pb > 0 && btnColorIdx[bi] >= 0) {
                    const size_t idx = (size_t)pb - 1u;
                    if (idx < 64) rxByteColor[idx] = C_FIELD[btnColorIdx[bi]];
                }
            }
        }
    }

    // ── CAN-TX Header ─────────────────────────────────────────────────────────
    {
        char hdr[80];
        snprintf(hdr, sizeof(hdr), "  CAN-TX  ID:0x%03X  \xB3  TX: %llu Frames",
                 mod.canTxId, (unsigned long long)state.framesSentMod);
        printAt(0, row++, "\xBA%-*.*s\xBA", W, W, hdr);
    }
    if (row >= CONSOLE_ROWS) return;

    // ── TX Hex-Dump (4 × 16 Byte) ─────────────────────────────────────────────
    for (int ls = 0; ls < 64 && row < CONSOLE_ROWS; ls += 16)
        renderHexLine(row++, ls, state.primaryFrame, usedTxLen, txByteColor);
    if (row >= CONSOLE_ROWS) return;

    // ── CAN-RX Header ─────────────────────────────────────────────────────────
    {
        // Input-Module empfangen Frames auf canTxId (Gerät sendet auf seiner eigenen ID)
        const uint32_t rxDisplayId = hasInput ? mod.canTxId : mod.canRxId;
        char hdr[80];
        if (state.lastRxMs > 0) {
            long long secs = (now - state.lastRxMs) / 1000LL;
            snprintf(hdr, sizeof(hdr),
                     "  CAN-RX  ID:0x%03X  \xB3  RX: %llu Frames  \xB3  vor %lld s",
                     rxDisplayId,
                     (unsigned long long)state.framesReceived,
                     secs);
        } else {
            snprintf(hdr, sizeof(hdr),
                     "  CAN-RX  ID:0x%03X  \xB3  RX: %llu Frames  \xB3  (kein Frame)",
                     rxDisplayId,
                     (unsigned long long)state.framesReceived);
        }
        printAt(0, row++, "\xBA%-*.*s\xBA", W, W, hdr);
    }
    if (row >= CONSOLE_ROWS) return;

    // ── RX Hex-Dump (4 × 16 Byte) ────────────────────────────────────────────
    const int usedRxLen = (int)std::min(state.rxLen, (size_t)64);
    {
        const WORD* rxColors = hasInput ? rxByteColor : nullptr;
        for (int ls = 0; ls < 64 && row < CONSOLE_ROWS; ls += 16)
            renderHexLine(row++, ls, state.rxPayload, usedRxLen, rxColors);
    }
    if (row >= CONSOLE_ROWS) return;

    // ── Nutzdaten: TX-Wert ↔ RX-Wert (nur wenn hasOutput) ───────────────────
    if (hasOutput) {
        const SimData& simData = state.simData;
        for (size_t si = 0; si < svN; si++) {
            if (row >= CONSOLE_ROWS) return;
            const auto& sv = mod.simvars[si];

            // TX-Seite
            std::string txVal = fmtVal(simData.get(sv.name), sv.unit);
            std::string txHex;
            size_t fieldBytes = (si < state.simvarFrameBytes.size()) ? state.simvarFrameBytes[si] : 0;
            size_t txOff      = (si < state.simvarFrameOffset.size()) ? state.simvarFrameOffset[si] : 0;
            if (fieldBytes > 0 && txOff + fieldBytes <= 64)
                txHex = compactHex(state.primaryFrame + txOff, fieldBytes);

            // RX-Seite (Feedback-Werte aus letzter STATUS_RESPONSE)
            std::string rxVal, rxHex;
            if (si < state.feedbackValues.size() && si < state.feedbackUnits.size()) {
                rxVal = fmtVal(state.feedbackValues[si], state.feedbackUnits[si]);
                // feedbackPayload: roher 0x04-Frame, Feld bei 5 + txOff
                size_t rxOff = 5 + txOff;
                if (fieldBytes > 0 && rxOff + fieldBytes <= state.feedbackPayloadLen)
                    rxHex = compactHex(state.feedbackPayload + rxOff, fieldBytes);
            }

            // Zeilenformat: "  %-32s : %8s %-10s %8s %-10s"
            // = 2+32+3+8+1+10+1+8+1+10 = 76 Zeichen
            WORD fc = (svColorIdx[si] >= 0) ? C_FIELD[svColorIdx[si]] : C_GRAY;

            setColor(C_WHITE);
            printAt(0,  row, "\xBA");
            printAt(1,  row, "  %-30.30s :", sv.name.c_str());   // col 1-34
            setColor(fc);
            printAt(35, row, "%8.8s %-10.10s",
                    txVal.c_str(), txHex.c_str());                 // col 35-53
            if (!rxVal.empty()) {
                printAt(54, row, " [%8.8s %-10.10s] ",
                        rxVal.c_str(), rxHex.c_str());             // col 54-76
            } else {
                printAt(54, row, "%-23s", "");
            }
            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
        }
        if (row >= CONSOLE_ROWS) return;
    }

    // ── Nutzdaten: Achsen + Buttons im CAN-Block (nur wenn hasInput) ──────────
    if (hasInput) {
        // Achsen: Name + HID-Index + aktueller Wert + Rohbytes + [Poll-Feedback]
        for (size_t ai = 0; ai < axN; ai++) {
            if (row >= CONSOLE_ROWS) return;
            const auto& ax  = mod.axes[ai];
            const int16_t v = state.axisValues[ax.hidAxisIdx];
            const uint8_t byLo = (uint8_t)(v & 0xFF);
            const uint8_t byHi = (uint8_t)((v >> 8) & 0xFF);
            WORD fc = (axColorIdx[ai] >= 0) ? C_FIELD[axColorIdx[ai]] : C_GRAY;
            setColor(C_WHITE);
            printAt(0,  row, "\xBA");
            char modPad[22];
            const std::string axMl = modLabel(ax.kbMod);
            snprintf(modPad, sizeof(modPad), "%-19.19s",
                     axMl.empty() ? "" : ("+" + axMl).c_str());
            printAt(1,  row, "  %-9.9s  HID%-2u%s :",
                    ax.name.c_str(), ax.hidAxisIdx, modPad);
            setColor(fc);
            printAt(37, row, " %+8d  %02X %02X", (int)v, byLo, byHi);
            // Poll-Feedback (0x04): Achse ai bei Offset 6 + ai*2
            const size_t fbOff = 6 + ai * 2;
            if (fbOff + 1 < state.feedbackPayloadLen) {
                int16_t fbVal;
                std::memcpy(&fbVal, state.feedbackPayload + fbOff, 2);
                char fbBuf[16];
                snprintf(fbBuf, sizeof(fbBuf), "%+d", (int)fbVal);
                const std::string fbHex = compactHex(state.feedbackPayload + fbOff, 2);
                printAt(54, row, " [%8.8s %-10.10s] ", fbBuf, fbHex.c_str());
            } else {
                printAt(54, row, "%-23s", "");
            }
            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
        }

        // Buttons: Name + B{byte}.{bit} + phy + JS + Zustand + Bit-Anzeige + [Poll-Feedback]
        for (size_t bi = 0; bi < btnN; bi++) {
            if (row >= CONSOLE_ROWS) return;
            const auto& btn = mod.buttons[bi];

            // Spontan-Zustand aus rxPayload: B{N} → Index N-1
            bool pressed = false;
            size_t byteIdx = SIZE_MAX;
            if (btn.payloadByte > 0) {
                byteIdx = (size_t)btn.payloadByte - 1u;
                if (byteIdx < state.rxLen)
                    pressed = (state.rxPayload[byteIdx] >> btn.payloadBit) & 0x01u;
            }

            const uint8_t byteVal = (byteIdx < state.rxLen) ? state.rxPayload[byteIdx] : 0;

            char jsBase[12];
            if (btn.hidB > 0) snprintf(jsBase, sizeof(jsBase), "JS%u+%u", btn.hidA, btn.hidB);
            else               snprintf(jsBase, sizeof(jsBase), "JS%u",    btn.hidA);
            const std::string btnMl = modLabel(btn.kbMod);
            const std::string jsMod = btnMl.empty() ? jsBase : (std::string(jsBase) + "+" + btnMl);

            WORD fc = (btnColorIdx[bi] >= 0) ? C_FIELD[btnColorIdx[bi]] : C_GRAY;
            setColor(C_WHITE);
            printAt(0,  row, "\xBA");
            printAt(1,  row, "  %-9.9s B%u.%u phy:%-3u %-13.13s",
                    btn.name.c_str(), btn.payloadByte, btn.payloadBit,
                    btn.physicalIdx, jsMod.c_str());

            setColor(pressed ? C_GREEN : C_WHITE);
            printAt(40, row, " [%-3s]  ", pressed ? "ON " : "off");

            // Bit-Anzeige: 8 Bits MSB→LSB, Nibble-Trenner nach Bit 4
            // Bit an payloadBit-Position: rot; andere Bits: Farbe wenn 1, grau wenn 0
            int bitCol = 48;
            for (int b = 7; b >= 0; b--) {
                if (b == 3) {
                    setColor(C_WHITE);
                    printAt(bitCol++, row, " ");
                }
                const bool bitSet = (byteVal >> b) & 1;
                if (b == (int)btn.payloadBit)
                    setColor(C_RED);
                else
                    setColor(fc);
                printAt(bitCol++, row, "%d", bitSet ? 1 : 0);
            }

            // Poll-Feedback (0x04): Button bei payloadByte + 4
            setColor(fc);
            const size_t fbBytePos = (size_t)btn.payloadByte + 4u;
            if (btn.payloadByte > 0 && fbBytePos < state.feedbackPayloadLen) {
                const bool   fbPressed = (state.feedbackPayload[fbBytePos] >> btn.payloadBit) & 0x01u;
                const std::string fbHex = compactHex(state.feedbackPayload + fbBytePos, 1);
                printAt(57, row, " [%6.6s %-9.9s] ",
                        fbPressed ? "ON " : "off", fbHex.c_str());
            } else {
                printAt(57, row, "%-19s", "");
            }

            setColor(C_WHITE);
            printAt(W + 1, row++, "\xBA");
        }
        if (row >= CONSOLE_ROWS) return;
    }

    // ── Trennlinie vor Geräte-Status ──────────────────────────────────────────
    setColor(C_WHITE);
    printAt(0, row++, "\xCC%s\xB9", std::string(W, '\xCD').c_str());
    if (row >= CONSOLE_ROWS) return;

    // ── Geräte-Status ─────────────────────────────────────────────────────────
    printAt(0, row++, "\xBA%-*s\xBA", W, "  Ger\x84te-Status");
    if (row >= CONSOLE_ROWS) return;

    const auto& devs = state.devices;
    for (size_t i = 0; i < devs.size(); i += 2) {
        if (row >= CONSOLE_ROWS) return;
        const DeviceStatus& d1 = devs[i];

        // Slot-Format: "  0xXXX  %-16s %-7s" = 2+5+2+16+1+7 = 33 Zeichen
        auto makeDev = [](const DeviceStatus& d, bool indent) -> std::pair<std::string, WORD> {
            const char* st  = d.online ? "Online " : "Offline";
            WORD        col = d.online ? C_GREEN : (d.everSeen ? C_RED : C_YELLOW);
            char slot[48];
            if (d.isMonitor) {
                if (indent)
                    snprintf(slot, sizeof(slot), "  Monitor             %-7s", st);
                else
                    snprintf(slot, sizeof(slot), "Monitor             %-7s", st);
            } else {
                if (indent)
                    snprintf(slot, sizeof(slot), "  0x%03X  %-16.16s%-7s",
                             d.canTxId, d.name.c_str(), st);
                else
                    snprintf(slot, sizeof(slot), "0x%03X  %-16.16s%-7s",
                             d.canTxId, d.name.c_str(), st);
            }
            return {std::string(slot), col};
        };

        auto [s1, c1] = makeDev(d1, true);
        setColor(C_WHITE);
        printAt(0, row, "\xBA");
        setColor(c1);
        printAt(1, row, "%-37.37s", s1.c_str());

        if (i + 1 < devs.size()) {
            auto [s2, c2] = makeDev(devs[i + 1], false);
            setColor(C_WHITE);
            printAt(38, row, "\xB3 ");
            setColor(c2);
            printAt(40, row, "%-36.36s", s2.c_str());
        } else {
            setColor(C_GRAY);
            printAt(38, row, "%-38s", "");
        }
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    }
    if (devs.empty()) {
        printAt(0, row++, "\xBA%-*s\xBA", W, "  (keine Geraete)");
    }
    if (row >= CONSOLE_ROWS) return;

    // ── Trennlinie vor Status-Zeile ───────────────────────────────────────────
    setColor(C_WHITE);
    printAt(0, row++, "\xCC%s\xB9", std::string(W, '\xCD').c_str());
    if (row >= CONSOLE_ROWS) return;

    // ── Status-Zeile ──────────────────────────────────────────────────────────
    {
        setColor(C_WHITE);
        printAt(0, row, "\xBA");

        if (!state.transportError.empty()) {
            setColor(C_RED);
            printAt(1, row, "  Fehler: %-*.*s", W - 11, W - 11,
                    state.transportError.c_str());
        } else if (!state.statusMsg.empty()) {
            setColor(C_YELLOW);
            printAt(1, row, "  %-*.*s", W - 2, W - 2, state.statusMsg.c_str());
        } else {
            // "  N Fehler  │  [P] Poll  [R] Reset  [Q] Quit"
            // [R] Reset: weiß wenn USB aktiv, grau wenn UDP
            setColor(C_GRAY);
            char base[80];
            int baseLen = snprintf(base, sizeof(base), "  %d Fehler  \xB3  [P] Poll  ",
                                   state.errorCount);
            printAt(1, row, "%s", base);

            setColor(state.usbActive ? C_WHITE : C_GRAY);
            printAt(1 + baseLen, row, "[R] Reset  ");

            setColor(C_GRAY);
            int qCol = 1 + baseLen + 11;
            printAt(qCol, row, "[Q] Quit%-*s", W - qCol, "");
        }
        setColor(C_WHITE);
        printAt(W + 1, row++, "\xBA");
    }
    if (row >= CONSOLE_ROWS) return;

    // ── Unterer Rahmen ────────────────────────────────────────────────────────
    setColor(C_WHITE);
    printAt(0, row++, "\xC8%s\xBC", std::string(W, '\xCD').c_str());
    setColor(C_GRAY);
}

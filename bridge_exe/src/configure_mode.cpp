#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include "configure_mode.h"

// ══════════════════════════════════════════════════════════════════════════════
// ConfigSerial — schlanke Text-Kommunikation ueber COM-Port (kein Framing)
// ══════════════════════════════════════════════════════════════════════════════
class ConfigSerial {
public:
    bool open(const std::string& port, int baud) {
        std::string path = "\\\\.\\" + port;
        m_handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            switch (err) {
                case ERROR_FILE_NOT_FOUND:
                case ERROR_DEVICE_NOT_CONNECTED:
                    m_error = "Port nicht gefunden - USB nicht angeschlossen"; break;
                case ERROR_ACCESS_DENIED:
                    m_error = "Port belegt - laeuft bridge.exe im Normalbetrieb?"; break;
                default: {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "Fehler beim Oeffnen (Code %lu)", (unsigned long)err);
                    m_error = buf;
                }
            }
            return false;
        }
        DCB dcb = {};
        dcb.DCBlength = sizeof(dcb);
        GetCommState(m_handle, &dcb);
        dcb.BaudRate    = (DWORD)baud;
        dcb.ByteSize    = 8;
        dcb.Parity      = NOPARITY;
        dcb.StopBits    = ONESTOPBIT;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        SetCommState(m_handle, &dcb);
        // Non-blocking: ReadFile gibt sofort zurueck wenn kein Byte wartet
        COMMTIMEOUTS ct = {};
        ct.ReadIntervalTimeout        = MAXDWORD;
        ct.ReadTotalTimeoutMultiplier = 0;
        ct.ReadTotalTimeoutConstant   = 0;
        ct.WriteTotalTimeoutConstant  = 200;
        SetCommTimeouts(m_handle, &ct);
        return true;
    }

    void close() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    bool        isOpen()    const { return m_handle != INVALID_HANDLE_VALUE; }
    std::string lastError() const { return m_error; }

    bool sendLine(const std::string& text) {
        std::string s = text + "\r\n";
        DWORD written = 0;
        return WriteFile(m_handle, s.c_str(), (DWORD)s.size(), &written, nullptr)
               && written == (DWORD)s.size();
    }

    // Liest eine Zeile (stripped \r). Ignoriert nicht-druckbare Bytes ausser \n \r.
    // Schuetzt gegen Binaer-Frames von taskUsbTx.
    bool readLine(std::string& out, int timeoutMs = 500) {
        out.clear();
        DWORD deadline = GetTickCount() + (DWORD)timeoutMs;
        while ((int)(deadline - GetTickCount()) > 0) {
            uint8_t b; DWORD got = 0;
            ReadFile(m_handle, &b, 1, &got, nullptr);
            if (got == 1) {
                if (b == '\n') return true;
                if (b >= 0x20 && b < 0x80) out += (char)b;
            } else {
                Sleep(1);
            }
        }
        return !out.empty();
    }

    void flushRx() { PurgeComm(m_handle, PURGE_RXCLEAR); }

private:
    HANDLE      m_handle = INVALID_HANDLE_VALUE;
    std::string m_error;
};

// ══════════════════════════════════════════════════════════════════════════════
// EspConfig — RAM-Spiegel der ESP32-Konfiguration
// ══════════════════════════════════════════════════════════════════════════════
struct EspConfig {
    std::string ssid;
    uint32_t can1Min = 0x000, can1Max = 0x3FF;
    uint32_t can2Min = 0x400, can2Max = 0x7FF;
};

// ══════════════════════════════════════════════════════════════════════════════
// Navigierbare Felder
// ══════════════════════════════════════════════════════════════════════════════
enum Field {
    F_SSID = 0, F_PASS,
    F_CAN1MIN, F_CAN1MAX, F_CAN2MIN, F_CAN2MAX,
    F_ACTION_SHOW,
    F_ACTION_SHOW_MAP,
    F_ACTION_SAVE,
    F_ACTION_RESET,
    F_ACTION_SILENT,
    F_ACTION_VERBOSE,
    F_COUNT  // = 12
};

// ══════════════════════════════════════════════════════════════════════════════
// TUI-Layout (Zeilen)
//   0-2:  Header
//   4-6:  WiFi (SSID, PASS)
//   8-12: CAN (4 Werte)
//  14-20: Aktionen (6 Eintraege)
//  21:    Trennlinie
//  22:    Hilfe
//  23:    Status
// ══════════════════════════════════════════════════════════════════════════════
static int fieldRow(int f) {
    switch (f) {
        case F_SSID:             return  5;
        case F_PASS:             return  6;
        case F_CAN1MIN:          return  9;
        case F_CAN1MAX:          return 10;
        case F_CAN2MIN:          return 11;
        case F_CAN2MAX:          return 12;
        case F_ACTION_SHOW:      return 15;
        case F_ACTION_SHOW_MAP:  return 16;
        case F_ACTION_SAVE:      return 17;
        case F_ACTION_RESET:     return 18;
        case F_ACTION_SILENT:    return 19;
        case F_ACTION_VERBOSE:   return 20;
        default:                 return  0;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// ConfigureTui
// ══════════════════════════════════════════════════════════════════════════════
class ConfigureTui {
public:
    ConfigureTui(ConfigSerial& ser, std::string port, int baud)
        : m_ser(ser), m_port(std::move(port)), m_baud(baud) {}

    void run() {
        m_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_CURSOR_INFO ci = {1, FALSE};
        SetConsoleCursorInfo(m_hOut, &ci);
        clearScreen();

        m_status = "Lade Konfiguration...";
        render();
        if (fetchConfig()) m_status = "Konfiguration geladen.";
        else               m_status = "Warnung: Keine Antwort auf SHOW.";
        render();

        while (m_running) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch == 0 || ch == 0xE0) handleExtKey(_getch());
                else                         handleChar(ch);
                render();
            } else {
                Sleep(20);
            }
        }

        ci.bVisible = TRUE;
        SetConsoleCursorInfo(m_hOut, &ci);
        clearScreen();
    }

private:
    ConfigSerial& m_ser;
    HANDLE        m_hOut    = INVALID_HANDLE_VALUE;
    std::string   m_port;
    int           m_baud;
    EspConfig     m_cfg;
    int           m_cursor       = 0;
    bool          m_editing      = false;
    std::string   m_editBuf;
    std::string   m_status;
    bool          m_running      = true;
    bool          m_resetConfirm = false;

    // ── Farben ───────────────────────────────────────────────────────────────
    static constexpr WORD C_NORMAL = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE;
    static constexpr WORD C_BRIGHT = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE|FOREGROUND_INTENSITY;
    static constexpr WORD C_HEAD   = FOREGROUND_BLUE|FOREGROUND_INTENSITY;
    static constexpr WORD C_SECT   = FOREGROUND_GREEN|FOREGROUND_BLUE|FOREGROUND_INTENSITY;
    static constexpr WORD C_CURSOR = FOREGROUND_GREEN|FOREGROUND_INTENSITY;
    static constexpr WORD C_EDIT   = FOREGROUND_GREEN|FOREGROUND_BLUE|FOREGROUND_INTENSITY|BACKGROUND_BLUE;
    static constexpr WORD C_ACT    = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_INTENSITY;
    static constexpr WORD C_DIM    = FOREGROUND_RED|FOREGROUND_GREEN|FOREGROUND_BLUE;
    static constexpr WORD C_STATUS = FOREGROUND_GREEN|FOREGROUND_INTENSITY;
    static constexpr WORD C_ERROR  = FOREGROUND_RED|FOREGROUND_INTENSITY;

    void setColor(WORD a) { SetConsoleTextAttribute(m_hOut, a); }
    void moveTo(int x, int y) { SetConsoleCursorPosition(m_hOut, {(SHORT)x, (SHORT)y}); }

    void writeStr(WORD attr, const char* text) {
        setColor(attr);
        DWORD w;
        WriteConsoleA(m_hOut, text, (DWORD)strlen(text), &w, nullptr);
    }

    // Zeile bei (x,y) ausgeben; fmt muss Zeile komplett ausfuellen (trailing spaces!)
    void printAt(WORD attr, int x, int y, const char* fmt, ...) {
        char buf[128];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        moveTo(x, y);
        writeStr(attr, buf);
    }

    void clearScreen() {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(m_hOut, &csbi);
        COORD orig = {0, 0};
        DWORD cells = (DWORD)csbi.dwSize.X * csbi.dwSize.Y, w;
        FillConsoleOutputCharacterA(m_hOut, ' ', cells, orig, &w);
        FillConsoleOutputAttribute(m_hOut, csbi.wAttributes, cells, orig, &w);
        moveTo(0, 0);
    }

    // ── TUI rendern ──────────────────────────────────────────────────────────
    void render() {
        // Header (Zeilen 0-2)
        printAt(C_HEAD,   0, 0, "%-44s", "============================================");
        printAt(C_BRIGHT, 0, 1, "  ESP32 Konfiguration  [%-6s * %d Bd]%-5s",
                m_port.c_str(), m_baud, "");
        printAt(C_HEAD,   0, 2, "%-44s", "============================================");

        // WiFi (Zeilen 4-6)
        printAt(C_SECT, 0, 4, "  WiFi%-38s", "");
        renderField(F_SSID, "SSID    ", m_cfg.ssid.empty() ? "(leer)" : m_cfg.ssid);
        renderField(F_PASS, "PASS    ", "****");

        // CAN (Zeilen 8-12)
        printAt(C_SECT, 0, 8, "  CAN%-39s", "");
        char v[10];
        snprintf(v, sizeof(v), "0x%03X", m_cfg.can1Min);
        renderField(F_CAN1MIN, "CAN1_MIN", v);
        snprintf(v, sizeof(v), "0x%03X", m_cfg.can1Max);
        renderField(F_CAN1MAX, "CAN1_MAX", v);
        snprintf(v, sizeof(v), "0x%03X", m_cfg.can2Min);
        renderField(F_CAN2MIN, "CAN2_MIN", v);
        snprintf(v, sizeof(v), "0x%03X", m_cfg.can2Max);
        renderField(F_CAN2MAX, "CAN2_MAX", v);

        // Aktionen (Zeilen 14-20)
        printAt(C_SECT, 0, 14, "  Aktionen%-34s", "");
        renderAction(F_ACTION_SHOW,     "SHOW    ", "Konfiguration anzeigen + laden  ");
        renderAction(F_ACTION_SHOW_MAP, "SHOW MAP", "Button/Achsen-Tabelle anzeigen  ");
        renderAction(F_ACTION_SAVE,     "SAVE    ", "In NVS speichern + Neustart     ");
        renderAction(F_ACTION_RESET,    "RESET   ", "NVS loeschen + Neustart         ");
        renderAction(F_ACTION_SILENT,   "SILENT  ", "Log-Ausgabe unterdrücken        ");
        renderAction(F_ACTION_VERBOSE,  "VERBOSE ", "Log-Ausgabe wiederherstellen    ");

        // Trennlinie + Hilfe (Zeilen 21-22)
        printAt(C_HEAD, 0, 21, "%-44s", "--------------------------------------------");
        if (m_resetConfirm) {
            printAt(C_ERROR, 0, 22,
                    "  NVS wirklich loeschen? (J = Ja / N = Nein)   ");
        } else if (m_editing) {
            printAt(C_DIM,   0, 22,
                    "  Zeichen tippen  Enter = OK  Esc = Abbrechen   ");
        } else {
            printAt(C_DIM,   0, 22,
                    "  Pfeiltasten = Navigieren  Enter = Auswaehlen   ");
        }

        // Status (Zeile 23)
        bool isErr = m_status.find("Fehler")  != std::string::npos
                  || m_status.find("Warnung") != std::string::npos;
        printAt(isErr ? C_ERROR : C_STATUS, 0, 23,
                "  %-42s", m_status.c_str());

        moveTo(0, 24);
    }

    void renderField(int field, const char* label, const std::string& value) {
        int row    = fieldRow(field);
        bool active  = (m_cursor == field);
        bool editing = (m_editing && active);

        char prefix[28];
        snprintf(prefix, sizeof(prefix), "  %c %-8s : ", active ? '>' : ' ', label);
        moveTo(0, row);
        writeStr(active ? C_CURSOR : C_NORMAL, prefix);

        if (editing) {
            char ef[28];
            snprintf(ef, sizeof(ef), "[%-20s]", m_editBuf.c_str());
            writeStr(C_EDIT, ef);
        } else {
            char val[28];
            snprintf(val, sizeof(val), "%-22s", value.c_str());
            writeStr(active ? C_BRIGHT : C_NORMAL, val);
        }
    }

    void renderAction(int field, const char* label, const char* desc) {
        int row    = fieldRow(field);
        bool active = (m_cursor == field);
        char line[80];
        snprintf(line, sizeof(line), "  %c %-8s  %s", active ? '>' : ' ', label, desc);
        moveTo(0, row);
        writeStr(active ? C_ACT : C_DIM, line);
    }

    // ── Tastatur ─────────────────────────────────────────────────────────────
    void handleExtKey(int code) {
        if (m_resetConfirm || m_editing) return;
        if      (code == 72) m_cursor = (m_cursor + F_COUNT - 1) % F_COUNT;
        else if (code == 80) m_cursor = (m_cursor + 1) % F_COUNT;
    }

    void handleChar(int ch) {
        if (m_resetConfirm) {
            if (ch == 'J' || ch == 'j') executeReset();
            else                         m_status = "RESET abgebrochen.";
            m_resetConfirm = false;
            return;
        }

        if (m_editing) {
            if (ch == 0x1B) {
                m_editing = false; m_editBuf.clear();
                m_status = "Eingabe abgebrochen.";
            } else if (ch == '\r' || ch == '\n') {
                commitEdit();
            } else if ((ch == 0x08 || ch == 0x7F) && !m_editBuf.empty()) {
                m_editBuf.pop_back();
            } else if (ch >= 0x20 && ch < 0x7F && m_editBuf.size() < 40) {
                m_editBuf += (char)ch;
            }
            return;
        }

        if (ch == 0x1B) { m_running = false; return; }
        if (ch == '\t') { m_cursor = (m_cursor + 1) % F_COUNT; return; }
        if (ch != '\r' && ch != '\n') return;

        if (m_cursor < F_ACTION_SHOW) {
            // Wert-Feld bearbeiten
            m_editing = true;
            m_editBuf.clear();
            if (m_cursor != F_PASS) {
                if (m_cursor == F_SSID) {
                    m_editBuf = m_cfg.ssid;
                } else {
                    uint32_t val = (m_cursor == F_CAN1MIN) ? m_cfg.can1Min :
                                   (m_cursor == F_CAN1MAX) ? m_cfg.can1Max :
                                   (m_cursor == F_CAN2MIN) ? m_cfg.can2Min : m_cfg.can2Max;
                    char tmp[10]; snprintf(tmp, sizeof(tmp), "0x%03X", val);
                    m_editBuf = tmp;
                }
            }
        } else {
            // Aktions-Feld
            switch (m_cursor) {
                case F_ACTION_SHOW:
                    showRawOutput("SHOW", "SHOW  --  Bridge-Konfiguration");
                    m_status = "Lade Werte..."; render();
                    if (fetchConfig()) m_status = "Werte aktualisiert.";
                    else               m_status = "Fehler: Keine Antwort von ESP32.";
                    break;
                case F_ACTION_SHOW_MAP:
                    showRawOutput("SHOW MAP", "SHOW MAP  --  Buttons & Achsen");
                    m_status = "Zurueck im Konfigurations-Modus.";
                    break;
                case F_ACTION_SAVE:
                    executeSave();
                    break;
                case F_ACTION_RESET:
                    m_resetConfirm = true;
                    break;
                case F_ACTION_SILENT:
                    sendSimple("SILENT", "Log-Ausgabe unterdrückt.");
                    break;
                case F_ACTION_VERBOSE:
                    sendSimple("VERBOSE", "Log-Ausgabe wiederhergestellt.");
                    break;
            }
        }
    }

    // ── Eingabe bestaetigen → SET-Befehl senden ──────────────────────────────
    void commitEdit() {
        m_editing = false;
        std::string val = m_editBuf;
        m_editBuf.clear();

        std::string cmd;

        if (m_cursor == F_SSID) {
            if (val.empty() || val.size() > 32) {
                m_status = "Fehler: SSID muss 1-32 Zeichen lang sein.";
                return;
            }
            m_cfg.ssid = val;
            cmd = "SET SSID " + val;

        } else if (m_cursor == F_PASS) {
            if (val.size() > 64) {
                m_status = "Fehler: PASS darf max. 64 Zeichen haben.";
                return;
            }
            cmd = "SET PASS " + val;

        } else {
            if (val.empty()) { m_status = "Fehler: Leere Eingabe."; return; }
            uint32_t v = (uint32_t)strtoul(val.c_str(), nullptr, 0);
            if (v > 0x7FF) { m_status = "Fehler: Wert > 0x7FF."; return; }

            uint32_t c1mn = m_cfg.can1Min, c1mx = m_cfg.can1Max;
            uint32_t c2mn = m_cfg.can2Min, c2mx = m_cfg.can2Max;
            if (m_cursor == F_CAN1MIN) c1mn = v;
            if (m_cursor == F_CAN1MAX) c1mx = v;
            if (m_cursor == F_CAN2MIN) c2mn = v;
            if (m_cursor == F_CAN2MAX) c2mx = v;

            if (c1mn > c1mx) { m_status = "Fehler: CAN1_MIN > CAN1_MAX."; return; }
            if (c2mn > c2mx) { m_status = "Fehler: CAN2_MIN > CAN2_MAX."; return; }
            if (c1mx >= c2mn && c1mn <= c2mx) {
                m_status = "Fehler: CAN1 und CAN2 ueberlappen sich.";
                return;
            }
            if (m_cursor == F_CAN1MIN) m_cfg.can1Min = v;
            if (m_cursor == F_CAN1MAX) m_cfg.can1Max = v;
            if (m_cursor == F_CAN2MIN) m_cfg.can2Min = v;
            if (m_cursor == F_CAN2MAX) m_cfg.can2Max = v;

            const char* key = (m_cursor == F_CAN1MIN) ? "CAN1_MIN" :
                              (m_cursor == F_CAN1MAX) ? "CAN1_MAX" :
                              (m_cursor == F_CAN2MIN) ? "CAN2_MIN" : "CAN2_MAX";
            char hexval[10]; snprintf(hexval, sizeof(hexval), "0x%03X", v);
            cmd = std::string("SET ") + key + " " + hexval;
        }

        m_status = "Sende: " + cmd + " ..."; render();
        std::string resp;
        if (sendCmd(cmd, &resp)) {
            m_status = "OK  (SAVE druecken um dauerhaft zu speichern)";
        } else {
            m_status = "Fehler: Keine Antwort von ESP32.";
        }
    }

    // ── Kommando senden, erste sinnvolle Antwortzeile zurueckgeben ───────────
    bool sendCmd(const std::string& cmd, std::string* resp = nullptr) {
        m_ser.flushRx();
        m_ser.sendLine(cmd);
        std::string line;
        for (int i = 0; i < 20; ++i) {
            if (!m_ser.readLine(line, 300)) continue;
            if (line.empty() || line[0] == '>' || line == cmd) continue;
            if (resp) *resp = line;
            return true;
        }
        return false;
    }

    // ── Einfaches Kommando ohne Ergebnis-Parsing (SILENT/VERBOSE) ────────────
    void sendSimple(const std::string& cmd, const char* okMsg) {
        m_status = "Sende: " + cmd + " ..."; render();
        std::string resp;
        if (sendCmd(cmd, &resp)) m_status = okMsg;
        else                     m_status = "Fehler: Keine Antwort von ESP32.";
    }

    // ── SHOW-Antwort einer Zeile parsen (SSID, CAN-Werte) ───────────────────
    // Wird von fetchConfig() und fetchAndShow() geteilt.
    void parseShowLine(const std::string& ln) {
        auto extractVal = [&](const std::string& key) -> std::string {
            if (ln.find(key) == std::string::npos) return "";
            auto c = ln.find(':');
            if (c == std::string::npos) return "";
            std::string s = ln.substr(c + 1);
            auto a = s.find_first_not_of(" \t");
            auto b = s.find_last_not_of(" \t\r\n");
            return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };

        // SSID: Zeile enthaelt "SSID" aber kein "CAN" (CAN1_MIN enthaelt kein SSID)
        if (ln.find("SSID") != std::string::npos && ln.find("CAN") == std::string::npos) {
            std::string s = extractVal("SSID");
            if (!s.empty() && s != "****") m_cfg.ssid = s;
        }

        auto parseHex = [&](const std::string& key, uint32_t& out) {
            std::string s = extractVal(key);
            if (!s.empty()) out = (uint32_t)strtoul(s.c_str(), nullptr, 0);
        };
        parseHex("CAN1_MIN", m_cfg.can1Min);
        parseHex("CAN1_MAX", m_cfg.can1Max);
        parseHex("CAN2_MIN", m_cfg.can2Min);
        parseHex("CAN2_MAX", m_cfg.can2Max);
    }

    // ── Konfiguration vom ESP32 laden (ohne Anzeige) ─────────────────────────
    // BUG-FIX: Liest bis Abschluss-Zeile (nur '=' Zeichen), NICHT beim Header.
    // "=== Bridge-Konfiguration ===" enthaelt Leerzeichen → kein Stop.
    // "===========================" besteht nur aus '=' → korrekter Stop.
    bool fetchConfig() {
        m_ser.flushRx();
        m_ser.sendLine("SHOW");

        bool inBlock = false, gotData = false;
        DWORD deadline = GetTickCount() + 3000;

        while ((int)(deadline - GetTickCount()) > 0) {
            std::string line;
            if (!m_ser.readLine(line, 200)) continue;

            // Echo und Prompt ueberspringen
            if (line.empty() || line == "SHOW" || line[0] == '>') continue;

            if (!inBlock) {
                // Warte auf oeffnende Header-Zeile (enthaelt "===" UND Text)
                // find_first_not_of('=') != npos → Zeile hat nicht nur '='
                if (line.find("===") != std::string::npos &&
                    line.find_first_not_of("= ") != std::string::npos) {
                    inBlock = true;
                }
            } else {
                // Abschluss-Zeile: besteht ausschliesslich aus '=' Zeichen
                if (!line.empty() && line.find_first_not_of('=') == std::string::npos) {
                    gotData = true;
                    break;
                }
                parseShowLine(line);
            }
        }
        return gotData;
    }

    // ── Rohausgabe eines Befehls fullscreen anzeigen ──────────────────────────
    // Nach Tastendruck zurueck zum TUI. Wird fuer SHOW und SHOW MAP verwendet.
    // SILENT vor dem Kommando: unterdrueckt taskUsbTx-Fragmente waehrend der Ausgabe.
    // VERBOSE danach: stellt den normalen Betrieb wieder her.
    void showRawOutput(const std::string& cmd, const std::string& title) {
        clearScreen();
        printAt(C_HEAD,   0, 0, "%-44s", "============================================");
        printAt(C_BRIGHT, 0, 1, "  %-42s", title.c_str());
        printAt(C_HEAD,   0, 2, "%-44s", "============================================");
        printAt(C_DIM,    0, 3, "  > %-40s", cmd.c_str());

        // SILENT senden und Antwort abwarten, dann Puffer leeren
        m_ser.flushRx();
        m_ser.sendLine("SILENT");
        std::string dummy;
        for (int i = 0; i < 15; ++i) {           // max 15 * 100 ms = 1,5 s
            if (!m_ser.readLine(dummy, 100)) break; // Timeout → fertig
            if (dummy[0] == '>') break;             // Prompt empfangen → fertig
        }
        m_ser.flushRx();

        // Eigentliches Kommando senden und Ausgabe anzeigen
        m_ser.sendLine(cmd);

        int row = 5;
        DWORD lastData = GetTickCount();
        DWORD deadline = GetTickCount() + 6000;

        while (row < 21 && (int)(deadline - GetTickCount()) > 0) {
            std::string line;
            if (m_ser.readLine(line, 100)) {
                lastData = GetTickCount();
                // Echo, leere Zeilen und Prompt ueberspringen
                if (!line.empty() && line != cmd && line[0] != '>') {
                    printAt(C_NORMAL, 2, row, "%-42s", line.c_str());
                    ++row;
                }
            } else if ((int)(GetTickCount() - lastData) > 800) {
                break;  // 800 ms ohne neue Daten: Ausgabe fertig
            }
        }

        // VERBOSE senden bevor Taste gewartet wird
        m_ser.flushRx();
        m_ser.sendLine("VERBOSE");
        for (int i = 0; i < 15; ++i) {
            if (!m_ser.readLine(dummy, 100)) break;
            if (dummy[0] == '>') break;
        }
        m_ser.flushRx();

        printAt(C_DIM, 0, 22, "%-44s", "--------------------------------------------");
        printAt(C_DIM, 0, 23, "  Druecke beliebige Taste um zurueckzukehren...");
        while (!_kbhit()) Sleep(20);
        _getch();
        clearScreen();
    }

    // ── SAVE: NVS schreiben + ESP32-Neustart + Reconnect ─────────────────────
    void executeSave() {
        m_status = "Speichere in NVS..."; render();
        m_ser.sendLine("SAVE");
        std::string line; m_ser.readLine(line, 2000);
        m_status = "ESP32 startet neu..."; render();
        Sleep(3500);
        doReconnect("Gespeichert und neu verbunden.",
                    "Fehler: Neuverbindung nach SAVE fehlgeschlagen.");
    }

    // ── RESET: NVS loeschen + ESP32-Neustart + Reconnect ─────────────────────
    void executeReset() {
        m_status = "NVS wird geloescht..."; render();
        m_ser.sendLine("RESET");
        std::string line; m_ser.readLine(line, 2000);
        m_status = "ESP32 startet neu..."; render();
        Sleep(3500);
        doReconnect("NVS geloescht  --  Compile-Zeit-Werte aktiv.",
                    "Fehler: Neuverbindung nach RESET fehlgeschlagen.");
    }

    // ── Wiederverbindung nach ESP32-Neustart ──────────────────────────────────
    void doReconnect(const char* successMsg, const char* failMsg) {
        m_ser.close();
        for (int i = 1; i <= 6; ++i) {
            Sleep(1000);
            char buf[48]; snprintf(buf, sizeof(buf), "Verbinde neu... (%d/6)", i);
            m_status = buf; render();
            if (m_ser.open(m_port, m_baud)) {
                Sleep(300);
                fetchConfig();
                m_status = successMsg;
                return;
            }
        }
        m_status = failMsg;
    }
};

// ══════════════════════════════════════════════════════════════════════════════
// USB-Verbindungspruefung
// ══════════════════════════════════════════════════════════════════════════════
static bool checkUsb(const std::string& port, int baud, ConfigSerial& ser) {
    printf("Pruefe USB-Verbindung (%s, %d Bd)...\n", port.c_str(), baud);

    if (!ser.open(port, baud)) {
        printf("\nFehler: %s\n\n", ser.lastError().c_str());
        printf("Bitte pruefen:\n");
        printf("  - USB-Kabel zwischen PC und ESP32 angeschlossen?\n");
        printf("  - ESP32 eingeschaltet und geflasht?\n");
        printf("  - COM-Port in bridge.ini korrekt? (aktuell: %s)\n\n", port.c_str());
        return false;
    }

    Sleep(300);
    ser.flushRx();

    // Bereitschaft: SHOW senden, auf "===" in Antwort warten
    ser.sendLine("SHOW");
    std::string line;
    bool responded = false;
    for (int i = 0; i < 30 && !responded; ++i) {
        if (ser.readLine(line, 200) && line.find("===") != std::string::npos)
            responded = true;
    }

    if (!responded) {
        printf("\nFehler: ESP32 antwortet nicht auf Port %s\n\n", port.c_str());
        printf("Moegliche Ursachen:\n");
        printf("  - ESP32 startet noch (kurz warten, dann erneut)\n");
        printf("  - Falscher COM-Port in bridge.ini  (aktuell: %s)\n", port.c_str());
        printf("  - Firmware noch nicht geflasht\n\n");
        ser.close();
        return false;
    }

    ser.flushRx();
    printf("ESP32 verbunden.\n");
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// Einstiegspunkt
// ══════════════════════════════════════════════════════════════════════════════
void runConfigureMode(const Config& cfg) {
    if (cfg.usbPort.empty()) {
        printf("\nFehler: kein USB-Port in bridge.ini konfiguriert.\n");
        printf("Benoetigt:  usb_port = COM3\n\n");
        return;
    }

    ConfigSerial ser;
    if (!checkUsb(cfg.usbPort, cfg.usbBaud, ser))
        return;

    ConfigureTui tui(ser, cfg.usbPort, cfg.usbBaud);
    tui.run();

    ser.close();
    printf("\nKonfiguration beendet.\n");
}

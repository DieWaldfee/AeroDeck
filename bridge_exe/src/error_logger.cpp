#include "error_logger.h"
#include <chrono>
#include <ctime>
#include <cstdarg>
#include <cstring>

// ── Spaltenbreiten ────────────────────────────────────────────────────────────
static constexpr int W_TS     = 25;   // [YYYY-MM-DD HH:MM:SS.mmm]
static constexpr int W_CLASS  = 10;
static constexpr int W_CANID  =  7;   // 0xXXX oder -------
static constexpr int W_DEVICE = 20;
static constexpr int W_MSG    = 80;
// Einzug Fortsetzungszeile = alle Präfix-Spalten inkl. Trennzeichen
static constexpr int W_PREFIX = W_TS + 2 + W_CLASS + 2 + W_CANID + 2 + W_DEVICE + 2; // 70

// ── Initialisierung ───────────────────────────────────────────────────────────

void ErrorLogger::setPaths(const std::string& errorPath, const std::string& statusPath) {
    m_errorPath  = errorPath;
    m_statusPath = statusPath;
}

void ErrorLogger::close() {
    if (m_errorFile)  { fclose(m_errorFile);  m_errorFile  = nullptr; }
    if (m_statusFile) { fclose(m_statusFile); m_statusFile = nullptr; }
}

// ── Hilfsfunktionen ───────────────────────────────────────────────────────────

std::string ErrorLogger::dateTime() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    time_t t = system_clock::to_time_t(now);
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec, (int)ms.count());
    return buf;
}

// Baut den Backup-Pfad: "BridgeStatus.log" + n=1 → "BridgeStatus.1.log"
std::string ErrorLogger::backupPath(const std::string& path, int n) {
    const auto dot = path.rfind('.');
    if (dot == std::string::npos)
        return path + "." + std::to_string(n);
    return path.substr(0, dot) + "." + std::to_string(n) + path.substr(dot);
}

bool ErrorLogger::ensureOpen(FILE*& f, const std::string& path) {
    if (f) return true;
    if (path.empty()) return false;
    f = fopen(path.c_str(), "a");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) writeHeader(f);
    return true;
}

void ErrorLogger::writeHeader(FILE* f) {
    fprintf(f,
        "%-*s  %-*s  %-*s  %-*s  %s\n",
        W_TS,     "Timestamp",
        W_CLASS,  "Class",
        W_CANID,  "CAN-ID",
        W_DEVICE, "Device Name",
        "Message");
    fprintf(f,
        "%-*s  %-*s  %-*s  %-*s  %s\n",
        W_TS,     "-------------------------",
        W_CLASS,  "----------",
        W_CANID,  "-------",
        W_DEVICE, "--------------------",
        "--------------------------------------------------------------------------------");
    fflush(f);
}

void ErrorLogger::rotateIfNeeded(FILE*& f, const std::string& path, long long maxBytes) {
    if (!f) return;
    if (ftell(f) < maxBytes) return;

    fclose(f);
    f = nullptr;

    // Ältestes Backup löschen, dann durchschieben
    for (int i = MAX_BACKUPS; i >= 1; --i) {
        const std::string older = backupPath(path, i);
        const std::string newer = (i == 1) ? path : backupPath(path, i - 1);
        rename(newer.c_str(), older.c_str());   // überschreibt vorhandene Datei
    }

    f = fopen(path.c_str(), "w");
    if (f) writeHeader(f);
}

void ErrorLogger::writeFormatted(FILE*& f, const std::string& path, long long maxBytes,
                                  const std::string& cls, const std::string& canIdStr,
                                  const std::string& device, const std::string& msg) {
    rotateIfNeeded(f, path, maxBytes);
    if (!ensureOpen(f, path)) return;

    const std::string ts = dateTime();
    const char* p   = msg.c_str();
    int         rem = (int)msg.size();
    bool        first = true;

    do {
        int chunk = (rem <= W_MSG) ? rem : W_MSG;
        if (chunk == W_MSG) {
            for (int i = W_MSG - 1; i >= 20; --i) {
                if (p[i] == ' ') { chunk = i; break; }
            }
        }

        if (first) {
            fprintf(f, "%-*s  %-*.*s  %-*.*s  %-*.*s  %.*s\n",
                    W_TS,     ts.c_str(),
                    W_CLASS,  W_CLASS,  cls.c_str(),
                    W_CANID,  W_CANID,  canIdStr.c_str(),
                    W_DEVICE, W_DEVICE, device.c_str(),
                    chunk, p);
            first = false;
        } else {
            fprintf(f, "%-*s%.*s\n", W_PREFIX, "", chunk, p);
        }

        p   += chunk;
        rem -= chunk;
        while (rem > 0 && *p == ' ') { ++p; --rem; }
    } while (rem > 0);

    fflush(f);
}

// ── Öffentliche Log-Funktionen ────────────────────────────────────────────────

void ErrorLogger::logOffline(uint32_t canId, const std::string& device, long long silenceMs) {
    char canStr[8], msg[64];
    snprintf(canStr, sizeof(canStr), "0x%03X", canId);
    snprintf(msg,    sizeof(msg),    "Signal lost for %lld ms", silenceMs);
    writeFormatted(m_errorFile, m_errorPath, MAX_ERROR_BYTES, "OFFLINE", canStr, device, msg);
}

void ErrorLogger::logOnline(uint32_t canId, const std::string& device, long long durationMs) {
    char canStr[8], msg[64];
    snprintf(canStr, sizeof(canStr), "0x%03X", canId);
    snprintf(msg,    sizeof(msg),    "Back online after %lld s", durationMs / 1000LL);
    writeFormatted(m_errorFile, m_errorPath, MAX_ERROR_BYTES, "ONLINE", canStr, device, msg);
}

void ErrorLogger::logError(uint32_t canId, const std::string& device,
                            uint8_t code, const std::string& text) {
    char canStr[8], msg[128];
    snprintf(canStr, sizeof(canStr), "0x%03X", canId);
    if (text.empty())
        snprintf(msg, sizeof(msg), "Code=0x%02X", code);
    else
        snprintf(msg, sizeof(msg), "Code=0x%02X %s", code, text.c_str());
    writeFormatted(m_errorFile, m_errorPath, MAX_ERROR_BYTES, "ERROR", canStr, device, msg);
}

void ErrorLogger::logStatus(uint32_t canId, const std::string& device,
                             int rttMs, const std::string& decoded) {
    char canStr[8];
    snprintf(canStr, sizeof(canStr), "0x%03X", canId);
    std::string msg = "RTT=" + std::to_string(rttMs) + " ms  " + decoded;
    writeFormatted(m_statusFile, m_statusPath, MAX_STATUS_BYTES, "STATUS", canStr, device, msg);
}

void ErrorLogger::logTransportError(const std::string& category, const std::string& device,
                                     const std::string& msg) {
    writeFormatted(m_errorFile, m_errorPath, MAX_ERROR_BYTES, category, "-------", device, msg);
}

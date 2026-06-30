#pragma once
#include <string>
#include <cstdint>
#include <cstdio>

// Schreibt Ereignisse strukturiert in BridgeError.log und BridgeStatus.log.
// Feste Spalten: Timestamp(25) | Class(10) | CAN-ID(7) | Device(20) | Message(max 80, umbrochen)
// Beide Dateien rotieren automatisch wenn die Größe den Schwellwert überschreitet.
class ErrorLogger {
public:
    ~ErrorLogger() { close(); }

    void setPaths(const std::string& errorPath, const std::string& statusPath);
    void close();

    void logOffline       (uint32_t canId, const std::string& device, long long silenceMs);
    void logOnline        (uint32_t canId, const std::string& device, long long durationMs);
    void logError         (uint32_t canId, const std::string& device,
                           uint8_t code, const std::string& text);
    void logStatus        (uint32_t canId, const std::string& device,
                           int rttMs, const std::string& decoded);
    void logTransportError(const std::string& category, const std::string& device,
                           const std::string& msg);

private:
    static constexpr long long MAX_ERROR_BYTES  = 1024LL * 1024;   // 1 MB
    static constexpr long long MAX_STATUS_BYTES =  512LL * 1024;   // 512 KB
    static constexpr int       MAX_BACKUPS      = 3;

    void writeFormatted(FILE*& f, const std::string& path, long long maxBytes,
                        const std::string& cls, const std::string& canIdStr,
                        const std::string& device, const std::string& msg);
    bool ensureOpen    (FILE*& f, const std::string& path);
    void writeHeader   (FILE* f);
    void rotateIfNeeded(FILE*& f, const std::string& path, long long maxBytes);
    static std::string backupPath(const std::string& path, int n);
    static std::string dateTime();

    std::string m_errorPath;
    std::string m_statusPath;
    FILE*       m_errorFile  = nullptr;
    FILE*       m_statusFile = nullptr;
};

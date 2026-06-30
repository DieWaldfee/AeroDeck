#include "file_logger.h"
#include <chrono>
#include <ctime>
#include <algorithm>
#include <vector>
#include <cstring>

// ── Hilfsfunktionen ──────────────────────────────────────────────────────────

std::string FileLogger::timestamp() {
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
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
             lt.tm_hour, lt.tm_min, lt.tm_sec, (int)ms.count());
    return buf;
}

std::string FileLogger::dateTime() {
    time_t t = time(nullptr);
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    return buf;
}

// ── Öffnen / Schließen ───────────────────────────────────────────────────────

bool FileLogger::open(const std::string& path,
                      const Config&      cfg,
                      const std::string& transport) {
    // Header nur schreiben wenn Datei leer oder neu ist
    bool writeHeader = true;
    if (FILE* test = fopen(path.c_str(), "rb")) {
        fseek(test, 0, SEEK_END);
        writeHeader = (ftell(test) == 0);
        fclose(test);
    }

    m_file = fopen(path.c_str(), "a");
    if (!m_file) return false;

    if (writeHeader) {
        fprintf(m_file,
            "================================================================\n"
            "bridge.exe  MSFS SimConnect Bridge  Log\n"
            "Erstellt :  %s\n"
            "Transport:  %s\n"
            "Module: %zu\n",
            dateTime().c_str(), transport.c_str(), cfg.modules.size());

        for (const auto& mod : cfg.modules) {
            fprintf(m_file, "  0x%03X  %-14s  %s  (%s)\n",
                    mod.canTxId,
                    mod.deviceName.empty() ? mod.iniKey.c_str() : mod.deviceName.c_str(),
                    mod.description.c_str(),
                    mod.type.c_str());
        }
        fprintf(m_file,
            "================================================================\n\n");
    }
    fflush(m_file);
    return true;
}

void FileLogger::close() {
    if (m_file) { fclose(m_file); m_file = nullptr; }
}

// ── Protokoll-Einträge ───────────────────────────────────────────────────────

void FileLogger::logStatus(const std::string& msg) {
    if (!m_file) return;
    fprintf(m_file, "[%s] STATUS  %s\n\n", timestamp().c_str(), msg.c_str());
    fflush(m_file);
}

void FileLogger::logInput(const SimData& data) {
    if (!m_file || data.empty()) return;
    fprintf(m_file, "[%s] INPUT  (SimConnect)\n", timestamp().c_str());

    // Sortiert nach Pool-Index für konsistente Reihenfolge
    std::vector<std::pair<size_t, std::string>> sorted;
    sorted.reserve(data.nameToIdx.size());
    for (const auto& kv : data.nameToIdx)
        sorted.push_back({kv.second, kv.first});
    std::sort(sorted.begin(), sorted.end());

    for (const auto& e : sorted) {
        const std::string& unit = (e.first < data.units.size()) ? data.units[e.first] : "";
        fprintf(m_file, "  %-46s = %+14.6f  %s\n",
                e.second.c_str(), data.values[e.first], unit.c_str());
    }
    fprintf(m_file, "\n");
    fflush(m_file);
}

void FileLogger::logOutput(const ModuleConfig& mod,
                           const uint8_t*      frame,
                           size_t              len) {
    if (!m_file) return;

    const char* dname = mod.deviceName.empty() ? mod.iniKey.c_str() : mod.deviceName.c_str();
    fprintf(m_file, "[%s] OUTPUT  0x%03X  %s  (%zu Byte)\n",
            timestamp().c_str(), mod.canTxId, dname, len);

    // Hex-Dump
    fprintf(m_file, "  HEX: ");
    for (size_t i = 0; i < len; ++i) fprintf(m_file, "%02X ", frame[i]);
    fprintf(m_file, "\n");

    // Dekodierung für attitude_indicator
    if (mod.type == "attitude_indicator" && len >= 14) {
        float   bank = 0.f, pitch = 0.f;
        int16_t slip = 0;
        memcpy(&bank,  frame + 1, 4);
        memcpy(&pitch, frame + 5, 4);
        memcpy(&slip,  frame + 9, 2);
        fprintf(m_file,
            "  flags=0x%02X  bank=%+.2f\xC2\xB0  pitch=%+.2f\xC2\xB0"
            "  slip=%+d  failure=%d  slew=%d\n",
            frame[0],
            bank  * 57.2957795f,
            pitch * 57.2957795f,
            (int)slip, frame[11], frame[12]);
    }
    fprintf(m_file, "\n");
    fflush(m_file);
}

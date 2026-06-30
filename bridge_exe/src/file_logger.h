#pragma once
#include <string>
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include "simconnect_client.h"
#include "module.h"
#include "config.h"

// Protokolliert SimConnect-Input und CAN-Output in eine Textdatei.
// Format ist strukturiert und KI-auswertbar (Timestamp + Schlüsselzeilen).
class FileLogger {
public:
    // Öffnet die Datei und schreibt den Konfigurations-Header.
    bool open(const std::string& path,
              const Config&      cfg,
              const std::string& transport);
    void close();
    bool isOpen() const { return m_file != nullptr; }

    void logStatus(const std::string& msg);
    void logInput (const SimData& data);
    void logOutput(const ModuleConfig& mod, const uint8_t* frame, size_t len);

private:
    static std::string timestamp();   // HH:MM:SS.mmm
    static std::string dateTime();    // YYYY-MM-DD HH:MM:SS

    FILE* m_file = nullptr;
};

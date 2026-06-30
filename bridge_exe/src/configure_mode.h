#pragma once
#include "config.h"

// Interaktiver TUI-Konfigurator fuer den ESP32 ueber den USB-Serial-Port.
// Aufruf: bridge.exe configure
void runConfigureMode(const Config& cfg);

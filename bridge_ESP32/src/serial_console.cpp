#include "serial_console.h"
#include "bridge_types.h"  // g_consoleRxQueue
#include "config_manager.h"
#include "hid_decoder.h"   // decodeTable, decodeTableSize
#include "usb_keyboard.h"  // kbModName
#include <Arduino.h>
#include <esp_log.h>
#include <string>
#include <cctype>
#include <cstring>

static const char* TAG = "CON";

// ── Log-Unterdrückung: verhindert UART0-Kollision zwischen Serial0 und ESP_LOGI ──
// ESP_LOGI und Serial0.println() verwenden unterschiedliche Schreibpfade auf UART0.
// Bei gleichzeitigem Zugriff werden Bytes gemischt → Ausgaben erscheinen truncated.
static bool           g_logQuiet = false;
static vprintf_like_t g_prevLog  = nullptr;

static int nullLog(const char*, va_list) { return 0; }

static void logSilent() {
    if (!g_logQuiet) {
        g_prevLog  = esp_log_set_vprintf(nullLog);
        g_logQuiet = true;
        if (g_taskUsbTxHandle) vTaskSuspend(g_taskUsbTxHandle);
        Serial0.flush();              // ESP32-UART-FIFO leeren
        vTaskDelay(pdMS_TO_TICKS(20)); // CH343 interner FIFO: ~5-16 ms bis USB-Packet gesendet
    }
}

static void logVerbose() {
    if (g_logQuiet && g_prevLog) {
        esp_log_set_vprintf(g_prevLog);
        g_prevLog  = nullptr;
        g_logQuiet = false;
        if (g_taskUsbTxHandle) vTaskResume(g_taskUsbTxHandle);
    }
}

// ── SHOW MAP: Button-Decode-Tabelle + Achsen-Map ausgeben ────────────────────
// logSilent() unterbricht taskUsbTx und ESP_LOGI bevor Text-Output beginnt.
// flush() vor logVerbose() stellt sicher, dass alle Bytes übertragen sind
// bevor taskUsbTx wieder läuft.
static void showMap() {
    const bool wasQuiet = g_logQuiet;
    logSilent();

    char tmp[64];

    // ── Buttons ───────────────────────────────────────────────────────────────
    uint8_t chords = 0, singles = 0;
    for (uint8_t i = 0; i < decodeTableSize; ++i) {
        if (decodeTable[i].hidB > 0) ++chords;
        else                          ++singles;
    }

    Serial0.printf("\r\n=== Buttons (%u: %uS %uC) ===\r\n", decodeTableSize, singles, chords);

    if (decodeTableSize == 0) {
        Serial0.printf("  (leer)\r\n");
    } else {
        Serial0.printf("  Name        CAN   By Bi Phys HID     KEY\r\n");
        Serial0.printf("  ----------  ----  -- -- ---- ------  ------\r\n");
        for (uint8_t i = 0; i < decodeTableSize; ++i) {
            const ButtonDecodeEntry& e = decodeTable[i];
            char hid[12];
            if (e.hidB > 0) snprintf(hid, sizeof(hid), "%u+%u", e.hidA, e.hidB);
            else             snprintf(hid, sizeof(hid), "%u",    e.hidA);
            snprintf(tmp, sizeof(tmp), "  %-10s  0x%03X %2u %2u  %3u %-6s  %s\r\n",
                     e.name, (unsigned)e.canId,
                     e.payByte, e.payBit, e.physIdx, hid, kbModName(e.kbMod));
            Serial0.printf("%s", tmp);
        }
    }
    Serial0.printf("================================\r\n");

    // ── Achsen ────────────────────────────────────────────────────────────────
    uint8_t totalAxes = 0;
    for (uint8_t i = 0; i < axisMapSize; ++i) totalAxes += axisMap[i].axisCount;

    Serial0.printf("\r\n=== Achsen (%uM %uA) ===\r\n", axisMapSize, totalAxes);

    if (axisMapSize == 0) {
        Serial0.printf("  (leer)\r\n");
    } else {
        Serial0.printf("  CAN   Anz  HID-Achsen  KEY\r\n");
        Serial0.printf("  ----  ---  ----------  ------\r\n");
        for (uint8_t i = 0; i < axisMapSize; ++i) {
            const AxisMapEntry& e = axisMap[i];
            char hidAxes[24] = {};
            int tpos = 0;
            for (uint8_t j = 0; j < e.axisCount && tpos < (int)sizeof(hidAxes) - 4; ++j) {
                if (j > 0) tpos += snprintf(hidAxes + tpos, sizeof(hidAxes) - tpos, ",");
                tpos += snprintf(hidAxes + tpos, sizeof(hidAxes) - tpos, "%u", e.hidIdx[j]);
            }
            snprintf(tmp, sizeof(tmp), "  0x%03X  %3u  %-10s  %s\r\n",
                     (unsigned)e.canId, e.axisCount, hidAxes, kbModName(e.kbMod));
            Serial0.printf("%s", tmp);
        }
    }
    Serial0.printf("================================\r\n\r\n");

    Serial0.flush();  // warten bis alle Bytes übertragen, bevor taskUsbTx fortgesetzt wird

    if (!wasQuiet) logVerbose();
}

// ── Kommando-Parser ───────────────────────────────────────────────────────────
// Zerlegt die Eingabe in Schlüsselwort (uppercase) und Wert (original).
// Format: "KEYWORD [SUBKEY [value]]"
static void handleCommand(const std::string& line) {
    if (line.empty()) return;

    // Ersten Token (Keyword) isolieren und uppercase
    auto sp1 = line.find(' ');
    std::string kw  = line.substr(0, sp1);
    std::string rest = (sp1 == std::string::npos) ? "" : line.substr(sp1 + 1);
    for (char& c : kw) c = (char)toupper((unsigned char)c);

    // ── SHOW ──────────────────────────────────────────────────────────────────
    if (kw == "SHOW") {
        if (rest == "MAP" || rest == "map") {
            showMap();
        } else {
            const bool wasQuiet = g_logQuiet;
            logSilent();
            ConfigManager::show();
            Serial0.flush();
            if (!wasQuiet) logVerbose();
        }
        return;
    }

    // ── SILENT / VERBOSE ──────────────────────────────────────────────────────
    if (kw == "SILENT") {
        logSilent();
        Serial0.println("Log unterdrückt. VERBOSE zum Wiederherstellen.");
        return;
    }
    if (kw == "VERBOSE") {
        logVerbose();
        Serial0.println("Log wiederhergestellt.");
        return;
    }

    // ── SAVE ──────────────────────────────────────────────────────────────────
    if (kw == "SAVE") {
        ConfigManager::save();
        Serial0.printf("Gespeichert. Neustart in 1 s...\r\n");
        delay(1000);
        esp_restart();
        return;
    }

    // ── RESET ─────────────────────────────────────────────────────────────────
    if (kw == "RESET") {
        ConfigManager::reset();
        Serial0.printf("NVS geleert. Neustart in 1 s...\r\n");
        delay(1000);
        esp_restart();
        return;
    }

    // ── SET <SUBKEY> <value> ──────────────────────────────────────────────────
    if (kw == "SET") {
        auto sp2 = rest.find(' ');
        std::string subkw = rest.substr(0, sp2);
        // Wert: alles nach dem zweiten Leerzeichen (case-sensitiv!)
        std::string value = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        for (char& c : subkw) c = (char)toupper((unsigned char)c);

        if (subkw == "SSID") {
            g_rtCfg.ssid = value;
            Serial0.printf("SSID = '%s'  (SAVE zum Speichern)\r\n", value.c_str());
        } else if (subkw == "PASS") {
            g_rtCfg.pass = value;
            Serial0.printf("PASS gesetzt  (SAVE zum Speichern)\r\n");
        } else if (subkw == "CAN1_MIN") {
            g_rtCfg.can1IdMin = (uint32_t)strtoul(value.c_str(), nullptr, 0);
            Serial0.printf("CAN1_MIN = 0x%03lX\r\n", (unsigned long)g_rtCfg.can1IdMin);
        } else if (subkw == "CAN1_MAX") {
            g_rtCfg.can1IdMax = (uint32_t)strtoul(value.c_str(), nullptr, 0);
            Serial0.printf("CAN1_MAX = 0x%03lX\r\n", (unsigned long)g_rtCfg.can1IdMax);
        } else if (subkw == "CAN2_MIN") {
            g_rtCfg.can2IdMin = (uint32_t)strtoul(value.c_str(), nullptr, 0);
            Serial0.printf("CAN2_MIN = 0x%03lX\r\n", (unsigned long)g_rtCfg.can2IdMin);
        } else if (subkw == "CAN2_MAX") {
            g_rtCfg.can2IdMax = (uint32_t)strtoul(value.c_str(), nullptr, 0);
            Serial0.printf("CAN2_MAX = 0x%03lX\r\n", (unsigned long)g_rtCfg.can2IdMax);
        } else {
            Serial0.printf("Unbekannt: SET %s\r\n", subkw.c_str());
        }
        return;
    }

    // ── Unbekanntes Kommando ──────────────────────────────────────────────────
    Serial0.printf("Unbekannt: '%s'\r\n", line.c_str());
    Serial0.printf("Kommandos: SHOW  SHOW MAP  SAVE  RESET\r\n");
    Serial0.printf("           SILENT  VERBOSE\r\n");
    Serial0.printf("           SET SSID <v>  SET PASS <v>\r\n");
    Serial0.printf("           SET CAN1_MIN/MAX <hex>  SET CAN2_MIN/MAX <hex>\r\n");
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: SerialConsole
// Wartet auf UART0-Eingabe (Serial0, CH343, COM3), parst Kommandos zeilenweise.
// Nicht-blockierend: wenn kein Byte anliegt, schläft der Task 50 ms.
// ══════════════════════════════════════════════════════════════════════════════
void taskSerialConsole(void* /*param*/) {
    std::string line;
    line.reserve(80);

    // Kurz warten damit USB CDC stabil eingelistet ist
    vTaskDelay(pdMS_TO_TICKS(1500));
    Serial0.printf("\r\n[bridge_ESP32]  Bereit. SHOW = Konfiguration anzeigen\r\n> ");

    for (;;) {
        bool gotInput = false;
        uint8_t b;

        while (xQueueReceive(g_consoleRxQueue, &b, 0) == pdTRUE) {
            char c = (char)b;
            gotInput = true;

            if (c == '\n' || c == '\r') {
                // Zeile abschliessen — führende/nachfolgende Leerzeichen entfernen
                while (!line.empty() && (line.front() == ' ')) line.erase(line.begin());
                while (!line.empty() && (line.back()  == ' ' || line.back() == '\r'))
                    line.pop_back();

                if (!line.empty()) {
                    Serial0.printf("\r\n");
                    handleCommand(line);
                    line.clear();
                }
                Serial0.printf("> ");
            } else if (c == 0x08 || c == 0x7F) {
                // Backspace: letztes Zeichen entfernen
                if (!line.empty()) {
                    line.pop_back();
                    Serial0.printf("\b \b");  // Zeichen löschen
                }
            } else if (c >= 0x20 && c < 0x7F) {
                line += c;
                Serial0.printf("%c", c);  // Echo
            }
        }

        if (!gotInput) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

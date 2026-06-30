#pragma once

// FreeRTOS-Task: liest USB-Serial, parst Kommandos, aktualisiert RuntimeConfig.
// Aktiv sobald USB CDC verbunden ist; blockiert nicht wenn kein Host angeschlossen.
//
// Kommandos:
//   SHOW                     aktuelle Konfiguration anzeigen
//   SET SSID <wert>          WiFi-SSID setzen (case-sensitiv)
//   SET PASS <wert>          WiFi-Passwort setzen (case-sensitiv)
//   SET CAN1_MIN <hex>       CAN-ID-Bereich Modul 1 unten  z.B. 0x000
//   SET CAN1_MAX <hex>       CAN-ID-Bereich Modul 1 oben   z.B. 0x3FF
//   SET CAN2_MIN <hex>       CAN-ID-Bereich Modul 2 unten  z.B. 0x400
//   SET CAN2_MAX <hex>       CAN-ID-Bereich Modul 2 oben   z.B. 0x7FF
//   SAVE                     in NVS schreiben + Neustart
//   RESET                    NVS löschen (Factory-Reset) + Neustart
void taskSerialConsole(void* param);

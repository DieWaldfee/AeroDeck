#pragma once

// ── WiFi-Manager-Task ─────────────────────────────────────────────────────────
// Läuft im Hintergrund, blockiert NICHT setup().
// Verbindet WiFi mit SSID/Pass aus RuntimeConfig (g_rtCfg).
// Setzt g_wifiReady = true bei Erfolg.
// Setzt g_wifiDisabled = true nach Timeout → WiFi.mode(WIFI_OFF).
// Beendet sich selbst (vTaskDelete) nach Ergebnis.
void taskWifiManager(void* param);

// ── UDP-Transport-Tasks ───────────────────────────────────────────────────────
// Beide Tasks warten intern auf g_wifiReady.
// Wenn g_wifiDisabled gesetzt wird, suspendieren sie sich dauerhaft.

// UDP-Empfang → Bridge-IP lernen → g_udpToCanQueue
void taskNetRx(void* param);

// g_canToUdpQueue → UDP senden an Bridge-IP:UDP_RETURN_PORT
void taskNetTx(void* param);

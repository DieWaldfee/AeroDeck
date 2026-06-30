#pragma once

// ══════════════════════════════════════════════════════════════════════════════
// WiFi-Zugangsdaten und Netzwerkkonfiguration  —  Compile-Time-Defaults
//
// VERWENDUNG UND PRIORITÄT:
//   Diese Datei liefert die Startwerte beim ersten Flash oder nach NVS-Reset.
//   Beim Boot lädt ConfigManager::load() (config_manager.cpp) SSID und Passwort
//   aus dem NVS-Namespace "bridge" (Schlüssel "ssid" / "pass").
//   Sind diese NVS-Einträge noch leer (Erstinbetriebnahme, NVS gecleart),
//   fallen die Werte auf WIFI_SSID / WIFI_PASSWORD hier zurück.
//
// STATISCHE IP:
//   WIFI_STATIC_IP = true  →  net_transport.cpp ruft WiFi.config() mit den
//   STATIC_IP/GATEWAY/SUBNET-Werten auf, BEVOR WiFi.begin() läuft.
//   Damit erhält der ESP32 immer dieselbe IP — unabhängig vom DHCP-Server.
//   Die statische IP wird NICHT im NVS gespeichert und kann nur durch
//   Ändern dieser Datei + Neu-Flashen geändert werden.
//
//   WIFI_STATIC_IP = false  →  DHCP; der ESP32 bekommt eine dynamische IP.
//   In diesem Fall sollte bridge.ini udp_ip = 255.255.255.255 gesetzt sein,
//   damit bridge.exe die aktuelle IP per erstem UDP-Paket automatisch lernt.
//
// NVS-INHALT ANZEIGEN / ZURÜCKSETZEN:
//   Serieller Monitor (921600 Baud), dann Befehle:
//     cfg show   → aktuelle Laufzeit-Konfiguration anzeigen
//     cfg reset  → NVS löschen, danach gelten wieder die Werte dieser Datei
// ══════════════════════════════════════════════════════════════════════════════

constexpr char WIFI_SSID[]     = "YourSSID";
constexpr char WIFI_PASSWORD[] = "YourPassWd";

// true  = statische IP (empfohlen, damit bridge.exe immer dieselbe IP anspricht)
// false = DHCP (bridge.ini: udp_ip = 255.255.255.255 setzen)
constexpr bool WIFI_STATIC_IP = false;

// Nur relevant wenn WIFI_STATIC_IP = true:
constexpr uint8_t STATIC_IP[]      = {192, 168,   2, 122};
constexpr uint8_t STATIC_GATEWAY[] = {192, 168,   2,   1};
constexpr uint8_t STATIC_SUBNET[]  = {255, 255, 255,   0};

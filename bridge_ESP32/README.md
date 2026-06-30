# bridge_ESP32

Firmware für den **ESP32-S3-DevKitC-1**. Er ist das zentrale Hardware-Gateway des SimConnectBridge-Systems: empfängt Flugdaten von `bridge.exe` über UART oder WiFi/UDP, verteilt sie als CAN-FD-Pakete an angeschlossene Instrumente und übersetzt Cockpit-Taster und -Regler in einen HID-Joystick.

---

## Hardware

### Benötigte Bauteile

| Bauteil | Bezugsquelle (Beispiel) | Hinweis |
|---|---|---|
| ESP32-S3-DevKitC-1 | Espressif, diverse | Variante mit 2 × USB-C |
| MCP2518FD-Modul (1–2×) | Soldered 333157 (5-V-Modul) | Enthält Level-Shifter für 3,3-V-SPI |
| USB-C-Kabel × 2 | — | Für UART-Port + HID-Port |
| 120-Ω-Widerstände | — | CAN-Bus-Abschluss (je 1× an beiden Enden) |

### Zwei USB-Ports — unterschiedliche Rollen!

Der ESP32-S3-DevKitC-1 hat **zwei USB-C-Anschlüsse** mit völlig unterschiedlichen Funktionen:

| Port | Chip | COM-Port | Rolle |
|---|---|---|---|
| UART-Brücken-Port | CP2102N (links) | z.B. COM3 | Daten zu bridge.exe + Serial-Konsole + Flash-Upload |
| Nativer USB-Port | GPIO 19/20 (rechts) | eigenes Gerät | HID-Joystick → Windows/MSFS |

**Achtung:** Nur den UART-Brücken-Port (CP2102N) zum Flashen und zu bridge.exe verwenden. Der native USB-Port ist ausschließlich der HID-Joystick.

---

## MCP2518FD — Verdrahtung

Der ESP32 hat keinen eingebauten CAN-FD-Controller. Der **MCP2518FD** wird als externer Controller über SPI angebunden. Es können **zwei Module** an zwei separaten SPI-Bussen betrieben werden.

### CAN-Modul 1 — SPI2 (FSPI, bis 80 MHz, empfohlen)

```
ESP32-S3          MCP2518FD-Modul
GPIO 12 ──SCK──▶  SCK
GPIO 13 ──MISO─◀  SDO
GPIO 11 ──MOSI─▶  SDI
GPIO 10 ──CS───▶  nCS
GPIO  9 ──INT──◀  INT
3,3 V   ────────  VCC  (oder 5 V — je nach Modul)
GND     ────────  GND  ← gemeinsam mit ESP32-GND!
```

### CAN-Modul 2 — SPI3 (HSPI, bis 40 MHz, optional)

```
ESP32-S3          MCP2518FD-Modul
GPIO  6 ──SCK──▶  SCK
GPIO  8 ──MISO─◀  SDO
GPIO  5 ──MOSI─▶  SDI
GPIO  4 ──CS───▶  nCS
GPIO  7 ──INT──◀  INT
GND     ────────  GND  ← gleicher GND-Rail!
```

### CAN-Bus-Verdrahtung

```
MCP2518FD (Bridge)        MCP2518FD (Horizont)     MCP2518FD (nächstes Gerät)
    CANH ══════════════════════ CANH ══════════════════ CANH
    CANL ══════════════════════ CANL ══════════════════ CANL
     │                                                   │
  120 Ω                                              120 Ω
(Abschluss)                                      (Abschluss)
```

- **Nur zwei Drähte**: CAN_H und CAN_L
- **120 Ω** an **beiden Enden** des Busses — nicht in der Mitte
- Verdrilltes Leitungspaar (Twisted Pair) bei höheren Bitraten oder längeren Leitungen empfohlen
- **Keine Sternverkabelung** — Linientopologie

### Kritisch: Gemeinsame Masse (GND)

Alle Komponenten (ESP32, beide MCP2518FD-Module, alle Instrumente) **müssen über einen gemeinsamen GND-Rail verbunden sein**. Selbst ein Potentialunterschied von 2–3 V zwischen den GND-Punkten führt zu:
- SPI-Initialisierungsfehlern (`kReadBackErrorWith1MHzSPIClock`, Fehlercode 0x02)
- Instabilem CAN-Bus-Betrieb

Lösung: Zentrale Stromversorgung mit gemeinsamem GND-Rail oder explizite GND-Verbindung zwischen allen Versorgungspunkten.

---

## CAN-FD-Grundlagen

### Bitraten

In `bridge_ESP32/src/config.h` konfiguriert — muss auf allen Busteilnehmern **identisch** sein:

```cpp
constexpr uint32_t CAN_ARB_BPS = 125UL * 1000UL;  // 125 kbit/s
```

| Konfiguration | Wert | Eignung |
|---|---|---|
| **125 kbit/s** (aktuell) | `125UL * 1000UL` | Jumperkabel, Breadboard |
| **500 kbit/s** (Ziel) | `500UL * 1000UL` | Twisted Pair + 120-Ω-Abschluss |
| **Daten-Phase BRS** | `DataBitRateFactor::x4` | Nur mit Twisted Pair + TDC-Konfiguration |

**Achtung:** Bei Bitratenmismatch zwischen zwei Busteilnehmern entstehen 100 % ACK-Fehler — kein Frame kommt durch.

### Erlaubte CAN-FD-Payload-Längen

CAN-FD erlaubt **keine beliebigen Paketlängen**. Gültig sind ausschließlich:

```
0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 Byte
```

Andere Längen (z.B. 30) sind ungültig. Der ACAN2517FD-Treiber verwirft sie lautlos — kein Fehler, kein Log. Immer `msg.pad()` aufrufen um auf die nächste gültige Länge aufzufüllen.

---

## Zweiter CAN-Bus (Erweiterung)

Die Firmware unterstützt zwei unabhängige MCP2518FD-Module:

In `bridge_ESP32/src/config.h`:

```cpp
constexpr uint8_t CAN_MODULE_COUNT = 2;   // 1 oder 2
```

| Modul | SPI-Bus | Pins | ID-Bereich | Verwendung |
|---|---|---|---|---|
| CAN 1 | SPI2 (FSPI) | GPIO 9–13 | `0x000–0x3FF` | Anzeige-Instrumente |
| CAN 2 | SPI3 (HSPI) | GPIO 4–8 | `0x400–0x7FF` | Input-Module + weitere Instrumente |

Der ESP32 routet eingehende CAN-Frames automatisch auf das richtige Modul anhand der ID-Grenzen.

---

## Build-Flags (platformio.ini)

```ini
build_flags =
    -DCAN_HW_ENABLED    ; MCP2518FD-Hardware aktiv (taskCanTx + taskCanRx)
    -DCAN_SIM_INPUTS    ; Achsen/Button-Simulator parallel zur Hardware
                        ; (solange keine echten Input-Module angeschlossen)
                        ; Entfernen sobald echte Input-Module vorhanden
    -DCORE_DEBUG_LEVEL=3  ; 0=aus  1=Fehler  2=Warn  3=Info  4=Debug  5=Verbose
```

Ohne `-DCAN_HW_ENABLED` läuft die Firmware im reinen Software-Simulationsmodus (für Tests ohne Hardware).

Mit `-DCAN_HW_ENABLED` und `-DCAN_SIM_INPUTS` laufen Hardware-CAN und Simulator **parallel** — nützlich, solange noch keine echten Input-Module vorhanden sind.

---

## Transport-Modi

### UART (primär, empfohlen)

- Verbindung über UART-Brücken-Port (CP2102N) mit bridge.exe
- 921600 Baud, 8N1
- Framing: `[0xAA][0x55][len_lo][len_hi][CAN-ID 4 Byte LE][Payload][XOR]`
- Wird aktiv sobald bridge.exe den ersten Frame sendet

### WiFi/UDP (alternativ)

- ESP32 verbindet sich beim Boot mit dem konfigurierten WLAN
- bridge.exe sendet an UDP-Port 4210, empfängt auf Port 4211
- ESP32 lernt die bridge.exe-IP aus dem ersten eingehenden Paket (Broadcast-Discovery mit 255.255.255.255)
- Timeout: nach 10 s ohne WLAN wird WiFi deaktiviert (nur UART-Modus)

**Priorität:** Solange UART-Frames eingehen, unterdrückt der ESP32 den WiFi-Rückkanal. Bei UART-Ausfall fällt er automatisch auf UDP zurück.

---

## Serial-Konsole

Zugang: UART-Brücken-Port, 921600 Baud (z.B. PlatformIO Monitor oder Putty).

```
> SHOW               Aktuelle Konfiguration, WiFi-Status, CAN-ID-Bereiche
> SHOW MAP           HID-Button-Decode-Tabelle aus dem Flash
> SET SSID <wert>    WiFi-SSID setzen
> SET PASS <wert>    WiFi-Passwort setzen
> SAVE               Alle Einstellungen in Flash (NVS) speichern + Neustart
> RESET              NVS löschen (Factory Reset) + Neustart
```

### WiFi-Ersteinrichtung

```
> SET SSID MeinNetzwerk
> SET PASS MeinPasswort
> SAVE
```

---

## HID-Joystick

Der ESP32 erscheint nach dem Einschalten sofort als HID-Joystick in Windows:

| Funktion | Kapazität |
|---|---|
| Buttons | 128 |
| Achsen | 8 (X, Y, Z, Rx, Ry, Rz, Slider, Dial) |

### Button-Typen

| Typ | bridge.ini | Verhalten |
|---|---|---|
| Single | `hid_a` ohne `hid_b` | Ein HID-Button |
| Chord | `hid_a` und `hid_b` | Zwei HID-Buttons gleichzeitig (MSFS-Chord-Binding) |
| + Modifier | `LSHIFT`, `RSHIFT`, `LALT`, `LCTRL`, `RCTRL` | Keyboard-Modifier parallel zum HID-Report |

Das Button-Mapping wird von bridge.exe beim Start übertragen und dauerhaft im Flash gespeichert. Nach Reboot sind Buttons sofort aktiv — ohne laufendes bridge.exe.

### Startreihenfolge (Erstinbetriebnahme)

1. **bridge.exe zuerst starten**
2. **ESP32 einschalten** (oder neustarten)

bridge.exe überträgt das vollständige HID-Button-Mapping an den ESP32, der es im Flash speichert. Danach ist die Startreihenfolge beliebig.

---

## NVS-Flash-Speicher

| Gespeichert | Gesetzt durch |
|---|---|
| WiFi-SSID + Passwort | Serial-Konsole (`SET` + `SAVE`) |
| Button-Decode-Tabelle | bridge.exe (beim Connect) |
| Achsen-Map | bridge.exe (beim Connect, RAM-only: wird immer neu übertragen) |

Factory Reset: `RESET` im Serial Monitor — löscht NVS, Neustart.

---

## WiFi-Zugangsdaten

`src/credentials.h` enthält Fallback-Werte für die erste Inbetriebnahme:

```cpp
// credentials.h — NICHT ins öffentliche Repository einchecken!
constexpr const char* DEFAULT_SSID = "MeinNetz";
constexpr const char* DEFAULT_PASS = "MeinPasswort";
```

Vorlage: `credentials.h.example`. Im Normalbetrieb werden die Werte aus dem NVS verwendet (per `SET SSID` / `SET PASS` / `SAVE` gesetzt).

---

## FreeRTOS-Task-Übersicht

| Task | Priorität | Kern | Funktion |
|---|---|---|---|
| `CanRx` | 6 (höchste) | 1 | CAN-Frames empfangen (zeitkritisch) |
| `CanTx` | 5 | 1 | CAN-Frames senden |
| `NetRx` / `NetTx` | 5 | 1 | UDP-Transport (WiFi) |
| `UartRx` / `UartTx` | 5 | 1 | UART-Transport (bridge.exe) |
| `HidDec` | 3 | 1 | Buttons/Achsen → HID-Joystick |
| `WiFiMgr` | 2 | 1 | WLAN-Verbindungsmanager |
| `SerCon` | 1 | 1 | Serial-Konsole |

---

## Build (PlatformIO)

```bash
# In bridge_ESP32/ öffnen:
pio run --target upload    # Bauen + Flashen (UART-Brücken-Port)
pio device monitor         # Serial Monitor (921600 Baud)
```

### Voraussetzungen

- PlatformIO IDE (VS Code Extension) oder PlatformIO Core CLI
- Korrekte COM-Port-Einstellung in `platformio.ini` (`monitor_port = COMx`)

### Wichtig: `ARDUINO_USB_CDC_ON_BOOT`

Das Board `esp32-s3-devkitc-1` setzt standardmäßig `ARDUINO_USB_CDC_ON_BOOT=1` → `Serial` mappt dann auf USB-CDC (GPIO 19/20, nicht auf UART0 (GPIO 43/44). Dadurch empfängt bridge.exe keine Daten.

Die `platformio.ini` enthält bereits die korrekte Konfiguration:

```ini
build_unflags = -DARDUINO_USB_CDC_ON_BOOT
build_flags   = ... -DARDUINO_USB_CDC_ON_BOOT=0
```

Dieser Fallstrick ist der häufigste Grund, warum bridge.exe keine Verbindung bekommt.

---

## Abhängigkeiten

| Bibliothek | Version | Verwendung |
|---|---|---|
| `espressif32` Platform | aktuell | ESP32-S3 Arduino-Framework |
| `ACAN2517FD` | ^2.1.8 | MCP2518FD CAN-FD-Treiber |
| Arduino ESP32 USB / TinyUSB | im Framework | HID-Joystick |
| WiFi / lwip | im Framework | UDP-Transport |
| Preferences / NVS | im Framework | Flash-Speicher |
| FreeRTOS | im Framework | Task-Verwaltung |

# SimConnectBridge

Dieses Projekt verbindet **Microsoft Flight Simulator 2020 / 2024** über die SimConnect-API mit selbst gebauten Cockpit-Instrumenten. Die Instrumente sind eigenständige ESP32-Module, die über einen **CAN-FD-Bus** angeschlossen werden. Cockpit-Taster und -Regler erscheinen gleichzeitig als **HID-Joystick** in Windows.

---

## Systemübersicht

```
┌─────────────────────────────────────────────────────────────────┐
│  Microsoft Flight Simulator 2020 / 2024 (SimConnect-API)        │
└───────────────────────────┬─────────────────────────────────────┘
                             │  Flugdaten (SimVars: Bank, Pitch, …)
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│  bridge.exe  (Windows-Anwendung)                                │
│  Liest SimConnect, verpackt Daten in CAN-FD-Frames              │
│  Überträgt HID-Button-Mapping an ESP32                          │
└──────┬──────────────────────┬───────────────────────────────────┘
       │ UART 921600 Baud     │ WiFi UDP :4210/:4211 (alternativ)
       │ (primär)             │
       ▼                      ▼
┌─────────────────────────────────────────────────────────────────┐
│  Bridge-ESP32-S3  (ESP32-S3-DevKitC-1)                          │
│  Empfängt CAN-Frames von bridge.exe                             │
│  Verteilt sie auf den CAN-FD-Bus                                │
│  Empfängt Buttons/Achsen aus dem CAN-FD-Bus                     │
│  Sendet HID-Joystick-Reports an Windows                         │
└──────┬───────────────────────────────────┬───────────────────────┘
       │ CAN-FD-Bus (125 kbit/s)           │ HID-Joystick (USB)
       │                                   ▼
       │                       ┌───────────────────────┐
       │                       │  Windows / MSFS        │
       │                       │  128 Buttons, 8 Achsen │
       │                       └───────────────────────┘
       │
       ├──────────────────────────────────────────────────────────
       │  CAN-ID 0x200                      CAN-ID 0x280 ←────────
       ▼                                                          │
┌─────────────────────────────────────────────────────────────────┤
│  Künstlicher Horizont  (Waveshare ESP32-S3 LCD 1.28")           │
│  Zeigt Roll, Pitch, Kugel                                       │
│  DEMO-Modus (IMU) wenn kein CAN-Signal                         │
└─────────────────────────────────────────────────────────────────┘
       │
       └──  weitere Instrumente möglich (CAN-IDs 0x201, 0x202, …)

Optional:
┌─────────────────────────────────────────────────────────────────┐
│  bridge_monitor  (Windows, Debugging)                           │
│  Echtzeit-Anzeige aller CAN-Frames, SimVar-Werte, Status        │
│  UDP :4220 ← bridge.exe                                         │
└─────────────────────────────────────────────────────────────────┘
```

---

## Komponenten

| Verzeichnis | Beschreibung |
|---|---|
| [bridge_exe/](bridge_exe/) | Windows-Anwendung: liest SimConnect, sendet CAN-FD-Frames |
| [bridge_monitor/](bridge_monitor/) | Optionales Debugging-Werkzeug: Echtzeit-Ansicht aller Frames |
| [bridge_ESP32/](bridge_ESP32/) | ESP32-S3-Firmware: CAN-FD-Gateway + HID-Joystick |
| [horizon/](horizon/) | Instrument: Künstlicher Horizont (ESP32-S3 LCD 1.28") |

---

## Benötigte Hardware

### Bridge-ESP32 (Gateway)

| Bauteil | Beschreibung |
|---|---|
| ESP32-S3-DevKitC-1 | Hauptmodul (USB-C, zwei USB-Ports!) |
| MCP2518FD-Modul (1–2×) | Externer CAN-FD-Controller über SPI (z.B. Soldered 333157) |
| USB-C-Kabel × 2 | UART-Port (Daten) + nativer USB-Port (HID-Joystick) |
| 120-Ω-Widerstände | CAN-Bus-Abschluss an beiden Busenden |

### Künstlicher Horizont

| Bauteil | Beschreibung |
|---|---|
| Waveshare ESP32-S3 LCD 1.28" | Rundes Display (GC9A01, 240×240) + QMI8658-IMU an Bord |
| MCP2518FD-Modul (1×) | CAN-FD-Anschluss ans Bus |

### PC-Software

| Voraussetzung | Verwendung |
|---|---|
| Windows 10/11 (64-Bit) | bridge.exe, bridge_monitor |
| Microsoft Flight Simulator | SimConnect-Quelle |
| Visual Studio 2022 (Build Tools) | Kompilieren von bridge.exe und bridge_monitor |
| PlatformIO | Flashen der ESP32-Firmware |

---

## CAN-FD-Bus — Konzept

### Warum CAN-FD?

CAN-FD (Controller Area Network with Flexible Data rate) ist ein Feldbus, der in der Automobilindustrie für robuste, echtzeitfähige Kommunikation entwickelt wurde. Vorteile für dieses Projekt:

- **Multi-Master-fähig**: Jedes Instrument kann selbst senden, kein separater Controller nötig
- **Robustheit**: Differenziell übertragenes Signal, unempfindlich gegen Störungen
- **Bis 64 Byte Payload**: ausreichend für alle Flugdaten eines Instruments
- **Einfaches Kabelrouting**: Alle Geräte hängen an einem gemeinsamen Zweileiterbus

### MCP2518FD — Externer CAN-FD-Controller

Der ESP32 hat keinen eingebauten CAN-FD-Controller (nur CAN 2.0). Daher wird der **MCP2518FD** von Microchip als externer Controller über SPI angebunden. Das Soldered-Modul (5 V, mit Level-Shifter) ist direkt mit dem 3,3-V-SPI des ESP32 kompatibel.

**Kritisch: Gemeinsame Masse (GND)**
Alle Komponenten müssen über einen **gemeinsamen GND-Rail** verbunden sein. Ein Potentialunterschied von nur 2 V zwischen ESP32-GND und MCP2518FD-Versorgung führt zu SPI-Initialisierungsfehlern (kReadBackError), die wie ein Treiber- oder Timingproblem aussehen — es aber nicht sind.

### CAN-FD-Payload-Längen (wichtig!)

CAN-FD unterstützt **keine beliebigen Paketlängen**. Erlaubt sind ausschließlich:

```
0–8, 12, 16, 20, 24, 32, 48, 64 Byte
```

Andere Längen (z.B. 30 Byte) sind **ungültig** und werden vom Treiber lautlos verworfen, ohne Fehlermeldung. Der ACAN2517FD-Treiber bietet `msg.pad()` um automatisch auf die nächste gültige Länge aufzufüllen.

### Bus-Topologie

```
Bridge-ESP32               Horizont               weiteres Instrument
    MCP2518FD ──────────── MCP2518FD ──────────── MCP2518FD
       │                       │                       │
      CAN_H ══════════════════CAN_H══════════════════CAN_H
      CAN_L ══════════════════CAN_L══════════════════CAN_L
       │                                               │
    120 Ω                                           120 Ω
    (Abschluss)                               (Abschluss)
```

- **Zwei Leitungen**: CAN_H und CAN_L (verdrilltes Paar empfohlen)
- **120-Ω-Abschlusswiderstände** an beiden Enden des Busses — **nicht in der Mitte**
- **Kein Sternanschluss** — alle Teilnehmer hintereinander (Linie)
- Stromversorgung ist **getrennt** vom CAN-Bus, nur GND teilen

### Bitraten

| Parameter | Aktuell | Produktionsziel |
|---|---|---|
| Arbitrations-Rate | 125 kbit/s | 500 kbit/s |
| Daten-Rate (BRS) | keine (BRS=0) | 2 Mbit/s (BRS=1, x4) |
| Anforderungen | Jumperkabel, Breadboard | Twisted Pair, TDC, 120-Ω |

Alle Busteilnehmer müssen **identische** Arbitrations-Bitraten verwenden. Mismatch führt zu 100 % ACK-Fehlern.

---

## CAN-ID-Schema

| ID-Bereich | Verwendung | Routing |
|---|---|---|
| `0x001` | ESP32-Bridge-Heartbeat | intern |
| `0x002–0x3FF` | Anzeige-Instrumente (SimVar-Daten) | CAN-Modul 1 |
| `0x400–0x5FF` | Anzeige-Instrumente (SimVar-Daten) | CAN-Modul 2 (falls vorhanden) |
| `0x600–0x6FF` | Input-Module (Buttons, Achsen → HID) | CAN-Modul 2 + HID-Decoder |
| `0x7F0–0x7F2` | HID-Mapping-Protokoll (intern, kein CAN-Bus) | nur intern |

Jedes Modul hat zwei IDs:
- `can_tx_id`: bridge.exe → Instrument (Instrument empfängt)
- `can_rx_id`: Instrument → bridge.exe (Instrument sendet)

Beispiel: Horizont empfängt auf `0x200`, antwortet auf `0x280`.

---

## Konfigurationsdatei `bridge.ini`

Eine einzige `bridge.ini` steuert alle Komponenten des Systems (bridge.exe, bridge_monitor, und indirekt die ESP32-Firmware). Sie liegt im Verzeichnis von `bridge.exe`.

### Wichtigste Abschnitte

```ini
[bridge]
usb_port         = COM3        ; UART-Port des Bridge-ESP32 (COM-Nummer anpassen!)
usb_baud         = 921600      ; Baudrate — nicht ändern
send_interval_ms = 50          ; Senderate SimVar-Daten (50 ms = 20 Hz)

[module.horizon]
can_tx_id             = 0x200   ; bridge → Instrument
can_rx_id             = 0x280   ; Instrument → bridge
heartbeat_interval_ms = 1000    ; Horizont sendet alle 1000 ms einen Keepalive
heartbeat_timeout_ms  = 3000    ; Nach 3 s ohne Keepalive → OFFLINE-Log

[module.horizon.simvars]
; SimVar|Einheit|field_id — field_id bestimmt Position im CAN-Frame
ATTITUDE INDICATOR BANK DEGREES|radians|0x01
ATTITUDE INDICATOR PITCH DEGREES|radians|0x02
```

Vollständige Konfigurationsreferenz → [bridge_exe/README.md](bridge_exe/README.md)

---

## Schnellstart

### 1. ESP32-Bridge-Firmware flashen

```bash
# PlatformIO öffnen: bridge_ESP32/
# Wichtig: UART-Brücken-Port auswählen (COM-Port des CP2102N)
pio run --target upload
```

> **Achtung:** Der ESP32-S3-DevKitC-1 hat **zwei USB-Ports**. Nur der UART-Brücken-Port (CP2102N, erscheint als COM3 oder ähnlich) ist für bridge.exe und den Flash-Upload gedacht. Der native USB-Port (GPIO19/20) ist der HID-Joystick — er erscheint als separates Gerät.

### 2. Horizon-Firmware flashen

```bash
# PlatformIO öffnen: horizon/
# Waveshare-Board anschließen, Upload starten
pio run --target upload
```

### 3. bridge.exe bauen

```powershell
# Aus dem Projekt-Root:
.\build_bridge_exe.ps1
```

Ausgabe: `bridge_exe/build/Release/bridge.exe`

### 4. bridge.ini anpassen

```ini
[bridge]
usb_port = COM3    ; ← COM-Port des UART-Adapters (Gerätemanager prüfen)
```

### 5. Erstinbetriebnahme

1. **bridge.exe zuerst starten** (überträgt beim Start das HID-Button-Mapping)
2. **Bridge-ESP32 einschalten** (oder neustarten)

Nach dieser einmaligen Prozedur ist die Startreihenfolge beliebig — das Mapping ist dauerhaft im ESP32-Flash gespeichert.

### 6. WiFi einrichten (optional, falls kein UART)

```
# Serieller Monitor zum Bridge-ESP32 öffnen (921600 Baud), dann:
> SET SSID MeinNetzwerk
> SET PASS MeinPasswort
> SAVE
```

---

## Transport-Wahl: UART oder WiFi

| Modus | Einstellung in bridge.ini | Vorteil |
|---|---|---|
| **UART** (empfohlen) | `usb_port = COM3` | Kein WLAN nötig, geringste Latenz |
| **WiFi/UDP** | `usb_port` auskommentieren | Kabellos, flexibler Aufbau |

Bei aktivem UART ignoriert der ESP32 automatisch den WiFi-Rückkanal.

---

## Build-Skripte

```powershell
.\build_bridge_exe.ps1       # bridge.exe + bridge.ini nach build/Release/
.\build_bridge_monitor.ps1   # bridge_monitor.exe nach build/Release/
```

---

## Zweiter CAN-Bus (Erweiterung)

Die Bridge-ESP32-Firmware unterstützt **zwei unabhängige MCP2518FD-Module** an zwei separaten SPI-Bussen:

- **CAN-Modul 1** (SPI2/FSPI, bis 80 MHz): Anzeige-Instrumente (IDs `0x000–0x3FF`)
- **CAN-Modul 2** (SPI3/HSPI, bis 40 MHz): Input-Module und weitere Instrumente (IDs `0x400–0x7FF`)

Dadurch können Input-Module (Buttons, Achsen) elektrisch vom Anzeige-Bus getrennt werden. In `bridge_ESP32/src/config.h` mit `CAN_MODULE_COUNT = 2` aktivieren und Pins eintragen.

---

## Abhängigkeiten / Lizenzen

Drittanbieter-Lizenzen: [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt)

| Bibliothek | Verwendung |
|---|---|
| SimConnect SDK (Microsoft) | bridge.exe: MSFS-Anbindung |
| ACAN2517FD (Pierre Molinaro) | ESP32: MCP2518FD CAN-FD-Treiber |
| TFT_eSPI (Bodmer) | horizon: Display-Treiber |
| Arduino ESP32 / TinyUSB | ESP32: USB HID, WiFi, NVS, FreeRTOS |

# bridge.exe

`bridge.exe` ist die zentrale Windows-Anwendung des SimConnectBridge-Systems. Sie liest Flugdaten aus Microsoft Flight Simulator über die SimConnect-API und leitet sie als CAN-FD-Frames an den Bridge-ESP32 weiter. Außerdem überträgt sie beim Start die Button-Decode-Tabelle an den ESP32, sodass der HID-Joystick auch ohne laufendes bridge.exe funktioniert.

---

## Normalbetrieb

```
bridge.exe
```

bridge.exe verbindet sich automatisch mit MSFS sobald dieser läuft — kein manueller Eingriff nötig. Läuft MSFS noch nicht, wartet bridge.exe und versucht alle 5 Sekunden neu zu verbinden. Im Normalbetrieb gibt es keine Konsolenausgabe.

---

## Kommandozeile

```
bridge.exe                               Normalbetrieb (keine Ausgabe)
bridge.exe debug <name>                  Debug-Konsole für Modul <name>
bridge.exe debug toFile [datei]          Alle Daten in Datei protokollieren
bridge.exe debug <name> toFile [datei]   Konsole + Protokoll kombiniert
bridge.exe help | /h | /?               Hilfe anzeigen

<name>   Modul-Schlüssel aus bridge.ini, z.B. "horizon"
[datei]  Protokolldatei (Standard: bridgeDump.log)
```

### Beispiele

```
bridge.exe debug horizon
bridge.exe debug toFile
bridge.exe debug toFile messung_01.log
bridge.exe debug horizon toFile debug.log
```

---

## Debug-Konsole

Gestartet mit `bridge.exe debug <name>` zeigt bridge.exe eine automatisch aktualisierte Konsolenanzeige:

```
┌─ bridge.exe Debug ─────────────────────────────────────────────────────────┐
│ Status: CONNECTED       Transport: COM3 921600       SimVars: 5            │
│ Rate: 19.8 Hz           Frames: 4231                 Letzte Sendung: 1 ms  │
├─ Modul: horizon (CAN 0x200) ───────────────────────────────────────────────┤
│ ATTITUDE INDICATOR BANK DEGREES   =   +12.3 °    Bytes: [1-4]  float32    │
│ ATTITUDE INDICATOR PITCH DEGREES  =    -2.1 °    Bytes: [5-8]  float32    │
│ INCIDENCE BETA                    =   +0.03 rad  Bytes: [9-12] float32    │
│ PARTIAL PANEL ATTITUDE            =      0        Bytes: [29]   uint8      │
│ CAN-Frame: 00 52 D4 63 3F A6 4E 51 BC ...                                 │
├─ Geräte-Status ────────────────────────────────────────────────────────────┤
│ k_Horizon  (0x280)  ONLINE   RTT: 4 ms   bank: +12.3°  pitch: -2.1°       │
│ Monitor               ONLINE   RTT: 2 ms                                   │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Transport-Modi

bridge.exe wählt den Transport automatisch anhand von `bridge.ini`:

| Modus | Bedingung | Beschreibung |
|---|---|---|
| **UART** (empfohlen) | `usb_port` in `[bridge]` gesetzt | Seriell über COM-Port, 921600 Baud |
| **UDP/WiFi** (alternativ) | `usb_port` leer | UDP über Netzwerk an ESP32-IP |

Bei UART hat bridge.exe Vorrang: Sobald ein UART-Frame empfangen wird, unterdrückt der ESP32 den WiFi-Rückkanal für 10 Sekunden.

---

## Konfiguration (bridge.ini)

`bridge.ini` muss im **gleichen Verzeichnis** wie `bridge.exe` liegen. Das Build-Skript kopiert sie automatisch nach `build/Release/`.

### [bridge] — Verbindungsparameter

| Schlüssel | Standard | Beschreibung |
|---|---|---|
| `udp_ip` | `255.255.255.255` | ESP32-IP für WiFi; `255.255.255.255` = Broadcast (Auto-Discovery) |
| `udp_port` | `4210` | UDP-Port des ESP32 (Empfang) |
| `receive_port` | `4211` | UDP-Port von bridge.exe (Rückkanal vom ESP32) |
| `send_interval_ms` | `50` | Senderate der SimVar-Daten in ms (50 = 20 Hz) |
| `usb_port` | *(leer)* | COM-Port für UART (z.B. `COM3`); leer = nur UDP |
| `usb_baud` | `921600` | Baudrate UART; muss mit ESP32 übereinstimmen |

> **Hinweis:** `send_interval_ms` bestimmt wie oft bridge.exe die aktuellen SimConnect-Werte verpackt und an alle Anzeige-Module sendet. Bei 50 ms werden 20 Frames pro Sekunde gesendet — genug für flüssige Anzeigen. Werte unter 50 ms können zu Sendeüberlastung auf dem CAN-Bus führen.

### [simconnect]

| Schlüssel | Standard | Beschreibung |
|---|---|---|
| `app_name` | `MSFS_Bridge` | Name der SimConnect-Verbindung (erscheint in MSFS unter Add-ons) |

### [monitor] — bridge_monitor-Integration

| Schlüssel | Standard | Beschreibung |
|---|---|---|
| `enabled` | `true` | Monitor-Daten weiterleiten (false = bridge_monitor empfängt nichts) |
| `ip` | `127.0.0.1` | IP des Rechners auf dem bridge_monitor läuft |
| `port` | `4220` | UDP-Port von bridge_monitor (Empfang) |
| `cmd_port` | `4221` | UDP-Port von bridge.exe (für Kommandos vom Monitor) |
| `timeout_ms` | `6000` | Nach N ms ohne Monitor-Heartbeat → Monitor OFFLINE |

### [debug]

| Schlüssel | Standard | Beschreibung |
|---|---|---|
| `refresh_ms` | `100` | Aktualisierungsrate der Debug-Konsole in ms |

---

## Module definieren

Ein Modul ist ein angeschlossenes Gerät — Anzeige-Instrument, Input-Panel oder beides. Jedes Modul wird durch einen `[module.NAME]`-Abschnitt beschrieben.

### Anzeige-Instrument (SimVar-Daten → CAN)

```ini
[module.horizon]
can_tx_id             = 0x200        ; bridge → Gerät (systemweit eindeutig!)
can_rx_id             = 0x280        ; Gerät → bridge (systemweit eindeutig!)
heartbeat_interval_ms = 1000         ; Gerät sendet alle 1000 ms einen Keepalive
heartbeat_timeout_ms  = 3000         ; Nach 3 s ohne Keepalive → OFFLINE-Log
name                  = k_Horizon    ; Kurzname für Log/Anzeige (kein Leerzeichen)
description           = Künstlicher Horizont
type                  = attitude_indicator

[module.horizon.simvars]
; Format: VARIABLE NAME|einheit|field_id
; field_id bestimmt Bedeutung und Byte-Position im CAN-Frame (→ [field_ids])
ATTITUDE INDICATOR BANK DEGREES|radians|0x01
ATTITUDE INDICATOR PITCH DEGREES|radians|0x02
INCIDENCE BETA|radians|0x09
PARTIAL PANEL ATTITUDE|status|0x06
```

### Input-Modul (Buttons/Achsen → HID-Joystick)

CAN-IDs **müssen** im Bereich `0x600..0x6FF` liegen (HID-Routing des ESP32).

```ini
[module.cockpit_panel]
can_tx_id            = 0x623
can_rx_id            = 0x6A3
heartbeat_timeout_ms = 3000

[module.cockpit_panel.buttons]
; name = payload_byte, payload_bit, physical_idx, hid_a [, hid_b] [, MODIFIER]
COM  = 8, 0, 37, 16, 17          ; Chord: HID-Button 16+17 gleichzeitig
NAV  = 8, 1, 38, 18              ; Single: HID-Button 18
AP   = 8, 2, 39, 19, 20, LSHIFT  ; Chord + Keyboard-Modifier LSHIFT

[module.cockpit_panel.axes]
; name = hid_axis_idx  (0 = X, 1 = Y, 2 = Z, 3 = Rx, 4 = Ry, 5 = Rz, 6 = Slider, 7 = Dial)
THROTTLE = 0
MIXTURE  = 1
```

### Button-Format im Detail

```
name = payload_byte, payload_bit, physical_idx, hid_a [, hid_b] [, MODIFIER]
```

| Feld | Bedeutung |
|---|---|
| `payload_byte` | Byte-Nummer im CAN-Payload (1-basiert: 1 = Data[0]) |
| `payload_bit` | Bit im Byte (0 = LSB, 7 = MSB) |
| `physical_idx` | Referenznummer 0–127 (für Diagnose/SHOW MAP) |
| `hid_a` | Erster virtueller Joystick-Button (1–128) |
| `hid_b` | Zweiter Button optional → Chord (beide gleichzeitig) |
| `MODIFIER` | Keyboard-Modifier: `LSHIFT`, `RSHIFT`, `LALT`, `LCTRL`, `RCTRL` |

Modifier können kombiniert werden: `LCTRL+LSHIFT`

### Gemischtes Gerät (Buttons + SimVar-Anzeige)

Kombination aus `.buttons`/`.axes` und `.simvars` im selben Modul — z.B. ein Panel mit Tasten und Kontrollleuchten.

---

## SimVar-Einheiten

Die Einheit bestimmt den Datentyp und die Byte-Größe im CAN-Frame:

| Einheit | Datentyp | Byte | Beschreibung |
|---|---|---|---|
| `radians` | float32 | 4 | Winkel in Bogenmaß |
| `degrees` | float32 | 4 | Winkel in Grad |
| `feet` | float32 | 4 | Höhe in Fuß |
| `feet per minute` | float32 | 4 | Steigrate |
| `feet per second squared` | float32 | 4 | Beschleunigung |
| `knots` | float32 | 4 | Geschwindigkeit |
| `percent` | float32 | 4 | Prozentwert 0–100 |
| `mbar` | float32 | 4 | Luftdruck in Millibar |
| `celsius` | float32 | 4 | Temperatur |
| `gallons` | float32 | 4 | Kraftstoffmenge |
| `number` | int16 | 2 | Ganzzahl ±32767 |
| `bool` | uint8 | 1 | Schalter 0/1 |
| `status` | uint8 | 1 | Status-Code 0–255 |

**Grenzen pro Modul:** Maximal 15 SimVars, maximal 63 Byte Payload-Summe.

---

## Field-ID-Tabelle

Die `[field_ids]`-Sektion definiert eine systemweite Zuordnung von SimVar-Namen zu numerischen IDs. Die ID bestimmt die **Byte-Position** im CAN-Frame. Das Instrument-Firmware kennt diese IDs fest im Code.

```ini
[field_ids]
ATTITUDE INDICATOR BANK DEGREES  = 0x01   ; float32  4 Byte  → Bytes 1–4
ATTITUDE INDICATOR PITCH DEGREES = 0x02   ; float32  4 Byte  → Bytes 5–8
INCIDENCE BETA                   = 0x09   ; float32  4 Byte
ACCELERATION BODY X              = 0x0A   ; float32  4 Byte
ACCELERATION BODY Y              = 0x0B   ; float32  4 Byte
PLANE BANK DEGREES               = 0x03   ; float32  4 Byte
PLANE PITCH DEGREES              = 0x04   ; float32  4 Byte
PARTIAL PANEL ATTITUDE           = 0x06   ; uint8    1 Byte
```

Die IDs werden in der Reihenfolge der `[module.X.simvars]`-Liste in den CAN-Frame eingebettet. Byte 0 ist immer der Pakettyp (`0x00` = Daten).

---

## Horizon-Datenmodi

Der Künstliche Horizont unterstützt zwei Quellmodi — gesteuert durch die aktiven Zeilen in `[module.horizon.simvars]`:

| Modus | Aktive SimVars | Beschreibung |
|---|---|---|
| **Gyro** (Standard) | `ATTITUDE INDICATOR BANK/PITCH DEGREES` | Kreisel-simuliert, mit Drift |
| **Direkt** | `PLANE BANK/PITCH DEGREES` | Ideale Fluglage, kein Drift |
| **Failure A** | + `PARTIAL PANEL ATTITUDE` | Einfaches Ausfallmodell (Wert 0/1/2) |
| **Failure B** | + `AVIONICS MASTER SWITCH`, `LOW VOLTAGE WARNING` | Reale Avionics-Simulation |

---

## Ausgabedateien

| Datei | Erstellt | Inhalt |
|---|---|---|
| `bridgeError.log` | Automatisch bei erstem Ereignis | Offline/Online-Ereignisse, Fehler |
| `bridgeDump.log` | Nur mit `debug toFile` | Vollständiges Protokoll |

Beide Dateien liegen im gleichen Verzeichnis wie `bridge.exe`.

---

## Fehlerverhalten

| Situation | Verhalten |
|---|---|
| `bridge.ini` fehlt | Warnung, Standardwerte werden verwendet |
| Keine Module konfiguriert | Fehler, Programm beendet sich |
| UART-Port nicht öffenbar | Fehler, Programm beendet sich |
| MSFS nicht gestartet | `WAITING`, Verbindungsversuch alle 5 s |
| MSFS trennt | `DISCONNECTED`, automatischer Reconnect |
| Instrument stumm (Keepalive-Timeout) | OFFLINE-Eintrag in bridgeError.log |
| Instrument meldet sich zurück | ONLINE-Eintrag, erneutes Init-Paket |

---

## Build

```powershell
# Aus dem Projekt-Root:
.\build_bridge_exe.ps1
```

Ausgabe: `bridge_exe/build/Release/bridge.exe` + `SimConnect.dll` + `bridge.ini`

**Voraussetzungen:**
- CMake 3.15+
- Visual Studio 2019+ mit MSVC (C++17)
- Microsoft Flight Simulator SimConnect SDK
  (wird vom Skript automatisch im Standard-Installationspfad gesucht)

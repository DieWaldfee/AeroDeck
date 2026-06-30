# horizon — Künstlicher Horizont

Firmware für den **Waveshare ESP32-S3 LCD 1.28"** als eigenständiges Cockpit-Instrument im SimConnectBridge-System. Das Instrument zeigt Querneigung (Roll), Längsneigung (Pitch) und Kugel (Slip-Anzeige) als animierten künstlichen Horizont auf einem runden 240×240-Farbdisplay.

Im LIVE-Modus empfängt der Horizont Flugdaten über den CAN-FD-Bus von der Bridge-ESP32. Im DEMO-Modus (kein CAN-Signal) nutzt er die eingebaute IMU (QMI8658) und zeigt die physische Ausrichtung des Geräts.

---

## Hardware

### Waveshare ESP32-S3 LCD 1.28"

Dieses Board enthält bereits Display, IMU und Spannungsregler auf einer Platine:

| Komponente | Details |
|---|---|
| Mikrocontroller | ESP32-S3-FH4R2 (8 MB Flash, 2 MB PSRAM) |
| Display | 1.28" rund, GC9A01-Treiber, 240×240 Pixel, SPI |
| IMU | QMI8658 (6-Achsen: Beschleunigung + Gyroskop), I²C |
| Schnittstellen | USB-C (nativ), weitere GPIOs frei |

### Zusätzlich benötigte Bauteile

| Bauteil | Beschreibung |
|---|---|
| MCP2518FD-Modul | Externer CAN-FD-Controller über SPI (z.B. Soldered 333157) |
| 120-Ω-Widerstand | CAN-Bus-Abschluss (wenn der Horizont am Ende des Busses liegt) |

---

## Verdrahtung

### Display (intern — bereits auf dem Board)

Das Display ist intern verdrahtet. Keine separate Verkabelung nötig.

### MCP2518FD — CAN-FD-Modul

```
Waveshare ESP32-S3    MCP2518FD-Modul
GPIO 14 ──SCK──▶      SCK
GPIO 13 ──MISO─◀      SDO
GPIO 15 ──MOSI─▶      SDI
GPIO 16 ──CS───▶      nCS
GPIO 17 ──INT──◀      INT
3,3 V oder 5 V ────   VCC  (je nach Modul)
GND         ────────  GND  ← gemeinsam mit Board-GND!
```

### CAN-Bus-Anschluss

```
MCP2518FD                   CAN-Bus
CANH ──────────────────────  CAN_H (Bus-Leitung)
CANL ──────────────────────  CAN_L (Bus-Leitung)
```

Falls der Horizont am **Ende des CAN-Busses** hängt: 120-Ω-Widerstand zwischen CAN_H und CAN_L anschließen.

### Kritisch: Gemeinsame Masse

GND des MCP2518FD-Moduls muss mit dem GND des Waveshare-Boards verbunden sein. Potentialunterschiede führen zu SPI-Initialisierungsfehlern.

---

## CAN-Konfiguration

In `horizon/src/main.cpp` (Kompilierzeit-Konstanten):

```cpp
static constexpr uint32_t CAN_ARB_BPS = 125UL * 1000UL;  // Muss mit Bridge übereinstimmen!
static constexpr uint32_t CAN_RX_ID   = 0x200;            // Horizon empfängt auf dieser ID
static constexpr uint32_t CAN_TX_ID   = 0x280;            // Horizon sendet auf dieser ID
```

**Wichtig:** `CAN_ARB_BPS` muss auf allen Busteilnehmern (Bridge-ESP32 und allen Instrumenten) identisch sein. Abweichungen führen zu 100 % ACK-Fehlern — kein Frame wird übertragen.

---

## CAN-Protokoll — Empfangene Frames

### 0x00 — Flugdaten (primärer Datenframe)

```
Byte  0   : 0x00 (Pakettyp "Daten")
Bytes 1– 4: ATTITUDE INDICATOR BANK DEGREES    (float32, Radiant)  field_id 0x01
Bytes 5– 8: ATTITUDE INDICATOR PITCH DEGREES   (float32, Radiant)  field_id 0x02
Bytes 9–12: INCIDENCE BETA                     (float32, Radiant)  field_id 0x09
Bytes 13–16: ACCELERATION BODY X               (float32, ft/s²)    field_id 0x0A
Bytes 17–20: ACCELERATION BODY Y               (float32, ft/s²)    field_id 0x0B
Bytes 21–24: PLANE BANK DEGREES                (float32, Radiant)  field_id 0x03
Bytes 25–28: PLANE PITCH DEGREES               (float32, Radiant)  field_id 0x04
Byte  29   : PARTIAL PANEL ATTITUDE            (uint8)             field_id 0x06
```

Mindest-Länge: 30 Byte (nach `msg.pad()` → 32 Byte, CAN-FD-konform).

### 0xFB — Konfiguration (einmalig beim Init)

Überträgt Geräteparameter (z.B. `slip_mode`) vom bridge.exe-Konfigurationsabschnitt `[module.horizon.config]`.

### 0xFC — Init-Bestätigung

Signalisiert dem Horizont, dass bridge.exe seine Heartbeats empfangen hat → Horizont wechselt in den Bereitschaftszustand.

### 0xFD — Heartbeat-Intervall

Teilt dem Horizont mit, in welchem Takt er seine Keepalive-Heartbeats senden soll.

### 0xFE — Status-Request

bridge_monitor fordert den aktuellen Status an. Der Horizont antwortet mit `0x04 STATUS_RESPONSE` (Uptime + aktuelle Lagewinkel) auf `CAN_TX_ID` (0x280).

---

## Betriebsmodi

### LIVE-Modus (CAN-Signal vorhanden)

Der Horizont erhält Flugdaten über CAN-FD von der Bridge-ESP32, die wiederum von bridge.exe/SimConnect befüllt werden. Angezeigt werden Roll und Pitch aus dem Simulator.

Wechsel in LIVE: beim ersten empfangenen `0x00`-Datenframe.  
Rückfall in DEMO: nach 10 Sekunden ohne gültigen Datenframe.

### DEMO-Modus (kein CAN-Signal)

Kein CAN-Bus angeschlossen oder Bridge nicht erreichbar. Der Horizont nutzt die eingebaute QMI8658-IMU und zeigt die physische Ausrichtung des Boards — nützlich für Installations-/Orientierungstests.

---

## Kugel-Modi (Slip-Anzeige)

Die Kugel-Anzeige (Querneigungsanzeige) hat zwei Modi, konfigurierbar in `bridge.ini`:

```ini
[module.horizon.config]
slip_mode = 0   ; 0 = INCIDENCE_BETA (SimVar, ideal)
                ; 1 = ACCEL_DAMPED (Berechnung aus Beschleunigungssensoren)
```

| Modus | Wert | Beschreibung |
|---|---|---|
| `INCIDENCE_BETA` | 0 | Seitengleitwinkel direkt aus SimConnect (exakt, kein Rauschen) |
| `ACCEL_DAMPED` | 1 | Berechnung aus Beschleunigungssensoren (physikalisch, mit Filterdämpfung) |

---

## Display-Design

Der Horizont ist nach dem **Mid-Continent 4300 Series**-Stil gestaltet. Das Design ist in `src/main.cpp` als Kompilierzeit-Konstante wählbar:

```cpp
static constexpr InstrumentConfig CFG = DESIGN_F;
```

| Eigenschaft | Optionen |
|---|---|
| Roll-Skala | `Fixed` (feststehend) / `Rotating` (mitdrehend) |
| Roll-Zeiger | `Fixed` / `Rotating` |
| Horizont-Skala | `Standard` / `HighResolution` |
| Flugzeug-Symbol | `Traditional` / `Delta` |

---

## Build (PlatformIO)

```bash
# In horizon/ öffnen:
pio run --target upload    # Bauen + Flashen
pio device monitor         # Serial Monitor (115200 Baud)
```

### Wichtige Build-Flags (platformio.ini)

```ini
build_flags =
    -DHORIZON_DEBUG=1     ; 0=nur Fehler  1=Boot+Events  2=verbose CAN-Log
    -DGC9A01_DRIVER=1     ; Display-Treiber für TFT_eSPI
    ; TFT-Pins (intern im Board, nicht ändern):
    -DTFT_MOSI=11  -DTFT_SCLK=10  -DTFT_CS=9
    -DTFT_DC=8     -DTFT_RST=12   -DTFT_BL=40
```

### Besonderheit: TFT_eSPI-Konfiguration

TFT_eSPI wird **nicht** über `User_Setup.h` konfiguriert, sondern ausschließlich über Build-Flags in `platformio.ini`. Die Flags müssen in `build_flags` stehen (nicht in `build_src_flags`), damit TFT_eSPI selbst die Display-Treiber-Defines sieht.

---

## Bootvorgang / Statusausgabe

Im Serial Monitor (115200 Baud) erscheinen beim Booten Meldungen wie:

```
IMU: OK
CAN: Arb=125 kbit/s  OSC=40 MHz  Tx-FIFO=8  Rx-FIFO=8
Setup fertig  canOk=ja  imuOk=ja
State: DEMO→LIVE          ← sobald erster CAN-Datenframe empfangen
CAN-RX: 0xFD HB-Interval=1000ms → g_initReceived=true
```

Bei `CAN Init fehler!` auf dem Display: Verkabelung prüfen (GND, SPI-Pins, Spannungsversorgung).

---

## Fehlersuche

| Symptom | Mögliche Ursache | Lösung |
|---|---|---|
| Display bleibt in DEMO-Modus | Kein CAN-Frame empfangen | CAN-Verdrahtung, Bitrate, GND prüfen |
| `kReadBackError` beim Boot | Falscher GND (Potentialunterschied) | GND zwischen Boards verbinden |
| Horizont zeigt falsche Richtung | Roll/Pitch-Vorzeichen falsch | Koordinatenkonvention prüfen (SimConnect liefert `ATTITUDE INDICATOR` in Cockpit-Koordinaten) |
| Ruckeliges Bild | Niedrige Datenrate von bridge.exe | `send_interval_ms` in bridge.ini reduzieren (z.B. 50 ms = 20 Hz) |
| `PARTIAL PANEL ATTITUDE = 2` | Totalausfall simuliert | Schwarzer Bildschirm = korrektes Verhalten |

---

## Abhängigkeiten

| Bibliothek | Version | Verwendung |
|---|---|---|
| `espressif32` Platform | 6.4.0 | ESP32-S3 Arduino-Framework |
| `TFT_eSPI` (Bodmer) | ^2.5.43 | Display-Treiber (GC9A01) |
| `ACAN2517FD` (P. Molinaro) | ^2.1.8 | MCP2518FD CAN-FD-Treiber |
| Wire / I²C | im Framework | QMI8658 IMU |
| Preferences / NVS | im Framework | slip_mode dauerhaft speichern |

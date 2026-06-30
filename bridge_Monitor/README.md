# bridge_monitor

`bridge_monitor` ist ein optionales Debugging-Werkzeug für das SimConnectBridge-System. Es empfängt alle CAN-Frames und Instrument-Feedbacks von `bridge.exe` über UDP und zeigt sie in einer farbigen Echtzeit-Konsolenanzeige. Es kann jederzeit gestartet oder beendet werden, ohne den laufenden Datenbetrieb zu unterbrechen.

**Keine SimConnect-Abhängigkeit** — bridge_monitor benötigt kein SimConnect SDK und kann unabhängig von bridge.exe gebaut werden.

---

## Normalbetrieb

```
bridge_monitor
```

bridge_monitor liest `bridge.ini` aus dem aktuellen Verzeichnis und öffnet die UDP-Ports automatisch. Die Konsolenanzeige aktualisiert sich mit 10 Hz.

Mit explizitem INI-Pfad:

```
bridge_monitor C:\Pfad\zu\bridge.ini
```

---

## Tastenkürzel

| Taste | Aktion |
|---|---|
| `↑` / `↓` | Instrument auswählen |
| `P` | Status-Poll für das gewählte Instrument senden |
| `Q` oder `Ctrl-C` | Beenden |

---

## Konsolenanzeige

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  Bridge Monitor  │  2 Instrumente  │  UDP :4220  │  Cmd :4221              ║
║  ■ Datenfluss: AKTIV  19.7 Hz     │  ■ bridge_exe: ONLINE  RTT:  3 ms      ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  ↑↓: Instrument auswählen   P: Status-Poll   Q: Beenden                    ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  INSTRUMENTE                                                                ║
╠══════════════════════════════════════════════════════════════════════════════╣
║► ■ 0x200  k_Horizon        ■ ONLINE   Frames:  4231  FB: vor 1s            ║
║  ■ 0x201  k_Altimeter      ● OFFLINE  Frames:   891                        ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  DETAIL: k_Horizon (horizon)  │  attitude_indicator                        ║
║  CAN-TX: 0x200  │  CAN-RX: 0x280  │  Timeout: 3000 ms                     ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  > CAN-FRAME (0x200)  │  32 Byte  │  vor 12 ms                             ║
║  00 52 D4 63 3F  A6 4E 51 BC  03 00  00  00  …                             ║
║  Byte 0  Pakettyp                        :     Daten  00                   ║
║  ATTITUDE INDICATOR BANK DEGREES         :   +12.35°  52 D4 63 3F          ║
║  ATTITUDE INDICATOR PITCH DEGREES        :    -2.14°  A6 4E 51 BC          ║
║  INCIDENCE BETA                          :   +0.03°   03 00 00 00          ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  < FEEDBACK (0x280)  │  vor 823 ms                                          ║
║  01 A3 2F 00 00                                                             ║
║  HEARTBEAT  Uptime: 52195 ms  (52 s)                                       ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

### Farbkodierung

| Farbe | Bedeutung |
|---|---|
| Grün | Instrument ONLINE / bridge_exe erreichbar |
| Gelb | Instrument war online, aktuell OFFLINE |
| Rot | Instrument noch nie gesehen |
| Cyan | Feedback-Bereich (Rückkanal vom Instrument) |
| Bunt (je SimVar) | SimVar-Name und Hex-Bytes — gleiche Farbe = zusammengehörig |

---

## Verbindungsstatus

bridge_monitor unterscheidet zwei unabhängige Zustände:

**Datenflussstatus:** Letztes empfangenes Paket < 3 Sekunden alt → `AKTIV`

**bridge_exe-Status (LWT/PONG):** bridge_monitor sendet alle 2 Sekunden einen Heartbeat (`MON_LWT`). bridge.exe antwortet mit `MON_BRIDGE_PONG` und dem Echo-Timestamp — die Differenz ergibt den RTT-Wert. Kein PONG für 6 Sekunden → `OFFLINE`.

---

## Status-Poll (Taste P)

Drücken von `P` sendet für das markierte Instrument einen `STATUS_REQUEST` durch die gesamte Kette:

```
bridge_monitor → bridge.exe → Bridge-ESP32 → Instrument-ESP32
                                          ← STATUS_RESPONSE (RTT, Lagwinkel)
              ←              ←            ←
```

Die Antwort enthält den Roundtrip-Timestamp und (bei entsprechender Instrument-Implementierung) die aktuellen Lagewinkel.

---

## Aktivierung in bridge.ini

bridge_monitor empfängt nur Daten wenn `bridge.exe` das Forwarding aktiviert hat:

```ini
[monitor]
enabled    = true        ; Pflicht — sonst kein Datenfluss
ip         = 127.0.0.1   ; IP des Rechners auf dem bridge_monitor läuft
port       = 4220        ; bridge_monitor lauscht hier
cmd_port   = 4221        ; bridge.exe lauscht hier (für Poll-Kommandos)
timeout_ms = 6000        ; Silence-Timeout für bridge_exe-Verbindung
```

Ist `enabled = false`, erscheint eine Warnung und bridge_monitor startet trotzdem — empfängt aber keine Daten.

---

## Frame-Dekodierung

bridge_monitor dekodiert CAN-Frames automatisch anhand der `bridge.ini`-Konfiguration:

| Einheit | Darstellung |
|---|---|
| `radians` | `±XX.XX°` (umgerechnet in Grad) |
| `degrees`, `feet`, `knots`, `percent`, `mbar`, `celsius` | `±XXXX.XXX` |
| `number` | `±XXXXX` |
| `status` | `0xXX` |
| `bool` | `ON` / `OFF` |

Feedback-Typen (Rückkanal vom Instrument):

| Typ | Anzeige |
|---|---|
| `0x01 HEARTBEAT` | `HEARTBEAT  Uptime: XXXXX ms  (XX s)` |
| `0x04 STATUS_RESPONSE` | `STATUS_RESPONSE  bank=±XX.X°  pitch=±XX.X°` |
| `0x03 ERROR` | `FEHLER  Code: 0xXX` |

---

## Verwendete bridge.ini-Abschnitte

bridge_monitor liest **dieselbe** `bridge.ini` wie bridge.exe. Ausgewertete Abschnitte:

| Abschnitt | Verwendung |
|---|---|
| `[monitor]` | UDP-Ports, LWT-Intervall, Timeout |
| `[module.X]` | CAN-IDs, Return-IDs, Namen, Typen |
| `[module.X.simvars]` | SimVar-Namen und Einheiten (für Frame-Dekodierung) |
| `[field_ids]` | Field-ID-Tabelle (für SimVar-Auflösung) |
| `[unit_sizes]` | Byte-Größen pro Einheit |

Ignoriert: `[bridge]`, `[simconnect]`, `[debug]`, `[module.X.buttons]`, `[module.X.axes]`

---

## Einschränkungen

- **Nur eine Instanz** kann gleichzeitig auf Port 4220 lauschen
- **Nur Beobachtung** — der einzige Eingriff ist der Status-Poll via `P`
- **Konsolengröße** — wird beim Start auf 80 Zeichen gesetzt; bei anderer Fensterbreite kann die Anzeige verzerren

---

## Fehlerverhalten

| Situation | Verhalten |
|---|---|
| `bridge.ini` nicht gefunden | Fehlermeldung, Programm beendet sich |
| Keine Module konfiguriert | Fehlermeldung, Programm beendet sich |
| `[monitor] enabled = false` | Warnung, 2 s Pause, läuft trotzdem |
| UDP-Port belegt (andere Instanz) | Fehlermeldung, Programm beendet sich |
| bridge.exe nicht erreichbar | Zeigt "warte auf PONG…" — läuft weiter |
| Instrument OFFLINE | Gelb oder Rot, Frames-Zähler eingefroren |

---

## Build

```powershell
# Aus dem Projekt-Root:
.\build_bridge_monitor.ps1
```

Ausgabe: `bridge_monitor/build/Release/bridge_monitor.exe` + `bridge.ini`

**Voraussetzungen:**
- CMake 3.15+
- Visual Studio 2019+ mit MSVC (C++17)
- Winsock (ws2_32) — automatisch verlinkt
- **Kein SimConnect SDK erforderlich**

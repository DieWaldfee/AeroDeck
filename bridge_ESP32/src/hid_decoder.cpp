#include "hid_decoder.h"
#include "bridge_types.h"
#include "config.h"
#include "config_manager.h"
#include "hid_joystick.h"
#include "usb_keyboard.h"
#include <cstring>
#include <esp_log.h>

static const char* TAG = "HID";

static constexpr TickType_t KEEPALIVE_TICKS = pdMS_TO_TICKS(20);

// Mapping-Kommando Frame-Konstanten (Payload-Byte 0 = 0xFE, Byte 1 = Subtyp)
static constexpr uint8_t MAP_CMD_BYTE    = 0xFE;
static constexpr uint8_t MAP_SUB_ENTRY   = 0x01;   // Button-Decode-Eintrag
static constexpr uint8_t MAP_SUB_AXIS    = 0x02;   // Achsen-Map-Eintrag
static constexpr uint8_t MAP_SUB_DONE    = 0xFD;   // Abschluss-Marker (alle Daten gesendet)
static constexpr uint8_t MAP_SUB_COMMIT  = 0xFE;   // Button-Tabelle RAM → NVS
static constexpr uint8_t MAP_SUB_RESET   = 0xFF;   // Tabelle leeren

// Daten-Frame Typ-Byte (Achsen + Buttons unified)
static constexpr uint8_t DATA_TYPE_BYTE  = 0x02;

// ── Achsen-Map: canId → (axisCount, hidIdx[]) ─────────────────────────────────
// Wird von bridge.exe via MAP_SUB_AXIS befüllt und in NVS gespeichert.
// AxisMapEntry ist in config_manager.h definiert (packed, 14 Bytes).
AxisMapEntry axisMap[MAX_AXIS_ENTRIES] = {};
uint8_t      axisMapSize               = 0;

// ── Globale Zustände ──────────────────────────────────────────────────────────
ButtonDecodeEntry decodeTable[MAX_DECODE_ENTRIES] = {};
uint8_t           decodeTableSize                 = 0;

JoystickState g_joystickState = {};

// Laufzeit-Zustand: welcher Eintrag ist gerade gedrückt (nicht in NVS gespeichert)
static bool entryPressed[MAX_DECODE_ENTRIES] = {};

// Keyboard-Modifier pro Achsen-Eintrag: aktiv solange mind. eine Achse ≠ 0
static bool axisKbActive[MAX_AXIS_ENTRIES] = {};

// Letzter gesendeter Button-Modifier — Keyboard-Report nur bei Zustandswechsel
static uint8_t s_prevBtnKbMod = KB_NONE;

// ── HID-Buttons aus Decode-Tabelle neu berechnen ─────────────────────────────
// Alle gedrückten Einträge → OR-Logik auf HID-Button-Bits.
// Simultaner Druck mehrerer physikalischer Buttons korrekt abgebildet.
static void recomputeHidButtons() {
    memset(g_joystickState.buttons, 0, sizeof(g_joystickState.buttons));
    uint8_t combinedKbMod = KB_NONE;

    for (uint8_t i = 0; i < decodeTableSize; ++i) {
        if (!entryPressed[i]) continue;

        const uint8_t hidA = decodeTable[i].hidA;
        const uint8_t hidB = decodeTable[i].hidB;

        if (hidA >= 1u && hidA <= 128u) {
            const uint8_t a = hidA - 1u;
            g_joystickState.buttons[a >> 3] |= (uint8_t)(1u << (a & 7u));
        }
        if (hidB >= 1u && hidB <= 128u) {
            const uint8_t b = hidB - 1u;
            g_joystickState.buttons[b >> 3] |= (uint8_t)(1u << (b & 7u));
        }
        combinedKbMod |= decodeTable[i].kbMod;
    }

    if (combinedKbMod != s_prevBtnKbMod) {
        if (combinedKbMod) {
            kbPressModifier(combinedKbMod);
        } else {
            // Nur loslassen wenn kein Achsen-Modifier gerade aktiv
            bool anyAxisActive = false;
            for (uint8_t i = 0; i < axisMapSize; ++i)
                if (axisKbActive[i]) { anyAxisActive = true; break; }
            if (!anyAxisActive) kbReleaseAll();
        }
        s_prevBtnKbMod = combinedKbMod;
    }
}

// ── Mapping-Zustand: gesetzt wenn COMMIT erfolgreich empfangen ───────────────
// Stoppt die REQUEST-Retry-Schleife in taskHidDecoder.
static bool mappingReceived = false;

// ── REQUEST-Frame an bridge.exe senden (CAN-ID = HID_MAP_REQ_CAN_ID) ─────────
// Wird beim Boot und nach Timeout wiederholt bis COMMIT empfangen wurde.
static void sendMapRequest() {
    RawPacket req;
    writeCanId(req.buf, HID_MAP_REQ_CAN_ID);
    req.buf[UDP_HEADER_BYTES] = 0x01;
    req.len = static_cast<uint16_t>(UDP_HEADER_BYTES + 1);
    xQueueSend(g_canToUsbQueue, &req, 0);
    ESP_LOGI(TAG, "HID-Mapping REQUEST gesendet (warte auf bridge.exe)");
}

// ── ACK-Frame an bridge.exe senden (CAN-ID = HID_MAP_ACK_CAN_ID) ─────────────
// sub  = Subtyp-Echo (0xFF RESET, 0x01 ENTRY, 0xFE COMMIT, 0x02 AXIS)
// count = aktueller RAM-Zähler nach der Operation
static void sendMapAck(uint8_t sub, uint8_t count) {
    RawPacket ack;
    writeCanId(ack.buf, HID_MAP_ACK_CAN_ID);
    ack.buf[UDP_HEADER_BYTES]     = sub;
    ack.buf[UDP_HEADER_BYTES + 1] = count;
    ack.len = static_cast<uint16_t>(UDP_HEADER_BYTES + 2);
    xQueueSend(g_canToUsbQueue, &ack, 0);
}

// ── Mapping-Kommando verarbeiten (CAN-ID = HID_MAP_CAN_ID) ───────────────────
static void handleMappingFrame(const uint8_t* pay, uint16_t payLen) {
    // pay[0] = 0xFE (bereits geprüft), pay[1] = Subtyp
    if (payLen < 2) return;
    const uint8_t sub = pay[1];

    if (sub == MAP_SUB_RESET) {
        ConfigManager::resetDecodeTable(decodeTable, decodeTableSize);
        memset(entryPressed, 0, sizeof(entryPressed));
        // axisMap wird NICHT geleert: AXIS-Einträge werden durch MAP_SUB_AXIS gepflegt
        // und haben einen eigenständigen Lebenszyklus unabhängig von der Button-Tabelle.
        memset(axisKbActive, 0, sizeof(axisKbActive));
        s_prevBtnKbMod = KB_NONE;
        recomputeHidButtons();
        ESP_LOGI(TAG, "Mapping: Reset empfangen — Button-Tabelle geleert (axisMap bleibt erhalten)");
        sendMapAck(MAP_SUB_RESET, 0);
    }
    else if (sub == MAP_SUB_AXIS) {
        // Format: [0xFE][0x02][canId 4B LE][axisCount][hidIdx0..N-1][kbMod?]
        if (payLen < 8) return;
        uint32_t cid;
        memcpy(&cid, pay + 2, 4);
        const uint8_t axCnt = pay[6];
        if (axCnt == 0 || axCnt > 8) return;
        AxisMapEntry* e = nullptr;
        for (uint8_t i = 0; i < axisMapSize; i++)
            if (axisMap[i].canId == cid) { e = &axisMap[i]; break; }
        if (!e) {
            if (axisMapSize >= 16) return;
            e = &axisMap[axisMapSize++];
        }
        e->canId     = cid;
        e->axisCount = axCnt;
        memset(e->hidIdx, 0, sizeof(e->hidIdx));
        for (uint8_t i = 0; i < axCnt && (7u + i) < payLen; i++)
            e->hidIdx[i] = pay[7 + i];
        e->kbMod = (payLen > 7u + (uint16_t)axCnt) ? pay[7u + axCnt] : 0u;
        ConfigManager::saveAxisMap(axisMap, axisMapSize);
        ESP_LOGI(TAG, "Mapping: Achsen-Map 0x%03lX → %u Achsen, kbMod=0x%02X",
                 (unsigned long)cid, axCnt, e->kbMod);
        sendMapAck(MAP_SUB_AXIS, axisMapSize);
    }
    else if (sub == MAP_SUB_COMMIT) {
        ConfigManager::saveDecodeTable(decodeTable, decodeTableSize);   // NVS-Write nur bei Änderung
        ESP_LOGI(TAG, "Mapping: Commit empfangen -- %u Eintraege in NVS", decodeTableSize);
        sendMapAck(MAP_SUB_COMMIT, decodeTableSize);
        mappingReceived = true;   // Fallback: REQUEST-Retry stoppen falls DONE verworfen wird
    }
    else if (sub == MAP_SUB_DONE) {
        mappingReceived = true;   // Primärer Trigger: alle Mapping-Daten vollständig empfangen
        sendMapAck(MAP_SUB_DONE, decodeTableSize);
        ESP_LOGI(TAG, "Mapping: Done empfangen -- Konfiguration vollstaendig");
    }
    else if (sub == MAP_SUB_ENTRY) {
        // Format: [0xFE][0x01][canId 4B LE][payByte][payBit][hidA][hidB][physIdx][kbMod][name...]
        // Mindestgröße: 2(cmd) + 4(canId) + 5(felder) + 1(kbMod) = 12 Bytes
        if (payLen < 12) return;
        if (decodeTableSize >= MAX_DECODE_ENTRIES) {
            ESP_LOGW(TAG, "Decode-Tabelle voll (%u Eintraege max)", MAX_DECODE_ENTRIES);
            return;
        }
        ButtonDecodeEntry& e = decodeTable[decodeTableSize];
        uint32_t cid;
        memcpy(&cid, pay + 2, 4);
        e.canId   = cid;
        e.payByte = pay[6];
        e.payBit  = pay[7] & 0x07u;
        e.hidA    = pay[8];
        e.hidB    = pay[9];
        e.physIdx = pay[10];
        e.kbMod   = pay[11];
        // Name: optional, ab pay[12], max. 9 Zeichen + Null
        const uint8_t nameBytes = (payLen > 12u) ? (uint8_t)(payLen - 12u) : 0u;
        const uint8_t copyLen   = (nameBytes < 9u) ? nameBytes : 9u;
        memset(e.name, 0, sizeof(e.name));
        if (copyLen > 0) memcpy(e.name, pay + 12, copyLen);
        entryPressed[decodeTableSize] = false;
        ++decodeTableSize;
        sendMapAck(MAP_SUB_ENTRY, decodeTableSize);
        // Kein NVS-Write hier — nur Commit (MAP_SUB_COMMIT) persistiert die Tabelle.
    }
    else {
        ESP_LOGW(TAG, "Mapping: unbekannter Subtyp 0x%02X", sub);
    }
}

// ── Achsen-Daten dekodieren: [0x02][ax0][ax1]...[btn] ────────────────────────
// Achsen in INI-Reihenfolge laut axisMap. Gibt true zurück wenn Achsen gefunden.
static bool handleAxisFrame(uint32_t srcCanId, const uint8_t* pay, uint16_t payLen) {
    uint8_t entryIdx = 0;
    const AxisMapEntry* entry = nullptr;
    for (uint8_t i = 0; i < axisMapSize; i++) {
        if (axisMap[i].canId == srcCanId) { entry = &axisMap[i]; entryIdx = i; break; }
    }
    if (!entry || entry->axisCount == 0) return false;

    bool anyNonZero = false;
    for (uint8_t i = 0; i < entry->axisCount; i++) {
        const size_t off = 1u + i * 2u;
        if (off + 1u >= payLen) break;
        int16_t val;
        memcpy(&val, pay + off, 2u);
        g_joystickState.axes[entry->hidIdx[i]] = val;
        if (val != 0) anyNonZero = true;
    }

    // Keyboard-Modifier: drücken solange mind. eine Achse ≠ 0; loslassen wenn alle = 0
    if (entry->kbMod) {
        if (anyNonZero && !axisKbActive[entryIdx]) {
            axisKbActive[entryIdx] = true;
            kbPressModifier(entry->kbMod);
        } else if (!anyNonZero && axisKbActive[entryIdx]) {
            axisKbActive[entryIdx] = false;
            // Nur loslassen wenn auch Button-Modifier inaktiv
            uint8_t btnMod = KB_NONE;
            for (uint8_t i = 0; i < decodeTableSize; ++i)
                if (entryPressed[i]) btnMod |= decodeTable[i].kbMod;
            if (!btnMod) kbReleaseAll();
        }
    }

    return true;
}

// ── Button-Daten dekodieren ───────────────────────────────────────────────────
// payload_byte ist 1-indexiert: B{N} = N-tes Byte der Nutzlast → Array-Index N-1.
// Gibt true zurück wenn sich ein Zustand geändert hat.
static bool handleButtonFrame(uint32_t srcCanId, const uint8_t* pay, uint16_t payLen) {
    bool anyChange = false;
    for (uint8_t i = 0; i < decodeTableSize; ++i) {
        if (decodeTable[i].canId != srcCanId) continue;
        if (decodeTable[i].payByte == 0u) continue;
        const size_t absPos = (size_t)decodeTable[i].payByte - 1u;
        if (absPos >= payLen) continue;
        const bool nowPressed = ((pay[absPos] >> decodeTable[i].payBit) & 1u) != 0u;
        if (nowPressed != entryPressed[i]) {
            entryPressed[i] = nowPressed;
            anyChange = true;
        }
    }
    return anyChange;
}

// ── Eingehenden CAN-Frame dispatchen ─────────────────────────────────────────
static bool decodeFrame(const RawPacket& pkt) {
    if (pkt.len < UDP_HEADER_BYTES + 1u) return false;

    const uint32_t srcCanId = readCanId(pkt.buf);
    const uint8_t* pay      = pkt.buf + UDP_HEADER_BYTES;
    const uint16_t payLen   = pkt.len  - UDP_HEADER_BYTES;

    // ── Mapping-Kommando von bridge.exe ───────────────────────────────────────
    if (srcCanId == HID_MAP_CAN_ID) {
        if (pay[0] == MAP_CMD_BYTE) handleMappingFrame(pay, payLen);
        return false;   // Mapping-Kommando ändert keinen HID-Button-Zustand
    }

    // ── Daten-Frame [0x02]: Achsen (INI-Reihenfolge) + Buttons (dahinter) ──────
    if (pay[0] == DATA_TYPE_BYTE) {
        handleAxisFrame(srcCanId, pay, payLen);
        const bool btnChanged = handleButtonFrame(srcCanId, pay, payLen);
        if (btnChanged) recomputeHidButtons();
        return true;   // Achswerte → immer HID-Report senden
    }

    return false;
}

// ══════════════════════════════════════════════════════════════════════════════
// Task: HID-Decoder
// Liest g_canToHidQueue, dekodiert Frames, sendet HID-Reports.
// decodeTable wird vor Task-Start in main.cpp via ConfigManager::loadDecodeTable()
// aus dem NVS geladen — Joystick funktioniert ab dem ersten Boot ohne bridge.exe.
// Prio: TASK_PRIO_HID_DEC | Core 1
// ══════════════════════════════════════════════════════════════════════════════
#ifndef CAN_HW_ENABLED
// Simulator: Button-Decode-Eintrag ohne NVS-Speicherung registrieren.
// Wird von canSimulatorInit() aufgerufen — bleibt nur bis zum nächsten Reboot.
void hidDecoderAddSimEntry(uint32_t canId, uint8_t byte, uint8_t bit, uint8_t hidButton) {
    if (decodeTableSize >= MAX_DECODE_ENTRIES) return;
    ButtonDecodeEntry& e = decodeTable[decodeTableSize];
    e.canId   = canId;
    e.payByte = byte;
    e.payBit  = bit & 0x07u;
    e.hidA    = hidButton;
    e.hidB    = 0;
    e.physIdx = 0;
    memset(e.name, 0, sizeof(e.name));
    entryPressed[decodeTableSize] = false;
    ++decodeTableSize;
}
#endif // !CAN_HW_ENABLED

void taskHidDecoder(void* /*param*/) {
    JoystickState prev       = {};
    TickType_t    lastSendTk = 0;
    RawPacket     pkt;
    uint32_t      lastReqMs  = 0;

    for (;;) {
        // REQUEST senden bis Mapping-COMMIT empfangen (MAP_REQUEST_INTERVAL_MS hardcoded)
        if (!mappingReceived) {
            const uint32_t nowMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (nowMs - lastReqMs >= MAP_REQUEST_INTERVAL_MS) {
                sendMapRequest();
                lastReqMs = nowMs;
            }
        }

        // Auf erstes Paket warten — max. KEEPALIVE_TICKS
        bool stateChanged = false;
        if (xQueueReceive(g_canToHidQueue, &pkt, KEEPALIVE_TICKS) == pdTRUE) {
            stateChanged = decodeFrame(pkt);
            // Burst-Handling: weitere Pakete nicht-blockierend drainieren
            while (xQueueReceive(g_canToHidQueue, &pkt, 0) == pdTRUE) {
                stateChanged |= decodeFrame(pkt);
            }
        }

        // HID-Report senden wenn Zustand geändert oder Keepalive fällig
        const TickType_t now      = xTaskGetTickCount();
        const bool       changed  = stateChanged ||
                                    (memcmp(&g_joystickState, &prev,
                                            sizeof(JoystickState)) != 0);
        const bool       keepaliv = ((now - lastSendTk) >= KEEPALIVE_TICKS);

        if ((changed || keepaliv) && g_hidJoystick.ready()) {
            if (g_hidJoystick.send(g_joystickState)) {
                prev       = g_joystickState;
                lastSendTk = now;
            }
        }
    }
}

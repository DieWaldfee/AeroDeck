#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "module.h"
#include "simconnect_client.h"

// ── Datentyp-Codes (identisch mit Init-Paket-Protokoll) ──────────────────────
// Öffentlich: wird auch in main.cpp für STATUS_RESPONSE-Dekodierung genutzt.
enum class FieldType : uint8_t {
    Float32 = 0x00,   // 4 Byte LE
    Int16   = 0x01,   // 2 Byte LE, mit Vorzeichen
    Uint8   = 0x02,   // 1 Byte
    Bool    = 0x03    // 1 Byte (0/1)
};

// Byte-Anzahl für den gegebenen Typ
uint8_t   fieldTypeBytes(FieldType ft);

// Leitet den Typ aus der SimVar-Einheit ab
FieldType fieldTypeFromSv(const SimVarDef& sv);

// Assembliert einen CAN-Frame für ein Modul mit Output-Fähigkeit.
// Byte 0 = 0x00 (Pakettyp = Daten). Gibt geschriebene Bytes zurück (0 bei Fehler).
size_t assembleCanFrame(const ModuleConfig& mod,
                        const SimData&      data,
                        uint8_t*            out,
                        size_t              outSize);

// Assembliert das Init-Paket (Byte 0 = 0xFC) für ein Modul.
// Enthält alle SimVars mit fieldId != 0 in INI-Reihenfolge mit Offset + Typ-Code.
// Gibt geschriebene Bytes zurück (0 wenn keine aktiven Field-IDs).
size_t assembleInitPacket(const ModuleConfig& mod,
                          uint8_t*            out,
                          size_t              outSize);

// Assembliert das Config-Paket (Byte 0 = 0xFB) für ein Modul.
// Enthält alle configEntries als Key-Value-Paare (config_id + uint8-Wert).
// Gibt geschriebene Bytes zurück (0 wenn configEntries leer).
size_t assembleConfigPacket(const ModuleConfig& mod,
                            uint8_t*            out,
                            size_t              outSize);

// Byte-Layout einer SimVar im CAN-Frame.
struct FrameField {
    size_t offset = 0;  // Startbyte (0-basiert); 0 mit bytes==0 = keine direkte Abbildung
    size_t bytes  = 0;  // Byte-Anzahl; 0 = kein direktes Frame-Byte für diese SimVar
};

// Gibt pro Eintrag in mod.simvars (in INI-Reihenfolge) die Frame-Bytes zurück.
// Vektorgröße == mod.simvars.size().
std::vector<FrameField> getFrameFields(const ModuleConfig& mod);

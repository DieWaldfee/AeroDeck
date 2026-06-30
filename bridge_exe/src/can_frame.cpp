#include "can_frame.h"
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <cstdio>

// ─── Datentyp-Hilfsfunktionen (öffentlich, Deklaration in can_frame.h) ───────

uint8_t fieldTypeBytes(FieldType ft) {
    switch (ft) {
        case FieldType::Float32: return 4;
        case FieldType::Int16:   return 2;
        default:                 return 1;
    }
}

// Leitet den Datentyp aus der SimVar-Einheit ab.
//   "bool"   → Bool    (1 Byte, 0/1)
//   "number" → Int16   (2 Byte, vorzeichenbehaftet)
//   "status" → Uint8   (1 Byte, vorzeichenlos)
//   alles andere → Float32 (radians, degrees, feet, knots, …)
FieldType fieldTypeFromSv(const SimVarDef& sv) {
    if (sv.unit == "bool")   return FieldType::Bool;
    if (sv.unit == "number") return FieldType::Int16;
    if (sv.unit == "status") return FieldType::Uint8;
    return FieldType::Float32;
}

// ─── Generischer Frame-Assembler ─────────────────────────────────────────────
// Frame-Layout (fieldId-gesteuert, INI-Reihenfolge):
//   Byte 0:    0x00  (Pakettyp = Daten)
//   Byte 1+:   Felder in INI-Reihenfolge, Typ und Größe aus Einheit abgeleitet
//
// SimConnect-Werte werden 1:1 übertragen — keine Transformationen.
static size_t assembleGeneric(const ModuleConfig& mod,
                              const SimData&      data,
                              uint8_t*            out,
                              size_t              outSize) {
    if (outSize < 1) return 0;
    std::memset(out, 0, std::min(outSize, (size_t)64));

    out[0] = 0x00;       // Pakettyp = Daten
    uint8_t offset = 1;

    for (const auto& sv : mod.simvars) {
        if (sv.fieldId == 0) continue;

        FieldType ft    = fieldTypeFromSv(sv);
        uint8_t   bytes = fieldTypeBytes(ft);
        if (offset + bytes > outSize) break;

        if (ft == FieldType::Float32) {
            float v = (float)data.get(sv.name);
            std::memcpy(out + offset, &v, 4);
        } else if (ft == FieldType::Int16) {
            int16_t v = (int16_t)(int)data.get(sv.name);
            std::memcpy(out + offset, &v, 2);
        } else {
            out[offset] = (uint8_t)(int)data.get(sv.name);
        }
        offset += bytes;
    }
    return offset;
}

// ─── Frame-Field-Info (für Debug-Anzeige) ────────────────────────────────────
std::vector<FrameField> getFrameFields(const ModuleConfig& mod) {
    std::vector<FrameField> out;
    out.reserve(mod.simvars.size());
    uint8_t offset = 1;   // Byte 0 = Pakettyp (0x00)
    for (const auto& sv : mod.simvars) {
        if (sv.fieldId == 0) {
            out.push_back({0, 0});
            continue;
        }
        FieldType ft    = fieldTypeFromSv(sv);
        uint8_t   bytes = fieldTypeBytes(ft);
        out.push_back({offset, bytes});
        offset += bytes;
    }
    return out;
}

// ─── Init-Paket (0xFC) ───────────────────────────────────────────────────────
size_t assembleInitPacket(const ModuleConfig& mod,
                          uint8_t*            out,
                          size_t              outSize) {
    size_t n = 0;
    for (const auto& sv : mod.simvars)
        if (sv.fieldId != 0) ++n;
    if (n == 0) return 0;

    size_t needed = 3 + n * 4;
    if (outSize < needed) return 0;

    out[0] = 0xFC;          // Pakettyp = Init
    out[1] = 0x01;          // Protokoll-Version
    out[2] = (uint8_t)n;

    uint8_t offset = 1;     // Byte 0 = Pakettyp (0x00) im Daten-Paket
    size_t  pos    = 3;

    for (const auto& sv : mod.simvars) {
        if (sv.fieldId == 0) continue;
        FieldType ft    = fieldTypeFromSv(sv);
        uint8_t   bytes = fieldTypeBytes(ft);
        out[pos++] = sv.fieldId;
        out[pos++] = offset;
        out[pos++] = (uint8_t)ft;
        out[pos++] = 0x00;    // reserviert
        offset    += bytes;
    }
    return pos;
}

// ─── Config-Paket (0xFB) ─────────────────────────────────────────────────────
// Format: [0xFB][N][config_id0][value0]...[config_idN][valueN]
// Wert wird auf uint8 geklemmt (0–255).
size_t assembleConfigPacket(const ModuleConfig& mod,
                            uint8_t*            out,
                            size_t              outSize) {
    const size_t n = mod.configEntries.size();
    if (n == 0) return 0;
    const size_t needed = 2 + n * 2;
    if (outSize < needed) return 0;

    out[0] = 0xFB;
    out[1] = (uint8_t)n;
    size_t pos = 2;
    for (const auto& ce : mod.configEntries) {
        out[pos++] = ce.configId;
        out[pos++] = (uint8_t)(ce.value < 0 ? 0 : ce.value > 255 ? 255 : ce.value);
    }
    return pos;
}

// ─── Dispatcher ──────────────────────────────────────────────────────────────
size_t assembleCanFrame(const ModuleConfig& mod,
                        const SimData&      data,
                        uint8_t*            out,
                        size_t              outSize) {
    return assembleGeneric(mod, data, out, outSize);
}

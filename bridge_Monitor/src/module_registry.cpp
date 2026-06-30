#include "module_registry.h"
#include "monitor_protocol.h"
#include <algorithm>

void ModuleRegistry::init(const std::vector<ModuleConfig>& modules) {
    m_configs = modules;
    m_states.clear();
    m_states.resize(m_configs.size());
    for (size_t i = 0; i < m_configs.size(); ++i)
        m_states[i].cfg = &m_configs[i];
}

ModuleState* ModuleRegistry::findByCan(uint32_t canId) {
    for (auto& s : m_states)
        if (s.cfg && s.cfg->canTxId == canId) return &s;
    return nullptr;
}

ModuleState* ModuleRegistry::findByReturn(uint32_t canRxId) {
    for (auto& s : m_states)
        if (s.cfg && s.cfg->canRxId != 0 && s.cfg->canRxId == canRxId) return &s;
    return nullptr;
}

// Ausgehender CAN-Frame (bridge_exe → Modul, via MON_FRAME_FORWARD)
void ModuleRegistry::updateFrame(uint32_t canId, const uint8_t* data,
                                  size_t len, long long nowMs) {
    auto* s = findByCan(canId);
    if (!s) return;
    size_t copy = std::min(len, sizeof(s->lastFrame));
    std::memcpy(s->lastFrame, data, copy);
    s->lastFrameLen = copy;
    s->lastFrameMs  = nowMs;
    ++s->frameCount;
}

// Feedback vom Modul (via MON_FEEDBACK_FORWARD): Heartbeat und Poll-Response
// Suche zuerst per returnId (Rückkanal-Antwort), dann per canId (Poll-Response-Fallback)
void ModuleRegistry::updateFeedback(uint32_t canId, const uint8_t* data,
                                     size_t len, long long nowMs) {
    auto* s = findByReturn(canId);
    if (!s) s = findByCan(canId);
    if (!s) return;

    size_t copy = std::min(len, sizeof(s->lastFeedback));
    std::memcpy(s->lastFeedback, data, copy);
    s->lastFeedbackLen = copy;
    // lastFeedbackMs: Zeitstempel des letzten Signals (Heartbeat ODER Poll-Response).
    // Abweichung von bridge_exe: bridge_exe nutzt lastHeartbeatMs getrennt für
    // Online/Offline. Hier dient lastFeedbackMs der Latenz-Diagnose ("vor X ms")
    // in der Modul-Liste — ausschließlich für Anzeigequalität im Monitor.
    s->lastFeedbackMs  = nowMs;
    s->everSeen        = true;

    if (len >= 1) {
        if (data[0] == FB_HEARTBEAT) {
            s->lastHeartbeatMs = nowMs;
            if (len >= 5) std::memcpy(&s->heartbeatUptime, data + 1, 4);
        } else if (data[0] == FB_STATUS_RESPONSE) {
            s->lastPollMs = nowMs;
            size_t pc = std::min(len, sizeof(s->lastPollFeedback));
            std::memcpy(s->lastPollFeedback, data, pc);
            s->lastPollFeedbackLen = pc;
        }
    }
}

// Online/Offline-Statusänderung (via MON_STATUS_CHANGE)
void ModuleRegistry::updateStatus(uint32_t canId, bool online) {
    auto* s = findByCan(canId);
    if (!s) return;
    s->online = online;
    if (online) s->everSeen = true;
}

// Eingehender CAN-Frame vom Modul (via MON_RX_FRAME): Heartbeat, Achsen, Buttons
void ModuleRegistry::updateRxFrame(uint32_t canId, const uint8_t* data,
                                    size_t len, long long nowMs) {
    auto* s = findByCan(canId);
    if (!s) return;

    // Heartbeat [0x01][uptime 4B LE]: getrennt tracken, lastFrame nicht überschreiben
    if (len >= 1 && data[0] == 0x01u) {
        s->lastHeartbeatMs = nowMs;
        s->lastFrameMs     = nowMs;
        if (len >= 5) std::memcpy(&s->heartbeatUptime, data + 1, 4);
        return;
    }

    size_t copy = std::min(len, sizeof(s->lastFrame));
    std::memcpy(s->lastFrame, data, copy);
    s->lastFrameLen = copy;
    s->lastFrameMs  = nowMs;
    ++s->frameCount;

    // Achswerte aus Datenframe dekodieren: [0x02][ax0_int16][ax1_int16]...[btn_bytes]
    if (len >= 1 && data[0] == 0x02u) {
        for (size_t ai = 0; ai < s->cfg->axes.size(); ai++) {
            const size_t off = 1 + ai * 2;
            if (off + 1 < len)
                std::memcpy(&s->axisValues[s->cfg->axes[ai].hidAxisIdx], data + off, 2);
        }
    }
}

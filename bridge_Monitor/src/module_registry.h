#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include "module.h"

class ModuleRegistry {
public:
    void init(const std::vector<ModuleConfig>& modules);

    void updateFrame(uint32_t canId, const uint8_t* data, size_t len, long long nowMs);
    void updateFeedback(uint32_t canId, const uint8_t* data, size_t len, long long nowMs);
    void updateStatus(uint32_t canId, bool online);
    void updateRxFrame(uint32_t canId, const uint8_t* data, size_t len, long long nowMs);

    const std::vector<ModuleState>& states() const { return m_states; }
    int count()      const { return (int)m_states.size(); }
    int totalCount() const { return count(); }

    const ModuleState& stateAt(int idx) const { return m_states[idx]; }

private:
    ModuleState* findByCan(uint32_t canId);
    ModuleState* findByReturn(uint32_t returnId);

    std::vector<ModuleConfig> m_configs;
    std::vector<ModuleState>  m_states;
};

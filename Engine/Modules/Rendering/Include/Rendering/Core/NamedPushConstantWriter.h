#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdint>
#include <optional>
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Core/CommandList.h"

namespace GameEngine { namespace Rendering {

// Convenience helper to build a push-constant payload by named members
// using ShaderMeta's PushConstantRangeMeta block layout information.
class NamedPushConstantWriter {
public:
    // Construct by range name
    NamedPushConstantWriter(const ShaderMeta& meta, const std::string& rangeName)
        : m_Meta(&meta), m_RangeName(rangeName) { InitFromName(); }

    // Construct by range id
    NamedPushConstantWriter(const ShaderMeta& meta, uint32_t rangeId)
        : m_Meta(&meta), m_RangeId(rangeId) { InitFromId(); }

    bool IsValid() const { return m_Range != nullptr; }
    uint32_t GetRangeId() const { return m_Range ? m_Range->Id : UINT32_MAX; }
    const std::string& GetRangeName() const { return m_RangeName; }
    uint32_t GetRangeSize() const { return m_Range ? m_Range->Size : 0u; }

    bool Has(const std::string& memberName) const { return m_MemberOffsets.find(memberName) != m_MemberOffsets.end(); }

    std::optional<std::pair<uint32_t,uint32_t>> GetOffsetAndSize(const std::string& memberName) const {
        auto it = m_MemberOffsets.find(memberName);
        if (it == m_MemberOffsets.end()) return std::nullopt;
        return it->second; // {offset,size}
    }

    void Clear(uint8_t value = 0) {
        std::fill(m_Buffer.begin(), m_Buffer.end(), value);
    }

    // Raw add
    bool AddRaw(const std::string& memberName, const void* data, size_t size) {
        auto it = m_MemberOffsets.find(memberName);
        if (it == m_MemberOffsets.end() || !m_Range) return false;
        const uint32_t offset = it->second.first;
        const uint32_t declaredSize = it->second.second;
        if (size > declaredSize || (offset + size) > m_Buffer.size()) return false;
        std::memcpy(m_Buffer.data() + offset, data, size);
        return true;
    }

    template<typename T>
    bool Add(const std::string& memberName, const T& value) {
        return AddRaw(memberName, &value, sizeof(T));
    }

    // Flush to command list using name or id, depending on constructor
    bool Flush(CommandList* cl, uint32_t offset = 0) const {
        if (!cl || !m_Range) return false;
        if (!m_RangeName.empty()) {
            return cl->SetPushConstantsByName(m_RangeName.c_str(), m_Buffer.data(), m_Buffer.size(), offset);
        } else {
            return cl->SetPushConstantsById(m_Range->Id, m_Buffer.data(), m_Buffer.size(), offset);
        }
    }

    // Expose buffer for tests
    const std::vector<uint8_t>& GetBuffer() const { return m_Buffer; }

private:
    void InitFromName() {
        m_Range = nullptr;
        for (const auto& r : m_Meta->PushConstants) if (r.Name == m_RangeName) { m_Range = &r; break; }
        Build();
    }
    void InitFromId() {
        m_Range = nullptr;
        for (const auto& r : m_Meta->PushConstants) if (r.Id == m_RangeId) { m_Range = &r; m_RangeName = r.Name; break; }
        Build();
    }
    void Build() {
        if (!m_Range) return;
        m_Buffer.assign(m_Range->Size, 0);
        // Flatten top-level members
        for (const auto& m : m_Range->Block.Members) {
            m_MemberOffsets[m.Name] = { m.Offset, m.Size };
        }
    }

    const ShaderMeta* m_Meta = nullptr;
    const PushConstantRangeMeta* m_Range = nullptr;
    std::string m_RangeName;
    uint32_t m_RangeId = UINT32_MAX;
    std::vector<uint8_t> m_Buffer;
    std::unordered_map<std::string, std::pair<uint32_t,uint32_t>> m_MemberOffsets;
};

}} // namespace


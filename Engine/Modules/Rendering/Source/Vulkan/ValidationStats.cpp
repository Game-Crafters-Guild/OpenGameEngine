#include "ValidationStats.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace GameEngine
{
namespace Rendering
{

ValidationStatsStore& ValidationStatsStore::Get()
{
    static ValidationStatsStore s_Store;
    return s_Store;
}

ValidationRecordOutcome ValidationStatsStore::Record(const ValidationMessageInfo& info)
{
    (info.IsError ? m_ErrorTotal : m_WarningTotal).fetch_add(1, std::memory_order_relaxed);
    const uint64_t frame = m_FrameSerial.load(std::memory_order_relaxed);

    // Key on the VUID; id-less messages get a stable synthetic key from the
    // message head so repeats still dedupe into one entry.
    char keyBuf[24];
    std::string_view key = info.Vuid;
    if (key.empty())
    {
        uint64_t h = 1469598103934665603ull; // FNV-1a
        const size_t n = std::min<size_t>(info.Message.size(), 256);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= static_cast<uint8_t>(info.Message[i]);
            h *= 1099511628211ull;
        }
        std::snprintf(keyBuf, sizeof(keyBuf), "msg-%016llx", static_cast<unsigned long long>(h));
        key = keyBuf;
    }

    std::scoped_lock lk(m_Mutex);
    if (info.Suppressed)
        ++m_SuppressedTotal;
    for (auto& e : m_Entries)
    {
        if (e.Vuid == key)
        {
            ++e.Count;
            if (info.Suppressed)
                ++e.SuppressedCount;
            e.LastFrame = frame;
            return {false, false, e.Count};
        }
    }
    if (m_Entries.size() >= kMaxTrackedVuids)
    {
        ++m_OverflowTotal;
        return {false, true, m_OverflowTotal};
    }
    ValidationVuidStat e;
    e.Vuid = std::string(key);
    e.IsError = info.IsError;
    e.Count = 1;
    e.SuppressedCount = info.Suppressed ? 1 : 0;
    e.FirstFrame = frame;
    e.LastFrame = frame;
    e.FirstMessage = std::string(info.Message.substr(0, kMaxFirstMessageBytes));
    e.FirstObjects = std::string(info.Objects);
    e.FirstLabels = std::string(info.Labels);
    m_Entries.push_back(std::move(e));
    return {true, false, 1};
}

ValidationStats ValidationStatsStore::Snapshot(bool enabled) const
{
    ValidationStats stats;
    stats.Enabled = enabled;
    stats.FrameSerial = m_FrameSerial.load(std::memory_order_relaxed);
    stats.ErrorCount = m_ErrorTotal.load(std::memory_order_relaxed);
    stats.WarningCount = m_WarningTotal.load(std::memory_order_relaxed);
    std::scoped_lock lk(m_Mutex);
    stats.SuppressedCount = m_SuppressedTotal;
    stats.OverflowCount = m_OverflowTotal;
    stats.Vuids = m_Entries;
    return stats;
}

void ValidationStatsStore::Reset()
{
    std::scoped_lock lk(m_Mutex);
    m_Entries.clear();
    m_SuppressedTotal = 0;
    m_OverflowTotal = 0;
    m_ErrorTotal.store(0, std::memory_order_relaxed);
    m_WarningTotal.store(0, std::memory_order_relaxed);
    // m_FrameSerial deliberately survives reset — it stamps time, not error state.
}

} // namespace Rendering
} // namespace GameEngine

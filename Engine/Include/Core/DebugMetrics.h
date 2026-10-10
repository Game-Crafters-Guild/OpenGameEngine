#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Types/StringId.h"

// Compile-time switch for named-monitor collection. When 0, Register/Push
// become no-ops and Snapshot/CopyHistory return empty data so panel/handler
// code can call through without #if guards.
// CMake: -DENABLE_METRICS=OFF → GE_ENABLE_METRICS=0.
#ifndef GE_ENABLE_METRICS
#define GE_ENABLE_METRICS 1
#endif

namespace GameEngine::Debug
{

enum class MonitorType : std::uint8_t
{
    Quantity,
    Memory,
    TimeMs,
    Percent,
};

struct MonitorInfo
{
    StringId Id = 0;
    std::string Name;
    MonitorType Type = MonitorType::Quantity;
    std::string Unit;
    float Latest = 0.0f;
    float Peak = 0.0f;
    float Average = 0.0f;
};

#if GE_ENABLE_METRICS

// Centralized, thread-safe registry of named time-series samples used by the
// editor Monitors panel and any other diagnostic surface. Panels read history
// on the main thread; producers (render/ECS/physics) push samples from any
// thread. No Engine initialization is required — this is a lazy singleton so
// subsystems can publish without plumbing dependencies.
class DebugMetrics
{
  public:
    static constexpr std::size_t kHistoryLength = 600;

    // Defined out-of-line in DebugMetrics.cpp so the singleton is exported from
    // Engine.dll and shared across every module that links its import lib. See
    // the definition for why header-inlining split the instance.
    static DebugMetrics& Get();

    // Register or update a monitor's metadata. Idempotent — safe to call each
    // frame. Re-registering with the same id does not reset the history.
    void RegisterMonitor(std::string_view name, MonitorType type, std::string_view unit = {})
    {
        const StringId id = HashStringId(name);
        std::scoped_lock lock(m_Mutex);
        auto [it, inserted] = m_Monitors.try_emplace(id);
        Monitor& m = it->second;
        if (inserted)
        {
            m.Name.assign(name);
            m.Type = type;
            m.Unit.assign(unit);
            m.History.fill(0.0f);
            m.Head = 0;
            m.Count = 0;
            m.Peak = 0.0f;
            m.Latest = 0.0f;
        }
    }

    // Push a sample. Unknown names are auto-registered as Quantity so throwaway
    // producers still get captured. Thread-safe.
    void PushSample(std::string_view name, float value)
    {
        if (m_Paused.load(std::memory_order_relaxed))
            return;
        const StringId id = HashStringId(name);
        std::scoped_lock lock(m_Mutex);
        auto it = m_Monitors.find(id);
        if (it == m_Monitors.end())
        {
            auto [inserted, _] = m_Monitors.try_emplace(id);
            inserted->second.Name.assign(name);
            inserted->second.Type = MonitorType::Quantity;
            inserted->second.History.fill(0.0f);
            it = inserted;
        }
        Monitor& m = it->second;
        m.History[m.Head] = value;
        m.Head = (m.Head + 1u) % kHistoryLength;
        if (m.Count < kHistoryLength)
            ++m.Count;
        m.Latest = value;
        if (value > m.Peak)
            m.Peak = value;
    }

    // Copy a monitor's history into dst in chronological order (oldest first).
    // Returns the number of valid samples written. dst is sized to count, so
    // charts never draw a phantom line across zero-padded tail slots.
    std::size_t CopyHistory(std::string_view name, std::vector<float>& dst) const
    {
        dst.clear();
        const StringId id = HashStringId(name);
        std::scoped_lock lock(m_Mutex);
        auto it = m_Monitors.find(id);
        if (it == m_Monitors.end())
            return 0;
        const Monitor& m = it->second;
        const std::size_t count = m.Count;
        dst.resize(count);
        // Oldest sample sits one slot after head when the buffer is full; when
        // partially filled, samples start at index 0 up to count.
        if (count < kHistoryLength)
        {
            for (std::size_t i = 0; i < count; ++i)
                dst[i] = m.History[i];
        }
        else
        {
            const std::size_t start = m.Head; // points at oldest when full
            for (std::size_t i = 0; i < kHistoryLength; ++i)
                dst[i] = m.History[(start + i) % kHistoryLength];
        }
        return count;
    }

    // Lightweight latest-value lookup. Returns true if the monitor exists.
    // Avoids the full string-copying Snapshot() when callers only need the
    // current sample (e.g. per-frame UI label updates).
    bool GetLatest(std::string_view name, float& outValue, MonitorType* outType = nullptr) const
    {
        const StringId id = HashStringId(name);
        std::scoped_lock lock(m_Mutex);
        auto it = m_Monitors.find(id);
        if (it == m_Monitors.end())
            return false;
        outValue = it->second.Latest;
        if (outType)
            *outType = it->second.Type;
        return true;
    }

    // Enumerate all monitors' current metadata as a snapshot (main-thread reader).
    void Snapshot(std::vector<MonitorInfo>& out) const
    {
        out.clear();
        std::scoped_lock lock(m_Mutex);
        out.reserve(m_Monitors.size());
        for (const auto& [id, m] : m_Monitors)
        {
            MonitorInfo info;
            info.Id = id;
            info.Name = m.Name;
            info.Type = m.Type;
            info.Unit = m.Unit;
            info.Latest = m.Latest;
            info.Peak = m.Peak;
            info.Average = ComputeAverage(m);
            out.push_back(std::move(info));
        }
    }

    void SetPaused(bool paused) { m_Paused.store(paused, std::memory_order_relaxed); }
    bool IsPaused() const { return m_Paused.load(std::memory_order_relaxed); }

    void ClearAll()
    {
        std::scoped_lock lock(m_Mutex);
        for (auto& [id, m] : m_Monitors)
        {
            m.History.fill(0.0f);
            m.Head = 0;
            m.Count = 0;
            m.Peak = 0.0f;
            m.Latest = 0.0f;
        }
    }

  private:
    DebugMetrics() = default;

    struct Monitor
    {
        std::string Name;
        std::string Unit;
        MonitorType Type = MonitorType::Quantity;
        std::array<float, kHistoryLength> History{};
        std::size_t Head = 0;
        std::size_t Count = 0;
        float Latest = 0.0f;
        float Peak = 0.0f;
    };

    static float ComputeAverage(const Monitor& m)
    {
        if (m.Count == 0)
            return 0.0f;
        double sum = 0.0;
        for (std::size_t i = 0; i < m.Count; ++i)
            sum += m.History[i];
        return static_cast<float>(sum / static_cast<double>(m.Count));
    }

    mutable std::mutex m_Mutex;
    std::unordered_map<StringId, Monitor> m_Monitors;
    std::atomic<bool> m_Paused{false};
};

#else // GE_ENABLE_METRICS == 0 — shipping/user build stub.

class DebugMetrics
{
  public:
    static constexpr std::size_t kHistoryLength = 600;

    // Out-of-line for the same single-instance reason as the enabled build; the
    // stub still needs one shared object to hang the no-op calls off of.
    static DebugMetrics& Get();

    void RegisterMonitor(std::string_view, MonitorType, std::string_view = {}) {}
    void PushSample(std::string_view, float) {}

    std::size_t CopyHistory(std::string_view, std::vector<float>& dst) const
    {
        dst.assign(kHistoryLength, 0.0f);
        return 0;
    }

    bool GetLatest(std::string_view, float& outValue, MonitorType* = nullptr) const
    {
        outValue = 0.0f;
        return false;
    }

    void Snapshot(std::vector<MonitorInfo>& out) const { out.clear(); }

    void SetPaused(bool) {}
    bool IsPaused() const { return false; }
    void ClearAll() {}

  private:
    DebugMetrics() = default;
};

#endif // GE_ENABLE_METRICS

} // namespace GameEngine::Debug

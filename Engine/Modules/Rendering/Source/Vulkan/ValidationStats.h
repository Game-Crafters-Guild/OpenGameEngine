#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

#include "Rendering/Core/Device.h"

namespace GameEngine
{
namespace Rendering
{

// One validation-layer message, normalized by the debug callback before recording.
struct ValidationMessageInfo
{
    bool IsError = false;
    std::string_view Vuid;    // pMessageIdName; empty when the layer gave none
    std::string_view Message; // full layer text
    std::string_view Objects; // formatted involved-object debug names
    std::string_view Labels;  // formatted command-buffer label stack
    bool Suppressed = false;  // matched kVuidSuppressions
};

struct ValidationRecordOutcome
{
    bool FirstOccurrence = false;
    bool Overflowed = false; // arrived after the distinct-VUID cap filled (counted, not detailed)
    uint64_t Count = 0;      // per-VUID count after this record; overflow ordinal when Overflowed
};

// One log line in every kValidationLogRepeatStride repeats of a VUID, after the
// full first occurrence.
inline constexpr uint64_t kValidationLogRepeatStride = 100;

// Thins the log for a repeating VUID while the store keeps counting it exactly.
//
// Cap the log, never the layer: the Khronos per-VUID cap is off by construction
// (duplicate_message_limit=0) so ValidationStatsStore sees every message, which
// makes this the only bound on log volume for a VUID that fires per-draw. Capping
// the layer instead would bound the log by making the store silently undercount —
// the instrument would lie rather than summarize.
inline bool ShouldLogValidationOccurrence(const ValidationRecordOutcome& outcome) noexcept
{
    return outcome.FirstOccurrence || (outcome.Count % kValidationLogRepeatStride == 0);
}

// Process-wide store behind every VkInstance's debug messenger: exact per-VUID
// counts plus first-occurrence context (message text, object names, label stack,
// frame serial). Mutex-guarded — the callback already pays for string formatting
// on WARNING/ERROR, so the lock is in the noise. The Khronos layer's own dedup cap
// is disabled at instance creation (duplicate_message_limit=0); this store owns
// dedup and always counts exactly, overflowing visibly past kMaxTrackedVuids.
class ValidationStatsStore
{
  public:
    static constexpr uint32_t kMaxTrackedVuids = 64;
    static constexpr size_t kMaxFirstMessageBytes = 2048;

    ValidationRecordOutcome Record(const ValidationMessageInfo& info);
    void NoteFrameBegin() { m_FrameSerial.fetch_add(1, std::memory_order_relaxed); }
    uint64_t ErrorCount() const { return m_ErrorTotal.load(std::memory_order_relaxed); }
    ValidationStats Snapshot(bool enabled) const;
    void Reset();

    // The single store every debug callback records into, whichever VkInstance's
    // messenger fired — a process holding two instances with different
    // configurations still has one set of counts. Tests may also exercise
    // standalone stores directly.
    static ValidationStatsStore& Get();

  private:
    mutable std::mutex m_Mutex;
    std::vector<ValidationVuidStat> m_Entries;
    uint64_t m_SuppressedTotal = 0;
    uint64_t m_OverflowTotal = 0;
    std::atomic<uint64_t> m_ErrorTotal{0};
    std::atomic<uint64_t> m_WarningTotal{0};
    std::atomic<uint64_t> m_FrameSerial{0};
};

} // namespace Rendering
} // namespace GameEngine

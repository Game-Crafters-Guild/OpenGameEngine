#pragma once

// RecomputeElision — exact-input idle elision for GPU pass families whose
// outputs live in persistent GPU resources (culling visibility, draw-stream
// records/counts, cluster light lists, SDSM depth bounds).
//
// A family's GPU output is a pure function of an input set the CPU can
// enumerate byte-for-byte. When every byte of that set equals the previous
// frame's — and has been equal for `settleFrames` CONSECUTIVE frames, so
// cross-frame GPU feedback loops (HZB prevVisible, SDSM readback smoothing)
// are provably stationary — the dispatch declarations can be skipped and the
// persistent outputs retained: re-running would reproduce them exactly.
//
// Discipline (CascadeShadowCache precedent): exact equality only — no hashes,
// no epsilons, no heuristics. Anything that could change the GPU result must
// be IN the blob (matrices, layout offsets, physical buffer identity bits,
// content epochs); anything deliberately excluded (TAA jitter via the
// unjittered logic-domain camera, GPUCullingData::frameIndex/deltaTime which
// no culling shader consumes) is excluded by documented argument at the
// callsite, never by omission. An evaluation gap (the callsite skipped a
// frame — its retained GPU ranges may have been overwritten by another
// callsite in between) or a forced invalidation always recomputes.
//
// Pure CPU state machine: no device, no RenderGraph — unit-testable.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Byte-exact record of one pass family's full input set for one frame.
class ElisionInputBlob
{
  public:
    void Clear() { m_Bytes.clear(); }

    template <typename T>
    void Append(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "ElisionInputBlob::Append requires a trivially copyable type; append "
                      "aggregate members field-by-field so struct padding never enters the blob");
        AppendBytes(&value, sizeof(T));
    }

    void AppendBytes(const void* data, size_t bytes)
    {
        if (bytes == 0)
            return;
        const size_t off = m_Bytes.size();
        m_Bytes.resize(off + bytes);
        std::memcpy(m_Bytes.data() + off, data, bytes);
    }

    bool operator==(const ElisionInputBlob& other) const
    {
        return m_Bytes.size() == other.m_Bytes.size() &&
               (m_Bytes.empty() ||
                std::memcmp(m_Bytes.data(), other.m_Bytes.data(), m_Bytes.size()) == 0);
    }
    bool operator!=(const ElisionInputBlob& other) const { return !(*this == other); }

    size_t SizeBytes() const { return m_Bytes.size(); }

  private:
    std::vector<uint8_t> m_Bytes;
};

// Why a family recomputed (or skipped) this frame. Instrumentation only;
// ordered by Evaluate's check priority.
enum class ElisionCause : uint8_t
{
    Skipped = 0,   // inputs byte-identical and settled — dispatches elided
    FirstEvaluate, // no prior record for this gate
    Forced,        // caller invalidated (device rebuild, pending growth, reset backlog)
    EvaluationGap, // gate not evaluated last frame — retained GPU ranges untrusted
    InputsChanged, // some input byte differs from last frame
    NotSettled,    // inputs match but not for settleFrames consecutive frames yet
    CauseCount,
};

const char* ElisionCauseName(ElisionCause cause);

// Why a gate is not eliding, for the disengage logs. `Skipped` is resolved
// independently of whether elision was allowed, so a decision can carry cause
// `Skipped` with Skip false — the inputs settled but the family was disabled
// this frame (kill switch, or a caller-side hold such as the LOD crossfade
// liveness rule). "disengaged: Skipped" reads as its own opposite; name the
// disabled state instead.
const char* ElisionDisengageReason(ElisionCause cause, bool skip);

// Parse the GE_IDLE_ELISION_LOG toggle. Leading-character idiom shared with
// ParseGpuCheckpointsEnabled / ParseSyncValidationEnabled, and OFF for the same
// reason — but note the polarity is the OPPOSITE of the neighbouring
// GE_IDLE_ELISION kill switches, which default ON: an unset or falsey value
// here means "do not log", so "false" must read as off rather than as any
// non-"0" string.
inline bool ParseIdleElisionLoggingEnabled(const char* envValue) noexcept
{
    return envValue != nullptr && envValue[0] != '\0' && envValue[0] != '0' &&
           envValue[0] != 'f' && envValue[0] != 'F';
}

// Opt-in for every idle-elision diagnostic line — the engaged/disengaged
// transition edges, the periodic cause-window summaries, and the [CascadeCache]
// window twin — read once (house latch idiom). Default OFF: all of it is an
// elision-investigation instrument, and a session not investigating elision
// otherwise pays two lines per family, view and settle for it.
// Two callsite orders, each for its own reason:
//  - Cause windows test this AFTER their own report gate, never instead of it:
//    ShouldReportWindow owns the window baseline and follows the consume-once
//    contract documented below, so it must be consumed whether or not the line
//    is emitted.
//  - Transition edges test this FIRST, so a callsite's edge latch tracks only
//    what was logged. Those latches feed nothing but these lines.
bool IdleElisionLoggingEnabled();

// Per-callsite skip decision over consecutive exact-equal input frames.
class RecomputeElisionGate
{
  public:
    struct Decision
    {
        bool Skip = false;
        ElisionCause Cause = ElisionCause::FirstEvaluate;
    };

    // Evaluate this frame's inputs against the previous frame's. `frameStamp`
    // is a monotonic per-frame index for THIS gate's callsite; consecutive
    // matching is only trusted when stamps advance by exactly one (a gap means
    // the callsite's retained arena/GPU regions may have been rewritten by
    // other callsites while it was absent). With `enabled` false the decision
    // is never Skip but the would-be cause is still counted, so a disabled run
    // measures the achievable skip rate. `settleFrames` is the number of
    // consecutive matches required BEFORE the first skip (>= 1); each unit
    // buys one extra executed frame so GPU feedback consumed one-frame-stale
    // (prevVisible, SDSM bounds) is stationary by induction when the skip
    // finally engages.
    Decision Evaluate(uint64_t frameStamp, ElisionInputBlob&& inputs, bool enabled,
                      uint32_t settleFrames);

    // Next Evaluate recomputes regardless of input equality and restarts the
    // settle count. For invalidations the blob cannot see (device rebuild
    // freed every physical without changing recorded handle bits).
    void ForceRecompute() { m_Forced = true; }

    struct Stats
    {
        uint64_t Evaluated = 0;
        uint64_t Skipped = 0;
        std::array<uint64_t, static_cast<size_t>(ElisionCause::CauseCount)> CauseCounts{};
    };
    const Stats& GetStats() const { return m_Stats; }
    ElisionCause LastCause() const { return m_LastCause; }
    uint32_t ConsecutiveMatches() const { return m_ConsecutiveMatches; }

    // Gate for the periodic cause-window logs: true once every `period`
    // evaluations, and only when that window contained an evaluation this gate
    // did NOT skip. The window exists to name the blocker of a run that never
    // engages, so a gate skipping everything it evaluates has nothing to say and
    // reprinting its identical totals is noise; a gate losing elision — or one
    // measuring with elision disabled, whose Skipped never advances — still
    // reports every window. Consuming advances the window baseline: call it once
    // per evaluation, from one callsite.
    bool ShouldReportWindow(uint64_t period);

  private:
    ElisionInputBlob m_Last;
    uint64_t m_LastStamp = 0;
    uint32_t m_ConsecutiveMatches = 0;
    bool m_HasLast = false;
    bool m_Forced = false;
    ElisionCause m_LastCause = ElisionCause::FirstEvaluate;
    Stats m_Stats;
    uint64_t m_WindowBaseEvaluated = 0;
    uint64_t m_WindowBaseSkipped = 0;
};

// Engine-injected per-frame epochs for the module-side gates. The engine layer
// owns the scene-content signals (extraction lane verdict, light-list version,
// depth-dynamic contributors); the modules own their local inputs (matrices,
// layouts, physical identities). A gate's blob appends BOTH.
struct ElisionFrameContext
{
    bool AllowElision = false;      // master && family kill-switch, resolved by the engine
    uint64_t ContentEpoch = 0;      // renderable content version (movers, structural,
                                    // material/vertex-mod). Does NOT cover skeletal/morph
                                    // animation playback: palette content never reaches the
                                    // extraction lane — that is DepthDynamicEpoch's palette term
    uint64_t LightEpoch = 0;        // world light-list version (FinalizeWorldLights memcmp)
    uint64_t DepthDynamicEpoch = 0; // advances on frames whose rasterized depth can change
                                    // outside the instance path: active depth-writing raster
                                    // contributors (terrain/ocean/grass) and skinned palette
                                    // content deltas (playing/scrubbed animation)
    uint64_t VisibilityWriteEpoch = 0; // GPUCulling visibility-write generation (scatter gates)
    uint64_t GpuSceneEpoch = 0;        // GPUScene::GetContentEpoch for gates without a scene ptr
};

} // namespace Rendering
} // namespace GameEngine

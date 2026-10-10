#pragma once

#include "Rendering/Core/QueryPool.h"

#include <Metal/Metal.hpp>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class MetalCommandList;

// Timestamp-only IQueryPool backed by MTLCounterSampleBuffer stage-boundary
// sampling. Apple GPUs support atStageBoundary ONLY — atDraw/atDispatch/
// atBlit/atTileDispatch all report unsupported (logged at Initialize) — so a
// timestamp cannot be placed mid-encoder the way vkCmdWriteTimestamp can, and
// the finest unit this pool can time is a whole command encoder.
//
// Each encoder created while profiling is active attaches boundary samples,
// and a span's two ends bind to that encoder's matching boundary: SpanBegin to
// the start of the encoder carrying the bracketed work (deferred until that
// encoder opens, because which encoder that is cannot be known when the begin
// is recorded), SpanEnd to its end. When no encoder opens inside the span at
// all, the work went into one that was already open and the begin binds to
// THAT encoder's start instead — never when a new encoder opened, which always
// claims the begin first. A span's cost is then the SUM of its
// encoders' own spans (GetTimestampSpanTicks), not the extent from the first to
// the last: the GPU can idle for milliseconds between two encoders of one pass,
// and a render encoder's vertex and fragment stages are scheduled separately,
// so all four render stage boundaries are sampled and both gaps stay out.
//
// What remains, reported as TimestampSemantics::EncoderSpan, must not be summed
// across passes:
//   · pass -> encoder is many-to-many. Several render-graph passes recorded
//     into one encoder all read that one encoder's span (GetTimestampUnitId
//     makes the sharing visible), and a pass whose work landed in an encoder
//     another pass opened is charged that encoder in full.
//   · the GPU runs encoders concurrently and out of order, so neighbouring
//     spans overlap; a span is an upper bound on the work's cost.
//
// Occlusion queries are backed by a per-frame MTLVisibilityResultBuffer that
// every render pass attaches (MetalCommandList::BeginRenderPass);
// BeginOcclusionQuery brackets draws with setVisibilityResultMode(Counting).
// Pipeline-statistics queries remain unsupported (UINT32_MAX) — no Metal equivalent.
class MetalQueryPool : public IQueryPool
{
  public:
    MetalQueryPool(MTL::Device* device, const Config& config = {});
    ~MetalQueryPool() override;

    bool Initialize() override;
    void Shutdown() override;

    void BeginFrame(uint32_t frameIndex) override;
    void InvalidateCachedTimestampResults(uint32_t frameIndex) override;
    void EndFrame() override;

    uint32_t WriteTimestamp(CommandList* commandList, TimestampPoint point,
                            const std::string& name = "") override;
    TimestampSemantics GetTimestampSemantics() const override
    {
        return TimestampSemantics::EncoderSpan;
    }
    uint32_t GetTimestampUnitId(uint32_t queryIndex) const override;
    bool GetTimestampSpanTicks(uint32_t beginQuery, uint32_t endQuery,
                               uint64_t& outTicks) const override;
    uint32_t BeginOcclusionQuery(CommandList* commandList, const std::string& name = "") override;
    void EndOcclusionQuery(CommandList* commandList, uint32_t queryIndex) override;
    uint32_t BeginPipelineStats(CommandList* commandList, const std::string& name = "") override;
    void EndPipelineStats(CommandList* commandList, uint32_t queryIndex) override;

    bool GetTimestampResult(uint32_t queryIndex, QueryResult& outResult) override;
    bool GetOcclusionResult(uint32_t queryIndex, QueryResult& outResult) override;
    bool GetPipelineStatsResult(uint32_t queryIndex, QueryResult& outResult) override;

    const ProfilingStats& GetProfilingStats() const override;
    double GetTimestampPeriod() const override;
    double TimestampToMs(uint64_t timestamp) const override;
    std::vector<QueryResult> GetAllTimestampResults() const override;
    void ResetQueries(CommandList* commandList) override;
    bool IsValid() const override;
    void DebugPrintResults() const override;

    // --- Backend internals (called by MetalCommandList) ---

    // True once any timestamp has been written this frame slot — encoders
    // attach boundary samples only while profiling is actually running.
    bool ShouldAttachSamples() const;
    MTL::CounterSampleBuffer* GetCurrentSampleBuffer() const;
    // Boundary sample slots of one encoder. Start/End are its outer boundaries
    // (what a span's two ends bind to). A render encoder additionally splits
    // vertex from fragment, because on a TBDR GPU those two stages are
    // scheduled separately and the gap between them is not the pass's cost.
    struct EncoderSampleSlots
    {
        uint32_t Start = UINT32_MAX;            // start of the first stage
        uint32_t End = UINT32_MAX;              // end of the last stage
        uint32_t FirstStageEnd = UINT32_MAX;    // render only: end of vertex
        uint32_t SecondStageBegin = UINT32_MAX; // render only: start of fragment
        bool SplitStages() const { return FirstStageEnd != UINT32_MAX; }
    };
    // Claims one encoder's boundary sample slots in the current frame slice,
    // tagged with that encoder's unit id. False when the slice is full (the
    // encoder is then created without samples).
    bool AllocateEncoderSampleSlots(bool splitStages, EncoderSampleSlots& outSlots);
    // Points a query returned by WriteTimestamp at a physical sample slot.
    void BindQueryToSample(uint32_t queryIndex, uint32_t sampleSlot);

    // The current frame's visibility-result buffer for render-pass attachment.
    // Returns null (zero-touch) unless occlusion querying is enabled — the
    // default-off path leaves the render hot path unchanged. A consumer that
    // wants occlusion queries must enable it BEFORE the render passes it queries
    // (the buffer is bound at encoder creation in BeginRenderPass).
    MTL::Buffer* GetCurrentVisibilityBuffer() const;
    void SetOcclusionEnabled(bool enabled) { m_OcclusionEnabled.store(enabled, std::memory_order_relaxed); }
    bool IsOcclusionEnabled() const { return m_OcclusionEnabled.load(std::memory_order_relaxed); }

  private:
    struct FrameSlice
    {
        MTL::CounterSampleBuffer* sampleBuffer = nullptr;
        std::vector<uint32_t> querySample; // query idx -> physical sample slot
        // Sample slot -> id of the encoder whose boundary it samples. Two
        // queries on slots of the same encoder are one measurement.
        std::vector<uint32_t> sampleUnit;
        std::vector<std::string> queryNames;
        uint32_t queryCount = 0;
        uint32_t sampleCount = 0;
        uint32_t encoderCount = 0;
        // GPU tick this slice was (re)opened at. Counter sample buffers cannot
        // be cleared from the host, so a slot the GPU never wrote this cycle
        // still holds an older cycle's tick; anything below this is stale and
        // must not be read as a result.
        uint64_t openTick = 0;
        // Resolved GPU ticks per QUERY index (dereferenced through querySample
        // at resolve time so slot reuse can overwrite the mapping freely).
        std::vector<uint64_t> cachedValues;
        // Encoder unit id and physical sample slot per QUERY index, captured at
        // resolve because the slot mapping is reset before consumers read the
        // results. Two queries on the same slot are one boundary (zero span).
        std::vector<uint32_t> cachedUnits;
        std::vector<uint32_t> cachedSlots;
        // Boundary slots and resolved busy ticks per encoder unit. A unit whose
        // boundaries did not resolve stays 0 rather than being guessed at.
        std::vector<EncoderSampleSlots> unitSlots;
        std::vector<uint64_t> cachedUnitTicks;
        bool cacheValid = false;

        // Occlusion: a Shared MTLBuffer of MaxOcclusionQueries * uint64 slots.
        // The GPU writes a fragment count per slot in Counting mode; resolved
        // into occlusionValues after the slot's frame completes.
        MTL::Buffer* visibilityBuffer = nullptr;
        uint32_t occlusionCount = 0;
        std::vector<uint64_t> occlusionValues;
        bool occlusionCacheValid = false;
    };

    void ResolveSlice(uint32_t frameIndex);
    void UpdateTimestampCorrelation();

    MTL::Device* m_Device = nullptr;
    Config m_Config;
    std::vector<FrameSlice> m_Frames;
    uint32_t m_CurrentFrame = 0;

    // GPU-tick -> nanosecond conversion from paired CPU/GPU samples.
    uint64_t m_BaseCpuTicks = 0;
    uint64_t m_BaseGpuTicks = 0;
    uint64_t m_LastSampledGpuTick = 0;
    double m_GpuPeriodNs = 1.0;

    ProfilingStats m_Stats{};
    mutable std::recursive_mutex m_Mutex;
    bool m_Valid = false;
    // Opt-in: off by default so the render path never attaches the visibility
    // buffer unless a consumer (e.g. GPU occlusion culling) turns it on.
    std::atomic<bool> m_OcclusionEnabled{false};
};

} // namespace Rendering
} // namespace GameEngine

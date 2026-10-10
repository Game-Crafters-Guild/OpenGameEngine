#pragma once

#include "Rendering/Core/QueryPool.h"

#include <webgpu/webgpu.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine::Rendering
{

class WebGpuDevice;

// GPU timestamps on WebGPU.
//
// Two constraints shape this against the Vulkan pool:
//
//  * Timestamps may only be written on the BARE ENCODER (native wgpu) or at
//    pass boundaries via timestampWrites (the browser, which has no encoder
//    write at all). The render graph brackets each pass — WriteTimestamp
//    before BeginRenderPass, again after EndRenderPass. Natively that is the
//    bare-encoder write, and one arriving while a pass is open is dropped.
//    In the browser WebGpuCommandList carries the span's begin onto the next
//    pass's beginningOfPassWriteIndex and aliases its end onto the
//    endOfPassWriteIndex of the last pass recorded before the span closed.
//  * Results come back through a mapped buffer, and mapping is asynchronous.
//    A frame's resolve target is read on a LATER frame (the slot's own next
//    turn), never waited on: pumping for a map mid-frame is what kills the
//    canvas texture on this backend.
//
// Occlusion and pipeline-statistics queries stay unimplemented: the graph asks
// only for timestamps, and WebGPU exposes no pipeline-statistics query at all.
class WebGpuQueryPool final : public IQueryPool
{
  public:
    WebGpuQueryPool(WebGpuDevice& device, WGPUDevice raw, const Config& config);
    ~WebGpuQueryPool() override;

    bool Initialize() override;
    void Shutdown() override;

    void BeginFrame(uint32_t frameIndex) override;
    void InvalidateCachedTimestampResults(uint32_t frameIndex) override;
    void EndFrame() override;

    // wgpuCommandEncoderWriteTimestamp is a per-command pipeline point (like vkCmdWriteTimestamp),
    // so a span's two ends need no different treatment and TimestampPoint is unused here.
    uint32_t WriteTimestamp(CommandList* commandList, TimestampPoint point,
                            const std::string& name = "") override;
    TimestampSemantics GetTimestampSemantics() const override
    {
        return TimestampSemantics::PipelinePoint;
    }
    uint32_t GetTimestampUnitId(uint32_t) const override { return kInvalidTimestampUnit; }
    bool GetTimestampSpanTicks(uint32_t, uint32_t, uint64_t&) const override { return false; }

    // WebGPU has occlusion queries only as a render-pass attachment and no
    // pipeline-statistics query; nothing in the engine asks this backend for
    // either, so they refuse rather than pretend.
    uint32_t BeginOcclusionQuery(CommandList*, const std::string& = "") override { return kInvalidQuery; }
    void EndOcclusionQuery(CommandList*, uint32_t) override {}
    uint32_t BeginPipelineStats(CommandList*, const std::string& = "") override { return kInvalidQuery; }
    void EndPipelineStats(CommandList*, uint32_t) override {}
    bool GetOcclusionResult(uint32_t, QueryResult&) override { return false; }
    bool GetPipelineStatsResult(uint32_t, QueryResult&) override { return false; }

    bool GetTimestampResult(uint32_t queryIndex, QueryResult& outResult) override;

    // Pass-boundary writes for the browser, where the encoder-level write does
    // not exist. WebGpuCommandList reserves one query per pass end and reports
    // which query a scope's begin or end actually landed on.
    WGPUQuerySet QuerySet() const { return m_QuerySet; }
    // A query slot for a pass boundary; kInvalidQuery when the frame's slot is
    // full. Local to the current frame slot, like WriteTimestamp's return.
    uint32_t ReserveQuery();
    // The query-set index a local query resolves to.
    uint32_t AbsoluteIndex(uint32_t local) const;
    // `from` was never written; it reads as `to`. Scopes that share a pass
    // boundary alias onto the one query that boundary wrote.
    void AliasQuery(uint32_t from, uint32_t to);
    const ProfilingStats& GetProfilingStats() const override { return m_Stats; }
    // WebGPU resolves timestamps to NANOSECONDS, so the period is 1 and the
    // browser's own quantisation (coarser than the hardware's) is what limits
    // resolution — not this number.
    double GetTimestampPeriod() const override { return 1.0; }
    double TimestampToMs(uint64_t timestamp) const override
    {
        return static_cast<double>(timestamp) * 1e-6;
    }
    std::vector<QueryResult> GetAllTimestampResults() const override;
    void ResetQueries(CommandList*) override {}
    bool IsValid() const override { return m_QuerySet != nullptr; }
    void DebugPrintResults() const override;

    static constexpr uint32_t kInvalidQuery = 0xFFFFFFFFu;

  private:
    // One slot per frame in flight: a slot's resolve buffer is only mapped once
    // its frame has retired, so slots never contend.
    struct FrameSlot
    {
        WGPUBuffer Resolve = nullptr;  // COPY_DST | QUERY_RESOLVE
        WGPUBuffer Readback = nullptr; // COPY_DST | MAP_READ
        uint32_t FirstQuery = 0;
        uint32_t Count = 0;
        // Queries the in-flight resolve covers; Count restarts when the slot
        // re-arms while that readback may still be on its way.
        uint32_t ReadbackCount = 0;
        bool MapPending = false;
        bool HaveResults = false;
        std::vector<uint64_t> Values;
        // Per turn, read together with Values: the graph resolves the retired
        // turn AFTER the slot re-arms for its next one, and only then clears
        // them (InvalidateCachedTimestampResults).
        std::unordered_map<uint32_t, std::string> Names;
        std::unordered_map<uint32_t, uint32_t> Alias; // local -> local
    };

    void ReadbackSlot(FrameSlot& slot);
    static void OnMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1,
                         void* userdata2);

    WebGpuDevice& m_Device;
    WGPUDevice m_Raw = nullptr;
    Config m_Config{};
    WGPUQuerySet m_QuerySet = nullptr;

    std::vector<FrameSlot> m_Slots;
    uint32_t m_FrameIndex = 0;
    uint32_t m_SlotCursor = 0;   // next free query within the current slot
    uint32_t m_QueriesPerSlot = 0;

    ProfilingStats m_Stats{};
    bool m_LoggedFirstReadback = false;
    bool m_LoggedFirstRead = false;
};

} // namespace GameEngine::Rendering

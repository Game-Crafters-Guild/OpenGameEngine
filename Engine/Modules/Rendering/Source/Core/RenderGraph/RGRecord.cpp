#include "Logger/Logger.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

// Engine stage/access mask bits. (Header drags the old RenderGraph.h include
// chain — pruned when the old graph is deleted at Stage 2c.)
#include "Rendering/Core/BarrierMapping.h"
#include "Rendering/Core/QueryPool.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef GE_ENABLE_GPU_PROFILING
#define GE_ENABLE_GPU_PROFILING 1
#endif

namespace GameEngine::Rendering::RenderGraph
{

namespace
{

constexpr uint64_t Bit(PipelineStageMask m) { return static_cast<uint64_t>(m); }
constexpr uint64_t Bit(ResourceAccessMask m) { return static_cast<uint64_t>(m); }

// Diagnostic marker names are copied into fixed-size records by the backend
// (the GPU-checkpoint name table interns a truncated copy and keys on it), so a
// name at or past the cap loses its tail. Build "<name>.end" by truncating the
// BASE first and appending the suffix into the reserved space: appending and
// then truncating would give a long pass the same bytes as its own start
// marker, and the two would intern to one record — the end marker would alias
// the very thing it exists to distinguish.
constexpr size_t kPassMarkerNameCapacity = 64;
constexpr char kPassEndMarkerSuffix[] = ".end";

void BuildPassEndMarkerName(const char* name, char (&out)[kPassMarkerNameCapacity])
{
    constexpr size_t kSuffixLength = sizeof(kPassEndMarkerSuffix) - 1;
    constexpr size_t kMaxBaseLength = kPassMarkerNameCapacity - 1 - kSuffixLength;
    size_t baseLength = 0;
    while (baseLength < kMaxBaseLength && name[baseLength] != '\0')
    {
        out[baseLength] = name[baseLength];
        ++baseLength;
    }
    std::memcpy(out + baseLength, kPassEndMarkerSuffix, kSuffixLength + 1);
}

// "Everything": the engine has no single all-commands bit; the union is the
// faithful translation of an execution dependency on all prior work.
const uint64_t kAllEngineStages =
    Bit(PipelineStageMask::GraphicsColor) | Bit(PipelineStageMask::GraphicsDepth) |
    Bit(PipelineStageMask::GraphicsVertex) | Bit(PipelineStageMask::GraphicsFragment) |
    Bit(PipelineStageMask::ComputeShader) | Bit(PipelineStageMask::Transfer) |
    Bit(PipelineStageMask::DrawIndirect);

uint64_t ToEngineStageMask(uint32_t s)
{
    if (s & RGStage::BottomOfPipe)
        return kAllEngineStages;
    uint64_t m = 0;
    if (s & RGStage::DrawIndirect)
        m |= Bit(PipelineStageMask::DrawIndirect);
    if (s & (RGStage::VertexInput | RGStage::VertexShader))
        m |= Bit(PipelineStageMask::GraphicsVertex);
    if (s & RGStage::FragmentShader)
        m |= Bit(PipelineStageMask::GraphicsFragment);
    if (s & (RGStage::EarlyFragmentTests | RGStage::LateFragmentTests))
        m |= Bit(PipelineStageMask::GraphicsDepth);
    if (s & RGStage::ColorAttachmentOutput)
        m |= Bit(PipelineStageMask::GraphicsColor);
    if (s & RGStage::ComputeShader)
        m |= Bit(PipelineStageMask::ComputeShader);
    if (s & RGStage::Transfer)
        m |= Bit(PipelineStageMask::Transfer);
    return m; // TopOfPipe-only → 0 → backend derives from state (Undefined → TOP)
}

uint64_t ToEngineAccessMask(uint32_t a)
{
    uint64_t m = 0;
    if (a & RGAccessMask::IndirectRead)
        m |= Bit(ResourceAccessMask::IndirectCommandRead);
    // No dedicated INDEX_READ bit in the engine mask; VertexAttributeRead is the
    // closest read scope on the same stage.
    if (a & (RGAccessMask::IndexRead | RGAccessMask::VertexAttribRead))
        m |= Bit(ResourceAccessMask::VertexAttributeRead);
    if (a & (RGAccessMask::UniformRead | RGAccessMask::ShaderRead))
        m |= Bit(ResourceAccessMask::ShaderRead);
    if (a & RGAccessMask::ShaderWrite)
        m |= Bit(ResourceAccessMask::ShaderWrite);
    if (a & RGAccessMask::ColorWrite)
        m |= Bit(ResourceAccessMask::ColorAttachmentWrite);
    if (a & RGAccessMask::ColorRead)
        m |= Bit(ResourceAccessMask::ColorAttachmentRead);
    if (a & RGAccessMask::DepthRead)
        m |= Bit(ResourceAccessMask::DepthStencilRead);
    if (a & RGAccessMask::DepthWrite)
        m |= Bit(ResourceAccessMask::DepthStencilWrite);
    if (a & RGAccessMask::TransferRead)
        m |= Bit(ResourceAccessMask::TransferRead);
    if (a & RGAccessMask::TransferWrite)
        m |= Bit(ResourceAccessMask::TransferWrite);
    return m;
}

// Representative engine state for buffer barriers (fallback path only — the
// explicit masks above carry the precise scopes).
ResourceState BufferStateFromAccess(uint32_t a)
{
    if (a & RGAccessMask::IndirectRead)
        return ResourceState::IndirectArgs;
    if (a & RGAccessMask::UniformRead)
        return ResourceState::ConstantBuffer;
    if (a & RGAccessMask::IndexRead)
        return ResourceState::IndexBuffer;
    if (a & RGAccessMask::VertexAttribRead)
        return ResourceState::VertexBuffer;
    if (a & RGAccessMask::TransferRead)
        return ResourceState::CopySource;
    if (a & RGAccessMask::TransferWrite)
        return ResourceState::CopyDest;
    if (a & RGAccessMask::ShaderWrite)
        return ResourceState::UnorderedAccess;
    if (a & RGAccessMask::ShaderRead)
        return ResourceState::ShaderResource;
    return ResourceState::Common;
}

// Physical queue ids share the IDevice::QueueType encoding (same order by
// contract): the identity map {0,1,2} = graphics/compute/transfer, and an
// aliased map points logical queues at the family that actually executes them.
IDevice::QueueType ToQueueType(uint8_t physicalQueue)
{
    return static_cast<IDevice::QueueType>(physicalQueue);
}

RenderPassDesc::LoadOp ToLoadOp(RGLoadOp op)
{
    switch (op)
    {
        case RGLoadOp::Load: return RenderPassDesc::LoadOp::Load;
        case RGLoadOp::Clear: return RenderPassDesc::LoadOp::Clear;
        case RGLoadOp::DontCare: return RenderPassDesc::LoadOp::DontCare;
    }
    return RenderPassDesc::LoadOp::Load;
}

RenderPassDesc::StoreOp ToStoreOp(RGStoreOp op)
{
    switch (op)
    {
        case RGStoreOp::DontCare: return RenderPassDesc::StoreOp::DontCare;
        case RGStoreOp::None: return RenderPassDesc::StoreOp::None;
        case RGStoreOp::Store: break;
    }
    return RenderPassDesc::StoreOp::Store;
}

} // namespace

void CrossQueueConsumerSrcScope(RGQueue consumerQueue, uint64_t& outStageMask,
                                uint64_t& outAccessMask)
{
    switch (consumerQueue)
    {
        case RGQueue::Compute:
            outStageMask = Bit(PipelineStageMask::ComputeShader);
            outAccessMask = Bit(ResourceAccessMask::ShaderRead);
            return;
        case RGQueue::Transfer:
            // Shader stages/accesses are invalid on a pure transfer queue.
            outStageMask = Bit(PipelineStageMask::Transfer);
            outAccessMask = Bit(ResourceAccessMask::TransferRead);
            return;
        case RGQueue::Graphics:
            break;
    }
    outStageMask = Bit(PipelineStageMask::GraphicsFragment);
    outAccessMask = Bit(ResourceAccessMask::ShaderRead);
}

RGFrame::~RGFrame()
{
    // Drain own submitted work first: destroying a timeline the GPU still
    // signals is a device-loss class bug (floating-window close path). Graph-
    // scoped — never a device-wide WaitForIdle (MoltenVK multi-window).
    WaitForPendingWork();
    for (uint32_t q = 0; q < kQueueCount; ++q)
        if (m_TimelineCreated[q])
            m_Device->DestroySemaphore(m_QueueTimelines[q]);
}

void RGFrame::SetPhysicalQueueMap(const uint8_t physicalOfLogical[3])
{
    for (int i = 0; i < 3; ++i)
        m_PhysicalQueueOf[i] = physicalOfLogical[i];
}

CommandList* RGFrame::GetOrCreateCommandList(uint8_t physicalQueue)
{
    // Stale cached lists from before an in-place device rebuild are dropped in
    // BeginFrame (which also clears the persistent pool) — see m_DeviceRebuildGen.
    //
    // Keyed by PHYSICAL queue: a submission whose passes were declared on an
    // aliased logical queue must record into (and submit on) the family that
    // actually executes it — a render pass recorded into a compute-family
    // command buffer is invalid regardless of the submission plan.
    auto& cl = m_CmdLists[physicalQueue];
    if (!cl)
        cl = m_Device->CreateCommandList(ToQueueType(physicalQueue));
    return cl.get();
}

void RGFrame::EnsureTimeline(uint8_t physicalQueue)
{
    if (!m_TimelineCreated[physicalQueue])
    {
        m_QueueTimelines[physicalQueue] = m_Device->CreateTimelineSemaphore(0);
        m_TimelineCreated[physicalQueue] = true;
    }
}

void RGFrame::BuildRenderPassDesc(uint32_t attFirst, uint32_t attCount, RenderPassDesc& out) const
{
    out = RenderPassDesc{}; // start from defaults, then set every used slot explicitly
    uint32_t colorCount = 0;
    for (uint32_t i = attFirst; i < attFirst + attCount; ++i)
    {
        const RGAttachmentRec& rec = m_Attachments[i];
        // Structural guard for the Load-consumes-content rule: any future
        // attachment helper that records a Load without the derived read
        // would silently cull producers and lose write visibility.
        assert((rec.Ops.Load != RGLoadOp::Load || m_Graph.HasReadAccess(rec.Pass, rec.Tex)) &&
               "RenderGraph: LoadOp::Load attachment without a declared read on the resource");
        const TextureHandle handle(PhysicalOf(rec.Tex));

        // A sub-range attach (the per-layer cascade contract: one cascade =
        // one array layer) must reach the BACKEND as a custom attachment
        // view — barrier generation already tracks per-cell, but without
        // this the device builds a default whole-resource view and every
        // cascade renders into layer 0.
        const RGResourceDesc& rd = m_Graph.ResourceDesc(rec.Tex);
        const uint32_t resLayers = rd.ArrayLayers > 0 ? rd.ArrayLayers : 1;
        const uint32_t resMips = rd.MipLevels > 0 ? rd.MipLevels : 1;
        const uint32_t baseLayer = rec.Range.BaseLayer;
        const uint32_t layerCount = rec.Range.LayerCount == kRemaining
                                        ? (resLayers - std::min(baseLayer, resLayers))
                                        : rec.Range.LayerCount;
        const uint32_t baseMip = rec.Range.BaseMip;
        const uint32_t mipCount = rec.Range.MipCount == kRemaining
                                      ? (resMips - std::min(baseMip, resMips))
                                      : rec.Range.MipCount;
        const bool subRange = baseLayer != 0 || layerCount != resLayers || baseMip != 0 ||
                              mipCount != resMips;
        TextureViewDesc viewDesc{};
        if (subRange)
        {
            viewDesc.viewType =
                layerCount <= 1 ? TextureViewType::View2D : TextureViewType::View2DArray;
            viewDesc.aspect = rec.IsDepth ? TextureAspect::Depth : TextureAspect::Color;
            viewDesc.baseMip = baseMip;
            viewDesc.levelCount = mipCount;
            viewDesc.baseLayer = baseLayer;
            viewDesc.layerCount = layerCount;
        }

        if (rec.IsDepth)
        {
            out.depthTarget = handle;
            if (rec.Resolve != kInvalidId)
                out.resolveDepthTarget = TextureHandle(PhysicalOf(rec.Resolve));
            out.depthReadOnly = rec.ReadOnly;
            out.depthLoadOp = ToLoadOp(rec.Ops.Load);
            out.depthStoreOp = ToStoreOp(rec.Ops.Store);
            out.clearDepth = rec.Ops.Load == RGLoadOp::Clear;
            out.clearDepthValue = rec.Ops.Clear.Depth; // reverse-Z: 0.0 = far
            out.clearStencil = false;
            out.stencilLoadOp = RenderPassDesc::LoadOp::DontCare;
            // A read-only attach writes neither aspect: DontCare on the stencil
            // of a combined-format target would be a discard AND an attachment
            // write outside the pass's declared access.
            out.stencilStoreOp = rec.ReadOnly ? RenderPassDesc::StoreOp::None
                                              : RenderPassDesc::StoreOp::DontCare;
            if (subRange)
            {
                out.useDepthView = true;
                out.depthViewDesc = viewDesc;
            }
        }
        else
        {
            const uint32_t s = rec.Slot;
            out.colorTargets[s] = handle;
            out.colorLoadOp[s] = ToLoadOp(rec.Ops.Load);
            out.colorStoreOp[s] = ToStoreOp(rec.Ops.Store);
            out.clearColor[s] = rec.Ops.Load == RGLoadOp::Clear;
            for (int c = 0; c < 4; ++c)
                out.clearColorValue[s][c] = rec.Ops.Clear.Color[c];
            if (rec.Resolve != kInvalidId)
                out.resolveColorTargets[s] = TextureHandle(PhysicalOf(rec.Resolve));
            if (subRange)
            {
                out.useColorView[s] = true;
                out.colorViewDesc[s] = viewDesc;
            }
            colorCount = colorCount > s + 1 ? colorCount : s + 1;
        }
    }
    out.colorTargetCount = colorCount;
#ifndef NDEBUG
    for (uint32_t s = 0; s < colorCount; ++s)
        assert(out.colorTargets[s].IsValid() && "RenderGraph: color attachment slots must be dense from 0");
#endif
}

void RGFrame::MarkRecordSecondary(RGPassId p)
{
    if (m_PassRecordSecondary.size() <= p)
        m_PassRecordSecondary.resize(p + 1, 0);
    m_PassRecordSecondary[p] = 1;
}

// A2.4-P0-R env kill switch — read once. `=0` (or unset) is the byte-identical
// serial fallback (§A2.4-D3): with it off, RecordAndSubmit forks nothing and the
// R2 loop records every pass interior inline, exactly as pre-fork.
static bool ParallelRecordEnabled()
{
    static const bool kEnabled = []() {
        const char* e = std::getenv("GE_PARALLEL_RECORD");
        return e != nullptr && e[0] != '0' && e[0] != '\0';
    }();
    return kEnabled;
}

uint32_t RGFrame::RecordSecondariesParallel()
{
    // Requires an injected fork primitive; empty ⇒ serial fallback. m_ScratchAttRange
    // is already built by the caller (attachment ranges per pass id).
    if (!m_RecordParallel)
        return 0;

    const size_t passCount = m_Graph.PassCount();
    m_Secondary.clear();
    m_Secondary.resize(passCount); // default-constructs null unique_ptrs (no copy)

    // Eligible = flagged RecordInSecondary AND has attachments (a secondary
    // inherits the primary's render pass; an attachment-less pass has none).
    std::vector<RGPassId> eligible;
    for (RGPassId p : m_Graph.ScheduledOrder())
    {
        if (!RecordsSecondaryPass(p))
            continue;
        if (p >= m_ScratchAttRange.size() || m_ScratchAttRange[p].second == 0)
            continue;
        eligible.push_back(p);
    }
    if (eligible.empty())
        return 0;

    // Fork each eligible pass's interior into its OWN secondary command list from
    // the worker's OWN per-thread pool (CreateSecondaryCommandList is thread-local).
    // Each worker writes a distinct m_Secondary[eligible[k]] slot (pre-sized), so
    // no slot races. m_RecordParallel joins before it returns.
    m_RecordParallel(static_cast<uint32_t>(eligible.size()), [this, &eligible](uint32_t k) {
        const RGPassId p = eligible[k];
        const auto attRange = m_ScratchAttRange[p];

        // Derive the secondary's inheritance formats the SAME way the pipeline
        // variants do (BuildCurrentFormatKey over this pass's attachments) —
        // guaranteed to match the primary's BeginRenderPass rendering formats and
        // the pipelines compiled for this pass.
        RGContext ctx;
        ctx.m_Frame = this;
        ctx.m_Pass = p;
        ctx.m_AttFirst = attRange.first;
        ctx.m_AttCount = attRange.second;
        ctx.m_KeyDirty = true;
        const PipelineFormatKey fk = ctx.BuildCurrentFormatKey();

        CommandList::SecondaryBeginInfo info{};
        constexpr uint32_t kMaxSecondaryColors =
            sizeof(info.colorFormats) / sizeof(info.colorFormats[0]);
        info.colorAttachmentCount =
            fk.ColorCount < kMaxSecondaryColors ? fk.ColorCount : kMaxSecondaryColors;
        for (uint32_t i = 0; i < info.colorAttachmentCount; ++i)
            info.colorFormats[i] = static_cast<uint32_t>(fk.ColorFormats[i]);
        info.depthFormat = static_cast<uint32_t>(fk.DepthFormat);
        info.sampleCount = fk.RasterizationSamples;
        // Depth read-only is NOT a format, so it does not travel in the variant
        // key: take it from the very RenderPassDesc phase R2 begins this pass
        // with, so the two cannot diverge. It has to be conveyed — a pass filled
        // by secondaries records no state on the primary, so the secondary is
        // the only place vkCmdSetDepthWriteEnable(VK_FALSE) is ever issued, and
        // a read-only attach's STORE_OP_NONE preserves the contents only while
        // nothing writes the attachment. BuildRenderPassDesc is const and reads
        // only per-pass declaration state — safe on a record worker.
        RenderPassDesc rp;
        BuildRenderPassDesc(attRange.first, attRange.second, rp);
        info.depthReadOnly = rp.depthReadOnly;

        std::unique_ptr<CommandList> sec =
            m_Device->CreateSecondaryCommandList(IDevice::QueueType::Graphics);
        if (!sec)
            return; // graceful fallback: null slot ⇒ R2 records this pass inline

        sec->BeginSecondary(info);
        ctx.Cmd = sec.get();
        m_Exec[p].Invoke(m_Exec[p].Fn, ctx);
        sec->End();
        m_Secondary[p] = std::move(sec);
    });

    uint32_t forked = 0;
    for (RGPassId p : eligible)
        if (m_Secondary[p])
            ++forked;
    return forked;
}

void RGFrame::RecordAndSubmit()
{
    const size_t passCount = m_Graph.PassCount();
    const auto& order = m_Graph.ScheduledOrder();

    // A2 STEP-0 record-floor brackets: QueueSubmit and vkCmd*Barrier RECORDING
    // (the barrier COMPUTE is timed separately by GenerateBarriersMs). Neither
    // lands in RGPassTiming::CpuMs — they are the serial floor A2.4 cannot
    // remove. Always-on; a handful of steady_clock pairs per frame.
    using RGClock = std::chrono::steady_clock;
    auto elapsedMs = [](RGClock::time_point begin) {
        return std::chrono::duration<double, std::milli>(RGClock::now() - begin).count();
    };

    // Per-pass attachment ranges (attachments are recorded contiguously per pass
    // during the AddPass setup lambda — single-threaded declaration).
    m_ScratchAttRange.assign(passCount, {0u, 0u});
    for (uint32_t i = 0; i < m_Attachments.size(); ++i)
    {
        auto& r = m_ScratchAttRange[m_Attachments[i].Pass];
        if (r.second == 0)
            r.first = i;
        ++r.second;
    }

    // ── Phase R1 (A2.4-P0-R): fork the RecordInSecondary passes' interiors onto
    // job-system workers into per-pass secondaries. Off ⇒ m_Secondary stays empty
    // and the R2 loop below records every interior inline (serial fallback). The
    // whole R1/R2 split is byte-identical to pre-fork when the env kill switch is
    // off or no pool is injected. Barriers / BeginRenderPass / submit stay serial.
    m_Secondary.clear();
    if (ParallelRecordEnabled())
        RecordSecondariesParallel();

    // Barrier-batch lookup by scheduled index. The export batch (sentinel
    // ScheduledIndex == pass count) owns no scheduled slot: like every other
    // placed barrier it records at the submission tail the plan derived.
    m_ScratchBatchOfSched.assign(order.size(), nullptr);
    for (const RGBarrierBatch& bb : m_Graph.BarrierBatches())
        if (bb.ScheduledIndex < order.size())
            m_ScratchBatchOfSched[bb.ScheduledIndex] = &bb;
    // Per-barrier derived placement, parallel to Barriers(): kInvalidId records
    // in schedule position, anything else names the submission at whose tail
    // the barrier records — see RGSubmission.cpp steps 2a (cross-physical
    // layout transitions) and 4 (export/normalize sentinels).
    const std::vector<uint32_t>& barrierSubOf = m_Graph.BarrierSubmissions();
    assert(barrierSubOf.size() == m_Graph.Barriers().size() &&
           "RenderGraph: barrier placement out of sync — BuildSubmissionPlan must run first");

    auto translate = [&](const RGBarrier& b, bool crossQueueConsumer, RGQueue consumerQueue,
                         ResourceBarrier& out)
    {
        out = ResourceBarrier{};
        out.type = b.IsTexture ? ResourceBarrier::Texture : ResourceBarrier::Buffer;
        if (b.IsTexture)
            out.textureHandle = TextureHandle(PhysicalOf(b.Resource));
        else
            out.bufferHandle = BufferHandle(PhysicalOf(b.Resource));
        // Depth-format textures sampled as ShaderReadOnly must transition to the
        // depth-read-only layout, not the color one: the descriptor writer binds
        // depth views in DEPTH_STENCIL_READ_ONLY_OPTIMAL (VulkanDevice
        // UpdateDescriptorSet), and a format-blind ShaderResource transition
        // parks the image in SHADER_READ_ONLY_OPTIMAL instead — every draw that
        // samples it then trips VUID-vkCmdDraw-None-09600. DepthSampled maps to
        // exactly the layout the descriptor claims. Applied at record time so
        // the barrier IR (and its unit tests) stays format-blind.
        auto textureState = [&](RGImageLayout layout) -> ResourceState
        {
            ResourceState s = ToResourceState(layout);
            if (s == ResourceState::ShaderResource)
            {
                TextureFormat fmt = static_cast<TextureFormat>(m_Graph.ResourceDesc(b.Resource).Format);
                // Imports may declare Unknown (resolved lazily at key build);
                // ask the device so external depth textures are not missed.
                if (fmt == TextureFormat::Unknown && m_Device)
                    fmt = m_Device->GetTextureFormat(TextureHandle(PhysicalOf(b.Resource)));
                if (IsDepthFormat(fmt))
                    s = ResourceState::DepthSampled;
            }
            return s;
        };
        out.stateBefore = b.IsTexture ? textureState(b.OldLayout) : BufferStateFromAccess(b.SrcAccess);
        out.stateAfter = b.IsTexture ? textureState(b.NewLayout) : BufferStateFromAccess(b.DstAccess);
        out.subresource = {b.Range.BaseMip, b.Range.MipCount, b.Range.BaseLayer, b.Range.LayerCount};
        out.rgResourceId = b.Resource;
        // The cell grid is the layout authority for graph-owned textures: the
        // backend must take oldLayout from the compiled stateBefore verbatim.
        out.rgAuthoritativeLayout = b.IsTexture;
        if (crossQueueConsumer)
        {
            // The producing queue's work is ordered by the submission plan's
            // timeline-semaphore wait (which also makes its writes available);
            // the recorded src scope must be valid on THIS queue (pinned rule).
            CrossQueueConsumerSrcScope(consumerQueue, out.srcStageMask, out.srcAccessMask);
        }
        else
        {
            out.srcStageMask = ToEngineStageMask(b.SrcStage);
            out.srcAccessMask = ToEngineAccessMask(b.SrcAccess);
        }
        out.dstStageMask = ToEngineStageMask(b.DstStage);
        out.dstAccessMask = ToEngineAccessMask(b.DstAccess);
    };

    RGContext ctx;
    ctx.m_Frame = this;

    // The cmd-fallback query reset is a frame-level responsibility RenderGraph owns
    // outright once the old graph deletes — the DEVICE writes a _FrameEnd
    // timestamp every presented frame regardless of profiling, and on devices
    // without hostQueryReset this is the only reset. Never gate it on the
    // profiling toggle; record it on the frame's FIRST submission whatever its
    // queue (vkCmdResetQueryPool is valid on compute queues). No-op on
    // hostQueryReset devices. Known inherited edge (same as the old graph): an
    // INDEPENDENT async submission with no timeline edge to the first one is
    // unordered against the reset on fallback devices.
    IQueryPool* resetPool = m_Device ? m_Device->GetQueryPool() : nullptr;
    if (resetPool && !resetPool->IsValid())
        resetPool = nullptr;
    // Profiling brackets gate separately. 2e item: per-queue-family
    // timestampValidBits gate (skip the bracket on unsupported families).
    IQueryPool* qp = m_ProfilingEnabled ? resetPool : nullptr;
    m_ProfilingCurrent.clear();
    bool queryResetDone = false;

    const std::vector<RGSubmission>& submissions = m_Graph.Submissions();
    for (uint32_t subIndex = 0; subIndex < submissions.size(); ++subIndex)
    {
        const RGSubmission& sub = submissions[subIndex];
        CommandList* cl = GetOrCreateCommandList(sub.PhysicalQueue);
        cl->Begin();

        if (resetPool && !queryResetDone)
        {
            resetPool->ResetQueries(cl); // outside any render pass, always
            queryResetDone = true;
        }

        // §4 lever — submission-front hoist: a FIRST-TOUCH barrier (first-use
        // Undefined init, imported first-touch sync) has no intra-frame
        // producer NOR consumer before it, so all of them batch into ONE call
        // here instead of one batch per producing pass. The flag is generation
        // ground truth — a BottomOfPipe source bit alone is NOT sufficient (a
        // writer after pure reads of an import inherits BottomOfPipe in its
        // source scope yet must stay ordered AFTER those readers).
        m_ScratchHoist.clear();
        for (uint32_t si = sub.ScheduledFirst; si < sub.ScheduledFirst + sub.ScheduledCount; ++si)
        {
            const RGBarrierBatch* bb = m_ScratchBatchOfSched[si];
            if (!bb)
                continue;
            for (uint32_t k = 0; k < bb->Count; ++k)
            {
                const RGBarrier& b = m_Graph.Barriers()[bb->First + k];
                if (!b.FirstTouch)
                    continue;
                // Same physical-property rule as the per-pass path below: a
                // hoisted barrier recorded on a non-source physical queue must
                // have its source scope sanitized to consumer-queue-valid
                // stages (the timeline wait provides the real ordering).
                const bool crossPhysical =
                    b.SrcQueue < kQueueCount &&
                    m_PhysicalQueueOf[b.SrcQueue] != sub.PhysicalQueue;
                ResourceBarrier eb;
                translate(b, crossPhysical, sub.Queue, eb);
                m_ScratchHoist.push_back(eb);
            }
        }
        if (!m_ScratchHoist.empty())
        {
            const auto emitBegin = RGClock::now();
            cl->BarrierBatch(m_ScratchHoist);
            m_Stats.BarrierEmitMs += elapsedMs(emitBegin);
            ++m_Stats.BarrierBatchesEmitted;
            m_Stats.BarriersEmitted += static_cast<uint32_t>(m_ScratchHoist.size());
            m_Stats.BarriersHoisted += static_cast<uint32_t>(m_ScratchHoist.size());
        }

        for (uint32_t si = sub.ScheduledFirst; si < sub.ScheduledFirst + sub.ScheduledCount; ++si)
        {
            const RGPassId p = order[si];
            const char* name = m_Graph.PassName(p);
            cl->BeginEvent(name ? name : "RGPass");

            // Diagnostic: feed the global marker ring per pass so a GPU-loss dump
            // (DumpRecentDiagnosticMarkers) names the passes in the hung frame. Off by
            // default (the ring is a cross-list global; a per-pass write every frame is
            // pure overhead outside a hang investigation). Enable with GE_VK_DEBUG_MARKERS.
            // GE_VK_GPU_CHECKPOINTS implies it: markers are what carry GPU
            // checkpoints, so arming checkpoints without per-pass markers would
            // leave every render-graph pass invisible to the executed-side dump.
            static const bool kRgPassDiagMarkers =
                std::getenv("GE_VK_DEBUG_MARKERS") != nullptr ||
                std::getenv("GE_VK_GPU_CHECKPOINTS") != nullptr;
            if (kRgPassDiagMarkers)
                cl->SetMarker(name ? name : "RGPass");

            // Remaining (non-hoisted) barriers — before BeginRenderPass, always.
            m_ScratchPassBarriers.clear();
            if (const RGBarrierBatch* bb = m_ScratchBatchOfSched[si])
            {
                for (uint32_t k = 0; k < bb->Count; ++k)
                {
                    const RGBarrier& b = m_Graph.Barriers()[bb->First + k];
                    if (b.FirstTouch)
                        continue; // hoisted
                    if (barrierSubOf[bb->First + k] != kInvalidId)
                        continue; // relocated onto a submission tail
                    // Cross-queue is a PHYSICAL property: producer and consumer
                    // on aliased logical queues share one queue's submission
                    // order, there is no semaphore between them, and rewriting
                    // the src scope would DROP the real exec dependency.
                    const bool crossPhysical =
                        b.SrcQueue < kQueueCount &&
                        m_PhysicalQueueOf[b.SrcQueue] != sub.PhysicalQueue;
                    ResourceBarrier eb;
                    translate(b, crossPhysical, sub.Queue, eb);
                    m_ScratchPassBarriers.push_back(eb);
                }
            }
            if (!m_ScratchPassBarriers.empty())
            {
                const auto emitBegin = RGClock::now();
                cl->BarrierBatch(m_ScratchPassBarriers);
                m_Stats.BarrierEmitMs += elapsedMs(emitBegin);
                ++m_Stats.BarrierBatchesEmitted;
                m_Stats.BarriersEmitted += static_cast<uint32_t>(m_ScratchPassBarriers.size());
            }

            // Profiling bracket: render pass + exec lambda; per-pass barriers
            // and the hoist excluded (matches the old graph's bracket). CPU
            // wall time is independent of GPU timestamps: qp is null on a
            // device without a query pool, and BeginQuery then stays ~0u.
            // Charts still need the CPU rows.
            std::chrono::steady_clock::time_point cpuBegin{};
            if (m_ProfilingEnabled)
            {
                RGPassTiming t;
                const char* src = name ? name : "RGPass";
                size_t len = std::strlen(src);
                if (len > sizeof(t.Name) - 1)
                    len = sizeof(t.Name) - 1;
                std::memcpy(t.Name, src, len); // t.Name is zero-initialized
                t.PassId = p;
                t.Phase = m_Graph.PassPhase(p);
                t.Queue = m_Graph.PassQueue(p); // declared queue, not the submission label
                if (qp)
                    t.BeginQuery = qp->WriteTimestamp(cl, TimestampPoint::SpanBegin);
                m_ProfilingCurrent.push_back(t);
                cpuBegin = std::chrono::steady_clock::now();
            }

            const auto attRange = m_ScratchAttRange[p];
            // Phase R2: a pass whose interior was recorded into a worker secondary
            // (Phase R1) is executed via vkCmdExecuteCommands inside a render pass
            // begun with useSecondaryCommandBuffers; every other pass records inline
            // exactly as before. A null slot (unflagged, attachment-less, or a
            // CreateSecondaryCommandList failure) always falls back to inline.
            CommandList* secondary = (p < m_Secondary.size()) ? m_Secondary[p].get() : nullptr;
            bool inRenderPass = false;
            if (attRange.second > 0)
            {
                RenderPassDesc rp;
                BuildRenderPassDesc(attRange.first, attRange.second, rp);
                if (secondary)
                    rp.useSecondaryCommandBuffers = true;
                cl->BeginRenderPass(rp);
                inRenderPass = true;
                ++m_Stats.RenderPassesBegun;
            }

            if (secondary && inRenderPass)
            {
                cl->ExecuteSecondary(&secondary, 1);
            }
            else
            {
                ctx.Cmd = cl;
                ctx.m_Pass = p;
                ctx.m_AttFirst = attRange.first;
                ctx.m_AttCount = attRange.second;
                ctx.m_KeyDirty = true; // every pass, incl. attachment-less — no stale key leaks
                m_Exec[p].Invoke(m_Exec[p].Fn, ctx);
            }

            if (inRenderPass)
                cl->EndRenderPass();

            if (m_ProfilingEnabled)
            {
                RGPassTiming& t = m_ProfilingCurrent.back();
                if (qp)
                    t.EndQuery = qp->WriteTimestamp(cl, TimestampPoint::SpanEnd);
                t.CpuMs = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - cpuBegin)
                              .count();
            }

            // Closing bracket for the marker above, so a device-loss dump can tell
            // "this pass began" from "this pass retired": with only the start
            // marker, the newest executed checkpoint names the last pass that was
            // ENTERED, which reads as the culprit even when it completed and the
            // real fault is in the next pass's work. Emitted after EndRenderPass —
            // a pass recorded into a secondary began its render pass with
            // useSecondaryCommandBuffers, where inline commands are illegal.
            if (kRgPassDiagMarkers)
            {
                char endMarker[kPassMarkerNameCapacity];
                BuildPassEndMarkerName(name ? name : "RGPass", endMarker);
                cl->SetMarker(endMarker);
            }
            cl->EndEvent();
        }

        // Barriers the plan placed on THIS submission record at its tail —
        // after every pass that touches their resource; foreign accessors are
        // ordered by the plan's derived timeline waits. Two classes arrive
        // here: export/normalize transitions (no pass owns them) and
        // cross-physical layout transitions relocated off their consumer so the
        // producing stage records where it is legal.
        //
        // On the placement's own queue the source scope records verbatim — that
        // is the point of relocating — and a foreign source is sanitized exactly
        // like the per-pass path. A dst scope naming a foreign queue is
        // rewritten the same way, and what carries the discarded visibility
        // differs by class:
        //   · relocated transition — the consumer is a submission in THIS frame
        //     that already waits on this one, and a semaphore signal/wait pair
        //     is a full memory dependency, not merely an execution one;
        //   · export contract — the consumer is next frame. On a graphics
        //     placement queue submission order covers it. On a NON-graphics
        //     placement nothing in this frame's plan orders it: the device's
        //     per-frame fences span MAX_FRAMES_IN_FLIGHT slots and the compute
        //     fence is never armed for graph work, so the edge holds only while
        //     some later frame carries a compute→graphics wait that transitively
        //     covers it. That residual is Hole 1's open half, not a guarantee.
        m_ScratchPassBarriers.clear();
        for (uint32_t k = 0; k < barrierSubOf.size(); ++k)
        {
            if (barrierSubOf[k] != subIndex)
                continue;
            const RGBarrier& b = m_Graph.Barriers()[k];
            const bool crossPhysicalSrc =
                b.SrcQueue < kQueueCount && m_PhysicalQueueOf[b.SrcQueue] != sub.PhysicalQueue;
            ResourceBarrier eb;
            translate(b, crossPhysicalSrc, sub.Queue, eb);
            const bool crossPhysicalDst =
                b.DstQueue < kQueueCount && m_PhysicalQueueOf[b.DstQueue] != sub.PhysicalQueue;
            if (crossPhysicalDst)
                CrossQueueConsumerSrcScope(sub.Queue, eb.dstStageMask, eb.dstAccessMask);
            m_ScratchPassBarriers.push_back(eb);
        }
        if (!m_ScratchPassBarriers.empty())
        {
            const auto emitBegin = RGClock::now();
            cl->BarrierBatch(m_ScratchPassBarriers);
            m_Stats.BarrierEmitMs += elapsedMs(emitBegin);
            ++m_Stats.BarrierBatchesEmitted;
            m_Stats.BarriersEmitted += static_cast<uint32_t>(m_ScratchPassBarriers.size());
        }

        cl->End();

        EnsureTimeline(sub.PhysicalQueue);
        m_ScratchWaits.clear();
        for (uint32_t wi = 0; wi < sub.WaitCount; ++wi)
        {
            const RGSemaphoreWait& w = sub.Waits[wi];
            EnsureTimeline(static_cast<uint8_t>(w.Queue));
            m_ScratchWaits.push_back({m_QueueTimelines[w.Queue], w.Value});
        }
        m_ScratchSignals.clear();
        m_ScratchSignals.push_back({m_QueueTimelines[sub.PhysicalQueue], sub.SignalValue});
        m_ScratchSubmitLists.clear();
        m_ScratchSubmitLists.push_back(cl);

        const auto submitBegin = RGClock::now();
        const bool ok = m_Device->QueueSubmit(ToQueueType(sub.PhysicalQueue), m_ScratchSubmitLists,
                                              m_ScratchWaits, m_ScratchSignals);
        m_Stats.QueueSubmitMs += elapsedMs(submitBegin);
        if (!ok)
        {
            // Q6 mid-frame short-circuit (design F10): a device loss suppresses
            // submits, so stop issuing work against the dead device for the rest
            // of this frame and let the health machine drive recovery. A submit
            // failure on a healthy device is still a bug worth catching (and the
            // GE_DEVICE_RECOVERY=0 lane keeps that assert — health stays Healthy).
            if (m_Device->GetDeviceHealth() != DeviceHealth::Healthy)
            {
                break;
            }
            assert(ok && "RenderGraph: QueueSubmit failed");
        }
        m_LastSignaled[sub.PhysicalQueue] = sub.SignalValue; // WaitForPendingWork target
        ++m_Stats.SubmissionsMade;
    }

    // Retire the Phase-R1 secondaries AFTER the last QueueSubmit (the primaries
    // that executed them are now in flight). RetireSecondaryCommandLists queues
    // each CB for recycle on this frame slot's fenced return and nulls the CB on
    // its wrapper, so the unique_ptrs (cleared next BeginFrame) are inert.
    if (!m_Secondary.empty())
    {
        m_ScratchSubmitLists.clear();
        for (auto& s : m_Secondary)
            if (s)
                m_ScratchSubmitLists.push_back(s.get());
        if (!m_ScratchSubmitLists.empty())
            m_Device->RetireSecondaryCommandLists(
                m_ScratchSubmitLists.data(), static_cast<uint32_t>(m_ScratchSubmitLists.size()));
    }

    if (m_ProfilingEnabled && !m_ProfilingCurrent.empty())
    {
        // Stash for resolve when this DEVICE frame slot returns (the query
        // pool's latency ring — not the upload ring's). CPU-only rows (no
        // pool) still publish on that slot so Visual Profiler charts fill.
        // swap, not move: the slot's drained vector keeps its capacity.
        const uint32_t fif = m_Device->GetFramesInFlight() > 0 ? m_Device->GetFramesInFlight() : 1u;
        if (m_ProfilingPendingBySlot.size() < fif)
            m_ProfilingPendingBySlot.resize(fif);
        std::swap(m_ProfilingPendingBySlot[m_Device->GetFrameIndex() % fif], m_ProfilingCurrent);
        m_ProfilingCurrent.clear();
    }
}

IDevice::GpuSyncToken RGFrame::SubmissionToken(RGQueue queue) const
{
    const uint8_t phys = m_PhysicalQueueOf[static_cast<uint32_t>(queue)];
    if (!m_TimelineCreated[phys] || m_LastSignaled[phys] == 0)
        return {};
    return IDevice::GpuSyncToken{m_QueueTimelines[phys], m_LastSignaled[phys]};
}

void RGFrame::SetProfilingEnabled(bool enabled)
{
#if GE_ENABLE_GPU_PROFILING
    m_ProfilingEnabled = enabled;
#else
    m_ProfilingEnabled = false;
    (void)enabled;
#endif
    if (!m_ProfilingEnabled)
    {
        std::lock_guard lock(m_ProfilingMutex);
        m_ProfilingLatest.clear();
        m_LastResolveStats = RGProfilingResolveStats{};
        for (auto& slot : m_ProfilingPendingBySlot)
            slot.clear();
    }
}

std::vector<RGFrame::RGPassTiming> RGFrame::LastFrameTimings() const
{
    std::lock_guard lock(m_ProfilingMutex);
    return m_ProfilingLatest;
}

RGFrame::RGProfilingResolveStats RGFrame::LastResolveStats() const
{
    std::lock_guard lock(m_ProfilingMutex);
    return m_LastResolveStats;
}

TimestampSemantics RGFrame::TimingSemantics() const
{
    IQueryPool* qp = m_Device ? m_Device->GetQueryPool() : nullptr;
    return qp ? qp->GetTimestampSemantics() : TimestampSemantics::PipelinePoint;
}

RGFrame::RGProfilingResolveStats RGFrame::ResolvePassGpuTimestamps(std::vector<RGPassTiming>& pend,
                                                                  IQueryPool* qp)
{
    RGProfilingResolveStats stats;
    const bool havePool = qp && qp->IsValid();
    for (RGPassTiming& t : pend)
    {
        if (t.BeginQuery == ~0u || t.EndQuery == ~0u)
        {
            // Pool cap exceeded at write time — only meaningful when a pool
            // existed to refuse the write. CPU-only (no encoder timestamps)
            // leaves both indices at ~0u by construction.
            if (havePool)
                ++stats.InvalidQueryIndices;
            continue;
        }
        if (!havePool)
            continue;
        QueryResult rBegin, rEnd;
        if (!qp->GetTimestampResult(t.BeginQuery, rBegin) || !rBegin.Available ||
            !qp->GetTimestampResult(t.EndQuery, rEnd) || !rEnd.Available)
        {
            ++stats.ReadFailures; // slot not back yet (caller order contract) — visible, no stall
            continue;
        }
        // Prefer the backend's per-work-unit accounting: on a backend that can
        // only sample at unit boundaries, the plain difference also charges the
        // pass for however long the GPU idled BETWEEN the units it created, and
        // the pair's order says nothing about the units — the GPU may run the
        // span's last unit before its first while every unit's own span is
        // valid (the backend clamps each on its own).
        uint64_t spanTicks = 0;
        if (!qp->GetTimestampSpanTicks(t.BeginQuery, t.EndQuery, spanTicks))
        {
            // The pair itself is the measurement, so an inverted pair is none.
            if (rEnd.Value < rBegin.Value)
            {
                ++stats.NonMonotonic;
                continue;
            }
            spanTicks = rEnd.Value - rBegin.Value;
        }
        t.GpuSpanMs = qp->TimestampToMs(spanTicks);
        t.BeginUnit = qp->GetTimestampUnitId(t.BeginQuery);
        t.EndUnit = qp->GetTimestampUnitId(t.EndQuery);
        ++stats.ResolvedPasses;
        // No work-unit range to fold against: the measurement is this pass's own.
        if (t.BeginUnit == IQueryPool::kInvalidTimestampUnit)
        {
            t.SpanCounted = true;
            stats.DistinctSpanGpuMs += t.GpuSpanMs;
        }
    }

    // Fold measurements that are not exclusive to one pass. A pass's span
    // covers the work-unit range [BeginUnit, EndUnit]; overlapping ranges are
    // the same GPU work reported twice (several passes recorded into one
    // encoder, or one pass's encoders sit inside another's range). Widest range
    // first, so a contained span is dropped in favour of the one that spans it.
    // Backends without unit accounting report no ranges and nothing folds.
    m_ScratchSpanOrder.clear();
    for (uint32_t i = 0; i < pend.size(); ++i)
    {
        // A zero span measured no GPU work of its own (both ends landed on one
        // boundary). It is an absence of measurement, not one shared with
        // another pass, so it neither folds nor counts as shared.
        if (pend[i].BeginUnit != IQueryPool::kInvalidTimestampUnit && pend[i].GpuSpanMs > 0.0)
            m_ScratchSpanOrder.push_back(i);
    }
    const auto unitFirst = [&](uint32_t i) { return std::min(pend[i].BeginUnit, pend[i].EndUnit); };
    const auto unitLast = [&](uint32_t i) { return std::max(pend[i].BeginUnit, pend[i].EndUnit); };
    std::stable_sort(m_ScratchSpanOrder.begin(), m_ScratchSpanOrder.end(),
                     [&](uint32_t a, uint32_t b)
                     { return (unitLast(a) - unitFirst(a)) > (unitLast(b) - unitFirst(b)); });
    m_ScratchSpanUnits.clear(); // packed [first, last] ranges already counted
    for (const uint32_t i : m_ScratchSpanOrder)
    {
        const uint32_t first = unitFirst(i);
        const uint32_t last = unitLast(i);
        bool covered = false;
        for (const uint64_t counted : m_ScratchSpanUnits)
        {
            const uint32_t cFirst = static_cast<uint32_t>(counted >> 32);
            const uint32_t cLast = static_cast<uint32_t>(counted & 0xFFFFFFFFu);
            if (first >= cFirst && last <= cLast)
            {
                covered = true;
                break;
            }
        }
        if (covered)
        {
            pend[i].SpanShared = true;
            ++stats.SharedSpanPasses;
            continue;
        }
        m_ScratchSpanUnits.push_back((static_cast<uint64_t>(first) << 32) | last);
        pend[i].SpanCounted = true;
        stats.DistinctSpanGpuMs += pend[i].GpuSpanMs;
    }
    // A span that swallowed another's is shared too — flagged, but counted,
    // since dropping both would lose the measurement entirely.
    for (RGPassTiming& t : pend)
    {
        if (t.SpanShared || t.BeginUnit == IQueryPool::kInvalidTimestampUnit || t.GpuSpanMs <= 0.0)
            continue;
        const uint32_t first = std::min(t.BeginUnit, t.EndUnit);
        const uint32_t last = std::max(t.BeginUnit, t.EndUnit);
        for (const RGPassTiming& other : pend)
        {
            if (&other == &t || other.BeginUnit == IQueryPool::kInvalidTimestampUnit ||
                other.GpuSpanMs <= 0.0)
                continue;
            const uint32_t oFirst = std::min(other.BeginUnit, other.EndUnit);
            const uint32_t oLast = std::max(other.BeginUnit, other.EndUnit);
            if (oFirst <= last && first <= oLast)
            {
                t.SpanShared = true;
                break;
            }
        }
    }
    return stats;
}

void RGFrame::ResolveProfiling()
{
    if (!m_ProfilingEnabled || !m_Device)
        return;
    if (m_ProfilingPendingBySlot.empty())
        return;
    const uint32_t slot = m_Device->GetFrameIndex() % static_cast<uint32_t>(m_ProfilingPendingBySlot.size());
    std::vector<RGPassTiming>& pend = m_ProfilingPendingBySlot[slot];
    if (pend.empty())
        return;

    IQueryPool* qp = m_Device->GetQueryPool();
    if (qp && !qp->IsValid())
        qp = nullptr;

    const RGProfilingResolveStats stats = ResolvePassGpuTimestamps(pend, qp);
    if (qp && !m_LoggedFirstGpuResolve)
    {
        m_LoggedFirstGpuResolve = true;
        Logger::Log::Info("RenderGraph: first GPU timing resolve of {} pending: {} passes resolved, {} read "
                          "failures, {} invalid indices, {} non-monotonic",
                          pend.size(), stats.ResolvedPasses, stats.ReadFailures, stats.InvalidQueryIndices,
                          stats.NonMonotonic);
    }
    {
        std::lock_guard lock(m_ProfilingMutex);
        m_ProfilingLatest = pend; // copy-assign: both vectors retain capacity
        m_LastResolveStats = stats;
    }
    pend.clear();
    // Mandatory: once the slot resets and indices are reused, a stale cache
    // would serve last frame's values for new queries.
    if (qp)
        qp->InvalidateCachedTimestampResults(slot);
}

} // namespace GameEngine::Rendering::RenderGraph

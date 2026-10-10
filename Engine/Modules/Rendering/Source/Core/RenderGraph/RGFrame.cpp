#include "Rendering/Core/RenderGraph/RGFrame.h"

#include "Logger/Logger.h"
#include "Rendering/Core/PipelineManager.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

// ── RGPassBuilder ───────────────────────────────────────────────────────────

namespace
{
RGAccess MapRead(RGTextureRead a)
{
    switch (a)
    {
        case RGTextureRead::Sampled: return RGAccess::Sampled;
        case RGTextureRead::SampledCompute: return RGAccess::SampledCompute;
        case RGTextureRead::SampledVertex: return RGAccess::SampledVertex;
        case RGTextureRead::Storage: return RGAccess::StorageRead;
        case RGTextureRead::CopySrc: return RGAccess::CopySrc;
    }
    return RGAccess::Sampled;
}
RGAccess MapRead(RGBufferRead a)
{
    switch (a)
    {
        case RGBufferRead::Storage: return RGAccess::StorageRead;
        case RGBufferRead::Uniform: return RGAccess::UniformRead;
        case RGBufferRead::Indirect: return RGAccess::IndirectRead;
        case RGBufferRead::Index: return RGAccess::IndexRead;
        case RGBufferRead::Vertex: return RGAccess::VertexRead;
        case RGBufferRead::CopySrc: return RGAccess::CopySrc;
    }
    return RGAccess::StorageRead;
}
RGAccess MapWrite(RGTextureWrite a)
{
    return a == RGTextureWrite::CopyDst ? RGAccess::CopyDst : RGAccess::StorageWrite;
}
RGAccess MapWrite(RGBufferWrite a)
{
    return a == RGBufferWrite::CopyDst ? RGAccess::CopyDst : RGAccess::StorageWrite;
}

// Transfer usage a transient's physical must carry, indexed by resource id.
//
// A transient is realized AFTER Compile, so the pass that copies it has already
// declared CopySrc/CopyDst — the same declaration the barrier generator turns
// into a TransferSrc/TransferDst layout transition. Deriving the usage bit from
// it means the fact lives in ONE place: an author cannot declare the copy and
// forget the bit, and a texture nothing copies never claims transfer usage.
// Culled passes are skipped: their copy is never recorded, so it requires nothing.
std::vector<uint32_t> CollectTransferUsage(const RGGraph& graph)
{
    std::vector<uint32_t> usageOf(graph.ResourceCount(), 0u);
    for (const RGAccessRecord& a : graph.Accesses())
    {
        if (a.Access != RGAccess::CopySrc && a.Access != RGAccess::CopyDst)
            continue;
        if (graph.CullReason(a.Pass) != RGCullReason::NotCulled)
            continue;
        usageOf[a.Resource] |= static_cast<uint32_t>(a.Access == RGAccess::CopySrc
                                                         ? TextureUsage::TransferSrc
                                                         : TextureUsage::TransferDst);
    }
    return usageOf;
}
} // namespace

void RGPassBuilder::Read(RGTexture t, RGTextureRead access, RGRange range)
{
    assert(t.IsValid());
    m_Frame->m_Graph.Read(m_Pass, t.Id, MapRead(access), range);
}

void RGPassBuilder::Read(RGBuffer b, RGBufferRead access)
{
    assert(b.IsValid());
    m_Frame->m_Graph.Read(m_Pass, b.Id, MapRead(access));
}

void RGPassBuilder::Write(RGTexture t, RGTextureWrite access, RGRange range)
{
    assert(t.IsValid());
    m_Frame->m_Graph.Write(m_Pass, t.Id, MapWrite(access), range);
}

void RGPassBuilder::Write(RGBuffer b, RGBufferWrite access)
{
    assert(b.IsValid());
    m_Frame->m_Graph.Write(m_Pass, b.Id, MapWrite(access));
}

void RGPassBuilder::Read(RGAccelerationStructure as)
{
    assert(as.IsValid());
    m_Frame->m_Graph.Read(m_Pass, as.Id, RGAccess::AccelerationStructureRead);
}

void RGPassBuilder::Write(RGAccelerationStructure as)
{
    assert(as.IsValid());
    m_Frame->m_Graph.Write(m_Pass, as.Id, RGAccess::AccelerationStructureBuild);
}

namespace
{
// Always-on (a release OOB slot would corrupt RenderPassDesc/format-key arrays
// downstream; declaration is a cold path — loud, survivable data error).
bool ValidColorSlot(uint32_t slot, const char* what)
{
    if (slot < kMaxColorAttachments)
        return true;
    Logger::Log::Error("[RenderGraph] {}: color attachment slot {} out of range (max {}) — attachment ignored",
                       what, slot, kMaxColorAttachments - 1);
    return false;
}

// RGStoreOp::None is "preserve, write nothing" — the recorded op for a
// read-only attach, never a caller's choice on an attachment the pass WRITES.
// Asked for on a written attachment it is a content-destroying lie: the backend
// emits VK_ATTACHMENT_STORE_OP_NONE, whose contract holds only "as long as no
// values are written to the attachment during the render pass", so the whole
// attachment's contents become undefined.
constexpr const char* kStoreNoneOnWrittenAttachment =
    "RenderGraph: RGStoreOp::None is only valid on a read-only attach — on a written "
    "attachment it leaves the contents undefined";

// The mirror of the above on the read-only side. A read-only attach records
// RGStoreOp::None whatever the caller spelled, so Store (the default, and what a
// shared ops struct carries for its ReadWrite arm) is harmless. DontCare is not:
// it is the one op whose meaning the override REVERSES, and a caller who asks
// for it believes the depth is being discarded while the graph preserves it.
constexpr const char* kStoreDontCareOnReadOnlyAttachment =
    "RenderGraph: a read-only depth attach preserves the contents — RGStoreOp::DontCare "
    "there asks to discard depth the pass is only reading";
} // namespace

void RGPassBuilder::AttachColor(uint32_t slot, RGTexture t, const RGAttachmentOps& ops, RGRange range)
{
    assert(t.IsValid());
    assert(ops.Store != RGStoreOp::None && kStoreNoneOnWrittenAttachment);
    if (!ValidColorSlot(slot, "AttachColor"))
        return;
    // LoadOp::Load CONSUMES prior contents (blend source / partial overwrite):
    // derive the read so the producer survives cull and the consumer scope
    // carries ColorRead (visibility — not just availability — of the write).
    if (ops.Load == RGLoadOp::Load)
        m_Frame->m_Graph.Read(m_Pass, t.Id, RGAccess::ColorLoad, range);
    m_Frame->m_Graph.Write(m_Pass, t.Id, RGAccess::ColorAttachment, range);
    m_Frame->m_Attachments.push_back(
        RGAttachmentRec{m_Pass, slot, t.Id, kInvalidId, ops, /*IsDepth=*/false, /*ReadOnly=*/false, range});
}

void RGPassBuilder::AttachColorResolve(uint32_t slot, RGTexture msaa, RGTexture resolve,
                                       const RGAttachmentOps& ops)
{
    assert(msaa.IsValid() && resolve.IsValid());
    assert(ops.Store != RGStoreOp::None && kStoreNoneOnWrittenAttachment);
    if (!ValidColorSlot(slot, "AttachColorResolve"))
        return;
    if (ops.Load == RGLoadOp::Load)
        m_Frame->m_Graph.Read(m_Pass, msaa.Id, RGAccess::ColorLoad); // load applies to the MSAA target
    m_Frame->m_Graph.Write(m_Pass, msaa.Id, RGAccess::ColorAttachment);
    m_Frame->m_Graph.Write(m_Pass, resolve.Id, RGAccess::ColorAttachment);
    m_Frame->m_Attachments.push_back(RGAttachmentRec{m_Pass, slot, msaa.Id, resolve.Id, ops,
                                                     /*IsDepth=*/false, /*ReadOnly=*/false,
                                                     RGRange::All()});
}

void RGPassBuilder::AttachDepth(RGTexture t, const RGAttachmentOps& ops, RGDepthAccess access,
                                RGRange range)
{
    assert(t.IsValid());
    const bool readOnly = access == RGDepthAccess::ReadOnly;
    RGAttachmentOps recorded = ops;
    if (readOnly)
    {
        assert(ops.Store != RGStoreOp::DontCare && kStoreDontCareOnReadOnlyAttachment);
        m_Frame->m_Graph.Read(m_Pass, t.Id, RGAccess::DepthRead, range);
        // The declared access is the truth the barriers are derived from: this
        // pass reads depth, so the render pass must write none. A store op is a
        // DEPTH_STENCIL_ATTACHMENT_WRITE at LATE_FRAGMENT_TESTS the graph never
        // recorded, and the next writer's barrier — built from a read-only
        // producer scope — would not make it available (WAW). DontCare would
        // instead discard depth a later pass still samples.
        recorded.Store = RGStoreOp::None;
    }
    else
    {
        assert(ops.Store != RGStoreOp::None && kStoreNoneOnWrittenAttachment);
        // Loaded depth (depth-test against a prepass) is a READ of the prior
        // contents too — keeps the prepass live (a Load+write attach of a
        // never-sampled intermediate would otherwise cull its producer) and
        // puts DepthRead in the consumer scope. Same (resource, range) merges
        // into one combined access (DepthRead|DepthWrite, attachment layout).
        if (ops.Load == RGLoadOp::Load)
            m_Frame->m_Graph.Read(m_Pass, t.Id, RGAccess::DepthRead, range);
        m_Frame->m_Graph.Write(m_Pass, t.Id, RGAccess::DepthWrite, range);
    }
    m_Frame->m_Attachments.push_back(
        RGAttachmentRec{m_Pass, 0, t.Id, kInvalidId, recorded, /*IsDepth=*/true, readOnly, range});
}

void RGPassBuilder::AttachDepthResolve(RGTexture msaa, RGTexture resolve, const RGAttachmentOps& ops)
{
    assert(msaa.IsValid() && resolve.IsValid());
    assert(ops.Store != RGStoreOp::None && kStoreNoneOnWrittenAttachment);
    if (ops.Load == RGLoadOp::Load)
        m_Frame->m_Graph.Read(m_Pass, msaa.Id, RGAccess::DepthRead);
    m_Frame->m_Graph.Write(m_Pass, msaa.Id, RGAccess::DepthWrite);
    m_Frame->m_Graph.Write(m_Pass, resolve.Id, RGAccess::DepthWrite); // resolve store
    m_Frame->m_Attachments.push_back(RGAttachmentRec{m_Pass, 0, msaa.Id, resolve.Id, ops,
                                                     /*IsDepth=*/true, /*ReadOnly=*/false,
                                                     RGRange::All()});
}

void RGPassBuilder::PreventCulling()
{
    m_Frame->m_Graph.PreventCulling(m_Pass);
}

void RGPassBuilder::RecordInSecondary()
{
    m_Frame->MarkRecordSecondary(m_Pass);
}

// ── RGContext ───────────────────────────────────────────────────────────────

TextureHandle RGContext::GetTexture(RGTexture t) const
{
    return TextureHandle(m_Frame->PhysicalOf(t.Id));
}

BufferHandle RGContext::GetBuffer(RGBuffer b) const
{
    return BufferHandle(m_Frame->PhysicalOf(b.Id));
}

IDevice* RGContext::GetDevice() const
{
    return m_Frame->m_Device;
}

TextureViewHandle RGContext::GetOrCreatePooledMipView(RGTexture t, uint32_t mip) const
{
    if (!t.IsValid() || !m_Frame->m_PersistentPool)
        return {};
    return m_Frame->m_PersistentPool->GetOrCreateMipView(TextureHandle(m_Frame->PhysicalOf(t.Id)),
                                                         mip);
}

PipelineFormatKey DeriveFormatKey(const RGAttachmentKeyInput* atts, uint32_t count,
                                  TextureFormat swapchainFallback)
{
    PipelineFormatKey fk{};
    uint32_t samples = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const RGAttachmentKeyInput& a = atts[i];
        if (a.IsDepth)
        {
            fk.DepthFormat = a.Format;
        }
        else
        {
            if (a.Slot >= PipelineFormatKey::kMaxColors)
                continue; // release-safe clamp (declaration already rejected + logged)
            fk.ColorFormats[a.Slot] = a.Format;
            if (fk.ColorCount < a.Slot + 1)
                fk.ColorCount = static_cast<uint8_t>(a.Slot + 1);
        }
        if (samples == 0 && a.SampleCount > 1)
            samples = a.SampleCount;
    }
    // StencilFormat stays value-zero: it participates in the cache-key hash and
    // every existing key was built without it.
    fk.RasterizationSamples = static_cast<uint8_t>(samples == 0 ? 1u : samples);
    // Attachment-less pass: fall back to the swapchain format — ONLY when the
    // pass binds nothing at all (a depth-only pass must keep ColorCount == 0).
    if (count == 0 && fk.ColorCount == 0 && fk.DepthFormat == TextureFormat{} &&
        swapchainFallback != TextureFormat{})
    {
        fk.ColorFormats[0] = swapchainFallback;
        fk.ColorCount = 1;
    }
    return fk;
}

PipelineFormatKey RGContext::BuildCurrentFormatKey() const
{
    if (!m_KeyDirty)
        return m_CachedKey;

    // Resolve each attachment's format (declared, or device-queried for
    // formatless external imports), then derive through the pure core. Keys on
    // the MSAA target (rec.Tex) — the resolve target is not part of the key.
    RGAttachmentKeyInput inputs[kMaxColorAttachments + 1];
    uint32_t count = 0;
    for (uint32_t i = m_AttFirst; i < m_AttFirst + m_AttCount; ++i)
    {
        const RGAttachmentRec& rec = m_Frame->m_Attachments[i];
        if (count >= kMaxColorAttachments + 1)
        {
            assert(false && "RenderGraph: more attachment records than attachable slots in one pass");
            break;
        }
        const RGResourceDesc& rd = m_Frame->m_Graph.ResourceDesc(rec.Tex);
        TextureFormat fmt = static_cast<TextureFormat>(rd.Format);
        if (fmt == TextureFormat{} && m_Frame->m_Device)
            fmt = m_Frame->m_Device->GetTextureFormat(TextureHandle(m_Frame->PhysicalOf(rec.Tex)));
        inputs[count++] = RGAttachmentKeyInput{rec.Slot, rec.IsDepth, fmt, rd.SampleCount};
    }
    const TextureFormat sw = m_Frame->m_Device ? m_Frame->m_Device->GetSwapchainTextureFormat()
                                               : TextureFormat{};
    m_CachedKey = DeriveFormatKey(inputs, count, sw);
    m_KeyDirty = false;
    return m_CachedKey;
}

uint32_t RGContext::GetCurrentSampleCount() const
{
    return BuildCurrentFormatKey().RasterizationSamples;
}

PipelineHandle RGContext::GetOrCreatePipelineVariant(GraphicsPipelineId id) const
{
    if (!m_Frame->m_Device || !id.IsValid())
        return {};
    if (m_Frame->m_Graph.PassQueue(m_Pass) != RGQueue::Graphics)
    {
        // Runtime guard, not just an assert: a release misuse would mint a
        // junk swapchain-keyed variant into the device-owned persistent cache.
        Logger::Log::Error("[RenderGraph] graphics pipeline variant requested from non-graphics pass '{}'",
                           m_Frame->m_Graph.PassName(m_Pass));
        return {};
    }
    const PipelineHandle h =
        m_Frame->m_Device->GetOrCreateGraphicsPipeline(id, BuildCurrentFormatKey());
    // The concrete cache can hand back a handle whose pipeline slot is gone
    // (a pipeline-change sweep destroys handles the cache still references).
    // A dead handle would make SetPipeline silently skip and the next Draw
    // abort; skip the draw with a diagnosable error instead. The probe MUST
    // be the device's VIRTUAL IsPipelineAlive — the handle managers are
    // header-inline singletons, so a statically double-linked module sees a
    // second empty table and a direct IsValidPipeline always says dead.
    if (h.IsValid() && !m_Frame->m_Device->IsPipelineAlive(h))
    {
        Logger::Log::Error(
            "[RenderGraph] pipeline variant cache returned a DEAD handle (id {}, pass '{}') — "
            "skipping this pass's draw",
            id.Value, m_Frame->m_Graph.PassName(m_Pass));
        return {};
    }
    return h;
}

PipelineHandle RGContext::GetOrCreatePipelineVariant(ComputePipelineId id) const
{
    if (!m_Frame->m_Device || !id.IsValid())
        return {};
    const PipelineHandle h = m_Frame->m_Device->GetOrCreateComputePipeline(id);
    if (h.IsValid() && !m_Frame->m_Device->IsPipelineAlive(h))
    {
        Logger::Log::Error(
            "[RenderGraph] compute pipeline variant cache returned a DEAD handle (id {}, pass '{}') — "
            "skipping this pass's dispatch",
            id.Value, m_Frame->m_Graph.PassName(m_Pass));
        return {};
    }
    return h;
}

void RGContext::SetPipelineAuto(GraphicsPipelineId id) const
{
    const PipelineHandle h = GetOrCreatePipelineVariant(id);
    if (!h.IsValid())
    {
        Logger::Log::Error("[RenderGraph] SetPipelineAuto: no variant for graphics pipeline id {} in pass '{}'",
                           id.Value, m_Frame->m_Graph.PassName(m_Pass));
        return;
    }
    Cmd->SetPipeline(h);
}

void RGContext::SetPipelineAuto(ComputePipelineId id) const
{
    const PipelineHandle h = GetOrCreatePipelineVariant(id);
    if (!h.IsValid())
    {
        Logger::Log::Error("[RenderGraph] SetPipelineAuto: no variant for compute pipeline id {} in pass '{}'",
                           id.Value, m_Frame->m_Graph.PassName(m_Pass));
        return;
    }
    Cmd->SetPipeline(h);
}

// ── RGFrame ─────────────────────────────────────────────────────────────────

RGFrame::RGFrame(IDevice* device, RGResourcePool* persistentPool, RGTransientPool* transientPool,
                 RGUploadRing* uploadRing)
    : m_Device(device), m_PersistentPool(persistentPool), m_TransientPool(transientPool),
      m_UploadRing(uploadRing)
{
}

namespace
{
// Shared scan for the two kind-checked name lookups. Returns kInvalidId when no
// resource of `kind` carries `name`.
RGResourceId FindResourceOfKind(const RGGraph& graph, const char* name, RGResourceKind kind)
{
    if (!name || name[0] == '\0')
        return kInvalidId;
    const size_t count = graph.ResourceCount();
    for (RGResourceId id = 0; id < count; ++id)
    {
        const RGResourceDesc& desc = graph.ResourceDesc(id);
        if (desc.Kind != kind || !desc.Name)
            continue;
        if (std::strcmp(desc.Name, name) == 0)
            return id;
    }
    return kInvalidId;
}
} // namespace

RGTexture RGFrame::FindTexture(const char* name) const
{
    return RGTexture{FindResourceOfKind(m_Graph, name, RGResourceKind::Texture)};
}

RGBuffer RGFrame::FindBuffer(const char* name) const
{
    return RGBuffer{FindResourceOfKind(m_Graph, name, RGResourceKind::Buffer)};
}

uint64_t RGFrame::MissingPhysical(RGResourceId id) const
{
    assert(false && "resource has no physical (culled, a transient before Execute's "
                    "realization, or a stale id)");
    Logger::Log::Error(
        "[RenderGraph] PhysicalOf({}): no realized physical — the resource was culled, is a transient "
        "queried before Execute realizes it, or the id is stale; returning a null handle",
        id);
    return 0;
}

void RGFrame::BeginFrame(uint64_t frameIndex)
{
    // Q6 slice 4 (§8-completion): an in-place device rebuild freed this frame's
    // cross-frame GPU state — the command pools our cached command lists hold
    // buffers + owners from, and every texture/buffer in the persistent pool
    // (GetOrCreateTexture would otherwise hand back a dead handle on a desc match,
    // producing the world pass's null-imageView depth attachment + sample-count
    // mismatch color targets that abort the first resumed frame). Drop both caches on
    // a rebuild-generation change so fresh resources bind to the rebuilt device: the
    // VulkanCommandList destructor detects the stale generation and touches nothing
    // dead (no UAF); the pool forgets its handles WITHOUT destroy (no double-free).
    const uint64_t rebuildGen = m_Device ? m_Device->GetDeviceRebuildGeneration() : 0;
    if (m_DeviceRebuildGen != rebuildGen)
    {
        for (auto& cl : m_CmdLists)
            cl.reset();
        if (m_PersistentPool)
            m_PersistentPool->DropAllAfterDeviceRebuild();
        if (m_TransientPool)
            m_TransientPool->DropAllAfterDeviceRebuild();
        // The per-frame upload ring's slots are persistently mapped; the rebuild
        // freed their buffers and unmapped the base pointers while AllocUpload still
        // reports Valid(). Re-create them here or the first resumed frame's
        // ScheduleHzbCullPass (and every other AllocUpload write) is a use-after-free.
        if (m_UploadRing)
            m_UploadRing->ReprovisionAfterDeviceRebuild();
        // The per-queue timeline semaphores died with the device too — forget them
        // (handle-forget only; the teardown already destroyed the VkSemaphores) so
        // EnsureTimeline recreates them on the rebuilt device at the next submit.
        // Left stale, every post-rebuild SubmissionToken() names a semaphore the
        // device no longer knows: QueryGpuSyncToken can never answer Complete, so
        // every readback ticket/ring pending stamped with it polls forever, and
        // QueueSubmit silently downgrades off the submit2 path.
        for (uint32_t q = 0; q < kQueueCount; ++q)
        {
            m_QueueTimelines[q] = {};
            m_TimelineCreated[q] = false;
            m_LastSignaled[q] = 0;
        }
        m_DeviceRebuildGen = rebuildGen;
    }

    // Resolve the returning device slot's pass timings FIRST (contract: the
    // caller has already run IDevice::BeginFrame, which fence-waited the slot
    // and cached its completed timestamps). Must precede the graph reset.
    ResolveProfiling();

    m_FrameIndex = frameIndex;
    ResetRecording();
    m_Stats = FrameStats{};
    m_UploadRing->BeginFrame(frameIndex);
}

void RGFrame::DiscardRecordedPasses()
{
    // A subsequent BeginFrame may have cleared declarations while the previous
    // submission still uses node-owned resources. Retirement must join it too.
    WaitForPendingWork();
    if (m_Graph.PassCount() == 0 && m_Graph.Arena().PendingDestructors() == 0)
        return;
    ResetRecording();
}

void RGFrame::ResetRecording()
{
    m_Graph.BeginFrame(); // resets the arena → runs last frame's exec-lambda dtors
    m_Exec.clear();
    m_PassRecordSecondary.clear();
    // m_Secondary is retired to the device at the end of the prior RecordAndSubmit;
    // clearing here just releases the now-empty unique_ptr slots.
    m_Secondary.clear();
    m_PendingTex.clear();
    m_PendingBuf.clear();
    m_Imports.clear();
    m_ZeroInitBufferIds.clear();
    m_InitializedTextureIds.clear();
    m_RequiredTransferUsage.clear();
    m_Attachments.clear();
    m_Physical.assign(m_Physical.size(), 0);
    m_HasPhysical.assign(m_HasPhysical.size(), 0);
    m_Backbuffer = {};
}

RGTexture RGFrame::CreateTexture(const char* name, const TextureDesc& desc)
{
    RGResourceDesc rd;
    rd.Kind = RGResourceKind::Texture;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.MipLevels = desc.mipLevels;
    rd.ArrayLayers = desc.arrayLayers;
    rd.Format = desc.format;
    rd.SampleCount = desc.sampleCount;
    // Mirror the descriptor claim exactly as ImportPersistentTexture does: the
    // transient pool realizes the physical FROM this desc, so its sampled
    // descriptors will claim GENERAL — without the mirror the graph would
    // transition sampled reads to ShaderReadOnly against that claim
    // (VUID-09600), a representable-invalid state.
    rd.SampledInGeneralLayout = desc.sampledInGeneralLayout;
    rd.Name = name;
    const RGResourceId id = m_Graph.CreateResource(rd);
    m_PendingTex.push_back(PendingTex{id, desc}); // realized after cull
    return RGTexture{id};
}

RGBuffer RGFrame::CreateBuffer(const char* name, const BufferDesc& desc)
{
    RGResourceDesc rd;
    rd.Kind = RGResourceKind::Buffer;
    rd.SizeBytes = desc.size;
    rd.Name = name;
    const RGResourceId id = m_Graph.CreateResource(rd);
    m_PendingBuf.push_back(PendingBuf{id, desc});
    return RGBuffer{id};
}

RGTexture RGFrame::ImportPersistentTexture(const char* name, const TextureDesc& desc,
                                           bool* outNeedsFreshInit)
{
    const TextureHandle handle =
        m_PersistentPool->GetOrCreateTexture(name, desc, m_FrameIndex, outNeedsFreshInit);
    // Same-frame dedup: a producer and a consumer both importing one pool name
    // (the shadow-array cascade arm + the world pass) must share one resource
    // id — two ids over one physical would be two independent hazard states.
    if (const ImportRec* existing = FindImport(static_cast<uint64_t>(handle), RGResourceKind::Texture))
    {
#ifndef NDEBUG
        if (!existing->PoolBacked)
            Logger::Log::Warning("[RenderGraph] ImportPersistentTexture('{}'): physical already imported "
                                 "externally as '{}' — pool state write-back will not run this frame",
                                 name, m_Graph.ResourceName(existing->Id));
#endif
        return RGTexture{existing->Id};
    }
    // Read the state AFTER GetOrCreate: a desc-change realloc resets it to
    // Undefined, which is exactly the truth for the fresh physical.
    const ResourceState state = m_PersistentPool->GetState(name);

    RGResourceDesc rd;
    rd.Kind = RGResourceKind::Texture;
    rd.Width = desc.width;
    rd.Height = desc.height;
    rd.MipLevels = desc.mipLevels;
    rd.ArrayLayers = desc.arrayLayers;
    rd.Format = desc.format;
    rd.SampleCount = desc.sampleCount;
    rd.SampledInGeneralLayout = desc.sampledInGeneralLayout;
    rd.Name = name;
    const RGResourceId id = m_Graph.CreateResource(rd);
    m_Graph.MarkImported(id, ToImageLayout(state));
    SetPhysical(id, static_cast<uint64_t>(handle));
    m_Imports.push_back(ImportRec{id, RGResourceKind::Texture, /*PoolBacked=*/true});
    return RGTexture{id};
}

void RGFrame::MarkPersistentTextureInitialized(RGTexture t)
{
    if (t.IsValid())
        m_InitializedTextureIds.push_back(t.Id);
}

void RGFrame::RequireTransferUsage(RGTexture t, TextureUsage usage)
{
    assert(t.IsValid());
    // Transfer bits only: any other usage is the importer's own declaration,
    // and rewriting a pooled desc's identity from outside it would be drift
    // in the other direction.
    assert((static_cast<uint32_t>(usage) & ~(static_cast<uint32_t>(TextureUsage::TransferSrc) |
                                             static_cast<uint32_t>(TextureUsage::TransferDst))) == 0 &&
           "RequireTransferUsage takes TransferSrc/TransferDst only");
    m_RequiredTransferUsage.push_back({t.Id, static_cast<uint32_t>(usage)});
}

RGBuffer RGFrame::ImportPersistentBuffer(const char* name, const BufferDesc& desc,
                                         bool* outNeedsZeroInit)
{
    const BufferHandle handle =
        m_PersistentPool->GetOrCreateBuffer(name, desc, m_FrameIndex, outNeedsZeroInit);
    if (const ImportRec* existing = FindImport(static_cast<uint64_t>(handle), RGResourceKind::Buffer))
    {
#ifndef NDEBUG
        if (!existing->PoolBacked)
            Logger::Log::Warning("[RenderGraph] ImportPersistentBuffer('{}'): physical already imported "
                                 "externally as '{}' — pool state write-back will not run this frame",
                                 name, m_Graph.ResourceName(existing->Id));
#endif
        return RGBuffer{existing->Id};
    }
    RGResourceDesc rd;
    rd.Kind = RGResourceKind::Buffer;
    rd.SizeBytes = desc.size;
    rd.Name = name;
    const RGResourceId id = m_Graph.CreateResource(rd);
    m_Graph.MarkImported(id, RGImageLayout::Undefined); // buffers carry no layout
    SetPhysical(id, static_cast<uint64_t>(handle));
    m_Imports.push_back(ImportRec{id, RGResourceKind::Buffer, /*PoolBacked=*/true});
    return RGBuffer{id};
}

void RGFrame::AddBufferZeroInit(RGBuffer b, const char* passName)
{
    if (!b.IsValid())
        return;
    // vkCmdFillBuffer requires 4-byte offset/size granularity; sizes come from
    // user-authored blueprint JSON, so round DOWN rather than trust them. A
    // tail remainder < 4 bytes stays unfilled — declare 4-byte-multiple sizes.
    const uint64_t sizeBytes = m_Graph.ResourceDesc(b.Id).SizeBytes & ~uint64_t{3};
    if (sizeBytes == 0)
        return;
    m_ZeroInitBufferIds.push_back(b.Id);
    AddPass(
        passName ? passName : "BufferZeroInit", PassPhase::kEarlySetup,
        [&](RGPassBuilder& p) { p.Write(b, RGBufferWrite::CopyDst); },
        [b, sizeBytes](RGContext& ctx)
        {
            auto* cl = ctx.Cmd;
            const BufferHandle buf = ctx.GetBuffer(b);
            if (!cl || !buf.IsValid())
                return;
            cl->FillBuffer(buf, 0, sizeBytes, 0u);
        });
}

const RGFrame::ImportRec* RGFrame::FindImport(uint64_t physical, RGResourceKind kind) const
{
    for (const ImportRec& imp : m_Imports)
        if (imp.Kind == kind && m_Physical[imp.Id] == physical)
            return &imp;
    return nullptr;
}

RGTexture RGFrame::ImportExternalTexture(const char* name, TextureHandle handle,
                                         ResourceState currentState, TextureFormat format,
                                         uint32_t mips, uint32_t layers)
{
    // Dedup by physical handle: one physical = one resource = one hazard
    // state. First import's declaration wins.
    if (const ImportRec* existing = FindImport(static_cast<uint64_t>(handle), RGResourceKind::Texture))
    {
#ifndef NDEBUG
        const RGResourceDesc& rd = m_Graph.ResourceDesc(existing->Id);
        if (rd.Format != static_cast<uint32_t>(format) && format != TextureFormat{})
            Logger::Log::Warning("[RenderGraph] ImportExternalTexture('{}'): handle already imported as '{}' "
                                 "with a different declared format — first import wins",
                                 name, m_Graph.ResourceName(existing->Id));
#endif
        return RGTexture{existing->Id};
    }
#ifndef NDEBUG
    // A wrong declared format keys a pipeline variant that mismatches the
    // bound image (the sRGB-vs-UNORM alias class) — catch it at import time.
    if (format != TextureFormat{} && m_Device)
    {
        const TextureFormat actual = m_Device->GetTextureFormat(handle);
        if (actual != TextureFormat{} && actual != format)
            Logger::Log::Error("[RenderGraph] ImportExternalTexture('{}'): declared format {} != device format {}",
                               name, static_cast<uint32_t>(format), static_cast<uint32_t>(actual));
    }
#endif
    RGResourceDesc rd;
    rd.Kind = RGResourceKind::Texture;
    rd.MipLevels = mips;
    rd.ArrayLayers = layers;
    rd.Format = static_cast<uint32_t>(format); // Unknown → device query at key build
    rd.SampleCount = m_Device ? m_Device->GetTextureSampleCount(handle) : 1;
    // Record the physical extent: viewport/letterbox math reads the declared
    // desc at declaration time, before any command list exists.
    if (m_Device)
        m_Device->GetTextureSize(handle, rd.Width, rd.Height);
    // Mirror the physical's own claim, exactly as the desc-carrying import
    // paths do. An external import has no desc, so ask the device: a texture
    // whose sampled descriptors claim GENERAL must not have its sampled reads
    // transitioned to ShaderReadOnly (VUID-...-00344).
    rd.SampledInGeneralLayout = m_Device && m_Device->GetTextureSampledInGeneralLayout(handle);
    rd.Name = name;
    const RGResourceId id = m_Graph.CreateResource(rd);
    m_Graph.MarkImported(id, ToImageLayout(currentState));
    SetPhysical(id, static_cast<uint64_t>(handle));
    m_Imports.push_back(ImportRec{id, RGResourceKind::Texture, /*PoolBacked=*/false});
    return RGTexture{id};
}

RGBuffer RGFrame::ImportExternalBuffer(const char* name, BufferHandle handle, uint64_t sizeBytes)
{
    if (const ImportRec* existing = FindImport(static_cast<uint64_t>(handle), RGResourceKind::Buffer))
        return RGBuffer{existing->Id};
    RGResourceDesc rd;
    rd.Kind = RGResourceKind::Buffer;
    rd.SizeBytes = sizeBytes;
    rd.Name = name;
    const RGResourceId id = m_Graph.CreateResource(rd);
    // Buffers carry no layout; MarkImported still arms the first-touch
    // cross-frame execution dependency (PendingImportSync).
    m_Graph.MarkImported(id, RGImageLayout::Undefined);
    SetPhysical(id, static_cast<uint64_t>(handle));
    m_Imports.push_back(ImportRec{id, RGResourceKind::Buffer, /*PoolBacked=*/false});
    return RGBuffer{id};
}

RGAccelerationStructure RGFrame::ImportAccelerationStructure(const char* name, TlasSlotHandle slot)
{
    if (!slot.IsValid())
        return {};
    if (const ImportRec* existing = FindImport(slot.id, RGResourceKind::AccelerationStructure))
        return RGAccelerationStructure{existing->Id};
    RGResourceDesc rd;
    rd.Kind = RGResourceKind::AccelerationStructure;
    rd.Name = name;
    const RGResourceId id = m_Graph.CreateResource(rd);
    m_Graph.MarkImported(id, RGImageLayout::Undefined);
    SetPhysical(id, slot.id);
    m_Imports.push_back(ImportRec{id, RGResourceKind::AccelerationStructure, /*PoolBacked=*/false});
    return RGAccelerationStructure{id};
}

RGTexture RGFrame::ImportBackbuffer(const char* name)
{
    if (!m_Device)
        return {};
    const TextureHandle img = m_Device->GetCurrentSwapchainImageHandle();
    if (!img.IsValid())
        return {};
    // Common → Undefined at barrier gen: a discarding first transition, which
    // is exactly right for an image whose prior contents are presentation
    // history. Format from the device so the pipeline format key is exact.
    const RGTexture t = ImportExternalTexture(name, img, ResourceState::Common,
                                              m_Device->GetSwapchainTextureFormat());
    MarkOutput(t);
    m_Backbuffer = t;
    return t;
}

RGPassId FindNonGraphicsBackbufferAccess(const RGGraph& graph, RGResourceId backbuffer)
{
    if (backbuffer == kInvalidId)
        return kInvalidId;
    for (const RGAccessRecord& a : graph.Accesses())
        if (a.Resource == backbuffer && graph.PassQueue(a.Pass) != RGQueue::Graphics)
            return a.Pass;
    return kInvalidId;
}

void RGFrame::WaitForPendingWork()
{
    // The old device already retired its work and destroyed these semaphores.
    if (!m_Device || m_DeviceRebuildGen != m_Device->GetDeviceRebuildGeneration())
        return;
    for (uint32_t q = 0; q < kQueueCount; ++q)
        if (m_TimelineCreated[q] && m_LastSignaled[q] > 0)
            m_Device->WaitTimelineSemaphoreValue(m_QueueTimelines[q], m_LastSignaled[q]);
}

void RGFrame::Execute()
{
    const size_t passCount = m_Graph.PassCount();
    if (passCount == 0)
        return;

    // Structural guard for the backbuffer's queue contract (the rule is stated
    // on FindNonGraphicsBackbufferAccess). Declaration is where a compute final
    // blit would be introduced and nothing downstream would report it; one
    // linear scan over declared accesses, compiled out of release.
    assert(FindNonGraphicsBackbufferAccess(m_Graph, m_Backbuffer.Id) == kInvalidId &&
           "RenderGraph: the backbuffer may only be accessed by graphics-queue passes");

    // A2 STEP-0: pre-record serial-floor brackets. Always-on, a handful of
    // steady_clock pairs per frame, zero allocations. m_Stats was reset in
    // BeginFrame, so these accumulate coherently through to the next frame.
    using RGClock = std::chrono::steady_clock;
    auto elapsedMs = [](RGClock::time_point begin) {
        return std::chrono::duration<double, std::milli>(RGClock::now() - begin).count();
    };

    const auto compileBegin = RGClock::now();
    m_Graph.Compile();
    m_Stats.CompileMs = elapsedMs(compileBegin);

    if (m_Graph.LivePassCount() == 0)
    {
        // The classic immediate-mode trap: everything declared, nothing anchored.
        Logger::Log::Warning(
            "[RenderGraph] all {} declared passes were culled — no external sink declared this frame "
            "(missing MarkOutput / import?)",
            passCount);
    }
    else
    {
        // Realize transients AFTER cull: a culled pass's resources never allocate.
        // Gate on USED-by-a-live-pass (not cull-"needed"): a live pass's depth
        // attachment must exist even when nothing downstream samples it.
        //
        // Transfer usage is folded in here rather than declared in the desc: the
        // copying pass already declared CopySrc/CopyDst, and one fact stated
        // twice is the drift. The widened usage is part of the pool's desc
        // identity, so a copied transient pools separately from an identical
        // one nothing copies — correct, and the copy-carrying frame is the rare one.
        // Pool imports get the same usage through the write-back below.
        std::vector<uint32_t> transferUsageOf = CollectTransferUsage(m_Graph);
        // Usage stated for graph-external consumers (RequireTransferUsage)
        // joins the derived usage here and takes both paths with it.
        for (const RequiredTransferUsage& r : m_RequiredTransferUsage)
        {
            assert(r.Id < transferUsageOf.size() && "RequireTransferUsage with a stale resource id");
            transferUsageOf[r.Id] |= r.Usage;
        }
        for (PendingTex& t : m_PendingTex)
            if (m_Graph.IsResourceUsed(t.Id))
            {
                t.Desc.usage |= transferUsageOf[t.Id];
                SetPhysical(t.Id,
                            static_cast<uint64_t>(m_TransientPool->AcquireTexture(t.Desc, m_FrameIndex)));
            }
        for (const PendingBuf& b : m_PendingBuf)
            if (m_Graph.IsResourceUsed(b.Id))
                SetPhysical(b.Id,
                            static_cast<uint64_t>(m_TransientPool->AcquireBuffer(b.Desc, m_FrameIndex)));

        const auto scheduleBegin = RGClock::now();
        m_Graph.Schedule();
        m_Stats.ScheduleMs = elapsedMs(scheduleBegin);

        const auto barriersBegin = RGClock::now();
        m_Graph.GenerateBarriers(m_PhysicalQueueOf);
        m_Stats.GenerateBarriersMs = elapsedMs(barriersBegin);

        const auto planBegin = RGClock::now();
        m_Graph.BuildSubmissionPlan(m_PhysicalQueueOf);
        m_Stats.BuildSubmissionPlanMs = elapsedMs(planBegin);

        // Record real command buffers per submission (barriers → render passes →
        // pass lambdas with ctx.Cmd) and submit with timeline-semaphore sync.
        RecordAndSubmit();
        // Only a frame that reached submission counts: an abandoned or
        // all-culled frame leaves the count where it was, which is what the
        // declare-side histories compare against.
        ++m_SubmittedFrameCount;

        // C3 write-back: the pool must carry each imported texture's end-of-frame
        // layout into the next frame (no execute-time staleness). Gate on
        // USED-by-a-live-pass, matching barrier generation: a live-but-not-
        // needed access (a depth attachment nobody samples) still transitions
        // the physical, and skipping its write-back would seed the next
        // frame's authoritative oldLayout from a stale pool state.
        //
        // The transfer usage derived from this frame's declared copies rides
        // the same write-back: an import's physical predates the declaration,
        // so the pool widens it on its next materialization — the transient
        // rule (usage follows the declared copy, never the desc) applied one
        // frame late, which is the earliest the physical can change hands.
        for (const ImportRec& imp : m_Imports)
        {
            if (!imp.PoolBacked || imp.Kind != RGResourceKind::Texture || !m_Graph.IsResourceUsed(imp.Id))
                continue;
            const char* name = m_Graph.ResourceName(imp.Id);
            m_PersistentPool->SetState(name, ToResourceState(m_Graph.FinalLayout(imp.Id)));
            if (transferUsageOf[imp.Id] != 0)
                m_PersistentPool->RequireTextureUsage(name,
                                                      static_cast<TextureUsage>(transferUsageOf[imp.Id]));
        }
        // Zero-init discharge: only an EXECUTED frame's fill counts. A frame
        // that declared the fill but was abandoned before Execute (swapchain
        // acquire failure, HDR recheck) never reaches here, so the pool arm
        // survives and the next importer re-schedules the fill.
        for (const RGResourceId id : m_ZeroInitBufferIds)
            m_PersistentPool->MarkBufferZeroFilled(m_Graph.ResourceName(id));
        // Same discharge rule for imported history textures: the arm clears only
        // when the frame that declared the rewrite actually executed.
        for (const RGResourceId id : m_InitializedTextureIds)
            m_PersistentPool->MarkTextureInitialized(m_Graph.ResourceName(id));
    }

    // Pool maintenance every frame (also when everything culled): transients
    // free, idle entries — incl. a hidden view's persistents — age out.
    m_TransientPool->ReleaseFrame(m_FrameIndex);
    m_TransientPool->EvictIdle(m_FrameIndex, kTransientMaxIdleFrames);
    m_PersistentPool->TickPoolElements(m_FrameIndex, kPersistentMaxIdleFrames,
                                       m_UploadRing->FramesInFlight());
}

} // namespace GameEngine::Rendering::RenderGraph

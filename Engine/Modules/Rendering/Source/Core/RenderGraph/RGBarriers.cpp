#include "Rendering/Core/RenderGraph/RGGraph.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

namespace
{

// RG-side arm of the GE_VK_WATCH_IMAGE diagnostic (backend arm in VulkanDevice.cpp):
// matches the env var against RESOURCE NAMES so emitted barriers carry pass
// attribution the backend can't recover from a raw VkImage.
const char* WatchedResourceName()
{
    static const char* name = std::getenv("GE_VK_WATCH_IMAGE");
    return (name && name[0] != '\0') ? name : nullptr;
}

using Detail::BarrierGroup;
using Detail::BarrierSrcScope;
using Detail::CombinedAccess;
using Detail::ResolvedRange;

ResolvedRange Resolve(const RGRange& r, uint32_t mips, uint32_t layers)
{
    assert(r.BaseMip < mips && "RGRange.BaseMip out of bounds");
    assert(r.BaseLayer < layers && "RGRange.BaseLayer out of bounds");
    ResolvedRange out;
    out.BaseMip = r.BaseMip;
    out.MipCount = (r.MipCount == kRemaining || r.BaseMip + r.MipCount > mips) ? mips - r.BaseMip
                                                                               : r.MipCount;
    out.BaseLayer = r.BaseLayer;
    out.LayerCount = (r.LayerCount == kRemaining || r.BaseLayer + r.LayerCount > layers)
                         ? layers - r.BaseLayer
                         : r.LayerCount;
    return out;
}

bool Overlap(const ResolvedRange& a, const ResolvedRange& b)
{
    const bool mips = a.BaseMip < b.BaseMip + b.MipCount && b.BaseMip < a.BaseMip + a.MipCount;
    const bool layers =
        a.BaseLayer < b.BaseLayer + b.LayerCount && b.BaseLayer < a.BaseLayer + a.LayerCount;
    return mips && layers;
}

// Order-independent meet of two READ layouts combined within one pass (a write
// layout always dominates and conflicting write layouts assert). Equal stays;
// depth-read + sampled resolves to DepthReadOnly (valid for both usages);
// any other mix degrades to General (valid superset for reads).
RGImageLayout MeetReadLayout(RGImageLayout a, RGImageLayout b)
{
    if (a == b)
        return a;
    if (a == RGImageLayout::Undefined)
        return b;
    if (b == RGImageLayout::Undefined)
        return a;
    const bool depthSampledPair =
        (a == RGImageLayout::DepthReadOnly && b == RGImageLayout::ShaderReadOnly) ||
        (b == RGImageLayout::DepthReadOnly && a == RGImageLayout::ShaderReadOnly);
    return depthSampledPair ? RGImageLayout::DepthReadOnly : RGImageLayout::General;
}

} // namespace

RGImageLayout RGGraph::FinalLayout(RGResourceId r) const
{
    assert(r + 1 < m_CellOff.size() && "FinalLayout: GenerateBarriers has not run");
    const uint32_t first = m_CellOff[r];
    const uint32_t last = m_CellOff[r + 1];
    if (first == last)
        return RGImageLayout::Undefined;
#ifndef NDEBUG
    if (m_ResourceImported[r])
        for (uint32_t c = first + 1; c < last; ++c)
            assert(m_Cells[c].Layout == m_Cells[first].Layout &&
                   "FinalLayout: imported resource ends the frame with heterogeneous subresource layouts");
#endif
    return m_Cells[first].Layout;
}

void RGGraph::GenerateBarriers(const uint8_t physicalOfLogical[kQueueCount])
{
    m_Barriers.clear();
    m_BarrierBatches.clear();

    const uint32_t resCount = static_cast<uint32_t>(m_Resources.size());

    // ── Cell grid (flat, dense; members so capacity persists across frames) ──
    m_CellMips.resize(resCount);
    m_CellOff.resize(resCount + 1);
    uint32_t totalCells = 0;
    for (RGResourceId r = 0; r < resCount; ++r)
    {
        const bool isTex = m_Resources[r].Kind == RGResourceKind::Texture;
        m_CellMips[r] = isTex ? std::max(1u, m_Resources[r].MipLevels) : 1u;
        const uint32_t layers = isTex ? std::max(1u, m_Resources[r].ArrayLayers) : 1u;
        m_CellOff[r] = totalCells;
        totalCells += m_CellMips[r] * layers;
    }
    m_CellOff[resCount] = totalCells;

    m_Cells.resize(totalCells);
    for (RGResourceId r = 0; r < resCount; ++r)
    {
        // Producer scope starts at BottomOfPipe (an execution dependency on ALL
        // prior GPU work): created resources may recycle pooled physical memory a
        // prior in-flight frame still touches, and imported resources carry last
        // frame's contents. TopOfPipe here would create NO execution dependency.
        // Cross-frame memory availability comes from the frame fence/semaphore.
        // PendingImportSync is seeded for CREATED resources too — textures get
        // the Undefined transition anyway, but a recycled transient BUFFER's
        // first write would otherwise emit nothing while the pool blanket-frees
        // at CPU frame end.
        RGCellState init{};
        init.WriteStage = RGStage::BottomOfPipe;
        init.WriteAccess = RGAccessMask::None;
        init.Layout = m_ResourceImported[r] ? m_ResourceInitLayout[r] : RGImageLayout::Undefined;
        init.Queue = 0;
        init.Init = m_ResourceImported[r] != 0;
        init.PendingImportSync = true;
        for (uint32_t c = m_CellOff[r]; c < m_CellOff[r + 1]; ++c)
            m_Cells[c] = init;
    }

    EnsurePassAccessIndex();

    // Flat member scratch (capacity retained — no steady-state heap churn):
    // combined accesses per pass; needy-cell groups keyed by source scope with
    // their (LayerCount × MipCount) masks packed into one byte buffer; mip runs
    // in CSR-by-layer layout for the rectangle merge.
    auto& combos = m_ScratchCombos;
    auto& groups = m_ScratchGroups;
    auto& needy = m_ScratchGroupNeedy;
    auto& runs = m_ScratchRuns;
    auto& runOff = m_ScratchRunOff;

    for (uint32_t si = 0; si < m_ScheduledOrder.size(); ++si)
    {
        const RGPassId p = m_ScheduledOrder[si];
        const RGQueue queue = m_Passes[p].Desc.Queue;
        const uint8_t passQueue = static_cast<uint8_t>(queue);
        // Coverage slot: PHYSICAL queue. Logical queues collapsed onto one
        // physical queue (the production default) must share reader coverage —
        // the submission plan emits no semaphore between them, so the barrier
        // is the only synchronization there is.
        const uint8_t physQueue = physicalOfLogical[passQueue];

        // ── Combine this pass's accesses per (resource, identical range) ──
        combos.clear();
        for (uint32_t k = m_PassAccOff[p]; k < m_PassAccOff[p + 1]; ++k)
        {
            const RGAccessRecord& a = m_Accesses[m_PassAccIdx[k]];
            RGAccessInfo info = MapAccess(a.Access, queue);
            // Descriptor-claim constraint (View.DepthResolved): this texture's
            // sampled descriptors claim GENERAL (TextureDesc::sampledInGeneralLayout),
            // so a sampled read must observe GENERAL — ShaderReadOnly would
            // contradict the claimed layout (VUID-vkCmdDraw-None-09600) and
            // ping-pong the layout against storage users across queues. Scoped
            // to sampled reads: storage accesses are natively General, and
            // non-sampled reads (copies) keep their API-required layouts with
            // the submission plan ordering the mix (RAW/WAR-on-layout edges).
            const bool sampledRead = a.Access == RGAccess::Sampled ||
                                     a.Access == RGAccess::SampledCompute ||
                                     a.Access == RGAccess::SampledVertex;
            if (sampledRead && m_Resources[a.Resource].SampledInGeneralLayout &&
                m_Resources[a.Resource].Kind == RGResourceKind::Texture)
                info.Layout = RGImageLayout::General;
            const uint32_t layers =
                (m_CellOff[a.Resource + 1] - m_CellOff[a.Resource]) / m_CellMips[a.Resource];
            const ResolvedRange rr = Resolve(a.Range, m_CellMips[a.Resource], layers);
            int ci = -1;
            for (size_t kk = 0; kk < combos.size(); ++kk)
                if (combos[kk].Res == a.Resource && combos[kk].Range == rr)
                {
                    ci = static_cast<int>(kk);
                    break;
                }
            if (ci < 0)
            {
                combos.push_back(CombinedAccess{a.Resource, rr, RGStage::None, RGAccessMask::None,
                                                RGImageLayout::Undefined, false, false});
                ci = static_cast<int>(combos.size()) - 1;
            }
            CombinedAccess& c = combos[static_cast<size_t>(ci)];
            c.Stage |= info.Stage;
            c.Access |= info.Access;
            c.HasSampled |= (a.Access == RGAccess::Sampled || a.Access == RGAccess::SampledCompute ||
                             a.Access == RGAccess::SampledVertex);
            // Layout bookkeeping is texture-only: buffers carry no layout,
            // and a mixed-kind buffer write (StorageWrite + CopyDst — the
            // fill-then-dispatch union) is a legal stage/access widening,
            // not a layout conflict.
            const bool isTexCombo = m_Resources[a.Resource].Kind == RGResourceKind::Texture;
            if (info.IsWrite)
            {
                assert((!isTexCombo || !c.HasWrite || c.Layout == info.Layout) &&
                       "RenderGraph: conflicting write layouts on one resource within one pass");
                c.HasWrite = true;
                if (isTexCombo)
                    c.Layout = info.Layout; // a write layout dominates within a pass
            }
            else if (!c.HasWrite && isTexCombo)
            {
                c.Layout = MeetReadLayout(c.Layout, info.Layout); // declaration-order independent
            }
        }

#ifndef NDEBUG
        for (size_t i = 0; i < combos.size(); ++i)
            for (size_t j = i + 1; j < combos.size(); ++j)
                if (combos[i].Res == combos[j].Res && Overlap(combos[i].Range, combos[j].Range))
                    assert(!(combos[i].HasWrite || combos[j].HasWrite) &&
                           "RenderGraph: same-pass overlapping subresource ranges with a write");
        // Backstop for the declaration-layer in==out guard: a descriptor-
        // sampled read folded into the SAME (resource, range) combo as a
        // write is a feedback loop the merge would otherwise silence (the
        // write layout dominates, so the sample reads the wrong layout).
        // Legal same-pass read+write pairs (DepthRead+DepthWrite, the
        // Load-derived ColorLoad+ColorAttachment, StorageRead+StorageWrite)
        // do not set HasSampled.
        for (const CombinedAccess& c : combos)
            assert(!(c.HasSampled && c.HasWrite &&
                     m_Resources[c.Res].Kind == RGResourceKind::Texture) &&
                   "RenderGraph: pass samples a texture range it also writes (feedback loop)");
#endif

        const uint32_t batchFirst = static_cast<uint32_t>(m_Barriers.size());
        uint32_t srcUnion = RGStage::None;
        uint32_t dstUnion = RGStage::None;

        for (const CombinedAccess& c : combos)
        {
            // Ordered and waited on like any resource (RGSubmission), never
            // transitioned: see RGResourceKind::AccelerationStructure.
            if (m_Resources[c.Res].Kind == RGResourceKind::AccelerationStructure)
                continue;
            const bool isTex = m_Resources[c.Res].Kind == RGResourceKind::Texture;
            const uint32_t mips = m_CellMips[c.Res];
            const uint32_t rMips = c.Range.MipCount;
            const uint32_t rLayers = c.Range.LayerCount;
            const uint32_t maskSize = rLayers * rMips;

            // ── Evaluate + update each cell; record needy cells by source scope ──
            groups.clear();
            needy.clear();
            for (uint32_t L = 0; L < rLayers; ++L)
            {
                for (uint32_t M = 0; M < rMips; ++M)
                {
                    const uint32_t absLayer = c.Range.BaseLayer + L;
                    const uint32_t absMip = c.Range.BaseMip + M;
                    RGCellState& cell = m_Cells[m_CellOff[c.Res] + absLayer * mips + absMip];

                    const bool layoutChange =
                        isTex && cell.Layout != c.Layout && c.Layout != RGImageLayout::Undefined;
                    const bool priorWrite = (cell.WriteAccess & RGAccessMask::WriteBits) != 0;
                    // First frame-touch, captured BEFORE any path clears it:
                    // feeds the barrier's FirstTouch (hoist safety) ground truth.
                    const bool cellFirstTouch = cell.PendingImportSync;

                    bool need = false;
                    BarrierSrcScope src{};
                    src.OldLayout = isTex ? cell.Layout : RGImageLayout::Undefined;
                    src.Queue = cell.Queue;

                    if (c.HasWrite)
                    {
                        // WAW (prior write), WAR (SAME-PHYSICAL-QUEUE synchronized
                        // readers — cross-physical readers are ordered by the
                        // submission plan's WAR semaphore waits, and their stages
                        // may be invalid masks on this queue), layout change,
                        // first texture use (Undefined transition), or the first
                        // touch of an imported/recycled resource (cross-frame
                        // exec dep).
                        const bool firstTexInit = isTex && !cell.Init;
                        const uint32_t sameQueueReads = cell.ReadStages[physQueue];
                        need = layoutChange || firstTexInit || priorWrite || sameQueueReads != 0 ||
                               cell.PendingImportSync;
                        if (cell.Init)
                        {
                            src.Stage = cell.WriteStage | sameQueueReads;
                            src.Access = cell.WriteAccess;
                        }
                        else
                        {
                            src.Stage = RGStage::BottomOfPipe; // recycled-memory exec dep
                            src.Access = RGAccessMask::None;
                        }
                        cell.WriteStage = c.Stage;
                        cell.WriteAccess = c.Access;
                        for (uint32_t q = 0; q < kQueueCount; ++q)
                        {
                            cell.ReadStages[q] = 0;
                            cell.ReadAccesses[q] = 0;
                        }
                        if (isTex)
                            cell.Layout = c.Layout;
                        cell.Queue = passQueue; // writes take ownership; reads never do
                        cell.Init = true;
                        cell.PendingImportSync = false; // intra-frame producer from here on
                    }
                    else if (layoutChange || (isTex && !cell.Init))
                    {
                        // Read needing a layout transition (or first-use read of an
                        // uninitialized texture). The transition becomes the new
                        // producer scope; this reader is covered by its dst scope.
                        if (!cell.Init && L == 0 && M == 0)
                        {
                            // Reading a never-written, non-imported texture: the
                            // Undefined transition is legal Vulkan but the content
                            // is garbage — almost certainly a missing producer.
                            Logger::Log::Warning(
                                "[RenderGraph] pass '{}' reads texture '{}' before anything writes it this frame",
                                m_Passes[p].Desc.Name ? m_Passes[p].Desc.Name : "<unnamed>",
                                m_Resources[c.Res].Name ? m_Resources[c.Res].Name : "<unnamed>");
                        }
                        need = true;
                        if (cell.Init)
                        {
                            // Same-queue producer scope. Both cross-queue halves are
                            // ordered by the submission plan: a reader AFTER this
                            // transition waits on it via the lastLayoutProducerSub edge
                            // (RAW-on-layout), and this transition drains earlier
                            // concurrent readers via the m_SubReadersSince edges
                            // (WAR-on-layout). The recording layer sanitizes this src
                            // scope to consumer-queue-valid stages when SrcQueue is
                            // foreign.
                            src.Stage = cell.WriteStage | cell.ReadStages[physQueue];
                            src.Access = cell.WriteAccess;
                        }
                        else
                        {
                            src.Stage = RGStage::BottomOfPipe;
                            src.Access = RGAccessMask::None;
                        }
                        cell.WriteStage = c.Stage;
                        cell.WriteAccess = c.Access;
                        for (uint32_t q = 0; q < kQueueCount; ++q)
                        {
                            cell.ReadStages[q] = 0;
                            cell.ReadAccesses[q] = 0;
                        }
                        cell.ReadStages[physQueue] = c.Stage;
                        cell.ReadAccesses[physQueue] = c.Access;
                        cell.Layout = c.Layout;
                        // A read-layout transition is a PRODUCER (it rewrites
                        // WriteStage/Layout): it must also take queue ownership so
                        // a later barrier that consumes this cell computes the
                        // cross-queue edge correctly. Without this, cell.Queue kept
                        // the prior writer's queue while WriteStage held this
                        // reader's (foreign) stage — a same-physical-queue barrier
                        // then emitted that foreign stage raw (VUID-09675).
                        cell.Queue = passQueue;
                        cell.Init = true;
                        cell.PendingImportSync = false; // the transition is the sync
                    }
                    else
                    {
                        // Pure read, stable layout: a barrier when this reader's
                        // stage/access is not yet covered by scopes already
                        // synchronized against the producer ON THIS QUEUE (RAW
                        // expansion), OR on the first touch of an imported/
                        // recycled resource. The producer gate is "any producer"
                        // (WriteAccess != 0) — a read-layout TRANSITION is a
                        // producer too, even though it stores read access bits.
                        const uint32_t missStage = c.Stage & ~cell.ReadStages[physQueue];
                        const uint32_t missAccess = c.Access & ~cell.ReadAccesses[physQueue];
                        const bool hasProducer = cell.WriteAccess != 0;
                        need = (hasProducer && (missStage != 0 || missAccess != 0)) ||
                               cell.PendingImportSync;
                        src.Stage = cell.WriteStage;
                        src.Access = cell.WriteAccess;
                        cell.ReadStages[physQueue] |= c.Stage;
                        cell.ReadAccesses[physQueue] |= c.Access;
                        if (need)
                            cell.PendingImportSync = false;
                        // cell.Queue unchanged: cross-queue reads need a semaphore
                        // (submission plan provides it), not an ownership transfer
                        // (resources use concurrent sharing until async lands).
                    }

                    if (!need)
                        continue;
                    int gi = -1;
                    for (size_t gg = 0; gg < groups.size(); ++gg)
                        if (groups[gg].Src == src)
                        {
                            gi = static_cast<int>(gg);
                            break;
                        }
                    if (gi < 0)
                    {
                        groups.push_back(BarrierGroup{src, static_cast<uint32_t>(needy.size())});
                        needy.resize(needy.size() + maskSize, 0);
                        gi = static_cast<int>(groups.size()) - 1;
                    }
                    groups[static_cast<size_t>(gi)].FirstTouch &= cellFirstTouch;
                    needy[groups[static_cast<size_t>(gi)].NeedyBase + L * rMips + M] = 1;
                }
            }

            // ── Emit rectangles per source-scope group ──
            for (const BarrierGroup& grp : groups)
            {
                const uint8_t* mask = needy.data() + grp.NeedyBase;
                runs.clear();
                runOff.resize(rLayers + 1);
                for (uint32_t L = 0; L < rLayers; ++L)
                {
                    runOff[L] = static_cast<uint32_t>(runs.size());
                    uint32_t M = 0;
                    while (M < rMips)
                    {
                        if (!mask[L * rMips + M])
                        {
                            ++M;
                            continue;
                        }
                        const uint32_t runStart = M;
                        while (M < rMips && mask[L * rMips + M])
                            ++M;
                        runs.push_back({runStart, M - runStart});
                    }
                }
                runOff[rLayers] = static_cast<uint32_t>(runs.size());
                const auto layerEqual = [&](uint32_t x, uint32_t y)
                {
                    const uint32_t bx = runOff[x];
                    const uint32_t by = runOff[y];
                    const uint32_t nx = runOff[x + 1] - bx;
                    if (nx != runOff[y + 1] - by)
                        return false;
                    for (uint32_t i = 0; i < nx; ++i)
                        if (runs[bx + i] != runs[by + i])
                            return false;
                    return true;
                };
                uint32_t L = 0;
                while (L < rLayers)
                {
                    if (runOff[L] == runOff[L + 1])
                    {
                        ++L;
                        continue;
                    }
                    uint32_t spanEnd = L + 1;
                    while (spanEnd < rLayers && layerEqual(spanEnd, L))
                        ++spanEnd;
                    for (uint32_t ri = runOff[L]; ri < runOff[L + 1]; ++ri)
                    {
                        const auto& run = runs[ri];
                        RGBarrier b;
                        b.Resource = c.Res;
                        b.IsTexture = isTex;
                        b.SrcStage = grp.Src.Stage;
                        b.DstStage = c.Stage;
                        b.SrcAccess = grp.Src.Access;
                        b.DstAccess = c.Access;
                        b.OldLayout = grp.Src.OldLayout;
                        b.NewLayout = isTex ? c.Layout : RGImageLayout::Undefined;
                        b.SrcQueue = grp.Src.Queue;
                        b.DstQueue = passQueue;
                        b.FirstTouch = grp.FirstTouch;
                        b.Range = RGRange{c.Range.BaseMip + run.first, run.second,
                                          c.Range.BaseLayer + L, spanEnd - L};
                        if (const char* watch = WatchedResourceName())
                        {
                            const char* rn = m_Resources[c.Res].Name ? m_Resources[c.Res].Name : "";
                            if (std::strstr(rn, watch))
                                Logger::Log::Warning(
                                    "[WatchImage] RG barrier: pass='{}' layout {}->{} mips[{}+{}] "
                                    "layers[{}+{}] firstTouch={}",
                                    m_Passes[p].Desc.Name ? m_Passes[p].Desc.Name : "<unnamed>",
                                    (int)b.OldLayout, (int)b.NewLayout, b.Range.BaseMip,
                                    b.Range.MipCount, b.Range.BaseLayer, b.Range.LayerCount,
                                    (int)b.FirstTouch);
                        }
                        m_Barriers.push_back(b);
                        srcUnion |= b.SrcStage;
                        dstUnion |= b.DstStage;
                    }
                    L = spanEnd;
                }
            }
        }

        const uint32_t batchCount = static_cast<uint32_t>(m_Barriers.size()) - batchFirst;
        if (batchCount > 0)
            m_BarrierBatches.push_back(RGBarrierBatch{p, si, batchFirst, batchCount, srcUnion, dstUnion});
    }

    // ── Export transitions: exported textures with a contractual final layout
    // get ONE trailing transition after their last access; cells are updated so
    // FinalLayout()/the pool write-back carry the exported layout. Emitted as a
    // sentinel batch (ScheduledIndex == pass count, Pass == kInvalidId); the
    // submission plan places each barrier at the tail of the latest submission
    // that accessed its resource (BarrierSubmissions) and derives the
    // RAW/WAR timeline waits a writing pass would get, so a compute-baked
    // export is legal without a hand-rolled graphics settle read. Src scopes
    // are own-queue (SrcQueue carries the owner; a foreign placement is
    // sanitized at recording); never BottomOfPipe (must not be hoisted). ──
    {
        const uint32_t exportFirst = static_cast<uint32_t>(m_Barriers.size());
        uint32_t srcUnion = RGStage::None;
        uint32_t dstUnion = RGStage::None;

        auto scopeForLayout = [](RGImageLayout t, uint32_t& stage, uint32_t& access)
        {
            switch (t)
            {
                case RGImageLayout::General:
                    stage = RGStage::FragmentShader | RGStage::ComputeShader;
                    access = RGAccessMask::ShaderRead | RGAccessMask::ShaderWrite;
                    break;
                case RGImageLayout::DepthAttachment:
                    stage = RGStage::EarlyFragmentTests | RGStage::LateFragmentTests;
                    access = RGAccessMask::DepthRead | RGAccessMask::DepthWrite;
                    break;
                case RGImageLayout::DepthReadOnly:
                    stage = RGStage::EarlyFragmentTests | RGStage::LateFragmentTests |
                            RGStage::FragmentShader;
                    access = RGAccessMask::DepthRead | RGAccessMask::ShaderRead;
                    break;
                case RGImageLayout::ColorAttachment:
                    stage = RGStage::ColorAttachmentOutput;
                    access = RGAccessMask::ColorRead | RGAccessMask::ColorWrite;
                    break;
                case RGImageLayout::TransferSrc:
                    stage = RGStage::Transfer;
                    access = RGAccessMask::TransferRead;
                    break;
                case RGImageLayout::TransferDst:
                    stage = RGStage::Transfer;
                    access = RGAccessMask::TransferWrite;
                    break;
                default: // ShaderReadOnly + anything else read-like
                    stage = RGStage::FragmentShader;
                    access = RGAccessMask::ShaderRead;
                    break;
            }
        };

        // `includeUninit`: the explicit-export path skips never-touched cells
        // (no contract over them); the NORMALIZE path transitions them too —
        // an untouched cell of a partially-written imported array must land
        // in the same single layout the pool stores.
        auto emitTransitions = [&](RGResourceId r, RGImageLayout target, bool includeUninit)
        {
            uint32_t dstStage = 0;
            uint32_t dstAccess = 0;
            scopeForLayout(target, dstStage, dstAccess);
            const uint32_t mips = m_CellMips[r];
            // Exports are 1-mip color targets in practice: merge consecutive
            // same-state LAYERS into one barrier then; per-cell otherwise.
            uint32_t c = m_CellOff[r];
            const uint32_t end = m_CellOff[r + 1];
            while (c < end)
            {
                RGCellState& cell = m_Cells[c];
                if ((!cell.Init && !includeUninit) || cell.Layout == target)
                {
                    ++c;
                    continue;
                }
                uint32_t run = c + 1;
                if (mips == 1)
                    while (run < end && m_Cells[run].Init == cell.Init &&
                           m_Cells[run].Layout == cell.Layout &&
                           m_Cells[run].WriteStage == cell.WriteStage &&
                           m_Cells[run].WriteAccess == cell.WriteAccess &&
                           m_Cells[run].Queue == cell.Queue)
                        ++run;
                RGBarrier b;
                b.Resource = r;
                b.IsTexture = true;
                b.SrcStage = cell.WriteStage | cell.ReadStages[physicalOfLogical[cell.Queue]];
                b.SrcAccess = cell.WriteAccess;
                b.DstStage = dstStage;
                b.DstAccess = dstAccess;
                b.OldLayout = cell.Layout;
                b.NewLayout = target;
                b.SrcQueue = cell.Queue;
                b.DstQueue = static_cast<uint32_t>(RGQueue::Graphics);
                const uint32_t rel = c - m_CellOff[r];
                b.Range = mips == 1 ? RGRange{0, 1, rel, run - c}
                                    : RGRange{rel % mips, 1, rel / mips, 1};
                if (const char* watch = WatchedResourceName())
                {
                    const char* rn = m_Resources[r].Name ? m_Resources[r].Name : "";
                    if (std::strstr(rn, watch))
                        Logger::Log::Warning(
                            "[WatchImage] RG barrier: pass='EXPORT' layout {}->{} mips[{}+{}] "
                            "layers[{}+{}]",
                            (int)b.OldLayout, (int)b.NewLayout, b.Range.BaseMip, b.Range.MipCount,
                            b.Range.BaseLayer, b.Range.LayerCount);
                }
                m_Barriers.push_back(b);
                srcUnion |= b.SrcStage;
                dstUnion |= b.DstStage;
                for (uint32_t k = c; k < run; ++k)
                    m_Cells[k].Layout = target;
                c = run;
            }
        };

        for (RGResourceId r = 0; r < resCount; ++r)
        {
            if (m_Resources[r].Kind != RGResourceKind::Texture || !m_ResourceUsedByLive[r])
                continue;
            const RGImageLayout exportTarget = m_ResourceExportLayout[r];
            if (exportTarget != RGImageLayout::Undefined)
            {
                emitTransitions(r, exportTarget, /*includeUninit=*/false);
                continue;
            }
            // NORMALIZE: an imported texture must end the frame at ONE layout
            // (the pool stores a single state; FinalLayout asserts uniformity).
            // Partial-write frames legitimately diverge — a cascade count
            // smaller than the array's layer count with no whole-resource
            // reader leaves written layers at DepthAttachment while the rest
            // keep the imported layout. Equalize to the first non-Undefined
            // layout; untouched/Undefined cells take a discard transition.
            if (!m_ResourceImported[r])
                continue;
            const uint32_t first = m_CellOff[r];
            const uint32_t end = m_CellOff[r + 1];
            bool heterogeneous = false;
            RGImageLayout target = RGImageLayout::Undefined;
            for (uint32_t c = first; c < end; ++c)
            {
                if (target == RGImageLayout::Undefined)
                    target = m_Cells[c].Layout;
                if (m_Cells[c].Layout != m_Cells[first].Layout)
                    heterogeneous = true;
            }
            if (heterogeneous && target != RGImageLayout::Undefined)
                emitTransitions(r, target, /*includeUninit=*/true);
        }

        const uint32_t exportCount = static_cast<uint32_t>(m_Barriers.size()) - exportFirst;
        if (exportCount > 0)
            m_BarrierBatches.push_back(RGBarrierBatch{kInvalidId,
                                                      static_cast<uint32_t>(m_ScheduledOrder.size()),
                                                      exportFirst, exportCount, srcUnion, dstUnion});
    }
}

} // namespace GameEngine::Rendering::RenderGraph

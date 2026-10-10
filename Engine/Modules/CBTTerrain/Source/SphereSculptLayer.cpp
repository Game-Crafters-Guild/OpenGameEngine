#include "CBTTerrain/SphereSculptLayer.h"

#include "CBTTerrain/SphereAnalyticModifiers.h" // the tangent-plane dab falloff (S3 preview form)
#include "Logger/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace GameEngine::CBTTerrain
{

uint32_t ResolveSculptPagePoolCount()
{
    // Read GE_SCULPT_PAGES once; a positive override wins outright, otherwise fit the pool to the
    // default memory budget. Both CBTResources (GPU pool ring size) and the CPU store call this, so
    // a page id addresses a live physical slot on both sides. Capped to the entry id field (the
    // top byte carries the S4 page level).
    static const uint32_t kResolved = [] {
        if (const char* env = std::getenv("GE_SCULPT_PAGES"))
        {
            const long n = std::atol(env);
            if (n > 0)
                return std::min(static_cast<uint32_t>(n), kSculptPageIdMask);
        }
        return DeriveSculptPagePoolCount(kSculptPagePoolBudgetBytes);
    }();
    return kResolved;
}

namespace
{

uint32_t ResolveSculptMaxPageLevel()
{
    // GE_SCULPT_MAX_LEVEL clamps the S4 escalation ceiling (0 disables escalation outright);
    // default = the layout cap. Read once — a per-layer override goes through SetMaxPageLevel.
    static const uint32_t kResolved = [] {
        if (const char* env = std::getenv("GE_SCULPT_MAX_LEVEL"))
        {
            const long n = std::atol(env);
            if (n >= 0)
                return std::min(static_cast<uint32_t>(n), kSculptMaxPageLevel);
        }
        return kSculptMaxPageLevel;
    }();
    return kResolved;
}

bool ResolveTangentDabFalloff()
{
    // GE_TERRAIN_TANGENT_DAB flips BASE-resolution dab writes to the tangent-plane falloff (the
    // S3 preview's closed form) — default OFF so an unflagged store's bytes stay pre-S4
    // byte-identical. Escalated writes always use the tangent form regardless (see the header).
    static const bool kResolved = [] {
        const char* v = std::getenv("GE_TERRAIN_TANGENT_DAB");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return kResolved;
}

// Pool slots a level-L block occupies (4^L).
uint32_t BlockSlotCount(uint32_t level)
{
    return 1u << (2u * level);
}

} // namespace

SphereSculptLayer::SphereSculptLayer()
    : m_MaxPageLevel(ResolveSculptMaxPageLevel()), m_TangentFalloff(ResolveTangentDabFalloff())
{
}

void SphereSculptLayer::SetMaxPageLevel(uint32_t level)
{
    m_MaxPageLevel = std::min(level, kSculptMaxPageLevel);
}

SphereEditRegions SphereSculptLayer::Configure(const SphereSculptGeometry& geom)
{
    if (geom.PoolPageCount == 0u || geom.VirtualDim < 2u)
        return {};
    if (geom.VirtualDim == m_Geom.VirtualDim && geom.PoolPageCount == m_Geom.PoolPageCount)
        return {}; // no change
    // A geometry change with authored content present (a live planet-radius edit re-deriving the
    // radius-scaled Dv) REMAPS the content onto the new grid — the sculpt is angular content, so
    // it stays put on the sphere while the resolution follows the new radius's m/texel budget.
    if (HasEdits())
        return RemapContent(geom);

    m_Geom = geom;
    m_Pool.assign(static_cast<size_t>(geom.PoolPageCount) * kSculptPageTexels, 0.0f);
    m_PageTable.assign(kSculptPageTableEntries, kSculptNoPage);
    m_PageLayers.clear();
    m_PageLayers.resize(geom.PoolPageCount); // unique_ptr is move-only -> resize (null-fills)
    m_PageMeta.assign(geom.PoolPageCount, PageMeta{});
    m_FreePages.clear();
    m_NextFreePage = 0u;
    m_AllocatedCount = 0u;
    m_EscalatedPageCount = 0u;
    m_Version = 0u;
    m_LastRegions = SphereEditRegions{};
    m_PrimaryFace = 0u;
    m_PoolExhausted = false;
    m_EscalationRefusedWarned = false;
    m_StrokeCaptureActive = false;
    m_StrokeCapture.clear();
    m_StrokeCaptureIndex.clear();
    m_StrokeCaptureLastEntry = kSculptNoPage;
    return {};
}

SphereEditRegions SphereSculptLayer::RemapContent(const SphereSculptGeometry& geom)
{
    // Move the old store aside so the new containers can build in place; on refusal it is moved
    // back untouched (the caller keeps the previous geometry + content — the pre-remap freeze).
    const SphereSculptGeometry oldGeom = m_Geom;
    std::vector<uint32_t> oldTable = std::move(m_PageTable);
    std::vector<std::unique_ptr<PageLayers>> oldLayers = std::move(m_PageLayers);
    std::vector<PageMeta> oldMeta = std::move(m_PageMeta);

    const float oldDimMinus1 = static_cast<float>(oldGeom.VirtualDim - 1u);
    const float newDimMinus1 = static_cast<float>(geom.VirtualDim - 1u);
    const float oldLastF = static_cast<float>(oldGeom.VirtualDim - 1u);
    const int32_t newLast = static_cast<int32_t>(geom.VirtualDim) - 1;

    // The set of NEW pages the old content can reach: each old allocated page's NON-ZERO content
    // rect (scanned per layer at the page's own fine grid — an all-zero page contributes
    // nothing), expanded by ONE fine texel (the bilinear fringe — a new texel up to one source
    // texel past a non-zero texel still interpolates against it), mapped through face UV (the
    // endpoint-exact texel<->UV convention uv = t/(Dv-1)) into new page indices. Marked pages
    // CARRY the max level of their contributing sources (escalated content keeps its density
    // across a resize; a new-grid face-BORDER page clamps to level 0 — the same cross-face
    // discipline PageMayEscalate enforces on live escalation). The marked SLOT total (4^level
    // per page) is the pool-feasibility gate; any page whose resample is non-zero is inside the
    // set (its source texels are within one source texel of it), so allocation cannot overflow.
    const uint32_t newPPA = geom.PagesPerAxis;
    std::vector<uint8_t> markedLevel(static_cast<size_t>(kCubeFaceCount) * newPPA * newPPA, 0xFFu);
    uint32_t neededSlots = 0u;
    for (uint32_t id = 0; id < oldGeom.PoolPageCount; ++id)
    {
        const PageMeta& meta = oldMeta[id];
        if (!meta.Allocated || meta.Follower)
            continue;
        // Local bounding rect of the page's authored content across BOTH layers, in FINE texels.
        const PageLayers& L = *oldLayers[id];
        const uint32_t fdim = SculptFineDim(meta.Level);
        const uint32_t step = 1u << meta.Level;
        const float stepInv = 1.0f / static_cast<float>(step);
        uint32_t lMinX = fdim, lMinY = fdim, lMaxX = 0u, lMaxY = 0u;
        for (uint32_t ly = 0; ly < fdim; ++ly)
        {
            for (uint32_t lx = 0; lx < fdim; ++lx)
            {
                const size_t li = static_cast<size_t>(ly) * fdim + lx;
                if (L.Dab[li] == 0.0f && L.Modifier[li] == 0.0f)
                    continue;
                lMinX = std::min(lMinX, lx);
                lMinY = std::min(lMinY, ly);
                lMaxX = std::max(lMaxX, lx);
                lMaxY = std::max(lMaxY, ly);
            }
        }
        if (lMinX > lMaxX)
            continue; // page holds no content (e.g. fully undone) — nothing to carry
        // Fringe = ONE BASE texel: within a page the bilinear reach is one fine texel, but across
        // a page seam a stored edge value reaches one full base cell (the seam-strip lerp), so
        // the base-texel fringe is the exact seam reach and merely conservative in the interior.
        const float colBase = static_cast<float>(meta.PageX * kSculptPageDim);
        const float rowBase = static_cast<float>(meta.PageY * kSculptPageDim);
        const float minTx =
            std::max(colBase + static_cast<float>(lMinX) * stepInv - 1.0f, 0.0f);
        const float minTy =
            std::max(rowBase + static_cast<float>(lMinY) * stepInv - 1.0f, 0.0f);
        const float maxTx =
            std::min(colBase + static_cast<float>(lMaxX) * stepInv + 1.0f, oldLastF);
        const float maxTy =
            std::min(rowBase + static_cast<float>(lMaxY) * stepInv + 1.0f, oldLastF);
        const float minU = minTx / oldDimMinus1;
        const float minV = minTy / oldDimMinus1;
        const float maxU = maxTx / oldDimMinus1;
        const float maxV = maxTy / oldDimMinus1;
        const uint32_t newMinPx = static_cast<uint32_t>(std::clamp<int32_t>(
                                      static_cast<int32_t>(std::floor(minU * newDimMinus1)), 0, newLast)) /
                                  kSculptPageDim;
        const uint32_t newMinPy = static_cast<uint32_t>(std::clamp<int32_t>(
                                      static_cast<int32_t>(std::floor(minV * newDimMinus1)), 0, newLast)) /
                                  kSculptPageDim;
        const uint32_t newMaxPx = static_cast<uint32_t>(std::clamp<int32_t>(
                                      static_cast<int32_t>(std::ceil(maxU * newDimMinus1)), 0, newLast)) /
                                  kSculptPageDim;
        const uint32_t newMaxPy = static_cast<uint32_t>(std::clamp<int32_t>(
                                      static_cast<int32_t>(std::ceil(maxV * newDimMinus1)), 0, newLast)) /
                                  kSculptPageDim;
        for (uint32_t py = newMinPy; py <= newMaxPy; ++py)
        {
            for (uint32_t px = newMinPx; px <= newMaxPx; ++px)
            {
                const bool border =
                    px == 0u || py == 0u || px + 1u >= newPPA || py + 1u >= newPPA;
                const uint8_t carried =
                    border ? static_cast<uint8_t>(0u)
                           : static_cast<uint8_t>(std::min<uint32_t>(meta.Level, m_MaxPageLevel));
                uint8_t& m = markedLevel[(static_cast<size_t>(meta.Face) * newPPA + py) * newPPA + px];
                if (m == 0xFFu)
                {
                    m = carried;
                    neededSlots += BlockSlotCount(carried);
                }
                else if (carried > m)
                {
                    neededSlots += BlockSlotCount(carried) - BlockSlotCount(m);
                    m = carried;
                }
            }
        }
    }

    if (neededSlots > geom.PoolPageCount)
    {
        // The resampled content cannot fit the physical pool (a large radius growth multiplies the
        // page count of the same angular footprint; escalated pages multiply slots per page).
        // Refuse the remap and keep the previous geometry + content untouched — no authored
        // height is ever lost to a resize.
        m_PageTable = std::move(oldTable);
        m_PageLayers = std::move(oldLayers);
        m_PageMeta = std::move(oldMeta);
        if (!m_RemapRefusedWarned)
        {
            m_RemapRefusedWarned = true;
            Logger::Log::Warning(
                "SphereSculptLayer: a sculpt resolution change (dim {} -> {}) would need {} physical "
                "pool slots but the pool holds {} — keeping the authored content at the original "
                "resolution. Raise GE_SCULPT_PAGES or reduce the sculpted area before resizing.",
                oldGeom.VirtualDim, geom.VirtualDim, neededSlots, geom.PoolPageCount);
        }
        return {};
    }

    // Level-aware sampling of one OLD authoring layer at face UV, through the SAME chokepoint
    // sampler the published pool uses (SampleSculptFaceUVWith — level-0 fast path expression-
    // identical to the pre-S4 remap): each layer is laid out into a pool-shaped scratch buffer at
    // the old blocks' slot offsets so dab and modifier stay separate through the remap.
    const size_t oldPoolFloats = static_cast<size_t>(oldGeom.PoolPageCount) * kSculptPageTexels;
    std::vector<float> oldDabPool(oldPoolFloats, 0.0f);
    std::vector<float> oldModPool(oldPoolFloats, 0.0f);
    for (uint32_t id = 0; id < oldGeom.PoolPageCount; ++id)
    {
        const PageMeta& meta = oldMeta[id];
        if (!meta.Allocated || meta.Follower)
            continue;
        const PageLayers& L = *oldLayers[id];
        std::copy(L.Dab.begin(), L.Dab.end(),
                  oldDabPool.begin() + static_cast<std::ptrdiff_t>(id) * kSculptPageTexels);
        std::copy(L.Modifier.begin(), L.Modifier.end(),
                  oldModPool.begin() + static_cast<std::ptrdiff_t>(id) * kSculptPageTexels);
    }
    const auto oldEntryAt = [&](uint32_t face, uint32_t px, uint32_t py)
    { return oldTable[face * kSculptPageTableFaceStride + py * oldGeom.Cap + px]; };
    auto sampleOld = [&](uint32_t face, float u, float v, bool dabLayer) -> float
    {
        return SampleSculptFaceUVWith(oldEntryAt, dabLayer ? oldDabPool.data() : oldModPool.data(),
                                      oldGeom, face, u, v);
    };

    // Adopt the new geometry and rebuild the physical store empty; the resample below re-fills it.
    const uint32_t version = m_Version; // survives the rebuild — a remap is an EDIT, not a reset
    m_Geom = geom;
    m_Pool.assign(static_cast<size_t>(geom.PoolPageCount) * kSculptPageTexels, 0.0f);
    m_PageTable.assign(kSculptPageTableEntries, kSculptNoPage);
    m_PageLayers.clear();
    m_PageLayers.resize(geom.PoolPageCount);
    m_PageMeta.assign(geom.PoolPageCount, PageMeta{});
    m_FreePages.clear();
    m_NextFreePage = 0u;
    m_AllocatedCount = 0u;
    m_EscalatedPageCount = 0u;
    m_PoolExhausted = false;

    // Resample every reachable new page at its CARRIED level. Amplitudes copy through unscaled:
    // heights are authored in absolute METRES (dab strength, modifier offsets), so a planet
    // resize keeps a 100 m mound 100 m tall — matching the planar heightfield, whose heights
    // don't scale with terrain size.
    struct FaceBounds
    {
        bool Any = false;
        uint32_t MinTx = 0u, MinTy = 0u, MaxTx = 0u, MaxTy = 0u;
    };
    std::array<FaceBounds, kCubeFaceCount> bounds{};
    std::vector<float> dabTmp; // heap: fine blocks reach 2 x ~1 MiB — far past the stack budget
    std::vector<float> modTmp;
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        for (uint32_t py = 0; py < newPPA; ++py)
        {
            for (uint32_t px = 0; px < newPPA; ++px)
            {
                const uint8_t carried =
                    markedLevel[(static_cast<size_t>(face) * newPPA + py) * newPPA + px];
                if (carried == 0xFFu)
                    continue;
                const uint32_t fdim = SculptFineDim(carried);
                const float stepInv = 1.0f / static_cast<float>(1u << carried);
                dabTmp.assign(static_cast<size_t>(fdim) * fdim, 0.0f);
                modTmp.assign(static_cast<size_t>(fdim) * fdim, 0.0f);
                bool any = false;
                for (uint32_t ly = 0; ly < fdim; ++ly)
                {
                    const float v = (static_cast<float>(py * kSculptPageDim) +
                                     static_cast<float>(ly) * stepInv) /
                                    newDimMinus1;
                    for (uint32_t lx = 0; lx < fdim; ++lx)
                    {
                        const float u = (static_cast<float>(px * kSculptPageDim) +
                                         static_cast<float>(lx) * stepInv) /
                                        newDimMinus1;
                        const float d = sampleOld(face, u, v, /*dabLayer=*/true);
                        const float m = sampleOld(face, u, v, /*dabLayer=*/false);
                        const size_t li = static_cast<size_t>(ly) * fdim + lx;
                        dabTmp[li] = d;
                        modTmp[li] = m;
                        any = any || d != 0.0f || m != 0.0f;
                    }
                }
                if (!any)
                    continue; // fringe page resampled to all-zero — stays sparse
                const uint32_t id =
                    AllocatePageBlock(face, px, py, carried); // cannot fail: needed <= pool
                PageLayers& L = *m_PageLayers[id];
                L.Dab = dabTmp;
                L.Modifier = modTmp;
                RecomposePageBlock(id);

                FaceBounds& b = bounds[face];
                const uint32_t minTx = px * kSculptPageDim;
                const uint32_t minTy = py * kSculptPageDim;
                const uint32_t maxTx = minTx + kSculptPageDim - 1u;
                const uint32_t maxTy = minTy + kSculptPageDim - 1u;
                if (!b.Any)
                {
                    b = FaceBounds{true, minTx, minTy, maxTx, maxTy};
                }
                else
                {
                    b.MinTx = std::min(b.MinTx, minTx);
                    b.MinTy = std::min(b.MinTy, minTy);
                    b.MaxTx = std::max(b.MaxTx, maxTx);
                    b.MaxTy = std::max(b.MaxTy, maxTy);
                }
            }
        }
    }

    // A mid-stroke resize (brush held down while the radius edit lands) invalidates the pre-images
    // captured so far — they name old-grid pages. Drop them but keep the capture ARMED: writes from
    // here capture fresh pre-images against the remapped state, so the stroke's eventual undo entry
    // restores to the post-remap store (valid), never to a mix of grids.
    if (m_StrokeCaptureActive)
    {
        m_StrokeCapture.clear();
        m_StrokeCaptureIndex.clear();
        m_StrokeCaptureLastEntry = kSculptNoPage;
    }
    m_RemapRefusedWarned = false; // a successful remap re-arms the refusal warning for a later resize

    // A remap IS an edit: advance the version (GPU upload gates + the editing-frames forced
    // VertexEval key on it) and publish covering regions (Classify re-tess + physics refresh).
    m_Version = version + 1u;
    SphereEditRegions regions{};
    const float invNewDimMinus1 = 1.0f / newDimMinus1;
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        const FaceBounds& b = bounds[face];
        if (!b.Any)
            continue;
        regions.Rects[regions.Count++] =
            SphereFaceUVRect{face, static_cast<float>(b.MinTx) * invNewDimMinus1,
                             static_cast<float>(b.MinTy) * invNewDimMinus1,
                             static_cast<float>(b.MaxTx) * invNewDimMinus1,
                             static_cast<float>(b.MaxTy) * invNewDimMinus1};
    }
    if (regions.Count > 0u)
    {
        m_LastRegions = regions;
        m_PrimaryFace = regions.Rects[0].Face;
    }
    Logger::Log::Info(
        "SphereSculptLayer: remapped authored sculpt across a resolution change (dim {} -> {}, {} "
        "pages resampled) — content kept at its angular position, amplitudes in absolute metres.",
        oldGeom.VirtualDim, geom.VirtualDim, m_AllocatedCount);
    return regions;
}

void SphereSculptLayer::Reset()
{
    m_Geom = SphereSculptGeometry{}; // PoolPageCount 0 -> unconfigured, the next Configure re-derives
    m_Pool.clear();
    m_PageTable.clear();
    m_PageLayers.clear();
    m_PageMeta.clear();
    m_FreePages.clear();
    m_NextFreePage = 0u;
    m_AllocatedCount = 0u;
    m_EscalatedPageCount = 0u;
    m_Version = 0u;
    m_LastRegions = SphereEditRegions{};
    m_PrimaryFace = 0u;
    m_PoolExhausted = false;
    m_RemapRefusedWarned = false;
    m_EscalationRefusedWarned = false;
    m_StrokeCaptureActive = false;
    m_StrokeCapture.clear();
    m_StrokeCaptureIndex.clear();
    m_StrokeCaptureLastEntry = kSculptNoPage;
}

uint32_t SphereSculptLayer::AllocatePageBlock(uint32_t face, uint32_t pageX, uint32_t pageY,
                                              uint32_t level, bool warnOnExhausted)
{
    const uint32_t slots = BlockSlotCount(level);
    uint32_t id = kSculptNoPage;
    if (slots == 1u)
    {
        // The pre-S4 single-slot path: free-list first, then bump (allocation ORDER is part of
        // the level-0 byte-lock — pool ids assign exactly as before).
        if (!m_FreePages.empty())
        {
            id = m_FreePages.back();
            m_FreePages.pop_back();
        }
        else if (m_NextFreePage < m_Geom.PoolPageCount)
        {
            id = m_NextFreePage++;
        }
    }
    else
    {
        // Escalated blocks need CONTIGUOUS slots (the fine grid is linear across the block, so
        // the GPU sampler addresses it from the base id alone). Bump region first; else scan the
        // free list for a run — the pool is small (hundreds of slots) and escalations rare, so a
        // sort + linear scan is cheap. Failure = refusal, never a scatter (refuse-don't-lose).
        if (m_NextFreePage + slots <= m_Geom.PoolPageCount)
        {
            id = m_NextFreePage;
            m_NextFreePage += slots;
        }
        else if (m_FreePages.size() >= slots)
        {
            std::sort(m_FreePages.begin(), m_FreePages.end());
            uint32_t runStart = 0u, runLen = 0u;
            size_t runBegin = 0u;
            for (size_t i = 0; i < m_FreePages.size(); ++i)
            {
                if (runLen == 0u || m_FreePages[i] != runStart + runLen)
                {
                    runStart = m_FreePages[i];
                    runLen = 1u;
                    runBegin = i;
                }
                else
                {
                    ++runLen;
                }
                if (runLen == slots)
                {
                    id = runStart;
                    m_FreePages.erase(m_FreePages.begin() + static_cast<std::ptrdiff_t>(runBegin),
                                      m_FreePages.begin() + static_cast<std::ptrdiff_t>(i + 1u));
                    break;
                }
            }
        }
    }
    if (id == kSculptNoPage)
    {
        if (warnOnExhausted && !m_PoolExhausted)
        {
            m_PoolExhausted = true;
            Logger::Log::Warning(
                "SphereSculptLayer: physical page pool cannot hold a {}-slot page block ({} slots "
                "of {}^2 texels total). Further sculpt writes at this footprint are REFUSED to "
                "protect authored content — no height is dropped. Raise GE_SCULPT_PAGES or reduce "
                "the sculpted area.",
                slots, m_Geom.PoolPageCount, kSculptPageDim);
        }
        return kSculptNoPage;
    }

    const uint32_t fdim = SculptFineDim(level);
    if (!m_PageLayers[id])
        m_PageLayers[id] = std::make_unique<PageLayers>();
    m_PageLayers[id]->Dab.assign(static_cast<size_t>(fdim) * fdim, 0.0f);
    m_PageLayers[id]->Modifier.assign(static_cast<size_t>(fdim) * fdim, 0.0f);
    std::fill_n(m_Pool.begin() + static_cast<std::ptrdiff_t>(id) * kSculptPageTexels,
                static_cast<std::ptrdiff_t>(slots) * kSculptPageTexels, 0.0f);
    m_PageMeta[id] = PageMeta{true, false, static_cast<uint8_t>(level), face, pageX, pageY};
    for (uint32_t i = 1u; i < slots; ++i)
        m_PageMeta[id + i] = PageMeta{true, true, static_cast<uint8_t>(level), face, pageX, pageY};
    m_PageTable[PageTableEntry(face, pageX, pageY)] = id | (level << kSculptPageLevelShift);
    m_AllocatedCount += slots;
    if (level > 0u)
        ++m_EscalatedPageCount;
    return id;
}

void SphereSculptLayer::FreePageBlock(uint32_t pageId)
{
    PageMeta& meta = m_PageMeta[pageId];
    if (!meta.Allocated || meta.Follower)
        return;
    const uint32_t level = meta.Level;
    const uint32_t slots = BlockSlotCount(level);
    // Clear the table entry only while it still points at THIS block — escalation replaces the
    // entry with the new block before retiring the old slots.
    uint32_t& entry = m_PageTable[PageTableEntry(meta.Face, meta.PageX, meta.PageY)];
    if (entry == (pageId | (level << kSculptPageLevelShift)))
        entry = kSculptNoPage;
    for (uint32_t i = 0u; i < slots; ++i)
    {
        m_PageMeta[pageId + i] = PageMeta{};
        m_FreePages.push_back(pageId + i);
    }
    if (m_PageLayers[pageId])
    {
        // Release the layer storage: block sizes vary per level, so a stale oversized buffer
        // would only be pinned memory (AllocatePageBlock reassigns on the next claim anyway).
        m_PageLayers[pageId]->Dab = {};
        m_PageLayers[pageId]->Modifier = {};
    }
    m_AllocatedCount -= slots;
    if (level > 0u)
        --m_EscalatedPageCount;
}

bool SphereSculptLayer::PageMayEscalate(uint32_t pageX, uint32_t pageY) const
{
    // Face-BORDER pages never escalate: a fine texel column on a cube edge has no counterpart on
    // the neighbouring face's (possibly coarser) grid, so the #488 shared-column write discipline
    // — the thing that makes cube edges crack-free — only holds at the base level. Interior page
    // seams are stitched by the sampler; the cross-face stitch is the S5 candidate.
    return pageX > 0u && pageY > 0u && pageX + 1u < m_Geom.PagesPerAxis &&
           pageY + 1u < m_Geom.PagesPerAxis;
}

void SphereSculptLayer::RecomposePageBlock(uint32_t pageId)
{
    const PageMeta& meta = m_PageMeta[pageId];
    const uint32_t fdim = SculptFineDim(meta.Level);
    const PageLayers& L = *m_PageLayers[pageId];
    const size_t base = static_cast<size_t>(pageId) * kSculptPageTexels;
    const size_t texels = static_cast<size_t>(fdim) * fdim;
    for (size_t i = 0; i < texels; ++i)
        m_Pool[base + i] = L.Dab[i] + L.Modifier[i];
}

bool SphereSculptLayer::TryEscalatePage(uint32_t face, uint32_t pageX, uint32_t pageY,
                                        uint32_t level)
{
    const uint32_t oldEntry = m_PageTable[PageTableEntry(face, pageX, pageY)];
    // The pre-image must be captured BEFORE the level changes, and must carry the modifier layer:
    // undoing a level-changing stroke rebuilds the block wholesale, which dab bytes alone cannot do.
    if (m_StrokeCaptureActive)
        CaptureStrokePage(face, pageX, pageY, oldEntry, /*withModifier=*/true);

    if (oldEntry == kSculptNoPage)
    {
        if (AllocatePageBlock(face, pageX, pageY, level, /*warnOnExhausted=*/false) !=
            kSculptNoPage)
            return true;
        if (!m_EscalationRefusedWarned)
        {
            m_EscalationRefusedWarned = true;
            Logger::Log::Warning(
                "SphereSculptLayer: page escalation to level {} refused — the pool has no "
                "contiguous {}-slot block free. The brush keeps writing at the base resolution "
                "(content is preserved; fine detail is quantized). Raise GE_SCULPT_PAGES or "
                "undo/erase escalated content to free blocks.",
                level, BlockSlotCount(level));
        }
        return false;
    }

    const uint32_t oldId = SculptEntryPageId(oldEntry);
    const uint32_t oldLevel = SculptEntryLevel(oldEntry);
    // Move the old block's layers aside; AllocatePageBlock below reassigns the base slot's
    // buffers, and the old block might even overlap the new one's slots after a free+realloc.
    std::vector<float> oldDab = std::move(m_PageLayers[oldId]->Dab);
    std::vector<float> oldMod = std::move(m_PageLayers[oldId]->Modifier);

    const uint32_t newId = AllocatePageBlock(face, pageX, pageY, level, /*warnOnExhausted=*/false);
    if (newId == kSculptNoPage)
    {
        m_PageLayers[oldId]->Dab = std::move(oldDab);
        m_PageLayers[oldId]->Modifier = std::move(oldMod);
        if (!m_EscalationRefusedWarned)
        {
            m_EscalationRefusedWarned = true;
            Logger::Log::Warning(
                "SphereSculptLayer: page escalation to level {} refused — the pool has no "
                "contiguous {}-slot block free. The brush keeps writing at the page's current "
                "resolution (content is preserved; fine detail is quantized). Raise "
                "GE_SCULPT_PAGES or undo/erase escalated content to free blocks.",
                level, BlockSlotCount(level));
        }
        return false;
    }

    // Upsample both authoring layers onto the finer endpoint-aligned grid: new fine texel j sits
    // at old fine position j / 2^(level-oldLevel) — bilinear between the two flanking old texels
    // per axis (exact copies at aligned positions). Amplitudes are metres; they copy through.
    const uint32_t oldFdim = SculptFineDim(oldLevel);
    const uint32_t newFdim = SculptFineDim(level);
    const uint32_t shift = level - oldLevel;
    const float scale = 1.0f / static_cast<float>(1u << shift);
    PageLayers& L = *m_PageLayers[newId];
    for (uint32_t jy = 0; jy < newFdim; ++jy)
    {
        const float sy = static_cast<float>(jy) * scale;
        const uint32_t k0 = std::min(static_cast<uint32_t>(sy), oldFdim - 1u);
        const uint32_t k1 = std::min(k0 + 1u, oldFdim - 1u);
        const float gy = sy - static_cast<float>(k0);
        for (uint32_t jx = 0; jx < newFdim; ++jx)
        {
            const float sx = static_cast<float>(jx) * scale;
            const uint32_t j0 = std::min(static_cast<uint32_t>(sx), oldFdim - 1u);
            const uint32_t j1 = std::min(j0 + 1u, oldFdim - 1u);
            const float gx = sx - static_cast<float>(j0);
            const size_t o00 = static_cast<size_t>(k0) * oldFdim + j0;
            const size_t o10 = static_cast<size_t>(k0) * oldFdim + j1;
            const size_t o01 = static_cast<size_t>(k1) * oldFdim + j0;
            const size_t o11 = static_cast<size_t>(k1) * oldFdim + j1;
            const size_t n = static_cast<size_t>(jy) * newFdim + jx;
            const float da = oldDab[o00] + (oldDab[o10] - oldDab[o00]) * gx;
            const float db = oldDab[o01] + (oldDab[o11] - oldDab[o01]) * gx;
            L.Dab[n] = da + (db - da) * gy;
            const float ma = oldMod[o00] + (oldMod[o10] - oldMod[o00]) * gx;
            const float mb = oldMod[o01] + (oldMod[o11] - oldMod[o01]) * gx;
            L.Modifier[n] = ma + (mb - ma) * gy;
        }
    }
    RecomposePageBlock(newId);
    FreePageBlock(oldId); // table entry already points at the new block — slots only
    return true;
}

void SphereSculptLayer::WriteDab(uint32_t face, uint32_t vtx, uint32_t vty, float delta)
{
    // Base-resolution write: on an escalated page this lands on the base-ALIGNED fine texel
    // (local << level). ApplyDab routes dabs overlapping escalated pages through the fine loop
    // instead, so in practice this only ever writes level-0 pages — kept general for safety.
    const uint32_t pageX = vtx / kSculptPageDim;
    const uint32_t pageY = vty / kSculptPageDim;
    const uint32_t level = [this, face, pageX, pageY] {
        const uint32_t e = m_PageTable[PageTableEntry(face, pageX, pageY)];
        return e == kSculptNoPage ? 0u : SculptEntryLevel(e);
    }();
    WriteDabFine(face, pageX, pageY, (vtx - pageX * kSculptPageDim) << level,
                 (vty - pageY * kSculptPageDim) << level, delta);
}

void SphereSculptLayer::WriteDabFine(uint32_t face, uint32_t pageX, uint32_t pageY, uint32_t jx,
                                     uint32_t jy, float delta)
{
    uint32_t entry = m_PageTable[PageTableEntry(face, pageX, pageY)];
    if (entry == kSculptNoPage)
    {
        if (delta == 0.0f)
            return; // do not materialize a zero write
        if (AllocatePageBlock(face, pageX, pageY, 0u) == kSculptNoPage)
            return; // pool exhausted — refuse, content preserved
        entry = m_PageTable[PageTableEntry(face, pageX, pageY)];
        if (m_StrokeCaptureActive)
            CaptureStrokePage(face, pageX, pageY, kSculptNoPage,
                              /*withModifier=*/false); // pre-image: "did not exist"
    }
    else if (m_StrokeCaptureActive)
    {
        CaptureStrokePage(face, pageX, pageY, entry, /*withModifier=*/false);
    }
    const uint32_t id = SculptEntryPageId(entry);
    const uint32_t fdim = SculptFineDim(SculptEntryLevel(entry));
    const size_t li = static_cast<size_t>(jy) * fdim + jx;
    PageLayers& L = *m_PageLayers[id];
    L.Dab[li] += delta;
    m_Pool[static_cast<size_t>(id) * kSculptPageTexels + li] = L.Dab[li] + L.Modifier[li];
}

void SphereSculptLayer::WriteModifier(uint32_t face, uint32_t vtx, uint32_t vty, float value)
{
    const uint32_t pageX = vtx / kSculptPageDim;
    const uint32_t pageY = vty / kSculptPageDim;
    const uint32_t level = [this, face, pageX, pageY] {
        const uint32_t e = m_PageTable[PageTableEntry(face, pageX, pageY)];
        return e == kSculptNoPage ? 0u : SculptEntryLevel(e);
    }();
    WriteModifierFine(face, pageX, pageY, (vtx - pageX * kSculptPageDim) << level,
                      (vty - pageY * kSculptPageDim) << level, value);
}

void SphereSculptLayer::WriteModifierFine(uint32_t face, uint32_t pageX, uint32_t pageY,
                                          uint32_t jx, uint32_t jy, float value)
{
    uint32_t entry = m_PageTable[PageTableEntry(face, pageX, pageY)];
    if (entry == kSculptNoPage)
    {
        if (value == 0.0f)
            return; // published would stay 0 — keep the page unallocated (sparse)
        if (AllocatePageBlock(face, pageX, pageY, 0u) == kSculptNoPage)
            return; // pool exhausted — refuse
        entry = m_PageTable[PageTableEntry(face, pageX, pageY)];
    }
    const uint32_t id = SculptEntryPageId(entry);
    const uint32_t fdim = SculptFineDim(SculptEntryLevel(entry));
    const size_t li = static_cast<size_t>(jy) * fdim + jx;
    PageLayers& L = *m_PageLayers[id];
    L.Modifier[li] = value;
    m_Pool[static_cast<size_t>(id) * kSculptPageTexels + li] = L.Dab[li] + L.Modifier[li];
}

bool SphereSculptLayer::ClearAllModifierLayers()
{
    bool changed = false;
    for (uint32_t id = 0; id < m_Geom.PoolPageCount; ++id)
    {
        const PageMeta& meta = m_PageMeta[id];
        if (!meta.Allocated || meta.Follower)
            continue;
        PageLayers& L = *m_PageLayers[id];
        const uint32_t fdim = SculptFineDim(meta.Level);
        const size_t texels = static_cast<size_t>(fdim) * fdim;
        const size_t base = static_cast<size_t>(id) * kSculptPageTexels;
        bool anyDab = false;
        for (size_t li = 0; li < texels; ++li)
        {
            if (L.Modifier[li] != 0.0f)
            {
                L.Modifier[li] = 0.0f;
                m_Pool[base + li] = L.Dab[li];
                changed = true;
            }
            if (L.Dab[li] != 0.0f)
                anyDab = true;
        }
        // A page left with neither dab nor modifier content is no longer authored — reclaim it so
        // repeated modifier moves (each a full re-bake) do not leak the pool toward exhaustion.
        if (!anyDab)
            FreePageBlock(id);
    }
    return changed;
}

SphereEditRegions SphereSculptLayer::ApplyDab(float cx, float cy, float cz, float angularRadius,
                                              float strength, bool lower)
{
    SphereEditRegions regions{};
    if (m_Geom.PoolPageCount == 0u)
        return regions;
    const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (clen <= 0.0f || angularRadius <= 0.0f)
        return regions;

    const float nx = cx / clen, ny = cy / clen, nz = cz / clen;
    regions = ClassifySphereCapEdit(nx, ny, nz, angularRadius);
    m_PrimaryFace = WorldDirToFaceUV(nx, ny, nz).Face;

    const float sign = lower ? -1.0f : 1.0f;
    const float cosR = std::cos(angularRadius);
    const int32_t dim = static_cast<int32_t>(m_Geom.VirtualDim);
    const float invDimMinus1 = 1.0f / static_cast<float>(dim - 1);

    // S4 escalation decision, once per dab: a footprint sub-representable at the base grid asks
    // for the smallest level that lifts it to kSculptMinDabFootprintTexels (0 = stay base — the
    // dark-ship trigger: resolvable brushes NEVER escalate). Escalated writes always use the
    // tangent-plane falloff: escalation exists exactly where the legacy acos form has collapsed
    // in fp32 (cos(angR) rounds to 1.0f), so committing acos values at a fine grid would write
    // a full-amplitude plateau instead of the brush's cone.
    const uint32_t desiredLevel =
        DesiredSculptDabLevel(angularRadius, m_Geom.VirtualDim, m_MaxPageLevel);
    const bool tangent = m_TangentFalloff || desiredLevel > 0u;
    // R=1: the tangent falloff argument is scale-invariant (the radius cancels in q/sin(angR)).
    const SphereAnalyticDab tangentDab =
        MakeSphereAnalyticDab(nx, ny, nz, angularRadius, sign * strength, 1.0f);
    const auto evalDelta = [&](float dx, float dy, float dz) -> float
    {
        if (tangent)
            return EvaluateSphereAnalyticDab(tangentDab, dx, dy, dz);
        const float cosd = dx * nx + dy * ny + dz * nz;
        if (cosd < cosR)
            return 0.0f; // outside the cap
        const float ang = std::acos(std::clamp(cosd, -1.0f, 1.0f));
        const float t = std::clamp(1.0f - ang / angularRadius, 0.0f, 1.0f);
        return sign * strength * (t * t * (3.0f - 2.0f * t)); // smoothstep
    };
    // Fine (per-page) writes are needed as soon as escalated pages can be involved — either this
    // dab escalates or earlier ones left escalated pages a broad brush must now write at THEIR
    // resolution (base-aligned writes into a fine grid would leave the unaligned texels stale).
    const bool finePath = desiredLevel > 0u || m_EscalatedPageCount > 0u;

    // The write rect must cover the TRUE cap footprint, not the sampled AABB from
    // ClassifySphereCapEdit (which undershoots the toward-edge extent by up to ~0.13*angularRadius).
    // Inflate the rect and let the per-texel support test decide the actual writes: the falloff
    // is a pure function of the texel's DIRECTION and both faces' shared-edge texel grids sample the
    // SAME directions, so the shared column is written to the identical value on both faces whenever
    // the cap covers it -> crack-free by construction. Margin scales with the virtual dim.
    const int32_t margin =
        static_cast<int32_t>(std::ceil(0.3f * angularRadius * static_cast<float>(dim - 1))) + 2;
    for (uint32_t ri = 0; ri < regions.Count; ++ri)
    {
        const SphereFaceUVRect& r = regions.Rects[ri];
        if (r.IsEmpty())
            continue;
        const int32_t minTx = ClampTexel(static_cast<int32_t>(std::floor(r.MinU * (dim - 1))) - margin);
        const int32_t maxTx = ClampTexel(static_cast<int32_t>(std::ceil(r.MaxU * (dim - 1))) + margin);
        const int32_t minTz = ClampTexel(static_cast<int32_t>(std::floor(r.MinV * (dim - 1))) - margin);
        const int32_t maxTz = ClampTexel(static_cast<int32_t>(std::ceil(r.MaxV * (dim - 1))) + margin);
        if (!finePath && !tangent)
        {
            // The pre-S4 base-resolution loop, byte-identical (values, write order, page
            // materialization order — the level-0 dark-ship lock).
            for (int32_t tz = minTz; tz <= maxTz; ++tz)
            {
                const float faceV = static_cast<float>(tz) * invDimMinus1;
                for (int32_t tx = minTx; tx <= maxTx; ++tx)
                {
                    const float faceU = static_cast<float>(tx) * invDimMinus1;
                    float dx, dy, dz;
                    FaceUVToWorldDir(r.Face, faceU, faceV, dx, dy, dz);
                    const float cosd = dx * nx + dy * ny + dz * nz;
                    if (cosd < cosR)
                        continue; // texel outside the cap
                    const float ang = std::acos(std::clamp(cosd, -1.0f, 1.0f));
                    const float t = std::clamp(1.0f - ang / angularRadius, 0.0f, 1.0f);
                    const float falloff = t * t * (3.0f - 2.0f * t); // smoothstep
                    const float delta = sign * strength * falloff;
                    WriteDab(r.Face, static_cast<uint32_t>(tx), static_cast<uint32_t>(tz), delta);
                }
            }
        }
        else if (!finePath)
        {
            // Tangent falloff at base resolution (GE_TERRAIN_TANGENT_DAB, no escalated pages):
            // the same rect walk, values from the preview's closed form.
            for (int32_t tz = minTz; tz <= maxTz; ++tz)
            {
                const float faceV = static_cast<float>(tz) * invDimMinus1;
                for (int32_t tx = minTx; tx <= maxTx; ++tx)
                {
                    const float faceU = static_cast<float>(tx) * invDimMinus1;
                    float dx, dy, dz;
                    FaceUVToWorldDir(r.Face, faceU, faceV, dx, dy, dz);
                    const float delta = evalDelta(dx, dy, dz);
                    if (delta != 0.0f)
                        WriteDab(r.Face, static_cast<uint32_t>(tx), static_cast<uint32_t>(tz),
                                 delta);
                }
            }
        }
        else
        {
            // Per-page fine writes: each page is written at its OWN level; pages the cap
            // actually touches escalate to desiredLevel first (probe-then-escalate, so a page
            // the margin merely grazes never burns a 4^L block).
            for (int32_t py = minTz / static_cast<int32_t>(kSculptPageDim);
                 py <= maxTz / static_cast<int32_t>(kSculptPageDim); ++py)
            {
                const int32_t rowBase = py * static_cast<int32_t>(kSculptPageDim);
                const int32_t qa = std::max(minTz, rowBase);
                const int32_t qb = std::min(maxTz, rowBase + static_cast<int32_t>(kSculptPageDim) - 1);
                for (int32_t px = minTx / static_cast<int32_t>(kSculptPageDim);
                     px <= maxTx / static_cast<int32_t>(kSculptPageDim); ++px)
                {
                    const int32_t colBase = px * static_cast<int32_t>(kSculptPageDim);
                    const int32_t pa = std::max(minTx, colBase);
                    const int32_t pb =
                        std::min(maxTx, colBase + static_cast<int32_t>(kSculptPageDim) - 1);
                    const uint32_t upx = static_cast<uint32_t>(px);
                    const uint32_t upy = static_cast<uint32_t>(py);
                    const uint32_t entry = m_PageTable[PageTableEntry(r.Face, upx, upy)];
                    uint32_t level = entry == kSculptNoPage ? 0u : SculptEntryLevel(entry);
                    if (desiredLevel > level && PageMayEscalate(upx, upy))
                    {
                        // Probe at the desired level: does the cap write anything here at all?
                        bool touches = false;
                        const uint32_t pStep = 1u << desiredLevel;
                        const float pStepInv = 1.0f / static_cast<float>(pStep);
                        for (uint32_t jy = static_cast<uint32_t>(qa - rowBase) * pStep;
                             !touches && jy <= static_cast<uint32_t>(qb - rowBase) * pStep; ++jy)
                        {
                            const float faceV = (static_cast<float>(rowBase) +
                                                 static_cast<float>(jy) * pStepInv) *
                                                invDimMinus1;
                            for (uint32_t jx = static_cast<uint32_t>(pa - colBase) * pStep;
                                 jx <= static_cast<uint32_t>(pb - colBase) * pStep; ++jx)
                            {
                                const float faceU = (static_cast<float>(colBase) +
                                                     static_cast<float>(jx) * pStepInv) *
                                                    invDimMinus1;
                                float dx, dy, dz;
                                FaceUVToWorldDir(r.Face, faceU, faceV, dx, dy, dz);
                                if (evalDelta(dx, dy, dz) != 0.0f)
                                {
                                    touches = true;
                                    break;
                                }
                            }
                        }
                        if (touches && TryEscalatePage(r.Face, upx, upy, desiredLevel))
                            level = desiredLevel;
                    }
                    const uint32_t step = 1u << level;
                    const float stepInv = 1.0f / static_cast<float>(step);
                    for (uint32_t jy = static_cast<uint32_t>(qa - rowBase) * step;
                         jy <= static_cast<uint32_t>(qb - rowBase) * step; ++jy)
                    {
                        const float faceV =
                            (static_cast<float>(rowBase) + static_cast<float>(jy) * stepInv) *
                            invDimMinus1;
                        for (uint32_t jx = static_cast<uint32_t>(pa - colBase) * step;
                             jx <= static_cast<uint32_t>(pb - colBase) * step; ++jx)
                        {
                            const float faceU =
                                (static_cast<float>(colBase) + static_cast<float>(jx) * stepInv) *
                                invDimMinus1;
                            float dx, dy, dz;
                            FaceUVToWorldDir(r.Face, faceU, faceV, dx, dy, dz);
                            const float delta = evalDelta(dx, dy, dz);
                            if (delta != 0.0f)
                                WriteDabFine(r.Face, upx, upy, jx, jy, delta);
                        }
                    }
                }
            }
        }
    }

    ++m_Version;
    m_LastRegions = regions;
    return regions;
}

void SphereSculptLayer::BeginStrokeCapture()
{
    // A Begin while a capture is active continues the existing capture (the pre-images already
    // recorded stay authoritative), so a lost mouse-up cannot drop a stroke's undo payload.
    if (m_StrokeCaptureActive)
        return;
    m_StrokeCaptureActive = true;
    m_StrokeCapture.clear();
    m_StrokeCaptureIndex.clear();
    m_StrokeCaptureLastEntry = kSculptNoPage;
}

std::vector<SphereSculptPageState> SphereSculptLayer::TakeStrokeCapture()
{
    m_StrokeCaptureActive = false;
    m_StrokeCaptureIndex.clear();
    m_StrokeCaptureLastEntry = kSculptNoPage;
    std::vector<SphereSculptPageState> out = std::move(m_StrokeCapture);
    m_StrokeCapture.clear();
    return out;
}

void SphereSculptLayer::CaptureStrokePage(uint32_t face, uint32_t pageX, uint32_t pageY,
                                          uint32_t entry, bool withModifier)
{
    const uint32_t tableIndex = PageTableEntry(face, pageX, pageY);
    if (tableIndex == m_StrokeCaptureLastEntry && !withModifier)
        return; // hot path: consecutive texel writes inside the page already captured
    const auto it = m_StrokeCaptureIndex.find(tableIndex);
    if (it != m_StrokeCaptureIndex.end())
    {
        // Already captured this stroke. An escalation touch UPGRADES the recorded pre-image with
        // the modifier bytes it will need for the level-crossing restore: strokes never write the
        // modifier layer and TryEscalatePage captures BEFORE re-allocating, so the live modifier
        // bytes at this moment still are the pre-stroke bytes at the recorded level.
        if (withModifier)
        {
            SphereSculptPageState& state = m_StrokeCapture[it->second];
            const uint32_t liveEntry = m_PageTable[tableIndex];
            if (state.Allocated && !state.HasModifier && liveEntry != kSculptNoPage)
            {
                state.HasModifier = true;
                state.Modifier = m_PageLayers[SculptEntryPageId(liveEntry)]->Modifier;
            }
        }
        m_StrokeCaptureLastEntry = tableIndex;
        return;
    }
    SphereSculptPageState state;
    state.Face = face;
    state.PageX = pageX;
    state.PageY = pageY;
    state.VirtualDim = m_Geom.VirtualDim;
    if (entry != kSculptNoPage)
    {
        const uint32_t id = SculptEntryPageId(entry);
        state.Allocated = true;
        state.Level = SculptEntryLevel(entry);
        state.Dab = m_PageLayers[id]->Dab;
        if (withModifier)
        {
            state.HasModifier = true;
            state.Modifier = m_PageLayers[id]->Modifier;
        }
    }
    m_StrokeCaptureIndex.emplace(tableIndex, m_StrokeCapture.size());
    m_StrokeCapture.push_back(std::move(state));
    m_StrokeCaptureLastEntry = tableIndex;
}

std::vector<SphereSculptPageState> SphereSculptLayer::SnapshotPages(
    const std::vector<SphereSculptPageState>& keys) const
{
    std::vector<SphereSculptPageState> out;
    out.reserve(keys.size());
    for (const SphereSculptPageState& key : keys)
    {
        SphereSculptPageState state;
        state.Face = key.Face;
        state.PageX = key.PageX;
        state.PageY = key.PageY;
        state.VirtualDim = m_Geom.VirtualDim;
        if (m_Geom.PoolPageCount != 0u && key.PageX < m_Geom.PagesPerAxis &&
            key.PageY < m_Geom.PagesPerAxis)
        {
            const uint32_t entry = m_PageTable[PageTableEntry(key.Face, key.PageX, key.PageY)];
            if (entry != kSculptNoPage)
            {
                const uint32_t id = SculptEntryPageId(entry);
                state.Allocated = true;
                state.Level = SculptEntryLevel(entry);
                state.Dab = m_PageLayers[id]->Dab;
                // A snapshot whose level differs from its pre-image key restores across a level
                // change (redoing an escalating stroke), which needs the modifier bytes for the
                // wholesale rebuild — capture them symmetrically with the escalating pre-image.
                if (key.HasModifier || state.Level != key.Level)
                {
                    state.HasModifier = true;
                    state.Modifier = m_PageLayers[id]->Modifier;
                }
            }
        }
        out.push_back(std::move(state));
    }
    return out;
}

SphereEditRegions SphereSculptLayer::RestoreDabPages(const std::vector<SphereSculptPageState>& pages)
{
    SphereEditRegions regions{};
    if (m_Geom.PoolPageCount == 0u || pages.empty())
        return regions;

    // Per-face virtual-texel bounds of everything restored, unioned into <= 6 face rects below.
    struct FaceBounds
    {
        bool Any = false;
        uint32_t MinTx = 0u, MinTy = 0u, MaxTx = 0u, MaxTy = 0u;
    };
    std::array<FaceBounds, kCubeFaceCount> bounds{};
    bool anyRestored = false;

    for (const SphereSculptPageState& p : pages)
    {
        // Stale snapshots from a different page grid are refused per page: a planet resize
        // remapped the store since capture (same (face,pageX,pageY) now names a different angular
        // rect — restoring old-grid bytes would misplace content), or the planet was deleted +
        // recreated while the entry sat on the undo stack. Grid-keyed (not epoch-keyed) so undoing
        // the resize itself makes the older entries valid again. The range check keeps pre-grid
        // snapshots (VirtualDim 0) and shrunken grids from indexing out of range.
        if (p.VirtualDim != m_Geom.VirtualDim || p.Face >= kCubeFaceCount ||
            p.PageX >= m_Geom.PagesPerAxis || p.PageY >= m_Geom.PagesPerAxis)
            continue;
        const uint32_t entry = m_PageTable[PageTableEntry(p.Face, p.PageX, p.PageY)];
        if (p.Allocated)
        {
            const size_t expected =
                static_cast<size_t>(SculptFineDim(p.Level)) * SculptFineDim(p.Level);
            if (p.Level > kSculptMaxPageLevel || p.Dab.size() != expected ||
                (p.HasModifier && p.Modifier.size() != expected))
                continue; // malformed snapshot — refuse rather than corrupt the store
            uint32_t id;
            if (entry == kSculptNoPage)
            {
                // The page was reclaimed since capture (e.g. redo after an undo freed it):
                // re-materialize at the snapshot's level. A fresh block's modifier layer is
                // zero, which is exactly the state a reclaim implies (pages with modifier
                // content are never reclaimed).
                id = AllocatePageBlock(p.Face, p.PageX, p.PageY, p.Level);
                if (id == kSculptNoPage)
                    continue; // pool exhausted — restore refused for this page (warned once)
            }
            else if (SculptEntryLevel(entry) == p.Level)
            {
                id = SculptEntryPageId(entry);
            }
            else if (p.HasModifier)
            {
                // Level change (undoing/redoing a stroke that escalated this page): rebuild the
                // block wholesale at the snapshot's level from BOTH captured layers.
                FreePageBlock(SculptEntryPageId(entry));
                id = AllocatePageBlock(p.Face, p.PageX, p.PageY, p.Level);
                if (id == kSculptNoPage)
                    continue; // pool cannot hold the block — restore refused for this page (warned)
            }
            else
            {
                // Dab-only snapshot against a live page at a DIFFERENT level: the modifier layer
                // cannot be reconstructed at the snapshot's level, so this page is refused (the
                // stale-level refusal). With the undo stack's LIFO discipline the level-changing
                // entry is the one carrying the modifier bytes, so this only guards malformed or
                // out-of-order stacks — the same per-page-refusal posture as the grid key.
                continue;
            }
            PageLayers& L = *m_PageLayers[id];
            L.Dab = p.Dab;
            if (p.HasModifier)
                L.Modifier = p.Modifier;
            RecomposePageBlock(id);
        }
        else
        {
            if (entry == kSculptNoPage)
                continue; // already unallocated — nothing changes for this page
            const uint32_t id = SculptEntryPageId(entry);
            PageLayers& L = *m_PageLayers[id];
            std::fill(L.Dab.begin(), L.Dab.end(), 0.0f);
            // Publish the dab-less block BEFORE any reclaim so the pool bytes of a freed block
            // read as the modifier layer (zeros when none) — undo must leave the raw pool
            // byte-identical to pre-stroke, and a freed slot's stale dab bytes would not be.
            RecomposePageBlock(id);
            bool anyModifier = false;
            for (const float v : L.Modifier)
            {
                if (v != 0.0f)
                {
                    anyModifier = true;
                    break;
                }
            }
            // The stroke materialized this page; with the dab gone and no modifier content it is
            // no longer authored — reclaim it so undone strokes don't leak the pool.
            if (!anyModifier)
                FreePageBlock(id);
        }

        anyRestored = true;
        FaceBounds& b = bounds[p.Face];
        const uint32_t minTx = p.PageX * kSculptPageDim;
        const uint32_t minTy = p.PageY * kSculptPageDim;
        const uint32_t maxTx = std::min(minTx + kSculptPageDim - 1u, m_Geom.VirtualDim - 1u);
        const uint32_t maxTy = std::min(minTy + kSculptPageDim - 1u, m_Geom.VirtualDim - 1u);
        if (!b.Any)
        {
            b = FaceBounds{true, minTx, minTy, maxTx, maxTy};
        }
        else
        {
            b.MinTx = std::min(b.MinTx, minTx);
            b.MinTy = std::min(b.MinTy, minTy);
            b.MaxTx = std::max(b.MaxTx, maxTx);
            b.MaxTy = std::max(b.MaxTy, maxTy);
        }
    }

    if (!anyRestored)
        return regions;

    // A restore IS an edit: advance the version (GPU upload gates + the editing-frames forced
    // VertexEval key on it) and publish covering regions (Classify re-tess + physics refresh).
    ++m_Version;
    const float invDimMinus1 = 1.0f / static_cast<float>(m_Geom.VirtualDim - 1u);
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        const FaceBounds& b = bounds[face];
        if (!b.Any)
            continue;
        regions.Rects[regions.Count++] =
            SphereFaceUVRect{face, static_cast<float>(b.MinTx) * invDimMinus1,
                             static_cast<float>(b.MinTy) * invDimMinus1,
                             static_cast<float>(b.MaxTx) * invDimMinus1,
                             static_cast<float>(b.MaxTy) * invDimMinus1};
    }
    m_LastRegions = regions;
    if (regions.Count > 0u)
        m_PrimaryFace = regions.Rects[0].Face;
    return regions;
}

std::vector<SphereSculptPageContent> SphereSculptLayer::ExportPages() const
{
    std::vector<SphereSculptPageContent> out;
    out.reserve(m_AllocatedCount);
    for (uint32_t id = 0; id < m_Geom.PoolPageCount; ++id)
    {
        const PageMeta& meta = m_PageMeta[id];
        if (!meta.Allocated || meta.Follower)
            continue;
        SphereSculptPageContent page;
        page.Face = meta.Face;
        page.PageX = meta.PageX;
        page.PageY = meta.PageY;
        page.Level = meta.Level;
        const PageLayers& L = *m_PageLayers[id];
        page.Dab = L.Dab;
        page.Modifier = L.Modifier;
        out.push_back(std::move(page));
    }
    return out;
}

SphereEditRegions SphereSculptLayer::ImportPages(const std::vector<SphereSculptPageContent>& pages)
{
    SphereEditRegions regions{};
    if (m_Geom.PoolPageCount == 0u || pages.empty())
        return regions;

    struct FaceBounds
    {
        bool Any = false;
        uint32_t MinTx = 0u, MinTy = 0u, MaxTx = 0u, MaxTy = 0u;
    };
    std::array<FaceBounds, kCubeFaceCount> bounds{};
    bool anyImported = false;

    for (const SphereSculptPageContent& p : pages)
    {
        // Out-of-grid keys are skipped per page (a snapshot from a larger saved grid fed to a
        // smaller store must never index out of range) — the same discipline as RestoreDabPages.
        // Malformed levels / layer sizes are refused per page too (the .tsculpt decoder already
        // validates, this guards direct callers).
        if (p.Face >= kCubeFaceCount || p.PageX >= m_Geom.PagesPerAxis ||
            p.PageY >= m_Geom.PagesPerAxis || p.Level > kSculptMaxPageLevel)
            continue;
        const size_t expected =
            static_cast<size_t>(SculptFineDim(p.Level)) * SculptFineDim(p.Level);
        if (p.Dab.size() != expected || p.Modifier.size() != expected)
            continue;
        uint32_t entry = m_PageTable[PageTableEntry(p.Face, p.PageX, p.PageY)];
        if (entry != kSculptNoPage && SculptEntryLevel(entry) != p.Level)
        {
            // The import overwrites the page wholesale at the snapshot's level.
            FreePageBlock(SculptEntryPageId(entry));
            entry = kSculptNoPage;
        }
        uint32_t id;
        if (entry == kSculptNoPage)
        {
            id = AllocatePageBlock(p.Face, p.PageX, p.PageY, p.Level);
            if (id == kSculptNoPage)
                continue; // pool exhausted — this page refused (warned once), the rest intact
        }
        else
        {
            id = SculptEntryPageId(entry);
        }
        PageLayers& L = *m_PageLayers[id];
        L.Dab = p.Dab;
        L.Modifier = p.Modifier;
        RecomposePageBlock(id);

        anyImported = true;
        FaceBounds& b = bounds[p.Face];
        const uint32_t minTx = p.PageX * kSculptPageDim;
        const uint32_t minTy = p.PageY * kSculptPageDim;
        const uint32_t maxTx = std::min(minTx + kSculptPageDim - 1u, m_Geom.VirtualDim - 1u);
        const uint32_t maxTy = std::min(minTy + kSculptPageDim - 1u, m_Geom.VirtualDim - 1u);
        if (!b.Any)
        {
            b = FaceBounds{true, minTx, minTy, maxTx, maxTy};
        }
        else
        {
            b.MinTx = std::min(b.MinTx, minTx);
            b.MinTy = std::min(b.MinTy, minTy);
            b.MaxTx = std::max(b.MaxTx, maxTx);
            b.MaxTy = std::max(b.MaxTy, maxTy);
        }
    }

    if (!anyImported)
        return regions;

    // A load IS an edit: advance the version (GPU upload gates + the editing-frames forced
    // VertexEval key on it) and publish covering regions (Classify re-tess + physics refresh) —
    // the same contract RestoreDabPages and RemapContent honour.
    ++m_Version;
    const float invDimMinus1 = 1.0f / static_cast<float>(m_Geom.VirtualDim - 1u);
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        const FaceBounds& b = bounds[face];
        if (!b.Any)
            continue;
        regions.Rects[regions.Count++] =
            SphereFaceUVRect{face, static_cast<float>(b.MinTx) * invDimMinus1,
                             static_cast<float>(b.MinTy) * invDimMinus1,
                             static_cast<float>(b.MaxTx) * invDimMinus1,
                             static_cast<float>(b.MaxTy) * invDimMinus1};
    }
    m_LastRegions = regions;
    if (regions.Count > 0u)
        m_PrimaryFace = regions.Rects[0].Face;
    return regions;
}

uint32_t SphereSculptLayer::AllocatedPageCountForFace(uint32_t face) const
{
    uint32_t count = 0u;
    for (uint32_t id = 0; id < m_Geom.PoolPageCount; ++id)
        if (m_PageMeta[id].Allocated && m_PageMeta[id].Face == face)
            ++count;
    return count;
}

bool SphereSculptLayer::DirtyFaceRect(uint32_t& outFace, float& minU, float& minV, float& maxU,
                                      float& maxV) const
{
    if (m_Version == 0u || m_LastRegions.Count == 0u)
        return false;
    // Prefer the centre-face region so the reclassification lands on the face the edit is painting;
    // fall back to the first region if the centre face was not recorded.
    for (uint32_t i = 0; i < m_LastRegions.Count; ++i)
    {
        if (m_LastRegions.Rects[i].Face == m_PrimaryFace)
        {
            const SphereFaceUVRect& r = m_LastRegions.Rects[i];
            outFace = r.Face;
            minU = r.MinU;
            minV = r.MinV;
            maxU = r.MaxU;
            maxV = r.MaxV;
            return !r.IsEmpty();
        }
    }
    const SphereFaceUVRect& r = m_LastRegions.Rects[0];
    outFace = r.Face;
    minU = r.MinU;
    minV = r.MinV;
    maxU = r.MaxU;
    maxV = r.MaxV;
    return !r.IsEmpty();
}

} // namespace GameEngine::CBTTerrain

#pragma once

// SphereSculptLayer — the CPU-authoritative editable height layer for a spherical CBT terrain
// (planet editing). It is the sphere analogue of a planar terrain's heightfield, now backed by a
// SPARSE VIRTUAL PAGE-TABLE:
//
//   * VIRTUAL resolution is radius-scaled — a per-face grid of runtime dim Dv derived from a
//     metres-per-texel budget (SphereSculptPaging.h DeriveSculptVirtualDim). At 8 m/texel a
//     default ~128 m brush covers ~32 texels at ANY radius, so the sub-texel invisible-brush case
//     on a 20 km planet (the old fixed 256^2/face -> ~123 m/texel) is gone.
//   * PHYSICAL storage is content-scaled — a fixed pool of kSculptPageDim^2 pages addressed through
//     a per-face page table. A page materializes on its first NON-ZERO write; an unallocated page
//     reads additive 0 (the "no edits" fast path). Pool memory is radius-INDEPENDENT: a 50 km
//     planet costs the same as a 2 km one (vs a flat scale-up's ~9.25 GB at 50 km).
//
// Two contributions compose into the published pool exactly as before:
//   * the DAB layer  — accumulated interactive brush strokes (ApplyDab, ADD),
//   * the MODIFIER layer — the baked terrain-modifier stack (BakeModifierLayer, SET),
// published = dab + modifier per texel. The shader / physics see only the sum (the page pool), so
// neither needs to know the layer split. Keeping the two apart lets a modifier re-bake over its
// footprint without erasing freehand dabs (the sphere analogue of the planar "reset region to base,
// re-apply the stack" bake).
//
// Seam-free by construction: both the dab falloff and every modifier contribution are a pure
// function of a texel's WORLD DIRECTION, and two texels straddling a cube edge sample the same set
// of world directions, so the shared column is written to the identical value on both faces (the
// #488 cross-face discipline) — independent of paging, because the page resolve is endpoint-exact.
//
// Device-free by design: the edit math + the page store live here (not in the GPU-owning feature)
// so the arc-law oracles (mirror mutation, region cost, cross-face falloff, quiescence, page
// materialization, pool exhaustion) run headless with no Vulkan device. The GPU-owning feature
// uploads Pool()/PageTable() into their SSBO rings and reads Version() to drive the upload gate.

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "CBTTerrain/CBTSphereFaceMap.h"   // SphereEditRegions, face map
#include "CBTTerrain/CBTLayout.h"          // kSculptPage* dims + kCBTFrameParamsRing + kSculptNoPage
#include "CBTTerrain/SphereSculptPaging.h" // SphereSculptGeometry / SphereSculptSampler / page math

namespace GameEngine::CBTTerrain
{

// Resolve the physical page pool count once (GE_SCULPT_PAGES env override, else the budget-derived
// default). CBTResources sizes the GPU pool ring from this and the CPU store allocates the same
// count, so a page id always addresses a live physical slot on both sides. Defined in the .cpp so
// the env is read exactly once.
uint32_t ResolveSculptPagePoolCount();

// One virtual page's DAB-layer state for the brush-stroke undo. Face/PageX/PageY key the virtual
// page; Allocated records whether the page existed at capture time (false = the stroke
// materialized it, so an undo restore returns it to unallocated). Dab is the page's full
// dab-layer content at capture (SculptFineDim(Level)^2 floats; empty when !Allocated). A stroke's
// snapshot is bounded by the pages it actually wrote, never the virtual face area.
//
// VirtualDim is the page GRID the snapshot was captured against. A planet resize remaps the store
// to a different grid (Configure), after which (face, pageX, pageY) names a DIFFERENT angular rect
// — restoring the old bytes there would misplace content, so RestoreDabPages refuses pages whose
// captured grid does not match the live one. Keyed on the grid (not a monotonic epoch) on purpose:
// undoing the resize itself returns the store to the captured grid, and the older stroke entries
// become valid again.
//
// Level (S4) is the page's resolution level at capture. When a restore must CHANGE the live
// page's level (undoing a stroke that escalated it, or redoing one that did), the dab bytes alone
// cannot rebuild the page — the modifier layer's bytes at the target level are needed too — so
// escalation captures BOTH layers (HasModifier). A dab-only snapshot whose level no longer
// matches the live page is refused per page (it cannot be restored without inventing modifier
// content); with the undo stack's LIFO discipline the level-changing entry is always the one
// carrying the modifier bytes, so the refusal only guards malformed stacks.
struct SphereSculptPageState
{
    uint32_t Face = 0u;
    uint32_t PageX = 0u;
    uint32_t PageY = 0u;
    uint32_t VirtualDim = 0u;
    uint32_t Level = 0u;
    bool Allocated = false;
    bool HasModifier = false;
    std::vector<float> Dab;      // SculptFineDim(Level)^2 when Allocated
    std::vector<float> Modifier; // SculptFineDim(Level)^2 when HasModifier
};

// One allocated virtual page's FULL authored content (both layers) for persistence
// (save/load), keyed like SphereSculptPageState. Unlike the stroke-undo state (dab-only,
// grid-keyed per page), a persistence snapshot carries ONE grid for all pages (the .tsculpt
// header's VirtualDim), so the page content needs no per-page grid field. Level (S4) sizes both
// layers at SculptFineDim(Level)^2 (.tsculpt v2 carries it per page; v1 decodes as level 0).
struct SphereSculptPageContent
{
    uint32_t Face = 0u;
    uint32_t PageX = 0u;
    uint32_t PageY = 0u;
    uint32_t Level = 0u;
    std::vector<float> Dab;
    std::vector<float> Modifier;
};

class SphereSculptLayer
{
  public:
    SphereSculptLayer(); // unconfigured (empty pool) until Configure — a fresh layer is quiescent.

    // (Re)size the store to a page geometry. Before the first edit this adopts the geometry
    // freely (zeroing pool + table). Once content exists, a geometry change REMAPS the authored
    // content onto the new grid instead of dropping or freezing it: both authoring layers (dab +
    // modifier) are resampled bilinearly at the new grid's face-UV texel centres, so the content
    // keeps its ANGULAR position (a mountain sculpted at a latitude stays at that latitude) and
    // its absolute metre amplitudes (a 100 m mound stays 100 m tall — heights are authored in
    // metres, matching the planar heightfield's size-independent heights). Returns the per-face
    // regions the remap wrote (empty when nothing changed / no remap) so the caller re-enters the
    // same dirty / re-tess / physics path an edit does — a remap IS an edit.
    //
    // If the remapped content would need more physical pages than the pool holds (a large radius
    // GROWTH multiplies the page count of the same angular footprint), the remap is REFUSED and
    // the store keeps the previous geometry + content untouched (warning once) — no authored
    // height is ever lost to a resize.
    SphereEditRegions Configure(const SphereSculptGeometry& geom);
    const SphereSculptGeometry& Geometry() const { return m_Geom; }

    // Drop all content + geometry, returning the store to the unconfigured state so the NEXT
    // Configure re-derives freely. Called when the planet the content belonged to is deleted or
    // switches domain away (the sculpt is per-planet authored data — content loss is correct here).
    // A create->edit->delete->recreate sequence must not leave the new planet with the old planet's
    // frozen dim + stale content (the "brush does nothing at 20k" resurrection this slice kills).
    void Reset();

    // Apply a raise/lower dab into the DAB layer. `cx,cy,cz` is the surface hit direction (planet
    // centred at the world origin); `angularRadius` is the brush footprint in radians
    // (worldRadius / planetRadius); `strength` is the peak offset in metres; `lower` subtracts.
    // Writes a smoothstep falloff in TRUE angular distance into every face the cap covers,
    // materializing the pages the falloff writes non-zero, and returns the touched (face, UV rect)
    // regions. Bumps Version().
    SphereEditRegions ApplyDab(float cx, float cy, float cz, float angularRadius, float strength,
                               bool lower);

    // Re-derive the MODIFIER layer over `regions` (a region bake): each texel is SET to
    // evalOffset(worldDir) metres and the published pool is recomposed = dab + modifier. SET
    // semantics clear any stale contribution the same way the planar bake resets a region to base.
    // Templated so the per-texel loop stays a direct call over the up-to-6*Dv*Dv texels of a bake.
    template <typename EvalFn>
    SphereEditRegions BakeModifierLayer(const SphereEditRegions& regions, EvalFn&& evalOffset)
    {
        return BakeModifier(regions, std::forward<EvalFn>(evalOffset), /*clearAllFirst=*/false);
    }

    // Full re-bake: clear the modifier layer of EVERY allocated page first (bounded by the pool
    // count, NOT the virtual face area — the fix that keeps a full bake cheap at a large Dv where a
    // whole-face iteration would be tens of millions of texels), then bake `regions`. `regions` for a
    // full bake is the union of the current modifiers' footprints (bounded), so a removed/moved
    // modifier's vacated pages are cleared to 0 (and reclaimed if they hold no dab). This is the
    // sphere analogue of resetting the whole heightfield to base before re-applying the stack.
    template <typename EvalFn>
    SphereEditRegions BakeModifierFull(const SphereEditRegions& regions, EvalFn&& evalOffset)
    {
        return BakeModifier(regions, std::forward<EvalFn>(evalOffset), /*clearAllFirst=*/true);
    }

    // A sampler VIEW over the published pool + page table for the shared bit-lock sampler
    // (SampleSculptFaceUV / SampleSphereSculptByDir in SphereSculptPaging.h). Valid only while the
    // layer is alive and unmutated.
    SphereSculptSampler MakeSampler() const { return {m_Pool.data(), m_PageTable.data(), m_Geom}; }

    // GPU-upload accessors: the published page pool (PoolPageCount * kSculptPageTexels floats) and
    // the page table (kSculptPageTableEntries uints). The CBT feature uploads these into their host-
    // visible SSBO rings, gated on Version().
    const std::vector<float>& Pool() const { return m_Pool; }
    const std::vector<uint32_t>& PageTable() const { return m_PageTable; }

    // Monotonic edit counter — 0 until the first dab or modifier bake. HasEdits() gates the shader
    // sample so an unedited planet pays nothing (the C7 default stays byte-identical).
    uint32_t Version() const { return m_Version; }
    bool HasEdits() const { return m_Version > 0u; }

    // Physical pool SLOTS currently claimed (materialization + VRAM-honesty oracle): a level-L
    // page counts 4^L (its whole block). Idle / untouched faces = 0.
    uint32_t AllocatedPageCount() const { return m_AllocatedCount; }
    uint32_t AllocatedPageCountForFace(uint32_t face) const;

    // Virtual pages currently hosted above the base resolution (S4 escalation observability).
    uint32_t EscalatedPageCount() const { return m_EscalatedPageCount; }

    // True once a write could not materialize a page (pool full). The write was REFUSED — no
    // authored height was dropped — and a one-shot warning was logged. Existing content is intact.
    bool PoolExhausted() const { return m_PoolExhausted; }

    // ---- Adaptive page levels (S4) ----
    // The maximum level ApplyDab may escalate a page to. Defaults to the env-resolved
    // GE_SCULPT_MAX_LEVEL (else kSculptMaxPageLevel); 0 disables escalation outright — the
    // in-process A/B the fails-before oracles and a tight-memory opt-out use.
    void SetMaxPageLevel(uint32_t level);
    uint32_t MaxPageLevel() const { return m_MaxPageLevel; }

    // Falloff form for BASE-resolution dab writes. The legacy form is smoothstep in true angular
    // distance (acos of a dot) — fp32-degenerate for small caps (cos(angR) rounds to 1.0f) and
    // noisy at planet radii (~0.1*strength angle-quantization rings, the documented store acos
    // artifact). The tangent-plane form is the S3 preview's own closed form
    // (EvaluateSphereAnalyticDab; falloff argument sin(ang)/sin(angR), scale-invariant), so a
    // committed stroke matches its held preview up to pure texel quantization. Defaults to the
    // env-resolved GE_TERRAIN_TANGENT_DAB (default OFF — flag-off store bytes stay pre-S4
    // byte-identical). ESCALATED writes always use the tangent form: escalation exists exactly
    // where acos has collapsed, so there is no legacy behaviour to preserve there.
    void SetTangentDabFalloff(bool enabled) { m_TangentFalloff = enabled; }
    bool TangentDabFalloff() const { return m_TangentFalloff; }

    // ---- Stroke undo capture (editor brush) ----
    // Between BeginStrokeCapture and TakeStrokeCapture, every page a dab WRITES records its
    // pre-image lazily on first touch — only touched pages, each captured once, so a stroke's
    // undo payload is memory-bounded by what it wrote. The editor wraps one interactive stroke
    // (mouse-down -> mouse-up) in a Begin/Take pair and pushes the result as ONE undo entry.
    // Dab-layer only by design: a stroke never writes the modifier layer.
    void BeginStrokeCapture();
    bool StrokeCaptureActive() const { return m_StrokeCaptureActive; }

    // End the capture and return the touched pages' PRE-stroke states (first-touch order).
    // Empty when the stroke wrote nothing (no undo entry warranted).
    std::vector<SphereSculptPageState> TakeStrokeCapture();

    // The CURRENT state of the pages keyed by `keys` (the stroke's post-images, captured at
    // stroke end for redo).
    std::vector<SphereSculptPageState> SnapshotPages(const std::vector<SphereSculptPageState>& keys) const;

    // Restore the dab layer of `pages` (undo -> pre-images, redo -> post-images) and recompose
    // the published pool. A page restored to !Allocated is reclaimed unless its modifier layer
    // still holds content. Advances Version() and returns per-face regions covering the restored
    // pages so the caller re-enters the same dirty / re-tess / physics path a dab does — an undo
    // IS an edit. Safe no-op against an unconfigured (reset) layer, stale out-of-range keys, or
    // pages captured against a different page grid (a resize remapped the store since capture —
    // restoring old-grid bytes would misplace content, so those pages are refused).
    SphereEditRegions RestoreDabPages(const std::vector<SphereSculptPageState>& pages);

    // ---- Persistence (save/load) ----
    // Snapshot every allocated page's authored layers for the .tsculpt save. Pages whose
    // content is entirely zero still export (the encoder skips them) — the seam stays dumb.
    std::vector<SphereSculptPageContent> ExportPages() const;

    // Restore a persistence snapshot into the CURRENT geometry: each in-grid page's layers are
    // written wholesale (allocating on demand) and the published pool recomposed. Out-of-grid
    // keys are skipped per page (never index out of range); pages the pool cannot hold are
    // refused (pool-exhaustion warning, content that fit is intact). Advances Version() once
    // and returns per-face covering regions — a LOAD is an edit, so the caller re-enters the
    // same dirty / re-tess / physics path a dab does. The caller Configures to the snapshot's
    // SAVED grid first; a different live radius then remaps through the normal Configure path.
    SphereEditRegions ImportPages(const std::vector<SphereSculptPageContent>& pages);

    // The primary (centre) region of the most recent edit, for Classify's (face,rect) reclassification.
    bool DirtyFaceRect(uint32_t& outFace, float& minU, float& minV, float& maxU, float& maxV) const;

    // The full set of regions the most recent edit touched (for the CBT.Edit signal / physics).
    const SphereEditRegions& LastRegions() const { return m_LastRegions; }

  private:
    // Per-page authoring layers (heap-allocated on first claim so an idle pool is cheap). The
    // published sum lives in the contiguous m_Pool (uploaded to the GPU); Dab/Modifier stay here,
    // both sized SculptFineDim(level)^2 (128^2 at the base level). Only a block's BASE slot owns
    // layers; follower slots of an escalated block carry none.
    struct PageLayers
    {
        std::vector<float> Dab;
        std::vector<float> Modifier;
    };
    struct PageMeta
    {
        bool Allocated = false;
        bool Follower = false; // slot is a continuation of an escalated block (base slot owns it)
        uint8_t Level = 0u;
        uint32_t Face = 0u;
        uint32_t PageX = 0u;
        uint32_t PageY = 0u;
    };

    // Table entry index for a virtual page (face, pageX, pageY) — face stride cap^2, row stride cap.
    uint32_t PageTableEntry(uint32_t face, uint32_t pageX, uint32_t pageY) const
    {
        return face * kSculptPageTableFaceStride + pageY * m_Geom.Cap + pageX;
    }

    // Remap the authored content (dab + modifier layers, kept separate) from the current geometry
    // onto `geom`: bilinear resample at the new grid's face-UV texel centres, page-sparse (only
    // pages the old content can reach are visited; all-zero results stay unallocated). Refuses —
    // restoring the old store untouched — when the resampled content would need more physical
    // pages than the new pool holds. Advances Version() on success and returns the per-face
    // regions written. Called by Configure when a geometry change lands on authored content.
    SphereEditRegions RemapContent(const SphereSculptGeometry& geom);

    // Claim a physical block for (face, pageX, pageY) at `level`: 4^level CONTIGUOUS pool slots
    // (level 0 = the single-slot path: free-list first, then bump; level > 0 = bump-region first,
    // then a contiguous run scan of the free list). Zeroes the block's layers + pool slots, points
    // the table entry at it (id | level bits), and bumps AllocatedCount by the block size.
    // Returns kSculptNoPage when the pool cannot hold the block. `warnOnExhausted` records
    // m_PoolExhausted + the one-shot warning (callers whose WRITE is refused by the failure);
    // TryEscalatePage passes false — its failure only keeps the page at its current level, which
    // has its own warning, and must not claim writes were refused.
    uint32_t AllocatePageBlock(uint32_t face, uint32_t pageX, uint32_t pageY, uint32_t level,
                               bool warnOnExhausted = true);

    // Release a physical block (base slot id) back to the free list (table entry -> NoPage when it
    // still points at this block). Used by the full-bake clear and undo to reclaim pages that end
    // up holding neither dab nor modifier content, and by escalation to retire the old block.
    void FreePageBlock(uint32_t pageId);

    // Re-escalate (face, pageX, pageY) to `level`, upsampling BOTH authoring layers bilinearly
    // onto the finer endpoint-aligned grid (amplitudes are metres — they copy through, the same
    // discipline as Configure's remap) and recomposing the published block. Refused (page keeps
    // its current level, one-shot warning) when the pool cannot hold the new block — the write
    // that wanted the escalation proceeds at the current level: refuse-don't-lose. Face-BORDER
    // pages are never escalated (PageMayEscalate): a fine cube-edge column has no fine
    // counterpart on the neighbouring face's grid, so cross-face crack-freedom holds only at the
    // base level today (the S5 candidate is a cross-face stitch). Captures the pre-image (both
    // layers) while a stroke capture is active.
    bool TryEscalatePage(uint32_t face, uint32_t pageX, uint32_t pageY, uint32_t level);
    bool PageMayEscalate(uint32_t pageX, uint32_t pageY) const;

    // Recompose the published pool of the block at `pageId` (= dab + modifier per fine texel).
    void RecomposePageBlock(uint32_t pageId);

    // Add `delta` to the DAB layer at virtual texel (face, vtx, vty) and recompose the published
    // pool. Materializes the page on a non-zero delta into an unallocated page (skips zero deltas).
    // While a stroke capture is active, records the touched page's pre-image on first touch.
    // The base-resolution write path (legacy byte-lock); escalated pages take WriteDabFine.
    void WriteDab(uint32_t face, uint32_t vtx, uint32_t vty, float delta);

    // Add `delta` to the DAB layer at FINE texel (jx, jy) of page (face, pageX, pageY) and
    // recompose that texel. The page's current level decides the fine grid; the caller iterates
    // fine texels of the target level (ApplyDab's escalated write loop).
    void WriteDabFine(uint32_t face, uint32_t pageX, uint32_t pageY, uint32_t jx, uint32_t jy,
                      float delta);

    // Record (face, pageX, pageY)'s pre-image once per stroke. `entry` is the page's CURRENT
    // table entry (kSculptNoPage = the write is materializing it, so the pre-image is
    // "unallocated"). `withModifier` snapshots the modifier layer too (level-change restores
    // need it — see SphereSculptPageState); when the page was already captured dab-only this
    // stroke, a later withModifier touch UPGRADES the recorded entry in place (the modifier
    // layer is stroke-invariant, so the live bytes still are the pre-stroke bytes).
    void CaptureStrokePage(uint32_t face, uint32_t pageX, uint32_t pageY, uint32_t entry,
                           bool withModifier);

    // SET the MODIFIER layer at virtual texel (face, vtx, vty) to `value` and recompose. Materializes
    // the page only when it would store a non-zero published value; a zero write into an unallocated
    // page is skipped (keeps the pool sparse to actual content). Base-resolution path.
    void WriteModifier(uint32_t face, uint32_t vtx, uint32_t vty, float value);

    // SET the MODIFIER layer at FINE texel (jx, jy) of page (face, pageX, pageY) — the escalated
    // twin of WriteModifier (same materialization rule).
    void WriteModifierFine(uint32_t face, uint32_t pageX, uint32_t pageY, uint32_t jx, uint32_t jy,
                           float value);

    // Clear the modifier layer of every allocated page (published -> dab), reclaiming pages left all-
    // zero. Returns whether anything changed. Bounded by the pool count. Used by the full bake.
    bool ClearAllModifierLayers();

    template <typename EvalFn>
    SphereEditRegions BakeModifier(const SphereEditRegions& regions, EvalFn&& evalOffset,
                                   bool clearAllFirst)
    {
        if (m_Geom.PoolPageCount == 0u)
            return {};
        bool changed = clearAllFirst ? ClearAllModifierLayers() : false;
        const int32_t dim = static_cast<int32_t>(m_Geom.VirtualDim);
        const float invDimMinus1 = 1.0f / static_cast<float>(dim - 1);
        SphereEditRegions baked{};
        uint32_t primaryFace = 0u;
        bool primarySet = false;
        for (uint32_t ri = 0; ri < regions.Count; ++ri)
        {
            const SphereFaceUVRect& r = regions.Rects[ri];
            if (r.IsEmpty())
                continue;
            const int32_t minTx = ClampTexel(static_cast<int32_t>(std::floor(r.MinU * (dim - 1))));
            const int32_t maxTx = ClampTexel(static_cast<int32_t>(std::ceil(r.MaxU * (dim - 1))));
            const int32_t minTz = ClampTexel(static_cast<int32_t>(std::floor(r.MinV * (dim - 1))));
            const int32_t maxTz = ClampTexel(static_cast<int32_t>(std::ceil(r.MaxV * (dim - 1))));
            if (m_EscalatedPageCount == 0u)
            {
                // No escalated pages anywhere: the pre-S4 base-texel loop, byte-identical
                // (same write ORDER too — page materialization order decides pool ids).
                for (int32_t tz = minTz; tz <= maxTz; ++tz)
                {
                    const float faceV = static_cast<float>(tz) * invDimMinus1;
                    for (int32_t tx = minTx; tx <= maxTx; ++tx)
                    {
                        const float faceU = static_cast<float>(tx) * invDimMinus1;
                        float dx, dy, dz;
                        FaceUVToWorldDir(r.Face, faceU, faceV, dx, dy, dz);
                        WriteModifier(r.Face, static_cast<uint32_t>(tx), static_cast<uint32_t>(tz),
                                      evalOffset(dx, dy, dz));
                    }
                }
            }
            else
            {
                // Escalated pages exist: bake per page at each page's OWN level, SETting every
                // fine texel of the rect so an escalated page's modifier layer is evaluated at
                // its full resolution (a base-only write would leave the unaligned fine texels
                // stale). Fine texel j sits at base position pageBase + j/step; u is computed as
                // basePos * invDimMinus1 — the SAME expression the base loop uses, so shared
                // cross-face columns still receive bit-identical values (#488 discipline).
                for (int32_t py = minTz / static_cast<int32_t>(kSculptPageDim);
                     py <= maxTz / static_cast<int32_t>(kSculptPageDim); ++py)
                {
                    const int32_t rowBase = py * static_cast<int32_t>(kSculptPageDim);
                    const int32_t qa = std::max(minTz, rowBase);
                    const int32_t qb =
                        std::min(maxTz, rowBase + static_cast<int32_t>(kSculptPageDim) - 1);
                    for (int32_t px = minTx / static_cast<int32_t>(kSculptPageDim);
                         px <= maxTx / static_cast<int32_t>(kSculptPageDim); ++px)
                    {
                        const int32_t colBase = px * static_cast<int32_t>(kSculptPageDim);
                        const int32_t pa = std::max(minTx, colBase);
                        const int32_t pb =
                            std::min(maxTx, colBase + static_cast<int32_t>(kSculptPageDim) - 1);
                        const uint32_t entry = m_PageTable[PageTableEntry(
                            r.Face, static_cast<uint32_t>(px), static_cast<uint32_t>(py))];
                        const uint32_t level = entry == kSculptNoPage ? 0u : SculptEntryLevel(entry);
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
                                    (static_cast<float>(colBase) +
                                     static_cast<float>(jx) * stepInv) *
                                    invDimMinus1;
                                float dx, dy, dz;
                                FaceUVToWorldDir(r.Face, faceU, faceV, dx, dy, dz);
                                WriteModifierFine(r.Face, static_cast<uint32_t>(px),
                                                  static_cast<uint32_t>(py), jx, jy,
                                                  evalOffset(dx, dy, dz));
                            }
                        }
                    }
                }
            }
            baked.Rects[baked.Count++] = r;
            if (!primarySet)
            {
                primaryFace = r.Face;
                primarySet = true;
            }
        }
        // A full bake is an explicit "clear + re-derive the whole modifier layer" event, so it
        // advances the version even when the net footprint is empty (no modifiers / all degenerate) —
        // matching the pre-paging whole-face re-bake, so downstream (GPU upload, physics) re-reads the
        // reset state. A region bake advances only when it actually touched a region.
        if (baked.Count > 0u || changed || clearAllFirst)
        {
            ++m_Version;
            if (baked.Count > 0u)
            {
                m_LastRegions = baked;
                m_PrimaryFace = primaryFace;
            }
        }
        return baked;
    }

    int32_t ClampTexel(int32_t v) const
    {
        const int32_t last = static_cast<int32_t>(m_Geom.VirtualDim) - 1;
        return v < 0 ? 0 : (v > last ? last : v);
    }

    SphereSculptGeometry m_Geom{}; // PoolPageCount 0 until Configure
    std::vector<float> m_Pool;     // published pool (dab + modifier), PoolPageCount * kSculptPageTexels
    std::vector<uint32_t> m_PageTable;                    // kSculptPageTableEntries, kSculptNoPage default
    std::vector<std::unique_ptr<PageLayers>> m_PageLayers; // per pool page, null until claimed
    std::vector<PageMeta> m_PageMeta;                      // per pool page owner metadata
    std::vector<uint32_t> m_FreePages;                     // reclaimed page ids
    uint32_t m_NextFreePage = 0u;                          // bump cursor into the pool
    uint32_t m_AllocatedCount = 0u;
    uint32_t m_EscalatedPageCount = 0u; // virtual pages above the base level (S4)
    uint32_t m_MaxPageLevel;            // env-resolved in the constructor (SetMaxPageLevel overrides)
    bool m_TangentFalloff;              // env-resolved in the constructor (SetTangentDabFalloff overrides)
    uint32_t m_Version = 0u;
    SphereEditRegions m_LastRegions{};
    uint32_t m_PrimaryFace = 0u;
    bool m_PoolExhausted = false;
    bool m_RemapRefusedWarned = false;
    bool m_EscalationRefusedWarned = false;

    // Stroke undo capture state (see BeginStrokeCapture). The index map keys on the page-table
    // entry; m_StrokeCaptureLastEntry short-circuits the per-texel lookup while a dab's writes
    // stay inside one page (rows cross a page boundary every kSculptPageDim texels).
    bool m_StrokeCaptureActive = false;
    std::vector<SphereSculptPageState> m_StrokeCapture;
    std::unordered_map<uint32_t, size_t> m_StrokeCaptureIndex;
    uint32_t m_StrokeCaptureLastEntry = kSculptNoPage;
};

// Per-ring-slot upload gate for the sculpt SSBOs (pool + page table). After an edit each ring slot
// must refresh ONCE (the frame it is current) and then stop — otherwise an edited planet re-uploads
// every frame, per view, forever (a quiescence violation). ShouldUpload claims the slot only when it
// has not yet uploaded the current sculpt version. Device-free, unit-testable.
class SphereSculptUploadGate
{
  public:
    // The version this ring element last uploaded (0 = never). Read before ShouldUpload to compute
    // the delta since its last sync (used by an incremental page upload). `frameCounter` is the
    // unwrapped counter the sculpt ring itself is written with, so the gate tracks the same element.
    uint64_t SyncedVersion(uint32_t frameCounter) const
    {
        return m_Uploaded[CBTFrameRingSlot(frameCounter)];
    }

    // True (and marks the element) iff this frame's element has not already uploaded `version`.
    // version 0 (no edits) never uploads.
    bool ShouldUpload(uint32_t frameCounter, uint64_t version)
    {
        if (version == 0u)
            return false;
        const uint32_t slot = CBTFrameRingSlot(frameCounter);
        if (m_Uploaded[slot] == version)
            return false;
        m_Uploaded[slot] = version;
        return true;
    }

    // Device-rebuild recovery: the sculpt ring buffers are re-created zeroed, so every
    // slot's "already holds version N" record is stale and would suppress the re-upload,
    // leaving an edited planet rendering its unedited base shape. Clearing forces each
    // slot to upload again on its next frame.
    void Reset() { m_Uploaded.fill(0u); }

  private:
    std::array<uint64_t, kCBTFrameParamsRing> m_Uploaded{};
};

} // namespace GameEngine::CBTTerrain

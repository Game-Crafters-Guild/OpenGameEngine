// Stroke-undo oracles for the sphere sculpt layer (planet brush undo). A stroke's undo payload is
// the touched pages' DAB-layer pre/post images, captured LAZILY (each page once, on the first dab
// that writes it) so the snapshot is bounded by what the stroke touched — never the virtual face
// area. RestoreDabPages is the undo/redo entry point: it must restore byte-identical content,
// advance Version() (an undo IS an edit — the version + regions it returns drive the same GPU
// upload / re-tess / physics invalidation a dab does), and reclaim pages the stroke materialized.

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "CBTTerrain/CBTSphereFaceMap.h"
#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/SphereSculptPaging.h"

using namespace GameEngine::CBTTerrain;

namespace
{

SphereSculptGeometry Geom(uint32_t virtualDim, uint32_t pageCount)
{
    return SphereSculptGeometry{virtualDim, virtualDim / kSculptPageDim, kSculptMaxPagesPerFaceAxis,
                                pageCount};
}

// A stroke-sized dab footprint: ~0.05 rad on a 1024^2 face covers ~65 texels of diameter — well
// inside one 128^2 page when centred, so a one-dab stroke touches only a handful of pages of the
// 8*8*6 = 384 virtual pages.
constexpr float kSmallAngularRadius = 0.05f;

void FaceCentreDir(uint32_t face, float& x, float& y, float& z)
{
    FaceUVToWorldDir(face, 0.5f, 0.5f, x, y, z);
}

bool PoolBytesEqual(const SphereSculptLayer& layer, const std::vector<float>& pool,
                    const std::vector<uint32_t>& table)
{
    const std::vector<float>& p = layer.Pool();
    const std::vector<uint32_t>& t = layer.PageTable();
    if (p.size() != pool.size() || t.size() != table.size())
        return false;
    return std::memcmp(p.data(), pool.data(), pool.size() * sizeof(float)) == 0 &&
           std::memcmp(t.data(), table.data(), table.size() * sizeof(uint32_t)) == 0;
}

// Logical bit-identity: the same set of allocated VIRTUAL pages and bit-identical published
// floats at every virtual texel. Redo re-allocates the pages an undo reclaimed, so the PHYSICAL
// page ids may permute (an allocator detail, like heap addresses) — every consumer (GPU sampler,
// physics mirror) resolves through the page table, so logical bit-identity IS byte-identical
// sampled output.
bool LogicalSculptEqual(const std::vector<float>& poolA, const std::vector<uint32_t>& tableA,
                        const std::vector<float>& poolB, const std::vector<uint32_t>& tableB)
{
    if (tableA.size() != tableB.size())
        return false;
    for (size_t e = 0; e < tableA.size(); ++e)
    {
        const bool aAlloc = tableA[e] != kSculptNoPage;
        const bool bAlloc = tableB[e] != kSculptNoPage;
        if (aAlloc != bAlloc)
            return false;
        if (!aAlloc)
            continue;
        const size_t aBase = static_cast<size_t>(tableA[e]) * kSculptPageTexels;
        const size_t bBase = static_cast<size_t>(tableB[e]) * kSculptPageTexels;
        if (aBase + kSculptPageTexels > poolA.size() || bBase + kSculptPageTexels > poolB.size())
            return false;
        if (std::memcmp(poolA.data() + aBase, poolB.data() + bBase,
                        kSculptPageTexels * sizeof(float)) != 0)
            return false;
    }
    return true;
}

} // namespace

// Gate 4 (memory bound): the stroke snapshot covers ONLY the pages the stroke wrote. On a fresh
// layer every touched page is newly materialized, so capture count == allocated count, and a small
// stroke on a big (high virtual dim) planet captures a handful of pages, not the face area.
TEST(SphereSculptUndo, StrokeCaptureCoversOnlyTouchedPages)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u)); // 8x8 pages/face, 384 virtual pages across the six faces

    float dx, dy, dz;
    FaceCentreDir(2u, dx, dy, dz);

    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 3.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();

    ASSERT_FALSE(before.empty());
    EXPECT_EQ(before.size(), layer.AllocatedPageCount())
        << "fresh layer: every touched page was materialized by the stroke";
    EXPECT_LE(before.size(), 4u) << "a page-sized dab must not snapshot beyond the pages it wrote";
    for (const SphereSculptPageState& p : before)
        EXPECT_FALSE(p.Allocated) << "pre-images on a fresh layer are 'page did not exist'";
}

// Lazy = once: a stroke of many dabs over the same page captures that page's pre-image exactly
// once (the first touch), so long strokes don't grow the snapshot per dab.
TEST(SphereSculptUndo, StrokeCaptureCapturesEachPageOnce)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u));

    float dx, dy, dz;
    FaceCentreDir(2u, dx, dy, dz);

    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 3.0f, false);
    const uint32_t pagesAfterFirstDab = layer.AllocatedPageCount();
    for (int i = 0; i < 8; ++i)
        layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 3.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();

    EXPECT_EQ(layer.AllocatedPageCount(), pagesAfterFirstDab);
    EXPECT_EQ(before.size(), pagesAfterFirstDab) << "repeat dabs on captured pages add no entries";
}

// Gate 1 (layer half): undo restores the published pool + page table byte-identical to the
// pre-stroke state — including a page that held PRE-EXISTING dab content and a cross-face edge
// dab (two faces' pages in one stroke).
TEST(SphereSculptUndo, UndoRestoreIsByteIdenticalToPreStroke)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u));

    // Pre-stroke content: an earlier committed stroke near the face centre.
    float dx, dy, dz;
    FaceCentreDir(2u, dx, dy, dz);
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 2.0f, false);

    const std::vector<float> prePool = layer.Pool();
    const std::vector<uint32_t> preTable = layer.PageTable();
    const uint32_t prePages = layer.AllocatedPageCount();
    const uint32_t preVersion = layer.Version();

    // The stroke: one dab overlapping the existing content, one at a cube edge (straddles two
    // faces), one lowering dab.
    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 5.0f, false);
    float ex, ey, ez;
    FaceUVToWorldDir(2u, 0.995f, 0.5f, ex, ey, ez); // at the face edge -> neighbour face too
    layer.ApplyDab(ex, ey, ez, kSmallAngularRadius, 4.0f, false);
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 1.0f, true);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());
    EXPECT_GT(layer.Version(), preVersion);
    EXPECT_FALSE(PoolBytesEqual(layer, prePool, preTable)) << "the stroke must have changed bytes";

    // Undo.
    const SphereEditRegions regions = layer.RestoreDabPages(before);
    EXPECT_GT(regions.Count, 0u);
    EXPECT_TRUE(PoolBytesEqual(layer, prePool, preTable))
        << "undo must restore the published pool + page table byte-identical to pre-stroke";
    EXPECT_EQ(layer.AllocatedPageCount(), prePages);
}

// Gate 2 (layer half): redo restores the post-stroke state bit-identical — same allocated
// virtual-page set, bit-identical published float at every texel. (Physical page ids may
// permute across the undo/redo reclaim/re-allocate cycle; see LogicalSculptEqual.)
TEST(SphereSculptUndo, RedoRestoreIsByteIdenticalToPostStroke)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u));

    float dx, dy, dz;
    FaceCentreDir(1u, dx, dy, dz);
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 2.0f, false);

    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 5.0f, false);
    float ex, ey, ez;
    FaceUVToWorldDir(1u, 0.5f, 0.01f, ex, ey, ez);
    layer.ApplyDab(ex, ey, ez, kSmallAngularRadius, 3.0f, true);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());
    std::vector<SphereSculptPageState> after = layer.SnapshotPages(before);

    const std::vector<float> postPool = layer.Pool();
    const std::vector<uint32_t> postTable = layer.PageTable();
    const uint32_t postPages = layer.AllocatedPageCount();

    layer.RestoreDabPages(before); // undo
    ASSERT_FALSE(LogicalSculptEqual(layer.Pool(), layer.PageTable(), postPool, postTable));

    layer.RestoreDabPages(after); // redo
    EXPECT_TRUE(LogicalSculptEqual(layer.Pool(), layer.PageTable(), postPool, postTable))
        << "redo must restore the post-stroke state bit-identical (per virtual texel)";
    EXPECT_EQ(layer.AllocatedPageCount(), postPages);
}

// Gate 3 (layer half): a restore IS an edit — Version() advances and the returned regions cover
// the restored pages (they feed the same dirty/re-tess path a dab's regions do).
TEST(SphereSculptUndo, RestoreAdvancesVersionAndReturnsCoveringRegions)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u));

    float dx, dy, dz;
    FaceCentreDir(3u, dx, dy, dz);

    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 3.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());

    const uint32_t postStrokeVersion = layer.Version();
    const SphereEditRegions regions = layer.RestoreDabPages(before);

    EXPECT_EQ(layer.Version(), postStrokeVersion + 1u) << "undo must advance the edit version";
    ASSERT_GT(regions.Count, 0u);

    // The dab centre (face 3, uv 0.5/0.5) must be inside a returned face-3 rect.
    bool covered = false;
    for (uint32_t i = 0; i < regions.Count; ++i)
    {
        const SphereFaceUVRect& r = regions.Rects[i];
        if (r.Face == 3u && r.MinU <= 0.5f && r.MaxU >= 0.5f && r.MinV <= 0.5f && r.MaxV >= 0.5f)
            covered = true;
    }
    EXPECT_TRUE(covered) << "restore regions must cover the restored pages for re-tess/physics";

    // LastRegions reflects the restore, so DirtyFaceRect-driven consumers see the undo.
    EXPECT_EQ(layer.LastRegions().Count, regions.Count);
}

// Undo of a stroke that materialized pages returns them to the free pool (allocation count and
// page table return to the pre-stroke state — no page leak per undone stroke).
TEST(SphereSculptUndo, UndoReclaimsPagesMaterializedByStroke)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u));

    float dx, dy, dz;
    FaceCentreDir(4u, dx, dy, dz);

    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 3.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_GT(layer.AllocatedPageCount(), 0u);

    layer.RestoreDabPages(before);
    EXPECT_EQ(layer.AllocatedPageCount(), 0u)
        << "undo must reclaim every page the stroke materialized";
}

// A page shared with MODIFIER content: undoing the dab stroke restores the dab layer only — the
// baked modifier contribution survives (published = modifier) and the page is not reclaimed.
TEST(SphereSculptUndo, UndoPreservesModifierLayerOnSharedPage)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u));

    SphereEditRegions bakeRegion{};
    bakeRegion.Rects[bakeRegion.Count++] = SphereFaceUVRect{5u, 0.45f, 0.45f, 0.55f, 0.55f};
    layer.BakeModifierLayer(bakeRegion, [](float, float, float) { return 5.0f; });

    const uint32_t pagesAfterBake = layer.AllocatedPageCount();
    ASSERT_GT(pagesAfterBake, 0u);

    float dx, dy, dz;
    FaceCentreDir(5u, dx, dy, dz);
    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 3.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());

    layer.RestoreDabPages(before);

    const uint32_t half = layer.Geometry().VirtualDim / 2u;
    EXPECT_FLOAT_EQ(SculptResolveTexel(layer.MakeSampler(), 5u, half, half), 5.0f)
        << "undo of the dab stroke must leave the modifier contribution intact";
    EXPECT_EQ(layer.AllocatedPageCount(), pagesAfterBake)
        << "a page still holding modifier content must not be reclaimed";
}

// Restoring into an unconfigured / reset layer is a safe no-op (the planet was deleted while the
// undo entry was still on the stack).
TEST(SphereSculptUndo, RestoreAfterResetIsSafeNoOp)
{
    SphereSculptLayer layer;
    layer.Configure(Geom(1024u, 256u));

    float dx, dy, dz;
    FaceCentreDir(0u, dx, dy, dz);
    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, kSmallAngularRadius, 3.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());

    layer.Reset();
    const SphereEditRegions regions = layer.RestoreDabPages(before);
    EXPECT_EQ(regions.Count, 0u);
    EXPECT_EQ(layer.Version(), 0u);
    EXPECT_EQ(layer.AllocatedPageCount(), 0u);
}

// ---- Resize (content remap) x stroke-undo composition (sculpt-content remap slice) ----
// A planet resize remaps the store onto a different page grid; stroke entries captured against the
// OLD grid name pages that now cover different angular rects. RestoreDabPages must refuse them
// per-page (no OOB, no crash, no content change) — and, because the refusal keys on the captured
// GRID rather than a monotonic epoch, undoing the resize itself re-validates the older entries.

// Undo/redo of a pre-resize stroke against a REMAPPED store is refused gracefully: content, version
// and allocation stay untouched, and no regions are returned (nothing downstream re-tessellates).
TEST(SphereSculptUndo, RestoreRefusedAfterResizeRemap)
{
    SphereSculptLayer layer;
    layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    float dx, dy, dz;
    FaceUVToWorldDir(0u, 0.35f, 0.35f, dx, dy, dz);
    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, 0.03f, 5.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());
    std::vector<SphereSculptPageState> after = layer.SnapshotPages(before);

    // Resize: the store remaps to the 20 km grid; the entry now names old-grid pages.
    ASSERT_GT(layer.Configure(MakeSphereSculptGeometry(20000.0f, 64u)).Count, 0u);
    const std::vector<float> poolAfterRemap = layer.Pool();
    const std::vector<uint32_t> tableAfterRemap = layer.PageTable();
    const uint32_t versionAfterRemap = layer.Version();

    EXPECT_EQ(layer.RestoreDabPages(before).Count, 0u) << "old-grid undo entry must be refused";
    EXPECT_EQ(layer.RestoreDabPages(after).Count, 0u) << "old-grid redo entry must be refused";
    EXPECT_EQ(layer.Version(), versionAfterRemap) << "a refused restore is not an edit";
    EXPECT_TRUE(PoolBytesEqual(layer, poolAfterRemap, tableAfterRemap))
        << "a refused restore must not touch the remapped content";

    // Shrink direction too (PagesPerAxis drops below the captured keys — the range guard's case).
    ASSERT_GT(layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u)).Count, 0u);
    ASSERT_GT(layer.Configure(MakeSphereSculptGeometry(50000.0f, 256u)).Count, 0u);
    EXPECT_EQ(layer.RestoreDabPages(before).Count, 0u);
}

// Undoing the RESIZE itself (radius back to the captured value -> same derived grid) makes the
// pre-resize stroke entries valid again: the round-tripped store restores the stroke's pages to
// their exact pre-images, and the stroke's pages are reclaimed (the dab is fully undone).
TEST(SphereSculptUndo, ResizeUndoRoundTripRevalidatesStrokeEntries)
{
    SphereSculptLayer layer;
    layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    // Centred inside one page (Dv 512: texel ~178 of page 1) with a footprint + fringe that stays
    // inside it, so the remap round-trip cannot smear content into pages the stroke never touched.
    float dx, dy, dz;
    FaceUVToWorldDir(0u, 0.35f, 0.35f, dx, dy, dz);
    layer.BeginStrokeCapture();
    layer.ApplyDab(dx, dy, dz, 0.03f, 5.0f, false);
    std::vector<SphereSculptPageState> before = layer.TakeStrokeCapture();
    ASSERT_FALSE(before.empty());
    const uint32_t dimAtCapture = layer.Geometry().VirtualDim;

    // Resize 2 km -> 6 km (remap), then undo the resize -> 2 km (remap back to the captured grid).
    ASSERT_GT(layer.Configure(MakeSphereSculptGeometry(6000.0f, 64u)).Count, 0u);
    ASSERT_GT(layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u)).Count, 0u);
    ASSERT_EQ(layer.Geometry().VirtualDim, dimAtCapture);

    const SphereEditRegions restored = layer.RestoreDabPages(before);
    EXPECT_GT(restored.Count, 0u) << "entries captured against the round-tripped grid are valid again";
    EXPECT_EQ(SampleSphereSculptByDir(layer.MakeSampler(), dx, dy, dz), 0.0f)
        << "undo after the resize round-trip must remove the dab";
    EXPECT_EQ(layer.AllocatedPageCount(), 0u)
        << "the dab (and its remap round-trip residue) lived only in the stroke's pages";
}

// A resize landing MID-STROKE (brush held down while the radius edit applies) drops the stale
// old-grid pre-images but keeps the capture armed: the eventual undo entry holds only new-grid
// pages and restores cleanly against the post-remap store.
TEST(SphereSculptUndo, MidStrokeResizeDropsStaleCaptureKeepsStrokeAlive)
{
    SphereSculptLayer layer;
    layer.Configure(MakeSphereSculptGeometry(2000.0f, 64u));
    float ax, ay, az, bx, by, bz;
    FaceUVToWorldDir(0u, 0.30f, 0.30f, ax, ay, az);
    FaceUVToWorldDir(0u, 0.70f, 0.70f, bx, by, bz); // disjoint from A

    layer.BeginStrokeCapture();
    layer.ApplyDab(ax, ay, az, 0.03f, 5.0f, false); // captured against the old grid...
    ASSERT_GT(layer.Configure(MakeSphereSculptGeometry(6000.0f, 64u)).Count, 0u); // ...invalidated
    ASSERT_TRUE(layer.StrokeCaptureActive()) << "a mid-stroke resize must not disarm the capture";
    layer.ApplyDab(bx, by, bz, 0.03f, 7.0f, false); // captured fresh against the new grid

    std::vector<SphereSculptPageState> entry = layer.TakeStrokeCapture();
    ASSERT_FALSE(entry.empty());
    const uint32_t newDim = layer.Geometry().VirtualDim;
    for (const SphereSculptPageState& p : entry)
        EXPECT_EQ(p.VirtualDim, newDim) << "no old-grid pre-image may survive a mid-stroke resize";

    // Restoring the entry undoes dab B only; the remapped dab A remains.
    const float aBefore = SampleSphereSculptByDir(layer.MakeSampler(), ax, ay, az);
    ASSERT_GT(aBefore, 4.0f);
    EXPECT_GT(layer.RestoreDabPages(entry).Count, 0u);
    EXPECT_EQ(SampleSphereSculptByDir(layer.MakeSampler(), bx, by, bz), 0.0f)
        << "the post-resize half of the stroke must undo";
    EXPECT_NEAR(SampleSphereSculptByDir(layer.MakeSampler(), ax, ay, az), aBefore, 1e-4f)
        << "the remapped pre-resize dab must survive the stroke undo";
}

// Oracles for sphere sculpt persistence at the layer level: the .tsculpt blob codec
// (round-trip exactness, sparse skipping, crafted-header rejection) and the layer's
// ExportPages/ImportPages seam (byte-identical published pool, load-is-an-edit version +
// covering regions, pool-exhaustion refusal). Device-free like every SphereSculptLayer oracle.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CBTTerrain/SphereSculptLayer.h"
#include "CBTTerrain/SphereSculptPaging.h"
#include "CBTTerrain/SphereSculptSerialization.h"

using namespace GameEngine::CBTTerrain;

namespace
{
SphereSculptGeometry Geom(uint32_t virtualDim, uint32_t pageCount)
{
    return SphereSculptGeometry{virtualDim, virtualDim / kSculptPageDim, kSculptMaxPagesPerFaceAxis,
                                pageCount};
}
SphereSculptGeometry Geom256() { return Geom(256u, 64u); }

float Texel(const SphereSculptLayer& layer, uint32_t face, uint32_t x, uint32_t y)
{
    return SculptResolveTexel(layer.MakeSampler(), face, x, y);
}

// Author a representative store: freehand dabs (one interior, one straddling the +X/+Y edge)
// plus a modifier-layer bake over a sub-rect, so both layers and several faces hold content.
void AuthorContent(SphereSculptLayer& layer)
{
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 25.0f, false);        // +X interior
    layer.ApplyDab(0.7071f, 0.7071f, 0.0f, 0.06f, 10.0f, false);  // +X/+Y edge (two faces)
    SphereEditRegions r{};
    r.Rects[r.Count++] = SphereFaceUVRect{2u, 0.4f, 0.4f, 0.6f, 0.6f};
    layer.BakeModifierLayer(r, [](float, float, float) { return 7.5f; });
}

// Both layers of every allocated page, byte-compared through the export seam. Sorting by
// (face, pageY, pageX) makes the comparison independent of physical page-id assignment order.
std::vector<SphereSculptPageContent> SortedExport(const SphereSculptLayer& layer)
{
    std::vector<SphereSculptPageContent> pages = layer.ExportPages();
    std::sort(pages.begin(), pages.end(),
              [](const SphereSculptPageContent& a, const SphereSculptPageContent& b) {
                  if (a.Face != b.Face)
                      return a.Face < b.Face;
                  if (a.PageY != b.PageY)
                      return a.PageY < b.PageY;
                  return a.PageX < b.PageX;
              });
    return pages;
}

void ExpectStoresIdentical(const SphereSculptLayer& a, const SphereSculptLayer& b)
{
    ASSERT_EQ(a.Geometry().VirtualDim, b.Geometry().VirtualDim);
    const std::vector<SphereSculptPageContent> pa = SortedExport(a);
    const std::vector<SphereSculptPageContent> pb = SortedExport(b);
    ASSERT_EQ(pa.size(), pb.size());
    for (size_t i = 0; i < pa.size(); ++i)
    {
        EXPECT_EQ(pa[i].Face, pb[i].Face);
        EXPECT_EQ(pa[i].PageX, pb[i].PageX);
        EXPECT_EQ(pa[i].PageY, pb[i].PageY);
        EXPECT_EQ(pa[i].Level, pb[i].Level);
        ASSERT_EQ(pa[i].Dab.size(), pb[i].Dab.size());
        ASSERT_EQ(pa[i].Modifier.size(), pb[i].Modifier.size());
        EXPECT_EQ(0, std::memcmp(pa[i].Dab.data(), pb[i].Dab.data(),
                                 pa[i].Dab.size() * sizeof(float)))
            << "dab layer differs on page " << i;
        EXPECT_EQ(0, std::memcmp(pa[i].Modifier.data(), pb[i].Modifier.data(),
                                 pa[i].Modifier.size() * sizeof(float)))
            << "modifier layer differs on page " << i;
    }
    // The published pool (what the shader/physics read) is identical over every authored page.
    for (const SphereSculptPageContent& p : pa)
    {
        const uint32_t baseX = p.PageX * kSculptPageDim;
        const uint32_t baseY = p.PageY * kSculptPageDim;
        for (uint32_t y = 0; y < kSculptPageDim; ++y)
            for (uint32_t x = 0; x < kSculptPageDim; ++x)
            {
                const float va = Texel(a, p.Face, baseX + x, baseY + y);
                const float vb = Texel(b, p.Face, baseX + x, baseY + y);
                ASSERT_EQ(0, std::memcmp(&va, &vb, sizeof(float)))
                    << "published height differs at face " << p.Face << " texel (" << baseX + x
                    << "," << baseY + y << ")";
            }
    }
}
} // namespace

// Export -> encode -> decode -> import into a fresh layer reproduces the store exactly:
// same geometry, same allocated pages, byte-identical layers AND published pool.
TEST(SphereSculptSerialization, RoundTripByteIdentity)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    AuthorContent(layer);
    ASSERT_GT(layer.AllocatedPageCount(), 0u);

    const std::vector<uint8_t> blob = EncodeSphereSculpt(layer.Geometry(), layer.ExportPages());

    SphereSculptGeometry savedGeom{};
    std::vector<SphereSculptPageContent> pages;
    ASSERT_TRUE(DecodeSphereSculpt(blob.data(), blob.size(), savedGeom, pages));
    EXPECT_EQ(savedGeom.VirtualDim, 256u);
    EXPECT_EQ(savedGeom.PagesPerAxis, 2u);
    EXPECT_EQ(savedGeom.PoolPageCount, 64u);

    SphereSculptLayer restored;
    restored.Configure(Geom(savedGeom.VirtualDim, 64u));
    restored.ImportPages(pages);

    EXPECT_EQ(restored.AllocatedPageCount(), layer.AllocatedPageCount());
    ExpectStoresIdentical(layer, restored);
}

// A load IS an edit: the import advances the version exactly once and returns covering
// regions for every touched face, so the caller can feed the same dirty / re-tess / physics
// unions a dab feeds. An empty import stays quiescent.
TEST(SphereSculptSerialization, ImportIsAnEdit)
{
    SphereSculptLayer author;
    author.Configure(Geom256());
    AuthorContent(author);
    const std::vector<SphereSculptPageContent> pages = author.ExportPages();

    SphereSculptLayer restored;
    restored.Configure(Geom256());
    EXPECT_EQ(restored.Version(), 0u);

    const SphereEditRegions regions = restored.ImportPages(pages);
    EXPECT_EQ(restored.Version(), 1u) << "import must advance the version exactly once";
    ASSERT_GT(regions.Count, 0u);
    // Every face that holds imported content is covered by a returned region.
    for (uint32_t face = 0; face < kCubeFaceCount; ++face)
    {
        if (restored.AllocatedPageCountForFace(face) == 0u)
            continue;
        bool covered = false;
        for (uint32_t i = 0; i < regions.Count; ++i)
            covered = covered || (regions.Rects[i].Face == face && !regions.Rects[i].IsEmpty());
        EXPECT_TRUE(covered) << "face " << face << " imported content but got no covering region";
    }

    // Importing nothing is a no-op (no version churn, no regions).
    SphereSculptLayer idle;
    idle.Configure(Geom256());
    const SphereEditRegions none = idle.ImportPages({});
    EXPECT_EQ(none.Count, 0u);
    EXPECT_EQ(idle.Version(), 0u);
}

// Sparseness on disk: an all-zero page is never encoded, and a page holding only one layer
// encodes only that layer (flag-gated), so a dab-only stroke costs half a page record.
TEST(SphereSculptSerialization, ZeroPagesAndZeroLayersSkipped)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    // Dab +5 then the exact inverse: the page stays allocated but its content is exactly zero
    // (same falloff weights, negated), so the encoder must skip it.
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 5.0f, false);
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 5.0f, true);
    ASSERT_GT(layer.AllocatedPageCount(), 0u);

    const std::vector<uint8_t> zeroBlob = EncodeSphereSculpt(layer.Geometry(), layer.ExportPages());
    SphereSculptGeometry geomOut{};
    std::vector<SphereSculptPageContent> pagesOut;
    ASSERT_TRUE(DecodeSphereSculpt(zeroBlob.data(), zeroBlob.size(), geomOut, pagesOut));
    EXPECT_EQ(pagesOut.size(), 0u) << "an exactly-cancelled page must not be encoded";

    // A dab-only page encodes one layer; adding modifier content re-encodes both.
    SphereSculptLayer dabOnly;
    dabOnly.Configure(Geom256());
    dabOnly.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 5.0f, false);
    const std::vector<uint8_t> dabBlob =
        EncodeSphereSculpt(dabOnly.Geometry(), dabOnly.ExportPages());
    ASSERT_TRUE(DecodeSphereSculpt(dabBlob.data(), dabBlob.size(), geomOut, pagesOut));
    ASSERT_GT(pagesOut.size(), 0u);
    const size_t layerBytes = kSculptPageTexels * sizeof(float);
    const size_t headerBytes = 5 * sizeof(uint32_t);
    const size_t prefixBytes = 4 * sizeof(uint32_t);
    EXPECT_EQ(dabBlob.size(), headerBytes + pagesOut.size() * (prefixBytes + layerBytes))
        << "a dab-only page must encode exactly one layer";
    for (const SphereSculptPageContent& p : pagesOut)
        EXPECT_TRUE(std::all_of(p.Modifier.begin(), p.Modifier.end(),
                                [](float v) { return v == 0.0f; }))
            << "absent modifier layer must decode zero-filled";
}

// The decoder rejects malformed blobs without touching the outputs: bad magic/version, a grid
// that is not a whole page multiple / beyond the page-table cap, out-of-range page keys,
// unknown layer flags, truncated payloads, and a page count the bytes cannot back.
TEST(SphereSculptSerialization, DecodeRejectsMalformedBlobs)
{
    SphereSculptLayer layer;
    layer.Configure(Geom256());
    layer.ApplyDab(1.0f, 0.0f, 0.0f, 0.05f, 5.0f, false);
    const std::vector<uint8_t> good = EncodeSphereSculpt(layer.Geometry(), layer.ExportPages());

    SphereSculptGeometry geomOut{};
    std::vector<SphereSculptPageContent> pagesOut;
    ASSERT_TRUE(DecodeSphereSculpt(good.data(), good.size(), geomOut, pagesOut));
    const SphereSculptGeometry sentinelGeom = geomOut;
    const size_t sentinelPages = pagesOut.size();

    auto expectRejected = [&](std::vector<uint8_t> blob, const char* what) {
        EXPECT_FALSE(DecodeSphereSculpt(blob.data(), blob.size(), geomOut, pagesOut)) << what;
        EXPECT_EQ(geomOut.VirtualDim, sentinelGeom.VirtualDim) << what << ": outputs mutated";
        EXPECT_EQ(pagesOut.size(), sentinelPages) << what << ": outputs mutated";
    };

    auto corrupt = [&](size_t u32Index, uint32_t value) {
        std::vector<uint8_t> blob = good;
        std::memcpy(blob.data() + u32Index * sizeof(uint32_t), &value, sizeof(uint32_t));
        return blob;
    };

    expectRejected(corrupt(0, 0xDEADBEEFu), "bad magic");
    expectRejected(corrupt(1, 999u), "unknown version");
    expectRejected(corrupt(2, 200u), "virtualDim not a page multiple");
    expectRejected(corrupt(2, (kSculptMaxPagesPerFaceAxis + 1u) * kSculptPageDim),
                   "virtualDim beyond the page-table cap");
    expectRejected(corrupt(4, 0xFFFFFFFFu), "page count the bytes cannot back");
    expectRejected(corrupt(5, 6u), "face out of range");     // first page record: face
    expectRejected(corrupt(6, 99u), "pageX outside grid");   // first page record: pageX
    expectRejected(corrupt(8, 0x4u), "unknown layer flags"); // first page record: flags
    expectRejected(corrupt(8, 0x0u), "empty layer flags");

    std::vector<uint8_t> truncated(good.begin(), good.end() - 8);
    expectRejected(truncated, "truncated payload");
    expectRejected(std::vector<uint8_t>(good.begin(), good.begin() + 10), "truncated header");
    EXPECT_FALSE(DecodeSphereSculpt(nullptr, 0, geomOut, pagesOut));
}

// Import against a pool too small for the content refuses the overflow pages (pool-exhaustion
// honesty: the pages that fit are intact, the layer flags PoolExhausted, no crash).
TEST(SphereSculptSerialization, ImportRefusesBeyondPool)
{
    SphereSculptLayer author;
    author.Configure(Geom256());
    AuthorContent(author);
    const std::vector<SphereSculptPageContent> pages = author.ExportPages();
    ASSERT_GT(pages.size(), 1u) << "need >1 authored page to exercise the refusal";

    SphereSculptLayer tiny;
    tiny.Configure(Geom(256u, 1u)); // one physical page
    tiny.ImportPages(pages);
    EXPECT_EQ(tiny.AllocatedPageCount(), 1u);
    EXPECT_TRUE(tiny.PoolExhausted());
    EXPECT_TRUE(tiny.HasEdits()) << "the page that fit is a real import";
}

// Import keys outside the LIVE grid are skipped per page (a blob from a larger saved grid fed
// to a smaller configured store must not index out of range) — mirror of RestoreDabPages'
// stale-key refusal.
TEST(SphereSculptSerialization, ImportSkipsOutOfGridKeys)
{
    SphereSculptPageContent far{};
    far.Face = 0u;
    far.PageX = 3u; // beyond a 256 grid's 2 pages/axis
    far.PageY = 0u;
    far.Dab.assign(kSculptPageTexels, 1.0f);
    far.Modifier.assign(kSculptPageTexels, 0.0f);

    SphereSculptPageContent ok{};
    ok.Face = 0u;
    ok.PageX = 1u;
    ok.PageY = 1u;
    ok.Dab.assign(kSculptPageTexels, 2.0f);
    ok.Modifier.assign(kSculptPageTexels, 0.0f);

    SphereSculptLayer layer;
    layer.Configure(Geom256());
    const SphereEditRegions regions = layer.ImportPages({far, ok});
    EXPECT_EQ(layer.AllocatedPageCount(), 1u) << "out-of-grid page must be skipped";
    EXPECT_EQ(layer.Version(), 1u);
    ASSERT_EQ(regions.Count, 1u);
    EXPECT_EQ(regions.Rects[0].Face, 0u);
    EXPECT_EQ(Texel(layer, 0u, kSculptPageDim + 1u, kSculptPageDim + 1u), 2.0f);
}

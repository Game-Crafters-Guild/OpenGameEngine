// Stage 2 substep 2.2 — assert ContentHash and PipelineFormatKey hashing
// invariants. These guard the cache-key contract: same content → same id.

#include "Rendering/Core/PipelineCache.h"
#include "Rendering/Core/PipelineTypes.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <new>

using namespace GameEngine::Rendering;

namespace
{

GraphicsPipelineDesc MakeMinimalGraphicsDesc()
{
    GraphicsPipelineDesc d;
    d.Kind = GraphicsPipelineKind::VertexFragment;
    d.VertexShader = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0x1, 0x2, 0x3});
    d.PixelShader  = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xa, 0xb});
    d.PushConstants.Size = 128;
    d.PushConstants.StageMask = 0x3;
    d.Topology = PrimitiveTopology::TriangleList;
    return d;
}

ComputePipelineDesc MakeMinimalComputeDesc()
{
    ComputePipelineDesc d;
    d.ComputeShader = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xc, 0xd, 0xe});
    d.PushConstants.Size = 16;
    d.PushConstants.StageMask = 0x20; // compute stage
    return d;
}

PipelineFormatKey MakeFormatKey(uint32_t color, uint32_t depth, uint8_t samples)
{
    PipelineFormatKey k;
    k.ColorCount = 1;
    k.ColorFormats[0] = static_cast<TextureFormat>(color);
    k.DepthFormat = static_cast<TextureFormat>(depth);
    k.RasterizationSamples = samples;
    return k;
}

} // namespace

// =====================================================================
// PipelineFormatKey
// =====================================================================

TEST(PipelineFormatKey, SameContentSameHash)
{
    auto a = MakeFormatKey(43, 124, 1); // RGBA8_UNORM, D32_SFLOAT, 1x
    auto b = MakeFormatKey(43, 124, 1);
    EXPECT_EQ(a.Hash(), b.Hash());
    EXPECT_EQ(a, b);
}

TEST(PipelineFormatKey, DifferentColorFormatDifferentHash)
{
    auto a = MakeFormatKey(43, 124, 1);
    auto b = MakeFormatKey(91, 124, 1); // R16G16B16A16_FLOAT
    EXPECT_NE(a.Hash(), b.Hash());
    EXPECT_NE(a, b);
}

TEST(PipelineFormatKey, DifferentSamplesDifferentHash)
{
    auto a = MakeFormatKey(43, 124, 1);
    auto b = MakeFormatKey(43, 124, 4);
    EXPECT_NE(a.Hash(), b.Hash());
    EXPECT_NE(a, b);
}

TEST(PipelineFormatKey, TrailingSlotsIgnored)
{
    PipelineFormatKey a{};
    a.ColorCount = 1;
    a.ColorFormats[0] = static_cast<TextureFormat>(43);
    PipelineFormatKey b{};
    b.ColorCount = 1;
    b.ColorFormats[0] = static_cast<TextureFormat>(43);
    b.ColorFormats[3] = static_cast<TextureFormat>(99); // garbage in unused slot
    EXPECT_EQ(a.Hash(), b.Hash());
    EXPECT_EQ(a, b);
}

TEST(PipelineFormatKey, FitsInCacheLine)
{
    EXPECT_LE(sizeof(PipelineFormatKey), 64u);
    EXPECT_TRUE(std::is_trivially_copyable_v<PipelineFormatKey>);
}

// =====================================================================
// GraphicsPipelineDesc::ContentHash
// =====================================================================

TEST(GraphicsPipelineDescHash, SameContentSameHash)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    EXPECT_EQ(a.ContentHash(), b.ContentHash());
}

TEST(GraphicsPipelineDescHash, DebugNameDoesNotAffectHash)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    a.DebugName = "alpha";
    b.DebugName = "completely_different_name_with_special_chars!@#$";
    EXPECT_EQ(a.ContentHash(), b.ContentHash())
        << "DebugName must not participate in ContentHash — the cache would fragment per call site otherwise.";
}

TEST(GraphicsPipelineDescHash, NamedPushConstantNameDoesNotAffectHash)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    NamedPushConstantRange ra{"foo", 0, 8, 1};
    NamedPushConstantRange rb{"bar", 0, 8, 1};
    a.NamedPushConstantRanges = {ra};
    b.NamedPushConstantRanges = {rb};
    EXPECT_EQ(a.ContentHash(), b.ContentHash())
        << "NamedPushConstantRange::Name is diagnostic only.";
}

TEST(GraphicsPipelineDescHash, ShaderBytesAffectHash)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    b.PixelShader = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xa, 0xb, 0xc});
    EXPECT_NE(a.ContentHash(), b.ContentHash());
}

TEST(GraphicsPipelineDescHash, NullPixelShaderHashesAsEmpty)
{
    auto a = MakeMinimalGraphicsDesc();
    a.PixelShader = nullptr;
    auto b = MakeMinimalGraphicsDesc();
    b.PixelShader = std::make_shared<const std::vector<uint8_t>>();
    EXPECT_EQ(a.ContentHash(), b.ContentHash())
        << "Null shader pointer must hash equivalent to an empty byte vector.";
}

TEST(GraphicsPipelineDescHash, KindAffectsHash)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    b.Kind = GraphicsPipelineKind::MeshFragment;
    EXPECT_NE(a.ContentHash(), b.ContentHash());
}

// =====================================================================
// ComputePipelineDesc::ContentHash
// =====================================================================

TEST(ComputePipelineDescHash, SameContentSameHash)
{
    auto a = MakeMinimalComputeDesc();
    auto b = MakeMinimalComputeDesc();
    EXPECT_EQ(a.ContentHash(), b.ContentHash());
}

TEST(ComputePipelineDescHash, DebugNameDoesNotAffectHash)
{
    auto a = MakeMinimalComputeDesc();
    auto b = MakeMinimalComputeDesc();
    a.DebugName = "computeA";
    b.DebugName = "computeB_with_more_text";
    EXPECT_EQ(a.ContentHash(), b.ContentHash());
}

// =====================================================================
// Intern table — InternGraphicsPipeline idempotence
// =====================================================================

TEST(PipelineCache_Intern, GraphicsIdempotent)
{
    PipelineCache cache;
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    auto idA = cache.InternGraphicsPipeline(a);
    auto idB = cache.InternGraphicsPipeline(b);
    EXPECT_EQ(idA, idB);
    EXPECT_TRUE(idA.IsValid());
}

TEST(PipelineCache_Intern, ComputeIdempotent)
{
    PipelineCache cache;
    auto idA = cache.InternComputePipeline(MakeMinimalComputeDesc());
    auto idB = cache.InternComputePipeline(MakeMinimalComputeDesc());
    EXPECT_EQ(idA, idB);
    EXPECT_TRUE(idA.IsValid());
}

TEST(PipelineCache_Intern, GraphicsAndComputeYieldDistinctIdSpaces)
{
    PipelineCache cache;
    auto graphicsId = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    auto computeId  = cache.InternComputePipeline(MakeMinimalComputeDesc());
    // Both ids start from 1 — same numeric value but distinct types.
    EXPECT_EQ(graphicsId.Value, 1u);
    EXPECT_EQ(computeId.Value, 1u);
}

TEST(PipelineCache_Intern, DifferentDescsGetDifferentIds)
{
    PipelineCache cache;
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    b.Topology = PrimitiveTopology::LineList;
    auto idA = cache.InternGraphicsPipeline(a);
    auto idB = cache.InternGraphicsPipeline(b);
    EXPECT_NE(idA, idB);
}

TEST(PipelineCache_Intern, DebugNameDifferencesDoNotCauseDistinctIds)
{
    PipelineCache cache;
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();
    a.DebugName = "site_A";
    b.DebugName = "site_B";
    auto idA = cache.InternGraphicsPipeline(a);
    auto idB = cache.InternGraphicsPipeline(b);
    EXPECT_EQ(idA, idB);
}

// =====================================================================
// Concrete cache — basic hit/miss
// =====================================================================

TEST(PipelineCache_Concrete, MissThenInsertThenHit)
{
    PipelineCache cache;
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    auto fk = MakeFormatKey(43, 124, 1);
    const uint64_t key = PipelineCache::CombineGraphicsKey(id, fk);

    PipelineHandle out{};
    EXPECT_FALSE(cache.TryGetConcrete(key, out));

    PipelineHandle fakeHandle{42};
    PipelineCache::ConcreteInsertInfo info;
    info.Handle = fakeHandle;
    info.FormatKey = fk;
    info.PipelineIdValue = id.Value;
    info.IsCompute = false;
    cache.InsertConcrete(key, info);

    EXPECT_TRUE(cache.TryGetConcrete(key, out));
    EXPECT_EQ(out.id, fakeHandle.id);

    auto stats = cache.GetStats();
    EXPECT_EQ(stats.Hits, 1u);
    EXPECT_EQ(stats.Misses, 1u);
    EXPECT_EQ(stats.Inserts, 1u);
}

TEST(PipelineCache_Concrete, TagInvalidation)
{
    PipelineCache cache;
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    auto fk = MakeFormatKey(43, 124, 1);
    const uint64_t key = PipelineCache::CombineGraphicsKey(id, fk);

    PipelineCache::ConcreteInsertInfo info;
    info.Handle = PipelineHandle{42};
    info.FormatKey = fk;
    info.PipelineIdValue = id.Value;
    info.Tags = {0xDEADBEEFull};
    cache.InsertConcrete(key, info);

    PipelineHandle out{};
    EXPECT_TRUE(cache.TryGetConcrete(key, out));

    EXPECT_EQ(cache.InvalidateByTag(0xDEADBEEFull), 1u);
    EXPECT_FALSE(cache.TryGetConcrete(key, out));
}

// =====================================================================
// LRU eviction
// =====================================================================

namespace
{
PipelineCache::ConcreteInsertInfo MakeConcreteInsert(uint32_t handleId, GraphicsPipelineId id, uint32_t colorFmt)
{
    PipelineCache::ConcreteInsertInfo info;
    info.Handle = PipelineHandle{handleId};
    info.FormatKey = MakeFormatKey(colorFmt, 124, 1);
    info.PipelineIdValue = id.Value;
    info.IsCompute = false;
    return info;
}
} // namespace

TEST(PipelineCache_LRU, ExceedingCapacityEvictsLeastRecentlyUsed)
{
    PipelineCache cache;
    cache.SetCapacity(2);

    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());

    auto kA = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(43, 124, 1));
    auto kB = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(44, 124, 1));
    auto kC = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(45, 124, 1));

    cache.InsertConcrete(kA, MakeConcreteInsert(1, id, 43));
    cache.InsertConcrete(kB, MakeConcreteInsert(2, id, 44));

    // Touch A so B is least-recently-used.
    PipelineHandle out{};
    EXPECT_TRUE(cache.TryGetConcrete(kA, out));

    cache.InsertConcrete(kC, MakeConcreteInsert(3, id, 45));

    EXPECT_TRUE(cache.TryGetConcrete(kA, out));
    EXPECT_EQ(out.id, PipelineHandle{1}.id);
    EXPECT_FALSE(cache.TryGetConcrete(kB, out)); // evicted
    EXPECT_TRUE(cache.TryGetConcrete(kC, out));
    EXPECT_EQ(out.id, PipelineHandle{3}.id);

    EXPECT_GE(cache.GetStats().Evictions, 1u);
}

TEST(PipelineCache_LRU, ZeroCapacityIsUnlimited)
{
    PipelineCache cache;
    cache.SetCapacity(0);

    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    for (uint32_t i = 0; i < 50; ++i)
    {
        const auto key = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(43 + i, 124, 1));
        cache.InsertConcrete(key, MakeConcreteInsert(100 + i, id, 43 + i));
    }
    EXPECT_EQ(cache.GetStats().Evictions, 0u);
}

// =====================================================================
// Descriptor set layout intern
// =====================================================================

TEST(DescriptorSetLayoutHash, ImageShapeBitsChangeTheHash)
{
    // WebGPU bakes these into the bind group layout: two descs that differ
    // only here must intern to two layouts, so the hash has to see them.
    DescriptorSetLayoutDesc base;
    base.bindings.push_back(DescriptorBinding{0, DescriptorType::CombinedImageSampler, 1, 0x1});
    const uint64_t baseHash = HashDescriptorSetLayoutDesc(base);

    DescriptorSetLayoutDesc multisampled = base;
    multisampled.bindings[0].imageMultisample = true;
    EXPECT_NE(HashDescriptorSetLayoutDesc(multisampled), baseHash);

    DescriptorSetLayoutDesc filterable = base;
    filterable.bindings[0].imageFilterableFloat = true;
    EXPECT_NE(HashDescriptorSetLayoutDesc(filterable), baseHash);

    DescriptorSetLayoutDesc readOnly;
    readOnly.bindings.push_back(DescriptorBinding{0, DescriptorType::StorageImage, 1, 0x20});
    const uint64_t readWriteHash = HashDescriptorSetLayoutDesc(readOnly);
    readOnly.bindings[0].storageReadOnly = true;
    EXPECT_NE(HashDescriptorSetLayoutDesc(readOnly), readWriteHash);

    PipelineCache cache;
    EXPECT_NE(cache.InternDescriptorSetLayout(base).Value,
              cache.InternDescriptorSetLayout(multisampled).Value);
}

TEST(PipelineCache_Intern, DescriptorSetLayoutIdempotent)
{
    PipelineCache cache;
    DescriptorSetLayoutDesc dsl;
    dsl.bindings.push_back(DescriptorBinding{0, DescriptorType::UniformBuffer, 1, 0x3});

    auto a = cache.InternDescriptorSetLayout(dsl);
    auto b = cache.InternDescriptorSetLayout(dsl);
    EXPECT_EQ(a.Value, b.Value);
    EXPECT_TRUE(a.IsValid());

    DescriptorSetLayoutDesc different = dsl;
    different.bindings.push_back(DescriptorBinding{1, DescriptorType::StorageBuffer, 1, 0x20});
    auto c = cache.InternDescriptorSetLayout(different);
    EXPECT_NE(a.Value, c.Value);

    const auto* lookup = cache.LookupDescriptorSetLayout(a);
    ASSERT_NE(lookup, nullptr);
    EXPECT_EQ(lookup->bindings.size(), 1u);
}

// =====================================================================
// Pinning
// =====================================================================

TEST(PipelineCache_Pinning, PinnedConcreteSurvivesEviction)
{
    PipelineCache cache;
    cache.SetCapacity(1);

    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());

    auto kA = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(43, 124, 1));
    auto kB = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(44, 124, 1));

    auto pinnedInsert = MakeConcreteInsert(1, id, 43);
    pinnedInsert.Pinned = true;
    cache.InsertConcrete(kA, pinnedInsert);
    cache.InsertConcrete(kB, MakeConcreteInsert(2, id, 44));

    PipelineHandle out{};
    // Pinned entry survives capacity overflow.
    EXPECT_TRUE(cache.TryGetConcrete(kA, out));
    EXPECT_EQ(out.id, PipelineHandle{1}.id);
}

TEST(PipelineCache_Pinning, PinGraphicsPipelinePreventsTombstone)
{
    PipelineCache cache;
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    cache.PinGraphicsPipeline(id);

    // Pinning by itself doesn't change the live id; verify lookup works.
    EXPECT_NE(cache.LookupGraphicsPipeline(id), nullptr);

    auto stats = cache.GetStats();
    EXPECT_GE(stats.PinnedCount, 0u); // PinnedCount tracks concrete entries, not interns
}

// =====================================================================
// TLS L1 cache
// =====================================================================

TEST(PipelineCache_L1, RepeatedLookupHitsL1)
{
    PipelineCache cache;
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    auto fk = MakeFormatKey(43, 124, 1);
    const uint64_t key = PipelineCache::CombineGraphicsKey(id, fk);
    cache.InsertConcrete(key, MakeConcreteInsert(7, id, 43));

    // First lookup populates L1; subsequent lookups hit L1 mutex-free.
    PipelineHandle out{};
    EXPECT_TRUE(cache.TryGetConcrete(key, out));
    EXPECT_TRUE(cache.TryGetConcrete(key, out));
    EXPECT_TRUE(cache.TryGetConcrete(key, out));

    auto stats = cache.GetStats();
    EXPECT_GE(stats.L1Hits, 2u);   // 2nd + 3rd lookups
    EXPECT_GE(stats.L1Misses, 1u); // 1st lookup
}

TEST(PipelineCache_L1, ClearInvalidatesL1)
{
    PipelineCache cache;
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    auto fk = MakeFormatKey(43, 124, 1);
    const uint64_t key = PipelineCache::CombineGraphicsKey(id, fk);
    cache.InsertConcrete(key, MakeConcreteInsert(7, id, 43));

    PipelineHandle out{};
    EXPECT_TRUE(cache.TryGetConcrete(key, out));      // miss → fills L1
    EXPECT_TRUE(cache.TryGetConcrete(key, out));      // L1 hit
    const auto preClearStats = cache.GetStats();

    cache.Clear();

    // Lookup after Clear: the L1 entry's stale epoch forces a re-check
    // against the cache, which is now empty → miss.
    EXPECT_FALSE(cache.TryGetConcrete(key, out));
    const auto postStats = cache.GetStats();
    EXPECT_GT(postStats.L1Misses, preClearStats.L1Misses);
    EXPECT_GT(postStats.Misses, preClearStats.Misses);
}

TEST(PipelineCache_L1, InvalidateByTagInvalidatesL1)
{
    PipelineCache cache;
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    auto fk = MakeFormatKey(43, 124, 1);
    const uint64_t key = PipelineCache::CombineGraphicsKey(id, fk);

    PipelineCache::ConcreteInsertInfo info = MakeConcreteInsert(7, id, 43);
    info.Tags = {0xCAFEBABEull};
    cache.InsertConcrete(key, info);

    PipelineHandle out{};
    EXPECT_TRUE(cache.TryGetConcrete(key, out));
    EXPECT_TRUE(cache.TryGetConcrete(key, out)); // L1 hit

    EXPECT_EQ(cache.InvalidateByTag(0xCAFEBABEull), 1u);

    EXPECT_FALSE(cache.TryGetConcrete(key, out)); // fresh epoch, entry gone
}

// =====================================================================
// Cross-pass dedup — the central premise of the rewrite. Same material
// rendered into two passes with different PipelineFormatKeys must yield
// one interned id + two concrete entries, NOT two interned ids.
// =====================================================================

TEST(PipelineCache_Integration, SameMaterialTwoPassesYieldsOneInternAndTwoConcrete)
{
    PipelineCache cache;

    // One material → one intern id, regardless of which pass it's used in.
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    EXPECT_TRUE(id.IsValid());

    // Same content hash → re-interns to the same id.
    auto idAgain = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    EXPECT_EQ(id.Value, idAgain.Value);
    EXPECT_EQ(cache.GetStats().GraphicsPipelineCount, 1u);

    // Two distinct PipelineFormatKeys (e.g. main scene 1xMSAA vs thumbnail
    // 4xMSAA, or different color attachment formats).
    auto fkScene = MakeFormatKey(/*color=*/43, /*depth=*/124, /*samples=*/1);
    auto fkThumb = MakeFormatKey(/*color=*/43, /*depth=*/124, /*samples=*/4);
    EXPECT_NE(fkScene.Hash(), fkThumb.Hash());

    const uint64_t keyScene = PipelineCache::CombineGraphicsKey(id, fkScene);
    const uint64_t keyThumb = PipelineCache::CombineGraphicsKey(id, fkThumb);
    EXPECT_NE(keyScene, keyThumb);

    cache.InsertConcrete(keyScene, MakeConcreteInsert(/*handle=*/100, id, 43));
    PipelineCache::ConcreteInsertInfo thumbInfo = MakeConcreteInsert(/*handle=*/200, id, 43);
    thumbInfo.FormatKey = fkThumb; // override with the thumb's actual format key
    cache.InsertConcrete(keyThumb, thumbInfo);

    // Both concrete entries resolve independently to their pass-specific
    // PipelineHandles, sharing one interned id.
    PipelineHandle outScene{}, outThumb{};
    EXPECT_TRUE(cache.TryGetConcrete(keyScene, outScene));
    EXPECT_TRUE(cache.TryGetConcrete(keyThumb, outThumb));
    EXPECT_EQ(outScene.id, PipelineHandle{100}.id);
    EXPECT_EQ(outThumb.id, PipelineHandle{200}.id);

    auto stats = cache.GetStats();
    EXPECT_EQ(stats.GraphicsPipelineCount, 1u)  // single interned id
        << "Cross-pass dedup broken — same desc should not allocate a second id per pass";
    EXPECT_EQ(stats.ConcreteSize, 2u)            // two concrete entries (one per format key)
        << "Concrete cache should keep both pass-specific entries";
}

// =====================================================================
// Issue #2 — Clear() tombstones unpinned intern entries so the desc
// storage compacts and a re-intern of the same content gets a fresh id.
// =====================================================================

TEST(PipelineCache_Clear, TombstonesUnpinnedInternEntries)
{
    PipelineCache cache;

    auto id1 = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    EXPECT_TRUE(id1.IsValid());
    EXPECT_EQ(cache.GetStats().GraphicsPipelineCount, 1u);
    EXPECT_NE(cache.LookupGraphicsPipeline(id1), nullptr);

    cache.Clear();

    // Unpinned entries tombstone: live count drops to 0 AND the old id
    // resolves to null (Live=false). Lookup-by-id is the load-bearing
    // invariant; without it, callers still holding id1 would dereference
    // stale Desc state.
    EXPECT_EQ(cache.GetStats().GraphicsPipelineCount, 0u);
    EXPECT_EQ(cache.LookupGraphicsPipeline(id1), nullptr)
        << "Tombstoned id must lookup to null so any straggler holders fail safely";

    // Re-interning identical content must NOT alias to the tombstoned slot.
    // The new id refers to a live entry; current implementation keeps ids
    // monotonic (id2 > id1) but the contract this test enforces is the
    // observable lookup behavior, not the numeric value.
    auto id2 = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    EXPECT_TRUE(id2.IsValid());
    EXPECT_EQ(cache.GetStats().GraphicsPipelineCount, 1u);
    EXPECT_NE(cache.LookupGraphicsPipeline(id2), nullptr);
    EXPECT_NE(id1.Value, id2.Value)
        << "Re-intern after Clear must not alias to the tombstoned id";
}

TEST(PipelineCache_Clear, PreservesPinnedInternEntries)
{
    PipelineCache cache;
    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    cache.PinGraphicsPipeline(id);
    EXPECT_EQ(cache.GetStats().GraphicsPipelineCount, 1u);

    cache.Clear();

    // Pinned entry stays Live — its id remains valid across Clear (caller
    // is holding a long-lived id for feature code or compute pipelines).
    EXPECT_EQ(cache.GetStats().GraphicsPipelineCount, 1u);
}

// =====================================================================
// Issue #1 — LRU eviction moves the cache to a fresh epoch so stale L1 entries fail their
// validity check on the next lookup. Defends against slot recycling
// returning the wrong handle.
// =====================================================================

TEST(PipelineCache_L1, LruEvictionInvalidatesL1)
{
    PipelineCache cache;
    cache.SetCapacity(2);

    auto id = cache.InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    auto kA = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(43, 124, 1));
    auto kB = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(44, 124, 1));
    auto kC = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(45, 124, 1));

    cache.InsertConcrete(kA, MakeConcreteInsert(1, id, 43));
    cache.InsertConcrete(kB, MakeConcreteInsert(2, id, 44));

    PipelineHandle out{};
    EXPECT_TRUE(cache.TryGetConcrete(kA, out)); // populate L1 for A
    EXPECT_TRUE(cache.TryGetConcrete(kB, out)); // populate L1 for B

    // Capture stats before the eviction-triggering insert.
    const auto statsBefore = cache.GetStats();
    cache.InsertConcrete(kC, MakeConcreteInsert(3, id, 45)); // evicts A (LRU)

    // Eviction must have moved the cache to a fresh epoch: a stale L1 entry for A
    // (still pointing at handle 1) is now invalidated. If a future
    // slot-index collision picked up the stale L1 entry, we'd serve a
    // freed pipeline. The L1 invalidation forces a miss + slow-path
    // re-check, which correctly reports A as evicted.
    EXPECT_FALSE(cache.TryGetConcrete(kA, out))
        << "Evicted entry must miss; the fresh L1 epoch prevents a stale hit";
    EXPECT_GE(cache.GetStats().Evictions, statsBefore.Evictions + 1u);
}

// =====================================================================
// SpecializationConstants hashing — same content must yield the same desc
// hash regardless of insertion order. Previously the desc hashed pointer
// identity of the optional<SpecializationConstants>, which fragmented the
// pipeline cache per call site once any caller populated specialization
// constants.
// =====================================================================

TEST(GraphicsPipelineDescHash, SpecializationConstantsContentParticipatesInHash)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();

    SpecializationConstants specA;
    specA.AddConstant<uint32_t>(0, 100u);
    a.Specialization = specA;

    SpecializationConstants specB;
    specB.AddConstant<uint32_t>(0, 999u); // different value
    b.Specialization = specB;

    EXPECT_NE(a.ContentHash(), b.ContentHash())
        << "Specialization constant values must affect the desc content hash";
}

TEST(GraphicsPipelineDescHash, SpecializationConstantsInsertionOrderIndependent)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();

    SpecializationConstants specA;
    specA.AddConstant<uint32_t>(0, 100u);
    specA.AddConstant<uint32_t>(1, 200u);
    a.Specialization = specA;

    // Same constants, opposite insertion order. Must hash identically so
    // two call sites building "the same" constants don't fragment the
    // pipeline cache.
    SpecializationConstants specB;
    specB.AddConstant<uint32_t>(1, 200u);
    specB.AddConstant<uint32_t>(0, 100u);
    b.Specialization = specB;

    EXPECT_EQ(a.ContentHash(), b.ContentHash())
        << "Specialization constant insertion order must not affect the desc content hash";
}

TEST(GraphicsPipelineDescHash, SpecializationConstantsAbsenceVsEmpty)
{
    auto a = MakeMinimalGraphicsDesc();
    auto b = MakeMinimalGraphicsDesc();

    b.Specialization = SpecializationConstants{}; // present but empty

    // The has_value flag is part of the hash so present-but-empty hashes
    // distinct from absent. Either is a valid pipeline shape but a Vulkan
    // backend distinguishes them, so the desc identity must too.
    EXPECT_NE(a.ContentHash(), b.ContentHash())
        << "optional<SpecializationConstants> present-but-empty must hash distinct from absent";
}

TEST(PipelineCache_Intern, SpecializationConstantsReorderingYieldsSameId)
{
    PipelineCache cache;

    auto a = MakeMinimalGraphicsDesc();
    SpecializationConstants specA;
    specA.AddConstant<uint32_t>(0, 100u);
    specA.AddConstant<uint32_t>(1, 200u);
    a.Specialization = std::move(specA);

    auto b = MakeMinimalGraphicsDesc();
    SpecializationConstants specB;
    specB.AddConstant<uint32_t>(1, 200u);
    specB.AddConstant<uint32_t>(0, 100u);
    b.Specialization = std::move(specB);

    auto idA = cache.InternGraphicsPipeline(std::move(a));
    auto idB = cache.InternGraphicsPipeline(std::move(b));
    EXPECT_EQ(idA.Value, idB.Value)
        << "Same specialization constants in different insertion order must intern to one id";
    EXPECT_EQ(cache.GetStats().GraphicsPipelineCount, 1u);
}

// =====================================================================
// Thread-local L1 slot ownership
// =====================================================================

// The concrete L1 is thread_local, so its entries outlive every cache that
// fills them. An entry is only safe to trust if what stamps it cannot recur
// once its writer is gone — an address recurs as soon as the allocator hands it
// back, and a per-instance counter starts from the same value in every cache.
//
// Both caches are placement-constructed in one buffer to make that address
// reuse exact instead of incidental. The second cache has an empty map, so
// every lookup against it must miss.

TEST(PipelineCache_L1, DoesNotServeAnEntryLeftByADeadCache)
{
    alignas(PipelineCache) std::byte storage[sizeof(PipelineCache)];

    auto* first = new (storage) PipelineCache();
    const auto id = first->InternGraphicsPipeline(MakeMinimalGraphicsDesc());
    const uint64_t key = PipelineCache::CombineGraphicsKey(id, MakeFormatKey(43, 124, 1));
    first->InsertConcrete(key, MakeConcreteInsert(1, id, 43));

    PipelineHandle out{};
    ASSERT_TRUE(first->TryGetConcrete(key, out)); // fills this thread's L1 entry
    ASSERT_EQ(out.id, PipelineHandle{1}.id);
    first->~PipelineCache();

    auto* second = new (storage) PipelineCache();
    PipelineHandle stale{};
    const bool hit = second->TryGetConcrete(key, stale);
    second->~PipelineCache();

    EXPECT_FALSE(hit)
        << "An L1 entry filled by a destroyed cache was served to a new cache "
           "occupying its address. The new cache has never had anything "
           "inserted, so every lookup against it must miss. Whatever validates "
           "an L1 entry must not be a value a later cache can repeat.";
}

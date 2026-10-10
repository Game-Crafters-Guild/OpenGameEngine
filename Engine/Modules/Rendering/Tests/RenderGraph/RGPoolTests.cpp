// Stage 1.2 / 2a isolation tests for the RenderGraph resource pools, over the REAL engine
// IDevice (headless Vulkan): reuse, frame-age eviction, bounded growth, deferred
// destruction, the invisible-view age-out, and cross-frame state carry.

#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Tests/RenderGraph/RGTestDevice.h"
#include "Tests/ScopedEnvVar.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace GameEngine::Rendering;     // TextureDesc/BufferDesc/handles/enums/IDevice
using namespace GameEngine::Rendering::RenderGraph; // RenderGraph pools
using GameEngine::Rendering::RenderGraph::Test::MakeHeadlessDevice;
using GameEngine::Rendering::Tests::ScopedEnvVar;

namespace
{
TextureDesc Tex(uint32_t w, uint32_t h, TextureFormat fmt = TextureFormat::RGBA8_UNORM)
{
    TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = static_cast<uint32_t>(fmt);
    d.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::RenderTarget);
    return d;
}
BufferDesc Buf(uint64_t size)
{
    BufferDesc d;
    d.size = size;
    d.usage = static_cast<uint32_t>(BufferUsage::Storage);
    d.memoryUsage = BufferMemoryUsage::DeviceLocal;
    return d;
}
} // namespace

#define RG_REQUIRE_DEVICE(dev)                                                                        \
    auto dev = MakeHeadlessDevice();                                                                   \
    if (!dev)                                                                                          \
    GTEST_SKIP() << "no headless device available"

// ── Transient pool ──────────────────────────────────────────────────────────

TEST(RGTransientPool, ReusesFreeEntryOfMatchingDescAcrossFrames)
{
    RG_REQUIRE_DEVICE(dev);
    RGTransientPool pool(dev.get());
    const TextureHandle h0 = pool.AcquireTexture(Tex(64, 64), 0);
    pool.ReleaseFrame(0);
    const TextureHandle h1 = pool.AcquireTexture(Tex(64, 64), 1);
    EXPECT_TRUE(h0.IsValid());
    EXPECT_EQ(h0, h1) << "matching desc must reuse the freed entry";
    EXPECT_EQ(pool.Allocs(), 1u);
    EXPECT_EQ(pool.Hits(), 1u);
    EXPECT_EQ(pool.TexturePoolSize(), 1u);
}

// Under the device's NaN fill a transient read before its first write must
// read the fill, so the pool hands out a fresh physical every frame.
TEST(RGTransientPool, DoesNotRecycleWhileTheDeviceFillsNewResourcesWithNaN)
{
    const ScopedEnvVar fill("GE_VK_FILL_NEW_TARGETS_NAN", "1");
    RG_REQUIRE_DEVICE(dev);
    ASSERT_TRUE(dev->DebugFillsNewResourcesWithNaN());
    RGTransientPool pool(dev.get());
    pool.AcquireTexture(Tex(64, 64), 0);
    pool.AcquireBuffer(Buf(256), 0);
    pool.ReleaseFrame(0);
    EXPECT_EQ(pool.TexturePoolSize(), 0u);
    EXPECT_EQ(pool.BufferPoolSize(), 0u);
    pool.AcquireTexture(Tex(64, 64), 1);
    pool.AcquireBuffer(Buf(256), 1);
    EXPECT_EQ(pool.Allocs(), 4u);
    EXPECT_EQ(pool.Hits(), 0u);
}

TEST(RGTransientPool, StableShapeReachesSteadyStateWithNoGrowthOver1000Frames)
{
    RG_REQUIRE_DEVICE(dev);
    RGTransientPool pool(dev.get());
    for (uint64_t f = 0; f < 1000; ++f)
    {
        pool.AcquireTexture(Tex(1920, 1080), f);
        pool.AcquireTexture(Tex(960, 540), f);
        pool.AcquireTexture(Tex(512, 512), f);
        pool.AcquireBuffer(Buf(4096), f);
        pool.AcquireBuffer(Buf(8192), f);
        pool.ReleaseFrame(f);
        pool.EvictIdle(f, /*maxIdleFrames=*/10);
    }
    EXPECT_EQ(pool.TexturePoolSize(), 3u);
    EXPECT_EQ(pool.BufferPoolSize(), 2u);
    EXPECT_EQ(pool.Allocs(), 5u) << "only the first frame allocates";
    const uint64_t bytes = pool.TextureBytesInPool() + pool.BufferBytesInPool();
    EXPECT_GT(bytes, 0u);
}

TEST(RGTransientPool, EvictsEntriesIdleBeyondMaxIdleFrames)
{
    RG_REQUIRE_DEVICE(dev);
    RGTransientPool pool(dev.get());
    for (uint64_t f = 0; f <= 2; ++f)
    {
        pool.AcquireTexture(Tex(128, 128), f);
        pool.ReleaseFrame(f);
        pool.EvictIdle(f, 5);
    }
    EXPECT_EQ(pool.TexturePoolSize(), 1u);
    for (uint64_t f = 3; f <= 9; ++f)
    {
        pool.ReleaseFrame(f);
        pool.EvictIdle(f, 5);
    }
    EXPECT_EQ(pool.TexturePoolSize(), 0u) << "idle entry reclaimed after maxIdleFrames";
}

TEST(RGTransientPool, BudgetEvictionCapsBytes)
{
    RG_REQUIRE_DEVICE(dev);
    RGTransientPool pool(dev.get());
    // Four distinct 4-bytes-per-texel formats at the same size -> equal byte cost.
    const TextureFormat fmts[4] = {TextureFormat::RGBA8_UNORM, TextureFormat::RGBA8_SRGB,
                                   TextureFormat::BGRA8_UNORM, TextureFormat::BGRA8_SRGB};
    pool.AcquireTexture(Tex(256, 256, fmts[0]), 0);
    const uint64_t perTex = pool.TextureBytesInPool();
    for (int i = 1; i < 4; ++i)
        pool.AcquireTexture(Tex(256, 256, fmts[i]), 0);
    pool.SetBudgetBytes(/*textureBudget=*/2 * perTex, /*bufferBudget=*/0);
    pool.ReleaseFrame(0);
    pool.EvictIdle(0, /*maxIdleFrames=*/100000);
    EXPECT_LE(pool.TextureBytesInPool(), 2 * perTex) << "budget enforced";
    EXPECT_EQ(pool.TexturePoolSize(), 2u);
}

// ── Cross-frame resource pool ───────────────────────────────────────────────

TEST(RGResourcePool, ReusesHandleForSameNameAndDesc)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle h0 = pool.GetOrCreateTexture("History", Tex(1920, 1080), 0);
    const TextureHandle h1 = pool.GetOrCreateTexture("History", Tex(1920, 1080), 1);
    EXPECT_TRUE(h0.IsValid());
    EXPECT_EQ(h0, h1);
    EXPECT_EQ(pool.Size(), 1u);
}

TEST(RGResourcePool, DescChangeReallocatesAndDefersOldUntilInFlightElapses)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle h0 = pool.GetOrCreateTexture("HiZ", Tex(1024, 1024), 0);
    const TextureHandle h1 = pool.GetOrCreateTexture("HiZ", Tex(2048, 2048), 1); // resize
    EXPECT_NE(h0, h1);
    EXPECT_EQ(pool.DeferredCount(), 1u);

    pool.TickPoolElements(/*frame=*/1, /*maxIdle=*/100, /*framesInFlight=*/2);
    EXPECT_EQ(pool.DeferredCount(), 1u) << "not yet retired (1 < 1+2)";

    pool.TickPoolElements(/*frame=*/3, 100, 2);
    EXPECT_EQ(pool.DeferredCount(), 0u) << "old handle retired after in-flight window";
    EXPECT_EQ(pool.Size(), 1u);
}

// A usage requirement is applied on the NEXT GetOrCreate (the current physical
// predates it), as one realloc; after that the importer's unwidened desc keeps
// matching, so a widened entry never realloc-thrashes against its importer.
TEST(RGResourcePool, RequiredUsageWidensTheNextMaterializationThenMatches)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle h0 = pool.GetOrCreateTexture("Output", Tex(256, 256), 0);
    pool.RequireTextureUsage("Output", TextureUsage::TransferSrc);
    EXPECT_EQ(pool.DeferredCount(), 0u) << "a requirement alone must not touch the live physical";

    bool fresh = false;
    const TextureHandle h1 = pool.GetOrCreateTexture("Output", Tex(256, 256), 1, &fresh);
    EXPECT_NE(h0, h1) << "the next materialization carries the required usage";
    EXPECT_TRUE(fresh) << "a widening realloc is a fresh physical like any other";
    EXPECT_EQ(pool.DeferredCount(), 1u);

    const TextureHandle h2 = pool.GetOrCreateTexture("Output", Tex(256, 256), 2, &fresh);
    EXPECT_EQ(h1, h2) << "the importer's unwidened desc must match the widened entry";
    EXPECT_EQ(pool.DeferredCount(), 1u) << "no second realloc";

    // Re-requiring what the entry already carries is a no-op.
    pool.RequireTextureUsage("Output", TextureUsage::TransferSrc);
    EXPECT_EQ(pool.GetOrCreateTexture("Output", Tex(256, 256), 3), h2);
    EXPECT_EQ(pool.DeferredCount(), 1u);
}

// The requirement is a fact about the NAME, not about one physical: a resize
// realloc must carry it, or the first capture after every resize would hit an
// unwidened target again.
TEST(RGResourcePool, RequiredUsageSurvivesAResizeRealloc)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    pool.GetOrCreateTexture("Output", Tex(256, 256), 0);
    pool.RequireTextureUsage("Output", TextureUsage::TransferSrc);
    const TextureHandle widened = pool.GetOrCreateTexture("Output", Tex(256, 256), 1);
    ASSERT_EQ(pool.DeferredCount(), 1u);

    const TextureHandle resized = pool.GetOrCreateTexture("Output", Tex(512, 512), 2);
    EXPECT_NE(widened, resized);
    ASSERT_EQ(pool.DeferredCount(), 2u);

    // Re-requiring after the resize must find nothing to widen: the resized
    // physical already carries it.
    pool.RequireTextureUsage("Output", TextureUsage::TransferSrc);
    EXPECT_EQ(pool.GetOrCreateTexture("Output", Tex(512, 512), 3), resized);
    EXPECT_EQ(pool.DeferredCount(), 2u) << "the resize realloc dropped the requirement";
}

TEST(RGResourcePool, AgesOutResourcesOfAHiddenView)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    for (uint64_t f = 0; f <= 3; ++f)
        pool.GetOrCreateTexture("GameView.Color", Tex(1920, 1080), f);
    EXPECT_EQ(pool.Size(), 1u);
    for (uint64_t f = 4; f <= 9; ++f)
        pool.TickPoolElements(f, /*maxIdle=*/5, /*framesInFlight=*/2);
    EXPECT_EQ(pool.Size(), 0u) << "hidden view's resource ages out";
}

// ── Freshness arm ───────────────────────────────────────────────────────────
// Pool memory is never zeroed and physicals are recycled from evicted entries,
// so a cross-frame history consumer (TAA / SSSR) needs to know when what it
// wrote is gone. Neither the name nor DescEqual can tell it: after an age-out,
// a resize realloc or a device rebuild the SAME name and desc come back over a
// DIFFERENT, undefined physical. These lock the four cases.

TEST(RGResourcePool, FreshnessArmClearsOnlyAfterInitializationAndStaysClearOnReuse)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    bool fresh = false;
    pool.GetOrCreateTexture("View0.History", Tex(256, 256), 0, &fresh);
    EXPECT_TRUE(fresh) << "first use is fresh: nothing was ever written";

    // Re-import BEFORE any executed frame wrote it: the arm must persist, or a
    // frame declared and then abandoned (swapchain acquire failure) would
    // silently discharge a zero-init that never recorded.
    fresh = false;
    pool.GetOrCreateTexture("View0.History", Tex(256, 256), 1, &fresh);
    EXPECT_TRUE(fresh) << "arm must survive until an executed frame initializes it";

    pool.MarkTextureInitialized("View0.History");
    fresh = true;
    pool.GetOrCreateTexture("View0.History", Tex(256, 256), 2, &fresh);
    EXPECT_FALSE(fresh) << "steady-state reuse must NOT report fresh";
    // Mutation: drop the `if (outNeedsFreshInit) *outNeedsFreshInit = e.NeedsFreshInit;`
    // re-report on the desc-match path and the abandoned-frame case goes red.
}

// The load-bearing one: this is the case no frame counter and no desc compare
// can see — a cross-frame history consumer would blend against uninitialized
// memory believing its own texels survived.
TEST(RGResourcePool, AgeOutThenReimportReportsFreshUnderSameNameAndDesc)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    bool fresh = false;
    pool.GetOrCreateTexture("HiddenView.History", Tex(512, 512), 0, &fresh);
    pool.MarkTextureInitialized("HiddenView.History");

    fresh = true;
    pool.GetOrCreateTexture("HiddenView.History", Tex(512, 512), 1, &fresh);
    ASSERT_FALSE(fresh) << "precondition: history is resident and initialized";

    // The view stops declaring (collapsed pane, background window) and ages out.
    for (uint64_t f = 2; f <= 12; ++f)
        pool.TickPoolElements(f, /*maxIdle=*/5, /*framesInFlight=*/2);
    ASSERT_EQ(pool.Size(), 0u) << "precondition: the entry was evicted";

    // The view comes back with byte-identical name and desc.
    fresh = false;
    pool.GetOrCreateTexture("HiddenView.History", Tex(512, 512), 13, &fresh);
    EXPECT_TRUE(fresh) << "a recycled physical must report fresh: blending against "
                          "it would read uninitialized memory";
    // Mutation: stop writing outNeedsFreshInit on the not-found path and this
    // goes red — which is exactly the pre-mechanism behaviour. (Dropping only
    // `e.NeedsFreshInit = true;` there is caught by the abandoned-frame assert
    // in FreshnessArmClears... instead: the out-param literal still reports the
    // creating frame itself.)
}

TEST(RGResourcePool, ResizeReallocReportsFresh)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    bool fresh = false;
    pool.GetOrCreateTexture("View0.History", Tex(1920, 1080), 0, &fresh);
    pool.MarkTextureInitialized("View0.History");

    fresh = false;
    pool.GetOrCreateTexture("View0.History", Tex(1600, 1080), 1, &fresh);
    EXPECT_TRUE(fresh) << "a desc change reallocates: the old contents are gone";

    // The realloc frame may be abandoned before Execute: the entry's arm must
    // persist so the next importer still sees fresh.
    fresh = false;
    pool.GetOrCreateTexture("View0.History", Tex(1600, 1080), 2, &fresh);
    EXPECT_TRUE(fresh) << "an abandoned realloc frame must not discharge the arm";
    // Mutations: stop writing outNeedsFreshInit on the realloc path -> first
    // assert red; drop `e.NeedsFreshInit = true;` there -> second assert red.
}

TEST(RGResourcePool, DeviceRebuildDropReportsFresh)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    bool fresh = false;
    pool.GetOrCreateTexture("View0.History", Tex(256, 256), 0, &fresh);
    pool.MarkTextureInitialized("View0.History");

    pool.DropAllAfterDeviceRebuild();

    fresh = false;
    pool.GetOrCreateTexture("View0.History", Tex(256, 256), 1, &fresh);
    EXPECT_TRUE(fresh) << "an in-place device rebuild freed the physical; the "
                          "recreated one holds nothing this view wrote";
    // Mutation: have DropAllAfterDeviceRebuild retain entries (or the not-found
    // path skip the arm) and this goes red.
}

TEST(RGResourcePool, StateCarriesAcrossFramesOnReuseAndResetsOnRealloc)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    pool.GetOrCreateTexture("Shadow", Tex(2048, 2048), 0);
    pool.SetState("Shadow", ResourceState::ShaderResource);
    pool.GetOrCreateTexture("Shadow", Tex(2048, 2048), 1); // reuse
    EXPECT_EQ(pool.GetState("Shadow"), ResourceState::ShaderResource) << "state carried across frames";

    pool.GetOrCreateTexture("Shadow", Tex(4096, 4096), 2); // desc change -> realloc
    EXPECT_EQ(pool.GetState("Shadow"), ResourceState::Undefined) << "fresh allocation resets state";
}

// ── Slice 0b: introspection enumeration (VRAM panel / MCP listing) ──────────

TEST(RGResourcePool, ForEachEntryEnumeratesNamesBytesAndState)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    pool.GetOrCreateTexture("View.History", Tex(64, 64), 0);
    pool.GetOrCreateTexture("View.HiZ", Tex(32, 32), 0);
    pool.GetOrCreateBuffer("View.Readback", Buf(256), 0);
    pool.SetState("View.History", ResourceState::ShaderResource);

    size_t count = 0;
    uint64_t bytes = 0;
    bool sawHistoryState = false;
    pool.ForEachEntry(
        [&](const RGResourcePool::EntryInfo& e)
        {
            ++count;
            bytes += e.Bytes;
            if (e.IsTexture && std::string(e.Name) == "View.History")
                sawHistoryState = e.State == ResourceState::ShaderResource;
        });
    EXPECT_EQ(count, pool.Size());
    EXPECT_EQ(bytes, pool.BytesInPool());
    EXPECT_TRUE(sawHistoryState);
}

TEST(RGTransientPool, ForEachEntryReflectsInUseLifecycle)
{
    RG_REQUIRE_DEVICE(dev);
    RGTransientPool pool(dev.get());
    pool.AcquireTexture(Tex(64, 64), 0);
    pool.AcquireBuffer(Buf(128), 0);

    size_t inUse = 0;
    pool.ForEachEntry([&](const RGTransientPool::EntryInfo& e) { inUse += e.InUse ? 1 : 0; });
    EXPECT_EQ(inUse, 2u);

    pool.ReleaseFrame(0);
    inUse = 0;
    size_t total = 0;
    pool.ForEachEntry(
        [&](const RGTransientPool::EntryInfo& e)
        {
            ++total;
            inUse += e.InUse ? 1 : 0;
        });
    EXPECT_EQ(total, 2u);
    EXPECT_EQ(inUse, 0u) << "released entries enumerate as free";
}

// ── Pool-owned single-mip views ─────────────────────────────────────────────
// Mip-chain compute (HZB / SPD) binds one level of a pooled texture at a time,
// which needs a single-mip VkImageView per level. The pool owns those views
// because it owns the image: every transition that releases a texture must
// release its views, and nothing outside the pool may hold them across a frame.

namespace
{
TextureDesc MipChainTex(uint32_t w, uint32_t h, uint32_t mips)
{
    TextureDesc d = Tex(w, h, TextureFormat::R32_FLOAT);
    d.mipLevels = mips;
    d.usage = static_cast<uint32_t>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
    return d;
}
} // namespace

// The rule GetOrCreateMipView applies when it decides between a 2D and a 2D
// ARRAY mip view. It must match the one the backend applies to an image's
// default view, or one texture presents two shapes: a sampler2DArray default
// SRV alongside an image2D storage view of the same image, or vice versa.
// ForceArrayView is the case a bare `arrayLayers > 1` misses — it is how a
// single-layer image declares itself array-shaped.
TEST(RGResourcePool, ArrayViewRuleMatchesTheBackendsDefaultViewRule)
{
    TextureDesc plain = MipChainTex(64, 64, 7);
    EXPECT_FALSE(NeedsArrayView(plain)) << "a plain single-layer image takes a 2D view";

    TextureDesc layered = MipChainTex(64, 64, 7);
    layered.arrayLayers = 4;
    EXPECT_TRUE(NeedsArrayView(layered)) << "more than one layer needs an array view";

    TextureDesc forced = MipChainTex(64, 64, 7);
    forced.flags = TextureCreateFlags::ForceArrayView;
    EXPECT_TRUE(NeedsArrayView(forced))
        << "a single-layer image declared array-shaped still needs an array view";
}

// Exercises the array branch of GetOrCreateMipView end to end. The view TYPE is
// not observable through any public device query, so this locks what is: an
// array image gets a valid, per-mip-distinct view for every level of its chain,
// through the same caching the single-layer tests cover.
TEST(RGResourcePool, MipViewsOfAnArrayImageAreCreatedPerMip)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    TextureDesc desc = MipChainTex(64, 64, 7);
    desc.arrayLayers = 4;
    const TextureHandle h = pool.GetOrCreateTexture("ShadowMinMaxPyramid", desc, 0);

    std::vector<TextureViewHandle> views;
    for (uint32_t mip = 0; mip < 7; ++mip)
    {
        views.push_back(pool.GetOrCreateMipView(h, mip));
        ASSERT_TRUE(views.back().IsValid()) << "array mip " << mip << " has no view";
    }
    for (uint32_t mip = 1; mip < 7; ++mip)
        EXPECT_NE(views[mip], views[mip - 1]) << "each mip gets its own view";
    EXPECT_EQ(pool.GetOrCreateMipView(h, 3), views[3]) << "array mip views cache like 2D ones";
}

TEST(RGResourcePool, MipViewsAreStableAcrossFramesForAnUnchangedPhysical)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle h = pool.GetOrCreateTexture("HiZ", MipChainTex(64, 64, 7), 0);
    const TextureViewHandle v0 = pool.GetOrCreateMipView(h, 3);
    ASSERT_TRUE(v0.IsValid());
    EXPECT_EQ(pool.GetOrCreateMipView(h, 3), v0) << "same mip must not be recreated";

    const TextureHandle h1 = pool.GetOrCreateTexture("HiZ", MipChainTex(64, 64, 7), 1);
    ASSERT_EQ(h1, h);
    EXPECT_EQ(pool.GetOrCreateMipView(h1, 3), v0) << "an unchanged physical keeps its views";
    EXPECT_NE(pool.GetOrCreateMipView(h1, 4), v0) << "each mip gets its own view";
}

// The two-HZB-view shape, at the layer that owns the lifetime: two pooled
// textures alternating (the Scene View's and the Game View's pyramids are
// separate pool entries, and one node builds both in one frame). Interleaved
// requests must be independent — a per-node cache keyed on "the current
// texture id" destroyed the other view's views on every alternation, while
// that frame's descriptor sets still referenced them.
TEST(RGResourcePool, MipViewsOfTwoPooledTexturesAreIndependentUnderAlternation)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle a = pool.GetOrCreateTexture("HiZ.ViewA", MipChainTex(64, 64, 7), 0);
    const TextureHandle b = pool.GetOrCreateTexture("HiZ.ViewB", MipChainTex(64, 64, 7), 0);
    ASSERT_NE(a, b);

    std::vector<TextureViewHandle> viewsA;
    std::vector<TextureViewHandle> viewsB;
    for (uint32_t mip = 0; mip < 7; ++mip)
    {
        viewsA.push_back(pool.GetOrCreateMipView(a, mip));
        viewsB.push_back(pool.GetOrCreateMipView(b, mip));
        ASSERT_TRUE(viewsA.back().IsValid());
        ASSERT_TRUE(viewsB.back().IsValid());
        EXPECT_NE(viewsA.back(), viewsB.back());
    }
    // Second frame, same alternation order: every handle is the one from frame 0.
    for (uint32_t mip = 0; mip < 7; ++mip)
    {
        EXPECT_EQ(pool.GetOrCreateMipView(a, mip), viewsA[mip]);
        EXPECT_EQ(pool.GetOrCreateMipView(b, mip), viewsB[mip]);
    }
}

TEST(RGResourcePool, MipViewsAreRebuiltAfterAResizeRealloc)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle h0 = pool.GetOrCreateTexture("HiZ", MipChainTex(64, 64, 7), 0);
    const TextureViewHandle v0 = pool.GetOrCreateMipView(h0, 2);
    ASSERT_TRUE(v0.IsValid());

    const TextureHandle h1 = pool.GetOrCreateTexture("HiZ", MipChainTex(128, 128, 8), 1);
    ASSERT_NE(h1, h0);
    // The realloc'd image's views ride the deferred record: still alive while
    // in-flight frames may reference them, gone with the image afterwards.
    EXPECT_EQ(pool.DeferredCount(), 1u);
    const TextureViewHandle v1 = pool.GetOrCreateMipView(h1, 2);
    EXPECT_TRUE(v1.IsValid());
    EXPECT_NE(v1, v0) << "a fresh physical must not reuse the old image's views";
    EXPECT_FALSE(pool.GetOrCreateMipView(h0, 2).IsValid())
        << "the pool no longer owns the old handle and must not vend views for it";

    pool.TickPoolElements(/*frame=*/3, /*maxIdle=*/100, /*framesInFlight=*/2);
    EXPECT_EQ(pool.DeferredCount(), 0u);
    EXPECT_EQ(pool.GetOrCreateMipView(h1, 2), v1) << "the live entry is untouched by the retire";
}

TEST(RGResourcePool, MipViewsGoAwayWithAnAgedOutTexture)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle h = pool.GetOrCreateTexture("GameView.HiZ", MipChainTex(64, 64, 7), 0);
    ASSERT_TRUE(pool.GetOrCreateMipView(h, 1).IsValid());
    for (uint64_t f = 1; f <= 9; ++f)
        pool.TickPoolElements(f, /*maxIdle=*/5, /*framesInFlight=*/2);
    ASSERT_EQ(pool.Size(), 0u);
    EXPECT_FALSE(pool.GetOrCreateMipView(h, 1).IsValid())
        << "views of an aged-out texture must not survive the image";
}

TEST(RGResourcePool, MipViewIsRefusedForAnUntrackedTextureOrAnOutOfRangeMip)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle pooled = pool.GetOrCreateTexture("HiZ", MipChainTex(64, 64, 7), 0);
    EXPECT_FALSE(pool.GetOrCreateMipView(pooled, 7).IsValid()) << "mip past the chain";
    EXPECT_FALSE(pool.GetOrCreateMipView(TextureHandle{}, 0).IsValid());

    const TextureHandle foreign = dev->CreateTexture(MipChainTex(64, 64, 7));
    EXPECT_FALSE(pool.GetOrCreateMipView(foreign, 0).IsValid())
        << "the pool cannot own views of a texture whose lifetime it does not track";
    dev->DestroyTexture(foreign);
}

// The dock-splitter shape: one view resizes while the other keeps rendering.
// Both pyramids are live in the same frame, so the resized entry's views must
// ride its own deferred record and the untouched entry's views must not move.
TEST(RGResourcePool, ResizingOnePooledTextureLeavesTheOtherEntrysMipViewsUntouched)
{
    RG_REQUIRE_DEVICE(dev);
    RGResourcePool pool(dev.get());
    const TextureHandle a0 = pool.GetOrCreateTexture("HiZ.ViewA", MipChainTex(64, 64, 7), 0);
    const TextureHandle b = pool.GetOrCreateTexture("HiZ.ViewB", MipChainTex(64, 64, 7), 0);
    ASSERT_NE(a0, b);

    std::vector<TextureViewHandle> viewsA0;
    std::vector<TextureViewHandle> viewsB;
    for (uint32_t mip = 0; mip < 7; ++mip)
    {
        viewsA0.push_back(pool.GetOrCreateMipView(a0, mip));
        viewsB.push_back(pool.GetOrCreateMipView(b, mip));
        ASSERT_TRUE(viewsA0.back().IsValid());
        ASSERT_TRUE(viewsB.back().IsValid());
    }

    // Only A resizes; B re-imports at its unchanged desc, as a live view does.
    // Handle inequality alone would not notice the outgoing views being freed
    // here, so count destroys at the device: the realloc must free nothing.
    const size_t destroysBeforeResize = dev->GetResourcePoolStats().deferredTextureViews;
    const TextureHandle a1 = pool.GetOrCreateTexture("HiZ.ViewA", MipChainTex(128, 128, 8), 1);
    ASSERT_NE(a1, a0);
    ASSERT_EQ(pool.GetOrCreateTexture("HiZ.ViewB", MipChainTex(64, 64, 7), 1), b);
    EXPECT_EQ(pool.DeferredCount(), 1u) << "only the resized entry defers";
    EXPECT_EQ(dev->GetResourcePoolStats().deferredTextureViews, destroysBeforeResize)
        << "the realloc destroyed the outgoing image's views instead of carrying them into its "
           "deferred record — frames still in flight name those views";

    for (uint32_t mip = 0; mip < 7; ++mip)
        EXPECT_EQ(pool.GetOrCreateMipView(b, mip), viewsB[mip])
            << "mip " << mip << " of the untouched entry was rebuilt by its neighbour's resize";

    EXPECT_FALSE(pool.GetOrCreateMipView(a0, 2).IsValid())
        << "the retired physical must not be vended a view";
    // The old chain is deferred, not destroyed, so no handle can have been
    // recycled yet: a match here means a stale view was handed out.
    for (uint32_t mip = 0; mip < 8; ++mip)
    {
        const TextureViewHandle v = pool.GetOrCreateMipView(a1, mip);
        ASSERT_TRUE(v.IsValid());
        EXPECT_EQ(std::find(viewsA0.begin(), viewsA0.end(), v), viewsA0.end())
            << "the fresh chain handed back a view belonging to the retired image";
    }

    // The retire window closes: the outgoing chain goes, and all of it — held
    // forever would be a leak, not safety.
    pool.TickPoolElements(/*frame=*/3, /*maxIdle=*/100, /*framesInFlight=*/2);
    EXPECT_EQ(pool.DeferredCount(), 0u);
    EXPECT_EQ(dev->GetResourcePoolStats().deferredTextureViews, destroysBeforeResize + 7u)
        << "the retire did not release the outgoing image's 7 mip views";
    for (uint32_t mip = 0; mip < 7; ++mip)
        EXPECT_EQ(pool.GetOrCreateMipView(b, mip), viewsB[mip])
            << "the neighbour's views were freed by the resized entry's retire";
}

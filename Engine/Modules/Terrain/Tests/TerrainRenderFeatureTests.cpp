#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include "Terrain/TerrainTypes.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/CommandList.h"
#include "TestDeviceHelper.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using namespace GameEngine::Terrain;
using namespace GameEngine::Rendering;

class TerrainRenderFeatureTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_Device = CreateVulkanDeviceFast();
        if (!m_Device)
            GTEST_SKIP() << "No Vulkan device available";
    }

    void TearDown() override
    {
        m_Feature = nullptr; // must destroy before device
        if (m_Device)
            m_Device->Shutdown();
    }

    std::unique_ptr<IDevice> m_Device;
    std::unique_ptr<TerrainRenderFeature> m_Feature;
};

TEST_F(TerrainRenderFeatureTest, Initialize_Succeeds)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    EXPECT_TRUE(m_Feature->Initialize(m_Device.get()));
    EXPECT_TRUE(m_Feature->IsInitialized());
}

TEST_F(TerrainRenderFeatureTest, UploadHeightmap_CreatesTexture)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    const uint32 width = 33, height = 33;
    std::vector<float32> samples(width * height, 0.5f);
    TerrainHandle handle{0, 1};

    m_Feature->UploadHeightmap(handle, samples.data(), width, height);

    auto tex = m_Feature->GetHeightmapTexture(handle);
    EXPECT_TRUE(tex.IsValid());
}

TEST_F(TerrainRenderFeatureTest, TerrainParams_UploadAndRetrieve)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    TerrainGPUParams params{};
    params.WorldOriginX = 10.0f;
    params.WorldSizeX = 1024.0f;
    params.HeightScale = 128.0f;

    m_Feature->UploadTerrainParamsArray(&params, 1, Terrain::kDefaultTerrainMaterials,
                                       Terrain::kTerrainLayerRoleCount, 0);

    EXPECT_TRUE(m_Feature->GetTerrainParamsSSBO(0).IsValid());
    EXPECT_EQ(m_Feature->GetTerrainParamsCount(0), 1u);
    EXPECT_EQ(m_Feature->GetLastTerrainParamsSlot(), 0u);
}

TEST_F(TerrainRenderFeatureTest, ActiveTerrains_SetGet)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    std::vector<TerrainInstanceInfo> terrains(2);
    terrains[0].SizeX = 512.0f;
    terrains[1].SizeX = 1024.0f;

    m_Feature->SetActiveTerrains(std::move(terrains));

    auto retrieved = m_Feature->GetActiveTerrains();
    ASSERT_EQ(retrieved.size(), 2u);
    EXPECT_FLOAT_EQ(retrieved[0].SizeX, 512.0f);
    EXPECT_FLOAT_EQ(retrieved[1].SizeX, 1024.0f);
}

// ---- C8: unified tiled-terrain heightmap render-source oracles ----
//
// A tiled terrain renders through one unified heightmap that per-tile band uploads
// patch on the graphics queue. These tests read the GPU texture back and compare it
// to CPU truth, proving the baked heights land in the right sub-rects.

namespace
{
// Deterministic stitched height as a function of GLOBAL sample position, so
// adjacent tiles agree at shared boundary samples (as world-space noise does).
inline float32 StitchedHeight(uint32 gx, uint32 gz)
{
    return 0.5f * static_cast<float32>(gx) + 0.25f * static_cast<float32>(gz);
}

// Flush pending uploads and read the unified heightmap back into `out`, restoring
// the resting ShaderResource layout so a subsequent region patch is content-valid.
// Everything runs in one command list so the copy sees the tracked upload layout.
void FlushAndReadHeightmap(TerrainRenderFeature& feat, IDevice* dev, TerrainHandle handle,
                           uint32 w, uint32 h, std::vector<float32>& out)
{
    const TextureHandle tex = feat.GetHeightmapTexture(handle);
    ASSERT_TRUE(tex.IsValid());
    const BufferHandle rb = dev->CreateReadbackBuffer(static_cast<size_t>(w) * h * sizeof(float32));
    ASSERT_TRUE(rb.IsValid());

    auto cl = dev->CreateCommandList(IDevice::QueueType::Graphics);
    ASSERT_TRUE(cl);
    cl->Begin();
    feat.FlushPendingUploads(cl.get());
    cl->CopyTextureSubresourceToBuffer(tex, 0, 0, rb, w, h, 0, 0, 0, 0);
    cl->Barrier(ResourceBarrier::CreateTextureBarrier(
        tex, ResourceState::CopySource, ResourceState::ShaderResource));
    cl->End();
    dev->ExecuteCommandLists({cl.get()});
    dev->FinalizeFrame();
    dev->WaitForIdle();

    const auto* data = static_cast<const float32*>(dev->MapBuffer(rb));
    ASSERT_NE(data, nullptr);
    out.assign(data, data + static_cast<size_t>(w) * h);
    dev->UnmapBuffer(rb);
    dev->DestroyBuffer(rb);
}
} // namespace

TEST_F(TerrainRenderFeatureTest, UnifiedTiledHeightmap_ReadbackMatchesStitchedTiles)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    constexpr uint32 kTileRes = 5;              // interior 4
    constexpr uint32 kTilesX = 2, kTilesZ = 2;
    constexpr uint32 kInterior = kTileRes - 1;
    const uint32 unifiedW = kTilesX * kInterior + 1; // 9
    const uint32 unifiedH = kTilesZ * kInterior + 1; // 9
    const TerrainHandle handle{0, 1};

    m_Feature->EnsureUnifiedTiledTextures(handle, unifiedW, unifiedH);

    // Patch every tile's sub-rect from CPU tiles carved out of the stitched field.
    for (uint32 tz = 0; tz < kTilesZ; ++tz)
    for (uint32 tx = 0; tx < kTilesX; ++tx)
    {
        std::vector<float32> tile(kTileRes * kTileRes);
        for (uint32 lz = 0; lz < kTileRes; ++lz)
        for (uint32 lx = 0; lx < kTileRes; ++lx)
            tile[lz * kTileRes + lx] = StitchedHeight(tx * kInterior + lx, tz * kInterior + lz);
        m_Feature->UploadHeightmapRegion(handle, tile.data(), kTileRes, kTileRes,
                                         tx * kInterior, tz * kInterior);
    }

    std::vector<float32> readback;
    FlushAndReadHeightmap(*m_Feature, m_Device.get(), handle, unifiedW, unifiedH, readback);
    ASSERT_EQ(readback.size(), static_cast<size_t>(unifiedW) * unifiedH);

    // Every unified texel must equal the stitched height at its global position.
    for (uint32 z = 0; z < unifiedH; ++z)
    for (uint32 x = 0; x < unifiedW; ++x)
        EXPECT_FLOAT_EQ(readback[z * unifiedW + x], StitchedHeight(x, z))
            << "unified texel (" << x << "," << z << ")";
}

TEST_F(TerrainRenderFeatureTest, UnifiedTiledHeightmap_RegionRebakePatchesOneTile)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    constexpr uint32 kTileRes = 5;
    constexpr uint32 kTilesX = 2, kTilesZ = 2;
    constexpr uint32 kInterior = kTileRes - 1;
    const uint32 unifiedW = kTilesX * kInterior + 1;
    const uint32 unifiedH = kTilesZ * kInterior + 1;
    const TerrainHandle handle{0, 1};

    m_Feature->EnsureUnifiedTiledTextures(handle, unifiedW, unifiedH);
    for (uint32 tz = 0; tz < kTilesZ; ++tz)
    for (uint32 tx = 0; tx < kTilesX; ++tx)
    {
        std::vector<float32> tile(kTileRes * kTileRes);
        for (uint32 lz = 0; lz < kTileRes; ++lz)
        for (uint32 lx = 0; lx < kTileRes; ++lx)
            tile[lz * kTileRes + lx] = StitchedHeight(tx * kInterior + lx, tz * kInterior + lz);
        m_Feature->UploadHeightmapRegion(handle, tile.data(), kTileRes, kTileRes,
                                         tx * kInterior, tz * kInterior);
    }
    std::vector<float32> baseline;
    FlushAndReadHeightmap(*m_Feature, m_Device.get(), handle, unifiedW, unifiedH, baseline);

    // Region re-bake: bump only tile (1,1) by a constant and re-upload just it.
    constexpr float32 kBump = 100.0f;
    const uint32 rbx = 1, rbz = 1;
    std::vector<float32> edited(kTileRes * kTileRes);
    for (uint32 lz = 0; lz < kTileRes; ++lz)
    for (uint32 lx = 0; lx < kTileRes; ++lx)
        edited[lz * kTileRes + lx] =
            StitchedHeight(rbx * kInterior + lx, rbz * kInterior + lz) + kBump;
    m_Feature->UploadHeightmapRegion(handle, edited.data(), kTileRes, kTileRes,
                                     rbx * kInterior, rbz * kInterior);

    std::vector<float32> after;
    FlushAndReadHeightmap(*m_Feature, m_Device.get(), handle, unifiedW, unifiedH, after);
    ASSERT_EQ(after.size(), baseline.size());

    const uint32 dstX0 = rbx * kInterior, dstY0 = rbz * kInterior;
    for (uint32 z = 0; z < unifiedH; ++z)
    for (uint32 x = 0; x < unifiedW; ++x)
    {
        const bool inRebake = x >= dstX0 && x < dstX0 + kTileRes &&
                              z >= dstY0 && z < dstY0 + kTileRes;
        const float32 expected = inRebake ? baseline[z * unifiedW + x] + kBump
                                          : baseline[z * unifiedW + x];
        EXPECT_FLOAT_EQ(after[z * unifiedW + x], expected)
            << "texel (" << x << "," << z << ") inRebake=" << inRebake;
    }
}

TEST_F(TerrainRenderFeatureTest, UnifiedTiledTextures_ResizeReseedsSource)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    const TerrainHandle handle{0, 1};

    // Seed at a small size, then resize larger (simulates a runtime tiling reconfig).
    m_Feature->EnsureUnifiedTiledTextures(handle, 5, 5);
    const TextureHandle small = m_Feature->GetHeightmapTexture(handle);
    m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9);
    const TextureHandle large = m_Feature->GetHeightmapTexture(handle);
    EXPECT_NE(small, large); // texture recreated at the new size

    // Patch the resized source and verify a readback reflects the new content.
    constexpr uint32 kTileRes = 5, kInterior = 4;
    for (uint32 tz = 0; tz < 2; ++tz)
    for (uint32 tx = 0; tx < 2; ++tx)
    {
        std::vector<float32> tile(kTileRes * kTileRes);
        for (uint32 lz = 0; lz < kTileRes; ++lz)
        for (uint32 lx = 0; lx < kTileRes; ++lx)
            tile[lz * kTileRes + lx] = StitchedHeight(tx * kInterior + lx, tz * kInterior + lz);
        m_Feature->UploadHeightmapRegion(handle, tile.data(), kTileRes, kTileRes,
                                         tx * kInterior, tz * kInterior);
    }

    std::vector<float32> readback;
    FlushAndReadHeightmap(*m_Feature, m_Device.get(), handle, 9, 9, readback);
    for (uint32 z = 0; z < 9; ++z)
    for (uint32 x = 0; x < 9; ++x)
        EXPECT_FLOAT_EQ(readback[z * 9 + x], StitchedHeight(x, z));
}

// In-place re-provision safety: ReleaseTerrainResources raises the retire-generation signal the CBT
// renderer polls to purge the retired heightmap from its own descriptor ring (a path bindless-slot
// invalidation does not cover). Every retire (a SamplesPerMeter/size edit destroys and recreates the
// unified textures) must advance the generation so no bump is missed; a call that retires nothing
// still counts as a retire event (the terrain may be mid-stream), so it advances too.
TEST_F(TerrainRenderFeatureTest, ReleaseTerrainResources_AdvancesRetireGeneration)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    const TerrainHandle handle{0, 1};
    m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9);
    ASSERT_TRUE(m_Feature->GetHeightmapTexture(handle).IsValid());

    const uint64 gen0 = m_Feature->GetTerrainTextureRetireGeneration();
    m_Feature->ReleaseTerrainResources(handle);
    const uint64 gen1 = m_Feature->GetTerrainTextureRetireGeneration();
    EXPECT_GT(gen1, gen0) << "retiring the unified set must raise the CBT purge signal";

    // A second re-provision (retire again) advances it once more — the CBT poll re-fires per event.
    m_Feature->EnsureUnifiedTiledTextures(TerrainHandle{1, 1}, 9, 9);
    m_Feature->ReleaseTerrainResources(TerrainHandle{1, 1});
    EXPECT_GT(m_Feature->GetTerrainTextureRetireGeneration(), gen1);
}

TEST_F(TerrainRenderFeatureTest, EnsureUnifiedTiledTextures_IdempotentNoReupload)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    const TerrainHandle handle{0, 1};
    m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9);
    EXPECT_TRUE(m_Feature->HasPendingUploads()); // first creation queues the zero-fill

    // Drain the pending zero-fill.
    auto cl = m_Device->CreateCommandList(IDevice::QueueType::Graphics);
    cl->Begin();
    m_Feature->FlushPendingUploads(cl.get());
    cl->End();
    m_Device->ExecuteCommandLists({cl.get()});
    m_Device->FinalizeFrame();
    m_Device->WaitForIdle();
    EXPECT_FALSE(m_Feature->HasPendingUploads());

    // Re-ensuring the same size must not recreate or re-upload (idle quiescence).
    m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9);
    EXPECT_FALSE(m_Feature->HasPendingUploads());
}

// ---- E6: re-provision / recreate unification oracles ----
//
// A tiled terrain rebuilds two ways: a from-scratch CREATE (fresh handle, the unified set
// allocated immediately, so tiles upload as they integrate) and an in-place RE-PROVISION (a
// SamplesPerMeter/size edit that retires the old set and, under the VRAM staging barrier,
// withholds the new one for several frames). The extraction system reacts to
// EnsureUnifiedTiledTextures reporting "(re)created this frame" by re-marking every resident
// tile for a full re-upload, so a tile whose upload was dropped while the set was withheld is
// refilled the frame the set allocates. Before the fix the re-provision path cleared those
// tiles without uploading them, freezing their patch at the fresh texture's zero-clear (the
// "stuck tile" artifact; disable/enable healed it only because a from-scratch create allocates
// the set immediately). These oracles pin the recreate report and prove the two paths converge
// on byte-identical unified content.

// The recreate report is the signal the extraction re-seed hangs on: true on the frame a
// texture is allocated (fresh or resized), false when the existing texture is reused.
TEST_F(TerrainRenderFeatureTest, EnsureUnifiedTiledTextures_ReportsRecreatedOnAllocateNotReuse)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    const TerrainHandle handle{0, 1};
    EXPECT_TRUE(m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9))
        << "first allocation must report recreated";
    EXPECT_FALSE(m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9))
        << "reusing the same-size set must not report recreated";
    EXPECT_TRUE(m_Feature->EnsureUnifiedTiledTextures(handle, 17, 17))
        << "a resize (SamplesPerMeter edit) recreates the set and must report it";
    EXPECT_FALSE(m_Feature->EnsureUnifiedTiledTextures(handle, 17, 17))
        << "reusing the resized set must not report recreated";
}

// The re-provision VRAM staging barrier withholds the new set after ReleaseTerrainResources
// retires the old one: EnsureUnifiedTiledTextures reports NOT recreated and leaves the texture
// unallocated until the drain passes. Because the report is false, the extraction skips the
// re-mark this frame and keeps every resident tile dirty across the withhold — the tiles cannot
// be lost. (Advancing past the drain needs the per-frame FlushDeferredDestroys tick, which the
// device-only harness cannot drive; the withhold->recreate->refill transition is covered by the
// editor runtime verification.)
TEST_F(TerrainRenderFeatureTest, EnsureUnifiedTiledTextures_WithheldWhileStagingBarrierPending)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    const TerrainHandle handle{0, 1};
    ASSERT_TRUE(m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9));
    ASSERT_TRUE(m_Feature->GetHeightmapTexture(handle).IsValid());

    // Retire the set — arms the staging barrier (withhold the new allocation for kMaxFrames).
    m_Feature->ReleaseTerrainResources(handle);
    EXPECT_FALSE(m_Feature->GetHeightmapTexture(handle).IsValid())
        << "retire clears the unified heightmap";

    // While withheld, the (re)allocation is deferred: no recreate report, no texture allocated.
    EXPECT_FALSE(m_Feature->EnsureUnifiedTiledTextures(handle, 9, 9))
        << "the staging barrier must withhold the new set (no recreate report)";
    EXPECT_FALSE(m_Feature->GetHeightmapTexture(handle).IsValid())
        << "no texture may be allocated while the barrier is pending";
}

// Recreate-vs-re-provision equivalence: the unified heightmap the RE-PROVISION path produces
// (allocate the old grid, then recreate at the new size and re-upload every tile — what the
// extraction does on the recreate report) is byte-identical to the one a from-scratch CREATE
// produces (allocate at the final size, upload every tile). Whatever partial/stale content the
// old grid held is discarded by the recreate; the full re-upload rebuilds the canonical source.
TEST_F(TerrainRenderFeatureTest, ReprovisionRecreate_UnifiedContentEquivalent)
{
    constexpr uint32 kTileRes = 5, kInterior = 4;
    constexpr uint32 kTilesX = 2, kTilesZ = 2;
    const uint32 unifiedW = kTilesX * kInterior + 1; // 9
    const uint32 unifiedH = kTilesZ * kInterior + 1; // 9
    const TerrainHandle handle{0, 1};

    auto uploadAllTiles = [&](TerrainRenderFeature& feat)
    {
        for (uint32 tz = 0; tz < kTilesZ; ++tz)
        for (uint32 tx = 0; tx < kTilesX; ++tx)
        {
            std::vector<float32> tile(kTileRes * kTileRes);
            for (uint32 lz = 0; lz < kTileRes; ++lz)
            for (uint32 lx = 0; lx < kTileRes; ++lx)
                tile[lz * kTileRes + lx] = StitchedHeight(tx * kInterior + lx, tz * kInterior + lz);
            feat.UploadHeightmapRegion(handle, tile.data(), kTileRes, kTileRes,
                                       tx * kInterior, tz * kInterior);
        }
    };

    // CREATE: allocate at the final size, upload every tile.
    std::vector<float32> createReadback;
    {
        TerrainRenderFeature feat;
        ASSERT_TRUE(feat.Initialize(m_Device.get()));
        ASSERT_TRUE(feat.EnsureUnifiedTiledTextures(handle, unifiedW, unifiedH));
        uploadAllTiles(feat);
        FlushAndReadHeightmap(feat, m_Device.get(), handle, unifiedW, unifiedH, createReadback);
    }

    // RE-PROVISION: allocate the old (smaller) grid holding pre-edit content, then recreate at
    // the new size (the SamplesPerMeter edit) and re-upload every tile on the recreate report.
    std::vector<float32> reprovReadback;
    {
        TerrainRenderFeature feat;
        ASSERT_TRUE(feat.Initialize(m_Device.get()));
        ASSERT_TRUE(feat.EnsureUnifiedTiledTextures(handle, 5, 5)); // old grid, recreated
        std::vector<float32> stale(5 * 5, -999.0f);                 // pre-edit content
        feat.UploadHeightmapRegion(handle, stale.data(), 5, 5, 0, 0);
        ASSERT_TRUE(feat.EnsureUnifiedTiledTextures(handle, unifiedW, unifiedH))
            << "the re-provision resize must report recreated so the extraction re-uploads";
        uploadAllTiles(feat); // the extraction's re-mark -> full re-upload
        FlushAndReadHeightmap(feat, m_Device.get(), handle, unifiedW, unifiedH, reprovReadback);
    }

    ASSERT_EQ(createReadback.size(), reprovReadback.size());
    for (size_t i = 0; i < createReadback.size(); ++i)
        EXPECT_FLOAT_EQ(reprovReadback[i], createReadback[i])
            << "unified texel " << i << " diverged between recreate and re-provision";
}

// The per-terrain params SSBO is a ring deeper than the device's pacing, so the write
// to an element lands at least one RETIRED frame after the frame that last read it.
// That reuse distance is only real if the rotation reaches every element.
// IDevice::GetFrameIndex() reports a slot in [0, framesInFlight), so reducing its VALUE
// caps the rotation at the device's pacing and leaves the last element allocated but
// never written. Drive the upload with the domain the device really produces — exactly
// what TerrainExtractionSystem passes — and observe the depth as the number of frames
// the published element takes to come back round.
TEST_F(TerrainRenderFeatureTest, TerrainParamsRingRotatesThroughEveryElement)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    const uint32 pacing = m_Device->GetFramesInFlight();
    ASSERT_GT(pacing, 1u) << "a paced device is the premise of this test";

    TerrainGPUParams params{};
    params.WorldOriginX = 10.0f;
    params.WorldSizeX = 1024.0f;
    params.HeightScale = 128.0f;

    // Far past any ring the feature could size: a rotation that has not come back by
    // then never will.
    constexpr uint32 kRotationFrameLimit = 64;
    std::vector<uint32> seen;
    bool cameBackRound = false;
    for (uint32 f = 0; f < kRotationFrameLimit && !cameBackRound; ++f)
    {
        m_Feature->UploadTerrainParamsArray(&params, 1, Terrain::kDefaultTerrainMaterials,
                                           Terrain::kTerrainLayerRoleCount, f % pacing);
        const uint32 slot = m_Feature->GetLastTerrainParamsSlot();
        cameBackRound = !seen.empty() && slot == seen.front();
        if (!cameBackRound)
            seen.push_back(slot);
    }
    ASSERT_TRUE(cameBackRound) << "the counter must cycle, not run off past the ring";

    const uint32 ringDepth = static_cast<uint32>(seen.size());
    ASSERT_LT(pacing, ringDepth)
        << "the rotation came back after " << ringDepth << " frames: the ring must stay "
           "deeper than the device's pacing (" << pacing << ")";

    for (uint32 a = 0; a < ringDepth; ++a)
    {
        EXPECT_TRUE(m_Feature->GetTerrainParamsSSBO(seen[a]).IsValid())
            << "element " << seen[a] << " was published but never allocated";
        for (uint32 b = a + 1; b < ringDepth; ++b)
            EXPECT_NE(seen[a], seen[b])
                << "frames " << a << " and " << b << " wrote the same params element: the "
                   "rotation collapsed onto the device's pacing (" << pacing << "), so the "
                   "element beyond it is never written and the reuse distance is the pacing, "
                   "not the ring depth";
    }
}

// Two entry points can observe the same device frame (a second world/registry extraction
// in one frame). A repeated token must hold the element: rotating inside one frame would
// spend the reuse distance the ring depth buys.
TEST_F(TerrainRenderFeatureTest, TerrainParamsRepeatedDeviceIndexHoldsTheElement)
{
    m_Feature = std::make_unique<TerrainRenderFeature>();
    ASSERT_TRUE(m_Feature->Initialize(m_Device.get()));

    TerrainGPUParams params{};
    params.WorldSizeX = 512.0f;

    m_Feature->UploadTerrainParamsArray(&params, 1, Terrain::kDefaultTerrainMaterials,
                                       Terrain::kTerrainLayerRoleCount, 0u);
    m_Feature->UploadTerrainParamsArray(&params, 1, Terrain::kDefaultTerrainMaterials,
                                       Terrain::kTerrainLayerRoleCount, 1u);
    const uint32 first = m_Feature->GetLastTerrainParamsSlot();
    m_Feature->UploadTerrainParamsArray(&params, 1, Terrain::kDefaultTerrainMaterials,
                                       Terrain::kTerrainLayerRoleCount, 1u);

    EXPECT_EQ(m_Feature->GetLastTerrainParamsSlot(), first)
        << "a repeated device index is the same frame, not the next one";
}

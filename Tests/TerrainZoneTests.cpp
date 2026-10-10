#include <gtest/gtest.h>

#include "AssetCore/GUID.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Terrain/Heightfield.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZoneAuthoring.h"
#include "TerrainECS/TerrainZonePayload.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace
{
using namespace GameEngine;
using namespace GameEngine::TerrainECS;

struct ScopedTerrainService
{
    ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
        TerrainService::Initialize();
    }
    ~ScopedTerrainService()
    {
        if (TerrainService::IsInitialized())
            TerrainService::Shutdown();
    }
};

constexpr float32 kWorldSize = 256.0f;
constexpr float32 kHeightScale = 64.0f;
constexpr uint32 kHeightmapDim = 129;
constexpr float32 kSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1); // 2.0
constexpr float32 kOrigin = -kWorldSize * 0.5f;                                    // -128

Terrain::TerrainConfig MakeTestConfig()
{
    Terrain::TerrainConfig cfg{};
    cfg.HeightmapWidth = kHeightmapDim;
    cfg.HeightmapHeight = kHeightmapDim;
    cfg.WorldSizeX = kWorldSize;
    cfg.WorldSizeZ = kWorldSize;
    cfg.HeightScale = kHeightScale;
    cfg.LODLevels = 4;
    return cfg;
}

// Flat-base terrain so a sculpt zone's Add offset lands directly in the sample.
TerrainHandle CreateFlatTerrain(TerrainService& svc)
{
    const TerrainHandle handle = svc.CreateTerrain(MakeTestConfig());
    auto* data = svc.GetTerrainData(handle);
    FillHeightfieldBaseRegion(data->Heightfield, Components::TerrainBaseSource::Flat, nullptr,
                              0, 0, static_cast<int32>(kHeightmapDim) - 1,
                              static_cast<int32>(kHeightmapDim) - 1);
    data->MarkFullDirty();
    svc.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();
    return handle;
}

void EnableZoneLifecycle(ECS::World& world)
{
    world.EnableLifecycleEvents<Components::TerrainSculptZone>();
    world.EnableLifecycleEvents<Components::TerrainPaintZone>();
}

ECS::EntityHandle CreateFlatTerrainEntity(ECS::World& world, TerrainHandle handle,
                                          Components::TerrainBaseSource baseSource)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = baseSource;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, Components::WorldTransform{});
    return e;
}

// Column-major Y-rotation * per-axis scale, translation in XZ. Built so the
// modifier gather extracts yaw = yawRad (atan2(m[8],m[10])) and XZ scale =
// (scaleX, scaleZ) (column basis lengths).
Components::WorldTransform MakeYRotScaleXf(float32 x, float32 z, float32 yawRad,
                                           float32 scaleX, float32 scaleZ)
{
    Components::WorldTransform xf{};
    const float32 c = std::cos(yawRad);
    const float32 s = std::sin(yawRad);
    xf.matrix[0] = c * scaleX;  xf.matrix[1] = 0.0f; xf.matrix[2] = -s * scaleX;
    xf.matrix[4] = 0.0f;        xf.matrix[5] = 1.0f; xf.matrix[6] = 0.0f;
    xf.matrix[8] = s * scaleZ;  xf.matrix[9] = 0.0f; xf.matrix[10] = c * scaleZ;
    xf.matrix[12] = x;          xf.matrix[13] = 0.0f; xf.matrix[14] = z;
    xf.matrix[15] = 1.0f;
    return xf;
}

// Payload whose R32F offset ramps linearly with U: offset = scale * (x/(dim-1)).
// Bilinear sampling of a linear field is exact, so the baked offset is a closed
// form of the zone-local U at each terrain sample.
TerrainZonePayload MakeRampPayload(uint32 dim, float32 scale)
{
    TerrainZonePayload p;
    p.Allocate(ZonePayloadFormat::SculptOffsetR32F, dim, dim);
    for (uint32 zz = 0; zz < dim; ++zz)
        for (uint32 xx = 0; xx < dim; ++xx)
            p.Offsets[static_cast<size_t>(zz) * dim + xx] =
                scale * (static_cast<float32>(xx) / static_cast<float32>(dim - 1));
    return p;
}

ECS::EntityHandle CreateSculptZone(ECS::World& world, const GUID& payload,
                                   float32 extentX, float32 extentZ,
                                   const Components::WorldTransform& xf,
                                   Components::TerrainModifierBlend blend =
                                       Components::TerrainModifierBlend::Add)
{
    auto e = world.CreateEntity();
    Components::TerrainSculptZone z{};
    z.ExtentX = extentX;
    z.ExtentZ = extentZ;
    z.Falloff = 0.0f;
    z.Priority = 1000.0f;
    z.Blend = blend;
    z.PayloadRef.Set(payload);
    world.AddComponentImmediate<Components::TerrainSculptZone>(e, z);
    world.AddComponentImmediate<Components::WorldTransform>(e, xf);
    return e;
}

int32 SampleForWorldX(float32 worldX) { return static_cast<int32>(std::lround((worldX - kOrigin) / kSpacing)); }

float32 SampleAtWorld(const Terrain::HeightfieldData& hf, float32 worldX, float32 worldZ)
{
    return hf.GetSample(static_cast<uint32>(SampleForWorldX(worldX)),
                        static_cast<uint32>(SampleForWorldX(worldZ)));
}
} // namespace

// ---------------------------------------------------------------------------
// Bake-quiescence guard (distinct from the command's structural correctness,
// which TerrainZoneStrokeCommandTests covers): a whole-payload restore via
// SetZonePayload — the store-side effect of a stroke undo — must re-bake ONCE
// and then settle. A non-settling gate here would busy-loop the editor tick.
// ---------------------------------------------------------------------------
TEST(TerrainZoneBake, StrokeUndoQuiescesAfterRebake)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const TerrainHandle handle = CreateFlatTerrain(svc);
    auto* data = svc.GetTerrainData(handle);

    const GUID payload = GUID::Generate();
    svc.SeedZonePayloadForTests(payload, MakeRampPayload(33, 32.0f)); // small dims
    svc.NotifyZonePayloadEdited(payload, 0, 0, 33, 33);

    ECS::World world;
    EnableZoneLifecycle(world);
    CreateFlatTerrainEntity(world, handle, Components::TerrainBaseSource::Flat);
    auto zone = CreateSculptZone(world, payload, 16.0f, 16.0f,
                                 MakeYRotScaleXf(0.0f, 0.0f, 0.0f, 1.0f, 1.0f));

    TerrainModifierSystem system;
    // Settle the initial stroke: bake, then confirm idle frames do not re-bake.
    system.Update(world, 1.0f / 60.0f);
    const uint64 vSettled = data->HeightfieldVersion;
    for (int i = 0; i < 5; ++i)
        system.Update(world, 1.0f / 60.0f);
    ASSERT_EQ(data->HeightfieldVersion, vSettled) << "stroke did not settle before undo";

    // Simulate an auto-grown stroke: larger payload dims + a wider component
    // extent, then a dab.
    svc.SetZonePayload(payload, MakeRampPayload(65, 40.0f)); // grown dims
    {
        auto* z = world.GetComponentForWrite<Components::TerrainSculptZone>(zone);
        z->ExtentX = 32.0f;
        z->ExtentZ = 32.0f;
    }
    world.SwapLifecycleEvents();
    svc.NotifyZonePayloadEdited(payload, 10, 10, 20, 20);
    system.Update(world, 1.0f / 60.0f);

    // Undo the stroke exactly as ZonePayloadStrokeCommand does: restore the
    // pre-stroke payload (smaller dims) WITHOUT reverting the grown component
    // extent (a non-compound stroke leaves the component change in place).
    svc.SetZonePayload(payload, MakeRampPayload(33, 32.0f));

    // The undo must bake at most a bounded number of times, then quiesce. A
    // non-settling gate would keep incrementing HeightfieldVersion forever.
    uint64 prev = data->HeightfieldVersion;
    int bakesAfterUndo = 0;
    for (int i = 0; i < 50; ++i)
    {
        system.Update(world, 1.0f / 60.0f);
        if (data->HeightfieldVersion != prev)
        {
            ++bakesAfterUndo;
            prev = data->HeightfieldVersion;
        }
    }
    // One re-bake for the restore is expected; anything beyond a tiny bound is
    // the runaway loop.
    EXPECT_LE(bakesAfterUndo, 2) << "undo did not quiesce — bake kept re-triggering";

    // And it is genuinely idle now.
    const uint64 vNow = data->HeightfieldVersion;
    for (int i = 0; i < 5; ++i)
        system.Update(world, 1.0f / 60.0f);
    EXPECT_EQ(data->HeightfieldVersion, vNow);
}

// Bake-quiescence guard: removing a zone entity + evicting its payload — the
// store/world effect of an auto-created stroke's undo — must let the modifier
// bake settle, not keep re-triggering the gate every frame.
TEST(TerrainZoneBake, CompoundUndoRemovesZoneAndQuiesces)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const TerrainHandle handle = CreateFlatTerrain(svc);
    auto* data = svc.GetTerrainData(handle);

    ECS::World world;
    EnableZoneLifecycle(world);
    CreateFlatTerrainEntity(world, handle, Components::TerrainBaseSource::Flat);

    // Zone A already exists; snapshot the world without zone B.
    const GUID payloadA = GUID::Generate();
    svc.SeedZonePayloadForTests(payloadA, MakeRampPayload(33, 20.0f));
    CreateSculptZone(world, payloadA, 16.0f, 16.0f, MakeYRotScaleXf(-40.0f, 0.0f, 0.0f, 1.0f, 1.0f));
    world.SwapLifecycleEvents();

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    const std::vector<uint8> worldBeforeB = world.SerializeWorld();

    // "Auto-create stroke": zone B appears + its payload is authored.
    const GUID payloadB = GUID::Generate();
    svc.SeedZonePayloadForTests(payloadB, MakeRampPayload(33, 20.0f));
    svc.NotifyZonePayloadEdited(payloadB, 0, 0, 33, 33);
    CreateSculptZone(world, payloadB, 16.0f, 16.0f, MakeYRotScaleXf(40.0f, 0.0f, 0.0f, 1.0f, 1.0f));
    world.SwapLifecycleEvents();
    system.Update(world, 1.0f / 60.0f);

    // Compound undo: restore the world without zone B + evict its payload.
    world.DeserializeWorld(worldBeforeB);
    EnableZoneLifecycle(world); // DeserializeWorld resets the world
    world.SwapLifecycleEvents();
    svc.EvictZonePayload(payloadB);

    // Bounded: the undo must re-bake a small number of times then quiesce.
    uint64 prev = data->HeightfieldVersion;
    int bakes = 0;
    for (int i = 0; i < 50; ++i)
    {
        system.Update(world, 1.0f / 60.0f);
        world.SwapLifecycleEvents();
        if (data->HeightfieldVersion != prev)
        {
            ++bakes;
            prev = data->HeightfieldVersion;
        }
    }
    EXPECT_LE(bakes, 3) << "compound undo did not quiesce — bake kept re-triggering";
}

// ---------------------------------------------------------------------------
// Payload codec round-trip.
// ---------------------------------------------------------------------------
TEST(TerrainZonePayload, CodecRoundTripsSculptAndPaint)
{
    TerrainZonePayload sculpt = MakeRampPayload(17, 3.5f);
    sculpt.Offsets[3] = -12.25f; // include a negative offset
    const std::vector<uint8> sBlob = EncodeZonePayload(sculpt);
    TerrainZonePayload sOut;
    ASSERT_TRUE(DecodeZonePayload(sBlob.data(), sBlob.size(), sOut));
    EXPECT_EQ(sOut.Format, ZonePayloadFormat::SculptOffsetR32F);
    EXPECT_EQ(sOut.Width, 17u);
    EXPECT_EQ(sOut.Height, 17u);
    ASSERT_EQ(sOut.Offsets.size(), sculpt.Offsets.size());
    EXPECT_EQ(0, std::memcmp(sOut.Offsets.data(), sculpt.Offsets.data(),
                             sculpt.Offsets.size() * sizeof(float32)));

    TerrainZonePayload paint;
    paint.Allocate(ZonePayloadFormat::PaintMaskR8, 9, 5);
    for (size_t i = 0; i < paint.Mask.size(); ++i)
        paint.Mask[i] = static_cast<uint8>(i * 7u);
    const std::vector<uint8> pBlob = EncodeZonePayload(paint);
    TerrainZonePayload pOut;
    ASSERT_TRUE(DecodeZonePayload(pBlob.data(), pBlob.size(), pOut));
    EXPECT_EQ(pOut.Format, ZonePayloadFormat::PaintMaskR8);
    EXPECT_EQ(pOut.Width, 9u);
    EXPECT_EQ(pOut.Height, 5u);
    EXPECT_EQ(pOut.Mask, paint.Mask);
}

TEST(TerrainZonePayload, DecodeRejectsGarbage)
{
    const uint8 garbage[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    TerrainZonePayload out;
    EXPECT_FALSE(DecodeZonePayload(garbage, sizeof(garbage), out));
    EXPECT_FALSE(DecodeZonePayload(nullptr, 0, out));

    // Correct header but truncated data must be rejected.
    TerrainZonePayload full = MakeRampPayload(8, 1.0f);
    std::vector<uint8> blob = EncodeZonePayload(full);
    blob.resize(blob.size() - 4);
    EXPECT_FALSE(DecodeZonePayload(blob.data(), blob.size(), out));
}

// ---------------------------------------------------------------------------
// Zone-local UV sampling: identity, rotation, scale — hand-computed oracles.
// ---------------------------------------------------------------------------
TEST(TerrainZoneBake, SamplesPayloadInZoneLocalUV_Identity)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const TerrainHandle handle = CreateFlatTerrain(svc);
    auto* data = svc.GetTerrainData(handle);

    constexpr float32 kExtent = 32.0f;
    constexpr float32 kRampScale = 64.0f; // offset(u) = 64*u world units
    const GUID payload = GUID::Generate();
    svc.SeedZonePayloadForTests(payload, MakeRampPayload(33, kRampScale));

    ECS::World world;
    EnableZoneLifecycle(world);
    CreateFlatTerrainEntity(world, handle, Components::TerrainBaseSource::Flat);
    CreateSculptZone(world, payload, kExtent, kExtent,
                     MakeYRotScaleXf(0.0f, 0.0f, 0.0f, 1.0f, 1.0f));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // Center: u=0.5 -> offset 32 -> 32/64 = 0.5.
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 0.0f, 0.0f), 0.5f, 1e-3f);
    // +16 in local X: u=0.75 -> offset 48 -> 0.75.
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 16.0f, 0.0f), 0.75f, 1e-3f);
    // -16 in local X: u=0.25 -> offset 16 -> 0.25.
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, -16.0f, 0.0f), 0.25f, 1e-3f);
    // Outside the rect: no effect (base is flat 0).
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 60.0f, 0.0f), 0.0f, 1e-3f);
}

TEST(TerrainZoneBake, SamplesPayloadInZoneLocalUV_Rotated90)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const TerrainHandle handle = CreateFlatTerrain(svc);
    auto* data = svc.GetTerrainData(handle);

    constexpr float32 kExtent = 32.0f;
    constexpr float32 kRampScale = 64.0f;
    const GUID payload = GUID::Generate();
    svc.SeedZonePayloadForTests(payload, MakeRampPayload(33, kRampScale));

    ECS::World world;
    EnableZoneLifecycle(world);
    CreateFlatTerrainEntity(world, handle, Components::TerrainBaseSource::Flat);
    // Yaw = +90 deg: the payload's local +X (increasing U) maps to world +Z.
    CreateSculptZone(world, payload, kExtent, kExtent,
                     MakeYRotScaleXf(0.0f, 0.0f, 1.57079632679f, 1.0f, 1.0f));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // Local +X now runs along world +Z: world (0,+16) -> u=0.75 -> 0.75.
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 0.0f, 16.0f), 0.75f, 1e-3f);
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 0.0f, -16.0f), 0.25f, 1e-3f);
    // World +X now runs along local ±Z (constant U=0.5) -> center offset 0.5.
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 16.0f, 0.0f), 0.5f, 1e-3f);
}

TEST(TerrainZoneBake, SamplesPayloadInZoneLocalUV_Scaled2x)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const TerrainHandle handle = CreateFlatTerrain(svc);
    auto* data = svc.GetTerrainData(handle);

    constexpr float32 kExtent = 32.0f;
    constexpr float32 kRampScale = 64.0f;
    const GUID payload = GUID::Generate();
    svc.SeedZonePayloadForTests(payload, MakeRampPayload(33, kRampScale));

    ECS::World world;
    EnableZoneLifecycle(world);
    CreateFlatTerrainEntity(world, handle, Components::TerrainBaseSource::Flat);
    // XZ scale 2x: the effective world half-extent is 64, so world +32 sits at
    // u=0.75 (it would have been the rect edge at scale 1).
    CreateSculptZone(world, payload, kExtent, kExtent,
                     MakeYRotScaleXf(0.0f, 0.0f, 0.0f, 2.0f, 2.0f));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 32.0f, 0.0f), 0.75f, 1e-3f);
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, -32.0f, 0.0f), 0.25f, 1e-3f);
    // Beyond the scaled rect (world +80 > 64) there is no effect.
    EXPECT_NEAR(SampleAtWorld(data->Heightfield, 80.0f, 0.0f), 0.0f, 1e-3f);
}

// ---------------------------------------------------------------------------
// Payload-dirty-rect: a brush stroke re-bakes only the dab's footprint, and the
// region re-bake is bit-identical to a full bake of the final payload (§3.2).
// ---------------------------------------------------------------------------
TEST(TerrainZoneBake, PayloadStrokeDirtiesSubRectNotWholeZone)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();

    // Terrain A: initial bake, then a small payload edit -> region re-bake.
    const TerrainHandle handleA = CreateFlatTerrain(svc);
    auto* dataA = svc.GetTerrainData(handleA);

    constexpr float32 kExtent = 60.0f; // large zone: full-zone re-bake would be big
    const GUID payload = GUID::Generate();
    svc.SeedZonePayloadForTests(payload, MakeRampPayload(121, 32.0f));

    ECS::World worldA;
    EnableZoneLifecycle(worldA);
    CreateFlatTerrainEntity(worldA, handleA, Components::TerrainBaseSource::Flat);
    CreateSculptZone(worldA, payload, kExtent, kExtent,
                     MakeYRotScaleXf(0.0f, 0.0f, 0.0f, 1.0f, 1.0f));

    TerrainModifierSystem systemA;
    systemA.Update(worldA, 1.0f / 60.0f); // full initial bake
    const uint64 cursor = dataA->HeightfieldVersion;

    // Stroke: bump a small interior texel block near the payload center.
    TerrainZonePayload* p = svc.GetZonePayload(payload);
    ASSERT_NE(p, nullptr);
    const int32 dabMin = 58, dabMax = 63; // texels (of 121) — a tiny footprint
    for (int32 tz = dabMin; tz < dabMax; ++tz)
        for (int32 tx = dabMin; tx < dabMax; ++tx)
            p->Offsets[static_cast<size_t>(tz) * p->Width + tx] += 20.0f;
    svc.NotifyZonePayloadEdited(payload, dabMin, dabMin, dabMax, dabMax);

    systemA.Update(worldA, 1.0f / 60.0f); // region re-bake of the dab

    DirtyRegionLog::Region region;
    ASSERT_TRUE(dataA->HeightfieldDirtyLog.CollectSince(cursor, region));
    // The dirty rect must be a small sub-rect, NOT the whole zone footprint
    // (~60 texels wide at 2 samples/m). The dab spans ~2.5 m, so << 20 samples.
    EXPECT_LT(region.MaxX - region.MinX, 20);
    EXPECT_LT(region.MaxZ - region.MinZ, 20);
    EXPECT_GT(region.MaxX, region.MinX);

    // Terrain B: fresh full bake of the FINAL payload state.
    const TerrainHandle handleB = CreateFlatTerrain(svc);
    auto* dataB = svc.GetTerrainData(handleB);
    ECS::World worldB;
    EnableZoneLifecycle(worldB);
    CreateFlatTerrainEntity(worldB, handleB, Components::TerrainBaseSource::Flat);
    CreateSculptZone(worldB, payload, kExtent, kExtent,
                     MakeYRotScaleXf(0.0f, 0.0f, 0.0f, 1.0f, 1.0f));
    TerrainModifierSystem systemB;
    systemB.Update(worldB, 1.0f / 60.0f);

    // Region re-bake must be bit-identical to the full bake everywhere.
    ASSERT_EQ(dataA->Heightfield.GetSampleCount(), dataB->Heightfield.GetSampleCount());
    EXPECT_EQ(0, std::memcmp(dataA->Heightfield.GetRawSamples(),
                             dataB->Heightfield.GetRawSamples(),
                             dataA->Heightfield.GetSampleCount() * sizeof(float32)));
}

// A moved zone dirties old ∪ new bounds (geometry change path), unchanged from
// the existing modifier machinery — proves zones relocate non-destructively.
TEST(TerrainZoneBake, MovingZoneDirtiesOldAndNewBounds)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const TerrainHandle handle = CreateFlatTerrain(svc);
    auto* data = svc.GetTerrainData(handle);

    const GUID payload = GUID::Generate();
    svc.SeedZonePayloadForTests(payload, MakeRampPayload(33, 32.0f));

    ECS::World world;
    EnableZoneLifecycle(world);
    CreateFlatTerrainEntity(world, handle, Components::TerrainBaseSource::Flat);
    auto zone = CreateSculptZone(world, payload, 16.0f, 16.0f,
                                 MakeYRotScaleXf(-40.0f, 0.0f, 0.0f, 1.0f, 1.0f));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    const uint64 cursor = data->HeightfieldVersion;

    // Move the zone from x=-40 to x=+40.
    auto* xf = world.GetComponentForWrite<Components::WorldTransform>(zone);
    ASSERT_NE(xf, nullptr);
    *xf = MakeYRotScaleXf(40.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    system.Update(world, 1.0f / 60.0f);

    DirtyRegionLog::Region region;
    ASSERT_TRUE(data->HeightfieldDirtyLog.CollectSince(cursor, region));
    // Must span from the old footprint (world -56) to the new (world +56).
    EXPECT_LT(SampleForWorldX(-56.0f) + 2, region.MinX + 4); // rough left cover
    EXPECT_LE(region.MinX, SampleForWorldX(-40.0f));
    EXPECT_GE(region.MaxX, SampleForWorldX(40.0f));
}

// ---------------------------------------------------------------------------
// Paint zone: a mask paints its target layer weight into the splatmap.
// ---------------------------------------------------------------------------
TEST(TerrainZoneBake, PaintZoneWritesLayerWeight)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const TerrainHandle handle = CreateFlatTerrain(svc);
    auto* data = svc.GetTerrainData(handle);

    // Full-strength mask over the whole payload.
    TerrainZonePayload mask;
    mask.Allocate(ZonePayloadFormat::PaintMaskR8, 33, 33);
    std::fill(mask.Mask.begin(), mask.Mask.end(), static_cast<uint8>(255));
    const GUID payload = GUID::Generate();
    svc.SeedZonePayloadForTests(payload, std::move(mask));

    ECS::World world;
    EnableZoneLifecycle(world);
    CreateFlatTerrainEntity(world, handle, Components::TerrainBaseSource::Flat);

    auto e = world.CreateEntity();
    Components::TerrainPaintZone z{};
    z.ExtentX = 24.0f;
    z.ExtentZ = 24.0f;
    z.Falloff = 0.0f;
    z.Priority = 1000.0f;
    z.LayerIndex = 2; // dirt
    z.Strength = 1.0f;
    z.PayloadRef.Set(payload);
    world.AddComponentImmediate<Components::TerrainPaintZone>(e, z);
    world.AddComponentImmediate<Components::WorldTransform>(
        e, MakeYRotScaleXf(0.0f, 0.0f, 0.0f, 1.0f, 1.0f));

    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);

    // Center splat texel: layer 2 (dirt) gains real weight from the mask paint.
    // Full-weight paint over full-grass renormalizes to a 50/50 split (PaintLayer
    // math, mask-scaled) — the signal is that layer 2 rose from ~0 to a large
    // share inside the zone, vs staying near 0 outside it.
    const uint32 cx = static_cast<uint32>(SampleForWorldX(0.0f));
    const size_t base = (static_cast<size_t>(cx) * data->SplatmapWidth + cx) * 4;
    ASSERT_LT(base + 3, data->Splatmap.size());
    EXPECT_GT(data->Splatmap[base + 2], 100);
    // A texel outside the zone keeps its procedural weights (layer 2 near 0).
    const uint32 ox = static_cast<uint32>(SampleForWorldX(100.0f));
    const size_t obase = (static_cast<size_t>(cx) * data->SplatmapWidth + ox) * 4;
    EXPECT_LT(data->Splatmap[obase + 2], 100);
    EXPECT_GT(data->Splatmap[base + 2], data->Splatmap[obase + 2]);
}

// ---------------------------------------------------------------------------
// Auto-grow authoring math (design §3.2 auto-create / auto-grow up to a cap).
// ---------------------------------------------------------------------------
TEST(TerrainZoneAuthoring, FitClampsToCap)
{
    ZoneAuthoringSettings s;
    s.MaxExtent = 64.0f;
    s.Padding = 4.0f;
    ZoneFootprint fit = FitZoneToDab(10.0f, 20.0f, 8.0f, s);
    EXPECT_FLOAT_EQ(fit.CenterX, 10.0f);
    EXPECT_FLOAT_EQ(fit.CenterZ, 20.0f);
    EXPECT_FLOAT_EQ(fit.ExtentX, 12.0f); // 8 + 4 padding
    // A huge brush clamps to the cap.
    ZoneFootprint big = FitZoneToDab(0.0f, 0.0f, 500.0f, s);
    EXPECT_FLOAT_EQ(big.ExtentX, 64.0f);
    EXPECT_FLOAT_EQ(big.ExtentZ, 64.0f);
}

TEST(TerrainZoneAuthoring, GrowExpandsUntilCapThenFails)
{
    ZoneAuthoringSettings s;
    s.MaxExtent = 40.0f;
    s.Padding = 2.0f;
    ZoneFootprint fp = FitZoneToDab(0.0f, 0.0f, 8.0f, s); // extent 10, center 0

    // A dab within reach grows the footprint and re-centers.
    ASSERT_TRUE(GrowZoneToDab(fp, 20.0f, 0.0f, 8.0f, s));
    EXPECT_GT(fp.ExtentX, 10.0f);
    EXPECT_LE(fp.ExtentX, 40.0f);

    // A dab far outside the cap fails and leaves the footprint untouched.
    const ZoneFootprint before = fp;
    EXPECT_FALSE(GrowZoneToDab(fp, 1000.0f, 0.0f, 8.0f, s));
    EXPECT_FLOAT_EQ(fp.ExtentX, before.ExtentX);
    EXPECT_FLOAT_EQ(fp.CenterX, before.CenterX);
}

TEST(TerrainZoneAuthoring, RegrowPreservesExistingTexels)
{
    ZoneAuthoringSettings s;
    s.TexelsPerMeter = 1.0f;
    ZoneFootprint oldFp{0.0f, 0.0f, 8.0f, 8.0f};
    const uint32 oldDim = PayloadDimForExtent(8.0f, s); // 2*8*1 + 1 = 17

    TerrainZonePayload old;
    old.Allocate(ZonePayloadFormat::SculptOffsetR32F, oldDim, oldDim);
    // Distinctive center value.
    const uint32 cc = oldDim / 2;
    old.Offsets[static_cast<size_t>(cc) * oldDim + cc] = 7.0f;

    // Grow the footprint symmetrically; density is constant, so the old center
    // texel must survive at the new center.
    ZoneFootprint newFp{0.0f, 0.0f, 16.0f, 16.0f};
    const uint32 newDim = PayloadDimForExtent(16.0f, s); // 33
    TerrainZonePayload grown = RegrowPayload(old, oldFp, newFp, newDim, newDim);
    ASSERT_EQ(grown.Width, newDim);
    const uint32 nc = newDim / 2;
    EXPECT_FLOAT_EQ(grown.Offsets[static_cast<size_t>(nc) * newDim + nc], 7.0f);
    // A corner far from the old data stays zero.
    EXPECT_FLOAT_EQ(grown.Offsets[0], 0.0f);
}

// ---------------------------------------------------------------------------
// Service store: edit epoch + dirty accumulation + Play snapshot/restore.
// ---------------------------------------------------------------------------
TEST(TerrainZoneStore, EditEpochAndDirtyAccumulate)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID g = GUID::Generate();
    const uint64 epoch0 = svc.GetZonePayloadEditEpoch();

    svc.EnsureZonePayload(g, ZonePayloadFormat::SculptOffsetR32F, 16, 16);
    EXPECT_GT(svc.GetZonePayloadEditEpoch(), epoch0);

    const uint64 epoch1 = svc.GetZonePayloadEditEpoch();
    svc.NotifyZonePayloadEdited(g, 2, 2, 5, 5);
    svc.NotifyZonePayloadEdited(g, 8, 8, 11, 11);
    EXPECT_GT(svc.GetZonePayloadEditEpoch(), epoch1);

    TerrainZonePayload* p = svc.GetZonePayload(g);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->DirtyAny);
    EXPECT_EQ(p->DirtyMinX, 2);
    EXPECT_EQ(p->DirtyMaxX, 11); // union of the two dabs
    EXPECT_GE(p->DataVersion, 3u);

    svc.ClearZonePayloadDirty(g);
    EXPECT_FALSE(svc.GetZonePayload(g)->DirtyAny);
}

// SetZonePayload (undo/redo restore path): monotonic version, no dirty rect,
// epoch bump so the next bake re-applies the whole footprint.
TEST(TerrainZoneStore, SetZonePayloadRestoresWholePayloadMonotonically)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID g = GUID::Generate();
    svc.EnsureZonePayload(g, ZonePayloadFormat::SculptOffsetR32F, 8, 8);
    svc.NotifyZonePayloadEdited(g, 0, 0, 4, 4);
    const uint64 vBefore = svc.GetZonePayload(g)->DataVersion;

    TerrainZonePayload restore = MakeRampPayload(8, 5.0f);
    const uint64 epoch = svc.GetZonePayloadEditEpoch();
    svc.SetZonePayload(g, restore);

    TerrainZonePayload* p = svc.GetZonePayload(g);
    ASSERT_NE(p, nullptr);
    EXPECT_GT(p->DataVersion, vBefore);   // monotonic: the bake diff sees a change
    EXPECT_FALSE(p->DirtyAny);            // no sub-rect: whole footprint re-bakes
    EXPECT_GT(svc.GetZonePayloadEditEpoch(), epoch);
    EXPECT_FLOAT_EQ(p->Offsets[7], 5.0f); // ramp u=1 -> 5
}

TEST(TerrainZoneStore, PlaySnapshotRestoresPayload)
{
    ScopedTerrainService scoped;
    auto& svc = TerrainService::Get();
    const GUID g = GUID::Generate();
    svc.SeedZonePayloadForTests(g, MakeRampPayload(9, 4.0f));

    svc.SnapshotZonePayloadsForPlay();
    // Mutate during "Play".
    svc.GetZonePayload(g)->Offsets[0] = 999.0f;
    svc.NotifyZonePayloadEdited(g, 0, 0, 1, 1);
    EXPECT_FLOAT_EQ(svc.GetZonePayload(g)->Offsets[0], 999.0f);

    const uint64 epochBefore = svc.GetZonePayloadEditEpoch();
    svc.RestoreZonePayloadsFromPlaySnapshot();
    EXPECT_FLOAT_EQ(svc.GetZonePayload(g)->Offsets[0], 0.0f); // ramp u=0 -> 0
    EXPECT_GT(svc.GetZonePayloadEditEpoch(), epochBefore);    // bake re-reads
}

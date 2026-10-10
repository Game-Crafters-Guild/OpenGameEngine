// HLOD bake driver (design v0.2 §8). End-to-end world-facing bake: resolve the
// HLODVolume config, gather members, bake, and write the .gehlod. Exercised with
// a World(nullptr) + in-memory models + a temp output path, then read back
// through HlodCache to confirm the round trip.

#include "Assets/HlodBakeDriver.h"
#include "Assets/HlodCache.h"
#include "Assets/ModelAsset.h"

#include "Components/Rendering/HLODVolume.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::Hlod;

namespace {

Mesh MakeBox(float extent) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    for (int i = 0; i < 3; ++i) {
        Vertex v{};
        v.Position[0] = static_cast<float>(i) * extent;
        v.Normal[2] = 1.0f;
        mesh.Vertices.push_back(v);
    }
    mesh.Indices = {0, 1, 2};
    mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = -extent;
    mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = extent;
    return mesh;
}

struct ModelBank {
    std::unordered_map<GUID, std::unique_ptr<ModelAsset>> Models;
    const ModelAsset* Add(const GUID& guid, Vector<Mesh> meshes) {
        auto a = std::make_unique<ModelAsset>(guid, std::filesystem::path{});
        a->SetMeshesForTest(std::move(meshes));
        const ModelAsset* raw = a.get();
        Models[guid] = std::move(a);
        return raw;
    }
    ModelResolver Resolver() {
        return [this](const GUID& g) -> const ModelAsset* {
            auto it = Models.find(g);
            return it == Models.end() ? nullptr : it->second.get();
        };
    }
};

WorldTransform WorldAt(float x, float y, float z) {
    WorldTransform wt{};
    wt.matrix[12] = x;
    wt.matrix[13] = y;
    wt.matrix[14] = z;
    return wt;
}

SceneEntityTag Tag(const std::string& s) {
    SceneEntityTag t{};
    std::memset(t.value, 0, sizeof(t.value));
    std::strncpy(t.value, s.c_str(), sizeof(t.value) - 1);
    return t;
}

LocalBounds BoxBounds(float extent) {
    LocalBounds lb{};
    lb.Box = Mathematics::BoundingBox::FromMinMax(
        Mathematics::Vector3{-extent, -extent, -extent},
        Mathematics::Vector3{extent, extent, extent});
    return lb;
}

std::filesystem::path TempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

// Spawn `count` members in one cell, each a distinct model+material (r = 1, so
// the benefit gate admits the cluster).
void SpawnGrid(ECS::World& world, ModelBank& bank, int count, float spacing) {
    for (int i = 0; i < count; ++i) {
        const GUID modelGuid = GUID::Derive(GUID::Null(), "bake/model/" + std::to_string(i));
        const GUID matGuid = GUID::Derive(GUID::Null(), "bake/mat/" + std::to_string(i));
        // Distinct source content hash per model so the benefit heuristic sees
        // distinct meshes (r = members/distinct = 1, a low-instancing cluster the
        // gate admits) rather than one shared mesh (r = 12, excluded).
        const ModelAsset* model = bank.Add(modelGuid, {MakeBox(1.0f)});
        const_cast<ModelAsset*>(model)->SetSourceContentHashForTest(0x5000u + i);
        ECS::Entity e = world.Create();
        e.Set(WorldAt(static_cast<float>(i) * spacing, 0.0f, 0.0f));
        e.Set(BoxBounds(1.0f));
        e.Set(Tag("member_" + std::to_string(i)));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(matGuid);
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
    }
}

} // namespace

TEST(HlodBakeDriver, NoVolumeWritesNothing) {
    ECS::World world(nullptr);
    ModelBank bank;
    SpawnGrid(world, bank, 12, 2.0f);
    world.ProcessCommands();

    const std::filesystem::path out = TempPath("hlod_no_volume.gehlod");
    std::filesystem::remove(out);

    BakeDriverResult r = BakeHlodForWorld(world, bank.Resolver(), out);
    EXPECT_EQ(r.Outcome, BakeOutcome::NoVolume);
    EXPECT_FALSE(std::filesystem::exists(out)) << "no opt-in volume must write nothing";
}

TEST(HlodBakeDriver, BakesAndWritesGehlodRoundTrip) {
    ECS::World world(nullptr);
    ModelBank bank;

    // One enabled volume: cell big enough to hold the whole grid in one cluster.
    ECS::Entity volume = world.Create();
    HLODVolume vol{};
    vol.CellSize = 1000.0f;
    vol.MaxInstancingRatio = 4.0f;
    vol.MinMembers = 8u;
    vol.VBBudgetMB = 256u;
    volume.Set(vol);

    SpawnGrid(world, bank, 12, 2.0f);
    world.ProcessCommands();

    const std::filesystem::path out = TempPath("hlod_bake_roundtrip.gehlod");
    std::filesystem::remove(out);

    BakeDriverResult r = BakeHlodForWorld(world, bank.Resolver(), out);
    ASSERT_EQ(r.Outcome, BakeOutcome::Wrote);
    EXPECT_EQ(r.GatheredMembers, 12u);
    EXPECT_EQ(r.Stats.ClusterCount, 1u);
    EXPECT_EQ(r.Stats.AdmittedCount, 1u);
    ASSERT_TRUE(std::filesystem::exists(out));

    // Read the .gehlod back and confirm the cluster survived the round trip.
    HlodBakedScene loaded;
    HlodCacheStatus status = ReadHlodCache(out, /*expectConfigHash=*/nullptr, loaded);
    EXPECT_EQ(status, HlodCacheStatus::Hit);
    ASSERT_EQ(loaded.Clusters.size(), 1u);
    EXPECT_EQ(loaded.Clusters[0].Members.size(), 12u);
    EXPECT_FALSE(loaded.Clusters[0].Submeshes.empty());

    std::error_code ec;
    std::filesystem::remove(out, ec);
}

TEST(HlodBakeDriver, DisabledVolumeIsInert) {
    ECS::World world(nullptr);
    ModelBank bank;
    ECS::Entity volume = world.Create();
    volume.Set(HLODVolume{});
    volume.SetEnabled<HLODVolume>(false);
    SpawnGrid(world, bank, 12, 2.0f);
    world.ProcessCommands();

    const std::filesystem::path out = TempPath("hlod_disabled_volume.gehlod");
    std::filesystem::remove(out);

    BakeDriverResult r = BakeHlodForWorld(world, bank.Resolver(), out);
    EXPECT_EQ(r.Outcome, BakeOutcome::NoVolume);
    EXPECT_FALSE(std::filesystem::exists(out));
}

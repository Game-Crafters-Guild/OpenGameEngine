// HLOD member gather (design v0.2 §3.2). Bridges the ECS scene to the pure
// baker: mirrors the extraction eligibility query, resolves each eligible static
// member's bake-key + spatial inputs, and emits the parallel MemberInput/Mesh*
// arrays. Pure enough to exercise with a World(nullptr) + in-memory ModelAssets;
// no device, no RenderServices, no AssetManager.

#include "Assets/HlodMemberGather.h"
#include "Assets/ModelAsset.h"

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/SceneEntityTag.h"
#include "Components/Transform.h"

#include "ECS/Components.h"
#include "ECS/ECSTemplates.h" // World::AddComponent<T> bodies (component e.Set paths)
#include "ECS/Entity.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Components;
using namespace GameEngine::Hlod;

namespace {

Mesh MakeTriangle(uint32 lodCount = 1u) {
    Mesh mesh;
    mesh.PrimitiveTopology = MeshPrimitiveTopology::Triangles;
    for (int i = 0; i < 3; ++i) {
        Vertex v{};
        v.Position[0] = static_cast<float>(i);
        v.Normal[2] = 1.0f;
        mesh.Vertices.push_back(v);
    }
    mesh.Indices = {0, 1, 2};
    // Add generated (index-only) LODs sharing the LOD0 vertices.
    for (uint32 l = 1; l < lodCount; ++l) {
        mesh.ExtraLODs.push_back({0, 1, 2});
        mesh.ExtraLODErrors.push_back(0.1f * static_cast<float>(l));
        mesh.ExtraLODSloppy.push_back(0);
    }
    mesh.MinBounds[0] = 0.0f; mesh.MinBounds[1] = 0.0f; mesh.MinBounds[2] = 0.0f;
    mesh.MaxBounds[0] = 2.0f; mesh.MaxBounds[1] = 1.0f; mesh.MaxBounds[2] = 0.0f;
    return mesh;
}

SceneEntityTag MakeTag(const char* id) {
    SceneEntityTag t{};
    std::memset(t.value, 0, sizeof(t.value));
    std::strncpy(t.value, id, sizeof(t.value) - 1);
    return t;
}

// A model registry the resolver reads from. Owns the assets so the Mesh
// pointers the gather returns stay valid across the whole test.
struct ModelBank {
    std::unordered_map<GUID, std::unique_ptr<ModelAsset>> Models;

    const ModelAsset* Add(const GUID& guid, Vector<Mesh> meshes) {
        auto asset = std::make_unique<ModelAsset>(guid, std::filesystem::path{});
        asset->SetMeshesForTest(std::move(meshes));
        const ModelAsset* raw = asset.get();
        Models[guid] = std::move(asset);
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

LocalBounds UnitBounds() {
    LocalBounds lb{};
    lb.Box = Mathematics::BoundingBox::FromMinMax(Mathematics::Vector3{0, 0, 0},
                                                  Mathematics::Vector3{2, 1, 0});
    lb.CastShadows = true;
    return lb;
}

} // namespace

TEST(HlodMemberGather, GathersEligibleStaticMember) {
    ECS::World world(nullptr);
    ModelBank bank;
    const GUID modelGuid = GUID::Derive(GUID::Null(), "model/A");
    const GUID materialGuid = GUID::Derive(GUID::Null(), "material/A");
    bank.Add(modelGuid, {MakeTriangle(3u)});

    ECS::Entity e = world.Create();
    e.Set(WorldAt(10.0f, 0.0f, 20.0f));
    e.Set(UnitBounds());
    e.Set(MakeTag("cube_1"));
    MeshRenderer mr{};
    mr.materialAssetGuid.Set(materialGuid);
    mr.modelAssetGuid.Set(modelGuid);
    mr.castShadows = true;
    e.Set(mr);
    world.ProcessCommands();

    GatheredMembers gathered = GatherHlodMembers(world, bank.Resolver());

    ASSERT_EQ(gathered.Members.size(), 1u);
    ASSERT_EQ(gathered.Geometry.size(), 1u);
    ASSERT_EQ(gathered.Entities.size(), 1u);

    const MemberInput& m = gathered.Members[0];
    EXPECT_EQ(m.MaterialGuid, materialGuid);
    EXPECT_EQ(m.StableId, GUID::Derive(GUID::Null(), "hlod-member:cube_1"));
    EXPECT_EQ(m.ChosenLod, 2u); // LODCount()-1 for a 3-LOD mesh
    EXPECT_EQ(m.VertexCount, 3u);
    EXPECT_TRUE(m.CastsShadow);
    EXPECT_FLOAT_EQ(m.WorldMatrix[12], 10.0f);
    EXPECT_FLOAT_EQ(m.WorldMatrix[14], 20.0f);
    // World AABB = local [0,2]x[0,1]x0 translated by (10,0,20).
    EXPECT_FLOAT_EQ(m.WorldAabbMin[0], 10.0f);
    EXPECT_FLOAT_EQ(m.WorldAabbMax[0], 12.0f);
    EXPECT_FLOAT_EQ(m.WorldAabbMax[1], 1.0f);
    EXPECT_EQ(gathered.Geometry[0], &bank.Models[modelGuid]->GetMesh(0));
    EXPECT_EQ(gathered.Entities[0].id, e.GetHandle().id);
    EXPECT_NE(m.MeshContentHash, 0ull);
    EXPECT_NE(m.MeshHandleKey, 0ull);
}

TEST(HlodMemberGather, ExcludesSkinnedMorphDisabledAndUnresolved) {
    ECS::World world(nullptr);
    ModelBank bank;
    const GUID modelGuid = GUID::Derive(GUID::Null(), "model/B");
    const GUID materialGuid = GUID::Derive(GUID::Null(), "material/B");
    bank.Add(modelGuid, {MakeTriangle()});

    // Skinned member.
    {
        ECS::Entity e = world.Create();
        e.Set(WorldAt(0, 0, 0));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(materialGuid);
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
        SkinnedMeshRenderer smr{};
        smr.skeletonId = 42u;
        e.Set(smr);
    }
    // Morph member.
    {
        ECS::Entity e = world.Create();
        e.Set(WorldAt(1, 0, 0));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(materialGuid);
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
        MorphTargetWeights morph{};
        e.Set(morph);
    }
    // Disabled member: its renderer is switched off, so the gather never visits it.
    {
        ECS::Entity e = world.Create();
        e.Set(WorldAt(2, 0, 0));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(materialGuid);
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
        e.SetEnabled<MeshRenderer>(false);
    }
    // No material.
    {
        ECS::Entity e = world.Create();
        e.Set(WorldAt(3, 0, 0));
        MeshRenderer mr{};
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
    }
    // Unresolvable model.
    {
        ECS::Entity e = world.Create();
        e.Set(WorldAt(4, 0, 0));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(materialGuid);
        mr.modelAssetGuid.Set(GUID::Derive(GUID::Null(), "model/missing"));
        e.Set(mr);
    }
    world.ProcessCommands();

    GatheredMembers gathered = GatherHlodMembers(world, bank.Resolver());

    EXPECT_TRUE(gathered.Members.empty());
    EXPECT_EQ(gathered.SkippedSkinned, 1u);
    EXPECT_EQ(gathered.SkippedMorph, 1u);
    EXPECT_EQ(gathered.SkippedNoMaterial, 1u);
    EXPECT_EQ(gathered.SkippedNoModel, 1u);
}

TEST(HlodMemberGather, StableIdAndKeysAreDeterministic) {
    ModelBank bank;
    const GUID modelGuid = GUID::Derive(GUID::Null(), "model/C");
    const GUID materialGuid = GUID::Derive(GUID::Null(), "material/C");
    bank.Add(modelGuid, {MakeTriangle()});

    auto gatherOnce = [&]() {
        ECS::World world(nullptr);
        ECS::Entity e = world.Create();
        e.Set(WorldAt(5, 6, 7));
        e.Set(MakeTag("stable_entity"));
        MeshRenderer mr{};
        mr.materialAssetGuid.Set(materialGuid);
        mr.modelAssetGuid.Set(modelGuid);
        e.Set(mr);
        world.ProcessCommands();
        return GatherHlodMembers(world, bank.Resolver());
    };

    GatheredMembers a = gatherOnce();
    GatheredMembers b = gatherOnce();
    ASSERT_EQ(a.Members.size(), 1u);
    ASSERT_EQ(b.Members.size(), 1u);
    EXPECT_EQ(a.Members[0].StableId, b.Members[0].StableId);
    EXPECT_EQ(a.Members[0].MeshContentHash, b.Members[0].MeshContentHash);
    EXPECT_EQ(a.Members[0].MeshHandleKey, b.Members[0].MeshHandleKey);
}

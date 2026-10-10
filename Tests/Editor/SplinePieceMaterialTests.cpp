// Which material a spawned spline piece ends up rendering with.
//
// MeshMaterialFill::FromModel pins a placed piece to the mesh's EMBEDDED model
// material; kit FBX routinely embed a bare untextured Lambert or name source
// textures the project never imported, so the recipes carry an optional
// override that wins when set and falls back to the embedded material when
// null or unrenderable.
//
// The two halves of that binding split by dependency: the bind touches no
// RenderServices and is pinned behaviourally here; registering the override with
// the runtime material registry needs a live RenderServices and is not reachable
// from this target (see the registration note at the end of this file).

#include <gtest/gtest.h>

#include "Assets/ModelAsset.h"
#include "AssetCore/GUID.h"
#include "Components/AssetRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Placement/SplinePieceMaterial.h"

#include <initializer_list>
#include <utility>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;
using Components::MaterialRef;
using Editor::BindPieceMaterial;

namespace
{

Vector<Mesh> NamedMeshes(std::initializer_list<std::pair<const char*, uint32>> items)
{
    Vector<Mesh> meshes;
    for (const auto& item : items)
    {
        Mesh mesh;
        mesh.Name = item.first;
        mesh.MaterialIndex = item.second;
        meshes.push_back(std::move(mesh));
    }
    return meshes;
}

// A two-submesh model whose submeshes carry distinct embedded materials, so a
// bind that silently fell back to the model can never be mistaken for a bind
// that honoured the override.
ModelRenderResources TwoSubmeshResources(ModelAsset& model, GUID& embeddedSlot0,
                                         GUID& embeddedSlot1)
{
    model.SetMeshesForTest(NamedMeshes({{"SM_Path_01", 0}, {"SM_Path_02", 1}}));

    embeddedSlot0 = GUID::Generate();
    embeddedSlot1 = GUID::Generate();

    ModelRenderResources resources{};
    resources.modelGuid = GUID::Generate();
    resources.modelAsset = &model;
    resources.meshHandles = {Rendering::MeshGPUHandle(1u, static_cast<uint8_t>(1)),
                             Rendering::MeshGPUHandle(2u, static_cast<uint8_t>(1))};

    ConvertedModelMaterial slot0{};
    slot0.materialIndex = 0;
    slot0.derivedGuid = embeddedSlot0;
    ConvertedModelMaterial slot1{};
    slot1.materialIndex = 1;
    slot1.derivedGuid = embeddedSlot1;
    resources.materials = {slot0, slot1};
    return resources;
}

} // namespace

// The regression guard for every recipe authored before the override existed: a
// null override must reproduce the old FromModel bind EXACTLY, field for field,
// on every submesh — not merely "some embedded material".
TEST(SplinePieceMaterial, NullOverrideBindsExactlyWhatFromModelDid)
{
    ModelAsset model(GUID::Generate(), "kit.fbx");
    GUID embeddedSlot0, embeddedSlot1;
    const ModelRenderResources resources = TwoSubmeshResources(model, embeddedSlot0, embeddedSlot1);

    for (uint32 submesh = 0; submesh < 2u; ++submesh)
    {
        Components::MeshRenderer viaOverride{};
        BindPieceMaterial(viaOverride, resources, submesh, MaterialRef{});

        Components::MeshRenderer viaFromModel{};
        PopulateMeshRenderer(viaFromModel, resources, submesh, MeshMaterialFill::FromModel);

        EXPECT_EQ(viaOverride.materialAssetGuid, viaFromModel.materialAssetGuid)
            << "submesh " << submesh;
        EXPECT_EQ(viaOverride.modelAssetGuid, viaFromModel.modelAssetGuid) << "submesh " << submesh;
        EXPECT_EQ(viaOverride.meshGpuHandleId, viaFromModel.meshGpuHandleId)
            << "submesh " << submesh;
    }

    // ...and that the fallback is the submesh's OWN material, so the equality
    // above is not two identically-wrong binds.
    Components::MeshRenderer slot1{};
    BindPieceMaterial(slot1, resources, 1, MaterialRef{});
    EXPECT_EQ(slot1.materialAssetGuid.ToGuid(), embeddedSlot1);
    EXPECT_NE(slot1.materialAssetGuid.ToGuid(), embeddedSlot0);
}

// A set override replaces the embedded material on EVERY submesh — the whole
// point being that one material covers a placement whose pieces disagree about
// what they embed.
TEST(SplinePieceMaterial, SetOverrideReplacesEmbeddedMaterialOnEverySubmesh)
{
    ModelAsset model(GUID::Generate(), "kit.fbx");
    GUID embeddedSlot0, embeddedSlot1;
    const ModelRenderResources resources = TwoSubmeshResources(model, embeddedSlot0, embeddedSlot1);

    const MaterialRef override_(GUID::Generate());

    for (uint32 submesh = 0; submesh < 2u; ++submesh)
    {
        Components::MeshRenderer mr{};
        BindPieceMaterial(mr, resources, submesh, override_);

        EXPECT_EQ(mr.materialAssetGuid, override_) << "submesh " << submesh;
        EXPECT_NE(mr.materialAssetGuid.ToGuid(), embeddedSlot0) << "submesh " << submesh;
        EXPECT_NE(mr.materialAssetGuid.ToGuid(), embeddedSlot1) << "submesh " << submesh;
    }
}

// Overriding the material must not cost the piece its geometry: the mesh handle
// and model GUID still bind, or the tile would vanish instead of retexturing.
TEST(SplinePieceMaterial, OverrideStillBindsMeshHandleAndModelGuid)
{
    ModelAsset model(GUID::Generate(), "kit.fbx");
    GUID embeddedSlot0, embeddedSlot1;
    const ModelRenderResources resources = TwoSubmeshResources(model, embeddedSlot0, embeddedSlot1);

    Components::MeshRenderer mr{};
    BindPieceMaterial(mr, resources, 1, MaterialRef(GUID::Generate()));

    EXPECT_NE(mr.meshGpuHandleId, 0u);
    EXPECT_EQ(mr.modelAssetGuid.ToGuid(), resources.modelGuid);
}

// Both controllers build fresh piece storage every rebuild, but the bind must
// still be order-independent on a reused MeshRenderer: PreserveExplicit keeps
// whatever the field holds, so a stale value would survive any future caller
// that does reuse one. This pins the fallback rather than trusting callers.
TEST(SplinePieceMaterial, ClearingTheOverrideRebindsTheEmbeddedMaterial)
{
    ModelAsset model(GUID::Generate(), "kit.fbx");
    GUID embeddedSlot0, embeddedSlot1;
    const ModelRenderResources resources = TwoSubmeshResources(model, embeddedSlot0, embeddedSlot1);

    Components::MeshRenderer mr{};
    BindPieceMaterial(mr, resources, 1, MaterialRef(GUID::Generate()));
    ASSERT_NE(mr.materialAssetGuid.ToGuid(), embeddedSlot1);

    BindPieceMaterial(mr, resources, 1, MaterialRef{});
    EXPECT_EQ(mr.materialAssetGuid.ToGuid(), embeddedSlot1)
        << "a cleared override must fall back to the embedded material, not keep the old one";
}

// Registration note — why EnsureOverrideMaterialRegistered has no test here.
//
// It needs a live RenderServices, and every RenderServices fixture in the tree
// requires a real Vulkan device (MaterialBridgeIntegrationTests GTEST_SKIPs
// without one), so a unit test here would silently skip headless. What the
// function must guarantee is instead stated in its contract and checked by the
// arc's runtime editor gate: RegisterOneStandaloneMaterial is void with silent
// failure exits, so Ensure* re-reads the registry after the attempt, warns once
// per GUID on failure, and returns renderability; callers bind the override
// only on true, so a failed registration degrades to the caller's registered
// fallback instead of binding a GUID render extraction refuses (what that
// refusal looks like per piece is stated in SplinePieceMaterial.h). The return
// is re-evaluated per rebuild, so a GUID registered later by any other path
// starts applying without an editor restart.

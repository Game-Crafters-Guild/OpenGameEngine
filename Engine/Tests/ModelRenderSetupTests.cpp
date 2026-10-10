// Tests for ModelRenderSetup's submesh resolution and MeshRenderer GPU-field
// population — the scene-load path that binds a model-asset MeshRenderer to a
// GPU mesh handle. A regression here shows up as scene entities that load but
// never bind a meshGpuHandle, so extraction builds zero GPU instances and the
// viewport renders only sky. #433 (submesh-by-name) introduced the name-based
// selection exercised here; these cases pin the default binding path plus the
// known Synty multi-material-part limitation.

#include <gtest/gtest.h>

#include "Engine/Rendering/MeshNameRegistry.h"
#include "Engine/Rendering/ModelRenderSetup.h"
#include "Engine/Rendering/SceneResolveService.h"
#include "Engine/Rendering/RenderServices.h"
#include "ECS/ChangeFilter.h"
#include "Assets/AuthoredLodImport.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/Rendering/MorphTargetWeights.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Transform.h"
#include "AssetCore/GUID.h"
#include "ECS/World.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Types/Fnv1a.h"

#include "EngineLogCapture.h"
#include "TestDeviceHelper.h"

#include <chrono>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Engine::Renderer;

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

} // namespace

// The default scene-load case (no positional meshId, no name selector) must
// resolve to submesh 0 — never a skip. This is the exact contract whose failure
// would leave every converted-scene MeshRenderer unbound (zero GPU instances).
TEST(ModelRenderSetupTest, ResolveSubmesh_DefaultNoIdNoName_BindsSubmeshZero)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}}));

    Components::MeshRenderer mr{};
    mr.meshId = 0;
    mr.MeshNameId = 0;

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
}

// An explicit positional meshId always wins, even when a name selector is set.
TEST(ModelRenderSetupTest, ResolveSubmesh_ExplicitMeshIdWinsOverName)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}, {"SM_Door_01", 2}}));

    Components::MeshRenderer mr{};
    mr.meshId = 2;
    mr.MeshNameId = Components::HashMeshName("SM_Wall_01"); // would pick 0 if consulted

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 2u);
}

// A non-zero MeshNameId selects the submesh whose name matches.
TEST(ModelRenderSetupTest, ResolveSubmesh_NameMatchSelectsNamedSubmesh)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}, {"SM_Door_01", 2}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("SM_Door_01");

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 2u);
}

// Name matching is ASCII case-folded so importer/DCC casing never blocks a match.
TEST(ModelRenderSetupTest, ResolveSubmesh_NameMatchIsCaseFolded)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("sm_roof_01"); // lowercase authored form

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 1u);
}

// A name miss falls back to submesh 0 (still binds) rather than skipping.
TEST(ModelRenderSetupTest, ResolveSubmesh_NameMissFallsBackToZero)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("SM_DoesNotExist");

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
}

// Known limitation (documented, not desired): when an FBX node carries multiple
// material parts, the loader splits it into submeshes suffixed `_<partIdx>`
// (e.g. Synty `SM_Bld_Roof_Convex_Tower_02_0..3`), but the Unity-scene converter
// authors the bare node name (no suffix). The bare name therefore matches none
// of the suffixed submeshes and falls back to submesh 0. A real fix belongs in
// the converter (emit per-submesh names) or the loader, not here — this case
// pins current behavior so such a change is a deliberate, visible edit.
TEST(ModelRenderSetupTest, ResolveSubmesh_SyntyPartSuffix_KnownFallbackToZero)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Tower_02_0", 0}, {"SM_Tower_02_1", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("SM_Tower_02"); // bare node name, no _<partIdx>

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
}

// Phase C1 (#3): the CONFIRMED-BROKEN collision case. Two distinct submeshes
// whose names differ only by a trailing "_LOD<N>" must resolve EXACTLY — a
// symmetric fold in the hash would collapse both to hash("Torso") and
// first-match-wins would misroute the "Torso_LOD1" query to submesh 0. The
// exact (raw) pass must resolve each to its own index.
TEST(ModelRenderSetupTest, ResolveSubmesh_DistinctLodSiblingsResolveExactly)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"Torso", 0}, {"Torso_LOD1", 1}, {"Head", 2}}));

    Components::MeshRenderer torso{};
    torso.MeshNameId = Components::HashMeshName("Torso");
    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, torso), 0u);

    Components::MeshRenderer lod1{};
    lod1.MeshNameId = Components::HashMeshName("Torso_LOD1");
    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, lod1), 1u)
        << "the _LOD1 sibling must resolve to its own index, not collapse onto Torso";
}

// Fallback pass: a bare base-name query resolves to a submesh that still carries
// a "_LOD0" suffix (no exact hit, so the folded pass strips the submesh suffix).
// The consumed base is the only such submesh, so nothing shadows it.
TEST(ModelRenderSetupTest, ResolveSubmesh_BareQueryFoldsToSuffixedConsumedBase)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"Torso_LOD0", 0}, {"Head", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("Torso"); // bare base name, no exact hit

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
}

// Phase C2a end-to-end (R3): after _LOD-suffix consumption, an entity referencing
// the BARE base name binds the assembled LOD0 submesh, and the consumed siblings
// are gone so none can shadow it (exact-first pass hits the base directly).
TEST(ModelRenderSetupTest, ResolveSubmesh_ConsumedLodFamilyResolvesBaseName)
{
    Vector<Mesh> meshes = NamedMeshes({{"Torso", 0}, {"Torso_LOD1", 0}, {"Torso_LOD2", 0}, {"Head", 1}});
    for (Mesh& m : meshes) // give the siblings real geometry to consume
    {
        m.Vertices.resize(3);
        m.Indices = {0u, 1u, 2u};
    }
    ConsumeLodSuffixFamilies(meshes);

    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(std::move(meshes));
    ASSERT_EQ(model.GetMeshCount(), 2u); // Torso (authored) + Head; siblings consumed
    EXPECT_TRUE(model.GetMesh(0).HasAuthoredLODs());

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("Torso");
    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
}

// A plain part suffix ("_0", no "_LOD") is NOT folded — only "_LOD<N>" is.
TEST(ModelRenderSetupTest, ResolveSubmesh_PartSuffixIsNotFolded)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"Bolt_0", 0}, {"Bolt_1", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("Bolt_1"); // exact match, no fold

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 1u);
}

// Phase C1 (#3): an explicit meshId past the submesh count must fall back to 0
// rather than flow out-of-range into GetMesh.
TEST(ModelRenderSetupTest, ResolveSubmesh_MeshIdOutOfRangeFallsBackToZero)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}}));

    Components::MeshRenderer mr{};
    mr.meshId = 5; // only 2 submeshes exist

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
}

// A single-submesh model ignores the name selector entirely (early out).
TEST(ModelRenderSetupTest, ResolveSubmesh_SingleMeshIgnoresName)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Only_01", 0}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("SM_Something_Else");

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
}

// A miss names the mesh it wanted. The hash alone told nobody which submesh was
// missing, which is what made a wrong-submesh draw silent.
TEST(ModelRenderSetupTest, ResolveSubmesh_MissWarnsNamingTheMesh)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = InternMeshName("SM_Absent_Leaves_07");

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("MeshNameProbe: sink is live");
        EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
        Logger::Log::Flush();
    }

    ASSERT_EQ(TestLog::CountLinesContaining(lines, "MeshNameProbe: sink is live"), 1u)
        << "capture saw nothing at all, so an absent warning would prove nothing";
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "SM_Absent_Leaves_07"), 1u)
        << "the miss must name the mesh: " << TestLog::FirstLineContaining(lines, "ModelRenderSetup");
}

// An id that never came through InternMeshName has no name to print — a scene
// that stored only `meshNameHash`. Say so rather than printing a bare number
// and leaving the reader to guess whether a name was lost or never existed.
TEST(ModelRenderSetupTest, ResolveSubmesh_MissWithoutANameSaysTheNameIsUnknown)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}}));

    Components::MeshRenderer mr{};
    // Deliberately NOT interned: this is what reading a raw hash out of a file gives.
    mr.MeshNameId = Components::HashMeshName("SM_NeverInterned_Unique_31337");

    std::vector<std::string> lines;
    {
        TestLog::ScopedEngineLogCapture capture(&lines, Logger::LogLevel::Warning);
        Logger::Log::Warning("MeshNameProbe: sink is live");
        EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
        Logger::Log::Flush();
    }

    ASSERT_EQ(TestLog::CountLinesContaining(lines, "MeshNameProbe: sink is live"), 1u);
    EXPECT_EQ(TestLog::CountLinesContaining(lines, "name unknown"), 1u)
        << TestLog::FirstLineContaining(lines, "ModelRenderSetup");
}

// The migration bridge: an id loaded as a bare hash gains its name the moment it
// binds, so saving that scene writes `meshName` and the file stops depending on
// the hash function.
TEST(ModelRenderSetupTest, ResolveSubmesh_ExactMatchInternsTheMatchedName)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_MigrateMe_02", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("SM_MigrateMe_02"); // as read from meshNameHash
    ASSERT_TRUE(FindMeshName(mr.MeshNameId).empty()) << "precondition: no name known yet";

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 1u);
    EXPECT_EQ(FindMeshName(mr.MeshNameId), "SM_MigrateMe_02");
}

// The folded pass must intern the FOLDED spelling: that is the string whose hash
// is the id, so it is the one a save can write and a reload can re-derive.
TEST(ModelRenderSetupTest, ResolveSubmesh_FoldedMatchInternsTheFoldedSpelling)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_FoldMe_09_LOD0", 0}, {"Head", 1}}));

    Components::MeshRenderer mr{};
    mr.MeshNameId = Components::HashMeshName("SM_FoldMe_09"); // bare base, folded pass hits
    ASSERT_TRUE(FindMeshName(mr.MeshNameId).empty());

    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, mr), 0u);
    EXPECT_EQ(FindMeshName(mr.MeshNameId), "SM_FoldMe_09");
    EXPECT_EQ(Components::HashMeshName(FindMeshName(mr.MeshNameId)), mr.MeshNameId);
}

// #1671's fold, from the file's point of view. A scene authored before the fold
// named the part `..._LOD1_0`; the model now exposes `..._LOD0_0`.
//   - the NAME the file stored re-hashes today into the id that binds;
//   - the HASH a pre-fold editor would have saved is a number today's function
//     never produces, so it misses both passes and silently draws submesh 0.
// This is the whole reason the selector is saved as text.
TEST(ModelRenderSetupTest, ResolveSubmesh_PreFoldNameBindsWherePreFoldHashOrphans)
{
    // Part _1 is the leaves; part _0 is the bark. Binding the wrong one is the
    // #1671 symptom, so the wanted submesh sits at a non-zero index and the
    // fallback-to-zero is visibly the wrong answer.
    constexpr const char* kAuthoredBeforeFold = "SM_Farm_GenTree_A_FullTree_01_LOD1_1";

    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest(NamedMeshes({{"SM_Farm_GenTree_A_FullTree_01_LOD0_0", 0},
                                        {"SM_Farm_GenTree_A_FullTree_01_LOD0_1", 1}}));

    // The name the scene file carries, re-hashed by today's engine.
    Components::MeshRenderer byName{};
    byName.MeshNameId = InternMeshName(kAuthoredBeforeFold);
    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, byName), 1u)
        << "the stored NAME still selects the leaves after the hash function changed";

    // What a pre-fold editor save stored instead: FNV-1a over the whole
    // case-folded name, with no interior "_LOD<N>" split.
    uint64_t preFoldHash = Hashing::kFnv1a64OffsetBasis;
    for (const char* p = kAuthoredBeforeFold; *p != '\0'; ++p)
    {
        const char folded = (*p >= 'A' && *p <= 'Z') ? static_cast<char>(*p - 'A' + 'a') : *p;
        preFoldHash ^= static_cast<unsigned char>(folded);
        preFoldHash *= Hashing::kFnv1a64Prime;
    }
    ASSERT_NE(preFoldHash, byName.MeshNameId) << "the fold must actually have changed this id";

    Components::MeshRenderer byHash{};
    byHash.MeshNameId = preFoldHash;
    EXPECT_EQ(ResolveSubmeshIndex(GUID::Generate(), model, byHash), 0u)
        << "an orphaned hash falls back to submesh 0 — bark drawn where leaves belong";
}

// PopulateMeshRenderer binds the GPU handle for a valid submesh and records the
// model GUID. This is the write half of the resolve path; if it left the handle
// zero, the entity would load but never render.
TEST(ModelRenderSetupTest, PopulateMeshRenderer_ValidSubmesh_BindsHandle)
{
    ModelRenderResources resources{};
    resources.modelGuid = GUID::Generate();
    resources.meshHandles = {Rendering::MeshGPUHandle(1u, static_cast<uint8_t>(1))};

    Components::MeshRenderer mr{};
    PopulateMeshRenderer(mr, resources, 0, MeshMaterialFill::FromModel);

    EXPECT_NE(mr.meshGpuHandleId, 0u);
    EXPECT_EQ(mr.modelAssetGuid.ToGuid(), resources.modelGuid);
}

// An out-of-range submesh leaves the handle unbound (zero) rather than reading
// past the handle array.
TEST(ModelRenderSetupTest, PopulateMeshRenderer_OutOfRangeSubmesh_LeavesHandleUnbound)
{
    ModelRenderResources resources{};
    resources.modelGuid = GUID::Generate();
    resources.meshHandles = {Rendering::MeshGPUHandle(1u, static_cast<uint8_t>(1))};

    Components::MeshRenderer mr{};
    PopulateMeshRenderer(mr, resources, 5, MeshMaterialFill::FromModel);

    EXPECT_EQ(mr.meshGpuHandleId, 0u);
}

namespace
{

// A model with two named submeshes whose material indices are 0 and 1, plus a
// converted-material table with a distinct derived GUID per slot and valid mesh
// handles. Shared by the material-fill tests below.
ModelRenderResources TwoSubmeshResources(ModelAsset& model,
                                         GUID& embeddedSlot0,
                                         GUID& embeddedSlot1)
{
    model.SetMeshesForTest(NamedMeshes({{"SM_Wall_01", 0}, {"SM_Roof_01", 1}}));

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

// The scene-load contract: an explicitly-authored MeshRenderer.material must win
// over the model's embedded per-submesh material when resolving a named submesh
// (k>0). This is the [mat k] recolor path — the regression was that resolution
// overwrote the override with the FBX-embedded material, so a submesh addressed
// by name rendered the wrong (model-default) material.
TEST(ModelRenderSetupTest, PopulateMeshRenderer_PreserveExplicit_KeepsAuthoredMaterialOnSubmeshK)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    GUID embeddedSlot0, embeddedSlot1;
    ModelRenderResources resources = TwoSubmeshResources(model, embeddedSlot0, embeddedSlot1);

    const GUID authored = GUID::Generate();
    Components::MeshRenderer mr{};
    mr.materialAssetGuid.Set(authored);

    PopulateMeshRenderer(mr, resources, 1, MeshMaterialFill::PreserveExplicit);

    EXPECT_EQ(mr.materialAssetGuid.ToGuid(), authored)
        << "explicit scene material must win over the embedded submesh material";
    EXPECT_NE(mr.materialAssetGuid.ToGuid(), embeddedSlot1);
    EXPECT_NE(mr.meshGpuHandleId, 0u); // still binds the named submesh's GPU handle
}

// PreserveExplicit with NO authored material falls back to the embedded submesh
// material, so a hand-authored entity that names only a mesh still renders.
TEST(ModelRenderSetupTest, PopulateMeshRenderer_PreserveExplicit_NoMaterialFallsBackToEmbedded)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    GUID embeddedSlot0, embeddedSlot1;
    ModelRenderResources resources = TwoSubmeshResources(model, embeddedSlot0, embeddedSlot1);

    Components::MeshRenderer mr{}; // no explicit material

    PopulateMeshRenderer(mr, resources, 1, MeshMaterialFill::PreserveExplicit);

    EXPECT_EQ(mr.materialAssetGuid.ToGuid(), embeddedSlot1);
}

// FromModel (drag-drop instantiation, inspector model reassignment) always binds
// the submesh's embedded material, overwriting any prior assignment.
TEST(ModelRenderSetupTest, PopulateMeshRenderer_FromModel_OverwritesWithEmbeddedMaterial)
{
    ModelAsset model(GUID::Generate(), "test.fbx");
    GUID embeddedSlot0, embeddedSlot1;
    ModelRenderResources resources = TwoSubmeshResources(model, embeddedSlot0, embeddedSlot1);

    Components::MeshRenderer mr{};
    mr.materialAssetGuid.Set(GUID::Generate()); // stale prior assignment

    PopulateMeshRenderer(mr, resources, 1, MeshMaterialFill::FromModel);

    EXPECT_EQ(mr.materialAssetGuid.ToGuid(), embeddedSlot1)
        << "authoring assignment binds the submesh's embedded material";
}

// --- Cache-seeded resolve-body parity (P2) -----------------------------------
// ResolveOneModelEntity is the per-entity body the scene-load resolve and the
// incremental SceneBuildPump share. These drive it directly with a PRE-SEEDED
// ModelResolveCache (so no AssetManager load, no real GPU model resolve) and
// pin the exact component set it writes — the guard against future drift when
// the body is edited. Device-gated only because ResolveOneModelEntity's bounds
// fallback queries RenderServices' mesh registry.

namespace
{

Mesh StaticMesh(const char* name, uint32 materialIndex)
{
    Mesh m;
    m.Name = name;
    m.MaterialIndex = materialIndex;
    m.Vertices.resize(3);
    m.Indices = {0u, 1u, 2u};
    return m;
}

Mesh SkinnedMesh(const char* name, uint32 materialIndex)
{
    Mesh m = StaticMesh(name, materialIndex);
    m.Skinned = true;
    m.Joints0.resize(m.Vertices.size() * 4); // IsSkinned() checks size == Vertices*4
    m.Weights0.resize(m.Vertices.size() * 4);
    return m;
}

// Synthetic resources for `model`, keyed by `modelGuid`, with a distinct derived
// material GUID per slot and handles that are NOT in any real registry (so the
// bounds path takes the ComputeModelBounds fallback rather than a GPU lookup).
ModelRenderResources SeededResources(const GUID& modelGuid, ModelAsset& model,
                                     GUID& outSlot0, GUID& outSlot1)
{
    outSlot0 = GUID::Generate();
    outSlot1 = GUID::Generate();
    ModelRenderResources res{};
    res.modelGuid = modelGuid;
    res.modelAsset = &model;
    res.meshHandles = {Rendering::MeshGPUHandle(1u, static_cast<uint8_t>(1)),
                       Rendering::MeshGPUHandle(2u, static_cast<uint8_t>(1))};
    ConvertedModelMaterial slot0{};
    slot0.materialIndex = 0;
    slot0.derivedGuid = outSlot0;
    ConvertedModelMaterial slot1{};
    slot1.materialIndex = 1;
    slot1.derivedGuid = outSlot1;
    res.materials = {slot0, slot1};
    return res;
}

} // namespace

// A static (non-skinned, no morph) entity gains a bound mesh handle, LocalBounds,
// and WorldTransform — and NO SkinnedMeshRenderer / MorphTargetWeights.
TEST(ModelRenderSetupTest, ResolveOneModelEntity_Static_WritesExpectedComponentSet)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world;
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest([]{ Vector<Mesh> v; v.push_back(StaticMesh("SM_Wall_01", 0)); return v; }());

    const GUID modelGuid = GUID::Generate();
    GUID slot0, slot1;
    ModelResolveCache cache;
    cache.emplace(modelGuid, SeededResources(modelGuid, model, slot0, slot1));

    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid); // meshGpuHandleId == 0 (unresolved)
    ECS::EntityHandle e = world.Create<Components::MeshRenderer>(mr).GetHandle();

    ResolveOneModelEntity(world, rs, e, modelGuid, cache);

    const auto* out = world.GetComponent<Components::MeshRenderer>(e);
    ASSERT_NE(out, nullptr);
    EXPECT_NE(out->meshGpuHandleId, 0u) << "static entity must bind its GPU mesh handle";
    EXPECT_EQ(out->materialAssetGuid.ToGuid(), slot0) << "fills the submesh's embedded material";
    EXPECT_EQ(world.GetComponent<Components::SkinnedMeshRenderer>(e), nullptr);
    EXPECT_EQ(world.GetComponent<Components::MorphTargetWeights>(e), nullptr);
    EXPECT_NE(world.GetComponent<Components::LocalBounds>(e), nullptr);
    EXPECT_NE(world.GetComponent<Components::WorldTransform>(e), nullptr);

    device->Shutdown();
}

// A skinned submesh additionally gains a SkinnedMeshRenderer (scene files author
// only the MeshRenderer; the resolve reconstructs the skinned component).
TEST(ModelRenderSetupTest, ResolveOneModelEntity_Skinned_AddsSkinnedMeshRenderer)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world;
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest([]{ Vector<Mesh> v; v.push_back(SkinnedMesh("SM_Body_01", 0)); return v; }());

    const GUID modelGuid = GUID::Generate();
    GUID slot0, slot1;
    ModelResolveCache cache;
    cache.emplace(modelGuid, SeededResources(modelGuid, model, slot0, slot1));

    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    ECS::EntityHandle e = world.Create<Components::MeshRenderer>(mr).GetHandle();

    ResolveOneModelEntity(world, rs, e, modelGuid, cache);

    EXPECT_NE(world.GetComponent<Components::MeshRenderer>(e)->meshGpuHandleId, 0u);
    EXPECT_NE(world.GetComponent<Components::SkinnedMeshRenderer>(e), nullptr)
        << "a skinned submesh must reconstruct its SkinnedMeshRenderer";

    device->Shutdown();
}

// PreserveExplicit: a scene-authored material on a multi-material submesh (k>0)
// survives the resolve — the [mat k] recolor path — rather than being overwritten
// by the model's embedded per-submesh material.
TEST(ModelRenderSetupTest, ResolveOneModelEntity_PreserveExplicit_KeepsAuthoredMaterial)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world;
    ModelAsset model(GUID::Generate(), "test.fbx");
    model.SetMeshesForTest([]{
        Vector<Mesh> v;
        v.push_back(StaticMesh("SM_Wall_01", 0));
        v.push_back(StaticMesh("SM_Roof_01", 1));
        return v;
    }());

    const GUID modelGuid = GUID::Generate();
    GUID slot0, slot1;
    ModelResolveCache cache;
    cache.emplace(modelGuid, SeededResources(modelGuid, model, slot0, slot1));

    const GUID authored = GUID::Generate();
    Components::MeshRenderer mr{};
    mr.modelAssetGuid.Set(modelGuid);
    mr.meshId = 1;                       // target submesh 1 (embedded material = slot1)
    mr.materialAssetGuid.Set(authored);  // explicit scene override
    ECS::EntityHandle e = world.Create<Components::MeshRenderer>(mr).GetHandle();

    ResolveOneModelEntity(world, rs, e, modelGuid, cache);

    const auto* out = world.GetComponent<Components::MeshRenderer>(e);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->materialAssetGuid.ToGuid(), authored)
        << "explicit scene material must win over the embedded submesh material";
    EXPECT_NE(out->materialAssetGuid.ToGuid(), slot1);
    EXPECT_NE(out->meshGpuHandleId, 0u);

    device->Shutdown();
}

// Runtime bind walks Changed MeshRenderers and hands the unresolved ones to the
// resolve service. An unresolvable model GUID is a miss the service records
// once: the contract is "no crash, no retry once the miss is recorded".
TEST(ModelRenderSetupTest, BindChangedMeshRenderers_UnresolvableGuidDoesNotCrash)
{
    auto device = CreateVulkanDeviceFast();
    if (!device)
        GTEST_SKIP() << "No Vulkan device available";
    RenderServices rs;
    ASSERT_TRUE(rs.Initialize(device.get()));

    ECS::World world;
    Components::MeshRenderer mr{};
    mr.meshGpuHandleId = 0;
    uint8_t bytes[16] = {0xEE, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                         0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    mr.modelAssetGuid.Set(GUID::FromBytes(bytes));
    world.Create<Components::MeshRenderer>(mr);

    ECS::ChangeGate gate{};
    SceneResolveService resolves;
    BindChangedMeshRenderers(world, rs, gate, resolves);
    EXPECT_NE(gate.LastRunVersion, 0u);
    EXPECT_EQ(resolves.Counts().HeldEntities, 1u) << "the unresolved entity was handed to the service";
    resolves.Step(world, rs, std::chrono::milliseconds(5));
    EXPECT_EQ(resolves.Counts().HeldEntities, 0u);
    EXPECT_EQ(resolves.Counts().MissedEntities, 1u);

    BindChangedMeshRenderers(world, rs, gate, resolves);
    EXPECT_EQ(resolves.Counts().HeldEntities, 0u) << "an unchanged entity was handed over again";

    device->Shutdown();
}

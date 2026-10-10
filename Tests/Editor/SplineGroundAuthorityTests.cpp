// What a spline recipe's declared ground authority provisions, and the one thing
// that has to be true of it: a run that says "grade the ground" must bake the
// SAME heightfield as the volume-plus-pooled-flatten an author would have placed
// by hand. Auto-provisioning that produced a nearly-identical region would be
// worse than none — the hand-authored path is the one the terrain modifier tests
// pin, and a second shape reaching the bake by a different route is a second
// thing to keep correct forever.

#include <gtest/gtest.h>

#include "Placement/SplineExtrudeSanitize.h"
#include "Placement/SplineGroundAuthority.h"
#include "UndoRedo/UndoRedoService.h"

#include "Components/Spline/SplineComponent.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h" // World's component templates: definitions, not just declarations
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "SplineECS/SplineService.h"
#include "TerrainECS/TerrainModifierSystem.h"
#include "TerrainECS/TerrainService.h"

#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::TerrainECS;
using GameEngine::Components::SplineGroundAuthority;

namespace
{

constexpr uint32 kHeightmapDim = 65;
constexpr float32 kWorldSize = 128.0f;
constexpr float32 kHeightScale = 100.0f;
constexpr float32 kSampleSpacing = kWorldSize / static_cast<float32>(kHeightmapDim - 1);
constexpr float32 kRunHalfWidth = 4.0f;
constexpr float32 kRunGrade = 12.0f;

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

struct ScopedSplineService
{
    ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
        SplineECS::SplineService::Initialize();
    }
    ~ScopedSplineService()
    {
        if (SplineECS::SplineService::IsInitialized())
            SplineECS::SplineService::Shutdown();
    }
};

Components::WorldTransform IdentityAt(float32 x, float32 y, float32 z)
{
    Components::WorldTransform xf{};
    xf.matrix[0] = xf.matrix[5] = xf.matrix[10] = xf.matrix[15] = 1.0f;
    xf.matrix[12] = x; xf.matrix[13] = y; xf.matrix[14] = z;
    return xf;
}

TerrainHandle CreateBakedTerrain(TerrainService& svc)
{
    Terrain::TerrainConfig config{};
    config.HeightmapWidth = kHeightmapDim;
    config.HeightmapHeight = kHeightmapDim;
    config.WorldSizeX = kWorldSize;
    config.WorldSizeZ = kWorldSize;
    config.HeightScale = kHeightScale;
    config.LODLevels = 4;
    const TerrainHandle handle = svc.CreateTerrain(config);
    auto* data = svc.GetTerrainData(handle);
    // FLAT base, so any difference between the two arms below is the graded run
    // and nothing else. Procedural noise would still compare equal, but a
    // failure would be far harder to read.
    for (uint32 z = 0; z < kHeightmapDim; ++z)
        for (uint32 x = 0; x < kHeightmapDim; ++x)
            data->Heightfield.SetSample(x, z, 0.25f);
    data->MarkFullDirty();
    svc.RebuildQuadtree(handle);
    data->ResetSplatmapAndCommitRange();
    return handle;
}

void CreateTerrainEntity(ECS::World& world, TerrainHandle handle)
{
    auto e = world.CreateEntity();
    Components::Terrain terrain{};
    terrain.SizeX = kWorldSize;
    terrain.SizeZ = kWorldSize;
    terrain.HeightScale = kHeightScale;
    terrain.TerrainDataHandle = handle.Index;
    terrain.TerrainDataGeneration = handle.Generation;
    terrain.BaseSource = Components::TerrainBaseSource::Flat;
    world.AddComponentImmediate<Components::Terrain>(e, terrain);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
}

// A straight run along X at constant height, carrying whatever recipe the caller
// hands it. The spline's width channel is the run's half-width, which is also
// what the volume's station polyline reads — there is no second width.
ECS::EntityHandle CreateRun(ECS::World& world, SplineECS::SplineService& svc, float32 z)
{
    const SplineECS::SplineHandle handle =
        svc.CreateSpline(Spline::SplineType::CatmullRom, false);
    auto* data = svc.GetSplineData(handle);
    for (const float32 x : {-40.0f, 0.0f, 40.0f})
        data->AddPoint({x, kRunGrade, z}, kRunHalfWidth);
    svc.RebuildCache(handle);

    auto e = world.CreateEntity();
    Components::SplineComponent comp{};
    comp.SplineDataIndex = handle.Index();
    comp.SplineDataGeneration = handle.Generation();
    world.AddComponentImmediate<Components::SplineComponent>(e, comp);
    world.AddComponentImmediate<Components::WorldTransform>(e, IdentityAt(0.0f, 0.0f, 0.0f));
    return e;
}

std::vector<float32> BakeWorld(ECS::World& world, TerrainService& svc, TerrainHandle handle)
{
    TerrainModifierSystem system;
    system.Update(world, 1.0f / 60.0f);
    auto* data = svc.GetTerrainData(handle);
    const float32* baked = data->Heightfield.GetRawSamples();
    std::vector<float32> out(baked, baked + data->Heightfield.GetSampleCount());
    return out;
}

} // namespace

// ---------------------------------------------------- what Auto resolves to --

TEST(SplineGroundAuthority, AutoReadsTheAnswerOffTheCrossSection)
{
    Components::SplineExtrude path{};
    path.Profile = Components::SplineExtrudeProfile::Bevel;
    EXPECT_EQ(Components::ResolveGroundAuthority(path), SplineGroundAuthority::Grades);

    Components::SplineExtrude road{};
    road.Profile = Components::SplineExtrudeProfile::Crown;
    EXPECT_EQ(Components::ResolveGroundAuthority(road), SplineGroundAuthority::Owns);

    // Water is a Bevel too, and the profile ALONE would have it grade the bed to
    // its own waterline — flattening the basin the fill needs in order to find
    // where the water reaches. WidthMode has to win, and this is why.
    Components::SplineExtrude water{};
    water.Profile = Components::SplineExtrudeProfile::Bevel;
    water.WidthMode = Components::SplineExtrudeWidthMode::FitToBanks;
    EXPECT_EQ(Components::ResolveGroundAuthority(water), SplineGroundAuthority::None)
        << "a filled water region READS the ground; grading it would remove the bed";

    // An explicit choice is never overridden, including back to None on a road.
    Components::SplineExtrude explicitNone{};
    explicitNone.Profile = Components::SplineExtrudeProfile::Crown;
    explicitNone.GroundAuthority = SplineGroundAuthority::None;
    EXPECT_EQ(Components::ResolveGroundAuthority(explicitNone), SplineGroundAuthority::None);
}

// A NEW profile must decide what it does to the ground. The switch in
// ResolveGroundAuthority falls through to None, which grades nothing and says
// nothing -- the one outcome an author cannot debug -- so this walks the
// REFLECTED enum table (what the scanner emits from the enum declaration) and
// fails by NAME on any profile the resolver has not been taught.
//
// The table is the forcing function a `Count` enumerator would have been. Count
// cannot be used here: SplineExtrudeProfile is reflected into the inspector's
// dropdown and a sentinel would offer itself there as a profile to pick.
TEST(SplineGroundAuthority, EveryDeclaredProfileDecidesWhatItDoesToTheGround)
{
    const ECS::ComponentTypeId typeId = ECS::GetComponentTypeId<Components::SplineExtrude>();
    const std::span<const ECS::FieldInfo> fields = ECS::ComponentFieldRegistry::Get(typeId);
    ASSERT_FALSE(fields.empty()) << "SplineExtrude carries no reflection; this gate is blind";

    const ECS::FieldInfo* profile = nullptr;
    for (const ECS::FieldInfo& f : fields)
        if (f.Name == "Profile")
            profile = &f;
    ASSERT_NE(profile, nullptr) << "SplineExtrude has no reflected Profile field";
    ASSERT_FALSE(profile->EnumNames.empty()) << "Profile carries no enum table to walk";

    EXPECT_EQ(static_cast<int32>(profile->EnumNames.size()),
              Components::kSplineExtrudeProfileCount)
        << "kSplineExtrudeProfileCount is hand-maintained and has drifted from the enum the "
           "build-time scanner actually saw. That drift is expected and is the point: adding an "
           "enumerator does not move the constant, so the static_assert beside it stays green and "
           "THIS is the gate. Bump the constant and teach ResolveGroundAuthority the new profile.";

    for (const ECS::EnumNameValue& entry : profile->EnumNames)
    {
        Components::SplineExtrude recipe{};
        recipe.Profile = static_cast<Components::SplineExtrudeProfile>(entry.Value);
        recipe.WidthMode = Components::SplineExtrudeWidthMode::Channel;
        const SplineGroundAuthority resolved = Components::ResolveGroundAuthority(recipe);

        if (entry.Name == "Bevel")
            EXPECT_EQ(resolved, SplineGroundAuthority::Grades) << "path grades";
        else if (entry.Name == "Crown")
            EXPECT_EQ(resolved, SplineGroundAuthority::Owns) << "road grades and owns";
        else
            ADD_FAILURE() << "SplineExtrudeProfile::" << entry.Name << " has no decided ground "
                             "authority. It currently resolves to " << static_cast<int>(resolved)
                          << " by falling through the switch, which is silent. Teach "
                             "ResolveGroundAuthority what this profile does to the terrain and "
                             "add it here.";
    }
}

// ------------------------------------------------------------- the parity --

TEST(SplineGroundAuthority, AProvisionedPathBakesTheSameGroundAsAHandAuthoredPooledFlatten)
{
    // ARM A: a path recipe and nothing else. Nobody opens the terrain effects.
    std::vector<float32> provisioned;
    {
        ScopedTerrainService terrainScope;
        ScopedSplineService splineScope;
        auto& terrainSvc = TerrainService::Get();
        const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
        ECS::World world;
        CreateTerrainEntity(world, handle);

        const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
        Components::SplineExtrude recipe{};
        recipe.Profile = Components::SplineExtrudeProfile::Bevel;
        world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

        Editor::SplineGroundAuthorityController controller;
        controller.Update(world, nullptr);
        provisioned = BakeWorld(world, terrainSvc, handle);
    }

    // ARM B: the volume and pooled flatten an author places by hand, at the
    // values the provisioning writes. Written out rather than read back off arm
    // A, so a provisioning that quietly changed a default has to move BOTH arms
    // or fail. RespectClaims is spelled out for that reason: the component
    // defaults it FALSE and a path provisions it TRUE.
    std::vector<float32> handAuthored;
    {
        ScopedTerrainService terrainScope;
        ScopedSplineService splineScope;
        auto& terrainSvc = TerrainService::Get();
        const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
        ECS::World world;
        CreateTerrainEntity(world, handle);

        const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
        Components::TerrainModifierVolume volume{};
        volume.Shape = Components::TerrainVolumeShape::SplinePath;
        volume.Falloff = Editor::kProvisionedRunFade;
        volume.Priority = Editor::kGradedRunPriority;
        world.AddComponentImmediate<Components::TerrainModifierVolume>(run, volume);

        Components::TerrainFlattenEffect flatten{};
        flatten.Blend = Components::TerrainModifierBlend::Average;
        flatten.UseVolumeHeight = true;
        flatten.TargetHeight = 0.0f;
        flatten.RespectClaims = true;
        // PoolGroup left blank: a path belongs to the SHARED pool.
        world.AddComponentImmediate<Components::TerrainFlattenEffect>(run, flatten);

        handAuthored = BakeWorld(world, terrainSvc, handle);
    }

    ASSERT_EQ(provisioned.size(), handAuthored.size());
    ASSERT_FALSE(provisioned.empty());
    EXPECT_EQ(0, std::memcmp(provisioned.data(), handAuthored.data(),
                             provisioned.size() * sizeof(float32)))
        << "a provisioned run must be the hand-authored one to the BIT, not merely close";

    // Both actually graded, so the agreement is not two untouched heightfields
    // matching. BaseSource::Flat means the bake refills the base to zero, so a
    // run that did nothing would leave 0 here rather than the run's height.
    const std::size_t centre =
        static_cast<std::size_t>(kHeightmapDim / 2) * kHeightmapDim + kHeightmapDim / 2;
    EXPECT_NEAR(provisioned[centre] * kHeightScale, kRunGrade, 1e-3f)
        << "the run must have graded the ground to its own height";
}

// ------------------------------------------------ what each authority writes --

TEST(SplineGroundAuthority, APathProvisionsAPooledFlattenInTheSharedPoolAndNoClaim)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Bevel;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr);

    const auto* volume = world.GetComponent<Components::TerrainModifierVolume>(run);
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(volume->Shape, Components::TerrainVolumeShape::SplinePath);
    EXPECT_FLOAT_EQ(volume->Priority, Editor::kGradedRunPriority);

    const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(flatten, nullptr);
    EXPECT_EQ(flatten->Blend, Components::TerrainModifierBlend::Average)
        << "the blend is what engages pooling at all: on any other blend the pool group and the "
           "claim flag below are fields nothing reads";
    EXPECT_TRUE(Components::EffectPoolName(*flatten).empty())
        << "a path belongs to the SHARED pool, which is the unnamed one — a generated name here "
           "would put every path in a system of its own and undo the blend-by-default slice";
    EXPECT_TRUE(flatten->UseVolumeHeight);
    EXPECT_TRUE(flatten->RespectClaims);
    EXPECT_EQ(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);
}

// A run saved with the retired Rectangle profile loads as a path (its Profile
// line is kept as text), and Auto on a path grades. What its author drew was a
// wall, so the controller leaves its ground alone: no volume, no flatten. The
// same run without the kept line provisions the path's flatten (the test above).
TEST(SplineGroundAuthority, ARunSavedAsTheRetiredWallProfileIsLeftUngraded)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    world.AddComponentImmediate<Components::SplineExtrude>(run, Components::SplineExtrude{});
    ECS::PreservedField kept;
    kept.Component = "SplineExtrude";
    kept.Field = "Profile";
    kept.RawText = "Rectangle";
    world.GetUnresolvedComponents().AddField(run, std::move(kept));
    ASSERT_TRUE(Editor::CarriesRetiredWallProfile(world, run));

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);
}

TEST(SplineGroundAuthority, ARoadProvisionsAClaimAndAPoolThatDoesNotDeferToIt)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr);

    const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(flatten, nullptr);
    EXPECT_EQ(flatten->Blend, Components::TerrainModifierBlend::Average);
    EXPECT_EQ(Components::EffectPoolName(*flatten),
              std::string_view(Editor::kOwnedRunGroupName));
    EXPECT_FALSE(flatten->RespectClaims)
        << "a road's pool reads ownership at its flush, by which time its OWN claim is in place; "
           "a road that deferred would hold itself off its own carriageway";
    EXPECT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);

    const auto* volume = world.GetComponent<Components::TerrainModifierVolume>(run);
    ASSERT_NE(volume, nullptr);
    EXPECT_LT(volume->Priority, Editor::kGradedRunPriority)
        << "a claim only holds ground against pools that flush AFTER it, so an owning run must "
           "sort below the graded runs that stop at it";
}

// ------------------------------------------------------ lifecycle contracts --

TEST(SplineGroundAuthority, SteadyStateWritesNothing)
{
    // The controller ticks every frame. A write it did not need would stamp the
    // effect's column, wake the terrain modifier system's change gate and re-bake
    // the run's region — every frame, forever. So the second sweep must be inert,
    // and the gather counter is the instrument that can tell.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& terrainSvc = TerrainService::Get();
    const TerrainHandle handle = CreateBakedTerrain(terrainSvc);
    ECS::World world;
    CreateTerrainEntity(world, handle);

    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::SplineGroundAuthorityController controller;
    TerrainModifierSystem system;
    controller.Update(world, nullptr);
    system.Update(world, 1.0f / 60.0f);
    system.Update(world, 1.0f / 60.0f); // settle any first-run gather

    const uint64 gathersBefore = TerrainModifierSystem::GetGatherCountForTests();
    for (int i = 0; i < 5; ++i)
    {
        controller.Update(world, nullptr);
        system.Update(world, 1.0f / 60.0f);
    }
    EXPECT_EQ(TerrainModifierSystem::GetGatherCountForTests(), gathersBefore)
        << "an unchanged recipe must not re-provision; each rewrite is a re-bake of the run";
}

TEST(SplineGroundAuthority, TurningTheAuthorityOffRemovesWhatItProvisioned)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr);
    ASSERT_NE(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::None;
    controller.Update(world, nullptr);

    EXPECT_EQ(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr)
        << "a volume with no effect still costs the gather a resolved spline every run";
}

TEST(SplineGroundAuthority, TurningAPathOffRemovesWhatItProvisioned)
{
    // The Grades arm of the same transition, separately, because a path carries
    // NO claim: the teardown asks the world to drop a component the entity never
    // held, and the Owns arm above cannot reach that case.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Bevel;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr);
    ASSERT_NE(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);
    ASSERT_EQ(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::None;
    controller.Update(world, nullptr);

    EXPECT_EQ(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
}

// ------------------------------------------------------ provenance contracts --

TEST(SplineGroundAuthority, ProvisioningRecordsWhichPiecesTheRecipeCreated)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr);

    const auto* marker = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(marker, nullptr) << "creation must leave a provenance ledger, or cleanup cannot "
                                  "tell the recipe's pieces from the author's";
    EXPECT_TRUE(marker->Flatten);
    EXPECT_TRUE(marker->Claim);
    EXPECT_TRUE(marker->Volume);
}

TEST(SplineGroundAuthority, HandAuthoredEffectsSurviveTheAuthorityTurningOff)
{
    // The trio an author placed by hand, adopted on first sight. Toggling the
    // authority off must remove NOTHING: the recipe created none of it, and
    // deleting authored work without undo is the defect this contract closes.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Bevel;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::SplinePath;
    volume.Falloff = 9.0f;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(run, volume);
    Components::TerrainFlattenEffect flatten{};
    flatten.Blend = Components::TerrainModifierBlend::Average;
    std::memcpy(flatten.PoolGroup, "Aqueduct", 8);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(run, flatten);
    world.AddComponentImmediate<Components::TerrainGroundClaimEffect>(
        run, Components::TerrainGroundClaimEffect{});

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr); // first sight: adopts

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::None;
    controller.Update(world, nullptr);

    const auto* after = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(after, nullptr) << "a hand-authored flatten was deleted by an authority toggle";
    EXPECT_EQ(Components::EffectPoolName(*after), std::string_view("Aqueduct"));
    EXPECT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);
    ASSERT_NE(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
    EXPECT_FLOAT_EQ(world.GetComponent<Components::TerrainModifierVolume>(run)->Falloff, 9.0f);
}

TEST(SplineGroundAuthority, RecipeCreatedCleanupIsUndoable)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::UndoRedoService undo;
    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, &undo);
    ASSERT_NE(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::None;
    controller.Update(world, &undo);

    EXPECT_EQ(world.GetComponent<Components::TerrainFlattenEffect>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
    EXPECT_EQ(world.GetComponent<Components::SplineGroundProvisioned>(run), nullptr)
        << "the ledger leaves with the pieces it records";
    ASSERT_TRUE(undo.CanUndo()) << "recipe-created cleanup must land in the undo journal";

    undo.Undo();

    const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(flatten, nullptr) << "undo must restore the removed pieces";
    EXPECT_EQ(flatten->Blend, Components::TerrainModifierBlend::Average);
    EXPECT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);
    EXPECT_NE(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
    const auto* marker = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(marker, nullptr) << "undo must restore the provenance ledger with the pieces";
    EXPECT_TRUE(marker->Flatten && marker->Claim && marker->Volume);
}

TEST(SplineGroundAuthority, GradesDoesNotRewriteAHandAuthoredFlattensBlend)
{
    // A recipe that leaves the ground alone first (authority None), so the
    // controller's record exists and the flatten added afterwards is
    // unambiguously the author's. The authority then moves to Grades: the recipe
    // provisions the volume it is missing, and must leave the author's flatten
    // exactly as written.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.GroundAuthority = SplineGroundAuthority::None;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr); // records authority None, provisions nothing

    Components::TerrainFlattenEffect flatten{};
    flatten.Blend = Components::TerrainModifierBlend::Set;
    flatten.TargetHeight = 42.0f;
    std::memcpy(flatten.PoolGroup, "Plaza", 5);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(run, flatten);

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::Grades;
    controller.Update(world, nullptr);

    const auto* after = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->Blend, Components::TerrainModifierBlend::Set)
        << "Grades rewrote a hand-authored flatten's blend";
    EXPECT_EQ(Components::EffectPoolName(*after), std::string_view("Plaza"));
    EXPECT_FLOAT_EQ(after->TargetHeight, 42.0f);

    // The recipe still provisions what is missing, and only that.
    const auto* marker = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(marker, nullptr);
    EXPECT_FALSE(marker->Flatten) << "the author's flatten must not be adopted into the ledger";
    EXPECT_TRUE(marker->Volume);
    ASSERT_NE(world.GetComponent<Components::TerrainModifierVolume>(run), nullptr);
}

TEST(SplineGroundAuthority, OwnsToGradesRemovesOnlyARecipeCreatedClaim)
{
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Crown;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    Editor::UndoRedoService undo;
    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, &undo);
    ASSERT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr);

    auto* live = world.GetComponentForWrite<Components::SplineExtrude>(run);
    ASSERT_NE(live, nullptr);
    live->GroundAuthority = SplineGroundAuthority::Grades;
    controller.Update(world, &undo);

    EXPECT_EQ(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr)
        << "the recipe-created claim comes off when the authority stops owning";
    const auto* flatten = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(flatten, nullptr);
    EXPECT_TRUE(flatten->RespectClaims) << "a graded run defers to claims again";
    EXPECT_TRUE(Components::EffectPoolName(*flatten).empty());
    const auto* marker = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(marker, nullptr);
    EXPECT_FALSE(marker->Claim);
    EXPECT_TRUE(marker->Flatten && marker->Volume);

    ASSERT_TRUE(undo.CanUndo());
    undo.Undo();
    EXPECT_NE(world.GetComponent<Components::TerrainGroundClaimEffect>(run), nullptr)
        << "undo must restore the claim";
    const auto* restored = world.GetComponent<Components::SplineGroundProvisioned>(run);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->Claim);
}

TEST(SplineGroundAuthority, AnExistingSceneIsAdoptedRatherThanRewritten)
{
    // A scene loads with its effects already on disk. The first sweep must record
    // what it finds and write nothing — a rewrite would stamp every run's column
    // on every scene open and re-bake the whole road network for nothing.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    ECS::World world;
    const ECS::EntityHandle run = CreateRun(world, SplineECS::SplineService::Get(), 0.0f);
    Components::SplineExtrude recipe{};
    recipe.Profile = Components::SplineExtrudeProfile::Bevel;
    world.AddComponentImmediate<Components::SplineExtrude>(run, recipe);

    // Authored state that does NOT match what provisioning would write: a tuned
    // pool group name and a fade the author widened.
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::SplinePath;
    volume.Falloff = 9.0f;
    volume.Priority = Editor::kGradedRunPriority;
    world.AddComponentImmediate<Components::TerrainModifierVolume>(run, volume);
    Components::TerrainFlattenEffect flatten{};
    flatten.Blend = Components::TerrainModifierBlend::Average;
    std::memcpy(flatten.PoolGroup, "Aqueduct", 8);
    world.AddComponentImmediate<Components::TerrainFlattenEffect>(run, flatten);

    Editor::SplineGroundAuthorityController controller;
    controller.Update(world, nullptr);

    const auto* after = world.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(Components::EffectPoolName(*after), std::string_view("Aqueduct"))
        << "adoption keeps what the author tuned; the inspector shows the real field, so what is "
           "displayed is always what is baking";
    EXPECT_FLOAT_EQ(world.GetComponent<Components::TerrainModifierVolume>(run)->Falloff, 9.0f);
}

TEST(SplineGroundAuthority, ASceneSwapAdoptsTheIncomingSceneAfterShutdown)
{
    // A scene swap clears the world and restarts entity versions, so a run in the
    // incoming scene can carry the very handle a run in the outgoing scene had.
    // After Shutdown the controller holds no record for that handle and adopts
    // the incoming run instead of rewriting it with the outgoing run's authority.
    ScopedTerrainService terrainScope;
    ScopedSplineService splineScope;
    auto& splines = SplineECS::SplineService::Get();
    Editor::SplineGroundAuthorityController controller;

    ECS::EntityHandle outgoingRun;
    {
        ECS::World outgoing;
        outgoingRun = CreateRun(outgoing, splines, 0.0f);
        Components::SplineExtrude road{};
        road.GroundAuthority = SplineGroundAuthority::Owns;
        outgoing.AddComponentImmediate<Components::SplineExtrude>(outgoingRun, road);
        controller.Update(outgoing, nullptr);
    }
    controller.Shutdown();

    ECS::World incoming;
    const ECS::EntityHandle run = CreateRun(incoming, splines, 0.0f);
    ASSERT_EQ(run, outgoingRun) << "the swap must recycle the outgoing run's handle for this "
                                   "test to reach the stale-record path";
    Components::SplineExtrude path{};
    path.GroundAuthority = SplineGroundAuthority::Grades;
    incoming.AddComponentImmediate<Components::SplineExtrude>(run, path);

    // The incoming run's saved, recipe-created pieces, with a pool group the
    // recipe would rewrite if it re-provisioned instead of adopting.
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::SplinePath;
    volume.Priority = Editor::kGradedRunPriority;
    incoming.AddComponentImmediate<Components::TerrainModifierVolume>(run, volume);
    Components::TerrainFlattenEffect flatten{};
    flatten.Blend = Components::TerrainModifierBlend::Average;
    std::memcpy(flatten.PoolGroup, "Aqueduct", 8);
    incoming.AddComponentImmediate<Components::TerrainFlattenEffect>(run, flatten);
    Components::SplineGroundProvisioned owned{};
    owned.Flatten = true;
    owned.Volume = true;
    incoming.AddComponentImmediate<Components::SplineGroundProvisioned>(run, owned);

    controller.Update(incoming, nullptr);

    const auto* after = incoming.GetComponent<Components::TerrainFlattenEffect>(run);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(Components::EffectPoolName(*after), std::string_view("Aqueduct"))
        << "a record surviving the swap made the incoming run look like an authority change "
           "and rewrote it on load";
}

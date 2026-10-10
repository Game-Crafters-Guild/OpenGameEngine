#include <gtest/gtest.h>

#include "Inspectors/ComponentPresets.h"

#include "Components/Rendering/Particles.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "ECS/ECS.h"
#include "ECS/ECSTemplates.h" // World::HasComponent definition
#include "ECS/World.h"
#include "TerrainECS/TerrainModifierComponents.h"

#include <algorithm>
#include <set>
#include <string>

// The Add Component preset vocabulary, and specifically the contract that makes
// a sixth canonical terrain effect a one-row change.
//
// Three consumers used to restate the five Terrain*Area enumerators as case
// labels — the add command, the undo name and the created-component list. The
// labels are gone; each consumer resolves the preset against the table first and
// answers from the row. Nothing in this file names a Terrain*Area enumerator
// either: every terrain expectation is folded from TerrainAreaPresets(), so a
// sixth row is covered here the moment it exists.
//
// What that leaves untested is stated rather than implied: AddPresetComponentsCommand
// lives in InspectorPanel.cpp, which no test target compiles. Its terrain branch
// is now three assignments and two notifications over ApplyTerrainAreaPreset,
// which is covered below; the notification wiring itself is editor-runtime only.
namespace
{
using namespace GameEngine;
using GameEngine::Editor::ComponentPreset;
using GameEngine::Editor::TerrainAreaPreset;

ECS::ComponentTypeId VolumeTypeId()
{
    return ECS::GetComponentTypeId<Components::TerrainModifierVolume>();
}

// Presets that must NOT resolve as area presets. TerrainSplineFlatten is the
// trap: it is a terrain preset that takes its region from a spline, so it is
// deliberately absent from the table and must keep its own answers.
constexpr ComponentPreset kNonAreaPresets[] = {
    ComponentPreset::Camera,
    ComponentPreset::Light,
    ComponentPreset::PhysicsDynamicBox,
    ComponentPreset::TerrainSplineFlatten,
    ComponentPreset::SplineComponent,
    ComponentPreset::ParticleCollisionEvents,
};

} // namespace

// ---------------------------------------------------------------------------
// The table against TerrainECS's canonical effect list — both directions.
// ---------------------------------------------------------------------------

TEST(ComponentPresetTable, EveryCanonicalEffectHasExactlyOnePreset)
{
    const auto presets = Editor::TerrainAreaPresets();
    const auto effects = TerrainECS::TerrainEffectTypes();

    ASSERT_EQ(presets.size(), effects.size());
    EXPECT_TRUE(Editor::TerrainAreaPresetsCoverCanonicalEffects());

    for (const TerrainECS::TerrainEffectTypeInfo& effect : effects)
    {
        const auto matches = std::count_if(presets.begin(), presets.end(),
                                           [&effect](const TerrainAreaPreset& area)
                                           { return area.EffectTypeId == effect.TypeId; });
        EXPECT_EQ(matches, 1) << "canonical effect '" << effect.Title
                              << "' needs exactly one Add Component preset row";
    }
}

TEST(ComponentPresetTable, EveryPresetRowNamesACanonicalEffect)
{
    for (const TerrainAreaPreset& area : Editor::TerrainAreaPresets())
    {
        EXPECT_TRUE(TerrainECS::IsTerrainEffectComponent(area.EffectTypeId))
            << "row '" << area.Label << "' pairs its preset with a type that is not a terrain effect";
    }
}

TEST(ComponentPresetTable, EveryRowResolvesFromItsPresetId)
{
    const auto presets = Editor::TerrainAreaPresets();
    for (const TerrainAreaPreset& area : presets)
    {
        const TerrainAreaPreset* found = Editor::FindTerrainAreaPreset(area.PresetId);
        ASSERT_NE(found, nullptr) << "row '" << area.Label << "' does not resolve from its own PresetId";
        EXPECT_EQ(found->EffectTypeId, area.EffectTypeId);
    }

    // One preset id per row: a duplicated id would make one row unreachable.
    std::set<ComponentPreset> ids;
    for (const TerrainAreaPreset& area : presets)
        EXPECT_TRUE(ids.insert(area.PresetId).second) << "duplicate PresetId for row '" << area.Label << "'";
}

TEST(ComponentPresetTable, NonAreaPresetsDoNotResolve)
{
    for (const ComponentPreset preset : kNonAreaPresets)
        EXPECT_EQ(Editor::FindTerrainAreaPreset(preset), nullptr);
}

// ---------------------------------------------------------------------------
// The two pure answers that used to carry case labels.
// ---------------------------------------------------------------------------

TEST(ComponentPresetAnswers, UndoNameForEveryAreaPresetComesFromItsRow)
{
    for (const TerrainAreaPreset& area : Editor::TerrainAreaPresets())
    {
        EXPECT_STREQ(Editor::PresetUndoDisplayName(area.PresetId), area.Label);
    }
}

TEST(ComponentPresetAnswers, ComponentIdsForEveryAreaPresetAreTheVolumePlusItsEffect)
{
    for (const TerrainAreaPreset& area : Editor::TerrainAreaPresets())
    {
        const std::vector<ECS::ComponentTypeId> ids = Editor::ComponentTypeIdsForPreset(area.PresetId);
        // A row whose effect needs a ROUTE also reports the spline that defines
        // it, because that is what the preset creates. Folded from the row
        // rather than special-cased by name, so a second such effect is covered
        // without touching this test — and a row that reports a spline it does
        // not create still fails.
        std::vector<ECS::ComponentTypeId> expected;
        if (area.RequiresSplineRoute)
            expected.push_back(ECS::GetComponentTypeId<Components::SplineComponent>());
        expected.push_back(VolumeTypeId());
        expected.push_back(area.EffectTypeId);
        EXPECT_EQ(ids, expected) << "preset '" << area.Label << "' must report the volume, its own "
                                    "effect, and a spline exactly when it needs a route";
    }
}

TEST(ComponentPresetAnswers, SplineFlattenKeepsItsOwnAnswersAndIsNotTableDriven)
{
    EXPECT_STREQ(Editor::PresetUndoDisplayName(ComponentPreset::TerrainSplineFlatten),
                 "Terrain: Spline Flatten");

    const std::vector<ECS::ComponentTypeId> ids =
        Editor::ComponentTypeIdsForPreset(ComponentPreset::TerrainSplineFlatten);
    EXPECT_EQ(ids.size(), 3u) << "spline flatten creates a spline, a volume and the flatten effect";
    EXPECT_NE(std::find(ids.begin(), ids.end(), VolumeTypeId()), ids.end());
}

// ---------------------------------------------------------------------------
// The catalog: what the picker offers must be exactly what the answers cover.
//
// This is the guard that a sixth effect inherits for free. Its row reaches the
// catalog through the same fold, and both assertions below then demand that the
// undo name and the component list answer for it — which only the table lookup
// can do, because no case label exists for a preset nobody has written one for.
// ---------------------------------------------------------------------------

TEST(ComponentPresetCatalogTests, EveryOfferedPresetHasANameAndBuildsSomething)
{
    const std::vector<Editor::ComponentPresetEntry>& catalog = Editor::ComponentPresetCatalog();
    ASSERT_FALSE(catalog.empty());

    for (const Editor::ComponentPresetEntry& entry : catalog)
    {
        const char* name = Editor::PresetUndoDisplayName(entry.Preset);
        EXPECT_STRNE(name, "Component")
            << "picker offers '" << entry.Label << "' but nothing names it — it would undo as \"Component\"";

        EXPECT_FALSE(Editor::ComponentTypeIdsForPreset(entry.Preset).empty())
            << "picker offers '" << entry.Label << "' but nothing reports what it creates";
    }
}

TEST(ComponentPresetCatalogTests, TerrainAreaRowsAreListedExactlyOnceUnderTerrain)
{
    const std::vector<Editor::ComponentPresetEntry>& catalog = Editor::ComponentPresetCatalog();

    for (const TerrainAreaPreset& area : Editor::TerrainAreaPresets())
    {
        const auto listed = std::count_if(catalog.begin(), catalog.end(),
                                          [&area](const Editor::ComponentPresetEntry& entry)
                                          { return entry.Preset == area.PresetId; });
        EXPECT_EQ(listed, 1) << "row '" << area.Label << "' must appear once in the Add Component picker";

        const auto found = std::find_if(catalog.begin(), catalog.end(),
                                        [&area](const Editor::ComponentPresetEntry& entry)
                                        { return entry.Preset == area.PresetId; });
        ASSERT_NE(found, catalog.end());
        EXPECT_EQ(found->Category, "Terrain");
        EXPECT_EQ(found->Label, std::string(area.Label));
    }
}

TEST(ComponentPresetCatalogTests, NoPresetIsOfferedTwice)
{
    std::set<ComponentPreset> seen;
    for (const Editor::ComponentPresetEntry& entry : Editor::ComponentPresetCatalog())
        EXPECT_TRUE(seen.insert(entry.Preset).second) << "preset offered twice as '" << entry.Label << "'";
}

// ---------------------------------------------------------------------------
// Applying a row: what the add command delegates to, and what its undo removes.
//
// Presence of the effect is probed by type id, never by naming a component type:
// AddTerrainEffectDefault refuses a type the entity already carries, and
// NextEffectStackOrder counts what is there. Both stay correct for a sixth row.
// ---------------------------------------------------------------------------

TEST(ComponentPresetApply, EveryRowAddsTheVolumeAndItsOwnEffect)
{
    for (const TerrainAreaPreset& area : Editor::TerrainAreaPresets())
    {
        ECS::World world;
        const ECS::EntityHandle entity = world.CreateEntity();

        const Editor::TerrainAreaPresetAdditions added =
            Editor::ApplyTerrainAreaPreset(world, entity, area);

        EXPECT_TRUE(added.AddedVolume) << "row '" << area.Label << "' must create the region volume";
        EXPECT_EQ(added.AddedEffectTypeId, area.EffectTypeId);
        EXPECT_TRUE(world.HasComponent<Components::TerrainModifierVolume>(entity));

        // The effect is really on the entity: a second default-add of the same
        // type is refused, and the stack has exactly one occupant.
        EXPECT_FALSE(TerrainECS::AddTerrainEffectDefault(world, entity, area.EffectTypeId, 0));
        EXPECT_EQ(TerrainECS::NextEffectStackOrder(world, entity), 1);
    }
}

TEST(ComponentPresetApply, ReapplyingTheSameRowAddsNothingForUndoToRemove)
{
    for (const TerrainAreaPreset& area : Editor::TerrainAreaPresets())
    {
        ECS::World world;
        const ECS::EntityHandle entity = world.CreateEntity();

        Editor::ApplyTerrainAreaPreset(world, entity, area);
        const Editor::TerrainAreaPresetAdditions again =
            Editor::ApplyTerrainAreaPreset(world, entity, area);

        EXPECT_FALSE(again.AddedVolume) << "row '" << area.Label << "' re-reported a volume it did not add";
        EXPECT_EQ(again.AddedEffectTypeId, 0u) << "row '" << area.Label << "' re-reported an effect it did not add";
    }
}

TEST(ComponentPresetApply, ASecondRowStacksOnTheVolumeAlreadyThere)
{
    const auto presets = Editor::TerrainAreaPresets();
    ASSERT_GE(presets.size(), 2u);

    ECS::World world;
    const ECS::EntityHandle entity = world.CreateEntity();

    const Editor::TerrainAreaPresetAdditions first =
        Editor::ApplyTerrainAreaPreset(world, entity, presets[0]);
    const Editor::TerrainAreaPresetAdditions second =
        Editor::ApplyTerrainAreaPreset(world, entity, presets[1]);

    EXPECT_TRUE(first.AddedVolume);
    EXPECT_FALSE(second.AddedVolume) << "the region volume is shared; only the first row creates it";
    EXPECT_EQ(second.AddedEffectTypeId, presets[1].EffectTypeId);
    EXPECT_EQ(TerrainECS::NextEffectStackOrder(world, entity), 2);
}

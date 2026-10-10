#include "Inspectors/TerrainVolumeNotices.h"

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
// The terrain EFFECT components have no compiled reflection TU (see
// TerrainModifierEffects.h), so no translation unit instantiates World's
// accessors for them. This header is the codebase's user-side instantiation
// path; without it GetComponent<TerrainStampEffect> is an unresolved external.
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/InspectorNotice.h"

#include <memory>

namespace GameEngine::Editor
{
namespace
{

bool IsGlobalVolume(ECS::World& world, ECS::EntityHandle entity)
{
    const auto* vol = world.GetComponent<Components::TerrainModifierVolume>(entity);
    return vol && vol->Shape == Components::TerrainVolumeShape::Global;
}

} // namespace

void AddGlobalVolumeScopeNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity,
                                bool anySphericalTerrain)
{
    if (!parent || !IsGlobalVolume(world, entity))
        return;

    // Says nothing about strength: Weight still multiplies into every effect
    // (ComputeWeight returns it for a global volume), and its row is right below.
    InspectorUI::AddInfoCard(parent,
                             "Region is every terrain - no edge, no falloff. Moving or resizing "
                             "this volume does not change what it covers.");

    if (anySphericalTerrain)
    {
        parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
            "Global shapes are not baked on spherical (planet) terrains yet - this volume "
            "affects planar / tiled terrains only."));
    }
}

void AddStampInGlobalVolumeNotice(UIElement* parent, ECS::World& world, ECS::EntityHandle entity)
{
    if (!parent || !IsGlobalVolume(world, entity))
        return;

    const auto* stamp = world.GetComponent<Components::TerrainStampEffect>(entity);
    if (!stamp || !stamp->Enabled)
        return;

    parent->AddChild(std::make_unique<EditorUI::InspectorNotice>(
        "The Stamp effect on this volume is ignored: a stamp projects its mask over a region's "
        "footprint, and a Global volume has none. Pick a Circle or Rectangle shape to use the "
        "stamp, or a Height Offset / Noise effect to change the whole world."));
}

} // namespace GameEngine::Editor

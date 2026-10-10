#pragma once

// The Add Component preset vocabulary: which component sets the Inspector's Add
// Component picker offers, what each is called, and which components each one
// creates. One owner for all four answers, so the picker cannot list a preset
// the command does not build, and no consumer restates the set.

#include "ECS/ECS.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// One component set the Add Component picker can create. The ordinal is the
// context-menu command id the panel assigns (kCmdInspectorAddPresetBase +
// ordinal); the menu and its dispatch derive it from this enum in the same
// frame, so nothing outside a session depends on a given entry's number. Append
// rather than reorder anyway — the one exception is the Terrain*Area run, which
// a static_assert requires to stay contiguous, so a new terrain effect's preset
// is INSERTED at the end of that run.
enum class ComponentPreset : uint8_t
{
    Camera = 0,
    Light,
    SkyEnvironment,
    Skybox,
    PhysicsDynamicBox,
    PhysicsDynamicSphere,
    PhysicsStaticPlane,
    PhysicsStaticBox,
    AudioEmitter,
    AudioListener,
    PostProcessVolume,
    WindVolume,
    // One per canonical terrain effect (TerrainAreaPresets()), plus the
    // spline-shaped flatten. Each adds a TerrainModifierVolume for the region
    // together with its effect — the region lives on the volume, so no effect
    // ever asks the user for a shape.
    TerrainFlattenArea,
    TerrainHeightOffsetArea,
    TerrainNoiseArea,
    TerrainStampArea,
    TerrainPaintArea,
    TerrainRulesArea,
    TerrainGroundClaimArea,
    TerrainGrassArea,
    TerrainSplineFlatten,
    SplineComponent,
    ValueCurve,
    VideoTexture,
    // Post-process effects are NOT presets: they add only through the Volume
    // inspector's Add Post FX picker (AddVolumeEffectCommand), driven by the
    // PostProcessEffectRegistry. Terrain effects likewise add only through the
    // modifier volume's Add Effect picker.
    ParticleEmitter2D,
    ParticleEmitter3D,
    ParticleCollisionEvents,
    PhysicsCharacterController,

    // Not a preset — bounds the command-id ordinal check (TryGetAddPreset).
    // Append new presets ABOVE this line.
    Count,
};

// One canonical terrain effect paired with the preset that creates a volume
// carrying it. This is the only place preset labels and effect types are
// paired.
struct TerrainAreaPreset
{
    ComponentPreset PresetId;
    const char* Label;
    ECS::ComponentTypeId EffectTypeId;
    /// The effect needs a ROUTE, so the preset shapes its volume as a spline
    /// path and gives the entity the SplineComponent that defines it. Without
    /// this the preset would create a circle volume with no route to follow, and
    /// the picker would produce a warning instead of a graded run.
    ///
    /// No canonical effect requires one today — every effect is defined on every
    /// shape — so this is currently false on every row. It stays because the
    /// constraint belongs to the row rather than to a named preset: an effect
    /// that does need a route sets it here and reaches ApplyTerrainAreaPreset,
    /// ComponentTypeIdsForPreset and their tests without touching any of them.
    bool RequiresSplineRoute = false;
};

/// One row per canonical terrain effect, in TerrainECS ModifierEffectComponents
/// order.
std::span<const TerrainAreaPreset> TerrainAreaPresets();

/// The area preset row for `p`, or null when `p` is not one.
///
/// Every consumer that would otherwise name the Terrain*Area enumerators asks
/// this instead: a sixth canonical effect reaches the picker, the add command,
/// the undo name and the created-component list from its table row alone.
const TerrainAreaPreset* FindTerrainAreaPreset(ComponentPreset p);

/// True when the table and TerrainECS's canonical effect list cover each other.
/// The count is a static_assert in the implementation; identity needs runtime
/// type ids, so an effect that was replaced or reordered rather than added is
/// only visible here.
bool TerrainAreaPresetsCoverCanonicalEffects();

// What ApplyTerrainAreaPreset added, so the caller's undo removes exactly that.
// False / 0 means the entity already carried it.
struct TerrainAreaPresetAdditions
{
    bool AddedVolume = false;
    bool AddedSpline = false;
    ECS::ComponentTypeId AddedEffectTypeId = 0;
};

/// Add the modifier volume that owns the region, then `area`'s effect stacked
/// above any effects already on the entity.
TerrainAreaPresetAdditions ApplyTerrainAreaPreset(ECS::World& world, ECS::EntityHandle entity,
                                                  const TerrainAreaPreset& area);

/// The undo-stack and menu name for `p`.
const char* PresetUndoDisplayName(ComponentPreset p);

/// The components `p` creates — what the picker draws its icon from and what
/// the inspector scrolls to after an add.
std::vector<ECS::ComponentTypeId> ComponentTypeIdsForPreset(ComponentPreset p);

// One picker row.
struct ComponentPresetEntry
{
    std::string Label;
    std::string Category;
    ComponentPreset Preset;
    // Menu/picker row image ("editor:" path), the same icon the component's
    // entity wears elsewhere in the editor.
    std::string Icon;
};

/// Single catalog for the Add Component picker and the inspector header context
/// menus, sorted by (category, label).
const std::vector<ComponentPresetEntry>& ComponentPresetCatalog();

} // namespace GameEngine::Editor

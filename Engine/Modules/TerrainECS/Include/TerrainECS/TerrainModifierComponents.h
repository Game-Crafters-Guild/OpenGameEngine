#pragma once

#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "ECS/ECS.h"

#include <cstddef>
#include <span>
#include <string_view>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::TerrainECS
{

// The ECS component types TerrainModifierSystem's gather reads, and the single
// place they are enumerated. Consumers that must stay in lockstep with the
// gather fold over these lists rather than restating them — the change gate's
// wake scans, and the lifecycle-event subscription below.
//
// The coupling is silent in the direction that matters: a component the gather
// reads but a consumer omits produces no error and no log, just a region that
// bakes once and then goes deaf to its own edits. Adding a component to the
// gather means adding it here.

template <typename... Ts>
struct ComponentTypeList
{
    static constexpr std::size_t Size = sizeof...(Ts);
};

// Types the gather queries directly as a modifier root. The entity's
// WorldTransform is part of a root's input, so moving a root is an edit to it.
using ModifierRootComponents = ComponentTypeList<
    Components::TerrainModifierVolume,
    Components::TerrainSculptZone,
    Components::TerrainPaintZone>;

// Types the gather reads as passengers on a volume entity: the effect stack.
// The region and the transform belong to the volume, so an effect's own column
// is the only part of it that can change independently.
using ModifierEffectComponents = ComponentTypeList<
    Components::TerrainFlattenEffect,
    Components::TerrainHeightOffsetEffect,
    Components::TerrainNoiseEffect,
    Components::TerrainStampEffect,
    Components::TerrainPaintLayerEffect,
    Components::TerrainSurfaceRulesEffect,
    Components::TerrainGroundClaimEffect,
    Components::TerrainGrassEffect>;

// Subscribe a world to the Added/Removed events the modifier change gate
// consumes. Removed<T> is the only detector for a component remove or an entity
// destroy — the entity leaves the archetype, so no Changed<T> scan can see it.
//
// Only worlds whose lifecycle swap is driven by the engine tick subscribe;
// thumbnail/preview worlds stay unsubscribed so recording there stays a no-op.
void EnableTerrainModifierLifecycleEvents(ECS::World& world);

// ---- ModifierEffectComponents, folded for consumers outside the gather ------
//
// The gather folds the list above directly. Everything else — the scene
// loader's migration, the editor's Add Effect picker, the Add Component
// picker's exclusion, the inspector's effect-nesting rule, section order and
// section titles, and the drag-reorder's stack renumbering — needs the same set
// as runtime values, and each hand-restated copy is a divergence waiting to
// happen. These fold the one list instead.

// Number of canonical effect types, read off the list itself. Compile-time, so
// a consumer that must carry one entry per effect (the editor's Add Component
// presets) can static_assert against it and break the build when the list grows.
inline constexpr std::size_t kTerrainEffectTypeCount = ModifierEffectComponents::Size;

// Authoring chrome for one effect type: what a picker shows for it. Specialized
// per effect below — an effect added to ModifierEffectComponents without a
// specialization fails to compile, which is what keeps list and chrome in step.
// (PostProcessEffectDescriptor plays this role for the post-process stack.)
template <typename TEffect>
struct TerrainEffectChrome;

template <>
struct TerrainEffectChrome<Components::TerrainFlattenEffect>
{
    static constexpr std::string_view Title = "Flatten";
    static constexpr std::string_view Description =
        "Levels the terrain toward a target height - building sites, road beds, landing pads.";
};

template <>
struct TerrainEffectChrome<Components::TerrainHeightOffsetEffect>
{
    static constexpr std::string_view Title = "Height Offset";
    static constexpr std::string_view Description =
        "Raises or lowers the terrain by a constant - sunken river beds, plateaus, embankments.";
};

template <>
struct TerrainEffectChrome<Components::TerrainNoiseEffect>
{
    static constexpr std::string_view Title = "Noise";
    static constexpr std::string_view Description =
        "Adds fBM noise displacement, with an optional erosion pass - detail and roughness.";
};

template <>
struct TerrainEffectChrome<Components::TerrainStampEffect>
{
    static constexpr std::string_view Title = "Stamp";
    static constexpr std::string_view Description =
        "Projects a height texture over the region - craters, cliff details, sculpt data.";
};

template <>
struct TerrainEffectChrome<Components::TerrainPaintLayerEffect>
{
    static constexpr std::string_view Title = "Paint Layer";
    static constexpr std::string_view Description =
        "Paints a material layer into the splatmap - rock on cliffs, dirt on paths.";
};

template <>
struct TerrainEffectChrome<Components::TerrainSurfaceRulesEffect>
{
    static constexpr std::string_view Title = "Surface Rules";
    static constexpr std::string_view Description =
        "Assigns materials by slope, height and noise bands - grass low, rock steep, snow high.";
};

template <>
struct TerrainEffectChrome<Components::TerrainGroundClaimEffect>
{
    static constexpr std::string_view Title = "Ground Claim";
    static constexpr std::string_view Description =
        "Marks ground this region owns, so pooled flattens grade up to it and stop. Writes no height.";
};

template <>
struct TerrainEffectChrome<Components::TerrainGrassEffect>
{
    static constexpr std::string_view Title = "Grass";
    static constexpr std::string_view Description =
        "Shortens or thins planar grass relative to its global profile, with the region's falloff.";
};

// One canonical effect type as runtime data.
struct TerrainEffectTypeInfo
{
    ECS::ComponentTypeId TypeId = 0;
    std::string_view Title;
    std::string_view Description;
};

// The canonical effect types, in ModifierEffectComponents order.
std::span<const TerrainEffectTypeInfo> TerrainEffectTypes();

// The canonical entry for `typeId`, or null when it is not an effect component.
const TerrainEffectTypeInfo* FindTerrainEffectType(ECS::ComponentTypeId typeId);

// True for the effect components a TerrainModifierVolume owns as its stack.
bool IsTerrainEffectComponent(ECS::ComponentTypeId typeId);

// The stack position for the next effect added to this entity: one past the
// highest already there, so effects keep the order they arrived in. Shared by
// the loader's migration and the editor's Add Effect so both agree.
int32 NextEffectStackOrder(const ECS::World& world, ECS::EntityHandle entity);

// Default-construct the effect `typeId` names onto the entity at `stackOrder`.
// False when the type is not an effect, or the entity already carries it.
//
// The editor picks an effect by type id and has no static type to add through,
// and ECS::ComponentFactory is not that path: the effects have no reflection at
// all (the build-time ComponentScanner cannot expand their shared-field macro,
// and no hand-written reflection TU stands in), so they have no factory entry.
// Folding the canonical list is
// both the registration point this header already owns and the only thing that
// works today.
bool AddTerrainEffectDefault(ECS::World& world, ECS::EntityHandle entity,
                             ECS::ComponentTypeId typeId, int32 stackOrder);

// Write `stackOrder` into the StackOrder of the effect `typeId` names on the
// entity. False when the type is not an effect, or the entity does not carry
// one — the same "nothing was written" answer AddTerrainEffectDefault gives.
//
// The inspector's drag-reorder renumbers a whole stack by type id and, like Add
// Effect, has no static type to write through. The write is a write-stamp: it
// marks the effect's column, which is what the modifier change gate scans.
bool SetTerrainEffectStackOrder(ECS::World& world, ECS::EntityHandle entity,
                                ECS::ComponentTypeId typeId, int32 stackOrder);

} // namespace GameEngine::TerrainECS

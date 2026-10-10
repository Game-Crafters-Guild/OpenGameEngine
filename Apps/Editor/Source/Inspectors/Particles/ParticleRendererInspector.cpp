#include "Inspectors/Particles/ParticleRendererInspector.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/ParticleStackDocument.h"
#include "Particles/Processors/ParticleTrailProcessor.h"
#include "UI/Controls/Foldout.h"

#include <array>
#include <memory>
#include <span>
#include <string_view>

namespace GameEngine::ParticleInspectors
{
namespace
{
using Components::ParticleLightingMode;
using Components::ParticleRenderer;

constexpr std::array<std::string_view, 9> kMaterialFields = {
    "Material",
    "Texture",
    "Columns",
    "Rows",
    "FrameCount",
    "FrameRate",
    "StartFrame",
    "Loop",
    "BlendFrames",
};
constexpr std::array<std::string_view, 1> kLightingFields = {"Lighting"};
constexpr std::array<std::string_view, 4> kSixWayFields = {"SixWayMapA", "SixWayMapB", "SixWayLayout", "SixWayContrast"};
constexpr std::array<std::string_view, 3> kEmissionFields = {"EmissionTexture", "EmissionColor", "EmissionIntensity"};
constexpr std::array<std::string_view, 4> kOrientationFields = {"Billboard", "InheritScale", "VelocityStretch",
                                                                "DrawOrder"};
constexpr std::array<std::string_view, 1> kMeshFields = {"Meshes"};
constexpr std::array<std::string_view, 10> kTrailFields = {
    "TrailOnly",
    "TrailInheritColor",
    "TrailSizeAffectsWidth",
    "TrailFadeOverLength",
    "TrailWidthMin",
    "TrailWidthMax",
    "TrailAlphaPeak",
    "TrailTextureTile",
    "TrailTextureScaleU",
    "TrailTextureScaleV",
};
constexpr std::array<std::string_view, 3> kVisibilityFields = {
    "RenderLayerMask",
    "ThinningStart",
    "ThinningEnd",
};

// The renderer values that change which rows the inspector shows.
struct RendererLayout
{
    ParticleLightingMode Lighting = ParticleLightingMode::Unlit;
    bool HasBothSixWayMaps = false;
    bool Thins = false;

    bool operator==(const RendererLayout&) const = default;
};

RendererLayout LayoutOf(const ParticleRenderer& renderer)
{
    return {renderer.Lighting, !renderer.SixWayMapA.IsNull() && !renderer.SixWayMapB.IsNull(),
            renderer.ThinningEnd > 0.0f};
}

// Whether the stack the entity's emitter runs records trails. True when that cannot be told yet
// (its stack asset is still loading), so the trail rows stay reachable.
bool EmitterRecordsTrails(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* emitter = world.GetComponent<Components::ParticleEmitter3D>(entity);
    if (!emitter || emitter->Stack.IsNull())
        return false;
    auto* assets = EngineCore::GetInstance().TryGetAssetManager();
    const auto asset =
        assets ? std::dynamic_pointer_cast<Particles::ParticleStackAsset>(assets->GetAsset(emitter->Stack.ToGuid()))
               : nullptr;
    if (!asset || !asset->Document())
        return true;
    for (const auto& phase : asset->Document()->Phases)
        for (const auto& processor : phase.Processors)
            if (processor.Descriptor == &Particles::ParticleTrailProcessor())
                return true;
    return false;
}

// A section of the reflected rows `fields` names; returns its content, for a note under the rows.
UIElement* AddFieldSection(const InspectorContext& context, std::string_view key, std::string_view title,
                           std::span<const std::string_view> fields)
{
    auto* section = InspectorUI::AddComponentSection(context.Parent, key, title);
    InspectorContext rows = context;
    rows.Parent = section->GetContentContainer();
    RenderReflectedFieldRows(rows, ECS::GetComponentTypeId<ParticleRenderer>(), fields);
    return rows.Parent;
}

void AddLightingSection(const InspectorContext& context, const ParticleRenderer& renderer)
{
    auto* section = InspectorUI::AddComponentSection(context.Parent, "ParticleRenderer/Shading", "Shading");
    InspectorContext rows = context;
    rows.Parent = section->GetContentContainer();
    const auto typeId = ECS::GetComponentTypeId<ParticleRenderer>();
    RenderReflectedFieldRows(rows, typeId, kLightingFields);
    if (renderer.Lighting == ParticleLightingMode::SixWay)
    {
        RenderReflectedFieldRows(rows, typeId, kSixWayFields);
        if (!LayoutOf(renderer).HasBothSixWayMaps)
            InspectorUI::AddInfoCard(rows.Parent, "Six-way lighting needs both response maps. The particles are lit "
                                                  "as Lit until both are assigned.");
    }
    RenderReflectedFieldRows(rows, typeId, kEmissionFields);
}

// Rebuilds the inspector when a value that decides which rows it shows changes.
void RefreshWhenLayoutChanges(const InspectorContext& context, const ParticleRenderer& renderer)
{
    if (!context.SimulationRefreshCallbacks || !context.RequestInspectorRefresh)
        return;
    context.SimulationRefreshCallbacks->push_back(
        [world = context.World, entity = context.Entity, built = LayoutOf(renderer),
         refresh = context.RequestInspectorRefresh, requested = false]() mutable
        {
            const auto* current = world ? world->GetComponent<ParticleRenderer>(entity) : nullptr;
            if (requested || !current || LayoutOf(*current) == built)
                return;
            requested = true;
            refresh();
        });
}

void BuildParticleRendererInspector(const InspectorContext& context)
{
    if (!context.Parent || !context.World || !context.Entity.IsValid())
        return;
    const auto* current = context.World->GetComponent<ParticleRenderer>(context.Entity);
    if (!current)
        return;
    const ParticleRenderer renderer = *current;
    if (!context.World->HasComponent<Components::ParticleEmitter3D>(context.Entity))
        InspectorUI::AddInfoCard(context.Parent, "Draws the particles of a Particle Emitter on this entity, and this "
                                                 "entity has none.");
    AddFieldSection(context, "ParticleRenderer/Material", "Material and Texture Sheet", kMaterialFields);
    AddLightingSection(context, renderer);
    AddFieldSection(context, "ParticleRenderer/Orientation", "Orientation", kOrientationFields);
    AddFieldSection(context, "ParticleRenderer/Meshes", "Meshes", kMeshFields);
    if (EmitterRecordsTrails(*context.World, context.Entity))
        AddFieldSection(context, "ParticleRenderer/Trails", "Trails", kTrailFields);
    auto* visibility = AddFieldSection(context, "ParticleRenderer/Visibility", "Visibility", kVisibilityFields);
    if (!LayoutOf(renderer).Thins)
        InspectorUI::AddTextBlock(visibility, "Thinning: off. Set Thinning End above 0 to thin the particles out with "
                                              "distance.");
    RefreshWhenLayoutChanges(context, renderer);
}
} // namespace

void RegisterParticleRendererInspector()
{
    InspectorRegistry::Get().RegisterComponentInspector<ParticleRenderer>(BuildParticleRendererInspector);
}

} // namespace GameEngine::ParticleInspectors

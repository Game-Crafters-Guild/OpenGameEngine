#include "Inspectors/Particles/ParticleInspector.h"

#include "Assets/AssetManager.h"
#include "Components/Rendering/ParticleRenderer.h"
#include "Components/Rendering/Particles.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "InspectorRegistry.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/Particles/Controls/ParticlePreviewControls.h"
#include "Inspectors/Particles/ParticleEmitterStackSection.h"
#include "Inspectors/Particles/ParticleRendererInspector.h"
#include "Inspectors/Particles/ParticleStackInspector.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/ParticleRuntime.h"
#include "Particles/ParticleStackDocument.h"
#include "SceneView/Gizmos/ParticleEmitterGizmo.h"
#include "UI/Controls/Foldout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{
namespace
{
using Components::ParticleEmitter3D;

constexpr std::array<std::string_view, 9> kSimulationFields = {
    "Emitting",
    "Amount",
    "PrewarmSeconds",
    "SimulationSpeed",
    "Dimension",
    "LocalSpace",
    "TicksPerSecond",
    "Interpolate",
    "Seed",
};

void AssignSubEmitter(const InspectorContext& context, uint32 slot, ECS::EntityHandle child)
{
    InspectorDrag::CommitComponentsWithUndo<ParticleEmitter3D>(
        context.World, context.Entity, InspectorDrag::GetAdditionalEntities(context), context.ChangeNotifications,
        context.Undo, "Change Particle Sub-emitter",
        [slot, child](ParticleEmitter3D& emitter)
        { emitter.SubEmitters[slot].Emitter = child; });
}

// The child emitters the stack's spawn rules name by slot ("Sub-emitter 1" to "Sub-emitter 4").
void AddSubEmitterSection(const InspectorContext& context, const ParticleEmitter3D& emitter)
{
    auto* section = InspectorUI::AddComponentSection(context.Parent, "ParticleEmitter/SubEmitters", "Sub-emitters");
    auto* content = section->GetContentContainer();
    const std::vector<ECS::ComponentTypeId> emitters{ECS::GetComponentTypeId<ParticleEmitter3D>()};
    for (uint32 slot = 0; slot < Components::ParticleMaxSubEmitters; ++slot)
        InspectorUI::AddEntityFieldRow(
            content, "Sub-emitter " + std::to_string(slot + 1), emitter.SubEmitters[slot].Emitter, context.World,
            [context, slot](ECS::EntityHandle child)
            { AssignSubEmitter(context, slot, child); }, emitters,
            "The emitter a spawn rule naming this slot spawns its particles in");
}

// The stack the emitter runs, when it is loaded; the default stack when it names none.
std::shared_ptr<const Particles::CompiledParticleStack> RunningStack(const ParticleEmitter3D& emitter)
{
    if (emitter.Stack.IsNull())
        return nullptr;
    auto* assets = EngineCore::GetInstance().TryGetAssetManager();
    const auto asset =
        assets ? std::dynamic_pointer_cast<Particles::ParticleStackAsset>(assets->GetAsset(emitter.Stack.ToGuid())) : nullptr;
    return asset ? asset->Compiled() : nullptr;
}

// What the running stack's continuous emission keeps alive (its rate times its longest lifetime), capped
// one above what an emitter holds; 0 while no stack is loaded.
uint32 KeptAlive(const ParticleEmitter3D& emitter)
{
    const auto stack = RunningStack(emitter);
    if (!stack || !stack->Document)
        return 0;
    const double peak = std::ceil(static_cast<double>(Particles::ContinuousEmissionPeak(*stack->Document)));
    return static_cast<uint32>(std::min(peak, static_cast<double>(Particles::kMaxParticlesPerEmitter) + 1.0));
}

// The Amount card's content: the kept-alive count when Amount is below it, 0 when there is no card.
uint32 AmountShortfall(const ParticleEmitter3D& emitter)
{
    const uint32 kept = KeptAlive(emitter);
    return emitter.Amount < kept ? kept : 0u;
}

// An Amount below what the stack's continuous emission keeps alive skips spawns without a word;
// say so under the Amount it concerns.
void AddAmountWarning(UIElement* parent, const ParticleEmitter3D& emitter)
{
    const uint32 kept = AmountShortfall(emitter);
    if (kept == 0)
        return;
    const std::string amount = std::to_string(emitter.Amount);
    if (kept > Particles::kMaxParticlesPerEmitter)
    {
        InspectorUI::AddInfoCard(parent, "The stack keeps more particles alive than an emitter holds (" +
                                             std::to_string(Particles::kMaxParticlesPerEmitter) +
                                             "), and Amount is " + amount +
                                             ": spawns beyond it are skipped. Lower the stack's emission rate or "
                                             "lifetime.");
        return;
    }
    InspectorUI::AddInfoCard(parent, "The stack keeps up to " + std::to_string(kept) +
                                         " particles alive (its emission rate times the longest lifetime), and "
                                         "Amount is " + amount + ": spawns beyond it are skipped. Raise Amount to " +
                                         std::to_string(kept) + " to keep them.");
}

// Stack edits land in the stack asset, not on this entity, so the card is re-checked as the stack and
// Amount change and the inspector rebuilt when it would read differently.
void RefreshWhenAmountShortfallChanges(const InspectorContext& context, const ParticleEmitter3D& emitter)
{
    if (!context.SimulationRefreshCallbacks || !context.RequestInspectorRefresh)
        return;
    context.SimulationRefreshCallbacks->push_back(
        [world = context.World, entity = context.Entity, built = AmountShortfall(emitter),
         builtAmount = emitter.Amount, refresh = context.RequestInspectorRefresh, requested = false]() mutable
        {
            const auto* current = world ? world->GetComponent<ParticleEmitter3D>(entity) : nullptr;
            if (requested || !current)
                return;
            const uint32 now = AmountShortfall(*current);
            if (now == built && (now == 0 || current->Amount == builtAmount))
                return;
            requested = true;
            refresh();
        });
}

// Where the particles' look is set: a Particle Renderer on this entity, or the default sprites.
void AddRenderingSection(const InspectorContext& context)
{
    if (context.World->HasComponent<Components::ParticleRenderer>(context.Entity))
        return;
    auto* section = InspectorUI::AddComponentSection(context.Parent, "ParticleEmitter/Rendering", "Rendering");
    InspectorUI::AddInfoCard(section->GetContentContainer(),
                             "The particles draw as soft smoke puffs facing the camera. Add a Particle Renderer "
                             "(Add Component, Rendering) to choose their material, texture sheet, lighting, "
                             "meshes and trails.");
}

void BuildParticleEmitterInspector(const InspectorContext& context)
{
    if (!context.Parent || !context.World || !context.Entity.IsValid())
        return;
    const auto* emitter = context.World->GetComponent<ParticleEmitter3D>(context.Entity);
    if (!emitter)
        return;
    const ParticleEmitter3D snapshot = *emitter;
    ParticleInspectors::AddParticlePreview(context);
    auto* simulation = InspectorUI::AddComponentSection(context.Parent, "ParticleEmitter/Simulation", "Simulation");
    InspectorContext rows = context;
    rows.Parent = simulation->GetContentContainer();
    RenderReflectedFieldRows(rows, ECS::GetComponentTypeId<ParticleEmitter3D>(), kSimulationFields);
    AddAmountWarning(rows.Parent, snapshot);
    RefreshWhenAmountShortfallChanges(context, snapshot);
    ParticleInspectors::AddEmitterStackSection(context);
    AddRenderingSection(context);
    AddSubEmitterSection(context, snapshot);
}

void RegisterParticleComponentTraits()
{
    auto& registry = Editor::EditorComponentTraitsRegistry::Get();
    Editor::EditorComponentTraits emitter;
    emitter.DisplayName = "Particle Emitter";
    emitter.InspectorCategory = "Rendering";
    emitter.InspectorIconClass = "inspector-section-icon-particles";
    emitter.InspectorTooltip = "When and how fast the particles simulate, and the stack that says what they do";
    registry.Register(ECS::GetComponentTypeId<ParticleEmitter3D>(), emitter);

    auto renderer = emitter;
    renderer.DisplayName = "Particle Renderer";
    renderer.InspectorTooltip = "How the emitter's particles draw";
    registry.Register(ECS::GetComponentTypeId<Components::ParticleRenderer>(), std::move(renderer));

    auto collisions = emitter;
    collisions.DisplayName = "Particle Collision Events";
    collisions.InspectorTooltip = "The particle collisions of the frame, for gameplay";
    registry.Register(ECS::GetComponentTypeId<Components::ParticleCollisionEventsBuffer>(), std::move(collisions));

    auto budget = emitter;
    budget.DisplayName = "Particle World Settings";
    budget.InspectorTooltip = "The most live particles every emitter in the world together simulates";
    registry.Register(ECS::GetComponentTypeId<Components::ParticleWorldSettings>(), std::move(budget));

    Editor::EditorComponentTraits playback;
    // The emitter inspector's preview controls show and drive it.
    playback.HideInInspector = true;
    registry.Register(ECS::GetComponentTypeId<Components::ParticlePlayback>(), std::move(playback));
}

} // namespace

void RegisterParticleInspector()
{
    RegisterParticleStackInspector();
    InspectorRegistry::Get().RegisterComponentInspector<ParticleEmitter3D>(BuildParticleEmitterInspector);
    ParticleInspectors::RegisterParticleRendererInspector();
    Editor::SceneTools::RegisterParticleEmitterGizmo();
    RegisterParticleComponentTraits();
}

} // namespace GameEngine

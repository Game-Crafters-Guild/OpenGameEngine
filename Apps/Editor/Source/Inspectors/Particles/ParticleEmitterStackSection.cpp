#include "Inspectors/Particles/ParticleEmitterStackSection.h"

#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Components/Rendering/Particles.h"
#include "Core/Engine.h"
#include "ECS/ECSTemplates.h"
#include "ECS/UnresolvedComponentStore.h"
#include "ECS/World.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/Particles/ParticleStackInspector.h"
#include "Inspectors/Particles/ParticleStackReference.h"
#include "Particles/Assets/ParticleStackAsset.h"
#include "Particles/ParticleStackAuthoring.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/InspectorActionButton.h"
#include "UI/Controls/InspectorNotice.h"
#include "UI/Controls/Label.h"

#include <atomic>
#include <memory>
#include <string>

namespace GameEngine::ParticleInspectors
{
namespace
{
constexpr const char* kStackExtension = ".particlestack";
constexpr const char* kStackBaseName = "Particle Stack";
constexpr const char* kStackFolder = "Particles";
constexpr int kAuthoringIndent = 2;

using Components::ParticleEmitter3D;

void AssignStack(const InspectorContext& context, const GUID& guid, const char* label)
{
    InspectorDrag::CommitComponentsWithUndo<ParticleEmitter3D>(
        context.World, context.Entity, InspectorDrag::GetAdditionalEntities(context), context.ChangeNotifications,
        context.Undo, label, [guid](ParticleEmitter3D& emitter)
        { emitter.Stack.Set(guid); });
    if (context.RequestInspectorRefresh)
        context.RequestInspectorRefresh();
}

std::string DefaultStackText(const std::string&)
{
    return Particles::SerializeParticleStack(Particles::MakeDefaultStack(), kAuthoringIndent) + "\n";
}

// Writes a stack asset holding the default stack into the Particles folder of the project and has
// the emitter run it, as one undo step.
void CreateStack(const InspectorContext& context, AssetManager& assets)
{
    if (context.Undo)
        context.Undo->BeginCompound("Create Particle Stack");
    const auto created = Editor::CreateAssetFile(assets.GetAssetRoot() / kStackFolder, kStackBaseName, kStackExtension,
                                                 DefaultStackText, "Create Particle Stack", context.Undo, &assets);
    const GUID guid = created.path.empty() ? GUID{} : assets.ResolveAssetGuid(created.path);
    if (!guid.IsNull())
        AssignStack(context, guid, "Assign Particle Stack");
    if (context.Undo)
        context.Undo->EndCompound();
}

// How the load of a stack asset the inspector waits for ended; written by the loader thread.
struct StackLoadOutcome
{
    std::atomic<bool> Failed = false;
    std::atomic<AssetError> Error = AssetError::ImportFailed;
};

const char* StackLoadFailureText(AssetError error)
{
    switch (error)
    {
    case AssetError::Missing:
        return "its file is missing";
    case AssetError::ImportFailed:
        return "its file does not hold a valid stack";
    case AssetError::MountUnavailable:
        return "its folder is unavailable";
    case AssetError::Cancelled:
        return "the load was cancelled";
    }
    return "the load failed";
}

// Requests the stack asset and shows the wait: the inspector rebuilds once the asset loads, and a
// load that fails says so in place, because an emitter whose stack does not load emits nothing.
void AddStackLoadStatus(UIElement* parent, const InspectorContext& context, AssetManager& assets, const GUID& guid)
{
    auto outcome = std::make_shared<StackLoadOutcome>();
    assets.LoadAsset(
        guid,
        [outcome](Result<SharedPtr<Asset>, AssetError> result)
        {
            if (result.IsOk())
                return;
            outcome->Error = result.Error();
            outcome->Failed = true;
        },
        AssetLoadPriority::High);
    auto status = std::make_unique<Label>();
    status->AddClass("inspector-text");
    status->SetText("Loading the stack...");
    const auto statusRef = UIElement::MakeWeakRef(status.get());
    parent->AddChild(std::move(status));
    if (!context.SimulationRefreshCallbacks || !context.RequestInspectorRefresh)
        return;
    context.SimulationRefreshCallbacks->push_back(
        [&assets, guid, outcome, statusRef, refresh = context.RequestInspectorRefresh, done = false]() mutable
        {
            if (done)
                return;
            if (assets.GetAsset(guid))
            {
                done = true;
                refresh();
                return;
            }
            if (!outcome->Failed)
                return;
            done = true;
            if (auto* label = statusRef.Get())
                label->SetText(std::string("The stack did not load: ") + StackLoadFailureText(outcome->Error.load()) +
                               ". This emitter emits nothing until it does; fix the file or choose another stack.");
        });
}

// Edits below change every emitter that runs the stack; say how many do when it is more than this one.
void AddSharedStackNote(UIElement* parent, const InspectorContext& context, const GUID& stack)
{
    uint32 emitters = 0;
    context.World->Query<ECS::Read<ParticleEmitter3D>>().Each(
        [&emitters, &stack](ECS::EntityHandle, const ParticleEmitter3D& emitter)
        { emitters += emitter.Stack.ToGuid() == stack ? 1u : 0u; });
    if (emitters > 1)
        InspectorUI::AddInfoCard(parent, "Shared: " + std::to_string(emitters) +
                                             " emitters run this stack, and editing it below changes all of them.");
}

// Whether the scene gave this entity a component no loaded module registers (kept, not applied).
bool HasUnregisteredComponent(const InspectorContext& context)
{
    const ECS::UnresolvedComponentStore* store = context.World->TryGetUnresolvedComponents();
    if (!store)
        return false;
    const auto entry = store->Map().find(context.Entity);
    return entry != store->Map().end() && !entry->second.empty();
}

void AddNewStackButton(UIElement* parent, const InspectorContext& context, AssetManager& assets)
{
    auto* field = InspectorUI::AddActionRow(parent, "particle-stack-add-row");
    auto button = std::make_unique<EditorUI::InspectorActionButton>("New Stack", "inspector-action-add");
    button->SetId("particle-emitter-new-stack");
    button->SetTooltip("Create a stack asset in the Particles folder, starting from the default stack, and run it here");
    button->SetOnClick([context, &assets](UIEvent&)
                       { CreateStack(context, assets); });
    field->AddChild(std::move(button));
}
} // namespace

void AddEmitterStackSection(const InspectorContext& context)
{
    auto* assets = EngineCore::GetInstance().TryGetAssetManager();
    const auto* emitter = context.World ? context.World->GetComponent<ParticleEmitter3D>(context.Entity) : nullptr;
    if (!context.Parent || !assets || !emitter)
        return;
    const GUID guid = emitter->Stack.ToGuid();
    ApplyParticleStackStyle(context.Parent);
    auto* section = InspectorUI::AddComponentSection(context.Parent, "ParticleEmitter/Stack", "Processor Stack", true);
    section->SetIconClass("particle-choice-curve");
    auto* content = section->GetContentContainer();
    InspectorUI::AddAssetFieldRow(
        content, "Stack", guid, {AssetType::ParticleStack}, &assets->GetRegistry(),
        [context](const GUID& chosen)
        { AssignStack(context, chosen, "Change Particle Stack"); }, context.Thumbnails,
        "The stack asset this emitter runs; editing it changes every emitter that runs it");
    switch (ResolveParticleStackReference(*assets, guid))
    {
    case ParticleStackReference::Default:
        if (HasUnregisteredComponent(context))
            content->AddChild(std::make_unique<EditorUI::InspectorNotice>(
                "Runs the default stack: continuous emission launched upward, with gravity. "
                "A component on this entity is not registered, so any settings it holds do "
                "not reach this emitter; create a stack to set what the particles do."));
        else
            InspectorUI::AddInfoCard(content, "Runs the default stack: continuous emission launched upward, with "
                                              "gravity. Create a stack to change what the particles do.");
        AddNewStackButton(content, context, *assets);
        return;
    case ParticleStackReference::Missing:
        InspectorUI::AddInfoCard(content, "The stack asset this emitter references is missing, so it emits nothing. "
                                          "Choose another stack or create one.");
        AddNewStackButton(content, context, *assets);
        return;
    case ParticleStackReference::NotAStack:
        InspectorUI::AddInfoCard(content, "The asset this emitter references is not a particle stack, so it emits "
                                          "nothing. Choose a stack or create one.");
        AddNewStackButton(content, context, *assets);
        return;
    case ParticleStackReference::Loading:
        AddStackLoadStatus(content, context, *assets, guid);
        return;
    case ParticleStackReference::Loaded:
        break;
    }
    AddSharedStackNote(content, context, guid);
    InspectorContext stackContext = context;
    stackContext.Parent = content;
    AddParticleStackRows(stackContext, *std::static_pointer_cast<Particles::ParticleStackAsset>(assets->GetAsset(guid)));
}

} // namespace GameEngine::ParticleInspectors

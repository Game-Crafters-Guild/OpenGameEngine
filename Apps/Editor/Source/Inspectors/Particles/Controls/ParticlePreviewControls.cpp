#include "Inspectors/Particles/Controls/ParticlePreviewControls.h"

#include "Components/Rendering/Particles.h"
#include "ECS/ECSTemplates.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/Controls/InspectorActionButton.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"

#include <cstdio>
#include <memory>
#include <string>

namespace GameEngine::ParticleInspectors
{
namespace
{
constexpr const char* kPreviewStyleAssetPath = "UI/inspectors/ParticlePreview/ParticlePreview.css";

struct PreviewAction
{
    const char* Text;
    const char* IconClass;
    const char* Tooltip;
};

constexpr PreviewAction kPreviewActions[] = {
    {"Restart", "inspector-action-restart", "Restart the particle preview"},
    {"Pause", "inspector-action-pause", "Pause or resume the particle preview"},
    {"Step", "inspector-action-step", "Advance one simulation step"},
};

struct ParticlePreviewBinding
{
    ECS::World* World = nullptr;
    uint64 WorldId = 0;
    ECS::EntityHandle Entity{};
    std::function<ECS::World*()> GetWorld;
};

ECS::World* ResolvePreviewWorld(const ParticlePreviewBinding& binding)
{
    auto* current = binding.GetWorld ? binding.GetWorld() : binding.World;
    return current && current->GetWorldId() == binding.WorldId ? current : nullptr;
}

// The emitter's playback commands, added on the first preview action. Preview state is runtime
// state: it is not saved with the scene and takes no undo step.
Components::ParticlePlayback* EnsurePlayback(const ParticlePreviewBinding& binding)
{
    auto* world = ResolvePreviewWorld(binding);
    if (!world || !world->IsValid(binding.Entity))
        return nullptr;
    if (!world->HasComponent<Components::ParticlePlayback>(binding.Entity))
        world->AddComponentImmediate(binding.Entity, Components::ParticlePlayback{});
    return world->GetComponentForWrite<Components::ParticlePlayback>(binding.Entity);
}

void ApplyPreviewAction(const ParticlePreviewBinding& binding, const std::string& action)
{
    auto* playback = EnsurePlayback(binding);
    if (!playback)
        return;
    if (action == "Restart")
        ++playback->Restart;
    else if (action == "Step")
    {
        playback->Paused = true;
        ++playback->SingleStep;
    }
    else
        playback->Paused = !playback->Paused;
}

void SetPreviewTime(const ParticlePreviewBinding& binding, float time)
{
    auto* playback = EnsurePlayback(binding);
    if (!playback)
        return;
    playback->SeekTime = time;
    playback->Paused = true;
    ++playback->Seek;
}

void RefreshPreviewDisplay(const ParticlePreviewBinding& binding,
                           EditorUI::CollapsibleInfoCard* status, EditorUI::InspectorActionButton* pauseButton)
{
    auto* world = ResolvePreviewWorld(binding);
    const auto* playback = world ? world->GetComponent<Components::ParticlePlayback>(binding.Entity) : nullptr;
    if (!playback)
    {
        status->SetText("Playing. Pause, step or scrub to preview; statistics show once the preview is driven.");
        return;
    }
    const char* label = playback->Paused ? "Play" : "Pause";
    if (pauseButton && pauseButton->GetText() != label)
    {
        pauseButton->SetText(label);
        pauseButton->SetIconClass(playback->Paused ? "inspector-action-play" : "inspector-action-pause");
        pauseButton->SetTooltip(playback->Paused ? "Resume the particle preview" : "Pause the particle preview");
    }
    char timing[160];
    std::snprintf(timing, sizeof(timing), " · Preview %.2f s\nSimulation %.3f ms · ", playback->SimulatedTime,
                  playback->SimulationMs);
    char dropped[48];
    std::snprintf(dropped, sizeof(dropped), " · Catch-up dropped %.2f s", playback->DroppedTime);
    // A fixed word before each count, never a plural: the counts change from one refresh to the next,
    // and a word that changes with them moves the rest of the line.
    status->SetText(std::string(playback->Paused ? "Paused" : "Playing") + " · Particles " +
                    std::to_string(playback->LiveCount) + timing + "Ticks " +
                    std::to_string(playback->SimulatedSteps) + dropped);
}
} // namespace

void AddParticlePreview(const InspectorContext& ctx)
{
    const ParticlePreviewBinding binding{ctx.World, ctx.World->GetWorldId(), ctx.Entity, ctx.GetWorld};
    auto* buttons = InspectorUI::AddActionRow(ctx.Parent, "particle-preview-controls");
    buttons->GetParent()->RequestSubtreeStyleAssetPath(kPreviewStyleAssetPath, "editor");
    buttons->AddClass("particle-preview-buttons");
    EditorUI::InspectorActionButton* pauseButton = nullptr;
    for (const PreviewAction& entry : kPreviewActions)
    {
        const std::string action = entry.Text;
        auto button = std::make_unique<EditorUI::InspectorActionButton>(action, entry.IconClass);
        button->SetTooltip(entry.Tooltip);
        button->SetOnClick([binding, action](UIEvent&) { ApplyPreviewAction(binding, action); });
        if (action == "Pause")
            pauseButton = button.get();
        buttons->AddChild(std::move(button));
    }
    auto* timeline = InspectorDrag::AddSliderRow(ctx.Parent, "Preview Time", 0.0f, 0.0f, 60.0f,
                                                 "Scrub a reproducible stationary-emitter preview in seconds; pauses playback")
                         .second;
    timeline->SetOnValueChanged([binding](float time) { SetPreviewTime(binding, time); });
    auto status = std::make_unique<EditorUI::CollapsibleInfoCard>("");
    status->SetId("particle-preview-stats");
    status->SetTooltip("Catch-up dropped: simulated time the preview skipped because frames took longer than the "
                       "steps one frame may run, as during an editor stall; the effect then trails the clock "
                       "instead of stalling the editor further");
    auto* statusPointer = status.get();
    const auto statusRef = UIElement::MakeWeakRef(statusPointer);
    const auto pauseRef = UIElement::MakeWeakRef(pauseButton);
    ctx.Parent->AddChild(std::move(status));
    RefreshPreviewDisplay(binding, statusPointer, pauseButton);
    if (ctx.SimulationRefreshCallbacks)
        ctx.SimulationRefreshCallbacks->push_back([binding, statusRef, pauseRef]
                                                  {
            auto* statusCard = statusRef.Get();
            auto* pause = pauseRef.Get();
            if (statusCard)
                RefreshPreviewDisplay(binding, statusCard, pause);
        });
}
} // namespace GameEngine::ParticleInspectors

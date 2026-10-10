#include "UI/Controls/ScenePostFxMenu.h"

#include "Core/Engine.h"
#include "Editor/Entities/ComponentEnabledToggle.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "SceneViewController.h"
#include "UI/Controls/SceneViewToolbar.h"
#include "UI/EditorIcons.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "ECS/Components.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"

#include <set>
#include <string>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

// Display names of effects that sit enabled on an enabled PostProcessVolume of an enabled
// entity — the scan that decides which per-effect rows exist this show.
//
// Queried by component, not walked over every alive entity: this runs on every menu open, and
// a scene holds a handful of volumes among however many entities it has.
std::vector<std::string> CollectScenePostFxLabels(ECS::World* world)
{
    if (!world)
        return {};

    std::set<std::string> tags;
    world->Query<ECS::Read<Components::PostProcessVolume>>()
        .Each(
            [&](ECS::EntityHandle e, const Components::PostProcessVolume& vol)
            {

                Rendering::PostProcessEffectRegistry::ForEach(
                    [&](const Rendering::PostProcessEffectDescriptor& descriptor)
                    {
                        if (!world->HasComponent(e, descriptor.Type))
                            return;

                        // The effect's own switch, as its section's dot reads it: the kept
                        // Enabled field the registry records for the type.
                        bool enabled = true;
                        (void)TryGetComponentEnabled(world, e, descriptor.Type, enabled);
                        if (enabled)
                            tags.emplace(descriptor.DisplayName);
                    });
            });
    return {tags.begin(), tags.end()};
}

bool SceneFxContains(const std::vector<std::string>& labels, std::string_view tag)
{
    for (const std::string& s : labels)
    {
        if (s == tag)
            return true;
    }
    return false;
}

/// Per-effect rows: checked state reflects Scene View prefs; rows disable when the master
/// post-processing toggle is off.
uint32_t PerEffectFlags(bool prefOn, bool masterOn)
{
    uint32_t f = prefOn ? MenuItemFlag_Checked : MenuItemFlag_None;
    if (!masterOn)
        f |= MenuItemFlag_Disabled;
    return f;
}

} // namespace

std::vector<ContextMenuManipulator::Item> BuildScenePostFxMenuItems(
    SceneViewController* controller, SceneViewToolbar* toolbar,
    std::function<void()> onAutoExposureToggled)
{
    if (!controller)
        return {};

    ECS::World* world = EngineCore::GetInstance().EnsurePrimaryWorld();
    const std::vector<std::string> sceneFxLabels = CollectScenePostFxLabels(world);

    auto& settings = Editor::SceneViewSettings::Get();
    const bool masterOn = controller->IsPostProcessingEnabled();

    const bool bloomInScene = SceneFxContains(sceneFxLabels, "Bloom Effect");
    const bool colorFilterInScene = SceneFxContains(sceneFxLabels, "Color Filter Effect");
    const bool casInScene = SceneFxContains(sceneFxLabels, "Contrast Adaptive Sharpening");
    const bool crtInScene = SceneFxContains(sceneFxLabels, "CRT Effect");

    const auto checkedFlag = [](bool v) -> uint32_t
    { return v ? MenuItemFlag_Checked : MenuItemFlag_None; };

    // Flat, single-level rows — no "Post Processing/…" parent, so the menu stays flat.
    std::vector<ContextMenuManipulator::Item> items;
    items.push_back({.Path = "Enable Post Processing",
                     .IconPath = EditorIcons::kPostFx,
                     .Flags = checkedFlag(masterOn),
                     .OnActivate =
                         [controller, toolbar]
                         {
                             controller->TogglePostProcessing();
                             if (toolbar)
                                 toolbar->UpdatePostProcessButtonState();
                         }});

    // Auto exposure meters the camera-less Scene View regardless of post-process volumes,
    // so it always appears and is independent of the master post-processing toggle.
    items.push_back({.Separator = true});
    items.push_back({.Path = "Auto Exposure",
                     .IconPath = EditorIcons::kPostFx,
                     .Flags = checkedFlag(settings.GetPostFxAutoExposureEnabled()),
                     .OnActivate =
                         [onAutoExposureToggled = std::move(onAutoExposureToggled)]
                         {
                             auto& s = Editor::SceneViewSettings::Get();
                             s.SetPostFxAutoExposureEnabled(!s.GetPostFxAutoExposureEnabled(),
                                                            /*fromUserToggle=*/true);
                             if (onAutoExposureToggled)
                                 onAutoExposureToggled();
                         }});

    const bool anySceneEffect = bloomInScene || colorFilterInScene || casInScene || crtInScene;
    if (anySceneEffect)
        items.push_back({.Separator = true});
    if (bloomInScene)
        items.push_back({.Path = "Bloom",
                         .IconPath = EditorIcons::kPostFx,
                         .Flags = PerEffectFlags(settings.GetPostFxBloomEnabled(), masterOn),
                         .OnActivate =
                             []
                             {
                                 auto& s = Editor::SceneViewSettings::Get();
                                 s.SetPostFxBloomEnabled(!s.GetPostFxBloomEnabled());
                             }});
    if (colorFilterInScene)
        items.push_back({.Path = "Color filter",
                         .IconPath = EditorIcons::kColorFilter,
                         .Flags = PerEffectFlags(settings.GetPostFxColorFilterEnabled(), masterOn),
                         .OnActivate =
                             []
                             {
                                 auto& s = Editor::SceneViewSettings::Get();
                                 s.SetPostFxColorFilterEnabled(!s.GetPostFxColorFilterEnabled());
                             }});
    if (casInScene)
        items.push_back({.Path = "CAS",
                         .IconPath = EditorIcons::kPostFx,
                         .Flags = PerEffectFlags(settings.GetPostFxCasEnabled(), masterOn),
                         .OnActivate =
                             []
                             {
                                 auto& s = Editor::SceneViewSettings::Get();
                                 s.SetPostFxCasEnabled(!s.GetPostFxCasEnabled());
                             }});
    if (crtInScene)
        items.push_back({.Path = "CRT",
                         .IconPath = EditorIcons::kPostFx,
                         .Flags = PerEffectFlags(settings.GetPostFxCrtEnabled(), masterOn),
                         .OnActivate =
                             []
                             {
                                 auto& s = Editor::SceneViewSettings::Get();
                                 s.SetPostFxCrtEnabled(!s.GetPostFxCrtEnabled());
                             }});

    return items;
}

} // namespace GameEngine::Editor

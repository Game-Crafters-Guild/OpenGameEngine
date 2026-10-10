#include "Editor/Settings/DirectionalShadowSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/RenderProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Logger/Logger.h"

#include <array>
#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
struct ResolutionOption
{
    uint32_t Value;
    const char* Label;
};

constexpr std::array<ResolutionOption, 4> kResolutionOptions = {{
    {1024u, "1024 (Performance)"},
    {2048u, "2048 (Default)"},
    {4096u, "4096 (High)"},
    {8192u, "8192 (Ultra)"},
}};

const char* ResolutionLabel(uint32_t resolution)
{
    for (const ResolutionOption& option : kResolutionOptions)
        if (option.Value == resolution)
            return option.Label;
    return kResolutionOptions[1].Label;
}

uint32_t ResolutionFromLabel(const std::string& label)
{
    for (const ResolutionOption& option : kResolutionOptions)
        if (label == option.Label)
            return option.Value;
    return kDefaultDirectionalShadowResolution;
}
} // namespace

SettingsCategoryDescriptor CreateDirectionalShadowSettingsSection(
    std::function<void()> onProjectRenderSettingsChanged)
{
    SettingsCategoryDescriptor section;
    section.CategoryId = "directionalShadows";
    section.Title = "Directional Shadows";
    section.Group = SettingsCategoryGroup::ProjectSettings;
    section.Description =
        "Applies to all directional cascades. 4096 uses 4x and 8192 uses 16x the shadow-map "
        "memory of 2048.";

    SettingsFieldDescriptor resolution;
    resolution.Label = "Shadow Resolution";
    resolution.Tooltip =
        "Resolution of each directional shadow cascade. Higher tiers improve edge detail but "
        "scale memory and shadow raster cost quadratically.";
    resolution.SearchKeywords = "shadow map cascade resolution 1024 2048 4096 8192 quality";

    SettingsFieldDescriptor::DropdownField dropdown;
    for (const ResolutionOption& option : kResolutionOptions)
        dropdown.Options.emplace_back(option.Label);
    dropdown.DefaultValue = ResolutionLabel(kDefaultDirectionalShadowResolution);
    dropdown.Get = []()
    {
        SettingsStore store = OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
        std::string error;
        (void)store.Load(&error);
        return std::string(ResolutionLabel(GetDirectionalShadowResolution(store)));
    };
    // Copied, not moved: the projection toggle below needs its own copy of the
    // same callback, and onProjectRenderSettingsChanged is only a
    // per-page-build std::function -- copying it twice is not a hot path.
    dropdown.Set = [onChanged = onProjectRenderSettingsChanged](const std::string& label)
    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        if (workspaceRoot.empty())
        {
            Logger::Log::Warning(
                "DirectionalShadowSettings: cannot save resolution -- no workspace root");
            return;
        }

        SettingsStore store = OpenProjectSettings(workspaceRoot);
        std::string error;
        (void)store.Load(&error);
        const uint32_t resolution = ResolutionFromLabel(label);
        if (GetDirectionalShadowResolution(store) == resolution)
            return;

        SetDirectionalShadowResolution(store, resolution);
        if (!store.Save(&error))
        {
            Logger::Log::Error(
                "DirectionalShadowSettings: failed to save resolution: {}", error);
            return;
        }
        if (onChanged)
            onChanged();
    };
    resolution.Control = std::move(dropdown);
    section.Fields.push_back(std::move(resolution));

    SettingsFieldDescriptor projection;
    projection.Label = "Stable Projection";
    projection.Tooltip =
        "Keeps the cascade's size fixed as the camera moves, so shadow texels stay welded to the "
        "world instead of crawling. On, the splits come only from the shadow distance and split "
        "lambda, and the cascade is fitted to the frustum slice's bounding sphere -- both are "
        "invariant to camera movement and rotation, so the world area each shadow texel covers "
        "never changes. Off (Close Fit), the splits follow the measured depth bounds and the "
        "cascade is fitted tightly to what is actually visible: sharper shadows, but the fit "
        "resizes as you move and every texel re-quantizes at once, which reads as shimmering or "
        "dancing shadow edges. Stable costs roughly a third of the near-cascade texel density; "
        "raise Shadow Resolution or Split Lambda to win it back.";
    projection.SearchKeywords =
        "stable projection close fit shimmer shimmering crawl crawling dancing texel swim "
        "flicker cascade sphere sdsm";

    SettingsFieldDescriptor::ToggleField projectionToggle;
    projectionToggle.DefaultValue = kDefaultDirectionalShadowStableProjection;
    projectionToggle.Get = []()
    {
        SettingsStore store = OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
        std::string error;
        (void)store.Load(&error);
        return GetDirectionalShadowStableProjection(store);
    };
    projectionToggle.Set = [onChanged = onProjectRenderSettingsChanged](bool value)
    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        if (workspaceRoot.empty())
        {
            Logger::Log::Warning(
                "DirectionalShadowSettings: cannot save projection mode -- no workspace root");
            return;
        }

        SettingsStore store = OpenProjectSettings(workspaceRoot);
        std::string error;
        (void)store.Load(&error);
        if (GetDirectionalShadowStableProjection(store) == value)
            return;

        SetDirectionalShadowStableProjection(store, value);
        if (!store.Save(&error))
        {
            Logger::Log::Error(
                "DirectionalShadowSettings: failed to save projection mode: {}", error);
            return;
        }
        if (onChanged)
            onChanged();
    };
    projection.Control = std::move(projectionToggle);
    section.Fields.push_back(std::move(projection));

    return section;
}

} // namespace GameEngine::Editor

#include "Engine/Rendering/LodProjectSettings.h"

#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Rendering/CameraTypes.h"

#include <algorithm>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace GameEngine::Rendering
{
namespace
{
// The editor's per-project settings file. Read-only from here; EditorPaths owns
// the authoritative construction for the editor, which is also its only writer.
constexpr const char* kEditorDirName = ".Editor";
constexpr const char* kProjectSettingsFileName = "ProjectSettings.json";

std::optional<float> ReadClampedNumber(const nlohmann::json& rendering, const char* key, float minValue,
                                       float maxValue)
{
    const auto it = rendering.find(key);
    if (it == rendering.end() || !it->is_number())
        return std::nullopt;
    return std::clamp(it->get<float>(), minValue, maxValue);
}

// A view class's override pair. Absent keys leave the default (disabled at
// 100%), which resolves to the global budget unchanged — so a project that
// never opened the page behaves exactly as one with no override keys.
LodViewBudgetOverride ReadViewBudget(const nlohmann::json& rendering,
                                     const char* enabledKey, const char* percentKey)
{
    LodViewBudgetOverride result;
    if (const auto it = rendering.find(enabledKey); it != rendering.end() && it->is_boolean())
        result.Enabled = it->get<bool>();
    if (const auto percent = ReadClampedNumber(rendering, percentKey,
                                               LodProjectSettings::kMinBudgetPercent,
                                               LodProjectSettings::kMaxBudgetPercent))
        result.BudgetPercent = *percent;
    return result;
}

// Written only while enabled, matching the absent-key contract the optionals
// encode: a project that never turned an override on carries no override keys.
// Disabling ERASES the pair rather than storing "false", so the file never
// accumulates dead settings.
void WriteViewBudget(nlohmann::json& rendering, const char* enabledKey, const char* percentKey,
                     const LodViewBudgetOverride& budget)
{
    if (budget.Enabled)
    {
        rendering[enabledKey] = true;
        rendering[percentKey] = budget.BudgetPercent;
        return;
    }
    rendering.erase(enabledKey);
    rendering.erase(percentKey);
}
} // namespace

LodProjectSettings LodProjectSettings::ReadFrom(const nlohmann::json& rendering)
{
    LodProjectSettings settings{};
    if (!rendering.is_object())
        return settings;

    if (const auto it = rendering.find(kModeKey); it != rendering.end() && it->is_string())
    {
        // An unrecognized token leaves the default standing rather than picking
        // a mode nobody asked for; the warning is what makes the typo findable.
        const std::string token = it->get<std::string>();
        if (!TryParseLodSelectionMode(token, settings.SelectionMode))
        {
            Logger::Log::Warning(
                "LodProjectSettings: rendering.{} is '{}', expected '{}', '{}' or '{}' — using '{}'",
                kModeKey, token, kLodSelectionModeOffToken, kLodSelectionModeCoverageToken,
                kLodSelectionModeSseToken, ToString(settings.SelectionMode));
        }
    }
    settings.ErrorBudgetPx =
        ReadClampedNumber(rendering, kErrorBudgetPxKey, kMinErrorBudgetPx, kMaxErrorBudgetPx);
    settings.SkinnedBudgetScale = ReadClampedNumber(rendering, kSkinnedBudgetScaleKey,
                                                    kMinSkinnedBudgetScale, kMaxSkinnedBudgetScale);
    settings.GameViewBudget =
        ReadViewBudget(rendering, kGameViewBudgetEnabledKey, kGameViewBudgetPercentKey);
    settings.SceneViewBudget =
        ReadViewBudget(rendering, kSceneViewBudgetEnabledKey, kSceneViewBudgetPercentKey);
    settings.CrossfadeDuration = ReadClampedNumber(rendering, kCrossfadeDurationKey,
                                                   kMinCrossfadeDuration, kMaxCrossfadeDuration);
    settings.HysteresisBand = ReadClampedNumber(rendering, kHysteresisBandKey,
                                                kMinHysteresisBand, kMaxHysteresisBand);
    return settings;
}

void LodProjectSettings::WriteTo(nlohmann::json& rendering) const
{
    rendering[kModeKey] = std::string(ToString(SelectionMode));
    if (ErrorBudgetPx.has_value())
        rendering[kErrorBudgetPxKey] = *ErrorBudgetPx;
    if (SkinnedBudgetScale.has_value())
        rendering[kSkinnedBudgetScaleKey] = *SkinnedBudgetScale;
    WriteViewBudget(rendering, kGameViewBudgetEnabledKey, kGameViewBudgetPercentKey,
                    GameViewBudget);
    WriteViewBudget(rendering, kSceneViewBudgetEnabledKey, kSceneViewBudgetPercentKey,
                    SceneViewBudget);
    if (CrossfadeDuration.has_value())
        rendering[kCrossfadeDurationKey] = *CrossfadeDuration;
    if (HysteresisBand.has_value())
        rendering[kHysteresisBandKey] = *HysteresisBand;
}

LodProjectSettings LodProjectSettings::Load(const std::filesystem::path& workspaceRoot)
{
    LodProjectSettings settings{};
    if (workspaceRoot.empty())
        return settings;

    const std::filesystem::path settingsPath =
        (workspaceRoot / kEditorDirName / kProjectSettingsFileName).lexically_normal();
    std::ifstream file(settingsPath);
    if (!file.is_open())
        return settings;

    // A malformed settings file must not take the renderer's LOD state with it:
    // every other rendering.* consumer of this file survives a bad parse on its
    // own defaults, and a built game has no editor to repair it in.
    const nlohmann::json root = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object())
    {
        Logger::Log::Warning("LodProjectSettings: '{}' is not readable JSON — using defaults",
                             settingsPath.generic_string());
        return settings;
    }

    const auto rendering = root.find(kRenderingKey);
    if (rendering == root.end())
        return settings;
    return ReadFrom(*rendering);
}

float LodProjectSettings::EffectiveErrorBudgetPx() const
{
    return ErrorBudgetPx.value_or(kDefaultLodErrorBudgetPx);
}

float LodProjectSettings::EffectiveSkinnedBudgetScale() const
{
    return SkinnedBudgetScale.value_or(kDefaultLodSkinnedBudgetScale);
}

float LodProjectSettings::EffectiveCrossfadeDuration() const
{
    return CrossfadeDuration.value_or(kDefaultCrossfadeDuration);
}

float LodProjectSettings::EffectiveHysteresisBand() const
{
    return HysteresisBand.value_or(kDefaultHysteresisBand);
}

void LodProjectSettings::ApplyTo(Engine::Renderer::RenderServices& renderServices) const
{
    // Every knob, always -- an absent key applies the engine default rather
    // than skipping the setter. On a project switch the renderer already holds
    // the OUTGOING project's values, so a skip would leak them into a project
    // whose settings page reports the default (EffectiveErrorBudgetPx).
    renderServices.SetLODErrorBudgetPx(EffectiveErrorBudgetPx());
    renderServices.SetLODSkinnedBudgetScale(EffectiveSkinnedBudgetScale());
    // Both classes always, for the same reason: an incoming project with no
    // override keys must CLEAR the outgoing project's override, not inherit it.
    renderServices.SetLODViewBudgetOverride(ViewPurpose::Game, GameViewBudget);
    renderServices.SetLODViewBudgetOverride(ViewPurpose::EditorScene, SceneViewBudget);
    renderServices.SetLODCrossfadeDuration(EffectiveCrossfadeDuration());
    renderServices.SetLODHysteresisBand(EffectiveHysteresisBand());
    renderServices.GetMeshGPURegistry().SetLodSelectionMode(SelectionMode);
}

} // namespace GameEngine::Rendering

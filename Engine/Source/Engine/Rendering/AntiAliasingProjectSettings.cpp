#include "Engine/Rendering/AntiAliasingProjectSettings.h"

#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ViewRegistry.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace GameEngine::Rendering
{
namespace
{
// The editor's per-project settings file. Read-only from here; EditorPaths owns
// the authoritative construction for the editor, which is also its only writer.
constexpr const char* kEditorDirName = ".Editor";
constexpr const char* kProjectSettingsFileName = "ProjectSettings.json";
constexpr const char* kRenderingKey = "rendering";

std::string LowerTrimmed(std::string value)
{
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char ch) { return std::isspace(ch) != 0; }),
                value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string ReadString(const nlohmann::json& rendering, const char* key)
{
    const auto it = rendering.find(key);
    if (it == rendering.end() || !it->is_string())
        return {};
    return LowerTrimmed(it->get<std::string>());
}
} // namespace

const char* ToAAModeToken(Engine::Renderer::AntiAliasingMode mode)
{
    using Engine::Renderer::AntiAliasingMode;
    switch (mode)
    {
        case AntiAliasingMode::MSAA: return "msaa";
        case AntiAliasingMode::TAA:  return "taa";
        case AntiAliasingMode::FXAA: return "fxaa";
        case AntiAliasingMode::SMAA: return "smaa";
        case AntiAliasingMode::TemporalFXAA: return "temporalfxaa";
        case AntiAliasingMode::Off:  break;
    }
    return "off";
}

Engine::Renderer::AntiAliasingMode ParseAAModeToken(std::string_view token)
{
    using Engine::Renderer::AntiAliasingMode;
    const std::string normalized = LowerTrimmed(std::string(token));
    if (normalized == "msaa")
        return AntiAliasingMode::MSAA;
    if (normalized == "taa")
        return AntiAliasingMode::TAA;
    if (normalized == "fxaa")
        return AntiAliasingMode::FXAA;
    if (normalized == "smaa")
        return AntiAliasingMode::SMAA;
    if (normalized == "temporalfxaa")
        return AntiAliasingMode::TemporalFXAA;
    return AntiAliasingMode::Off;
}

AntiAliasingProjectSettings AntiAliasingProjectSettings::ReadFrom(const nlohmann::json& rendering)
{
    using Engine::Renderer::AntiAliasingMode;
    AntiAliasingProjectSettings settings{};
    if (!rendering.is_object())
        return settings;

    // "auto" is the retired token: it named a per-frame device resolution, not
    // a mode, so a project still carrying it counts as never having chosen.
    const std::string token = ReadString(rendering, "aaMode");
    const bool hasKey = !token.empty();
    const bool hasMode = hasKey && token != "auto";
    if (hasMode)
    {
        settings.ModeChosen = true;
        settings.AAMode = ParseAAModeToken(token);
    }

    // "off"/"1" = 1, else the explicit count. Absent, "auto" (the retired token
    // that named a per-frame device resolution) and anything unreadable all
    // leave kDefaultMsaaSampleCount standing — this is the ONE place a file
    // naming no count acquires one, and the device clamp in
    // SetDefaultMSAASampleCount caps it per machine from there.
    if (const auto it = rendering.find("msaa"); it != rendering.end())
    {
        std::string value;
        if (it->is_string())
            value = LowerTrimmed(it->get<std::string>());
        else if (it->is_number_integer())
            value = std::to_string(it->get<int>());
        settings.SamplesChosen = true;
        if (value == "off" || value == "1" || value == "1x")
            settings.MsaaSamples = 1;
        else if (value == "2" || value == "2x")
            settings.MsaaSamples = 2;
        else if (value == "4" || value == "4x")
            settings.MsaaSamples = 4;
        else if (value == "8" || value == "8x")
            settings.MsaaSamples = 8;
        else
        {
            settings.SamplesChosen = false;
            Logger::Log::Warning(
                "AntiAliasingProjectSettings: rendering.msaa is {} — expected off|1|2|4|8 (or "
                "1x|2x|4x|8x); using {} sample(s). Opening the project in the editor rewrites the "
                "key concretely.",
                it->dump(), settings.MsaaSamples);
        }
    }

    // No aaMode key AT ALL. A legacy explicit sample count still migrates to
    // MSAA, as it always has — but only a count the file actually named: the
    // default filled in above is not a choice, and reading it as one would
    // hand every keyless project MSAA without asking the device first.
    if (!hasKey && settings.SamplesChosen && settings.MsaaSamples > 1)
    {
        settings.ModeChosen = true;
        settings.AAMode = AntiAliasingMode::MSAA;
    }

    if (ReadString(rendering, "fxaaQuality") == "fast")
        settings.FxaaQualityValue = Engine::Renderer::FxaaQuality::Fast;

    if (const auto it = rendering.find("taaRenderScale"); it != rendering.end() && it->is_number())
        settings.RenderScale = std::clamp(it->get<float>(), Engine::Renderer::kMinRenderScale,
                                          Engine::Renderer::kMaxRenderScale);

    settings.ScaleMode = Engine::Renderer::ResolveStartupDynamicResolutionMode(
        ReadString(rendering, "drsMode"), settings.RenderScale);

    // Same [15, 240] range the editor's DRS settings page clamps to.
    if (const auto it = rendering.find("drsTargetFps"); it != rendering.end() && it->is_number())
        settings.DrsTargetFps = std::clamp(it->get<float>(), 15.0f, 240.0f);

    return settings;
}

void AntiAliasingProjectSettings::WriteTo(nlohmann::json& rendering) const
{
    // The keyed writes below would auto-vivify an object; erase() on a
    // non-object throws, so normalize before either can run.
    if (!rendering.is_object())
        rendering = nlohmann::json::object();

    // ABSENT is how "this project has never chosen" is spelled on disk, so an
    // unchosen snapshot must write neither key — and must clear a retired
    // "auto" it is writing over. Emitting ToAAModeToken's "off" here would
    // convert "never chose" into "chose no anti-aliasing" on the way out, which
    // is how the cook used to hand a packaged game an unantialiased default the
    // project never asked for.
    if (ModeChosen)
    {
        rendering["aaMode"] = ToAAModeToken(AAMode);
        rendering["msaa"] = std::to_string(MsaaSamples);
    }
    else
    {
        rendering.erase("aaMode");
        rendering.erase("msaa");
    }
    rendering["fxaaQuality"] =
        FxaaQualityValue == Engine::Renderer::FxaaQuality::Fast ? "fast" : "quality";
    rendering["taaRenderScale"] = RenderScale;
    rendering["drsMode"] = Engine::Renderer::ToString(ScaleMode);
    rendering["drsTargetFps"] = DrsTargetFps;
}

AntiAliasingProjectSettings AntiAliasingProjectSettings::Load(
    const std::filesystem::path& workspaceRoot)
{
    AntiAliasingProjectSettings settings{};
    if (workspaceRoot.empty())
        return settings;

    const std::filesystem::path settingsPath =
        (workspaceRoot / kEditorDirName / kProjectSettingsFileName).lexically_normal();
    std::ifstream file(settingsPath);
    if (!file.is_open())
        return settings;

    // A malformed settings file must not take the renderer's AA state with it;
    // a built game has no editor to repair it in.
    const nlohmann::json root = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object())
    {
        Logger::Log::Warning("AntiAliasingProjectSettings: '{}' is not readable JSON — using defaults",
                             settingsPath.generic_string());
        return settings;
    }

    const auto rendering = root.find(kRenderingKey);
    if (rendering == root.end())
        return settings;
    return ReadFrom(*rendering);
}

void AntiAliasingProjectSettings::ApplyAntiAliasingTo(
    Engine::Renderer::RenderServices& renderServices) const
{
    // A project that has never chosen a mode takes this device's rung, which
    // owns the mode AND the count together — the stored count belongs to a mode
    // the project never picked, so it does not survive.
    const Engine::Renderer::ResolvedAntiAliasing aa =
        ModeChosen ? Engine::Renderer::ResolvedAntiAliasing{AAMode, MsaaSamples}
                   : Engine::Renderer::ResolveDefaultAntiAliasing(
                         renderServices.GetAntiAliasingDeviceCaps());

    renderServices.SetDefaultMSAASampleCount(aa.SampleCount);
    // GE_AA_MODE is read once in RenderServices::Initialize and outranks the
    // project file, so the mode setter is the one it suppresses; the sample
    // count and FXAA quality are not its business and still apply. The guard
    // lives here rather than in a host because both hosts want the same
    // precedence, and the Player applies this snapshot with nothing in front of
    // it to hold the override back.
    if (std::getenv("GE_AA_MODE") == nullptr)
        renderServices.SetDefaultAntiAliasingMode(aa.Mode);
    renderServices.SetFxaaQuality(FxaaQualityValue);
}

void AntiAliasingProjectSettings::ApplyRenderScaleTo(
    Engine::Renderer::RenderServices& renderServices) const
{
    using Engine::Renderer::DynamicResolutionMode;

    // GE_TAA_RENDER_SCALE pins the scale, and every setter below writes it (Off
    // pins 1.0, leaving Dynamic restores its remembered value), so the whole
    // half is suppressed rather than re-fed its own current values.
    if (std::getenv("GE_TAA_RENDER_SCALE") != nullptr)
        return;

    auto drsConfig = renderServices.GetDynamicResolutionConfig();
    drsConfig.TargetGpuMs = 1000.0f / std::max(DrsTargetFps, 1.0f);
    renderServices.SetDynamicResolutionConfig(drsConfig);

    // Off first to normalize (mode Off pins the scale to 1.0), then the scale,
    // then the mode — entering Fixed records the scale as the value leaving
    // Dynamic later restores.
    renderServices.SetDynamicResolutionMode(DynamicResolutionMode::Off);
    renderServices.SetDefaultRenderScale(RenderScale);
    renderServices.SetDynamicResolutionMode(ScaleMode);
}

void AntiAliasingProjectSettings::ApplyTo(Engine::Renderer::RenderServices& renderServices) const
{
    ApplyAntiAliasingTo(renderServices);
    ApplyRenderScaleTo(renderServices);
}

} // namespace GameEngine::Rendering

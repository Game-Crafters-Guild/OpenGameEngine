#include "Editor/Settings/RenderProjectSettings.h"

#include "Editor/Settings/DynamicResolutionProjectSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Engine/Rendering/AntiAliasingProjectSettings.h"
#include "Engine/Rendering/LodProjectSettings.h"
#include "Engine/Rendering/AntiAliasing.h"
#include "Engine/Rendering/DynamicResolutionController.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>

namespace GameEngine::Editor
{
namespace
{

const nlohmann::json& FindRenderingSettings(const SettingsStore& store)
{
    const auto& root = store.Json();
    if (root.is_object())
        if (const auto it = root.find("rendering"); it != root.end() && it->is_object())
            return *it;

    static const nlohmann::json kEmptyRendering = nlohmann::json::object();
    return kEmptyRendering;
}

uint32_t NormalizeDirectionalShadowResolution(uint32_t requested)
{
    static constexpr uint32_t kTiers[] = {1024u, 2048u, 4096u, 8192u};
    uint32_t snapped = kTiers[0];
    uint32_t bestDistance = requested > snapped ? requested - snapped : snapped - requested;
    for (uint32_t tier : kTiers)
    {
        const uint32_t distance = requested > tier ? requested - tier : tier - requested;
        if (distance < bestDistance)
        {
            snapped = tier;
            bestDistance = distance;
        }
    }
    return snapped;
}

uint32_t LoadDirectionalShadowResolutionValue(const nlohmann::json& rendering)
{
    uint32_t requested = kDefaultDirectionalShadowResolution;
    const auto it = rendering.find("directionalShadowResolution");
    try
    {
        if (it != rendering.end() && it->is_number_unsigned())
            requested = it->get<uint32_t>();
        else if (it != rendering.end() && it->is_number_integer())
            requested = static_cast<uint32_t>(std::max(0, it->get<int>()));
        else if (it != rendering.end() && it->is_string())
            requested = static_cast<uint32_t>(std::stoul(it->get<std::string>()));
    }
    catch (...)
    {
        requested = kDefaultDirectionalShadowResolution;
    }

    return NormalizeDirectionalShadowResolution(requested);
}

// Stored as a string so the file stays readable and a future third mode does
// not have to reinterpret a bool.
bool LoadDirectionalShadowStableProjectionValue(const nlohmann::json& rendering)
{
    const auto it = rendering.find("directionalShadowProjection");
    if (it != rendering.end() && it->is_string())
    {
        try
        {
            const std::string mode = it->get<std::string>();
            if (mode == "close")
                return false;
            if (mode == "stable")
                return true;
        }
        catch (...)
        {
        }
    }
    return kDefaultDirectionalShadowStableProjection;
}

// The one value rendering.aaMode could carry that named a per-frame device
// resolution rather than a mode. Retired; understood here and nowhere else, so
// a project still carrying it migrates on its next open and the token is then
// gone. AntiAliasingProjectSettings::ReadFrom reads it as "never chose", which
// is what routes such a project into the materializer below.
constexpr const char* kRetiredAutoAAModeSettingValue = "auto";

// The rendering.msaa value exactly as stored, for a file that has the key at
// all. AntiAliasingProjectSettings::SamplesChosen says whether the parser could
// read it; this says whether there was anything to read, and gives the log
// something to name. The two together separate "named nothing" from "named
// something unusable" without restating the count vocabulary here.
std::optional<std::string> StoredSampleCountValue(const SettingsStore& store)
{
    const nlohmann::json& rendering = FindRenderingSettings(store);
    const auto it = rendering.find("msaa");
    if (it == rendering.end())
        return std::nullopt;
    return it->dump();
}

bool CarriesRetiredAutoToken(const SettingsStore& store)
{
    const nlohmann::json& rendering = FindRenderingSettings(store);
    const auto it = rendering.find("aaMode");
    if (it == rendering.end() || !it->is_string())
        return false;
    std::string value = it->get<std::string>();
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char ch) { return std::isspace(ch) != 0; }),
                value.end());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value == kRetiredAutoAAModeSettingValue;
}

} // namespace

bool MaterializeDefaultAntiAliasing(SettingsStore& store,
                                    Engine::Renderer::AntiAliasingDeviceCaps caps)
{
    Rendering::AntiAliasingProjectSettings settings = GetAntiAliasingSettings(store);

    // Two retirements with two different triggers, because they need different
    // things to answer them. The MODE needs a device. The COUNT does not: the
    // parser already answered an unusable one against the vocabulary alone, and
    // SetDefaultMSAASampleCount clamps whatever lands per machine — so a stored
    // "auto" is rewritten even under a mode the project HAS chosen, which is
    // the case that would otherwise keep the retired token in the file forever.
    const std::optional<std::string> storedCount = StoredSampleCountValue(store);
    const bool writesMode = !settings.ModeChosen && caps.MaxMsaaSamples != 0u;
    const bool rewritesCount = storedCount.has_value() && !settings.SamplesChosen;
    if (!writesMode && !rewritesCount)
        return false;

    const bool migrating = CarriesRetiredAutoToken(store);
    bool msaa = false;
    if (writesMode)
    {
        const Engine::Renderer::ResolvedAntiAliasing resolved =
            Engine::Renderer::ResolveDefaultAntiAliasing(caps);
        settings.ModeChosen = true;
        settings.AAMode = resolved.Mode;
        // ResolveDefaultAntiAliasing yields MSAA or TAA and never Off. The count
        // is honoured only in msaa mode, so the TAA rung leaves the parsed count
        // alone rather than rewriting a count the user may have set.
        msaa = resolved.Mode == Engine::Renderer::AntiAliasingMode::MSAA;
        if (msaa)
            settings.MsaaSamples = resolved.SampleCount;
    }
    SetAntiAliasingSettings(store, settings);

    std::string error;
    if (!store.Save(&error))
    {
        Logger::Log::Error(
            "Editor: could not write the project's anti-aliasing default to '{}': {}",
            store.GetFilePath().string(), error.empty() ? "unknown error" : error);
        return false;
    }

    if (!writesMode)
    {
        Logger::Log::Warning(
            "Editor: project '{}' stored an unusable MSAA sample count {}; rewrote it as {}x. "
            "The count is a concrete stored setting from now on and travels with the project.",
            store.GetFilePath().string(), *storedCount, settings.MsaaSamples);
        return true;
    }

    const std::string chosen =
        msaa ? ("MSAA " + std::to_string(settings.MsaaSamples) + "x") : std::string("TAA");
    if (migrating)
        Logger::Log::Warning(
            "Editor: project '{}' carried the retired anti-aliasing mode 'auto'; rewrote it as "
            "{} for this device ({}x MSAA max). The mode is a concrete stored setting from now "
            "on and travels with the project.",
            store.GetFilePath().string(), chosen, caps.MaxMsaaSamples);
    else
        Logger::Log::Info(
            "Editor: project '{}' had never chosen an anti-aliasing mode; wrote {} for this "
            "device ({}x MSAA max).",
            store.GetFilePath().string(), chosen, caps.MaxMsaaSamples);
    return true;
}

uint32_t GetDirectionalShadowResolution(const SettingsStore& store)
{
    return LoadDirectionalShadowResolutionValue(FindRenderingSettings(store));
}

void SetDirectionalShadowResolution(SettingsStore& store, uint32_t resolution)
{
    nlohmann::json rendering = FindRenderingSettings(store);
    rendering["directionalShadowResolution"] = NormalizeDirectionalShadowResolution(resolution);
    store.SetJson("rendering", rendering);
}

bool GetDirectionalShadowStableProjection(const SettingsStore& store)
{
    return LoadDirectionalShadowStableProjectionValue(FindRenderingSettings(store));
}

void SetDirectionalShadowStableProjection(SettingsStore& store, bool stable)
{
    nlohmann::json rendering = FindRenderingSettings(store);
    rendering["directionalShadowProjection"] = stable ? "stable" : "close";
    store.SetJson("rendering", rendering);
}

void ApplyProjectRenderSettings(Engine::Renderer::RenderServices& renderServices,
                                const SettingsStore& store)
{
    const nlohmann::json& rendering = FindRenderingSettings(store);

    {
        auto& shadowFeature =
            renderServices.EnsureFeature<Engine::Renderer::ShadowMapRenderFeature>();
        shadowFeature.SetProjectResolutionOverride(LoadDirectionalShadowResolutionValue(rendering));
        shadowFeature.SetShadowProjection(
            LoadDirectionalShadowStableProjectionValue(rendering)
                ? Engine::Renderer::ShadowProjection::Stable
                : Engine::Renderer::ShadowProjection::Close);
    }

    // AA + render scale, from the struct that owns their serialized shape —
    // including which env overrides outrank it and how far each one reaches,
    // which is the struct's business because every host needs the same answer.
    //
    // A project that has never chosen a mode takes the capability default,
    // which owns the mode AND the sample count together; ApplyAntiAliasingTo
    // resolves that against this RenderServices' device.
    // MaterializeDefaultAntiAliasing writes the same answer into the file when
    // a project is opened, making the in-memory resolve the fallback for a
    // store applied without one: a device-less host, or a window built before
    // the write landed.
    Rendering::AntiAliasingProjectSettings::ReadFrom(rendering).ApplyTo(renderServices);

    Rendering::LodProjectSettings::ReadFrom(rendering).ApplyTo(renderServices);

    Logger::Log::Info(
        "Editor: applied project render settings from '{}': MSAA {} sample(s), AA mode {}",
        store.GetFilePath().string(), renderServices.GetDefaultMSAASampleCount(),
        static_cast<uint32_t>(renderServices.GetDefaultAntiAliasingMode()));
}

Rendering::AntiAliasingProjectSettings GetAntiAliasingSettings(const SettingsStore& store)
{
    return Rendering::AntiAliasingProjectSettings::ReadFrom(FindRenderingSettings(store));
}

void SetAntiAliasingSettings(SettingsStore& store,
                             const Rendering::AntiAliasingProjectSettings& settings)
{
    nlohmann::json rendering = FindRenderingSettings(store);
    settings.WriteTo(rendering);
    store.SetJson("rendering", rendering);
}

Rendering::AntiAliasingProjectSettings LoadProjectAntiAliasingSettings(
    const std::filesystem::path& workspaceRoot)
{
    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error);
    return GetAntiAliasingSettings(store);
}

bool SaveProjectAntiAliasingSettings(const std::filesystem::path& workspaceRoot,
                                     const Rendering::AntiAliasingProjectSettings& settings)
{
    if (workspaceRoot.empty())
        return false;

    SettingsStore store = OpenProjectSettings(workspaceRoot);
    std::string error;
    (void)store.Load(&error);
    SetAntiAliasingSettings(store, settings);
    if (!store.Save(&error))
    {
        Logger::Log::Error("Editor: failed to save project anti-aliasing settings: {}", error);
        return false;
    }
    return true;
}

void ApplyProjectRenderSettings(Engine::Renderer::RenderServices& renderServices,
                                const std::filesystem::path& projectRoot)
{
    SettingsStore store = OpenProjectSettings(projectRoot);
    std::string error;
    (void)store.Load(&error);
    // Reading the project's own file is the moment its settings are
    // materialized, so this is where a project that has never chosen an
    // anti-aliasing mode gets one written. A no-op for every project that has.
    (void)MaterializeDefaultAntiAliasing(store, renderServices.GetAntiAliasingDeviceCaps());
    ApplyProjectRenderSettings(renderServices, store);
}

} // namespace GameEngine::Editor

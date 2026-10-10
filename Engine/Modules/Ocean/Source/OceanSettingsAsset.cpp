#include "Ocean/OceanSettingsAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace GameEngine::Ocean
{
namespace
{
void SetOceanSettingsError(std::string* error, const char* text)
{
    if (error)
        *error = text;
}

template <typename T>
T Value(const nlohmann::json& object, const char* key, T fallback)
{
    const auto found = object.find(key);
    if (found == object.end())
        return fallback;
    try { return found->get<T>(); } catch (...) { return fallback; }
}
} // namespace

bool OceanSettingsAsset::Validate(std::string* error) const
{
    if (static_cast<uint32>(Kind) > static_cast<uint32>(OceanSettingsKind::Shadows))
    {
        SetOceanSettingsError(error, "unknown ocean settings kind");
        return false;
    }
    if (AnimatedWavesCollision.MaximumQueryPoints == 0u ||
        !std::isfinite(AnimatedWavesCollision.DefaultMinimumSpatialLength) ||
        AnimatedWavesCollision.DefaultMinimumSpatialLength < 0.0f)
    {
        SetOceanSettingsError(error, "invalid animated-wave collision settings");
        return false;
    }
    if (!std::isfinite(DynamicWaves.SimulationFrequency) ||
        DynamicWaves.SimulationFrequency <= 0.0f || DynamicWaves.MaximumSubsteps == 0u ||
        DynamicWaves.CourantNumber <= 0.0f || DynamicWaves.CourantNumber > 1.0f ||
        DynamicWaves.Gravity <= 0.0f || DynamicWaves.MinimumCascade > DynamicWaves.MaximumCascade)
    {
        SetOceanSettingsError(error, "invalid dynamic-wave settings");
        return false;
    }
    if (!std::isfinite(Foam.SimulationFrequency) || Foam.SimulationFrequency <= 0.0f ||
        Foam.MaximumSubsteps == 0u || Foam.FadeRate < 0.0f)
    {
        SetOceanSettingsError(error, "invalid foam settings");
        return false;
    }
    if (!std::isfinite(Shadows.SimulationFrequency) || Shadows.SimulationFrequency <= 0.0f ||
        !std::isfinite(Shadows.TemporalWeight) || Shadows.TemporalWeight < 0.0f ||
        Shadows.TemporalWeight >= 1.0f || !std::isfinite(Shadows.JitterDiameter) ||
        Shadows.JitterDiameter < 0.0f || !std::isfinite(Shadows.HardJitterDiameter) ||
        Shadows.HardJitterDiameter < 0.0f || !std::isfinite(Shadows.HardChannelScale) ||
        Shadows.HardChannelScale < 0.0f || !std::isfinite(Shadows.SoftChannelScale) ||
        Shadows.SoftChannelScale < 0.0f || Shadows.CascadeCount == 0u || Shadows.Resolution == 0u)
    {
        SetOceanSettingsError(error, "invalid ocean shadow settings");
        return false;
    }
    return true;
}

bool OceanSettingsAsset::SaveJson(const std::filesystem::path& path, std::string* error) const
{
    if (!Validate(error))
        return false;
    nlohmann::json data;
    data["FormatVersion"] = kFormatVersion;
    data["Kind"] = static_cast<uint32>(Kind);
    data["AnimatedWavesCollision"] = {
        {"Provider", static_cast<uint32>(AnimatedWavesCollision.Provider)},
        {"MaximumQueryPoints", AnimatedWavesCollision.MaximumQueryPoints},
        {"DefaultMinimumSpatialLength", AnimatedWavesCollision.DefaultMinimumSpatialLength},
        {"AllowGPUQueries", AnimatedWavesCollision.AllowGPUQueries},
        {"AllowBakedFFT", AnimatedWavesCollision.AllowBakedFFT},
        {"AllowGerstnerFallback", AnimatedWavesCollision.AllowGerstnerFallback}};
    data["DynamicWaves"] = {
        {"SimulationFrequency", DynamicWaves.SimulationFrequency},
        {"MaximumSubsteps", DynamicWaves.MaximumSubsteps},
        {"Damping", DynamicWaves.Damping}, {"CourantNumber", DynamicWaves.CourantNumber},
        {"Gravity", DynamicWaves.Gravity}, {"ShallowAttenuation", DynamicWaves.ShallowAttenuation},
        {"HorizontalDisplacement", DynamicWaves.HorizontalDisplacement},
        {"DisplacementClamp", DynamicWaves.DisplacementClamp},
        {"MinimumCascade", DynamicWaves.MinimumCascade},
        {"MaximumCascade", DynamicWaves.MaximumCascade}};
    data["Foam"] = {{"SimulationFrequency", Foam.SimulationFrequency},
                      {"MaximumSubsteps", Foam.MaximumSubsteps}, {"FadeRate", Foam.FadeRate},
                      {"WaveCoverage", Foam.WaveCoverage}, {"WaveStrength", Foam.WaveStrength},
                      {"ShorelineStrength", Foam.ShorelineStrength},
                      {"FlowAdvection", Foam.FlowAdvection}};
    data["Shadows"] = {{"Enabled", Shadows.Enabled},
                       {"SimulationFrequency", Shadows.SimulationFrequency},
                       {"TemporalWeight", Shadows.TemporalWeight},
                       {"JitterDiameter", Shadows.JitterDiameter},
                       {"HardJitterDiameter", Shadows.HardJitterDiameter},
                       {"HardChannelScale", Shadows.HardChannelScale},
                       {"SoftChannelScale", Shadows.SoftChannelScale},
                       {"CascadeCount", Shadows.CascadeCount},
                       {"Resolution", Shadows.Resolution}};
    std::ofstream stream(path, std::ios::trunc);
    if (!stream)
    {
        SetOceanSettingsError(error, "failed to open ocean settings asset for writing");
        return false;
    }
    stream << data.dump(2) << '\n';
    return static_cast<bool>(stream);
}

bool OceanSettingsAsset::LoadJson(const std::filesystem::path& path, std::string* error)
{
    String text;
    if (!ReadFileTextShared(path, text))
    {
        SetOceanSettingsError(error, "failed to open ocean settings asset");
        return false;
    }
    nlohmann::json data = nlohmann::json::parse(text, nullptr, false);
    if (data.is_discarded())
    {
        SetOceanSettingsError(error, "invalid ocean settings JSON");
        return false;
    }
    if (Value<uint32>(data, "FormatVersion", 0u) != kFormatVersion)
    {
        SetOceanSettingsError(error, "unsupported ocean settings format version");
        return false;
    }
    Kind = static_cast<OceanSettingsKind>(Value<uint32>(data, "Kind", 0u));
    const auto& collision = data.value("AnimatedWavesCollision", nlohmann::json::object());
    AnimatedWavesCollision.Provider = static_cast<OceanCollisionProviderMode>(
        Value<uint32>(collision, "Provider", 0u));
    AnimatedWavesCollision.MaximumQueryPoints = Value<uint32>(
        collision, "MaximumQueryPoints", AnimatedWavesCollision.MaximumQueryPoints);
    AnimatedWavesCollision.DefaultMinimumSpatialLength = Value<float32>(
        collision, "DefaultMinimumSpatialLength",
        AnimatedWavesCollision.DefaultMinimumSpatialLength);
    AnimatedWavesCollision.AllowGPUQueries = Value<bool>(collision, "AllowGPUQueries", true);
    AnimatedWavesCollision.AllowBakedFFT = Value<bool>(collision, "AllowBakedFFT", true);
    AnimatedWavesCollision.AllowGerstnerFallback = Value<bool>(
        collision, "AllowGerstnerFallback", true);
    const auto& dynamic = data.value("DynamicWaves", nlohmann::json::object());
    DynamicWaves.SimulationFrequency = Value<float32>(dynamic, "SimulationFrequency", DynamicWaves.SimulationFrequency);
    DynamicWaves.MaximumSubsteps = Value<uint32>(dynamic, "MaximumSubsteps", DynamicWaves.MaximumSubsteps);
    DynamicWaves.Damping = Value<float32>(dynamic, "Damping", DynamicWaves.Damping);
    DynamicWaves.CourantNumber = Value<float32>(dynamic, "CourantNumber", DynamicWaves.CourantNumber);
    DynamicWaves.Gravity = Value<float32>(dynamic, "Gravity", DynamicWaves.Gravity);
    DynamicWaves.ShallowAttenuation = Value<float32>(dynamic, "ShallowAttenuation", DynamicWaves.ShallowAttenuation);
    DynamicWaves.HorizontalDisplacement = Value<float32>(dynamic, "HorizontalDisplacement", DynamicWaves.HorizontalDisplacement);
    DynamicWaves.DisplacementClamp = Value<float32>(dynamic, "DisplacementClamp", DynamicWaves.DisplacementClamp);
    DynamicWaves.MinimumCascade = Value<uint32>(dynamic, "MinimumCascade", DynamicWaves.MinimumCascade);
    DynamicWaves.MaximumCascade = Value<uint32>(dynamic, "MaximumCascade", DynamicWaves.MaximumCascade);
    const auto& foam = data.value("Foam", nlohmann::json::object());
    Foam.SimulationFrequency = Value<float32>(foam, "SimulationFrequency", Foam.SimulationFrequency);
    Foam.MaximumSubsteps = Value<uint32>(foam, "MaximumSubsteps", Foam.MaximumSubsteps);
    Foam.FadeRate = Value<float32>(foam, "FadeRate", Foam.FadeRate);
    Foam.WaveCoverage = Value<float32>(foam, "WaveCoverage", Foam.WaveCoverage);
    Foam.WaveStrength = Value<float32>(foam, "WaveStrength", Foam.WaveStrength);
    Foam.ShorelineStrength = Value<float32>(foam, "ShorelineStrength", Foam.ShorelineStrength);
    Foam.FlowAdvection = Value<float32>(foam, "FlowAdvection", Foam.FlowAdvection);
    const auto& shadows = data.value("Shadows", nlohmann::json::object());
    Shadows.Enabled = Value<bool>(shadows, "Enabled", Shadows.Enabled);
    Shadows.SimulationFrequency = Value<float32>(shadows, "SimulationFrequency", Shadows.SimulationFrequency);
    Shadows.TemporalWeight = Value<float32>(shadows, "TemporalWeight", Shadows.TemporalWeight);
    Shadows.JitterDiameter = Value<float32>(shadows, "JitterDiameter", Shadows.JitterDiameter);
    Shadows.HardJitterDiameter = Value<float32>(shadows, "HardJitterDiameter", Shadows.HardJitterDiameter);
    Shadows.HardChannelScale = Value<float32>(shadows, "HardChannelScale", Shadows.HardChannelScale);
    Shadows.SoftChannelScale = Value<float32>(shadows, "SoftChannelScale", Shadows.SoftChannelScale);
    Shadows.CascadeCount = Value<uint32>(shadows, "CascadeCount", Shadows.CascadeCount);
    Shadows.Resolution = Value<uint32>(shadows, "Resolution", Shadows.Resolution);
    return Validate(error);
}

} // namespace GameEngine::Ocean

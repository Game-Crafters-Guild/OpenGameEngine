#pragma once

#include "Ocean/OceanTypes.h"
#include "Ocean/OceanPresetCache.h"
#include "Ocean/OceanPresetOverrideValidator.h"
#include "Ocean/OceanTime.h"

#include "AssetCore/GUID.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

#include <array>
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Components
{
struct OceanSurface;
} // namespace GameEngine::Components

namespace GameEngine::Ocean
{

class OceanFFTCollisionAsset;
class OceanPresetAsset;
class OceanSettingsAsset;

// Writes the surface's shallow clarity window into the per-frame params, clamped
// to what the shader can evaluate (a zero distance would leave its smoothstep
// undefined; a floor outside [0, 1] would brighten or invert the fog). A non-finite
// value falls back to the field's default (8 m, 0.22).
void PackShallowClarityWindow(const Components::OceanSurface& surface, OceanParamsGPU& params);

// Extracts the ocean state from ECS each frame: finds the OceanSurface (+ its
// wind spectrum), builds the per-frame OceanParamsGPU (FFT controls + appearance),
// and pushes it to the OceanRenderFeature. Also advances the
// base simulation clock; OceanRenderer can scale, offset, or fix the resolved
// ocean time used by waves and stateful ocean simulations.
class OceanExtractionSystem : public ECS::ISystem
{
public:
    explicit OceanExtractionSystem(Engine::Renderer::RenderServices* renderServices);

    const char* GetName() const override { return "OceanExtraction"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    Engine::Renderer::RenderServices* m_RenderServices = nullptr;
    float32 m_Time = 0.0f;

    // Last spectrum-affecting inputs, to detect when the FFT H0 must be re-built.
    // Includes the global wind (resolved from the renderer or spectrum), the
    // gravity multiplier, and the per-octave shaping arrays + disable mask.
    static constexpr uint32 kSpectrumOctaves = 14;
    bool m_HaveLastSpectrum = false;
    float32 m_LastWindSpeed = 0.0f;
    float32 m_LastWindDirDeg = 0.0f;
    float32 m_LastTurbulence = 0.0f;
    float32 m_LastMultiplier = 0.0f;
    float32 m_LastPeriod = 0.0f;
    float32 m_LastGravityMul = 0.0f;
    float32 m_LastSpectrumPower[kSpectrumOctaves] = {};
    float32 m_LastChopScales[kSpectrumOctaves] = {};
    float32 m_LastGravityScales[kSpectrumOctaves] = {};
    uint32 m_LastOctaveDisableMask = 0u;

    bool m_HaveLastLocalSpectrum[kMaxOceanLocalFFTStreams] = {};
    float32 m_LastLocalWindSpeed[kMaxOceanLocalFFTStreams] = {};
    float32 m_LastLocalWindDirDeg[kMaxOceanLocalFFTStreams] = {};
    float32 m_LastLocalTurbulence[kMaxOceanLocalFFTStreams] = {};
    float32 m_LastLocalMultiplier[kMaxOceanLocalFFTStreams] = {};
    float32 m_LastLocalPeriod[kMaxOceanLocalFFTStreams] = {};
    float32 m_LastLocalGravityMul[kMaxOceanLocalFFTStreams] = {};
    float32 m_LastLocalSpectrumPower[kMaxOceanLocalFFTStreams][kSpectrumOctaves] = {};
    float32 m_LastLocalChopScales[kMaxOceanLocalFFTStreams][kSpectrumOctaves] = {};
    float32 m_LastLocalGravityScales[kMaxOceanLocalFFTStreams][kSpectrumOctaves] = {};
    uint32 m_LastLocalOctaveDisableMask[kMaxOceanLocalFFTStreams] = {};

    // Previous-frame world XYZ per OceanWaterInteraction entity, so wake/splash
    // injection can derive horizontal and vertical speed from the position delta
    // (no physics dependency — works for kinematic and simulated bodies alike).
    std::unordered_map<uint32, std::array<float32, 3>> m_PrevInteractionXYZ;

    GUID m_LastCollisionAssetGuid{};
    std::shared_ptr<const OceanFFTCollisionAsset> m_CollisionAsset;
    // Renderer and water-body material presets.
    OceanPresetCache m_Presets;
    GUID m_LastPresetAssetGuid{};
    std::shared_ptr<const OceanPresetAsset> m_PresetAsset;
    OceanPresetOverrideValidator m_PresetOverrideValidator;
    std::array<GUID, 4> m_LastSettingsAssetGuids{};
    std::array<std::shared_ptr<const OceanSettingsAsset>, 4> m_SettingsAssets{};
    std::array<std::filesystem::path, 4> m_SettingsAssetPaths{};
    std::array<std::filesystem::file_time_type, 4> m_SettingsAssetWriteTimes{};
    OceanDefaultTimeProvider m_DefaultTimeProvider;
    OceanCustomTimeProvider m_CustomTimeProvider;
    OceanNetworkOffsetTimeProvider m_NetworkTimeProvider;
    OceanTimelineTimeProvider m_TimelineTimeProvider;
};

} // namespace GameEngine::Ocean

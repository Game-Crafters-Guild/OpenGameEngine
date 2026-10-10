#pragma once

// PP-ARCH Phase 2: the per-volume extraction record and context shared between
// RenderExtractionSystem (which gathers volumes and runs the spatial blend) and
// the per-effect Extract hooks registered on PostProcessEffectDescriptors —
// extraction stops naming effects; each effect's fold lives with its
// registration.

#include "AssetCore/GUID.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/ResolvedShadowSettings.h"
#include "Engine/Rendering/VolumetricFogTypes.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Engine {
namespace Renderer {

class RenderServices;

// One enabled PostProcessVolume entity, flattened for the per-frame spatial
// blend. Volume-core spatial fields (transform decomposition, priority,
// weight) are stamped by the extraction walk BEFORE the effect hooks run, so
// hooks may read them (fog bounds mirror the volume's own shape).
struct PostProcessExtractedVolume
{
    int32 Priority = 0;
    float32 BaseWeight = 0.0f;
    uint32 LayerMask = 0;
    bool IsGlobal = false;
    int32 Shape = 0;
    float32 BlendDistance = 0.0f;
    float32 Center[3]{};
    float32 AxisX[3]{};
    float32 AxisY[3]{};
    float32 AxisZ[3]{};
    float32 HalfExtents[3]{};
    bool ValidSpatial = false;
    bool HasColorGrade = false;
    bool HasHeightFog = false;
    bool HasVolumetricFog = false;
    bool HasAtmosphericCloud = false;
    bool HasVolumetricClouds = false;
    VolumetricFogLocalVolume LocalFogVolume{};
    GUID CubeLutAssetGuid{};
    bool HasCubeLutAsset = false;
    GUID BloomLensDirtAssetGuid{};
    bool HasBloomLensDirtAsset = false;
    // Directional shadow overrides from this volume's ShadowSettingsEffect
    // (HasShadowSettings gates the dominant-volume pick).
    bool HasShadowSettings = false;
    ResolvedShadowSettings ShadowSettings{};
    PostProcessSettings Settings;
};

// Read-only per-world context handed to each effect's Extract hook.
struct PostProcessExtractContext
{
    RenderServices* Services = nullptr; // null in tests / headless worlds
    // World time-of-day (SkyEnvironment), consumed by height fog presets.
    float32 TimeOfDayHours = 12.0f;
    float32 DayKeyTimesHours[4]{0.0f, 6.0f, 12.0f, 18.0f};
};

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine

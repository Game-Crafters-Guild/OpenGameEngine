#pragma once

#include "Components/AssetRef.h"
#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class CubeLutInputEncoding : uint32
{
    Linear = 0,
    Rec709SRGB = 1,
    ArriLogC3 = 2,
    DaVinciWideGamutIntermediate = 3,
    ACEScct = 4,
    Cineon = 5,
};

enum class CubeLutTextureFormat : uint32
{
    R32G32B32A32_FLOAT = 0,
    R16G16B16A16_FLOAT = 1,
};

// Resolve-style .cube color LUT (1D, 3D, or 1D shaper + 3D). Attach to the same entity as PostProcessVolume.
struct CubeLutEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Relative execution order among reorderable LDR post FX.
    int32 StackOrder{0};
    // 0 = bypass, 1 = full LUT blend.
    float32 Intensity{1.0f};
    // Encoding expected by the LUT. Shader converts from/to scene linear around the LUT sample.
    uint32 InputEncoding{static_cast<uint32>(CubeLutInputEncoding::Linear)};
    // GPU texture precision for uploaded LUT tables.
    uint32 TextureFormat{static_cast<uint32>(CubeLutTextureFormat::R32G32B32A32_FLOAT)};
    // The .cube color LUT asset.
    AssetRef<AssetType::CubeLut> LutAssetGuid{};
};

} // namespace Components
} // namespace GameEngine

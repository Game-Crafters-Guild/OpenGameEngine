#pragma once

#include "Types/Types.h"

namespace GameEngine::SceneBvh
{

// Floats per record in the packed buffer. Uber materials, instance records and
// (via a separate stride) TLAS nodes all live in one float array; materials and
// instances deliberately share this stride so a shader indexes both with the
// same multiply.
inline constexpr uint32 kUberMaterialStrideFloats = 28u;

// One flat, shading-model-agnostic material record, carried field-for-field
// from the reference packer so a traversal shader can unpack it by slot index.
//
// Map layer slots address one array-texture layer per map type; -1 means "no
// map bound, use the scalar field". Colours are LINEAR. All the layer slots
// index the SAME array texture, and the one uv transform in [18..21] applies to
// every one of them.
//
// DispersionB and NirAlbedo drive the reference's spectral path tracer and have
// no meaning for diffuse GI; they are retained because the slot indices after
// them are load-bearing for anything that unpacks this record.
struct UberMaterial
{
    float32 BaseColorR = 1.0f;         // [0]
    float32 BaseColorG = 1.0f;         // [1]
    float32 BaseColorB = 1.0f;         // [2]
    float32 Roughness = 1.0f;          // [3]
    float32 Metalness = 0.0f;          // [4]
    float32 Transmission = 0.0f;       // [5]
    float32 Ior = 1.5f;                // [6]
    float32 EmissiveR = 0.0f;          // [7]  scene-linear: tint x nits / referenceWhite
    float32 EmissiveG = 0.0f;          // [8]
    float32 EmissiveB = 0.0f;          // [9]
    float32 Opacity = 1.0f;            // [10]
    float32 DispersionB = 0.0f;        // [11] per-wavelength IOR spread
    float32 AlbedoMapLayer = -1.0f;    // [12]
    float32 NormalMapLayer = -1.0f;    // [13]
    float32 RoughnessMapLayer = -1.0f; // [14]
    float32 MetalnessMapLayer = -1.0f; // [15]
    float32 EmissiveMapLayer = -1.0f;  // [16]
    float32 NormalScale = 1.0f;        // [17]
    float32 UvRepeatX = 1.0f;          // [18]
    float32 UvRepeatY = 1.0f;          // [19]
    float32 UvOffsetX = 0.0f;          // [20]
    float32 UvOffsetY = 0.0f;          // [21]
    float32 Side = 0.0f;               // [22] 0 front, 1 back, 2 double
    float32 AlphaTest = 0.0f;          // [23]
    float32 AlphaMapLayer = -1.0f;     // [24]
    float32 NirAlbedo = -1.0f;         // [25] -1 = untagged
    float32 Reserved0 = 0.0f;          // [26]
    float32 Reserved1 = 0.0f;          // [27]
};

static_assert(sizeof(UberMaterial) == kUberMaterialStrideFloats * sizeof(float32),
              "UberMaterial must be exactly 28 tightly packed floats: TlasPacker copies it "
              "straight into the packed buffer.");

} // namespace GameEngine::SceneBvh

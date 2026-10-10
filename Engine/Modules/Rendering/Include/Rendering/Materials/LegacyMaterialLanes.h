#pragma once

// The one name -> lane placement map for surfaces that read the material
// parameter block by LANE NAME rather than through `// @property` declarations.
//
// Three consumers read it: MaterialRegistry (name -> byte offset in the CPU
// cache), ShaderComposer (the adapter reads it binds on an undeclared surface)
// and the DDGI bake (base colour / metallic / emission of a material it shades
// on the CPU). The GLSL side is Shaders/Includes/material_param_lanes.glsl,
// whose `#define uParamsN uParams[N+1]` aliases are asserted against these
// offsets by LegacyMaterialLaneTests, so a placement that moves on one side
// and not the other fails a test.
//
// A surface with declarations replaces the whole map through
// Material::SetDeclaredProperties, so nothing here applies to it. The table goes
// when every surface — the shader graph included — declares its properties.

#include "Rendering/Materials/MaterialParamsLayout.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine::Rendering
{

struct LegacyMaterialLane
{
    std::string_view Name;
    uint32_t Lane;       // vec4 lane index in the param block
    uint32_t Component;  // first float component within the lane, [0..3]
    uint32_t Components; // floats the name covers (a vec3 colour is 3, so a write never spills into .w)
};

// Byte offset of the name within MaterialGpuParams, and the byte size a write covers.
constexpr uint32_t LegacyLaneByteOffset(const LegacyMaterialLane& lane)
{
    return MaterialParamLaneOffset(lane.Lane) + lane.Component * sizeof(float);
}
constexpr uint32_t LegacyLaneByteSize(const LegacyMaterialLane& lane)
{
    return lane.Components * static_cast<uint32_t>(sizeof(float));
}

// Lane placements of every legacy name, grouped by the surface family that owns
// them. Families that no shipped surface uses together deliberately overlap:
// each addresses the block privately, and a material only ever runs one of them.
inline constexpr std::array<LegacyMaterialLane, 108> kLegacyMaterialLanes = {{
    {"baseColor", 0, 0, 4},
    {"metallic", 1, 0, 1},
    {"roughness", 1, 1, 1},
    {"hexTiling", 1, 2, 1},
    {"hexBlend", 1, 3, 1},
    {"hexRotation", 4, 0, 1},
    {"alphaCutoff", 4, 1, 1},
    {"windStrength", 2, 0, 4},
    {"windParams", 3, 0, 4},
    {"sixAxisScatter", 2, 0, 1},
    {"sixAxisTeaOcclusionBlend", 2, 1, 1},
    {"sixAxisEmissiveScale", 2, 2, 1},
    {"sixAxisDensityScale", 2, 3, 1},
    {"flipbookColumns", 3, 0, 1},
    {"flipbookRows", 3, 1, 1},
    {"flipbookFps", 3, 2, 1},
    {"flipbookStartFrame", 3, 3, 1},
    {"gpuFogSimpleNoiseScale", 1, 0, 1},
    {"gpuFogSimplexNoiseScale", 1, 1, 1},
    {"gpuFogVoronoiScale", 1, 2, 1},
    {"gpuFogCombinedNoiseRemap", 1, 3, 1},
    {"gpuFogSimpleNoiseAmount", 2, 0, 1},
    {"gpuFogSimplexNoiseAmount", 2, 1, 1},
    {"gpuFogVoronoiNoiseAmount", 2, 2, 1},
    {"gpuFogRadialMaskPower", 2, 3, 1},
    {"gpuFogSimpleNoiseRemap", 3, 0, 1},
    {"gpuFogSimplexNoiseRemap", 3, 1, 1},
    {"gpuFogVoronoiNoiseRemap", 3, 2, 1},
    {"gpuFogEdgeSoftness", 3, 3, 1},
    {"gpuFogSimpleAnimationX", 6, 0, 1},
    {"gpuFogSimpleAnimationY", 6, 1, 1},
    {"gpuFogSimpleAnimationZ", 6, 2, 1},
    {"gpuFogSimpleAnimationW", 6, 3, 1},
    {"gpuFogSimplexAnimationX", 7, 0, 1},
    {"gpuFogSimplexAnimationY", 7, 1, 1},
    {"gpuFogSimplexAnimationZ", 7, 2, 1},
    {"gpuFogSimplexAnimationW", 7, 3, 1},
    {"gpuFogVoronoiAnimationX", 8, 0, 1},
    {"gpuFogVoronoiAnimationY", 8, 1, 1},
    {"gpuFogVoronoiAnimationZ", 8, 2, 1},
    {"gpuFogVoronoiAnimationW", 8, 3, 1},
    {"gpuFogSurfaceDepthFade", 5, 0, 1},
    {"gpuFogShapeDistortion", 5, 1, 1},
    {"gpuFogWispyNoiseAmount", 5, 2, 1},
    {"gpuFogDetailNoiseAmount", 5, 3, 1},
    {"gpuFogCameraDepthFadeRange", 9, 0, 1},
    {"gpuFogCameraDepthFadeOffset", 9, 1, 1},
    // Clear-coat params live in uParams9. Anchor the base to the X-macro struct so
    // the offsets follow material_params.glsl automatically (no magic 160/164).
    {"clearCoat", 10, 0, 1}, // uParams9.x
    {"clearCoatRoughness", 10, 1, 1}, // uParams9.y
    {"coatDarkening", 10, 2, 1}, // uParams9.z

    // Sheen params live in uParams10. sheenColor is a vec3 (size 12) in .rgb — NOT 16:
    // its .w is sheenRoughness, a distinct property, so a 4-float color write would clobber it.
    {"sheenColor", 11, 0, 3}, // uParams10.rgb
    {"sheenRoughness", 11, 3, 1}, // uParams10.w

    // Anisotropy lives in uParams11.x (signed [-1..1]); the rotation angle in uParams11.y
    // (radians, revolving the tangent frame in-plane). Anchor to the X-macro struct.
    {"anisotropy", 12, 0, 1}, // uParams11.x
    {"anisotropyRotation", 12, 1, 1}, // uParams11.y

    // Subsurface lives in uParams12. subsurfaceColor is a vec3 (size 12) so a color
    // write never clobbers thickness in .w.
    {"subsurfaceColor", 13, 0, 3}, // uParams12.rgb
    {"thickness", 13, 3, 1}, // uParams12.w

    // OpenPBR base specular (uParams13: rgb=color, w=weight) + base/coat IOR + diffuse roughness
    // (uParams14: x=specularIor, y=clearCoatIor, z=diffuseRoughness). specularColor is a vec3
    // (size 12) so a color write never clobbers weight in .w. Always-on; neutral defaults
    // reproduce the legacy 0.04.
    {"specularColor", 14, 0, 3}, // uParams13.rgb
    {"specularWeight", 14, 3, 1}, // uParams13.w
    {"specularIor", 15, 0, 1}, // uParams14.x
    {"clearCoatIor", 15, 1, 1}, // uParams14.y
    {"diffuseRoughness", 15, 2, 1}, // uParams14.z

    // Transmission (refractive glass) lives in uParams15: rgb=transmissionColor, w=transmissionWeight.
    // transmissionColor is a vec3 (size 12) so a color write never clobbers weight in .w.
    {"transmissionColor", 16, 0, 3}, // uParams15.rgb
    {"transmissionWeight", 16, 3, 1}, // uParams15.w
    // Thick (two-surface) refraction path length in world units (uParams16.x).
    {"refractionDistance", 17, 0, 1}, // uParams16.x
    // Relief depth of the standard surface's height map, a fraction of one height repeat
    // (0 disables the march); zw reserved.
    {"reliefDepth", 17, 1, 1}, // uParams16.y
    // Thin-film iridescence (uParams17): x=thinFilmThickness (nm), y=thinFilmIor, z=thinFilmWeight.
    {"thinFilmThickness", 18, 0, 1}, // uParams17.x
    {"thinFilmIor", 18, 1, 1}, // uParams17.y
    {"thinFilmWeight", 18, 2, 1}, // uParams17.z
    // Emission (uParams18): rgb=emissive colour tint (size 12 so a colour write never clobbers
    // .w), w=emissionLuminance in nits.
    {"emissive", 19, 0, 3}, // uParams18.rgb
    {"emissionLuminance", 19, 3, 1}, // uParams18.w
    // Coat medium tint (uParams19.rgb): tints the through-coat base in the clear-coat lobe (OpenPBR
    // coat_color). vec3 (size 12) so a colour write never touches .w.
    {"coatColor", 20, 0, 3}, // uParams19.rgb
    // How far the emission follows the view's exposure, [0, 1] (uParams19.w): 1 physical, 0 shown
    // at its authored brightness whatever the exposure (adapter_forward.glsl).
    {"emissiveExposureWeight", 20, 3, 1}, // uParams19.w
    // Fuzz params live in uParams20 — same packing as the sheen lobe (uParams10): fuzzColor is a
    // vec3 (size 12) in .rgb so a colour write never clobbers fuzzRoughness in .w. Distinct lobe:
    // fuzz layers OVER the coat (OpenPBR fuzz_*), sheen under it.
    {"fuzzColor", 21, 0, 3}, // uParams20.rgb
    {"fuzzRoughness", 21, 3, 1}, // uParams20.w
    // Thick-glass Beer–Lambert volume absorption (uParams21): rgb=attenuationColor (vec3, size 12 so
    // a colour write never touches .w), w=attenuationDistance (world units). Separate from the
    // uParams15 surface tint — this is OpenPBR transmission_color @ transmission_depth.
    {"attenuationColor", 22, 0, 3}, // uParams21.rgb
    {"attenuationDistance", 22, 3, 1}, // uParams21.w

    // triplanar_pbr front-end (uParams22: per-axis world tiling + axis-blend sharpness;
    // uParams23: per-axis metallic scalar + the ALBEDO layered flag; uParams24: per-axis
    // roughness scalar + the NORMAL layered flag). Albedo and normal layering are
    // independent flags — see triplanar_pbr.glsl on the slot-3 black-default trap.
    // Anchored to the X-macro struct so offsets follow material_params.glsl automatically.
    {"triplanarTilingTop", 23, 0, 1}, // uParams22.x
    {"triplanarTilingSide", 23, 1, 1}, // uParams22.y
    {"triplanarTilingBottom", 23, 2, 1}, // uParams22.z
    {"triplanarBlendSharpness", 23, 3, 1}, // uParams22.w
    {"triplanarMetallicTop", 24, 0, 1}, // uParams23.x
    {"triplanarMetallicSide", 24, 1, 1}, // uParams23.y
    {"triplanarMetallicBottom", 24, 2, 1}, // uParams23.z
    {"triplanarLayered", 24, 3, 1}, // uParams23.w
    {"triplanarRoughnessTop", 25, 0, 1}, // uParams24.x
    {"triplanarRoughnessSide", 25, 1, 1}, // uParams24.y
    {"triplanarRoughnessBottom", 25, 2, 1}, // uParams24.z
    {"triplanarNormalLayered", 25, 3, 1}, // uParams24.w


    // The four generic lanes (uUser0..uUser3) a surface can address without
    // declaring anything: the graph editor's live preview writes a scrubbed pin
    // default here so a value edit is a uniform push instead of a recompile
    // (Editor MaterialGraphPreviewModel), and a hand-authored surface can still
    // read Mat.uUser0.x against the "user0" key. Scalar and vec4 names alias the
    // same bytes on purpose, so a shader can address either way. They go with
    // this whole table once every surface — the shader graph included — declares
    // its properties.
    {"user0", 26, 0, 1}, // uUser0.x
    {"user1", 26, 1, 1}, // uUser0.y
    {"user2", 26, 2, 1}, // uUser0.z
    {"user3", 26, 3, 1}, // uUser0.w
    {"user4", 27, 0, 1}, // uUser1.x
    {"user5", 27, 1, 1}, // uUser1.y
    {"user6", 27, 2, 1}, // uUser1.z
    {"user7", 27, 3, 1}, // uUser1.w
    {"user8", 28, 0, 1}, // uUser2.x
    {"user9", 28, 1, 1}, // uUser2.y
    {"user10", 28, 2, 1}, // uUser2.z
    {"user11", 28, 3, 1}, // uUser2.w
    {"user12", 29, 0, 1}, // uUser3.x
    {"user13", 29, 1, 1}, // uUser3.y
    {"user14", 29, 2, 1}, // uUser3.z
    {"user15", 29, 3, 1}, // uUser3.w
    {"userVec0", 26, 0, 4}, // uUser0
    {"userVec1", 27, 0, 4}, // uUser1
    {"userVec2", 28, 0, 4}, // uUser2
    {"userVec3", 29, 0, 4}, // uUser3
}};

// The first of the four generic lanes (uUser0..uUser3) — the ones a surface can
// address without declaring anything, and the only legacy lanes that carry no
// engine meaning. Names on them are the project-extensibility seam.
inline constexpr uint32_t kGenericLaneFirst = 26;

// The entry for `name`, or nullptr when the name is not a legacy lane.
const LegacyMaterialLane* FindLegacyMaterialLane(std::string_view name);

// The GLSL the composer splices for that placement, e.g. "uParams[4].y",
// "uParams[16].xyz". Indexes the lane array directly, so it is independent of
// the material_param_lanes.glsl alias macros.
std::string LegacyMaterialLaneGlsl(const LegacyMaterialLane& lane);

} // namespace GameEngine::Rendering

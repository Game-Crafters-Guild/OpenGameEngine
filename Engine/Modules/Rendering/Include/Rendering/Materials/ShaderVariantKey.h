#pragma once

// ShaderVariantKey: encodes the combination of vertex format, material
// properties, and rendering mode that together select a unique shader variant.
//
// Lighting model is a StringId (FNV-1a hash of the model name string).
// Adding a new lighting model (e.g. "Toon") requires no C++ code changes —
// just a new GLSL include and a .material file referencing it.

#include "Rendering/Core/HashUtils.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/ShaderProfileDefines.h"
#include "Types/StringId.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// Every copy of a keyword set (a signature, a sort key, a hash input, a log
// line) carries the whole 64-bit field: a narrower copy merges two variants
// that differ only in the bits it drops.
enum class MaterialKeyword : uint64_t
{
    None = 0,

    AlphaTest    = 1ull << 0,  // Alpha masking (discard)
    AlphaBlend   = 1ull << 1,  // Transparent blending: the adapter outputs the surface opacity as alpha (an opaque variant outputs full coverage)
    DoubleSided  = 1ull << 2,  // No backface culling (render-state only, not a SPIR-V variant)
    Instanced    = 1ull << 3,  // Instance-data driven (SSBO fetch vs push constants)
    HasVertexMod       = 1ull << 4,  // Simple vertex modifier (vec3 ModifyVertex)
    ForwardPlus        = 1ull << 5,  // Clustered Forward+ lighting (vs single-light fallback)
    // bit 6 retired (was Bindless): material params + textures are always
    // bindless (shared MaterialParams SSBO), so there is no non-bindless variant.
    Shadows            = 1ull << 7,  // Cascaded shadow map sampling enabled
    HasVertexOutputMod = 1ull << 8,  // Extended vertex output modifier (inout VertexOutput ModifyVertex)
    ProceduralVertexOutput = 1ull << 9, // Vertex output modifier owns draw/instance indexing
    IBL                = 1ull << 10, // Image-based lighting (environment irradiance + specular) enabled
    ClearCoat          = 1ull << 11, // Additive clear-coat lobe on StandardPBR (per-material opt-in)
    Sheen              = 1ull << 12, // Additive Charlie sheen lobe on StandardPBR (per-material opt-in)
    Anisotropy         = 1ull << 13, // Anisotropic GGX reshape of the StandardPBR base lobe (per-material opt-in)
    Subsurface         = 1ull << 14, // Additive NdotL-independent translucency on StandardPBR (per-material opt-in)
    DepthOnlyFragment  = 1ull << 15, // Depth/shadow variant that needs a FRAGMENT stage to decide coverage and attaches NO colour target: the body runs the discards (ALPHA_TEST for a Mask material, GE_LOD_CROSSFADE for a fading tail) and nothing else. It forfeits early-Z on every draw it is compiled into, so the recorder sets it per DRAW SEGMENT, never per pass
    Transmission       = 1ull << 16, // Refractive dielectric (glass) lobe on StandardPBR (per-material opt-in)
    SceneColorGrab     = 1ull << 17, // Pass keyword (transmissive pass only): the post-opaque scene-colour grab is bound (ge_sceneColor), selecting screen-space refraction over the env-cube fallback
    TransmissionThick  = 1ull << 18, // Two-surface (sphere-proxy) thick refraction: walks so.refractionDistance (uParams16.x, world units) through the glass for crystal-ball lensing instead of the single-tap thin bend (implies Transmission)
    DepthOnlyTransmissionColor = 1ull << 19, // Depth/shadow fragment variant (glass-only light-space pass): outputs the glass tint (transmissionColor*weight) into the per-cascade transmittance array for translucent shadows
    GTAO               = 1ull << 20, // Pass keyword (set per-view only when an AO PostProcessVolume is active): the StandardPBR variant declares ge_gtao (set-0 b27) and runs GE_ApplyGTAO, which min()s the screen-space visibility into the surface occlusion; without it, non-AO pipelines pay no fetch and the ambient shades from the material's occlusion map alone (the multi-bounce response is variant-independent)
    Iridescence        = 1ull << 21, // Thin-film interference on the StandardPBR specular Fresnel (per-material opt-in); uParams17 drives film thickness (nm) / IOR / weight
    Fuzz               = 1ull << 22, // Additive Charlie fuzz lobe layered OVER the clear-coat (per-material opt-in); the outermost OpenPBR fuzz layer, distinct from the under-coat Sheen lobe. uParams20 drives fuzzColor / fuzzRoughness
    CoatNormal         = 1ull << 23, // Clear-coat normal map (per-material opt-in): samples coatNormalMap (slot 5) and shades the coat lobe about its own normal while the base keeps the geometry/base normal. Only meaningful alongside ClearCoat; a no-op otherwise.
    CustomVertexShader = 1ull << 24, // Fully-procedural geometry (CBT/LEB, GPU-driven): the vertex modifier supplies all vertex data, there is no vertex buffer. Forces vertexFlags->None, emits CUSTOM_VERTEX_SHADER, routes ge_FetchInstanceData to an identity stub. Always pairs with HasVertexOutputMod (never the simple HasVertexMod form).
    RTShadowMask       = 1ull << 25, // Pass keyword (set per-view only when the RayTraced directional-shadow-mode ray-query lane produced a mask this frame): GE_SampleShadow declares ge_rtShadowMask (set-0 b28) and returns the ray-traced factor instead of cascade sampling; without the keyword the branch compiles out — the variant is bit-identical to the pre-RT shader
    SSSRNormalRoughness = 1ull << 26, // Pass keyword (set per-view only when a PostProcessVolume makes SSSR active): the forward opaque adapter grows three MRT outputs — location 1 (octahedral view normal + roughness + metallic), location 2 (base-lobe specular weight) and location 3 (incident radiance); their alpha channels carry the reflection direction — feeding the screen-space reflection G-buffer. Without the keyword the adapter declares location 0 alone and the variant is bit-identical to the pre-SSSR shader
    LodCrossfade       = 1ull << 27, // Pass keyword, selected per DRAW SEGMENT (BatchDrawRange::Segments marks the crossfade tails; never set view-wide): the vertex adapter unpacks the scatter's per-record fade code and the forward adapter dithers against it, so a level transition dissolves instead of popping. BOTH the depth prepass and the colour pass set it on a tail segment — the shared GE_LodCrossfadeKeep (Includes/lod_crossfade.glsl) is what makes early-Z admit exactly the fragments the colour pass keeps. Only tail records carry a non-zero fade code, and the dither's discard forfeits early-Z on every draw it is compiled into — so heads and forward contributors must never receive it. Without the keyword neither the unpack nor the discard is in the SPIR-V and the variant is bit-identical to the pre-crossfade shader
    DDGI               = 1ull << 28, // Pass keyword (set per-view only when a DDGIVolume component resolved for the view's world): the forward adapter declares the DDGI probe atlas + volume UBO (Includes/ddgi_probes.glsl, set-0 b29/b30) and ibl.glsl folds GE_SampleDDGIIrradiance into the diffuse ambient term; without the keyword neither binding nor the extra sample is in the SPIR-V and the variant is bit-identical to the pre-DDGI shader
    ScreenSpaceShadows = 1ull << 29, // Pass keyword (set per-view only when the view's shadow settings enable screen-space shadows AND the feature produced a mask for this view this frame): GE_SampleShadow declares ge_screenSpaceShadowMask (set-0 b48) and combines the packed depth-validated visibility with the cascade or ray-traced factor; without the keyword neither the binding nor the extra sample is in the SPIR-V and the variant is bit-identical to the pre-contact shader
    MotionVectors      = 1ull << 30, // Pass keyword (set per draw by the producer that writes the motion target): the vertex stage evaluates the vertex modifier at BOTH deformation endpoints — this frame's InstanceData and ge_PreviousEndpoint()'s — projects the previous one through its unjittered view-projection (set-0 b45) and emits that clip position packed into free components of locations 0/2/15, so the variant takes no varying location beyond the 16 every other variant stays inside; the fragment stage recovers the current endpoint from gl_FragCoord, runs the depth-only coverage discard and writes the viewport-space motion delta to ONE colour target instead of shading. Only meaningful alongside a vertex-modifier keyword: a rigid surface's motion comes from the mover lane. Mutually exclusive with DepthOnlyFragment and DepthOnlyTransmissionColor (different attachment shapes; the adapter refuses both by name) and it drops SSSRNormalRoughness, which describes the shading path's G-buffer. Without the keyword neither endpoint nor output is in the SPIR-V and the variant is bit-identical to the pre-motion shader
    Parallax           = 1ull << 31, // Relief march, per material and never authored: ApplyParallaxKeyword (MaterialKeywordDerivation.h) derives it from a bound heightMap on a surface that declares one. The standard surface runs Includes/parallax_occlusion.glsl and samples every map at the displaced UV with explicit gradients. Only the shading shapes and the depth-offset prepass march (GE_PARALLAX_MARCH); the other depth-only shapes, the motion and the glass-tint shapes decide coverage on the polygon. Without the keyword none of it is in the SPIR-V and the variant is bit-identical to the pre-parallax shader
    ParallaxStepsView  = 1ull << 32, // Pass keyword, a debug view (set on the world pass of a Scene View whose Parallax steps view is on; NarrowColorPassKeywords drops it for every material without Parallax, so only the marching variants recompile): the shading path draws the height samples its view march and self-shadow took (Includes/parallax_steps_view.glsl), a palette colour at the lit colour's luminance, in place of the lit colour. Never cooked or prewarmed. Without the keyword none of it is in the SPIR-V and the variant is bit-identical to the pre-view shader
    ParallaxDepthOffset = 1ull << 33, // Pass keyword, set per draw (ChooseDepthHeadPipeline and the world pass, WorldPassReliefDepthKeywords; never for a material without Parallax, never under the compatibility profile): the variant writes the depth of the relief hit along the view ray, layout(depth_less) gl_FragDepth, clamp(z', 0, z). The camera prepass sets it with DepthOnlyFragment; the world pass sets it on the colour pass when no prepass wrote this view's depth (the exact writer), or with ParallaxDepthTolerance when the depth it wrote cannot be read. Shadow cascades never set it: the flat polygon casts. Without the keyword neither depth_less nor gl_FragDepth is in the SPIR-V and the variant is bit-identical to the pre-offset shader
    ParallaxDepthFromPrepass = 1ull << 34, // Pass keyword, the colour pass of a parallax material after a camera prepass that wrote the relief depth, with that depth attached read-only: the variant does not march. It reads the depth the prepass wrote at its pixel (ge_prepassDepth, set-0 b49), rebuilds the relief hit from it (Includes/parallax_depth.glsl), and discards where that depth is something else in front of the relief. It tests at the depth it read (layout(depth_less) gl_FragDepth, one value per pixel as the prepass wrote it) and its pipeline writes no depth. Both passes agree on the hit by construction
    ParallaxPrepassDepthMultisample = 1ull << 35, // Pass keyword, beside ParallaxDepthFromPrepass when the depth is multisampled: ge_prepassDepth is a sampler2DMS and the hit comes from the farthest covered sample, the one the relief wrote
    ParallaxDepthTolerance = 1ull << 36, // Pass keyword, beside ParallaxDepthOffset on the colour pass of a view whose prepass wrote the relief depth but whose world pass keeps depth writable, so it cannot read it (forward contributors that draw no prepass depth): the colour variant marches, writes no depth (its pipeline's depth write is off) and tests at min(z, z' + margin), one march step of depth plus 2^-20 relative, so a march that lands a step from the prepass's leaves no hole
};

inline constexpr MaterialKeyword operator|(MaterialKeyword a, MaterialKeyword b)
{
    return static_cast<MaterialKeyword>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline constexpr MaterialKeyword operator&(MaterialKeyword a, MaterialKeyword b)
{
    return static_cast<MaterialKeyword>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}

inline constexpr MaterialKeyword operator~(MaterialKeyword a)
{
    return static_cast<MaterialKeyword>(~static_cast<uint64_t>(a));
}

inline constexpr MaterialKeyword& operator|=(MaterialKeyword& a, MaterialKeyword b)
{
    a = a | b;
    return a;
}

inline constexpr bool HasKeyword(MaterialKeyword flags, MaterialKeyword test)
{
    return (flags & test) == test;
}

// Well-known lighting model StringId constants.
namespace LightingModel
{
    inline const StringId kUnlit       = HashStringId("unlit");
    inline const StringId kStandardPBR = HashStringId("standardpbr");
    inline const StringId kClearCoat   = HashStringId("clearcoat");
    inline const StringId kShadowOnly  = HashStringId("shadowonly");
}

// Order-independent, non-cancelling combine of a material's enabled user-keyword
// StringIds. Keywords are a SET: de-duplicate + sort so ["A","B"] == ["B","A"],
// then fold with an FNV-1a-style mixer. Raw XOR is forbidden — duplicates would
// cancel ({A,A} == {}), and at the 8-keyword cap structural degeneracy, not
// random collision, is the real risk.
inline uint64_t CombineUserKeywordHash(std::vector<uint64_t> keywordIds)
{
    std::sort(keywordIds.begin(), keywordIds.end());
    keywordIds.erase(std::unique(keywordIds.begin(), keywordIds.end()), keywordIds.end());
    uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
    for (uint64_t id : keywordIds)
    {
        h ^= id;
        h *= 1099511628211ULL; // FNV-1a prime
        h ^= (h >> 29);        // extra avalanche so set structure does not survive
    }
    return keywordIds.empty() ? 0ULL : h;
}

struct ShaderVariantKey
{
    VertexAttributeFlags vertexFlags = VertexAttributeFlags::None;
    MaterialKeyword materialKeywords = MaterialKeyword::None;
    StringId lightingModel = LightingModel::kUnlit;
    // Separate lane for OPEN, author-minted keyword names (§2). The engine
    // MaterialKeyword bitfield is closed; user keywords cannot map onto fixed
    // bits, so their order-independent set hash rides here. Zero when none.
    uint64_t userKeywordHash = 0;

    bool operator==(const ShaderVariantKey& other) const
    {
        return vertexFlags == other.vertexFlags
            && materialKeywords == other.materialKeywords
            && lightingModel == other.lightingModel
            && userKeywordHash == other.userKeywordHash;
    }

    uint64_t Hash() const
    {
        // Every field enters whole through the combine, so no field shifts
        // another's high bits out of the 64-bit result.
        uint64_t h = static_cast<uint64_t>(vertexFlags);
        h = HashUtils::HashCombine(h, static_cast<uint64_t>(materialKeywords));
        h = HashUtils::HashCombine(h, static_cast<uint64_t>(lightingModel));
        h = HashUtils::HashCombine(h, userKeywordHash);
        // The compat profile is process-wide. Its on-disk cache entries fork
        // through the GE_COMPAT_PROFILE define (GenerateDefines), not through
        // this hash; folding it here only keeps a compat key's hash and its
        // stand-in material path apart from the desktop key's.
        if (IsCompatShaderProfile())
            h ^= 0xC0FFEEULL + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

// Apply the customVertexShader clamp to a variant key. Fully-procedural
// geometry (CBT/LEB, GPU-driven) has no vertex buffer, so force the layout to
// None, emit CUSTOM_VERTEX_SHADER, and pin the extended-output modifier form —
// the simple HAS_VERTEX_MODIFIER path assumes mesh attributes. Single source of
// truth for the clamp: every site that builds a key for a procedural material
// calls this rather than open-coding the three fields.
//
// What keeps the two modifier defines from co-emitting is
// ApplyVertexModifierKeyword (MaterialKeywordDerivation.h), which clears both
// bits before setting one; this clamp only overrides its answer for procedural
// geometry, and only for the materials that opt in.
inline void ApplyCustomVertexShaderClamp(ShaderVariantKey& key)
{
    key.vertexFlags = VertexAttributeFlags::None;
    key.materialKeywords = (key.materialKeywords & ~MaterialKeyword::HasVertexMod)
                         | MaterialKeyword::CustomVertexShader
                         | MaterialKeyword::HasVertexOutputMod;
}

// Generate preprocessor #define strings for a variant key.
// `lightingModelName` is the original string (e.g. "StandardPBR") used to
// produce the LIGHTING_MODEL_* define. It must correspond to the StringId
// stored in key.lightingModel.
inline std::vector<std::string> GenerateDefines(const ShaderVariantKey& key,
                                                const std::string& lightingModelName)
{
    std::vector<std::string> defs;

    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasPosition))
        defs.push_back("HAS_POSITION");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasNormal))
        defs.push_back("HAS_NORMAL");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV0))
        defs.push_back("HAS_UV0");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasTangent))
        defs.push_back("HAS_TANGENT");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV1))
        defs.push_back("HAS_UV1");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV2))
        defs.push_back("HAS_UV2");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV3))
        defs.push_back("HAS_UV3");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV4))
        defs.push_back("HAS_UV4");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV5))
        defs.push_back("HAS_UV5");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV6))
        defs.push_back("HAS_UV6");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasUV7))
        defs.push_back("HAS_UV7");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasColor))
        defs.push_back("HAS_COLOR");
    if (IsSkinned(key.vertexFlags))
        defs.push_back("SKINNED");
    if (HasFlag(key.vertexFlags, VertexAttributeFlags::HasJoints1)
        && HasFlag(key.vertexFlags, VertexAttributeFlags::HasWeights1))
        defs.push_back("SKINNED_8");

    if (HasKeyword(key.materialKeywords, MaterialKeyword::AlphaTest))
        defs.push_back("ALPHA_TEST");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::AlphaBlend))
        defs.push_back("GE_ALPHA_BLEND");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Instanced))
        defs.push_back("GE_INSTANCED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexMod))
        defs.push_back("HAS_VERTEX_MODIFIER");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::HasVertexOutputMod))
    {
        defs.push_back("HAS_VERTEX_OUTPUT_MODIFIER");
        defs.push_back("HAS_NORMAL"); // output modifier guarantees normal output
    }
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ProceduralVertexOutput))
        defs.push_back("GE_PROCEDURAL_VERTEX_OUTPUT");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ForwardPlus))
        defs.push_back("FORWARD_PLUS");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Shadows))
        defs.push_back("HAS_SHADOWS");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::IBL))
        defs.push_back("GE_IBL_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ClearCoat))
        defs.push_back("GE_CLEARCOAT_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Sheen))
        defs.push_back("GE_SHEEN_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Anisotropy))
        defs.push_back("GE_ANISOTROPY_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Subsurface))
        defs.push_back("GE_SUBSURFACE_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyFragment))
        defs.push_back("GE_DEPTH_ONLY_FRAGMENT");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Transmission))
        defs.push_back("GE_TRANSMISSION_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::SceneColorGrab))
        defs.push_back("GE_SCENECOLOR_GRAB");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::TransmissionThick))
        defs.push_back("GE_TRANSMISSION_THICK");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyTransmissionColor))
        defs.push_back("GE_GLASS_SHADOW_COLOR");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::GTAO))
        defs.push_back("GE_GTAO_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::RTShadowMask))
        defs.push_back("GE_RT_SHADOW_MASK_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ScreenSpaceShadows))
        defs.push_back("GE_SCREEN_SPACE_SHADOWS_ENABLED");
    // The SSSR G-buffer is a property of the SHADING path: its three outputs sit
    // beside the shading output at locations 1..3, inside the same guard. A
    // variant that shades nothing declares none of them — the depth/shadow
    // coverage variant attaches no colour target, the motion variant attaches the
    // payload alone — so the pass-wide keyword names outputs the program does not
    // have, and composing it fails on an undeclared identifier. The world pass ORs
    // the keyword onto every key for a view with reflections active
    // (WorldRenderNode.cpp), which is how it reaches a key that shades nothing.
    // Dropping it here is what makes such a key compose the same program as the
    // same key without it.
    const bool shadesNothing = HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyFragment)
                            || HasKeyword(key.materialKeywords, MaterialKeyword::MotionVectors);
    if (HasKeyword(key.materialKeywords, MaterialKeyword::SSSRNormalRoughness) && !shadesNothing)
        defs.push_back("GE_SSSR_NORMAL_ROUGHNESS");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Iridescence))
        defs.push_back("GE_IRIDESCENCE_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Fuzz))
        defs.push_back("GE_FUZZ_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::CoatNormal))
        defs.push_back("GE_COAT_NORMAL_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::CustomVertexShader))
        defs.push_back("CUSTOM_VERTEX_SHADER");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::LodCrossfade))
        defs.push_back("GE_LOD_CROSSFADE");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::DDGI))
        defs.push_back("GE_DDGI_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::MotionVectors))
        defs.push_back("GE_MOTION_VECTORS");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::Parallax))
        defs.push_back("GE_PARALLAX_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ParallaxStepsView))
        defs.push_back("GE_PARALLAX_STEPS_VIEW_ENABLED");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ParallaxDepthOffset))
        defs.push_back("GE_PARALLAX_DEPTH_OFFSET");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ParallaxDepthFromPrepass))
        defs.push_back("GE_PARALLAX_DEPTH_FROM_PREPASS");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ParallaxPrepassDepthMultisample))
        defs.push_back("GE_PARALLAX_PREPASS_DEPTH_MULTISAMPLE");
    if (HasKeyword(key.materialKeywords, MaterialKeyword::ParallaxDepthTolerance))
        defs.push_back("GE_PARALLAX_DEPTH_TOLERANCE");

    defs.push_back("LIGHTING_MODEL_" + ToUpperSnakeCase(lightingModelName));

    // Device-wide, not per-material: the compat profile selects the fixed
    // material texture bindings and the derivative/LOD fallbacks that the
    // WebGPU-class target needs (Includes/compat_profile.glsl).
    if (IsCompatShaderProfile())
        defs.push_back("GE_COMPAT_PROFILE");
    // Device-wide as well. Only the relief footprint reads it, so it reaches only the keys that march:
    // a Parallax key in a shading shape or in the depth-offset prepass shape, the condition
    // Includes/surface_io.glsl names GE_PARALLAX_MARCH. Every other program's defines, and its on-disk
    // cache entry, stay the same on any device.
    const bool depthOffsetPrepass = HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyFragment) &&
                                    HasKeyword(key.materialKeywords, MaterialKeyword::ParallaxDepthOffset);
    const bool marchesParallax = HasKeyword(key.materialKeywords, MaterialKeyword::Parallax) &&
                                 (!shadesNothing || depthOffsetPrepass) &&
                                 !HasKeyword(key.materialKeywords, MaterialKeyword::DepthOnlyTransmissionColor);
    if (marchesParallax && AreInterpolationFunctionsAvailable() && !IsCompatShaderProfile())
        defs.push_back("GE_INTERPOLATION_FUNCTIONS");

    return defs;
}

} // namespace Rendering
} // namespace GameEngine

namespace std
{
template <>
struct hash<GameEngine::Rendering::ShaderVariantKey>
{
    size_t operator()(const GameEngine::Rendering::ShaderVariantKey& k) const noexcept
    {
        return static_cast<size_t>(k.Hash());
    }
};
} // namespace std

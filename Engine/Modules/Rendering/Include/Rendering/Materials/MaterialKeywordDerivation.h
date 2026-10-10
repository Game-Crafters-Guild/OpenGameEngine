#pragma once

#include "Rendering/Materials/MaterialAlphaMode.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
struct MaterialDocument;
}

namespace GameEngine::Rendering
{
struct MaterialBuildContext;
struct ShaderVariantKey;
enum class MaterialKeyword : uint64_t;

/// Set the one vertex-modifier keyword a variant key may carry for `vertexModifierRef`.
///
/// The modifier form is a property of the resolved FILE — whether it defines the extended
/// `void ModifyVertex(inout VertexOutput, InstanceData)` or the simple
/// `vec3 ModifyVertex(vec3, InstanceData)` — not of the material document, which only
/// records a reference. Any site that builds a variant key for a modifier material must
/// therefore resolve that reference through the same root chain the composer uses
/// (ShaderComposer::ResolveShaderReference), or its key names a different shader than the
/// one that compiles.
///
/// Exactly one of HasVertexMod / HasVertexOutputMod survives the call and an empty
/// reference clears both: the two defines select mutually exclusive adapter paths, so a
/// key carrying both describes a variant no single derivation produces. The call is
/// idempotent and clears before it sets, which is what lets it normalise a key that
/// reached it with the other form already on (pipeline keywords merged in by a caller).
///
/// An unresolvable or unreadable reference takes the simple form.
///
/// `materialDir` may be empty when the material has no asset path; the material-relative
/// probe is then skipped and resolution walks project roots -> packages -> engine tree.
void ApplyVertexModifierKeyword(ShaderVariantKey& key,
                                const std::string& vertexModifierRef,
                                const std::filesystem::path& materialDir,
                                const MaterialBuildContext& context);

/// Add the keyword a material's alpha mode compiles into its variant key: AlphaTest for
/// Mask (the adapter discards below the cutoff) and AlphaBlend for Blend (the adapter
/// keeps the surface opacity as the output alpha the blend state reads). Opaque adds
/// neither, and its output alpha is full coverage. Every site that derives a key from a
/// material's alpha mode calls this, so registration, prewarm and the build service
/// agree on the variant.
void ApplyAlphaModeKeyword(ShaderVariantKey& key, MaterialAlphaMode alphaMode);

/// The texture name the relief march reads its height map through, on every surface that
/// declares one (`// @texture heightMap`).
inline constexpr const char* kParallaxHeightMapSlot = "heightMap";

/// Why ApplyParallaxKeyword left a bound height map without the march.
enum class ParallaxRefusal : uint8_t
{
    None,
    SurfaceHasNoHeightMap, // the surface declares no heightMap slot
    HexTiling,             // hex tiling is on, and the two do not combine
};

/// Set MaterialKeyword::Parallax exactly when `doc` binds a `heightMap` and the surface declares
/// that slot; clear it otherwise. Binding the map is the whole opt-in: there is no enable flag,
/// and a relief depth of 0 still compiles the variant.
///
/// `surfaceSlots` is the resolved `@texture` set of the surface `doc` composes with
/// (ShaderComposer::ResolveTextureSlots), or null when that surface does not resolve, cannot be
/// read or is rejected. Nothing is refused then: the compose reports that surface by name, and a
/// height-map refusal would name the wrong cause. The caller owns the surface read, so
/// registration derives the keyword from the same table it installs as the material's slot map.
ParallaxRefusal ApplyParallaxKeyword(ShaderVariantKey& key,
                                     const MaterialDocument& doc,
                                     const std::vector<std::pair<std::string, uint8_t>>* surfaceSlots);

/// The message for a refusal, stating the fix; empty for ParallaxRefusal::None. `surfaceName` is
/// the surface as the user knows it, its root-relative reference without the extension
/// ("Surfaces/triplanar_pbr").
std::string DescribeParallaxRefusal(ParallaxRefusal refusal, const std::string& surfaceName);

/// The pass keywords a colour variant of a material whose own keywords are `materialKeywords`
/// compiles with. The Parallax steps view (MaterialKeyword::ParallaxStepsView) and the relief's depth
/// (ParallaxDepthOffset, ParallaxDepthFromPrepass, ParallaxPrepassDepthMultisample, ParallaxDepthTolerance) reach only a material that marches, so turning the
/// view on recompiles the parallax variants alone and every other material keeps the variant it
/// already draws. The compatibility profile never carries the relief's depth: WGSL has no conservative
/// depth, and a depth-writing fragment there would lose early depth testing.
MaterialKeyword NarrowColorPassKeywords(MaterialKeyword materialKeywords, MaterialKeyword passKeywords);

} // namespace GameEngine::Rendering

#pragma once

#include "Engine/Rendering/DepthDrawRecorder.h"
#include "Engine/Rendering/ParallaxReliefDepth.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Types/Types.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer
{
// Shader-only warm-up keys, shared by demand registration and project preload.
// Preserve the entire base key, especially authored keyword hashes.
//
// Colour: the pass keywords over the standard, tangent and skinned layouts, the
// asset preview pass's keywords over the same layouts, the transmissive pass's
// scene-colour-grab variants, and the LOD-crossfade tails of both passes (their
// keywords plus LodCrossfade). Depth: whatever the depth recorder
// draws the material with (FilterDepthPassMaterial, ChooseDepthHeadPipeline,
// ChooseDepthSegmentPipeline) in the camera prepass, the shadow passes and the
// glass tint pass, on the layout the recorder draws each head with
// (DepthHeadVertexFlags): for every colour layout warmed here, the depth pass's
// stripped layout, or the colour layout itself for a vertex-modified material,
// whose own layout is warmed too since its modifier may read any of its streams.
// A head that takes the shared depth pipeline compiles nothing of the material's own.
// Crossfade tails are warmed only for the passes that draw them
// (DepthPassDrawsCrossfadeTails), on every layout since skinned meshes draw them
// too; fully-procedural materials have no LOD chain and get none.
// A Mask material whose coverage reads vertex colour (DepthPassReadsVertexColor)
// draws its depth heads with the colour stream beside a colour layout that has
// one, so those heads and tails are warmed on each warmed colour layout with
// HasColor added too. A material that ignores vertex colour (`ignoresVertexColor`)
// never binds the stream (BoundStreamVertexFlags) and gets none.
//
// Not warmed: the colour pass on the vertex-colour layouts (HasColor), which it
// draws for a mesh with a colour stream; a vertex-modified material's depth heads
// on those layouts, which draw with the colour layout itself; the
// deforming-motion pass (drawn only for a view that renders motion vectors); the
// per-view pass keywords a volume or view setting adds; and the asset preview
// with its environment-lighting toggle on (Instanced | IBL).
//
// The colour half is hand-kept: the world pass keywords arrive as `worldPass`, but
// the grab, crossfade, preview and relief-depth keywords below restate rules their
// passes apply inline. A parallax material's world pass is warmed as the shipped
// pipelines draw it, after the camera prepass: it reads the prepass's relief depth,
// multisampled and single-sampled (ParallaxDepthFromPrepass), or, in a view whose
// forward contributors (CBT terrain, terrain grass) keep the depth writable, marches
// and tests with a tolerance (ParallaxDepthTolerance). A pipeline without a prepass
// compiles its variant on first draw.

// The editor's asset preview pass (ModelThumbnailHandler, the material and model
// previews and thumbnails) draws its world pass with Instanced alone: no
// ForwardPlus (the preview view runs no cluster cull) and no Shadows. Like any
// world pass it draws crossfading tails with LodCrossfade added. The rule is
// inline in that pass (previewKeywords); keep this in step with it.
inline constexpr Rendering::MaterialKeyword kAssetPreviewPassKeywords = Rendering::MaterialKeyword::Instanced;

inline Vector<Rendering::ShaderVariantKey> MaterialPrewarmVariantKeys(
    const Rendering::ShaderVariantKey& base, Rendering::MaterialKeyword worldPass,
    bool customVertexShader, MaterialAlphaMode alphaMode, bool ignoresVertexColor = false)
{
    using Rendering::MaterialKeyword;
    using Rendering::VertexAttributeFlags;
    Vector<Rendering::ShaderVariantKey> keys;
    const auto add = [&](VertexAttributeFlags flags, MaterialKeyword extra) {
        auto key = base;
        key.vertexFlags = flags;
        key.materialKeywords |= extra;
        if (customVertexShader)
            Rendering::ApplyCustomVertexShaderClamp(key);
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    };
    const bool transmission = Rendering::HasKeyword(base.materialKeywords, MaterialKeyword::Transmission);
    // A parallax material's colour pass carries the relief's depth: read from the camera prepass the
    // world pass runs, written in the asset preview, which runs no prepass.
    const MaterialKeyword colorPass =
        worldPass | WorldPassReliefDepthKeywords(base.materialKeywords, alphaMode,
                                                 ReliefDepthSource::PrepassReadableMultisample);
    const MaterialKeyword colorPassSingleSample =
        worldPass |
        WorldPassReliefDepthKeywords(base.materialKeywords, alphaMode, ReliefDepthSource::PrepassReadable);
    const MaterialKeyword colorPassTolerant =
        worldPass |
        WorldPassReliefDepthKeywords(base.materialKeywords, alphaMode, ReliefDepthSource::PrepassUnreadable);
    const MaterialKeyword previewPass =
        kAssetPreviewPassKeywords |
        WorldPassReliefDepthKeywords(base.materialKeywords, alphaMode, ReliefDepthSource::NoPrepass);
    add(base.vertexFlags, MaterialKeyword::None);
    constexpr VertexAttributeFlags standard =
        VertexAttributeFlags::HasPosition | VertexAttributeFlags::HasNormal | VertexAttributeFlags::HasUV0;
    constexpr VertexAttributeFlags layouts[] = {
        standard, standard | VertexAttributeFlags::HasTangent, standard | VertexAttributeFlags::Skinned,
        standard | VertexAttributeFlags::HasTangent | VertexAttributeFlags::Skinned};
    for (const VertexAttributeFlags layout : layouts)
    {
        add(layout, colorPass);
        add(layout, colorPassSingleSample);
        add(layout, colorPassTolerant);
        add(layout, previewPass);
        if (transmission)
            add(layout, colorPass | MaterialKeyword::SceneColorGrab);
        if (customVertexShader)
            continue;
        add(layout, colorPass | MaterialKeyword::LodCrossfade);
        add(layout, colorPassSingleSample | MaterialKeyword::LodCrossfade);
        add(layout, colorPassTolerant | MaterialKeyword::LodCrossfade);
        add(layout, previewPass | MaterialKeyword::LodCrossfade);
        if (transmission)
            add(layout, colorPass | MaterialKeyword::SceneColorGrab | MaterialKeyword::LodCrossfade);
    }

    const MaterialKeyword depthPassKeywords = worldPass & MaterialKeyword::Instanced;
    const bool alphaTest = alphaMode == MaterialAlphaMode::Mask;
    const bool writesReliefDepth = WritesReliefDepth(base.materialKeywords);
    // The procedural clamp gives a fully-procedural material the extended modifier form.
    const bool vertexModified = customVertexShader ||
                                Rendering::HasKeyword(base.materialKeywords, MaterialKeyword::HasVertexMod) ||
                                Rendering::HasKeyword(base.materialKeywords, MaterialKeyword::HasVertexOutputMod);
    constexpr DepthPassType depthPasses[] = {DepthPassType::Prepass, DepthPassType::ShadowCascade,
                                             DepthPassType::TransmittanceCascade};
    // The colour layouts a depth head is drawn beside; the material's own layout too when its modifier may
    // read a stream the fixed layouts lack (a vertex-modified material draws its depth heads with it).
    Vector<VertexAttributeFlags> depthColorLayouts(std::begin(layouts), std::end(layouts));
    if (vertexModified && std::find(depthColorLayouts.begin(), depthColorLayouts.end(), base.vertexFlags) ==
                              depthColorLayouts.end())
        depthColorLayouts.push_back(base.vertexFlags);
    for (const DepthPassType pass : depthPasses)
    {
        if (FilterDepthPassMaterial(pass, alphaMode, transmission) != DepthPassMaterialFilter::Draws)
            continue;
        const bool sharedDepthOffered = pass != DepthPassType::Prepass || PrepassSharedDepthEnabled();
        const DepthSegmentPipelineChoice head =
            ChooseDepthHeadPipeline(depthPassKeywords, pass, alphaTest, vertexModified, sharedDepthOffered,
                                    writesReliefDepth);
        const bool coverageReadsVertexColor = DepthPassReadsVertexColor(pass, alphaMode);
        // The heads that keep the colour stream of a colour layout that has one: a vertex-modified
        // material's heads draw with the colour layout itself instead, and are not warmed on it.
        const bool warmsVertexColorHeads = coverageReadsVertexColor && !ignoresVertexColor && !vertexModified;
        const bool drawsTails = !customVertexShader && DepthPassDrawsCrossfadeTails(pass);
        const MaterialKeyword tailKeywords =
            drawsTails
                ? ChooseDepthSegmentPipeline(head.Keywords, head.UseSharedDepth, head.ComposesFragment, true).Keywords
                : MaterialKeyword::None;
        const auto addHead = [&](VertexAttributeFlags colorLayout) {
            const VertexAttributeFlags drawn = DepthHeadVertexFlags(
                StrippedDepthVertexFlags(colorLayout), colorLayout, vertexModified, coverageReadsVertexColor,
                head.Keywords);
            if (!head.UseSharedDepth)
                add(drawn, head.Keywords);
            if (drawsTails)
                add(drawn, tailKeywords);
        };
        for (const VertexAttributeFlags colorLayout : depthColorLayouts)
        {
            addHead(colorLayout);
            if (warmsVertexColorHeads)
                addHead(colorLayout | VertexAttributeFlags::HasColor);
        }
    }
    return keys;
}
} // namespace GameEngine::Engine::Renderer

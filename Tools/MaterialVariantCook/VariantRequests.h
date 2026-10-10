#pragma once

#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace GameEngine::Tools::MaterialCook
{
struct VariantRequest
{
    std::string Name;
    Rendering::MaterialKeyword PassKeywords;
    Rendering::VertexAttributeFlags VertexFlags;
};

inline std::vector<VariantRequest> SelectVariantRequests(
    std::span<const VariantRequest> meshRows, std::span<const VariantRequest> contributorRows,
    std::span<const VariantRequest> maskedMeshDepthRows, MaterialAlphaMode alphaMode)
{
    const auto selected = contributorRows.empty() ? meshRows : contributorRows;
    std::vector<VariantRequest> rows(selected.begin(), selected.end());
    // Contributor rows describe their own geometry/pass contract: mesh depth
    // layouts cannot be appended to terrain or procedural grass. This does NOT
    // suppress alpha mode: BuildMaterialToShaderPackage applies the document's
    // AlphaTest keyword to every selected row, including contributor rows.
    if (contributorRows.empty() && alphaMode == MaterialAlphaMode::Mask)
        rows.insert(rows.end(), maskedMeshDepthRows.begin(), maskedMeshDepthRows.end());
    return rows;
}

// The vertex-colour rows of a mesh material that reads vertex colour: every row whose stages read the
// colour stream when the mesh has one, again with HasColor. Those are the colour pass rows (a mesh with a
// COLOR_0 stream draws its colour pass with it, BoundStreamVertexFlags) and a Mask material's coverage
// depth rows (DepthPassReadsVertexColor, DepthHeadVertexFlags). An opaque depth row draws the stripped
// layout and reads no colour. `readsVertexColor` is false for a material no mesh with a colour stream
// reaches on a cooked-only runtime (only the imported mesh materials do) and for one that ignores vertex
// colour; such a material lists none.
inline void ExpandVertexColorVariants(std::vector<VariantRequest>& rows, bool readsVertexColor)
{
    using Rendering::MaterialKeyword;
    using Rendering::VertexAttributeFlags;
    if (!readsVertexColor)
        return;
    const size_t count = rows.size();
    for (size_t i = 0; i < count; ++i)
    {
        const auto row = rows[i];
        const bool colourPass = Rendering::HasKeyword(row.PassKeywords, MaterialKeyword::ForwardPlus);
        const bool coverageDepth = Rendering::HasKeyword(row.PassKeywords, MaterialKeyword::DepthOnlyFragment);
        const auto flags = row.VertexFlags | VertexAttributeFlags::HasColor;
        if ((!colourPass && !coverageDepth) || flags == row.VertexFlags ||
            std::any_of(rows.begin(), rows.end(), [&](const VariantRequest& existing)
                        { return existing.PassKeywords == row.PassKeywords && existing.VertexFlags == flags; }))
            continue;
        rows.push_back({row.Name + "+color", row.PassKeywords, flags});
    }
}

inline void ExpandOptionalPassVariants(std::vector<VariantRequest>& rows)
{
    using Rendering::MaterialKeyword;
    struct OptionalPassVariant
    {
        MaterialKeyword Required;
        MaterialKeyword Enabled;
        const char* Suffix;
    };
    // A produced contact mask changes descriptor layout. Cook both on/off
    // receiver variants; retain all existing lighting bits and vertex layouts.
    constexpr OptionalPassVariant options[] = {
        {MaterialKeyword::Shadows, MaterialKeyword::ScreenSpaceShadows, "-contact-shadows"}};
    for (const auto& option : options)
    {
        const size_t count = rows.size();
        for (size_t i = 0; i < count; ++i)
        {
            const auto& row = rows[i];
            if (!Rendering::HasKeyword(row.PassKeywords, option.Required) ||
                Rendering::HasKeyword(row.PassKeywords, option.Enabled) ||
                Rendering::HasKeyword(row.PassKeywords, MaterialKeyword::DepthOnlyFragment))
                continue;
            const auto keywords = row.PassKeywords | option.Enabled;
            const auto flags = row.VertexFlags;
            if (std::any_of(rows.begin(), rows.end(), [&](const VariantRequest& existing)
                { return existing.PassKeywords == keywords && existing.VertexFlags == flags; }))
                continue;
            rows.push_back({row.Name + option.Suffix, keywords, flags});
        }
    }
}

// The relief's depth rows of a height-mapped mesh material, for a desktop cook: every camera depth row
// again as the prepass head that marches and writes the relief's depth (DepthOnlyFragment |
// ParallaxDepthOffset), and every forward colour row again as the world pass draws it after that
// prepass: reading the prepass's depth, multisampled and single-sampled (ParallaxDepthFromPrepass, with
// and without ParallaxPrepassDepthMultisample), and marching with a tolerance where forward contributors
// keep the depth writable (ParallaxDepthOffset | ParallaxDepthTolerance). A web cook lists none: the compatibility
// profile never carries the relief's depth. Declared like the rest of the table; the build narrows the
// keywords away from a material whose surface does not march.
inline void ExpandParallaxDepthVariants(std::vector<VariantRequest>& rows, bool bindsHeightMap, bool web)
{
    using Rendering::MaterialKeyword;
    if (!bindsHeightMap || web)
        return;
    const size_t count = rows.size();
    for (size_t i = 0; i < count; ++i)
    {
        const auto row = rows[i];
        const MaterialKeyword coverage = row.PassKeywords & ~MaterialKeyword::DepthOnlyFragment;
        if (Rendering::HasKeyword(row.PassKeywords, MaterialKeyword::ForwardPlus))
        {
            rows.push_back({row.Name + "-prepass-depth", row.PassKeywords | MaterialKeyword::ParallaxDepthFromPrepass,
                            row.VertexFlags});
            rows.push_back({row.Name + "-prepass-depth-msaa",
                            row.PassKeywords | MaterialKeyword::ParallaxDepthFromPrepass |
                                MaterialKeyword::ParallaxPrepassDepthMultisample,
                            row.VertexFlags});
            rows.push_back({row.Name + "-prepass-tolerance",
                            row.PassKeywords | MaterialKeyword::ParallaxDepthOffset |
                                MaterialKeyword::ParallaxDepthTolerance,
                            row.VertexFlags});
        }
        else if (coverage == MaterialKeyword::Instanced)
            rows.push_back({row.Name + "-depth-offset",
                            row.PassKeywords | MaterialKeyword::DepthOnlyFragment | MaterialKeyword::ParallaxDepthOffset,
                            row.VertexFlags});
    }
}
} // namespace GameEngine::Tools::MaterialCook

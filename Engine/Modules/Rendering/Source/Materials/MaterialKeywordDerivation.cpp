#include "Rendering/Materials/MaterialKeywordDerivation.h"

#include "Rendering/Materials/MaterialBuildContext.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderCapabilityDetector.h"
#include "Rendering/Materials/ShaderComposer.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <algorithm>
#include <variant>

namespace GameEngine::Rendering
{

namespace
{
// Hex tiling as the standard surface reads it: on when the hexTiling lane is at least 0.5.
// Documents author it as a float; the registry lands a bool or an int as 1 or 0.
bool IsHexTilingOn(const MaterialDocument& doc)
{
    const auto it = doc.properties.find("hexTiling");
    if (it == doc.properties.end())
        return false;
    if (const float* value = std::get_if<float>(&it->second))
        return *value >= 0.5f;
    if (const int32_t* value = std::get_if<int32_t>(&it->second))
        return *value >= 1;
    if (const bool* value = std::get_if<bool>(&it->second))
        return *value;
    return false;
}

bool DeclaresHeightMap(const std::vector<std::pair<std::string, uint8_t>>& slots)
{
    return std::any_of(slots.begin(), slots.end(),
                       [](const auto& slot) { return slot.first == kParallaxHeightMapSlot; });
}
} // namespace

void ApplyVertexModifierKeyword(ShaderVariantKey& key,
                                const std::string& vertexModifierRef,
                                const std::filesystem::path& materialDir,
                                const MaterialBuildContext& context)
{
    key.materialKeywords =
        key.materialKeywords & ~(MaterialKeyword::HasVertexMod | MaterialKeyword::HasVertexOutputMod);

    if (vertexModifierRef.empty())
        return;

    const std::filesystem::path modifierPath =
        ShaderComposer::ResolveShaderReference(vertexModifierRef, materialDir, context);
    const ShaderCapability caps = ShaderCapabilityDetector::DetectFromFile(modifierPath.string());

    key.materialKeywords |= HasCapability(caps, ShaderCapability::VertexOutputModifier)
                                ? MaterialKeyword::HasVertexOutputMod
                                : MaterialKeyword::HasVertexMod;
}

void ApplyAlphaModeKeyword(ShaderVariantKey& key, MaterialAlphaMode alphaMode)
{
    if (alphaMode == MaterialAlphaMode::Mask)
        key.materialKeywords |= MaterialKeyword::AlphaTest;
    else if (alphaMode == MaterialAlphaMode::Blend)
        key.materialKeywords |= MaterialKeyword::AlphaBlend;
}

ParallaxRefusal ApplyParallaxKeyword(ShaderVariantKey& key,
                                     const MaterialDocument& doc,
                                     const std::vector<std::pair<std::string, uint8_t>>* surfaceSlots)
{
    key.materialKeywords = key.materialKeywords & ~MaterialKeyword::Parallax;

    const auto binding = doc.textures.find(kParallaxHeightMapSlot);
    if (binding == doc.textures.end() || binding->second.empty() || !surfaceSlots)
        return ParallaxRefusal::None;
    if (!DeclaresHeightMap(*surfaceSlots))
        return ParallaxRefusal::SurfaceHasNoHeightMap;
    if (IsHexTilingOn(doc))
        return ParallaxRefusal::HexTiling;

    key.materialKeywords |= MaterialKeyword::Parallax;
    return ParallaxRefusal::None;
}

std::string DescribeParallaxRefusal(ParallaxRefusal refusal, const std::string& surfaceName)
{
    switch (refusal)
    {
    case ParallaxRefusal::SurfaceHasNoHeightMap:
        return surfaceName + " does not use a Height map; bind it on a Standard PBR material";
    case ParallaxRefusal::HexTiling:
        return "Parallax cannot combine with hex tiling: turn Hex Tiling off, or remove the Height map";
    case ParallaxRefusal::None:
        break;
    }
    return {};
}

MaterialKeyword NarrowColorPassKeywords(MaterialKeyword materialKeywords, MaterialKeyword passKeywords)
{
    constexpr MaterialKeyword parallaxDepth = MaterialKeyword::ParallaxDepthOffset |
                                              MaterialKeyword::ParallaxDepthFromPrepass |
                                              MaterialKeyword::ParallaxPrepassDepthMultisample |
                                              MaterialKeyword::ParallaxDepthTolerance;
    if (!HasKeyword(materialKeywords, MaterialKeyword::Parallax))
        passKeywords = passKeywords & ~(MaterialKeyword::ParallaxStepsView | parallaxDepth);
    if (IsCompatShaderProfile())
        passKeywords = passKeywords & ~parallaxDepth;
    return passKeywords;
}

} // namespace GameEngine::Rendering

#include "Editor/Materials/MaterialRows.h"

#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <variant>

namespace GameEngine::Editor::MaterialRows
{

namespace
{

bool UsesVolumetricSixAxisSurface(const MaterialDocument& doc)
{
    const std::string s = ToLowerAscii(doc.surfaceShader);
    return s.find("volumetric_six_axis") != std::string::npos;
}

// camelCase -> spaced PascalCase, the shared half of both label mappings.
std::string SplitCamelCase(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (i == 0)
            out.push_back(static_cast<char>(std::toupper(c)));
        else if (std::isupper(c))
        {
            out.push_back(' ');
            out.push_back(static_cast<char>(c));
        }
        else
            out.push_back(static_cast<char>(c));
    }
    return out;
}

} // namespace

bool IsLightingModelUnlit(const std::string& lightingModel)
{
    return ToLowerAscii(lightingModel) == "unlit";
}

bool IsLightingModelShadowOnly(const std::string& lightingModel)
{
    return ToLowerAscii(lightingModel) == "shadowonly";
}

const std::vector<std::string>& BuiltInPropertyOrder()
{
    static const std::vector<std::string> order = {
        "baseColor",
        "opacity",
        // Directly under `opacity`: the alpha test compares the two, and the
        // pair only reads as a threshold when they sit together.
        "alphaCutoff",
        "sixAxisScatter",
        "sixAxisTeaOcclusionBlend",
        "sixAxisEmissiveScale",
        "sixAxisDensityScale",
        "flipbookColumns",
        "flipbookRows",
        "flipbookFps",
        "flipbookStartFrame",
        "roughness",
        "diffuseRoughness",
        "metallic",
        "emissive",
        "emissionLuminance",
        "emissiveExposureWeight",
        "ao",
        "specularColor",
        "specularWeight",
        "specularIor",
        "enableClearCoat",
        "clearCoat",
        "clearCoatRoughness",
        "coatDarkening",
        "coatColor",
        "clearCoatIor",
        "enableSheen",
        "sheenColor",
        "sheenRoughness",
        "enableFuzz",
        "fuzzColor",
        "fuzzRoughness",
        "enableAnisotropy",
        "anisotropy",
        "anisotropyRotation",
        "enableSubsurface",
        "subsurfaceColor",
        "thickness",
        "enableTransmission",
        "transmissionColor",
        "transmissionWeight",
        "enableTransmissionThick",
        "refractionDistance",
        "attenuationColor",
        "attenuationDistance",
        "enableIridescence",
        "thinFilmThickness",
        "thinFilmIor",
        "thinFilmWeight",
    };
    return order;
}

const std::unordered_set<std::string>& BuiltInPropertyNames()
{
    static const std::unordered_set<std::string> names = {
        "baseColor",
        "roughness",
        "metallic",
        "emissive",
        "emissionLuminance",
        "emissiveExposureWeight",
        "ao",
        "opacity",
        "alphaCutoff",
        "sixAxisScatter",
        "sixAxisTeaOcclusionBlend",
        "sixAxisEmissiveScale",
        "sixAxisDensityScale",
        "flipbookColumns",
        "flipbookRows",
        "flipbookFps",
        "flipbookStartFrame",
        "hexTiling",
        "hexBlend",
        "hexRotation",
        "enableClearCoat",
        "clearCoat",
        "clearCoatRoughness",
        "coatDarkening",
        "enableSheen",
        "sheenColor",
        "sheenRoughness",
        "enableFuzz",
        "fuzzColor",
        "fuzzRoughness",
        "enableAnisotropy",
        "anisotropy",
        "enableSubsurface",
        "subsurfaceColor",
        "thickness",
        "enableTransmission",
        "transmissionColor",
        "transmissionWeight",
        "enableTransmissionThick",
        "refractionDistance",
        "attenuationColor",
        "attenuationDistance",
        "enableIridescence",
        "thinFilmThickness",
        "thinFilmIor",
        "thinFilmWeight",
        "specularColor",
        "specularWeight",
        "specularIor",
        "clearCoatIor",
        "diffuseRoughness",
        // Laid out under the Height row in the Textures section, not with the
        // property rows (IsReliefDepthVisible).
        "reliefDepth",
    };
    return names;
}

const std::vector<std::string>& TextureSlotOrder()
{
    static const std::vector<std::string> order = {
        "albedoMap", "normalMap", "coatNormalMap", "metallicRoughnessMap", "heightMap", "emissiveMap", "aoMap"
    };
    return order;
}

bool IsPropertyVisible(const std::string& key, const MaterialDocument& doc)
{
    const bool unlit = IsLightingModelUnlit(doc.lightingModel);
    const bool shadowOnly = IsLightingModelShadowOnly(doc.lightingModel);
    const bool sixAxis = UsesVolumetricSixAxisSurface(doc) && unlit;

    // `opacity` is shown for every alpha mode. It is folded into baseColor.a at
    // registration unconditionally (SyncMaterialOpacityFromDocument), and an Opaque
    // material's alpha still reaches the alpha-preserving thumbnail path, so hiding
    // the row on Opaque left a value that changes rendering with no way to edit it.
    if (key == "baseColor" || key == "emissive" || key == "emissionLuminance" || key == "emissiveExposureWeight" ||
        key == "opacity")
        return true;

    // `alphaCutoff` is the value `opacity` is tested against, and nothing else
    // reads it: only Mask compiles the alpha test in, so on any other mode the
    // row would be a control that changes nothing. ShadowOnly forces Blend, which
    // is why the mode is taken effectively rather than from doc.alphaMode.
    if (key == "alphaCutoff")
        return doc.alphaMode == MaterialAlphaMode::Mask && !shadowOnly;

    const bool isSixAxisBlockProp =
        key == "sixAxisScatter" || key == "sixAxisTeaOcclusionBlend" || key == "sixAxisEmissiveScale" ||
        key == "sixAxisDensityScale" || key == "flipbookColumns" || key == "flipbookRows" ||
        key == "flipbookFps" || key == "flipbookStartFrame";
    if (isSixAxisBlockProp)
        return sixAxis;

    if (sixAxis && (key == "metallic" || key == "roughness" || key == "ao"))
        return false;

    if (key == "roughness" || key == "metallic" || key == "ao")
        return !unlit && !shadowOnly;

    if (key == "enableClearCoat")
        return !unlit && !shadowOnly;
    if (key == "clearCoat" || key == "clearCoatRoughness" || key == "clearCoatIor" ||
        key == "coatDarkening" || key == "coatColor")
    {
        // Coat scalars only show once the coat is enabled (keeps the panel tidy
        // and mirrors the keyword gate that compiles the lobe in).
        if (unlit || shadowOnly)
            return false;
        auto it = doc.properties.find("enableClearCoat");
        const bool* on = (it != doc.properties.end()) ? std::get_if<bool>(&it->second) : nullptr;
        return on && *on;
    }

    if (key == "enableSheen")
        return !unlit && !shadowOnly;
    if (key == "sheenColor" || key == "sheenRoughness")
    {
        // sheenColor contains "color" so it would be auto-visible by the default
        // return below; gate it (and the roughness) on enableSheen explicitly.
        if (unlit || shadowOnly)
            return false;
        auto it = doc.properties.find("enableSheen");
        const bool* on = (it != doc.properties.end()) ? std::get_if<bool>(&it->second) : nullptr;
        return on && *on;
    }
    if (key == "enableFuzz")
        return !unlit && !shadowOnly;
    if (key == "fuzzColor" || key == "fuzzRoughness")
    {
        // fuzzColor contains "color" so it would be auto-visible by the default
        // return below; gate it (and the roughness) on enableFuzz explicitly.
        if (unlit || shadowOnly)
            return false;
        auto it = doc.properties.find("enableFuzz");
        const bool* on = (it != doc.properties.end()) ? std::get_if<bool>(&it->second) : nullptr;
        return on && *on;
    }

    if (key == "enableAnisotropy")
        return !unlit && !shadowOnly;
    if (key == "anisotropy" || key == "anisotropyRotation")
    {
        if (unlit || shadowOnly)
            return false;
        auto it = doc.properties.find("enableAnisotropy");
        const bool* on = (it != doc.properties.end()) ? std::get_if<bool>(&it->second) : nullptr;
        return on && *on;
    }

    if (key == "enableSubsurface")
        return !unlit && !shadowOnly;
    auto boolPropOn = [&](const char* k) {
        auto it = doc.properties.find(k);
        const bool* on = (it != doc.properties.end()) ? std::get_if<bool>(&it->second) : nullptr;
        return on && *on;
    };
    if (key == "subsurfaceColor")
    {
        // subsurfaceColor contains "color" so it would be auto-visible by the default
        // return below; gate it on enableSubsurface explicitly.
        return !unlit && !shadowOnly && boolPropOn("enableSubsurface");
    }
    if (key == "thickness")
        return !unlit && !shadowOnly && boolPropOn("enableSubsurface");
    if (key == "refractionDistance") // world-unit thick-lens path length; tracks the visible Thick toggle
        return !unlit && !shadowOnly && boolPropOn("enableTransmission") && boolPropOn("enableTransmissionThick");
    if (key == "attenuationColor" || key == "attenuationDistance")
        // Beer–Lambert volume absorption is a thick-glass-only effect (thin-walled has no volume
        // depth); attenuationColor also contains "color" so it would be auto-visible — gate both on
        // the same Transmission + Thick pair as refractionDistance.
        return !unlit && !shadowOnly && boolPropOn("enableTransmission") && boolPropOn("enableTransmissionThick");

    if (key == "enableTransmission")
        return !unlit && !shadowOnly;
    if (key == "transmissionColor" || key == "transmissionWeight" || key == "enableTransmissionThick")
        // transmissionColor contains "color" so it would be auto-visible by the default return
        // below; gate it (and the weight slider + the thick toggle) on enableTransmission.
        return !unlit && !shadowOnly && boolPropOn("enableTransmission");

    // OpenPBR base specular is always-on (not a lobe) — show it for every lit material,
    // no enable toggle. specularColor would be auto-visible via the "color" heuristic;
    // this also confines weight/ior to lit materials.
    if (key == "specularColor" || key == "specularWeight" || key == "specularIor")
        return !unlit && !shadowOnly;

    if (key == "enableIridescence")
        return !unlit && !shadowOnly;
    if (key == "thinFilmThickness" || key == "thinFilmIor" || key == "thinFilmWeight")
        return !unlit && !shadowOnly && boolPropOn("enableIridescence");

    return true;
}

bool IsTextureVisible(const std::string& slot, const MaterialDocument& doc,
                      const std::vector<std::string>& surfaceTextureNames)
{
    const bool unlit = IsLightingModelUnlit(doc.lightingModel);
    const bool shadowOnly = IsLightingModelShadowOnly(doc.lightingModel);
    const bool sixAxis = UsesVolumetricSixAxisSurface(doc) && unlit;

    if (slot == "albedoMap" || slot == "emissiveMap")
        return true;

    if (sixAxis && (slot == "normalMap" || slot == "metallicRoughnessMap"))
        return true;

    if (slot == "metallicRoughnessMap" || slot == "aoMap")
        return !unlit && !shadowOnly;
    if (slot == "heightMap")
        return !unlit && !shadowOnly &&
               std::find(surfaceTextureNames.begin(), surfaceTextureNames.end(), slot) != surfaceTextureNames.end();
    if (slot == "normalMap")
        return !unlit;
    if (slot == "coatNormalMap")
    {
        // The coat normal only perturbs the clear-coat lobe, so it is meaningless (and the
        // keyword a no-op) without the coat. Show the slot under the Clear Coat section only
        // once the coat is enabled, mirroring the coat scalar gate above.
        if (unlit || shadowOnly)
            return false;
        auto it = doc.properties.find("enableClearCoat");
        const bool* on = (it != doc.properties.end()) ? std::get_if<bool>(&it->second) : nullptr;
        return on && *on;
    }

    return true;
}

bool IsReliefDepthVisible(const MaterialDocument& doc, const std::vector<std::string>& surfaceTextureNames)
{
    const auto binding = doc.textures.find("heightMap");
    return binding != doc.textures.end() && !binding->second.empty() &&
           IsTextureVisible("heightMap", doc, surfaceTextureNames);
}

bool IsColorProperty(const std::string& name)
{
    return ContainsIgnoreCase(name, "color") || name == "emissive";
}

bool IsSlider01Property(const std::string& name)
{
    // alphaCutoff: the fragment stage clamps the cutoff to [0,1], so a 0..1
    // slider is the whole meaningful range rather than a display convenience.
    return name == "alphaCutoff" ||
           name == "roughness" || name == "metallic" || name == "ao" || name == "opacity" ||
           name == "sixAxisTeaOcclusionBlend" || name == "hexBlend" || name == "hexRotation" ||
           name == "clearCoat" || name == "clearCoatRoughness" || name == "coatDarkening" ||
           name == "sheenRoughness" || name == "fuzzRoughness" ||
           name == "thickness" || name == "specularWeight" || name == "diffuseRoughness" ||
           name == "transmissionWeight" || name == "thinFilmWeight";
}

std::string PropertyLabel(const std::string& key)
{
    if (key == "ao")
        return "AO";
    return SplitCamelCase(key);
}

std::string TextureSlotLabel(const std::string& slot)
{
    std::string s = slot;
    if (s.size() > 3 && s.compare(s.size() - 3, 3, "Map") == 0)
        s.erase(s.size() - 3);
    return SplitCamelCase(s);
}

} // namespace GameEngine::Editor::MaterialRows

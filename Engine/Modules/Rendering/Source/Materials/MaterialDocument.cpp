#include "Rendering/Materials/MaterialDocument.h"

#include <algorithm>
#include <cctype>

namespace GameEngine
{

const char* MaterialTextureFilterToString(MaterialTextureFilter f)
{
    switch (f)
    {
    case MaterialTextureFilter::Bilinear: return "Bilinear";
    case MaterialTextureFilter::Point:    return "Point";
    case MaterialTextureFilter::Trilinear:
    default:                              return "Trilinear";
    }
}

MaterialTextureFilter MaterialTextureFilterFromString(const std::string& s)
{
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "point" || lower == "nearest") return MaterialTextureFilter::Point;
    if (lower == "bilinear")                    return MaterialTextureFilter::Bilinear;
    return MaterialTextureFilter::Trilinear;
}

float MaterialDocument::DefaultOpacity() const
{
    if (auto it = properties.find("baseColor"); it != properties.end())
    {
        if (const auto* rgba = std::get_if<std::vector<float>>(&it->second); rgba && rgba->size() >= 4)
            return (*rgba)[3];
    }
    return 1.0f;
}

void MaterialDocument::FillMissingStandardPBRDefaults()
{
    std::string lower;
    lower.reserve(lightingModel.size());
    for (char c : lightingModel)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    // Lighting models with no PBR shading lobes have no use for these scalars, and
    // seeding them would surface dead inspector rows.
    const bool skipPbrScalars = (lower == "unlit" || lower == "shadowonly");

    MaterialDocument defaults = CreateDefaultPBR(materialName);
    defaults.properties["opacity"] = DefaultOpacity();

    for (const auto& [key, value] : defaults.properties)
    {
        if (skipPbrScalars && (key == "roughness" || key == "metallic" || key == "ao"))
            continue;
        properties.emplace(key, value);
    }
}

MaterialDocument MaterialDocument::CreateUnlitTextured(const std::string& name)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = name;
    doc.lightingModel = "Unlit";
    doc.surfaceShader = "Surfaces/unlit_solid.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    doc.doubleSided = true;
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
    // The null GUID, spelled out: Rendering does not depend on AssetCore, and
    // only the SLOT's presence matters to the compose — consumers overwrite
    // the value with their real texture.
    doc.textures["albedoMap"] = "00000000-0000-0000-0000-000000000000";
    return doc;
}

MaterialDocument MaterialDocument::CreateDefaultPBR(const std::string& name)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = name;
    doc.lightingModel = "StandardPBR";
    // Unified PBR surface shader that samples albedoMap / normalMap /
    // metallicRoughnessMap. Unassigned slots fall back to 1x1 default textures
    // so the UBO values still control the result.
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    doc.doubleSided = false;
    doc.properties["baseColor"] = std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f};
    doc.properties["roughness"] = 0.5f;
    doc.properties["diffuseRoughness"] = 0.0f; // OpenPBR Oren-Nayar diffuse roughness; 0 = smooth Lambert
    doc.properties["metallic"] = 0.0f;
    doc.properties["emissive"] = std::vector<float>{1.0f, 1.0f, 1.0f}; // emission colour (multiplies the emissive texture)
    doc.properties["emissionLuminance"] = 0.0f;                         // emission in nits; 0 emits nothing, 203 (reference white) == scene-linear 1.0
    doc.properties["emissiveExposureWeight"] = kDefaultEmissiveExposureWeight; // 1 physical, 0 shown whatever the exposure
    doc.properties["ao"] = 1.0f;
    doc.properties["opacity"] = doc.DefaultOpacity(); // == baseColor.a, seeded above
    doc.properties["enableClearCoat"] = false;    // compiles the coat lobe in when true
    doc.properties["clearCoat"] = 1.0f;           // coat strength [0..1] — full when enabled (URP default)
    doc.properties["clearCoatRoughness"] = 0.03f; // coat GGX roughness [0..1], sharp by default
    doc.properties["coatDarkening"] = 0.0f;       // OpenPBR coat_darkening [0..1]; 0 = inert (opt-in wet-look)
    doc.properties["enableCoatNormal"] = false;   // samples coatNormalMap (slot 5) so the coat lobe shades about its own normal; only meaningful with enableClearCoat
    doc.properties["enableSheen"] = false;                                  // compiles the sheen lobe in when true
    doc.properties["sheenColor"] = std::vector<float>{0.5f, 0.5f, 0.5f};    // mid-grey = visible fuzz on enable (color IS strength)
    doc.properties["sheenRoughness"] = 0.3f;                                // soft fabric fuzz [0..1]
    doc.properties["enableFuzz"] = false;                                   // compiles the over-coat Charlie fuzz lobe in when true (OpenPBR fuzz_*, distinct from sheen)
    doc.properties["fuzzColor"] = std::vector<float>{0.5f, 0.5f, 0.5f};     // mid-grey = visible fuzz on enable (color IS strength)
    doc.properties["fuzzRoughness"] = 0.3f;                                 // soft fabric fuzz [0..1]
    doc.properties["enableAnisotropy"] = false;                            // reshapes the base GGX lobe along the tangent when true
    doc.properties["anisotropy"] = 0.5f;                                    // signed [-1..1]; 0=isotropic so default non-zero
    doc.properties["anisotropyRotation"] = 0.0f;                            // radians; 0 = groove follows the geometric tangent (rotate guard no-op)
    doc.properties["enableSubsurface"] = false;                            // compiles the translucency term in when true
    doc.properties["subsurfaceColor"] = std::vector<float>{0.9f, 0.3f, 0.2f}; // fleshy translucent default — visible on enable (tints transmitted light)
    doc.properties["thickness"] = 0.5f;                                    // [0..1]; 0 makes transmission vanish, so seed non-zero
    doc.properties["enableTransmission"] = false;                          // compiles the refractive glass lobe in when true
    doc.properties["enableTransmissionThick"] = false;                     // two-surface (crystal-ball) refraction; walks `thickness` through the glass — implies enableTransmission
    doc.properties["transmissionColor"] = std::vector<float>{1.0f, 1.0f, 1.0f}; // tint on the refracted background (white = colorless glass)
    doc.properties["transmissionWeight"] = 1.0f;                           // [0..1]; full glass on enable (0 renders inert, so seed non-zero)
    doc.properties["refractionDistance"] = 1.0f;                           // world units (~sphere radius); thick (crystal-ball) lens path length
    doc.properties["attenuationColor"] = std::vector<float>{1.0f, 1.0f, 1.0f}; // thick-glass Beer–Lambert volume colour (white = no absorption)
    doc.properties["attenuationDistance"] = 0.0f;                          // world units; reference distance for the absorption (0 = inert/off)
    // OpenPBR base specular (always-on, not a lobe): F0 from IOR, tinted + weighted.
    // Neutral defaults (white / 1 / 1.5) reproduce the legacy fixed F0=0.04 exactly.
    doc.properties["specularColor"] = std::vector<float>{1.0f, 1.0f, 1.0f}; // dielectric specular tint (white = neutral)
    doc.properties["specularWeight"] = 1.0f;                                // [0..1] specular strength (1 = physical)
    doc.properties["specularIor"] = 1.5f;                                   // dielectric IOR; 1.5 -> F0 0.04
    doc.properties["clearCoatIor"] = 1.5f;                                  // clear-coat IOR; 1.5 -> F0 0.04
    doc.properties["enableIridescence"] = false;                           // compiles the thin-film interference term in when true
    doc.properties["thinFilmThickness"] = 300.0f;                          // film thickness in nm (~0..2000 visible band); 0 makes it inert
    doc.properties["thinFilmIor"] = 1.3f;                                   // film refractive index (soap/oil ~1.3, anodized metal ~2.0)
    doc.properties["thinFilmWeight"] = 1.0f;                                // [0..1]; full effect on enable (0 renders inert)
    // Texture slots: declared empty so the PBR surface shader sees the map
    // names in the material document. The inspector shows them as pickers
    // regardless, but leaving explicit empties here keeps the saved .material
    // consistent with ModelMaterialBridge-produced materials.
    doc.textures["albedoMap"] = "";
    doc.textures["normalMap"] = "";
    doc.textures["metallicRoughnessMap"] = "";
    return doc;
}

} // namespace GameEngine

// MaterialRegistry implementation: GUID-keyed Material ownership, UBO creation,
// per-frame dirty upload.

#include "Engine/Rendering/MaterialRegistry.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

MaterialAlphaMode GetEffectiveMaterialAlphaMode(const MaterialDocument& doc)
{
    if (ToLowerAscii(doc.lightingModel) == "shadowonly")
        return MaterialAlphaMode::Blend;
    return doc.alphaMode;
}

void ApplyDocumentPropertiesToMaterial(Material& mat, const MaterialDocument& doc)
{
    for (const auto& [propName, propValue] : doc.properties)
    {
        if (mat.GetDeclaredProperties())
        {
            // Declared surface: the table owns the type, so an int lands as int
            // bits and a vector is clamped to its declared width. An undeclared
            // key stores nothing; the inspector and the validator report it.
            mat.SetDeclaredPropertyValue(propName, propValue);
            continue;
        }

        const StringId sid = HashStringId(propName);
        if (const float* fval = std::get_if<float>(&propValue))
        {
            mat.SetFloat(sid, *fval);
        }
        else if (const int32_t* ival = std::get_if<int32_t>(&propValue))
        {
            // Integer-typed document values are a JSON formatting accident, not a type
            // declaration: JS/JSON cannot express "1.0" distinctly, so a converter- or
            // hand-authored 1 parses as int. Every hand-wired lane is a float, so int
            // BITS memcpy'd into a lane read 1 as 1e-45 (~0.0) on the GPU. Land
            // document scalars as float, like the bool branch below.
            mat.SetFloat(sid, static_cast<float>(*ival));
        }
        else if (const std::vector<float>* vval = std::get_if<std::vector<float>>(&propValue))
        {
            if (!vval->empty())
                mat.SetVector(sid, vval->data(), static_cast<uint32_t>(vval->size()));
        }
        else if (const bool* bval = std::get_if<bool>(&propValue))
        {
            mat.SetFloat(sid, *bval ? 1.0f : 0.0f);
        }
    }
}

void SyncMaterialOpacityFromDocument(Material& mat, const MaterialDocument& doc)
{
    auto opIt = doc.properties.find("opacity");
    if (opIt == doc.properties.end())
        return;

    float opacity = 1.0f;
    if (const float* o = std::get_if<float>(&opIt->second))
        opacity = *o;
    else if (const int32_t* oi = std::get_if<int32_t>(&opIt->second))
        opacity = static_cast<float>(*oi);

    float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (mat.GetVector(HashStringId("baseColor"), rgba, 4))
    {
        rgba[3] = std::max(0.0f, opacity);
        mat.SetVector(HashStringId("baseColor"), rgba, 4);
    }
}

void MaterialRegistry::Initialize(Rendering::IDevice* device)
{
    assert(device && "MaterialRegistry requires a valid IDevice");
    m_Device = device;
}

void MaterialRegistry::SeedTextureSlotDefaults(uint32_t whiteIdx, uint32_t flatNormalIdx, uint32_t blackIdx)
{
    m_DefaultWhiteIdx      = whiteIdx;
    m_DefaultFlatNormalIdx = flatNormalIdx;
    m_DefaultBlackIdx      = blackIdx;
    m_TextureSlotDefaultsSeeded = true;
}

void MaterialRegistry::Shutdown()
{
    for (auto& [guid, mat] : m_Materials)
    {
        if (mat && m_PreUnregisterCallback)
            m_PreUnregisterCallback(mat.get());
    }
    m_Materials.clear();
    m_Device = nullptr;
}

Rendering::ShaderVariantKey DeriveBaseVariantKey(const MaterialDocument& doc)
{
    Rendering::ShaderVariantKey key{};
    key.vertexFlags = Rendering::VertexAttributeFlags::StandardMesh;
    key.lightingModel = doc.lightingModel.empty()
        ? Rendering::LightingModel::kUnlit
        : HashStringId(ToLowerAscii(doc.lightingModel));
    // User keywords ride the separate order-independent hash lane so the variant
    // key (and cache key) disambiguate a keyworded variant; the names themselves
    // ride the compile spec (ApplyDocumentToMaterial) for emission. A keyword
    // edit therefore changes the variant key, which drives the recompile path.
    if (!doc.keywords.empty())
    {
        std::vector<uint64_t> ids;
        ids.reserve(doc.keywords.size());
        for (const std::string& kw : doc.keywords)
            ids.push_back(static_cast<uint64_t>(HashStringId(kw)));
        key.userKeywordHash = Rendering::CombineUserKeywordHash(std::move(ids));
    }
    Rendering::ApplyAlphaModeKeyword(key, GetEffectiveMaterialAlphaMode(doc));
    // No vertex-modifier keyword here: which of the two forms a modifier implements is a
    // property of the resolved FILE, and resolving it needs the material's asset path plus
    // the shader-root chain — neither of which reaches a MaterialRegistry handed a document
    // alone. MaterialSystem::RegisterMaterialFromDocument applies
    // Rendering::ApplyVertexModifierKeyword where it already derives vertexFlags for the
    // same reason.
    if (doc.customVertexShader)
        Rendering::ApplyCustomVertexShaderClamp(key);
    // Clear-coat opt-in: an explicit enableClearCoat bool compiles the additive coat
    // lobe into the variant (the clearCoat strength + clearCoatRoughness floats then
    // drive it). Off by default, so uncoated materials carry no coat code.
    if (auto it = doc.properties.find("enableClearCoat"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::ClearCoat;
    // Sheen opt-in: an explicit enableSheen bool compiles the additive Charlie fabric
    // lobe into the variant (sheenColor + sheenRoughness then drive it). Off by default.
    if (auto it = doc.properties.find("enableSheen"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::Sheen;
    // Fuzz opt-in: an explicit enableFuzz bool compiles the additive Charlie fuzz lobe
    // into the variant (fuzzColor + fuzzRoughness then drive it). Distinct from sheen —
    // fuzz layers OVER the coat (OpenPBR's outermost fuzz_*), sheen under it. Off by default.
    if (auto it = doc.properties.find("enableFuzz"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::Fuzz;
    // Coat-normal opt-in: an explicit enableCoatNormal bool compiles the coat-normal sampling
    // (coatNormalMap, slot 5) into the variant and shades the coat lobe about its own normal.
    // Only meaningful alongside ClearCoat; a no-op otherwise. Off by default — no slot-5 fetch.
    if (auto it = doc.properties.find("enableCoatNormal"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::CoatNormal;
    // Anisotropy opt-in: an explicit enableAnisotropy bool reshapes the base GGX lobe
    // along the tangent (uParams11.x drives the signed amount). Off by default.
    if (auto it = doc.properties.find("enableAnisotropy"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::Anisotropy;
    // Subsurface opt-in: an explicit enableSubsurface bool compiles the additive
    // back-transmission (translucency) term in (uParams12 drives color + thickness).
    if (auto it = doc.properties.find("enableSubsurface"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::Subsurface;
    // Transmission opt-in: an explicit enableTransmission bool compiles the refractive
    // dielectric (glass) lobe in. uParams15 drives color + weight; the existing always-on
    // specularIor drives the refraction so reflection and refraction share one Fresnel. The
    // term is evaluated inside the IBL ambient (it samples the environment cube), so it requires
    // an IBL-enabled rendergraph — ForwardPlus (the engine default) carries the IBL pass keyword.
    // We deliberately do NOT force MaterialKeyword::IBL here: the IBL cube descriptors bind only
    // under the IBL pass keyword (RenderServices BuildPassResourcesRG), so a forced-IBL variant
    // drawn by a non-IBL pass would declare bindings the pass never binds — a descriptor mismatch.
    // Without IBL the lobe is simply inert (the material renders as an opaque dielectric).
    if (auto it = doc.properties.find("enableTransmission"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::Transmission;
    // Thick (two-surface) refraction opt-in: walks the authored thickness through the glass for a
    // crystal-ball lens instead of the single-surface bend. Implies Transmission so the lobe + grab
    // variant compile in (the thick GLSL is nested under GE_TRANSMISSION_ENABLED / GE_SCENECOLOR_GRAB).
    if (auto it = doc.properties.find("enableTransmissionThick"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::Transmission
                                  | Rendering::MaterialKeyword::TransmissionThick;
    // Iridescence opt-in: an explicit enableIridescence bool compiles the thin-film
    // interference term into the variant (uParams17 drives film thickness/IOR/weight).
    // It substitutes the specular Fresnel in-place — no IBL force is needed: the direct
    // lobe iridesces under GE_IRIDESCENCE_ENABLED, and the IBL substitution rides the same
    // keyword inside ibl.glsl (which only compiles when an IBL pass is active anyway).
    if (auto it = doc.properties.find("enableIridescence"); it != doc.properties.end())
        if (const bool* on = std::get_if<bool>(&it->second); on && *on)
            key.materialKeywords |= Rendering::MaterialKeyword::Iridescence;
    return key;
}

MaterialAlphaMode DeriveRenderedAlphaMode(const MaterialDocument& doc)
{
    // Transmission is a screen-space refraction lobe drawn in the opaque-depth pass:
    // it must stay Opaque so it writes/tests depth. A Blend transmissive material
    // would be skipped by the depth prepass, breaking the refraction foreground guard.
    if (Rendering::HasKeyword(DeriveBaseVariantKey(doc).materialKeywords,
                              Rendering::MaterialKeyword::Transmission))
        return MaterialAlphaMode::Opaque;
    return GetEffectiveMaterialAlphaMode(doc);
}

namespace
{
MaterialCompileSpec BuildCompileSpec(const MaterialDocument& doc, MaterialAlphaMode renderedAlphaMode)
{
    MaterialCompileSpec spec;
    spec.surfaceShaderPath = doc.surfaceShader;
    spec.vertexModifierPath = doc.vertexModifier;
    spec.lightingModel = doc.lightingModel;
    spec.customVertexShader = doc.customVertexShader;
    // Keyword strings ride the spec so a cache-driven recompile (GetOrCompile's
    // synthetic doc) reproduces the GE_USER_ defines, not just the hash.
    spec.userKeywords = doc.keywords;
    spec.alphaTest = renderedAlphaMode == MaterialAlphaMode::Mask;
    return spec;
}
} // namespace

MaterialCompileSpec DeriveCompileSpec(const MaterialDocument& doc)
{
    return BuildCompileSpec(doc, DeriveRenderedAlphaMode(doc));
}

void MaterialRegistry::ApplyDocumentToMaterial(Material& mat, const MaterialDocument& doc)
{
    const Rendering::ShaderVariantKey key = DeriveBaseVariantKey(doc);
    const MaterialAlphaMode effectiveAlphaMode = DeriveRenderedAlphaMode(doc);
    // Coerce + warn rather than silently misrender.
    if (effectiveAlphaMode != GetEffectiveMaterialAlphaMode(doc))
    {
        Logger::Log::Warning(
            "Material '{}': Transmission requires Opaque alpha mode; coercing to Opaque.",
            doc.materialName);
    }

    mat.m_VariantKey = key;
    mat.m_AlphaMode = effectiveAlphaMode;
    mat.m_DoubleSided = doc.doubleSided;
    mat.m_BlendState = doc.blend;
    mat.m_ZWriteOverride = doc.zWrite;
    mat.m_ZTestOverride = doc.zTest;
    mat.m_IgnoreVertexColor = doc.ignoreVertexColor;
    mat.m_LightingModel = key.lightingModel;

    mat.m_CompileSpec = BuildCompileSpec(doc, effectiveAlphaMode);

    // Reset texture transforms to identity so removing a transform from the
    // document on hot-reload reverts the slot to default rather than leaving
    // the previous override in place.
    for (uint32_t i = 0; i < kTextureSlotArraySize; ++i)
        mat.SetTextureTransform(static_cast<TextureSlot>(i), 1.0f, 1.0f, 0.0f, 0.0f);

    mat.m_AuthoredProperties = doc.properties;
    if (mat.m_DeclaredProperties)
    {
        // Declared surface: defaults come from the table, so re-installing it
        // resets every lane and replays the document — a removed key reverts.
        mat.SetDeclaredProperties(mat.m_DeclaredProperties);
        SyncMaterialOpacityFromDocument(mat, doc);
        for (const auto& [texName, st] : doc.textureTransforms)
            mat.SetTextureTransform(HashStringId(texName), st);
        return;
    }

    // Reset known scalar/color properties to defaults before applying the
    // document so unset properties revert correctly across a reload.
    {
        const float defaultBaseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        mat.SetVector(HashStringId("baseColor"), defaultBaseColor, 4);
        mat.SetFloat(HashStringId("metallic"), 0.0f);
        mat.SetFloat(HashStringId("roughness"), 0.5f);
        mat.SetFloat(HashStringId("hexTiling"), 0.0f);
        mat.SetFloat(HashStringId("hexBlend"), 0.5f);
        mat.SetFloat(HashStringId("hexRotation"), 1.0f);
        mat.SetFloat(HashStringId("alphaCutoff"), kDefaultAlphaCutoff);
        mat.SetFloat(HashStringId("flipbookColumns"), 0.0f);
        mat.SetFloat(HashStringId("flipbookRows"), 0.0f);
        mat.SetFloat(HashStringId("flipbookFps"), 0.0f);
        mat.SetFloat(HashStringId("flipbookStartFrame"), 0.0f);
        mat.SetFloat(HashStringId("clearCoat"), 1.0f);          // full coat when enabled (URP default)
        mat.SetFloat(HashStringId("clearCoatRoughness"), 0.03f); // sharp by default
        mat.SetFloat(HashStringId("coatDarkening"), 0.0f);       // wet-look off by default (opt-in, no surprise on enable)
        const float defaultCoatColor[3] = {1.0f, 1.0f, 1.0f};
        mat.SetVector(HashStringId("coatColor"), defaultCoatColor, 3); // colourless coat by default (3 comps -> uParams19.rgb)
        // Sheen has no separate strength scalar — the color magnitude IS the strength —
        // so default to mid-grey, not black, or enabling the toggle would render nothing.
        const float defaultSheenColor[3] = {0.5f, 0.5f, 0.5f};
        mat.SetVector(HashStringId("sheenColor"), defaultSheenColor, 3); // 3 comps — matches size-12 layout
        mat.SetFloat(HashStringId("sheenRoughness"), 0.3f);             // broad/soft fabric lobe
        // Fuzz (OpenPBR fuzz_*, the over-coat lobe). Like sheen, the colour magnitude IS the
        // strength, so default to mid-grey or enabling the toggle would render nothing.
        const float defaultFuzzColor[3] = {0.5f, 0.5f, 0.5f};
        mat.SetVector(HashStringId("fuzzColor"), defaultFuzzColor, 3); // 3 comps — matches size-12 layout
        mat.SetFloat(HashStringId("fuzzRoughness"), 0.3f);            // broad/soft fabric lobe
        mat.SetFloat(HashStringId("anisotropy"), 0.5f);                 // non-zero so enabling is visibly stretched (0 = isotropic)
        mat.SetFloat(HashStringId("anisotropyRotation"), 0.0f);         // radians; 0 = groove aligned with the geometric tangent (rotate guard no-op)
        const float defaultSubsurfaceColor[3] = {0.9f, 0.3f, 0.2f};
        mat.SetVector(HashStringId("subsurfaceColor"), defaultSubsurfaceColor, 3); // 3 comps — matches size-12 layout; fleshy so enabling is visible
        mat.SetFloat(HashStringId("thickness"), 0.5f);                  // 0 would make transmission vanish
        // Refractive transmission (glass). Full weight on enable so toggling the lobe is
        // immediately visible (weight 0 would render inert); white tint = colorless glass.
        const float defaultTransmissionColor[3] = {1.0f, 1.0f, 1.0f};
        mat.SetVector(HashStringId("transmissionColor"), defaultTransmissionColor, 3); // 3 comps — size-12 layout
        mat.SetFloat(HashStringId("transmissionWeight"), 1.0f);
        mat.SetFloat(HashStringId("refractionDistance"), 1.0f);         // world units; thick-lens path length
        mat.SetFloat(HashStringId("reliefDepth"), kDefaultReliefDepth); // fraction of one height repeat; binding a height map is the opt-in
        // Thick-glass Beer–Lambert volume absorption: white colour + distance 0 = inert (no-op), so
        // existing glass is byte-identical until the artist dials in an absorption colour and distance.
        const float defaultAttenuationColor[3] = {1.0f, 1.0f, 1.0f};
        mat.SetVector(HashStringId("attenuationColor"), defaultAttenuationColor, 3); // 3 comps — size-12 layout
        mat.SetFloat(HashStringId("attenuationDistance"), 0.0f);        // world units; 0 disables absorption

        // OpenPBR base specular + coat IOR are ALWAYS-on (ungated). Neutral defaults
        // (white / weight 1 / ior 1.5) reproduce the legacy fixed F0=0.04 exactly, so
        // every material that does not author them stays byte-identical.
        const float defaultSpecularColor[3] = {1.0f, 1.0f, 1.0f};
        mat.SetVector(HashStringId("specularColor"), defaultSpecularColor, 3); // 3 comps — size-12 layout
        mat.SetFloat(HashStringId("specularWeight"), 1.0f);
        mat.SetFloat(HashStringId("specularIor"), 1.5f);
        mat.SetFloat(HashStringId("clearCoatIor"), 1.5f);

        // Thin-film iridescence (uParams17). Visible defaults on enable: a ~300 nm soap/oil
        // film at full weight; thickness 0 or weight 0 makes the lobe inert.
        mat.SetFloat(HashStringId("thinFilmThickness"), 300.0f); // nm
        mat.SetFloat(HashStringId("thinFilmIor"), 1.3f);
        mat.SetFloat(HashStringId("thinFilmWeight"), 1.0f);

        // Emission (uParams18): texture x colour x luminance, so a white colour and luminance 0
        // (OpenPBR's defaults) keep a material that authors neither dark.
        const float defaultEmissive[3] = {1.0f, 1.0f, 1.0f};
        mat.SetVector(HashStringId("emissive"), defaultEmissive, 3); // 3 comps — size-12 layout
        mat.SetFloat(HashStringId("emissionLuminance"), 0.0f);       // nits; 0 emits nothing
        // A zero lane would show every emitter whatever the exposure: physical unless authored.
        mat.SetFloat(HashStringId("emissiveExposureWeight"), kDefaultEmissiveExposureWeight);

        // GPU-fog params alias the PBR/hex UBO offsets (e.g. gpuFogVoronoiScale and
        // hexTiling both map to offset 24; gpuFogSimpleNoiseScale and metallic to 16).
        // Only seed them for gpu-fog materials — otherwise they clobber
        // hexTiling/metallic/roughness for every PBR material whose document does not
        // explicitly author those (e.g. glTF-imported meshes), which silently enabled
        // hex tiling by default. gpu-fog materials author their own values.
        const bool isGpuFog = doc.surfaceShader.find("gpu_fog") != std::string::npos;
        if (isGpuFog)
        {
            mat.SetFloat(HashStringId("gpuFogSimpleNoiseScale"), 20.0f);
            mat.SetFloat(HashStringId("gpuFogSimplexNoiseScale"), 4.0f);
            mat.SetFloat(HashStringId("gpuFogVoronoiScale"), 5.0f);
            mat.SetFloat(HashStringId("gpuFogCombinedNoiseRemap"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimpleNoiseAmount"), 0.25f);
            mat.SetFloat(HashStringId("gpuFogSimplexNoiseAmount"), 0.25f);
            mat.SetFloat(HashStringId("gpuFogVoronoiNoiseAmount"), 0.5f);
            mat.SetFloat(HashStringId("gpuFogRadialMaskPower"), 1.0f);
            mat.SetFloat(HashStringId("gpuFogSimpleNoiseRemap"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimplexNoiseRemap"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogVoronoiNoiseRemap"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogEdgeSoftness"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimpleAnimationX"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimpleAnimationY"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimpleAnimationZ"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimpleAnimationW"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimplexAnimationX"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimplexAnimationY"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSimplexAnimationZ"), 0.02f);
            mat.SetFloat(HashStringId("gpuFogSimplexAnimationW"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogVoronoiAnimationX"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogVoronoiAnimationY"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogVoronoiAnimationZ"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogVoronoiAnimationW"), 0.0f);
            mat.SetFloat(HashStringId("gpuFogSurfaceDepthFade"), 0.66f);
            mat.SetFloat(HashStringId("gpuFogShapeDistortion"), 0.28f);
            mat.SetFloat(HashStringId("gpuFogWispyNoiseAmount"), 0.35f);
            mat.SetFloat(HashStringId("gpuFogDetailNoiseAmount"), 0.25f);
            mat.SetFloat(HashStringId("gpuFogCameraDepthFadeRange"), 1.0f);
            mat.SetFloat(HashStringId("gpuFogCameraDepthFadeOffset"), 0.0f);
        }

        // triplanar_pbr front-end defaults. Only seed for triplanar materials — the
        // params live in the appended uParams22-24 lanes no other surface reads, so
        // leaving them zero elsewhere is harmless, but seeding them here keeps a
        // partially-authored triplanar document (or a hot-reload that drops a key)
        // rendering sanely instead of collapsing weights to a zero-tiling smear.
        const bool isTriplanar = doc.surfaceShader.find("triplanar_pbr") != std::string::npos;
        if (isTriplanar)
        {
            mat.SetFloat(HashStringId("triplanarTilingTop"), 1.0f);
            mat.SetFloat(HashStringId("triplanarTilingSide"), 1.0f);
            mat.SetFloat(HashStringId("triplanarTilingBottom"), 1.0f);
            mat.SetFloat(HashStringId("triplanarBlendSharpness"), 0.5f); // mid axis-blend
            mat.SetFloat(HashStringId("triplanarMetallicTop"), 0.0f);
            mat.SetFloat(HashStringId("triplanarMetallicSide"), 0.0f);
            mat.SetFloat(HashStringId("triplanarMetallicBottom"), 0.0f);
            mat.SetFloat(HashStringId("triplanarLayered"), 0.0f); // default to the collapsed fast path
            mat.SetFloat(HashStringId("triplanarRoughnessTop"), 0.8f);
            mat.SetFloat(HashStringId("triplanarRoughnessSide"), 0.8f);
            mat.SetFloat(HashStringId("triplanarRoughnessBottom"), 0.8f);
            // Normals collapse independently of albedo (slot-3 black-default trap): a doc
            // that authors only triplanarLayered must not drag the normal reads onto the
            // unbound layered slots, so the absent-key default is collapsed here too.
            mat.SetFloat(HashStringId("triplanarNormalLayered"), 0.0f);
        }
    }

    ApplyDocumentPropertiesToMaterial(mat, doc);
    SyncMaterialOpacityFromDocument(mat, doc);

    for (const auto& [texName, st] : doc.textureTransforms)
        mat.SetTextureTransform(HashStringId(texName), st);
}

void MaterialRegistry::InstallLegacyPropertyLayouts(Material& mat)
{
    // The name -> offset wiring for a surface that reads the block by lane name.
    // The placements live in one table the composer and the DDGI bake read too
    // (Materials/LegacyMaterialLanes.h); a surface with `// @property`
    // declarations replaces this map through Material::SetDeclaredProperties.
    for (const Rendering::LegacyMaterialLane& lane : Rendering::kLegacyMaterialLanes)
        mat.m_PropertyLayouts[HashStringId(std::string(lane.Name))] = {LegacyLaneByteOffset(lane),
                                                                       LegacyLaneByteSize(lane)};
}

void MaterialRegistry::ApplyPropertyTable(Material& mat, std::shared_ptr<const Rendering::ShaderPropertyTable> table)
{
    if (!table || table->Rejected())
        return;
    if (table->HasSurfaceDeclarations)
    {
        if (mat.GetDeclaredProperties() != table.get())
            mat.SetDeclaredProperties(std::move(table));
        return;
    }
    if (!mat.GetDeclaredProperties())
        return; // already on the hand-typed table
    // The surface dropped its declarations: back to the lane-name wiring, with
    // the document values replayed through the float path.
    mat.SetDeclaredProperties(nullptr);
    InstallLegacyPropertyLayouts(mat);
    MaterialDocument authored{};
    authored.properties = mat.m_AuthoredProperties;
    ApplyDocumentPropertiesToMaterial(mat, authored);
    SyncMaterialOpacityFromDocument(mat, authored);
}

Material* MaterialRegistry::Register(const GUID& guid,
                                      const MaterialDocument& doc,
                                      std::string* /*outError*/)
{
    // Idempotent path: when the GUID is already registered, refresh the
    // mutable runtime state from the new document. This is the hot-reload
    // path for `.material` edits — Asset::Reload() preserves the GUID, so
    // returning the cached Material* without re-applying values would
    // silently drop the user's changes. Pipeline/shader recompiles on
    // shader-source changes are handled by RenderingHotReloadBridge.
    if (auto it = m_Materials.find(guid); it != m_Materials.end())
    {
        Material* existing = it->second.get();
        if (existing)
        {
            // ApplyDocumentToMaterial's property setters bump the global content
            // epoch; PackMaterialSSBO repacks the shared MaterialParams SSBO on
            // the next frame. No per-material GPU upload.
            ApplyDocumentToMaterial(*existing, doc);
        }
        return existing;
    }

    auto mat = std::unique_ptr<Material>(new Material());
    mat->m_Guid = guid;
    mat->m_Name = doc.materialName.empty() ? guid.ToCompactString() : doc.materialName;

    // CPU cache mirrors the shared MaterialData param block packed into the
    // MaterialParams SSBO; size comes from the shared field list (material_params.glsl).
    mat->InitCache(GameEngine::kMaterialParamBlockBytes);

    // Seed every bindless texture slot with a valid descriptor index BEFORE
    // the material can be observed by any draw path. Slots not bound by the
    // doc would otherwise stay at the "not set" sentinel (0) — the shader
    // samples ge_BindlessTextures[0] which is an undefined descriptor on the
    // GPU side. See RenderServices.cpp ~3415 for the bindless slot-0 invariant.
    assert(m_TextureSlotDefaultsSeeded &&
           "MaterialRegistry::Register called before SeedTextureSlotDefaults — "
           "every Material must be seeded with valid texture slot defaults");
    mat->InitBindlessDefaults(m_DefaultWhiteIdx, m_DefaultFlatNormalIdx, m_DefaultBlackIdx);

    InstallLegacyPropertyLayouts(*mat);

    // Property setters above bump the global content epoch; PackMaterialSSBO
    // packs the CPU cache into the shared MaterialParams SSBO on the next frame.
    ApplyDocumentToMaterial(*mat, doc);

    Material* result = mat.get();
    m_Materials[guid] = std::move(mat);
    return result;
}

Material* MaterialRegistry::Find(const GUID& guid)
{
    auto it = m_Materials.find(guid);
    return (it != m_Materials.end()) ? it->second.get() : nullptr;
}

const Material* MaterialRegistry::Find(const GUID& guid) const
{
    auto it = m_Materials.find(guid);
    return (it != m_Materials.end()) ? it->second.get() : nullptr;
}

void MaterialRegistry::Unregister(const GUID& guid)
{
    auto it = m_Materials.find(guid);
    if (it == m_Materials.end())
        return;

    if (it->second && m_PreUnregisterCallback)
        m_PreUnregisterCallback(it->second.get());

    m_Materials.erase(it);
}

} // namespace Engine::Renderer
} // namespace GameEngine

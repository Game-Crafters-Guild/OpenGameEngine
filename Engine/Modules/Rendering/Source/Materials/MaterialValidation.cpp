#include "Rendering/Materials/MaterialValidation.h"

#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderPropertyTable.h"
#include "Types/NearestName.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine { namespace Rendering {

namespace {

static bool IsTextureLikeDescriptor(uint32_t bindingTypeValue)
{
    // DescriptorBindingMeta::Type holds ShaderMetaBindingType constants (not
    // Rendering::DescriptorType — the two enums order their members differently).
    switch (bindingTypeValue)
    {
    case ShaderMetaBindingType::kSampler:
    case ShaderMetaBindingType::kSampledImage:
    case ShaderMetaBindingType::kStorageImage:
    case ShaderMetaBindingType::kCombinedImageSampler:
        return true;
    default:
        return false;
    }
}

// Engine-provided global resources (shadow map arrays, IBL cubemaps, GTAO,
// scene color/depth, the bindless texture/sampler arrays, ...) follow the
// `ge_` naming convention and are bound by the renderer per pass. A material
// never supplies them, so they must not count as required material textures —
// composed M0 adapters reflect a dozen of these and every material would
// otherwise fail validation with MissingTextureBinding errors.
static bool IsEngineProvidedBinding(const std::string& name)
{
    return name.rfind("ge_", 0) == 0;
}

static void CollectBlockMemberNames(const BlockLayout& block, std::unordered_set<std::string>& out)
{
    for (const auto& m : block.Members)
    {
        if (!m.Name.empty())
            out.insert(m.Name);
        // NOTE: nested structs could be flattened later; for now, collect only top-level names.
    }
}

static void AddMaterialPropertyAliases(std::unordered_set<std::string>& inOutKnown)
{
    // Material v2 (M0) authoring uses user-friendly property names (e.g. "baseColor"),
    // while the composed adapter shaders expose UBO member names (e.g. "uBaseColor")
    // through reflection. Treat these as aliases so validation doesn't spam warnings
    // and the Editor can keep stable, ergonomic property keys.
    if (inOutKnown.find("uBaseColor") != inOutKnown.end())
    {
        inOutKnown.insert("baseColor");
        inOutKnown.insert("BaseColor");
    }
    if (inOutKnown.find("uParams0") != inOutKnown.end() ||
        inOutKnown.find("uParams1") != inOutKnown.end() ||
        inOutKnown.find("uParams2") != inOutKnown.end() ||
        inOutKnown.find("uParams3") != inOutKnown.end() ||
        inOutKnown.find("uParams4") != inOutKnown.end() ||
        inOutKnown.find("uParams5") != inOutKnown.end() ||
        inOutKnown.find("uParams6") != inOutKnown.end() ||
        inOutKnown.find("uParams7") != inOutKnown.end() ||
        inOutKnown.find("uParams8") != inOutKnown.end())
    {
        inOutKnown.insert("gpuFogSimpleNoiseScale");
        inOutKnown.insert("gpuFogSimplexNoiseScale");
        inOutKnown.insert("gpuFogVoronoiScale");
        inOutKnown.insert("gpuFogCombinedNoiseRemap");
        inOutKnown.insert("gpuFogSimpleNoiseAmount");
        inOutKnown.insert("gpuFogSimplexNoiseAmount");
        inOutKnown.insert("gpuFogVoronoiNoiseAmount");
        inOutKnown.insert("gpuFogRadialMaskPower");
        inOutKnown.insert("gpuFogSimpleNoiseRemap");
        inOutKnown.insert("gpuFogSimplexNoiseRemap");
        inOutKnown.insert("gpuFogVoronoiNoiseRemap");
        inOutKnown.insert("gpuFogEdgeSoftness");
        inOutKnown.insert("gpuFogSimpleAnimationX");
        inOutKnown.insert("gpuFogSimpleAnimationY");
        inOutKnown.insert("gpuFogSimpleAnimationZ");
        inOutKnown.insert("gpuFogSimpleAnimationW");
        inOutKnown.insert("gpuFogSimplexAnimationX");
        inOutKnown.insert("gpuFogSimplexAnimationY");
        inOutKnown.insert("gpuFogSimplexAnimationZ");
        inOutKnown.insert("gpuFogSimplexAnimationW");
        inOutKnown.insert("gpuFogVoronoiAnimationX");
        inOutKnown.insert("gpuFogVoronoiAnimationY");
        inOutKnown.insert("gpuFogVoronoiAnimationZ");
        inOutKnown.insert("gpuFogVoronoiAnimationW");
        inOutKnown.insert("gpuFogSurfaceDepthFade");
        inOutKnown.insert("gpuFogShapeDistortion");
        inOutKnown.insert("gpuFogWispyNoiseAmount");
        inOutKnown.insert("gpuFogDetailNoiseAmount");
        inOutKnown.insert("gpuFogCameraDepthFadeRange");
        inOutKnown.insert("gpuFogCameraDepthFadeOffset");
        inOutKnown.insert("windStrength");
        inOutKnown.insert("windParams");
    }
}

} // namespace

ValidationReport ValidateMaterialAgainstMeta(const ShaderMeta& meta,
                                            const MaterialValidationInput& input)
{
    ValidationReport report{};

    std::unordered_set<std::string> materialProps(input.propertyNames.begin(), input.propertyNames.end());
    std::unordered_set<std::string> materialTextures(input.textureNames.begin(), input.textureNames.end());

    // Gather material-relevant texture binding names from descriptor sets.
    std::unordered_set<std::string> knownTextureBindings;
    for (const auto& set : meta.Sets)
    {
        for (const auto& b : set.Bindings)
        {
            if (IsTextureLikeDescriptor(b.Type) && !b.Name.empty() && !IsEngineProvidedBinding(b.Name))
            {
                knownTextureBindings.insert(b.Name);
            }
        }
    }

    // Missing textures -> error
    for (const auto& requiredName : knownTextureBindings)
    {
        if (materialTextures.find(requiredName) == materialTextures.end())
        {
            ValidationIssue iss{};
            iss.Severity = IssueSeverity::Error;
            iss.Code = "MissingTextureBinding";
            iss.Message = "Material is missing texture binding '" + requiredName + "'";
            report.Issues.push_back(std::move(iss));
        }
    }

    // Unknown textures -> warning
    for (const auto& provided : materialTextures)
    {
        if (!provided.empty() && knownTextureBindings.find(provided) == knownTextureBindings.end())
        {
            ValidationIssue iss{};
            iss.Severity = IssueSeverity::Warning;
            iss.Code = "UnknownTextureBinding";
            iss.Message = "Material provides texture '" + provided + "' but shader does not declare a matching binding";
            report.Issues.push_back(std::move(iss));
        }
    }

    // Gather known property names (best-effort).
    std::unordered_set<std::string> knownProperties;
    for (const auto& sc : meta.SpecConstants)
    {
        if (!sc.Name.empty())
            knownProperties.insert(sc.Name);
    }
    for (const auto& pc : meta.PushConstants)
    {
        CollectBlockMemberNames(pc.Block, knownProperties);
    }
    for (const auto& set : meta.Sets)
    {
        for (const auto& b : set.Bindings)
        {
            if (b.Block.has_value())
            {
                CollectBlockMemberNames(*b.Block, knownProperties);
            }
        }
    }
    AddMaterialPropertyAliases(knownProperties);

    // A program's `// @property` declarations are the authoritative key set for
    // a declared surface; their names are also the suggestions for a typo. A
    // laneless declaration is an adapter read no surface stores — the composer
    // folded it to its default — so an authored key for it is dead: it gets the
    // declare-it fix-it below instead of counting as known.
    std::vector<std::string> declaredNames;
    std::unordered_map<std::string, ShaderPropertyType> lanelessDeclared;
    for (const auto& p : meta.DeclaredProperties)
    {
        if (p.Name.empty())
            continue;
        if (p.HasLane)
        {
            knownProperties.insert(p.Name);
            declaredNames.push_back(p.Name);
        }
        else
        {
            lanelessDeclared.emplace(p.Name, p.Type);
        }
    }

    // Treat explicitly-bound property keys as known when the target uniform exists.
    // This avoids warnings when users author with ergonomic names and map them to
    // shader variables via the MaterialDocument bindings.
    if (!knownProperties.empty() && !input.bindings.empty())
    {
        for (const auto& kv : input.bindings)
        {
            const std::string& uniform = kv.first;
            const std::string& propKey = kv.second;
            if (uniform.empty() || propKey.empty())
                continue;
            if (knownProperties.find(uniform) != knownProperties.end())
                knownProperties.insert(propKey);
        }
    }

    // Unknown properties -> warning (only when we actually have any reflected property names).
    if (!knownProperties.empty())
    {
        // The transitional StandardPBR parse-time fill seeds its key set into
        // every document, so a seeded key matching a laneless declaration is the
        // fill's, not the author's — reporting it would flag every material.
        std::unordered_set<std::string> seededKeys;
        if (!lanelessDeclared.empty())
        {
            for (const auto& kv : MaterialDocument::CreateDefaultPBR("").properties)
                seededKeys.insert(kv.first);
        }
        for (const auto& prop : materialProps)
        {
            if (prop.empty())
                continue;
            if (knownProperties.find(prop) != knownProperties.end())
                continue;
            ValidationIssue iss{};
            iss.Severity = IssueSeverity::Warning;
            iss.Code = "UnknownMaterialProperty";
            const auto laneless = lanelessDeclared.find(prop);
            if (laneless != lanelessDeclared.end())
            {
                if (seededKeys.count(prop) != 0)
                    continue;
                iss.Message = "Material property '" + prop +
                              "' matches an adapter constant no surface declares, so the value never "
                              "reaches the shader; add '// @property " +
                              ShaderPropertyTypeName(laneless->second) + " " + prop +
                              " ...' to the surface to make it authorable";
            }
            else
            {
                iss.Message = declaredNames.empty()
                                  ? "Material property '" + prop + "' is not declared in shader reflection"
                                  : "Material property '" + prop + "' is not declared by the surface" +
                                        NearestNameSuffix(prop, declaredNames) +
                                        "; remove the key or add a // @property line for it";
            }
            report.Issues.push_back(std::move(iss));
        }
    }

    return report;
}

}} // namespace GameEngine::Rendering

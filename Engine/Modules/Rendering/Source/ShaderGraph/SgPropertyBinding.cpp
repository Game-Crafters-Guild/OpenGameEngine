#include "Rendering/ShaderGraph/SgPropertyBinding.h"

#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/MaterialPropertyBinding.h"

#include <cctype>

namespace GameEngine::ShaderGraph
{
namespace
{

bool ParseFloatToken(const std::string& text, float& out)
{
    try
    {
        out = std::stof(text);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::vector<float> ParseVecTokens(const std::string& text)
{
    std::vector<float> values;
    std::string cleaned = text;
    if (!cleaned.empty() && cleaned.front() == '(')
        cleaned.erase(cleaned.begin());
    if (!cleaned.empty() && cleaned.back() == ')')
        cleaned.pop_back();

    std::string token;
    for (char c : cleaned)
    {
        if (c == ',')
        {
            float v = 0.f;
            if (ParseFloatToken(token, v))
                values.push_back(v);
            token.clear();
            continue;
        }
        token.push_back(c);
    }
    if (!token.empty())
    {
        float v = 0.f;
        if (ParseFloatToken(token, v))
            values.push_back(v);
    }
    return values;
}

std::string NormalizePropertyName(std::string_view name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::optional<SgPublicPropertyBinding> BindingForNormalizedName(const std::string& normalized,
                                                                std::string_view graphGlslType)
{
    if (normalized == "basecolor")
    {
        return SgPublicPropertyBinding{
            .ParameterSlot = 0,
            .VectorSwizzle = "xyz",
            .MaterialPropertyKeys = {"baseColor"},
        };
    }
    if (normalized == "metallic")
    {
        return SgPublicPropertyBinding{
            .ParameterSlot = 1,
            .FloatComponent = "x",
            .MaterialPropertyKeys = {"metallic"},
        };
    }
    if (normalized == "roughness")
    {
        return SgPublicPropertyBinding{
            .ParameterSlot = 1,
            .FloatComponent = "y",
            .MaterialPropertyKeys = {"roughness"},
        };
    }
    if (normalized == "rimcolor")
    {
        return SgPublicPropertyBinding{
            .ParameterSlot = 2,
            .VectorSwizzle = "xyz",
            .MaterialPropertyKeys = {"flipbookColumns", "flipbookRows", "flipbookFps"},
        };
    }

    if (graphGlslType == "float")
    {
        return SgPublicPropertyBinding{
            .ParameterSlot = 1,
            .FloatComponent = "z",
            .MaterialPropertyKeys = {"hexTiling"},
        };
    }
    if (graphGlslType == "vec2")
    {
        return SgPublicPropertyBinding{
            .ParameterSlot = 1,
            .VectorSwizzle = "zw",
            .MaterialPropertyKeys = {"hexTiling", "hexBlend"},
        };
    }
    // Unknown vec3/vec4 names must not fall back to flipbook slot 2 — only explicit
    // names (e.g. rimcolor) or parameter-node slot bindings map to UBO fields.
    return std::nullopt;
}

bool IsPreservedBuiltInMaterialKey(const std::string& key)
{
    return key == "baseColor" || key == "metallic" || key == "roughness" || key == "emissive" ||
           key == "emissionLuminance" || key == "ao" || key == "opacity";
}

std::string PrettyPropertyKeyLabel(const std::string& key)
{
    if (key == "ao")
        return "AO";
    std::string out;
    for (size_t i = 0; i < key.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(key[i]);
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

std::string PrettyGraphPropertyLabel(std::string_view graphPropertyName)
{
    using namespace GameEngine::Rendering::MaterialPropertyBinding;
    return PrettyPropertyKeyLabel(SuggestPropertyKeyForUniform(graphPropertyName));
}

bool UsesDedicatedShaderGraphInspectorRow(const SgPublicPropertyBinding& binding,
                                          std::string_view graphPropertyName)
{
    if (binding.MaterialPropertyKeys.size() > 1)
        return true;
    if (binding.MaterialPropertyKeys.empty())
        return false;

    using namespace GameEngine::Rendering::MaterialPropertyBinding;
    const std::string suggested = SuggestPropertyKeyForUniform(graphPropertyName);
    return binding.MaterialPropertyKeys.front() != suggested;
}

std::optional<SgPublicPropertyBinding> ResolvePublicPropertyBinding(std::string_view graphPropertyName,
                                                                    std::string_view graphGlslType)
{
    if (graphPropertyName.empty())
        return std::nullopt;

    using namespace GameEngine::Rendering::MaterialPropertyBinding;
    const std::string suggested = SuggestPropertyKeyForUniform(graphPropertyName);
    if (auto binding = BindingForNormalizedName(NormalizePropertyName(suggested), graphGlslType))
        return binding;
    return BindingForNormalizedName(NormalizePropertyName(graphPropertyName), graphGlslType);
}

std::optional<SgPublicPropertyBinding> ResolveParameterSlotMaterialBinding(int slot,
                                                                           std::string_view nodeTypeId,
                                                                           std::string_view floatComponent)
{
    SgPublicPropertyBinding binding;
    binding.ParameterSlot = slot;

    if (nodeTypeId == "FloatParameter")
    {
        binding.FloatComponent = floatComponent.empty() ? "x" : std::string(floatComponent);
        if (slot == 0)
            binding.MaterialPropertyKeys = {"baseColor"};
        else if (slot == 1)
        {
            if (binding.FloatComponent == "x")
                binding.MaterialPropertyKeys = {"metallic"};
            else if (binding.FloatComponent == "y")
                binding.MaterialPropertyKeys = {"roughness"};
            else if (binding.FloatComponent == "z")
                binding.MaterialPropertyKeys = {"hexTiling"};
            else if (binding.FloatComponent == "w")
                binding.MaterialPropertyKeys = {"hexBlend"};
        }
        else if (slot == 3 && binding.FloatComponent == "x")
        {
            binding.MaterialPropertyKeys = {"hexRotation"};
        }
        return binding.MaterialPropertyKeys.empty() ? std::nullopt
                                                    : std::optional<SgPublicPropertyBinding>(binding);
    }

    if (nodeTypeId == "Vec2Parameter" && slot == 1)
    {
        binding.VectorSwizzle = "zw";
        binding.MaterialPropertyKeys = {"hexTiling", "hexBlend"};
        return binding;
    }

    if ((nodeTypeId == "Vec3Parameter" || nodeTypeId == "ColorParameter") && slot == 0)
    {
        binding.VectorSwizzle = "xyz";
        binding.MaterialPropertyKeys = {"baseColor"};
        return binding;
    }

    if ((nodeTypeId == "Vec3Parameter" || nodeTypeId == "ColorParameter") && slot == 2)
    {
        binding.VectorSwizzle = "xyz";
        binding.MaterialPropertyKeys = {"flipbookColumns", "flipbookRows", "flipbookFps"};
        return binding;
    }

    if (nodeTypeId == "Vec4Parameter" && slot == 0)
    {
        binding.VectorSwizzle = "xyzw";
        binding.MaterialPropertyKeys = {"baseColor"};
        return binding;
    }

    if (nodeTypeId == "Vec4Parameter" && slot == 2)
    {
        binding.VectorSwizzle = "xyzw";
        binding.MaterialPropertyKeys = {"flipbookColumns", "flipbookRows", "flipbookFps", "flipbookStartFrame"};
        return binding;
    }

    return std::nullopt;
}

void ApplyPublicPropertyToMaterialDocument(const SgGraphProperty& prop,
                                           const SgPublicPropertyBinding& binding,
                                           MaterialDocument& doc)
{
    if (binding.MaterialPropertyKeys.empty())
        return;

    if (prop.Type == "float")
    {
        float v = 0.f;
        if (!ParseFloatToken(prop.DefaultValue, v))
            return;
        doc.properties[binding.MaterialPropertyKeys.front()] = v;
        return;
    }

    if (prop.Type != "vec2" && prop.Type != "vec3" && prop.Type != "vec4")
        return;

    const std::vector<float> values = ParseVecTokens(prop.DefaultValue);
    if (values.empty())
        return;

    if (binding.MaterialPropertyKeys.size() >= 3 && values.size() >= 3)
    {
        doc.properties[binding.MaterialPropertyKeys[0]] = values[0];
        doc.properties[binding.MaterialPropertyKeys[1]] = values[1];
        doc.properties[binding.MaterialPropertyKeys[2]] = values[2];
        return;
    }

    if (binding.MaterialPropertyKeys.size() >= 2 && values.size() >= 2)
    {
        doc.properties[binding.MaterialPropertyKeys[0]] = values[0];
        doc.properties[binding.MaterialPropertyKeys[1]] = values[1];
        return;
    }

    const std::string& key = binding.MaterialPropertyKeys.front();
    if (key == "baseColor" && values.size() == 3)
    {
        float alpha = 1.f;
        if (auto it = doc.properties.find("baseColor"); it != doc.properties.end())
        {
            if (const auto* existing = std::get_if<std::vector<float>>(&it->second);
                existing && existing->size() >= 4)
            {
                alpha = (*existing)[3];
            }
        }
        doc.properties[key] = std::vector<float>{values[0], values[1], values[2], alpha};
        return;
    }

    doc.properties[key] = values;
}

void ApplyParameterNodeValueToMaterialDocument(const SgGraphProperty& prop,
                                               const SgPublicPropertyBinding& binding,
                                               MaterialDocument& doc)
{
    if (binding.MaterialPropertyKeys.empty())
        return;

    if (prop.Type == "float" && binding.MaterialPropertyKeys.front() == "baseColor")
    {
        float v = 0.f;
        if (!ParseFloatToken(prop.DefaultValue, v))
            return;

        std::vector<float> baseColor = {1.f, 1.f, 1.f, 1.f};
        if (auto it = doc.properties.find("baseColor"); it != doc.properties.end())
        {
            if (const auto* existing = std::get_if<std::vector<float>>(&it->second);
                existing && existing->size() >= 4)
            {
                baseColor = *existing;
            }
        }

        const char component = binding.FloatComponent.empty() ? 'x' : binding.FloatComponent.front();
        switch (component)
        {
        case 'x':
            baseColor[0] = v;
            break;
        case 'y':
            baseColor[1] = v;
            break;
        case 'z':
            baseColor[2] = v;
            break;
        case 'w':
            baseColor[3] = v;
            break;
        default:
            return;
        }
        doc.properties["baseColor"] = std::move(baseColor);
        return;
    }

    ApplyPublicPropertyToMaterialDocument(prop, binding, doc);
}

void RemoveGraphPropertyFromMaterialDocument(const SgGraphProperty& prop,
                                             const SgPublicPropertyBinding& binding,
                                             MaterialDocument& doc)
{
    doc.properties.erase(prop.Name);
    for (const std::string& key : binding.MaterialPropertyKeys)
    {
        if (IsPreservedBuiltInMaterialKey(key))
            continue;
        doc.properties.erase(key);
    }
}

void ApplyShaderGraphPropertiesToMaterialDocument(const std::vector<SgGraphProperty>& properties,
                                                  MaterialDocument& doc)
{
    doc.shaderGraphPublicProperties.clear();

    for (const SgGraphProperty& prop : properties)
    {
        const auto binding = ResolvePublicPropertyBinding(prop.Name, prop.Type);
        if (!binding)
        {
            if (!prop.IsPublic)
                doc.properties.erase(prop.Name);
            continue;
        }

        if (!prop.IsPublic)
        {
            RemoveGraphPropertyFromMaterialDocument(prop, *binding, doc);
            continue;
        }

        ApplyPublicPropertyToMaterialDocument(prop, *binding, doc);

        if (UsesDedicatedShaderGraphInspectorRow(*binding, prop.Name))
        {
            MaterialShaderGraphPublicProperty entry;
            entry.GraphName = prop.Name;
            entry.Type = prop.Type;
            entry.MaterialPropertyKeys = binding->MaterialPropertyKeys;
            doc.shaderGraphPublicProperties.push_back(std::move(entry));
        }
    }
}

} // namespace GameEngine::ShaderGraph

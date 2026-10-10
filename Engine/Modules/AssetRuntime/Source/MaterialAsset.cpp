#include "Assets/MaterialAsset.h"
#include "Assets/MaterialXImport.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Rendering/Materials/UserKeyword.h"
#include "Types/StringUtils.h"

#include "AssetCore/SharedFileRead.h"
#include "Components/Rendering/LightPhotometry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <regex>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace GameEngine {

namespace {

static void ParseProperties(const nlohmann::json& j, MaterialDocument& outDoc)
{
    if (!j.contains("properties") || !j["properties"].is_object())
        return;

    // JSON "1" without a fraction parses as integer; these keys must stay float
    // in the MaterialValue variant so the inspector sliders and runtime UBO
    // sync paths match CreateDefaultPBR / Serialize round-trips.
    static const std::unordered_set<std::string> kFloatScalarKeys = {
        "opacity", "roughness", "metallic", "ao", "emissionLuminance", "emissiveExposureWeight",
        "flipbookColumns", "flipbookRows", "flipbookFps", "flipbookStartFrame",
        "hexTiling", "hexBlend", "hexRotation",
        "clearCoat", "clearCoatRoughness", "coatDarkening", "sheenRoughness", "fuzzRoughness", "anisotropy", "thickness",
        "specularWeight", "specularIor", "clearCoatIor", "refractionDistance", "reliefDepth",
        "thinFilmThickness", "thinFilmIor", "thinFilmWeight"};

    for (auto it = j["properties"].begin(); it != j["properties"].end(); ++it)
    {
        const std::string k = it.key();
        const auto& v = it.value();
        if (v.is_boolean())
        {
            outDoc.properties[k] = v.get<bool>();
        }
        else if (kFloatScalarKeys.count(k) != 0
                 && (v.is_number_integer() || v.is_number_unsigned() || v.is_number_float()))
        {
            outDoc.properties[k] = static_cast<float>(v.get<double>());
        }
        else if (v.is_number_integer())
        {
            outDoc.properties[k] = (int32_t)v.get<int64_t>();
        }
        else if (v.is_number_unsigned())
        {
            outDoc.properties[k] = (int32_t)v.get<uint64_t>();
        }
        else if (v.is_number_float() || v.is_number())
        {
            outDoc.properties[k] = v.get<float>();
        }
        else if (v.is_array())
        {
            std::vector<float> arr;
            for (const auto& e : v)
            {
                if (e.is_number())
                    arr.push_back(e.get<float>());
            }
            outDoc.properties[k] = std::move(arr);
        }
    }
}

static void ParseTextures(const nlohmann::json& j, MaterialDocument& outDoc)
{
    if (!j.contains("textures") || !j["textures"].is_object())
        return;

    for (auto it = j["textures"].begin(); it != j["textures"].end(); ++it)
    {
        const std::string k = it.key();
        if (it.value().is_null())
        {
            outDoc.textures[k] = std::string();
        }
        else if (it.value().is_string())
        {
            outDoc.textures[k] = it.value().get<std::string>();
        }
        else if (it.value().is_object())
        {
            const auto& obj = it.value();
            outDoc.textures[k] = obj.value("guid", "");
            // Source-relative path companion (path-hash self-heal). Optional —
            // a guid-only object form simply omits it.
            const std::string texPath = obj.value("path", "");
            if (!texPath.empty())
                outDoc.texturePaths[k] = texPath;
            std::array<float, 8> st = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
            if (obj.contains("uvMatrix") && obj["uvMatrix"].is_array() && obj["uvMatrix"].size() >= 6)
            {
                st[0] = obj["uvMatrix"][0].get<float>();
                st[1] = obj["uvMatrix"][1].get<float>();
                st[2] = obj["uvMatrix"][2].get<float>();
                st[4] = obj["uvMatrix"][3].get<float>();
                st[5] = obj["uvMatrix"][4].get<float>();
                st[6] = obj["uvMatrix"][5].get<float>();
            }
            if (obj.contains("tiling") && obj["tiling"].is_array() && obj["tiling"].size() >= 2)
            {
                st[0] = obj["tiling"][0].get<float>();
                st[5] = obj["tiling"][1].get<float>();
            }
            if (obj.contains("offset") && obj["offset"].is_array() && obj["offset"].size() >= 2)
            {
                st[2] = obj["offset"][0].get<float>();
                st[6] = obj["offset"][1].get<float>();
            }
            if (st[0] != 1.0f || st[1] != 0.0f || st[2] != 0.0f ||
                st[4] != 0.0f || st[5] != 1.0f || st[6] != 0.0f)
                outDoc.textureTransforms[k] = st;
        }
    }
}

static void ParseBindings(const nlohmann::json& j, MaterialDocument& outDoc)
{
    if (!j.contains("bindings") || !j["bindings"].is_object())
        return;

    for (auto it = j["bindings"].begin(); it != j["bindings"].end(); ++it)
    {
        const std::string k = it.key();
        if (k.empty())
            continue;
        if (it.value().is_string())
            outDoc.bindings[k] = it.value().get<std::string>();
    }
}

static void ParseShaderGraphPublicProperties(const nlohmann::json& j, MaterialDocument& outDoc)
{
    if (!j.contains("shaderGraphPublicProperties") || !j["shaderGraphPublicProperties"].is_array())
        return;

    for (const auto& entry : j["shaderGraphPublicProperties"])
    {
        if (!entry.is_object())
            continue;

        MaterialShaderGraphPublicProperty prop;
        prop.GraphName = entry.value("graphName", std::string());
        prop.Type = entry.value("type", std::string());
        if (entry.contains("materialPropertyKeys") && entry["materialPropertyKeys"].is_array())
        {
            for (const auto& key : entry["materialPropertyKeys"])
            {
                if (key.is_string())
                    prop.MaterialPropertyKeys.push_back(key.get<std::string>());
            }
        }
        if (prop.GraphName.empty() || prop.MaterialPropertyKeys.empty())
            continue;
        outDoc.shaderGraphPublicProperties.push_back(std::move(prop));
    }
}

static bool ParseAlphaMode(const nlohmann::json& j, MaterialDocument& outDoc)
{
    if (!j.contains("alphaMode"))
        return true;
    outDoc.alphaMode = MaterialAlphaModeFromString(j.value("alphaMode", std::string("Opaque")));
    return true;
}

// Parse explicit blend authoring (T1): the optional `blend` object of
// src/dst color+alpha factors and ops, plus the top-level `zWrite` override.
// Any missing sub-field defaults to the historical hardwired Blend equation
// (MaterialBlendState's defaults). An unparseable factor/op name is a non-fatal
// warning that leaves that sub-field at its default rather than failing the load.
static void ParseBlend(const nlohmann::json& j, MaterialDocument& outDoc,
                       std::vector<std::string>& outErrors)
{
    if (j.contains("zWrite") && j["zWrite"].is_boolean())
        outDoc.zWrite = j["zWrite"].get<bool>();
    if (j.contains("zTest") && j["zTest"].is_boolean())
        outDoc.zTest = j["zTest"].get<bool>();

    if (!j.contains("blend") || !j["blend"].is_object())
        return;

    const auto& b = j["blend"];
    MaterialBlendState state{};

    auto readFactor = [&](const char* key, MaterialBlendFactor& field) {
        if (!b.contains(key) || !b[key].is_string())
            return;
        const std::string name = b[key].get<std::string>();
        if (auto f = MaterialBlendFactorFromString(name))
            field = *f;
        else
            outErrors.push_back(std::string("MaterialAsset: unknown blend factor '") + name +
                                "' for '" + key + "'; using default.");
    };
    auto readOp = [&](const char* key, MaterialBlendOp& field) {
        if (!b.contains(key) || !b[key].is_string())
            return;
        const std::string name = b[key].get<std::string>();
        if (auto o = MaterialBlendOpFromString(name))
            field = *o;
        else
            outErrors.push_back(std::string("MaterialAsset: unknown blend op '") + name +
                                "' for '" + key + "'; using default.");
    };

    readFactor("srcColor", state.SrcColorFactor);
    readFactor("dstColor", state.DstColorFactor);
    readOp("colorOp", state.ColorOp);
    readFactor("srcAlpha", state.SrcAlphaFactor);
    readFactor("dstAlpha", state.DstAlphaFactor);
    readOp("alphaOp", state.AlphaOp);

    outDoc.blend = state;
}

// Parse the `keywords` array: sanitize each authored name to a GE_USER_-safe
// identifier, drop invalid/reserved names, de-duplicate, and cap the accepted
// set at kMaxUserKeywords. Rejections and truncation are surfaced as non-fatal
// warnings (the material still loads).
static void ParseKeywords(const nlohmann::json& j, MaterialDocument& outDoc,
                          std::vector<std::string>& outErrors)
{
    if (!j.contains("keywords") || !j["keywords"].is_array())
        return;

    bool warnedCap = false;
    for (const auto& e : j["keywords"])
    {
        if (!e.is_string())
            continue;
        const std::string raw = e.get<std::string>();
        const auto sanitized = Rendering::SanitizeUserKeyword(raw);
        if (!sanitized)
        {
            outErrors.push_back("MaterialAsset: keyword '" + raw +
                                "' is not a valid or is a reserved identifier; skipped.");
            continue;
        }
        if (std::find(outDoc.keywords.begin(), outDoc.keywords.end(), *sanitized) !=
            outDoc.keywords.end())
            continue; // de-dup
        if (static_cast<int>(outDoc.keywords.size()) >= Rendering::kMaxUserKeywords)
        {
            if (!warnedCap)
            {
                outErrors.push_back("MaterialAsset: more than " +
                                    std::to_string(Rendering::kMaxUserKeywords) +
                                    " keywords; the excess is truncated.");
                warnedCap = true;
            }
            continue;
        }
        outDoc.keywords.push_back(*sanitized);
    }
}

static bool ParseJsonMaterial(const std::string& text, MaterialDocument& outDoc, std::vector<std::string>& outErrors)
{
    try
    {
        auto j = nlohmann::json::parse(text);
        outDoc.schemaVersion = j.value("schemaVersion", 2);

        if (outDoc.schemaVersion != 2 && outDoc.schemaVersion != 3)
        {
            outErrors.push_back("MaterialAsset: unsupported schemaVersion (expected 2 or 3).");
            return false;
        }

        outDoc.materialName = j.value("materialName", j.value("material_name", std::string()));
        outDoc.lightingModel = j.value("lightingModel", std::string());

        if (outDoc.schemaVersion >= 3)
        {
            // V3: GUID-based shader references. Surface shader is optional.
            outDoc.surfaceShaderGuid = j.value("surfaceShaderGuid", std::string());
            outDoc.vertexModifierGuid = j.value("vertexModifierGuid", std::string());
            outDoc.surfaceGraphGuid = j.value("surfaceGraphGuid", std::string());

            // Resolved-path hints carried alongside the GUID refs above (v3
            // emits these too; ShaderComposer reads them at compile time).
            outDoc.surfaceShader = j.value("surfaceShader", std::string());
            outDoc.vertexModifier = j.value("vertexModifier", std::string());
            outDoc.surfaceGraph = j.value("surfaceGraph", std::string());
        }
        else
        {
            // V2: path-based shader references. Surface shader was required.
            outDoc.surfaceShader = j.value("surfaceShader", std::string());
            outDoc.vertexModifier = j.value("vertexModifier", std::string());

            if (outDoc.surfaceShader.empty())
                outErrors.push_back("MaterialAsset: missing required field 'surfaceShader'.");
        }

        ParseAlphaMode(j, outDoc);
        ParseBlend(j, outDoc, outErrors);
        outDoc.doubleSided = j.value("doubleSided", false);
        outDoc.ignoreVertexColor = j.value("ignoreVertexColor", false);
        outDoc.customVertexShader = j.value("customVertexShader", false);
        outDoc.textureFilter = MaterialTextureFilterFromString(
            j.value("textureFilter", std::string("Trilinear")));

        ParseProperties(j, outDoc);

        // Fill the rest of the StandardPBR property set, so a partial .material (only a
        // few keys authored) still exposes every control — toggles, specular, emissive,
        // AO — in the inspector, matching a freshly-created material. Runs after
        // ParseProperties so the fill only ever plugs gaps and every authored value
        // wins; that ordering is what lets DefaultOpacity see the authored baseColor
        // instead of the factory white. A foreign lighting model is left untouched
        // entirely — the fill's own unlit/shadow-only skips serve the inspector, which
        // seeds a document whose lighting model the user is in the act of changing.
        if (outDoc.lightingModel == "StandardPBR")
            outDoc.FillMissingStandardPBRDefaults();

        ParseTextures(j, outDoc);
        ParseBindings(j, outDoc);
        ParseShaderGraphPublicProperties(j, outDoc);
        ParseKeywords(j, outDoc, outErrors);

        return true;
    }
    catch (const std::exception& e)
    {
        outErrors.push_back(std::string("JSON parse error: ") + e.what());
        return false;
    }
}

static std::string TrimCopy(std::string value)
{
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
        return !std::isspace(c);
    }).base(), value.end());
    return value;
}

static std::string CanonicalGuidString(const std::string& raw)
{
    GUID guid(raw.c_str());
    return guid.IsNull() ? std::string() : guid.ToString();
}

static bool TryFindFloat(const std::string& text, const std::string& key, float& outValue)
{
    const std::regex pattern("-\\s+" + key + R"(\s*:\s*([-+0-9.eE]+))");
    std::smatch match;
    if (!std::regex_search(text, match, pattern) || match.size() < 2)
        return false;

    try
    {
        outValue = std::stof(match[1].str());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static bool TryFindColor(const std::string& text, const std::string& key, std::vector<float>& outColor)
{
    const std::regex pattern(
        "-\\s+" + key + R"(\s*:\s*\{r:\s*([-+0-9.eE]+),\s*g:\s*([-+0-9.eE]+),\s*b:\s*([-+0-9.eE]+),\s*a:\s*([-+0-9.eE]+)\})");
    std::smatch match;
    if (!std::regex_search(text, match, pattern) || match.size() < 5)
        return false;

    try
    {
        outColor = {
            std::stof(match[1].str()),
            std::stof(match[2].str()),
            std::stof(match[3].str()),
            std::stof(match[4].str())};
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static std::string FindUnityTextureGuid(const std::string& text, const std::string& key)
{
    const std::regex pattern(
        "-\\s+" + key + R"(\s*:\s*[\s\S]*?m_Texture:\s*\{[^}]*guid:\s*([0-9a-fA-F]{32}|[0-9a-fA-F-]{36}))");
    std::smatch match;
    if (!std::regex_search(text, match, pattern) || match.size() < 2)
        return {};
    return CanonicalGuidString(match[1].str());
}

static bool ParseUnityMaterial(const std::string& text,
                               const std::filesystem::path& sourcePath,
                               MaterialDocument& outDoc,
                               std::vector<std::string>& outErrors)
{
    if (text.find("Material:") == std::string::npos ||
        text.find("m_SavedProperties:") == std::string::npos)
    {
        outErrors.push_back("MaterialAsset: expected JSON object.");
        return false;
    }

    MaterialDocument doc = MaterialDocument::CreateDefaultPBR(sourcePath.stem().string());
    doc.schemaVersion = 3;
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";

    std::smatch nameMatch;
    if (std::regex_search(text, nameMatch, std::regex(R"(\bm_Name:\s*([^\r\n]+))")) && nameMatch.size() >= 2)
    {
        const std::string name = TrimCopy(nameMatch[1].str());
        if (!name.empty())
            doc.materialName = name;
    }

    std::vector<float> baseColor;
    if (TryFindColor(text, "_BaseColor", baseColor) ||
        TryFindColor(text, "_BaseMapColor", baseColor) ||
        TryFindColor(text, "_Color", baseColor))
    {
        doc.properties["baseColor"] = baseColor;
        // Unity .mat has no `opacity` concept; CreateDefaultPBR above seeded the
        // factory 1.0, so re-derive it from the colour just assigned.
        doc.properties["opacity"] = doc.DefaultOpacity();
        if (baseColor.size() >= 4 && baseColor[3] < 0.999f)
            doc.alphaMode = MaterialAlphaMode::Blend;
    }

    float value = 0.0f;
    if (TryFindFloat(text, "_Metallic", value))
        doc.properties["metallic"] = std::clamp(value, 0.0f, 1.0f);
    if (TryFindFloat(text, "_Roughness", value))
        doc.properties["roughness"] = std::clamp(value, 0.04f, 1.0f);
    else if (TryFindFloat(text, "_Smoothness", value) || TryFindFloat(text, "_Glossiness", value))
        doc.properties["roughness"] = std::clamp(1.0f - value, 0.04f, 1.0f);

    if (TryFindFloat(text, "_Cutoff", value))
        doc.properties["alphaCutoff"] = std::clamp(value, 0.0f, 1.0f);

    if (TryFindFloat(text, "_Mode", value))
    {
        const int mode = static_cast<int>(std::round(value));
        if (mode == 1)
            doc.alphaMode = MaterialAlphaMode::Mask;
        else if (mode >= 2)
            doc.alphaMode = MaterialAlphaMode::Blend;
    }
    if (text.find("RenderType: Transparent") != std::string::npos ||
        text.find("_SURFACE_TYPE_TRANSPARENT") != std::string::npos)
    {
        doc.alphaMode = MaterialAlphaMode::Blend;
    }

    auto assignTexture = [&](const std::string& slot, std::initializer_list<const char*> unityKeys)
    {
        for (const char* unityKey : unityKeys)
        {
            const std::string guid = FindUnityTextureGuid(text, unityKey);
            if (!guid.empty())
            {
                doc.textures[slot] = guid;
                return;
            }
        }
    };

    assignTexture("albedoMap", {"_BaseMap", "_MainTex"});
    assignTexture("normalMap", {"_BumpMap", "_NormalMap"});
    assignTexture("metallicRoughnessMap", {"_MetallicGlossMap", "_SpecGlossMap"});
    assignTexture("aoMap", {"_OcclusionMap"});
    assignTexture("emissiveMap", {"_EmissionMap"});
    // The surface multiplies the map by the (white) color and the luminance, whose default is 0;
    // the map emits at reference white.
    if (doc.textures.contains("emissiveMap"))
        doc.properties["emissionLuminance"] = Components::kReferenceWhiteNits;

    outDoc = std::move(doc);
    return true;
}

} // namespace

bool MaterialAsset::Load()
{
    try
    {
        Vector<uint8> bytes;
        if (!ReadFileBytesShared(GetPath(), bytes))
        {
            SetState(AssetState::Failed);
            return false;
        }
        return LoadFromData(bytes);
    }
    catch (...)
    {
        m_Errors = {"Exception while loading material"};
        SetState(AssetState::Failed);
        return false;
    }
}

bool MaterialAsset::LoadFromData(const Vector<uint8>& data)
{
    try
    {
        std::string text;
        text.assign(reinterpret_cast<const char*>(data.data()), data.size());

        m_Doc = MaterialDocument{};
        m_Errors.clear();

        if (!ParseFromText(text, GetPath()))
        {
            SetState(AssetState::Failed);
            return false;
        }

        // Default name if missing
        if (m_Doc.materialName.empty())
        {
            m_Doc.materialName = GetName();
        }

        // V2 materials require a surface shader path. V3 materials use GUIDs
        // and can omit a surface shader (the adapter's default surface is used).
        if (m_Doc.schemaVersion <= 2 && m_Doc.surfaceShader.empty())
        {
            if (std::find(m_Errors.begin(), m_Errors.end(),
                          "MaterialAsset: missing required field 'surfaceShader'.") == m_Errors.end())
            {
                m_Errors.push_back("MaterialAsset: missing required field 'surfaceShader'.");
            }
            SetState(AssetState::Failed);
            return false;
        }

        // NOTE: no Mask demotion here — the parsed document must stay exactly
        // as authored. Editor save paths serialize m_Doc, so a load-time
        // demotion would bake Opaque into the asset the moment it is re-saved
        // (with no self-heal when its texture later gains alpha). The runtime
        // demotion chokepoint is MaterialSystem::RegisterMaterialFromDocument.
        SetState(AssetState::Loaded);
        return true;
    }
    catch (...)
    {
        m_Errors = {"Exception while parsing material"};
        SetState(AssetState::Failed);
        return false;
    }
}

void MaterialAsset::Unload()
{
    m_Doc = MaterialDocument{};
    m_Errors.clear();
    SetState(AssetState::Unloaded);
}

bool MaterialAsset::ParseFromText(const std::string& text, const std::filesystem::path& sourcePathForRelativeErrors)
{
    const std::string trimmed = TrimWhitespace(text);
    if (trimmed.empty())
    {
        m_Errors.push_back("MaterialAsset: expected JSON object.");
        return false;
    }
    if (trimmed.front() == '<')
    {
        // MaterialX (.mtlx) OpenPBR surface -> StandardPBR. The registry resolves texture file
        // paths (relative to the .mtlx) to asset GUIDs; it is reachable via the loading thread's
        // AssetManager (null in raw test harnesses, where paths are stored verbatim).
        AssetRegistry* registry = nullptr;
        if (AssetManager* am = AssetManager::GetThreadCurrent())
            registry = &am->GetRegistry();
        return ParseMaterialX(text, sourcePathForRelativeErrors, registry, m_Doc, m_Errors);
    }
    if (trimmed.front() != '{')
        return ParseUnityMaterial(text, sourcePathForRelativeErrors, m_Doc, m_Errors);
    return ParseJsonMaterial(text, m_Doc, m_Errors);
}

nlohmann::json SerializeMaterialDocument(const MaterialDocument& doc)
{
    nlohmann::json j = nlohmann::json::object();
    j["schemaVersion"] = 3;
    j["materialName"] = doc.materialName;
    if (!doc.lightingModel.empty())
        j["lightingModel"] = doc.lightingModel;
    if (!doc.surfaceShaderGuid.empty())
        j["surfaceShaderGuid"] = doc.surfaceShaderGuid;
    if (!doc.surfaceShader.empty())
        j["surfaceShader"] = doc.surfaceShader;
    if (!doc.vertexModifierGuid.empty())
        j["vertexModifierGuid"] = doc.vertexModifierGuid;
    if (!doc.vertexModifier.empty())
        j["vertexModifier"] = doc.vertexModifier;
    if (!doc.surfaceGraphGuid.empty())
        j["surfaceGraphGuid"] = doc.surfaceGraphGuid;
    if (!doc.surfaceGraph.empty())
        j["surfaceGraph"] = doc.surfaceGraph;

    j["alphaMode"] = MaterialAlphaModeToString(doc.alphaMode);
    if (doc.doubleSided)
        j["doubleSided"] = true;

    // Blend authoring: only emit the sub-fields that differ from the historical
    // defaults, and drop the whole object when nothing differs, so existing
    // materials stay byte-identical. zWrite is emitted only when authored.
    if (doc.blend)
    {
        const MaterialBlendState& bs = *doc.blend;
        const MaterialBlendState def{};
        nlohmann::json blendJson = nlohmann::json::object();
        if (bs.SrcColorFactor != def.SrcColorFactor)
            blendJson["srcColor"] = std::string(MaterialBlendFactorToString(bs.SrcColorFactor));
        if (bs.DstColorFactor != def.DstColorFactor)
            blendJson["dstColor"] = std::string(MaterialBlendFactorToString(bs.DstColorFactor));
        if (bs.ColorOp != def.ColorOp)
            blendJson["colorOp"] = std::string(MaterialBlendOpToString(bs.ColorOp));
        if (bs.SrcAlphaFactor != def.SrcAlphaFactor)
            blendJson["srcAlpha"] = std::string(MaterialBlendFactorToString(bs.SrcAlphaFactor));
        if (bs.DstAlphaFactor != def.DstAlphaFactor)
            blendJson["dstAlpha"] = std::string(MaterialBlendFactorToString(bs.DstAlphaFactor));
        if (bs.AlphaOp != def.AlphaOp)
            blendJson["alphaOp"] = std::string(MaterialBlendOpToString(bs.AlphaOp));
        if (!blendJson.empty())
            j["blend"] = std::move(blendJson);
    }
    if (doc.zWrite)
        j["zWrite"] = *doc.zWrite;
    if (doc.zTest)
        j["zTest"] = *doc.zTest;
    if (doc.ignoreVertexColor)
        j["ignoreVertexColor"] = true;
    if (doc.customVertexShader)
        j["customVertexShader"] = true;
    if (doc.textureFilter != MaterialTextureFilter::Trilinear)
        j["textureFilter"] = MaterialTextureFilterToString(doc.textureFilter);

    // Omitted when empty so existing (keyword-free) materials stay byte-identical.
    if (!doc.keywords.empty())
        j["keywords"] = doc.keywords;

    if (!doc.properties.empty())
    {
        nlohmann::json props = nlohmann::json::object();
        for (const auto& kv : doc.properties)
        {
            const std::string& name = kv.first;
            const MaterialValue& v = kv.second;
            if (std::holds_alternative<bool>(v))
                props[name] = std::get<bool>(v);
            else if (std::holds_alternative<int32_t>(v))
                props[name] = std::get<int32_t>(v);
            else if (std::holds_alternative<float>(v))
                props[name] = std::get<float>(v);
            else if (std::holds_alternative<std::vector<float>>(v))
                props[name] = std::get<std::vector<float>>(v);
        }
        j["properties"] = std::move(props);
    }

    if (!doc.textures.empty())
    {
        nlohmann::json tex = nlohmann::json::object();
        for (const auto& kv : doc.textures)
        {
            auto stIt = doc.textureTransforms.find(kv.first);
            const bool hasTransform = stIt != doc.textureTransforms.end() &&
                (stIt->second[0] != 1.0f || stIt->second[1] != 0.0f ||
                 stIt->second[2] != 0.0f || stIt->second[4] != 0.0f ||
                 stIt->second[5] != 1.0f || stIt->second[6] != 0.0f);

            // A path companion is only meaningful alongside a real GUID; never emit
            // {guid:"", path:...} for a cleared slot — keep its bare null form.
            auto pIt = doc.texturePaths.find(kv.first);
            const bool hasPath = pIt != doc.texturePaths.end() && !pIt->second.empty() &&
                                 !kv.second.empty();

            if (!hasTransform && !hasPath)
            {
                // Bare form: guid-only (or path-only legacy) slot with no companions.
                if (kv.second.empty())
                    tex[kv.first] = nullptr;
                else
                    tex[kv.first] = kv.second;
            }
            else
            {
                // Object form carries the guid plus optional path companion (for
                // self-heal across the identity flip) and/or UV transform.
                nlohmann::json obj = nlohmann::json::object();
                obj["guid"] = kv.second;
                if (hasPath)
                    obj["path"] = pIt->second;
                if (hasTransform)
                {
                    obj["uvMatrix"] = {
                        stIt->second[0], stIt->second[1], stIt->second[2],
                        stIt->second[4], stIt->second[5], stIt->second[6]};
                    if (stIt->second[1] == 0.0f && stIt->second[4] == 0.0f)
                    {
                        obj["tiling"] = {stIt->second[0], stIt->second[5]};
                        obj["offset"] = {stIt->second[2], stIt->second[6]};
                    }
                }
                tex[kv.first] = std::move(obj);
            }
        }
        j["textures"] = std::move(tex);
    }

    if (!doc.bindings.empty())
    {
        nlohmann::json binds = nlohmann::json::object();
        for (const auto& kv : doc.bindings)
        {
            if (!kv.first.empty() && !kv.second.empty())
                binds[kv.first] = kv.second;
        }
        if (!binds.empty())
            j["bindings"] = std::move(binds);
    }

    if (!doc.shaderGraphPublicProperties.empty())
    {
        nlohmann::json graphProps = nlohmann::json::array();
        for (const MaterialShaderGraphPublicProperty& prop : doc.shaderGraphPublicProperties)
        {
            nlohmann::json entry = nlohmann::json::object();
            entry["graphName"] = prop.GraphName;
            if (!prop.Type.empty())
                entry["type"] = prop.Type;
            entry["materialPropertyKeys"] = prop.MaterialPropertyKeys;
            graphProps.push_back(std::move(entry));
        }
        j["shaderGraphPublicProperties"] = std::move(graphProps);
    }

    return j;
}

} // namespace GameEngine

#include "Scene/SceneEngineEmbedMaterializer.h"

#include "Assets/AssetManager.h"
#include "Assets/MaterialAsset.h"
#include "Logger/Logger.h"
#include "Types/Types.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace GameEngine::Scene
{
namespace
{
static std::string EmbedTrim(std::string s)
{
    auto isws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && isws((unsigned char)s.front()))
        s.erase(s.begin());
    while (!s.empty() && isws((unsigned char)s.back()))
        s.pop_back();
    return s;
}

static std::string StripSceneQuoted(std::string s)
{
    s = EmbedTrim(std::move(s));
    // @"path"
    if (s.size() >= 3 && s[0] == '@' && s[1] == '"' && s.back() == '"')
        return s.substr(2, s.size() - 3);
    // "path"
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

static bool TryParseFloatArrayParenSyntax(const std::string& s, std::vector<float>& out)
{
    out.clear();
    std::string t = EmbedTrim(s);
    if (t.size() < 2 || t.front() != '(' || t.back() != ')')
        return false;
    t = EmbedTrim(t.substr(1, t.size() - 2));
    if (t.empty())
        return false;
    std::vector<float> vals;
    std::string cur;
    for (char ch : t)
    {
        if (ch == ',')
        {
            cur = EmbedTrim(cur);
            if (!cur.empty())
            {
                try
                {
                    vals.push_back(std::stof(cur));
                }
                catch (...)
                {
                    return false;
                }
            }
            cur.clear();
            continue;
        }
        cur.push_back(ch);
    }
    cur = EmbedTrim(cur);
    if (!cur.empty())
    {
        try
        {
            vals.push_back(std::stof(cur));
        }
        catch (...)
        {
            return false;
        }
    }
    if (vals.empty())
        return false;
    out = std::move(vals);
    return true;
}

static bool TryParseBool(const std::string& s, bool& out)
{
    std::string t = EmbedTrim(s);
    for (auto& c : t)
        c = (char)std::tolower((unsigned char)c);
    if (t == "true")
    {
        out = true;
        return true;
    }
    if (t == "false")
    {
        out = false;
        return true;
    }
    return false;
}
} // namespace

SceneEngineEmbedMaterializer::SceneEngineEmbedMaterializer(AssetManager& assets)
    : m_Assets(assets)
{
}

bool SceneEngineEmbedMaterializer::Materialize(const std::filesystem::path& owningSceneFile,
                                               const GUID& embedGuid,
                                               std::string_view embedId,
                                               AssetType assetType,
                                               std::string_view embedTypeName,
                                               const std::unordered_map<std::string, std::string>& properties,
                                               AssetReference& outRef,
                                               std::string* outError)
{
    (void)owningSceneFile;

    if (assetType != AssetType::Material)
    {
        if (outError)
            *outError = "Embed materializer does not support asset type '" + std::string(embedTypeName) + "'";
        return false;
    }

    // If already registered as loaded, just return a reference.
    if (m_Assets.IsAssetLoaded(embedGuid))
    {
        outRef = AssetReference(embedGuid, assetType, ("<embed:" + std::string(embedId) + ">"));
        return true;
    }

    nlohmann::json j;
    j["materialName"] = std::string(embedId);
    nlohmann::json propsJson = nlohmann::json::object();
    nlohmann::json texturesJson = nlohmann::json::object();

    for (const auto& kv : properties)
    {
        const std::string key = kv.first;
        const std::string raw = kv.second;
        std::string kLower = key;
        for (auto& c : kLower)
            c = (char)std::tolower((unsigned char)c);

        // Top-level known fields.
        if (kLower == "shaderdesc" || kLower == "shader_desc")
        {
            j["shaderDesc"] = StripSceneQuoted(raw);
            continue;
        }
        if (kLower == "shaderprogram" || kLower == "shader_program")
        {
            j["shaderProgram"] = StripSceneQuoted(raw);
            continue;
        }

        // Heuristic texture mapping: "texture.X = path" or "textures.X = path"
        const std::string texPrefix1 = "texture.";
        const std::string texPrefix2 = "textures.";
        if (kLower.rfind(texPrefix1, 0) == 0 || kLower.rfind(texPrefix2, 0) == 0)
        {
            const size_t cut = (kLower.rfind(texPrefix1, 0) == 0) ? texPrefix1.size() : texPrefix2.size();
            const std::string texName = key.substr(cut);
            texturesJson[texName] = StripSceneQuoted(raw);
            continue;
        }

        // Property values.
        std::vector<float> vec;
        if (TryParseFloatArrayParenSyntax(raw, vec))
        {
            propsJson[key] = vec;
            continue;
        }

        bool b = false;
        if (TryParseBool(raw, b))
        {
            propsJson[key] = b;
            continue;
        }

        // Try numeric (prefer float).
        try
        {
            float f = std::stof(EmbedTrim(raw));
            propsJson[key] = f;
            continue;
        }
        catch (...)
        {
        }

        // Unknown/unparsed: keep as string (MaterialAsset ignores unsupported property value types).
        propsJson[key] = StripSceneQuoted(raw);
    }

    if (!propsJson.empty())
        j["properties"] = std::move(propsJson);
    if (!texturesJson.empty())
        j["textures"] = std::move(texturesJson);

    const std::string jsonText = j.dump(2);
    Vector<uint8> data;
    data.resize(jsonText.size());
    if (!jsonText.empty())
        std::memcpy(data.data(), jsonText.data(), jsonText.size());

    auto asset = MakeShared<MaterialAsset>(embedGuid, std::filesystem::path("<embed>"));
    if (!asset->LoadFromData(data))
    {
        if (outError)
        {
            std::string msg = "Failed to load embedded material";
            const auto& errs = asset->GetErrors();
            if (!errs.empty())
                msg += ": " + errs.front();
            *outError = msg;
        }
        return false;
    }
    asset->PostLoad();

    m_Assets.RegisterLoadedAsset(embedGuid, asset);
    Logger::Log::Debug("Materialized embedded material asset {} (id='{}')", embedGuid.ToString(), std::string(embedId));

    outRef = AssetReference(embedGuid, assetType, ("<embed:" + std::string(embedId) + ">"));
    return true;
}

} // namespace GameEngine::Scene


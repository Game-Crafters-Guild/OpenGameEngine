#include "Assets/RenderPipelineAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <cstring>

#include <nlohmann/json.hpp>

namespace GameEngine {

namespace {
static std::string ReadAllText(const std::filesystem::path& p)
{
    std::string text;
    if (!ReadFileTextShared(p, text))
        return {};
    return text;
}

static uint64_t Fnv1a64(const std::string& s)
{
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h = kOffset;
    for (unsigned char c : s)
    {
        h ^= static_cast<uint64_t>(c);
        h *= kPrime;
    }
    return h;
}
} // namespace

bool RenderPipelineAsset::Load()
{
    m_Errors.clear();
    m_Doc = RenderPipelineDocument{};
    m_SourceHash = 0;

    const std::string text = ReadAllText(GetPath());
    if (text.empty())
    {
        m_Errors.push_back("RenderPipelineAsset: failed to read file or file is empty.");
        SetState(AssetState::Failed);
        return false;
    }

    const bool ok = ParseFromText(text, GetPath());
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    if (ok)
    {
        UpdateLastModifiedTime();
    }
    return ok;
}

bool RenderPipelineAsset::LoadFromData(const Vector<uint8>& data)
{
    m_Errors.clear();
    m_Doc = RenderPipelineDocument{};
    m_SourceHash = 0;

    if (data.empty())
    {
        m_Errors.push_back("RenderPipelineAsset: LoadFromData received empty data.");
        SetState(AssetState::Failed);
        return false;
    }

    std::string text;
    text.resize(data.size());
    std::memcpy(text.data(), data.data(), data.size());

    const bool ok = ParseFromText(text, GetPath());
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void RenderPipelineAsset::Unload()
{
    m_Doc = RenderPipelineDocument{};
    m_Errors.clear();
    m_SourceHash = 0;
    SetState(AssetState::Unloaded);
}

bool RenderPipelineAsset::ParseFromText(const std::string& text, const std::filesystem::path& /*sourcePathForRelativeErrors*/)
{
    m_Doc.jsonText = text;
    m_Doc.passes.clear();
    m_Doc.resources.clear();
    m_SourceHash = Fnv1a64(text);

    try
    {
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded())
        {
            m_Errors.push_back("RenderPipelineAsset: JSON parse failed (malformed JSON).");
            return false;
        }
        if (!j.is_object())
        {
            m_Errors.push_back("RenderPipelineAsset: root must be a JSON object.");
            return false;
        }

        if (!j.contains("schemaVersion") || !j["schemaVersion"].is_number_integer())
        {
            m_Errors.push_back("RenderPipelineAsset: missing required integer field 'schemaVersion'.");
        }
        else
        {
            const int v = j["schemaVersion"].get<int>();
            if (v <= 0)
            {
                m_Errors.push_back("RenderPipelineAsset: 'schemaVersion' must be > 0.");
            }
            else
            {
                m_Doc.schemaVersion = static_cast<uint32_t>(v);
            }
        }

        // Runtime is schemaVersion 2 (passes-only). No legacy support.
        if (m_Doc.schemaVersion != 2)
        {
            m_Errors.push_back(
                "RenderPipelineAsset: unsupported 'schemaVersion' (expected 2).");
        }

        if (j.contains("pipelineName") && j["pipelineName"].is_string())
        {
            m_Doc.pipelineName = j["pipelineName"].get<std::string>();
        }
        else if (j.contains("name") && j["name"].is_string())
        {
            // Back-compat alias: allow 'name' as a synonym for 'pipelineName'
            m_Doc.pipelineName = j["name"].get<std::string>();
        }
        else
        {
            // Default to filename stem for basic UX.
            m_Doc.pipelineName = GetPath().stem().string();
        }

        if (!j.contains("passes") || !j["passes"].is_array())
        {
            m_Errors.push_back("RenderPipelineAsset: missing required array field 'passes'.");
        }
        else
        {
            const auto& passes = j["passes"];
            for (size_t i = 0; i < passes.size(); ++i)
            {
                const auto& p = passes[i];
                if (!p.is_object())
                {
                    m_Errors.push_back("RenderPipelineAsset: pass entry must be an object (passes[" + std::to_string(i) + "]).");
                    continue;
                }
                if (!p.contains("id") || !p["id"].is_string())
                {
                    m_Errors.push_back("RenderPipelineAsset: pass is missing string field 'id' (passes[" + std::to_string(i) + "]).");
                    continue;
                }
                if (!p.contains("type") || !p["type"].is_string())
                {
                    m_Errors.push_back("RenderPipelineAsset: pass '" + p["id"].get<std::string>() + "' is missing string field 'type'.");
                    continue;
                }
                RenderPipelinePassSummary s{};
                s.id = p["id"].get<std::string>();
                s.type = p["type"].get<std::string>();
                if (p.contains("enabled") && p["enabled"].is_boolean())
                    s.enabled = p["enabled"].get<bool>();
                m_Doc.passes.push_back(std::move(s));
            }
        }

        // Optional resource summaries for quick UI/diagnostics. Full validation happens in the pipeline compiler.
        if (j.contains("resources"))
        {
            const auto& r = j["resources"];
            if (r.is_array())
            {
                for (size_t i = 0; i < r.size(); ++i)
                {
                    const auto& e = r[i];
                    if (!e.is_object())
                        continue;
                    if (!e.contains("name") || !e["name"].is_string())
                        continue;
                    RenderPipelineResourceSummary rs{};
                    rs.name = e["name"].get<std::string>();
                    if (e.contains("kind") && e["kind"].is_string())
                        rs.kind = e["kind"].get<std::string>();
                    if (e.contains("scope") && e["scope"].is_string())
                        rs.scope = e["scope"].get<std::string>();
                    m_Doc.resources.push_back(std::move(rs));
                }
            }
            else if (r.is_object())
            {
                for (auto it = r.begin(); it != r.end(); ++it)
                {
                    const std::string name = it.key();
                    const auto& e = it.value();
                    RenderPipelineResourceSummary rs{};
                    rs.name = name;
                    if (e.is_object())
                    {
                        if (e.contains("kind") && e["kind"].is_string())
                            rs.kind = e["kind"].get<std::string>();
                        if (e.contains("scope") && e["scope"].is_string())
                            rs.scope = e["scope"].get<std::string>();
                    }
                    m_Doc.resources.push_back(std::move(rs));
                }
            }
        }

        // For MVP we validate only the envelope. Node/resource schema is validated by the pipeline compiler.
    }
    catch (const std::exception& e)
    {
        m_Errors.push_back(std::string("RenderPipelineAsset: JSON parse failed: ") + e.what());
        return false;
    }

    return m_Errors.empty();
}

} // namespace GameEngine



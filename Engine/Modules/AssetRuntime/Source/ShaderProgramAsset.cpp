#include "Assets/ShaderProgramAsset.h"

#include "AssetCore/SharedFileRead.h"

#include <filesystem>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "Types/StringUtils.h"

namespace GameEngine {

namespace {
static std::filesystem::path g_shaderCacheRoot;
} // namespace

void ShaderProgramAsset::SetShaderCacheRoot(std::filesystem::path root) noexcept
{
    g_shaderCacheRoot = std::move(root);
}

const std::filesystem::path& ShaderProgramAsset::GetShaderCacheRoot()
{
    return g_shaderCacheRoot;
}

void ShaderProgramAsset::ShaderPackageDeleter::operator()(Rendering::ShaderPackage* p) const noexcept
{
    delete p;
}

bool ShaderProgramAsset::Load()
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
        SetState(AssetState::Failed);
        return false;
    }
}

bool ShaderProgramAsset::LoadFromData(const Vector<uint8>& data)
{
    m_Doc = ShaderProgramDocument{};
    m_Errors.clear();
    m_Package.reset();
    m_GeneratedShaderPkgPath.clear();

    try
    {
        std::string text;
        text.assign(reinterpret_cast<const char*>(data.data()), data.size());

        auto j = nlohmann::json::parse(text);
        m_Doc.name = j.value("name", GetName());

        if (j.contains("stages") && j["stages"].is_object())
        {
            for (auto it = j["stages"].begin(); it != j["stages"].end(); ++it)
            {
                const std::string stage = ToLowerAscii(it.key());
                ShaderStageProgramEntry e{};
                if (it.value().is_string())
                {
                    e.source = it.value().get<std::string>();
                }
                else if (it.value().is_object())
                {
                    e.source = it.value().value("source", std::string());
                    e.entryPoint = it.value().value("entryPoint", std::string("main"));
                    if (it.value().contains("defines") && it.value()["defines"].is_array())
                    {
                        for (const auto& d : it.value()["defines"])
                        {
                            if (d.is_string())
                                e.defines.push_back(d.get<std::string>());
                        }
                    }
                }
                if (e.source.empty())
                {
                    m_Errors.push_back("ShaderProgram: stage '" + stage + "' missing 'source'");
                }
                m_Doc.stages[stage] = std::move(e);
            }
        }
        else
        {
            m_Errors.push_back("ShaderProgram: missing object 'stages'");
        }

        if (!m_Errors.empty())
        {
            SetState(AssetState::Failed);
            return false;
        }

        // Attempt to compile + reflect into a cached .shaderpkg for editor/dev usage.
        // This keeps Assets clean by writing derived artifacts to <workspace>/.Cache/Shaders/...
        try
        {
            if (g_shaderCacheRoot.empty())
                throw std::runtime_error("ShaderProgramAsset: shader cache root not configured. "
                                         "Call ShaderProgramAsset::SetShaderCacheRoot() before loading.");
            const std::filesystem::path& cacheRoot = g_shaderCacheRoot;

            Rendering::ShaderProgramCompileRequest creq{};
            creq.debugName = GetName();
            creq.baseDirectory = GetPath().parent_path();
            creq.cacheRoot = cacheRoot;
            creq.includeDirs = {};
            creq.stages.reserve(m_Doc.stages.size());
            for (const auto& kv : m_Doc.stages)
            {
                Rendering::ShaderStageCompileSpec s{};
                s.stage = kv.first;
                s.sourcePath = kv.second.source;
                s.entryPoint = kv.second.entryPoint;
                s.defines = kv.second.defines;
                creq.stages.push_back(std::move(s));
            }

            Rendering::ShaderProgramCompileResult cres{};
            std::string compileErr;
            // SPIR-V: the package this asset keeps is read by the shader
            // inspector for reflection and byte counts, never fed to pipeline
            // creation, so no device's ingestion form is in play.
            if (!Rendering::ShaderCompileService::CompileProgramToCache(
                    creq, Rendering::ShaderSourceKind::SpirV, cres, &compileErr))
            {
                m_Errors.push_back("ShaderProgram compile failed: " + compileErr);
                SetState(AssetState::Failed);
                return false;
            }

            // Store results
            m_GeneratedShaderPkgPath = cres.shaderPkgPath.string();
            // Store a copy of the package payload (meta + stage bytes); callers can use it directly.
            auto pkg = std::make_unique<Rendering::ShaderPackage>();
            pkg->version = Rendering::kShaderPackageVersion;
            pkg->meta = std::move(cres.meta);
            pkg->stageBytes = std::move(cres.stageBytes);
            pkg->cacheInfoJson = std::move(cres.cacheInfoJson);
            m_Package.reset(pkg.release());
        }
        catch (const std::exception& e)
        {
            m_Errors.push_back(std::string("ShaderProgram compile exception: ") + e.what());
            SetState(AssetState::Failed);
            return false;
        }

        SetState(AssetState::Loaded);
        return true;
    }
    catch (const std::exception& e)
    {
        m_Errors.push_back(std::string("ShaderProgram JSON parse error: ") + e.what());
        SetState(AssetState::Failed);
        return false;
    }
}

void ShaderProgramAsset::Unload()
{
    m_Doc = ShaderProgramDocument{};
    m_Errors.clear();
    m_Package.reset();
    m_GeneratedShaderPkgPath.clear();
    SetState(AssetState::Unloaded);
}

} // namespace GameEngine


#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include "FileSystem/FileSystem.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Types/StringUtils.h"
#include "Rendering/Materials/ShaderMetaJson.h"
#include "Rendering/ShaderCache/ShaderMetaBinary.h"
#include "Rendering/ShaderCache/ShaderPackageContainer.h"

#include <fstream>
#include <span>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace GameEngine::Rendering
{
namespace
{
bool WriteAll(std::ofstream& out, const void* src, size_t size, std::string* outError)
{
    out.write(reinterpret_cast<const char*>(src), static_cast<std::streamsize>(size));
    if (!out.good())
    {
        if (outError)
            *outError = "Failed writing shaderpkg";
        return false;
    }
    return true;
}

bool EndsWith(const std::string& text, std::string_view suffix)
{
    return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

ShaderPackageRebuildActionFunc s_ShaderPackageRebuildAction = nullptr;

// The reader's reason, followed by what to rebuild when the package is of another
// format version and the host can say which rebuild applies to this name.
std::string DescribeReadFailure(std::string reason, ShaderPackageReadResult result, const char* packageName)
{
    if (result == ShaderPackageReadResult::UnsupportedVersion && s_ShaderPackageRebuildAction)
        reason += " " + s_ShaderPackageRebuildAction(packageName);
    return reason;
}
} // namespace

std::string BuildShaderCacheInfoJson(const ShaderCacheInfo& info)
{
    try
    {
        nlohmann::json j = nlohmann::json::object();
        j["version"] = 1;
        nlohmann::json inc = nlohmann::json::array();
        for (const auto& i : info.includes)
        {
            nlohmann::json e = nlohmann::json::object();
            e["path"] = i.path;
            e["hash64"] = i.hash64;
            inc.push_back(std::move(e));
        }
        j["includes"] = std::move(inc);
        return j.dump();
    }
    catch (...)
    {
        return {};
    }
}

bool ParseShaderCacheInfoJson(const std::string& jsonText, ShaderCacheInfo& out, std::string* outError)
{
    out = ShaderCacheInfo{};
    try
    {
        const nlohmann::json j = nlohmann::json::parse(jsonText);
        if (!j.is_object())
        {
            if (outError)
                *outError = "cache-info is not a JSON object";
            return false;
        }

        if (j.contains("includes") && j["includes"].is_array())
        {
            for (const auto& e : j["includes"])
            {
                if (!e.is_object())
                    continue;
                ShaderCacheInfo::Include inc{};
                if (e.contains("path") && e["path"].is_string())
                    inc.path = e["path"].get<std::string>();
                if (e.contains("hash64") && e["hash64"].is_number_unsigned())
                    inc.hash64 = e["hash64"].get<uint64_t>();
                if (!inc.path.empty())
                    out.includes.push_back(std::move(inc));
            }
        }
        return true;
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = e.what();
        return false;
    }
}

ShaderPackageReadResult ParseShaderPkgFromBytes(const std::vector<uint8_t>& bytes,
                                                ShaderSourceKind kind,
                                                ShaderPackage& out,
                                                std::string* outError)
{
    out = ShaderPackage{};
    std::unordered_map<std::string, std::vector<uint8_t>> wgslBytes;

    auto fail = [&](const std::string& msg) {
        if (outError)
            *outError = msg;
        return ShaderPackageReadResult::Malformed;
    };

    std::vector<ShaderPackageChunkView> chunks;
    const ShaderPackageReadResult containerResult = ReadShaderPackageContainer(bytes, chunks, outError);
    if (containerResult != ShaderPackageReadResult::Read)
        return containerResult;

    bool foundMeta = false;
    for (const ShaderPackageChunkView& chunk : chunks)
    {
        const std::string nm = ToLowerAscii(chunk.Name);
        const uint8_t* p = chunk.Bytes.data();
        const size_t n = chunk.Bytes.size();

        if (nm == kShaderPackageMetaChunk)
        {
            std::string metaErr;
            if (!DecodeShaderMetaBinary(p, n, out.meta, &metaErr))
                return fail("Failed to decode meta-bin in shaderpkg: " + metaErr);
            foundMeta = true;
        }
        else if (nm == kShaderPackageCacheInfoChunk)
        {
            out.cacheInfoJson = std::string(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p) + n);
        }
        else if (EndsWith(nm, kShaderPackageSpirvSuffix))
        {
            const std::string stage = nm.substr(0, nm.size() - kShaderPackageSpirvSuffix.size());
            out.stageBytes[stage] = std::vector<uint8_t>(p, p + n);
        }
        else if (EndsWith(nm, kShaderPackageWgslSuffix))
        {
            const std::string stage = nm.substr(0, nm.size() - kShaderPackageWgslSuffix.size());
            out.wgslStages.insert(stage);
            if (kind == ShaderSourceKind::Wgsl)
                wgslBytes[stage] = std::vector<uint8_t>(p, p + n);
        }
    }

    if (!foundMeta)
        return fail("shaderpkg missing meta-bin chunk");

    // Per-stage, not wholesale: a stage the cook produced WGSL for is served as
    // WGSL, one it did not keeps its SPIR-V so the failure is the backend's
    // named ingestion error rather than a missing stage.
    for (auto& kv : wgslBytes)
        out.stageBytes[kv.first] = std::move(kv.second);

    out.version = kShaderPackageVersion;
    return ShaderPackageReadResult::Read;
}

std::string ResolveShaderPkgPath(const std::string& pathOrName)
{
    namespace fs = std::filesystem;
    if (pathOrName.empty())
        return {};

    std::error_code ec;
    fs::path in(pathOrName);

    // Absolute paths bypass the resolver entirely.
    if (in.is_absolute())
    {
        if (fs::exists(in, ec))
            return in.string();
        ec.clear();

        if (in.extension().empty())
        {
            in.replace_extension(".shaderpkg");
            if (fs::exists(in, ec))
                return in.string();
        }
        return {};
    }

    // Ensure the extension is present before handing off to the resolver.
    if (in.extension().empty())
        in.replace_extension(".shaderpkg");

    // Use the existing shader resolver (GE_SHADERS_DIR / exe-dir staging / etc).
    // With no resolver installed (headless tests, standalone tools) nobody can look,
    // so fall back to the name as a working-dir-relative file — tests run from the
    // build root with staged Shaders/. A configured resolver that finds nothing still
    // returns empty: the fallback must not invent new resolution behavior in production.
    if (!Utils::HasShaderPathResolver())
    {
        if (fs::exists(in, ec))
            return in.string();
        return {};
    }

    const std::string resolved = Utils::ResolveShaderPath(in.string().c_str());
    if (!resolved.empty())
        return resolved;
    return {};
}

ShaderPackageRebuildActionFunc GetShaderPackageRebuildAction()
{
    return s_ShaderPackageRebuildAction;
}

void SetShaderPackageRebuildAction(ShaderPackageRebuildActionFunc action)
{
    s_ShaderPackageRebuildAction = action;
}

bool LoadShaderPkg(const std::string& pathOrName,
                   ShaderSourceKind kind,
                   ShaderPackage& out,
                   std::string* outError)
{
    const std::string resolved = ResolveShaderPkgPath(pathOrName);
    if (resolved.empty())
    {
        if (outError)
            *outError = "ResolveShaderPkgPath failed for: " + pathOrName;
        return false;
    }

    std::vector<uint8_t> fileBytes = Utils::ReadFile(resolved);
    if (fileBytes.empty())
    {
        if (outError)
            *outError = "Failed to read shaderpkg: " + resolved;
        return false;
    }
    std::string reason;
    const ShaderPackageReadResult result = ParseShaderPkgFromBytes(fileBytes, kind, out, &reason);
    if (result == ShaderPackageReadResult::Read)
        return true;
    if (outError)
        *outError = DescribeReadFailure(std::move(reason), result, pathOrName.c_str());
    return false;
}

std::vector<uint8_t> LoadComputeStageBytes(const char* pkgPath,
                                           ShaderSourceKind kind,
                                           std::vector<uint8_t> (*loader)(const char* name),
                                           std::string* outError)
{
    try
    {
        const std::vector<uint8_t> pkgBytes =
            loader ? loader(pkgPath) : Utils::LoadShaderFile(pkgPath);
        ShaderPackage pkg{};
        std::string err;
        const ShaderPackageReadResult result = ParseShaderPkgFromBytes(pkgBytes, kind, pkg, &err);
        if (result != ShaderPackageReadResult::Read)
        {
            if (outError)
                *outError = "parse failed: " + DescribeReadFailure(std::move(err), result, pkgPath);
            return {};
        }
        auto it = pkg.stageBytes.find("cs");
        if (it == pkg.stageBytes.end())
        {
            if (outError)
                *outError = "package has no compute ('cs') stage";
            return {};
        }
        return it->second;
    }
    catch (const std::exception& e)
    {
        if (outError)
            *outError = e.what();
        return {};
    }
}

bool SaveShaderPkg(const std::filesystem::path& outPath,
                   const ShaderMeta& meta,
                   const std::unordered_map<std::string, std::vector<uint8_t>>& stageSpvBytes,
                   const std::optional<std::string>& cacheInfoJson,
                   std::string* outError,
                   const std::unordered_map<std::string, std::vector<uint8_t>>& stageWgslBytes)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);

    std::vector<uint8_t> metaBytes;
    if (!EncodeShaderMetaBinary(meta, metaBytes, outError))
        return false;

    std::vector<ShaderPackageChunkSource> chunks;
    chunks.reserve(2 + stageSpvBytes.size() + stageWgslBytes.size());
    chunks.push_back({std::string(kShaderPackageMetaChunk), metaBytes});
    if (cacheInfoJson.has_value())
        chunks.push_back({std::string(kShaderPackageCacheInfoChunk),
                          std::span(reinterpret_cast<const uint8_t*>(cacheInfoJson->data()), cacheInfoJson->size())});
    for (const auto& kv : stageSpvBytes)
        chunks.push_back({ToLowerAscii(kv.first) + std::string(kShaderPackageSpirvSuffix), kv.second});
    for (const auto& kv : stageWgslBytes)
        chunks.push_back({ToLowerAscii(kv.first) + std::string(kShaderPackageWgslSuffix), kv.second});

    std::vector<uint8_t> container;
    if (!WriteShaderPackageContainer(chunks, container, outError))
        return false;

    // Stage each writer separately: content-derived cache keys can make
    // concurrent compiles target the same destination. FileSystem owns the
    // temporary name and publication; readers validate the complete container
    // before consuming any payload.
    const fs::path tmp = FileSystem::MakeTemporarySiblingPath(outPath);

    std::ofstream out(tmp, std::ios::binary);
    if (!out.is_open())
    {
        if (outError)
            *outError = "Failed to open shaderpkg for write: " + tmp.string();
        return false;
    }

    if (!WriteAll(out, container.data(), container.size(), outError))
        return false;

    out.flush();
    if (!out.good())
    {
        if (outError)
            *outError = "Failed writing shaderpkg: " + tmp.string();
        return false;
    }
    out.close();

    if (!FileSystem::PublishFile(tmp, outPath))
    {
        // Content-derived keys mean any existing same-key file is a
        // content-equivalent winner written by a concurrent caller. On Windows
        // the replace fails with a sharing violation while a reader holds the
        // pkg open (ifstream opens without FILE_SHARE_DELETE) — that must not
        // convert a successful compile into a build failure.
        ec.clear();
        if (fs::exists(outPath, ec) && !ec)
            return true;
        if (outError)
            *outError = "Failed to finalize shaderpkg: " + outPath.string();
        return false;
    }

    return true;
}
} // namespace GameEngine::Rendering


#include "Picking/MeshBvhDiskFormat.h"

#include <format>
#include <fstream>
#include <string_view>
#include <system_error>

#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector3.h"
#include "Types/Fnv1a.h"

namespace GameEngine::Editor::Picking
{

namespace
{

constexpr const char* kBvhCacheSubdir = "MeshBvh";

// File header preceding the engine-serialized BVH payload.
struct FileHeader
{
    uint32 Magic;
    uint32 FileVersion;
    uint32 BuilderVersion;
    uint32 _pad;                  // align the u64 field to 8
    uint64 GeometryHash;
};
static_assert(sizeof(FileHeader) == 24, "MeshBvh file header must be 24 bytes");

std::string GuidCompact(const GUID& guid)
{
    // Hex-stringified GUID without separators — short and filesystem-safe.
    std::string s = guid.ToString();
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c != '-' && c != '{' && c != '}')
            out.push_back(c);
    }
    return out;
}

std::filesystem::path AssetCacheDir(const std::filesystem::path& cacheRoot, const GUID& guid)
{
    return cacheRoot / kBvhCacheSubdir / GuidCompact(guid);
}

std::filesystem::path SubmeshCachePath(const std::filesystem::path& cacheRoot,
                                       const GUID& guid,
                                       uint32 submesh)
{
    return AssetCacheDir(cacheRoot, guid) /
           ("sm" + std::to_string(submesh) + "_v" + std::to_string(kBvhBuilderVersion) + ".meshbvh");
}

// Logs why a cached BVH file was not used and returns false. A load runs once per build, so
// each rejection is logged once.
bool RejectLoad(const GUID& guid, uint32 submesh, const std::filesystem::path& path, std::string_view reason)
{
    Logger::Log::Info("[MeshBvh] disk miss {} submesh {}: {} ({})", guid.ToString(), submesh, reason, path.string());
    return false;
}

// Persist failures are rare and informative (disk full, perms, race with
// invalidation). Callers attempt persist once per build, so duplicate
// warnings only show up on rebuilds — no per-guid suppression needed.
struct PersistFailureLog
{
    static void Warn(const GUID& guid, const std::string& msg)
    {
        Logger::Log::Warning("[MeshBvh] persist failed for {}: {}", guid.ToString(), msg);
    }
};

}

uint64 ComputeGeometryHash(const MeshPicking::MeshView& mesh)
{
    uint64 hash = Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, mesh.VertexCount);
    hash = Hashing::Fnv1a64Value(hash, mesh.IndexCount);
    if (!mesh.IsValid())
        return hash;
    const auto* positionBytes = reinterpret_cast<const uint8*>(mesh.Positions);
    for (uint32 vertex = 0; vertex < mesh.VertexCount; ++vertex)
        hash = Hashing::Fnv1a64(positionBytes + static_cast<size_t>(vertex) * mesh.VertexStride,
                                sizeof(Mathematics::Vector3), hash);
    return Hashing::Fnv1a64(mesh.Indices, static_cast<size_t>(mesh.IndexCount) * sizeof(uint32), hash);
}

bool PersistBvh(const std::filesystem::path& cacheRoot,
                const GUID& modelGuid,
                uint32 submesh,
                uint64 geometryHash,
                const MeshPicking::MeshBvh& bvh)
{
    if (cacheRoot.empty() || modelGuid.IsNull() || bvh.IsEmpty())
        return false;

    std::error_code ec;
    const auto dir = AssetCacheDir(cacheRoot, modelGuid);
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        PersistFailureLog::Warn(modelGuid, "create_directories: " + ec.message());
        return false;
    }

    const auto finalPath = SubmeshCachePath(cacheRoot, modelGuid, submesh);
    const auto tempPath  = finalPath.string() + ".tmp";

    std::vector<uint8> bvhBytes;
    bvh.Serialize(bvhBytes);

    FileHeader hdr{};
    hdr.Magic             = kBvhFileMagic;
    hdr.FileVersion       = kBvhFileVersion;
    hdr.BuilderVersion    = kBvhBuilderVersion;
    hdr._pad              = 0;
    hdr.GeometryHash      = geometryHash;

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            PersistFailureLog::Warn(modelGuid, "open temp for write");
            return false;
        }
        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
        if (!bvhBytes.empty())
            out.write(reinterpret_cast<const char*>(bvhBytes.data()), bvhBytes.size());
        out.close();
        if (out.fail())
        {
            PersistFailureLog::Warn(modelGuid, "write/close temp");
            std::filesystem::remove(tempPath, ec);
            return false;
        }
    }

    if (!GameEngine::FileSystem::PublishFile(tempPath, finalPath))
    {
        PersistFailureLog::Warn(modelGuid, "could not publish the temp file");
        return false;
    }
    Logger::Log::Info("[MeshBvh] persisted {} submesh {} ({} bytes) to {}",
                      modelGuid.ToString(), submesh,
                      sizeof(FileHeader) + bvhBytes.size(),
                      finalPath.string());
    return true;
}

bool TryLoadBvh(const std::filesystem::path& cacheRoot,
                const GUID& modelGuid,
                uint32 submesh,
                uint64 expectedGeometryHash,
                const MeshPicking::MeshView& source,
                MeshPicking::MeshBvh& outBvh)
{
    if (cacheRoot.empty() || modelGuid.IsNull() || !source.IsValid())
        return false;

    const auto path = SubmeshCachePath(cacheRoot, modelGuid, submesh);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return RejectLoad(modelGuid, submesh, path, "no cached file");

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return RejectLoad(modelGuid, submesh, path, "the file cannot be opened");

    FileHeader hdr{};
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(hdr)))
        return RejectLoad(modelGuid, submesh, path, "the header is truncated");

    if (hdr.Magic != kBvhFileMagic ||
        hdr.FileVersion != kBvhFileVersion ||
        hdr.BuilderVersion != kBvhBuilderVersion)
        return RejectLoad(modelGuid, submesh, path, "written by another file or builder version");
    if (hdr.GeometryHash != expectedGeometryHash)
        return RejectLoad(modelGuid, submesh, path,
                          std::format("geometry hash {:016x} in the file, {:016x} now", hdr.GeometryHash,
                                      expectedGeometryHash));

    // Read remaining bytes (the BVH payload).
    std::vector<uint8> payload;
    in.seekg(0, std::ios::end);
    const std::streampos fileSize = in.tellg();
    if (fileSize <= 0 || static_cast<size_t>(fileSize) <= sizeof(hdr))
        return RejectLoad(modelGuid, submesh, path, "no payload after the header");
    const size_t payloadSize = static_cast<size_t>(fileSize) - sizeof(hdr);
    payload.resize(payloadSize);
    in.seekg(sizeof(hdr), std::ios::beg);
    in.read(reinterpret_cast<char*>(payload.data()), payloadSize);
    if (!in)
        return RejectLoad(modelGuid, submesh, path, "the payload is truncated");

    if (!outBvh.Deserialize(payload, source))
        return RejectLoad(modelGuid, submesh, path, "the payload does not match the source mesh");
    return true;
}

void InvalidateBvhCacheFiles(const std::filesystem::path& cacheRoot, const GUID& modelGuid)
{
    if (cacheRoot.empty() || modelGuid.IsNull())
        return;
    const auto dir = AssetCacheDir(cacheRoot, modelGuid);
    std::error_code ec;
    if (std::filesystem::exists(dir, ec) && std::filesystem::remove_all(dir, ec) > 0)
        Logger::Log::Info("[MeshBvh] removed the cached BVH files of {} ({})", modelGuid.ToString(), dir.string());
}

}

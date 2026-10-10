#include "TerrainECS/TerrainBakeCache.h"

#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"
#include "Types/Fnv1a.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <system_error>

namespace GameEngine::TerrainECS
{
namespace
{

// "GETB" read as a little-endian uint32.
constexpr uint32 kTerrainBakeMagic = 0x42544547u;
// magic, version, key, width, height, splat min, splat max.
constexpr std::size_t kHeaderBytes = sizeof(uint32) * 2 + sizeof(uint64) + sizeof(uint32) * 2 + sizeof(float32) * 2;

static_assert(std::endian::native == std::endian::little,
              "the .getbake layout is little-endian and written with raw copies");

template <typename T>
void WriteScalar(std::ostream& os, const T& value)
{
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void ReadScalar(const uint8* data, std::size_t& cursor, T& out)
{
    std::memcpy(&out, data + cursor, sizeof(T));
    cursor += sizeof(T);
}

} // namespace

TerrainBakeKeyBuilder::TerrainBakeKeyBuilder()
    : m_Hash(Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, kTerrainBakeFormatVersion))
{
}

void TerrainBakeKeyBuilder::Bytes(const void* data, std::size_t size)
{
    m_Hash = Hashing::Fnv1a64(data, size, m_Hash);
}

const char* TerrainBakeCacheStatusName(TerrainBakeCacheStatus status)
{
    switch (status)
    {
    case TerrainBakeCacheStatus::Hit: return "hit";
    case TerrainBakeCacheStatus::Missing: return "missing";
    case TerrainBakeCacheStatus::FormatMismatch: return "format mismatch";
    case TerrainBakeCacheStatus::KeyMismatch: return "key mismatch";
    case TerrainBakeCacheStatus::Corrupt: return "corrupt";
    }
    return "unknown";
}

std::filesystem::path TerrainBakeFile(const std::filesystem::path& directory, const GUID& scene,
                                      std::string_view entityTag)
{
    if (directory.empty() || scene.IsNull() || entityTag.empty())
        return {};
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.getbake",
                  static_cast<unsigned long long>(Hashing::Fnv1a64(entityTag)));
    return directory / scene.ToString() / name;
}

bool WriteTerrainBake(const std::filesystem::path& file, uint64 key, const TerrainBakeArtifact& artifact)
{
    const std::size_t samples = static_cast<std::size_t>(artifact.Width) * artifact.Height;
    if (file.empty() || samples == 0 || artifact.Heights.size() != samples || artifact.Splat.size() != samples * 4u)
        return false;

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    FileSystem::RemoveOrphanedTemporaryFiles(file.parent_path(), file.filename().string());
    const std::filesystem::path temp = FileSystem::MakeTemporarySiblingPath(file);
    {
        std::ofstream os(temp, std::ios::binary | std::ios::trunc);
        if (!os)
            return false;
        WriteScalar(os, kTerrainBakeMagic);
        WriteScalar(os, kTerrainBakeFormatVersion);
        WriteScalar(os, key);
        WriteScalar(os, artifact.Width);
        WriteScalar(os, artifact.Height);
        WriteScalar(os, artifact.SplatMinH);
        WriteScalar(os, artifact.SplatMaxH);
        os.write(reinterpret_cast<const char*>(artifact.Heights.data()),
                 static_cast<std::streamsize>(samples * sizeof(float32)));
        os.write(reinterpret_cast<const char*>(artifact.Splat.data()), static_cast<std::streamsize>(samples * 4u));
        os.flush();
        if (!os.good())
        {
            os.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    return FileSystem::PublishFile(temp, file);
}

TerrainBakeCacheStatus ReadTerrainBake(const std::filesystem::path& file, uint64 key, uint32 width, uint32 height,
                                       TerrainBakeArtifact& out)
{
    Vector<uint8> bytes;
    if (file.empty() || !ReadFileBytesShared(file, bytes))
        return TerrainBakeCacheStatus::Missing;

    const uint8* data = bytes.data();
    const std::size_t size = bytes.size();
    if (size < kHeaderBytes)
        return TerrainBakeCacheStatus::Corrupt;
    std::size_t cursor = 0;
    uint32 magic = 0;
    uint32 version = 0;
    ReadScalar(data, cursor, magic);
    ReadScalar(data, cursor, version);
    if (magic != kTerrainBakeMagic || version != kTerrainBakeFormatVersion)
        return TerrainBakeCacheStatus::FormatMismatch;

    uint64 storedKey = 0;
    TerrainBakeArtifact artifact;
    ReadScalar(data, cursor, storedKey);
    ReadScalar(data, cursor, artifact.Width);
    ReadScalar(data, cursor, artifact.Height);
    ReadScalar(data, cursor, artifact.SplatMinH);
    ReadScalar(data, cursor, artifact.SplatMaxH);
    if (storedKey != key)
        return TerrainBakeCacheStatus::KeyMismatch;
    // The header's sizes are trusted only once they equal the terrain's own grid,
    // which bounds the product below; nothing is allocated before this.
    if (width == 0 || height == 0 || artifact.Width != width || artifact.Height != height)
        return TerrainBakeCacheStatus::Corrupt;
    const std::size_t samples = static_cast<std::size_t>(width) * height;
    if (size - cursor != samples * (sizeof(float32) + 4u))
        return TerrainBakeCacheStatus::Corrupt;

    artifact.Heights.resize(samples);
    std::memcpy(artifact.Heights.data(), data + cursor, samples * sizeof(float32));
    cursor += samples * sizeof(float32);
    artifact.Splat.assign(data + cursor, data + cursor + samples * 4u);
    out = std::move(artifact);
    return TerrainBakeCacheStatus::Hit;
}

std::optional<uint64> ReadTerrainBakeKey(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    uint32 magic = 0;
    uint32 version = 0;
    uint64 key = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&key), sizeof(key));
    if (!in || magic != kTerrainBakeMagic || version != kTerrainBakeFormatVersion)
        return std::nullopt;
    return key;
}

void PruneTerrainBakeScene(const std::filesystem::path& directory, const GUID& scene,
                           std::span<const std::filesystem::path> keep)
{
    if (directory.empty() || scene.IsNull())
        return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory / scene.ToString(), ec))
    {
        if (entry.path().extension() != ".getbake" ||
            std::find(keep.begin(), keep.end(), entry.path()) != keep.end())
            continue;
        std::error_code removeEc;
        std::filesystem::remove(entry.path(), removeEc);
    }
}

std::vector<TerrainBakeStagedScene> StageTerrainBakeArtifacts(const std::filesystem::path& cacheDirectory,
                                                              std::span<const GUID> scenes,
                                                              const std::filesystem::path& packageDirectory,
                                                              std::vector<std::filesystem::path>& outFailures)
{
    std::vector<TerrainBakeStagedScene> staged;
    staged.reserve(scenes.size());
    for (const GUID& scene : scenes)
    {
        TerrainBakeStagedScene entry{scene};
        const std::string folder = scene.ToString();
        std::error_code ec;
        for (const auto& file : std::filesystem::directory_iterator(cacheDirectory / folder, ec))
        {
            std::error_code fileEc;
            if (!file.is_regular_file(fileEc) || file.path().extension() != ".getbake")
                continue;
            if (!FileSystem::CopyFileContents(file.path(), packageDirectory / folder / file.path().filename()))
            {
                outFailures.push_back(file.path());
                continue;
            }
            ++entry.Artifacts;
            entry.Bytes += file.file_size(fileEc);
        }
        staged.push_back(entry);
    }
    return staged;
}

} // namespace GameEngine::TerrainECS

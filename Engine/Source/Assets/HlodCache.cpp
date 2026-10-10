#include "Assets/HlodCache.h"
#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"
#include "SubmeshElementLimits.h"
#include "Types/Fnv1a.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>

// meshopt version pinned by the vcpkg manifest (member LODs the proxy bakes were
// meshopt-generated), injected by CMake so a bump invalidates stale cooks — the
// .gelod F7 discipline. Falls back to a sentinel if the build did not wire it.
#ifndef GE_MESHOPT_VERSION
#define GE_MESHOPT_VERSION "unpinned"
#endif

namespace GameEngine {
namespace Hlod {

namespace {

// "GEHL" — GameEngine HLOD. Reads G-E-H-L in a hex dump.
constexpr uint32 kHlodCacheMagic = 0x4745484Cu;

// Sanity caps: a blob claiming more than these is treated as Corrupt before any
// allocation, so a truncated/hostile file can never drive a huge reserve. A submesh's
// vertex and index caps are kMaxVerticesPerSubmesh and kMaxIndicesPerSubmesh.
constexpr uint32 kMaxClusters = 1u << 20;
constexpr uint32 kMaxMembersPerCluster = 1u << 22;
constexpr uint32 kMaxSubmeshesPerCluster = 4096u;

// Transform quantization for the per-cluster source hash: ~1/4096 world-unit
// resolution, so float noise below the grid doesn't spuriously stale a cluster
// while a real move does.
constexpr float kTransformQuant = 4096.0f;

uint64 FnvAppendGuid(uint64 h, const GUID& guid) {
    uint8 bytes[GUID::kSize];
    guid.WriteBytes(bytes);
    return Hashing::Fnv1a64(bytes, GUID::kSize, h);
}

template <typename T>
void WriteScalar(std::ostream& os, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "WriteScalar requires trivially-copyable");
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

void WriteGuid(std::ostream& os, const GUID& guid) {
    uint8 bytes[GUID::kSize];
    guid.WriteBytes(bytes);
    os.write(reinterpret_cast<const char*>(bytes), GUID::kSize);
}

template <typename T>
bool ReadScalar(const char* data, std::size_t size, std::size_t& cursor, T& out) {
    static_assert(std::is_trivially_copyable_v<T>, "ReadScalar requires trivially-copyable");
    if (cursor + sizeof(T) > size)
        return false;
    std::memcpy(&out, data + cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

bool ReadGuid(const char* data, std::size_t size, std::size_t& cursor, GUID& out) {
    if (cursor + GUID::kSize > size)
        return false;
    uint8 bytes[GUID::kSize];
    std::memcpy(bytes, data + cursor, GUID::kSize);
    cursor += GUID::kSize;
    out = GUID::FromBytes(bytes);
    return true;
}

// Bytes remaining / element size, overflow-safe (divide, never multiply).
std::size_t RemainingElements(std::size_t size, std::size_t cursor, std::size_t elemSize) {
    if (cursor >= size)
        return 0;
    return (size - cursor) / elemSize;
}

std::string UniqueTempSuffix() {
    static const uint64 kProcessToken =
        static_cast<uint64>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
        reinterpret_cast<uintptr_t>(&Hashing::kFnv1a64Prime);
    static std::atomic<uint64> s_Counter{0};
    const uint64 tid = static_cast<uint64>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return "." + std::to_string(kProcessToken) + "." + std::to_string(tid) + "." +
           std::to_string(s_Counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
}

} // namespace

uint64 ComputeClusterSourceHash(std::span<const BakedMemberRef> members) {
    uint64 h = Hashing::kFnv1a64OffsetBasis;
    for (const BakedMemberRef& m : members) {
        h = FnvAppendGuid(h, m.StableId);
        h = Hashing::Fnv1a64Value(h, m.MeshContentHash);
        h = FnvAppendGuid(h, m.MaterialGuid);
        h = Hashing::Fnv1a64Value(h, m.MeshHandleKey);
        for (int i = 0; i < 16; ++i) {
            const int64 q = static_cast<int64>(std::llround(m.Transform[i] * kTransformQuant));
            h = Hashing::Fnv1a64Value(h, q);
        }
        h = Hashing::Fnv1a64Value(h, m.ChosenLod);
    }
    return h;
}

uint64 ComputeHlodConfigHash(const GridConfig& config, uint8 fbxGeometryVersion,
                             uint32 lodGeneratorVersion, uint32 bakerVersion) {
    uint64 h = Hashing::kFnv1a64OffsetBasis;
    h = Hashing::Fnv1a64Value(h, config.CellSize);
    h = Hashing::Fnv1a64(config.GridOrigin, sizeof(config.GridOrigin), h);
    h = Hashing::Fnv1a64Value(h, config.MaxInstancingRatio);
    h = Hashing::Fnv1a64Value(h, config.MinMembers);
    h = Hashing::Fnv1a64Value(h, config.VbBudgetBytes);
    h = Hashing::Fnv1a64Value(h, bakerVersion);
    h = Hashing::Fnv1a64Value(h, fbxGeometryVersion);
    // The proxy merges each member's coarsest COOKED LOD, so a .gelod cook-
    // semantics bump must invalidate .gehlod bakes built from the old cooks.
    h = Hashing::Fnv1a64Value(h, lodGeneratorVersion);
    const char* meshoptVersion = GE_MESHOPT_VERSION;
    h = Hashing::Fnv1a64(meshoptVersion, std::strlen(meshoptVersion), h);
    return h;
}

namespace {

void WriteSubmesh(std::ostream& os, const BakedSubmesh& sm) {
    WriteGuid(os, sm.MaterialGuid);
    const uint32 vertexCount = static_cast<uint32>(sm.Vertices.size());
    const uint32 indexCount = static_cast<uint32>(sm.Indices.size());
    const uint8 hasColor = (sm.Color0.size() == static_cast<size_t>(vertexCount) * 4u) ? 1u : 0u;
    const uint8 hasUv1 = (sm.TexCoords1.size() == static_cast<size_t>(vertexCount) * 2u) ? 1u : 0u;
    WriteScalar(os, vertexCount);
    WriteScalar(os, indexCount);
    WriteScalar(os, hasColor);
    WriteScalar(os, hasUv1);
    if (vertexCount > 0)
        os.write(reinterpret_cast<const char*>(sm.Vertices.data()),
                 static_cast<std::streamsize>(vertexCount * sizeof(Vertex)));
    if (indexCount > 0)
        os.write(reinterpret_cast<const char*>(sm.Indices.data()),
                 static_cast<std::streamsize>(indexCount * sizeof(uint32)));
    if (hasColor)
        os.write(reinterpret_cast<const char*>(sm.Color0.data()),
                 static_cast<std::streamsize>(sm.Color0.size() * sizeof(float)));
    if (hasUv1)
        os.write(reinterpret_cast<const char*>(sm.TexCoords1.data()),
                 static_cast<std::streamsize>(sm.TexCoords1.size() * sizeof(float)));
}

void WriteMember(std::ostream& os, const BakedMemberRef& m) {
    WriteGuid(os, m.StableId);
    WriteScalar(os, m.MeshContentHash);
    WriteGuid(os, m.MaterialGuid);
    WriteScalar(os, m.MeshHandleKey);
    os.write(reinterpret_cast<const char*>(m.Transform), sizeof(m.Transform));
    WriteScalar(os, m.ChosenLod);
    WriteScalar(os, m.CastsShadow);
}

// Read a single submesh, fully bounds-validated. Returns false → Corrupt.
bool ReadSubmesh(const char* data, std::size_t size, std::size_t& cursor, BakedSubmesh& out) {
    uint32 vertexCount = 0, indexCount = 0;
    uint8 hasColor = 0, hasUv1 = 0;
    if (!ReadGuid(data, size, cursor, out.MaterialGuid) ||
        !ReadScalar(data, size, cursor, vertexCount) ||
        !ReadScalar(data, size, cursor, indexCount) ||
        !ReadScalar(data, size, cursor, hasColor) ||
        !ReadScalar(data, size, cursor, hasUv1))
        return false;
    if (vertexCount > kMaxVerticesPerSubmesh || indexCount > kMaxIndicesPerSubmesh)
        return false;
    if ((indexCount % 3u) != 0u)
        return false;

    if (vertexCount > RemainingElements(size, cursor, sizeof(Vertex)))
        return false;
    out.Vertices.resize(vertexCount);
    if (vertexCount > 0)
        std::memcpy(out.Vertices.data(), data + cursor, vertexCount * sizeof(Vertex));
    cursor += vertexCount * sizeof(Vertex);

    if (indexCount > RemainingElements(size, cursor, sizeof(uint32)))
        return false;
    out.Indices.resize(indexCount);
    if (indexCount > 0)
        std::memcpy(out.Indices.data(), data + cursor, indexCount * sizeof(uint32));
    cursor += indexCount * sizeof(uint32);
    // Every index must address a real vertex of this submesh (A4).
    for (uint32 idx : out.Indices)
        if (idx >= vertexCount)
            return false;

    if (hasColor) {
        const std::size_t floats = static_cast<std::size_t>(vertexCount) * 4u;
        if (floats > RemainingElements(size, cursor, sizeof(float)))
            return false;
        out.Color0.resize(floats);
        if (floats > 0)
            std::memcpy(out.Color0.data(), data + cursor, floats * sizeof(float));
        cursor += floats * sizeof(float);
    }
    if (hasUv1) {
        const std::size_t floats = static_cast<std::size_t>(vertexCount) * 2u;
        if (floats > RemainingElements(size, cursor, sizeof(float)))
            return false;
        out.TexCoords1.resize(floats);
        if (floats > 0)
            std::memcpy(out.TexCoords1.data(), data + cursor, floats * sizeof(float));
        cursor += floats * sizeof(float);
    }
    return true;
}

bool ReadMember(const char* data, std::size_t size, std::size_t& cursor, BakedMemberRef& out) {
    if (!ReadGuid(data, size, cursor, out.StableId) ||
        !ReadScalar(data, size, cursor, out.MeshContentHash) ||
        !ReadGuid(data, size, cursor, out.MaterialGuid) ||
        !ReadScalar(data, size, cursor, out.MeshHandleKey))
        return false;
    if (cursor + sizeof(out.Transform) > size)
        return false;
    std::memcpy(out.Transform, data + cursor, sizeof(out.Transform));
    cursor += sizeof(out.Transform);
    return ReadScalar(data, size, cursor, out.ChosenLod) &&
           ReadScalar(data, size, cursor, out.CastsShadow);
}

} // namespace

bool WriteHlodCache(const std::filesystem::path& file, const HlodBakedScene& scene) {
    if (file.empty())
        return false;

    std::error_code ec;
    const std::filesystem::path parent = file.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    const std::filesystem::path tmp = file.string() + UniqueTempSuffix();
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os)
            return false;

        WriteScalar(os, kHlodCacheMagic);
        WriteScalar(os, kHlodCacheFormatVersion);
        WriteScalar(os, scene.Key.SourceHash);
        WriteScalar(os, scene.Key.ConfigHash);
        WriteScalar(os, scene.CellSize);
        os.write(reinterpret_cast<const char*>(scene.GridOrigin), sizeof(scene.GridOrigin));
        WriteScalar(os, static_cast<uint32>(scene.Clusters.size()));

        for (const BakedCluster& cluster : scene.Clusters) {
            WriteScalar(os, cluster.Cell.X);
            WriteScalar(os, cluster.Cell.Y);
            WriteScalar(os, cluster.Cell.Z);
            os.write(reinterpret_cast<const char*>(cluster.SphereCenter), sizeof(cluster.SphereCenter));
            WriteScalar(os, cluster.SphereRadius);
            os.write(reinterpret_cast<const char*>(cluster.BoundsMin), sizeof(cluster.BoundsMin));
            os.write(reinterpret_cast<const char*>(cluster.BoundsMax), sizeof(cluster.BoundsMax));
            WriteScalar(os, cluster.ClusterSourceHash);
            WriteScalar(os, static_cast<uint32>(cluster.Members.size()));
            for (const BakedMemberRef& m : cluster.Members)
                WriteMember(os, m);
            WriteScalar(os, static_cast<uint32>(cluster.Submeshes.size()));
            for (const BakedSubmesh& sm : cluster.Submeshes)
                WriteSubmesh(os, sm);
        }

        os.flush();
        if (!os.good()) {
            os.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    return FileSystem::PublishFile(tmp, file);
}

HlodCacheStatus ReadHlodCache(const std::filesystem::path& file,
                              const uint64* expectConfigHash,
                              HlodBakedScene& out) {
    out = HlodBakedScene{};

    Vector<uint8> buffer;
    if (!ReadFileBytesShared(file, buffer))
        return HlodCacheStatus::Missing;

    const char* data = reinterpret_cast<const char*>(buffer.data());
    const std::size_t size = buffer.size();
    std::size_t cursor = 0;

    uint32 magic = 0, version = 0;
    if (!ReadScalar(data, size, cursor, magic) || magic != kHlodCacheMagic)
        return HlodCacheStatus::FormatMismatch;
    if (!ReadScalar(data, size, cursor, version) || version != kHlodCacheFormatVersion)
        return HlodCacheStatus::FormatMismatch;

    HlodBakedScene parsed;
    uint32 clusterCount = 0;
    if (!ReadScalar(data, size, cursor, parsed.Key.SourceHash) ||
        !ReadScalar(data, size, cursor, parsed.Key.ConfigHash) ||
        !ReadScalar(data, size, cursor, parsed.CellSize) ||
        cursor + sizeof(parsed.GridOrigin) > size)
        return HlodCacheStatus::Corrupt;
    std::memcpy(parsed.GridOrigin, data + cursor, sizeof(parsed.GridOrigin));
    cursor += sizeof(parsed.GridOrigin);
    if (!ReadScalar(data, size, cursor, clusterCount))
        return HlodCacheStatus::Corrupt;

    if (expectConfigHash && parsed.Key.ConfigHash != *expectConfigHash)
        return HlodCacheStatus::KeyMismatch;

    if (clusterCount > kMaxClusters)
        return HlodCacheStatus::Corrupt;

    parsed.Clusters.resize(clusterCount);
    for (uint32 c = 0; c < clusterCount; ++c) {
        BakedCluster& cluster = parsed.Clusters[c];
        uint32 memberCount = 0, submeshCount = 0;
        if (!ReadScalar(data, size, cursor, cluster.Cell.X) ||
            !ReadScalar(data, size, cursor, cluster.Cell.Y) ||
            !ReadScalar(data, size, cursor, cluster.Cell.Z) ||
            cursor + sizeof(cluster.SphereCenter) > size)
            return HlodCacheStatus::Corrupt;
        std::memcpy(cluster.SphereCenter, data + cursor, sizeof(cluster.SphereCenter));
        cursor += sizeof(cluster.SphereCenter);
        if (!ReadScalar(data, size, cursor, cluster.SphereRadius) ||
            cursor + sizeof(cluster.BoundsMin) > size)
            return HlodCacheStatus::Corrupt;
        std::memcpy(cluster.BoundsMin, data + cursor, sizeof(cluster.BoundsMin));
        cursor += sizeof(cluster.BoundsMin);
        if (cursor + sizeof(cluster.BoundsMax) > size)
            return HlodCacheStatus::Corrupt;
        std::memcpy(cluster.BoundsMax, data + cursor, sizeof(cluster.BoundsMax));
        cursor += sizeof(cluster.BoundsMax);
        if (!ReadScalar(data, size, cursor, cluster.ClusterSourceHash) ||
            !ReadScalar(data, size, cursor, memberCount))
            return HlodCacheStatus::Corrupt;
        if (memberCount > kMaxMembersPerCluster)
            return HlodCacheStatus::Corrupt;
        cluster.Members.resize(memberCount);
        for (uint32 m = 0; m < memberCount; ++m)
            if (!ReadMember(data, size, cursor, cluster.Members[m]))
                return HlodCacheStatus::Corrupt;

        if (!ReadScalar(data, size, cursor, submeshCount))
            return HlodCacheStatus::Corrupt;
        if (submeshCount > kMaxSubmeshesPerCluster)
            return HlodCacheStatus::Corrupt;
        cluster.Submeshes.resize(submeshCount);
        for (uint32 s = 0; s < submeshCount; ++s)
            if (!ReadSubmesh(data, size, cursor, cluster.Submeshes[s]))
                return HlodCacheStatus::Corrupt;
    }

    out = std::move(parsed);
    return HlodCacheStatus::Hit;
}

} // namespace Hlod
} // namespace GameEngine

#include "Assets/MeshLODCache.h"
#include "AssetCore/SharedFileRead.h"

#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"
#include "FileSystem/FileSystem.h"
#include "Types/Fnv1a.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>

// meshopt version pinned by the vcpkg manifest, injected by CMake so a version
// bump changes the config hash (a bump alters produced indices while the config
// is identical — without this a stale cook would be a silent hit; amendment
// A3/F7). Falls back to a sentinel if the build did not wire it.
#ifndef GE_MESHOPT_VERSION
#define GE_MESHOPT_VERSION "unpinned"
#endif

namespace GameEngine {

namespace {

// "GELD" — GameEngine LOD. Spelled so a hex dump reads G-E-L-D.
constexpr uint32 kLodCacheMagic = 0x47454C44u;

template <typename T>
void WriteScalar(std::ostream& os, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "WriteScalar requires trivially-copyable");
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
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

// One deserialized submesh, held until the whole file validates so a partial
// parse never mutates the caller's meshes.
struct ParsedMeshLods {
    Vector<Vector<uint32>> Lods;
    Vector<float> Errors;
    Vector<uint8> Sloppy;
    Vector<Vector<Vertex>> VertexBlocks; // v3: parallel to Lods; empty => index-only
    bool AnyOwnVertexLevel = false;
    bool Authored = false; // v2: skip the apply, keep the parse-supplied chain
};

// Per-write-unique temp suffix so concurrent writers of the SAME final file
// (two background PostLoad workers loading the same GUID) never alias one temp
// and interleave into a corrupt blob. Process token + thread id + counter is
// unique across threads, calls, and cooperating processes sharing a cache dir.
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

uint64 ComputeLodSourceHash(const uint8* data, std::size_t size) {
    return Hashing::Fnv1a64(data, size);
}

uint64 ComputeLodConfigHash(const MeshLODConfig& config, bool generateSkinned,
                            uint64 parseOptionsHash, uint32 authoredImportVersion) {
    uint64 h = Hashing::kFnv1a64OffsetBasis;
    h = Hashing::Fnv1a64Value(h, config.LodCount);
    h = Hashing::Fnv1a64(config.TargetRatios, sizeof(config.TargetRatios), h);
    h = Hashing::Fnv1a64(config.TargetError, sizeof(config.TargetError), h);
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(config.BorderRule));
    h = Hashing::Fnv1a64Value(h, config.NormalWeight);
    h = Hashing::Fnv1a64Value(h, config.UvWeight);
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(config.AllowSloppy));
    h = Hashing::Fnv1a64Value(h, config.SloppyRatioThreshold);
    h = Hashing::Fnv1a64Value(h, static_cast<uint8>(generateSkinned));
    // Fold parse options only when present, so formats without any (0) hash
    // byte-identically to the pre-parse-options key and their caches survive.
    if (parseOptionsHash != 0)
        h = Hashing::Fnv1a64Value(h, parseOptionsHash);
    h = Hashing::Fnv1a64Value(h, kLodGeneratorVersion);
    h = Hashing::Fnv1a64Value(h, authoredImportVersion);
    const char* meshoptVersion = GE_MESHOPT_VERSION;
    h = Hashing::Fnv1a64(meshoptVersion, std::strlen(meshoptVersion), h);
    return h;
}

uint64 ComputeGeneratedLodHash(const Vector<Mesh>& meshes) {
    uint64 h = Hashing::kFnv1a64OffsetBasis;
    for (const Mesh& mesh : meshes) {
        // Authored submeshes cook as zero-LOD entries (their LOD-local chain is
        // not serialized), so the provenance fingerprint counts them as zero-LOD
        // to stay consistent with the bytes WriteLodCache actually emits.
        if (mesh.HasAuthoredLODs()) {
            h = Hashing::Fnv1a64Value(h, static_cast<uint32>(0u));
            continue;
        }
        h = Hashing::Fnv1a64Value(h, static_cast<uint32>(mesh.ExtraLODs.size()));
        for (size_t j = 0; j < mesh.ExtraLODs.size(); ++j) {
            const Vector<uint32>& lod = mesh.ExtraLODs[j];
            h = Hashing::Fnv1a64Value(h, static_cast<uint32>(lod.size()));
            if (!lod.empty())
                h = Hashing::Fnv1a64(lod.data(), lod.size() * sizeof(uint32), h);
            // Own-vertex shell blocks are cooked bytes too (v3); fold them so
            // the provenance fingerprint tracks what WriteLodCache emits. An
            // index-only level folds a constant 0, keeping pre-shell chains'
            // structure.
            const bool ownVerts = j < mesh.ExtraLODVertices.size() &&
                                  !mesh.ExtraLODVertices[j].empty();
            const uint32 blockSize =
                ownVerts ? static_cast<uint32>(mesh.ExtraLODVertices[j].size()) : 0u;
            h = Hashing::Fnv1a64Value(h, blockSize);
            if (ownVerts)
                h = Hashing::Fnv1a64(mesh.ExtraLODVertices[j].data(),
                                     blockSize * sizeof(Vertex), h);
        }
        if (!mesh.ExtraLODErrors.empty())
            h = Hashing::Fnv1a64(mesh.ExtraLODErrors.data(),
                                 mesh.ExtraLODErrors.size() * sizeof(float), h);
    }
    return h;
}

bool WriteLodCache(const std::filesystem::path& file,
                   const LodCacheKey& key,
                   uint64 generatedHash,
                   const Vector<Mesh>& meshes) {
    if (file.empty())
        return false;

    std::error_code ec;
    const std::filesystem::path parent = file.parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    // Atomic write: assemble in a per-writer-unique temp next to the target, then
    // rename. A crash mid-write leaves the temp, never a truncated .gelod that
    // passes the header; the unique suffix keeps concurrent writers of the same
    // target from interleaving into one temp.
    const std::filesystem::path tmp = file.string() + UniqueTempSuffix();
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os)
            return false;

        WriteScalar(os, kLodCacheMagic);
        WriteScalar(os, kLodCacheFormatVersion);
        WriteScalar(os, key.SourceHash);
        WriteScalar(os, key.ConfigHash);
        WriteScalar(os, generatedHash);
        WriteScalar(os, static_cast<uint32>(meshes.size()));

        for (uint32 i = 0; i < static_cast<uint32>(meshes.size()); ++i) {
            const Mesh& mesh = meshes[i];
            // Authored submeshes' chains come from the source parse, not the
            // cook, and must not be clobbered on read: emit a zero-LOD authored
            // entry so the submesh array stays aligned and the reader knows to
            // leave the parse-supplied chain intact. (Generated own-vertex
            // shells are NOT authored — their blocks serialize below.)
            const uint8 authored = mesh.HasAuthoredLODs() ? 1u : 0u;
            const uint32 lodCount =
                authored ? 0u : static_cast<uint32>(mesh.ExtraLODs.size());
            WriteScalar(os, i);
            WriteScalar(os, static_cast<uint32>(mesh.Vertices.size()));
            WriteScalar(os, authored);
            WriteScalar(os, lodCount);
            for (uint32 j = 0; j < lodCount; ++j) {
                const Vector<uint32>& indices = mesh.ExtraLODs[j];
                const float error = j < mesh.ExtraLODErrors.size() ? mesh.ExtraLODErrors[j] : 0.0f;
                const uint8 sloppy = j < mesh.ExtraLODSloppy.size() ? mesh.ExtraLODSloppy[j] : 0u;
                // v3: 0 => index-only level over the parsed vertices; non-zero =>
                // the level owns this many Vertex records (LOD-local indices).
                const bool ownVerts = j < mesh.ExtraLODVertices.size() &&
                                      !mesh.ExtraLODVertices[j].empty();
                const uint32 blockSize =
                    ownVerts ? static_cast<uint32>(mesh.ExtraLODVertices[j].size()) : 0u;
                WriteScalar(os, static_cast<uint32>(indices.size()));
                WriteScalar(os, error);
                WriteScalar(os, sloppy);
                WriteScalar(os, blockSize);
                if (!indices.empty())
                    os.write(reinterpret_cast<const char*>(indices.data()),
                             static_cast<std::streamsize>(indices.size() * sizeof(uint32)));
                if (ownVerts)
                    os.write(reinterpret_cast<const char*>(mesh.ExtraLODVertices[j].data()),
                             static_cast<std::streamsize>(blockSize * sizeof(Vertex)));
            }
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

LodCacheStatus ReadLodCacheInto(const std::filesystem::path& file,
                                const LodCacheKey* expectKey,
                                Vector<Mesh>& meshes) {
    Vector<uint8> buffer;
    if (!ReadFileBytesShared(file, buffer))
        return LodCacheStatus::Missing;

    const char* data = reinterpret_cast<const char*>(buffer.data());
    const std::size_t size = buffer.size();
    std::size_t cursor = 0;

    uint32 magic = 0;
    uint32 version = 0;
    if (!ReadScalar(data, size, cursor, magic) || magic != kLodCacheMagic)
        return LodCacheStatus::FormatMismatch;
    if (!ReadScalar(data, size, cursor, version) || version != kLodCacheFormatVersion)
        return LodCacheStatus::FormatMismatch;

    uint64 sourceHash = 0;
    uint64 configHash = 0;
    uint64 generatedHash = 0;
    uint32 meshCount = 0;
    if (!ReadScalar(data, size, cursor, sourceHash) ||
        !ReadScalar(data, size, cursor, configHash) ||
        !ReadScalar(data, size, cursor, generatedHash) ||
        !ReadScalar(data, size, cursor, meshCount))
        return LodCacheStatus::Corrupt;

    if (expectKey &&
        (sourceHash != expectKey->SourceHash || configHash != expectKey->ConfigHash))
        return LodCacheStatus::KeyMismatch;

    if (meshCount != static_cast<uint32>(meshes.size()))
        return LodCacheStatus::StructureMismatch;

    Vector<ParsedMeshLods> parsed(meshCount);
    for (uint32 i = 0; i < meshCount; ++i) {
        uint32 submeshIndex = 0;
        uint32 vertexCount = 0;
        uint8  authored = 0;
        uint32 lodCount = 0;
        if (!ReadScalar(data, size, cursor, submeshIndex) ||
            !ReadScalar(data, size, cursor, vertexCount) ||
            !ReadScalar(data, size, cursor, authored) ||
            !ReadScalar(data, size, cursor, lodCount))
            return LodCacheStatus::Corrupt;

        if (submeshIndex != i ||
            vertexCount != static_cast<uint32>(meshes[i].Vertices.size()))
            return LodCacheStatus::StructureMismatch;
        if (lodCount > MeshLODConfig::kMaxLODs)
            return LodCacheStatus::Corrupt;

        ParsedMeshLods& out = parsed[i];
        out.Authored = authored != 0u;
        if (authored > 1u || (out.Authored && lodCount != 0u))
            return LodCacheStatus::Corrupt;
        if (out.Authored != meshes[i].HasAuthoredLODs())
            return LodCacheStatus::StructureMismatch;
        out.Lods.resize(lodCount);
        out.Errors.resize(lodCount);
        out.Sloppy.resize(lodCount);
        out.VertexBlocks.resize(lodCount);
        for (uint32 j = 0; j < lodCount; ++j) {
            uint32 indexCount = 0;
            float error = 0.0f;
            uint8 sloppy = 0;
            uint32 blockSize = 0;
            if (!ReadScalar(data, size, cursor, indexCount) ||
                !ReadScalar(data, size, cursor, error) ||
                !ReadScalar(data, size, cursor, sloppy) ||
                !ReadScalar(data, size, cursor, blockSize))
                return LodCacheStatus::Corrupt;

            // A triangle list with at least one triangle; an index array whose
            // bytes fit the file remainder (overflow-safe divide, not multiply).
            if (indexCount < 3u || (indexCount % 3u) != 0u)
                return LodCacheStatus::Corrupt;
            const std::size_t remainingIndices = (size - cursor) / sizeof(uint32);
            if (indexCount > remainingIndices)
                return LodCacheStatus::Corrupt;
            // An own-vertex block can never carry more vertices than indices (a
            // per-face shell has exactly indexCount; any indexed list referencing
            // all its vertices has <=).
            if (blockSize > indexCount)
                return LodCacheStatus::Corrupt;

            Vector<uint32>& lodIndices = out.Lods[j];
            lodIndices.resize(indexCount);
            std::memcpy(lodIndices.data(), data + cursor, indexCount * sizeof(uint32));
            cursor += indexCount * sizeof(uint32);

            if (blockSize > 0u) {
                const std::size_t remainingVerts = (size - cursor) / sizeof(Vertex);
                if (blockSize > remainingVerts)
                    return LodCacheStatus::Corrupt;
                Vector<Vertex>& block = out.VertexBlocks[j];
                block.resize(blockSize);
                static_assert(std::is_trivially_copyable_v<Vertex>,
                              "raw .gelod vertex block requires trivially-copyable Vertex");
                std::memcpy(block.data(), data + cursor, blockSize * sizeof(Vertex));
                cursor += blockSize * sizeof(Vertex);
                out.AnyOwnVertexLevel = true;
            }

            // A4: a truncated / corrupt blob must never hand OOB indices to GPU
            // upload. An index-only level must address the submesh's parsed
            // vertices; an own-vertex level is LOD-local and must address its
            // own block.
            const uint32 indexBound = blockSize > 0u ? blockSize : vertexCount;
            for (uint32 idx : lodIndices) {
                if (idx >= indexBound)
                    return LodCacheStatus::Corrupt;
            }
            out.Errors[j] = error;
            out.Sloppy[j] = sloppy;
        }
    }

    // Every mesh validated — apply. Only now do we touch the caller's meshes.
    for (uint32 i = 0; i < meshCount; ++i) {
        // Authored submeshes were cooked as zero-LOD placeholders; their real
        // (LOD-local, own-vertex) chain came from the source parse and lives in
        // meshes[i] already. Overwriting here would clobber it — leave it intact.
        if (parsed[i].Authored)
            continue;
        Mesh& mesh = meshes[i];
        mesh.ExtraLODColor0.clear(); // generated replacement never owns RGBA blocks
        mesh.ExtraLODs = std::move(parsed[i].Lods);
        mesh.ExtraLODErrors = std::move(parsed[i].Errors);
        mesh.ExtraLODSloppy = std::move(parsed[i].Sloppy);
        // Same canonical form GenerateMeshLODsInto produces: an all-index-only
        // chain keeps an EMPTY block array (not N empty blocks), so the GPU
        // content hash agrees between the generate and cache-hit paths.
        if (parsed[i].AnyOwnVertexLevel)
            mesh.ExtraLODVertices = std::move(parsed[i].VertexBlocks);
        else
            mesh.ExtraLODVertices.clear();
    }
    return LodCacheStatus::Hit;
}

} // namespace GameEngine

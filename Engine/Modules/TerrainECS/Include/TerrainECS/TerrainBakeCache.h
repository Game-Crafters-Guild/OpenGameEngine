#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace GameEngine::TerrainECS
{

/// Version of a single terrain's baked output and of the .getbake layout. Bump it
/// with every change that can alter a baked height or splat texel for unchanged
/// inputs (the base noise, a modifier evaluator, the splat rules, the band split),
/// and with every layout change. It seeds every bake key, so a bump turns every
/// cached artifact into a miss.
///
/// v2: one artifact per terrain identity (scene GUID + entity tag), the key in the
/// header.
inline constexpr uint32 kTerrainBakeFormatVersion = 2u;

/// Accumulates a terrain bake's inputs into its content key: FNV-1a 64 over the
/// bytes fed in, in order, seeded with kTerrainBakeFormatVersion. Feed only
/// portable content (authored values, payload and asset bytes); never a runtime
/// handle, generation, pointer or per-process counter, or two processes baking
/// the same scene would disagree.
class TerrainBakeKeyBuilder
{
public:
    TerrainBakeKeyBuilder();

    void Bytes(const void* data, std::size_t size);

    template <typename T>
    void Value(const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>,
                      "feed a padding-free value; pad-carrying structs must be fed field by field");
        Bytes(&value, sizeof(T));
    }

    void Float(float32 value) { Bytes(&value, sizeof(value)); }

    uint64 Key() const { return m_Hash; }

private:
    uint64 m_Hash;
};

/// What a single (untiled) terrain's full modifier bake produces.
struct TerrainBakeArtifact
{
    uint32 Width = 0;
    uint32 Height = 0;
    std::vector<float32> Heights; // Width * Height samples, row-major
    std::vector<uint8> Splat;     // Width * Height RGBA8 texels, row-major
    float32 SplatMinH = 0.0f;     // the height range the splat was normalized against
    float32 SplatMaxH = 0.0f;
};

enum class TerrainBakeCacheStatus : uint8
{
    Hit,
    Missing,        // no file at the terrain's path, or unreadable
    FormatMismatch, // wrong magic or format version
    KeyMismatch,    // the file holds an earlier state of the terrain: the ordinary miss
    Corrupt,        // truncated, or a size that disagrees with the terrain's grid
};

const char* TerrainBakeCacheStatusName(TerrainBakeCacheStatus status);

/// Where this process looks for baked terrains and whether it may add to them.
/// An empty Directory turns the cache off.
struct TerrainBakeCacheConfig
{
    std::filesystem::path Directory;
    bool Writable = false;
};

/// The one artifact path of a terrain: <directory>/<scene GUID>/<FNV-1a 64 of the
/// entity's scene tag, 16 hex digits>.getbake. A terrain is identified by the
/// scene asset it was loaded from and its persistent per-entity tag in that scene,
/// so each terrain owns exactly one file and a store replaces it. Empty when the
/// terrain has no identity (no source scene, or an untagged entity): such a
/// terrain is not cached.
std::filesystem::path TerrainBakeFile(const std::filesystem::path& directory, const GUID& scene,
                                      std::string_view entityTag);

/// Writes `artifact` under `key`: assembled in a temporary sibling, then published
/// by rename over the terrain's previous artifact, so a reader never sees a partial
/// file. Removes the temporary siblings of writers no longer running, left by a
/// writer that died between its write and its rename. False on any I/O failure.
bool WriteTerrainBake(const std::filesystem::path& file, uint64 key, const TerrainBakeArtifact& artifact);

/// Reads and validates the artifact at `file` for a terrain of `width` x `height`
/// samples whose current inputs give `key`. Every size is checked against the
/// expected grid before anything is allocated; `out` is written only on a Hit.
/// Never throws.
TerrainBakeCacheStatus ReadTerrainBake(const std::filesystem::path& file, uint64 key, uint32 width, uint32 height,
                                       TerrainBakeArtifact& out);

/// The key in an artifact's header, or none when the file is missing or not a
/// current-format artifact. Reads the header only.
std::optional<uint64> ReadTerrainBakeKey(const std::filesystem::path& file);

/// Removes from `scene`'s folder of `directory` every artifact not in `keep`: the
/// artifacts of terrains the scene no longer has. Other scenes' folders are never
/// touched.
void PruneTerrainBakeScene(const std::filesystem::path& directory, const GUID& scene,
                           std::span<const std::filesystem::path> keep);

/// One shipped scene's baked terrains, as StageTerrainBakeArtifacts found them.
struct TerrainBakeStagedScene
{
    GUID Scene;
    uint32 Artifacts = 0;
    uint64 Bytes = 0;
};

/// Copies the artifacts of exactly the given scenes from a cache directory to a
/// package's directory, keeping the <scene GUID>/ layout. Artifacts of any other
/// scene stay behind. Returns one entry per scene, with 0 artifacts for a scene
/// that has none stored (its terrains bake at load); `outFailures` receives the
/// files that could not be copied.
std::vector<TerrainBakeStagedScene> StageTerrainBakeArtifacts(const std::filesystem::path& cacheDirectory,
                                                              std::span<const GUID> scenes,
                                                              const std::filesystem::path& packageDirectory,
                                                              std::vector<std::filesystem::path>& outFailures);

} // namespace GameEngine::TerrainECS

#pragma once

// The sample grid of a raw terrain heightmap (.r16 = uint16 samples, .r32 = float32 samples), which
// the file itself does not record: a raw file is the samples, row-major (rows along +Z, samples
// along +X), and nothing else. The width and height are the asset's import settings; without them
// a square grid is assumed. A non-square file whose sample count is a perfect square (16384 x 4096
// = 8192^2) reads as a square grid of the wrong stride, so its rows interleave and the terrain
// renders as stripes: a non-square heightmap must declare its shape.

#include "Types/Types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace GameEngine
{
class AssetRegistry;
}

namespace GameEngine::Terrain
{
class HeightfieldData;
}

namespace GameEngine::TerrainECS
{

// Import-setting keys (the asset database's per-asset key/value store) holding a raw heightmap's
// sample counts. Both are set, or neither.
inline constexpr const char* kRawHeightmapWidthKey = "heightmap.width";
inline constexpr const char* kRawHeightmapHeightKey = "heightmap.height";

struct RawHeightmapLayout
{
    uint32 Width = 0;  // samples along X
    uint32 Height = 0; // samples along Z
};

// The grid of a raw heightmap of `fileSizeBytes` with `bytesPerSample`-byte samples.
// `declaredWidth` and `declaredHeight` are the import settings (0 = unset). Both declared, they must
// account for every byte of the file; one declared, the other is the file's sample count divided by
// it (which must divide evenly); neither, the file must hold a square grid. Returns an empty
// string and fills `out` on success, else the reason, worded as the fix (`out` is left zero).
std::string ResolveRawHeightmapLayout(uint64 fileSizeBytes, std::size_t bytesPerSample,
                                      uint32 declaredWidth, uint32 declaredHeight,
                                      RawHeightmapLayout& out);

// The grid the raw heightmap file at `path` (.r16 or .r32, by extension) is read on, from its size
// and the declared width and height, as ResolveRawHeightmapLayout.
std::string ResolveRawHeightmapFileLayout(const std::filesystem::path& path, uint32 declaredWidth,
                                          uint32 declaredHeight, RawHeightmapLayout& out);

// A raw heightmap's two grid settings as the store holds them: text, or absent.
struct RawHeightmapSettings
{
    std::optional<std::string> Width;
    std::optional<std::string> Height;
};

// The grid settings of the asset at `path`, as stored.
RawHeightmapSettings ReadRawHeightmapSettings(const AssetRegistry& registry, const std::filesystem::path& path);

// One stored grid setting as a sample count: absent, "" or "0" is 0 (unset: the store cannot remove
// a value, so an undo writes "" and a field set back to 0 stores "0"), anything else must be a whole
// number with nothing around it. Returns an empty string and fills `out` on success, else the
// reason, worded as the fix (`out` is left 0).
std::string ParseRawHeightmapSampleCount(std::string_view key, const std::optional<std::string>& text,
                                         uint32& out);

// The grid the settings declare (zero where unset), or the reason a present setting cannot be read.
std::string ResolveDeclaredRawHeightmapLayout(const RawHeightmapSettings& settings, RawHeightmapLayout& out);

// Decode the raw heightmap at `path` (.r16 or .r32, by extension) into `out` on the grid
// ResolveRawHeightmapLayout gives it. .r32 samples are used as they are (meters when the terrain's
// Height Scale is 1); .r16 samples are normalized to [0, 1]. Returns an empty string on success,
// else the reason, worded as the fix (`out` is left empty).
std::string DecodeRawHeightmap(const std::filesystem::path& path, uint32 declaredWidth,
                               uint32 declaredHeight, Terrain::HeightfieldData& out);

} // namespace GameEngine::TerrainECS

#include "TerrainECS/RawHeightmap.h"

#include "Assets/AssetRegistry.h"
#include "Terrain/Heightfield.h"
#include "Types/StringUtils.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <utility>
#include <system_error>

namespace GameEngine::TerrainECS
{

namespace
{

// The sides that read the whole file as a grid of 2 or more samples each way (the divisors of
// `count` from 2 to count / 2) nearest `value` from below and from above; 0 where there is none.
// What a typed side that does not fit the file could have meant.
std::pair<uint64, uint64> NearestSides(uint64 count, uint64 value)
{
    uint64 below = 0;
    uint64 above = 0;
    for (uint64 d = 2; d * d <= count; ++d)
    {
        if (count % d != 0)
            continue;
        for (const uint64 side : {d, count / d})
        {
            if (side > count / 2)
                continue;
            if (side <= value && side > below)
                below = side;
            if (side >= value && (above == 0 || side < above))
                above = side;
        }
    }
    return {below, above};
}

// "the nearest that do are A or B", "the nearest that does is A", or that no grid reads the file at all (a prime sample count,
// or fewer than 4 samples).
std::string NearestSidesText(uint64 count, uint64 value)
{
    const auto [below, above] = NearestSides(count, value);
    if (below == 0 && above == 0)
        return "no grid of 2 or more samples each way reads this file";
    if (below != 0 && above != 0 && below != above)
        return "the nearest that do are " + FormatGroupedInteger(below) + " or " + FormatGroupedInteger(above);
    return "the nearest that does is " + FormatGroupedInteger(below != 0 ? below : above);
}

} // namespace

std::string ResolveRawHeightmapLayout(uint64 fileSizeBytes, std::size_t bytesPerSample,
                                      uint32 declaredWidth, uint32 declaredHeight,
                                      RawHeightmapLayout& out)
{
    out = {};
    if (bytesPerSample == 0 || fileSizeBytes == 0 || fileSizeBytes % bytesPerSample != 0)
    {
        return "the file is " + FormatGroupedInteger(fileSizeBytes) + " bytes, not a whole number of " +
               std::to_string(bytesPerSample) + "-byte samples";
    }
    const uint64 sampleCount = fileSizeBytes / bytesPerSample;
    const std::string held = "the file holds " + FormatGroupedInteger(sampleCount) +
                             (sampleCount == 1 ? " sample" : " samples");

    // One side declared: the other is what the file's sample count leaves, so one typed number is
    // enough. It must divide the count, or no grid with that side reads the whole file.
    if ((declaredWidth == 0) != (declaredHeight == 0))
    {
        const bool widthDeclared = declaredWidth != 0;
        const uint64 side = widthDeclared ? declaredWidth : declaredHeight;
        const char* const sideName = widthDeclared ? "Samples X" : "Samples Z";
        if (side < 2 || sampleCount % side != 0 || sampleCount / side < 2 ||
            sampleCount / side > std::numeric_limits<uint32>::max())
        {
            return held + ", which " + sideName + " " + FormatGroupedInteger(side) +
                   " does not divide into rows of 2 samples or more; " + NearestSidesText(sampleCount, side);
        }
        const auto other = static_cast<uint32>(sampleCount / side);
        out = widthDeclared ? RawHeightmapLayout{declaredWidth, other} : RawHeightmapLayout{other, declaredHeight};
        return {};
    }

    if (declaredWidth != 0)
    {
        if (declaredWidth < 2 || declaredHeight < 2)
            return "Samples X and Samples Z must each be 2 or more (or 0 to read that side from the file)";
        if (static_cast<uint64>(declaredWidth) * declaredHeight != sampleCount)
        {
            return "Samples X " + FormatGroupedInteger(declaredWidth) + " x Samples Z " +
                   FormatGroupedInteger(declaredHeight) + " is " +
                   FormatGroupedInteger(static_cast<uint64>(declaredWidth) * declaredHeight) + " samples, but " +
                   held + "; set Samples X and Samples Z to the grid it was exported at, or one of them and 0 "
                   "for the other";
        }
        out = {declaredWidth, declaredHeight};
        return {};
    }

    const auto side = static_cast<uint32>(std::llround(std::sqrt(static_cast<double>(sampleCount))));
    if (side < 2 || static_cast<uint64>(side) * side != sampleCount)
        return held + ", which is not a square grid; set Samples X (or Samples Z) in the heightmap's Import Settings";
    out = {side, side};
    return {};
}

std::string ResolveRawHeightmapFileLayout(const std::filesystem::path& path, uint32 declaredWidth,
                                          uint32 declaredHeight, RawHeightmapLayout& out)
{
    out = {};
    const std::string ext = ToLowerAscii(path.extension().string());
    if (ext != ".r16" && ext != ".r32")
        return "a raw heightmap is a .r16 or .r32 file";
    const std::size_t bytesPerSample = ext == ".r16" ? sizeof(uint16) : sizeof(float32);

    std::error_code ec;
    const uint64 fileSize = std::filesystem::file_size(path, ec);
    if (ec)
        return "the file could not be read (" + ec.message() + ")";
    return ResolveRawHeightmapLayout(fileSize, bytesPerSample, declaredWidth, declaredHeight, out);
}

RawHeightmapSettings ReadRawHeightmapSettings(const AssetRegistry& registry, const std::filesystem::path& path)
{
    RawHeightmapSettings settings;
    std::string text;
    if (registry.TryGetMetaValue(path, kRawHeightmapWidthKey, text))
        settings.Width = text;
    if (registry.TryGetMetaValue(path, kRawHeightmapHeightKey, text))
        settings.Height = text;
    return settings;
}

std::string ParseRawHeightmapSampleCount(std::string_view key, const std::optional<std::string>& text, uint32& out)
{
    out = 0;
    // Absent, empty and "0" are all unset: the store cannot remove a value, so an undo of the first
    // edit writes back "" and a field set back to 0 stores "0".
    if (!text || text->empty())
        return {};
    uint32 value = 0;
    const char* const begin = text->data();
    const char* const end = begin + text->size();
    const auto [stop, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || stop != end)
    {
        const char* const label = key == kRawHeightmapWidthKey ? "Samples X" : "Samples Z";
        return std::string(label) + " is stored as \"" + *text +
               "\"; type the number of samples, or 0 to read the other side from the file";
    }
    out = value;
    return {};
}

std::string ResolveDeclaredRawHeightmapLayout(const RawHeightmapSettings& settings, RawHeightmapLayout& out)
{
    out = {};
    RawHeightmapLayout declared;
    if (std::string reason = ParseRawHeightmapSampleCount(kRawHeightmapWidthKey, settings.Width, declared.Width);
        !reason.empty())
        return reason;
    if (std::string reason = ParseRawHeightmapSampleCount(kRawHeightmapHeightKey, settings.Height, declared.Height);
        !reason.empty())
        return reason;
    out = declared;
    return {};
}

std::string DecodeRawHeightmap(const std::filesystem::path& path, uint32 declaredWidth,
                               uint32 declaredHeight, Terrain::HeightfieldData& out)
{
    out = Terrain::HeightfieldData{};
    RawHeightmapLayout layout;
    if (std::string reason = ResolveRawHeightmapFileLayout(path, declaredWidth, declaredHeight, layout);
        !reason.empty())
        return reason;

    const bool loaded = ToLowerAscii(path.extension().string()) == ".r16"
                            ? out.LoadFromRawUInt16(path, layout.Width, layout.Height)
                            : out.LoadFromRawFloat(path, layout.Width, layout.Height);
    if (!loaded)
    {
        out = Terrain::HeightfieldData{};
        return "the file could not be read";
    }
    return {};
}

} // namespace GameEngine::TerrainECS

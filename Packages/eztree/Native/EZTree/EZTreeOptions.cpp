// Native C++ option helpers for @dgreenheck/ez-tree.
// Portions adapted from EZ-Tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.

#include "EZTree/EZTreeOptions.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace GameEngine::EZTree
{
namespace
{

std::string Lower(std::string_view v)
{
    std::string out(v);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

template <typename T>
void HashBytes(uint64& h, const T& value)
{
    const auto* data = reinterpret_cast<const uint8*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        h ^= static_cast<uint64>(data[i]);
        h *= 1099511628211ull;
    }
}

float FiniteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

float ClampFinite(float value, float minValue, float maxValue, float fallback)
{
    return std::clamp(FiniteOr(value, fallback), minValue, maxValue);
}

void HashVec2(uint64& h, const Vec2& v)
{
    HashBytes(h, v.x);
    HashBytes(h, v.y);
}

void HashVec3(uint64& h, const Mathematics::Vector3& v)
{
    HashBytes(h, v.x);
    HashBytes(h, v.y);
    HashBytes(h, v.z);
}

template <typename T, size_t N>
void HashArray(uint64& h, const std::array<T, N>& values)
{
    for (const T& value : values)
        HashBytes(h, value);
}

void HashBranchOverride(uint64& h, const BranchOverride& branchOverride)
{
    HashBytes(h, branchOverride.enabled);
    HashBytes(h, branchOverride.branchId);
    HashBytes(h, branchOverride.level);
    HashBytes(h, branchOverride.lengthScale);
    HashBytes(h, branchOverride.radiusScale);
    HashBytes(h, branchOverride.angleOffsetDegrees);
    HashBytes(h, branchOverride.twistOffset);
}

} // namespace

TreeOptions MakeDefaultOptions()
{
    return {};
}

std::string ToString(TreeType v)
{
    return v == TreeType::Evergreen ? "Evergreen" : "Deciduous";
}

std::string ToString(BarkType v)
{
    switch (v)
    {
    case BarkType::Birch: return "Birch";
    case BarkType::Pine: return "Pine";
    case BarkType::Willow: return "Willow";
    case BarkType::Oak:
    default: return "Oak";
    }
}

std::string ToString(LeafType v)
{
    switch (v)
    {
    case LeafType::Ash: return "Ash";
    case LeafType::Aspen: return "Aspen";
    case LeafType::Pine: return "Pine";
    case LeafType::Oak:
    default: return "Oak";
    }
}

std::string ToString(BillboardMode v)
{
    return v == BillboardMode::Single ? "Single" : "Double";
}

TreeType TreeTypeFromString(std::string_view v, TreeType fallback)
{
    const std::string s = Lower(v);
    if (s == "evergreen")
        return TreeType::Evergreen;
    if (s == "deciduous")
        return TreeType::Deciduous;
    return fallback;
}

BarkType BarkTypeFromString(std::string_view v, BarkType fallback)
{
    const std::string s = Lower(v);
    if (s == "birch" || s == "bark002")
        return BarkType::Birch;
    if (s == "pine" || s == "bark003")
        return BarkType::Pine;
    if (s == "willow")
        return BarkType::Willow;
    if (s == "oak" || s == "bark001")
        return BarkType::Oak;
    return fallback;
}

LeafType LeafTypeFromString(std::string_view v, LeafType fallback)
{
    const std::string s = Lower(v);
    if (s == "ash")
        return LeafType::Ash;
    if (s == "aspen")
        return LeafType::Aspen;
    if (s == "pine")
        return LeafType::Pine;
    if (s == "oak")
        return LeafType::Oak;
    return fallback;
}

BillboardMode BillboardModeFromString(std::string_view v, BillboardMode fallback)
{
    const std::string s = Lower(v);
    if (s == "single")
        return BillboardMode::Single;
    if (s == "double")
        return BillboardMode::Double;
    return fallback;
}

TreeOptions SanitizeOptions(TreeOptions options)
{
    constexpr uint32 kMaxLevels = 3u;
    constexpr uint32 kMaxSections = 128u;
    constexpr uint32 kMaxSegments = 64u;
    constexpr uint32 kMaxLeavesPerBranch = 32u;
    constexpr uint32 kMaxAtlasSize = 32u;

    options.bark.textureScale.x = ClampFinite(options.bark.textureScale.x, 0.0001f, 1024.0f, 1.0f);
    options.bark.textureScale.y = ClampFinite(options.bark.textureScale.y, 0.0001f, 1024.0f, 1.0f);

    options.branch.levels = std::min(options.branch.levels, kMaxLevels);
    const std::array<uint32, 4> childCaps = options.branch.levels <= 1u
        ? std::array<uint32, 4>{128u, 16u, 16u, 0u}
        : (options.branch.levels == 2u
            ? std::array<uint32, 4>{32u, 16u, 8u, 0u}
            : std::array<uint32, 4>{16u, 8u, 8u, 0u});
    options.branch.forceDirection.x = FiniteOr(options.branch.forceDirection.x, 0.0f);
    options.branch.forceDirection.y = FiniteOr(options.branch.forceDirection.y, 1.0f);
    options.branch.forceDirection.z = FiniteOr(options.branch.forceDirection.z, 0.0f);
    options.branch.forceStrength = ClampFinite(options.branch.forceStrength, -16.0f, 16.0f, 0.01f);
    for (uint32 i = 0; i < options.branch.angle.size(); ++i)
    {
        options.branch.angle[i] = ClampFinite(options.branch.angle[i], -360.0f, 360.0f, 0.0f);
        options.branch.children[i] = std::min(options.branch.children[i], childCaps[i]);
        options.branch.gnarliness[i] = ClampFinite(options.branch.gnarliness[i], 0.0f, 16.0f, 0.0f);
        options.branch.length[i] = ClampFinite(options.branch.length[i], 0.001f, 512.0f, 1.0f);
        options.branch.radius[i] = ClampFinite(options.branch.radius[i], 0.001f, 64.0f, 0.1f);
        options.branch.sections[i] = std::clamp(options.branch.sections[i], 1u, kMaxSections);
        options.branch.segments[i] = std::clamp(options.branch.segments[i], 3u, kMaxSegments);
        options.branch.start[i] = ClampFinite(options.branch.start[i], 0.0f, 1.0f, 0.0f);
        options.branch.taper[i] = ClampFinite(options.branch.taper[i], 0.0f, 1.0f, 0.7f);
        options.branch.twist[i] = ClampFinite(options.branch.twist[i], -6.2831855f, 6.2831855f, 0.0f);
    }

    options.leaves.angle = ClampFinite(options.leaves.angle, -360.0f, 360.0f, 10.0f);
    options.leaves.count = std::min(options.leaves.count, kMaxLeavesPerBranch);
    options.leaves.start = ClampFinite(options.leaves.start, 0.0f, 1.0f, 0.0f);
    options.leaves.size = ClampFinite(options.leaves.size, 0.001f, 128.0f, 2.5f);
    options.leaves.sizeVariance = ClampFinite(options.leaves.sizeVariance, 0.0f, 4.0f, 0.7f);
    options.leaves.alphaTest = ClampFinite(options.leaves.alphaTest, 0.0f, 1.0f, 0.5f);
    options.leaves.textureColumns = std::clamp(options.leaves.textureColumns, 1u, kMaxAtlasSize);
    options.leaves.textureRows = std::clamp(options.leaves.textureRows, 1u, kMaxAtlasSize);
    const uint32 maxTile = options.leaves.textureColumns * options.leaves.textureRows - 1u;
    options.leaves.textureTile = std::min(options.leaves.textureTile, maxTile);
    options.leaves.textureScale.x = ClampFinite(options.leaves.textureScale.x, 0.0001f, 1024.0f, 1.0f);
    options.leaves.textureScale.y = ClampFinite(options.leaves.textureScale.y, 0.0001f, 1024.0f, 1.0f);
    options.leaves.textureOffset.x = ClampFinite(options.leaves.textureOffset.x, -1024.0f, 1024.0f, 0.0f);
    options.leaves.textureOffset.y = ClampFinite(options.leaves.textureOffset.y, -1024.0f, 1024.0f, 0.0f);

    options.trellis.position.x = FiniteOr(options.trellis.position.x, 0.0f);
    options.trellis.position.y = FiniteOr(options.trellis.position.y, 0.0f);
    options.trellis.position.z = FiniteOr(options.trellis.position.z, -2.0f);
    options.trellis.width = ClampFinite(options.trellis.width, 0.001f, 256.0f, 10.0f);
    options.trellis.height = ClampFinite(options.trellis.height, 0.001f, 256.0f, 20.0f);
    options.trellis.spacing = ClampFinite(options.trellis.spacing, 0.05f, 256.0f, 2.0f);
    options.trellis.forceStrength = ClampFinite(options.trellis.forceStrength, 0.0f, 16.0f, 0.02f);
    options.trellis.forceMaxDistance = ClampFinite(options.trellis.forceMaxDistance, 0.001f, 256.0f, 3.0f);
    options.trellis.forceFalloff = ClampFinite(options.trellis.forceFalloff, 0.001f, 16.0f, 1.0f);
    options.trellis.cylinderRadius = ClampFinite(options.trellis.cylinderRadius, 0.001f, 16.0f, 0.05f);

    options.wind.strength.x = ClampFinite(options.wind.strength.x, -100.0f, 100.0f, 0.5f);
    options.wind.strength.y = ClampFinite(options.wind.strength.y, -100.0f, 100.0f, 0.0f);
    options.wind.strength.z = ClampFinite(options.wind.strength.z, -100.0f, 100.0f, 0.5f);
    options.wind.frequency = ClampFinite(options.wind.frequency, 0.0f, 1000.0f, 0.5f);
    options.wind.scale = ClampFinite(options.wind.scale, 0.001f, 10000.0f, 70.0f);

    options.branchOverrideCount = std::min<uint32>(options.branchOverrideCount, static_cast<uint32>(options.branchOverrides.size()));
    for (uint32 i = 0; i < options.branchOverrideCount; ++i)
    {
        BranchOverride& branchOverride = options.branchOverrides[i];
        branchOverride.level = std::min(branchOverride.level, kMaxLevels);
        branchOverride.lengthScale = ClampFinite(branchOverride.lengthScale, 0.0f, 16.0f, 1.0f);
        branchOverride.radiusScale = ClampFinite(branchOverride.radiusScale, 0.0f, 16.0f, 1.0f);
        branchOverride.angleOffsetDegrees = ClampFinite(branchOverride.angleOffsetDegrees, -360.0f, 360.0f, 0.0f);
        branchOverride.twistOffset = ClampFinite(branchOverride.twistOffset, -6.2831855f, 6.2831855f, 0.0f);
    }

    return options;
}

uint64 HashOptions(const TreeOptions& options)
{
    const TreeOptions sanitized = SanitizeOptions(options);
    uint64 h = 1469598103934665603ull;
    HashBytes(h, sanitized.seed);
    HashBytes(h, sanitized.type);

    HashBytes(h, sanitized.bark.type);
    HashBytes(h, sanitized.bark.tint);
    HashBytes(h, sanitized.bark.flatShading);
    HashBytes(h, sanitized.bark.textured);
    HashVec2(h, sanitized.bark.textureScale);

    HashBytes(h, sanitized.branch.levels);
    HashArray(h, sanitized.branch.angle);
    HashArray(h, sanitized.branch.children);
    HashVec3(h, sanitized.branch.forceDirection);
    HashBytes(h, sanitized.branch.forceStrength);
    HashArray(h, sanitized.branch.gnarliness);
    HashArray(h, sanitized.branch.length);
    HashArray(h, sanitized.branch.radius);
    HashArray(h, sanitized.branch.sections);
    HashArray(h, sanitized.branch.segments);
    HashArray(h, sanitized.branch.start);
    HashArray(h, sanitized.branch.taper);
    HashArray(h, sanitized.branch.twist);

    HashBytes(h, sanitized.leaves.type);
    HashBytes(h, sanitized.leaves.billboard);
    HashBytes(h, sanitized.leaves.angle);
    HashBytes(h, sanitized.leaves.count);
    HashBytes(h, sanitized.leaves.start);
    HashBytes(h, sanitized.leaves.size);
    HashBytes(h, sanitized.leaves.sizeVariance);
    HashBytes(h, sanitized.leaves.tint);
    HashBytes(h, sanitized.leaves.alphaTest);
    HashBytes(h, sanitized.leaves.textureColumns);
    HashBytes(h, sanitized.leaves.textureRows);
    HashBytes(h, sanitized.leaves.textureTile);
    HashVec2(h, sanitized.leaves.textureScale);
    HashVec2(h, sanitized.leaves.textureOffset);
    HashBytes(h, sanitized.leaves.randomTextureTile);
    HashBytes(h, sanitized.leaves.roundedNormals);

    HashBytes(h, sanitized.trellis.enabled);
    HashVec3(h, sanitized.trellis.position);
    HashBytes(h, sanitized.trellis.width);
    HashBytes(h, sanitized.trellis.height);
    HashBytes(h, sanitized.trellis.spacing);
    HashBytes(h, sanitized.trellis.forceStrength);
    HashBytes(h, sanitized.trellis.forceMaxDistance);
    HashBytes(h, sanitized.trellis.forceFalloff);
    HashBytes(h, sanitized.trellis.cylinderRadius);
    HashBytes(h, sanitized.trellis.visible);
    HashBytes(h, sanitized.trellis.color);

    HashBytes(h, sanitized.wind.enabled);
    HashVec3(h, sanitized.wind.strength);
    HashBytes(h, sanitized.wind.frequency);
    HashBytes(h, sanitized.wind.scale);

    HashBytes(h, sanitized.branchOverrideCount);
    for (uint32 i = 0; i < sanitized.branchOverrideCount; ++i)
        HashBranchOverride(h, sanitized.branchOverrides[i]);
    return h;
}

} // namespace GameEngine::EZTree

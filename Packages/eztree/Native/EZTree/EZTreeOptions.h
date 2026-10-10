#pragma once

// Native C++ port of @dgreenheck/ez-tree option data.
// Portions adapted from EZ-Tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.

#include "AssetCore/GUID.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::EZTree
{

inline constexpr std::string_view kPluginId = "ezTree";
// Asset-source alias of the eztree package mount (sanitized package.json name).
// Editor-side tooling resolves the package's default materials/textures
// through this alias; the GUIDs themselves stay the pre-extraction originals
// via the package's stored-identity .assetmanifest.
inline constexpr std::string_view kPackageAlias = "eztree";
// Upstream EZ-Tree pin — must match cmake/ports/ez-tree-upstream (the vcpkg
// port that supplies the preset JSONs staged next to consumers).
inline constexpr std::string_view kUpstreamVersion = "1.1.0";
inline constexpr std::string_view kUpstreamCommit = "28c16503da2a8a6f2ccb6c070f47ff8ca13ad4f6";

enum class TreeType : uint8
{
    Deciduous = 0,
    Evergreen = 1,
};

enum class BarkType : uint8
{
    Oak = 0,
    Birch = 1,
    Pine = 2,
    Willow = 3,
};

enum class LeafType : uint8
{
    Oak = 0,
    Ash = 1,
    Aspen = 2,
    Pine = 3,
};

enum class BillboardMode : uint8
{
    Single = 0,
    Double = 1,
};

struct Vec2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct ColorRgb
{
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

struct BarkOptions
{
    BarkType type = BarkType::Oak;
    uint32 tint = 0xFFFFFFu;
    bool flatShading = false;
    bool textured = true;
    Vec2 textureScale{1.0f, 1.0f};
};

struct BranchOptions
{
    uint32 levels = 3;
    std::array<float, 4> angle{0.0f, 70.0f, 60.0f, 60.0f};
    std::array<uint32, 4> children{7u, 7u, 5u, 0u};
    Mathematics::Vector3 forceDirection{0.0f, 1.0f, 0.0f};
    float forceStrength = 0.01f;
    std::array<float, 4> gnarliness{0.15f, 0.2f, 0.3f, 0.02f};
    std::array<float, 4> length{20.0f, 20.0f, 10.0f, 1.0f};
    std::array<float, 4> radius{1.5f, 0.7f, 0.7f, 0.7f};
    std::array<uint32, 4> sections{12u, 10u, 8u, 6u};
    std::array<uint32, 4> segments{8u, 6u, 4u, 3u};
    std::array<float, 4> start{0.0f, 0.4f, 0.3f, 0.3f};
    std::array<float, 4> taper{0.7f, 0.7f, 0.7f, 0.7f};
    std::array<float, 4> twist{0.0f, 0.0f, 0.0f, 0.0f};
};

struct LeafOptions
{
    LeafType type = LeafType::Oak;
    BillboardMode billboard = BillboardMode::Double;
    float angle = 10.0f;
    uint32 count = 1;
    float start = 0.0f;
    float size = 2.5f;
    float sizeVariance = 0.7f;
    uint32 tint = 0xFFFFFFu;
    float alphaTest = 0.5f;
    uint32 textureColumns = 1u;
    uint32 textureRows = 1u;
    uint32 textureTile = 0u;
    Vec2 textureScale{1.0f, 1.0f};
    Vec2 textureOffset{0.0f, 0.0f};
    bool randomTextureTile = false;
    bool roundedNormals = true;
};

struct TrellisOptions
{
    bool enabled = false;
    Mathematics::Vector3 position{0.0f, 0.0f, -2.0f};
    float width = 10.0f;
    float height = 20.0f;
    float spacing = 2.0f;
    float forceStrength = 0.02f;
    float forceMaxDistance = 3.0f;
    float forceFalloff = 1.0f;
    float cylinderRadius = 0.05f;
    bool visible = true;
    uint32 color = 0x8B4513u;
};

struct WindOptions
{
    bool enabled = true;
    Mathematics::Vector3 strength{0.5f, 0.0f, 0.5f};
    float frequency = 0.5f;
    float scale = 70.0f;
};

struct BranchOverride
{
    bool enabled = false;
    uint32 branchId = 0;
    uint32 level = 0;
    float lengthScale = 1.0f;
    float radiusScale = 1.0f;
    float angleOffsetDegrees = 0.0f;
    float twistOffset = 0.0f;
};

struct TreeOptions
{
    uint32 seed = 0;
    TreeType type = TreeType::Deciduous;
    BarkOptions bark{};
    BranchOptions branch{};
    LeafOptions leaves{};
    TrellisOptions trellis{};
    WindOptions wind{};
    std::array<BranchOverride, 16> branchOverrides{};
    uint32 branchOverrideCount = 0;
    std::array<uint8, 16> barkMaterialGuid{};
    std::array<uint8, 16> leafMaterialGuid{};
    std::array<uint8, 16> trellisMaterialGuid{};
    std::array<uint8, 16> barkColorTextureGuid{};
    std::array<uint8, 16> barkNormalTextureGuid{};
    std::array<uint8, 16> barkRoughnessTextureGuid{};
    std::array<uint8, 16> barkAoTextureGuid{};
    std::array<uint8, 16> leafColorTextureGuid{};
    std::array<uint8, 16> trellisTextureGuid{};
};

struct PresetInfo
{
    std::string name;
    std::string displayName;
};

TreeOptions MakeDefaultOptions();
std::vector<PresetInfo> ListPresets();
bool LoadPreset(std::string_view name, TreeOptions& outOptions, std::string* outError = nullptr);
std::string ToString(TreeType v);
std::string ToString(BarkType v);
std::string ToString(LeafType v);
std::string ToString(BillboardMode v);
TreeType TreeTypeFromString(std::string_view v, TreeType fallback = TreeType::Deciduous);
BarkType BarkTypeFromString(std::string_view v, BarkType fallback = BarkType::Oak);
LeafType LeafTypeFromString(std::string_view v, LeafType fallback = LeafType::Oak);
BillboardMode BillboardModeFromString(std::string_view v, BillboardMode fallback = BillboardMode::Double);
TreeOptions SanitizeOptions(TreeOptions options);
uint64 HashOptions(const TreeOptions& options);

} // namespace GameEngine::EZTree

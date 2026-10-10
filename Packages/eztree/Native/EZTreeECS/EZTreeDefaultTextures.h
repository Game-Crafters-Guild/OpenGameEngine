#pragma once

// Package-relative paths for the Tree Generator's default bark and leaf maps.
// Extraction and the editor plugin must agree on these names: GUID resolve
// is path-based, and leaf albedo stays PNG so cutout alpha survives. The
// extraction loads them by path for any texture slot the component leaves empty
// (a scene written by hand, a bark or leaf type changed at runtime). No
// dependency edge names those loads, so package.json lists the folder under
// "runtimeAssets" for the EZTree component.

#include "EZTree/EZTreeOptions.h"

#include <cstring>
#include <filesystem>
#include <string>

namespace GameEngine::EZTreeECS
{

inline const char* BarkTypeAssetName(EZTree::BarkType type)
{
    switch (type)
    {
    case EZTree::BarkType::Birch:  return "birch";
    case EZTree::BarkType::Pine:   return "pine";
    case EZTree::BarkType::Willow: return "willow";
    case EZTree::BarkType::Oak:
    default:                       return "oak";
    }
}

inline const char* LeafTypeAssetName(EZTree::LeafType type)
{
    switch (type)
    {
    case EZTree::LeafType::Ash:   return "ash";
    case EZTree::LeafType::Aspen: return "aspen";
    case EZTree::LeafType::Pine:  return "pine";
    case EZTree::LeafType::Oak:
    default:                      return "oak";
    }
}

inline const char* LeafTextureExtension(const char* suffix)
{
    return std::strcmp(suffix, "color") == 0 ? ".png" : ".jpg";
}

inline std::filesystem::path DefaultBarkTextureRelativePath(EZTree::BarkType type, const char* suffix)
{
    return std::filesystem::path("Textures") / "EZTree" / "bark" /
        (std::string(BarkTypeAssetName(type)) + "_" + suffix + "_1k.jpg");
}

inline std::filesystem::path DefaultLeafTextureRelativePath(EZTree::LeafType type,
                                                            const char* suffix = "color")
{
    return std::filesystem::path("Textures") / "EZTree" / "leaves" /
        (std::string(LeafTypeAssetName(type)) + "_" + suffix + LeafTextureExtension(suffix));
}

// One map of a bark or leaf set: the file-name suffix the paths above take, and
// the material slot the extraction system binds that map to.
struct DefaultTextureSlot
{
    const char* Suffix;
    const char* MaterialSlot;
};

// The four maps every generated tree binds, bark and leaf alike. The slot
// decides each texture's cooked compression (TextureCookUsageForMaterialSlot)
// and its upload colour space (IsLinearTextureSlot), and the package's
// committed .assetmanifest carries both as authored rows — so the bindings and
// those rows must come from one table, or the package ships import settings
// that no longer match what binds the texture.
inline constexpr DefaultTextureSlot kDefaultTextureSlots[] = {
    {"color", "albedoMap"},
    {"normal", "normalMap"},
    {"roughness", "metallicRoughnessMap"},
    {"ao", "aoMap"},
};

} // namespace GameEngine::EZTreeECS

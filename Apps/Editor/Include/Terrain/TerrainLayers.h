#pragma once

#include "AssetCore/GUID.h"
#include "Components/Terrain/Terrain.h"
#include "Terrain/TerrainMaterialRecord.h"
#include "Terrain/TerrainTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace GameEngine::TerrainLayers
{

inline constexpr uint32_t kCount = Terrain::kMaxTerrainMaterialLayers;

/// Semantic names for the four channel roles, in the order the surface shades them
/// (0 grass, 1 rock, 2 dirt, 3 snow). A reference to the engine's array rather than a copy: the
/// same names label a channel with no material bound and name the entries of a library minted from
/// a terrain's per-layer fields, and two copies would be two answers. What a channel is called once
/// a material IS bound to it is a different question — see TerrainRoleMaterials::RoleLabel.
inline constexpr const auto& kNames = Terrain::kTerrainLayerRoleNames;

/// The colour the surface shades a layer with while its albedo slot is empty, sRGB-encoded as
/// 0xFFRRGGBB. Reads the engine's terrain material defaults directly, so there is no editor-side
/// copy of the palette to drift; binding a texture neutralises the tint to white, so this colour is
/// what the user sees exactly while the slot is empty. UI background colours are sRGB-encoded
/// bytes, so the linear engine colour has to be encoded before it reads as the same colour.
inline uint32_t FallbackTintSwatchArgb(uint32_t layer)
{
    if (layer >= kCount)
        return 0xFF000000u;
    const auto encode = [](float linear) -> uint32_t {
        const float c = std::clamp(linear, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<uint32_t>(s * 255.0f + 0.5f);
    };
    const Terrain::TerrainMaterialRecord& material = Terrain::kDefaultTerrainMaterials[layer];
    return (0xFFu << 24) | (encode(material.AlbedoR) << 16) | (encode(material.AlbedoG) << 8) |
           encode(material.AlbedoB);
}

/// True when the layer shades from a bound albedo texture; false leaves it on the built-in
/// tint + procedural variation fallback.
inline bool IsTextured(const Components::Terrain& terrain, uint32_t layer)
{
    return layer < kCount && !terrain.LayerAlbedoTexture[layer].IsNull();
}

/// Bind the layer's albedo texture. A null GUID unbinds it — that is the empty-picker path, and it
/// drops the layer back to the tint fallback.
inline void SetAlbedo(Components::Terrain& terrain, uint32_t layer, const GUID& guid)
{
    if (layer >= kCount)
        return;
    terrain.LayerAlbedoTexture[layer].Set(guid);
}

/// Per-layer tiling multiplier. Clamped to >= 0 exactly as the scene schema clamps it on parse;
/// 0 means "no per-layer scaling", which the render feature resolves to the global tiling.
inline void SetTiling(Components::Terrain& terrain, uint32_t layer, float tiling)
{
    if (layer >= kCount)
        return;
    terrain.LayerTiling[layer] = std::max(tiling, 0.0f);
}

inline bool IsHexTiling(const Components::Terrain& terrain, uint32_t layer)
{
    return layer < kCount && ((terrain.LayerHexTiling >> layer) & 1u) != 0u;
}

inline void SetHexTiling(Components::Terrain& terrain, uint32_t layer, bool enabled)
{
    if (layer >= kCount)
        return;
    if (enabled)
        terrain.LayerHexTiling |= (1u << layer);
    else
        terrain.LayerHexTiling &= ~(1u << layer);
}

} // namespace GameEngine::TerrainLayers

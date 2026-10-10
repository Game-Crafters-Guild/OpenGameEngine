#pragma once

// Per-frame derived shadow geometry for the punctual (area/spot/point) light
// families: the light view/projection matrices, world position, and near/far
// planes resolved once per frame from the world light list. Built by the free
// helpers Build{Area,Spot,Point}ShadowFrameInfo (declared in the RenderServices
// implementation-private RenderServicesDetail.h) and carried as snapshots on
// FeatureDeclareContext — which is why these types live in a public header the
// interface can include, while the builders stay private to Engine/Source.

#include "Mathematics/Matrix4x4.h"
#include "Mathematics/Vector3.h"

#include <array>
#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

// Cube faces for an omnidirectional point-light shadow.
inline constexpr uint32_t kPointShadowFaceCount = 6u;

// All six faces visible — the default when no camera-cull inputs are supplied
// (debug/thumbnail paths) and the pre-culling behaviour.
inline constexpr uint8_t kPointShadowAllFaces = 0x3Fu;

// Per-light punctual shadow resolution tiers, mirroring Components::LightShadowTier
// as a plain uint (this public header must not depend on the Components layer).
// Tier 0 (Inherit) resolves to the pipeline node's punctual resolution so
// pre-existing content is byte-identical; the fixed tiers cap VRAM for opt-in
// lights. Kept in sync with the enum by the extraction static_assert.
inline constexpr uint32_t kPunctualShadowTierInherit = 0u;

inline uint32_t ResolvePunctualShadowResolution(uint32_t tier, uint32_t inheritResolution)
{
    switch (tier)
    {
    case 1:  return 256u;  // Low
    case 2:  return 512u;  // Medium
    case 3:  return 1024u; // High
    default: return inheritResolution;
    }
}

struct AreaShadowFrameInfo
{
    bool valid = false;
    uint32_t packedLightIndex = 0;
    Mathematics::Matrix4x4 lightView{};
    Mathematics::Matrix4x4 lightProj{};
    Mathematics::Matrix4x4 lightVP{};
    Mathematics::Vector3 position{};
    Mathematics::Vector3 direction{};
    float lightSize = 0.0f;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
};

struct SpotShadowFrameInfo
{
    bool valid = false;
    uint32_t packedLightIndex = 0;
    Mathematics::Matrix4x4 lightView{};
    Mathematics::Matrix4x4 lightProj{};
    Mathematics::Matrix4x4 lightVP{};
    Mathematics::Vector3 position{};
    Mathematics::Vector3 direction{};
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
};

struct PointShadowFrameInfo
{
    bool valid = false;
    uint32_t packedLightIndex = 0;
    std::array<Mathematics::Matrix4x4, kPointShadowFaceCount> lightView{};
    Mathematics::Matrix4x4 lightProj{};
    std::array<Mathematics::Matrix4x4, kPointShadowFaceCount> lightVP{};
    Mathematics::Vector3 position{};
    std::array<Mathematics::Vector3, kPointShadowFaceCount> direction{};
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    // Opt-in per-light resolution tier (Components::LightShadowTier as uint);
    // 0 = inherit the node's punctual resolution.
    uint32_t resolutionTier = kPunctualShadowTierInherit;
    // 6-bit camera-visibility mask (bit f => render face f). Computed ONCE here
    // by PopulatePointShadowGeometry and shared by pass declaration + GPU cull
    // scheduling (design A10). All-visible when no camera-cull inputs are given.
    uint8_t faceMask = kPointShadowAllFaces;
    // M1 atlas assignment (filled by the atlas planner, not the geometry builder):
    // the atlas slot this light occupies (-1 = unassigned), and the committed tile
    // resolution its faces render at (<= the atlas tile size). The slot's six cube
    // faces occupy array layers [shadowSlot*6, shadowSlot*6 + 6); a lower tier
    // renders into the tileResolution^2 sub-rect of each full-resolution layer.
    int32_t shadowSlot = -1;
    uint32_t tileResolution = 0;
    // L1a whole-light on-dirty caching: false when this slot's (view,slot) content
    // is unchanged since it was last rendered, so its face passes are NOT declared
    // and the persistent atlas layers are sampled as-is (ImportPersistentTexture
    // retains un-written layers). The atlas planner sets this; it is honoured
    // identically by pass declaration and GPU-cull scheduling (they read the one
    // idempotent assignment, so the skip cannot diverge and corrupt M2a survivor
    // keys). Defaults true so non-planner paths (first-wins debug, thumbnails)
    // always render.
    bool needsRender = true;
};

} // namespace Engine::Renderer
} // namespace GameEngine

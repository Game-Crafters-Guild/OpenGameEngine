#include "TerrainECS/TerrainGrassField.h"
#include "TerrainECS/TerrainModifierSystem.h"

#include <algorithm>
#include <cmath>

namespace GameEngine::TerrainECS
{
namespace
{
// Ease the volume's weight across the falloff before a grass target is blended with it.
//
// The shared weight ramps linearly from 0 at the outer edge to 1 at the inner boundary, which is
// right for a height offset: the ground it produces is that ramp. Grass is not that ramp. Measured
// on a 60 m region with a 25 m falloff, visible coverage is an S-curve under either blend, steepest
// in the middle of the band and far shallower at both boundaries than mid-band - the linear blend
// still arrives at the outer edge at roughly 40% of its peak slope, which is the corner this
// softens. That shape is consistent with visible coverage saturating in a dense field: it holds
// near full until height x density has fallen well below 1.
//
// What this changes is the CORNERS. Smoothstep has zero slope at both ends of the weight, so the
// slope jump where a region meets the untouched field is 1.5-2.3x smaller (0.0156 -> 0.0107 per
// metre at the outer edge, 0.0100 -> 0.0044 at the inner one, on a 50%/50% region), and the change
// it takes off the corners reappears mid-band, which is about 1.4x steeper. A corner is what reads
// as a seam; the two blends are otherwise hard to tell apart.
//
// Grass only: the shared ComputeWeight is what every other effect ramps with and is untouched, so
// no terrain without a grass region changes by a texel.
float32 GrassRegionEase(float32 weight)
{
    return weight * weight * (3.0f - 2.0f * weight);
}
}

void ComposeTerrainGrassField(TerrainGrassField& field, uint32 width, uint32 height,
                              float32 sizeX, float32 sizeZ, float32 originX, float32 originZ,
                              std::span<const ResolvedModifier> modifiers,
                              int32 minX, int32 minZ, int32 maxX, int32 maxZ)
{
    if (width < 2 || height < 2 || !std::isfinite(sizeX) || !std::isfinite(sizeZ)
        || !std::isfinite(originX) || !std::isfinite(originZ)
        || sizeX <= 0 || sizeZ <= 0) return;
    const bool firstComposition = !field.Initialized;
    const bool resized = field.Width != width || field.Height != height;
    bool changed = resized && field.IsActive();
    if (resized)
    {
        field.Pixels.clear();
        field.NonNeutralTexels = 0;
        field.Width = width; field.Height = height;
        minX = minZ = 0; maxX = maxZ = -1;
    }
    minX = std::clamp(minX, 0, static_cast<int32>(width - 1));
    minZ = std::clamp(minZ, 0, static_cast<int32>(height - 1));
    maxX = maxX < 0 ? width - 1 : std::min(maxX, static_cast<int32>(width - 1));
    maxZ = maxZ < 0 ? height - 1 : std::min(maxZ, static_cast<int32>(height - 1));
    // A fresh field is implicit white. Only targets below one can make it
    // nonneutral, so avoid scanning an entire large terrain for a small region.
    // Neutral restorers are still evaluated inside this union in stack order.
    if (field.Pixels.empty() && !modifiers.empty())
    {
        int32 loX = width, loZ = height, hiX = -1, hiZ = -1;
        for (const auto& mod : modifiers)
        {
            if (!mod.Enabled || mod.Weight <= 0
                || std::isnan(mod.BoundsMinX) || std::isnan(mod.BoundsMaxX)
                || std::isnan(mod.BoundsMinZ) || std::isnan(mod.BoundsMaxZ)
                || mod.BoundsMaxX < originX || mod.BoundsMinX > originX + sizeX
                || mod.BoundsMaxZ < originZ || mod.BoundsMinZ > originZ + sizeZ) continue;
            const bool lowers = std::any_of(mod.Effects.begin(), mod.Effects.end(), [](const auto& fx) {
                return fx.EffectKind == ResolvedEffect::Kind::Grass
                    && (Components::ClampTerrainGrassEffectScale(fx.Grass.HeightScale) < 1
                        || Components::ClampTerrainGrassEffectScale(fx.Grass.DensityScale) < 1);
            });
            if (!lowers) continue;
            auto index = [](float32 value, float32 origin, float32 size, uint32 count, bool upper) {
                const float32 sample = std::clamp((value-origin)/size, 0.0f, 1.0f) * (count-1);
                return static_cast<int32>(upper ? std::ceil(sample) : std::floor(sample));
            };
            loX = std::min(loX, index(mod.BoundsMinX, originX, sizeX, width, false));
            loZ = std::min(loZ, index(mod.BoundsMinZ, originZ, sizeZ, height, false));
            hiX = std::max(hiX, index(mod.BoundsMaxX, originX, sizeX, width, true));
            hiZ = std::max(hiZ, index(mod.BoundsMaxZ, originZ, sizeZ, height, true));
        }
        minX = std::max(minX, loX); minZ = std::max(minZ, loZ);
        maxX = std::min(maxX, hiX); maxZ = std::min(maxZ, hiZ);
        if (minX > maxX || minZ > maxZ)
        {
            std::vector<uint8>{}.swap(field.Pixels);
            field.Initialized = true;
            // A resized former active field still has to retire its old image.
            if (changed) { ++field.Version; field.Dirty = true; }
            return;
        }
    }
    if (modifiers.empty())
    {
        changed |= field.IsActive();
        std::vector<uint8>{}.swap(field.Pixels);
        field.NonNeutralTexels = 0;
        minX = minZ = 0; maxX = width - 1; maxZ = height - 1;
    }
    else for (int32 z = minZ; z <= maxZ; ++z)
    {
        const float32 worldZ = originZ + sizeZ * (static_cast<float32>(z) / (height - 1));
        for (int32 x = minX; x <= maxX; ++x)
        {
            const float32 worldX = originX + sizeX * (static_cast<float32>(x) / (width - 1));
            float32 heightScale = 1.0f, densityScale = 1.0f;
            for (const auto& mod : modifiers)
            {
                if (!mod.Enabled || worldX < mod.BoundsMinX || worldX > mod.BoundsMaxX
                    || worldZ < mod.BoundsMinZ || worldZ > mod.BoundsMaxZ) continue;
                const float32 rawWeight = ComputeWeight(mod, worldX, worldZ);
                const float32 linearWeight = std::isfinite(rawWeight) ? std::clamp(rawWeight, 0.0f, 1.0f) : 0.0f;
                if (linearWeight <= 0) continue;
                const float32 weight = GrassRegionEase(linearWeight);
                for (const auto& fx : mod.Effects)
                {
                    if (fx.EffectKind != ResolvedEffect::Kind::Grass) continue;
                    heightScale += (Components::ClampTerrainGrassEffectScale(fx.Grass.HeightScale) - heightScale) * weight;
                    densityScale += (Components::ClampTerrainGrassEffectScale(fx.Grass.DensityScale) - densityScale) * weight;
                }
            }
            const uint8 h = static_cast<uint8>(std::lround(std::clamp(heightScale, 0.0f, 1.0f) * 255.0f));
            const uint8 d = static_cast<uint8>(std::lround(std::clamp(densityScale, 0.0f, 1.0f) * 255.0f));
            if (field.Pixels.empty() && h == 255 && d == 255) continue;
            if (field.Pixels.empty())
                field.Pixels.assign(static_cast<size_t>(width) * height * kTerrainGrassFieldChannels, 255);
            const size_t offset = (static_cast<size_t>(z) * width + x) * kTerrainGrassFieldChannels;
            if (field.Pixels[offset] == h && field.Pixels[offset + 1] == d) continue;
            if (field.Pixels[offset] != 255 || field.Pixels[offset + 1] != 255) --field.NonNeutralTexels;
            if (h != 255 || d != 255) ++field.NonNeutralTexels;
            field.Pixels[offset] = h; field.Pixels[offset + 1] = d;
            changed = true;
        }
    }
    if (!field.NonNeutralTexels) std::vector<uint8>{}.swap(field.Pixels);
    field.Initialized = true;
    if (changed)
    {
        ++field.Version;
        if (!field.Dirty || resized)
        {
            // A new/replaced CPU field must replace the whole destination,
            // even if its new local Version coincides with an old tile's one.
            field.DirtyMinX = (resized || firstComposition) ? 0 : minX;
            field.DirtyMinZ = (resized || firstComposition) ? 0 : minZ;
            field.DirtyMaxX = (resized || firstComposition) ? width : maxX + 1;
            field.DirtyMaxZ = (resized || firstComposition) ? height : maxZ + 1;
        }
        else
        {
            field.DirtyMinX = std::min(field.DirtyMinX, static_cast<uint32>(minX));
            field.DirtyMinZ = std::min(field.DirtyMinZ, static_cast<uint32>(minZ));
            field.DirtyMaxX = std::max(field.DirtyMaxX, static_cast<uint32>(maxX + 1));
            field.DirtyMaxZ = std::max(field.DirtyMaxZ, static_cast<uint32>(maxZ + 1));
        }
        field.Dirty = true;
    }
}

} // namespace GameEngine::TerrainECS

#include "TerrainECS/TerrainHeightPageDriver.h"

#include "TerrainECS/HeightPageStoreLoader.h"
#include "TerrainECS/TerrainHeightPageFeature.h"
#include "TerrainECS/TerrainHeightPages.h"
#include "TerrainECS/TerrainHeightSource.h"
#include "TerrainECS/TerrainRenderFeature.h"
#include "TerrainECS/TerrainService.h"
#include "CBTTerrain/CBTLayout.h"
#include "Components/Terrain/Terrain.h"
#include "PageStreaming/GeneratedHeightPageProvider.h"
#include "PageStreaming/PageStoreReader.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/HashUtils.h"

#include <bit>
#include <cmath>

namespace GameEngine::TerrainECS
{

namespace
{

using Rendering::HashUtils::HashCombine;

// The generated lattice of a noise or flat tiled terrain: its tile grid and the tile fill's noise.
PageStreaming::GeneratedHeightLattice MakeGeneratedLattice(const TiledTerrainData& tiled)
{
    PageStreaming::GeneratedHeightLattice lattice;
    lattice.Source = tiled.Config.Base.Source == Components::TerrainBaseSource::ProceduralNoise
                         ? PageStreaming::GeneratedHeightSource::Noise
                         : PageStreaming::GeneratedHeightSource::Flat;
    lattice.TerrainOriginX = tiled.WorldOriginX;
    lattice.TerrainOriginZ = tiled.WorldOriginZ;
    lattice.TileWorldSize = tiled.Config.TileWorldSize;
    lattice.TileSamples = tiled.Config.TileConfig.HeightmapWidth;
    lattice.TilesX = tiled.Config.TilesPerAxisX;
    lattice.TilesZ = tiled.Config.TilesPerAxisZ;
    lattice.Noise = {kTileNoiseFrequency, kTileNoiseAmplitude, kTileNoiseOctaves, kTileNoiseSeed};
    return lattice;
}

uint64 GeneratedIdentity(const PageStreaming::GeneratedHeightLattice& lattice)
{
    uint64 hash = HashCombine(0x9E3779B97F4A7C15ull, static_cast<uint64>(lattice.Source));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.TerrainOriginX));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.TerrainOriginZ));
    hash = HashCombine(hash, std::bit_cast<uint32>(lattice.TileWorldSize));
    hash = HashCombine(hash, lattice.TileSamples);
    hash = HashCombine(hash, lattice.TilesX);
    return HashCombine(hash, lattice.TilesZ);
}

} // namespace

void RequestTiledTerrainHeightPages(TerrainService& service, const Rendering::IDevice& device,
                                    TiledTerrainData& tiled, const TiledRenderExtent& extent,
                                    const TerrainHandle& renderTerrain, std::string_view name, float32 originY,
                                    float32 heightScale,
                                    float32 targetPixelError, std::span<const Mathematics::Vector3> cameras,
                                    float32 focalScale)
{
    if (focalScale <= 0.0f)
        return;
    const bool gpuReadsPages = !CBTTerrain::CBTDeviceRunsNarrowArm(
        device.PreferredShaderSource() == Rendering::ShaderSourceKind::Wgsl,
        device.GetCapabilities().supportsShaderInt64);
    TerrainHeightPages& pages = service.GetHeightPages();
    const TiledTerrainBase& base = tiled.Config.Base;
    const bool generated = base.Source != Components::TerrainBaseSource::HeightmapAsset;

    PagedTerrainRequest request;
    request.Terrain = renderTerrain.Index;
    request.TerrainGeneration = renderTerrain.Generation;
    request.Name = name;
    uint32 samplesX = 0;
    uint32 samplesZ = 0;
    std::shared_ptr<const PageStreaming::PageStoreReader> store;
    PageStreaming::GeneratedHeightLattice lattice;
    if (generated)
    {
        lattice = MakeGeneratedLattice(tiled);
        request.SourceIdentity = GeneratedIdentity(lattice);
        samplesX = lattice.TilesX * (lattice.TileSamples - 1u) + 1u;
        samplesZ = lattice.TilesZ * (lattice.TileSamples - 1u) + 1u;
    }
    else
    {
        store = service.GetHeightPageStores().Store(base.HeightmapGuid, base.HeightmapContentVersion, name);
        if (store)
        {
            request.SourceIdentity = HashCombine(reinterpret_cast<uintptr_t>(store.get()), base.HeightmapContentVersion);
            samplesX = store->Layout().Header.SamplesX;
            samplesZ = store->Layout().Header.SamplesZ;
        }
    }

    TerrainHeightSourceInputs inputs;
    inputs.ModifiersPending = service.HeightModifiersMayApply() && !tiled.PageOverlayCurrent;
    inputs.GeneratedBase = generated;
    inputs.CookedStoreOpen = store != nullptr;
    inputs.GpuReadsPages = gpuReadsPages;
    // The terrain UV the CBT samples spans the tile grid's extent; a generated pyramid that does
    // not cover exactly that lattice would place every page wrong, so it pages only on the lattice.
    // A cooked store on its own grid pages too, but takes no modifier overlay (PageOverlayAllowance).
    const bool onLattice = samplesX == extent.UnifiedWidth && samplesZ == extent.UnifiedHeight;
    const std::string_view label = name.empty() ? std::string_view("A terrain") : name;
    const bool fitsLattice = samplesX > 1u && samplesZ > 1u && (!generated || onLattice);
    std::string refusal = fitsLattice ? PageTableRefusal(label, samplesX, samplesZ, pages.Profile()) : std::string();
    inputs.PageTableFits = fitsLattice && refusal.empty();
    // The modifier system builds the overlay only for a terrain that would page but for its modifiers.
    TerrainHeightSourceInputs unmodified = inputs;
    unmodified.ModifiersPending = false;
    const HeightPageOverlayAllowance allowance = PageOverlayAllowance(
        TerrainHeightIsPaged(unmodified), samplesX, samplesZ, extent.UnifiedWidth, extent.UnifiedHeight, pages.Profile());
    tiled.PageOverlayByteCap = allowance.ByteCap;
    tiled.PageOverlayOffGrid = allowance.OffGrid;
    if (!allowance.OffGrid)
        tiled.PageOverlayRefusedOffGrid = false; // its store is on the lattice now: the overlay is baked again
    if (allowance.ByteCap != 0 && tiled.PageOverlayRefusedBytes != 0)
        refusal = OverlayRefusal(label, tiled.PageOverlayRefusedBytes, pages.Profile());
    else if (allowance.ByteCap != 0 && tiled.PageOverlayRefusedOffGrid)
        refusal = OverlayGridRefusal(label, samplesX, samplesZ, extent.UnifiedWidth, extent.UnifiedHeight);
    if (!TerrainHeightIsPaged(inputs))
    {
        if (!refusal.empty() && gpuReadsPages)
            pages.ReportRefusal(request.Terrain, refusal);
        return; // not requested this frame: its pages are forgotten at the next update
    }

    if (pages.NeedsSource(request.Terrain, request.SourceIdentity))
    {
        if (generated)
            request.Source.Generated = std::make_shared<const PageStreaming::GeneratedHeightPageProvider>(lattice);
        else
            request.Source.Store = store;
    }
    request.Overlay = tiled.PageOverlay;
    request.OverlayVersion = tiled.PageOverlayVersion;
    request.OverlayChanged = tiled.PageOverlayChanged;
    request.OverlayChangedSince = tiled.PageOverlayChangedSince;
    request.Shape.OriginX = tiled.WorldOriginX;
    request.Shape.OriginZ = tiled.WorldOriginZ;
    request.Shape.Level0TexelX = extent.WorldSizeX / static_cast<float32>(samplesX - 1u);
    request.Shape.Level0TexelZ = extent.WorldSizeZ / static_cast<float32>(samplesZ - 1u);
    request.Shape.SamplesX = samplesX;
    request.Shape.SamplesZ = samplesZ;
    const float32 low = tiled.CachedGlobalMinH * heightScale + originY;
    const float32 high = tiled.CachedGlobalMaxH * heightScale + originY;
    request.Shape.MinHeight = std::min(low, high);
    request.Shape.MaxHeight = std::max(low, high);
    const PageLevelView view =
        MakePageLevelView(kPageLevelReferenceRows, 2.0f * std::atan(1.0f / focalScale), targetPixelError);
    pages.Request(request, cameras, view);
}

void UpdateHeightPages(TerrainService& service, TerrainRenderFeature& terrainFeature, TerrainHeightPageFeature& pages,
                       uint64 frameIndex, float32 deltaSeconds, uint32 framesInFlight)
{
    TerrainHeightPages& heightPages = service.GetHeightPages();
    heightPages.Update(frameIndex, deltaSeconds, false, framesInFlight);
    heightPages.RestartOnDeviceEpoch(pages.DeviceEpoch());
    pages.Publish(heightPages);
    for (const HeightPageDirtyRect& rect : heightPages.DirtyRects())
        terrainFeature.AccumulateAtlasHeightDirtyUV(TerrainHandle{rect.Terrain, rect.TerrainGeneration}, rect.MinU,
                                                    rect.MinV, rect.MaxU, rect.MaxV);
    heightPages.DirtyRects().clear();
}

} // namespace GameEngine::TerrainECS

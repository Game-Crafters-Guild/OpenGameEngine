#pragma once

#include "Ocean/OceanSeabedDepth.h"
#include "Ocean/OceanTypes.h"

#include <vector>

namespace GameEngine::Ocean
{

// Per-frame summary of authored ocean inputs gathered by OceanExtractionSystem.
// Kept POD so debug/editor tooling can copy it without owning the source arrays.
struct OceanInputFrameStats
{
    uint32 SavedDepthCaches = 0;
    uint32 SavedDepthCacheCandidates = 0;
    uint32 SavedDepthCacheBudget = 0;
    uint32 Seabeds = 0;
    uint32 DepthContributors = 0;
    uint32 FlowSources = 0;
    uint32 FlowPolygons = 0;
    uint32 FlowMapSources = 0;
    uint32 WaveMaskSources = 0;
    uint32 WaveMaskPolygons = 0;
    uint32 WaveMaskTextureSources = 0;
    uint32 ClipSources = 0;
    uint32 ClipPolygons = 0;
    uint32 AlbedoSources = 0;
    uint32 UnderwaterVolumes = 0;
    uint32 UnderwaterVolumePolygons = 0;
    uint32 UnderwaterExclusionVolumes = 0;
    uint32 DynamicWaveImpulses = 0;
    uint32 WaterBodyBoxes = 0;
    uint32 WaterBodyPolygons = 0;
    uint32 WaterBodyStamps = 0;
    uint32 SplineInputs = 0;
    uint32 RibbonTriangles = 0;
    uint32 TypedInputs = 0;
    uint32 FoamInputs = 0;

    uint32 DroppedSeabeds = 0;
    uint32 DroppedSavedDepthCaches = 0;
    uint32 DroppedDepthContributors = 0;
    uint32 DroppedFlowSources = 0;
    uint32 DroppedFlowPolygons = 0;
    uint32 DroppedFlowMapSources = 0;
    uint32 DroppedWaveMaskSources = 0;
    uint32 DroppedWaveMaskPolygons = 0;
    uint32 DroppedWaveMaskTextureSources = 0;
    uint32 DroppedClipSources = 0;
    uint32 DroppedClipPolygons = 0;
    uint32 DroppedAlbedoSources = 0;
    uint32 DroppedDynamicWaveImpulses = 0;
    uint32 DroppedFoamInputs = 0;

    uint32 MaxSeabeds = kMaxOceanSeabeds;
    uint32 MaxDepthContributors = kMaxOceanDepthContributors;
    uint32 MaxFlowSources = kMaxOceanFlowSources;
    uint32 MaxFlowPolygons = kMaxOceanFlowPolygons;
    uint32 MaxFlowMapSources = kMaxOceanFlowMapSources;
    uint32 MaxWaveMaskSources = kMaxOceanWaveMaskSources;
    uint32 MaxWaveMaskPolygons = kMaxOceanWaveMaskPolygons;
    uint32 MaxWaveMaskTextureSources = kMaxOceanWaveMaskTextureSources;
    uint32 MaxClipSources = kMaxOceanClipSources;
    uint32 MaxClipPolygons = kMaxOceanClipPolygons;
    uint32 MaxAlbedoSources = kMaxOceanAlbedoSources;
    uint32 MaxDynamicWaveImpulses = kMaxOceanWaveImpulses;
    uint32 MaxFoamInputs = kMaxOceanFoamInputs;

    uint32 TotalDroppedInputs() const
    {
        return DroppedSeabeds + DroppedSavedDepthCaches + DroppedDepthContributors +
               DroppedFlowSources + DroppedFlowPolygons + DroppedFlowMapSources +
               DroppedWaveMaskSources + DroppedWaveMaskPolygons +
               DroppedWaveMaskTextureSources + DroppedClipSources + DroppedClipPolygons +
               DroppedAlbedoSources + DroppedDynamicWaveImpulses + DroppedFoamInputs;
    }

    bool HasDroppedInputs() const { return TotalDroppedInputs() != 0u; }
};

// Shared per-frame input container for ocean extraction. This is intentionally a
// lightweight registry, not an ECS owner: component systems still author their
// existing source types, while extraction routes every family through one cap /
// dropped-count path before handing arrays to the simulations.
struct OceanInputRegistryFrame
{
    OceanInputFrameStats Stats;

    std::vector<OceanSavedDepthCacheSource> SavedDepthCaches;
    std::vector<OceanSeabedGPU> Seabeds;
    std::vector<OceanDepthContributorGPU> DepthContributors;
    std::vector<OceanFlowSourceGPU> FlowSources;
    std::vector<OceanFlowPolygonGPU> FlowPolygons;
    std::vector<OceanFlowMapSourceGPU> FlowMapSources;
    std::vector<OceanWaveMaskSourceGPU> WaveMaskSources;
    std::vector<OceanWaveMaskPolygonGPU> WaveMaskPolygons;
    std::vector<OceanWaveMaskTextureSourceGPU> WaveMaskTextureSources;
    std::vector<OceanClipSourceGPU> ClipSources;
    std::vector<OceanClipPolygonGPU> ClipPolygons;
    std::vector<OceanAlbedoSourceGPU> AlbedoSources;
    std::vector<OceanWaveImpulseGPU> DynamicWaveImpulses;
    std::vector<OceanFoamInputGPU> FoamInputs;

    bool AddSavedDepthCache(const OceanSavedDepthCacheSource& source)
    {
        SavedDepthCaches.push_back(source);
        return true;
    }

    bool AddSeabed(const OceanSeabedGPU& source)
    {
        if (Seabeds.size() >= static_cast<size_t>(kMaxOceanSeabeds))
        {
            ++Stats.DroppedSeabeds;
            return false;
        }
        Seabeds.push_back(source);
        return true;
    }

    bool AddDepthContributor(const OceanDepthContributorGPU& source)
    {
        if (DepthContributors.size() >= static_cast<size_t>(kMaxOceanDepthContributors))
        {
            ++Stats.DroppedDepthContributors;
            return false;
        }
        DepthContributors.push_back(source);
        return true;
    }

    bool AddFlowSource(const OceanFlowSourceGPU& source)
    {
        if (FlowSources.size() >= static_cast<size_t>(kMaxOceanFlowSources))
        {
            ++Stats.DroppedFlowSources;
            return false;
        }
        FlowSources.push_back(source);
        return true;
    }

    bool AddFlowPolygon(const OceanFlowPolygonGPU& source)
    {
        if (FlowPolygons.size() >= static_cast<size_t>(kMaxOceanFlowPolygons))
        {
            ++Stats.DroppedFlowPolygons;
            return false;
        }
        FlowPolygons.push_back(source);
        return true;
    }

    bool AddFlowMapSource(const OceanFlowMapSourceGPU& source)
    {
        if (FlowMapSources.size() >= static_cast<size_t>(kMaxOceanFlowMapSources))
        {
            ++Stats.DroppedFlowMapSources;
            return false;
        }
        FlowMapSources.push_back(source);
        return true;
    }

    bool AddWaveMaskSource(const OceanWaveMaskSourceGPU& source)
    {
        if (WaveMaskSources.size() >= static_cast<size_t>(kMaxOceanWaveMaskSources))
        {
            ++Stats.DroppedWaveMaskSources;
            return false;
        }
        WaveMaskSources.push_back(source);
        return true;
    }

    bool AddWaveMaskPolygon(const OceanWaveMaskPolygonGPU& source)
    {
        if (WaveMaskPolygons.size() >= static_cast<size_t>(kMaxOceanWaveMaskPolygons))
        {
            ++Stats.DroppedWaveMaskPolygons;
            return false;
        }
        WaveMaskPolygons.push_back(source);
        return true;
    }

    bool AddWaveMaskTextureSource(const OceanWaveMaskTextureSourceGPU& source)
    {
        if (WaveMaskTextureSources.size() >=
            static_cast<size_t>(kMaxOceanWaveMaskTextureSources))
        {
            ++Stats.DroppedWaveMaskTextureSources;
            return false;
        }
        WaveMaskTextureSources.push_back(source);
        return true;
    }

    bool AddClipSource(const OceanClipSourceGPU& source)
    {
        if (ClipSources.size() >= static_cast<size_t>(kMaxOceanClipSources))
        {
            ++Stats.DroppedClipSources;
            return false;
        }
        ClipSources.push_back(source);
        return true;
    }

    bool AddClipPolygon(const OceanClipPolygonGPU& source)
    {
        if (ClipPolygons.size() >= static_cast<size_t>(kMaxOceanClipPolygons))
        {
            ++Stats.DroppedClipPolygons;
            return false;
        }
        ClipPolygons.push_back(source);
        return true;
    }

    bool AddAlbedoSource(const OceanAlbedoSourceGPU& source)
    {
        if (AlbedoSources.size() >= static_cast<size_t>(kMaxOceanAlbedoSources))
        {
            ++Stats.DroppedAlbedoSources;
            return false;
        }
        AlbedoSources.push_back(source);
        return true;
    }

    bool AddDynamicWaveImpulse(const OceanWaveImpulseGPU& source)
    {
        if (DynamicWaveImpulses.size() >= static_cast<size_t>(kMaxOceanWaveImpulses))
        {
            ++Stats.DroppedDynamicWaveImpulses;
            return false;
        }
        DynamicWaveImpulses.push_back(source);
        return true;
    }

    bool AddFoamInput(const OceanFoamInputGPU& source)
    {
        if (FoamInputs.size() >= static_cast<size_t>(kMaxOceanFoamInputs))
        {
            ++Stats.DroppedFoamInputs;
            return false;
        }
        FoamInputs.push_back(source);
        return true;
    }

    void FinalizeCounts()
    {
        Stats.SavedDepthCaches = static_cast<uint32>(SavedDepthCaches.size());
        Stats.Seabeds = static_cast<uint32>(Seabeds.size());
        Stats.DepthContributors = static_cast<uint32>(DepthContributors.size());
        Stats.FlowSources = static_cast<uint32>(FlowSources.size());
        Stats.FlowPolygons = static_cast<uint32>(FlowPolygons.size());
        Stats.FlowMapSources = static_cast<uint32>(FlowMapSources.size());
        Stats.WaveMaskSources = static_cast<uint32>(WaveMaskSources.size());
        Stats.WaveMaskPolygons = static_cast<uint32>(WaveMaskPolygons.size());
        Stats.WaveMaskTextureSources = static_cast<uint32>(WaveMaskTextureSources.size());
        Stats.ClipSources = static_cast<uint32>(ClipSources.size());
        Stats.ClipPolygons = static_cast<uint32>(ClipPolygons.size());
        Stats.AlbedoSources = static_cast<uint32>(AlbedoSources.size());
        Stats.DynamicWaveImpulses = static_cast<uint32>(DynamicWaveImpulses.size());
        Stats.FoamInputs = static_cast<uint32>(FoamInputs.size());
    }
};

} // namespace GameEngine::Ocean

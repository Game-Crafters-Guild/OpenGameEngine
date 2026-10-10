#pragma once

#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Handle.h"
#include "Rendering/Core/RenderGraph/RGFrame.h" // RGTexture

#include <array>
#include <cstdint>

namespace GameEngine::Ocean
{

// The shared wave-shape inputs the fullscreen ocean passes (underwater overlay,
// reflected caustics) sample: the global FFT displacement, the per-stream local
// displacements, and the local-FFT mask pages. Each dynamic texture carries BOTH
// the raw handle (bound descriptor-direct at record) and its render-graph import
// (declared read — the layout transition + producer edge; the bind alone forms
// no edge). Built once per underwater-node declare by OceanRenderNode from the
// feature's ready state; an unready slot leaves both handles invalid and the
// consuming pass falls back to its dummy texture.
struct OceanShapeSampleInputs
{
    ::GameEngine::Rendering::TextureHandle Displacement{};
    ::GameEngine::Rendering::SamplerHandle DisplacementSampler{};
    ::GameEngine::Rendering::RenderGraph::RGTexture DisplacementRG{};

    std::array<::GameEngine::Rendering::TextureHandle, kMaxOceanLocalFFTStreams>
        LocalDisplacements{};
    std::array<::GameEngine::Rendering::SamplerHandle, kMaxOceanLocalFFTStreams>
        LocalDisplacementSamplers{};
    std::array<::GameEngine::Rendering::RenderGraph::RGTexture, kMaxOceanLocalFFTStreams>
        LocalDisplacementsRG{};
    uint32 LocalFFTReadyMask = 0;

    std::array<::GameEngine::Rendering::TextureHandle, kOceanLocalFFTMaskPages> LocalFFTMasks{};
    std::array<::GameEngine::Rendering::SamplerHandle, kOceanLocalFFTMaskPages>
        LocalFFTMaskSamplers{};
    std::array<::GameEngine::Rendering::RenderGraph::RGTexture, kOceanLocalFFTMaskPages>
        LocalFFTMasksRG{};
    const OceanCascadeLayoutGPU* LocalFFTMaskLayout = nullptr;
};

} // namespace GameEngine::Ocean

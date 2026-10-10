#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/Device.h"

#include <vector>

namespace GameEngine { namespace Engine { namespace Renderer {

class GPUAnimationDataStore;

// Render graph compute pass that dispatches the GPU animation skinning shader.
// One dispatch per frame for all accumulated animation instances.
// Writes skin matrices directly into the SkinPaletteAtlas SSBO.
class AnimationComputePass final {
public:
    void SetComputeShader(const std::vector<uint8_t>& spirv) { m_ComputeShader = spirv; }
    void SetDataStore(GPUAnimationDataStore* store) { m_DataStore = store; }

    // Set the atlas buffer handle for the current frame (called before compile).
    void SetAtlasBuffer(GameEngine::Rendering::BufferHandle atlas) { m_AtlasBuffer = atlas; }

    // Phase 6-iii: runtime-visibility gate config. When enabled and Declare()
    // receives a valid runtimeVisible buffer, the skinning compute shader
    // early-outs for runtimes whose entities are all off-screen.
    // `runtimeVisibleCap` is the buffer capacity in element units (uint32s),
    // bounds-checked inside the shader against AnimInstance.runtimeId.
    void SetRuntimeVisibility(bool gateEnabled, uint32_t runtimeVisibleCap)
    {
        m_GateEnabled       = gateEnabled;
        m_RuntimeVisibleCap = runtimeVisibleCap;
    }

    // The declared Read(runtimeVisible) — recorded BEFORE the aggregate's Write —
    // is the WAR edge that pins skinning-before-aggregate. Invalid runtimeVisible ⇒
    // ungated dispatch.
    void Declare(GameEngine::Rendering::RenderGraph::RGFrame& frame,
                 GameEngine::Rendering::RenderGraph::RGBuffer atlas,
                 GameEngine::Rendering::RenderGraph::RGBuffer runtimeVisible);

private:
    // Dispatch recording: binds the data store + atlas + optional
    // runtime-visibility gate and dispatches one group per instance.
    void RecordDispatch(GameEngine::Rendering::IDevice* dev,
                        GameEngine::Rendering::CommandList* cl,
                        GameEngine::Rendering::BufferHandle atlasBuf,
                        GameEngine::Rendering::BufferHandle runtimeVisBuf);
    std::vector<uint8_t> m_ComputeShader;
    GPUAnimationDataStore* m_DataStore = nullptr;
    GameEngine::Rendering::BufferHandle m_AtlasBuffer;

    // Phase 6-iii: visibility-gate state.
    bool     m_GateEnabled       = false;
    uint32_t m_RuntimeVisibleCap = 0;

    GameEngine::Rendering::ComputePipelineId m_PipelineId{};
};

}}} // namespace GameEngine::Engine::Renderer

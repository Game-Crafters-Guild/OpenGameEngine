#pragma once

#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Core/Device.h"

#include <vector>

namespace GameEngine { namespace Engine { namespace Renderer {

class RetargetGPUDataStore;

// Single fused compute pass that runs the entire humanoid retarget pipeline
// per character per frame: clip sample → src FK walk → tgt FK walk →
// translation → skin-matrix build → SkinPaletteAtlas write.
//
// One workgroup per character (64 threads). All intermediate state lives in
// LDS — no global SSBO roundtrip between phases. See retarget_full.comp for
// the math; this class wires the data store's three SSBOs (clip, rig, chars)
// + the atlas to the shader and dispatches with characterCount as a push
// constant.
//
// Scheduled in the early-setup render-graph phase so it completes before any
// graphics pass that reads the atlas.
class RetargetFullPass final {
public:
    void SetComputeShader(const std::vector<uint8_t>& spirv) { m_ComputeShader = spirv; }
    void SetDataStore(RetargetGPUDataStore* store) { m_DataStore = store; }

    // Declared on the SAME atlas RGBuffer value the skinning pass writes —
    // handle-dedup makes them one resource, so RenderGraph orders the two writers.
    void Declare(GameEngine::Rendering::RenderGraph::RGFrame& frame,
                 GameEngine::Rendering::RenderGraph::RGBuffer atlas);

private:
    void RecordDispatch(GameEngine::Rendering::IDevice* dev,
                        GameEngine::Rendering::CommandList* cl,
                        GameEngine::Rendering::BufferHandle atlasBuf);
    std::vector<uint8_t> m_ComputeShader;
    RetargetGPUDataStore* m_DataStore = nullptr;

    GameEngine::Rendering::ComputePipelineId m_PipelineId{};
};

}}} // namespace GameEngine::Engine::Renderer

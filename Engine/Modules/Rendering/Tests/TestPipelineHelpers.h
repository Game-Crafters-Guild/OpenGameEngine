#pragma once

#include "Rendering/Core/Device.h"
#include "TestUtils.h"

namespace GameEngine {
namespace Rendering {

// Minimal helper to create a dynamic-rendering compatible triangle pipeline for tests
inline PipelineHandle CreateTestTrianglePipeline(IDevice* device, uint32_t colorFormat,
                                                 const char* debugName = "TestTrianglePipeline") {
    if (!device) return INVALID_PIPELINE_HANDLE;

    Tests::InstallTestShaderResolver();

    PipelineDesc pso{};
    pso.debugName = debugName;
    pso.type = PipelineType::Graphics;

    pso.vertexShader = Tests::ReadSpirvBytes("fullscreen_noinput.vert.spv");
    pso.pixelShader  = Tests::ReadSpirvBytes("solidcolor.frag.spv");

    // Dynamic rendering: provide the target formats explicitly
    pso.colorAttachmentFormats.push_back(colorFormat);
    // viewport/scissor made dynamic by backend automatically; also request here for completeness
    pso.AddDynamicState(DynamicState::Viewport);
    pso.AddDynamicState(DynamicState::Scissor);

    // Keep defaults for rasterization/depth states
    return device->CreatePipeline(pso);
}

} // namespace Rendering
} // namespace GameEngine


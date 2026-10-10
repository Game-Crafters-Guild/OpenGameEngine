#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Common/Utils.h"

namespace GameEngine {
namespace Rendering {

inline PipelineHandle CreateTestTrianglePipeline(IDevice* device, uint32_t colorFormat,
                                                 const char* debugName = "TestTrianglePipeline") {
    if (!device) return INVALID_PIPELINE_HANDLE;

    PipelineDesc pso{};
    pso.debugName = debugName;
    pso.type = PipelineType::Graphics;

    // Load repo-provided precompiled SPIR-V shaders
    pso.vertexShader = Utils::LoadShaderFile("triangle.vert.spv");
    pso.pixelShader  = Utils::LoadShaderFile("triangle.frag.spv");

    // Dynamic rendering formats
    pso.colorAttachmentFormats.push_back(colorFormat);
    // Reverse-Z requires float depth. D32_SFLOAT_S8_UINT keeps stencil available for tests.
    pso.depthAttachmentFormat = (uint32_t)TextureFormat::D32_SFLOAT_S8_UINT;
    pso.stencilAttachmentFormat = (uint32_t)TextureFormat::D32_SFLOAT_S8_UINT;
    // Keep depth test/writes disabled for headless tests unless explicitly requested

    pso.AddDynamicState(DynamicState::Viewport);
    pso.AddDynamicState(DynamicState::Scissor);

    // Disable culling to avoid driver-dependent winding with negative viewport height
    pso.rasterizationState.cullMode = CullModeFlagBits::None;

    return device->CreatePipeline(pso);
}

} // namespace Rendering
} // namespace GameEngine


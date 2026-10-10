#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{

class SpecializationConstants;

// SPIR-V -> MSL translation via SPIRV-Cross. Descriptor sets become Metal 3
// argument buffers at [[buffer(set)]]; per-binding [[id(n)]] assignment is
// forced to match ComputeMetalArgumentBufferLayout (see
// MetalArgumentBufferLayout.h). Push constants land at
// [[buffer(kMetalPushConstantBufferIndex)]].
struct MetalShaderTranslation
{
    bool Success = false;
    std::string Msl;
    std::string EntryPoint; // SPIRV-Cross renames the entry to "main0"
    std::string Error;
    // Compute and Mesh stages: SPIR-V workgroup size, needed at dispatch /
    // drawMeshThreadgroups time.
    uint32_t LocalSizeX = 0;
    uint32_t LocalSizeY = 0;
    uint32_t LocalSizeZ = 0;
};

enum class MetalShaderStage
{
    Vertex,
    Fragment,
    Compute,
    // GL_EXT_mesh_shader stages. Object is the optional amplification/task
    // stage that dispatches mesh threadgroups.
    Mesh,
    Object
};

MetalShaderTranslation TranslateSpirvToMsl(const std::vector<uint8_t>& spirv,
                                           MetalShaderStage stage,
                                           const std::vector<const DescriptorSetLayoutDesc*>& setLayouts,
                                           const SpecializationConstants* specialization);

} // namespace GameEngine::Rendering

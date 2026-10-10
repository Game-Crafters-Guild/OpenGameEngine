#pragma once

#include <cstdint>

namespace GameEngine::Rendering {

// Engine-level pipeline stage and access bitmasks for RenderGraph barrier reasoning.
// These are backend-agnostic; backends translate them to native flags.
enum class PipelineStageMask : uint64_t {
    None              = 0,
    GraphicsColor     = 1ull << 0,
    GraphicsDepth     = 1ull << 1,
    GraphicsVertex    = 1ull << 2,
    GraphicsFragment  = 1ull << 3,
    ComputeShader     = 1ull << 4,
    Transfer          = 1ull << 5,
    DrawIndirect      = 1ull << 6,
};

inline PipelineStageMask operator|(PipelineStageMask a, PipelineStageMask b) {
    return static_cast<PipelineStageMask>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline PipelineStageMask& operator|=(PipelineStageMask& a, PipelineStageMask b) {
    a = a | b;
    return a;
}

enum class ResourceAccessMask : uint64_t {
    None                  = 0,
    ColorAttachmentRead   = 1ull << 0, // reserved for future use
    ColorAttachmentWrite  = 1ull << 1,
    DepthStencilRead      = 1ull << 2,
    DepthStencilWrite     = 1ull << 3,
    ShaderRead            = 1ull << 4,
    ShaderWrite           = 1ull << 5,
    VertexAttributeRead   = 1ull << 6,
    TransferRead          = 1ull << 7,
    TransferWrite         = 1ull << 8,
    IndirectCommandRead   = 1ull << 9,
};

inline ResourceAccessMask operator|(ResourceAccessMask a, ResourceAccessMask b) {
    return static_cast<ResourceAccessMask>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

inline ResourceAccessMask& operator|=(ResourceAccessMask& a, ResourceAccessMask b) {
    a = a | b;
    return a;
}

struct StageAccess {
    PipelineStageMask stage;
    ResourceAccessMask access;
};

} // namespace GameEngine::Rendering


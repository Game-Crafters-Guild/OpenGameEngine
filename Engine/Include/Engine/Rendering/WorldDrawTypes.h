#pragma once

// World draw submission record shared between ECS extraction, the world
// draw builder, and the draw batch compactor.
//
// The compacted output type is `DrawCommand` (see DrawCommand.h), which is
// the unified per-draw record consumed by the world pass execute lambda
// regardless of whether the draw originated from ECS or a contributor.

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/CameraTypes.h"
#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Types/Types.h"

#include <cstdint>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::MeshGPUHandle;
using ::GameEngine::Rendering::ViewId;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace Engine::Renderer
{
class Material;

using GameEngine::MaterialAlphaMode;

// WorldSubmissionRecord::flags bits, mirroring GPUInstance::flags.
inline constexpr uint32 kSubmissionFlagCastShadows = 1u << 0;

// High-level per-entity submission record produced by extraction systems.
// Lightweight and job-friendly. Mesh and material are resolved before
// submission so the compactor can batch without additional lookups.
struct WorldSubmissionRecord
{
    Rendering::ViewId viewId{0};             // Logical view this submission targets
    Rendering::MeshGPUHandle meshHandle{};   // MeshGPURegistry handle
    const Material* material = nullptr;      // Runtime material (non-owning)
    uint32 instanceIndex{0};                 // Index into GPUScene instance buffer
    uint32 renderLayerMask{0};
    uint32 flags{0};                         // e.g., opaque/transparent, castsShadows
};

} // namespace Engine::Renderer
} // namespace GameEngine

// Per-draw resource bindings consumed by MaterialBinder. The engine maps
// these onto whichever descriptor set(s) the material's shaders ask for via
// SPIR-V reflection — callers refer to bindings by name, not by set/binding
// indices.
//
// SoA bag with spans rather than owned vectors: typical contributors fill
// fewer than 8 of each entry from stack arrays, so heap allocation is rare.

#pragma once

#include "Rendering/Core/Handle.h"
#include "Types/StringId.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace GameEngine::Engine::Renderer
{

struct DrawBindings
{
    struct BufferEntry
    {
        StringId Name{};
        ::GameEngine::Rendering::BufferHandle Buffer{};
        uint64_t Offset = 0;
        // 0 == whole buffer.
        uint64_t Range  = 0;
    };

    struct TextureEntry
    {
        StringId Name{};
        ::GameEngine::Rendering::TextureHandle  Texture{};
        // Optional; engine substitutes the material's sampler preset when invalid.
        ::GameEngine::Rendering::SamplerHandle  Sampler{};
        uint32_t ArrayIndex = 0;
    };

    std::span<const BufferEntry>  Buffers;
    std::span<const TextureEntry> Textures;
    // Small typed POD reinterpreted as bytes. Layout must match the shader.
    std::span<const std::byte>    PushConstants;
};

} // namespace GameEngine::Engine::Renderer

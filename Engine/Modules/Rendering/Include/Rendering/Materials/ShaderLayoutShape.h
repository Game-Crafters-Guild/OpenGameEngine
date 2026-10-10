#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <cstdint>

namespace GameEngine::Rendering
{
    /**
     * @brief Stamps a hand-built descriptor layout with the image shape its shader declares.
     *
     * A layout written by hand carries binding index, type, count and stage —
     * everything Vulkan, Metal and D3D12 read. WebGPU validates four more
     * things against the shader module and rejects a layout that is silent
     * about them: the storage texel format, the view dimension, the storage
     * access (a `readonly` image is read in WGSL and a write-only entry is
     * refused), and whether a compute binding is sampled through a filtering
     * op. Reflection knows all four, so they are copied from the program's own
     * meta rather than restated at the call site where they can drift from the
     * shader.
     *
     * Bindings the meta does not describe are left alone, so a superset layout
     * shared by several programs keeps its other entries.
     */
    void ApplyMetaImageShapeToLayout(const ShaderMeta& meta,
                                     DescriptorSetLayoutDesc& layout,
                                     uint32_t set = 0);
}

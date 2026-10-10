#pragma once

#include "Engine/Rendering/Material.h"
#include "Rendering/Materials/MaterialParamsLayout.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{

// Byte layout of one MaterialParams SSBO row (set 0, binding 13) — the GLSL
// `MaterialData` element the adapters index per instance. Param block (the
// material_params.glsl X-macro, sizeof(MaterialGpuParams)) + texture indices
// (8 x uint32) + texture UV transform rows (8 x 2 vec4) + one packed uint of
// per-slot sampler indices. The param-block size derives from the shared field
// list: hardcoding it desyncs TextureIndices/TextureST/stride from the GLSL the
// moment a field is appended, silently misaligning every texture index.
inline constexpr uint32_t kMaterialParamBytes = GameEngine::kMaterialParamBlockBytes;
inline constexpr uint32_t kTextureIndexBytes = kTextureSlotArraySize * sizeof(uint32_t);
inline constexpr uint32_t kTextureSTBytes = kTextureSlotArraySize * 8 * sizeof(float); // two affine rows per slot
inline constexpr uint32_t kSamplerIndicesOffset = kMaterialParamBytes + kTextureIndexBytes + kTextureSTBytes;
// std430 rounds the MaterialData array stride up to vec4 alignment, so the packed
// sampler-index uint owns a full 16-byte tail block (4 used + 12 pad).
inline constexpr uint32_t kMaterialEntryStride = kSamplerIndicesOffset + 16;
static_assert(kMaterialEntryStride % 16 == 0, "MaterialData std430 array stride must be 16-byte aligned");

} // namespace GameEngine::Engine::Renderer

// Material texture access, set 1 — single declaration site for all material,
// terrain, and shadow shaders. Guarded so multiple includes in one translation
// unit declare exactly once; whichever shader includes this first wins, and
// they all get identical bindings/types.
//
// Full profile — unbounded bindless arrays indexed per-instance:
//   binding 0: a SAMPLED_IMAGE texture array (textures only)
//   binding 1: a small SAMPLER array, one entry per Rendering::SamplerPreset
// A texture's sampler is chosen per-draw shader-side via an index into
// ge_BindlessSamplers (see GE_BTEX / adapter_forward's GE_SLOT_SAMPLER).
//
// Compat profile — one fixed binding per material texture slot, filled from a
// per-material bind group (MaterialBindingCache). No descriptor indexing and
// no unbounded arrays exist on the WebGPU-class target, so the slot ordinal
// resolves in a switch instead. Samplers sit at binding+64, mirroring the
// shader cook's combined-image-sampler split rule, and are pre-selected
// CPU-side per slot — the packed SamplerIndices lane is unused here.
#ifndef GE_TEXTURE_ARRAYS_DECLARED
#define GE_TEXTURE_ARRAYS_DECLARED

#if defined(GE_COMPAT_PROFILE)

// Must match Engine::Renderer::kTextureSlotArraySize and the layout
// MaterialBindingCache builds.
#define GE_COMPAT_MATERIAL_SLOT_COUNT 8

layout(set = 1, binding = 0) uniform texture2D ge_MaterialTextures0;
layout(set = 1, binding = 1) uniform texture2D ge_MaterialTextures1;
layout(set = 1, binding = 2) uniform texture2D ge_MaterialTextures2;
layout(set = 1, binding = 3) uniform texture2D ge_MaterialTextures3;
layout(set = 1, binding = 4) uniform texture2D ge_MaterialTextures4;
layout(set = 1, binding = 5) uniform texture2D ge_MaterialTextures5;
layout(set = 1, binding = 6) uniform texture2D ge_MaterialTextures6;
layout(set = 1, binding = 7) uniform texture2D ge_MaterialTextures7;

layout(set = 1, binding = 64) uniform sampler ge_MaterialSamplers0;
layout(set = 1, binding = 65) uniform sampler ge_MaterialSamplers1;
layout(set = 1, binding = 66) uniform sampler ge_MaterialSamplers2;
layout(set = 1, binding = 67) uniform sampler ge_MaterialSamplers3;
layout(set = 1, binding = 68) uniform sampler ge_MaterialSamplers4;
layout(set = 1, binding = 69) uniform sampler ge_MaterialSamplers5;
layout(set = 1, binding = 70) uniform sampler ge_MaterialSamplers6;
layout(set = 1, binding = 71) uniform sampler ge_MaterialSamplers7;

// GLSL cannot return an opaque sampler type from a function, so the slot walk
// has to complete the sample. Both entry points take the slot ordinal the
// full-profile path would have used as a bindless index.
// Implicit derivatives and a sampling bias are fragment-stage operations.
#ifndef GE_STAGE_VERTEX
vec4 ge_CompatSampleSlot(uint slot, vec2 uv, float bias)
{
    switch (slot)
    {
    case 0u: return texture(sampler2D(ge_MaterialTextures0, ge_MaterialSamplers0), uv, bias);
    case 1u: return texture(sampler2D(ge_MaterialTextures1, ge_MaterialSamplers1), uv, bias);
    case 2u: return texture(sampler2D(ge_MaterialTextures2, ge_MaterialSamplers2), uv, bias);
    case 3u: return texture(sampler2D(ge_MaterialTextures3, ge_MaterialSamplers3), uv, bias);
    case 4u: return texture(sampler2D(ge_MaterialTextures4, ge_MaterialSamplers4), uv, bias);
    case 5u: return texture(sampler2D(ge_MaterialTextures5, ge_MaterialSamplers5), uv, bias);
    case 6u: return texture(sampler2D(ge_MaterialTextures6, ge_MaterialSamplers6), uv, bias);
    default: return texture(sampler2D(ge_MaterialTextures7, ge_MaterialSamplers7), uv, bias);
    }
}

#endif

vec4 ge_CompatSampleSlotGrad(uint slot, vec2 uv, vec2 uvPerPixelX, vec2 uvPerPixelY)
{
    switch (slot)
    {
    case 0u: return textureGrad(sampler2D(ge_MaterialTextures0, ge_MaterialSamplers0), uv, uvPerPixelX, uvPerPixelY);
    case 1u: return textureGrad(sampler2D(ge_MaterialTextures1, ge_MaterialSamplers1), uv, uvPerPixelX, uvPerPixelY);
    case 2u: return textureGrad(sampler2D(ge_MaterialTextures2, ge_MaterialSamplers2), uv, uvPerPixelX, uvPerPixelY);
    case 3u: return textureGrad(sampler2D(ge_MaterialTextures3, ge_MaterialSamplers3), uv, uvPerPixelX, uvPerPixelY);
    case 4u: return textureGrad(sampler2D(ge_MaterialTextures4, ge_MaterialSamplers4), uv, uvPerPixelX, uvPerPixelY);
    case 5u: return textureGrad(sampler2D(ge_MaterialTextures5, ge_MaterialSamplers5), uv, uvPerPixelX, uvPerPixelY);
    case 6u: return textureGrad(sampler2D(ge_MaterialTextures6, ge_MaterialSamplers6), uv, uvPerPixelX, uvPerPixelY);
    default: return textureGrad(sampler2D(ge_MaterialTextures7, ge_MaterialSamplers7), uv, uvPerPixelX, uvPerPixelY);
    }
}

ivec2 ge_CompatSlotSize(uint slot, int lod)
{
    switch (slot)
    {
    case 0u: return textureSize(sampler2D(ge_MaterialTextures0, ge_MaterialSamplers0), lod);
    case 1u: return textureSize(sampler2D(ge_MaterialTextures1, ge_MaterialSamplers1), lod);
    case 2u: return textureSize(sampler2D(ge_MaterialTextures2, ge_MaterialSamplers2), lod);
    case 3u: return textureSize(sampler2D(ge_MaterialTextures3, ge_MaterialSamplers3), lod);
    case 4u: return textureSize(sampler2D(ge_MaterialTextures4, ge_MaterialSamplers4), lod);
    case 5u: return textureSize(sampler2D(ge_MaterialTextures5, ge_MaterialSamplers5), lod);
    case 6u: return textureSize(sampler2D(ge_MaterialTextures6, ge_MaterialSamplers6), lod);
    default: return textureSize(sampler2D(ge_MaterialTextures7, ge_MaterialSamplers7), lod);
    }
}

vec4 ge_CompatSampleSlotLod(uint slot, vec2 uv, float lod)
{
    switch (slot)
    {
    case 0u: return textureLod(sampler2D(ge_MaterialTextures0, ge_MaterialSamplers0), uv, lod);
    case 1u: return textureLod(sampler2D(ge_MaterialTextures1, ge_MaterialSamplers1), uv, lod);
    case 2u: return textureLod(sampler2D(ge_MaterialTextures2, ge_MaterialSamplers2), uv, lod);
    case 3u: return textureLod(sampler2D(ge_MaterialTextures3, ge_MaterialSamplers3), uv, lod);
    case 4u: return textureLod(sampler2D(ge_MaterialTextures4, ge_MaterialSamplers4), uv, lod);
    case 5u: return textureLod(sampler2D(ge_MaterialTextures5, ge_MaterialSamplers5), uv, lod);
    case 6u: return textureLod(sampler2D(ge_MaterialTextures6, ge_MaterialSamplers6), uv, lod);
    default: return textureLod(sampler2D(ge_MaterialTextures7, ge_MaterialSamplers7), uv, lod);
    }
}

#else // full profile

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 1, binding = 0) uniform texture2D ge_BindlessTextures[];
layout(set = 1, binding = 1) uniform sampler   ge_BindlessSamplers[];

// Sampler-preset indices into ge_BindlessSamplers. MUST match the
// Rendering::SamplerPreset enum order (Engine .../Core/Device.h).
#define GE_TS_REPEAT 0u // SamplerPreset::LinearRepeat
#define GE_TS_CLAMP  1u // SamplerPreset::LinearClamp
#define GE_TS_CLAMP_ANISO 5u // SamplerPreset::LinearClampAnisotropic

// Build a sampled sampler2D from a bindless texture index + a sampler-preset index.
// Use only as a direct texture()/textureLod() operand, never assign to a sampler2D local.
#define GE_BTEX(texIdx, samplerIdx) \
    sampler2D(ge_BindlessTextures[nonuniformEXT(texIdx)], ge_BindlessSamplers[nonuniformEXT(samplerIdx)])

#endif // GE_COMPAT_PROFILE

#endif // GE_TEXTURE_ARRAYS_DECLARED

#pragma once

#include "Rendering/Core/Device.h"

namespace GameEngine {
namespace Rendering {

// Small POD that groups pre-baked IBL resources (GPU handles).
struct IBLSet
{
    TextureHandle diffuseEnvTex = INVALID_HANDLE;    // Pre-baked diffuse irradiance cubemap
    TextureHandle specularEnvTex = INVALID_HANDLE;   // Pre-baked specular env cubemap

    TextureViewHandle diffuseEnvView = INVALID_TEXTURE_VIEW_HANDLE;
    TextureViewHandle specularEnvView = INVALID_TEXTURE_VIEW_HANDLE;

    // Reserved for future extensions (BRDF LUT, sheen, etc.).
    TextureHandle brdfLutTex = INVALID_HANDLE;
    TextureHandle sheenLutTex = INVALID_HANDLE;
    TextureViewHandle sheenLutView = INVALID_TEXTURE_VIEW_HANDLE;
};

inline void ResetIBLSet(IBLSet& set)
{
    set = IBLSet{};
}

} // namespace Rendering
} // namespace GameEngine


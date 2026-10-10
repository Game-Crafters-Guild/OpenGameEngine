#include "Rendering/Core/RendererProfile.h"
#include "Rendering/Core/Device.h"

#include "Logger/Logger.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace GameEngine
{
namespace Rendering
{

namespace
{
// Anisotropy the material sampler requests on every tier: 16x wherever the device
// allows it (FromCapabilities clamps it to maxSamplerAnisotropy), chosen for the
// detail eye-height views keep along the view direction. WebGPU clamps a request
// to the platform the same way, so the compatibility tier takes the same value.
constexpr float kMaterialSamplerAnisotropy = 16.0f;
} // namespace

RendererProfile RendererProfile::FromCapabilities(const RenderingDeviceCapabilities& caps)
{
    RendererProfile profile;
    if (!caps.supportsBindlessResources)
    {
        profile.ActiveTier = Tier::Compatibility;
        profile.UseBindlessMaterials = false;
        profile.UseGpuDrivenDraws = false;
        profile.UseGpuOcclusionCulling = false;
    }
    else
    {
        profile.UseGpuDrivenDraws = caps.supportsBufferDeviceAddress;
        profile.UseGpuOcclusionCulling = caps.supportsBufferDeviceAddress;
    }
    profile.UseMeshShaders = caps.supportsMeshShaders;
    profile.UseRayTracing = caps.supportsRayQuery;
    profile.UseInt64Compute = caps.supportsShaderInt64;
    profile.MaterialSamplerAnisotropy =
        std::clamp(caps.maxSamplerAnisotropy, 1.0f, kMaterialSamplerAnisotropy);
    profile.UseInterpolationFunctions = caps.supportsSampleRateShading;
    return profile;
}

void RendererProfile::LogResolved() const
{
    Logger::Log::Info("RendererProfile: {}", IsCompat() ? "COMPATIBILITY" : "Full");
    Logger::Log::Info("  bindless materials    : {}", UseBindlessMaterials ? "on" : "off");
    Logger::Log::Info("  GPU-driven draws      : {}", UseGpuDrivenDraws ? "on" : "off");
    Logger::Log::Info("  GPU occlusion culling : {}", UseGpuOcclusionCulling ? "on" : "off");
    Logger::Log::Info("  mesh shaders          : {}", UseMeshShaders ? "on" : "off");
    Logger::Log::Info("  ray tracing           : {}", UseRayTracing ? "on" : "off");
    Logger::Log::Info("  int64 compute         : {}", UseInt64Compute ? "on" : "off");
    Logger::Log::Info("  material anisotropy   : {}x", MaterialSamplerAnisotropy);
    Logger::Log::Info("  interpolation funcs   : {}", UseInterpolationFunctions ? "on" : "off");
}

bool ApplyForceCompatOverride(RenderingDeviceCapabilities& caps)
{
    const char* env = std::getenv("GE_FORCE_COMPAT");
    if (env == nullptr || std::strcmp(env, "1") != 0)
    {
        return false;
    }

    caps.supportsBindlessResources = false;
    caps.supportsDescriptorBuffer = false;
    caps.supportsBufferDeviceAddress = false;
    caps.supportsDrawIndirectCountNative = false;
    caps.supportsShaderInt64 = false;
    // WGSL has no interpolate-at-offset builtin.
    caps.supportsSampleRateShading = false;
    caps.supportsMeshShaders = false;
    caps.supportsRayTracing = false;
    caps.supportsRayQuery = false;
    caps.supportsWorkGraphs = false;
    caps.supportsAliasedStorageTextureBindings = false;
    caps.maxBindlessTextures = 0;
    caps.maxBindlessBuffers = 0;
    // The per-stage storage-buffer budget is deliberately left as the device
    // reports it: the browser's budget (10) is met by filtering per-kernel WGSL
    // layouts, which a SPIR-V blob cannot do, so clamping it here would only
    // turn the terrain off instead of exercising the narrow-heap kernels.
    // WebGPU's default maxStorageBufferBindingSize. Devices grant more only on
    // explicit request, so the compat profile budgets against the default.
    constexpr size_t kWebGpuDefaultMaxStorageBufferBindingSize = 128ull << 20;
    caps.maxStorageBufferBindingSize =
        caps.maxStorageBufferBindingSize == 0
            ? kWebGpuDefaultMaxStorageBufferBindingSize
            : std::min(caps.maxStorageBufferBindingSize,
                       kWebGpuDefaultMaxStorageBufferBindingSize);

    Logger::Log::Warning(
        "GE_FORCE_COMPAT=1: device capabilities clamped to the WebGPU-class "
        "compatibility subset (bindless/BDA/indirect-count/int64/mesh/RT off, "
        "storage-buffer binding size {} MB)",
        caps.maxStorageBufferBindingSize >> 20);
    return true;
}

} // namespace Rendering
} // namespace GameEngine

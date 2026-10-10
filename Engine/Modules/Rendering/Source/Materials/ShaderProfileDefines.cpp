#include "Rendering/Materials/ShaderProfileDefines.h"

#include "Rendering/Core/RendererProfile.h"

#include <atomic>

namespace GameEngine
{
namespace Rendering
{
namespace
{
std::atomic<bool> s_CompatShaderProfile{false};
std::atomic<bool> s_InterpolationFunctionsAvailable{false};
}

bool IsCompatShaderProfile()
{
    return s_CompatShaderProfile.load(std::memory_order_relaxed);
}

void SetCompatShaderProfile(bool compat)
{
    s_CompatShaderProfile.store(compat, std::memory_order_relaxed);
}

bool AreInterpolationFunctionsAvailable()
{
    return s_InterpolationFunctionsAvailable.load(std::memory_order_relaxed);
}

void SetInterpolationFunctionsAvailable(bool available)
{
    s_InterpolationFunctionsAvailable.store(available, std::memory_order_relaxed);
}

void ApplyShaderCompileProfile(const RendererProfile& profile)
{
    SetCompatShaderProfile(profile.IsCompat());
    SetInterpolationFunctionsAvailable(profile.UseInterpolationFunctions);
}

} // namespace Rendering
} // namespace GameEngine

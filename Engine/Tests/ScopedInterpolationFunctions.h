#pragma once

// Sets whether the interpolation functions are available (ShaderProfileDefines.h) for a scope and
// restores the previous value, as ScopedCompatShaderProfile does for the compat profile: a device
// test earlier in the process may have set it from a real device.

#include "Rendering/Materials/ShaderProfileDefines.h"

namespace GameEngine::TestSupport
{

struct ScopedInterpolationFunctions
{
    explicit ScopedInterpolationFunctions(bool available) : Prev(Rendering::AreInterpolationFunctionsAvailable())
    {
        Rendering::SetInterpolationFunctionsAvailable(available);
    }
    ~ScopedInterpolationFunctions() { Rendering::SetInterpolationFunctionsAvailable(Prev); }
    ScopedInterpolationFunctions(const ScopedInterpolationFunctions&) = delete;
    ScopedInterpolationFunctions& operator=(const ScopedInterpolationFunctions&) = delete;
    bool Prev;
};

} // namespace GameEngine::TestSupport

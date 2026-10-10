#pragma once

// Turns the compat shader profile (the WebGPU-class device fold, ShaderProfileDefines.h) on for a
// scope. It saves and restores rather than forcing the flag back off: another test in the process
// may legitimately have the profile on, and clobbering it would move the bug to that test.

#include "Rendering/Materials/ShaderProfileDefines.h"

namespace GameEngine::TestSupport
{

struct ScopedCompatShaderProfile
{
    ScopedCompatShaderProfile() : Prev(Rendering::IsCompatShaderProfile()) { Rendering::SetCompatShaderProfile(true); }
    ~ScopedCompatShaderProfile() { Rendering::SetCompatShaderProfile(Prev); }
    ScopedCompatShaderProfile(const ScopedCompatShaderProfile&) = delete;
    ScopedCompatShaderProfile& operator=(const ScopedCompatShaderProfile&) = delete;
    bool Prev;
};

} // namespace GameEngine::TestSupport

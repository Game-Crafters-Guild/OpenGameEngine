#pragma once

namespace GameEngine
{
namespace Rendering
{

struct RendererProfile;

// The compat shader profile is a property of the device the process booted on,
// not of any one material, so it lives beside the variant key rather than
// inside it: GenerateDefines emits GE_COMPAT_PROFILE from here, and
// ShaderVariantKey::Hash folds the same bit in so a compat run can never serve
// a desktop run's cached SPIR-V (or the reverse) out of the on-disk shader
// cache. Set once from the resolved RendererProfile at renderer init, before
// any variant is requested.
//
// The storage lives in the Rendering module, never inline in this header: an
// inline definition gives every binary that links the engine its own copy, and
// on Windows an executable's copy is not the engine DLL's. An offline tool
// setting the profile from its own main() would then set a flag that no shader
// compose ever reads.
bool IsCompatShaderProfile();
void SetCompatShaderProfile(bool compat);

// Whether the device enabled the interpolation functions (interpolateAtOffset and its kin: the
// SPIR-V InterpolationFunction capability, which Vulkan gates on sampleRateShading). Device-wide
// like the compat profile, and set at the same point from the resolved RendererProfile. Off until
// a device says otherwise, so a shader composed without one never declares the capability.
bool AreInterpolationFunctionsAvailable();
void SetInterpolationFunctionsAvailable(bool available);

// Sets every shader compile input a renderer profile decides (the compat profile, the interpolation
// functions) together. The runtime passes the profile its device resolved; an offline cook passes the
// profile of the device class it cooks for, so each cooked program carries the defines that class's
// runtime composes and the runtime finds it under the same cache key.
void ApplyShaderCompileProfile(const RendererProfile& profile);

} // namespace Rendering
} // namespace GameEngine

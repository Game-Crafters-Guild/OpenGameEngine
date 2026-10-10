#pragma once

#include <cstddef>
#include <cstdint>

// C++ mirror of the per-view ViewParams UBO (std140, 480 B, set 0 binding 7).
// The field list lives in Shaders/Includes/view_params_fields.glsl and is shared
// verbatim with the canonical GLSL block (view_params.glsl) and every truncated
// prefix the packaged Forward+/post-FX shaders declare, so the layout is defined
// exactly once. Both CPU producers — the per-view upload (ViewParamsUploadNode)
// and the RenderServices identity fallback — build this struct.

namespace GameEngine::Rendering
{

struct ViewParamsUBO
{
#define GE_VP_MAT4(name) float name[16];
#define GE_VP_VEC4(name) float name[4];
#include "../../../Shaders/Includes/view_params_fields.glsl"
#undef GE_VP_MAT4
#undef GE_VP_VEC4
};

// std140 layout lock. The field list is an append-only ABI shared verbatim with
// the GLSL block; pinning size + every interior offset turns an accidental
// reorder/insert — which would silently desync the shader's std140 reads — into
// a compile error here. The packaged shaders' prefixes are additionally checked
// against these offsets by ShaderReflectionDiffTests.
static_assert(sizeof(ViewParamsUBO) == 480, "ViewParamsUBO must be 480 bytes (std140)");
static_assert(offsetof(ViewParamsUBO, ge_invProj) == 0, "ge_invProj offset moved");
static_assert(offsetof(ViewParamsUBO, ge_view) == 64, "ge_view offset moved");
static_assert(offsetof(ViewParamsUBO, ge_nearFar) == 128, "ge_nearFar offset moved");
static_assert(offsetof(ViewParamsUBO, ge_cameraPosWS) == 144, "ge_cameraPosWS offset moved");
static_assert(offsetof(ViewParamsUBO, ge_screenSize) == 160, "ge_screenSize offset moved");
static_assert(offsetof(ViewParamsUBO, ge_viewProj) == 176, "ge_viewProj offset moved");
static_assert(offsetof(ViewParamsUBO, ge_proj) == 240, "ge_proj offset moved");
static_assert(offsetof(ViewParamsUBO, ge_mipBiasParams) == 304, "ge_mipBiasParams offset moved");
static_assert(offsetof(ViewParamsUBO, ge_prevViewProj) == 320, "ge_prevViewProj offset moved");
static_assert(offsetof(ViewParamsUBO, ge_invView) == 384, "ge_invView offset moved");
static_assert(offsetof(ViewParamsUBO, ge_taaJitter) == 448, "ge_taaJitter offset moved");
static_assert(offsetof(ViewParamsUBO, ge_exposureParams) == 464, "ge_exposureParams offset moved");

} // namespace GameEngine::Rendering

// Global per-view parameters UBO (set 0, binding 7) — single source of truth.
//
// Included by adapter_forward.glsl and clustered_lighting.glsl. The
// GE_VIEW_PARAMS_DECLARED guard makes a double-include a no-op (the adapter
// includes this directly, then again transitively through clustered_lighting)
// and lets surface shaders composed into the adapter reference ge_* without
// re-declaring the block.
//
// Fields come from the shared list (Includes/view_params_fields.glsl); this is
// the full 480 B block. ViewParamsUBO in Rendering/Core/ViewParamsLayout.h
// mirrors the same list on the C++ side.
#ifndef GE_VIEW_PARAMS_DECLARED
#define GE_VIEW_PARAMS_DECLARED 1

layout(set = 0, binding = 7, std140) uniform ViewParams
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#include "view_params_fields.glsl"
#undef GE_VP_MAT4
#undef GE_VP_VEC4
};

#endif // GE_VIEW_PARAMS_DECLARED

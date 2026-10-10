// Single source of truth for the MATERIAL PARAMETER BLOCK layout.
//
// Consumed by:
//   - GLSL: adapter_forward.glsl / adapter_vertex.glsl declare the shared
//     `MaterialData` SSBO row from this list.
//   - C++:  Rendering/Materials/MaterialParamsLayout.h builds `MaterialGpuParams`
//     from this list, and its sizeof drives the SSBO byte stride.
//
// Mechanism: each consumer #defines GE_FIELD(type, name) to emit a member, then
// #includes this file, then #undefs GE_FIELD. There is intentionally NO include
// guard — this is an X-macro field list, included multiple times by design.
//
// The block is GE_MATERIAL_PARAM_LANE_COUNT vec4 lanes with no fixed meaning.
// A program's `// @property` declarations are packed into them per program
// (ShaderPropertyTable) and the composer emits the GE_Props accessors that read
// them, so lane offsets and GLSL are generated together and no cross-surface
// offset agreement exists to break. Surfaces that still address lanes by name
// go through Includes/material_param_lanes.glsl.

#define GE_MATERIAL_PARAM_LANE_COUNT 30
GE_FIELD(vec4, uParams[GE_MATERIAL_PARAM_LANE_COUNT])

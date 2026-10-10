#ifndef GE_SSSR_DISPATCH_GLSL
#define GE_SSSR_DISPATCH_GLSL

// Indirect-dispatch arguments for the SSSR passes, plus the workgroup-grid
// arithmetic that fills and decodes them.
//
// The block is declared HERE and not once per shader. Four passes address the
// same buffer, two of them at byte offsets computed in C++, and std430 makes the
// field order a GPU ABI: a field inserted on one side only would dispatch a pass
// off the wrong counter, which fails silently in both directions (no work, or a
// wildly oversized grid). Same reasoning, and the same shape, as
// view_params_fields.glsl.
//
// Two VkDispatchIndirectCommand triples, at word 0 (Intersect) and word 4
// (Prefilter). Each triple's fourth word carries that pass's append counter —
// vkCmdDispatchIndirect reads exactly three uints, so the counter rides along in
// the padding instead of costing a separate buffer.
//
// Includers define GE_SSSR_ARGS_BINDING, and GE_SSSR_ARGS_READONLY if they only
// read the counters.
#ifndef GE_SSSR_ARGS_BINDING
#error "define GE_SSSR_ARGS_BINDING before including sssr_dispatch.glsl"
#endif

#ifdef GE_SSSR_ARGS_READONLY
#define GE_SSSR_ARGS_ACCESS readonly
#else
#define GE_SSSR_ARGS_ACCESS
#endif

layout(std430, set = 0, binding = GE_SSSR_ARGS_BINDING)
GE_SSSR_ARGS_ACCESS buffer IndirectArgsBuffer
{
    uint rayGroupCountX;
    uint rayGroupCountY;
    uint rayGroupCountZ;
    uint rayCount;
    uint tileGroupCountX;
    uint tileGroupCountY;
    uint tileGroupCountZ;
    uint tileCount;
} Args;

// Widest workgroup grid a dispatch may request along one dimension. This is the
// Vulkan spec's MINIMUM guarantee for maxComputeWorkGroupCount[0] and [1], so it
// is the only width safe to emit without querying the device.
//
// It is reachable: a fully reflective 4K frame is 129,600 8x8 tiles, and at trace
// rate 1 the same count of 64-ray groups. Exceeding a driver's real limit drops
// the excess workgroups silently — the frame is simply missing its reflections
// past that point, with no validation error to notice.
const uint kSssrMaxDispatchWidth = 65535u;

// A one-dimensional work count as a 2D workgroup grid inside that width. The
// spill into Y is exact, never a clamp, so no work is dropped; consumers recover
// the linear index with GE_SssrLinearGroupIndex and discard the grid's remainder
// against the true count.
//
// At or below the width this returns (count, 1) — the grid every pass ran before
// the spill existed, so nothing changes until a frame actually needs the second
// row.
uvec2 GE_SssrDispatchGrid(uint groupCount)
{
    if (groupCount <= kSssrMaxDispatchWidth)
        return uvec2(groupCount, 1u);
    return uvec2(kSssrMaxDispatchWidth,
                 (groupCount + kSssrMaxDispatchWidth - 1u) / kSssrMaxDispatchWidth);
}

// Inverse of the spill. Exactly gl_WorkGroupID.x whenever the grid is one row
// tall, which is every dispatch the grid above does not widen.
uint GE_SssrLinearGroupIndex(uvec2 groupId)
{
    return groupId.x + groupId.y * kSssrMaxDispatchWidth;
}

#endif

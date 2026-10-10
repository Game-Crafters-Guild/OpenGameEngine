#pragma once

// RenderGraph core value types. Deliberately device-agnostic for Stage 1 so the graph
// algorithm (cull, levels, clustering, barrier intent) is unit-testable with no
// GPU. Physical realization (device handles, formats, layouts) is layered on in
// Stage 1.2 and never leaks into the scheduling logic.

#include <cstdint>

namespace GameEngine::Rendering::RenderGraph
{

// Dense per-frame ids: indices into flat arrays, rebuilt every frame. kInvalidId
// marks "none" (0 is a valid id). Because ids are dense and per-frame, all state
// tracking uses flat arrays indexed by id — never hash maps in the hot path.
using RGResourceId = uint32_t;
using RGPassId = uint32_t;
constexpr uint32_t kInvalidId = 0xFFFFFFFFu;

// Sentinel for "through the last mip/layer" in RGRange.
constexpr uint32_t kRemaining = 0xFFFFFFFFu;

// Subresource range for an access (mip/layer granularity). Defaults to the whole
// resource; buffers ignore it. Needed so e.g. a Hi-Z pass reading mip N-1 while
// writing mip N of the SAME texture doesn't self-hazard, and so barrier
// generation can transition exactly the touched subresources.
struct RGRange
{
    uint32_t BaseMip = 0;
    uint32_t MipCount = kRemaining;
    uint32_t BaseLayer = 0;
    uint32_t LayerCount = kRemaining;
    static constexpr RGRange All() { return {}; }
};

// Logical queue indices are fixed: Graphics=0, Compute=1, Transfer=2 — arrays
// sized kQueueCount index by static_cast<uint32_t>(queue). Physical-queue
// remapping (aliased families) happens in BuildSubmissionPlan.
enum class RGQueue : uint8_t
{
    Graphics = 0,
    Compute = 1,
    Transfer = 2
};
constexpr uint32_t kQueueCount = 3;

// AccelerationStructure: a ray-query acceleration structure (a TLAS slot). The
// graph orders its builder before its readers and derives the cross-queue wait
// between them; it records no barrier for it, because the backend records the
// build's own acceleration-structure barriers (RecordTlasBuild) and a timeline
// semaphore carries the cross-queue memory dependency.
enum class RGResourceKind : uint8_t
{
    Texture,
    Buffer,
    AccelerationStructure
};

// How a pass touches a resource. Drives three things at once:
//   * cull        — writes produce, reads consume (backward reachability from sinks)
//   * barriers    — the access maps to a stage/access/layout transition
//   * clustering  — attachment roles define a pass's tile working-set
enum class RGAccess : uint8_t
{
    // reads
    Sampled,
    SampledCompute, // sampled by a compute shader dispatched from a GRAPHICS pass
                    // (stage-qualified: plain Sampled maps to the fragment stage there)
    SampledVertex,  // sampled from the vertex stage onward (vertex-displacement fields:
                    // ocean cascades). Scope covers vertex AND fragment consumers.
    UniformRead,
    StorageRead,
    IndirectRead,
    IndexRead,
    VertexRead,
    CopySrc,
    DepthRead, // depth bound read-only as an attachment (still part of the working-set)
    ColorLoad, // attachment LoadOp::Load / blend reads prior contents (derived by Attach*)
    AccelerationStructureRead, // ray queries traverse it
    // writes
    ColorAttachment,
    DepthWrite,
    StorageWrite,
    CopyDst,
    AccelerationStructureBuild
};

inline bool IsWrite(RGAccess a)
{
    switch (a)
    {
        case RGAccess::ColorAttachment:
        case RGAccess::DepthWrite:
        case RGAccess::StorageWrite:
        case RGAccess::CopyDst:
        case RGAccess::AccelerationStructureBuild:
            return true;
        default:
            return false;
    }
}

// An attachment access binds the resource into the render pass's framebuffer, so
// it participates in the tile working-set used for cluster scheduling.
inline bool IsAttachment(RGAccess a)
{
    return a == RGAccess::ColorAttachment || a == RGAccess::DepthWrite || a == RGAccess::DepthRead;
}

// Why a pass was dropped during cull (surfaced over MCP for "why didn't this run").
enum class RGCullReason : uint8_t
{
    NotCulled,
    NoConsumer,     // its outputs reach no sink (present/export/external) and have no reader
    ProducerCulled, // its outputs have readers, but every reader was itself culled
    Cycle           // part of a dependency cycle; dropped to keep the schedule valid
};

} // namespace GameEngine::Rendering::RenderGraph

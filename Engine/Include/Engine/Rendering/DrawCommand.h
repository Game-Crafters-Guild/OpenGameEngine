// DrawCommand — the per-draw record produced by contributors (terrain,
// gizmos, etc.). ECS-derived entity draws no longer flow through this type
// post-Phase 7-4: they are issued as one DrawIndexedIndirectCount per
// (Material, mesh) batch via the bucketer-written slot buffers, driven by
// the BatchKey set on WorldDrawBuilder. The world / depth-pass execute
// lambdas iterate contributor DrawCommands after the entity-batch loop.
//
// POD-shaped on purpose: pipeline + material are interned ids / non-owning
// pointers, geometry references raw VB/IB pairs, and bindings are spans into
// producer-owned scratch storage that remains valid for the frame.

#pragma once

#include "Engine/Rendering/DrawBindings.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <cstdint>
#include <span>

namespace GameEngine::Rendering
{
struct ShaderMeta;
} // namespace GameEngine::Rendering

namespace GameEngine::Engine::Renderer
{

class Material;

// Geometry source for a DrawCommand. Exactly one of `Mesh` (resolved via
// MeshGPURegistry into core/tangent/uv/joints/weights VBs + IB) or `AltGeom`
// (raw VB + IB, used by procedural producers like terrain) is consulted at
// record time. `Mesh.IsValid()` selects between them.
struct DrawCommandGeometry
{
    ::GameEngine::Rendering::MeshGPUHandle Mesh{};

    struct Alternative
    {
        ::GameEngine::Rendering::BufferHandle VB{};
        ::GameEngine::Rendering::BufferHandle IB{};
        ::GameEngine::Rendering::IndexType    IndexType = ::GameEngine::Rendering::IndexType::Uint32;
    };
    Alternative AltGeom{};
};

// A sub-draw is a `DrawIndexed` issued with the same pipeline + descriptor
// sets as its parent DrawCommand but a different index range / instance
// offset. Carrying these inside one DrawCommand (instead of one record per
// quadrant) lets terrain's LOD seam-fix expand into N draws after a single
// bind block.
struct DrawSubCommand
{
    uint32_t IndexCount    = 0;
    uint32_t InstanceCount = 1;
    uint32_t FirstIndex    = 0;
    uint32_t FirstInstance = 0;
    int32_t  VertexOffset  = 0;
};

struct DrawCommand
{
    // Identity + variant resolution
    ::GameEngine::Rendering::GraphicsPipelineId  InternedPipeline{};
    const Material*                              Material = nullptr;
    // Reflection meta for the actual interned pipeline. Two roles:
    //   1. Material == nullptr: routes through BindPipelineForDraw, this is
    //      the materialless path's only descriptor source (terrain depth).
    //   2. Material != nullptr: serves as `metaOverride` to
    //      BindMaterialForDraw, so the binder walks the variant's reflected
    //      set layout instead of the material's base ShaderMeta. Required
    //      when InternedPipeline was compiled with keywords beyond the
    //      material's authored set (e.g. a forward contributor that picks
    //      up per-pass Shadows/Instanced bindings the base material doesn't
    //      see). Null-safe — the binder falls back to material's base meta.
    const ::GameEngine::Rendering::ShaderMeta*   PipelineMeta = nullptr;
    ::GameEngine::Rendering::VertexAttributeFlags VertexFlags =
        ::GameEngine::Rendering::VertexAttributeFlags::None;
    // Set count bound for the binder (mirrors MaterialBinder set-count
    // parameter). 0 = use the meta's full set count; depth pipelines pass
    // 1 to skip fragment-stage sets the shared meta still reflects.
    uint32_t PipelineSetCount = 0;
    ::GameEngine::Rendering::MaterialKeyword     PassKeywords =
        ::GameEngine::Rendering::MaterialKeyword::None;

    // Sort hint (currently unused; populated by ECS path once the entity
    // loop folds in). Producers may leave at 0.
    uint64_t SortKey = 0;

    // Geometry — exactly one source consulted at record time.
    DrawCommandGeometry Geometry{};

    // Per-draw parameters used for the primary DrawIndexed when SubDraws is
    // empty. Ignored when SubDraws is non-empty.
    uint32_t IndexCount    = 0;
    uint32_t InstanceCount = 1;
    uint32_t FirstIndex    = 0;
    uint32_t FirstInstance = 0;
    int32_t  VertexOffset  = 0;

    // Per-draw resource bindings forwarded to MaterialBinder::BindMaterialForDraw.
    DrawBindings Bindings{};

    // Optional GPU-driven draw source. When enabled, IndexCount/InstanceCount
    // are ignored and the pass records DrawIndexedIndirectCount instead.
    bool UseIndirect = false;
    ::GameEngine::Rendering::BufferHandle IndirectCommandBuffer{};
    ::GameEngine::Rendering::BufferHandle IndirectCountBuffer{};
    uint32_t IndirectMaxDrawCount = 0;
    uint32_t IndirectStride = 0;
    // Byte offset of the first indirect command inside IndirectCommandBuffer.
    // Lets a producer point at one record within a multi-record args buffer
    // (CBT selects its visible/all VkDrawIndexedIndirectCommand by stream offset).
    size_t IndirectCommandOffset = 0;
    // Byte offset of this draw's uint32 count inside IndirectCountBuffer. Lets
    // producers pack many per-run counts into one buffer (the sorted
    // transparent drain emits one count per run).
    size_t IndirectCountOffset = 0;

    // When non-empty the primary DrawIndexed is suppressed and each SubDraw
    // emits its own DrawIndexed with the same pipeline / descriptor sets /
    // bindings. Spans must remain valid until the world pass executes
    // (producer owns the backing storage for the frame).
    std::span<const DrawSubCommand> SubDraws{};
};
// PipelineSetCount shares the 8-byte slot after VertexFlags so the 64-bit
// PassKeywords adds no padding hole. The size holds wherever pointers are
// 8 bytes; the wasm32 build differs.
static_assert(sizeof(void*) != 8 || sizeof(DrawCommand) == 216,
              "DrawCommand changed size: order its fields so none leaves a padding hole, then update this size");

} // namespace GameEngine::Engine::Renderer

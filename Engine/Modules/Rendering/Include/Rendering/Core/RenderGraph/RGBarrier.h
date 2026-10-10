#pragma once

// Abstract barrier model. Stage/access masks and image layouts mirror Vulkan's
// structure but are backend-agnostic, so whole-frame barrier derivation is
// unit-testable with no GPU; the recording layer maps them onto the backend's
// own stage, access and image-layout enums.

#include "Rendering/Core/RenderGraph/RGTypes.h"

#include <cassert>
#include <cstdint>

namespace GameEngine::Rendering::RenderGraph
{

namespace RGStage
{
constexpr uint32_t None = 0;
constexpr uint32_t TopOfPipe = 1u << 0;
constexpr uint32_t DrawIndirect = 1u << 1;
constexpr uint32_t VertexInput = 1u << 2;
constexpr uint32_t VertexShader = 1u << 3;
constexpr uint32_t FragmentShader = 1u << 4;
constexpr uint32_t EarlyFragmentTests = 1u << 5;
constexpr uint32_t LateFragmentTests = 1u << 6;
constexpr uint32_t ColorAttachmentOutput = 1u << 7;
constexpr uint32_t ComputeShader = 1u << 8;
constexpr uint32_t Transfer = 1u << 9;
constexpr uint32_t BottomOfPipe = 1u << 10;
} // namespace RGStage

namespace RGAccessMask
{
constexpr uint32_t None = 0;
constexpr uint32_t IndirectRead = 1u << 0;
constexpr uint32_t IndexRead = 1u << 1;
constexpr uint32_t VertexAttribRead = 1u << 2;
constexpr uint32_t UniformRead = 1u << 3;
constexpr uint32_t ShaderRead = 1u << 4;
constexpr uint32_t ShaderWrite = 1u << 5;
constexpr uint32_t ColorWrite = 1u << 6;
constexpr uint32_t DepthRead = 1u << 7;
constexpr uint32_t DepthWrite = 1u << 8;
constexpr uint32_t TransferRead = 1u << 9;
constexpr uint32_t TransferWrite = 1u << 10;
constexpr uint32_t ColorRead = 1u << 11; // attachment LoadOp::Load / blend source

constexpr uint32_t WriteBits = ShaderWrite | ColorWrite | DepthWrite | TransferWrite;
constexpr uint32_t ReadBits = IndirectRead | IndexRead | VertexAttribRead | UniformRead |
                              ShaderRead | DepthRead | TransferRead | ColorRead;
} // namespace RGAccessMask

enum class RGImageLayout : uint8_t
{
    Undefined,
    General,
    ColorAttachment,
    DepthAttachment,
    DepthReadOnly,
    ShaderReadOnly,
    TransferSrc,
    TransferDst,
    Present
};

// One resource transition. For buffers the layout fields are Undefined and ignored.
// Range carries RESOLVED counts (no kRemaining sentinels) covering exactly the
// subresources transitioning; whole-resource transitions cover all mips/layers.
struct RGBarrier
{
    RGResourceId Resource = kInvalidId;
    bool IsTexture = false;
    uint32_t SrcStage = RGStage::None;
    uint32_t DstStage = RGStage::None;
    uint32_t SrcAccess = RGAccessMask::None;
    uint32_t DstAccess = RGAccessMask::None;
    RGImageLayout OldLayout = RGImageLayout::Undefined;
    RGImageLayout NewLayout = RGImageLayout::Undefined;
    // Queue info (writer's queue / consuming queue). SrcQueue != DstQueue on a
    // texture layout transition is a CROSSING: the submission plan derives a
    // placement for it (BarrierSubmissions) so the transition records on the
    // producing queue, where the source scope below is legal. The recording
    // layer owns scope sanitization for whichever side the recording queue
    // cannot express — the source when the placement is foreign, the
    // destination when it is not (timeline waits provide the real ordering).
    // These fields never become ownership-transfer halves while resources use
    // concurrent sharing.
    uint32_t SrcQueue = 0;
    uint32_t DstQueue = 0;
    RGRange Range{0, 1, 0, 1};
    // Set by barrier generation when the resource had NO intra-frame accesses
    // before this barrier (first-use init / imported first-touch sync). Only
    // these are safe for the recording layer's submission-front hoist: a
    // BottomOfPipe source bit alone does NOT imply hoistability — a writer
    // after pure reads of an import inherits BottomOfPipe in its source scope
    // yet must stay ordered AFTER those readers.
    bool FirstTouch = false;
};

// A coalesced set of transitions emitted as ONE barrier call before a pass (the
// realization of "batch barriers" given the working-set-contiguous execution
// order — see RGGraph::GenerateBarriers). SrcStageUnion/DstStageUnion are the OR
// of the member barriers' stage masks (the single call's global stage masks).
struct RGBarrierBatch
{
    RGPassId Pass = kInvalidId;
    uint32_t ScheduledIndex = 0;
    uint32_t First = 0;
    uint32_t Count = 0;
    uint32_t SrcStageUnion = RGStage::None;
    uint32_t DstStageUnion = RGStage::None;
};

// Translate a declared access into its (stage, access, layout) requirement.
struct RGAccessInfo
{
    uint32_t Stage;
    uint32_t Access;
    RGImageLayout Layout; // meaningful for textures only
    bool IsWrite;
};

// Queue-aware: shader reads/writes on a compute queue map to the compute stage
// (FragmentShader bits are an invalid mask there); transfer queues only accept
// copies. Attachment/geometry accesses are graphics-only (debug assert).
inline RGAccessInfo MapAccess(RGAccess a, RGQueue queue)
{
    RGAccessInfo info{RGStage::None, RGAccessMask::None, RGImageLayout::Undefined, false};
    switch (a)
    {
        case RGAccess::Sampled:
            info = {RGStage::FragmentShader, RGAccessMask::ShaderRead, RGImageLayout::ShaderReadOnly, false};
            break;
        case RGAccess::SampledCompute:
            // Stage-qualified: a compute shader dispatched from a GRAPHICS
            // pass samples this resource — plain Sampled would scope the
            // barrier to the fragment stage and the dispatch would read
            // non-visible data. Same ShaderReadOnly layout as Sampled, so
            // mixed consumers don't flap the per-cell layout.
            info = {RGStage::ComputeShader, RGAccessMask::ShaderRead, RGImageLayout::ShaderReadOnly, false};
            break;
        case RGAccess::SampledVertex:
            // Sampled by the vertex stage (displacement/height fields fetched in a
            // vertex modifier) and typically again by the fragment stage. Fragment
            // is included so one declaration covers both consumers; same
            // ShaderReadOnly layout as Sampled.
            info = {RGStage::VertexShader | RGStage::FragmentShader, RGAccessMask::ShaderRead,
                    RGImageLayout::ShaderReadOnly, false};
            break;
        case RGAccess::UniformRead:
            info = {RGStage::VertexShader | RGStage::FragmentShader, RGAccessMask::UniformRead,
                    RGImageLayout::ShaderReadOnly, false};
            break;
        case RGAccess::StorageRead:
            // VertexShader is LOAD-BEARING on graphics: the slice-3 contract
            // orders compute skinning before world/depth/shadow passes via a
            // Read(SkinPaletteAtlas) whose consumer is the VERTEX shader
            // (bone palettes bound descriptor-direct). Without the VS bit the
            // barrier grants visibility to FS only and the VS reads stale
            // palettes. (Compute queue narrows to ComputeShader below.)
            info = {RGStage::VertexShader | RGStage::ComputeShader | RGStage::FragmentShader,
                    RGAccessMask::ShaderRead, RGImageLayout::General, false};
            break;
        case RGAccess::IndirectRead:
            info = {RGStage::DrawIndirect, RGAccessMask::IndirectRead, RGImageLayout::Undefined, false};
            break;
        case RGAccess::IndexRead:
            info = {RGStage::VertexInput, RGAccessMask::IndexRead, RGImageLayout::Undefined, false};
            break;
        case RGAccess::VertexRead:
            info = {RGStage::VertexInput, RGAccessMask::VertexAttribRead, RGImageLayout::Undefined, false};
            break;
        case RGAccess::CopySrc:
            info = {RGStage::Transfer, RGAccessMask::TransferRead, RGImageLayout::TransferSrc, false};
            break;
        case RGAccess::DepthRead:
            info = {RGStage::EarlyFragmentTests | RGStage::LateFragmentTests, RGAccessMask::DepthRead,
                    RGImageLayout::DepthReadOnly, false};
            break;
        case RGAccess::ColorLoad:
            // LoadOp::Load / blending READS the attachment: without ColorRead
            // in the consumer scope the prior write is available but not
            // VISIBLE (the old graph knew this — BarrierMapping.h LoadOp note).
            info = {RGStage::ColorAttachmentOutput, RGAccessMask::ColorRead,
                    RGImageLayout::ColorAttachment, false};
            break;
        case RGAccess::ColorAttachment:
            info = {RGStage::ColorAttachmentOutput, RGAccessMask::ColorWrite, RGImageLayout::ColorAttachment, true};
            break;
        case RGAccess::DepthWrite:
            info = {RGStage::EarlyFragmentTests | RGStage::LateFragmentTests, RGAccessMask::DepthWrite,
                    RGImageLayout::DepthAttachment, true};
            break;
        case RGAccess::StorageWrite:
            info = {RGStage::ComputeShader, RGAccessMask::ShaderWrite, RGImageLayout::General, true};
            break;
        case RGAccess::CopyDst:
            info = {RGStage::Transfer, RGAccessMask::TransferWrite, RGImageLayout::TransferDst, true};
            break;
        // Acceleration structures carry no graph barrier (RGResourceKind::
        // AccelerationStructure); these scopes only keep the cell state honest.
        case RGAccess::AccelerationStructureRead:
            info = {RGStage::ComputeShader, RGAccessMask::ShaderRead, RGImageLayout::Undefined, false};
            break;
        case RGAccess::AccelerationStructureBuild:
            info = {RGStage::ComputeShader, RGAccessMask::ShaderWrite, RGImageLayout::Undefined, true};
            break;
    }

    if (queue == RGQueue::Compute)
    {
        switch (a)
        {
            case RGAccess::Sampled:
            case RGAccess::SampledCompute:
            case RGAccess::SampledVertex:
            case RGAccess::UniformRead:
            case RGAccess::StorageRead:
            case RGAccess::StorageWrite:
            case RGAccess::AccelerationStructureRead:
            case RGAccess::AccelerationStructureBuild:
                info.Stage = RGStage::ComputeShader;
                break;
            case RGAccess::IndirectRead: // dispatch-indirect args
            case RGAccess::CopySrc:
            case RGAccess::CopyDst:
                break;
            default:
                assert(false && "RenderGraph: access kind not valid on a compute queue");
                break;
        }
    }
    else if (queue == RGQueue::Transfer)
    {
        assert((a == RGAccess::CopySrc || a == RGAccess::CopyDst) &&
               "RenderGraph: only copies are valid on a transfer queue");
    }
    return info;
}

} // namespace GameEngine::Rendering::RenderGraph

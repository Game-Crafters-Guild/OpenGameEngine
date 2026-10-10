#pragma once

#include "MetalIndirectCommandArena.h"
#include "MetalIndirectCountResources.h"

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class MetalDevice;
struct MetalBuffer;

// DrawIndexedIndirectCount for one Metal command list. Metal has no draw with
// a GPU-written count, so each call becomes one executeCommandsInBuffer over an
// indirect command buffer, run with an execution range whose length the GPU
// writes: min(*count, maxDrawCount). A small compute dispatch (the
// translation) encodes those commands from the counted draw records just
// before the render pass that executes them, so the CPU encodes one command per
// call instead of one per record, and the GPU runs only the counted records.
//
// Placement. A translation is encoded before a render pass whenever the open
// one cannot serve it: none is open yet in the command buffer, a compute
// encoder or a blit into an indirect-argument buffer ran since (either may
// have rewritten draw records or counts), or the previous render pass already
// executed commands from it (each pass reuses the command pages, which keeps
// the arena at the size of the largest pass). Draw arguments therefore come
// from compute, blit or CPU work recorded before the pass; a render pass that
// binds an indirect-argument buffer writable seals the translation so the next
// pass reads its writes. The translation's draw list is filled on the CPU
// while the pass records, which is valid because the GPU reads it only after
// commit.
//
// Ordering. The translation's reads of records and counts are declared on its
// encoder (every indirect-argument buffer, read-only), so the hazard tracker
// orders it after their producers and before later writers. Producers that
// write through device addresses are invisible to the tracker; the
// translation waits for them on the device's GPU-driven fence, the one render
// passes wait on for what their vertex stage reads. Its writes reach
// untracked arena memory, so two fences order the rest inside the command
// buffer: a render encoder that executes commands waits on m_TranslatedFence
// before its vertex stage (passes without indirect-count draws do not depend
// on the translation), and updates m_ConsumedFence after its vertex stage,
// which the next translation waits on before it reuses the command pages.
class MetalIndirectCountEncoder
{
  public:
    struct Draw
    {
        const MetalBuffer* Records = nullptr;
        size_t RecordsOffset = 0;
        uint32_t RecordStride = 0;
        const MetalBuffer* Count = nullptr;
        size_t CountOffset = 0;
        uint32_t MaxDrawCount = 0;
        const MetalBuffer* Indices = nullptr;
        MTL::IndexType IndexType = MTL::IndexTypeUInt16;
        MTL::PrimitiveType PrimitiveType = MTL::PrimitiveTypeTriangle;
    };

    explicit MetalIndirectCountEncoder(MetalDevice& device);
    ~MetalIndirectCountEncoder();

    MetalIndirectCountEncoder(const MetalIndirectCountEncoder&) = delete;
    MetalIndirectCountEncoder& operator=(const MetalIndirectCountEncoder&) = delete;

    // A new command buffer starts recording. An arena still held belongs to a
    // command buffer that was never submitted and goes straight back.
    void BeginCommandBuffer();
    // A compute encoder, a blit into an indirect-argument buffer or a writable
    // indirect-argument binding in a render pass was recorded: the next render
    // pass gets a fresh translation. A pass already open keeps drawing from
    // its own.
    void InvalidateTranslation();
    // Before the render encoder of a pass is created (no encoder open).
    // `producerFence`, when not null, is updated by compute that may have
    // written draw records or counts through device addresses; a new
    // translation waits on it.
    void PrepareRenderPass(MTL::CommandBuffer* commandBuffer, MTL::Fence* producerFence);
    void EncodeDraw(MTL::RenderCommandEncoder* encoder, const Draw& draw);
    void EndRenderEncoder(MTL::RenderCommandEncoder* encoder);
    // Before `commandBuffer` is committed: completes the open translation and
    // hands the arena to the command buffer's completion.
    void FinishCommandBuffer(MTL::CommandBuffer* commandBuffer);

  private:
    enum class TranslationState : uint8_t
    {
        None,
        Open,   // the next render pass may draw from it
        Sealed, // the current render pass may still draw from it; the next needs a new one
    };

    void EncodeTranslation(MTL::CommandBuffer* commandBuffer, MTL::Fence* producerFence);
    // Writes the translation's draw list into the arena and its count into
    // the dispatch arguments.
    void CloseTranslation();
    void ReleaseArena();

    MetalDevice& m_Device;
    std::shared_ptr<MetalIndirectCountResources> m_Resources;
    MTL::Fence* m_TranslatedFence = nullptr;            // owned (+1)
    MTL::Fence* m_ConsumedFence = nullptr;              // owned (+1)
    MetalIndirectCommandArena* m_Arena = nullptr;       // pool-owned, held for one command buffer

    TranslationState m_State = TranslationState::None;
    MetalIndirectCommandArena::UploadRange m_Translation{};
    std::vector<MetalIndirectCountSegment> m_Segments; // the open translation's draw list
    bool m_EncoderExecutedCommands = false;
    bool m_CommandsAwaitConsumption = false;
};

} // namespace Rendering
} // namespace GameEngine

#pragma once

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class MetalIndirectCountResources;

// The memory one command buffer's indirect-count draws use: indirect command
// buffer pages that the translate kernel encodes draws into, and shared upload
// pages for the translate tables and the execution ranges. Taken from the
// device's pool when a command buffer first needs it and returned once that
// command buffer completes, so pages are never rewritten while the GPU reads
// them for an earlier command buffer.
//
// Command pages are reused pass by pass inside one command buffer
// (ReuseCommands); the command list orders that reuse with fences. Upload
// pages are append-only for the command buffer's life: the CPU writes them
// while recording, so a later pass must never overwrite what an earlier
// pass's translate dispatch will read.
class MetalIndirectCommandArena
{
  public:
    struct CommandRange
    {
        MTL::IndirectCommandBuffer* CommandBuffer = nullptr; // nullptr when allocation failed
        uint32_t FirstCommand = 0;
        uint32_t CommandCount = 0;
    };

    struct UploadRange
    {
        MTL::Buffer* Buffer = nullptr; // nullptr when allocation failed
        size_t Offset = 0;
        uint8_t* Data = nullptr;
        uint64_t GpuAddress = 0;
    };

    explicit MetalIndirectCommandArena(MetalIndirectCountResources& resources);
    ~MetalIndirectCommandArena();

    MetalIndirectCommandArena(const MetalIndirectCommandArena&) = delete;
    MetalIndirectCommandArena& operator=(const MetalIndirectCommandArena&) = delete;

    // Between one and `count` consecutive commands in one page: what is left
    // of the current page, else a new or reused page. Grows the arena when
    // needed.
    CommandRange AllocateCommands(uint32_t count);
    // Makes every command page free again for the next pass.
    void ReuseCommands();
    // `bytes` of 16-byte-aligned shared memory; grows the arena when needed.
    UploadRange AllocateUpload(size_t bytes);
    // Frees everything; the GPU no longer reads the arena.
    void Reset();

  private:
    struct CommandPage
    {
        MTL::IndirectCommandBuffer* Buffer = nullptr; // owned (+1)
        uint32_t Used = 0;
    };

    struct UploadPage
    {
        MTL::Buffer* Buffer = nullptr; // owned (+1)
        size_t Used = 0;
    };

    MetalIndirectCountResources& m_Resources;
    std::vector<CommandPage> m_CommandPages;
    size_t m_CurrentCommandPage = 0;
    std::vector<UploadPage> m_UploadPages;
    size_t m_CurrentUploadPage = 0;
};

} // namespace Rendering
} // namespace GameEngine

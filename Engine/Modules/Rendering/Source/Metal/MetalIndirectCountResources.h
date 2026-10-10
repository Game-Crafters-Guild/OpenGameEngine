#pragma once

#include <Metal/Metal.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class MetalIndirectCommandArena;

// One run of consecutive records of a DrawIndexedIndirectCount call, as the
// translate kernel reads it. A call whose maxDrawCount exceeds one command
// page is split into several. The call draws min(*count, MaxDrawCount)
// records; this segment covers records FirstRecord to
// FirstRecord + CommandCount - 1 of them, starting at RecordsAddress with
// RecordStride bytes between records. The kernel encodes the drawn ones into
// the indirect command buffer from FirstCommand on and writes the execution
// range the render encoder runs them with to RangeAddress. Layout mirrors the
// kernel's `Segment` struct.
struct MetalIndirectCountSegment
{
    MTL::ResourceID CommandBuffer{};
    uint64_t RecordsAddress = 0;
    uint64_t CountAddress = 0;
    uint64_t IndexBufferAddress = 0;
    uint64_t RangeAddress = 0;
    uint32_t FirstCommand = 0;
    uint32_t MaxDrawCount = 0;
    uint32_t FirstRecord = 0;
    uint32_t CommandCount = 0;
    uint32_t RecordStride = 0;
    uint32_t IndexType = 0;     // MTL::IndexType
    uint32_t PrimitiveType = 0; // MTL::PrimitiveType
    uint32_t Padding = 0;
};
static_assert(sizeof(MetalIndirectCountSegment) == 72, "mirrors the translate kernel's Segment");

// One translate dispatch: its first 12 bytes are the dispatch's own indirect
// threadgroup counts (one threadgroup per segment), so the CPU can add
// segments after the dispatch is encoded, up to the command buffer's commit.
struct MetalIndirectCountTranslation
{
    uint32_t ThreadgroupCount[3] = {0, 1, 1};
    uint32_t SegmentCount = 0;
    uint64_t SegmentsAddress = 0;
};
static_assert(sizeof(MetalIndirectCountTranslation) == 24, "mirrors the translate kernel's Translation");

// Device-wide state behind the Metal DrawIndexedIndirectCount: the kernel that
// turns draw records into indirect command buffer commands, the pool of
// per-command-buffer arenas those commands and their tables live in, and the
// residency set that keeps every arena allocation resident on the device's
// queues. Arena memory is reached only through GPU addresses and resource ids,
// and a pass can grow its arena after the translate dispatch it uses was
// encoded, so per-encoder useResource cannot cover it.
class MetalIndirectCountResources
{
  public:
    // Threads per translate threadgroup; each threadgroup walks one segment.
    static constexpr uint32_t kTranslateThreads = 64;

    explicit MetalIndirectCountResources(MTL::Device* device);
    ~MetalIndirectCountResources();

    MetalIndirectCountResources(const MetalIndirectCountResources&) = delete;
    MetalIndirectCountResources& operator=(const MetalIndirectCountResources&) = delete;

    // Creates the residency set and attaches it to `queues`. False when the
    // OS has no residency sets; indirect-count draws are then refused.
    bool Initialize(const std::vector<MTL::CommandQueue*>& queues);
    // Detaches the residency set from the queues Initialize attached it to.
    // The device's queues must be idle.
    void Shutdown();
    bool IsAvailable() const { return m_ResidencySet != nullptr; }

    // Compiled on first use; nullptr if the kernel failed to build.
    MTL::ComputePipelineState* GetTranslatePipeline();

    // An arena no in-flight command buffer uses. Thread-safe.
    MetalIndirectCommandArena* AcquireArena();
    // Returns `arena` once the GPU no longer reads it (a command buffer
    // completion handler). Thread-safe.
    void ReleaseArena(MetalIndirectCommandArena* arena);

    // Resident allocations for arenas; owned (+1) by the caller.
    MTL::IndirectCommandBuffer* CreateCommandPage(uint32_t commandCapacity);
    MTL::Buffer* CreateUploadPage(size_t bytes);

  private:
    void MakeResident(const MTL::Allocation* allocation);

    MTL::Device* m_Device = nullptr;                                    // not owned
    MTL::ResidencySet* m_ResidencySet = nullptr;                        // owned (+1)
    MTL::IndirectCommandBufferDescriptor* m_CommandPageDesc = nullptr;  // owned (+1)
    MTL::ComputePipelineState* m_TranslatePipeline = nullptr;           // owned (+1)
    std::vector<MTL::CommandQueue*> m_Queues;                           // not owned

    std::mutex m_Mutex;
    std::vector<std::unique_ptr<MetalIndirectCommandArena>> m_Arenas;
    std::vector<MetalIndirectCommandArena*> m_FreeArenas;
};

} // namespace Rendering
} // namespace GameEngine

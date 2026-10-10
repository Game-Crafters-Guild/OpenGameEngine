#include "MetalIndirectCountResources.h"

#include "MetalIndirectCommandArena.h"

#include "Logger/Logger.h"

namespace GameEngine
{
namespace Rendering
{

namespace
{

// Turns the drawn records of each segment (the call draws its first
// min(*count, maxDrawCount); VkDrawIndexedIndirectCommand layout, the same
// five words as MTLDrawIndexedPrimitivesIndirectArguments) into indexed draws
// in the segment's indirect command buffer, and writes the execution range the
// render encoder runs. Pipeline state and buffer bindings are inherited from
// the render encoder, so a command carries only its draw arguments and the
// index buffer address.
constexpr const char* kTranslateSource = R"(
#include <metal_stdlib>
using namespace metal;

struct DrawRecord
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

struct ExecutionRange
{
    uint location;
    uint length;
};

struct Segment
{
    command_buffer commands;
    device const uchar* records;
    device const uint* count;
    device const uint* indices;
    device ExecutionRange* range;
    uint firstCommand;
    uint maxDrawCount;
    uint firstRecord;
    uint commandCount;
    uint recordStride;
    uint indexType;
    uint primitiveType;
    uint padding;
};

struct Translation
{
    uint threadgroupCount[3];
    uint segmentCount;
    device const Segment* segments;
};

// MTLPrimitiveType values.
primitive_type ToPrimitiveType(uint value)
{
    switch (value)
    {
    case 0u: return primitive_type::point;
    case 1u: return primitive_type::line;
    case 2u: return primitive_type::line_strip;
    case 4u: return primitive_type::triangle_strip;
    default: return primitive_type::triangle;
    }
}

kernel void ge_translate_indirect_count(device const Translation& translation [[buffer(0)]],
                                        uint segmentIndex [[threadgroup_position_in_grid]],
                                        uint lane [[thread_position_in_threadgroup]],
                                        uint laneCount [[threads_per_threadgroup]])
{
    if (segmentIndex >= translation.segmentCount)
    {
        return;
    }
    device const Segment& segment = translation.segments[segmentIndex];
    const uint callDrawCount = min(*segment.count, segment.maxDrawCount);
    const uint drawCount = min(max(callDrawCount, segment.firstRecord) - segment.firstRecord, segment.commandCount);
    if (lane == 0u)
    {
        *segment.range = ExecutionRange{segment.firstCommand, drawCount};
    }
    const primitive_type primitive = ToPrimitiveType(segment.primitiveType);
    // MTLIndexTypeUInt32 == 1.
    const bool indices32 = segment.indexType == 1u;
    for (uint i = lane; i < drawCount; i += laneCount)
    {
        device const DrawRecord& record =
            *reinterpret_cast<device const DrawRecord*>(segment.records + i * segment.recordStride);
        render_command command(segment.commands, segment.firstCommand + i);
        // The draw takes the vertex offset as a uint; two's complement keeps a
        // negative offset's bits, which is what the hardware adds.
        if (indices32)
        {
            command.draw_indexed_primitives(primitive, record.indexCount, segment.indices + record.firstIndex,
                                            record.instanceCount, uint(record.vertexOffset), record.firstInstance);
        }
        else
        {
            device const ushort* indices16 = reinterpret_cast<device const ushort*>(segment.indices);
            command.draw_indexed_primitives(primitive, record.indexCount, indices16 + record.firstIndex,
                                            record.instanceCount, uint(record.vertexOffset), record.firstInstance);
        }
    }
}
)";

constexpr const char* kTranslateEntryPoint = "ge_translate_indirect_count";

} // namespace

MetalIndirectCountResources::MetalIndirectCountResources(MTL::Device* device) : m_Device(device)
{
}

MetalIndirectCountResources::~MetalIndirectCountResources()
{
    Shutdown();
    // Arena destructors release their pages; the pool outlives every command
    // buffer that used them (the device waits for idle before destroying it).
    m_FreeArenas.clear();
    m_Arenas.clear();
    if (m_TranslatePipeline != nullptr)
    {
        m_TranslatePipeline->release();
        m_TranslatePipeline = nullptr;
    }
    if (m_CommandPageDesc != nullptr)
    {
        m_CommandPageDesc->release();
        m_CommandPageDesc = nullptr;
    }
    if (m_ResidencySet != nullptr)
    {
        m_ResidencySet->release();
        m_ResidencySet = nullptr;
    }
}

bool MetalIndirectCountResources::Initialize(const std::vector<MTL::CommandQueue*>& queues)
{
    MTL::ResidencySetDescriptor* setDesc = MTL::ResidencySetDescriptor::alloc()->init();
    setDesc->setLabel(NS::String::string("GE Indirect Count Commands", NS::UTF8StringEncoding));
    NS::Error* error = nullptr;
    m_ResidencySet = m_Device->newResidencySet(setDesc, &error);
    setDesc->release();
    if (m_ResidencySet == nullptr)
    {
        Logger::Log::Error("MetalIndirectCountResources: residency set creation failed ({}); "
                           "DrawIndexedIndirectCount needs macOS 15 or later",
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
        return false;
    }
    for (MTL::CommandQueue* queue : queues)
    {
        queue->addResidencySet(m_ResidencySet);
    }
    m_Queues = queues;

    // Commands inherit the render encoder's pipeline state and every buffer
    // binding (argument buffers, vertex buffers, push constants), so they bind
    // nothing of their own.
    m_CommandPageDesc = MTL::IndirectCommandBufferDescriptor::alloc()->init();
    m_CommandPageDesc->setCommandTypes(MTL::IndirectCommandTypeDrawIndexed);
    m_CommandPageDesc->setInheritPipelineState(true);
    m_CommandPageDesc->setInheritBuffers(true);
    m_CommandPageDesc->setMaxVertexBufferBindCount(0);
    m_CommandPageDesc->setMaxFragmentBufferBindCount(0);
    return true;
}

void MetalIndirectCountResources::Shutdown()
{
    if (m_ResidencySet == nullptr)
    {
        return;
    }
    for (MTL::CommandQueue* queue : m_Queues)
    {
        queue->removeResidencySet(m_ResidencySet);
    }
    m_Queues.clear();
}

MTL::ComputePipelineState* MetalIndirectCountResources::GetTranslatePipeline()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_TranslatePipeline != nullptr)
    {
        return m_TranslatePipeline;
    }
    NS::Error* error = nullptr;
    MTL::Library* library =
        m_Device->newLibrary(NS::String::string(kTranslateSource, NS::UTF8StringEncoding), nullptr, &error);
    if (library == nullptr)
    {
        Logger::Log::Error("MetalIndirectCountResources: translate kernel compile failed: {}",
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
        return nullptr;
    }
    MTL::Function* function = library->newFunction(NS::String::string(kTranslateEntryPoint, NS::UTF8StringEncoding));
    library->release();
    if (function == nullptr)
    {
        Logger::Log::Error("MetalIndirectCountResources: translate kernel entry point missing");
        return nullptr;
    }
    error = nullptr;
    m_TranslatePipeline = m_Device->newComputePipelineState(function, &error);
    function->release();
    if (m_TranslatePipeline == nullptr)
    {
        Logger::Log::Error("MetalIndirectCountResources: translate pipeline creation failed: {}",
                           error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
    }
    return m_TranslatePipeline;
}

MetalIndirectCommandArena* MetalIndirectCountResources::AcquireArena()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_FreeArenas.empty())
    {
        MetalIndirectCommandArena* arena = m_FreeArenas.back();
        m_FreeArenas.pop_back();
        return arena;
    }
    m_Arenas.push_back(std::make_unique<MetalIndirectCommandArena>(*this));
    return m_Arenas.back().get();
}

void MetalIndirectCountResources::ReleaseArena(MetalIndirectCommandArena* arena)
{
    if (arena == nullptr)
    {
        return;
    }
    arena->Reset();
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_FreeArenas.push_back(arena);
}

MTL::IndirectCommandBuffer* MetalIndirectCountResources::CreateCommandPage(uint32_t commandCapacity)
{
    // Untracked: the translate kernel writes commands through a resource id,
    // which the hazard tracker never sees; the command lists order the writes
    // and the executions with fences instead.
    MTL::IndirectCommandBuffer* page = m_Device->newIndirectCommandBuffer(
        m_CommandPageDesc, commandCapacity,
        MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeUntracked);
    if (page == nullptr)
    {
        Logger::Log::Error("MetalIndirectCountResources: indirect command buffer allocation failed ({} commands)",
                           commandCapacity);
        return nullptr;
    }
    page->setLabel(NS::String::string("GE Indirect Count Commands", NS::UTF8StringEncoding));
    MakeResident(page);
    return page;
}

MTL::Buffer* MetalIndirectCountResources::CreateUploadPage(size_t bytes)
{
    MTL::Buffer* page =
        m_Device->newBuffer(bytes, MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    if (page == nullptr)
    {
        Logger::Log::Error("MetalIndirectCountResources: upload page allocation failed ({} bytes)", bytes);
        return nullptr;
    }
    page->setLabel(NS::String::string("GE Indirect Count Tables", NS::UTF8StringEncoding));
    MakeResident(page);
    return page;
}

void MetalIndirectCountResources::MakeResident(const MTL::Allocation* allocation)
{
    // Committed at once: the command buffer that triggered the allocation is
    // still recording, so the commit lands before that command buffer's.
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ResidencySet->addAllocation(allocation);
    m_ResidencySet->commit();
}

} // namespace Rendering
} // namespace GameEngine

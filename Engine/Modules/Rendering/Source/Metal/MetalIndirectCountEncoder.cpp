#include "MetalIndirectCountEncoder.h"

#include "MetalDevice.h"

#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine
{
namespace Rendering
{

MetalIndirectCountEncoder::MetalIndirectCountEncoder(MetalDevice& device)
    : m_Device(device), m_Resources(device.GetIndirectCountResources())
{
    MTL::Device* mtlDevice = device.GetMTLDevice();
    if (mtlDevice != nullptr)
    {
        m_TranslatedFence = mtlDevice->newFence();
        m_ConsumedFence = mtlDevice->newFence();
    }
}

MetalIndirectCountEncoder::~MetalIndirectCountEncoder()
{
    ReleaseArena();
    // Command buffers retain the fences they encode, so releasing here is safe
    // while one of them is still in flight.
    if (m_TranslatedFence != nullptr)
    {
        m_TranslatedFence->release();
    }
    if (m_ConsumedFence != nullptr)
    {
        m_ConsumedFence->release();
    }
}

void MetalIndirectCountEncoder::BeginCommandBuffer()
{
    ReleaseArena();
    m_State = TranslationState::None;
    m_Segments.clear();
    m_EncoderExecutedCommands = false;
    m_CommandsAwaitConsumption = false;
}

void MetalIndirectCountEncoder::InvalidateTranslation()
{
    if (m_State == TranslationState::Open)
    {
        m_State = TranslationState::Sealed;
    }
}

void MetalIndirectCountEncoder::CloseTranslation()
{
    if (m_State == TranslationState::None)
    {
        return;
    }
    if (!m_Segments.empty())
    {
        const size_t bytes = m_Segments.size() * sizeof(MetalIndirectCountSegment);
        const MetalIndirectCommandArena::UploadRange segments = m_Arena->AllocateUpload(bytes);
        MetalIndirectCountTranslation translation{};
        if (segments.Buffer != nullptr)
        {
            std::memcpy(segments.Data, m_Segments.data(), bytes);
            translation.ThreadgroupCount[0] = static_cast<uint32_t>(m_Segments.size());
            translation.SegmentCount = static_cast<uint32_t>(m_Segments.size());
            translation.SegmentsAddress = segments.GpuAddress;
        }
        std::memcpy(m_Translation.Data, &translation, sizeof(translation));
    }
    m_Segments.clear();
    m_State = TranslationState::None;
}

void MetalIndirectCountEncoder::PrepareRenderPass(MTL::CommandBuffer* commandBuffer, MTL::Fence* producerFence)
{
    if (m_State == TranslationState::Open)
    {
        return;
    }
    CloseTranslation();
    EncodeTranslation(commandBuffer, producerFence);
}

void MetalIndirectCountEncoder::EncodeTranslation(MTL::CommandBuffer* commandBuffer, MTL::Fence* producerFence)
{
    if (m_Resources == nullptr || !m_Resources->IsAvailable() || m_TranslatedFence == nullptr ||
        m_ConsumedFence == nullptr)
    {
        return;
    }
    MTL::ComputePipelineState* pipeline = m_Resources->GetTranslatePipeline();
    if (pipeline == nullptr)
    {
        return;
    }
    if (m_Arena == nullptr)
    {
        m_Arena = m_Resources->AcquireArena();
    }
    else
    {
        // Safe once the previous pass has passed its vertex stage: this
        // translation waits on m_ConsumedFence below before it writes.
        m_Arena->ReuseCommands();
    }
    m_Translation = m_Arena->AllocateUpload(sizeof(MetalIndirectCountTranslation));
    if (m_Translation.Buffer == nullptr)
    {
        return;
    }
    const MetalIndirectCountTranslation empty{};
    std::memcpy(m_Translation.Data, &empty, sizeof(empty));

    MTL::ComputeCommandEncoder* encoder = commandBuffer->computeCommandEncoder(); // autoreleased
    if (encoder == nullptr)
    {
        Logger::Log::Error("MetalIndirectCountEncoder: failed to create the translate encoder");
        return;
    }
    static NS::String* const kLabel =
        NS::String::string("GE Indirect Count Translate", NS::UTF8StringEncoding)->retain();
    encoder->setLabel(kLabel);
    if (producerFence != nullptr)
    {
        encoder->waitForFence(producerFence);
    }
    if (m_CommandsAwaitConsumption)
    {
        encoder->waitForFence(m_ConsumedFence);
        m_CommandsAwaitConsumption = false;
    }
    m_Device.DeclareIndirectArgumentReads(encoder);
    encoder->setComputePipelineState(pipeline);
    encoder->setBuffer(m_Translation.Buffer, m_Translation.Offset, 0);
    encoder->dispatchThreadgroups(m_Translation.Buffer, m_Translation.Offset,
                                  MTL::Size(MetalIndirectCountResources::kTranslateThreads, 1, 1));
    encoder->updateFence(m_TranslatedFence);
    encoder->endEncoding();
    m_State = TranslationState::Open;
}

void MetalIndirectCountEncoder::EncodeDraw(MTL::RenderCommandEncoder* encoder, const Draw& draw)
{
    if (m_State == TranslationState::None)
    {
        static bool s_Logged = false;
        if (!s_Logged)
        {
            Logger::Log::Error("MetalCommandList::DrawIndexedIndirectCount: no translation prepared for this render "
                               "pass (indirect command resources unavailable); the draw is skipped");
            s_Logged = true;
        }
        return;
    }
    if (!m_EncoderExecutedCommands)
    {
        // Metal may hoist a render encoder's fence waits to its start, which
        // is equally correct here: draws encoded before this one do not read
        // the translation.
        encoder->waitForFence(m_TranslatedFence, MTL::RenderStageVertex);
        m_EncoderExecutedCommands = true;
    }
    m_State = TranslationState::Sealed;
    // One execute per page-sized run of records; the translation writes each
    // run's range, so runs past the count execute nothing.
    uint32_t firstRecord = 0;
    while (firstRecord < draw.MaxDrawCount)
    {
        const MetalIndirectCommandArena::CommandRange commands =
            m_Arena->AllocateCommands(draw.MaxDrawCount - firstRecord);
        const MetalIndirectCommandArena::UploadRange range =
            m_Arena->AllocateUpload(sizeof(MTL::IndirectCommandBufferExecutionRange));
        if (commands.CommandBuffer == nullptr || range.Buffer == nullptr)
        {
            return;
        }
        // Overwritten by the translation; an empty range until then.
        const MTL::IndirectCommandBufferExecutionRange emptyRange{commands.FirstCommand, 0};
        std::memcpy(range.Data, &emptyRange, sizeof(emptyRange));

        MetalIndirectCountSegment segment{};
        segment.CommandBuffer = commands.CommandBuffer->gpuResourceID();
        segment.RecordsAddress = draw.Records->buffer->gpuAddress() + draw.RecordsOffset +
                                 static_cast<uint64_t>(firstRecord) * draw.RecordStride;
        segment.CountAddress = draw.Count->buffer->gpuAddress() + draw.CountOffset;
        segment.IndexBufferAddress = draw.Indices->buffer->gpuAddress();
        segment.RangeAddress = range.GpuAddress;
        segment.FirstCommand = commands.FirstCommand;
        segment.MaxDrawCount = draw.MaxDrawCount;
        segment.FirstRecord = firstRecord;
        segment.CommandCount = commands.CommandCount;
        segment.RecordStride = draw.RecordStride;
        segment.IndexType = static_cast<uint32_t>(draw.IndexType);
        segment.PrimitiveType = static_cast<uint32_t>(draw.PrimitiveType);
        m_Segments.push_back(segment);

        encoder->executeCommandsInBuffer(commands.CommandBuffer, range.Buffer, range.Offset);
        firstRecord += commands.CommandCount;
    }
}

void MetalIndirectCountEncoder::EndRenderEncoder(MTL::RenderCommandEncoder* encoder)
{
    if (!m_EncoderExecutedCommands)
    {
        return;
    }
    // Commands are consumed by the vertex stage; the next translation may
    // reuse their pages once it is done, while fragments still shade.
    encoder->updateFence(m_ConsumedFence, MTL::RenderStageVertex);
    m_EncoderExecutedCommands = false;
    m_CommandsAwaitConsumption = true;
}

void MetalIndirectCountEncoder::FinishCommandBuffer(MTL::CommandBuffer* commandBuffer)
{
    CloseTranslation();
    if (m_Arena != nullptr)
    {
        // The handler keeps the pool alive past a device shutdown that races
        // the command buffer's completion.
        std::shared_ptr<MetalIndirectCountResources> resources = m_Resources;
        MetalIndirectCommandArena* arena = m_Arena;
        commandBuffer->addCompletedHandler(
            [resources, arena](MTL::CommandBuffer*) { resources->ReleaseArena(arena); });
        m_Arena = nullptr;
    }
    m_EncoderExecutedCommands = false;
    m_CommandsAwaitConsumption = false;
}

void MetalIndirectCountEncoder::ReleaseArena()
{
    if (m_Arena != nullptr)
    {
        m_Resources->ReleaseArena(m_Arena);
        m_Arena = nullptr;
    }
}

} // namespace Rendering
} // namespace GameEngine

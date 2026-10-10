#include "MetalIndirectCommandArena.h"

#include "MetalIndirectCountResources.h"

#include <algorithm>

namespace GameEngine
{
namespace Rendering
{

namespace
{
// An indirect command costs about 700 bytes of private memory on Apple GPUs
// (M4 Max: 692 bytes per DrawIndexed command), so a page is about 1.4 MB. The
// capacity stays under 2046 commands: with GPU shader validation on
// (MTL_SHADER_VALIDATION=1, macOS 26.5), an indirect command buffer of 2046
// commands or more draws nothing.
constexpr uint32_t kCommandPageCapacity = 2040;
constexpr size_t kUploadPageBytes = 64 * 1024;
constexpr size_t kUploadAlignment = 16;

size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

MetalIndirectCommandArena::MetalIndirectCommandArena(MetalIndirectCountResources& resources)
    : m_Resources(resources)
{
}

MetalIndirectCommandArena::~MetalIndirectCommandArena()
{
    for (CommandPage& page : m_CommandPages)
    {
        page.Buffer->release();
    }
    for (UploadPage& page : m_UploadPages)
    {
        page.Buffer->release();
    }
}

MetalIndirectCommandArena::CommandRange MetalIndirectCommandArena::AllocateCommands(uint32_t count)
{
    while (m_CurrentCommandPage < m_CommandPages.size())
    {
        CommandPage& page = m_CommandPages[m_CurrentCommandPage];
        if (page.Used < kCommandPageCapacity)
        {
            const uint32_t first = page.Used;
            const uint32_t taken = std::min(count, kCommandPageCapacity - first);
            page.Used += taken;
            return CommandRange{page.Buffer, first, taken};
        }
        ++m_CurrentCommandPage;
    }
    MTL::IndirectCommandBuffer* buffer = m_Resources.CreateCommandPage(kCommandPageCapacity);
    if (buffer == nullptr)
    {
        return CommandRange{};
    }
    const uint32_t taken = std::min(count, kCommandPageCapacity);
    m_CommandPages.push_back(CommandPage{buffer, taken});
    m_CurrentCommandPage = m_CommandPages.size() - 1;
    return CommandRange{buffer, 0, taken};
}

void MetalIndirectCommandArena::ReuseCommands()
{
    for (CommandPage& page : m_CommandPages)
    {
        page.Used = 0;
    }
    m_CurrentCommandPage = 0;
}

MetalIndirectCommandArena::UploadRange MetalIndirectCommandArena::AllocateUpload(size_t bytes)
{
    const size_t alignedBytes = AlignUp(bytes, kUploadAlignment);
    while (m_CurrentUploadPage < m_UploadPages.size())
    {
        UploadPage& page = m_UploadPages[m_CurrentUploadPage];
        if (page.Buffer->length() - page.Used >= alignedBytes)
        {
            const size_t offset = page.Used;
            page.Used += alignedBytes;
            return UploadRange{page.Buffer, offset, static_cast<uint8_t*>(page.Buffer->contents()) + offset,
                               page.Buffer->gpuAddress() + offset};
        }
        ++m_CurrentUploadPage;
    }
    MTL::Buffer* buffer = m_Resources.CreateUploadPage(std::max(kUploadPageBytes, alignedBytes));
    if (buffer == nullptr)
    {
        return UploadRange{};
    }
    m_UploadPages.push_back(UploadPage{buffer, alignedBytes});
    m_CurrentUploadPage = m_UploadPages.size() - 1;
    return UploadRange{buffer, 0, static_cast<uint8_t*>(buffer->contents()), buffer->gpuAddress()};
}

void MetalIndirectCommandArena::Reset()
{
    ReuseCommands();
    for (UploadPage& page : m_UploadPages)
    {
        page.Used = 0;
    }
    m_CurrentUploadPage = 0;
}

} // namespace Rendering
} // namespace GameEngine

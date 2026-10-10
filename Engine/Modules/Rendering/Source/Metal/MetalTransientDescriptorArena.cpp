#include "MetalTransientDescriptorArena.h"

#include "Rendering/Common/Utils.h"

#include <algorithm>
#include <cstring>

namespace GameEngine
{
namespace Rendering
{

MetalTransientDescriptorArena::~MetalTransientDescriptorArena()
{
    Release();
}

MetalTransientDescriptorArena::Allocation MetalTransientDescriptorArena::Allocate(MTL::Device& device, size_t bytes)
{
    const size_t size = Utils::AlignUp(std::max<size_t>(bytes, 1), kOffsetAlignment);
    while (m_CurrentPage < m_Pages.size() && m_Pages[m_CurrentPage].Used + size > m_Pages[m_CurrentPage].Capacity)
    {
        ++m_CurrentPage;
    }
    if (m_CurrentPage == m_Pages.size())
    {
        const size_t capacity = std::max(kPageBytes, size);
        MTL::Buffer* buffer =
            device.newBuffer(capacity, MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
        if (buffer == nullptr)
        {
            return {};
        }
        static NS::String* const kPageLabel =
            NS::String::alloc()->init("Transient descriptor sets", NS::UTF8StringEncoding);
        buffer->setLabel(kPageLabel);
        m_Pages.push_back({buffer, static_cast<uint8_t*>(buffer->contents()), buffer->gpuAddress(), capacity, 0});
    }

    Page& page = m_Pages[m_CurrentPage];
    Allocation allocation{page.Buffer, page.Used, page.Data + page.Used, page.GpuAddress + page.Used};
    page.Used += size;
    std::memset(allocation.Data, 0, bytes);
    return allocation;
}

void MetalTransientDescriptorArena::Reset()
{
    for (Page& page : m_Pages)
    {
        page.Used = 0;
    }
    m_CurrentPage = 0;
}

void MetalTransientDescriptorArena::Release()
{
    for (Page& page : m_Pages)
    {
        page.Buffer->release();
    }
    m_Pages.clear();
    m_CurrentPage = 0;
}

} // namespace Rendering
} // namespace GameEngine

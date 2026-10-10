#include "TerrainECS/GrassWindVolumeRing.h"

#include "Rendering/Core/Device.h"

#include <algorithm>
#include <cstring>

namespace GameEngine
{
namespace
{

Rendering::BufferHandle CreateUploadStorageBuffer(Rendering::IDevice& device, uint32 recordCount,
                                                  const char* debugName)
{
    Rendering::BufferDesc desc{};
    desc.size = static_cast<size_t>(recordCount) * sizeof(Rendering::WindVolumeGPU);
    desc.usage = static_cast<uint32>(Rendering::BufferUsage::Storage);
    desc.memoryUsage = Rendering::BufferMemoryUsage::Upload;
    desc.persistent = true;
    desc.debugName = debugName;
    return device.CreateBuffer(desc);
}

} // namespace

void GrassWindVolumeRing::EnsureZeroRecord(Rendering::IDevice& device)
{
    if (m_ZeroRecord.IsValid())
        return;
    m_ZeroRecord = CreateUploadStorageBuffer(device, 1u, "TerrainGrass_WindVolumesNone");
    if (!m_ZeroRecord.IsValid())
        return;
    if (void* mapped = device.MapBuffer(m_ZeroRecord))
    {
        const Rendering::WindVolumeGPU empty{};
        std::memcpy(mapped, &empty, sizeof(empty));
        device.UnmapBuffer(m_ZeroRecord);
    }
}

void GrassWindVolumeRing::Upload(Rendering::IDevice& device, uint32 slot,
                                 const Rendering::WindVolumeGPU* volumes, uint32 count)
{
    EnsureZeroRecord(device);

    Element& element = m_Elements[slot % kSlots];
    // Reset first so a failed upload publishes "no volumes" rather than last frame's count
    // against whatever bytes the buffer holds.
    element.Count = 0;

    const uint32 recordCount = volumes ? count : 0u;
    const uint32 requiredCapacity = std::max(1u, recordCount);
    if (requiredCapacity > element.Capacity || !element.Buffer.IsValid())
    {
        // The device defers the destroy past the frames in flight.
        if (element.Buffer.IsValid())
            device.DestroyBuffer(element.Buffer);
        element.Buffer = CreateUploadStorageBuffer(device, requiredCapacity, "TerrainGrass_WindVolumes");
        element.Capacity = element.Buffer.IsValid() ? requiredCapacity : 0u;
    }
    if (!element.Buffer.IsValid())
        return;

    void* mapped = device.MapBuffer(element.Buffer);
    if (!mapped)
    {
        // Buffer(slot) answers with the zero record until a later upload succeeds.
        device.DestroyBuffer(element.Buffer);
        element.Buffer = {};
        element.Capacity = 0;
        return;
    }
    const Rendering::WindVolumeGPU empty{};
    std::memcpy(mapped, recordCount ? volumes : &empty,
                static_cast<size_t>(requiredCapacity) * sizeof(Rendering::WindVolumeGPU));
    device.UnmapBuffer(element.Buffer);
    element.Count = recordCount;
}

Rendering::BufferHandle GrassWindVolumeRing::Buffer(uint32 slot) const
{
    const Element& element = m_Elements[slot % kSlots];
    return element.Buffer.IsValid() ? element.Buffer : m_ZeroRecord;
}

void GrassWindVolumeRing::Destroy(Rendering::IDevice& device)
{
    for (Element& element : m_Elements)
    {
        if (element.Buffer.IsValid())
            device.DestroyBuffer(element.Buffer);
        element = {};
    }
    if (m_ZeroRecord.IsValid())
        device.DestroyBuffer(m_ZeroRecord);
    m_ZeroRecord = {};
}

void GrassWindVolumeRing::ForgetAfterDeviceRebuild()
{
    for (Element& element : m_Elements)
        element = {};
    m_ZeroRecord = {};
}

} // namespace GameEngine

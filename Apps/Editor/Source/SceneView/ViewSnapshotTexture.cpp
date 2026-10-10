#include "SceneView/ViewSnapshotTexture.h"

namespace GameEngine::Editor
{

void ViewSnapshotTexture::ForgetIfDeviceRebuilt(uint64_t currentGeneration)
{
    if (currentGeneration == m_DeviceRebuildGeneration)
        return;

    m_Texture = {};
    m_Width = 0;
    m_Height = 0;
    m_Format = 0;
    m_DeviceRebuildGeneration = currentGeneration;
}

Rendering::TextureHandle ViewSnapshotTexture::Texture(const Rendering::IDevice& device) const
{
    // Const on purpose: a read must not mutate a cache shared by every consumer
    // of this frame. The next Ensure is what actually forgets the handle.
    if (device.GetDeviceRebuildGeneration() != m_DeviceRebuildGeneration)
        return {};
    return m_Texture;
}

Rendering::TextureHandle ViewSnapshotTexture::Ensure(Rendering::IDevice& device, uint32_t width,
                                                     uint32_t height, uint32_t format,
                                                     const char* debugName)
{
    ForgetIfDeviceRebuilt(device.GetDeviceRebuildGeneration());

    if (m_Texture.IsValid() && m_Width == width && m_Height == height && m_Format == format)
        return m_Texture;

    Destroy(&device);

    Rendering::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.depth = 1;
    desc.mipLevels = 1;
    desc.arrayLayers = 1;
    desc.sampleCount = 1;
    desc.format = format;
    desc.usage = static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource) |
                 static_cast<uint32_t>(Rendering::TextureUsage::TransferDst);
    desc.debugName = debugName;

    const auto texture = device.CreateTexture(desc);
    if (!texture.IsValid())
        return {};

    m_Texture = texture;
    m_Width = width;
    m_Height = height;
    m_Format = format;
    m_DeviceRebuildGeneration = device.GetDeviceRebuildGeneration();
    return m_Texture;
}

void ViewSnapshotTexture::Destroy(Rendering::IDevice* device)
{
    if (device)
        ForgetIfDeviceRebuilt(device->GetDeviceRebuildGeneration());

    if (m_Texture.IsValid() && device)
        device->DestroyTexture(m_Texture);

    m_Texture = {};
    m_Width = 0;
    m_Height = 0;
    m_Format = 0;
}

} // namespace GameEngine::Editor

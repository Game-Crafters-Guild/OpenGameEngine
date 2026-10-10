#pragma once

#include "Rendering/Core/Device.h"

#include <cstdint>

namespace GameEngine::Editor
{

/// A texture the editor keeps across frames, keyed to the device generation
/// that created it.
///
/// An in-place device rebuild destroys every live texture and frees its
/// generational slot, but a handle is only an id — `TextureHandle::IsValid()`
/// is `id != 0` and keeps answering true for an image that no longer exists.
/// Reusing one after a rebuild hands the render graph a handle that resolves to
/// nothing: the descriptor write is skipped, the descriptor keeps its previous
/// contents, and the GPU samples freed memory. Destroying it is equally wrong —
/// the slot is already free. So the rebuild generation is part of the cache key,
/// and a generation change forgets the handle instead of destroying it.
///
/// Every read takes the device, and that is the whole safety property: the
/// generation compare is the only thing separating a live handle from a dead
/// id, and a read that could skip it would put the dead id into
/// RGFrame::ImportExternalTexture — which records it as the physical without a
/// liveness check — or into a UI texture slot, which registers it without one
/// either. Callers that never call Ensure are exactly the ones that need it, so
/// it cannot live in Ensure alone: the presentation snapshot's
/// waiting-for-extraction arm imports the cached handle directly, and the
/// camera-bookmark popup binds it hover cycles after the copy wrote it.
class ViewSnapshotTexture
{
  public:
    /// The handle to use this frame, created or recreated as the shape and the
    /// device generation require. Invalid when creation fails.
    Rendering::TextureHandle Ensure(Rendering::IDevice& device, uint32_t width, uint32_t height,
                                    uint32_t format, const char* debugName);

    /// Releases the texture. Safe across a rebuild — a handle the rebuild
    /// already destroyed is dropped, not destroyed a second time.
    void Destroy(Rendering::IDevice* device);

    /// The cached handle, or an invalid handle once `device` has been rebuilt
    /// since the texture was created. There is no device-free read.
    Rendering::TextureHandle Texture(const Rendering::IDevice& device) const;
    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }
    uint32_t Format() const { return m_Format; }

  private:
    /// Drops the handle when the device has been rebuilt since it was created.
    void ForgetIfDeviceRebuilt(uint64_t currentGeneration);

    Rendering::TextureHandle m_Texture{};
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_Format = 0; // TextureDesc::format domain (platform format enum)
    uint64_t m_DeviceRebuildGeneration = 0;
};

} // namespace GameEngine::Editor

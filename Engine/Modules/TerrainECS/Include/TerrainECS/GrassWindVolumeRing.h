#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/WindVolumeGPU.h"
#include "Types/Types.h"

namespace GameEngine::Rendering
{
class IDevice;
}

namespace GameEngine
{

/// Per-slot SSBO ring of the wind volumes the terrain grass shaders sample.
///
/// Elements are addressed by the terrain-params slot the volumes were extracted
/// with, so a draw binds the volumes that match its params. A slot's buffer is
/// only rewritten when that slot comes around again, kSlots extraction frames
/// later, which is what makes the persistently mapped upload memory safe with
/// frames in flight. Upload runs in the application UPDATE phase, so kSlots must
/// EXCEED the device's pacing rather than match it — see
/// FrameBufferAllocator::BeginFrame for the rule.
///
/// Every slot binds something: an empty snapshot writes one zero record, and a
/// slot whose upload failed binds a shared zero-record buffer with a count of 0,
/// so a mapping failure degrades to "no volumes" instead of removing the grass.
///
/// Threading: Upload runs on the extraction tick, right after the terrain feature
/// publishes the params slot; Buffer/Count are read by render features later in
/// the same frame through that slot. Extraction and render-graph declaration are
/// ordered by joining extraction workers before render-graph declaration,
/// the same ordering the params ring relies on.
class GrassWindVolumeRing
{
public:
    // Deepest pacing any backend reports, plus the update-phase slot. Sized from the
    // bound rather than from the pacing every current backend happens to use.
    static constexpr uint32 kSlots =
        GameEngine::Rendering::IDevice::kMaxSupportedFramesInFlight + 1u;

    void Upload(Rendering::IDevice& device, uint32 slot,
                const Rendering::WindVolumeGPU* volumes, uint32 count);
    void Destroy(Rendering::IDevice& device);
    // After an in-place device rebuild: the buffers died with the old device. Forgets them
    // without destroying them; the next Upload recreates them.
    void ForgetAfterDeviceRebuild();

    /// Valid after the first Upload; the descriptor range is Count * sizeof(WindVolumeGPU),
    /// at least one record.
    Rendering::BufferHandle Buffer(uint32 slot) const;
    uint32 Count(uint32 slot) const { return m_Elements[slot % kSlots].Count; }

private:
    struct Element
    {
        Rendering::BufferHandle Buffer{};
        uint32 Capacity = 0;
        uint32 Count = 0;
    };

    void EnsureZeroRecord(Rendering::IDevice& device);

    Element m_Elements[kSlots]{};
    Rendering::BufferHandle m_ZeroRecord{};
};

} // namespace GameEngine

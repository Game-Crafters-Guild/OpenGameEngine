#pragma once

// Unwraps a backend's per-frame slot into a monotonically increasing frame counter,
// so a ring deeper than the device's pacing can actually rotate through all of it.

#include <cstdint>

namespace GameEngine::Rendering
{

// IDevice::GetFrameIndex() reports a slot in [0, GetFramesInFlight()) — VulkanDevice
// wraps it at MAX_FRAMES_IN_FLIGHT. Its VALUE therefore cannot select a ring element:
// `deviceFrameIndex % ringDepth` caps the effective rotation at the device's pacing and
// leaves every element beyond it unreachable, silently deleting the reuse margin that
// extra depth was bought for. Only the fact that the slot CHANGED carries information —
// it changes exactly when the device advances a frame, which is the unit every
// fence-distance argument is stated in.
//
// Tick() consumes that change token and returns a counter that steps once per device
// frame. Ring selection is `counter % ringDepth`; for a power-of-two ringDepth the
// counter's own 2^32 wrap divides evenly and so keeps the rotation seamless.
class DeviceFrameCounter
{
public:
    // This device frame's counter, stepped iff the token changed since the last call.
    // Idempotent within a device frame, so several observers of the same frame (a
    // per-view declaration, a second subsystem entry point) all select the same
    // element; a repeated token holds the counter rather than spending rotation
    // margin inside one frame.
    uint32_t Tick(uint32_t deviceFrameIndex)
    {
        if (m_Started && deviceFrameIndex != m_LastDeviceFrameIndex)
            ++m_Counter;
        m_LastDeviceFrameIndex = deviceFrameIndex;
        m_Started = true;
        return m_Counter;
    }

private:
    uint32_t m_Counter = 0;
    uint32_t m_LastDeviceFrameIndex = 0;
    bool m_Started = false; // false until the first Tick, which claims counter 0
};

} // namespace GameEngine::Rendering
